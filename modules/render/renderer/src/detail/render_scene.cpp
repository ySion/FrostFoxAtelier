#include "render_scene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace atelier::renderer::detail {
namespace {

bool matrix_same_bits(const math::Matrix4& a, const math::Matrix4& b) noexcept {
    return std::memcmp(a.elements.data(), b.elements.data(), sizeof(a.elements)) == 0;
}

math::Bounds transform_aabb(const math::Bounds& local, const math::Matrix4& world) noexcept {
    const math::Vector3 lc = (local.minimum + local.maximum) * 0.5f;
    const math::Vector3 le = (local.maximum - local.minimum) * 0.5f;
    math::Vector3 wc{world[12], world[13], world[14]};
    math::Vector3 we{0.f, 0.f, 0.f};
    for (std::size_t col = 0; col < 3u; ++col) {
        wc[col] += lc[0] * world[0 + col] + lc[1] * world[4 + col] + lc[2] * world[8 + col];
        we[col] =
            std::abs(world[0 + col]) * le[0] + std::abs(world[4 + col]) * le[1] + std::abs(world[8 + col]) * le[2];
    }
    return {wc - we, wc + we};
}

std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v;
    h *= 0x100000001b3ull;
    return h;
}

std::uint64_t fnv_mix_bytes(std::uint64_t h, const void* data, std::size_t len) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t i = 0u;
    for (; i + 8u <= len; i += 8u) {
        std::uint64_t word = 0u;
        std::memcpy(&word, bytes + i, sizeof(word));
        h = fnv_mix(h, word);
    }
    if (i < len) {
        std::uint64_t tail = 0u;
        std::memcpy(&tail, bytes + i, len - i);
        h = fnv_mix(h, tail);
    }
    return h;
}

Result<void> ensure_scene_structured_buffer(rhi::Device& device, OwnedBuffer& buf, std::uint64_t needed_bytes,
                                            std::uint32_t stride, const char* debug_name, bool& out_reallocated) {
    out_reallocated = false;
    const std::uint64_t min_bytes = std::max<std::uint64_t>(stride, needed_bytes);
    if (buf.buffer && buf.capacity >= min_bytes)
        return {};

    buf.retire(device);
    out_reallocated = true;
    const std::uint64_t alloc_bytes = align_up_bytes(std::max<std::uint64_t>(4096u, std::bit_ceil(min_bytes)), stride);
    bool ok = false;
    struct Guard {
        OwnedBuffer* b;

        rhi::Device* d;

        bool* done;

        ~Guard() {
            if (!*done)
                b->retire(*d);
        }
    } guard{&buf, &device, &ok};

    const auto& core = rhi::BackendAccess::core(device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);

    nri::BufferDesc desc{};
    desc.size = alloc_bytes;
    desc.structureStride = stride;
    desc.usage = nri::BufferUsageBits::SHADER_RESOURCE;
    ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, desc, buf.buffer),
                          rhi::ErrorCode::out_of_memory, "Scene persistent DEVICE buffer allocation failed", true));
    if (debug_name)
        core.SetDebugName(buf.buffer, debug_name);

    nri::BufferViewDesc v_desc{};
    v_desc.buffer = buf.buffer;
    v_desc.type = nri::BufferView::STRUCTURED_BUFFER;
    v_desc.offset = 0u;
    v_desc.size = nri::WHOLE_SIZE;
    v_desc.structureStride = stride;
    ATELIER_TRY(check_nri(core.CreateBufferView(v_desc, buf.srv), rhi::ErrorCode::creation_failed,
                          "Scene persistent DEVICE buffer SRV failed", true));
    auto bindless = rhi::BackendAccess::bindless(device).allocate_resource(buf.srv);
    if (!bindless)
        return std::unexpected(Error{bindless.error(), true});
    buf.srv_bindless = *bindless;
    buf.capacity = alloc_bytes;
    ok = true;
    return {};
}

} // namespace

void PersistentRenderScene::retire(rhi::Device& device) noexcept {
    tlas.retire(device);
    tlas_scratch.retire(device);
    tlas_instance_slots.retire_all(device);
    selected_mask_slots.retire_all(device);
    gpu_instance_buffer.retire(device);
    gpu_material_slot_map.retire(device);
    delta_upload_slots.retire_all(device);
    cached_gpu_instances.clear();
    cached_slot_map.clear();
    staged_gpu_instances.clear();
    staged_slot_map.clear();
    pending_delta_copies.clear();
    pending_delta_src = nullptr;
    gpu_instances_ever_written = false;
    gpu_slot_map_ever_written = false;
    pending_instances_copy = false;
    pending_slot_map_copy = false;
    tracked_objects.clear();
    vsm_instances.clear();
    pending_tlas_build = false;
    tlas_valid = false;
    tlas_gpu_built = false;
    tlas_scratch_gpu_used = false;
    built_tlas_hash = 0u;
}

PersistentRenderScene& RenderSceneTable::acquire(rhi::Device& device, std::uint64_t scene_identity,
                                                 std::uint64_t sequence) {
    for (auto& sc : scenes_) {
        if (sc->scene_identity == scene_identity) {
            sc->last_used_sequence = sequence;
            return *sc;
        }
    }
    if (scenes_.size() >= max_warm_contexts) {
        auto oldest = std::ranges::min_element(scenes_, {}, &PersistentRenderScene::last_used_sequence);
        (*oldest)->retire(device);
        **oldest = PersistentRenderScene{};
        (*oldest)->scene_identity = scene_identity;
        (*oldest)->last_used_sequence = sequence;
        return **oldest;
    }
    auto created = std::make_unique<PersistentRenderScene>();
    created->scene_identity = scene_identity;
    created->last_used_sequence = sequence;
    PersistentRenderScene& ref = *created;
    scenes_.add(std::move(created));
    return ref;
}

Result<void> RenderSceneTable::update_scene(rhi::Device& device, PersistentRenderScene& scene, GeometryPool& geo_pool,
                                            const Frame& frame, Span<const GpuSurfaceRecord> surface_records,
                                            std::uint32_t masked_bin_count, std::size_t slot_index,
                                            bool build_ray_scene, bool vsm_active, std::uint64_t current_sequence,
                                            const std::function<bool()>* stop_requested, bool& out_cancelled,
                                            FrameStats& stats) {
    out_cancelled = false;
    ++scene.scene_tick;
    scene.any_selected = false;
    scene.any_masked_instance = false;
    scene.total_opaque_meshlets = 0u;
    scene.max_masked_bin_meshlets = 0u;
    scene.total_selected_meshlets = 0u;
    scene.total_visible_chunks = 0u;

    if (frame.meshes.empty()) {
        scene.instance_count = 0u;
        scene.tracked_objects.clear();
        scene.vsm_instances.clear();
        scene.staged_gpu_instances.clear();
        scene.staged_slot_map.clear();
        scene.pending_delta_copies.clear();
        scene.pending_delta_src = nullptr;
        scene.pending_instances_copy = false;
        scene.pending_slot_map_copy = false;
        scene.tlas.retire(device);
        scene.tlas_scratch.retire(device);
        scene.pending_tlas_build = false;
        scene.tlas_valid = false;
        scene.tlas_gpu_built = false;
        scene.tlas_scratch_gpu_used = false;
        scene.built_tlas_hash = 0u;
        return {};
    }

    const SceneFrustum primary_frustum(frame.camera.view, frame.camera.projection, frame.scene_viewport.width,
                                       frame.scene_viewport.height, frame.camera.far_depth);
    const math::Vector3 cam_eye = extract_camera_eye(frame.camera.view);

    const std::size_t count = frame.meshes.size();
    bool has_sun_contact_shadow = false;
    bool has_local_shadow_light = false;
    if (vsm_active && count > 256u) {
        for (const Light& l : frame.lights) {
            if (!l.casts_shadow || l.intensity <= 0.0f || math::max_component(l.color) <= 0.0f)
                continue;
            if (l.kind == LightKind::directional)
                has_sun_contact_shadow = true;
            else
                has_local_shadow_light = true;
        }
    }
    Array<GpuInstanceRecord> gpu_instances(count);
    scene.vsm_instances.assign(count, {});
    Array<std::uint32_t> selected_words((count + 31u) / 32u, 0u);
    Array<std::uint32_t> material_slot_map;
    material_slot_map.reserve(count * 2u);
    Array<nri::TopLevelInstance> tlas_instances;
    if (build_ray_scene)
        tlas_instances.reserve(count);

    Array<std::uint64_t> masked_bin_totals(std::max<std::uint32_t>(1u, masked_bin_count), 0u);
    std::uint64_t opaque_meshlet_total = 0u;
    std::uint64_t selected_meshlet_total = 0u;
    std::uint64_t visible_chunk_total = 0u;
    std::size_t seen_unique_keys = 0u;
    Array<std::uint64_t> non_opaque_words;
    Array<std::uint64_t> discarded_words;

    std::uint64_t tlas_hash = fnv_mix(0xcbf29ce484222325ull, geo_pool.layout_epoch());

    for (std::size_t i = 0; i < count; ++i) {
        if ((i & 63u) == 0u && stop_requested && (*stop_requested)()) {
            out_cancelled = true;
            return {};
        }

        const MeshInstance& m = frame.meshes[i];
        const auto normal_tf = math::normal_transform({m.world[0], m.world[1], m.world[2], m.world[4], m.world[5],
                                                       m.world[6], m.world[8], m.world[9], m.world[10]});
        if (!normal_tf) {
            return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                              normal_tf.error() == math::NormalTransformFailure::singular
                                                  ? "Mesh world matrix is singular"
                                                  : "Mesh normal transform cannot be represented by GPU floats"));
        }

        ATELIER_TRY_BIND(geo_entry, geo_pool.resolve(device, m.geometry, current_sequence));
        const CookedMeshSummary& sum = (*geo_entry)->summary;

        const bool frustum_vis = primary_frustum.visible(m.geometry->bounds(), m.world);
        ++stats.scene_instances;
        if (!frustum_vis)
            ++stats.scene_culled_instances;

        if (m.selected) {
            selected_words[i >> 5u] |= (1u << (i & 31u));
            scene.any_selected = true;
        }

        const std::uint32_t mat_offset = static_cast<std::uint32_t>(material_slot_map.size());
        const std::size_t slot_count = m.material_slots->size();
        non_opaque_words.assign((slot_count + 63u) / 64u, 0u);
        discarded_words.assign((slot_count + 63u) / 64u, 0u);
        bool any_non_opaque_slot = false;
        bool any_discarded_slot = false;
        for (std::size_t s = 0; s < slot_count; ++s) {
            const std::uint32_t surf_idx = (*m.material_slots)[s];
            material_slot_map.add(surf_idx);
            if (surf_idx < surface_records.size()) {
                const std::uint32_t sflags = surface_records[surf_idx].flags;
                if ((sflags & 8u) != 0u) {
                    discarded_words[s / 64u] |= (1ull << (s % 64u));
                    any_discarded_slot = true;
                } else if ((sflags & 1u) != 0u) {
                    non_opaque_words[s / 64u] |= (1ull << (s % 64u));
                    any_non_opaque_slot = true;
                    scene.any_masked_instance = true;
                }
            }
        }

        if (frustum_vis) {
            const geometry::MeshLodLevel& l0 = sum.lods[0];
            visible_chunk_total += (l0.meshlet_count + 63u) / 64u;
            if (m.selected)
                selected_meshlet_total += l0.meshlet_count;

            for (std::uint32_t p = 0u; p < l0.part_count; ++p) {
                const std::uint32_t p_mls = sum.lod0_part_meshlets[p];
                const std::uint32_t m_slot = sum.lod_parts[l0.first_part + p].material_slot;
                const std::uint32_t surf_idx = (m_slot < slot_count) ? (*m.material_slots)[m_slot] : 0u;
                if (surf_idx < surface_records.size()) {
                    const GpuSurfaceRecord& sr = surface_records[surf_idx];
                    if ((sr.flags & 8u) != 0u)
                        continue;
                    if ((sr.flags & 1u) != 0u && sr.masked_bin_index < masked_bin_totals.size()) {
                        masked_bin_totals[sr.masked_bin_index] += p_mls;
                        continue;
                    }
                }
                opaque_meshlet_total += p_mls;
            }
        }

        const std::uint64_t geo_cook_id = (*geo_entry)->cook_id;
        math::Matrix4 prev_world = m.world;
        bool has_prev_motion = false;
        bool is_dynamic_shadow = (m.stable_key == 0u);
        PersistentRenderScene::TrackedObject* tracked_ptr = nullptr;
        if (m.stable_key != 0u && !frame.camera_cut) {
            auto [tr_ptr, inserted] = scene.tracked_objects.try_emplace(m.stable_key);
            auto& tr = *tr_ptr;
            const bool seen_prev_tick = !inserted && (tr.last_seen_tick + 1u == scene.scene_tick);
            const bool duplicate_in_frame = !inserted && (tr.last_seen_tick == scene.scene_tick);
            if (!duplicate_in_frame) {
                ++seen_unique_keys;
                tracked_ptr = &tr;
            }
            if (seen_prev_tick && !duplicate_in_frame) {
                if (tr.geometry_cook_id == geo_cook_id) {
                    const bool moved = !matrix_same_bits(tr.curr_world, m.world);
                    if (moved) {
                        prev_world = tr.curr_world;
                        has_prev_motion = true;
                        tr.dynamic_cooldown = 3u;
                    } else if (tr.dynamic_cooldown > 0u) {
                        --tr.dynamic_cooldown;
                    }
                } else {
                    tr.dynamic_cooldown = 3u; // Live SOP geometry swap -> route to VSM CoW dynamic pool!
                }
            } else if (!inserted && !duplicate_in_frame) {
                tr.dynamic_cooldown = 0u;
            }
            is_dynamic_shadow = duplicate_in_frame || (tr.dynamic_cooldown > 0u);
            if (!duplicate_in_frame) {
                tr.curr_world = m.world;
                tr.geometry_cook_id = geo_cook_id;
                tr.last_seen_tick = scene.scene_tick;
            }
        } else if (m.stable_key != 0u && frame.camera_cut) {
            auto [tr_ptr, inserted] = scene.tracked_objects.try_emplace(m.stable_key);
            auto& tr = *tr_ptr;
            const bool duplicate_in_frame = !inserted && (tr.last_seen_tick == scene.scene_tick);
            if (!duplicate_in_frame) {
                ++seen_unique_keys;
                tracked_ptr = &tr;
                tr.curr_world = m.world;
                tr.geometry_cook_id = geo_cook_id;
                tr.last_seen_tick = scene.scene_tick;
                tr.dynamic_cooldown = 0u;
            } else {
                is_dynamic_shadow = true;
            }
        }

        const math::Bounds inst_bounds = transform_aabb(m.geometry->bounds(), m.world);
        const math::Vector3 b_center = (inst_bounds.minimum + inst_bounds.maximum) * 0.5f;
        const math::Vector3 b_ext = (inst_bounds.maximum - inst_bounds.minimum) * 0.5f;
        const float b_radius = std::sqrt(math::dot(b_ext, b_ext));
        const math::Vector3 d_cam_v = b_center - cam_eye;
        const math::Vector3 d_foc_v = b_center - frame.lighting_focus;
        const float d_cam = std::max(0.0f, std::sqrt(math::dot(d_cam_v, d_cam_v)) - b_radius);
        const float d_foc = std::max(0.0f, std::sqrt(math::dot(d_foc_v, d_foc_v)) - b_radius);
        const float d_rel = std::min(d_cam, d_foc);

        // Step 5 §2.2 & §3.3 3-Zone Forest TLAS & VSM Clutter Admission:
        bool casts_vsm_shadow = true;
        bool cull_from_tlas = false;
        if (vsm_active && count > 256u) {
            if (has_sun_contact_shadow && !has_local_shadow_light && b_radius < 0.25f) {
                casts_vsm_shadow = false;
                cull_from_tlas = true;
            } else if (b_radius < 16.0f) {
                const bool was_far = tracked_ptr ? tracked_ptr->in_tlas_far_cull : false;
                cull_from_tlas = was_far ? (d_rel > 78.0f) : (d_rel > 82.0f);
            }
        }
        if (tracked_ptr)
            tracked_ptr->in_tlas_far_cull = cull_from_tlas;

        std::uint64_t static_sig = fnv_mix(0xcbf29ce484222325ull, geo_cook_id);
        static_sig = fnv_mix_bytes(static_sig, m.world.elements.data(), sizeof(m.world.elements));
        if (any_non_opaque_slot || any_discarded_slot) {
            static_sig =
                fnv_mix_bytes(static_sig, m.material_slots->data(), m.material_slots->size() * sizeof(std::uint32_t));
        }
        scene.vsm_instances[i] = VsmInstanceMeta{
            .world_bounds = inst_bounds,
            .static_signature = static_sig,
            .lod0_meshlets = sum.lods[0].meshlet_count,
            .casts_vsm_shadow = casts_vsm_shadow,
            .is_dynamic_shadow = is_dynamic_shadow,
            .has_masked_slot = any_non_opaque_slot,
        };

        std::uint32_t part_base = 0u;
        if (build_ray_scene && !cull_from_tlas) {
            bool use_proxy_blas = false;
            if (!any_non_opaque_slot && !any_discarded_slot && sum.lod_count >= 3u && count > 256u &&
                b_radius < 16.0f) {
                const bool was_zone2 = tracked_ptr ? tracked_ptr->in_zone2_proxy : false;
                use_proxy_blas = was_zone2 ? (d_rel > 24.0f) : (d_rel > 26.0f);
            }
            if (tracked_ptr)
                tracked_ptr->in_zone2_proxy = use_proxy_blas;

            ATELIER_TRY_BIND(blas_pair, geo_pool.ensure_blas(device, **geo_entry, non_opaque_words, discarded_words,
                                                             use_proxy_blas));
            part_base = blas_pair->second;

            nri::TopLevelInstance tli{};
            tli.transform[0][0] = m.world[0];
            tli.transform[0][1] = m.world[4];
            tli.transform[0][2] = m.world[8];
            tli.transform[0][3] = m.world[12];
            tli.transform[1][0] = m.world[1];
            tli.transform[1][1] = m.world[5];
            tli.transform[1][2] = m.world[9];
            tli.transform[1][3] = m.world[13];
            tli.transform[2][0] = m.world[2];
            tli.transform[2][1] = m.world[6];
            tli.transform[2][2] = m.world[10];
            tli.transform[2][3] = m.world[14];
            tli.instanceId = static_cast<std::uint32_t>(i) & 0xFFFFFFu;
            tli.mask = 0xFFu;
            tli.shaderBindingTableRecordOffset = 0u;
            nri::TopLevelInstanceBits inst_flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
            if (normal_tf->mirror < 0.0f)
                inst_flags |= nri::TopLevelInstanceBits::TRIANGLE_FLIP_FACING;
            if (!any_non_opaque_slot)
                inst_flags |= nri::TopLevelInstanceBits::FORCE_OPAQUE;
            tli.flags = inst_flags;
            tli.accelerationStructureHandle = blas_pair->first;
            tlas_instances.add(tli);

            tlas_hash = fnv_mix_bytes(tlas_hash, &tli, sizeof(tli));
            tlas_hash = fnv_mix(tlas_hash, (std::uint64_t(part_base) << 32u) | mat_offset);
            tlas_hash = fnv_mix(tlas_hash, (*geo_entry)->mesh_pool_index);
        }

        GpuInstanceRecord rec{};
        rec.world = m.world.elements;
        rec.prev_world = prev_world.elements;
        rec.normal_row0 = {normal_tf->matrix[0], normal_tf->matrix[1], normal_tf->matrix[2]};
        rec.tangent_sign = normal_tf->mirror;
        rec.normal_row1 = {normal_tf->matrix[3], normal_tf->matrix[4], normal_tf->matrix[5]};
        rec.mesh_pool_index = (*geo_entry)->mesh_pool_index;
        rec.normal_row2 = {normal_tf->matrix[6], normal_tf->matrix[7], normal_tf->matrix[8]};
        rec.material_slot_offset = mat_offset;
        rec.part_table_base = part_base;
        rec.flags = (has_prev_motion ? instance_flag_has_prev_motion : 0u) |
                    (frustum_vis ? instance_flag_frustum_visible : 0u) |
                    (normal_tf->mirror < 0.0f ? instance_flag_neg_det : 0u) |
                    (casts_vsm_shadow ? instance_flag_casts_vsm_shadow : 0u) |
                    (is_dynamic_shadow ? instance_flag_dynamic_shadow : 0u);
        rec.stable_key_lo = static_cast<std::uint32_t>(m.stable_key & 0xFFFFFFFFu);
        rec.stable_key_hi = static_cast<std::uint32_t>(m.stable_key >> 32u);
        gpu_instances[i] = rec;
    }

    if (seen_unique_keys < scene.tracked_objects.size()) {
        scene.tracked_objects.remove_if(
            [&](std::uint64_t, const PersistentRenderScene::TrackedObject& tr) {
                return tr.last_seen_tick != scene.scene_tick;
            });
    }

    std::uint64_t max_masked_bin = 0u;
    for (std::uint64_t bm : masked_bin_totals)
        max_masked_bin = std::max(max_masked_bin, bm);

    scene.instance_count = static_cast<std::uint32_t>(count);
    scene.total_opaque_meshlets = static_cast<std::uint32_t>(std::min<std::uint64_t>(opaque_meshlet_total, 0x3FFFFFu));
    scene.max_masked_bin_meshlets = static_cast<std::uint32_t>(std::min<std::uint64_t>(max_masked_bin, 0x3FFFFFu));
    scene.total_selected_meshlets =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(selected_meshlet_total, 0x3FFFFFu));
    scene.total_visible_chunks = static_cast<std::uint32_t>(std::min<std::uint64_t>(visible_chunk_total, 0x3FFFFFu));

    if (material_slot_map.empty())
        material_slot_map.add(0u);

    ATELIER_TRY_BIND(sel_w, scene.selected_mask_slots.write(device, slot_index, selected_words));
    scene.selected_mask_srv = sel_w->second;

    bool inst_realloc = false;
    ATELIER_TRY(ensure_scene_structured_buffer(
        device, scene.gpu_instance_buffer, gpu_instances.size() * sizeof(GpuInstanceRecord),
        static_cast<std::uint32_t>(sizeof(GpuInstanceRecord)), "scene_instances", inst_realloc));
    if (inst_realloc) {
        scene.cached_gpu_instances.clear();
        scene.gpu_instances_ever_written = false;
    }

    bool slot_realloc = false;
    ATELIER_TRY(ensure_scene_structured_buffer(
        device, scene.gpu_material_slot_map, material_slot_map.size() * sizeof(std::uint32_t),
        static_cast<std::uint32_t>(sizeof(std::uint32_t)), "scene_slot_map", slot_realloc));
    if (slot_realloc) {
        scene.cached_slot_map.clear();
        scene.gpu_slot_map_ever_written = false;
    }

    scene.instances_srv = scene.gpu_instance_buffer.srv_bindless;
    scene.material_slot_map_srv = scene.gpu_material_slot_map.srv_bindless;
    scene.pending_delta_copies.clear();
    scene.pending_delta_src = nullptr;
    scene.pending_instances_copy = false;
    scene.pending_slot_map_copy = false;

    Array<std::byte> delta_bytes;
    {
        std::size_t idx = 0u;
        while (idx < gpu_instances.size()) {
            const bool same =
                (idx < scene.cached_gpu_instances.size()) &&
                (std::memcmp(&gpu_instances[idx], &scene.cached_gpu_instances[idx], sizeof(GpuInstanceRecord)) == 0);
            if (same) {
                ++idx;
                continue;
            }
            const std::size_t run_start = idx;
            ++idx;
            while (idx < gpu_instances.size()) {
                const bool next_same = (idx < scene.cached_gpu_instances.size()) &&
                                       (std::memcmp(&gpu_instances[idx], &scene.cached_gpu_instances[idx],
                                                    sizeof(GpuInstanceRecord)) == 0);
                if (next_same)
                    break;
                ++idx;
            }
            const std::uint64_t byte_len = (idx - run_start) * sizeof(GpuInstanceRecord);
            const std::uint64_t src_off = align_up_bytes(delta_bytes.size(), 16u);
            delta_bytes.resize(static_cast<std::size_t>(src_off + byte_len));
            std::memcpy(delta_bytes.data() + src_off, &gpu_instances[run_start], static_cast<std::size_t>(byte_len));
            scene.pending_delta_copies.add({
                .dst = scene.gpu_instance_buffer.buffer,
                .dst_offset = run_start * sizeof(GpuInstanceRecord),
                .src_offset = src_off,
                .byte_size = byte_len,
            });
            scene.pending_instances_copy = true;
        }
    }
    {
        std::size_t idx = 0u;
        while (idx < material_slot_map.size()) {
            const bool same =
                (idx < scene.cached_slot_map.size()) && (material_slot_map[idx] == scene.cached_slot_map[idx]);
            if (same) {
                ++idx;
                continue;
            }
            const std::size_t run_start = idx;
            ++idx;
            while (idx < material_slot_map.size()) {
                const bool next_same =
                    (idx < scene.cached_slot_map.size()) && (material_slot_map[idx] == scene.cached_slot_map[idx]);
                if (next_same)
                    break;
                ++idx;
            }
            const std::uint64_t byte_len = (idx - run_start) * sizeof(std::uint32_t);
            const std::uint64_t src_off = align_up_bytes(delta_bytes.size(), 16u);
            delta_bytes.resize(static_cast<std::size_t>(src_off + byte_len));
            std::memcpy(delta_bytes.data() + src_off, &material_slot_map[run_start],
                        static_cast<std::size_t>(byte_len));
            scene.pending_delta_copies.add({
                .dst = scene.gpu_material_slot_map.buffer,
                .dst_offset = run_start * sizeof(std::uint32_t),
                .src_offset = src_off,
                .byte_size = byte_len,
            });
            scene.pending_slot_map_copy = true;
        }
    }

    if (!delta_bytes.empty()) {
        ATELIER_TRY_BIND(delta_w, scene.delta_upload_slots.write(device, slot_index, delta_bytes, true,
                                                                 nri::BufferUsageBits::NONE, false));
        scene.pending_delta_src = delta_w->first;
    }
    scene.staged_gpu_instances = std::move(gpu_instances);
    scene.staged_slot_map = std::move(material_slot_map);

    scene.pending_tlas_build = false;
    if (build_ray_scene) {
        const bool need_rebuild =
            !scene.tlas_valid || !scene.tlas_gpu_built || !scene.tlas.as || (tlas_hash != scene.built_tlas_hash);
        if (need_rebuild) {
            const auto& core = rhi::BackendAccess::core(device);
            const auto& rt = rhi::BackendAccess::ray_tracing(device);
            nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);

            const std::uint32_t tlas_num = static_cast<std::uint32_t>(tlas_instances.size());
            const std::uint32_t needed_cap = std::max<std::uint32_t>(64u, std::bit_ceil(std::max(1u, tlas_num)));
            if (!scene.tlas.as || scene.tlas.capacity < needed_cap) {
                scene.tlas.retire(device);
                scene.tlas_gpu_built = false;
                bool tlas_ok = false;
                struct TlasGuard {
                    OwnedAccelerationStructure* tlas;

                    rhi::Device* dev;

                    bool* ok;

                    ~TlasGuard() {
                        if (!*ok)
                            tlas->retire(*dev);
                    }
                } tlas_guard{&scene.tlas, &device, &tlas_ok};

                nri::AccelerationStructureDesc as_desc{};
                as_desc.type = nri::AccelerationStructureType::TOP_LEVEL;
                as_desc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
                as_desc.geometryOrInstanceNum = needed_cap;

                nri::MemoryDesc mem_desc{};
                rt.GetAccelerationStructureMemoryDesc2(nri_dev, as_desc, nri::MemoryLocation::DEVICE, mem_desc);
                scene.tlas.byte_size = mem_desc.size;
                scene.tlas.capacity = needed_cap;

                ATELIER_TRY(check_nri(rt.CreateCommittedAccelerationStructure(nri_dev, nri::MemoryLocation::DEVICE,
                                                                              0.0f, as_desc, scene.tlas.as),
                                      rhi::ErrorCode::out_of_memory, "TLAS allocation failed", true));
                ATELIER_TRY(check_nri(rt.CreateAccelerationStructureDescriptor(*scene.tlas.as, scene.tlas.srv),
                                      rhi::ErrorCode::creation_failed, "TLAS SRV descriptor failed", true));
                auto b = rhi::BackendAccess::bindless(device).allocate_resource(scene.tlas.srv);
                if (!b)
                    return std::unexpected(Error{b.error(), true});
                scene.tlas.srv_bindless = *b;
                scene.tlas.handle = rt.GetAccelerationStructureHandle(*scene.tlas.as);
                tlas_ok = true;
            }

            const std::uint64_t scratch_sz =
                std::max<std::uint64_t>(256u, rt.GetAccelerationStructureBuildScratchBufferSize(*scene.tlas.as));
            if (!scene.tlas_scratch.buffer || scene.tlas_scratch.capacity < scratch_sz) {
                scene.tlas_scratch.retire(device);
                scene.tlas_scratch_gpu_used = false;
                nri::BufferDesc s_desc{};
                s_desc.size = std::bit_ceil(scratch_sz);
                s_desc.usage = nri::BufferUsageBits::SCRATCH;
                ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, s_desc,
                                                                 scene.tlas_scratch.buffer),
                                      rhi::ErrorCode::out_of_memory, "TLAS scratch allocation failed", true));
                scene.tlas_scratch.capacity = s_desc.size;
            }

            ATELIER_TRY_BIND(
                ti_w, scene.tlas_instance_slots.write(device, slot_index, tlas_instances, false,
                                                      nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT, false));
            scene.pending_tlas_instance_buf = ti_w->first;
            scene.pending_tlas_instance_num = tlas_num;
            scene.pending_tlas_build = true;
            scene.tlas_valid = true;
            scene.built_tlas_hash = tlas_hash;
        }
    }

    return {};
}

void RenderSceneTable::record_scene_uploads(rhi::Device& device, nri::CommandBuffer& cmd,
                                            PersistentRenderScene& scene) noexcept {
    if (scene.pending_delta_copies.empty() || !scene.pending_delta_src)
        return;
    const auto& core = rhi::BackendAccess::core(device);

    std::array<nri::BufferBarrierDesc, 2> pre_bars{};
    std::uint32_t pre_num = 0u;
    if (scene.pending_instances_copy && scene.gpu_instance_buffer.buffer) {
        pre_bars[pre_num].buffer = scene.gpu_instance_buffer.buffer;
        pre_bars[pre_num].before = scene.gpu_instances_ever_written
                                       ? nri::AccessStage{nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS}
                                       : nri::AccessStage{nri::AccessBits::NONE, nri::StageBits::NONE};
        pre_bars[pre_num].after = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        ++pre_num;
    }
    if (scene.pending_slot_map_copy && scene.gpu_material_slot_map.buffer) {
        pre_bars[pre_num].buffer = scene.gpu_material_slot_map.buffer;
        pre_bars[pre_num].before = scene.gpu_slot_map_ever_written
                                       ? nri::AccessStage{nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS}
                                       : nri::AccessStage{nri::AccessBits::NONE, nri::StageBits::NONE};
        pre_bars[pre_num].after = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        ++pre_num;
    }
    if (pre_num > 0u) {
        nri::BarrierDesc bg{.globals = nullptr,
                            .globalNum = 0u,
                            .buffers = pre_bars.data(),
                            .bufferNum = pre_num,
                            .textures = nullptr,
                            .textureNum = 0u};
        core.CmdBarrier(cmd, bg);
    }

    for (const auto& sp : scene.pending_delta_copies) {
        core.CmdCopyBuffer(cmd, *sp.dst, sp.dst_offset, *scene.pending_delta_src, sp.src_offset, sp.byte_size);
    }

    std::array<nri::BufferBarrierDesc, 2> post_bars{};
    std::uint32_t post_num = 0u;
    if (scene.pending_instances_copy && scene.gpu_instance_buffer.buffer) {
        post_bars[post_num].buffer = scene.gpu_instance_buffer.buffer;
        post_bars[post_num].before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        post_bars[post_num].after = {nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS};
        ++post_num;
    }
    if (scene.pending_slot_map_copy && scene.gpu_material_slot_map.buffer) {
        post_bars[post_num].buffer = scene.gpu_material_slot_map.buffer;
        post_bars[post_num].before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        post_bars[post_num].after = {nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL_SHADERS};
        ++post_num;
    }
    if (post_num > 0u) {
        nri::BarrierDesc bg{.globals = nullptr,
                            .globalNum = 0u,
                            .buffers = post_bars.data(),
                            .bufferNum = post_num,
                            .textures = nullptr,
                            .textureNum = 0u};
        core.CmdBarrier(cmd, bg);
    }
}

void RenderSceneTable::record_tlas_build(rhi::Device& device, nri::CommandBuffer& cmd,
                                         PersistentRenderScene& scene) noexcept {
    if (!scene.pending_tlas_build || !scene.tlas.as)
        return;
    const auto& core = rhi::BackendAccess::core(device);
    const auto& rt = rhi::BackendAccess::ray_tracing(device);

    std::array<nri::BufferBarrierDesc, 2> pre_bars{};
    std::uint32_t pre_num = 0u;
    if (scene.tlas_gpu_built) {
        pre_bars[pre_num].buffer = rt.GetAccelerationStructureBuffer(*scene.tlas.as);
        pre_bars[pre_num].before = {nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::COMPUTE_SHADER};
        pre_bars[pre_num].after = {nri::AccessBits::ACCELERATION_STRUCTURE_WRITE,
                                   nri::StageBits::ACCELERATION_STRUCTURE};
        ++pre_num;
    }
    if (scene.tlas_scratch_gpu_used && scene.tlas_scratch.buffer) {
        pre_bars[pre_num].buffer = scene.tlas_scratch.buffer;
        pre_bars[pre_num].before = {nri::AccessBits::SCRATCH_BUFFER, nri::StageBits::ACCELERATION_STRUCTURE};
        pre_bars[pre_num].after = {nri::AccessBits::SCRATCH_BUFFER, nri::StageBits::ACCELERATION_STRUCTURE};
        ++pre_num;
    }
    if (pre_num > 0u) {
        nri::BarrierDesc bg{.globals = nullptr,
                            .globalNum = 0u,
                            .buffers = pre_bars.data(),
                            .bufferNum = pre_num,
                            .textures = nullptr,
                            .textureNum = 0u};
        core.CmdBarrier(cmd, bg);
    }

    nri::BuildTopLevelAccelerationStructureDesc build_desc{};
    build_desc.dst = scene.tlas.as;
    build_desc.instanceBuffer = scene.pending_tlas_instance_buf;
    build_desc.instanceOffset = 0u;
    build_desc.instanceNum = scene.pending_tlas_instance_num;
    build_desc.scratchBuffer = scene.tlas_scratch.buffer;
    build_desc.scratchOffset = 0u;
    rt.CmdBuildTopLevelAccelerationStructures(cmd, &build_desc, 1u);

    nri::BufferBarrierDesc post_bar{};
    post_bar.buffer = rt.GetAccelerationStructureBuffer(*scene.tlas.as);
    post_bar.before = {nri::AccessBits::ACCELERATION_STRUCTURE_WRITE, nri::StageBits::ACCELERATION_STRUCTURE};
    post_bar.after = {nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::COMPUTE_SHADER};
    nri::BarrierDesc post_bg{.globals = nullptr,
                             .globalNum = 0u,
                             .buffers = &post_bar,
                             .bufferNum = 1u,
                             .textures = nullptr,
                             .textureNum = 0u};
    core.CmdBarrier(cmd, post_bg);
}

void RenderSceneTable::commit_submitted(PersistentRenderScene& scene) noexcept {
    if (scene.pending_instances_copy)
        scene.gpu_instances_ever_written = true;
    if (scene.pending_slot_map_copy)
        scene.gpu_slot_map_ever_written = true;
    scene.cached_gpu_instances = std::move(scene.staged_gpu_instances);
    scene.cached_slot_map = std::move(scene.staged_slot_map);
    scene.pending_delta_copies.clear();
    scene.pending_delta_src = nullptr;
    scene.pending_instances_copy = false;
    scene.pending_slot_map_copy = false;
}

void RenderSceneTable::clear(rhi::Device& device) noexcept {
    for (auto& sc : scenes_)
        sc->retire(device);
    scenes_.clear();
}

} // namespace atelier::renderer::detail

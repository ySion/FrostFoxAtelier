#include "geometry_pool.hpp"

#include <algorithm>
#include <cmath>

namespace atelier::renderer::detail {

void GpuGeometryEntry::retire_all(rhi::Device& device) noexcept {
    positions.retire(device);
    attributes.retire(device);
    meshlets.retire(device);
    meshlet_vertices.retire(device);
    meshlet_triangles.retire(device);
    blas_indices.retire(device);
    face_meta.retire(device);
    for (auto& bv : lod0_blas_variants)
        bv.blas.retire(device);
    lod0_blas_variants.clear();
    proxy_blas.retire(device);
    proxy_part_table_base = 0u;
    logical_bytes = 0u;
}

GpuMeshDescriptor GeometryPool::make_gpu_descriptor(const GpuGeometryEntry& e) noexcept {
    const CookedMeshSummary& s = e.summary;
    GpuMeshDescriptor desc{};
    desc.positions_srv = e.positions.srv_bindless;
    desc.attributes_srv = e.attributes.srv_bindless;
    desc.meshlets_srv = e.meshlets.srv_bindless;
    desc.meshlet_vertices_srv = e.meshlet_vertices.srv_bindless;
    desc.meshlet_triangles_srv = e.meshlet_triangles.srv_bindless;
    desc.blas_indices_srv = e.blas_indices.srv_bindless;
    desc.face_meta_srv = e.face_meta.srv_bindless;
    desc.lod_count = s.lod_count;
    for (std::uint32_t l = 0u; l < s.lod_count; ++l) {
        desc.lod_first_meshlet[l] = s.lods[l].first_meshlet;
        desc.lod_meshlet_count[l] = s.lods[l].meshlet_count;
        desc.lod_error[l] = s.lods[l].error;
    }
    desc.bounds_center = s.bounds_center;
    desc.bounds_radius = s.bounds_radius;
    return desc;
}

void GeometryPool::begin_frame() noexcept {
    for (auto [k, ep] : entries_) {
        (void)k;
        ep->used_this_frame = false;
    }
}

Result<GpuGeometryEntry*> GeometryPool::resolve(rhi::Device& device, const geometry::MeshRef& mesh,
                                                std::uint64_t current_sequence) {
    auto [slot_ptr, inserted] = entries_.try_emplace(mesh.get());
    if (!*slot_ptr)
        *slot_ptr = std::make_unique<GpuGeometryEntry>();
    GpuGeometryEntry& entry = **slot_ptr;
    const bool same_owner = !inserted && !entry.weak_source.owner_before(mesh) && !mesh.owner_before(entry.weak_source);
    if (!inserted && same_owner && entry.positions.buffer) {
        entry.used_this_frame = true;
        entry.last_used_sequence = current_sequence;
        return &entry;
    }

    const bool replacing_slot = !inserted && entry.positions.buffer != nullptr;
    const std::uint32_t existing_pool_idx = entry.mesh_pool_index;
    if (replacing_slot) {
        total_logical_bytes_ -= std::min(total_logical_bytes_, entry.logical_bytes);
        entry.retire_all(device);
        ++layout_epoch_;
    }

    const auto cooked = geometry::load_or_cook_meshlets(*mesh, cache_dir_);
    const auto& c = *cooked;

    auto cleanup_on_fail = [&]() noexcept {
        entry.retire_all(device);
        if (!replacing_slot)
            entries_.remove(mesh.get());
    };

    UploadBatch batch(device, false);
    auto pos = batch.stage_buffer(c.positions.data(), c.positions.size() * sizeof(geometry::PackedPosition),
                                  sizeof(geometry::PackedPosition), false,
                                  nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT, "geo_positions");
    if (!pos) {
        cleanup_on_fail();
        return std::unexpected(pos.error());
    }
    entry.positions = std::move(*pos);

    auto attr = batch.stage_buffer(c.attributes.data(), c.attributes.size() * sizeof(geometry::PackedVertexAttributes),
                                   sizeof(geometry::PackedVertexAttributes), false, nri::BufferUsageBits::NONE,
                                   "geo_attributes");
    if (!attr) {
        cleanup_on_fail();
        return std::unexpected(attr.error());
    }
    entry.attributes = std::move(*attr);

    auto ml = batch.stage_buffer(c.meshlets.data(), c.meshlets.size() * sizeof(geometry::GpuMeshlet),
                                 sizeof(geometry::GpuMeshlet), false, nri::BufferUsageBits::NONE, "geo_meshlets");
    if (!ml) {
        cleanup_on_fail();
        return std::unexpected(ml.error());
    }
    entry.meshlets = std::move(*ml);

    auto mv = batch.stage_buffer(c.meshlet_vertices.data(), c.meshlet_vertices.size() * sizeof(std::uint32_t),
                                 sizeof(std::uint32_t), false, nri::BufferUsageBits::NONE, "geo_ml_verts");
    if (!mv) {
        cleanup_on_fail();
        return std::unexpected(mv.error());
    }
    entry.meshlet_vertices = std::move(*mv);

    auto mt = batch.stage_buffer(c.meshlet_triangles.data(), c.meshlet_triangles.size(), 0u, true,
                                 nri::BufferUsageBits::NONE, "geo_ml_tris");
    if (!mt) {
        cleanup_on_fail();
        return std::unexpected(mt.error());
    }
    entry.meshlet_triangles = std::move(*mt);

    auto bi = batch.stage_buffer(
        c.blas_indices.data(), c.blas_indices.size() * sizeof(std::uint32_t), sizeof(std::uint32_t), false,
        nri::BufferUsageBits::INDEX | nri::BufferUsageBits::ACCELERATION_STRUCTURE_BUILD_INPUT, "geo_blas_idx");
    if (!bi) {
        cleanup_on_fail();
        return std::unexpected(bi.error());
    }
    entry.blas_indices = std::move(*bi);

    auto fm = batch.stage_buffer(c.face_meta.data(), c.face_meta.size() * sizeof(std::uint32_t), sizeof(std::uint32_t),
                                 false, nri::BufferUsageBits::NONE, "geo_face_meta");
    if (!fm) {
        cleanup_on_fail();
        return std::unexpected(fm.error());
    }
    entry.face_meta = std::move(*fm);

    if (auto uploaded = batch.execute(); !uploaded) {
        cleanup_on_fail();
        return std::unexpected(uploaded.error());
    }

    entry.weak_source = mesh;
    entry.cook_id = next_cook_id_++;
    CookedMeshSummary sum{};
    sum.vertex_count = static_cast<std::uint32_t>(c.positions.size());
    sum.material_slot_count = static_cast<std::uint32_t>(std::max<std::size_t>(1u, mesh->materials().size()));
    sum.lod_count = c.lod_count;
    sum.lods = c.lods;
    sum.lod_parts = c.lod_parts;

    const math::Bounds& b = mesh->bounds();
    const math::Vector3 center = (b.minimum + b.maximum) * 0.5f;
    const math::Vector3 ext = (b.maximum - b.minimum) * 0.5f;
    sum.bounds_center = {center[0], center[1], center[2]};
    sum.bounds_radius = std::sqrt(math::dot(ext, ext));

    const geometry::MeshLodLevel& l0 = c.lods[0];
    sum.lod0_part_meshlets.assign(l0.part_count, 0u);
    for (std::uint32_t li = 0u; li < c.lod_count; ++li) {
        const geometry::MeshLodLevel& lv = c.lods[li];
        Array<std::uint32_t> lv_counts(lv.part_count, 0u);
        std::uint32_t cur_part = 0u;
        for (std::uint32_t m = 0u; m < lv.meshlet_count; ++m) {
            const std::uint32_t tri_base = c.meshlets[lv.first_meshlet + m].global_tri_base;
            while (cur_part + 1u < lv.part_count) {
                const geometry::PartRange& pr = c.lod_parts[lv.first_part + cur_part];
                if (tri_base < pr.global_tri_base + pr.triangle_count)
                    break;
                ++cur_part;
            }
            if (cur_part < lv.part_count)
                ++lv_counts[cur_part];
        }
        const std::uint32_t common_parts = std::min<std::uint32_t>(l0.part_count, lv.part_count);
        for (std::uint32_t p = 0u; p < common_parts; ++p)
            sum.lod0_part_meshlets[p] = std::max(sum.lod0_part_meshlets[p], lv_counts[p]);
    }
    entry.summary = std::move(sum);

    entry.logical_bytes = entry.positions.capacity + entry.attributes.capacity + entry.meshlets.capacity +
                          entry.meshlet_vertices.capacity + entry.meshlet_triangles.capacity +
                          entry.blas_indices.capacity + entry.face_meta.capacity;
    total_logical_bytes_ += entry.logical_bytes;

    if (replacing_slot && existing_pool_idx < mesh_descriptors_.size()) {
        entry.mesh_pool_index = existing_pool_idx;
        mesh_descriptors_[existing_pool_idx] = make_gpu_descriptor(entry);
    } else {
        entry.mesh_pool_index = static_cast<std::uint32_t>(mesh_descriptors_.size());
        mesh_descriptors_.add(make_gpu_descriptor(entry));
    }

    entry.used_this_frame = true;
    entry.last_used_sequence = current_sequence;
    return &entry;
}

Result<OwnedAccelerationStructure>
GeometryPool::build_blas_for_lod(rhi::Device& device, const GpuGeometryEntry& entry, std::uint32_t lod_idx,
                                 Span<const std::uint64_t> non_opaque_slot_words,
                                 Span<const std::uint64_t> discarded_slot_words, bool force_all_opaque,
                                 std::uint32_t& out_part_base) {
    const CookedMeshSummary& s = entry.summary;
    const geometry::MeshLodLevel& level = s.lods[lod_idx];
    const std::size_t prev_part_count = part_tri_bases_.size();
    out_part_base = static_cast<std::uint32_t>(prev_part_count);

    Array<nri::BottomLevelGeometryDesc> geoms;
    geoms.reserve(std::max<std::uint32_t>(1u, level.part_count));
    for (std::uint32_t p = 0u; p < level.part_count; ++p) {
        const geometry::PartRange& pr = s.lod_parts[level.first_part + p];
        part_tri_bases_.add(pr.global_tri_base);

        const std::size_t word_idx = pr.material_slot / 64u;
        const std::uint64_t bit = 1ull << (pr.material_slot % 64u);
        const bool is_discarded = !force_all_opaque && (word_idx < discarded_slot_words.size()) &&
                                  ((discarded_slot_words[word_idx] & bit) != 0u);
        const bool is_non_opaque = !force_all_opaque && !is_discarded && (word_idx < non_opaque_slot_words.size()) &&
                                   ((non_opaque_slot_words[word_idx] & bit) != 0u);
        nri::BottomLevelGeometryDesc gd{};
        gd.type = nri::BottomLevelGeometryType::TRIANGLES;
        gd.flags = is_non_opaque ? nri::BottomLevelGeometryBits::NO_DUPLICATE_ANY_HIT_INVOCATION
                                 : nri::BottomLevelGeometryBits::OPAQUE_GEOMETRY;
        gd.triangles.vertexBuffer = entry.positions.buffer;
        gd.triangles.vertexOffset = 0u;
        gd.triangles.vertexNum = s.vertex_count;
        gd.triangles.vertexStride = sizeof(geometry::PackedPosition);
        gd.triangles.vertexFormat = nri::Format::RGB32_SFLOAT;
        gd.triangles.indexBuffer = entry.blas_indices.buffer;
        gd.triangles.indexOffset = std::uint64_t(pr.global_tri_base) * 3u * sizeof(std::uint32_t);
        gd.triangles.indexNum = is_discarded ? 0u : (pr.triangle_count * 3u);
        gd.triangles.indexType = nri::IndexType::UINT32;
        geoms.add(gd);
    }

    const auto& core = rhi::BackendAccess::core(device);
    const auto& rt = rhi::BackendAccess::ray_tracing(device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);

    OwnedAccelerationStructure out{};
    nri::Buffer* scratch = nullptr;
    nri::CommandAllocator* cmd_alloc = nullptr;
    nri::CommandBuffer* cmd = nullptr;
    bool cmd_open = false;
    bool blas_ok = false;

    struct BlasCleanupGuard {
        const nri::CoreInterface* core;

        rhi::Device* device;

        OwnedAccelerationStructure* out;

        nri::Buffer** scratch;

        nri::CommandAllocator** alloc;

        nri::CommandBuffer** cmd;

        bool* open;

        Array<std::uint32_t>* parts;

        std::size_t prev_parts;

        bool* ok;

        ~BlasCleanupGuard() {
            if (*cmd) {
                if (*open)
                    static_cast<void>(core->EndCommandBuffer(**cmd));
                core->DestroyCommandBuffer(*cmd);
            }
            if (*alloc)
                core->DestroyCommandAllocator(*alloc);
            if (!*ok) {
                if (*scratch)
                    core->DestroyBuffer(*scratch);
                out->retire(*device);
                parts->resize(prev_parts);
            }
        }
    } guard{&core, &device, &out, &scratch, &cmd_alloc, &cmd, &cmd_open, &part_tri_bases_, prev_part_count, &blas_ok};

    nri::AccelerationStructureDesc as_desc{};
    as_desc.type = nri::AccelerationStructureType::BOTTOM_LEVEL;
    as_desc.flags = nri::AccelerationStructureBits::PREFER_FAST_TRACE;
    as_desc.geometryOrInstanceNum = static_cast<std::uint32_t>(geoms.size());
    as_desc.geometries = geoms.data();

    nri::MemoryDesc mem_desc{};
    rt.GetAccelerationStructureMemoryDesc2(nri_dev, as_desc, nri::MemoryLocation::DEVICE, mem_desc);
    out.byte_size = mem_desc.size;

    ATELIER_TRY(
        check_nri(rt.CreateCommittedAccelerationStructure(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, as_desc, out.as),
                  rhi::ErrorCode::out_of_memory, "BLAS allocation failed", true));

    const std::uint64_t scratch_sz =
        std::max<std::uint64_t>(256u, rt.GetAccelerationStructureBuildScratchBufferSize(*out.as));
    nri::BufferDesc s_desc{};
    s_desc.size = scratch_sz;
    s_desc.usage = nri::BufferUsageBits::SCRATCH;
    ATELIER_TRY(check_nri(core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::DEVICE, 0.0f, s_desc, scratch),
                          rhi::ErrorCode::out_of_memory, "BLAS scratch allocation failed", true));

    nri::Queue* gfx_q = rhi::BackendAccess::timeline(device).queue(rhi::QueueKind::graphics);
    ATELIER_TRY(check_nri(core.CreateCommandAllocator(*gfx_q, cmd_alloc), rhi::ErrorCode::creation_failed,
                          "BLAS CommandAllocator failed", true));
    ATELIER_TRY(check_nri(core.CreateCommandBuffer(*cmd_alloc, cmd), rhi::ErrorCode::creation_failed,
                          "BLAS CommandBuffer failed", true));
    ATELIER_TRY(check_nri(core.BeginCommandBuffer(*cmd, nullptr), rhi::ErrorCode::execution_failed,
                          "BLAS BeginCommandBuffer failed", true));
    cmd_open = true;

    nri::BuildBottomLevelAccelerationStructureDesc build_desc{};
    build_desc.dst = out.as;
    build_desc.geometries = geoms.data();
    build_desc.geometryNum = static_cast<std::uint32_t>(geoms.size());
    build_desc.scratchBuffer = scratch;
    build_desc.scratchOffset = 0u;
    rt.CmdBuildBottomLevelAccelerationStructures(*cmd, &build_desc, 1u);

    nri::BufferBarrierDesc bar{};
    bar.buffer = rt.GetAccelerationStructureBuffer(*out.as);
    bar.before = {nri::AccessBits::ACCELERATION_STRUCTURE_WRITE, nri::StageBits::ACCELERATION_STRUCTURE};
    bar.after = {nri::AccessBits::ACCELERATION_STRUCTURE_READ, nri::StageBits::COMPUTE_SHADER};
    nri::BarrierDesc b_group{};
    b_group.buffers = &bar;
    b_group.bufferNum = 1u;
    core.CmdBarrier(*cmd, b_group);
    ATELIER_TRY(
        check_nri(core.EndCommandBuffer(*cmd), rhi::ErrorCode::execution_failed, "BLAS EndCommandBuffer failed", true));
    cmd_open = false;

    auto& timeline = rhi::BackendAccess::timeline(device);
    auto sub = timeline.submit(rhi::QueueKind::graphics, Span{&cmd, 1u});
    if (!sub)
        return std::unexpected(Error{sub.error(), true});

    const rhi::FencePoint fp = timeline.current_point();
    rhi::BackendAccess::reclaimer(device).retire({
        .fence = fp,
        .buffer = std::exchange(scratch, nullptr),
        .command_buffer = std::exchange(cmd, nullptr),
        .command_allocator = std::exchange(cmd_alloc, nullptr),
    });

    const rhi::QueueKind comp_q = timeline.resolve_kind(rhi::QueueKind::compute);
    if (comp_q != rhi::QueueKind::graphics) {
        const std::pair<rhi::QueueKind, std::uint64_t> blas_sig{rhi::QueueKind::graphics, *sub};
        auto comp_wait = timeline.submit(comp_q, {}, Span{&blas_sig, 1u});
        if (!comp_wait)
            return std::unexpected(Error{comp_wait.error(), true});
    }

    out.handle = rt.GetAccelerationStructureHandle(*out.as);
    total_logical_bytes_ += out.byte_size;
    blas_ok = true;
    return out;
}

Result<std::pair<std::uint64_t, std::uint32_t>>
GeometryPool::ensure_blas(rhi::Device& device, GpuGeometryEntry& entry,
                          Span<const std::uint64_t> non_opaque_slot_words,
                          Span<const std::uint64_t> discarded_slot_words, bool want_proxy) {
    if (want_proxy && entry.summary.lod_count >= 3u) {
        if (!entry.proxy_blas.as) {
            const std::uint32_t proxy_lod = std::min<std::uint32_t>(2u, entry.summary.lod_count - 1u);
            ATELIER_TRY_ASSIGN(entry.proxy_blas,
                               build_blas_for_lod(device, entry, proxy_lod, {}, {}, true, entry.proxy_part_table_base));
            entry.logical_bytes += entry.proxy_blas.byte_size;
        }
        return std::pair{entry.proxy_blas.handle, entry.proxy_part_table_base};
    }

    for (const auto& v : entry.lod0_blas_variants) {
        if (v.blas.as && std::ranges::equal(v.non_opaque_slot_words, non_opaque_slot_words) &&
            std::ranges::equal(v.discarded_slot_words, discarded_slot_words))
            return std::pair{v.blas.handle, v.part_table_base};
    }

    GpuGeometryEntry::BlasVariant nv{};
    nv.non_opaque_slot_words.assign(non_opaque_slot_words);
    nv.discarded_slot_words.assign(discarded_slot_words);
    ATELIER_TRY_ASSIGN(nv.blas, build_blas_for_lod(device, entry, 0u, non_opaque_slot_words, discarded_slot_words,
                                                   false, nv.part_table_base));
    entry.logical_bytes += nv.blas.byte_size;
    const auto res = std::pair{nv.blas.handle, nv.part_table_base};
    entry.lod0_blas_variants.add(std::move(nv));
    return res;
}

Result<void> GeometryPool::commit_frame(rhi::Device& device, std::size_t slot_index) {
    if (mesh_descriptors_.empty())
        mesh_descriptors_.emplace();
    if (part_tri_bases_.empty())
        part_tri_bases_.add(0u);

    ATELIER_TRY_BIND(mt, mesh_table_slots_.write(device, slot_index, mesh_descriptors_));
    mesh_table_srv_ = mt->second;

    ATELIER_TRY_BIND(pt, part_tri_base_slots_.write(device, slot_index, part_tri_bases_));
    part_tri_base_srv_ = pt->second;
    return {};
}

void GeometryPool::sweep_unused(rhi::Device& device, std::uint64_t current_sequence) noexcept {
    const std::size_t removed =
        entries_.remove_if([&](const geometry::Mesh*, const std::unique_ptr<GpuGeometryEntry>& ep) {
            auto& e = *ep;
            const bool keep_warm = !e.weak_source.expired() && (current_sequence <= e.last_used_sequence + 64u);
            if (e.used_this_frame || keep_warm)
                return false;
            total_logical_bytes_ -= std::min(total_logical_bytes_, e.logical_bytes);
            e.retire_all(device);
            return true;
        });
    if (removed > 0u) {
        ++layout_epoch_;
        mesh_descriptors_.clear();
        part_tri_bases_.clear();
        for (auto [k, ep] : entries_) {
            (void)k;
            auto& e = *ep;
            e.mesh_pool_index = static_cast<std::uint32_t>(mesh_descriptors_.size());
            mesh_descriptors_.add(make_gpu_descriptor(e));

            const geometry::MeshLodLevel& l0 = e.summary.lods[0];
            for (auto& bv : e.lod0_blas_variants) {
                bv.part_table_base = static_cast<std::uint32_t>(part_tri_bases_.size());
                for (std::uint32_t p = 0u; p < l0.part_count; ++p)
                    part_tri_bases_.add(e.summary.lod_parts[l0.first_part + p].global_tri_base);
            }
            if (e.proxy_blas.as) {
                const std::uint32_t proxy_lod = std::min<std::uint32_t>(2u, e.summary.lod_count - 1u);
                const geometry::MeshLodLevel& lp = e.summary.lods[proxy_lod];
                e.proxy_part_table_base = static_cast<std::uint32_t>(part_tri_bases_.size());
                for (std::uint32_t p = 0u; p < lp.part_count; ++p)
                    part_tri_bases_.add(e.summary.lod_parts[lp.first_part + p].global_tri_base);
            }
        }
    }
}

void GeometryPool::clear(rhi::Device& device) noexcept {
    for (auto [k, ep] : entries_) {
        (void)k;
        ep->retire_all(device);
    }
    entries_.clear();
    mesh_descriptors_.clear();
    part_tri_bases_.clear();
    mesh_table_slots_.retire_all(device);
    part_tri_base_slots_.retire_all(device);
    mesh_table_srv_ = rhi::bindless_invalid_index;
    part_tri_base_srv_ = rhi::bindless_invalid_index;
    total_logical_bytes_ = 0u;
    ++layout_epoch_;
}

} // namespace atelier::renderer::detail

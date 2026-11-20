#include "application/procedural_preview.hpp"
#include "material/resources.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace atelier::application {
namespace {

math::Vector3 safe_unit_normal(math::Vector3 n, math::Vector3 fallback = {0.f, 1.f, 0.f}) noexcept {
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z))
        return fallback;
    const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
    if (!std::isfinite(len2) || len2 < 1e-12f)
        return fallback;
    const float inv = 1.f / std::sqrt(len2);
    return {n.x * inv, n.y * inv, n.z * inv};
}

bool finite_v3(math::Vector3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

math::Vector2 safe_uv(math::Vector2 uv) noexcept {
    return (std::isfinite(uv.x) && std::isfinite(uv.y)) ? uv : math::Vector2{0.f, 0.f};
}

math::Matrix4 identity_matrix() noexcept {
    return {1.f, 0.f, 0.f, 0.f, //
            0.f, 1.f, 0.f, 0.f, //
            0.f, 0.f, 1.f, 0.f, //
            0.f, 0.f, 0.f, 1.f};
}

math::Vector3 rotate_by_quat(math::Vector3 v, math::Quaternion q) noexcept {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
        return v;
    const float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(len2) || len2 < 1e-12f)
        return v;
    const float inv = 1.f / std::sqrt(len2);
    q = {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    const math::Vector3 u{q.x, q.y, q.z};
    const math::Vector3 t = math::cross(u, v) * 2.f;
    return v + t * q.w + math::cross(u, t);
}

} // namespace

geometry::MeshRef build_procedural_surface_mesh(const procedural::Geometry& geometry) {
    const auto prim_count = geometry.count(procedural::Layer::primitive);
    if (prim_count == 0)
        return {};
    const auto& positions = geometry.positions();
    const auto vp = geometry.vertex_points();

    const Array<math::Vector3>* vtx_n = nullptr;
    const Array<math::Vector3>* pt_n = nullptr;
    if (auto found = geometry.read<math::Vector3>("N", procedural::Layer::vertex); found && *found)
        vtx_n = *found;
    else if (auto found_p = geometry.read<math::Vector3>("N", procedural::Layer::point); found_p && *found_p)
        pt_n = *found_p;

    const Array<math::Vector2>* vtx_uv = nullptr;
    const Array<math::Vector2>* pt_uv = nullptr;
    if (auto found = geometry.read<math::Vector2>("uv", procedural::Layer::vertex); found && *found)
        vtx_uv = *found;
    else if (auto found_p = geometry.read<math::Vector2>("uv", procedural::Layer::point); found_p && *found_p)
        pt_uv = *found_p;

    std::vector<geometry::Vertex> vertices;
    std::vector<std::uint8_t> edge_masks;

    for (std::uint32_t p = 0; p < prim_count; ++p) {
        const auto range = geometry.primitive(p);
        if (range.kind != procedural::PrimitiveKind::polygon || range.count < 3)
            continue;
        const std::uint32_t v0 = range.first;
        const std::uint32_t p0_idx = vp[v0];
        const math::Vector3 p0 = positions[p0_idx];
        if (!finite_v3(p0))
            continue;

        math::Vector3 face_n{0.f, 1.f, 0.f};
        if (!vtx_n && !pt_n) {
            const math::Vector3 p1 = positions[vp[v0 + 1]];
            const math::Vector3 p2 = positions[vp[v0 + 2]];
            if (finite_v3(p1) && finite_v3(p2))
                face_n = safe_unit_normal(math::cross(p1 - p0, p2 - p0));
        }

        const auto make_vertex = [&](std::uint32_t v_idx) {
            const std::uint32_t pt_idx = vp[v_idx];
            const math::Vector3 pos = positions[pt_idx];
            const math::Vector3 n = vtx_n  ? safe_unit_normal((*vtx_n)[v_idx], face_n)
                                    : pt_n ? safe_unit_normal((*pt_n)[pt_idx], face_n)
                                           : face_n;
            const math::Vector2 uv = vtx_uv  ? safe_uv((*vtx_uv)[v_idx])
                                     : pt_uv ? safe_uv((*pt_uv)[pt_idx])
                                             : math::Vector2{pos.x * 0.5f, pos.z * 0.5f};
            geometry::Vertex out{};
            out.position[0] = pos.x;
            out.position[1] = pos.y;
            out.position[2] = pos.z;
            out.normal[0] = n.x;
            out.normal[1] = n.y;
            out.normal[2] = n.z;
            out.uv[0] = uv.x;
            out.uv[1] = uv.y;
            return out;
        };

        for (std::uint32_t i = 1; i + 1 < range.count; ++i) {
            const std::uint32_t v1 = v0 + i;
            const std::uint32_t v2 = v0 + i + 1;
            if (!finite_v3(positions[vp[v1]]) || !finite_v3(positions[vp[v2]]))
                continue;
            vertices.push_back(make_vertex(v0));
            vertices.push_back(make_vertex(v1));
            vertices.push_back(make_vertex(v2));
            std::uint8_t mask = 1u; // Edge opposite v0 is (v1, v2), always a polygon outer edge.
            if (i + 2 == range.count)
                mask |= 2u; // Edge opposite v1 is (v2, v0), outer edge on last fan triangle.
            if (i == 1)
                mask |= 4u; // Edge opposite v2 is (v0, v1), outer edge on first fan triangle.
            edge_masks.push_back(mask);
        }
    }

    if (vertices.empty())
        return {};
    std::vector<geometry::MaterialSlot> slots{{geometry::MaterialSlot{"default"}}};
    std::vector<geometry::MeshPart> parts{{geometry::MeshPart{0, static_cast<std::uint32_t>(vertices.size()), 0}}};
    auto created =
        geometry::Mesh::create(std::move(vertices), std::move(edge_masks), std::move(slots), std::move(parts));
    return created ? std::move(*created) : geometry::MeshRef{};
}

void append_procedural_guides(const procedural::Geometry& geometry, const editor::ViewportView& view,
                              const SopGuideOptions& options, std::vector<renderer::OverlayShape>& overlay) {
    const auto& positions = geometry.positions();
    const std::size_t point_count = positions.size();
    const float dpi = std::max(view.dpi_scale, 1.f);

    // Estimate characteristic scale from bounding box so normal/orient guides scale proportionally.
    float guide_scale = 0.2f;
    if (point_count > 0) {
        math::Vector3 low{1e30f, 1e30f, 1e30f};
        math::Vector3 high{-1e30f, -1e30f, -1e30f};
        bool any = false;
        const std::size_t sample_step = std::max<std::size_t>(1, point_count / 1024);
        for (std::size_t i = 0; i < point_count; i += sample_step) {
            if (finite_v3(positions[i])) {
                low.x = std::min(low.x, positions[i].x);
                low.y = std::min(low.y, positions[i].y);
                low.z = std::min(low.z, positions[i].z);
                high.x = std::max(high.x, positions[i].x);
                high.y = std::max(high.y, positions[i].y);
                high.z = std::max(high.z, positions[i].z);
                any = true;
            }
        }
        if (any) {
            const float diag = math::length(high - low);
            if (std::isfinite(diag) && diag > 1e-4f)
                guide_scale = std::clamp(diag * 0.045f, 0.02f, 25.f);
        }
    }

    const auto add_segment = [&](math::Vector3 a, math::Vector3 b, std::array<float, 4> color, float width_dip) {
        if (!finite_v3(a) || !finite_v3(b))
            return;
        const auto pa = view.project(a);
        const auto pb = view.project(b);
        if (!pa || !pb)
            return;
        renderer::OverlayShape shape;
        shape.kind = renderer::OverlayShape::Kind::segment;
        shape.points = {pa->x, pa->y, pb->x, pb->y};
        shape.color = {color[0], color[1], color[2], color[3]};
        shape.width = width_dip * dpi;
        overlay.push_back(shape);
    };

    const auto add_point_disc = [&](math::Vector3 p, std::array<float, 4> color, float diameter_dip) {
        if (!finite_v3(p))
            return;
        const auto sp = view.project(p);
        if (!sp)
            return;
        renderer::OverlayShape shape;
        shape.kind = renderer::OverlayShape::Kind::segment;
        shape.points = {sp->x, sp->y, sp->x, sp->y};
        shape.color = {color[0], color[1], color[2], color[3]};
        shape.width = diameter_dip * dpi;
        overlay.push_back(shape);
    };

    // 1. Open Polylines (always drawn when enabled, e.g. Line / Polyline curves).
    if (options.show_polylines) {
        const auto prim_count = geometry.count(procedural::Layer::primitive);
        const auto vp = geometry.vertex_points();
        std::size_t emitted = 0;
        constexpr std::size_t max_polyline_segments = 16384;
        for (std::uint32_t p = 0; p < prim_count && emitted < max_polyline_segments; ++p) {
            const auto range = geometry.primitive(p);
            if (range.kind != procedural::PrimitiveKind::polyline || range.count < 2)
                continue;
            for (std::uint32_t i = 0; i + 1 < range.count && emitted < max_polyline_segments; ++i) {
                add_segment(positions[vp[range.first + i]], positions[vp[range.first + i + 1]],
                            {0.92f, 0.78f, 0.36f, 0.95f}, 1.75f);
                ++emitted;
            }
        }
    }

    constexpr std::size_t max_guide_points = 16384;
    const std::size_t stride =
        point_count > max_guide_points ? (point_count + max_guide_points - 1) / max_guide_points : 1;

    // 2. Point Markers (colored by Cd when present, otherwise Houdini cyan).
    if (options.show_points && point_count > 0) {
        const Array<math::Vector3>* cd3 = nullptr;
        const Array<math::Vector4>* cd4 = nullptr;
        if (auto c3 = geometry.read<math::Vector3>("Cd", procedural::Layer::point); c3 && *c3)
            cd3 = *c3;
        else if (auto c4 = geometry.read<math::Vector4>("Cd", procedural::Layer::point); c4 && *c4)
            cd4 = *c4;

        for (std::size_t i = 0; i < point_count; i += stride) {
            std::array<float, 4> color{0.32f, 0.78f, 0.98f, 0.95f};
            if (cd3) {
                const auto& c = (*cd3)[i];
                if (finite_v3(c))
                    color = {std::clamp(c.x, 0.f, 1.f), std::clamp(c.y, 0.f, 1.f), std::clamp(c.z, 0.f, 1.f), 0.95f};
            } else if (cd4) {
                const auto& c = (*cd4)[i];
                if (std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z))
                    color = {std::clamp(c.x, 0.f, 1.f), std::clamp(c.y, 0.f, 1.f), std::clamp(c.z, 0.f, 1.f), 0.95f};
            }
            add_point_disc(positions[i], color, 4.25f);
        }
    }

    // 3. Normals (point N, falling back to vertex N).
    if (options.show_normals) {
        if (auto pt_n = geometry.read<math::Vector3>("N", procedural::Layer::point); pt_n && *pt_n) {
            const auto& normals = **pt_n;
            for (std::size_t i = 0; i < point_count; i += stride) {
                const auto n = safe_unit_normal(normals[i], {0.f, 0.f, 0.f});
                if (n.x != 0.f || n.y != 0.f || n.z != 0.f)
                    add_segment(positions[i], positions[i] + n * guide_scale, {0.35f, 0.90f, 0.85f, 0.88f}, 1.25f);
            }
        } else if (auto vtx_n = geometry.read<math::Vector3>("N", procedural::Layer::vertex); vtx_n && *vtx_n) {
            const auto& normals = **vtx_n;
            const auto vp = geometry.vertex_points();
            const std::size_t v_stride =
                vp.size() > max_guide_points ? (vp.size() + max_guide_points - 1) / max_guide_points : 1;
            for (std::size_t v = 0; v < vp.size(); v += v_stride) {
                const auto p = positions[vp[v]];
                const auto n = safe_unit_normal(normals[v], {0.f, 0.f, 0.f});
                if (n.x != 0.f || n.y != 0.f || n.z != 0.f)
                    add_segment(p, p + n * guide_scale, {0.72f, 0.52f, 0.96f, 0.88f}, 1.25f);
            }
        }
    }

    // 4. Orientations (point orient + optional pscale -> local RGB triad).
    if (options.show_orient && point_count > 0) {
        const Array<math::Quaternion>* orients = nullptr;
        if (auto found = geometry.read<math::Quaternion>("orient", procedural::Layer::point); found && *found)
            orients = *found;
        const Array<float>* pscales = nullptr;
        if (auto found = geometry.read<float>("pscale", procedural::Layer::point); found && *found)
            pscales = *found;

        if (orients || pscales) {
            const std::size_t orient_stride = std::max<std::size_t>(stride, point_count / 4096 + 1);
            for (std::size_t i = 0; i < point_count; i += orient_stride) {
                const auto p = positions[i];
                const math::Quaternion q = orients ? (*orients)[i] : math::Quaternion{0.f, 0.f, 0.f, 1.f};
                float s = guide_scale * 0.8f;
                if (pscales && std::isfinite((*pscales)[i]))
                    s *= std::clamp(std::abs((*pscales)[i]), 0.1f, 10.f);
                add_segment(p, p + rotate_by_quat({s, 0.f, 0.f}, q), {0.95f, 0.32f, 0.32f, 0.9f}, 1.25f);
                add_segment(p, p + rotate_by_quat({0.f, s, 0.f}, q), {0.35f, 0.90f, 0.38f, 0.9f}, 1.25f);
                add_segment(p, p + rotate_by_quat({0.f, 0.f, s}, q), {0.35f, 0.58f, 0.98f, 0.9f}, 1.25f);
            }
        }
    }
}

void ProceduralPreview::reset() noexcept {
    ++identity_;
    cached_shown_ = nullptr;
    cached_generation_ = 0;
    cached_surface_mesh_.reset();
    meshes_.clear();
    surfaces_.clear();
    picking_.reset();
    pending_.reset();
    adopted_.reset();
    ++request_;
}

Result<void> ProceduralPreview::refresh(editor::Session& session, editor::Project* project,
                                        std::shared_ptr<const procedural::Geometry> shown, std::uint64_t generation) {
    if (picking_ && cached_shown_ == shown && cached_generation_ == generation)
        return {};

    meshes_.clear();
    surfaces_.clear();
    std::unordered_map<const material::ProgramSurface*, std::uint32_t> surface_indices;
    const auto register_surfaces =
        [&](std::span<const material::ProgramSurfaceRef> list) -> std::shared_ptr<const std::vector<std::uint32_t>> {
        std::vector<std::uint32_t> slots;
        slots.reserve(list.size());
        for (const auto& surf : list) {
            const auto [it, added] =
                surface_indices.try_emplace(surf.get(), static_cast<std::uint32_t>(surfaces_.size()));
            if (added)
                surfaces_.push_back(surf);
            slots.push_back(it->second);
        }
        return std::make_shared<const std::vector<std::uint32_t>>(std::move(slots));
    };

    assets::Reader installed;
    auto& reader = project ? project->assets() : installed;

    ATELIER_TRY_BIND(grid_mat, session.material_resource(reader, "builtin/material/default-grid"));
    ATELIER_TRY_BIND(default_surface, material::capture_program(session.world().runtime(), grid_mat->id()));

    std::uint64_t stable_key = 1;

    if (shown) {
        if (cached_shown_ != shown || cached_generation_ != generation)
            cached_surface_mesh_ = build_procedural_surface_mesh(*shown);
        if (cached_surface_mesh_) {
            renderer::MeshInstance surface_inst;
            surface_inst.world = identity_matrix();
            surface_inst.geometry = cached_surface_mesh_;
            surface_inst.stable_key = stable_key++;
            surface_inst.material_slots = register_surfaces({&*default_surface, 1});
            meshes_.push_back(std::move(surface_inst));
        }

        const auto refs = shown->references();
        const auto& inst_indices = shown->instance_references();
        const auto& inst_transforms = shown->instance_transforms();
        std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> ref_slots(refs.size());
        for (std::size_t r = 0; r < refs.size(); ++r) {
            if (!refs[r].mesh)
                continue;
            const auto key_sv = refs[r].key.view().std_view();
            if (auto resolved_asset = reader.mesh(key_sv)) {
                if (auto surfs = session.mesh_surfaces(reader, *resolved_asset);
                    surfs && (*surfs)->size() == refs[r].mesh->materials().size()) {
                    ref_slots[r] = register_surfaces(**surfs);
                }
            }
            if (!ref_slots[r]) {
                std::vector<material::ProgramSurfaceRef> fallback(refs[r].mesh->materials().size(), *default_surface);
                ref_slots[r] = register_surfaces(fallback);
            }
        }

        for (std::size_t i = 0; i < inst_indices.size(); ++i) {
            const auto r = inst_indices[i];
            if (r >= refs.size() || !refs[r].mesh)
                continue;
            renderer::MeshInstance inst;
            inst.world = inst_transforms[i];
            inst.geometry = refs[r].mesh;
            inst.stable_key = stable_key++;
            inst.material_slots = ref_slots[r];
            meshes_.push_back(std::move(inst));
        }
    } else {
        cached_surface_mesh_.reset();
    }

    auto picking = std::make_shared<editor::ViewportScene>();
    picking->scene_identity = identity_;
    picking->materials = surfaces_;
    picking->meshes.reserve(meshes_.size());
    for (const auto& m : meshes_)
        picking->meshes.push_back({ObjectId{}, m.world, m.geometry, m.material_slots});
    picking_ = std::move(picking);

    cached_shown_ = std::move(shown);
    cached_generation_ = generation;
    ++request_;
    return {};
}

renderer::Result<renderer::Preparation> ProceduralPreview::prepare(renderer::Renderer& renderer) {
    if (!picking_)
        return renderer::Preparation{0, 0, true};
    if (adopted_ && adopted_->request == request_)
        return renderer::Preparation{0, 0, true};
    if (!pending_ || pending_->request != request_) {
        std::shared_ptr<renderer::MaterialBatch> batch;
        const auto matches = [&](const std::optional<Prepared>& group) {
            return group && group->draw.materials && std::ranges::equal(group->draw.materials->surfaces(), surfaces_);
        };
        if (matches(pending_))
            batch = pending_->draw.materials;
        else if (matches(adopted_))
            batch = adopted_->draw.materials;
        pending_ = Prepared{request_, Draw{meshes_, identity_, picking_, std::move(batch), cached_shown_}, {}};
        if (!surfaces_.empty() && !pending_->draw.materials) {
            auto submitted = renderer.prepare_materials(surfaces_);
            if (!submitted)
                pending_->failure = submitted.error();
            else
                pending_->draw.materials = std::make_shared<renderer::MaterialBatch>(std::move(*submitted));
        }
    }
    if (pending_->failure)
        return std::unexpected(*pending_->failure);
    auto progress = pending_->draw.materials
                        ? renderer.preparation(*pending_->draw.materials)
                        : renderer::Result<renderer::Preparation>{renderer::Preparation{0, 0, true}};
    if (!progress) {
        pending_->failure = progress.error();
        return progress;
    }
    if (progress->ready) {
        adopted_ = std::move(pending_);
        pending_.reset();
    }
    return progress;
}

} // namespace atelier::application

#include "application/procedural_preview.hpp"
#include "editor/procedural_panes.hpp"
#include "material/resources.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <set>
#include <tuple>
#include <unordered_map>

namespace atelier::application {
namespace {

bool finite_v3(math::Vector3 p) noexcept {
    return math::is_finite(p);
}

math::Vector3 rotate_by_quat(math::Vector3 v, math::Quaternion q) noexcept {
    if (const auto unit = math::try_normalize(q))
        return math::rotate(*unit, v);
    return v;
}

math::Vector3 transform_point_row(math::Vector3 p, const math::Matrix4& m, bool identity) noexcept {
    if (identity)
        return p;
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

math::Vector3 transform_vector_row(math::Vector3 v, const math::Matrix4& m, bool identity) noexcept {
    if (identity)
        return v;
    return {v.x * m[0] + v.y * m[4] + v.z * m[8], v.x * m[1] + v.y * m[5] + v.z * m[9],
            v.x * m[2] + v.y * m[6] + v.z * m[10]};
}

struct GeometryBoundsSample {
    math::Vector3 low{};

    math::Vector3 high{};

    bool valid{};
};

GeometryBoundsSample compute_geometry_bounds(const procedural::Geometry& geometry) noexcept {
    const auto& positions = geometry.positions();
    math::Vector3 low{1e30f, 1e30f, 1e30f};
    math::Vector3 high{-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (const auto& p : positions) {
        if (!finite_v3(p))
            continue;
        low.x = std::min(low.x, p.x);
        low.y = std::min(low.y, p.y);
        low.z = std::min(low.z, p.z);
        high.x = std::max(high.x, p.x);
        high.y = std::max(high.y, p.y);
        high.z = std::max(high.z, p.z);
        any = true;
    }
    for (const auto& m : geometry.instance_transforms()) {
        const math::Vector3 t{m[12], m[13], m[14]};
        if (!finite_v3(t))
            continue;
        low.x = std::min(low.x, t.x);
        low.y = std::min(low.y, t.y);
        low.z = std::min(low.z, t.z);
        high.x = std::max(high.x, t.x);
        high.y = std::max(high.y, t.y);
        high.z = std::max(high.z, t.z);
        any = true;
    }
    if (!any)
        return {};
    return {low, high, true};
}

// Clip guide strokes before perspective division. Off-screen endpoints can project arbitrarily far,
// and a segment can cross the viewport even when neither endpoint is visible.
struct GuideStrokes {
    const editor::ViewportView& view;

    std::vector<renderer::OverlayShape>& overlay;

    math::Matrix4d view_projection =
        static_cast<math::Matrix4d>(view.view) * static_cast<math::Matrix4d>(view.projection);

    void point(math::Vector3 p, ui::Color color, float diameter_dip) const {
        const auto screen = view.project(p);
        const float width = pixels(diameter_dip);
        const float pad = (width + 1.f) * .5f;
        if (screen && screen->x >= view.bounds.x - pad && screen->x <= view.bounds.x + view.bounds.width + pad &&
            screen->y >= view.bounds.y - pad && screen->y <= view.bounds.y + view.bounds.height + pad)
            overlay.push_back(
                {renderer::OverlayShape::Kind::segment, {screen->x, screen->y, screen->x, screen->y}, width, color});
    }

    void segment(math::Vector3 start, math::Vector3 end, ui::Color color, float width_dip) const {
        auto a = math::Vector4d{start.x, start.y, start.z, 1.} * view_projection;
        auto b = math::Vector4d{end.x, end.y, end.z, 1.} * view_projection;
        if (!math::is_finite(a) || !math::is_finite(b) || view.bounds.width <= 0.f || view.bounds.height <= 0.f)
            return;
        const float width = pixels(width_dip);
        const double x_limit = 1. + (width + 1.) / view.bounds.width;
        const double y_limit = 1. + (width + 1.) / view.bounds.height;
        if (!clip_plane(a, b, 2, view.far_depth, 1.) || !clip_plane(a, b, 2, 1., -1.) ||
            !clip_plane(a, b, 0, -x_limit, 1.) || !clip_plane(a, b, 0, x_limit, -1.) ||
            !clip_plane(a, b, 1, -y_limit, 1.) || !clip_plane(a, b, 1, y_limit, -1.) || a.w <= 0. || b.w <= 0.)
            return;
        const auto screen = [&](math::Vector4d p) -> math::Vector2 {
            return {static_cast<float>(view.bounds.x + (p.x / p.w + 1.) * view.bounds.width * .5),
                    static_cast<float>(view.bounds.y + (1. - p.y / p.w) * view.bounds.height * .5)};
        };
        const auto pa = screen(a);
        const auto pb = screen(b);
        overlay.push_back({renderer::OverlayShape::Kind::segment, {pa.x, pa.y, pb.x, pb.y}, width, color});
    }

private:
    float pixels(float dip) const noexcept { return std::clamp(dip * std::max(view.dpi_scale, 1.f), 0.f, 64.f); }

    static bool clip_plane(math::Vector4d& a, math::Vector4d& b, std::size_t axis, double limit, double sign) noexcept {
        const double da = sign * (a[axis] - limit * a.w);
        const double db = sign * (b[axis] - limit * b.w);
        if (da < 0. && db < 0.)
            return false;
        if ((da < 0.) != (db < 0.)) {
            // Start at the nearer endpoint for accuracy when segment lengths span many orders of magnitude.
            auto cut = std::abs(da) < std::abs(db) ? a + (b - a) * (da / (da - db)) : b + (a - b) * (db / (db - da));
            cut[axis] = limit * cut.w;
            (da < 0. ? a : b) = cut;
        }
        return true;
    }
};

} // namespace

bool is_pure_point_cloud(const procedural::Geometry& geometry) noexcept {
    if (geometry.count(procedural::Layer::point) == 0 || geometry.count(procedural::Layer::instance) > 0)
        return false;
    const auto prim_count = geometry.count(procedural::Layer::primitive);
    return prim_count == 0;
}

std::wstring format_geometry_info_hud(std::wstring_view title, const procedural::Geometry& geometry, bool auto_points,
                                      std::optional<double> cook_ms, std::size_t chunk_count) {
    std::wstring out{title};
    if (auto_points)
        out += L"  [Auto Points]";
    out += std::format(L"\nPts: {}  Prims: {}  Vtx: {}  Inst: {}", geometry.count(procedural::Layer::point),
                       geometry.count(procedural::Layer::primitive), geometry.count(procedural::Layer::vertex),
                       geometry.count(procedural::Layer::instance));
    if (const auto bounds = compute_geometry_bounds(geometry); bounds.valid) {
        const math::Vector3 size = bounds.high - bounds.low;
        out += std::format(L"\nSize: {:.2f} × {:.2f} × {:.2f} m", size.x, size.y, size.z);
    }
    if (cook_ms || chunk_count > 0) {
        out += L"\n";
        if (cook_ms)
            out += std::format(L"Cook: {:.2f} ms", *cook_ms);
        if (cook_ms && chunk_count > 0)
            out += L"  ·  ";
        if (chunk_count > 0)
            out += std::format(L"Chunks: {}", chunk_count);
    }
    const auto& attrs = geometry.attributes();
    if (!attrs.empty()) {
        out += L"\nAttrs: ";
        const std::size_t shown = std::min<std::size_t>(attrs.size(), 6);
        for (std::size_t i = 0; i < shown; ++i) {
            if (i > 0)
                out += L", ";
            out += platform::utf8_to_utf16_validated(attrs[i].name.view().std_view());
        }
        if (attrs.size() > shown)
            out += std::format(L" (+{})", attrs.size() - shown);
    }
    return out;
}

void append_procedural_guides(const procedural::Geometry& geometry, const editor::ViewportView& view,
                              const SopGuideOptions& options, std::vector<renderer::OverlayShape>& overlay) {
    const auto& positions = geometry.positions();
    const std::size_t point_count = positions.size();
    const GuideStrokes guides{view, overlay};
    const bool identity_world = options.world_transform == math::Matrix4::identity();
    const std::expected<math::NormalTransform, math::NormalTransformFailure> normal_xform =
        identity_world ? std::expected<math::NormalTransform, math::NormalTransformFailure>{math::NormalTransform{
                             {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f}, 1.f}}
                       : math::normal_transform(
                             {options.world_transform[0], options.world_transform[1], options.world_transform[2],
                              options.world_transform[4], options.world_transform[5], options.world_transform[6],
                              options.world_transform[8], options.world_transform[9], options.world_transform[10]});

    const auto transform_normal = [&](math::Vector3 n) noexcept -> std::optional<math::Vector3> {
        if (!normal_xform)
            return std::nullopt;
        if (identity_world)
            return math::try_normalize(n);
        const auto& m = normal_xform->matrix;
        return math::try_normalize(math::Vector3{n.x * m[0] + n.y * m[3] + n.z * m[6],
                                                 n.x * m[1] + n.y * m[4] + n.z * m[7],
                                                 n.x * m[2] + n.y * m[5] + n.z * m[8]});
    };

    const auto front_facing = [&](math::Vector3 world_p, math::Vector3 unit_world_n) noexcept {
        const math::Vector3 to_eye = view.orthographic ? -view.forward : (view.eye - world_p);
        return math::dot(unit_world_n, to_eye) >= -0.04f * std::max(1e-4f, math::length(to_eye));
    };

    float guide_scale = 0.2f;
    math::Vector3 local_low{1e30f, 1e30f, 1e30f};
    math::Vector3 local_high{-1e30f, -1e30f, -1e30f};
    bool has_bounds = false;
    if (point_count > 0) {
        const std::size_t sample_step = options.show_bounds ? 1 : std::max<std::size_t>(1, point_count / 1024);
        for (std::size_t i = 0; i < point_count; i += sample_step) {
            if (finite_v3(positions[i])) {
                local_low.x = std::min(local_low.x, positions[i].x);
                local_low.y = std::min(local_low.y, positions[i].y);
                local_low.z = std::min(local_low.z, positions[i].z);
                local_high.x = std::max(local_high.x, positions[i].x);
                local_high.y = std::max(local_high.y, positions[i].y);
                local_high.z = std::max(local_high.z, positions[i].z);
                has_bounds = true;
            }
        }
        if (has_bounds) {
            const math::Vector3 world_a = transform_point_row(local_low, options.world_transform, identity_world);
            const math::Vector3 world_b = transform_point_row(local_high, options.world_transform, identity_world);
            const float diag = math::length(world_b - world_a);
            if (std::isfinite(diag) && diag > 1e-4f)
                guide_scale = std::clamp(diag * 0.042f, 0.02f, 25.f);
        }
    }

    const auto add_wire_box = [&](math::Vector3 low, math::Vector3 high, ui::Color color, float width_dip) {
        const std::array<math::Vector3, 8> corners{{
            transform_point_row({low.x, low.y, low.z}, options.world_transform, identity_world),
            transform_point_row({high.x, low.y, low.z}, options.world_transform, identity_world),
            transform_point_row({high.x, high.y, low.z}, options.world_transform, identity_world),
            transform_point_row({low.x, high.y, low.z}, options.world_transform, identity_world),
            transform_point_row({low.x, low.y, high.z}, options.world_transform, identity_world),
            transform_point_row({high.x, low.y, high.z}, options.world_transform, identity_world),
            transform_point_row({high.x, high.y, high.z}, options.world_transform, identity_world),
            transform_point_row({low.x, high.y, high.z}, options.world_transform, identity_world),
        }};
        constexpr std::array<std::pair<unsigned, unsigned>, 12> edges{
            {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
        for (const auto [i0, i1] : edges)
            guides.segment(corners[i0], corners[i1], color, width_dip);
    };

    // 1. Bounds & Generator Spatial Chunk Cages:
    if (options.show_bounds) {
        const auto full_bounds =
            has_bounds ? GeometryBoundsSample{local_low, local_high, true} : compute_geometry_bounds(geometry);
        if (full_bounds.valid)
            add_wire_box(full_bounds.low, full_bounds.high, {0.96f, 0.78f, 0.28f, 0.85f}, 1.45f);

        if (options.chunk_size > 0.f && std::isfinite(options.chunk_size) && !geometry.instance_transforms().empty()) {
            const float inv_chunk = 1.f / options.chunk_size;
            std::set<std::tuple<int, int, int>> occupied_chunks;
            for (const auto& m : geometry.instance_transforms()) {
                if (!std::isfinite(m[12]) || !std::isfinite(m[13]) || !std::isfinite(m[14]))
                    continue;
                occupied_chunks.emplace(static_cast<int>(std::floor(m[12] * inv_chunk)),
                                        static_cast<int>(std::floor(m[13] * inv_chunk)),
                                        static_cast<int>(std::floor(m[14] * inv_chunk)));
                if (occupied_chunks.size() >= 128)
                    break;
            }
            for (const auto& [cx, cy, cz] : occupied_chunks) {
                const math::Vector3 clow{cx * options.chunk_size, cy * options.chunk_size, cz * options.chunk_size};
                const math::Vector3 chigh{clow.x + options.chunk_size, clow.y + options.chunk_size,
                                          clow.z + options.chunk_size};
                add_wire_box(clow, chigh, {0.30f, 0.82f, 0.88f, 0.36f}, 1.0f);
            }
        }
    }

    // 2. Open Polylines:
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
                const auto a =
                    transform_point_row(positions[vp[range.first + i]], options.world_transform, identity_world);
                const auto b =
                    transform_point_row(positions[vp[range.first + i + 1]], options.world_transform, identity_world);
                guides.segment(a, b, {0.92f, 0.78f, 0.36f, 0.95f}, 1.75f);
                ++emitted;
            }
        }
    }

    constexpr std::size_t max_guide_points = 16384;
    const std::size_t stride =
        point_count > max_guide_points ? (point_count + max_guide_points - 1) / max_guide_points : 1;

    const Array<math::Vector3>* point_normals = nullptr;
    if (auto pt_n = geometry.read<math::Vector3>("N", procedural::Layer::point); pt_n && *pt_n)
        point_normals = *pt_n;

    // 3. Point Markers (colored by Cd when present; hemisphere-dimmed when point N faces away from camera):
    if (options.show_points && point_count > 0) {
        const Array<math::Vector3>* cd3 = nullptr;
        const Array<math::Vector4>* cd4 = nullptr;
        if (auto c3 = geometry.read<math::Vector3>("Cd", procedural::Layer::point); c3 && *c3)
            cd3 = *c3;
        else if (auto c4 = geometry.read<math::Vector4>("Cd", procedural::Layer::point); c4 && *c4)
            cd4 = *c4;

        for (std::size_t i = 0; i < point_count; i += stride) {
            const auto wp = transform_point_row(positions[i], options.world_transform, identity_world);
            bool front = true;
            if (point_normals) {
                if (const auto wn = transform_normal((*point_normals)[i]))
                    front = front_facing(wp, *wn);
            }
            const float alpha = front ? 0.95f : 0.28f;
            const float diam = front ? 4.25f : 2.65f;
            ui::Color color{0.32f, 0.78f, 0.98f, alpha};
            if (cd3) {
                const auto& c = (*cd3)[i];
                if (finite_v3(c))
                    color = {std::clamp(c.x, 0.f, 1.f), std::clamp(c.y, 0.f, 1.f), std::clamp(c.z, 0.f, 1.f), alpha};
            } else if (cd4) {
                const auto& c = (*cd4)[i];
                if (std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z))
                    color = {std::clamp(c.x, 0.f, 1.f), std::clamp(c.y, 0.f, 1.f), std::clamp(c.z, 0.f, 1.f), alpha};
            }
            guides.point(wp, color, diam);
        }
    }

    // 4. Point / Vertex Normals:
    if (options.show_point_normals) {
        if (point_normals) {
            const auto& normals = *point_normals;
            for (std::size_t i = 0; i < point_count; i += stride) {
                const auto wp = transform_point_row(positions[i], options.world_transform, identity_world);
                if (const auto wn = transform_normal(normals[i])) {
                    const float alpha = front_facing(wp, *wn) ? 0.88f : 0.26f;
                    guides.segment(wp, wp + *wn * guide_scale, {0.35f, 0.90f, 0.85f, alpha}, 1.25f);
                }
            }
        } else if (auto vtx_n = geometry.read<math::Vector3>("N", procedural::Layer::vertex); vtx_n && *vtx_n) {
            const auto& normals = **vtx_n;
            const auto vp = geometry.vertex_points();
            const std::size_t v_stride =
                vp.size() > max_guide_points ? (vp.size() + max_guide_points - 1) / max_guide_points : 1;
            for (std::size_t v = 0; v < vp.size(); v += v_stride) {
                const auto wp = transform_point_row(positions[vp[v]], options.world_transform, identity_world);
                if (const auto wn = transform_normal(normals[v])) {
                    const float alpha = front_facing(wp, *wn) ? 0.88f : 0.26f;
                    guides.segment(wp, wp + *wn * guide_scale, {0.72f, 0.52f, 0.96f, alpha}, 1.25f);
                }
            }
        }
    }

    // 5. Primitive (Face) Geometric Normals at Polygon Centroids (CCW winding inspection):
    if (options.show_face_normals) {
        const auto prim_count = geometry.count(procedural::Layer::primitive);
        const auto vp = geometry.vertex_points();
        constexpr std::size_t max_face_normals = 8192;
        const std::size_t f_stride =
            prim_count > max_face_normals ? (prim_count + max_face_normals - 1) / max_face_normals : 1;
        for (std::size_t p = 0; p < prim_count; p += f_stride) {
            const auto range = geometry.primitive(static_cast<std::uint32_t>(p));
            if (range.kind != procedural::PrimitiveKind::polygon || range.count < 3)
                continue;
            math::Vector3d centroid{0., 0., 0.};
            bool finite_face = true;
            for (std::uint32_t k = 0; k < range.count; ++k) {
                const auto& pos = positions[vp[range.first + k]];
                if (!finite_v3(pos)) {
                    finite_face = false;
                    break;
                }
                centroid = centroid + static_cast<math::Vector3d>(pos);
            }
            if (!finite_face)
                continue;
            centroid = centroid * (1. / static_cast<double>(range.count));
            const auto p0d = static_cast<math::Vector3d>(positions[vp[range.first]]);
            math::Vector3d area_n{0., 0., 0.};
            for (std::uint32_t i = 1; i + 1 < range.count; ++i) {
                const auto e1 = static_cast<math::Vector3d>(positions[vp[range.first + i]]) - p0d;
                const auto e2 = static_cast<math::Vector3d>(positions[vp[range.first + i + 1]]) - p0d;
                area_n = area_n + math::cross(e1, e2);
            }
            const auto local_n = math::try_normalize(area_n);
            if (!local_n)
                continue;
            const auto wc =
                transform_point_row(static_cast<math::Vector3>(centroid), options.world_transform, identity_world);
            if (const auto wn = transform_normal(static_cast<math::Vector3>(*local_n))) {
                const bool front = front_facing(wc, *wn);
                const float alpha = front ? 0.92f : 0.30f;
                guides.point(wc, {0.98f, 0.72f, 0.24f, alpha}, 2.6f);
                guides.segment(wc, wc + *wn * guide_scale, {0.98f, 0.72f, 0.24f, alpha}, 1.35f);
            }
        }
    }

    // 6. Orientations & pscale Footprint Rings:
    if (options.show_orient && point_count > 0) {
        const Array<math::Quaternion>* orients = nullptr;
        if (auto found = geometry.read<math::Quaternion>("orient", procedural::Layer::point); found && *found)
            orients = *found;
        const Array<float>* pscales = nullptr;
        if (auto found = geometry.read<float>("pscale", procedural::Layer::point); found && *found)
            pscales = *found;

        if (orients || pscales) {
            const std::size_t orient_stride = std::max<std::size_t>(stride, point_count / 2048 + 1);
            constexpr int ring_segments = 12;
            for (std::size_t i = 0; i < point_count; i += orient_stride) {
                const auto lp = positions[i];
                const auto wp = transform_point_row(lp, options.world_transform, identity_world);
                const math::Quaternion q = orients ? (*orients)[i] : math::Quaternion{0.f, 0.f, 0.f, 1.f};
                float s = guide_scale * 0.8f;
                float radius = 0.f;
                if (pscales && std::isfinite((*pscales)[i])) {
                    const float raw = std::abs((*pscales)[i]);
                    s *= std::clamp(raw, 0.1f, 10.f);
                    radius = std::clamp(raw * 0.5f, 0.01f, 500.f);
                }
                const auto ax =
                    transform_vector_row(rotate_by_quat({s, 0.f, 0.f}, q), options.world_transform, identity_world);
                const auto ay =
                    transform_vector_row(rotate_by_quat({0.f, s, 0.f}, q), options.world_transform, identity_world);
                const auto az =
                    transform_vector_row(rotate_by_quat({0.f, 0.f, s}, q), options.world_transform, identity_world);
                guides.segment(wp, wp + ax, {0.95f, 0.32f, 0.32f, 0.9f}, 1.25f);
                guides.segment(wp, wp + ay, {0.35f, 0.90f, 0.38f, 0.9f}, 1.25f);
                guides.segment(wp, wp + az, {0.35f, 0.58f, 0.98f, 0.9f}, 1.25f);

                if (radius > 0.f) {
                    math::Vector3 prev{};
                    for (int seg = 0; seg <= ring_segments; ++seg) {
                        const float ang = static_cast<float>(seg) *
                                          (2.f * std::numbers::pi_v<float> / static_cast<float>(ring_segments));
                        const auto offset = transform_vector_row(
                            rotate_by_quat({std::cos(ang) * radius, 0.f, std::sin(ang) * radius}, q),
                            options.world_transform, identity_world);
                        const math::Vector3 cur = wp + offset;
                        if (seg > 0)
                            guides.segment(prev, cur, {0.96f, 0.82f, 0.36f, 0.52f}, 1.0f);
                        prev = cur;
                    }
                }
            }
        }
    }
    if (options.group && options.group->resolve(geometry)) {
        constexpr ui::Color amber{1.f, .68f, .12f, .96f};
        const auto world_point = [&](std::uint32_t point) {
            return transform_point_row(positions[point], options.world_transform, identity_world);
        };
        std::size_t edge_budget = 16384;
        for (const auto member : options.group->samples()) {
            if (options.group->layer() == procedural::Layer::point) {
                guides.point(world_point(member), amber, 7.f);
                continue;
            }
            const auto prim = geometry.primitive(member);
            const auto points = geometry.vertex_points().subspan(prim.first, prim.count);
            math::Vector3d sum{};
            for (const auto p : points)
                sum += math::Vector3d(positions[p]);
            const auto center = transform_point_row(math::Vector3(sum / static_cast<double>(points.size())),
                                                    options.world_transform, identity_world);
            guides.point(center, amber, 5.f);
            const bool closed = prim.kind == procedural::PrimitiveKind::polygon;
            const auto edges = closed ? prim.count : prim.count - 1;
            if (edges > edge_budget)
                continue;
            edge_budget -= edges;
            const auto outline_point = [&](std::uint32_t index) {
                const auto world = world_point(points[index]);
                return closed ? world + (center - world) * .08f : world;
            };
            auto start = outline_point(0);
            for (std::uint32_t i = 0; i < edges; ++i) {
                const auto end = outline_point(i + 1 == prim.count ? 0 : i + 1);
                guides.segment(start, end, amber, 2.f);
                start = end;
            }
        }
    }
}

void append_template_wireframe(const procedural::Geometry& geometry, const editor::ViewportView& view,
                               std::vector<renderer::OverlayShape>& overlay, const math::Matrix4& world_transform) {
    const auto& positions = geometry.positions();
    const std::size_t point_count = positions.size();
    if (point_count == 0)
        return;
    const bool identity_world = (world_transform == math::Matrix4::identity());
    const GuideStrokes guides{view, overlay};
    const std::size_t prim_count = geometry.count(procedural::Layer::primitive);
    const auto vp = geometry.vertex_points();
    constexpr ui::Color wire_color{0.82f, 0.44f, 0.96f, 0.48f};

    if (prim_count > 0) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
        edges.reserve(std::min<std::size_t>(vp.size(), 24576));
        constexpr std::size_t max_raw_edges = 32768;
        for (std::uint32_t p = 0; p < prim_count && edges.size() < max_raw_edges; ++p) {
            const auto range = geometry.primitive(p);
            if (range.kind == procedural::PrimitiveKind::polygon && range.count >= 3) {
                for (std::uint32_t i = 0; i < range.count && edges.size() < max_raw_edges; ++i) {
                    const std::uint32_t a = vp[range.first + i];
                    const std::uint32_t b = vp[range.first + ((i + 1) % range.count)];
                    if (a != b)
                        edges.push_back(a < b ? std::pair{a, b} : std::pair{b, a});
                }
            } else if (range.kind == procedural::PrimitiveKind::polyline && range.count >= 2) {
                for (std::uint32_t i = 0; i + 1 < range.count && edges.size() < max_raw_edges; ++i) {
                    const std::uint32_t a = vp[range.first + i];
                    const std::uint32_t b = vp[range.first + i + 1];
                    if (a != b)
                        edges.push_back(a < b ? std::pair{a, b} : std::pair{b, a});
                }
            }
        }
        std::ranges::sort(edges);
        const auto unique_end = std::ranges::unique(edges).begin();
        edges.erase(unique_end, edges.end());
        constexpr std::size_t max_segments = 16384;
        const std::size_t step = edges.size() > max_segments ? (edges.size() + max_segments - 1) / max_segments : 1;
        for (std::size_t i = 0; i < edges.size(); i += step) {
            const auto [a, b] = edges[i];
            if (a >= point_count || b >= point_count)
                continue;
            guides.segment(transform_point_row(positions[a], world_transform, identity_world),
                           transform_point_row(positions[b], world_transform, identity_world), wire_color, 1.15f);
        }
    } else {
        constexpr std::size_t max_pts = 8192;
        const std::size_t stride = point_count > max_pts ? (point_count + max_pts - 1) / max_pts : 1;
        for (std::size_t i = 0; i < point_count; i += stride)
            guides.point(transform_point_row(positions[i], world_transform, identity_world),
                         {wire_color.r, wire_color.g, wire_color.b, 0.56f}, 3.2f);
    }
}

void append_orientation_gnomon(const editor::ViewportView& view, std::vector<renderer::OverlayShape>& overlay) {
    const float dpi = std::max(view.dpi_scale, 1.f);
    if (view.bounds.width < 180.f * dpi || view.bounds.height < 160.f * dpi)
        return;

    const float g_scale = std::min(dpi, 1.25f);
    const float cx = view.bounds.x + 38.f * dpi;
    const float cy = view.bounds.y + view.bounds.height - 54.f * dpi;
    const float arm = 20.f * g_scale;

    // Subtle dark circular backdrop hub (clamped within Renderer's 64px overlay capsule limit):
    overlay.push_back({renderer::OverlayShape::Kind::segment,
                       {cx, cy, cx, cy},
                       std::min(50.f * g_scale, 62.f),
                       {0.08f, 0.09f, 0.11f, 0.42f}});

    struct AxisEntry {
        int axis; // 0=X, 1=Y, 2=Z

        bool positive;

        float sx;

        float sy;

        float depth;

        std::array<float, 3> rgb;
    };

    const std::array<math::Vector3, 3> world_axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const std::array<std::array<float, 3>, 3> colors{
        {{0.94f, 0.30f, 0.30f}, {0.30f, 0.84f, 0.45f}, {0.30f, 0.60f, 0.98f}}};
    std::array<AxisEntry, 6> entries{};
    for (int a = 0; a < 3; ++a) {
        const float dx = math::dot(world_axes[a], view.right);
        const float dy = -math::dot(world_axes[a], view.up);
        const float dz = math::dot(world_axes[a], view.forward);
        entries[a * 2] = {a, true, cx + dx * arm, cy + dy * arm, dz, colors[a]};
        entries[a * 2 + 1] = {a, false, cx - dx * arm * 0.78f, cy - dy * arm * 0.78f, -dz, colors[a]};
    }

    // Back-to-front painter's order (larger forward depth is further from eye):
    std::ranges::sort(entries, [](const AxisEntry& lhs, const AxisEntry& rhs) { return lhs.depth > rhs.depth; });

    const auto glyph_seg = [&](float x0, float y0, float x1, float y1, std::array<float, 4> col) {
        overlay.push_back({renderer::OverlayShape::Kind::segment,
                           {x0, y0, x1, y1},
                           std::min(1.35f * g_scale, 64.f),
                           {col[0], col[1], col[2], col[3]}});
    };

    for (const auto& e : entries) {
        if (e.positive) {
            overlay.push_back({renderer::OverlayShape::Kind::segment,
                               {cx, cy, e.sx, e.sy},
                               std::min(2.0f * g_scale, 64.f),
                               {e.rgb[0], e.rgb[1], e.rgb[2], 0.92f}});
            overlay.push_back({renderer::OverlayShape::Kind::segment,
                               {e.sx, e.sy, e.sx, e.sy},
                               std::min(11.5f * g_scale, 64.f),
                               {e.rgb[0], e.rgb[1], e.rgb[2], 0.96f}});
            const float r = 2.6f * g_scale;
            const std::array<float, 4> ink{0.08f, 0.09f, 0.11f, 0.95f};
            if (e.axis == 0) { // X
                glyph_seg(e.sx - r, e.sy - r, e.sx + r, e.sy + r, ink);
                glyph_seg(e.sx + r, e.sy - r, e.sx - r, e.sy + r, ink);
            } else if (e.axis == 1) { // Y
                glyph_seg(e.sx - r, e.sy - r, e.sx, e.sy, ink);
                glyph_seg(e.sx + r, e.sy - r, e.sx, e.sy, ink);
                glyph_seg(e.sx, e.sy, e.sx, e.sy + r * 1.15f, ink);
            } else { // Z
                glyph_seg(e.sx - r, e.sy - r, e.sx + r, e.sy - r, ink);
                glyph_seg(e.sx + r, e.sy - r, e.sx - r, e.sy + r, ink);
                glyph_seg(e.sx - r, e.sy + r, e.sx + r, e.sy + r, ink);
            }
        } else {
            overlay.push_back({renderer::OverlayShape::Kind::segment,
                               {cx, cy, e.sx, e.sy},
                               std::min(1.2f * g_scale, 64.f),
                               {e.rgb[0], e.rgb[1], e.rgb[2], 0.30f}});
            overlay.push_back({renderer::OverlayShape::Kind::segment,
                               {e.sx, e.sy, e.sx, e.sy},
                               std::min(5.5f * g_scale, 64.f),
                               {e.rgb[0], e.rgb[1], e.rgb[2], 0.42f}});
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
    if (picking_ && cached_shown_ == shown &&
        (cached_generation_ == generation || !shown || shown->references().empty())) {
        cached_generation_ = generation;
        return {};
    }

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

    ATELIER_TRY_BIND(default_surface, material::default_world_surface());

    std::uint64_t stable_key = 1;

    if (shown) {
        if (cached_shown_ != shown)
            cached_surface_mesh_ = presentation::build_procedural_surface_mesh(*shown);
        if (cached_surface_mesh_) {
            renderer::MeshInstance surface_inst;
            surface_inst.world = math::Matrix4::identity();
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
            if (!key_sv.empty()) {
                if (auto resolved_asset = reader.mesh(key_sv)) {
                    std::vector<editor::MissingResource> ignored_missing;
                    if (auto surfs = session.mesh_surfaces(reader, *resolved_asset, &ignored_missing);
                        surfs && (*surfs)->size() == refs[r].mesh->materials().size()) {
                        ref_slots[r] = register_surfaces(**surfs);
                    }
                }
            }
            if (!ref_slots[r]) {
                std::vector<material::ProgramSurfaceRef> fallback(refs[r].mesh->materials().size(), *default_surface);
                ref_slots[r] = register_surfaces(fallback);
            }
        }

        for (std::size_t i = 0; i < inst_indices.size() && i < inst_transforms.size(); ++i) {
            const auto r = inst_indices[i];
            if (r >= refs.size() || !refs[r].mesh)
                continue;
            const auto& m = inst_transforms[i];
            if (!math::is_finite(m))
                continue;
            const procedural::Affine affine{m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]};
            const auto valid_world = presentation::compose_instance_world(affine, math::Matrix4::identity());
            if (!valid_world)
                continue;
            renderer::MeshInstance inst;
            inst.world = *valid_world;
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

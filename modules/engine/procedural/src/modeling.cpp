#include "detail/triangles.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"
#include "procedural/operations.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace atelier::procedural {
namespace {

using math::Vector2;

using math::Vector3;

using math::Vector3d;

struct DirectedEdge {
    std::uint32_t a{};

    std::uint32_t b{};

    bool operator==(const DirectedEdge&) const = default;
};

} // namespace
} // namespace atelier::procedural

template <>
struct atelier::Hash<atelier::procedural::DirectedEdge> {
    using is_avalanching = void;

    std::size_t operator()(const atelier::procedural::DirectedEdge& edge) const noexcept {
        return atelier::hash_fields(edge.a, edge.b);
    }
};

namespace atelier::procedural {
namespace {

Result<Vector3d> compute_polygon_normal(const Geometry& surface, PrimitiveRange range) {
    const auto corners = surface.vertex_points();
    const auto& positions = surface.positions();
    const Vector3d origin(positions[corners[range.first]]);
    Vector3d area{};
    for (std::uint32_t i = 1; i + 1 < range.count; ++i) {
        const Vector3d p1(positions[corners[range.first + i]]);
        const Vector3d p2(positions[corners[range.first + i + 1]]);
        area += math::cross(p1 - origin, p2 - origin);
    }
    const double len = math::length(area);
    if (!(len > 0.0))
        return failure("Polygon has zero area.");
    return area / len;
}

Vector3 face_unit_normal(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 fallback) noexcept {
    const Vector3d a(p0);
    const Vector3d cross = math::cross(Vector3d(p1) - a, Vector3d(p2) - a);
    if (const auto unit = math::try_normalize(cross))
        return Vector3(*unit);
    return fallback;
}

ColumnData gather_column_rows(const ColumnData& source, std::span<const std::uint32_t> mapping) {
    return std::visit(
        [&](const auto& column) -> ColumnData {
            using T = typename std::remove_cvref_t<decltype(column)>::Value;
            Array<T> out;
            out.reserve(mapping.size());
            const auto& arr = column.array();
            for (const auto src_idx : mapping)
                out.add(arr[src_idx]);
            return SharedArray<T>{std::move(out)};
        },
        source);
}

struct SurfaceBuilder {
    Array<Vector3> positions;

    Array<std::uint32_t> corners;

    Array<std::uint32_t> offsets{0u};

    Array<Vector3> normals;

    Array<Vector2> uvs;

    std::vector<std::uint32_t> point_sources;

    std::vector<std::uint32_t> prim_sources;

    Array<bool> front_flags;

    Array<bool> side_flags;

    bool track_groups{};

    void reserve(std::size_t approx_points, std::size_t approx_corners, std::size_t approx_prims,
                 bool with_point_sources) {
        positions.reserve(approx_points);
        corners.reserve(approx_corners);
        normals.reserve(approx_corners);
        uvs.reserve(approx_corners);
        offsets.reserve(approx_prims + 1u);
        prim_sources.reserve(approx_prims);
        if (with_point_sources)
            point_sources.reserve(approx_points);
        if (track_groups) {
            front_flags.reserve(approx_prims);
            side_flags.reserve(approx_prims);
        }
    }

    std::uint32_t add_point(Vector3 pos, std::uint32_t src_pt = 0u, bool track_point = true) {
        const auto idx = static_cast<std::uint32_t>(positions.size());
        positions.add(pos);
        if (track_point)
            point_sources.push_back(src_pt);
        return idx;
    }

    void record_face(std::uint32_t src_prim, bool is_front, bool is_side) {
        offsets.add(static_cast<std::uint32_t>(corners.size()));
        prim_sources.push_back(src_prim);
        if (track_groups) {
            front_flags.add(is_front);
            side_flags.add(is_side);
        }
    }

    void emit_polygon(std::span<const std::uint32_t> pts, std::span<const Vector2> face_uvs,
                      std::span<const Vector3> face_normals, std::uint32_t src_prim, bool is_front = false,
                      bool is_side = false, bool reverse = false) {
        const std::size_t k = pts.size();
        for (std::size_t j = 0; j < k; ++j) {
            const std::size_t idx = reverse ? (j == 0 ? 0 : k - j) : j;
            corners.add(pts[idx]);
            uvs.add(face_uvs[idx]);
            normals.add(face_normals[idx]);
        }
        record_face(src_prim, is_front, is_side);
    }

    void emit_flat_face(std::span<const std::uint32_t> pts, std::span<const Vector2> face_uvs, Vector3 normal,
                        std::uint32_t src_prim, bool is_front, bool is_side, bool reverse) {
        const std::size_t k = pts.size();
        for (std::size_t j = 0; j < k; ++j) {
            const std::size_t idx = reverse ? (j == 0 ? 0 : k - j) : j;
            corners.add(pts[idx]);
            uvs.add(face_uvs[idx]);
            normals.add(normal);
        }
        record_face(src_prim, is_front, is_side);
    }

    void emit_quad_or_split(std::array<std::uint32_t, 4> q, std::array<Vector2, 4> q_uv,
                            const std::array<Vector3, 4>* custom_normals, std::uint32_t src_prim, bool is_front,
                            bool is_side, bool reverse) {
        if (reverse) {
            std::swap(q[1], q[3]);
            std::swap(q_uv[1], q_uv[3]);
        }
        const std::array<Vector3, 4> poly{positions[q[0]], positions[q[1]], positions[q[2]], positions[q[3]]};
        const Vector3 flat_n0 = face_unit_normal(poly[0], poly[1], poly[2], {0.f, 1.f, 0.f});
        if (detail::validate_polygon(poly)) {
            if (custom_normals)
                emit_polygon(q, q_uv, *custom_normals, src_prim, is_front, is_side, false);
            else
                emit_flat_face(q, q_uv, flat_n0, src_prim, is_front, is_side, false);
            return;
        }
        const std::array<std::uint32_t, 3> t0{q[0], q[1], q[2]};
        const std::array<Vector2, 3> uv0{q_uv[0], q_uv[1], q_uv[2]};
        if (custom_normals) {
            const std::array<Vector3, 3> n0{(*custom_normals)[0], (*custom_normals)[1], (*custom_normals)[2]};
            emit_polygon(t0, uv0, n0, src_prim, is_front, is_side, false);
        } else {
            emit_flat_face(t0, uv0, flat_n0, src_prim, is_front, is_side, false);
        }
        const std::array<std::uint32_t, 3> t1{q[0], q[2], q[3]};
        const std::array<Vector2, 3> uv1{q_uv[0], q_uv[2], q_uv[3]};
        if (custom_normals) {
            const std::array<Vector3, 3> n1{(*custom_normals)[0], (*custom_normals)[2], (*custom_normals)[3]};
            emit_polygon(t1, uv1, n1, src_prim, is_front, is_side, false);
        } else {
            const Vector3 flat_n1 = face_unit_normal(poly[0], poly[2], poly[3], flat_n0);
            emit_flat_face(t1, uv1, flat_n1, src_prim, is_front, is_side, false);
        }
    }

    Result<Geometry> finish(const Geometry& source) {
        ATELIER_TRY_BIND(result, Geometry::polygons(std::move(positions), std::move(corners), offsets));
        ATELIER_TRY(detail::TriangleView::create(*result, true));
        for (const auto& attr : source.attributes()) {
            if (attr.layer == Layer::detail) {
                ATELIER_TRY(result->set(attr.name, Layer::detail, attr.data, attr.usage));
            } else if (attr.layer == Layer::point && !point_sources.empty() && attr.name != "P" && attr.name != "N" &&
                       attr.name != "uv") {
                ATELIER_TRY(
                    result->set(attr.name, Layer::point, gather_column_rows(attr.data, point_sources), attr.usage));
            } else if (attr.layer == Layer::primitive && attr.name != "N" && attr.name != "uv") {
                ATELIER_TRY(
                    result->set(attr.name, Layer::primitive, gather_column_rows(attr.data, prim_sources), attr.usage));
            }
        }
        ATELIER_TRY(result->set(String::literal<"N">(), Layer::vertex, SharedArray<Vector3>{std::move(normals)},
                                Usage::normal));
        ATELIER_TRY(
            result->set(String::literal<"uv">(), Layer::vertex, SharedArray<Vector2>{std::move(uvs)}, Usage::none));
        return result;
    }
};

Result<void> set_optional_group(Geometry& geometry, StringView group_name, Array<bool>&& flags) {
    if (group_name.empty())
        return {};
    auto name = String::from_utf8(group_name);
    if (!name)
        return failure("Attribute name requires valid UTF-8.");
    return geometry.set(std::move(*name), Layer::primitive, SharedArray<bool>{std::move(flags)}, Usage::none);
}

} // namespace

Result<Geometry> extrude(const Geometry& surface, const ExtrudeSettings& settings) {
    if (surface.count(Layer::instance) > 0)
        return failure("Extrude does not accept instances.");
    if (!std::isfinite(settings.distance) || !std::isfinite(settings.inset))
        return failure("Extrude distance and inset must be finite.");
    if (!settings.front_group.empty() && settings.front_group == settings.side_group)
        return failure("Extrude front_group and side_group must be distinct attribute names.");
    for (const auto group_name : {settings.front_group, settings.side_group}) {
        if (group_name.empty())
            continue;
        ATELIER_TRY(Geometry::admit_name(group_name));
        if (const auto* existing = surface.find(group_name)) {
            if (existing->layer != Layer::primitive || !std::holds_alternative<SharedArray<bool>>(existing->data))
                return failure("Attribute " + std::string{group_name.std_view()} + " has another layer or type.");
        }
    }

    const auto prim_count = surface.count(Layer::primitive);
    if (prim_count == 0) {
        if (surface.count(Layer::point) > 0)
            return failure("Extrude requires polygon primitives.");
        return surface;
    }

    ATELIER_TRY(detail::TriangleView::create(surface, true));

    const auto& in_positions = surface.positions();
    const auto in_corners = surface.vertex_points();

    std::vector<bool> selected(prim_count, true);
    if (!settings.group.empty()) {
        const auto* group_attr = surface.find(settings.group);
        if (!group_attr)
            return failure("Extrude group attribute " + std::string{settings.group.std_view()} + " was not found.");
        if (!std::holds_alternative<SharedArray<bool>>(group_attr->data))
            return failure("Extrude group " + std::string{settings.group.std_view()} + " must be a bool attribute.");
        const auto& flags = std::get<SharedArray<bool>>(group_attr->data).array();
        if (group_attr->layer == Layer::primitive) {
            for (std::uint32_t p = 0; p < prim_count; ++p)
                selected[p] = flags[p];
        } else if (group_attr->layer == Layer::point) {
            for (std::uint32_t p = 0; p < prim_count; ++p) {
                const auto range = surface.primitive(p);
                bool all_in = true;
                for (std::uint32_t j = 0; j < range.count; ++j)
                    all_in &= flags[in_corners[range.first + j]];
                selected[p] = all_in;
            }
        } else {
            return failure("Extrude group must live on the primitive or point layer.");
        }
    }

    const auto read_prim_override = [&](StringView name, const char* label) -> Result<const Array<float>*> {
        if (name.empty())
            return nullptr;
        ATELIER_TRY_BIND(col, surface.read<float>(name, Layer::primitive));
        if (!*col)
            return failure(std::string("Extrude ") + label + " " + std::string{name.std_view()} +
                           " was not found on primitive layer.");
        return *col;
    };
    ATELIER_TRY_BIND(dist_override_res, read_prim_override(settings.distance_attribute, "distance_attribute"));
    ATELIER_TRY_BIND(inset_override_res, read_prim_override(settings.inset_attribute, "inset_attribute"));
    const Array<float>* dist_override = *dist_override_res;
    const Array<float>* inset_override = *inset_override_res;

    const Array<Vector2>* in_vtx_uv = nullptr;
    const Array<Vector2>* in_pt_uv = nullptr;
    if (const auto* uv_attr = surface.find("uv")) {
        if (uv_attr->layer == Layer::vertex && std::holds_alternative<SharedArray<Vector2>>(uv_attr->data))
            in_vtx_uv = &std::get<SharedArray<Vector2>>(uv_attr->data).array();
        else if (uv_attr->layer == Layer::point && std::holds_alternative<SharedArray<Vector2>>(uv_attr->data))
            in_pt_uv = &std::get<SharedArray<Vector2>>(uv_attr->data).array();
    }

    std::vector<Vector3d> face_normals(prim_count);
    bool any_selected = false;
    bool any_nonzero_inset = false;
    for (std::uint32_t p = 0; p < prim_count; ++p) {
        ATELIER_TRY_BIND(n, compute_polygon_normal(surface, surface.primitive(p)));
        face_normals[p] = *n;
        if (selected[p]) {
            any_selected = true;
            const float d = dist_override ? (*dist_override)[p] : settings.distance;
            const float ins = inset_override ? (*inset_override)[p] : settings.inset;
            if (!std::isfinite(d) || !std::isfinite(ins))
                return failure("Extrude per-primitive distance and inset overrides must be finite.");
            any_nonzero_inset |= (ins != 0.f);
        }
    }

    if (!settings.individual && any_nonzero_inset)
        return failure("Region extrude (individual = false) requires zero inset; use individual = true for inset.");

    SurfaceBuilder mesh;
    mesh.track_groups = !settings.front_group.empty() || !settings.side_group.empty();
    mesh.reserve(in_corners.size() * 2u, in_corners.size() * 6u, prim_count * 6u, true);
    std::vector<std::uint32_t> shared_input_map(in_positions.size(), std::numeric_limits<std::uint32_t>::max());
    const auto ensure_shared_point = [&](std::uint32_t in_pt) -> std::uint32_t {
        if (shared_input_map[in_pt] == std::numeric_limits<std::uint32_t>::max())
            shared_input_map[in_pt] = mesh.add_point(in_positions[in_pt], in_pt);
        return shared_input_map[in_pt];
    };
    const auto corner_uv = [&](std::uint32_t v_idx, std::uint32_t pt_idx, Vector3 pos) -> Vector2 {
        if (in_vtx_uv)
            return (*in_vtx_uv)[v_idx];
        if (in_pt_uv)
            return (*in_pt_uv)[pt_idx];
        return {pos.x * 0.5f, pos.z * 0.5f};
    };
    std::vector<std::uint32_t> base_ring;
    std::vector<std::uint32_t> top_ring;
    std::vector<Vector2> base_uv;
    std::vector<Vector2> top_uv;
    const auto emit_input_polygon = [&](std::uint32_t p, bool is_front) {
        const auto range = surface.primitive(p);
        base_ring.resize(range.count);
        base_uv.resize(range.count);
        for (std::uint32_t j = 0; j < range.count; ++j) {
            const std::uint32_t v_idx = range.first + j;
            const std::uint32_t pt_idx = in_corners[v_idx];
            base_ring[j] = ensure_shared_point(pt_idx);
            base_uv[j] = corner_uv(v_idx, pt_idx, in_positions[pt_idx]);
        }
        mesh.emit_flat_face(base_ring, base_uv, Vector3(face_normals[p]), p, is_front, false, false);
    };

    for (std::uint32_t p = 0; p < prim_count; ++p) {
        if (!selected[p])
            emit_input_polygon(p, false);
    }

    if (any_selected && settings.individual) {
        std::vector<Vector3d> proj;
        std::vector<Vector3d> e_unit;
        std::vector<Vector3d> m_inward;
        std::vector<Vector3d> inset_pts;
        for (std::uint32_t p = 0; p < prim_count; ++p) {
            if (!selected[p])
                continue;
            const auto range = surface.primitive(p);
            const std::uint32_t k = range.count;
            const float d = dist_override ? (*dist_override)[p] : settings.distance;
            const float ins = inset_override ? (*inset_override)[p] : settings.inset;
            const Vector3d n = face_normals[p];
            const double tolerance = detail::polygon_scale(surface, range).tolerance;

            if (d == 0.f && ins == 0.f) {
                emit_input_polygon(p, true);
                continue;
            }
            if (d == 0.f && settings.output_back)
                return failure("Zero-distance inset extrude requires output_back = false.");

            const Vector3d origin(in_positions[in_corners[range.first]]);
            proj.resize(k);
            for (std::uint32_t j = 0; j < k; ++j) {
                const Vector3d raw(in_positions[in_corners[range.first + j]]);
                proj[j] = raw - n * math::dot(raw - origin, n);
            }
            e_unit.resize(k);
            m_inward.resize(k);
            for (std::uint32_t j = 0; j < k; ++j) {
                const auto unit = math::try_normalize(proj[(j + 1) % k] - proj[j]);
                if (!unit)
                    return failure("Extrude encountered a degenerate edge on primitive " + std::to_string(p) + ".");
                e_unit[j] = *unit;
                m_inward[j] = math::cross(n, *unit);
            }

            inset_pts.resize(k);
            for (std::uint32_t j = 0; j < k; ++j) {
                const Vector3d m_prev = m_inward[(j + k - 1) % k];
                const Vector3d m_next = m_inward[j];
                const Vector3d bis = m_prev + m_next;
                const double denom = math::dot(bis, m_next);
                if (!(denom > 1e-6))
                    return failure("Extrude inset collapsed a corner on primitive " + std::to_string(p) + ".");
                inset_pts[j] = proj[j] + bis * (double{ins} / denom);
            }
            for (std::uint32_t j = 0; j < k; ++j) {
                if (!(math::dot(inset_pts[(j + 1) % k] - inset_pts[j], e_unit[j]) > tolerance))
                    return failure("Extrude inset collapsed or inverted primitive " + std::to_string(p) + ".");
            }

            base_ring.resize(k);
            top_ring.resize(k);
            base_uv.resize(k);
            top_uv.resize(k);
            for (std::uint32_t j = 0; j < k; ++j) {
                const std::uint32_t v_idx = range.first + j;
                const std::uint32_t orig_pt = in_corners[v_idx];
                base_ring[j] = settings.output_back ? mesh.add_point(in_positions[orig_pt], orig_pt)
                                                    : ensure_shared_point(orig_pt);
                top_ring[j] = mesh.add_point(Vector3(inset_pts[j] + n * double{d}), orig_pt);
                base_uv[j] = corner_uv(v_idx, orig_pt, in_positions[orig_pt]);
                const Vector3d rel = inset_pts[j] - origin;
                top_uv[j] = (ins == 0.f) ? base_uv[j]
                                         : Vector2{static_cast<float>(math::dot(rel, e_unit[0])),
                                                   static_cast<float>(math::dot(rel, m_inward[0]))};
            }

            const bool flip = (d < 0.f && settings.output_back) || (d == 0.f && ins < 0.f);
            mesh.emit_flat_face(top_ring, top_uv, flip ? Vector3(-n) : Vector3(n), p, true, false, flip);

            double u_cursor = 0.0;
            const float v_height = d != 0.f ? std::abs(d) : std::abs(ins);
            for (std::uint32_t j = 0; j < k; ++j) {
                const std::uint32_t next = (j + 1) % k;
                const double seg_len = math::length(proj[next] - proj[j]);
                const float u0 = static_cast<float>(u_cursor);
                const float u1 = static_cast<float>(u_cursor + seg_len);
                u_cursor += seg_len;
                mesh.emit_quad_or_split(
                    {base_ring[j], base_ring[next], top_ring[next], top_ring[j]},
                    {Vector2{u0, 0.f}, Vector2{u1, 0.f}, Vector2{u1, v_height}, Vector2{u0, v_height}}, nullptr, p,
                    false, true, flip);
            }

            if (settings.output_back)
                mesh.emit_flat_face(base_ring, base_uv, flip ? Vector3(n) : Vector3(-n), p, false, false, !flip);
        }
    } else if (any_selected) {
        Map<DirectedEdge, std::uint32_t> region_edges;
        std::vector<Vector3d> point_normal_sum(in_positions.size(), Vector3d{});
        std::vector<double> point_dist_sum(in_positions.size(), 0.0);
        std::vector<std::uint32_t> point_valence(in_positions.size(), 0u);
        bool any_negative = false;
        bool any_positive = false;

        for (std::uint32_t p = 0; p < prim_count; ++p) {
            if (!selected[p])
                continue;
            const float d = dist_override ? (*dist_override)[p] : settings.distance;
            if (d == 0.f)
                return failure("Region extrude (individual = false) requires nonzero extrusion distance.");
            any_negative |= (d < 0.f);
            any_positive |= (d > 0.f);
            const auto range = surface.primitive(p);
            for (std::uint32_t j = 0; j < range.count; ++j) {
                const std::uint32_t a = in_corners[range.first + j];
                const std::uint32_t b = in_corners[range.first + (j + 1) % range.count];
                region_edges.insert_or_assign(DirectedEdge{a, b}, p);
                point_normal_sum[a] += face_normals[p];
                point_dist_sum[a] += double{d};
                ++point_valence[a];
            }
        }
        if (any_negative && any_positive && settings.output_back)
            return failure("Closed region extrude requires consistent distance sign across the region.");

        const bool flip = any_negative && settings.output_back;
        std::vector<std::uint32_t> base_map(in_positions.size(), std::numeric_limits<std::uint32_t>::max());
        std::vector<std::uint32_t> top_map(in_positions.size(), std::numeric_limits<std::uint32_t>::max());
        for (std::size_t pt = 0; pt < in_positions.size(); ++pt) {
            if (point_valence[pt] == 0)
                continue;
            const auto src_pt = static_cast<std::uint32_t>(pt);
            base_map[pt] =
                settings.output_back ? mesh.add_point(in_positions[pt], src_pt) : ensure_shared_point(src_pt);
            const Vector3d avg_n = math::try_normalize(point_normal_sum[pt]).value_or(Vector3d{0.0, 1.0, 0.0});
            top_map[pt] = mesh.add_point(
                Vector3(Vector3d(in_positions[pt]) + avg_n * (point_dist_sum[pt] / point_valence[pt])), src_pt);
        }

        for (std::uint32_t p = 0; p < prim_count; ++p) {
            if (!selected[p])
                continue;
            const auto range = surface.primitive(p);
            const std::uint32_t k = range.count;
            base_ring.resize(k);
            top_ring.resize(k);
            base_uv.resize(k);
            for (std::uint32_t j = 0; j < k; ++j) {
                const std::uint32_t v_idx = range.first + j;
                const std::uint32_t orig_pt = in_corners[v_idx];
                base_ring[j] = base_map[orig_pt];
                top_ring[j] = top_map[orig_pt];
                base_uv[j] = corner_uv(v_idx, orig_pt, in_positions[orig_pt]);
            }
            if (k == 4) {
                mesh.emit_quad_or_split({top_ring[0], top_ring[1], top_ring[2], top_ring[3]},
                                        {base_uv[0], base_uv[1], base_uv[2], base_uv[3]}, nullptr, p, true, false,
                                        flip);
            } else {
                const Vector3 front_n = face_unit_normal(mesh.positions[top_ring[0]], mesh.positions[top_ring[1]],
                                                         mesh.positions[top_ring[2]], Vector3(face_normals[p]));
                mesh.emit_flat_face(top_ring, base_uv, flip ? -front_n : front_n, p, true, false, flip);
            }

            const float d_abs = std::abs(dist_override ? (*dist_override)[p] : settings.distance);
            for (std::uint32_t j = 0; j < k; ++j) {
                const std::uint32_t next = (j + 1) % k;
                const std::uint32_t orig_a = in_corners[range.first + j];
                const std::uint32_t orig_b = in_corners[range.first + next];
                if (region_edges.contains(DirectedEdge{orig_b, orig_a}))
                    continue;
                const float seg_len = math::distance(in_positions[orig_a], in_positions[orig_b]);
                mesh.emit_quad_or_split(
                    {base_ring[j], base_ring[next], top_ring[next], top_ring[j]},
                    {Vector2{0.f, 0.f}, Vector2{seg_len, 0.f}, Vector2{seg_len, d_abs}, Vector2{0.f, d_abs}}, nullptr,
                    p, false, true, flip);
            }

            if (settings.output_back) {
                mesh.emit_flat_face(base_ring, base_uv, flip ? Vector3(face_normals[p]) : Vector3(-face_normals[p]), p,
                                    false, false, !flip);
            }
        }
    }

    ATELIER_TRY_BIND(result, mesh.finish(surface));
    ATELIER_TRY(set_optional_group(*result, settings.front_group, std::move(mesh.front_flags)));
    ATELIER_TRY(set_optional_group(*result, settings.side_group, std::move(mesh.side_flags)));
    return std::move(*result);
}

Result<Geometry> sweep(const Geometry& backbone, const SweepSettings& settings, const Geometry* cross_section) {
    if (backbone.count(Layer::instance) > 0)
        return failure("Sweep backbone does not accept instances.");
    if (!math::is_finite(settings.up))
        return failure("Sweep up vector must be finite.");
    const auto up_unit_opt = math::try_normalize(Vector3d(settings.up));
    if (!up_unit_opt)
        return failure("Sweep up vector must be nonzero.");
    const Vector3d up_unit = *up_unit_opt;

    const auto prim_count = backbone.count(Layer::primitive);
    if (prim_count == 0) {
        if (backbone.count(Layer::point) > 0)
            return failure("Sweep backbone requires open polyline primitives.");
        return Geometry{};
    }
    for (std::uint32_t p = 0; p < prim_count; ++p) {
        if (backbone.primitive(p).kind != PrimitiveKind::polyline)
            return failure("Sweep backbone accepts only open polylines.");
    }

    struct ProfileVertex {
        double x{};

        double y{};

        float v{};
    };

    std::vector<ProfileVertex> profile_pts;
    bool closed_profile = true;
    bool smooth_ring_normals = false;

    // A bent side quad can split into two triangles; a non-convex cap becomes a fan.
    // Check their worst-case counts before allocating even the first profile ring.
    std::size_t point_count{}, corner_reserve{}, face_reserve{};
    const auto size_output = [&](std::uint32_t ring) -> Result<void> {
        const std::uint64_t stations = backbone.vertex_points().size();
        const std::uint64_t points = stations * ring;
        if (points > element_limit)
            return failure("Sweep exceeds the Geometry point limit.");
        const auto quads = (stations - prim_count) * (closed_profile ? ring : ring - 1u);
        const std::uint64_t caps = closed_profile && settings.end_caps ? 2ULL * prim_count : 0;
        const auto cap_triangles = caps * (ring - 2u);
        if (2 * quads + cap_triangles > element_limit)
            return failure("Sweep exceeds the Geometry face limit.");
        if (6 * quads + 3 * cap_triangles > element_limit)
            return failure("Sweep exceeds the Geometry corner limit.");
        point_count = points;
        corner_reserve = 4 * quads + caps * ring;
        face_reserve = quads + caps;
        return {};
    };

    switch (settings.profile) {
    case SweepProfile::rectangle: {
        if (!std::isfinite(settings.width) || !(settings.width > 0.f) || !std::isfinite(settings.height) ||
            !(settings.height > 0.f))
            return failure("Sweep rectangle requires finite positive width and height.");
        ATELIER_TRY(size_output(4));
        const double hx = 0.5 * double{settings.width};
        const double hy = 0.5 * double{settings.height};
        profile_pts = {{-hx, -hy, 0.f}, {hx, -hy, 0.25f}, {hx, hy, 0.5f}, {-hx, hy, 0.75f}};
        break;
    }
    case SweepProfile::tube: {
        if (!std::isfinite(settings.radius) || !(settings.radius > 0.f) || settings.segments < 3)
            return failure("Sweep tube requires finite positive radius and segments >= 3.");
        const double r = double{settings.radius};
        ATELIER_TRY(size_output(settings.segments));
        profile_pts.resize(settings.segments);
        for (std::uint32_t i = 0; i < settings.segments; ++i) {
            const double t = static_cast<double>(i) / settings.segments;
            const double phi = 2.0 * std::numbers::pi * t;
            profile_pts[i] = {r * std::cos(phi), r * std::sin(phi), static_cast<float>(t)};
        }
        smooth_ring_normals = settings.segments >= 8;
        break;
    }
    case SweepProfile::custom: {
        if (!cross_section || cross_section->count(Layer::primitive) != 1)
            return failure("Sweep custom profile requires a cross_section input with exactly 1 primitive.");
        const auto cr = cross_section->primitive(0);
        closed_profile = (cr.kind == PrimitiveKind::polygon);
        ATELIER_TRY(size_output(cr.count));
        const auto c_vp = cross_section->vertex_points();
        const auto& c_pos = cross_section->positions();
        profile_pts.resize(cr.count);
        double perim = 0.0;
        std::vector<double> acc(cr.count, 0.0);
        for (std::uint32_t i = 1; i < cr.count; ++i) {
            const Vector3 a = c_pos[c_vp[cr.first + i - 1]];
            const Vector3 b = c_pos[c_vp[cr.first + i]];
            perim += std::hypot(double{b.x} - double{a.x}, double{b.y} - double{a.y});
            acc[i] = perim;
        }
        if (closed_profile) {
            const Vector3 a = c_pos[c_vp[cr.first + cr.count - 1]];
            const Vector3 b = c_pos[c_vp[cr.first]];
            perim += std::hypot(double{b.x} - double{a.x}, double{b.y} - double{a.y});
        }
        const double denom = perim > 0.0 ? perim : 1.0;
        for (std::uint32_t i = 0; i < cr.count; ++i) {
            const Vector3 pt = c_pos[c_vp[cr.first + i]];
            profile_pts[i] = {double{pt.x}, double{pt.y}, static_cast<float>(acc[i] / denom)};
        }
        break;
    }
    default:
        return failure("Invalid SweepProfile value.");
    }

    const Array<float>* pscale_col = nullptr;
    if (!settings.scale_attribute.empty()) {
        ATELIER_TRY_BIND(col, backbone.read<float>(settings.scale_attribute, Layer::point));
        pscale_col = *col;
    }

    const auto& bb_positions = backbone.positions();
    const auto bb_corners = backbone.vertex_points();
    const std::uint32_t ring_size = static_cast<std::uint32_t>(profile_pts.size());
    const std::uint32_t seg_edges = closed_profile ? ring_size : (ring_size - 1u);

    SurfaceBuilder mesh;
    mesh.reserve(point_count, corner_reserve, face_reserve, false);
    std::vector<Vector3d> path_pts;
    std::vector<double> scales;
    std::vector<Vector3d> seg_t;
    std::vector<Vector3d> seg_r;
    std::vector<Vector3d> seg_u;
    std::vector<double> arc_u;
    std::vector<Vector3d> station_r_unit;
    std::vector<Vector3d> station_u_unit;
    std::vector<std::uint32_t> cap_pts;
    std::vector<Vector2> cap_uvs;
    std::vector<Vector3> cap_pos;
    for (std::uint32_t p = 0; p < prim_count; ++p) {
        const auto range = backbone.primitive(p);
        const std::uint32_t m = range.count;
        path_pts.resize(m);
        scales.assign(m, 1.0);
        for (std::uint32_t k = 0; k < m; ++k) {
            const std::uint32_t pt_idx = bb_corners[range.first + k];
            path_pts[k] = Vector3d(bb_positions[pt_idx]);
            if (pscale_col) {
                const float s = (*pscale_col)[pt_idx];
                if (!std::isfinite(s) || !(s > 0.f))
                    return failure("Sweep point scale attribute must be finite and positive.");
                scales[k] = double{s};
            }
        }

        seg_t.resize(m - 1);
        seg_r.resize(m - 1);
        seg_u.resize(m - 1);
        arc_u.resize(m);
        arc_u[0] = 0.0;
        for (std::uint32_t s = 0; s + 1 < m; ++s) {
            const Vector3d delta = path_pts[s + 1] - path_pts[s];
            const double len = math::length(delta);
            if (!(len > 1e-6))
                return failure("Sweep polyline " + std::to_string(p) + " contains a zero-length segment.");
            arc_u[s + 1] = arc_u[s] + len;
            const Vector3d t = delta / len;
            seg_t[s] = t;
            const Vector3d up_cross = math::cross(up_unit, t);
            if (const auto r_opt = math::try_normalize(up_cross); r_opt && math::length(up_cross) > 1e-4) {
                seg_r[s] = *r_opt;
                seg_u[s] = math::cross(t, *r_opt);
            } else if (s == 0) {
                const Vector3d fallback_axis = std::abs(t.x) < 0.9 ? Vector3d{1.0, 0.0, 0.0} : Vector3d{0.0, 0.0, 1.0};
                const Vector3d r0 = *math::try_normalize(math::cross(fallback_axis, t));
                seg_r[0] = r0;
                seg_u[0] = math::cross(t, r0);
            } else {
                const Vector3d prev_r = seg_r[s - 1];
                const Vector3d r_next = math::try_normalize(prev_r - t * math::dot(prev_r, t)).value_or(prev_r);
                seg_r[s] = r_next;
                seg_u[s] = math::cross(t, r_next);
            }
        }

        const auto base_station = static_cast<std::uint32_t>(mesh.positions.size());
        station_r_unit.resize(m);
        station_u_unit.resize(m);

        for (std::uint32_t k = 0; k < m; ++k) {
            Vector3d x_axis{};
            Vector3d y_axis{};
            if (k == 0 || k + 1 == m) {
                const std::uint32_t seg = (k == 0) ? 0u : (m - 2u);
                x_axis = station_r_unit[k] = seg_r[seg];
                y_axis = station_u_unit[k] = seg_u[seg];
            } else {
                const Vector3d r_dir = math::try_normalize(seg_r[k - 1] + seg_r[k]).value_or(seg_r[k]);
                const Vector3d u_dir = math::try_normalize(seg_u[k - 1] + seg_u[k]).value_or(seg_u[k]);
                x_axis = r_dir * (1.0 / std::max(0.25, math::dot(r_dir, seg_r[k])));
                y_axis = u_dir * (1.0 / std::max(0.25, math::dot(u_dir, seg_u[k])));
                station_r_unit[k] = r_dir;
                station_u_unit[k] = u_dir;
            }
            for (std::uint32_t i = 0; i < ring_size; ++i) {
                mesh.add_point(Vector3(path_pts[k] + x_axis * (scales[k] * profile_pts[i].x) +
                                       y_axis * (scales[k] * profile_pts[i].y)),
                               0u, false);
            }
        }

        const auto ring_point = [&](std::uint32_t k, std::uint32_t i) -> std::uint32_t {
            return base_station + k * ring_size + i;
        };
        const auto smooth_normal = [&](std::uint32_t k, std::uint32_t i, Vector3 fallback) -> Vector3 {
            const Vector3d radial = station_r_unit[k] * profile_pts[i].x + station_u_unit[k] * profile_pts[i].y;
            if (const auto unit = math::try_normalize(radial))
                return Vector3(*unit);
            return fallback;
        };

        for (std::uint32_t k = 0; k + 1 < m; ++k) {
            const float u0 = static_cast<float>(arc_u[k]);
            const float u1 = static_cast<float>(arc_u[k + 1]);
            for (std::uint32_t i = 0; i < seg_edges; ++i) {
                const std::uint32_t next = (i + 1) % ring_size;
                const float v0 = profile_pts[i].v;
                const float v1 = (closed_profile && next == 0) ? 1.f : profile_pts[next].v;
                const std::array<std::uint32_t, 4> q{ring_point(k, i), ring_point(k, next), ring_point(k + 1, next),
                                                     ring_point(k + 1, i)};
                const std::array<Vector2, 4> q_uv{Vector2{u0, v0}, Vector2{u0, v1}, Vector2{u1, v1}, Vector2{u1, v0}};
                if (smooth_ring_normals) {
                    const Vector3 flat_n0 = face_unit_normal(mesh.positions[q[0]], mesh.positions[q[1]],
                                                             mesh.positions[q[2]], {0.f, 1.f, 0.f});
                    const std::array<Vector3, 4> q_nrm{smooth_normal(k, i, flat_n0), smooth_normal(k, next, flat_n0),
                                                       smooth_normal(k + 1, next, flat_n0),
                                                       smooth_normal(k + 1, i, flat_n0)};
                    mesh.emit_quad_or_split(q, q_uv, &q_nrm, p, false, false, false);
                } else {
                    mesh.emit_quad_or_split(q, q_uv, nullptr, p, false, false, false);
                }
            }
        }

        if (closed_profile && settings.end_caps) {
            const auto emit_cap = [&](std::uint32_t k, bool reverse, Vector3 cap_normal) {
                cap_pts.resize(ring_size);
                cap_uvs.resize(ring_size);
                cap_pos.resize(ring_size);
                for (std::uint32_t j = 0; j < ring_size; ++j) {
                    const std::uint32_t idx = reverse ? (j == 0 ? 0u : ring_size - j) : j;
                    cap_pts[j] = ring_point(k, idx);
                    cap_pos[j] = mesh.positions[cap_pts[j]];
                    cap_uvs[j] = {static_cast<float>(profile_pts[idx].x), static_cast<float>(profile_pts[idx].y)};
                }
                if (detail::validate_polygon(cap_pos)) {
                    mesh.emit_flat_face(cap_pts, cap_uvs, cap_normal, p, false, false, false);
                } else {
                    for (std::uint32_t j = 1; j + 1 < ring_size; ++j) {
                        const std::array<std::uint32_t, 3> tri{cap_pts[0], cap_pts[j], cap_pts[j + 1]};
                        const std::array<Vector2, 3> tri_uv{cap_uvs[0], cap_uvs[j], cap_uvs[j + 1]};
                        mesh.emit_flat_face(tri, tri_uv, cap_normal, p, false, false, false);
                    }
                }
            };
            emit_cap(0u, true, Vector3(-seg_t[0]));
            emit_cap(m - 1u, false, Vector3(seg_t[m - 2]));
        }
    }

    return mesh.finish(backbone);
}

} // namespace atelier::procedural

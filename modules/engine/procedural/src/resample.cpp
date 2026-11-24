#include "procedural/operations.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace atelier::procedural {
namespace {
using math::Vector3;
using math::Vector3d;

struct Edge {
    std::uint32_t vertex; // First source corner of a nonzero edge.
    double end; // Distance from the start of this primitive.
    std::uint32_t last_grid{};
    bool ends_on_grid{};
    bool keep_end{};
};

struct Curve {
    std::size_t first_edge;
    std::size_t end_edge;
    bool closed;
    double step;
};

struct Sample {
    std::uint32_t a, b; // Source corner indices; point attributes go through vertex_points().
    double t;
};

template <class T>
T blend(const T& a, const T& b, double t, Usage usage) {
    if (t == 0.0)
        return a;
    if (t == 1.0)
        return b;
    if constexpr (std::same_as<T, float>) {
        return static_cast<float>(std::lerp(double(a), double(b), t));
    } else if constexpr (std::same_as<T, math::Vector2> || std::same_as<T, Vector3> || std::same_as<T, math::Vector4>) {
        T value{};
        for (std::size_t axis = 0; axis < T::dimensions; ++axis)
            value[axis] = static_cast<float>(std::lerp(double(a[axis]), double(b[axis]), t));
        if constexpr (std::same_as<T, Vector3>)
            if (usage == Usage::normal)
                return Vector3(math::try_normalize(Vector3d(value)).value_or(Vector3d{}));
        return value;
    } else {
        // Match the Boolean transfer policy for labels, matrices and rotations: take a donor.
        return t <= 0.5 ? a : b;
    }
}
} // namespace

Result<Geometry> resample(const Geometry& input, const ResampleSettings& settings) {
    const bool by_length = settings.mode == ResampleMode::spacing;
    if (!by_length && settings.mode != ResampleMode::segments)
        return failure("Resample has an invalid mode.");
    if (by_length && (!(settings.spacing > 0.f) || !std::isfinite(settings.spacing)))
        return failure("Resample spacing must be finite and positive.");
    if (!by_length && settings.segments == 0)
        return failure("Resample requires at least one segment.");
    const auto primitive_count = input.count(Layer::primitive);
    if (input.count(Layer::instance))
        return failure("Resample does not accept instances.");
    if (!primitive_count && input.count(Layer::point))
        return failure("Resample requires polylines; connect loose points with Polyline first.");
    ATELIER_TRY(input.read<float>("arclength", Layer::point));
    ATELIER_TRY(input.read<float>("curveu", Layer::point));
    ATELIER_TRY(input.read<Vector3>("tangent", Layer::point));

    const auto points = input.vertex_points();
    const auto& pos = input.positions();
    const auto edge_position = [&](std::uint32_t v, double t) {
        return blend(pos[points[v]], pos[points[v + 1]], t, Usage::point);
    };
    const auto delta = [&](std::uint32_t v) { return Vector3d(pos[points[v + 1]]) - Vector3d(pos[points[v]]); };

    // Size the complete output before reserving any sample-sized buffers. Only authored
    // edges are visited here, even when an untrusted spacing asks for billions of samples.
    std::vector<Edge> edges;
    std::vector<Curve> curves;
    std::uint64_t vertex_count{}, point_count{};
    for (std::uint32_t p = 0; p < primitive_count; ++p) {
        const auto prim = input.primitive(p);
        if (prim.kind != PrimitiveKind::polyline)
            return failure("Resample requires polyline primitives.");
        const auto first = edges.size();
        double length = 0.0;
        for (std::uint32_t v = prim.first; v + 1 < prim.first + prim.count; ++v) {
            const auto distance = math::length(delta(v));
            if (distance == 0.0)
                continue;
            const double next = length + distance;
            if (next == length || next > std::numeric_limits<float>::max())
                return failure("Resample arc length exceeds representable precision.");
            length = next;
            edges.push_back({v, length});
        }
        if (first == edges.size())
            return failure("Resample encountered a zero-length polyline at primitive " + std::to_string(p) + ".");
        double requested = by_length ? std::ceil(length / settings.spacing) : double(settings.segments);
        const double step = by_length ? double(settings.spacing) : length / requested;
        if (by_length && requested >= 2 && requested <= element_limit) {
            const double last_grid = (requested - 1) * step;
            const double last_start = edges.size() == first + 1 ? 0.0 : edges[edges.size() - 2].end;
            // A decimal spacing (e.g. 0.1f on a 0.3f line) can leave a tail shorter
            // than one position ULP. Emit its exact endpoint once.
            if (last_grid >= last_start && length - last_grid < step * 0.5 &&
                edge_position(edges.back().vertex, (last_grid - last_start) / (length - last_start)) ==
                    pos[points[prim.first + prim.count - 1]])
                requested -= 1;
        }
        if (requested + 1 > element_limit - vertex_count)
            return failure("Resample exceeds the Geometry vertex limit.");
        const auto divisions = static_cast<std::uint32_t>(requested);
        const bool closed = points[prim.first] == points[prim.first + prim.count - 1];
        std::uint64_t count = std::uint64_t(divisions) + 1;
        for (auto i = first; i + 1 < edges.size(); ++i) {
            auto& edge = edges[i];
            const auto nearest = static_cast<std::uint32_t>(std::round(edge.end / step));
            // Snap a coincident grid point to the authored corner only at rounding scale.
            // The local edge bound prevents two short adjacent edges claiming the same sample.
            const double prev = i == first ? 0.0 : edges[i - 1].end;
            const double offset = nearest * step - edge.end;
            const double local_span = std::min({step, edge.end - prev, edges[i + 1].end - edge.end});
            if (nearest > 0 && nearest < divisions && std::abs(offset) <= 0.25 * local_span) {
                const auto on_grid = offset < 0.0
                                         ? edge_position(edge.vertex, 1.0 + offset / (edge.end - prev))
                                         : edge_position(edges[i + 1].vertex, offset / (edges[i + 1].end - edge.end));
                edge.ends_on_grid = std::abs(offset) <= 8.0 * std::numeric_limits<double>::epsilon() * edge.end ||
                                    on_grid == pos[points[edge.vertex + 1]];
            }
            edge.last_grid = edge.ends_on_grid
                                 ? nearest
                                 : std::min(divisions - 1, static_cast<std::uint32_t>(std::floor(edge.end / step)));
            if (settings.preserve_corners && !edge.ends_on_grid) {
                const auto a = delta(edge.vertex), b = delta(edges[i + 1].vertex);
                edge.keep_end = math::cross(a, b) != Vector3d{} || math::dot(a, b) < 0.0;
                count += edge.keep_end;
            }
        }
        if (closed && count < 4)
            return failure("Resample needs at least three sample points on a closed polyline.");
        if (count > element_limit - vertex_count)
            return failure("Resample exceeds the Geometry vertex limit.");
        vertex_count += count;
        point_count += count - closed;
        edges.back().last_grid = divisions;
        edges.back().ends_on_grid = true;
        curves.push_back({first, edges.size(), closed, step});
    }

    Array<Vector3> positions, tangents;
    Array<float> lengths, parameters;
    Array<std::uint32_t> vertices, offsets{0};
    Array<PrimitiveKind> kinds;
    std::vector<Sample> donors;
    positions.reserve(point_count);
    tangents.reserve(point_count);
    lengths.reserve(point_count);
    parameters.reserve(point_count);
    vertices.reserve(vertex_count);
    const auto generated = [](const Attribute& attr) {
        return attr.name == "P" || attr.name == "arclength" || attr.name == "curveu" || attr.name == "tangent";
    };
    const bool need_donors = std::ranges::any_of(input.attributes(), [&](const Attribute& attr) {
        return (attr.layer == Layer::point || attr.layer == Layer::vertex) && !generated(attr);
    });
    if (need_donors)
        donors.reserve(vertex_count);
    offsets.reserve(curves.size() + 1);
    kinds.resize(curves.size(), PrimitiveKind::polyline);

    for (std::uint32_t p = 0; p < curves.size(); ++p) {
        const auto& curve = curves[p];
        const auto prim = input.primitive(p);
        const auto first_point = static_cast<std::uint32_t>(positions.size());
        const auto length = edges[curve.end_edge - 1].end;
        auto emit = [&](Sample donor, double d, Vector3 tangent) -> Result<void> {
            if (need_donors)
                donors.push_back(donor);
            if (d == length && curve.closed) {
                if (positions.back() == positions[first_point])
                    return failure("Resample spacing collapses the seam at float precision.");
                vertices.add(first_point);
                return {};
            }
            const Vector3 point = blend(pos[points[donor.a]], pos[points[donor.b]], donor.t, Usage::point);
            if (positions.size() > first_point && positions.back() == point)
                return failure("Resample produced coincident consecutive samples; adjust spacing or preserve corners.");
            vertices.add(static_cast<std::uint32_t>(positions.size()));
            positions.add(point);
            tangents.add(tangent);
            lengths.add(static_cast<float>(d));
            parameters.add(static_cast<float>(d / length));
            return {};
        };
        const auto direction = [&](std::size_t i) { return Vector3(*math::try_normalize(delta(edges[i].vertex))); };
        Vector3 tangent = direction(curve.first_edge);
        ATELIER_TRY(emit({prim.first, prim.first, 0}, 0.0, tangent));
        std::uint32_t grid = 1;
        double start = 0.0;
        for (auto i = curve.first_edge; i < curve.end_edge; ++i) {
            const auto& edge = edges[i];
            const bool last = i + 1 == curve.end_edge;
            const auto next_tangent = last ? tangent : direction(i + 1);
            const auto end_vertex = last ? prim.first + prim.count - 1 : edges[i + 1].vertex;
            for (; grid <= edge.last_grid; ++grid) {
                if (edge.ends_on_grid && grid == edge.last_grid) {
                    ATELIER_TRY(emit({end_vertex, end_vertex, 0}, edge.end, next_tangent));
                } else {
                    const double d = grid * curve.step;
                    ATELIER_TRY(emit({edge.vertex, edge.vertex + 1, (d - start) / (edge.end - start)}, d, tangent));
                }
            }
            if (edge.keep_end)
                ATELIER_TRY(emit({end_vertex, end_vertex, 0}, edge.end, next_tangent));
            tangent = next_tangent;
            start = edge.end;
        }
        offsets.add(static_cast<std::uint32_t>(vertices.size()));
    }
    ATELIER_TRY_BIND(output,
                     Geometry::primitives(std::move(positions), std::move(vertices), offsets, std::move(kinds)));
    for (const auto& attr : input.attributes()) {
        if (generated(attr))
            continue;
        if (attr.layer != Layer::point && attr.layer != Layer::vertex) {
            ATELIER_TRY(output->set(attr.name, attr.layer, attr.data, attr.usage));
            continue;
        }
        const bool on_points = attr.layer == Layer::point;
        const auto transfer = [&](const auto& column) -> ColumnData {
            using T = typename std::remove_cvref_t<decltype(column)>::Value;
            Array<T> values;
            values.reserve(output->count(attr.layer));
            const auto& source = column.array();
            for (std::size_t v = 0; v < donors.size(); ++v) {
                if (on_points && output->vertex_points()[v] < values.size())
                    continue; // Closed seam shares its start point but has a separate vertex attribute.
                const auto [a, b, t] = donors[v];
                values.add(
                    blend<T>(source[on_points ? points[a] : a], source[on_points ? points[b] : b], t, attr.usage));
            }
            return SharedArray<T>{std::move(values)};
        };
        ATELIER_TRY(output->set(attr.name, attr.layer, std::visit(transfer, attr.data), attr.usage));
    }
    ATELIER_TRY(output->set(String::literal<"arclength">(), Layer::point, SharedArray<float>{std::move(lengths)}));
    ATELIER_TRY(output->set(String::literal<"curveu">(), Layer::point, SharedArray<float>{std::move(parameters)}));
    ATELIER_TRY(output->set(String::literal<"tangent">(), Layer::point, SharedArray<Vector3>{std::move(tangents)},
                            Usage::vector));
    return std::move(*output);
}
} // namespace atelier::procedural

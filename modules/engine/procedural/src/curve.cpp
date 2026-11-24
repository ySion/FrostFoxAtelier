#include "detail/curve_output.hpp"
#include "foundation/result.hpp"
#include "procedural/operations.hpp"

namespace atelier::procedural {
using detail::ArcEdge;
using detail::ArcStation;
using detail::CurveOutput;
using detail::PolylineArc;
using detail::PolylineArcs;
using math::Vector3;
using math::Vector3d;

namespace {
Result<double> measure_polyline(const Geometry& input, std::uint32_t p, std::vector<ArcEdge>* edges) {
    if (p >= input.count(Layer::primitive))
        return failure("Polyline primitive index " + std::to_string(p) + " is out of bounds.");
    const auto prim = input.primitive(p);
    if (prim.kind != PrimitiveKind::polyline)
        return failure("Expected a polyline at primitive " + std::to_string(p) + ".");
    const auto points = input.vertex_points();
    const auto& pos = input.positions();
    double length = 0.;
    for (auto v = prim.first; v + 1 < prim.first + prim.count; ++v) {
        const double d = math::length(Vector3d(pos[points[v + 1]]) - Vector3d(pos[points[v]]));
        if (d == 0.)
            continue;
        const double next = length + d;
        if (!(next > length && next <= std::numeric_limits<float>::max()))
            return failure("Polyline arc length exceeds representable precision at primitive " + std::to_string(p) +
                           ".");
        length = next;
        if (edges)
            edges->push_back({v, length});
    }
    if (length == 0.)
        return failure("Zero-length polyline at primitive " + std::to_string(p) + ".");
    return length;
}

Result<void> measure_edges(const Geometry& input, std::uint32_t p, std::vector<ArcEdge>& edges) {
    ATELIER_TRY(measure_polyline(input, p, &edges));
    return {};
}

bool generated(const Attribute& attr) {
    return attr.name == "P" || attr.name == "arclength" || attr.name == "curveu" || attr.name == "tangent";
}

template <class T>
T blend(const T& a, const T& b, double t, Usage usage) {
    if (t == 0.)
        return a;
    if (t == 1.)
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
        return t <= 0.5 ? a : b;
    }
}

struct Slice {
    double start, end;
    std::size_t first_edge, last_edge, first_corner, end_corner;

    std::uint64_t count() const { return 2 + end_corner - first_corner; }
};

Result<Slice> slice_interval(const PolylineArc& arc, float start, float end, bool end_from_tip) {
    const double a = arc.distance(start), b = end_from_tip ? arc.length() - arc.distance(end) : arc.distance(end);
    if (!(start >= 0.f && end >= 0.f && a >= 0. && a < b && b <= arc.length()))
        return failure("Curve slice requires 0 <= start < end <= polyline length.");
    const auto lo = arc.edge_at(a), hi = arc.edge_at(b);
    // Each retained edge end is strictly inside the interval. Values that round to the
    // same output position as a cut bind to that endpoint only once.
    auto first = lo, stop = hi;
    if (stop > first && arc.edges[stop - 1].end == b)
        --stop;
    const auto pa = arc.position(arc.station(lo, a)), pb = arc.position(arc.station(hi, b));
    if (first < stop && arc.position(arc.corner_after(first)) == pa)
        ++first;
    if (first < stop && arc.position(arc.corner_after(stop - 1)) == pb)
        --stop;
    if (first == stop && pa == pb)
        return failure("Curve slice endpoints collapse at output precision.");
    return Slice{a, b, lo, hi, first, stop};
}

template <class Emit>
Result<void> visit_slice(const PolylineArc& arc, const Slice& slice, Emit emit) {
    ATELIER_TRY(emit(arc.station(slice.first_edge, slice.start), slice.start, slice.first_edge));
    for (auto i = slice.first_corner; i < slice.end_corner; ++i)
        ATELIER_TRY(emit(arc.corner_after(i), arc.edges[i].end, i + 1));
    return emit(arc.station(slice.last_edge, slice.end), slice.end, slice.last_edge);
}
} // namespace

Result<void> PolylineArcs::measure() {
    const auto count = source.count(Layer::primitive);
    if (source.count(Layer::instance))
        return failure("Curve operations do not accept instances.");
    if (!count && source.count(Layer::point))
        return failure("Curve operations require polylines; connect loose points with Polyline first.");
    ATELIER_TRY(source.read<float>("arclength", Layer::point));
    ATELIER_TRY(source.read<float>("curveu", Layer::point));
    ATELIER_TRY(source.read<Vector3>("tangent", Layer::point));
    offsets.reserve(std::size_t(count) + 1);
    for (std::uint32_t p = 0; p < count; ++p) {
        ATELIER_TRY(measure_edges(source, p, edges));
        offsets.push_back(edges.size());
    }
    return {};
}

CurveOutput::CurveOutput(const Geometry& source, bool points_only)
    : source_(source), points_only_(points_only),
      need_donors_(std::ranges::any_of(source.attributes(), [](const Attribute& attr) {
          return (attr.layer == Layer::point || attr.layer == Layer::vertex) && !generated(attr);
      })) {}

void CurveOutput::reserve(std::uint64_t points, std::uint64_t vertices) {
    positions_.reserve(points);
    lengths_.reserve(points);
    parameters_.reserve(points);
    tangents_.reserve(points);
    if (!points_only_) {
        vertices_.reserve(vertices);
        offsets_.reserve(std::size_t(source_.count(Layer::primitive)) + 1);
    }
    if (need_donors_)
        donors_.reserve(points_only_ ? points : vertices);
}

Result<void> CurveOutput::emit(const PolylineArc& arc, ArcStation donor, double distance, Vector3 tangent, bool close) {
    if (need_donors_)
        donors_.push_back(donor);
    if (close) {
        if (positions_.back() == positions_[first_point_])
            return failure("Curve samples collapse at the closed seam.");
        vertices_.add(first_point_);
        return {};
    }
    const auto point = arc.position(donor);
    if (positions_.size() > first_point_ && positions_.back() == point)
        return failure("Coincident consecutive curve samples; adjust spacing or preserve corners.");
    if (!points_only_)
        vertices_.add(positions_.size());
    positions_.add(point);
    lengths_.add(static_cast<float>(distance));
    parameters_.add(static_cast<float>(distance / arc.length()));
    tangents_.add(tangent);
    return {};
}

void CurveOutput::finish_primitive() {
    first_point_ = positions_.size();
    if (!points_only_)
        offsets_.add(vertices_.size());
}

Result<Geometry> CurveOutput::geometry() {
    Array<PrimitiveKind> kinds;
    if (!points_only_)
        kinds.resize(offsets_.size() - 1, PrimitiveKind::polyline);
    ATELIER_TRY_BIND(output, points_only_ ? Geometry::points(std::move(positions_))
                                          : Geometry::primitives(std::move(positions_), std::move(vertices_), offsets_,
                                                                 std::move(kinds)));
    const auto points = source_.vertex_points();
    for (const auto& attr : source_.attributes()) {
        if (generated(attr))
            continue;
        const auto layer =
            points_only_ && (attr.layer == Layer::vertex || attr.layer == Layer::primitive) ? Layer::point : attr.layer;
        if (attr.layer != Layer::point && attr.layer != Layer::vertex) {
            ATELIER_TRY(output->set(attr.name, layer, attr.data, attr.usage));
            continue;
        }
        const bool on_points = attr.layer == Layer::point;
        const auto transfer = [&](const auto& column) -> ColumnData {
            using T = typename std::remove_cvref_t<decltype(column)>::Value;
            Array<T> values;
            values.reserve(output->count(layer));
            const auto& data = column.array();
            for (std::size_t v = 0; v < donors_.size(); ++v) {
                if (!points_only_ && on_points && output->vertex_points()[v] < values.size())
                    continue;
                const auto [a, b, t] = donors_[v];
                values.add(blend<T>(data[on_points ? points[a] : a], data[on_points ? points[b] : b], t, attr.usage));
            }
            return SharedArray<T>{std::move(values)};
        };
        ATELIER_TRY(output->set(attr.name, layer, std::visit(transfer, attr.data), attr.usage));
    }
    ATELIER_TRY(output->set(String::literal<"arclength">(), Layer::point, SharedArray<float>{std::move(lengths_)}));
    ATELIER_TRY(output->set(String::literal<"curveu">(), Layer::point, SharedArray<float>{std::move(parameters_)}));
    ATELIER_TRY(output->set(String::literal<"tangent">(), Layer::point, SharedArray<Vector3>{std::move(tangents_)},
                            Usage::vector));
    return std::move(*output);
}

Result<Geometry> sample_curves(const Geometry& input, float distance, bool from_end) {
    if (!(std::isfinite(distance) && distance >= 0.f))
        return failure("Curve sample distance must be finite and nonnegative.");
    PolylineArcs curves{input};
    ATELIER_TRY(curves.measure());
    CurveOutput output(input, true);
    const auto count = input.count(Layer::primitive);
    output.reserve(count, 0);
    for (std::uint32_t p = 0; p < count; ++p) {
        const auto arc = curves[p];
        const double d = from_end ? arc.length() - arc.distance(distance) : arc.distance(distance);
        if (!(d >= 0. && d <= arc.length()))
            return failure("Curve sample distance exceeds the length of primitive " + std::to_string(p) + ".");
        const auto i = arc.edge_at(d);
        ATELIER_TRY(output.emit(arc, arc.station(i, d), d, arc.tangent(i)));
        output.finish_primitive();
    }
    return output.geometry();
}

Result<Geometry> slice_curves(const Geometry& input, float start, float end, bool end_from_tip) {
    if (!(std::isfinite(start) && std::isfinite(end) && start >= 0.f && end >= 0.f))
        return failure("Curve slice distances must be finite and nonnegative.");
    PolylineArcs curves{input};
    ATELIER_TRY(curves.measure());
    std::vector<Slice> slices;
    std::uint64_t points{}, vertices{};
    for (std::uint32_t p = 0; p < input.count(Layer::primitive); ++p) {
        const auto arc = curves[p];
        ATELIER_TRY_BIND(slice, slice_interval(arc, start, end, end_from_tip));
        vertices += slice->count();
        if (vertices > element_limit)
            return failure("Curve slices exceed the Geometry vertex limit.");
        points += slice->count() - (arc.closed() && slice->start == 0. && slice->end == arc.length());
        slices.push_back(*slice);
    }
    CurveOutput output(input, false);
    output.reserve(points, vertices);
    for (std::uint32_t p = 0; p < slices.size(); ++p) {
        const auto arc = curves[p];
        const bool closed = arc.closed() && slices[p].start == 0.;
        const auto emit = [&](ArcStation s, double d, std::size_t i) {
            return output.emit(arc, s, d, arc.tangent(i), closed && d == arc.length());
        };
        ATELIER_TRY(visit_slice(arc, slices[p], emit));
        output.finish_primitive();
    }
    return output.geometry();
}

Result<float> polyline_length(const Geometry& curves, std::uint32_t primitive) {
    ATELIER_TRY_BIND(length, measure_polyline(curves, primitive, nullptr));
    return static_cast<float>(*length);
}

// This position-only projection lets a Source modeler apply its own layout to many pieces.
// Measure once per batch. Locate each boundary logarithmically and visit only retained corners.
Result<void> read_curve_slices(const Geometry& curves, std::uint32_t primitive, const Array<math::Vector2>& intervals,
                               Array<Vector3>& positions, Array<std::uint32_t>& offsets) {
    if (primitive >= curves.count(Layer::primitive))
        return failure("Curve slice primitive index is out of bounds.");
    std::vector<ArcEdge> edges;
    ATELIER_TRY(measure_edges(curves, primitive, edges));
    const PolylineArc arc{curves, curves.primitive(primitive), edges};
    std::vector<Slice> slices;
    std::uint64_t count{};
    for (const auto& interval : intervals) {
        ATELIER_TRY_BIND(slice, slice_interval(arc, interval.x, interval.y, false));
        count += slice->count();
        if (count > element_limit)
            return failure("Curve slices exceed the Geometry vertex limit.");
        slices.push_back(*slice);
    }
    Array<Vector3> result_points;
    Array<std::uint32_t> result_offsets{0};
    result_points.reserve(count);
    result_offsets.reserve(slices.size() + 1);
    for (const auto& slice : slices) {
        const auto append = [&](ArcStation s, double, std::size_t) -> Result<void> {
            result_points.add(arc.position(s));
            return {};
        };
        ATELIER_TRY(visit_slice(arc, slice, append));
        result_offsets.add(result_points.size());
    }
    positions = std::move(result_points);
    offsets = std::move(result_offsets);
    return {};
}
} // namespace atelier::procedural

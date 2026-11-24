#include "detail/curve_output.hpp"
#include "foundation/result.hpp"
#include "procedural/operations.hpp"

namespace atelier::procedural {
namespace {
struct GridEdge {
    std::uint32_t last{};
    bool ends_on_grid{}, keep_end{};
};
}

Result<Geometry> resample(const Geometry& input, const ResampleSettings& settings) {
    const bool fixed_step = settings.mode == ResampleMode::spacing;
    const bool by_count = settings.mode == ResampleMode::segments;
    if (!fixed_step && !by_count && settings.mode != ResampleMode::fit_spacing)
        return failure("Resample has an invalid mode.");
    if (!by_count && (!(settings.spacing > 0.f) || !std::isfinite(settings.spacing)))
        return failure("Resample spacing must be finite and positive.");
    if (by_count && settings.segments == 0)
        return failure("Resample requires at least one segment.");

    detail::PolylineArcs curves{input};
    ATELIER_TRY(curves.measure());
    const auto primitive_count = input.count(Layer::primitive);
    std::vector<GridEdge> grid_edges(curves.edges.size());
    std::vector<double> steps;
    steps.reserve(primitive_count);
    // Count the entire lattice before allocating output buffers, including preserved turns.
    std::uint64_t point_count{}, vertex_count{};
    for (std::uint32_t p = 0; p < primitive_count; ++p) {
        const auto arc = curves[p];
        const double length = arc.length();
        double requested = by_count ? double(settings.segments) : std::ceil(length / double(settings.spacing));
        if (settings.mode == ResampleMode::fit_spacing && arc.closed())
            requested = std::max(3., requested);
        const double step = fixed_step ? double(settings.spacing) : length / requested;
        if (fixed_step && requested >= 2 && requested <= element_limit) {
            const double last_grid = (requested - 1) * step;
            const auto last = arc.edges.size() - 1;
            // An unrepresentable tail in fixed-step mode emits the exact endpoint once.
            if (last_grid >= arc.start(last) && length - last_grid < step * 0.5 &&
                arc.position(arc.station(last, last_grid)) == arc.position(arc.corner_after(last)))
                --requested;
        }
        if (requested + 1 > element_limit - vertex_count)
            return failure("Resample exceeds the Geometry vertex limit.");
        const auto divisions = static_cast<std::uint32_t>(requested);
        auto grid = std::span{grid_edges}.subspan(curves.offsets[p], arc.edges.size());
        std::uint64_t count = std::uint64_t(divisions) + 1;
        for (std::size_t i = 0; i + 1 < grid.size(); ++i) {
            auto& edge = grid[i];
            const auto end = arc.edges[i].end;
            const auto nearest = static_cast<std::uint32_t>(std::round(end / step));
            // At most one corner can claim a station. Compare at position precision as well as distance precision.
            const double offset = nearest * step - end;
            const double local_span = std::min({step, end - arc.start(i), arc.edges[i + 1].end - end});
            if (nearest > 0 && nearest < divisions && std::abs(offset) <= 0.25 * local_span) {
                const auto candidate = arc.station(offset < 0. ? i : i + 1, nearest * step);
                edge.ends_on_grid = std::abs(offset) <= 8. * std::numeric_limits<double>::epsilon() * end ||
                                    arc.position(candidate) == arc.position(arc.corner_after(i));
            }
            edge.last = edge.ends_on_grid ? nearest
                                          : std::min(divisions - 1, static_cast<std::uint32_t>(std::floor(end / step)));
            if (settings.preserve_corners && !edge.ends_on_grid) {
                const auto a = arc.delta(i), b = arc.delta(i + 1);
                edge.keep_end = math::cross(a, b) != math::Vector3d{} || math::dot(a, b) < 0.;
                count += edge.keep_end;
            }
        }
        if (arc.closed() && count < 4)
            return failure("Resample needs at least three sample points on a closed polyline.");
        if (count > element_limit - vertex_count)
            return failure("Resample exceeds the Geometry vertex limit.");
        vertex_count += count;
        point_count += count - arc.closed();
        grid.back() = {divisions, true, false};
        steps.push_back(step);
    }

    detail::CurveOutput output(input, false);
    output.reserve(point_count, vertex_count);
    for (std::uint32_t p = 0; p < primitive_count; ++p) {
        const auto arc = curves[p];
        auto tangent = arc.tangent(0);
        ATELIER_TRY(output.emit(arc, arc.station(0, 0.), 0., tangent));
        std::uint32_t station = 1;
        for (std::size_t i = 0; i < arc.edges.size(); ++i) {
            const auto& edge = grid_edges[curves.offsets[p] + i];
            const bool last = i + 1 == arc.edges.size();
            const auto next_tangent = last ? tangent : arc.tangent(i + 1);
            for (; station <= edge.last; ++station) {
                if (edge.ends_on_grid && station == edge.last) {
                    ATELIER_TRY(
                        output.emit(arc, arc.corner_after(i), arc.edges[i].end, next_tangent, last && arc.closed()));
                } else {
                    const double d = station * steps[p];
                    ATELIER_TRY(output.emit(arc, arc.station(i, d), d, tangent));
                }
            }
            if (edge.keep_end)
                ATELIER_TRY(output.emit(arc, arc.corner_after(i), arc.edges[i].end, next_tangent));
            tangent = next_tangent;
        }
        output.finish_primitive();
    }
    return output.geometry();
}
} // namespace atelier::procedural

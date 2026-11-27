#include "detail/bevel_attributes.hpp"
#include "detail/interpolate_attribute.hpp"
#include "detail/triangles.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace atelier::procedural::detail {
namespace {

using math::Vector3;

using math::Vector3d;

struct Sample {
    std::array<std::uint32_t, 3> ids;

    std::array<double, 3> weights;

    Sample as_points(Span<const std::uint32_t> corners) const {
        return {{corners[ids[0]], corners[ids[1]], corners[ids[2]]}, weights};
    }
};

Sample closest_sample(const Geometry& mesh, PrimitiveRange range, Vector3 position) {
    const auto corners = mesh.vertex_points();
    const auto& positions = mesh.positions();
    const auto point = [&](std::uint32_t corner) { return Vector3d(positions[corners[corner]]); };
    const Vector3d query(position);
    const auto origin = point(range.first);
    const auto q = query - origin;
    Sample best{};
    double distance = std::numeric_limits<double>::infinity();
    bool collinear = false;
    for (std::uint32_t i = 0; i < range.count - 2; ++i) {
        const auto ids = fan_corners(range, i);
        const auto b = point(ids[1]) - origin;
        const auto c = point(ids[2]) - origin;
        if (math::length_squared(math::cross(b, c)) == 0.) {
            collinear = true;
            continue;
        }
        const auto uv = closest_on_triangle(q, {}, b, c);
        const auto d = math::length_squared(q - b * uv.x - c * uv.y);
        if (d < distance) {
            best = {ids, {1. - uv.x - uv.y, uv.x, uv.y}};
            distance = d;
        }
    }
    // A fan diagonal may span several collinear authored boundary edges. Resolve the
    // closest boundary segment in that case, so that an intermediate UV or group survives.
    if (collinear || !std::isfinite(distance)) {
        for (std::uint32_t i = 0; i < range.count; ++i) {
            const auto a = range.first + i;
            const auto b = range.first + (i + 1) % range.count;
            const auto first = point(a);
            const auto delta = point(b) - first;
            const auto to_query = query - first;
            const double t = std::clamp(math::dot(to_query, delta) / math::length_squared(delta), 0., 1.);
            const double d = math::length_squared(to_query - delta * t);
            if (d <= distance * (1. + 16. * std::numeric_limits<double>::epsilon())) {
                best = {{a, b, b}, {1. - t, t, 0.}};
                distance = d;
            }
        }
    }
    return best;
}

Result<Vector3> face_normal(const Geometry& mesh, PrimitiveRange range) {
    const auto corners = mesh.vertex_points();
    const auto& positions = mesh.positions();
    const Vector3d a(positions[corners[range.first]]);
    Vector3d area{};
    for (std::uint32_t i = 1; i < range.count - 1; ++i)
        area += math::cross(Vector3d(positions[corners[range.first + i]]) - a,
                            Vector3d(positions[corners[range.first + i + 1]]) - a);
    const auto unit = math::try_normalize(area);
    if (!unit)
        return failure("Bevel cannot resolve the normal of an output face.");
    return Vector3(*unit);
}

} // namespace

Result<void> bevel_attributes(const Geometry& source, Geometry& output, Span<const std::uint32_t> primitive_sources,
                              Span<const std::uint8_t> original_faces, Span<const std::uint32_t> point_sources) {
    const auto invalid = [&](auto p) { return p >= source.count(Layer::primitive); };
    if (primitive_sources.size() != output.count(Layer::primitive) ||
        original_faces.size() != primitive_sources.size() || point_sources.size() != output.count(Layer::point) ||
        std::ranges::any_of(primitive_sources, invalid) || std::ranges::any_of(point_sources, invalid))
        return failure("Bevel output has an invalid source primitive mapping.");

    const auto* normal = source.find("N");
    const Array<Vector3>* authored = nullptr;
    if (normal) {
        const auto* column = std::get_if<SharedArray<Vector3>>(&normal->data);
        if (!column ||
            (normal->layer != Layer::vertex && normal->layer != Layer::point && normal->layer != Layer::primitive))
            return failure("Bevel requires N to be a vertex, point, or primitive vector3 normal.");
        authored = &column->array();
    }
    const auto needs_samples = [&](Layer layer) {
        return std::ranges::any_of(source.attributes(), [&](const Attribute& attr) {
            return attr.layer == layer && attr.name.view() != "P" && attr.name.view() != "N";
        });
    };
    const bool sample_corners = needs_samples(Layer::vertex);
    Array<Sample> corner_samples;
    Array<Sample> point_samples;
    Array<Vector3> normals;
    normals.reserve(output.count(Layer::vertex));
    if (sample_corners)
        corner_samples.reserve(output.count(Layer::vertex));
    if (needs_samples(Layer::point)) {
        point_samples.reserve(point_sources.size());
        for (std::size_t p = 0; p < point_sources.size(); ++p)
            point_samples.add(closest_sample(source, source.primitive(point_sources[p]), output.positions()[p])
                                  .as_points(source.vertex_points()));
    }
    for (std::uint32_t p = 0; p < primitive_sources.size(); ++p) {
        const auto range = output.primitive(p);
        const auto donor = primitive_sources[p];
        const bool keep_normal = original_faces[p] && authored;
        const bool varying_normal = keep_normal && normal->layer != Layer::primitive;
        Vector3 constant{};
        if (!keep_normal) {
            ATELIER_TRY_ASSIGN(constant, face_normal(output, range));
        } else if (!varying_normal) {
            constant = math::try_normalize((*authored)[donor]).value_or(Vector3{});
        }
        for (std::uint32_t j = 0; j < range.count; ++j) {
            Vector3 n = constant;
            if (sample_corners || varying_normal) {
                const auto point = output.vertex_points()[range.first + j];
                auto sample = closest_sample(source, source.primitive(donor), output.positions()[point]);
                if (sample_corners)
                    corner_samples.add(sample);
                if (varying_normal) {
                    if (normal->layer == Layer::point)
                        sample = sample.as_points(source.vertex_points());
                    n = interpolate_attribute(*authored, sample.ids, sample.weights, Usage::normal);
                }
            }
            normals.add(n);
        }
    }
    for (const auto& attr : source.attributes()) {
        if (attr.name.view() == "P" || attr.name.view() == "N")
            continue;
        if (attr.layer == Layer::detail || attr.layer == Layer::instance) {
            ATELIER_TRY(output.set(attr.name, attr.layer, attr.data, attr.usage));
            continue;
        }
        auto column = std::visit(
            [&](const auto& src) -> ColumnData {
                using T = typename std::decay_t<decltype(src)>::Value;
                Array<T> values;
                values.reserve(output.count(attr.layer));
                if (attr.layer == Layer::primitive) {
                    for (const auto donor : primitive_sources)
                        values.add(src.array()[donor]);
                } else {
                    for (const auto& sample : attr.layer == Layer::point ? point_samples : corner_samples)
                        values.add(interpolate_attribute(src.array(), sample.ids, sample.weights, attr.usage));
                }
                return SharedArray<T>{std::move(values)};
            },
            attr.data);
        ATELIER_TRY(output.set(attr.name, attr.layer, std::move(column), attr.usage));
    }
    return output.set(String::literal<"N">(), Layer::vertex, SharedArray{std::move(normals)}, Usage::normal);
}

}

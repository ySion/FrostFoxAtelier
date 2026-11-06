#include "procedural/nodes.hpp"
#include "foundation/result.hpp"
#include "procedural/bvh.hpp"
#include "procedural/operations.hpp"
#include "procedural/random.hpp"
#include "detail/triangles.hpp"

#include <cmath>
#include <limits>

namespace atelier::procedural {
namespace {
// Preserve the node invocation error category without using exceptions as transport.
template <class T>
Result<T> invocation_result(Result<T> result) noexcept {
    if (!result)
        result.error().code = ErrorCode::invocation_failed;
    return result;
}
}

Result<void> MeshImport::evaluate(const Context& context, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(reference, invocation_result(context.mesh(mesh)));
    output = import_mesh(*reference->mesh);
    return {};
}

Result<void> Grid::evaluate(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(grid(size, rows, columns)));
    output = std::move(*result);
    return {};
}

Result<void> Line::evaluate(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(line(start, end, count)));
    output = std::move(*result);
    return {};
}

Result<void> Box::evaluate(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(box(size)));
    output = std::move(*result);
    return {};
}

Result<void> Sphere::evaluate(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(sphere(radius, segments, latitudes)));
    output = std::move(*result);
    return {};
}

Result<void> Tube::evaluate(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(tube(radius, height, segments)));
    output = std::move(*result);
    return {};
}

Result<void> Polyline::evaluate(const Geometry& points, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(points.connect_points()));
    output = std::move(*result);
    return {};
}

Result<void> GroupBox::evaluate(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(group_box(output, group, minimum, maximum));
}

Result<void> Scatter::evaluate(const Context& context, const Geometry& surface, Geometry& points) const noexcept {
    ATELIER_TRY_BIND(
        result, invocation_result(scatter(
                    surface, {.density = density, .seed = hash(context.seed(), seed), .min_distance = min_distance})));
    points = std::move(*result);
    return {};
}

Result<void> VolumeScatter::evaluate(const Context& context, const Geometry& surface, Geometry& points) const noexcept {
    auto result = invocation_result(volume_scatter(
        surface, {.density = density, .seed = hash(context.seed(), seed), .origin = origin, .cell_size = cell_size}));
    if (!result)
        return std::unexpected(std::move(result).error());
    points = std::move(*result);
    return {};
}

Result<void> Transform::evaluate(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(transform(output, translate, rotate, scale));
}

Result<void> Promote::evaluate(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(promote(output, name, to, reduce));
}

Result<void> Remove::evaluate(const Geometry& input, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(input.remove_flagged(layer, flag)));
    output = std::move(*result);
    return {};
}

Result<void> Merge::evaluate(const Geometries& inputs, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(Geometry::merge({inputs.data(), inputs.size()})));
    output = std::move(*result);
    return {};
}

Result<void> Ray::evaluate(const Geometry& points, const Geometry& surface, Geometry& output) const noexcept {
    if (!std::isfinite(max_distance) || max_distance < 0)
        return std::unexpected(
            Error{ErrorCode::invocation_failed, "Ray max distance must be finite and non-negative."});
    const auto unit = math::try_normalize(direction);
    if (mode == RayMode::project && !unit)
        return std::unexpected(Error{ErrorCode::invocation_failed, "Ray direction must be finite and nonzero."});
    if (surface.count(Layer::primitive) > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        return std::unexpected(Error{ErrorCode::invocation_failed, "Ray surfaces have fewer than 2^31 primitives."});
    for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p)
        if (surface.primitive(p).count - 2 > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
            return std::unexpected(
                Error{ErrorCode::invocation_failed, "Ray polygons have fewer than 2^31 fan triangles."});
    ATELIER_TRY_BIND(bvh, invocation_result(Bvh::build(surface)));
    output = points;
    const auto count = output.count(Layer::point);
    // Finish adding columns before borrowing them: growing attributes moves their handles.
    ATELIER_TRY(invocation_result(output.add<std::int32_t>("hitprim", Layer::point)));
    ATELIER_TRY(invocation_result(output.add<std::int32_t>("hittri", Layer::point)));
    ATELIER_TRY(invocation_result(output.add<math::Vector2>("hitprimuv", Layer::point)));
    ATELIER_TRY(invocation_result(output.add<float>("hitdist", Layer::point)));
    if (normal)
        ATELIER_TRY(invocation_result(output.add<math::Vector3>("N", Layer::point)));
    ATELIER_TRY_BIND(positions, invocation_result(output.write<math::Vector3>("P", Layer::point)));
    ATELIER_TRY_BIND(primitives, invocation_result(output.write<std::int32_t>("hitprim", Layer::point)));
    ATELIER_TRY_BIND(subtriangles, invocation_result(output.write<std::int32_t>("hittri", Layer::point)));
    ATELIER_TRY_BIND(uvs, invocation_result(output.write<math::Vector2>("hitprimuv", Layer::point)));
    ATELIER_TRY_BIND(distances, invocation_result(output.write<float>("hitdist", Layer::point)));
    Array<math::Vector3>* normals{};
    if (normal) {
        ATELIER_TRY_BIND(writable, invocation_result(output.write<math::Vector3>("N", Layer::point)));
        normals = *writable;
    }
    Array<bool> missed;
    missed.resize(remove_misses ? count : 0);
    const auto& corners = surface.positions();
    const auto vertices = surface.vertex_points();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto hit = mode == RayMode::project ? bvh->ray((**positions)[i], *unit, max_distance)
                                                  : bvh->closest((**positions)[i], max_distance);
        if (!hit) {
            (**primitives)[i] = -1;
            (**subtriangles)[i] = -1;
            (**uvs)[i] = {};
            (**distances)[i] = -1;
            if (remove_misses)
                missed[i] = true;
            continue;
        }
        (**positions)[i] = hit->position;
        (**primitives)[i] = static_cast<std::int32_t>(hit->primitive);
        (**subtriangles)[i] = static_cast<std::int32_t>(hit->triangle);
        (**uvs)[i] = hit->uv;
        (**distances)[i] = hit->distance;
        if (normals) {
            const auto indices = detail::fan_corners(surface.primitive(hit->primitive), hit->triangle);
            const auto a = corners[vertices[indices[0]]];
            const auto b = corners[vertices[indices[1]]];
            const auto c = corners[vertices[indices[2]]];
            if (const auto n = math::try_normalize(math::cross(b - a, c - a)))
                (*normals)[i] = *n;
        }
    }
    if (remove_misses) {
        ATELIER_TRY_BIND(result, invocation_result(output.remove_flagged(
                                     Layer::point, std::span<const bool>{missed.data(), missed.size()})));
        output = std::move(*result);
    }
    return {};
}

Result<void> CopyToPoints::evaluate(const Context& context, const Geometry& points, Geometry& output) const noexcept {
    std::vector<Reference> references;
    references.reserve(meshes.size());
    for (const auto& key : meshes) {
        ATELIER_TRY_BIND(reference, invocation_result(context.mesh(key)));
        references.push_back(std::move(*reference));
    }
    ATELIER_TRY_BIND(result, invocation_result(copy_to_points(points, references, variant)));
    output = std::move(*result);
    return {};
}

Result<void> Realize::evaluate(const Geometries& sources, const Geometry& points, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(realize({sources.data(), sources.size()}, points, group, variant,
                                                       {target_attributes.data(), target_attributes.size()})));
    output = std::move(*result);
    return {};
}

}

#include "procedural/nodes.hpp"
#include "foundation/result.hpp"
#include "procedural/operations.hpp"
#include "procedural/random.hpp"

#include <utility>
#include <vector>

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

Result<void> MeshImport::process(const Context& context, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(reference, invocation_result(context.mesh(mesh)));
    output = import_mesh(*reference->mesh);
    return {};
}

Result<void> Grid::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(grid(size, rows, columns)));
    output = std::move(*result);
    return {};
}

Result<void> Line::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(line(start, end, count)));
    output = std::move(*result);
    return {};
}

Result<void> Box::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(box(size)));
    output = std::move(*result);
    return {};
}

Result<void> Sphere::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(sphere(radius, segments, latitudes)));
    output = std::move(*result);
    return {};
}

Result<void> Tube::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(tube(radius, height, segments)));
    output = std::move(*result);
    return {};
}

Result<void> Polyline::process(const Geometry& points, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(points.connect_points()));
    output = std::move(*result);
    return {};
}

Result<void> GroupBox::process(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(group_box(output, group, minimum, maximum));
}

Result<void> Scatter::process(const Context& context, const Geometry& surface, Geometry& points) const noexcept {
    ATELIER_TRY_BIND(
        result, invocation_result(scatter(
                    surface, {.density = density, .seed = hash(context.seed(), seed), .min_distance = min_distance})));
    points = std::move(*result);
    return {};
}

Result<void> VolumeScatter::process(const Context& context, const Geometry& surface, Geometry& points) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(volume_scatter(surface, {.density = density,
                                                                        .seed = hash(context.seed(), seed),
                                                                        .origin = origin,
                                                                        .cell_size = cell_size,
                                                                        .cache_bvh = true})));
    points = std::move(*result);
    return {};
}

Result<void> Transform::process(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(transform(output, translate, rotate, scale));
}

Result<void> Promote::process(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(promote(output, name, to, reduce));
}

Result<void> Remove::process(const Geometry& input, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(input.remove_flagged(layer, flag)));
    output = std::move(*result);
    return {};
}

Result<void> Merge::process(const Geometries& inputs, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(Geometry::merge({inputs.data(), inputs.size()})));
    output = std::move(*result);
    return {};
}

Result<void> Ray::process(const Geometry& points, const Geometry& surface, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(ray(points, surface,
                                                   {.mode = mode,
                                                    .direction = direction,
                                                    .max_distance = max_distance,
                                                    .normal = normal,
                                                    .remove_misses = remove_misses})));
    output = std::move(*result);
    return {};
}

Result<void> CopyToPoints::process(const Context& context, const Geometry& points, Geometry& output) const noexcept {
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

Result<void> Realize::process(const Geometries& sources, const Geometry& points, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(realize({sources.data(), sources.size()}, points, group, variant,
                                                       {target_attributes.data(), target_attributes.size()})));
    output = std::move(*result);
    return {};
}

}

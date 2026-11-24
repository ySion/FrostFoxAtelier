#include "procedural/nodes.hpp"
#include "detail/native_cook.hpp"
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
    ATELIER_TRY_BIND(result, invocation_result(grid({.primitive_type = primitive_type,
                                                     .orientation = orientation,
                                                     .size = size,
                                                     .center = center,
                                                     .rows = rows,
                                                     .columns = columns})));
    output = std::move(*result);
    return {};
}

Result<void> Line::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(line(start, end, count)));
    output = std::move(*result);
    return {};
}

Result<void> Box::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(box_primitive({.size = size,
                                                              .center = center,
                                                              .base_on_ground = base_on_ground,
                                                              .divisions_x = divisions_x,
                                                              .divisions_y = divisions_y,
                                                              .divisions_z = divisions_z})));
    output = std::move(*result);
    return {};
}

Result<void> Sphere::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(sphere(radius, segments, latitudes)));
    output = std::move(*result);
    return {};
}

Result<void> Tube::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(tube({.radius_top = radius_top,
                                                     .radius_bottom = radius_bottom,
                                                     .inner_radius = inner_radius,
                                                     .height = height,
                                                     .center = center,
                                                     .base_on_ground = base_on_ground,
                                                     .segments = segments,
                                                     .rows = rows,
                                                     .arc_degrees = arc_degrees,
                                                     .end_caps = end_caps})));
    output = std::move(*result);
    return {};
}

Result<void> Wedge::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(wedge({.size = size,
                                                      .peak_ratio = peak_ratio,
                                                      .center = center,
                                                      .base_on_ground = base_on_ground,
                                                      .divisions_z = divisions_z})));
    output = std::move(*result);
    return {};
}

Result<void> Circle::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(circle({.primitive_type = primitive_type,
                                                       .orientation = orientation,
                                                       .radius = radius,
                                                       .center = center,
                                                       .segments = segments,
                                                       .start_degrees = start_degrees,
                                                       .arc_degrees = arc_degrees})));
    output = std::move(*result);
    return {};
}

Result<void> Torus::process(Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(torus({.orientation = orientation,
                                                      .major_radius = major_radius,
                                                      .minor_radius = minor_radius,
                                                      .center = center,
                                                      .base_on_ground = base_on_ground,
                                                      .major_segments = major_segments,
                                                      .minor_segments = minor_segments,
                                                      .arc_degrees = arc_degrees,
                                                      .end_caps = end_caps})));
    output = std::move(*result);
    return {};
}

Result<void> Polyline::process(const Geometry& points, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(points.connect_points()));
    output = std::move(*result);
    return {};
}

Result<void> Resample::process(const Geometry& curves, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(resample(curves, {mode, spacing, segments, preserve_corners})));
    output = std::move(*result);
    return {};
}

Result<void> Group::process(const Geometry& input, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(build_group(input, {group, criterion, direction, max_angle, source_group,
                                                                   operation, base_group})));
    output = std::move(*result);
    return {};
}

Result<void> GroupBox::process(const Geometry& input, Geometry& output) const noexcept {
    Geometry result = input;
    ATELIER_TRY(invocation_result(group_box(result, group, minimum, maximum)));
    output = std::move(result);
    return {};
}

Result<void> Connectivity::process(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return invocation_result(connectivity(output, attribute));
}

Result<void> Scatter::process(const Context& context, const Geometry& surface, Geometry& points) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(scatter(surface, {.density = density,
                                                                 .seed = hash(context.seed(), seed),
                                                                 .min_distance = min_distance,
                                                                 .group = group})));
    points = std::move(*result);
    return {};
}

Result<void> Boolean::process(const Geometry& a, const Geometry& b, Geometry& output) const noexcept {
    auto stats = detail::NativeCook::current ? std::make_shared<BooleanStats>() : nullptr;
    auto cooked = solid_boolean(a, b, operation, stats.get());
    if (stats)
        detail::NativeCook::current->boolean_stats = std::move(stats);
    ATELIER_TRY_BIND(result, invocation_result(std::move(cooked)));
    output = std::move(*result);
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
    return invocation_result(
        transform(output, {.translate = translate, .rotate_degrees = rotate, .scale = scale, .pivot = pivot}));
}

Result<void> MatchSize::process(const Geometry& input, const Geometry& reference, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(match_size(input,
                                                          {.justify_x = justify_x,
                                                           .justify_y = justify_y,
                                                           .justify_z = justify_z,
                                                           .target_position = target_position,
                                                           .scale_to_fit = scale_to_fit,
                                                           .uniform_scale = uniform_scale,
                                                           .target_size = target_size},
                                                          &reference)));
    output = std::move(*result);
    return {};
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

Result<void> Extrude::process(const Geometry& surface, Geometry& output) const noexcept {
    ATELIER_TRY_BIND(result, invocation_result(extrude(surface, {.group = group.view(),
                                                                 .distance = distance,
                                                                 .inset = inset,
                                                                 .distance_attribute = distance_attribute.view(),
                                                                 .inset_attribute = inset_attribute.view(),
                                                                 .individual = individual,
                                                                 .output_back = output_back,
                                                                 .front_group = front_group.view(),
                                                                 .side_group = side_group.view()})));
    output = std::move(*result);
    return {};
}

Result<void> Sweep::process(const Geometry& backbone, const Geometry& cross_section, Geometry& output) const noexcept {
    const Geometry* profile_geo = profile == SweepProfile::custom ? &cross_section : nullptr;
    ATELIER_TRY_BIND(result, invocation_result(sweep(backbone,
                                                     {.profile = profile,
                                                      .width = width,
                                                      .height = height,
                                                      .radius = radius,
                                                      .segments = segments,
                                                      .up = up,
                                                      .end_caps = end_caps,
                                                      .scale_attribute = scale_attribute.view()},
                                                     profile_geo)));
    output = std::move(*result);
    return {};
}

Result<void> Null::process(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return {};
}

Result<void> GraphInput::process(Geometry& output) const noexcept {
    const auto build = [&]() -> Result<Geometry> {
        switch (preview_fallback) {
        case PreviewFallback::empty:
            return Geometry{};
        case PreviewFallback::grid:
            return grid(
                GridSettings{.primitive_type = GridPrimitive::quads, .size = {10.f, 10.f}, .rows = 10, .columns = 10});
        case PreviewFallback::box:
            return box({1.f, 1.f, 1.f});
        case PreviewFallback::sphere:
            return sphere(1.f, 24, 12);
        case PreviewFallback::line:
            return line({0.f, 0.f, 0.f}, {0.f, 2.f, 0.f}, 8);
        }
        return failure("Invalid preview_fallback value.");
    };
    ATELIER_TRY_BIND(built, invocation_result(build()));
    output = std::move(*built);
    return {};
}

Result<void> GraphOutput::process(const Geometry& input, Geometry& output) const noexcept {
    output = input;
    return {};
}

Result<void> AssetNode::process(const Context&, Geometry&) const noexcept {
    return std::unexpected(Error{ErrorCode::invocation_failed, "AssetNode has no bound procedural graph asset."});
}

}

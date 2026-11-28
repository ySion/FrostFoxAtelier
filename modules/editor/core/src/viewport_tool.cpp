#include "editor/viewport_tool.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>

namespace atelier::editor {
namespace {
using V2 = math::Vector2;

using V3 = math::Vector3;

using Handle = ViewportTool::Handle;

using Shape = renderer::OverlayShape;

constexpr float pi = std::numbers::pi_v<float>;

constexpr std::array<V3, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

// Houdini handle palette, sampled from the Move/Rotate/Scale handles: #ff1414 / #14ff14 / #1414ff, #ffd900 when picked.
// ui::Color is linear; reference colours are sampled as sRGB bytes.
ui::Color srgb(int r, int g, int b, float a = 1) {
    const auto f = [](int v) {
        const double c = v / 255.;
        return float(c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4));
    };
    return {f(r), f(g), f(b), a};
}

const std::array<ui::Color, 3> colors{srgb(0xff, 0x14, 0x14), srgb(0x14, 0xff, 0x14), srgb(0x14, 0x14, 0xff)};

const ui::Color picked = srgb(0xff, 0xd9, 0x00);

auto invalid(Utf8Text text) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(text)});
}

float segment_distance(V2 p, V2 a, V2 b) {
    const auto d = b - a;
    const auto square = math::length_squared(d);
    return math::distance(p, a + d * (square > 0 ? std::clamp(math::dot(p - a, d) / square, 0.f, 1.f) : 0.f));
}

math::Quaternion frame_rotation(V3 x, V3 y, V3 z) {
    const float trace = x.x + y.y + z.z;
    math::Quaternion q;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s * .25f};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        q = {s * .25f, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        q = {(y.x + x.y) / s, s * .25f, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, s * .25f, (x.y - y.x) / s};
    }
    return math::try_normalize(q).value_or(math::Quaternion{});
}

Optional<V3> on_plane(const ViewportView& view, V2 point, V3 origin, V3 normal) {
    const auto ray = view.ray(point.x, point.y);
    const auto denominator = math::dot(ray.direction, normal);
    if (std::abs(denominator) < 1e-5f)
        return {};
    const auto t = math::dot(origin - ray.origin, normal) / denominator;
    if (!std::isfinite(t) || t < 0)
        return {};
    return ray.origin + ray.direction * t;
}

// Drawing and hit testing visit exactly the same physical geometry. Proportions
// follow Houdini 22's object handles at 1x: 1 px axis lines reaching ~84 px,
// solid 15 px tip markers, outlined plane diamonds at 0.3 of the axis, and a
// rotate ball whose rings fade on the far hemisphere inside a purple view ring.
math::Quaternion transform_basis(math::Quaternion frame, Orientation orientation) noexcept {
    return orientation == Orientation::local ? frame : math::Quaternion{};
}

template <class Emit>
void handles(const ViewportView& view, V3 pivot, math::Quaternion orientation, Orientation requested,
             TransformTool tool, Emit emit) {
    const auto basis = transform_basis(orientation, requested);
    const std::array<V3, 3> frame{math::rotate(basis, axes[0]), math::rotate(basis, axes[1]),
                                  math::rotate(basis, axes[2])};
    const auto center = view.project(pivot);
    if (!center || tool == TransformTool::select)
        return;
    const float scale = view.dpi_scale;
    const float unit = view.pixel_size(pivot) * scale;
    const auto line = [&](Handle h, V2 a, V2 b, ui::Color color, float width = 1.f) {
        emit(h, Shape{Shape::Kind::segment, {a.x, a.y, b.x, b.y}, width * scale, color});
    };
    const auto box = [&](Handle h, V2 at, float size, ui::Color color, ui::Color edge = {0, 0, 0, .55f}) {
        emit(h, Shape{Shape::Kind::rectangle,
                      {at.x - size * scale * .5f, at.y - size * scale * .5f, size * scale, size * scale},
                      scale,
                      color,
                      edge});
    };
    const auto shade = [](ui::Color c, float k) { return ui::Color{c.r * k, c.g * k, c.b * k, c.a}; };
    const auto ring = [&](Handle h, V3 u, V3 v, float radius, ui::Color color, float width) {
        for (int step = 0; step < 96; ++step) {
            const auto at = [&](int n) {
                const auto a = 2 * pi * float(n) / 96;
                return pivot + (u * std::cos(a) + v * std::sin(a)) * (unit * radius);
            };
            const auto a = at(step);
            const auto b = at(step + 1);
            const auto pa = view.project(a);
            const auto pb = view.project(b);
            if (!pa || !pb)
                continue;
            // Front arcs are opaque; arcs behind the ball's silhouette plane read as faded.
            const bool front = math::dot((a + b) * .5f - pivot, view.forward) <= 0;
            line(h, *pa, *pb, front ? color : ui::Color{color.r, color.g, color.b, .42f}, front ? width : width * .8f);
        }
    };
    if (tool == TransformTool::rotate || tool == TransformTool::transform) {
        const float radius = tool == TransformTool::rotate ? 78.f : 62.f;
        for (int axis = 0; axis < 3; ++axis)
            ring(static_cast<Handle>(int(Handle::rotate_x) + axis), frame[(axis + 1) % 3], frame[(axis + 2) % 3],
                 radius, colors[axis], 1.5f);
        if (tool == TransformTool::rotate)
            for (int step = 0; step < 96; ++step) {
                const auto at = [&](int n) {
                    const auto a = 2 * pi * float(n) / 96;
                    return *center + V2{std::cos(a), std::sin(a)} * (radius * 1.18f * scale);
                };
                line(Handle::rotate_view, at(step), at(step + 1), srgb(0x80, 0x33, 0x80, .95f), 1.5f);
            }
    }
    const float reach = tool == TransformTool::transform ? 92.f : 84.f;
    for (int axis = 0; axis < 3; ++axis) {
        const auto color = colors[axis];
        if (tool == TransformTool::transform || tool == TransformTool::move) {
            const auto tip = view.project(pivot + frame[axis] * (reach * unit));
            if (!tip)
                continue;
            const auto d = *tip - *center;
            const auto size = math::length(d);
            if (size < 8 * scale)
                continue;
            const auto direction = d / size;
            const auto side = V2{-direction.y, direction.x};
            const auto h = static_cast<Handle>(int(Handle::move_x) + axis);
            line(h, *center, *tip, shade(color, .62f), 1.25f);
            // Solid cone: filled by a fan of strokes from the tip to the base.
            const auto base = *tip - direction * (15 * scale);
            for (int k = -4; k <= 4; ++k)
                line(h, *tip, base + side * (float(k) * 1.f * scale), color, 1.5f);
            line(h, base - side * (5 * scale), base + side * (5 * scale), shade(color, .45f), 1.25f);
            // Plane handle: a diamond centred at 0.3 of the two other axes.
            const auto u = frame[(axis + 1) % 3];
            const auto v = frame[(axis + 2) % 3];
            const float at = reach * .3f;
            const float half = reach * .045f;
            const std::array corners{
                pivot + (u * at + v * (at - half)) * unit, pivot + (u * (at + half) + v * at) * unit,
                pivot + (u * at + v * (at + half)) * unit, pivot + (u * (at - half) + v * at) * unit};
            std::array<Optional<V2>, 4> p{};
            for (int i = 0; i < 4; ++i)
                p[i] = view.project(corners[i]);
            const auto plane = static_cast<Handle>(int(Handle::plane_yz) + axis);
            if (p[0] && p[1] && p[2] && p[3])
                for (int i = 0; i < 4; ++i)
                    line(plane, *p[i], *p[(i + 1) % 4],
                         {color.r * .62f + .24f, color.g * .62f + .24f, color.b * .62f + .24f, 1}, 1.25f);
        }
        if (tool == TransformTool::transform || tool == TransformTool::scale) {
            const float length = tool == TransformTool::transform ? reach * .62f : reach;
            const auto tip = view.project(pivot + math::rotate(orientation, axes[axis]) * (length * unit));
            if (tip) {
                const auto h = static_cast<Handle>(int(Handle::scale_x) + axis);
                if (tool == TransformTool::scale)
                    line(h, *center, *tip, shade(color, .62f), 1.25f);
                box(h, *tip, tool == TransformTool::scale ? 13.f : 9.f, color);
            }
        }
    }
    if (tool == TransformTool::scale)
        box(Handle::scale_uniform, *center, 11, srgb(0xae, 0x8b, 0xd2));
    else if (tool != TransformTool::rotate)
        box(Handle::move_view, *center, 7, srgb(0xee, 0xee, 0xe6));
}

struct TriangleHit {
    float distance;

    float u;

    float v;
};

Optional<TriangleHit> triangle(const ViewportView::Ray& ray, V3 a, V3 b, V3 c) {
    const auto edge1 = b - a;
    const auto edge2 = c - a;
    const auto h = math::cross(ray.direction, edge2);
    const auto det = math::dot(edge1, h);
    if (std::abs(det) < 1e-8f)
        return {};
    const auto offset = ray.origin - a;
    const float u = math::dot(offset, h) / det;
    if (u < 0 || u > 1)
        return {};
    const auto q = math::cross(offset, edge1);
    const float v = math::dot(ray.direction, q) / det;
    if (v < 0 || u + v > 1)
        return {};
    const float t = math::dot(edge2, q) / det;
    return t >= 0 ? Optional<TriangleHit>(TriangleHit{t, u, v}) : Optional<TriangleHit>{};
}

bool ray_hits_bounds(const ViewportView::Ray& ray, const math::Bounds& bounds, float max_t) noexcept {
    if (math::is_empty(bounds) || !math::is_finite(bounds))
        return false;
    float t_min = 0.f;
    float t_max = max_t;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float o = ray.origin[axis];
        const float d = ray.direction[axis];
        const float lo = bounds.minimum[axis];
        const float hi = bounds.maximum[axis];
        if (std::abs(d) < 1e-12f) {
            if (o < lo || o > hi)
                return false;
            continue;
        }
        const float inv = 1.f / d;
        float t0 = (lo - o) * inv;
        float t1 = (hi - o) * inv;
        if (t0 > t1)
            std::swap(t0, t1);
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_min > t_max)
            return false;
    }
    return true;
}
}

math::Vector3 euler_degrees(math::Quaternion value) noexcept {
    // Convert in double and round only at the float storage boundary.
    const auto q = math::try_normalize(math::Quaterniond(value)).value_or(math::Quaterniond{});
    constexpr double degrees = 180 / std::numbers::pi;
    const double y = std::asin(std::clamp(2 * (q.w * q.y - q.z * q.x), -1., 1.));
    if (std::abs(std::cos(y)) < 1e-5)
        return {float(2 * std::atan2(q.x, q.w) * degrees), float(y * degrees), 0};
    return {float(std::atan2(2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)) * degrees),
            float(y * degrees),
            float(std::atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z)) * degrees)};
}

math::Quaternion from_euler_degrees(math::Vector3 value) noexcept {
    constexpr double radians = std::numbers::pi / 180;
    const auto q = math::from_axis_angle(math::Vector3d(axes[2]), value.z * radians) *
                   math::from_axis_angle(math::Vector3d(axes[1]), value.y * radians) *
                   math::from_axis_angle(math::Vector3d(axes[0]), value.x * radians);
    return math::Quaternion(q);
}

ViewportView ViewportView::make(const Camera& camera, ui::Rect bounds, float dpi) {
    ViewportView result;
    result.bounds = bounds;
    result.dpi_scale = dpi;
    result.eye = camera.eye();
    result.orthographic = camera.view() != CameraView::perspective;
    result.construction_normal = camera.construction_normal();
    const auto matrices = camera.matrices(std::max(bounds.width, 1.f) / std::max(bounds.height, 1.f));
    result.view = matrices.view;
    result.projection = matrices.projection;
    result.far_depth = matrices.far_depth;
    result.inverse =
        math::try_inverse(math::Matrix4d(matrices.view * matrices.projection)).value_or(math::Matrix4d::identity());
    result.forward = math::try_normalize(camera.target() - camera.eye()).value_or(V3{0, 0, 1});
    result.right = math::try_normalize(math::cross(camera.up_hint(), result.forward)).value_or(V3{1, 0, 0});
    result.up = math::cross(result.forward, result.right);
    return result;
}

bool ViewportView::contains(float x, float y) const noexcept {
    return x >= bounds.x && y >= bounds.y && x < bounds.x + bounds.width && y < bounds.y + bounds.height;
}

Optional<V2> ViewportView::project(V3 p) const noexcept {
    const auto clip = math::Vector4{p.x, p.y, p.z, 1} * view * projection;
    if (!math::is_finite(clip) || clip.w <= 0 || clip.z < far_depth * clip.w || clip.z > clip.w)
        return {};
    return V2{bounds.x + (clip.x / clip.w + 1) * bounds.width * .5f,
              bounds.y + (1 - clip.y / clip.w) * bounds.height * .5f};
}

ViewportView::Ray ViewportView::ray(float x, float y) const noexcept {
    const double nx = 2 * (x - bounds.x) / std::max(bounds.width, 1.f) - 1;
    const double ny = 1 - 2 * (y - bounds.y) / std::max(bounds.height, 1.f);
    const auto p = math::Vector4d{nx, ny, 1, 1} * inverse;
    const auto far = math::Vector4d{nx, ny, .5, 1} * inverse;
    const V3 near{float(p.x / p.w), float(p.y / p.w), float(p.z / p.w)};
    return {near, math::try_normalize(
                      V3{float(far.x / far.w) - near.x, float(far.y / far.w) - near.y, float(far.z / far.w) - near.z})
                      .value_or(forward)};
}

float ViewportView::pixel_size(V3 at) const noexcept {
    return (orthographic ? 1.f : std::max(math::dot(at - eye, forward), .05f)) * 2 /
           (std::max(bounds.height, 1.f) * projection[5]);
}

namespace {
Result<world::Trs> viewport_frame(const Session& session, ObjectId entity, const math::Matrix4& matrix,
                                  Optional<world::Trs> authored = {}) {
    auto& runtime = session.world().runtime();
    ATELIER_TRY_BIND(value, authored ? Result<world::Trs>{*authored} : world::read_transform(runtime, entity));
    ATELIER_TRY_BIND(parent, world::spatial_parent(runtime, entity));
    while (*parent != ObjectId{}) {
        const auto spatial = world::has_transform(runtime, *parent);
        if (!spatial)
            return std::unexpected(spatial.error());
        if (*spatial) {
            ATELIER_TRY_BIND(pose, world::read_transform(runtime, *parent));
            value->rotation = pose->rotation * value->rotation;
        }
        parent = world::spatial_parent(runtime, *parent);
        if (!parent)
            return std::unexpected(parent.error());
    }
    value->position = {matrix[12], matrix[13], matrix[14]};
    for (std::size_t axis = 0; axis < 3; ++axis)
        value->scale[axis] = math::length(V3{matrix[axis * 4], matrix[axis * 4 + 1], matrix[axis * 4 + 2]});
    return world::canonical_transform(*value);
}
}

Optional<ViewportTool::Targets> ViewportTool::targets(const Session& session, Span<const ObjectId> entities,
                                                      ObjectId active, PivotMode mode, V3 cursor) {
    Targets result;
    for (const auto entity : entities) {
        const auto spatial = world::has_transform(session.world().runtime(), entity);
        if (!spatial || !*spatial)
            continue;
        auto parent = world::spatial_parent(session.world().runtime(), entity);
        bool inherited{};
        while (parent && *parent != ObjectId{}) {
            if (std::ranges::find(entities, *parent) != entities.end()) {
                inherited = true;
                break;
            }
            parent = world::spatial_parent(session.world().runtime(), *parent);
        }
        if (inherited && entity != active)
            continue;
        const auto matrix = world::world_matrix(session.world().runtime(), entity);
        if (!matrix)
            continue;
        const auto value = viewport_frame(session, entity, *matrix);
        if (!value)
            continue;
        // The active Entity is last in selection order, even when its selected
        // spatial ancestor owns the write. Keep its frame independent of roots.
        result.active_frame = *value;
        if (inherited)
            continue;
        result.entities.add(entity);
        result.transforms.add(*value);
        result.pivot += value->position;
    }
    if (result.entities.empty())
        return {};
    result.pivot = mode == PivotMode::cursor ? cursor : result.pivot / float(result.entities.size());
    return result;
}

Result<void> ViewportTool::preview_world(Session& session, std::uint64_t token, Span<const ObjectId> entities,
                                         Span<const world::Trs> values, bool local_scale) {
    Array<world::Trs> local;
    local.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        ATELIER_TRY_BIND(parent, world::spatial_parent(session.world().runtime(), entities[i]));
        auto matrix = math::Matrix4::identity();
        if (*parent != ObjectId{}) {
            ATELIER_TRY_BIND(read, world::world_matrix(session.world().runtime(), *parent));
            matrix = *read;
        }
        const auto original = session.original_transform(token, entities[i]);
        if (!original)
            return std::unexpected(original.error());
        // A zero scale axis is valid but leaves the Entity's own frame singular. The gesture works on
        // a unit axis in its place, leaves that axis unchanged, and the result keeps it zero.
        auto regular = *original;
        auto value = values[i];
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (regular.scale[axis] == 0)
                regular.scale[axis] = 1;
        const auto original_world = world::multiply(world::transform_matrix(regular), matrix);
        if (!original_world)
            return std::unexpected(original_world.error());
        const auto frame = viewport_frame(session, entities[i], *original_world, regular);
        if (!frame)
            return std::unexpected(frame.error());
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (original->scale[axis] == 0)
                value.scale[axis] = frame->scale[axis];
        if (value == *frame) {
            local.add(*original);
            continue;
        }
        if (local_scale) {
            auto desired = *original_world;
            desired[12] = value.position.x;
            desired[13] = value.position.y;
            desired[14] = value.position.z;
            ATELIER_TRY_BIND(converted, world::relative_transform(desired, matrix, original->scale));
            converted->scale = math::cwise_div(math::cwise_mul(original->scale, value.scale), frame->scale);
            local.add(*converted);
            continue;
        }
        const auto inverse = math::try_inverse(world::transform_matrix(*frame));
        if (!inverse)
            return invalid("The viewport frame is singular.");
        // Keep the original affine frame, including inherited shear. The proxy
        // TRS only expresses the gesture's delta; it never replaces object space.
        ATELIER_TRY_BIND(delta, world::multiply(*inverse, world::transform_matrix(value)));
        ATELIER_TRY_BIND(desired, world::multiply(*original_world, *delta));
        // A mirrored Entity keeps its own negative axes instead of the canonical X reflection.
        ATELIER_TRY_BIND(converted, world::relative_transform(*desired, matrix, original->scale));
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (original->scale[axis] == 0)
                converted->scale[axis] = 0;
        local.add(*converted);
    }
    return session.preview_transform(token, local);
}

ViewportTool::Handle ViewportTool::hit(const ViewportView& view, const Targets& selected, TransformTool tool, V2 p,
                                       Orientation orientation) const {
    Handle best = Handle::none;
    float closest = 7 * view.dpi_scale;
    handles(view, selected.pivot, selected.active_frame.rotation, orientation, tool,
            [&](Handle handle, const Shape& shape) {
                float d;
                if (shape.kind == Shape::Kind::segment)
                    d = segment_distance(p, {shape.points[0], shape.points[1]}, {shape.points[2], shape.points[3]});
                else
                    d = math::distance(
                            p, V2{shape.points[0] + shape.points[2] * .5f, shape.points[1] + shape.points[3] * .5f}) -
                        shape.points[2] * .5f;
                if (d <= closest) {
                    closest = d;
                    best = handle;
                }
            });
    return best;
}

Optional<ObjectId> ViewportTool::pick(const Session& session, const ViewportView& view, float x, float y) const {
    if (!view.scene || view.scene->scene_identity != session.presentation().identity())
        return {};
    const auto found = trace(view, x, y, true);
    return found ? Optional<ObjectId>(found->entity) : Optional<ObjectId>{};
}

Optional<ViewportTool::Hit> ViewportTool::trace(const ViewportView& view, float x, float y, bool locators) {
    if (!view.scene)
        return {};
    const auto ray = view.ray(x, y);
    float nearest = std::numeric_limits<float>::max();
    Optional<Hit> found;
    if (view.gpu_pick_pixel) {
        if (x >= 0.f && y >= 0.f) {
            const auto gpu_hit = view.gpu_pick_pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            if (gpu_hit && gpu_hit->instance_index < view.scene->meshes.size()) {
                const auto& item = view.scene->meshes[gpu_hit->instance_index];
                const auto& m = item.world;
                const auto normal_transform =
                    math::normal_transform({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]});
                std::array<V3, 3> tri_pos{};
                std::array<V3, 3> tri_nrm{};
                const bool has_tri = item.geometry->cooked_triangle(gpu_hit->global_triangle_id, tri_pos, tri_nrm);
                V3 facing = -ray.direction;
                if (normal_transform && has_tri) {
                    const math::Vector3d p0{tri_pos[0].x, tri_pos[0].y, tri_pos[0].z};
                    const math::Vector3d p1{tri_pos[1].x, tri_pos[1].y, tri_pos[1].z};
                    const math::Vector3d p2{tri_pos[2].x, tri_pos[2].y, tri_pos[2].z};
                    auto face = math::cross(p1 - p0, p2 - p0);
                    math::Vector3d authored{};
                    for (std::size_t n = 0; n < 3u; ++n)
                        authored += math::Vector3d{tri_nrm[n].x, tri_nrm[n].y, tri_nrm[n].z};
                    if (math::dot(face, authored) < 0)
                        face = -face;
                    face = math::try_normalize(face).value_or(math::Vector3d{});
                    const auto& a = normal_transform->matrix;
                    const V3 fn{float(face.x) * a[0] + float(face.y) * a[3] + float(face.z) * a[6],
                                float(face.x) * a[1] + float(face.y) * a[4] + float(face.z) * a[7],
                                float(face.x) * a[2] + float(face.y) * a[5] + float(face.z) * a[8]};
                    facing = math::try_normalize(fn).value_or(-ray.direction);
                    if (math::dot(facing, ray.direction) > 0)
                        facing = -facing;
                }
                Optional<V3> hit_point;
                if (has_tri) {
                    const V3 wp0 = math::transform_point(tri_pos[0], item.world);
                    const V3 wp1 = math::transform_point(tri_pos[1], item.world);
                    const V3 wp2 = math::transform_point(tri_pos[2], item.world);
                    if (const auto inverse = math::try_inverse(item.world)) {
                        const ViewportView::Ray local{math::transform_point(ray.origin, *inverse),
                                                      math::transform_direction(ray.direction, *inverse)};
                        if (const auto t = triangle(local, tri_pos[0], tri_pos[1], tri_pos[2])) {
                            hit_point = wp0 + (wp1 - wp0) * t->u + (wp2 - wp0) * t->v;
                            nearest = t->distance;
                        }
                    }
                    if (!hit_point) {
                        // Subpixel TAA silhouette edge: project & clamp to the exact picked foreground triangle!
                        const V3 e1 = wp1 - wp0;
                        const V3 e2 = wp2 - wp0;
                        const V3 wn = math::cross(e1, e2);
                        const float denom = math::dot(ray.direction, wn);
                        V3 cand = (wp0 + wp1 + wp2) * (1.f / 3.f);
                        if (std::abs(denom) > 1e-12f) {
                            const float rt = math::dot(wp0 - ray.origin, wn) / denom;
                            if (rt >= 0.f && std::isfinite(rt))
                                cand = ray.origin + ray.direction * rt;
                        }
                        const V3 v0 = cand - wp0;
                        const float d00 = math::dot(e1, e1);
                        const float d01 = math::dot(e1, e2);
                        const float d11 = math::dot(e2, e2);
                        const float d20 = math::dot(v0, e1);
                        const float d21 = math::dot(v0, e2);
                        const float det = d00 * d11 - d01 * d01;
                        if (det > 1e-20f) {
                            float u = std::max(0.f, (d11 * d20 - d01 * d21) / det);
                            float v = std::max(0.f, (d00 * d21 - d01 * d20) / det);
                            if (u + v > 1.f) {
                                const float s_uv = 1.f / (u + v);
                                u *= s_uv;
                                v *= s_uv;
                            }
                            cand = wp0 + e1 * u + e2 * v;
                        }
                        hit_point = cand;
                        nearest = std::max(0.f, math::dot(cand - ray.origin, ray.direction));
                    }
                }
                if (!hit_point) {
                    const double nx =
                        (double(x) - double(view.bounds.x)) / double(std::max(view.bounds.width, 1.f)) * 2.0 - 1.0;
                    const double ny =
                        1.0 - (double(y) - double(view.bounds.y)) / double(std::max(view.bounds.height, 1.f)) * 2.0;
                    const math::Vector4d world4 = math::Vector4d{nx, ny, double(gpu_hit->depth), 1.0} * view.inverse;
                    if (std::abs(world4.w) > 1e-12) {
                        const V3 p{float(world4.x / world4.w), float(world4.y / world4.w), float(world4.z / world4.w)};
                        hit_point = p;
                        nearest = std::max(0.f, math::dot(p - ray.origin, ray.direction));
                    }
                }
                if (hit_point)
                    found = Hit{item.entity, *hit_point, facing};
            }
        }
    } else {
        for (const auto& item : view.scene->meshes) {
            const auto inverse = math::try_inverse(item.world);
            if (!inverse)
                continue;
            const ViewportView::Ray local{math::transform_point(ray.origin, *inverse),
                                          math::transform_direction(ray.direction, *inverse)};
            if (!ray_hits_bounds(local, item.geometry->bounds(), nearest))
                continue;
            const auto& m = item.world;
            const auto normal_transform =
                math::normal_transform({m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]});
            if (!normal_transform)
                continue;
            const auto normal = [&](V3 n) {
                const auto& a = normal_transform->matrix;
                return V3{n.x * a[0] + n.y * a[3] + n.z * a[6], n.x * a[1] + n.y * a[4] + n.z * a[7],
                          n.x * a[2] + n.y * a[5] + n.z * a[8]};
            };
            // Same common scale as material_transform_tangent: it avoids overflow
            // without normalizing individual corners before interpolation.
            float scale{};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 3; ++column)
                    scale = std::max(scale, std::abs(m[row * 4 + column]));
            auto tangent_matrix = m;
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 3; ++column)
                    tangent_matrix[row * 4 + column] /= scale;
            const auto vertices = item.geometry->vertices();
            for (std::size_t i = 0; i < vertices.size(); i += 3) {
                const auto at = [&](std::size_t n) {
                    const auto& p = vertices[n].position;
                    return V3{p[0], p[1], p[2]};
                };
                const auto t = triangle(local, at(i), at(i + 1), at(i + 2));
                if (t && t->distance < nearest) {
                    const auto slot = item.geometry->material_slot(i / 3);
                    const auto& surface = *view.scene->materials[(*item.material_slots)[slot]];
                    // Renderer orients each face toward its authored vertex normals
                    // before applying the inverse transpose, including reflections.
                    const auto position = [&](std::size_t n) {
                        const auto& p = vertices[n].position;
                        return math::Vector3d{p[0], p[1], p[2]};
                    };
                    auto face = math::cross(position(i + 1) - position(i), position(i + 2) - position(i));
                    math::Vector3d authored{};
                    for (std::size_t n = i; n < i + 3; ++n) {
                        const auto& a = vertices[n].normal;
                        authored += math::Vector3d{a[0], a[1], a[2]};
                    }
                    if (math::dot(face, authored) < 0)
                        face = -face;
                    face = math::try_normalize(face).value_or(math::Vector3d{});
                    material::Context context;
                    context.geometric_normal_world = math::try_normalize(normal(V3(face))).value_or(V3{});
                    if (!surface.policy().double_sided && math::dot(context.geometric_normal_world, ray.direction) >= 0)
                        continue;
                    const std::array weights{1 - t->u - t->v, t->u, t->v};
                    V3 shading{};
                    V3 tangent{};
                    float handedness{};
                    std::array<V3, 3> positions;
                    for (std::size_t corner = 0; corner < 3; ++corner) {
                        const auto& vertex = vertices[i + corner];
                        const auto weight = weights[corner];
                        positions[corner] = math::transform_point(at(i + corner), item.world);
                        context.uv += V2{vertex.uv[0], vertex.uv[1]} * weight;
                        shading += normal({vertex.normal[0], vertex.normal[1], vertex.normal[2]}) * weight;
                        tangent += math::transform_direction(V3{vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]},
                                                             tangent_matrix) *
                                   weight;
                        handedness += vertex.tangent[3] * normal_transform->mirror * weight;
                    }
                    context.position_world =
                        positions[0] + (positions[1] - positions[0]) * t->u + (positions[2] - positions[0]) * t->v;
                    context.normal_world = math::try_normalize(shading).value_or(V3{});
                    context.tangent_world = {tangent.x, tangent.y, tangent.z, handedness < 0 ? -1.f : 1.f};
                    if (surface.policy().coverage == material::Coverage::mask) {
                        const auto output = surface.evaluate(context);
                        if (!output || !(output->coverage >= 0))
                            continue; // Includes NaN, as in the raster/ray consumers.
                    }
                    const auto point = context.position_world;
                    if (view.project(point)) {
                        auto facing = context.geometric_normal_world;
                        if (math::dot(facing, ray.direction) > 0)
                            facing = -facing;
                        nearest = t->distance;
                        found = Hit{item.entity, point, facing};
                    }
                }
            }
        }
    }
    if (!locators || !view.show_locators)
        return found;
    for (const auto& locator : view.scene->locators) {
        const auto origin = locator.position;
        const auto p = view.project(origin);
        const auto depth = math::dot(origin - ray.origin, ray.direction);
        if (p && math::distance(*p, V2{x, y}) <= 8 * view.dpi_scale && depth < nearest && depth >= 0) {
            nearest = depth;
            found = Hit{locator.entity, origin};
        }
    }
    return found;
}

Result<void> ViewportTool::place_cursor(const ViewportView& view, V2 p, ToolSettings settings, bool control) {
    Optional<V3> position;
    V3 normal = view.construction_normal;
    if (settings.cursor_surface)
        if (const auto hit = trace(view, p.x, p.y, false)) {
            position = hit->position;
            normal = hit->normal;
        }
    if (!position)
        position = on_plane(view, p, {}, view.construction_normal);
    if (!position)
        position = on_plane(view, p, cursor, view.forward);
    if (!position)
        return invalid("The cursor cannot be placed on the current construction plane.");
    if (settings.grid_snap != control)
        for (int axis = 0; axis < 3; ++axis)
            (*position)[axis] = std::round((*position)[axis] / settings.grid_spacing) * settings.grid_spacing;
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite((*position)[axis]) || std::abs((*position)[axis]) > 10000)
            return invalid("Cursor position is outside the editor range.");
    cursor = *position;
    // Blender's "Surface Project" orientation: Z follows the surface normal, X stays
    // as close to the view's right vector as the plane allows.
    const auto z = math::try_normalize(normal).value_or(V3{0, 1, 0});
    auto x = math::try_normalize(view.right - z * math::dot(view.right, z));
    if (!x)
        x = math::try_normalize(math::cross(V3{0, 0, 1}, z));
    const auto y = math::cross(z, *x);
    cursor_rotation = frame_rotation(*x, y, z);
    hint_ = L"3D Cursor placed · Right click selects · W opens the viewport menu";
    return {};
}

TransformTool ViewportTool::displayed_tool(ToolSettings settings) const noexcept {
    if (gesture_)
        return gesture_->operation == Operation::move     ? TransformTool::move
               : gesture_->operation == Operation::rotate ? TransformTool::rotate
                                                          : TransformTool::scale;
    return marquee_ ? TransformTool::select : settings.tool;
}

void ViewportTool::cursor_to_grid(ToolSettings settings) noexcept {
    for (int axis = 0; axis < 3; ++axis)
        cursor[axis] = std::round(cursor[axis] / settings.grid_spacing) * settings.grid_spacing;
}

Result<void> ViewportTool::cursor_to_selection(const Session& session, Span<const ObjectId> entities) {
    const auto selected = targets(session, entities, {}, PivotMode::median, cursor);
    if (!selected)
        return invalid("Select an Entity with a Transform.");
    cursor = selected->pivot;
    return {};
}

Result<void> ViewportTool::snap_selection(Session& session, Span<const ObjectId> entities, ToolSettings settings,
                                          Optional<V3> destination) {
    const auto selected = targets(session, entities, {}, PivotMode::median, cursor);
    if (!selected)
        return invalid("Select an Entity with a Transform.");
    auto values = selected->transforms;
    for (auto& value : values) {
        if (destination)
            value.position += *destination - selected->pivot;
        else
            for (int axis = 0; axis < 3; ++axis)
                value.position[axis] = std::round(value.position[axis] / settings.grid_spacing) * settings.grid_spacing;
    }
    const auto token = next_session_++;
    ATELIER_TRY_BIND(result, session.begin_transform(selected->entities, token));
    result = preview_world(session, token, selected->entities, values);
    if (result)
        return session.commit_edit(token);
    const auto cancelled = session.cancel_edit();
    return cancelled ? result : cancelled;
}

Array<ObjectId> ViewportTool::pick_box(const Session& session, const ViewportView& view, ui::Rect box) const {
    Array<ObjectId> found;
    if (!view.scene || view.scene->scene_identity != session.presentation().identity())
        return found;
    const auto intersects = [&](float left, float top, float right, float bottom) {
        return left <= box.x + box.width && right >= box.x && top <= box.y + box.height && bottom >= box.y;
    };
    Set<ObjectId> selected_set;
    if (view.gpu_pick_box && box.width >= 0.f && box.height >= 0.f) {
        const float bx0 = std::max(0.f, std::floor(box.x));
        const float by0 = std::max(0.f, std::floor(box.y));
        const float bx1 = std::max(bx0, std::floor(box.x + box.width));
        const float by1 = std::max(by0, std::floor(box.y + box.height));
        for (const auto idx : view.gpu_pick_box(static_cast<std::uint32_t>(bx0), static_cast<std::uint32_t>(by0),
                                                static_cast<std::uint32_t>(bx1), static_cast<std::uint32_t>(by1))) {
            if (idx < view.scene->meshes.size()) {
                const auto entity = view.scene->meshes[idx].entity;
                if (selected_set.add(entity))
                    found.add(entity);
            }
        }
    }
    for (const auto& item : view.scene->meshes) {
        if (selected_set.contains(item.entity))
            continue;
        const auto origin = view.project({item.world[12], item.world[13], item.world[14]});
        if (origin && intersects(origin->x, origin->y, origin->x, origin->y)) {
            selected_set.add(item.entity);
            found.add(item.entity);
            continue;
        }
        const auto& local_bounds = item.geometry->bounds();
        if (!math::is_empty(local_bounds) && math::is_finite(local_bounds)) {
            float b_left = std::numeric_limits<float>::max();
            float b_top = b_left;
            float b_right = -b_left;
            float b_bottom = -b_left;
            bool all_projected = true;
            for (int corner = 0; corner < 8; ++corner) {
                const V3 local_p{(corner & 1) ? local_bounds.maximum.x : local_bounds.minimum.x,
                                 (corner & 2) ? local_bounds.maximum.y : local_bounds.minimum.y,
                                 (corner & 4) ? local_bounds.maximum.z : local_bounds.minimum.z};
                const auto proj = view.project(math::transform_point(local_p, item.world));
                if (!proj) {
                    all_projected = false;
                    break;
                }
                b_left = std::min(b_left, proj->x);
                b_right = std::max(b_right, proj->x);
                b_top = std::min(b_top, proj->y);
                b_bottom = std::max(b_bottom, proj->y);
            }
            if (all_projected) {
                if (!intersects(b_left, b_top, b_right, b_bottom))
                    continue;
                if (b_left >= box.x && b_right <= box.x + box.width && b_top >= box.y &&
                    b_bottom <= box.y + box.height) {
                    selected_set.add(item.entity);
                    found.add(item.entity);
                    continue;
                }
            }
        }
        float left = std::numeric_limits<float>::max();
        float top = left;
        float right = -left;
        float bottom = -left;
        for (const auto& vertex : item.geometry->vertices()) {
            const auto& p = vertex.position;
            const auto projected = view.project(math::transform_point(V3{p[0], p[1], p[2]}, item.world));
            if (!projected)
                continue;
            left = std::min(left, projected->x);
            right = std::max(right, projected->x);
            top = std::min(top, projected->y);
            bottom = std::max(bottom, projected->y);
        }
        if (left <= right && intersects(left, top, right, bottom)) {
            selected_set.add(item.entity);
            found.add(item.entity);
        }
    }
    if (view.show_locators) {
        for (const auto& locator : view.scene->locators) {
            const auto p = view.project(locator.position);
            if (p && intersects(p->x, p->y, p->x, p->y))
                found.add(locator.entity);
        }
    }
    return found;
}

// Shift/Ctrl keep their Blender meaning on top of the header mode: Shift toggles
// (extend/remove), Ctrl subtracts; with neither, the header mode decides.
Result<void> ViewportTool::combine(Session& session, Span<const ObjectId> hits, ObjectId active, SelectMode mode) {
    Array<ObjectId> selected;
    if (mode != SelectMode::set)
        selected.append(session.selected_entities());
    const auto contains = [&](ObjectId entity) { return std::ranges::find(selected, entity) != selected.end(); };
    if (mode == SelectMode::intersect)
        selected.remove_if([&](ObjectId entity) { return std::ranges::find(hits, entity) == hits.end(); });
    else
        for (const auto entity : hits) {
            if (mode == SelectMode::subtract || (mode == SelectMode::invert && contains(entity)))
                selected.remove_all(entity);
            else if (!contains(entity))
                selected.add(entity);
        }
    if (!contains(active))
        active = selected.empty() ? ObjectId{} : selected.back();
    return session.select_entities(selected, active);
}

Result<void> ViewportTool::select_click(Session& session, const ViewportView& view, V2 p, ToolSettings settings,
                                        bool shift, bool control) {
    const auto object = pick(session, view, p.x, p.y);
    const auto mode = control ? SelectMode::subtract : shift ? SelectMode::invert : settings.select_mode;
    // A miss in Set mode clears (Blender's deselect-on-nothing); other modes keep the selection.
    if (!object && mode != SelectMode::set)
        return {};
    Array<ObjectId> hits;
    if (object)
        hits.add(*object);
    return combine(session, hits, object.value_or(ObjectId{}), mode);
}

Result<void> ViewportTool::select_box(Session& session, const ViewportView& view, ToolSettings settings,
                                      const Marquee& marquee) {
    const auto a = marquee.start;
    const auto b = marquee.current;
    const auto hits =
        pick_box(session, view, {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)});
    const auto mode = marquee.control ? SelectMode::subtract
                      : marquee.shift ? SelectMode::extend
                                      : settings.select_mode;
    return combine(session, hits, hits.empty() ? ObjectId{} : hits.back(), mode);
}

Result<void> ViewportTool::select_all(Session& session, Span<const ObjectId> selected, SelectAll action) {
    const auto items = session.world().members();
    if (!items)
        return std::unexpected(items.error());
    Array<ObjectId> all;
    for (const auto& entity : *items)
        all.add(entity.id());
    const auto contains = [&](ObjectId entity) { return std::ranges::find(selected, entity) != selected.end(); };
    const bool everything = !all.empty() && std::ranges::all_of(all, contains);
    if (action == SelectAll::deselect || (action == SelectAll::toggle && everything))
        all.clear();
    else if (action == SelectAll::invert)
        all.remove_if(contains);
    hint_ = all.empty() ? L"Deselect All" : action == SelectAll::invert ? L"Invert Selection" : L"Select All";
    return session.select_entities(all, all.empty() ? ObjectId{} : all.back());
}

void ViewportTool::arm_box_select(V2 pointer, bool extend) {
    if (active())
        return;
    marquee_ = Marquee{pointer, pointer, false, extend, false, true};
    hint_ = L"Box select · Drag to select · Esc cancel";
}

Result<void> ViewportTool::place_duplicates(Session& session, const ViewportView& view, ToolSettings settings,
                                            V2 pointer) {
    if (!targets(session, session.selected_entities(), session.selection().entity, settings.pivot, cursor))
        return {}; // Nonspatial Entities still duplicate normally.
    return begin(session, view, settings, Operation::move, Handle::none, true, pointer, true);
}

Result<void> ViewportTool::begin(Session& session, const ViewportView&, ToolSettings settings, Operation operation,
                                 Handle handle, bool modal, V2 p, bool amend_creation) {
    if (active())
        return invalid("Finish the active viewport gesture before starting another.");
    auto selected = targets(session, session.selected_entities(), session.selection().entity, settings.pivot, cursor);
    if (!selected)
        return invalid("Select an Entity with a Transform.");
    const auto token = next_session_++;
    ATELIER_TRY(session.begin_transform(selected->entities, token, amend_creation));
    Gesture gesture;
    gesture.operation = operation;
    gesture.handle = handle;
    gesture.modal = modal;
    gesture.session = token;
    gesture.settings = settings;
    gesture.entities = std::move(selected->entities);
    gesture.before = std::move(selected->transforms);
    gesture.active_frame = selected->active_frame;
    gesture.pivot = selected->pivot;
    gesture.start = gesture.last = p;
    if (handle >= Handle::move_x && handle <= Handle::move_z)
        gesture.axis = int(handle) - int(Handle::move_x);
    if (handle >= Handle::rotate_x && handle <= Handle::rotate_z)
        gesture.axis = int(handle) - int(Handle::rotate_x);
    if (handle >= Handle::scale_x && handle <= Handle::scale_z)
        gesture.axis = int(handle) - int(Handle::scale_x);
    if (handle >= Handle::plane_yz && handle <= Handle::plane_xy) {
        gesture.axis = int(handle) - int(Handle::plane_yz);
        gesture.plane = true;
    }
    gesture_ = std::move(gesture);
    hint_ = L"X/Y/Z constrain · Ctrl snap · Enter confirm · Esc cancel";
    return {};
}

math::Quaternion ViewportTool::Gesture::basis() const noexcept {
    return transform_basis(active_frame.rotation, orientation());
}

Result<void> ViewportTool::update(Session& session, const ViewportView& view, V2 p, bool snap, bool fine) {
    auto& g = *gesture_;
    g.valid = false;
    Optional<float> typed;
    if (!g.typed.empty()) {
        char text[32]{};

        const std::size_t count = std::min<std::size_t>(g.typed.size(), sizeof(text));
        for (std::size_t i = 0; i < count; ++i)
            text[i] = static_cast<char>(g.typed[i]);

        float value{};

        const auto parsed = std::from_chars(text, text + count, value);
        if (parsed.ec != std::errc{} || parsed.ptr != text + count || !std::isfinite(value)) {
            hint_ = L"Value: " + g.typed + L" · Enter a complete number / Esc cancel";
            return {};
        }
        typed = value;
    }
    const float precision = fine ? .1f : 1.f;
    Array<world::Trs> values = g.before;
    std::wstring readout; // Blender header text, e.g. "D: 0.95 m (0.95 m) along global Z".
    const auto number = [](double value, int digits) {
        wchar_t text[48];
        std::swprintf(text, 48, L"%.*f", digits, value);
        return std::wstring(text);
    };
    const bool local = g.orientation() == Orientation::local;
    const auto frame_q = g.basis();
    const std::array<V3, 3> frame{math::rotate(frame_q, axes[0]), math::rotate(frame_q, axes[1]),
                                  math::rotate(frame_q, axes[2])};
    const auto basis = [&] {
        if (g.axis < 0)
            return std::wstring{};
        return std::wstring(g.plane ? (local ? L" locking local " : L" locking global ")
                                    : (local ? L" along local " : L" along global ")) +
               L"XYZ"[g.axis];
    };
    if (g.operation == Operation::move) {
        V3 delta{};
        if (g.axis >= 0 && !g.plane) {
            const auto axis = frame[g.axis];
            const auto normal = math::try_normalize(view.forward - axis * math::dot(view.forward, axis));
            if (normal) {
                const auto a = on_plane(view, g.start, g.pivot, *normal);
                const auto b = on_plane(view, p, g.pivot, *normal);
                if (a && b)
                    delta = axis * math::dot(*b - *a, axis);
            }
            if (typed)
                delta = axis * *typed;
        } else {
            const auto normal = g.plane ? frame[g.axis] : view.forward;
            const auto a = on_plane(view, g.start, g.pivot, normal);
            const auto b = on_plane(view, p, g.pivot, normal);
            if (a && b)
                delta = *b - *a;
            if (typed)
                delta = math::try_normalize(delta).value_or(g.plane ? frame[(g.axis + 1) % 3] : view.right) * *typed;
        }
        if (!typed)
            delta *= precision;
        if (((g.settings.grid_snap && g.settings.snap_move) != snap) && !typed && !local)
            for (int axis = 0; axis < 3; ++axis) {
                if (g.axis >= 0 && (g.plane ? axis == g.axis : axis != g.axis))
                    continue;
                const float step = g.settings.grid_spacing;
                const float origin = g.active_frame.position[axis];
                delta[axis] = std::round((origin + delta[axis]) / step) * step - origin;
            }
        for (auto& value : values)
            value.position += delta;
        if (g.axis >= 0 && !g.plane) {
            const auto d = math::dot(delta, frame[g.axis]);
            readout = L"D: " + number(d, 4) + L" m (" + number(d, 4) + L" m)" + basis();
        } else
            readout = L"Dx: " + number(delta.x, 4) + L" m   Dy: " + number(delta.y, 4) + L" m   Dz: " +
                      number(delta.z, 4) + L" m (" + number(math::length(delta), 4) + L" m)" + basis();
    } else if (g.operation == Operation::rotate) {
        const auto axis = g.axis >= 0 ? frame[g.axis] : -view.forward;
        const auto a = on_plane(view, g.last, g.pivot, axis);
        const auto b = on_plane(view, p, g.pivot, axis);
        if (a && b) {
            const auto u = math::try_normalize(*a - g.pivot);
            const auto v = math::try_normalize(*b - g.pivot);
            if (u && v)
                g.angle += std::atan2(math::dot(axis, math::cross(*u, *v)), math::dot(*u, *v));
        } else
            g.angle += (p.x - g.last.x) * .01f;
        float angle = typed ? *typed * pi / 180 : g.angle * precision;
        // Ctrl inverts the snap toggle, as for movement; the increment comes from the snap panel.
        const float step = g.settings.rotation_step * pi / 180;
        if (((g.settings.grid_snap && g.settings.snap_rotate) != snap) && !typed && step > 0)
            angle = std::round(angle / step) * step;
        const auto q = math::from_axis_angle(axis, angle);
        for (auto& value : values) {
            value.position = g.pivot + math::rotate(q, value.position - g.pivot);
            value.rotation = q * value.rotation;
        }
        readout = L"Rotation: " + number(angle * 180 / pi, 2) + (g.axis >= 0 ? basis() : std::wstring{});
    } else {
        float amount = 1 + (p.x - g.start.x - (p.y - g.start.y)) * .007f * precision;
        const auto orientation = frame_q;
        if (g.axis >= 0 && !g.plane) {
            const auto center = view.project(g.pivot);
            const auto tip = view.project(g.pivot + math::rotate(orientation, axes[g.axis]) *
                                                        (84 * view.pixel_size(g.pivot) * view.dpi_scale));
            if (center && tip) {
                const auto direction = *tip - *center;
                const auto square = math::dot(direction, direction);
                if (square > 1)
                    amount = 1 + math::dot(p - g.start, direction) / square * precision;
            }
        }
        if (typed)
            amount = *typed;
        else if (((g.settings.grid_snap && g.settings.snap_scale) != snap) && g.settings.scale_step > 0)
            amount = std::round(amount / g.settings.scale_step) * g.settings.scale_step;
        V3 factors{1, 1, 1};
        for (int axis = 0; axis < 3; ++axis)
            if (g.axis < 0 || (g.plane ? axis != g.axis : axis == g.axis))
                factors[axis] = amount;
        for (auto& value : values) {
            const auto offset =
                math::cwise_mul(math::rotate(math::conjugate(orientation), value.position - g.pivot), factors);
            value.scale = math::cwise_mul(value.scale, factors);
            value.position = g.pivot + math::rotate(orientation, offset);
        }
        // Scale always follows the active Entity's Local axes.
        readout = g.axis >= 0 && !g.plane ? L"Scale: " + number(amount, 4) + basis()
                                          : L"Scale X: " + number(factors.x, 4) + L"   Y: " + number(factors.y, 4) +
                                                L"  Z: " + number(factors.z, 4) + basis();
    }
    g.last = p;
    ATELIER_TRY_BIND(changed, preview_world(session, g.session, g.entities, values, g.operation == Operation::scale));
    g.valid = true;
    // Typed input shows the raw entry in brackets like Blender: "[2|]".
    hint_ = g.typed.empty() ? readout : readout + L"  [" + g.typed + L"|]";
    return {};
}

Result<void> ViewportTool::clear_channel(Session& session, Span<const ObjectId> entities, Operation channel) {
    const auto selected = targets(session, entities, {}, PivotMode::median, cursor);
    if (!selected)
        return invalid("Select an Entity with a Transform.");
    auto values = selected->transforms;
    for (std::size_t i = 0; i < values.size(); ++i) {
        ATELIER_TRY_BIND(original, world::read_transform(session.world().runtime(), selected->entities[i]));
        auto& value = values[i];
        value = *original;
        if (channel == Operation::move)
            value.position = {};
        else if (channel == Operation::rotate)
            value.rotation = {};
        else
            value.scale = {1, 1, 1};
    }
    const auto token = next_session_++;
    ATELIER_TRY_BIND(result, session.begin_transform(selected->entities, token));
    result = session.preview_transform(token, values);
    if (result)
        result = session.commit_edit(token);
    if (!result) {
        const auto cancelled = session.cancel_edit();
        return cancelled ? result : cancelled;
    }
    hint_ = channel == Operation::move     ? L"Clear Location"
            : channel == Operation::rotate ? L"Clear Rotation"
                                           : L"Clear Scale";
    return {};
}

Result<void> ViewportTool::finish(Session& session) {
    if (!gesture_)
        return {};
    if (!gesture_->valid)
        return invalid("Correct the invalid transform value before confirming, or press Esc to cancel.");
    ATELIER_TRY_BIND(result, session.commit_edit(gesture_->session));
    gesture_.reset();
    hover_ = Handle::none;
    hint_.clear();
    return {};
}

Result<void> ViewportTool::cancel(Session& session) {
    if (gesture_ && session.owns_edit(gesture_->session)) {
        ATELIER_TRY_BIND(result, session.cancel_edit());
    }
    gesture_.reset();
    marquee_.reset();
    hover_ = Handle::none;
    hint_.clear();
    return {};
}

void ViewportTool::forget(const Session& session) noexcept {
    if (gesture_ && !session.owns_edit(gesture_->session)) {
        gesture_.reset();
        hover_ = Handle::none;
        hint_.clear();
    }
}

ui::Cursor ViewportTool::pointer_cursor() const noexcept {
    if (marquee_)
        return ui::Cursor::crosshair;
    if (gesture_)
        return gesture_->operation == Operation::move     ? ui::Cursor::translate
               : gesture_->operation == Operation::rotate ? ui::Cursor::rotate
                                                          : ui::Cursor::scale;
    if (hover_ == Handle::none)
        return ui::Cursor::arrow;
    return hover_ >= Handle::scale_x    ? ui::Cursor::scale
           : hover_ >= Handle::rotate_x ? ui::Cursor::rotate
                                        : ui::Cursor::translate;
}

Result<bool> ViewportTool::pointer(Session& session, const ViewportView& view, const ui::Event& event,
                                   ToolSettings settings) {
    const V2 p{event.x, event.y};
    if (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::secondary &&
        (gesture_ || (marquee_ && marquee_->await_press))) {
        ATELIER_TRY_BIND(result, cancel(session));
        return true;
    }
    if (gesture_) {
        Result<void> result;
        if (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::middle && gesture_->modal) {
            // Blender: middle click during G/R/S constrains to the operation axis whose
            // screen direction best matches the pointer's travel since the start.
            const auto travel = p - gesture_->start;
            const auto center = view.project(gesture_->pivot);
            int best = -1;
            float score = 0;
            const auto basis = gesture_->basis();
            if (center && math::length(travel) > 2 * view.dpi_scale)
                for (int axis = 0; axis < 3; ++axis) {
                    const auto tip =
                        view.project(gesture_->pivot + math::rotate(basis, axes[axis]) *
                                                           view.pixel_size(gesture_->pivot) * view.dpi_scale * 60);
                    if (!tip)
                        continue;
                    const auto direction = math::try_normalize(*tip - *center);
                    if (!direction)
                        continue;
                    const float match = std::abs(math::dot(*direction, travel / math::length(travel)));
                    if (match > score) {
                        score = match;
                        best = axis;
                    }
                }
            gesture_->axis = best;
            gesture_->plane = false;
            gesture_->angle = 0;
            gesture_->last = gesture_->start;
            result = update(session, view, p, event.control, event.shift);
            if (!result)
                return std::unexpected(result.error());
            return true;
        }
        if (event.kind == ui::EventKind::pointer_move)
            result = update(session, view, p, event.control, event.shift);
        else if ((event.kind == ui::EventKind::pointer_up && !gesture_->modal &&
                  event.button == ui::PointerButton::primary) ||
                 (event.kind == ui::EventKind::pointer_down && gesture_->modal &&
                  event.button == ui::PointerButton::primary))
            result = finish(session);
        if (!result)
            return std::unexpected(result.error());
        return true;
    }
    if (marquee_) {
        if (event.kind == ui::EventKind::pointer_down && event.button == marquee_->button && marquee_->await_press) {
            if (!view.contains(p.x, p.y))
                return true;
            marquee_->start = marquee_->current = p;
            marquee_->await_press = false;
        }
        if (event.kind == ui::EventKind::pointer_move && !marquee_->await_press) {
            marquee_->current = math::clamp(p, {view.bounds.x, view.bounds.y},
                                            {view.bounds.x + view.bounds.width, view.bounds.y + view.bounds.height});
            marquee_->dragging |= math::distance(marquee_->current, marquee_->start) >= 4 * view.dpi_scale;
        }
        if (event.kind == ui::EventKind::pointer_up && event.button == marquee_->button && !marquee_->await_press) {
            const auto result = marquee_->dragging
                                    ? select_box(session, view, settings, *marquee_)
                                    : select_click(session, view, p, settings, marquee_->shift, marquee_->control);
            marquee_.reset();
            hint_.clear();
            if (!result)
                return std::unexpected(result.error());
        }
        return true;
    }
    if (!view.contains(p.x, p.y)) {
        hover_ = Handle::none;
        return false;
    }
    if (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::secondary) {
        marquee_ = Marquee{p, p, false, event.shift, event.control, false, ui::PointerButton::secondary};
        return true;
    }
    if (event.kind == ui::EventKind::pointer_move ||
        (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::primary)) {
        const auto selected =
            targets(session, session.selected_entities(), session.selection().entity, settings.pivot, cursor);
        hover_ = selected ? hit(view, *selected, settings.tool, p, settings.orientation) : Handle::none;
        if (event.kind == ui::EventKind::pointer_move)
            return false;
        if (hover_ != Handle::none && !event.shift && !event.control) {
            const auto operation = hover_ >= Handle::scale_x    ? Operation::scale
                                   : hover_ >= Handle::rotate_x ? Operation::rotate
                                                                : Operation::move;
            const auto result = begin(session, view, settings, operation, hover_, false, p);
            if (!result)
                return std::unexpected(result.error());
        } else {
            const auto placed = place_cursor(view, p, settings, event.control);
            if (!placed)
                return std::unexpected(placed.error());
        }
        return true;
    }
    return false;
}

Result<bool> ViewportTool::key(Session& session, const ViewportView& view, const ui::Event& event, std::uint32_t vk,
                               ToolSettings settings, V2 pointer) {
    if (active() && event.kind == ui::EventKind::key_down && event.key == ui::Key::escape) {
        const auto result = cancel(session);
        if (!result)
            return std::unexpected(result.error());
        return true;
    }
    if (marquee_)
        return true;
    if (gesture_) {
        if (event.kind == ui::EventKind::text) {
            if (const auto* text = std::get_if<String>(&event.data))
                for (std::size_t i = 0; i < text->size(); ++i) {
                    const char c = (*text)[i];
                    if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
                        if (gesture_->typed.size() < 24)
                            gesture_->typed += static_cast<wchar_t>(c);
                    }
                }
            const auto result = update(session, view, pointer, event.control, event.shift);
            if (!result)
                return std::unexpected(result.error());
            return true;
        }
        if (event.kind == ui::EventKind::key_down) {
            if (event.key == ui::Key::enter) {
                const auto result = finish(session);
                if (!result)
                    return std::unexpected(result.error());
                return true;
            }
            if (!event.repeat && !event.control && !event.alt && (vk == 'X' || vk == 'Y' || vk == 'Z')) {
                const int axis = int(vk - 'X');
                gesture_->axis = gesture_->axis == axis && gesture_->plane == event.shift ? -1 : axis;
                gesture_->plane = gesture_->axis >= 0 && event.shift;
                gesture_->angle = 0;
                gesture_->last = gesture_->start;
            } else if (event.key == ui::Key::backspace && !gesture_->typed.empty())
                gesture_->typed.pop_back();
        } else if (event.kind != ui::EventKind::key_up)
            return true;
        const auto result = update(session, view, pointer, event.control, event.shift);
        if (!result)
            return std::unexpected(result.error());
        return true;
    }
    if (event.kind != ui::EventKind::key_down)
        return false;
    if (event.control || event.alt)
        return false;
    if (event.repeat)
        return true;
    if (vk == 'G' || vk == 'R' || vk == 'S') {
        const auto result = begin(session, view, settings,
                                  vk == 'G'   ? Operation::move
                                  : vk == 'R' ? Operation::rotate
                                              : Operation::scale,
                                  Handle::none, true, pointer);
        if (!result)
            return std::unexpected(result.error());
        return true;
    }
    if (vk == 'B') {
        arm_box_select(pointer, event.shift);
        return true;
    }
    return false;
}

void ViewportTool::overlay(const Session& session, const ViewportView& view, ToolSettings settings,
                           Array<Shape>& out) const {
    const auto stroke = [&](V2 a, V2 b, float width, ui::Color color) {
        out.add({Shape::Kind::segment, {a.x, a.y, b.x, b.y}, width * view.dpi_scale, color});
    };
    if (view.scene && view.show_locators)
        for (const auto& locator : view.scene->locators) {
            const auto p = view.project(locator.position);
            if (!p)
                continue;
            const float r = 6 * view.dpi_scale;
            const ui::Color color = locator.selected ? picked : srgb(0xd8, 0xdc, 0xe0);
            stroke(*p - V2{r, 0}, *p + V2{r, 0}, 1.5f, color);
            stroke(*p - V2{0, r}, *p + V2{0, r}, 1.5f, color);
        }
    if (const auto selected =
            targets(session, session.selected_entities(), session.selection().entity, settings.pivot, cursor))
        handles(view, selected->pivot, selected->active_frame.rotation, settings.orientation, settings.tool,
                [&](Handle handle, Shape shape) {
                    if (handle == hover_ || (gesture_ && handle == gesture_->handle))
                        shape.color = picked;
                    out.add(shape);
                });
    if (gesture_) {
        // Blender modal feedback: the active constraint as an infinite line
        // through the pivot, and a dashed rubber band from the pivot to the pointer.
        const auto center = view.project(gesture_->pivot);
        if (gesture_->axis >= 0 && center) {
            const auto basis = gesture_->basis();
            const auto draw_axis = [&](int axis) {
                const auto direction = math::rotate(basis, axes[axis]);
                const float reach = view.pixel_size(gesture_->pivot) * view.dpi_scale * 4000;
                const auto a = view.project(gesture_->pivot - direction * reach);
                const auto b = view.project(gesture_->pivot + direction * reach);
                const auto color = colors[axis];
                if (a && b)
                    stroke(*a, *b, 1.5f, {color.r, color.g, color.b, .9f});
                else if (const auto tip = view.project(gesture_->pivot + direction * (reach * .01f))) {
                    const auto d = math::try_normalize(*tip - *center);
                    if (d)
                        stroke(*center - *d * 4000.f, *center + *d * 4000.f, 1.5f, {color.r, color.g, color.b, .9f});
                }
            };
            if (gesture_->plane) {
                for (int axis = 0; axis < 3; ++axis)
                    if (axis != gesture_->axis)
                        draw_axis(axis);
            } else
                draw_axis(gesture_->axis);
        }
        if (center && gesture_->modal && gesture_->operation != Operation::move) {
            const auto d = gesture_->last - *center;
            const float total = math::length(d);
            const float dash = 6 * view.dpi_scale;
            for (float at = 0; at < total; at += dash * 2) {
                const auto a = *center + d * (at / total);
                const auto b = *center + d * (std::min(at + dash, total) / total);
                stroke(a, b, 1.f, {0, 0, 0, .85f});
            }
        }
    }
    if (const auto p = view.project(cursor)) {
        // Blender 4.x 3D cursor, measured at 1x: 10 px dashed red/white ring and
        // four black ticks crossing it along the view's screen axes.
        const float scale = view.dpi_scale;
        const float r = 10 * scale;
        for (int segment = 0; segment < 48; ++segment) {
            // 16 alternating dashes, each drawn as three short chords.
            const auto point = [&](int n) {
                const auto angle = 2 * pi * float(n) / 48;
                return *p + V2{std::cos(angle), std::sin(angle)} * r;
            };
            stroke(point(segment), point(segment + 1), 1.5f,
                   (segment / 3) % 2 ? ui::Color{1, 1, 1, 1} : ui::Color{1, 0, 0, 1});
        }
        // Ticks are the cursor frame's X and Y axes in 3D, so a surface-aligned
        // cursor visibly lies on that surface. The ring stays screen-facing.
        const float metres = view.pixel_size(cursor) * scale;
        for (const auto local : {V3{1, 0, 0}, V3{0, 1, 0}}) {
            const auto axis = math::rotate(cursor_rotation, local);
            const auto tip = view.project(cursor + axis * (metres * 10));
            if (!tip)
                continue;
            auto direction = math::try_normalize(*tip - *p);
            if (!direction)
                continue;
            for (const float sign : {1.f, -1.f})
                stroke(*p + *direction * (sign * r * .55f), *p + *direction * (sign * r * 1.85f), 1.25f, {0, 0, 0, 1});
        }
    }
    if (marquee_ && marquee_->dragging) {
        const auto a = marquee_->start;
        const auto b = marquee_->current;
        out.add({Shape::Kind::rectangle,
                 {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)},
                 view.dpi_scale,
                 {.35f, .55f, .72f, .15f},
                 {.65f, .8f, .9f, 1}});
    }
}
}

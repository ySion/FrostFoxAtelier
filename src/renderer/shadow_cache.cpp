#include "shadow_cache.hpp"
#include "math/math.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace atelier::renderer::detail {
namespace {
using V = math::Vector3d;

using M = math::Matrix4d;

constexpr std::uint32_t cell_size = 128;

constexpr std::uint32_t grid_size = shadow_atlas_extent / cell_size;

// lit_shadow_sampling offsets each receiver along its normal by one interior
// texel: at most this fraction of its point distance or cascade radius, at
// the smallest face. The 3x3 filter footprint stays inside the guard texels.
constexpr double normal_offset_ratio = 2.0 / (cell_size - 2 * shadow_tile_border);

constexpr std::uint32_t no_slot = ~0u;

// Cascade fitting to the measured receiver depth; scene-lit.md gives the reasons.
constexpr double fit_near_margin = .8;

constexpr double fit_far_margin = 1.25;

constexpr std::int32_t fit_shrink_quarters = 2; // Quarter octaves.

constexpr std::uint32_t fit_shrink_frames = 32;

void check(bool condition, const char* message) {
    if (!condition)
        throw Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, message}, false};
}

M matrix(const std::array<float, 16>& input) {
    M result;
    for (std::size_t i = 0; i < 16; ++i)
        result.elements[i] = input[i];
    return result;
}

std::array<float, 16> packed(const M& input) {
    std::array<float, 16> result;
    for (std::size_t i = 0; i < 16; ++i) {
        result[i] = static_cast<float>(input.elements[i]);
        check(std::isfinite(result[i]), "Shadow projection exceeds GPU float representation");
    }
    return result;
}

double dot(V a, V b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double length(V a) {
    return std::sqrt(dot(a, a));
}

V cross(V a, V b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

V unit(V a) {
    const auto l = length(a);
    check(l > 0 && std::isfinite(l), "Invalid shadow basis");
    return a / l;
}

V corner(const ShadowBounds& b, std::uint32_t i) {
    return {i & 1 ? b.maximum[0] : b.minimum[0], i & 2 ? b.maximum[1] : b.minimum[1],
            i & 4 ? b.maximum[2] : b.minimum[2]};
}

double distance_squared(const ShadowBounds& b, V p) {
    double result = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double d = std::max(std::max(double(b.minimum[axis]) - p[axis], p[axis] - b.maximum[axis]), 0.0);
        result += d * d;
    }
    return result;
}

double far_distance(const ShadowBounds& b, V p) {
    double result = 0;
    for (std::uint32_t i = 0; i < 8; ++i)
        result = std::max(result, length(corner(b, i) - p));
    return result;
}

// Homogeneous plane rejection is conservative for boxes crossing the eye or
// near plane, unlike projecting only the corners and dividing by w.
bool intersects(const ShadowBounds& b, const M& vp, double padding = 0, double far_depth = 0) {
    std::array<bool, 6> outside{true, true, true, true, true, true};
    for (std::uint32_t i = 0; i < 8; ++i) {
        const auto p = corner(b, i);
        const auto q = math::Vector4d{p.x, p.y, p.z, 1} * vp;
        const double w = q.w * (1 + padding);
        // The GPU uses FP32 matrices/world positions. Include dot-product
        // cancellation error instead of assuming the double CPU plane is exact.
        double scale = 1;
        for (std::size_t column = 0; column < 4; ++column)
            scale = std::max(scale, std::abs(p.x * vp.elements[column]) + std::abs(p.y * vp.elements[4 + column]) +
                                        std::abs(p.z * vp.elements[8 + column]) + std::abs(vp.elements[12 + column]));
        const double epsilon = 1e-5 * scale;
        outside[0] &= q.x < -w - epsilon;
        outside[1] &= q.x > w + epsilon;
        outside[2] &= q.y < -w - epsilon;
        outside[3] &= q.y > w + epsilon;
        outside[4] &= q.z - far_depth * q.w < -epsilon;
        outside[5] &= q.z > q.w + epsilon;
    }
    return std::ranges::none_of(outside, [](bool value) { return value; });
}

ShadowBounds expanded(const ShadowBounds& b, double amount) {
    ShadowBounds result;
    for (std::size_t i = 0; i < 3; ++i) {
        // Outward rounding keeps the float bound conservative.
        result.minimum[i] =
            std::nextafter(static_cast<float>(double(b.minimum[i]) - amount), -std::numeric_limits<float>::infinity());
        result.maximum[i] =
            std::nextafter(static_cast<float>(double(b.maximum[i]) + amount), std::numeric_limits<float>::infinity());
    }
    return result;
}

double ceil_power_two(double value) {
    check(value > 0 && std::isfinite(value), "Invalid shadow projection extent");
    return std::exp2(std::ceil(std::log2(value)));
}

M view(V origin, V forward, V up) {
    const auto z = unit(forward);
    const auto x = unit(cross(up, z));
    const auto y = cross(z, x);
    return {
        {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -dot(origin, x), -dot(origin, y), -dot(origin, z), 1}};
}

M point_projection(V origin, std::uint32_t face, double near_z, double far_z) {
    constexpr std::array<V, 6> directions{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    constexpr std::array<V, 6> ups{{{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}}};
    const M projection{
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -near_z / (far_z - near_z), 1, 0, 0, near_z * far_z / (far_z - near_z), 0}};
    return view(origin, directions[face], ups[face]) * projection;
}

M expanded_projection(M vp, std::uint32_t size) {
    const double scale = double(size - 2 * shadow_tile_border) / size;
    for (std::size_t row = 0; row < 4; ++row) {
        vp.elements[row * 4] *= scale;
        vp.elements[row * 4 + 1] *= scale;
    }
    return vp;
}

struct Request {
    std::uint32_t kind{};

    std::uint32_t size{128};

    std::uint32_t count{};

    double projected_diameter{};

    double importance{}; // Point power times the camera solid angle of its support.

    std::array<bool, 6> needed{};

    std::array<M, 6> projection{};

    std::array<std::array<double, 2>, 6> depth{};

    std::array<float, 4> splits{};

    std::array<float, 4> starts{};

    V origin{}; // Point and rectangle cube centre.

    // Orthographic matrices are snapped at the allocated resolution.
    V axis_x{};

    V axis_y{};

    V axis_z{};

    std::array<V, 4> centers{};

    std::array<double, 4> radii{};
};

M request_projection(const Request& request, std::uint32_t face, std::uint32_t size) {
    if (request.kind == 1)
        return request.projection[face];
    const auto x = request.axis_x;
    const auto y = request.axis_y;
    const auto z = request.axis_z;
    const auto center = request.centers[face];
    const double radius = request.radii[face];
    const double texel = 2 * radius / (size - 2 * shadow_tile_border);
    const double cx = std::floor(dot(center, x) / texel + .5) * texel;
    const double cy = std::floor(dot(center, y) / texel + .5) * texel;
    const double near_z = request.depth[face][0];
    const double far_z = request.depth[face][1];
    return {{x.x / radius, y.x / radius, -z.x / (far_z - near_z), 0, x.y / radius, y.y / radius,
             -z.y / (far_z - near_z), 0, x.z / radius, y.z / radius, -z.z / (far_z - near_z), 0, -cx / radius,
             -cy / radius, far_z / (far_z - near_z), 1}};
}

V unproject(const M& inverse, double x, double y, double z) {
    const auto p = math::Vector4d{x, y, z, 1} * inverse;
    check(std::isfinite(p.w) && p.w != 0, "Shadow planning needs a finite camera near plane");
    return {p.x / p.w, p.y / p.w, p.z / p.w};
}

// Rectangles cast from their centre; their support matches lit_cluster_radius.
// `drawn` is the origin the light's faces were drawn from when it differs from
// the current one: while their redraw waits for budget the shader selects
// faces from it, so for every receiver the light now reaches, the faces it
// lies in as seen from there are kept too.
Request point_request(const Frame& frame, const GpuLightSource& source, std::span<const ShadowCaster> receivers,
                      const M& camera_view, const std::array<float, 3>* drawn) {
    Request request;
    request.kind = 1;
    request.count = 6;
    const V origin{source.origin_range[0], source.origin_range[1], source.origin_range[2]};
    request.origin = origin;
    // Match the FP32 minimum in lit_local_range before widening to double.
    const double effective_range = std::max(source.origin_range[3], .001f);
    const double extent = source.meta[0] == 1
                              ? .5 * (length({source.edge1_area[0], source.edge1_area[1], source.edge1_area[2]}) +
                                      length({source.edge2[0], source.edge2[1], source.edge2[2]}))
                              : source.direction_radius[3];
    const double support = effective_range + extent;
    const double rounding_margin = std::max(
        1e-4, 1e-5 * std::max(std::max(support, std::abs(origin.x)), std::max(std::abs(origin.y), std::abs(origin.z))));
    const double conservative_support = support + rounding_margin;
    const double far_z = ceil_power_two(conservative_support * (1 + normal_offset_ratio));
    const double near_z = std::max(1e-30, std::min(double(shadow_maximum_near), far_z * shadow_near_ratio));
    for (std::uint32_t face = 0; face < request.count; ++face) {
        request.projection[face] = point_projection(origin, face, near_z, far_z);
        request.depth[face] = {near_z, far_z};
    }
    const auto mark = [&](V at, const std::array<M, 6>& projections) {
        for (const auto& receiver : receivers) {
            if (distance_squared(receiver.bounds, origin) > conservative_support * conservative_support)
                continue;
            const auto bounds =
                expanded(receiver.bounds, far_distance(receiver.bounds, at) * normal_offset_ratio + rounding_margin);
            for (std::uint32_t face = 0; face < 6; ++face)
                request.needed[face] = request.needed[face] || intersects(bounds, projections[face], .04);
        }
    };
    mark(origin, request.projection);
    if (drawn) {
        const V at{(*drawn)[0], (*drawn)[1], (*drawn)[2]};
        std::array<M, 6> projections;
        for (std::uint32_t face = 0; face < 6; ++face)
            projections[face] = point_projection(at, face, near_z, far_z);
        mark(at, projections);
    }
    const auto eye = math::Vector4d{origin.x, origin.y, origin.z, 1} * camera_view;
    const auto projection = matrix(frame.camera.projection);
    const double pixels = std::max(double(frame.scene_viewport.width) * std::abs(projection.elements[0]),
                                   double(frame.scene_viewport.height) * std::abs(projection.elements[5]));
    const bool perspective = std::abs(projection.elements[11]) > 0;
    const double diameter = perspective ? pixels * support / std::max(eye.z - support, 1e-6) : pixels * support;
    request.projected_diameter = diameter;
    request.size = diameter > 512 ? 512u : (diameter > 192 ? 256u : 128u);
    const double radiance = .2126 * source.radiance[0] + .7152 * source.radiance[1] + .0722 * source.radiance[2];
    const double flux = source.meta[0] == 1 ? std::numbers::pi * source.edge1_area[3] * ((source.meta[1] & 1u) ? 2 : 1)
                                            : 4 * std::numbers::pi;
    const double camera_distance = length({eye.x, eye.y, eye.z});
    const double solid_angle =
        camera_distance <= support
            ? 4 * std::numbers::pi
            : 2 * std::numbers::pi * (1 - std::sqrt(1 - (support / camera_distance) * (support / camera_distance)));
    request.importance = std::max(0., radiance) * flux * solid_angle;
    return request;
}

// fit is the measured receiver range [near, far] in camera view Z. Without
// it, splits are fixed fractions of the conservative receiver bound.
Request directional_request(const Frame& frame, const GpuLightSource& source, std::span<const ShadowCaster> receivers,
                            std::span<const ShadowCaster> casters, const M& camera_view,
                            std::optional<std::array<double, 2>> fit) {
    Request request;
    request.kind = 2;
    request.count = 4;
    request.size = 1024;
    if (receivers.empty())
        return request;
    const auto inverse_projection = math::try_inverse(matrix(frame.camera.projection));
    const auto inverse_view = math::try_inverse(camera_view);
    check(bool(inverse_projection) && bool(inverse_view), "Invalid shadow camera inverse");
    std::array<V, 4> near_corners;
    std::array<V, 4> rays;
    double camera_near = std::numeric_limits<double>::infinity();
    double camera_far = -camera_near;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const double x = i & 1 ? 1 : -1;
        const double y = i & 2 ? 1 : -1;
        near_corners[i] = unproject(*inverse_projection, x, y, 1);
        const auto far_clip = math::Vector4d{x, y, frame.camera.far_depth, 1} * (*inverse_projection);
        check(std::isfinite(far_clip.x) && std::isfinite(far_clip.y) && std::isfinite(far_clip.z) &&
                  std::isfinite(far_clip.w),
              "Invalid shadow camera far direction");
        // H.xyz-N*H.w is a ray even when the reversed-Z far endpoint is at
        // infinity. Retain the previous division order for finite projections.
        const V homogeneous{far_clip.x, far_clip.y, far_clip.z};
        if (far_clip.w != 0) {
            const auto endpoint = homogeneous / far_clip.w;
            rays[i] = endpoint - near_corners[i];
            camera_far = std::max(camera_far, endpoint.z);
        } else {
            rays[i] = homogeneous - near_corners[i] * far_clip.w;
            camera_far = std::numeric_limits<double>::infinity();
        }
        camera_near = std::min(camera_near, near_corners[i].z);
    }
    check(camera_far > camera_near, "Invalid shadow camera depth interval");
    double receiver_far = camera_near;
    for (const auto& receiver : receivers)
        for (std::uint32_t i = 0; i < 8; ++i) {
            const auto p = corner(receiver.bounds, i);
            receiver_far = std::max(receiver_far, (math::Vector4d{p.x, p.y, p.z, 1} * camera_view).z);
        }
    double receiver_limit = ceil_power_two(std::max(receiver_far, 1e-3));
    // A receiver touching a power-of-two near plane must still leave a
    // nonzero cascade interval; finite camera far clipping remains authoritative.
    if (receiver_limit <= camera_near)
        receiver_limit = ceil_power_two(std::max(std::abs(camera_near), 1e-3) * 2);
    const double far_z = std::min(camera_far, receiver_limit);
    check(far_z > camera_near, "Invalid shadow receiver depth interval");
    // Fitted splits: cascade 0 ends at the first logarithmic split of the
    // fitted range and the other three split the rest logarithmically; no
    // split lies past the conservative one, so no cascade is coarser. Cascade
    // 0 still starts at the camera near plane, so no receiver in front of the
    // fitted range is missed.
    double fit_near = 0;
    double fit_far = 0;
    if (fit && camera_near > 0) {
        fit_near = std::max(camera_near, (*fit)[0]);
        fit_far = std::min(far_z, (*fit)[1]);
    }
    const bool fitted = fit_far > fit_near;
    constexpr std::array<double, 4> fractions{1.0 / 64, 1.0 / 16, 1.0 / 4, 1};
    std::array<double, 4> ends;
    for (std::uint32_t face = 0; face < 4; ++face)
        ends[face] = camera_near + (far_z - camera_near) * fractions[face];
    if (fitted) {
        const double first = std::min(ends[0], fit_near * std::pow(fit_far / fit_near, .25));
        for (std::uint32_t face = 0; face < 4; ++face)
            ends[face] = face == 3 ? fit_far : std::min(ends[face], first * std::pow(fit_far / first, face / 3.0));
    }
    request.axis_z = unit({source.direction_radius[0], source.direction_radius[1], source.direction_radius[2]});
    request.axis_x = unit(cross(std::abs(request.axis_z.y) < .95 ? V{0, 1, 0} : V{1, 0, 0}, request.axis_z));
    request.axis_y = cross(request.axis_z, request.axis_x);
    // Stable world-space depth planes include off-camera casters. Quantization
    // avoids rebuilding the static layer for small motion inside scene bounds.
    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    for (const auto& caster : casters)
        for (std::uint32_t i = 0; i < 8; ++i) {
            const double z = dot(corner(caster.bounds, i), request.axis_z);
            low = std::min(low, z);
            high = std::max(high, z);
        }
    const double step = ceil_power_two(std::max(high - low, 1.0)) / 16;
    double previous = camera_near;
    double offset = 0;
    for (std::uint32_t face = 0; face < 4; ++face) {
        const double end = ends[face];
        const double start = face == 0 ? camera_near : previous - .1 * (previous - camera_near);
        std::array<V, 8> corners;
        V center{};
        for (std::uint32_t i = 0; i < 8; ++i) {
            const auto ray = rays[i % 4];
            const double depth = i < 4 ? start : end;
            check(ray.z != 0, "Invalid shadow frustum ray");
            const auto camera = near_corners[i % 4] + ray * ((depth - near_corners[i % 4].z) / ray.z);
            corners[i] = math::transform_point(camera, *inverse_view);
            center = center + corners[i];
        }
        center = center / 8.0;
        double radius = 0;
        for (const auto& p : corners)
            radius = std::max(radius, length(p - center));
        // The margin covers snapping and the receiver normal offset at the
        // minimum 128 resolution. It is independent of allocation.
        radius = std::max(.01, std::ceil(radius * 16) / 16) * 1.04;
        offset = std::max(offset, radius * normal_offset_ratio);
        request.centers[face] = center;
        request.radii[face] = radius;
        request.starts[face] = static_cast<float>(start);
        request.splits[face] = static_cast<float>(end);
        request.needed[face] = true;
        previous = end;
    }
    // Offset receivers stay inside the depth range. The radii do not depend
    // on the camera pose, so the quantized planes stay stable as it moves.
    low = (std::floor((low - offset) / step) - 1) * step;
    high = (std::ceil((high + offset) / step) + 1) * step;
    for (auto& depth : request.depth)
        depth = {low, high};
    return request;
}
}

struct ShadowCache::State {
    struct Face {
        std::uint32_t slot{no_slot};

        std::uint32_t size{};

        std::uint32_t generation{};

        std::array<float, 16> projection{};

        std::array<float, 4> depth{}; // GpuShadowFace::depth of the drawn projection.

        std::uint64_t used{};

        std::uint64_t pinned{};

        bool stationary_valid{};

        bool dynamic_valid{};

        bool dynamic_nonempty{};

        bool drawn{};
    };

    struct Light {
        std::array<Face, 6> faces;

        std::uint32_t preferred_size{};

        bool high_tier{};

        std::uint32_t waiting{}; // Consecutive frames this light's redraw was deferred.

        std::array<float, 3> origin{}; // Cube centre all drawn faces share.

        float fade{1}; // Direct-light factor; withheld lights restart at 0.
    };

    // Cascades and lights without a stable key (no cached state to defer to)
    // always redraw, outside the budget.
    struct Redraw {
        std::uint32_t source;

        std::uint32_t face;

        std::uint32_t plan_face;

        std::uint32_t size;

        std::uint32_t rank;

        bool exempt;
    };

    struct ReservationUndo {
        Face* face;

        Face previous;
    };

    std::vector<Light> lights;

    std::vector<ReservationUndo> reservation_undo;

    std::vector<Light> previous_lights;

    std::vector<Request> requests;

    std::vector<std::uint32_t> sizes;

    std::vector<std::uint32_t> order;

    std::vector<Redraw> redraws;

    std::vector<std::uint8_t> queued; // Per light: 1 has redraws, 2 also has an undrawn face.

    std::vector<ShadowCaster> receivers;

    std::array<bool, grid_size * grid_size> occupied{};

    ShadowFramePlan plan;

    std::uint64_t scene{};

    std::uint64_t sequence{};

    std::uint64_t plan_serial{1};

    std::uint32_t next_generation{};

    bool initialized{};

    bool pending{};

    bool plan_ready{};

    // Fitted cascade range in quarter octaves of camera view Z. Each end grows
    // at once and shrinks on its own, to the envelope of fit_shrink_frames
    // consecutive readings that all stayed at least fit_shrink_quarters inside it.
    struct FitEnd {
        std::int32_t value{};

        std::int32_t envelope{};

        std::uint32_t frames{};
    };

    struct Fit {
        FitEnd lower;

        FitEnd upper;

        bool valid{};
    } fit;

    void measure(const std::optional<ShadowReceiverDepth>& depth) {
        if (!depth || !(depth->minimum > 0) || !(depth->maximum >= depth->minimum) || !std::isfinite(depth->maximum)) {
            fit = {};
            return;
        }
        const auto lower = static_cast<std::int32_t>(std::floor(4 * std::log2(fit_near_margin * depth->minimum)));
        const auto upper = static_cast<std::int32_t>(std::ceil(4 * std::log2(fit_far_margin * depth->maximum)));
        if (!fit.valid) {
            fit = {{lower, lower, 0}, {upper, upper, 0}, true};
            return;
        }
        // outward is +1 for the far end and -1 for the near end.
        const auto track = [](FitEnd& end, std::int32_t reading, std::int32_t outward) {
            if (outward * reading > outward * end.value) {
                end.value = reading;
                end.frames = 0;
                return;
            }
            if (outward * reading > outward * end.value - fit_shrink_quarters) {
                end.frames = 0;
                return;
            }
            end.envelope = end.frames && outward * end.envelope > outward * reading ? end.envelope : reading;
            if (++end.frames == fit_shrink_frames) {
                end.value = end.envelope;
                end.frames = 0;
            }
        };
        track(fit.lower, lower, -1);
        track(fit.upper, upper, 1);
    }

    std::optional<std::array<double, 2>> fitted() const {
        if (!fit.valid)
            return {};
        return std::array{std::exp2(fit.lower.value / 4.0), std::exp2(fit.upper.value / 4.0)};
    }

    std::uint32_t generation() {
        if (++next_generation == 0)
            ++next_generation;
        return next_generation;
    }

    void mark(const Face& face, bool value) {
        if (face.slot == no_slot)
            return;
        const auto x = face.slot % grid_size;
        const auto y = face.slot / grid_size;
        const auto n = face.size / cell_size;
        for (std::uint32_t row = 0; row < n; ++row)
            for (std::uint32_t column = 0; column < n; ++column)
                occupied[(y + row) * grid_size + x + column] = value;
    }

    void release(Face& face) {
        mark(face, false);
        face.slot = no_slot;
        face.stationary_valid = face.dynamic_valid = face.drawn = false;
    }

    bool allocate(Face& face, std::uint32_t size) {
        const auto n = size / cell_size;
        for (std::uint32_t y = 0; y + n <= grid_size; y += n)
            for (std::uint32_t x = 0; x + n <= grid_size; x += n) {
                bool free = true;
                for (std::uint32_t row = 0; row < n && free; ++row)
                    for (std::uint32_t column = 0; column < n; ++column)
                        if (occupied[(y + row) * grid_size + x + column]) {
                            free = false;
                            break;
                        }
                if (!free)
                    continue;
                face.slot = y * grid_size + x;
                face.size = size;
                face.stationary_valid = face.dynamic_valid = face.drawn = false;
                face.generation = generation();
                mark(face, true);
                return true;
            }
        return false;
    }

    bool evict() {
        Face* oldest = nullptr;
        for (auto& light : lights)
            for (auto& face : light.faces)
                if (face.slot != no_slot && face.pinned != plan_serial && (!oldest || face.used < oldest->used))
                    oldest = &face;
        if (!oldest)
            return false;
        reservation_undo.push_back({oldest, *oldest});
        release(*oldest);
        return true;
    }

    bool reserve(std::uint32_t source, const Request& request, std::uint32_t size) {
        reservation_undo.clear();
        bool resident = true;
        for (std::uint32_t i = 0; i < request.count; ++i)
            if (request.needed[i])
                resident &= lights[source].faces[i].slot != no_slot && lights[source].faces[i].size == size;
        if (resident) {
            for (std::uint32_t i = 0; i < request.count; ++i)
                if (request.needed[i]) {
                    lights[source].faces[i].pinned = plan_serial;
                    lights[source].faces[i].used = sequence;
                }
            return true;
        }
        // At most six faces are one atomic reservation. Rollback is CPU-only,
        // before jobs or descriptors are published; never overwrite this plan.
        const auto saved_occupied = occupied;
        auto& light = lights[source];
        // lights never resizes during a reservation. Record every needed face
        // before pinning, then only record actual evictions; keep journal capacity
        // across calls instead of copying the complete light table per source.
        for (std::uint32_t i = 0; i < request.count; ++i)
            if (request.needed[i])
                reservation_undo.push_back({&light.faces[i], light.faces[i]});
        for (std::uint32_t i = 0; i < request.count; ++i)
            if (request.needed[i])
                light.faces[i].pinned = plan_serial;
        for (std::uint32_t i = 0; i < request.count; ++i)
            if (request.needed[i]) {
                auto& face = light.faces[i];
                if (face.slot != no_slot && face.size != size)
                    release(face);
                if (face.slot == no_slot) {
                    while (!allocate(face, size))
                        if (!evict()) {
                            for (auto undo = reservation_undo.rbegin(); undo != reservation_undo.rend(); ++undo)
                                *undo->face = undo->previous;
                            occupied = saved_occupied;
                            reservation_undo.clear();
                            return false;
                        }
                }
                face.pinned = plan_serial;
                face.used = sequence;
            }
        reservation_undo.clear();
        return true;
    }

    void begin_plan(std::size_t sources) {
        plan.faces.clear();
        plan.updates.clear();
        plan.stats = {};
        plan.lights.assign(sources, {});
        redraws.clear();
        if (++plan_serial == 0) {
            plan_serial = 1;
            for (auto& light : lights)
                for (auto& face : light.faces)
                    face.pinned = 0;
        }
    }

    // Publishes the face's drawn projection; the near-plane bit set by prepare is kept.
    void publish(GpuShadowFace& gpu, const Face& face, std::uint32_t source, std::uint32_t index) {
        const float inverse = 1.f / shadow_atlas_extent;
        gpu.view_projection = face.projection;
        gpu.depth = face.depth;
        gpu.atlas = {float(face.size - 2 * shadow_tile_border) * inverse,
                     float(face.size - 2 * shadow_tile_border) * inverse,
                     float((face.slot % grid_size) * cell_size + shadow_tile_border) * inverse,
                     float((face.slot / grid_size) * cell_size + shadow_tile_border) * inverse};
        gpu.meta = {1u | (face.dynamic_nonempty ? 2u : 0u) | (gpu.meta[0] & 4u), face.generation, source, index};
    }

    void append(std::uint32_t source, const Request& request, std::uint32_t size, std::uint32_t rank, bool exempt) {
        auto& gpu_light = plan.lights[source];
        gpu_light.meta = {request.kind, static_cast<std::uint32_t>(plan.faces.size()), request.count, 0};
        gpu_light.splits = request.splits;
        for (std::uint32_t i = 0; i < request.count; ++i) {
            plan.faces.emplace_back();
            auto& gpu = plan.faces.back();
            gpu.meta = {0, 0, source, i};
            if (!request.needed[i])
                continue;
            ++plan.stats.requested_faces;
            plan.stats.atlas_cells += (size / cell_size) * (size / cell_size);
            auto& face = lights[source].faces[i];
            if (face.drawn && face.stationary_valid && face.dynamic_valid &&
                face.projection == packed(request_projection(request, i, size))) {
                // Equal cascade projections can follow different splits; the
                // blend band reads the current cascade start.
                if (request.kind == 2)
                    face.depth[3] = request.starts[i];
                ++plan.stats.reused_faces;
                publish(gpu, face, source, i);
                continue;
            }
            redraws.push_back({source, i, static_cast<std::uint32_t>(plan.faces.size() - 1), size, rank, exempt});
        }
    }

    void redraw(const Redraw& item, const Request& request, std::span<const ShadowCaster> casters) {
        auto& face = lights[item.source].faces[item.face];
        const auto vp = request_projection(request, item.face, item.size);
        const auto draw_vp = expanded_projection(vp, item.size);
        const auto projection = packed(vp);
        if (!face.drawn || face.projection != projection) {
            face.projection = projection;
            face.stationary_valid = face.dynamic_valid = false;
            face.generation = generation();
        }
        face.depth = {static_cast<float>(request.depth[item.face][0]), static_cast<float>(request.depth[item.face][1]),
                      1.f / float(item.size - 2 * shadow_tile_border),
                      request.kind == 2 ? request.starts[item.face] : 0};
        check(std::isfinite(face.depth[0]) && std::isfinite(face.depth[1]) && face.depth[1] > face.depth[0],
              "Shadow near/far planes exceed GPU float representation");
        const std::array<std::uint32_t, 4> rect{(face.slot % grid_size) * cell_size,
                                                (face.slot / grid_size) * cell_size, item.size, item.size};
        for (const auto layer : {ShadowLayer::stationary, ShadowLayer::dynamic}) {
            bool& valid = layer == ShadowLayer::stationary ? face.stationary_valid : face.dynamic_valid;
            if (valid)
                continue;
            ShadowUpdate job;
            job.face = item.plan_face;
            job.layer = layer;
            job.rect = rect;
            job.view_projection = packed(draw_vp);
            for (const auto& caster : casters)
                if (caster.dynamic == (layer == ShadowLayer::dynamic) && intersects(caster.bounds, draw_vp))
                    job.instances.push_back(caster.instance);
            if (layer == ShadowLayer::dynamic) {
                face.dynamic_nonempty = !job.instances.empty();
                ++plan.stats.dynamic_updates;
            } else
                ++plan.stats.static_updates;
            plan.stats.rendered_instances += static_cast<std::uint32_t>(job.instances.size());
            plan.updates.push_back(std::move(job));
            valid = true;
        }
        face.drawn = true;
        lights[item.source].origin = {float(request.origin.x), float(request.origin.y), float(request.origin.z)};
    }

    // A light's stale faces redraw together, so every drawn face of a cube
    // shares one origin; the shader selects and projects faces from it. A
    // light whose redraw does not fit the remaining budget waits whole.
    void schedule(std::span<const Request> requests, std::span<const ShadowCaster> casters) {
        queued.assign(lights.size(), 0);
        for (const auto& item : redraws)
            queued[item.source] =
                std::max<std::uint8_t>(queued[item.source], lights[item.source].faces[item.face].drawn ? 1 : 2);
        std::ranges::sort(redraws, [&](const Redraw& a, const Redraw& b) {
            if (a.exempt != b.exempt)
                return a.exempt;
            if (queued[a.source] != queued[b.source])
                return queued[a.source] > queued[b.source];
            const auto wa = lights[a.source].waiting;
            const auto wb = lights[b.source].waiting;
            if (wa != wb)
                return wa > wb;
            return a.rank != b.rank ? a.rank < b.rank : a.face < b.face;
        });
        std::uint32_t drawn = 0;
        for (auto first = redraws.begin(); first != redraws.end();) {
            const auto last =
                std::find_if(first, redraws.end(), [&](const Redraw& item) { return item.source != first->source; });
            const auto count = static_cast<std::uint32_t>(last - first);
            auto& light = lights[first->source];
            const bool draw = first->exempt || drawn + count <= shadow_face_redraw_budget;
            if (draw) {
                if (!first->exempt)
                    drawn += count;
                light.waiting = 0;
                // Faces kept from an earlier origin would be selected from the new one.
                const auto& request = requests[first->source];
                if (request.kind == 1 && light.origin != std::array{float(request.origin.x), float(request.origin.y),
                                                                    float(request.origin.z)}) {
                    std::array<bool, 6> redrawn{};
                    for (auto item = first; item != last; ++item)
                        redrawn[item->face] = true;
                    for (std::uint32_t i = 0; i < 6; ++i)
                        if (!redrawn[i])
                            light.faces[i].drawn = false;
                }
            } else {
                ++light.waiting;
                plan.stats.deferred_faces += count;
                plan.stats.deferral_frames = std::max(plan.stats.deferral_frames, light.waiting);
            }
            for (auto item = first; item != last; ++item) {
                auto& face = light.faces[item->face];
                if (draw)
                    redraw(*item, requests[item->source], casters);
                if (face.drawn)
                    publish(plan.faces[item->plan_face], face, item->source, item->face);
            }
            first = last;
        }
        // Undrawn faces would sample nothing; a partly drawn light would light
        // some directions only. Withhold the whole light instead.
        for (std::uint32_t source = 0; source < lights.size(); ++source) {
            const auto& request = requests[source];
            if (!request.kind)
                continue;
            auto& light = lights[source];
            auto& gpu = plan.lights[source];
            bool complete = gpu.meta[0] != 0;
            for (std::uint32_t i = 0; i < request.count; ++i)
                complete &= !request.needed[i] || light.faces[i].drawn;
            if (!complete) {
                if (gpu.meta[0]) {
                    gpu.meta[0] = 0;
                    ++plan.stats.pending_lights;
                }
                light.fade = 0;
                continue;
            }
            if (!queued[source])
                light.waiting = 0;
            light.fade = std::min(1.f, light.fade + 1.f / shadow_fade_frames);
            gpu.meta[3] = std::bit_cast<std::uint32_t>(light.fade);
            if (request.kind == 1)
                gpu.splits = {light.origin[0], light.origin[1], light.origin[2], 0};
        }
    }
};

ShadowCache::ShadowCache() : state_(std::make_unique<State>()) {}

ShadowCache::~ShadowCache() = default;

void ShadowCache::invalidate() noexcept {
    auto& s = *state_;
    s.lights.clear();
    s.previous_lights.clear();
    s.occupied.fill(false);
    s.plan.lights.clear();
    s.plan.faces.clear();
    s.plan.updates.clear();
    s.initialized = false;
    s.pending = false;
    s.plan_ready = false;
    s.fit = {};
}

void ShadowCache::commit() noexcept {
    if (state_->plan_ready)
        state_->pending = false;
}

const ShadowFramePlan& ShadowCache::prepare(const Frame& frame, const LightFrame& sources,
                                            std::span<const ShadowCaster> casters,
                                            std::span<const ShadowChange> changes,
                                            std::optional<ShadowReceiverDepth> depth, std::uint64_t sequence) {
    auto& s = *state_;
    if (s.plan_ready && s.initialized && s.scene == frame.scene_identity && s.sequence == sequence)
        return s.plan;
    // An unconsumed plan is never GPU-valid. It can be abandoned safely without
    // allowing its scheduled writes to masquerade as completed cache contents.
    if (s.pending || !s.initialized || s.scene != frame.scene_identity)
        invalidate();
    s.scene = frame.scene_identity;
    s.sequence = sequence;
    s.initialized = true;
    s.pending = true;
    s.plan_ready = false;
    s.measure(depth);
    s.previous_lights.swap(s.lights);
    auto& previous = s.previous_lights;
    s.lights.assign(sources.sources.size(), {});
    for (std::uint32_t i = 0; i < s.lights.size(); ++i) {
        const auto old = i < sources.current_to_previous.size() ? sources.current_to_previous[i] : no_slot;
        if (old < previous.size())
            s.lights[i] = std::move(previous[old]);
    }
    s.begin_plan(s.lights.size());
    s.occupied.fill(false);
    for (const auto& light : s.lights)
        for (const auto& face : light.faces)
            s.mark(face, true);
    for (auto& light : s.lights)
        for (auto& face : light.faces)
            if (face.slot != no_slot) {
                const auto vp = expanded_projection(matrix(face.projection), face.size);
                for (const auto& change : changes) {
                    if (!((change.old_present && intersects(change.old_bounds, vp)) ||
                          (change.new_present && intersects(change.new_bounds, vp))))
                        continue;
                    if (change.static_changed)
                        face.stationary_valid = false;
                    if (change.dynamic_changed)
                        face.dynamic_valid = false;
                }
            }
    const auto camera_view = matrix(frame.camera.view);
    const auto camera_vp = camera_view * matrix(frame.camera.projection);
    auto& receivers = s.receivers;
    receivers.clear();
    receivers.reserve(casters.size());
    const double camera_padding =
        2.0 / std::max(1.f, std::min(frame.scene_viewport.width, frame.scene_viewport.height));
    for (const auto& caster : casters) {
        for (std::size_t axis = 0; axis < 3; ++axis)
            check(std::isfinite(caster.bounds.minimum[axis]) && std::isfinite(caster.bounds.maximum[axis]) &&
                      caster.bounds.minimum[axis] <= caster.bounds.maximum[axis],
                  "Invalid shadow caster bounds");
        check(caster.instance < frame.meshes.size(), "Shadow caster instance exceeds current frame");
        if (intersects(caster.bounds, camera_vp, camera_padding, frame.camera.far_depth))
            receivers.push_back(caster);
    }
    auto& requests = s.requests;
    requests.assign(s.lights.size(), {});
    auto& sizes = s.sizes;
    sizes.assign(s.lights.size(), 0);
    std::uint64_t required_cells = 0;
    const auto cell_count = [](const Request& request, std::uint32_t size) {
        return std::uint64_t(std::count(request.needed.begin(), request.needed.end(), true)) * (size / cell_size) *
               (size / cell_size);
    };
    for (std::uint32_t index = 0; index < s.lights.size(); ++index) {
        const auto& light = sources.sources[index];
        // A light without radiance needs no visibility.
        if ((light.meta[1] & 2u) == 0 || (light.radiance[0] <= 0 && light.radiance[1] <= 0 && light.radiance[2] <= 0))
            continue;
        const auto& held = s.lights[index];
        const bool lagging =
            std::ranges::any_of(held.faces, [](const auto& face) { return face.drawn; }) &&
            held.origin != std::array{light.origin_range[0], light.origin_range[1], light.origin_range[2]};
        requests[index] = light.meta[0] == 2
                              ? directional_request(frame, light, receivers, casters, camera_view, s.fitted())
                              : point_request(frame, light, receivers, camera_view, lagging ? &held.origin : nullptr);
        if (requests[index].kind == 1) {
            auto& request = requests[index];
            const auto previous_size = s.lights[index].preferred_size;
            const auto diameter = request.projected_diameter;
            if (previous_size == 512 && diameter >= 384)
                request.size = 512;
            else if (previous_size == 256)
                request.size = diameter > 576 ? 512u : (diameter < 144 ? 128u : 256u);
            else if (previous_size == 128)
                request.size = diameter > 576 ? 512u : (diameter > 224 ? 256u : 128u);
            s.lights[index].preferred_size = request.size;
        }
    }
    // Lights already in the high tier rank as 1.25 times as important, so
    // near-equal lights do not trade tiers, and reallocate, on small motion.
    auto& order = s.order;
    order.clear();
    for (std::uint32_t index = 0; index < s.lights.size(); ++index)
        if (requests[index].kind == 1)
            order.push_back(index);
    const auto ranked = [&](std::uint32_t index) {
        return requests[index].importance * (s.lights[index].high_tier ? 1.25 : 1.);
    };
    std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        return ranked(a) != ranked(b) ? ranked(a) > ranked(b) : a < b;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const bool high = rank < shadow_high_tier_lights;
        s.lights[order[rank]].high_tier = high;
        if (!high)
            requests[order[rank]].size = cell_size;
    }
    for (std::uint32_t index = 0; index < s.lights.size(); ++index)
        if (requests[index].kind) {
            sizes[index] = requests[index].size;
            required_cells += cell_count(requests[index], sizes[index]);
        }
    // Fit the entire current demand before dropping any light. Point faces
    // are reduced first; the sun otherwise loses near detail for many
    // unrelated small lamps. The same demand gives the same tier choices.
    while (required_cells > grid_size * grid_size) {
        std::size_t selected = sizes.size();
        std::uint64_t largest = 0;
        for (std::uint32_t kind : {1u, 2u}) {
            for (std::size_t i = 0; i < sizes.size(); ++i)
                if (requests[i].kind == kind && sizes[i] > cell_size) {
                    const auto cost = cell_count(requests[i], sizes[i]);
                    if (cost > largest) {
                        largest = cost;
                        selected = i;
                    }
                }
            if (selected != sizes.size())
                break;
        }
        if (selected == sizes.size())
            break;
        required_cells -= largest;
        sizes[selected] /= 2;
        required_cells += cell_count(requests[selected], sizes[selected]);
    }
    // Equal importance prefers lights that already hold faces: reordering the
    // sources must not swap which light is resident and redraw both.
    const auto resident = [&](std::uint32_t index) {
        return std::ranges::any_of(s.lights[index].faces, [](const auto& face) { return face.slot != no_slot; });
    };
    order.clear();
    for (std::uint32_t index = 0; index < s.lights.size(); ++index)
        if (requests[index].kind)
            order.push_back(index);
    std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        const auto& ra = requests[a];
        const auto& rb = requests[b];
        if (ra.kind != rb.kind)
            return ra.kind == 2;
        if (ra.importance != rb.importance)
            return ra.importance > rb.importance;
        if (resident(a) != resident(b))
            return resident(a);
        return a < b;
    });
    for (std::uint32_t rank = 0; rank < order.size(); ++rank) {
        const auto index = order[rank];
        const auto& light = sources.sources[index];
        const auto& request = requests[index];
        std::uint32_t size = sizes[index];
        bool resident = s.reserve(index, request, size);
        while (!resident && size > cell_size)
            resident = s.reserve(index, request, size /= 2);
        if (!resident) {
            ++s.plan.stats.dropped_lights;
            continue;
        }
        if (size < request.size)
            ++s.plan.stats.resolution_downgrades;
        bool near_caster = false;
        if (request.kind == 1) {
            const V origin{light.origin_range[0], light.origin_range[1], light.origin_range[2]};
            const double near_z = request.depth[0][0];
            for (const auto& caster : casters)
                if (distance_squared(caster.bounds, origin) <= near_z * near_z) {
                    near_caster = true;
                    break;
                }
            if (near_caster)
                for (bool needed : request.needed)
                    if (needed)
                        ++s.plan.stats.near_plane_faces;
        }
        s.append(index, request, size, rank, request.kind == 2 || !sources.keyed(index));
        if (near_caster) {
            const auto first = s.plan.lights[index].meta[1];
            for (std::uint32_t face = 0; face < request.count; ++face)
                if (request.needed[face])
                    s.plan.faces[first + face].meta[0] |= 4;
        }
    }
    s.schedule(requests, casters);
    s.plan_ready = true;
    return s.plan;
}
}

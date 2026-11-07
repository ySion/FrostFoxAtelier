#include "procedural/bvh.hpp"
#include "foundation/fixed_array.hpp"
#include "foundation/inline_array.hpp"
#include "detail/triangles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace atelier::procedural {
namespace {
using detail::narrow;

using detail::widen;

using math::Vector3;

using math::Vector3d;

constexpr float infinity = std::numeric_limits<float>::infinity();

constexpr std::uint32_t leaf_size = 4;

constexpr std::uint32_t bins = 16;

// Ranges deeper than this split at the median, so a tree over fewer than 2^32 triangles is
// at most 72 levels deep and traversal fits a fixed stack.
constexpr std::uint32_t sah_depth = 40;

constexpr std::size_t stack_size = 128;

Vector3 lower(Vector3 a, Vector3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vector3 upper(Vector3 a, Vector3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

struct Box {
    Vector3 minimum{infinity, infinity, infinity};

    Vector3 maximum{-infinity, -infinity, -infinity};

    void add(Vector3 point) {
        minimum = lower(minimum, point);
        maximum = upper(maximum, point);
    }

    void add(const Box& box) {
        minimum = lower(minimum, box.minimum);
        maximum = upper(maximum, box.maximum);
    }

    [[nodiscard]] double area() const {
        const auto d = widen(maximum) - widen(minimum);
        return d.x < 0 ? 0 : d.x * d.y + d.y * d.z + d.z * d.x;
    }
};

constexpr double double_infinity = std::numeric_limits<double>::infinity();

double down(double x) {
    return std::nextafter(x, -double_infinity);
}

double up(double x) {
    return std::nextafter(x, double_infinity);
}

// Each elementary IEEE double operation is rounded once; its two adjacent values
// enclose the exact result. Composing these intervals includes the initial coordinate
// subtractions and cancellation, unlike an epsilon scaled by the final determinant.
// No reassociation/fast-math is allowed. This is a sign certificate, not a world-space band.
struct Interval {
    double lo;

    double hi;

    explicit Interval(double x) : lo{x}, hi{x} {}

    Interval(double lower, double upper) : lo{lower}, hi{upper} {}

    [[nodiscard]] int sign() const { return lo > 0 ? 1 : hi < 0 ? -1 : 0; }
};

Interval operator+(Interval a, Interval b) {
    return {down(a.lo + b.lo), up(a.hi + b.hi)};
}

Interval operator-(Interval a, Interval b) {
    return {down(a.lo - b.hi), up(a.hi - b.lo)};
}

Interval operator*(Interval a, Interval b) {
    const double ll = a.lo * b.lo;
    const double lh = a.lo * b.hi;
    const double hl = a.hi * b.lo;
    const double hh = a.hi * b.hi;
    return {down(std::min(std::min(ll, lh), std::min(hl, hh))), up(std::max(std::max(ll, lh), std::max(hl, hh)))};
}

Interval operator/(Interval a, double b) {
    return b > 0 ? Interval{down(a.lo / b), up(a.hi / b)} : Interval{down(a.hi / b), up(a.lo / b)};
}

using Interval3 = std::array<Interval, 3>;

Interval3 relative(Vector3 vertex, Vector3 point) {
    return {Interval{vertex.x} - Interval{point.x}, Interval{vertex.y} - Interval{point.y},
            Interval{vertex.z} - Interval{point.z}};
}

Interval determinant(const Interval3& a, const Interval3& b, const Interval3& c) {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
}

bool ray_box_interval(Vector3 minimum, Vector3 maximum, Vector3 point, Vector3d direction) {
    double near = 0;
    double far = double_infinity;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0) {
            if (point[axis] < minimum[axis] || point[axis] > maximum[axis])
                return false;
            continue;
        }
        const auto a = (Interval{minimum[axis]} - Interval{point[axis]}) / direction[axis];
        const auto b = (Interval{maximum[axis]} - Interval{point[axis]}) / direction[axis];
        near = std::max(near, std::min(a.lo, b.lo));
        far = std::min(far, std::max(a.hi, b.hi));
    }
    return near <= far;
}

bool ray_box(Vector3 minimum, Vector3 maximum, Vector3 point, Vector3d direction) {
#pragma clang fp contract(off) reassociate(off)
    // Certify the existing Interval box result, not a tighter geometric test:
    // changing which triangles reach the leaf could change retry/fallback counts.
    // Let u=2^-53, s=fl(bound-point), t=fl(s/d). Each old endpoint is
    // (s/d)*(1+eta1)*(1+eps)*(1+eta2), |eta|<=2u from nextafter.
    // In any IEEE rounding mode |eps|<=2u, so endpoint/t lies between
    // (1-2u)^3/(1+2u) and (1+2u)^3/(1-2u): error <9u*|t|.
    // h=16u*|t| is exact; fl(t +/- h) retains at least (14u-32u^2)*|t|,
    // more than 9u*|t| after rounding. No MXCSR check or change is needed.
    // Nonzero float-derived s and d give |t| about 2^-277..2^278 and
    // |h|>=about 2^-326: no double subnormals or overflow. DAZ conversion
    // of input floats is shared with the old path; zero s uses that path.
    double near_lo = 0;
    double near_hi = 0;
    double far_lo = double_infinity;
    double far_hi = double_infinity;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0) {
            if (point[axis] < minimum[axis] || point[axis] > maximum[axis])
                return false;
            continue;
        }
        const double a = double{minimum[axis]} - point[axis];
        const double b = double{maximum[axis]} - point[axis];
        if (a == 0 || b == 0)
            return ray_box_interval(minimum, maximum, point, direction);
        const double ta = a / direction[axis];
        const double tb = b / direction[axis];
        const double ha = std::abs(ta) * 0x1p-49;
        const double hb = std::abs(tb) * 0x1p-49;
        near_lo = std::max(near_lo, std::min(ta - ha, tb - hb));
        near_hi = std::max(near_hi, std::min(ta + ha, tb + hb));
        far_lo = std::min(far_lo, std::max(ta - ha, tb - hb));
        far_hi = std::min(far_hi, std::max(ta + ha, tb + hb));
    }
    // Monotone min/max enclose the old near/far; only decide when separated.
    if (near_lo > far_hi)
        return false;
    if (near_hi <= far_lo)
        return true;
    return ray_box_interval(minimum, maximum, point, direction);
}

bool band_box(Vector3 minimum, Vector3 maximum, double tolerance, Vector3 point) {
    // Let u=2^-53 and S=tolerance/(8*float epsilon). The nonnegative weights
    // returned by closest_on_triangle sum to at most 1+8u: at most 16u*S
    // beyond the vertex box. Edge differences and reconstruction cost <16u*S;
    // the accepted squared-distance rounding adds <8u*tolerance. Together
    // these are <34u*S. 32*double epsilon*S = tolerance*2^-27 covers them.
    // This only widens pruning, never the leaf's original surface tolerance.
    const double prune_tolerance = up(tolerance + tolerance * 0x1p-27);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // Round the gap outward, not bound +/- tolerance: the latter can cancel
        // to zero and prune a point whose leaf distance rounds down to tolerance.
        // Float bounds/point widen exactly; no narrowing back to float occurs.
        if (down(double{minimum[axis]} - point[axis]) > prune_tolerance ||
            down(double{point[axis]} - maximum[axis]) > prune_tolerance)
            return false;
    }
    return true;
}

}

Result<Bvh> Bvh::build(const Geometry& surface) {
    const auto view = detail::TriangleView::create(surface);
    if (!view)
        return std::unexpected(view.error());
    const auto& positions = surface.positions();
    const auto corners = surface.vertex_points();
    Bvh result;
    std::vector<Triangle> triangles;
    triangles.reserve(view->size());
    for (const auto& [p, j, vertices] : *view) {
        const Triangle triangle{positions[corners[vertices[0]]], positions[corners[vertices[1]]],
                                positions[corners[vertices[2]]], p, j};
        // A zero-area triangle has no surface to hit or project onto.
        const auto a = widen(triangle.a);
        if (math::cross(widen(triangle.b) - a, widen(triangle.c) - a) != Vector3d{})
            triangles.push_back(triangle);
    }
    const auto count = static_cast<std::uint32_t>(triangles.size());
    if (!count)
        return result;
    result.polygon_tolerances_.reserve(surface.count(Layer::primitive));
    for (std::uint32_t p = 0; p < surface.count(Layer::primitive); ++p)
        result.polygon_tolerances_.push_back(detail::polygon_scale(surface, surface.primitive(p)).tolerance);
    std::vector<Box> bounds(count);
    std::vector<Vector3> centers(count);
    std::vector<std::uint32_t> order(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        bounds[i].add(triangles[i].a);
        bounds[i].add(triangles[i].b);
        bounds[i].add(triangles[i].c);
        centers[i] = bounds[i].minimum * 0.5f + bounds[i].maximum * 0.5f;
        order[i] = i;
    }
    struct Task {
        std::uint32_t node;

        std::uint32_t begin;

        std::uint32_t end;

        std::uint32_t depth;
    };
    InlineArray<Task, 64> tasks{{0, 0, count, 0}};
    result.nodes_.reserve(2 * std::size_t{count} / leaf_size + 1);
    result.nodes_.push_back({});
    result.node_tolerances_.reserve(result.nodes_.capacity());
    result.node_tolerances_.push_back(0);
    while (!tasks.empty()) {
        const auto [node, begin, end, depth] = tasks.back();
        tasks.pop();
        Box box;
        Box spread;
        double tolerance = 0;
        for (auto i = begin; i < end; ++i) {
            box.add(bounds[order[i]]);
            spread.add(centers[order[i]]);
            tolerance = std::max(tolerance, result.polygon_tolerances_[triangles[order[i]].primitive]);
        }
        result.nodes_[node].minimum = box.minimum;
        result.nodes_[node].maximum = box.maximum;
        result.node_tolerances_[node] = tolerance;
        const auto leaf = [&] {
            result.nodes_[node].first = begin;
            result.nodes_[node].count = end - begin;
        };
        const auto extent = widen(spread.maximum) - widen(spread.minimum);
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        if (end - begin <= leaf_size || !(extent[axis] > 0)) {
            leaf();
            continue;
        }
        auto middle = begin;
        if (depth < sah_depth) {
            const double scale = bins / extent[axis];
            const double origin = spread.minimum[axis];
            const auto bin = [&](std::uint32_t i) {
                return std::min(bins - 1, static_cast<std::uint32_t>((centers[i][axis] - origin) * scale));
            };
            std::array<Box, bins> boxes;
            std::array<std::uint32_t, bins> counts{};
            for (auto i = begin; i < end; ++i) {
                const auto b = bin(order[i]);
                ++counts[b];
                boxes[b].add(bounds[order[i]]);
            }
            std::array<double, bins> left{};
            Box sweep;
            std::uint32_t swept = 0;
            for (std::uint32_t b = 0; b + 1 < bins; ++b) {
                sweep.add(boxes[b]);
                swept += counts[b];
                left[b] = sweep.area() * swept;
            }
            sweep = {};
            swept = 0;
            double best = std::numeric_limits<double>::infinity();
            std::uint32_t split = 1;
            for (std::uint32_t b = bins - 1; b > 0; --b) {
                sweep.add(boxes[b]);
                swept += counts[b];
                if (const double cost = left[b - 1] + sweep.area() * swept; cost < best) {
                    best = cost;
                    split = b;
                }
            }
            if (end - begin <= 4 * leaf_size && best >= box.area() * (end - begin)) {
                leaf();
                continue;
            }
            middle = static_cast<std::uint32_t>(std::partition(order.begin() + begin, order.begin() + end,
                                                               [&](std::uint32_t i) { return bin(i) < split; }) -
                                                order.begin());
        }
        if (middle == begin || middle == end) {
            middle = begin + (end - begin) / 2;
            std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
                             [&](std::uint32_t a, std::uint32_t b) { return centers[a][axis] < centers[b][axis]; });
        }
        const auto child = static_cast<std::uint32_t>(result.nodes_.size());
        result.nodes_[node].first = child;
        result.nodes_[node].count = 0;
        result.nodes_.push_back({});
        result.nodes_.push_back({});
        result.node_tolerances_.push_back(0);
        result.node_tolerances_.push_back(0);
        tasks.add({child + 1, middle, end, depth + 1});
        tasks.add({child, begin, middle, depth + 1});
    }
    result.triangles_.reserve(count);
    for (const auto i : order)
        result.triangles_.push_back(triangles[i]);
    return result;
}

std::optional<Hit> Bvh::ray(Vector3 origin_float, Vector3 direction_float, float max_distance) const noexcept {
    if (nodes_.empty())
        return std::nullopt;
    const auto origin = widen(origin_float);
    const auto direction = widen(direction_float);
    // Box tests stay in float: an overflowing slab distance is an infinite one, which is right.
    const Vector3 inverse{1 / direction_float.x, 1 / direction_float.y, 1 / direction_float.z};
    double best = max_distance;
    float bound = max_distance; // best rounded up to float.
    // Entry distance into a box, or infinity when the ray misses it within `bound`.
    const auto enter = [&](const Node& node) {
        float near = 0;
        float far = bound;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            // Parallel to the slab: both signs of zero keep the range when the origin is inside.
            if (direction_float[axis] == 0) {
                if (origin_float[axis] < node.minimum[axis] || origin_float[axis] > node.maximum[axis])
                    return infinity;
                continue;
            }
            float a = (node.minimum[axis] - origin_float[axis]) * inverse[axis];
            float b = (node.maximum[axis] - origin_float[axis]) * inverse[axis];
            if (a > b)
                std::swap(a, b);
            if (a > near)
                near = a;
            if (b < far)
                far = b;
        }
        return near <= far ? near : infinity;
    };
    std::optional<Hit> hit;
    FixedArray<std::pair<std::uint32_t, float>, stack_size> stack;
    if (const float entry = enter(nodes_[0]); entry != infinity)
        stack.add({0, entry});
    while (!stack.empty()) {
        const auto [index, entry] = stack.back();
        stack.pop();
        if (entry > bound)
            continue;
        const auto& node = nodes_[index];
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto& triangle = triangles_[i];
                const auto a = widen(triangle.a);
                const auto e1 = widen(triangle.b) - a;
                const auto e2 = widen(triangle.c) - a;
                const auto p = math::cross(direction, e2);
                const double determinant = math::dot(e1, p);
                if (determinant == 0)
                    continue;
                const double scale = 1 / determinant;
                const auto s = origin - a;
                const double u = math::dot(s, p) * scale;
                if (u < 0 || u > 1)
                    continue;
                const auto q = math::cross(s, e1);
                const double v = math::dot(direction, q) * scale;
                if (v < 0 || u + v > 1)
                    continue;
                const double distance = math::dot(e2, q) * scale;
                if (distance < 0 || distance > best)
                    continue;
                best = distance;
                bound = static_cast<float>(distance);
                if (bound < distance)
                    bound = std::nextafter(bound, infinity);
                hit = Hit{triangle.primitive,
                          triangle.triangle,
                          static_cast<float>(distance),
                          {static_cast<float>(u), static_cast<float>(v)},
                          narrow(a + e1 * u + e2 * v)};
            }
            continue;
        }
        const float left = enter(nodes_[node.first]);
        const float right = enter(nodes_[node.first + 1]);
        const bool left_first = left <= right;
        const std::pair<std::uint32_t, float> near{left_first ? node.first : node.first + 1, left_first ? left : right};
        const std::pair<std::uint32_t, float> far{left_first ? node.first + 1 : node.first, left_first ? right : left};
        if (far.second != infinity)
            stack.add(far);
        if (near.second != infinity)
            stack.add(near);
    }
    return hit;
}

std::optional<Hit> Bvh::closest(Vector3 point_float, float max_distance) const noexcept {
    if (nodes_.empty())
        return std::nullopt;
    const auto point = widen(point_float);
    double best = double(max_distance) * max_distance;
    const auto distance = [&](const Node& node) {
        double sum = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double d =
                std::max(std::max(node.minimum[axis] - point[axis], 0.0), point[axis] - node.maximum[axis]);
            sum += d * d;
        }
        return sum;
    };
    std::optional<Hit> hit;
    FixedArray<std::pair<std::uint32_t, double>, stack_size> stack;
    stack.add({0, distance(nodes_[0])});
    while (!stack.empty()) {
        const auto [index, gap] = stack.back();
        stack.pop();
        if (gap > best)
            continue;
        const auto& node = nodes_[index];
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto& triangle = triangles_[i];
                const auto a = widen(triangle.a);
                const auto b = widen(triangle.b);
                const auto c = widen(triangle.c);
                const auto uv = detail::closest_on_triangle(point, a, b, c);
                const auto position = a + (b - a) * uv.x + (c - a) * uv.y;
                const auto offset = position - point;
                if (const double squared = math::dot(offset, offset); squared <= best) {
                    best = squared;
                    hit = Hit{triangle.primitive,
                              triangle.triangle,
                              static_cast<float>(std::sqrt(squared)),
                              {static_cast<float>(uv.x), static_cast<float>(uv.y)},
                              narrow(position)};
                }
            }
            continue;
        }
        const double left = distance(nodes_[node.first]);
        const double right = distance(nodes_[node.first + 1]);
        const bool left_first = left <= right;
        stack.add({left_first ? node.first + 1 : node.first, left_first ? right : left});
        stack.add({left_first ? node.first : node.first + 1, left_first ? left : right});
    }
    return hit;
}

bool Bvh::on_surface_band(Vector3 point) const noexcept {
    if (nodes_.empty())
        return false;
    const auto q = widen(point);
    FixedArray<std::uint32_t, stack_size> stack{0};
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop();
        const auto& node = nodes_[index];
        if (!band_box(node.minimum, node.maximum, node_tolerances_[index], point))
            continue;
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto& t = triangles_[i];
                const double tolerance = polygon_tolerances_[t.primitive];
                if (!band_box(lower(lower(t.a, t.b), t.c), upper(upper(t.a, t.b), t.c), tolerance, point))
                    continue;
                const auto a = widen(t.a);
                const auto b = widen(t.b);
                const auto c = widen(t.c);
                const auto cross = math::cross(b - a, c - a);
                const auto normal = cross / math::length(cross);
                // Preserve C R2's plane broad phase and bounded-distance leaf predicate.
                if (std::abs(math::dot(q - a, normal)) > tolerance)
                    continue;
                const auto uv = detail::closest_on_triangle(q, a, b, c);
                const auto closest = a + (b - a) * uv.x + (c - a) * uv.y;
                const auto offset = closest - q;
                if (math::dot(offset, offset) <= tolerance * tolerance)
                    return true;
            }
            continue;
        }
        stack.add(node.first + 1);
        stack.add(node.first);
    }
    return false;
}

std::optional<std::int64_t> Bvh::signed_crossings(Vector3 point, Vector3 direction_float) const noexcept {
    std::int64_t count = 0;
    if (nodes_.empty())
        return count;
    // Float inputs widen exactly and keep all degree-three products within double range.
    const auto direction = widen(direction_float);
    const Interval3 d{Interval{direction.x}, Interval{direction.y}, Interval{direction.z}};
    FixedArray<std::uint32_t, stack_size> stack{0};
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop();
        const auto& node = nodes_[index];
        if (!ray_box(node.minimum, node.maximum, point, direction))
            continue;
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto& t = triangles_[i];
                // This per-triangle gate makes uncertainty independent of leaf grouping.
                if (!ray_box(lower(lower(t.a, t.b), t.c), upper(upper(t.a, t.b), t.c), point, direction))
                    continue;
                const auto a = relative(t.a, point);
                const auto b = relative(t.b, point);
                const auto c = relative(t.c, point);
                const int u = determinant(b, c, d).sign();
                const int v = determinant(c, a, d).sign();
                const int w = determinant(a, b, d).sign();
                if (std::min(std::min(u, v), w) < 0 && std::max(std::max(u, v), w) > 0)
                    continue; // Certified outside, including a parallel line missing the face.
                if (!u || !v || !w)
                    return std::nullopt;
                // u+v+w = normal dot d. Equal nonzero signs certify its sign without
                // division. det(a,b,c)/(u+v+w) is t; opposite signs mean a rearward hit.
                const int numerator = determinant(a, b, c).sign();
                if (!numerator)
                    return std::nullopt;
                if (numerator == u)
                    count += u;
            }
            continue;
        }
        stack.add(node.first + 1);
        stack.add(node.first);
    }
    return count;
}

}

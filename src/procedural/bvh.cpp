#include "procedural/bvh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace atelier::procedural {
namespace {
using math::Vector2;
using math::Vector3;

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr std::uint32_t leaf_size = 4;
constexpr std::uint32_t bins = 16;
// Ranges deeper than this split at the median, so a tree over fewer than 2^32 triangles is
// at most 72 levels deep and traversal fits a fixed stack.
constexpr std::uint32_t sah_depth = 40;
constexpr std::size_t stack_size = 128;

Vector3 lower(Vector3 a, Vector3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vector3 upper(Vector3 a, Vector3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Box {
    Vector3 minimum{infinity, infinity, infinity}, maximum{-infinity, -infinity, -infinity};
    void add(Vector3 point) { minimum = lower(minimum, point); maximum = upper(maximum, point); }
    void add(const Box& box) { minimum = lower(minimum, box.minimum); maximum = upper(maximum, box.maximum); }
    [[nodiscard]] float area() const {
        const auto d = maximum - minimum;
        return d.x < 0 ? 0 : d.x * d.y + d.y * d.z + d.z * d.x;
    }
};

// Ericson, Real-Time Collision Detection 5.1.5: (v, w) with q = a + v (b - a) + w (c - a).
Vector2 closest_on_triangle(Vector3 p, Vector3 a, Vector3 b, Vector3 c) {
    const auto ab = b - a, ac = c - a, ap = p - a;
    const float d1 = math::dot(ab, ap), d2 = math::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return {0, 0};
    const auto bp = p - b;
    const float d3 = math::dot(ab, bp), d4 = math::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return {1, 0};
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return {d1 / (d1 - d3), 0};
    const auto cp = p - c;
    const float d5 = math::dot(ab, cp), d6 = math::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return {0, 1};
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return {0, d2 / (d2 - d6)};
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {1 - w, w};
    }
    const float scale = 1 / (va + vb + vc);
    return {vb * scale, vc * scale};
}

}

Result<Bvh> Bvh::build(const Geometry& surface) {
    const auto primitives = surface.count(Layer::primitive);
    if (primitives && surface.uniform_primitive_size() != 3) return failure("A BVH needs triangle primitives.");
    const auto& positions = surface.positions();
    const auto corners = surface.vertex_points();
    Bvh result;
    std::vector<Triangle> triangles;
    triangles.reserve(primitives);
    for (std::uint32_t p = 0; p < primitives; ++p) {
        const Triangle triangle{positions[corners[3 * p]], positions[corners[3 * p + 1]], positions[corners[3 * p + 2]], p};
        // A zero-area triangle has no surface to hit or project onto.
        if (math::cross(triangle.b - triangle.a, triangle.c - triangle.a) != Vector3{}) triangles.push_back(triangle);
    }
    const auto count = static_cast<std::uint32_t>(triangles.size());
    if (!count) return result;
    std::vector<Box> bounds(count);
    std::vector<Vector3> centers(count);
    std::vector<std::uint32_t> order(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        bounds[i].add(triangles[i].a); bounds[i].add(triangles[i].b); bounds[i].add(triangles[i].c);
        centers[i] = (bounds[i].minimum + bounds[i].maximum) * 0.5f;
        order[i] = i;
    }
    struct Task { std::uint32_t node, begin, end, depth; };
    std::vector<Task> tasks{{0, 0, count, 0}};
    result.nodes_.reserve(2 * std::size_t{count} / leaf_size + 1);
    result.nodes_.push_back({});
    while (!tasks.empty()) {
        const auto [node, begin, end, depth] = tasks.back();
        tasks.pop_back();
        Box box, spread;
        for (auto i = begin; i < end; ++i) { box.add(bounds[order[i]]); spread.add(centers[order[i]]); }
        result.nodes_[node].minimum = box.minimum;
        result.nodes_[node].maximum = box.maximum;
        const auto leaf = [&] { result.nodes_[node].first = begin; result.nodes_[node].count = end - begin; };
        const auto extent = spread.maximum - spread.minimum;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        if (end - begin <= leaf_size || !(extent[axis] > 0)) { leaf(); continue; }
        auto middle = begin;
        if (depth < sah_depth) {
            const float scale = bins / extent[axis];
            const auto bin = [&](std::uint32_t i) {
                return std::min(bins - 1, static_cast<std::uint32_t>((centers[i][axis] - spread.minimum[axis]) * scale));
            };
            std::array<Box, bins> boxes;
            std::array<std::uint32_t, bins> counts{};
            for (auto i = begin; i < end; ++i) { const auto b = bin(order[i]); ++counts[b]; boxes[b].add(bounds[order[i]]); }
            std::array<float, bins> left{};
            Box sweep;
            std::uint32_t swept = 0;
            for (std::uint32_t b = 0; b + 1 < bins; ++b) { sweep.add(boxes[b]); swept += counts[b]; left[b] = sweep.area() * float(swept); }
            sweep = {};
            swept = 0;
            float best = infinity;
            std::uint32_t split = 1;
            for (std::uint32_t b = bins - 1; b > 0; --b) {
                sweep.add(boxes[b]);
                swept += counts[b];
                if (const float cost = left[b - 1] + sweep.area() * float(swept); cost < best) { best = cost; split = b; }
            }
            if (end - begin <= 4 * leaf_size && best >= box.area() * float(end - begin)) { leaf(); continue; }
            middle = static_cast<std::uint32_t>(std::partition(order.begin() + begin, order.begin() + end,
                [&](std::uint32_t i) { return bin(i) < split; }) - order.begin());
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
        tasks.push_back({child + 1, middle, end, depth + 1});
        tasks.push_back({child, begin, middle, depth + 1});
    }
    result.triangles_.reserve(count);
    for (const auto i : order) result.triangles_.push_back(triangles[i]);
    return result;
}

std::optional<Hit> Bvh::ray(Vector3 origin, Vector3 direction, float max_distance) const noexcept {
    if (nodes_.empty()) return std::nullopt;
    const Vector3 inverse{1 / direction.x, 1 / direction.y, 1 / direction.z};
    float best = max_distance;
    // Entry distance into a box, or infinity when the ray misses it within `best`. A NaN slab
    // (zero direction with the origin on its plane) compares false and leaves the range open.
    const auto enter = [&](const Node& node) {
        float near = 0, far = best;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            float a = (node.minimum[axis] - origin[axis]) * inverse[axis], b = (node.maximum[axis] - origin[axis]) * inverse[axis];
            if (a > b) std::swap(a, b);
            if (a > near) near = a;
            if (b < far) far = b;
        }
        return near <= far ? near : infinity;
    };
    std::optional<Hit> hit;
    std::array<std::pair<std::uint32_t, float>, stack_size> stack;
    std::size_t top = 0;
    if (const float entry = enter(nodes_[0]); entry != infinity) stack[top++] = {0, entry};
    while (top) {
        const auto [index, entry] = stack[--top];
        if (entry > best) continue;
        const auto& node = nodes_[index];
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto& triangle = triangles_[i];
                const auto e1 = triangle.b - triangle.a, e2 = triangle.c - triangle.a;
                const auto p = math::cross(direction, e2);
                const float determinant = math::dot(e1, p);
                if (determinant == 0) continue;
                const float scale = 1 / determinant;
                const auto s = origin - triangle.a;
                const float u = math::dot(s, p) * scale;
                if (u < 0 || u > 1) continue;
                const auto q = math::cross(s, e1);
                const float v = math::dot(direction, q) * scale;
                if (v < 0 || u + v > 1) continue;
                const float distance = math::dot(e2, q) * scale;
                if (distance < 0 || distance > best) continue;
                best = distance;
                hit = Hit{triangle.primitive, distance, {u, v}, triangle.a + e1 * u + e2 * v};
            }
            continue;
        }
        const float left = enter(nodes_[node.first]), right = enter(nodes_[node.first + 1]);
        const bool left_first = left <= right;
        const std::pair<std::uint32_t, float> near{left_first ? node.first : node.first + 1, left_first ? left : right};
        const std::pair<std::uint32_t, float> far{left_first ? node.first + 1 : node.first, left_first ? right : left};
        if (far.second != infinity) stack[top++] = far;
        if (near.second != infinity) stack[top++] = near;
    }
    return hit;
}

std::optional<Hit> Bvh::closest(Vector3 point, float max_distance) const noexcept {
    if (nodes_.empty()) return std::nullopt;
    float best = max_distance * max_distance;
    const auto distance = [&](const Node& node) {
        float sum = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float d = std::max({node.minimum[axis] - point[axis], 0.f, point[axis] - node.maximum[axis]});
            sum += d * d;
        }
        return sum;
    };
    std::optional<Hit> hit;
    std::array<std::pair<std::uint32_t, float>, stack_size> stack;
    std::size_t top = 0;
    stack[top++] = {0, distance(nodes_[0])};
    while (top) {
        const auto [index, gap] = stack[--top];
        if (gap > best) continue;
        const auto& node = nodes_[index];
        if (node.count) {
            for (auto i = node.first; i < node.first + node.count; ++i) {
                const auto& triangle = triangles_[i];
                const auto uv = closest_on_triangle(point, triangle.a, triangle.b, triangle.c);
                const auto position = triangle.a + (triangle.b - triangle.a) * uv.x + (triangle.c - triangle.a) * uv.y;
                const auto offset = position - point;
                if (const float squared = math::dot(offset, offset); squared <= best) {
                    best = squared;
                    hit = Hit{triangle.primitive, std::sqrt(squared), uv, position};
                }
            }
            continue;
        }
        const float left = distance(nodes_[node.first]), right = distance(nodes_[node.first + 1]);
        const bool left_first = left <= right;
        stack[top++] = {left_first ? node.first + 1 : node.first, left_first ? right : left};
        stack[top++] = {left_first ? node.first : node.first + 1, left_first ? left : right};
    }
    return hit;
}

}

#include "procedural/operations.hpp"
#include "foundation/result.hpp"

#include <cmath>
#include <numbers>

namespace atelier::procedural {
namespace {

using math::Vector3;

void append_face(Array<std::uint32_t>& corners, Array<std::uint32_t>& offsets,
                 std::initializer_list<std::uint32_t> face) {
    corners.append({face.begin(), face.size()});
    offsets.add(static_cast<std::uint32_t>(corners.size()));
}

Result<Geometry> surface(Array<Vector3> positions, Array<std::uint32_t> corners, const Array<std::uint32_t>& offsets,
                         Array<Vector3> normals) {
    ATELIER_TRY_BIND(result, Geometry::polygons(std::move(positions), std::move(corners), offsets));
    ATELIER_TRY(result->set(String::literal<"N">(), Layer::vertex, SharedArray<Vector3>{std::move(normals)}));
    return std::move(*result);
}

}

Result<Geometry> box(Vector3 size) {
    if (!math::is_finite(size) || size.x <= 0 || size.y <= 0 || size.z <= 0)
        return failure("Box dimensions must be finite and positive.");
    Array<Vector3> positions;
    positions.reserve(8);
    for (std::uint32_t i = 0; i < 8; ++i)
        positions.add({size.x * (i & 1 ? .5f : -.5f), size.y * (i & 2 ? .5f : -.5f), size.z * (i & 4 ? .5f : -.5f)});
    Array<std::uint32_t> corners{0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4, 2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    Array<Vector3> normals;
    normals.reserve(24);
    for (const auto n : {Vector3{-1, 0, 0}, Vector3{1, 0, 0}, Vector3{0, -1, 0}, Vector3{0, 1, 0}, Vector3{0, 0, -1},
                         Vector3{0, 0, 1}})
        for (unsigned i = 0; i < 4; ++i)
            normals.add(n);
    return surface(std::move(positions), std::move(corners), {0, 4, 8, 12, 16, 20, 24}, std::move(normals));
}

Result<Geometry> sphere(float radius, std::uint32_t segments, std::uint32_t latitudes) {
    if (!std::isfinite(radius) || radius <= 0 || segments < 3 || latitudes < 2)
        return failure("Sphere needs a finite positive radius, at least 3 segments and 2 latitude intervals.");
    // Divide before multiplying: even the caller's uint32 dimensions may overflow
    // uint64 when multiplied by four corners. All resulting counts fit Geometry.
    if (4ULL * latitudes - 2 > element_limit / segments)
        return failure("Sphere would create 2^32 - 1 corners or more.");
    Array<Vector3> positions;
    positions.reserve(2 + std::size_t{latitudes - 1} * segments);
    positions.add({0, radius, 0});
    for (std::uint32_t j = 1; j < latitudes; ++j) {
        const double theta = std::numbers::pi * j / latitudes;
        const double r = radius * std::sin(theta);
        const float y = static_cast<float>(radius * std::cos(theta));
        for (std::uint32_t i = 0; i < segments; ++i) {
            const double phi = 2 * std::numbers::pi * i / segments;
            positions.add({static_cast<float>(r * std::cos(phi)), y, static_cast<float>(r * std::sin(phi))});
        }
    }
    const auto south = static_cast<std::uint32_t>(positions.size());
    positions.add({0, -radius, 0});
    Array<std::uint32_t> corners;
    corners.reserve(std::size_t{segments} * (4 * latitudes - 2));
    Array<std::uint32_t> offsets{0};
    offsets.reserve(std::size_t{segments} * latitudes + 1);
    for (std::uint32_t i = 0; i < segments; ++i)
        append_face(corners, offsets, {0, 1 + (i + 1) % segments, 1 + i});
    for (std::uint32_t j = 0; j + 2 < latitudes; ++j) {
        const auto upper = 1 + j * segments;
        for (std::uint32_t i = 0; i < segments; ++i) {
            const auto next = (i + 1) % segments;
            append_face(corners, offsets, {upper + i, upper + next, upper + segments + next, upper + segments + i});
        }
    }
    const auto last = south - segments;
    for (std::uint32_t i = 0; i < segments; ++i)
        append_face(corners, offsets, {south, last + i, last + (i + 1) % segments});
    Array<Vector3> normals;
    normals.reserve(corners.size());
    for (const auto corner : corners) {
        const auto p = positions[corner];
        const auto n = *math::try_normalize(math::Vector3d{p.x, p.y, p.z});
        normals.add({static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
    }
    return surface(std::move(positions), std::move(corners), offsets, std::move(normals));
}

Result<Geometry> tube(float radius, float height, std::uint32_t segments) {
    if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(height) || height <= 0 || segments < 3)
        return failure("Tube needs finite positive radius and height and at least 3 segments.");
    if (6ULL * segments > element_limit)
        return failure("Tube would create 2^32 - 1 corners or more.");
    Array<Vector3> positions;
    positions.reserve(2 * std::size_t{segments});
    for (std::uint32_t i = 0; i < segments; ++i) {
        const double phi = 2 * std::numbers::pi * i / segments;
        positions.add(
            {static_cast<float>(radius * std::cos(phi)), -height / 2, static_cast<float>(radius * std::sin(phi))});
    }
    for (std::uint32_t i = 0; i < segments; ++i)
        positions.add({positions[i].x, height / 2, positions[i].z});
    Array<std::uint32_t> corners;
    corners.reserve(6 * std::size_t{segments});
    Array<std::uint32_t> offsets{0};
    offsets.reserve(std::size_t{segments} + 3);
    for (std::uint32_t i = 0; i < segments; ++i) {
        const auto next = (i + 1) % segments;
        append_face(corners, offsets, {i, segments + i, segments + next, next});
    }
    for (std::uint32_t i = 0; i < segments; ++i)
        corners.add(i);
    offsets.add(static_cast<std::uint32_t>(corners.size()));
    corners.add(segments);
    for (std::uint32_t i = segments - 1; i > 0; --i)
        corners.add(segments + i);
    offsets.add(static_cast<std::uint32_t>(corners.size()));
    Array<Vector3> normals;
    normals.reserve(corners.size());
    for (std::size_t v = 0; v < 4 * std::size_t{segments}; ++v) {
        const auto p = positions[corners[v]];
        const auto n = *math::try_normalize(math::Vector3d{p.x, 0, p.z});
        normals.add({static_cast<float>(n.x), 0, static_cast<float>(n.z)});
    }
    for (std::uint32_t i = 0; i < segments; ++i)
        normals.add({0, -1, 0});
    for (std::uint32_t i = 0; i < segments; ++i)
        normals.add({0, 1, 0});
    return surface(std::move(positions), std::move(corners), offsets, std::move(normals));
}

Result<void> group_box(Geometry& geometry, StringView group, Vector3 minimum, Vector3 maximum) {
    if (!math::is_finite(minimum) || !math::is_finite(maximum) || minimum.x > maximum.x || minimum.y > maximum.y ||
        minimum.z > maximum.z)
        return failure("GroupBox bounds must be finite and ordered on each axis.");
    ATELIER_TRY_BIND(selected, geometry.add<bool>(group, Layer::point));
    const auto& positions = geometry.positions();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto p = positions[i];
        (**selected)[i] = p.x >= minimum.x && p.x <= maximum.x && p.y >= minimum.y && p.y <= maximum.y &&
                          p.z >= minimum.z && p.z <= maximum.z;
    }
    return {};
}

}

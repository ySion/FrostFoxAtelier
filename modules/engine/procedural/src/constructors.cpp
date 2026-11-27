#include "procedural/operations.hpp"
#include "foundation/map.hpp"
#include "foundation/result.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace atelier::procedural {
namespace {

using math::Vector2;

using math::Vector3;

using math::Vector3d;

struct PlaneBasis {
    Vector3d u;

    Vector3d v;

    Vector3 normal;
};

// Right-handed basis with u x v = +normal. On vertical planes (xy, yz), +v is +Y (upright).
PlaneBasis plane_basis(OrientationPlane plane) noexcept {
    switch (plane) {
    case OrientationPlane::zx:
        return {{1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.f, 1.f, 0.f}};
    case OrientationPlane::xy:
        return {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.f, 0.f, 1.f}};
    case OrientationPlane::yz:
        return {{0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}, {1.f, 0.f, 0.f}};
    }
    return {{1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.f, 1.f, 0.f}};
}

bool valid_plane(OrientationPlane plane) noexcept {
    return plane == OrientationPlane::zx || plane == OrientationPlane::xy || plane == OrientationPlane::yz;
}

void append_face(Array<std::uint32_t>& corners, Array<std::uint32_t>& offsets,
                 std::initializer_list<std::uint32_t> face) {
    corners.append({face.begin(), face.size()});
    offsets.add(static_cast<std::uint32_t>(corners.size()));
}

Result<Geometry> surface(Array<Vector3> positions, Array<std::uint32_t> corners, const Array<std::uint32_t>& offsets,
                         Array<Vector3> normals, Array<Vector2> uvs) {
    ATELIER_TRY_BIND(result, Geometry::polygons(std::move(positions), std::move(corners), offsets));
    ATELIER_TRY(result->set(String::literal<"N">(), Layer::vertex, SharedArray<Vector3>{std::move(normals)}));
    ATELIER_TRY(result->set(String::literal<"uv">(), Layer::vertex, SharedArray<Vector2>{std::move(uvs)}));
    return std::move(*result);
}

}

Result<Geometry> grid(const GridSettings& settings) {
    if (!valid_plane(settings.orientation))
        return failure("Invalid Grid orientation plane.");
    if (!math::is_finite(settings.size) || !math::is_finite(settings.center))
        return failure("Grid size and center must be finite.");
    if (std::uint64_t{settings.rows} * settings.columns > element_limit)
        return failure("Grid point count must be below 2^32 - 1.");

    const auto at = [](double extent, std::uint32_t i, std::uint32_t count) noexcept {
        return count < 2 ? 0.0 : extent * (double(i) / (count - 1) - 0.5);
    };

    if (settings.primitive_type == GridPrimitive::points) {
        if (settings.size.x < 0.f || settings.size.y < 0.f)
            return failure("Grid size must be non-negative.");
        Array<Vector3> positions;
        positions.reserve(std::size_t{settings.rows} * settings.columns);
        const Vector3d c(settings.center);
        for (std::uint32_t row = 0; row < settings.rows; ++row) {
            const double dv = at(settings.size.y, row, settings.rows);
            for (std::uint32_t col = 0; col < settings.columns; ++col) {
                const double du = at(settings.size.x, col, settings.columns);
                Vector3d p = c;
                if (settings.orientation == OrientationPlane::zx) {
                    p.x += du;
                    p.z += dv;
                } else if (settings.orientation == OrientationPlane::xy) {
                    p.x += du;
                    p.y += dv;
                } else {
                    p.z += du;
                    p.y += dv;
                }
                if (!math::is_finite(Vector3(p)))
                    return failure("Grid point coordinates overflowed float range.");
                positions.add(Vector3(p));
            }
        }
        ATELIER_TRY_BIND(result, Geometry::points(std::move(positions)));
        const Vector3 n = plane_basis(settings.orientation).normal;
        ATELIER_TRY(result->add<Vector3>("N", Layer::point, n));
        return std::move(*result);
    }

    if (settings.primitive_type != GridPrimitive::quads && settings.primitive_type != GridPrimitive::triangles)
        return failure("Invalid Grid primitive_type.");
    if (settings.rows < 2 || settings.columns < 2)
        return failure("Surface Grid requires at least 2 rows and 2 columns.");
    if (settings.size.x <= 0.f || settings.size.y <= 0.f)
        return failure("Surface Grid requires positive size on both axes.");

    const std::uint64_t cells = std::uint64_t{settings.rows - 1} * (settings.columns - 1);
    const std::uint64_t corners_per_cell = settings.primitive_type == GridPrimitive::quads ? 4ULL : 6ULL;
    if (cells > element_limit / corners_per_cell)
        return failure("Grid would create 2^32 - 1 corners or more.");

    Array<Vector3> positions;
    positions.reserve(std::size_t{settings.rows} * settings.columns);
    const Vector3d c(settings.center);
    for (std::uint32_t row = 0; row < settings.rows; ++row) {
        const double dv = at(settings.size.y, row, settings.rows);
        for (std::uint32_t col = 0; col < settings.columns; ++col) {
            const double du = at(settings.size.x, col, settings.columns);
            Vector3d p = c;
            if (settings.orientation == OrientationPlane::zx) {
                p.x += du;
                p.z += dv;
            } else if (settings.orientation == OrientationPlane::xy) {
                p.x += du;
                p.y += dv;
            } else {
                p.z += du;
                p.y += dv;
            }
            if (!math::is_finite(Vector3(p)))
                return failure("Grid point coordinates overflowed float range.");
            positions.add(Vector3(p));
        }
    }

    const Vector3 face_n = plane_basis(settings.orientation).normal;
    const bool flip_quad = (settings.orientation != OrientationPlane::xy);
    Array<std::uint32_t> corners;
    corners.reserve(static_cast<std::size_t>(cells * corners_per_cell));
    Array<std::uint32_t> offsets{0};
    offsets.reserve(static_cast<std::size_t>(cells * (settings.primitive_type == GridPrimitive::quads ? 1ULL : 2ULL)) +
                    1);
    Array<Vector3> normals;
    normals.reserve(corners.capacity());
    Array<Vector2> uvs;
    uvs.reserve(corners.capacity());

    for (std::uint32_t row = 0; row + 1 < settings.rows; ++row) {
        const float v0 = static_cast<float>(double(row) / (settings.rows - 1));
        const float v1 = static_cast<float>(double(row + 1) / (settings.rows - 1));
        for (std::uint32_t col = 0; col + 1 < settings.columns; ++col) {
            const float u0 = static_cast<float>(double(col) / (settings.columns - 1));
            const float u1 = static_cast<float>(double(col + 1) / (settings.columns - 1));
            const std::uint32_t p00 = row * settings.columns + col;
            const std::uint32_t p10 = row * settings.columns + (col + 1);
            const std::uint32_t p11 = (row + 1) * settings.columns + (col + 1);
            const std::uint32_t p01 = (row + 1) * settings.columns + col;
            const std::array<std::uint32_t, 4> q =
                flip_quad ? std::array{p00, p01, p11, p10} : std::array{p00, p10, p11, p01};
            const std::array<Vector2, 4> q_uv =
                flip_quad ? std::array{Vector2{u0, v0}, Vector2{u0, v1}, Vector2{u1, v1}, Vector2{u1, v0}}
                          : std::array{Vector2{u0, v0}, Vector2{u1, v0}, Vector2{u1, v1}, Vector2{u0, v1}};
            if (settings.primitive_type == GridPrimitive::quads) {
                append_face(corners, offsets, {q[0], q[1], q[2], q[3]});
                for (unsigned k = 0; k < 4; ++k) {
                    normals.add(face_n);
                    uvs.add(q_uv[k]);
                }
            } else {
                append_face(corners, offsets, {q[0], q[1], q[2]});
                append_face(corners, offsets, {q[0], q[2], q[3]});
                for (const unsigned idx : {0u, 1u, 2u, 0u, 2u, 3u}) {
                    normals.add(face_n);
                    uvs.add(q_uv[idx]);
                }
            }
        }
    }
    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<Geometry> grid(Vector2 size, std::uint32_t rows, std::uint32_t columns) {
    return grid(GridSettings{.primitive_type = GridPrimitive::points,
                             .orientation = OrientationPlane::zx,
                             .size = size,
                             .rows = rows,
                             .columns = columns});
}

Result<Geometry> box_primitive(const BoxSettings& settings) {
    if (!math::is_finite(settings.size) || !math::is_finite(settings.center) || settings.size.x <= 0.f ||
        settings.size.y <= 0.f || settings.size.z <= 0.f)
        return failure("Box dimensions must be finite and positive, and center must be finite.");
    if (settings.divisions_x < 1 || settings.divisions_y < 1 || settings.divisions_z < 1)
        return failure("Box divisions must be at least 1 on each axis.");

    const std::uint64_t nx = settings.divisions_x;
    const std::uint64_t ny = settings.divisions_y;
    const std::uint64_t nz = settings.divisions_z;
    if (nx > element_limit / ny || nz > element_limit / nx || nz > element_limit / ny)
        return failure("Box subdivisions exceed Geometry limits.");
    const std::uint64_t face_pairs = ny * nz + nx * nz + nx * ny;
    if (face_pairs > element_limit / 8ULL)
        return failure("Box would create 2^32 - 1 corners or more.");

    const auto x_at = [&](std::uint32_t i) noexcept {
        return static_cast<float>(double{settings.center.x} + double{settings.size.x} * (double(i) / nx - 0.5));
    };
    const auto y_at = [&](std::uint32_t i) noexcept {
        const double t = settings.base_on_ground ? double(i) / ny : (double(i) / ny - 0.5);
        return static_cast<float>(double{settings.center.y} + double{settings.size.y} * t);
    };
    const auto z_at = [&](std::uint32_t i) noexcept {
        return static_cast<float>(double{settings.center.z} + double{settings.size.z} * (double(i) / nz - 0.5));
    };

    if (nx == 1 && ny == 1 && nz == 1) {
        Array<Vector3> positions;
        positions.reserve(8);
        for (std::uint32_t i = 0; i < 8; ++i) {
            const Vector3 p{x_at(i & 1 ? 1u : 0u), y_at(i & 2 ? 1u : 0u), z_at(i & 4 ? 1u : 0u)};
            if (!math::is_finite(p))
                return failure("Box coordinates overflowed float range.");
            positions.add(p);
        }
        Array<std::uint32_t> corners{0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4, 2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
        Array<Vector3> normals;
        normals.reserve(24);
        for (const auto n : {Vector3{-1, 0, 0}, Vector3{1, 0, 0}, Vector3{0, -1, 0}, Vector3{0, 1, 0},
                             Vector3{0, 0, -1}, Vector3{0, 0, 1}})
            for (unsigned i = 0; i < 4; ++i)
                normals.add(n);
        Array<Vector2> uvs{
            {0, 0}, {1, 0}, {1, 1}, {0, 1}, // -X
            {0, 0}, {0, 1}, {1, 1}, {1, 0}, // +X
            {0, 0}, {1, 0}, {1, 1}, {0, 1}, // -Y
            {0, 0}, {0, 1}, {1, 1}, {1, 0}, // +Y
            {0, 0}, {0, 1}, {1, 1}, {1, 0}, // -Z
            {0, 0}, {1, 0}, {1, 1}, {0, 1}, // +Z
        };
        return surface(std::move(positions), std::move(corners), {0, 4, 8, 12, 16, 20, 24}, std::move(normals),
                       std::move(uvs));
    }

    struct GridKey {
        std::uint32_t x;

        std::uint32_t y;

        std::uint32_t z;

        bool operator==(const GridKey&) const noexcept = default;
    };
    struct GridKeyHash {
        std::size_t operator()(const GridKey& k) const noexcept {
            std::uint64_t h = (std::uint64_t{k.x} * 0x9e3779b97f4a7c15ULL) ^
                              (std::uint64_t{k.y} * 0xbf58476d1ce4e5b9ULL) ^
                              (std::uint64_t{k.z} * 0x94d049bb133111ebULL);
            return static_cast<std::size_t>(h ^ (h >> 32));
        }
    };

    const std::size_t total_quads = static_cast<std::size_t>(2ULL * face_pairs);
    Array<Vector3> positions;
    Map<GridKey, std::uint32_t, GridKeyHash> point_map;
    bool coord_overflow = false;
    const auto point_at = [&](std::uint32_t ix, std::uint32_t iy, std::uint32_t iz) -> std::uint32_t {
        const auto [ptr, inserted] =
            point_map.try_emplace(GridKey{ix, iy, iz}, static_cast<std::uint32_t>(positions.size()));
        if (inserted) {
            const Vector3 p{x_at(ix), y_at(iy), z_at(iz)};
            if (!math::is_finite(p))
                coord_overflow = true;
            positions.add(p);
        }
        return *ptr;
    };

    Array<std::uint32_t> corners;
    corners.reserve(total_quads * 4);
    Array<std::uint32_t> offsets{0};
    offsets.reserve(total_quads + 1);
    Array<Vector3> normals;
    normals.reserve(total_quads * 4);
    Array<Vector2> uvs;
    uvs.reserve(total_quads * 4);

    const auto emit_quad = [&](std::array<std::uint32_t, 4> q, Vector3 n, std::array<Vector2, 4> q_uv) {
        append_face(corners, offsets, {q[0], q[1], q[2], q[3]});
        for (unsigned k = 0; k < 4; ++k) {
            normals.add(n);
            uvs.add(q_uv[k]);
        }
    };

    // -X and +X faces
    for (const bool pos_x : {false, true}) {
        const std::uint32_t ix = pos_x ? settings.divisions_x : 0u;
        const Vector3 n = pos_x ? Vector3{1, 0, 0} : Vector3{-1, 0, 0};
        for (std::uint32_t iy = 0; iy < settings.divisions_y; ++iy) {
            const float v0 = static_cast<float>(double(iy) / ny);
            const float v1 = static_cast<float>(double(iy + 1) / ny);
            for (std::uint32_t iz = 0; iz < settings.divisions_z; ++iz) {
                const float u0 = static_cast<float>(double(iz) / nz);
                const float u1 = static_cast<float>(double(iz + 1) / nz);
                if (!pos_x)
                    emit_quad({point_at(ix, iy, iz), point_at(ix, iy, iz + 1), point_at(ix, iy + 1, iz + 1),
                               point_at(ix, iy + 1, iz)},
                              n, {Vector2{u0, v0}, Vector2{u1, v0}, Vector2{u1, v1}, Vector2{u0, v1}});
                else
                    emit_quad({point_at(ix, iy, iz), point_at(ix, iy + 1, iz), point_at(ix, iy + 1, iz + 1),
                               point_at(ix, iy, iz + 1)},
                              n, {Vector2{u0, v0}, Vector2{u0, v1}, Vector2{u1, v1}, Vector2{u1, v0}});
            }
        }
    }
    // -Y and +Y faces
    for (const bool pos_y : {false, true}) {
        const std::uint32_t iy = pos_y ? settings.divisions_y : 0u;
        const Vector3 n = pos_y ? Vector3{0, 1, 0} : Vector3{0, -1, 0};
        for (std::uint32_t iz = 0; iz < settings.divisions_z; ++iz) {
            const float v0 = static_cast<float>(double(iz) / nz);
            const float v1 = static_cast<float>(double(iz + 1) / nz);
            for (std::uint32_t ix = 0; ix < settings.divisions_x; ++ix) {
                const float u0 = static_cast<float>(double(ix) / nx);
                const float u1 = static_cast<float>(double(ix + 1) / nx);
                if (!pos_y)
                    emit_quad({point_at(ix, iy, iz), point_at(ix + 1, iy, iz), point_at(ix + 1, iy, iz + 1),
                               point_at(ix, iy, iz + 1)},
                              n, {Vector2{u0, v0}, Vector2{u1, v0}, Vector2{u1, v1}, Vector2{u0, v1}});
                else
                    emit_quad({point_at(ix, iy, iz), point_at(ix, iy, iz + 1), point_at(ix + 1, iy, iz + 1),
                               point_at(ix + 1, iy, iz)},
                              n, {Vector2{u0, v0}, Vector2{u0, v1}, Vector2{u1, v1}, Vector2{u1, v0}});
            }
        }
    }
    // -Z and +Z faces
    for (const bool pos_z : {false, true}) {
        const std::uint32_t iz = pos_z ? settings.divisions_z : 0u;
        const Vector3 n = pos_z ? Vector3{0, 0, 1} : Vector3{0, 0, -1};
        for (std::uint32_t iy = 0; iy < settings.divisions_y; ++iy) {
            const float v0 = static_cast<float>(double(iy) / ny);
            const float v1 = static_cast<float>(double(iy + 1) / ny);
            for (std::uint32_t ix = 0; ix < settings.divisions_x; ++ix) {
                const float u0 = static_cast<float>(double(ix) / nx);
                const float u1 = static_cast<float>(double(ix + 1) / nx);
                if (!pos_z)
                    emit_quad({point_at(ix, iy, iz), point_at(ix, iy + 1, iz), point_at(ix + 1, iy + 1, iz),
                               point_at(ix + 1, iy, iz)},
                              n, {Vector2{u0, v0}, Vector2{u0, v1}, Vector2{u1, v1}, Vector2{u1, v0}});
                else
                    emit_quad({point_at(ix, iy, iz), point_at(ix + 1, iy, iz), point_at(ix + 1, iy + 1, iz),
                               point_at(ix, iy + 1, iz)},
                              n, {Vector2{u0, v0}, Vector2{u1, v0}, Vector2{u1, v1}, Vector2{u0, v1}});
            }
        }
    }
    if (coord_overflow)
        return failure("Box coordinates overflowed float range.");
    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<Geometry> box(Vector3 size) {
    return box_primitive(BoxSettings{.size = size});
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
    Array<Vector2> uvs;
    uvs.reserve(corners.capacity());
    const auto u_at = [&](std::uint32_t i) noexcept { return static_cast<float>(double(i) / segments); };
    const auto v_at = [&](std::uint32_t j) noexcept { return static_cast<float>(1.0 - double(j) / latitudes); };
    for (std::uint32_t i = 0; i < segments; ++i) {
        append_face(corners, offsets, {0, 1 + (i + 1) % segments, 1 + i});
        uvs.add({static_cast<float>((double(i) + 0.5) / segments), 1.f});
        uvs.add({u_at(i + 1), v_at(1)});
        uvs.add({u_at(i), v_at(1)});
    }
    for (std::uint32_t j = 0; j + 2 < latitudes; ++j) {
        const auto upper = 1 + j * segments;
        const float v_hi = v_at(j + 1);
        const float v_lo = v_at(j + 2);
        for (std::uint32_t i = 0; i < segments; ++i) {
            const auto next = (i + 1) % segments;
            append_face(corners, offsets, {upper + i, upper + next, upper + segments + next, upper + segments + i});
            uvs.add({u_at(i), v_hi});
            uvs.add({u_at(i + 1), v_hi});
            uvs.add({u_at(i + 1), v_lo});
            uvs.add({u_at(i), v_lo});
        }
    }
    const auto last = south - segments;
    for (std::uint32_t i = 0; i < segments; ++i) {
        append_face(corners, offsets, {south, last + i, last + (i + 1) % segments});
        uvs.add({static_cast<float>((double(i) + 0.5) / segments), 0.f});
        uvs.add({u_at(i), v_at(latitudes - 1)});
        uvs.add({u_at(i + 1), v_at(latitudes - 1)});
    }
    Array<Vector3> normals;
    normals.reserve(corners.size());
    for (const auto corner : corners) {
        const auto p = positions[corner];
        const auto n = *math::try_normalize(math::Vector3d(p));
        normals.add(Vector3(n));
    }
    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<Geometry> tube(const TubeSettings& settings) {
    if (!std::isfinite(settings.radius_top) || !std::isfinite(settings.radius_bottom) ||
        !std::isfinite(settings.inner_radius) || !std::isfinite(settings.height) || !math::is_finite(settings.center) ||
        !std::isfinite(settings.arc_degrees))
        return failure("Tube dimensions, center and arc angle must be finite.");
    if (settings.radius_top < 0.f || settings.radius_bottom < 0.f ||
        std::max(settings.radius_top, settings.radius_bottom) <= 0.f || settings.height <= 0.f ||
        settings.segments < 3 || settings.rows < 1)
        return failure("Tube needs non-negative radii (not both zero), positive height, segments >= 3 and rows >= 1.");
    if (settings.inner_radius < 0.f)
        return failure("Tube inner_radius must be non-negative.");
    if (settings.inner_radius > 0.f &&
        (settings.radius_top <= settings.inner_radius || settings.radius_bottom <= settings.inner_radius))
        return failure("Hollow Tube requires both radius_top and radius_bottom to exceed inner_radius.");
    if (settings.arc_degrees <= 0.f || settings.arc_degrees > 360.f)
        return failure("Tube arc_degrees must be in (0, 360].");

    const bool full_circle = settings.arc_degrees >= 360.f;
    const double total_rad =
        full_circle ? 2.0 * std::numbers::pi : double{settings.arc_degrees} * (std::numbers::pi / 180.0);
    const std::uint32_t ring_pts = full_circle ? settings.segments : (settings.segments + 1);
    const bool hollow = settings.inner_radius > 0.f;

    // Upper-bound corner count before reserving:
    const std::uint64_t row_quads = std::uint64_t{settings.segments} * settings.rows * (hollow ? 2ULL : 1ULL);
    if (row_quads > element_limit / 8ULL || std::uint64_t{ring_pts} * (settings.rows + 1ULL) > element_limit / 4ULL)
        return failure("Tube would create 2^32 - 1 corners or more.");

    const auto y_at = [&](std::uint32_t k) noexcept {
        const double t = double(k) / settings.rows;
        const double rel = settings.base_on_ground ? t : (t - 0.5);
        return static_cast<float>(double{settings.center.y} + double{settings.height} * rel);
    };
    const auto r_at = [&](std::uint32_t k) noexcept {
        const double t = double(k) / settings.rows;
        return double{settings.radius_bottom} + (double{settings.radius_top} - double{settings.radius_bottom}) * t;
    };

    Array<Vector3> positions;
    Array<std::uint32_t> outer_base;
    outer_base.resize(settings.rows + 1, UINT32_MAX);
    Array<std::uint32_t> inner_base;
    inner_base.resize(settings.rows + 1, UINT32_MAX);
    std::uint32_t bottom_apex = UINT32_MAX;
    std::uint32_t top_apex = UINT32_MAX;

    for (std::uint32_t k = 0; k <= settings.rows; ++k) {
        const float y = y_at(k);
        const double r = r_at(k);
        if (k == 0 && settings.radius_bottom == 0.f) {
            bottom_apex = static_cast<std::uint32_t>(positions.size());
            positions.add({settings.center.x, y, settings.center.z});
        } else if (k == settings.rows && settings.radius_top == 0.f) {
            top_apex = static_cast<std::uint32_t>(positions.size());
            positions.add({settings.center.x, y, settings.center.z});
        } else {
            outer_base[k] = static_cast<std::uint32_t>(positions.size());
            for (std::uint32_t i = 0; i < ring_pts; ++i) {
                const double phi = total_rad * double(i) / settings.segments;
                const Vector3 p{static_cast<float>(double{settings.center.x} + r * std::cos(phi)), y,
                                static_cast<float>(double{settings.center.z} + r * std::sin(phi))};
                if (!math::is_finite(p))
                    return failure("Tube coordinates overflowed float range.");
                positions.add(p);
            }
        }
    }
    if (hollow) {
        for (std::uint32_t k = 0; k <= settings.rows; ++k) {
            const float y = y_at(k);
            inner_base[k] = static_cast<std::uint32_t>(positions.size());
            for (std::uint32_t i = 0; i < ring_pts; ++i) {
                const double phi = total_rad * double(i) / settings.segments;
                const Vector3 p{
                    static_cast<float>(double{settings.center.x} + double{settings.inner_radius} * std::cos(phi)), y,
                    static_cast<float>(double{settings.center.z} + double{settings.inner_radius} * std::sin(phi))};
                if (!math::is_finite(p))
                    return failure("Tube coordinates overflowed float range.");
                positions.add(p);
            }
        }
    }

    Array<std::uint32_t> axis_pts;
    if (!hollow && !full_circle && settings.end_caps) {
        axis_pts.resize(settings.rows + 1, UINT32_MAX);
        for (std::uint32_t k = 0; k <= settings.rows; ++k) {
            if (k == 0 && bottom_apex != UINT32_MAX)
                axis_pts[k] = bottom_apex;
            else if (k == settings.rows && top_apex != UINT32_MAX)
                axis_pts[k] = top_apex;
            else {
                axis_pts[k] = static_cast<std::uint32_t>(positions.size());
                positions.add({settings.center.x, y_at(k), settings.center.z});
            }
        }
    }

    const auto outer_pt = [&](std::uint32_t k, std::uint32_t i) noexcept {
        return outer_base[k] + (full_circle ? (i % settings.segments) : i);
    };
    const auto inner_pt = [&](std::uint32_t k, std::uint32_t i) noexcept {
        return inner_base[k] + (full_circle ? (i % settings.segments) : i);
    };
    const double dr = double{settings.radius_bottom} - double{settings.radius_top};
    const auto outer_normal = [&](std::uint32_t i) noexcept -> Vector3 {
        const double phi = total_rad * double(full_circle ? (i % settings.segments) : i) / settings.segments;
        const Vector3d raw{double{settings.height} * std::cos(phi), dr, double{settings.height} * std::sin(phi)};
        return Vector3(*math::try_normalize(raw));
    };
    const auto inner_normal = [&](std::uint32_t i) noexcept -> Vector3 {
        const double phi = total_rad * double(full_circle ? (i % settings.segments) : i) / settings.segments;
        return {static_cast<float>(-std::cos(phi)), 0.f, static_cast<float>(-std::sin(phi))};
    };

    Array<std::uint32_t> corners;
    Array<std::uint32_t> offsets{0};
    Array<Vector3> normals;
    Array<Vector2> uvs;

    // Outer side wall
    for (std::uint32_t k = 0; k < settings.rows; ++k) {
        const float v0 = static_cast<float>(double(k) / settings.rows);
        const float v1 = static_cast<float>(double(k + 1) / settings.rows);
        for (std::uint32_t i = 0; i < settings.segments; ++i) {
            const float u0 = static_cast<float>(double(i) / settings.segments);
            const float u1 = static_cast<float>(double(i + 1) / settings.segments);
            const Vector3 n0 = outer_normal(i);
            const Vector3 n1 = outer_normal(i + 1);
            if (k == 0 && bottom_apex != UINT32_MAX) {
                append_face(corners, offsets, {bottom_apex, outer_pt(1, i), outer_pt(1, i + 1)});
                const double mid_phi = total_rad * (double(i) + 0.5) / settings.segments;
                const Vector3 n_apex = Vector3(*math::try_normalize(Vector3d{
                    double{settings.height} * std::cos(mid_phi), dr, double{settings.height} * std::sin(mid_phi)}));
                normals.add(n_apex);
                normals.add(n0);
                normals.add(n1);
                uvs.add({0.5f * (u0 + u1), v0});
                uvs.add({u0, v1});
                uvs.add({u1, v1});
            } else if (k + 1 == settings.rows && top_apex != UINT32_MAX) {
                append_face(corners, offsets, {outer_pt(k, i), top_apex, outer_pt(k, i + 1)});
                const double mid_phi = total_rad * (double(i) + 0.5) / settings.segments;
                const Vector3 n_apex = Vector3(*math::try_normalize(Vector3d{
                    double{settings.height} * std::cos(mid_phi), dr, double{settings.height} * std::sin(mid_phi)}));
                normals.add(n0);
                normals.add(n_apex);
                normals.add(n1);
                uvs.add({u0, v0});
                uvs.add({0.5f * (u0 + u1), v1});
                uvs.add({u1, v0});
            } else {
                append_face(corners, offsets,
                            {outer_pt(k, i), outer_pt(k + 1, i), outer_pt(k + 1, i + 1), outer_pt(k, i + 1)});
                normals.add(n0);
                normals.add(n0);
                normals.add(n1);
                normals.add(n1);
                uvs.add({u0, v0});
                uvs.add({u0, v1});
                uvs.add({u1, v1});
                uvs.add({u1, v0});
            }
        }
    }

    // Inner side wall (for hollow pipes/rings)
    if (hollow) {
        for (std::uint32_t k = 0; k < settings.rows; ++k) {
            const float v0 = static_cast<float>(double(k) / settings.rows);
            const float v1 = static_cast<float>(double(k + 1) / settings.rows);
            for (std::uint32_t i = 0; i < settings.segments; ++i) {
                const float u0 = static_cast<float>(double(i) / settings.segments);
                const float u1 = static_cast<float>(double(i + 1) / settings.segments);
                const Vector3 n0 = inner_normal(i);
                const Vector3 n1 = inner_normal(i + 1);
                append_face(corners, offsets,
                            {inner_pt(k, i + 1), inner_pt(k + 1, i + 1), inner_pt(k + 1, i), inner_pt(k, i)});
                normals.add(n1);
                normals.add(n1);
                normals.add(n0);
                normals.add(n0);
                uvs.add({u1, v0});
                uvs.add({u1, v1});
                uvs.add({u0, v1});
                uvs.add({u0, v0});
            }
        }
    }

    // End caps
    if (settings.end_caps) {
        const auto polar_uv = [&](double r_ratio, std::uint32_t i) noexcept -> Vector2 {
            const double phi = total_rad * double(full_circle ? (i % settings.segments) : i) / settings.segments;
            return {static_cast<float>(0.5 + 0.5 * r_ratio * std::cos(phi)),
                    static_cast<float>(0.5 + 0.5 * r_ratio * std::sin(phi))};
        };

        if (bottom_apex == UINT32_MAX) {
            if (!hollow && full_circle) {
                for (std::uint32_t i = 0; i < settings.segments; ++i) {
                    corners.add(outer_pt(0, i));
                    normals.add({0.f, -1.f, 0.f});
                    uvs.add(polar_uv(1.0, i));
                }
                offsets.add(static_cast<std::uint32_t>(corners.size()));
            } else if (!hollow) {
                for (std::uint32_t i = 0; i < settings.segments; ++i) {
                    append_face(corners, offsets, {axis_pts[0], outer_pt(0, i), outer_pt(0, i + 1)});
                    normals.add({0.f, -1.f, 0.f});
                    normals.add({0.f, -1.f, 0.f});
                    normals.add({0.f, -1.f, 0.f});
                    uvs.add({0.5f, 0.5f});
                    uvs.add(polar_uv(1.0, i));
                    uvs.add(polar_uv(1.0, i + 1));
                }
            } else {
                const double ir_ratio = double{settings.inner_radius} / double{settings.radius_bottom};
                for (std::uint32_t i = 0; i < settings.segments; ++i) {
                    append_face(corners, offsets,
                                {inner_pt(0, i), outer_pt(0, i), outer_pt(0, i + 1), inner_pt(0, i + 1)});
                    for (unsigned c = 0; c < 4; ++c)
                        normals.add({0.f, -1.f, 0.f});
                    uvs.add(polar_uv(ir_ratio, i));
                    uvs.add(polar_uv(1.0, i));
                    uvs.add(polar_uv(1.0, i + 1));
                    uvs.add(polar_uv(ir_ratio, i + 1));
                }
            }
        }

        if (top_apex == UINT32_MAX) {
            const std::uint32_t top = settings.rows;
            if (!hollow && full_circle) {
                corners.add(outer_pt(top, 0));
                normals.add({0.f, 1.f, 0.f});
                uvs.add(polar_uv(1.0, 0));
                for (std::uint32_t i = settings.segments - 1; i > 0; --i) {
                    corners.add(outer_pt(top, i));
                    normals.add({0.f, 1.f, 0.f});
                    uvs.add(polar_uv(1.0, i));
                }
                offsets.add(static_cast<std::uint32_t>(corners.size()));
            } else if (!hollow) {
                for (std::uint32_t i = 0; i < settings.segments; ++i) {
                    append_face(corners, offsets, {axis_pts[top], outer_pt(top, i + 1), outer_pt(top, i)});
                    normals.add({0.f, 1.f, 0.f});
                    normals.add({0.f, 1.f, 0.f});
                    normals.add({0.f, 1.f, 0.f});
                    uvs.add({0.5f, 0.5f});
                    uvs.add(polar_uv(1.0, i + 1));
                    uvs.add(polar_uv(1.0, i));
                }
            } else {
                const double ir_ratio = double{settings.inner_radius} / double{settings.radius_top};
                for (std::uint32_t i = 0; i < settings.segments; ++i) {
                    append_face(corners, offsets,
                                {inner_pt(top, i + 1), outer_pt(top, i + 1), outer_pt(top, i), inner_pt(top, i)});
                    for (unsigned c = 0; c < 4; ++c)
                        normals.add({0.f, 1.f, 0.f});
                    uvs.add(polar_uv(ir_ratio, i + 1));
                    uvs.add(polar_uv(1.0, i + 1));
                    uvs.add(polar_uv(1.0, i));
                    uvs.add(polar_uv(ir_ratio, i));
                }
            }
        }

        // Radial slice caps when arc_degrees < 360
        if (!full_circle) {
            const Vector3 n_start{0.f, 0.f, -1.f};
            const Vector3 n_end{static_cast<float>(-std::sin(total_rad)), 0.f, static_cast<float>(std::cos(total_rad))};
            for (std::uint32_t k = 0; k < settings.rows; ++k) {
                const float v0 = static_cast<float>(double(k) / settings.rows);
                const float v1 = static_cast<float>(double(k + 1) / settings.rows);
                const std::uint32_t in0_lo = hollow ? inner_pt(k, 0) : axis_pts[k];
                const std::uint32_t in0_hi = hollow ? inner_pt(k + 1, 0) : axis_pts[k + 1];
                const std::uint32_t in1_lo = hollow ? inner_pt(k, settings.segments) : axis_pts[k];
                const std::uint32_t in1_hi = hollow ? inner_pt(k + 1, settings.segments) : axis_pts[k + 1];

                if (k == 0 && bottom_apex != UINT32_MAX) {
                    append_face(corners, offsets, {bottom_apex, outer_pt(1, 0), in0_hi});
                    for (unsigned c = 0; c < 3; ++c)
                        normals.add(n_start);
                    uvs.add({0.f, v0});
                    uvs.add({1.f, v1});
                    uvs.add({0.f, v1});

                    append_face(corners, offsets, {bottom_apex, in1_hi, outer_pt(1, settings.segments)});
                    for (unsigned c = 0; c < 3; ++c)
                        normals.add(n_end);
                    uvs.add({0.f, v0});
                    uvs.add({0.f, v1});
                    uvs.add({1.f, v1});
                } else if (k + 1 == settings.rows && top_apex != UINT32_MAX) {
                    append_face(corners, offsets, {in0_lo, top_apex, outer_pt(k, 0)});
                    for (unsigned c = 0; c < 3; ++c)
                        normals.add(n_start);
                    uvs.add({0.f, v0});
                    uvs.add({0.f, v1});
                    uvs.add({1.f, v0});

                    append_face(corners, offsets, {outer_pt(k, settings.segments), top_apex, in1_lo});
                    for (unsigned c = 0; c < 3; ++c)
                        normals.add(n_end);
                    uvs.add({1.f, v0});
                    uvs.add({0.f, v1});
                    uvs.add({0.f, v0});
                } else {
                    append_face(corners, offsets, {in0_lo, in0_hi, outer_pt(k + 1, 0), outer_pt(k, 0)});
                    for (unsigned c = 0; c < 4; ++c)
                        normals.add(n_start);
                    uvs.add({0.f, v0});
                    uvs.add({0.f, v1});
                    uvs.add({1.f, v1});
                    uvs.add({1.f, v0});

                    append_face(corners, offsets,
                                {outer_pt(k, settings.segments), outer_pt(k + 1, settings.segments), in1_hi, in1_lo});
                    for (unsigned c = 0; c < 4; ++c)
                        normals.add(n_end);
                    uvs.add({1.f, v0});
                    uvs.add({1.f, v1});
                    uvs.add({0.f, v1});
                    uvs.add({0.f, v0});
                }
            }
        }
    }

    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<Geometry> tube(float radius, float height, std::uint32_t segments) {
    return tube(TubeSettings{.radius_top = radius, .radius_bottom = radius, .height = height, .segments = segments});
}

Result<Geometry> wedge(const WedgeSettings& settings) {
    if (!math::is_finite(settings.size) || !math::is_finite(settings.center) || !std::isfinite(settings.peak_ratio))
        return failure("Wedge size, center and peak_ratio must be finite.");
    if (settings.size.x <= 0.f || settings.size.y <= 0.f || settings.size.z <= 0.f)
        return failure("Wedge size must be positive on all axes.");
    if (settings.peak_ratio < 0.f || settings.peak_ratio > 1.f)
        return failure("Wedge peak_ratio must be in [0, 1].");
    if (settings.divisions_z < 1)
        return failure("Wedge divisions_z must be at least 1.");
    if (12ULL * settings.divisions_z + 6ULL > element_limit)
        return failure("Wedge would create 2^32 - 1 corners or more.");

    const double x0 = double{settings.center.x} - 0.5 * double{settings.size.x};
    const double x1 = double{settings.center.x} + 0.5 * double{settings.size.x};
    const double xr = double{settings.center.x} + (double{settings.peak_ratio} - 0.5) * double{settings.size.x};
    const double y0 = settings.base_on_ground ? double{settings.center.y}
                                              : (double{settings.center.y} - 0.5 * double{settings.size.y});
    const double y1 = y0 + double{settings.size.y};
    const std::uint32_t nz = settings.divisions_z;

    Array<Vector3> positions;
    positions.reserve(3 * std::size_t{nz + 1});
    for (std::uint32_t k = 0; k <= nz; ++k) {
        const double z = double{settings.center.z} + double{settings.size.z} * (double(k) / nz - 0.5);
        const Vector3 a{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z)};
        const Vector3 b{static_cast<float>(x1), static_cast<float>(y0), static_cast<float>(z)};
        const Vector3 c{static_cast<float>(xr), static_cast<float>(y1), static_cast<float>(z)};
        if (!math::is_finite(a) || !math::is_finite(b) || !math::is_finite(c))
            return failure("Wedge coordinates overflowed float range.");
        positions.add(a);
        positions.add(b);
        positions.add(c);
    }

    const Vector3 n_bottom{0.f, -1.f, 0.f};
    const Vector3 n_right = Vector3(*math::try_normalize(Vector3d{double{settings.size.y}, x1 - xr, 0.0}));
    const Vector3 n_left = Vector3(*math::try_normalize(Vector3d{-double{settings.size.y}, xr - x0, 0.0}));

    Array<std::uint32_t> corners;
    corners.reserve(12 * std::size_t{nz} + 6);
    Array<std::uint32_t> offsets{0};
    offsets.reserve(3 * std::size_t{nz} + 3);
    Array<Vector3> normals;
    normals.reserve(corners.capacity());
    Array<Vector2> uvs;
    uvs.reserve(corners.capacity());

    const auto emit_quad = [&](std::array<std::uint32_t, 4> q, Vector3 n, std::array<Vector2, 4> q_uv) {
        append_face(corners, offsets, {q[0], q[1], q[2], q[3]});
        for (unsigned i = 0; i < 4; ++i) {
            normals.add(n);
            uvs.add(q_uv[i]);
        }
    };

    for (std::uint32_t k = 0; k < nz; ++k) {
        const float v0 = static_cast<float>(double(k) / nz);
        const float v1 = static_cast<float>(double(k + 1) / nz);
        const std::uint32_t a0 = 3 * k;
        const std::uint32_t b0 = 3 * k + 1;
        const std::uint32_t c0 = 3 * k + 2;
        const std::uint32_t a1 = 3 * (k + 1);
        const std::uint32_t b1 = 3 * (k + 1) + 1;
        const std::uint32_t c1 = 3 * (k + 1) + 2;
        emit_quad({a0, b0, b1, a1}, n_bottom, {Vector2{0.f, v0}, Vector2{1.f, v0}, Vector2{1.f, v1}, Vector2{0.f, v1}});
        emit_quad({b0, c0, c1, b1}, n_right, {Vector2{0.f, v0}, Vector2{0.f, v1}, Vector2{1.f, v1}, Vector2{1.f, v0}});
        emit_quad({c0, a0, a1, c1}, n_left, {Vector2{0.f, v1}, Vector2{0.f, v0}, Vector2{1.f, v0}, Vector2{1.f, v1}});
    }

    // Back (-Z) and front (+Z) gable triangles
    append_face(corners, offsets, {1, 0, 2});
    for (unsigned i = 0; i < 3; ++i)
        normals.add({0.f, 0.f, -1.f});
    uvs.add({1.f, 0.f});
    uvs.add({0.f, 0.f});
    uvs.add({settings.peak_ratio, 1.f});

    const std::uint32_t front = 3 * nz;
    append_face(corners, offsets, {front, front + 1, front + 2});
    for (unsigned i = 0; i < 3; ++i)
        normals.add({0.f, 0.f, 1.f});
    uvs.add({0.f, 0.f});
    uvs.add({1.f, 0.f});
    uvs.add({settings.peak_ratio, 1.f});

    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<Geometry> circle(const CircleSettings& settings) {
    if (!valid_plane(settings.orientation))
        return failure("Invalid Circle orientation plane.");
    if (!math::is_finite(settings.radius) || !math::is_finite(settings.center) ||
        !std::isfinite(settings.start_degrees) || !std::isfinite(settings.arc_degrees))
        return failure("Circle radius, center and angles must be finite.");
    if (settings.radius.x <= 0.f || settings.radius.y <= 0.f)
        return failure("Circle radii must be positive.");
    if (settings.arc_degrees <= 0.f || settings.arc_degrees > 360.f)
        return failure("Circle arc_degrees must be in (0, 360].");
    const std::uint32_t min_segments = settings.primitive_type == CirclePrimitive::open_arc ? 1u : 3u;
    if (settings.segments < min_segments)
        return failure("Circle requires segments >= 3 (or >= 1 for open_arc).");
    if (std::uint64_t{settings.segments} > element_limit / 4ULL)
        return failure("Circle would create 2^32 - 1 corners or more.");

    const PlaneBasis basis = plane_basis(settings.orientation);
    const Vector3d c(settings.center);
    const double start_rad = double{settings.start_degrees} * (std::numbers::pi / 180.0);
    const bool full_circle = settings.arc_degrees >= 360.f;
    const double arc_rad =
        full_circle ? 2.0 * std::numbers::pi : double{settings.arc_degrees} * (std::numbers::pi / 180.0);

    const auto make_pt = [&](double theta) -> std::pair<Vector3, Vector2> {
        const double ct = std::cos(theta);
        const double st = std::sin(theta);
        const Vector3d p = c + basis.u * (double{settings.radius.x} * ct) + basis.v * (double{settings.radius.y} * st);
        return {Vector3(p), {static_cast<float>(0.5 + 0.5 * ct), static_cast<float>(0.5 + 0.5 * st)}};
    };

    if (settings.primitive_type == CirclePrimitive::open_arc ||
        settings.primitive_type == CirclePrimitive::closed_polyline) {
        const bool closed_loop = settings.primitive_type == CirclePrimitive::closed_polyline;
        const std::uint32_t pt_count = (closed_loop && full_circle) ? settings.segments : (settings.segments + 1);
        Array<Vector3> positions;
        positions.reserve(pt_count);
        for (std::uint32_t i = 0; i < pt_count; ++i) {
            const double theta = start_rad + arc_rad * double(i) / settings.segments;
            const auto [p, uv] = make_pt(theta);
            if (!math::is_finite(p))
                return failure("Circle coordinates overflowed float range.");
            positions.add(p);
        }
        Array<std::uint32_t> vertex_points;
        const std::uint32_t vtx_count = closed_loop ? (pt_count + 1) : pt_count;
        vertex_points.reserve(vtx_count);
        for (std::uint32_t i = 0; i < pt_count; ++i)
            vertex_points.add(i);
        if (closed_loop)
            vertex_points.add(0);
        Array<std::uint32_t> offsets{0, vtx_count};
        return Geometry::polylines(std::move(positions), std::move(vertex_points), offsets);
    }

    if (settings.primitive_type != CirclePrimitive::polygon)
        return failure("Invalid Circle primitive_type.");

    if (full_circle) {
        Array<Vector3> positions;
        positions.reserve(settings.segments);
        Array<std::uint32_t> corners;
        corners.reserve(settings.segments);
        Array<Vector3> normals;
        normals.reserve(settings.segments);
        Array<Vector2> uvs;
        uvs.reserve(settings.segments);
        for (std::uint32_t i = 0; i < settings.segments; ++i) {
            const double theta = start_rad + arc_rad * double(i) / settings.segments;
            const auto [p, uv] = make_pt(theta);
            if (!math::is_finite(p))
                return failure("Circle coordinates overflowed float range.");
            positions.add(p);
            corners.add(i);
            normals.add(basis.normal);
            uvs.add(uv);
        }
        return surface(std::move(positions), std::move(corners), {0, settings.segments}, std::move(normals),
                       std::move(uvs));
    }

    // Partial-arc sector: emit convex pie triangles sharing the center point so angles > 180 deg remain convex.
    Array<Vector3> positions;
    positions.reserve(settings.segments + 2);
    positions.add(settings.center);
    Array<Vector2> ring_uvs;
    ring_uvs.resize(settings.segments + 1);
    for (std::uint32_t i = 0; i <= settings.segments; ++i) {
        const double theta = start_rad + arc_rad * double(i) / settings.segments;
        const auto [p, uv] = make_pt(theta);
        if (!math::is_finite(p))
            return failure("Circle coordinates overflowed float range.");
        positions.add(p);
        ring_uvs[i] = uv;
    }
    Array<std::uint32_t> corners;
    corners.reserve(3 * std::size_t{settings.segments});
    Array<std::uint32_t> offsets{0};
    offsets.reserve(settings.segments + 1);
    Array<Vector3> normals;
    normals.reserve(3 * std::size_t{settings.segments});
    Array<Vector2> uvs;
    uvs.reserve(3 * std::size_t{settings.segments});
    for (std::uint32_t i = 0; i < settings.segments; ++i) {
        append_face(corners, offsets, {0, 1 + i, 2 + i});
        for (unsigned c_idx = 0; c_idx < 3; ++c_idx)
            normals.add(basis.normal);
        uvs.add({0.5f, 0.5f});
        uvs.add(ring_uvs[i]);
        uvs.add(ring_uvs[i + 1]);
    }
    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<Geometry> torus(const TorusSettings& settings) {
    if (!valid_plane(settings.orientation))
        return failure("Invalid Torus orientation plane.");
    if (!std::isfinite(settings.major_radius) || !std::isfinite(settings.minor_radius) ||
        !math::is_finite(settings.center) || !std::isfinite(settings.arc_degrees))
        return failure("Torus radii, center and arc angle must be finite.");
    if (settings.major_radius <= 0.f || settings.minor_radius <= 0.f || settings.minor_radius >= settings.major_radius)
        return failure("Torus requires 0 < minor_radius < major_radius.");
    if (settings.major_segments < 3 || settings.minor_segments < 3)
        return failure("Torus requires at least 3 major_segments and 3 minor_segments.");
    if (settings.arc_degrees <= 0.f || settings.arc_degrees > 360.f)
        return failure("Torus arc_degrees must be in (0, 360].");
    if (std::uint64_t{settings.major_segments} > element_limit / (4ULL * settings.minor_segments + 4ULL))
        return failure("Torus would create 2^32 - 1 corners or more.");

    const PlaneBasis basis = plane_basis(settings.orientation);
    const Vector3d normal_d(basis.normal);
    const bool full_circle = settings.arc_degrees >= 360.f;
    const double arc_rad =
        full_circle ? 2.0 * std::numbers::pi : double{settings.arc_degrees} * (std::numbers::pi / 180.0);
    const std::uint32_t ring_count = full_circle ? settings.major_segments : (settings.major_segments + 1);

    Array<Vector3> positions;
    positions.reserve(std::size_t{ring_count} * settings.minor_segments);
    Array<Vector3> point_normals;
    point_normals.resize(std::size_t{ring_count} * settings.minor_segments);

    float min_y = 0.f;
    bool first_pt = true;
    for (std::uint32_t i = 0; i < ring_count; ++i) {
        const double u = arc_rad * double(i) / settings.major_segments;
        const Vector3d radial = basis.u * std::cos(u) + basis.v * std::sin(u);
        const Vector3d ring_center = Vector3d(settings.center) + radial * double{settings.major_radius};
        for (std::uint32_t j = 0; j < settings.minor_segments; ++j) {
            const double v = 2.0 * std::numbers::pi * double(j) / settings.minor_segments;
            const Vector3d unit_n = radial * std::cos(v) + normal_d * std::sin(v);
            const Vector3 p = Vector3(ring_center + unit_n * double{settings.minor_radius});
            if (!math::is_finite(p))
                return failure("Torus coordinates overflowed float range.");
            if (first_pt || p.y < min_y) {
                min_y = p.y;
                first_pt = false;
            }
            point_normals[positions.size()] = Vector3(*math::try_normalize(unit_n));
            positions.add(p);
        }
    }
    if (settings.base_on_ground && !positions.empty()) {
        const double dy = double{settings.center.y} - double{min_y};
        for (auto& p : positions) {
            p.y = static_cast<float>(double{p.y} + dy);
            if (!std::isfinite(p.y))
                return failure("Torus coordinates overflowed float range.");
        }
    }

    const auto pt_at = [&](std::uint32_t i, std::uint32_t j) noexcept {
        const std::uint32_t ri = full_circle ? (i % settings.major_segments) : i;
        const std::uint32_t rj = j % settings.minor_segments;
        return ri * settings.minor_segments + rj;
    };

    const std::size_t quad_count = std::size_t{settings.major_segments} * settings.minor_segments;
    const std::size_t cap_corners = (!full_circle && settings.end_caps) ? 2 * std::size_t{settings.minor_segments} : 0;
    Array<std::uint32_t> corners;
    corners.reserve(4 * quad_count + cap_corners);
    Array<std::uint32_t> offsets{0};
    offsets.reserve(quad_count + (cap_corners ? 3 : 1));
    Array<Vector3> normals;
    normals.reserve(corners.capacity());
    Array<Vector2> uvs;
    uvs.reserve(corners.capacity());

    for (std::uint32_t i = 0; i < settings.major_segments; ++i) {
        const float u0 = static_cast<float>(double(i) / settings.major_segments);
        const float u1 = static_cast<float>(double(i + 1) / settings.major_segments);
        for (std::uint32_t j = 0; j < settings.minor_segments; ++j) {
            const float v0 = static_cast<float>(double(j) / settings.minor_segments);
            const float v1 = static_cast<float>(double(j + 1) / settings.minor_segments);
            const std::array<std::uint32_t, 4> q{pt_at(i, j), pt_at(i + 1, j), pt_at(i + 1, j + 1), pt_at(i, j + 1)};
            append_face(corners, offsets, {q[0], q[1], q[2], q[3]});
            for (const auto idx : q)
                normals.add(point_normals[idx]);
            uvs.add({u0, v0});
            uvs.add({u1, v0});
            uvs.add({u1, v1});
            uvs.add({u0, v1});
        }
    }

    if (!full_circle && settings.end_caps) {
        const Vector3 n_start = Vector3(-basis.v);
        for (std::uint32_t j = 0; j < settings.minor_segments; ++j) {
            const double v = 2.0 * std::numbers::pi * double(j) / settings.minor_segments;
            corners.add(pt_at(0, j));
            normals.add(n_start);
            uvs.add({static_cast<float>(0.5 + 0.5 * std::cos(v)), static_cast<float>(0.5 + 0.5 * std::sin(v))});
        }
        offsets.add(static_cast<std::uint32_t>(corners.size()));

        const Vector3d t_end = -basis.u * std::sin(arc_rad) + basis.v * std::cos(arc_rad);
        const Vector3 n_end = Vector3(*math::try_normalize(t_end));
        for (std::uint32_t j = settings.minor_segments; j > 0; --j) {
            const std::uint32_t jj = j - 1;
            const double v = 2.0 * std::numbers::pi * double(jj) / settings.minor_segments;
            corners.add(pt_at(settings.major_segments, jj));
            normals.add(n_end);
            uvs.add({static_cast<float>(0.5 + 0.5 * std::cos(v)), static_cast<float>(0.5 + 0.5 * std::sin(v))});
        }
        offsets.add(static_cast<std::uint32_t>(corners.size()));
    }

    return surface(std::move(positions), std::move(corners), offsets, std::move(normals), std::move(uvs));
}

Result<void> group_box(Geometry& geometry, StringView group, Vector3 minimum, Vector3 maximum) {
    const math::Bounds box{minimum, maximum};
    if (!math::is_finite(box) || math::is_empty(box))
        return failure("GroupBox bounds must be finite and ordered on each axis.");
    ATELIER_TRY_BIND(selected, geometry.add<bool>(group, Layer::point));
    const auto& positions = geometry.positions();
    for (std::size_t i = 0; i < positions.size(); ++i)
        (**selected)[i] = math::contains(box, positions[i]);
    return {};
}

}

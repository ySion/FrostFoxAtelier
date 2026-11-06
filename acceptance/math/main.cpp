#include "memory/fatal.hpp"
#include "math/math.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace atelier::math;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool near(float a, float b) {
    return std::abs(a - b) <= std::numeric_limits<float>::epsilon() * 64 * std::max(1.f, std::abs(b));
}

void scene() {
    // A 120-degree turn about (1,1,1) maps x->y, y->z, z->x exactly.
    const Quaternion turn{0.5f, 0.5f, 0.5f, 0.5f};
    const auto local = trs_matrix(Vector3{4, 5, 6}, turn, Vector3{2, 3, 4});
    require(local == Matrix4{{0, 2, 0, 0, 0, 0, 3, 0, 4, 0, 0, 0, 4, 5, 6, 1}},
            "TRS matrix changed scale, rotation or translation.");
    const Matrix4 parent{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, 1, 3, 1}};
    const auto combined = local * parent;
    require(combined == Matrix4{{0, 2, 0, 0, 0, 0, 3, 0, 4, 0, 0, 0, 2, 6, 9, 1}},
            "Matrix composition changed row-vector order.");
    require(transform_point(Vector3{1, 2, 3}, combined) == Vector3{14, 8, 15}, "Point transform changed coordinates.");
    require(transform_direction(Vector3{1, 2, 3}, local) == Vector3{12, 2, 6},
            "Direction transform applied translation.");
    require(rotate(turn, Vector3{1, 2, 3}) == Vector3{3, 1, 2}, "Quaternion rotation convention differs.");
    require(turn * turn == Quaternion{0.5f, 0.5f, 0.5f, -0.5f}, "Quaternion product is incorrect.");
    require(conjugate(turn) == Quaternion{-0.5f, -0.5f, -0.5f, 0.5f}, "Quaternion conjugate is incorrect.");
    require(transpose(combined) == Matrix4{{0, 0, 4, 2, 2, 0, 0, 6, 0, 3, 0, 9, 0, 0, 0, 1}},
            "Transpose changed values.");
    // x' = 4z+2, y' = 2x+6, z' = 3y+9; solve each equation independently.
    const Matrix4 expected_inverse{{0, 0, 0.25f, 0, 0.5f, 0, 0, 0, 0, 1.f / 3, 0, 0, -3, -3, -0.5f, 1}};
    const auto inverse = try_inverse(combined);
    require(inverse.has_value(), "Scene matrix is invertible.");
    for (std::size_t i = 0; i < 16; ++i)
        require(near((*inverse)[i], expected_inverse[i]), "Matrix inverse is incorrect.");
    // 2^2 + 3^2 + 6^2 = 49; one native normalization, not a float/double matrix.
    require(near(length(Vector3{2, 3, 6}), 7), "Vector length is incorrect.");
    const auto normalized = try_normalize(Vector3{2, 3, 6});
    require(normalized && near(normalized->x, 2.f / 7) && near(normalized->y, 3.f / 7) && near(normalized->z, 6.f / 7),
            "Vector normalization is incorrect.");
    require(cross(Vector3{1, 2, 3}, Vector3{4, -5, 6}) == Vector3{27, 6, -13},
            "Cross product orientation is incorrect.");
    require(lerp(Vector3{1, 2, 3}, Vector3{5, 6, 7}, 0.25f) == Vector3{2, 3, 4}, "Vector interpolation failed.");
}
}

int main() {
    atelier::install_fatal_handler();
    try {
        scene();
        std::puts("Native math acceptance: PASS (analytic vector, quaternion and matrix representatives)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native math acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}

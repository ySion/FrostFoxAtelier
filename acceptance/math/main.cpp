#include "memory/fatal.hpp"
#include "math/math.hpp"
#include "../helpers.hpp"

#include <cstdio>
#include <limits>

namespace {
using namespace atelier::math;

using atelier::acceptance::helpers::require;

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
    // x' = 4z+2, y' = 2x+6, z' = 3y+9; solve each equation independently.
    const Matrix4 expected_inverse{{0, 0, 0.25f, 0, 0.5f, 0, 0, 0, 0, 1.f / 3, 0, 0, -3, -3, -0.5f, 1}};
    const auto inverse = try_inverse(combined);
    require(inverse.has_value(), "Scene matrix is invertible.");
    for (std::size_t i = 0; i < 16; ++i)
        require(near((*inverse)[i], expected_inverse[i]), "Matrix inverse is incorrect.");
}
}

int main() {
    atelier::install_fatal_handler();
    scene();
    std::puts("Native math acceptance: PASS (analytic transform and inverse)");
    return 0;
}

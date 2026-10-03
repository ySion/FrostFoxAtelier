#include "math/math.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace atelier::math;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<Real T> bool near(T a, T b) {
    return std::abs(a - b) <= std::numeric_limits<T>::epsilon() * T{64} * std::max(T{1}, std::abs(b));
}
template<Real T, std::size_t N> bool near(BasicVector<T, N> a, BasicVector<T, N> b) {
    for (std::size_t i = 0; i < N; ++i) if (!near(a[i], b[i])) return false;
    return true;
}

// A complete native scene calculation shares exactly the compact values later
// consumed through Runtime and source code. No Runtime is required to use math.
template<Real T> void scene() {
    using V = BasicVector<T, 3>;
    using Q = BasicQuaternion<T>;
    using M = BasicMatrix4<T>;
    const T half_angle = std::numbers::pi_v<T> / T{4};
    const Q quarter_turn{0, 0, std::sin(half_angle), std::cos(half_angle)};
    const auto local = trs_matrix(V{4, 5, 6}, quarter_turn, V{2, 3, 4});
    const auto parent = trs_matrix(V{-2, 1, 3}, Q{}, V{1, 1, 1});
    const auto combined = local * parent;
    const auto inverse = try_inverse(combined);
    require(inverse.has_value(), "Scene matrix is invertible.");
    std::array<V, 64> positions{};
    const V velocity{1, 2, 3};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        positions[i] = V{static_cast<T>(i), T{0.5}, T{-0.25}} + velocity * T{0.125};
        const V transformed = transform_point(positions[i], combined);
        require(near(transform_point(transformed, *inverse), positions[i]), "Scene inverse lost point coordinates.");
        require(near(transformed, transform_point(transform_point(positions[i], local), parent)),
            "Matrix composition changed row-vector order.");
    }
    require(near(rotate(quarter_turn, V{1, 0, 0}), V{0, 1, 0}), "Quaternion rotation convention differs.");
    require(near(transform_direction(V{1, 0, 0}, local), V{0, 2, 0}), "Direction transform applied translation.");
    auto translated = M::identity(); translated[12] = std::numeric_limits<T>::infinity();
    require(transform_direction(V{1, 2, 3}, translated) == V{1, 2, 3},
        "Direction transform evaluated unused translation.");
    require(near(rotate(quarter_turn * quarter_turn, V{1, 0, 0}), V{-1, 0, 0}), "Quaternion product is incorrect.");
    require(near(rotate(conjugate(quarter_turn), rotate(quarter_turn, V{2, 3, 4})), V{2, 3, 4}),
        "Quaternion conjugate did not undo rotation.");
    const auto identity = combined * *inverse;
    const auto expected_identity = M::identity();
    for (std::size_t i = 0; i < 16; ++i) require(near(identity[i], expected_identity[i]), "Matrix inverse is incorrect.");
    require(transpose(transpose(combined)) == combined, "Transpose changed values.");
    require(!try_inverse(M{}), "Singular matrix accepted.");
    auto invalid = M::identity(); invalid[0] = std::numeric_limits<T>::infinity();
    require(!try_inverse(invalid), "Nonfinite matrix accepted.");
    const T maximum = std::numeric_limits<T>::max();
    const T tiny = std::numeric_limits<T>::denorm_min();
    const T magnitude = [] {
        if constexpr (std::same_as<T, double>) return T{1e308};
        else return T{2e38f};
    }();
    const M huge{{magnitude, magnitude, 0, 0, magnitude, -magnitude, 0, 0,
        0, 0, magnitude, magnitude, 0, 0, magnitude, -magnitude}};
    const auto huge_inverse = try_inverse(huge);
    require(huge_inverse.has_value(), "Well-conditioned huge matrix overflowed during inversion.");
    const auto huge_identity = huge * *huge_inverse;
    for (std::size_t i = 0; i < 16; ++i)
        require(near(huge_identity[i], expected_identity[i]), "Huge matrix inverse lost finite subnormal coefficients.");
    M disparate = M::identity(); disparate[0] = magnitude; disparate[5] = T{1} / magnitude;
    const auto disparate_inverse = try_inverse(disparate);
    require(disparate_inverse.has_value(), "Matrix row scaling discarded independently small diagonal coefficients.");
    const auto disparate_identity = disparate * *disparate_inverse;
    for (std::size_t i = 0; i < 16; ++i)
        require(near(disparate_identity[i], expected_identity[i]), "Disparate diagonal inverse is incorrect.");
    auto unrepresentable_inverse = M::identity(); unrepresentable_inverse[0] = tiny;
    require(!try_inverse(unrepresentable_inverse), "Unrepresentable inverse escaped finite-range validation.");
    const auto huge_direction = try_normalize(V{maximum, maximum, 0});
    const auto tiny_direction = try_normalize(V{tiny, tiny, 0});
    require(huge_direction && tiny_direction && near(*huge_direction, *tiny_direction) && near(length(*huge_direction), T{1}),
        "Normalization overflowed or lost subnormal input.");
    require(!try_normalize(V{}) && !try_normalize(V{std::numeric_limits<T>::infinity(), 0, 0}),
        "Invalid vector normalization succeeded.");
    require(!try_normalize(Q{0, 0, 0, 0}) && try_normalize(Q{maximum, maximum, maximum, maximum}).has_value(),
        "Quaternion normalization does not handle its full finite range.");
    require(near(length(V{maximum / T{4}, maximum / T{4}, 0}), maximum / T{4} * std::sqrt(T{2})),
        "Representable vector length overflowed.");
    const BasicVector<T, 2> uv{2, 4};
    require(uv / T{2} == BasicVector<T, 2>{1, 2}, "Vector2 division failed.");
    const BasicVector<T, 4> homogeneous{1, 2, 3, 1};
    require(homogeneous * M::identity() == homogeneous, "Vector4 identity failed.");
    require(cross(V{1, 0, 0}, V{0, 1, 0}) == V{0, 0, 1}, "Cross product orientation is incorrect.");
    require(lerp(V{1, 2, 3}, V{3, 4, 5}, T{0.5}) == V{2, 3, 4}, "Vector interpolation failed.");
}
}

int main() {
    try {
        scene<float>();
        scene<double>();
        const Vector3d position{1e12, -1e12, 1e12};
        const auto moved = position + Vector3d{0.125, 0.25, -0.5};
        require(moved - position == Vector3d{0.125, 0.25, -0.5}, "Double math lost precision through float conversion.");
        std::puts("Native math acceptance: PASS (float/double scene, compact layout, full-range normalization)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native math acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}

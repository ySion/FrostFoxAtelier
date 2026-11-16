#pragma once

#include "math/matrix.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <limits>
#include <type_traits>

namespace atelier::math {

template <Real T>
struct BasicBounds {
    using Scalar = T;

    BasicVector<T, 3> minimum{};

    BasicVector<T, 3> maximum{};

    template <Real U>
        requires(!std::same_as<T, U>)
    explicit constexpr operator BasicBounds<U>() const noexcept {
        return {BasicVector<U, 3>(minimum), BasicVector<U, 3>(maximum)};
    }

    // Canonical empty identity for encapsulate() and distance_squared():
    // arbitrary finite inverted boxes satisfy is_empty() but are not identities.
    [[nodiscard]] static constexpr BasicBounds empty() noexcept {
        constexpr T inf = std::numeric_limits<T>::infinity();
        return {{inf, inf, inf}, {-inf, -inf, -inf}};
    }

    [[nodiscard]] friend constexpr bool operator==(const BasicBounds&, const BasicBounds&) noexcept = default;
};

template <Real T>
[[nodiscard]] constexpr bool is_empty(const BasicBounds<T>& bounds) noexcept {
    return !(bounds.minimum.x <= bounds.maximum.x && bounds.minimum.y <= bounds.maximum.y &&
             bounds.minimum.z <= bounds.maximum.z);
}

template <Real T>
[[nodiscard]] constexpr bool is_finite(const BasicBounds<T>& bounds) noexcept {
    return is_finite(bounds.minimum) && is_finite(bounds.maximum);
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> center(const BasicBounds<T>& bounds) noexcept {
    return bounds.minimum * T(0.5) + bounds.maximum * T(0.5);
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> size(const BasicBounds<T>& bounds) noexcept {
    return bounds.maximum - bounds.minimum;
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> extents(const BasicBounds<T>& bounds) noexcept {
    return (bounds.maximum - bounds.minimum) * T(0.5);
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> corner(const BasicBounds<T>& bounds, std::uint32_t index) noexcept {
    return {(index & 1u) ? bounds.maximum.x : bounds.minimum.x, (index & 2u) ? bounds.maximum.y : bounds.minimum.y,
            (index & 4u) ? bounds.maximum.z : bounds.minimum.z};
}

// Precondition: empty operands must be BasicBounds<T>::empty(); finite operands follow cwise_min/cwise_max NaN rules.
template <Real T>
[[nodiscard]] constexpr BasicBounds<T> encapsulate(const BasicBounds<T>& bounds, BasicVector<T, 3> point) noexcept {
    return {cwise_min(bounds.minimum, point), cwise_max(bounds.maximum, point)};
}

template <Real T>
[[nodiscard]] constexpr BasicBounds<T> encapsulate(const BasicBounds<T>& a, const BasicBounds<T>& b) noexcept {
    return {cwise_min(a.minimum, b.minimum), cwise_max(a.maximum, b.maximum)};
}

template <Real T>
[[nodiscard]] constexpr bool contains(const BasicBounds<T>& bounds, BasicVector<T, 3> point) noexcept {
    return point.x >= bounds.minimum.x && point.x <= bounds.maximum.x && point.y >= bounds.minimum.y &&
           point.y <= bounds.maximum.y && point.z >= bounds.minimum.z && point.z <= bounds.maximum.z;
}

template <Real T>
[[nodiscard]] constexpr bool intersects(const BasicBounds<T>& a, const BasicBounds<T>& b) noexcept {
    return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x && a.minimum.y <= b.maximum.y &&
           a.maximum.y >= b.minimum.y && a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z && !is_empty(a) &&
           !is_empty(b);
}

// Precondition: bounds is non-empty or BasicBounds<T>::empty() (which yields +inf for finite points).
template <Real T>
[[nodiscard]] constexpr T distance_squared(const BasicBounds<T>& bounds, BasicVector<T, 3> point) noexcept {
    const auto excess = cwise_max(cwise_max(bounds.minimum - point, point - bounds.maximum), BasicVector<T, 3>{});
    return length_squared(excess);
}

// Affine AABB transform (row-vector convention). Preserves empty() and matches
// encapsulating all eight transformed corners without perspective division.
template <Real T>
[[nodiscard]] constexpr BasicBounds<T> transform_bounds(const BasicBounds<T>& bounds,
                                                        const BasicMatrix4<T>& affine) noexcept {
    if (is_empty(bounds))
        return BasicBounds<T>::empty();
    if !consteval {
        if constexpr (std::same_as<T, float>) {
            const __m128 r0 = _mm_loadu_ps(&affine[0]);
            const __m128 r1 = _mm_loadu_ps(&affine[4]);
            const __m128 r2 = _mm_loadu_ps(&affine[8]);
            const __m128 r3 = _mm_loadu_ps(&affine[12]);
            const __m128 a0 = _mm_mul_ps(_mm_set1_ps(bounds.minimum.x), r0);
            const __m128 b0 = _mm_mul_ps(_mm_set1_ps(bounds.maximum.x), r0);
            const __m128 a1 = _mm_mul_ps(_mm_set1_ps(bounds.minimum.y), r1);
            const __m128 b1 = _mm_mul_ps(_mm_set1_ps(bounds.maximum.y), r1);
            const __m128 a2 = _mm_mul_ps(_mm_set1_ps(bounds.minimum.z), r2);
            const __m128 b2 = _mm_mul_ps(_mm_set1_ps(bounds.maximum.z), r2);
            // _mm_min_ps/_mm_max_ps return their second operand on NaN or signed-zero tie,
            // so (b, a) matches std::min(a, b) / std::max(a, b) bit-for-bit.
            const __m128 lo =
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_min_ps(b0, a0), _mm_min_ps(b1, a1)), _mm_min_ps(b2, a2)), r3);
            const __m128 hi =
                _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_max_ps(b0, a0), _mm_max_ps(b1, a1)), _mm_max_ps(b2, a2)), r3);
            alignas(16) float lo_out[4];
            alignas(16) float hi_out[4];
            _mm_store_ps(lo_out, lo);
            _mm_store_ps(hi_out, hi);
            return {{lo_out[0], lo_out[1], lo_out[2]}, {hi_out[0], hi_out[1], hi_out[2]}};
        } else {
            const __m128d r0_xy = _mm_loadu_pd(&affine[0]);
            const __m128d r1_xy = _mm_loadu_pd(&affine[4]);
            const __m128d r2_xy = _mm_loadu_pd(&affine[8]);
            const __m128d r3_xy = _mm_loadu_pd(&affine[12]);
            const __m128d a0 = _mm_mul_pd(_mm_set1_pd(bounds.minimum.x), r0_xy);
            const __m128d b0 = _mm_mul_pd(_mm_set1_pd(bounds.maximum.x), r0_xy);
            const __m128d a1 = _mm_mul_pd(_mm_set1_pd(bounds.minimum.y), r1_xy);
            const __m128d b1 = _mm_mul_pd(_mm_set1_pd(bounds.maximum.y), r1_xy);
            const __m128d a2 = _mm_mul_pd(_mm_set1_pd(bounds.minimum.z), r2_xy);
            const __m128d b2 = _mm_mul_pd(_mm_set1_pd(bounds.maximum.z), r2_xy);
            const __m128d lo_xy =
                _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_min_pd(b0, a0), _mm_min_pd(b1, a1)), _mm_min_pd(b2, a2)), r3_xy);
            const __m128d hi_xy =
                _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_max_pd(b0, a0), _mm_max_pd(b1, a1)), _mm_max_pd(b2, a2)), r3_xy);
            alignas(16) double lo_out[2];
            alignas(16) double hi_out[2];
            _mm_store_pd(lo_out, lo_xy);
            _mm_store_pd(hi_out, hi_xy);
            const double za0 = bounds.minimum.x * affine[2];
            const double zb0 = bounds.maximum.x * affine[2];
            const double za1 = bounds.minimum.y * affine[6];
            const double zb1 = bounds.maximum.y * affine[6];
            const double za2 = bounds.minimum.z * affine[10];
            const double zb2 = bounds.maximum.z * affine[10];
            const double lo_z = ((std::min(za0, zb0) + std::min(za1, zb1)) + std::min(za2, zb2)) + affine[14];
            const double hi_z = ((std::max(za0, zb0) + std::max(za1, zb1)) + std::max(za2, zb2)) + affine[14];
            return {{lo_out[0], lo_out[1], lo_z}, {hi_out[0], hi_out[1], hi_z}};
        }
    }
    BasicBounds<T> result;
    for (std::size_t column = 0; column < 3; ++column) {
        const T a0 = bounds.minimum.x * affine(0, column);
        const T b0 = bounds.maximum.x * affine(0, column);
        const T a1 = bounds.minimum.y * affine(1, column);
        const T b1 = bounds.maximum.y * affine(1, column);
        const T a2 = bounds.minimum.z * affine(2, column);
        const T b2 = bounds.maximum.z * affine(2, column);
        result.minimum[column] = ((std::min(a0, b0) + std::min(a1, b1)) + std::min(a2, b2)) + affine(3, column);
        result.maximum[column] = ((std::max(a0, b0) + std::max(a1, b1)) + std::max(a2, b2)) + affine(3, column);
    }
    return result;
}

using Bounds = BasicBounds<float>;

using Boundsd = BasicBounds<double>;

static_assert(sizeof(Bounds) == 24 && sizeof(Boundsd) == 48);

static_assert(alignof(Bounds) == alignof(float) && alignof(Boundsd) == alignof(double));

static_assert(std::is_aggregate_v<Bounds> && std::is_standard_layout_v<Bounds> && std::is_trivially_copyable_v<Bounds>);

}

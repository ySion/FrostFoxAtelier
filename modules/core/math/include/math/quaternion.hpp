#pragma once

#include "math/vector.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>

namespace atelier::math {

template <Real T>
struct BasicQuaternion {
    using Scalar = T;

    T x{};

    T y{};

    T z{};

    T w{1};

    template <Real U>
        requires(!std::same_as<T, U>)
    explicit constexpr operator BasicQuaternion<U>() const noexcept {
        return {static_cast<U>(x), static_cast<U>(y), static_cast<U>(z), static_cast<U>(w)};
    }

    constexpr bool operator==(const BasicQuaternion&) const = default;
};

template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> operator+(BasicQuaternion<T> value) noexcept {
    return value;
}

template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> operator-(BasicQuaternion<T> value) noexcept {
    return {-value.x, -value.y, -value.z, -value.w};
}

template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> conjugate(BasicQuaternion<T> q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

template <Real T>
[[nodiscard]] constexpr T dot(BasicQuaternion<T> a, BasicQuaternion<T> b) noexcept {
    return ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w;
}

// Hamilton product: a*b applies b then a. For our row-vector matrices this is
// rotation_matrix(a*b) == rotation_matrix(b) * rotation_matrix(a).
template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> operator*(BasicQuaternion<T> a, BasicQuaternion<T> b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Requires a unit axis; angle is in radians.
template <Real T>
[[nodiscard]] BasicQuaternion<T> from_axis_angle(BasicVector<T, 3> unit_axis,
                                                 std::type_identity_t<T> radians) noexcept {
    const T half = radians * T(0.5);
    const T s = std::sin(half);
    return {unit_axis.x * s, unit_axis.y * s, unit_axis.z * s, std::cos(half)};
}

template <Real T>
[[nodiscard]] constexpr bool is_finite(BasicQuaternion<T> q) noexcept {
    return is_finite(BasicVector<T, 4>{q.x, q.y, q.z, q.w});
}

template <Real T>
[[nodiscard]] std::optional<BasicQuaternion<T>> try_normalize(BasicQuaternion<T> q) noexcept {
    const auto unit = try_normalize(BasicVector<T, 4>{q.x, q.y, q.z, q.w});
    if (!unit)
        return std::nullopt;
    return BasicQuaternion<T>{unit->x, unit->y, unit->z, unit->w};
}

// Shortest-arc spherical interpolation on [0, 1] between unit quaternions.
// Clamps t with source math.clamp semantics (NaN -> 0), negates unit_b when
// dot(unit_a, unit_b) < 0 (so t = 1 yields -unit_b), and takes a normalized
// linear branch for near-collinear inputs to avoid dividing by a vanishing sine.
template <Real T>
[[nodiscard]] BasicQuaternion<T> slerp01(BasicQuaternion<T> unit_a, BasicQuaternion<T> unit_b,
                                         std::type_identity_t<T> t) noexcept {
    const T clamped = std::max(T{0}, std::min(t, T{1}));
    T cosine = dot(unit_a, unit_b);
    if (cosine < T{}) {
        unit_b = -unit_b;
        cosine = -cosine;
    }
    constexpr T threshold = std::same_as<T, double> ? T(1) - T(1e-10) : T(1) - T(1e-4);
    if (cosine > threshold) {
        const T u = T(1) - clamped;
        const BasicVector<T, 4> blended{unit_a.x * u + unit_b.x * clamped, unit_a.y * u + unit_b.y * clamped,
                                        unit_a.z * u + unit_b.z * clamped, unit_a.w * u + unit_b.w * clamped};
        const T norm = std::sqrt(length_squared(blended));
        return {blended.x / norm, blended.y / norm, blended.z / norm, blended.w / norm};
    }
    const T sine = std::sqrt(std::max(T{}, T(1) - cosine * cosine));
    const T angle = std::atan2(sine, cosine);
    const T wa = std::sin((T(1) - clamped) * angle) / sine;
    const T wb = std::sin(clamped * angle) / sine;
    return {unit_a.x * wa + unit_b.x * wb, unit_a.y * wa + unit_b.y * wb, unit_a.z * wa + unit_b.z * wb,
            unit_a.w * wa + unit_b.w * wb};
}

// Requires a unit quaternion. Normalize at input boundaries, not per vertex.
template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> rotate(BasicQuaternion<T> unit, BasicVector<T, 3> value) noexcept {
    const BasicVector<T, 3> axis{unit.x, unit.y, unit.z};
    const auto twice_cross = T{2} * cross(axis, value);
    return value + unit.w * twice_cross + cross(axis, twice_cross);
}

using Quaternion = BasicQuaternion<float>;

using Quaterniond = BasicQuaternion<double>;

template <Real T>
inline constexpr Type type_of<BasicQuaternion<T>>{Shape::quaternion,
                                                  std::same_as<T, double> ? Precision::f64 : Precision::f32};

static_assert(sizeof(Quaternion) == 16 && sizeof(Quaterniond) == 32);

}

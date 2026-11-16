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

template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> fast_normalize_or_zero(BasicQuaternion<T> q) noexcept {
    const auto unit = fast_normalize_or_zero(BasicVector<T, 4>{q.x, q.y, q.z, q.w});
    return {unit.x, unit.y, unit.z, unit.w};
}

// Branchless shortest-arc spherical interpolation on [0, 1] between unit quaternions.
// Clamps t with source math.clamp semantics (NaN -> 0), negates unit_b when
// dot(unit_a, unit_b) < 0 (so t = 1 yields -unit_b), and selects a normalized
// linear blend for near-collinear inputs without dividing by a vanishing sine.
template <Real T>
[[nodiscard]] BasicQuaternion<T> slerp01(BasicQuaternion<T> unit_a, BasicQuaternion<T> unit_b,
                                         std::type_identity_t<T> t) noexcept {
    const T clamped = std::max(T{0}, std::min(t, T{1}));
    const T u = T{1} - clamped;
    const T raw_cosine = dot(unit_a, unit_b);
    const bool flip = raw_cosine < T{};
    const BasicQuaternion<T> target = flip ? -unit_b : unit_b;
    const T cosine = flip ? -raw_cosine : raw_cosine;
    constexpr T threshold = std::same_as<T, double> ? T(1) - T(1e-10) : T(1) - T(1e-4);
    const bool near_collinear = cosine > threshold;
    const BasicVector<T, 4> linear{unit_a.x * u + target.x * clamped, unit_a.y * u + target.y * clamped,
                                   unit_a.z * u + target.z * clamped, unit_a.w * u + target.w * clamped};
    const T safe_norm = std::sqrt(near_collinear ? length_squared(linear) : std::max(T{}, T{1} - cosine * cosine));
    const T sine = near_collinear ? T{} : safe_norm;
    const T angle = std::atan2(sine, cosine);
    const T wa = std::sin(u * angle) / safe_norm;
    const T wb = std::sin(clamped * angle) / safe_norm;
    const BasicQuaternion<T> collinear{linear.x / safe_norm, linear.y / safe_norm, linear.z / safe_norm,
                                       linear.w / safe_norm};
    const BasicQuaternion<T> spherical{unit_a.x * wa + target.x * wb, unit_a.y * wa + target.y * wb,
                                       unit_a.z * wa + target.z * wb, unit_a.w * wa + target.w * wb};
    return near_collinear ? collinear : spherical;
}

namespace detail {

template <Real T, std::size_t K>
[[nodiscard]] constexpr T slerp_horner(T x, const T (&coefficients)[K]) noexcept {
    T acc = coefficients[K - 1];
    for (std::size_t i = K - 1; i-- > 0;)
        acc = acc * x + coefficients[i];
    return acc;
}

}

// Branchless polynomial shortest-arc spherical interpolation on [0, 1] between unit quaternions.
// Exact at t = 0 (unit_a) and t = 1 (+-unit_b), NaN t -> 0; max angular error <= 1e-4 rad (float) / <= 1e-7 rad (double).
template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> fast_slerp01(BasicQuaternion<T> unit_a, BasicQuaternion<T> unit_b,
                                                        std::type_identity_t<T> t) noexcept {
    const T clamped = std::max(T{0}, std::min(t, T{1}));
    const T u = T{1} - clamped;
    const T raw_cosine = dot(unit_a, unit_b);
    const bool flip = raw_cosine < T{};
    const BasicQuaternion<T> target = flip ? -unit_b : unit_b;
    const T cosine = flip ? -raw_cosine : raw_cosine;
    const T z = cosine > T{1} ? T{} : (T{1} - cosine);
    T wa{};
    T wb{};
    if constexpr (std::same_as<T, float>) {
        constexpr float c0[]{-0x1.5597fap-2f, -0x1.06ea0cp-3f, -0x1.581d72p-4f, 0x1.f29a3cp-6f, -0x1.c41f92p-5f};
        constexpr float c1[]{-0x1.bf9492p-11f, 0x1.67037p-5f, -0x1.948e78p-7f, 0x1.6b391ep-5f};
        constexpr float c2[]{-0x1.12b68p-10f, 0x1.65b8dcp-8f, -0x1.1bd13cp-7f};
        const float p0 = detail::slerp_horner(z, c0);
        const float p1 = detail::slerp_horner(z, c1);
        const float p2 = detail::slerp_horner(z, c2);
        const auto weight = [&](float s) noexcept {
            const float s2 = s * s;
            return s + s * (s2 - 1.0f) * z * ((p2 * s2 + p1) * s2 + p0);
        };
        wa = weight(u);
        wb = weight(clamped);
    } else {
        constexpr double c0[]{-0x1.55556e5087672p-2, -0x1.1106eee24dc3bp-3, -0x1.d6bc55d25c563p-5,
                              -0x1.77e80b37c0b13p-6, -0x1.634b85c2b89d3p-6, 0x1.345eda48891bap-6,
                              -0x1.2670d071ac7bdp-5, 0x1.8ef7d6468286bp-6,  -0x1.304f363d0c806p-7};
        constexpr double c1[]{-0x1.82e7604a3c924p-20, 0x1.1188acce1be50p-5,  0x1.4640a775ca28ep-6,
                              0x1.e9f53308a08bep-7,   -0x1.ea01c405b9f51p-8, 0x1.8cbdc98df78b7p-6,
                              -0x1.27ace869c9084p-6,  0x1.0d69dd3f8496ep-7};
        constexpr double c2[]{-0x1.125d965a3dee4p-19, 0x1.ed6b7e46b09fdp-15, -0x1.1764fb499fad1p-9,
                              0x1.056b857d13e06p-10,  -0x1.6e763331d2ab3p-8, 0x1.351c9fe1cf82dp-8,
                              -0x1.511e13b8c042ap-9};
        constexpr double c3[]{-0x1.3bbf40807e05fp-20, 0x1.9bc814adb423bp-16,  -0x1.50fba5662a71fp-13,
                              0x1.06311f349fab1p-11,  -0x1.246b1d81fd286p-11, 0x1.70cace8f38834p-12};
        constexpr double c4[]{-0x1.7faf9b6e594d7p-22, 0x1.347c850fd5f82p-18, -0x1.380eea723bbc5p-16,
                              0x1.0091e063c6f80p-15, -0x1.495a4a063e900p-16};
        const double p0 = detail::slerp_horner(z, c0);
        const double p1 = detail::slerp_horner(z, c1);
        const double p2 = detail::slerp_horner(z, c2);
        const double p3 = detail::slerp_horner(z, c3);
        const double p4 = detail::slerp_horner(z, c4);
        const auto weight = [&](double s) noexcept {
            const double s2 = s * s;
            return s + s * (s2 - 1.0) * z * ((((p4 * s2 + p3) * s2 + p2) * s2 + p1) * s2 + p0);
        };
        wa = weight(u);
        wb = weight(clamped);
    }
    return {unit_a.x * wa + target.x * wb, unit_a.y * wa + target.y * wb, unit_a.z * wa + target.z * wb,
            unit_a.w * wa + target.w * wb};
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

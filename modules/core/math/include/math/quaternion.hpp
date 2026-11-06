#pragma once

#include "math/vector.hpp"

#include <optional>

namespace atelier::math {

template <Real T>
struct BasicQuaternion {
    using Scalar = T;

    T x{};

    T y{};

    T z{};

    T w{1};

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

// Hamilton product: a*b applies b then a. For our row-vector matrices this is
// rotation_matrix(a*b) == rotation_matrix(b) * rotation_matrix(a).
template <Real T>
[[nodiscard]] constexpr BasicQuaternion<T> operator*(BasicQuaternion<T> a, BasicQuaternion<T> b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

template <Real T>
[[nodiscard]] bool is_finite(BasicQuaternion<T> q) noexcept {
    return is_finite(BasicVector<T, 4>{q.x, q.y, q.z, q.w});
}

template <Real T>
[[nodiscard]] std::optional<BasicQuaternion<T>> try_normalize(BasicQuaternion<T> q) noexcept {
    const auto unit = try_normalize(BasicVector<T, 4>{q.x, q.y, q.z, q.w});
    if (!unit)
        return std::nullopt;
    return BasicQuaternion<T>{unit->x, unit->y, unit->z, unit->w};
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

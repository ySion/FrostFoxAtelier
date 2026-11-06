#pragma once

#include "math/type.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace atelier::math {

// Named components stay compact in arrays. Arithmetic may use wider registers;
// the storage contract never permits a fourth-lane load from a Vector3 object.
template <Real T, std::size_t N>
struct BasicVector;

template <Real T>
struct BasicVector<T, 2> {
    using Scalar = T;

    static constexpr std::size_t dimensions = 2;

    T x{};

    T y{};

    constexpr T& operator[](std::size_t i) noexcept {
        if (i == 0)
            return x;
        if (i == 1)
            return y;
        std::unreachable();
    }

    constexpr const T& operator[](std::size_t i) const noexcept {
        if (i == 0)
            return x;
        if (i == 1)
            return y;
        std::unreachable();
    }

    constexpr bool operator==(const BasicVector&) const = default;
};

template <Real T>
struct BasicVector<T, 3> {
    using Scalar = T;

    static constexpr std::size_t dimensions = 3;

    T x{};

    T y{};

    T z{};

    constexpr T& operator[](std::size_t i) noexcept {
        if (i == 0)
            return x;
        if (i == 1)
            return y;
        if (i == 2)
            return z;
        std::unreachable();
    }

    constexpr const T& operator[](std::size_t i) const noexcept {
        if (i == 0)
            return x;
        if (i == 1)
            return y;
        if (i == 2)
            return z;
        std::unreachable();
    }

    constexpr bool operator==(const BasicVector&) const = default;
};

template <Real T>
struct BasicVector<T, 4> {
    using Scalar = T;

    static constexpr std::size_t dimensions = 4;

    T x{};

    T y{};

    T z{};

    T w{};

    constexpr T& operator[](std::size_t i) noexcept {
        if (i == 0)
            return x;
        if (i == 1)
            return y;
        if (i == 2)
            return z;
        if (i == 3)
            return w;
        std::unreachable();
    }

    constexpr const T& operator[](std::size_t i) const noexcept {
        if (i == 0)
            return x;
        if (i == 1)
            return y;
        if (i == 2)
            return z;
        if (i == 3)
            return w;
        std::unreachable();
    }

    constexpr bool operator==(const BasicVector&) const = default;
};

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator+(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        a[i] += b[i];
    return a;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator-(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        a[i] -= b[i];
    return a;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator+(BasicVector<T, N> value) noexcept {
    return value;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator-(BasicVector<T, N> value) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        value[i] = -value[i];
    return value;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator*(BasicVector<T, N> value, std::type_identity_t<T> scale) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        value[i] *= scale;
    return value;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator*(std::type_identity_t<T> scale, BasicVector<T, N> value) noexcept {
    return value * scale;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator/(BasicVector<T, N> value, std::type_identity_t<T> scale) noexcept {
    // Divide each component: multiplying by 1/scale overflows for small scale.
    for (std::size_t i = 0; i < N; ++i)
        value[i] /= scale;
    return value;
}

template <Real T, std::size_t N>
constexpr BasicVector<T, N>& operator+=(BasicVector<T, N>& a, BasicVector<T, N> b) noexcept {
    return a = a + b;
}

template <Real T, std::size_t N>
constexpr BasicVector<T, N>& operator-=(BasicVector<T, N>& a, BasicVector<T, N> b) noexcept {
    return a = a - b;
}

template <Real T, std::size_t N>
constexpr BasicVector<T, N>& operator*=(BasicVector<T, N>& value, std::type_identity_t<T> scale) noexcept {
    return value = value * scale;
}

template <Real T, std::size_t N>
constexpr BasicVector<T, N>& operator/=(BasicVector<T, N>& value, std::type_identity_t<T> scale) noexcept {
    return value = value / scale;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr T dot(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    T result = a[0] * b[0];
    for (std::size_t i = 1; i < N; ++i)
        result += a[i] * b[i];
    return result;
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> cross(BasicVector<T, 3> a, BasicVector<T, 3> b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <Real T, std::size_t N>
[[nodiscard]] bool is_finite(BasicVector<T, N> value) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        if (!std::isfinite(value[i]))
            return false;
    return true;
}

template <Real T, std::size_t N>
[[nodiscard]] T length(BasicVector<T, N> value) noexcept {
    if constexpr (N == 2)
        return std::hypot(value.x, value.y);
    if constexpr (N == 3)
        return std::hypot(value.x, value.y, value.z);
    if constexpr (N == 4)
        return std::hypot(std::hypot(value.x, value.y), std::hypot(value.z, value.w));
}

// Zero and nonfinite values have no normalized result. Rescaling also handles
// finite values whose length cannot be represented in the component type.
template <Real T, std::size_t N>
[[nodiscard]] std::optional<BasicVector<T, N>> try_normalize(BasicVector<T, N> value) noexcept {
    if (!is_finite(value))
        return std::nullopt;
    T scale{};
    for (std::size_t i = 0; i < N; ++i)
        scale = std::max(scale, std::abs(value[i]));
    if (scale == T{})
        return std::nullopt;
    value /= scale;
    return value / std::sqrt(dot(value, value));
}

// Same formula as source math.lerp: exact at t = 0 and t = 1 for finite values, not monotonic.
template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> lerp(BasicVector<T, N> a, BasicVector<T, N> b,
                                               std::type_identity_t<T> t) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        a[i] = a[i] * (T(1) - t) + b[i] * t;
    return a;
}

using Vector2 = BasicVector<float, 2>;

using Vector3 = BasicVector<float, 3>;

using Vector4 = BasicVector<float, 4>;

using Vector2d = BasicVector<double, 2>;

using Vector3d = BasicVector<double, 3>;

using Vector4d = BasicVector<double, 4>;

template <Real T, std::size_t N>
    requires(N >= 2 && N <= 4)
inline constexpr Type type_of<BasicVector<T, N>>{N == 2   ? Shape::vector2
                                                 : N == 3 ? Shape::vector3
                                                          : Shape::vector4,
                                                 std::same_as<T, double> ? Precision::f64 : Precision::f32};

static_assert(sizeof(Vector2) == 8 && sizeof(Vector3) == 12 && sizeof(Vector4) == 16);

static_assert(sizeof(Vector2d) == 16 && sizeof(Vector3d) == 24 && sizeof(Vector4d) == 32);

static_assert(alignof(Vector3) == alignof(float) && alignof(Vector3d) == alignof(double));

static_assert(std::is_standard_layout_v<Vector3> && std::is_trivially_copyable_v<Vector3>);

}

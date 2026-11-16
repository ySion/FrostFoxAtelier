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

    template <Real U>
        requires(!std::same_as<T, U>)
    explicit constexpr operator BasicVector<U, 2>() const noexcept {
        return {static_cast<U>(x), static_cast<U>(y)};
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

    template <Real U>
        requires(!std::same_as<T, U>)
    explicit constexpr operator BasicVector<U, 3>() const noexcept {
        return {static_cast<U>(x), static_cast<U>(y), static_cast<U>(z)};
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

    template <Real U>
        requires(!std::same_as<T, U>)
    explicit constexpr operator BasicVector<U, 4>() const noexcept {
        return {static_cast<U>(x), static_cast<U>(y), static_cast<U>(z), static_cast<U>(w)};
    }

    constexpr bool operator==(const BasicVector&) const = default;
};

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator+(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return {a.x + b.x, a.y + b.y};
    if constexpr (N == 3)
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    if constexpr (N == 4)
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator-(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return {a.x - b.x, a.y - b.y};
    if constexpr (N == 3)
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    if constexpr (N == 4)
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator+(BasicVector<T, N> value) noexcept {
    return value;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator-(BasicVector<T, N> value) noexcept {
    if constexpr (N == 2)
        return {-value.x, -value.y};
    if constexpr (N == 3)
        return {-value.x, -value.y, -value.z};
    if constexpr (N == 4)
        return {-value.x, -value.y, -value.z, -value.w};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator*(BasicVector<T, N> value, std::type_identity_t<T> scale) noexcept {
    if constexpr (N == 2)
        return {value.x * scale, value.y * scale};
    if constexpr (N == 3)
        return {value.x * scale, value.y * scale, value.z * scale};
    if constexpr (N == 4)
        return {value.x * scale, value.y * scale, value.z * scale, value.w * scale};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator*(std::type_identity_t<T> scale, BasicVector<T, N> value) noexcept {
    return value * scale;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> operator/(BasicVector<T, N> value, std::type_identity_t<T> scale) noexcept {
    // Divide each component: multiplying by 1/scale overflows for small scale.
    if constexpr (N == 2)
        return {value.x / scale, value.y / scale};
    if constexpr (N == 3)
        return {value.x / scale, value.y / scale, value.z / scale};
    if constexpr (N == 4)
        return {value.x / scale, value.y / scale, value.z / scale, value.w / scale};
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
[[nodiscard]] constexpr BasicVector<T, N> cwise_mul(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return {a.x * b.x, a.y * b.y};
    if constexpr (N == 3)
        return {a.x * b.x, a.y * b.y, a.z * b.z};
    if constexpr (N == 4)
        return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> cwise_div(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return {a.x / b.x, a.y / b.y};
    if constexpr (N == 3)
        return {a.x / b.x, a.y / b.y, a.z / b.z};
    if constexpr (N == 4)
        return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}

// GLSL / std::min / std::max order: only a first-operand NaN is preserved.
template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> cwise_min(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return {std::min(a.x, b.x), std::min(a.y, b.y)};
    if constexpr (N == 3)
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    if constexpr (N == 4)
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), std::min(a.w, b.w)};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> cwise_max(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return {std::max(a.x, b.x), std::max(a.y, b.y)};
    if constexpr (N == 3)
        return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    if constexpr (N == 4)
        return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)};
}

namespace detail {

template <Real T>
[[nodiscard]] constexpr T scalar_abs(T value) noexcept {
    if constexpr (std::same_as<T, float>)
        return __builtin_fabsf(value);
    else
        return __builtin_fabs(value);
}

}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> cwise_abs(BasicVector<T, N> value) noexcept {
    if constexpr (N == 2)
        return {detail::scalar_abs(value.x), detail::scalar_abs(value.y)};
    if constexpr (N == 3)
        return {detail::scalar_abs(value.x), detail::scalar_abs(value.y), detail::scalar_abs(value.z)};
    if constexpr (N == 4)
        return {detail::scalar_abs(value.x), detail::scalar_abs(value.y), detail::scalar_abs(value.z),
                detail::scalar_abs(value.w)};
}

// Same formula as source math.clamp: cwise_max(low, cwise_min(value, high)).
template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> clamp(BasicVector<T, N> value, BasicVector<T, N> low,
                                                BasicVector<T, N> high) noexcept {
    return cwise_max(low, cwise_min(value, high));
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> clamp(BasicVector<T, N> value, std::type_identity_t<T> low,
                                                std::type_identity_t<T> high) noexcept {
    if constexpr (N == 2)
        return clamp(value, BasicVector<T, 2>{low, low}, BasicVector<T, 2>{high, high});
    if constexpr (N == 3)
        return clamp(value, BasicVector<T, 3>{low, low, low}, BasicVector<T, 3>{high, high, high});
    if constexpr (N == 4)
        return clamp(value, BasicVector<T, 4>{low, low, low, low}, BasicVector<T, 4>{high, high, high, high});
}

// Pair with is_finite() when rejecting NaN: later NaN lanes do not overwrite a finite first lane.
template <Real T, std::size_t N>
[[nodiscard]] constexpr T min_component(BasicVector<T, N> value) noexcept {
    if constexpr (N == 2)
        return std::min(value.x, value.y);
    if constexpr (N == 3)
        return std::min(std::min(value.x, value.y), value.z);
    if constexpr (N == 4)
        return std::min(std::min(value.x, value.y), std::min(value.z, value.w));
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr T max_component(BasicVector<T, N> value) noexcept {
    if constexpr (N == 2)
        return std::max(value.x, value.y);
    if constexpr (N == 3)
        return std::max(std::max(value.x, value.y), value.z);
    if constexpr (N == 4)
        return std::max(std::max(value.x, value.y), std::max(value.z, value.w));
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr T dot(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    if constexpr (N == 2)
        return a.x * b.x + a.y * b.y;
    if constexpr (N == 3)
        return (a.x * b.x + a.y * b.y) + a.z * b.z;
    if constexpr (N == 4)
        return ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w;
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr T length_squared(BasicVector<T, N> value) noexcept {
    return dot(value, value);
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr T distance_squared(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    return length_squared(b - a);
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> cross(BasicVector<T, 3> a, BasicVector<T, 3> b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <Real T, std::size_t N>
[[nodiscard]] constexpr bool is_finite(BasicVector<T, N> value) noexcept {
    if constexpr (N == 2)
        return __builtin_isfinite(value.x) && __builtin_isfinite(value.y);
    if constexpr (N == 3)
        return __builtin_isfinite(value.x) && __builtin_isfinite(value.y) && __builtin_isfinite(value.z);
    if constexpr (N == 4)
        return __builtin_isfinite(value.x) && __builtin_isfinite(value.y) && __builtin_isfinite(value.z) &&
               __builtin_isfinite(value.w);
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

template <Real T, std::size_t N>
[[nodiscard]] T distance(BasicVector<T, N> a, BasicVector<T, N> b) noexcept {
    return length(b - a);
}

// Zero and nonfinite values have no normalized result. Rescaling also handles
// finite values whose length cannot be represented in the component type.
template <Real T, std::size_t N>
[[nodiscard]] std::optional<BasicVector<T, N>> try_normalize(BasicVector<T, N> value) noexcept {
    if (!is_finite(value))
        return std::nullopt;
    const T scale = max_component(cwise_abs(value));
    if (scale == T{})
        return std::nullopt;
    value /= scale;
    return value / std::sqrt(length_squared(value));
}

// Same formula as source math.lerp: exact at t = 0 and t = 1 for finite values, not monotonic.
template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> lerp(BasicVector<T, N> a, BasicVector<T, N> b,
                                               std::type_identity_t<T> t) noexcept {
    const T u = T(1) - t;
    if constexpr (N == 2)
        return {a.x * u + b.x * t, a.y * u + b.y * t};
    if constexpr (N == 3)
        return {a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t};
    if constexpr (N == 4)
        return {a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t, a.w * u + b.w * t};
}

// Clamped [0, 1] linear interpolation; t uses source math.clamp semantics (NaN -> 0).
template <Real T, std::size_t N>
[[nodiscard]] constexpr BasicVector<T, N> lerp01(BasicVector<T, N> a, BasicVector<T, N> b,
                                                 std::type_identity_t<T> t) noexcept {
    return lerp(a, b, std::max(T{0}, std::min(t, T{1})));
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

#pragma once

#include "math/type.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace atelier::math {

template <class T>
concept Real = std::same_as<T, float> || std::same_as<T, double>;

// Named components stay compact in arrays. Arithmetic may use wider registers;
// the storage contract never permits a fourth-lane load from a Vector3 object.
template <Real T, std::size_t N>
struct BasicVector;
template <Real T>
struct BasicVector<T, 2> {
    using Scalar = T;
    static constexpr std::size_t dimensions = 2;
    T x{}, y{};
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
    T x{}, y{}, z{};
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
    T x{}, y{}, z{}, w{};
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

template <Real T>
struct BasicQuaternion {
    using Scalar = T;
    T x{}, y{}, z{}, w{1};
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

// Row-major, row vectors: p * local * parent; local TRS is S * R * T.
// Default construction is zero; identity() is explicit. Indices are [0,16).
template <Real T>
struct BasicMatrix4 {
    using Scalar = T;
    std::array<T, 16> elements{};
    constexpr T& operator[](std::size_t i) noexcept { return elements[i]; }
    constexpr const T& operator[](std::size_t i) const noexcept { return elements[i]; }
    constexpr T& operator()(std::size_t row, std::size_t column) noexcept { return elements[row * 4 + column]; }
    constexpr const T& operator()(std::size_t row, std::size_t column) const noexcept {
        return elements[row * 4 + column];
    }
    constexpr T* data() noexcept { return elements.data(); }
    constexpr const T* data() const noexcept { return elements.data(); }
    constexpr auto begin() noexcept { return elements.begin(); }
    constexpr auto begin() const noexcept { return elements.begin(); }
    constexpr auto end() noexcept { return elements.end(); }
    constexpr auto end() const noexcept { return elements.end(); }
    static constexpr std::size_t size() noexcept { return 16; }
    static constexpr BasicMatrix4 identity() noexcept { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; }
    constexpr bool operator==(const BasicMatrix4&) const = default;
};
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator+(BasicMatrix4<T> a, const BasicMatrix4<T>& b) noexcept {
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] += b[i];
    return a;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator-(BasicMatrix4<T> a, const BasicMatrix4<T>& b) noexcept {
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] -= b[i];
    return a;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator+(BasicMatrix4<T> value) noexcept {
    return value;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator-(BasicMatrix4<T> value) noexcept {
    for (T& component : value)
        component = -component;
    return value;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator*(BasicMatrix4<T> value, std::type_identity_t<T> scale) noexcept {
    for (T& component : value)
        component *= scale;
    return value;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator*(std::type_identity_t<T> scale, BasicMatrix4<T> value) noexcept {
    return value * scale;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator/(BasicMatrix4<T> value, std::type_identity_t<T> scale) noexcept {
    for (T& component : value)
        component /= scale;
    return value;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> operator*(const BasicMatrix4<T>& a, const BasicMatrix4<T>& b) noexcept {
    BasicMatrix4<T> result;
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result(row, column) = ((a(row, 0) * b(0, column) + a(row, 1) * b(1, column)) + a(row, 2) * b(2, column)) +
                                  a(row, 3) * b(3, column);
    return result;
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> transpose(const BasicMatrix4<T>& value) noexcept {
    BasicMatrix4<T> result;
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result(row, column) = value(column, row);
    return result;
}
template <Real T>
[[nodiscard]] constexpr BasicVector<T, 4> operator*(BasicVector<T, 4> v, const BasicMatrix4<T>& m) noexcept {
    return {((v.x * m[0] + v.y * m[4]) + v.z * m[8]) + v.w * m[12],
            ((v.x * m[1] + v.y * m[5]) + v.z * m[9]) + v.w * m[13],
            ((v.x * m[2] + v.y * m[6]) + v.z * m[10]) + v.w * m[14],
            ((v.x * m[3] + v.y * m[7]) + v.z * m[11]) + v.w * m[15]};
}
// Affine operations: projection requires an explicit homogeneous divide.
template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> transform_point(BasicVector<T, 3> point,
                                                          const BasicMatrix4<T>& affine) noexcept {
    const auto value = BasicVector<T, 4>{point.x, point.y, point.z, 1} * affine;
    return {value.x, value.y, value.z};
}
template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> transform_direction(BasicVector<T, 3> direction,
                                                              const BasicMatrix4<T>& affine) noexcept {
    return {(direction.x * affine[0] + direction.y * affine[4]) + direction.z * affine[8],
            (direction.x * affine[1] + direction.y * affine[5]) + direction.z * affine[9],
            (direction.x * affine[2] + direction.y * affine[6]) + direction.z * affine[10]};
}
// Requires a unit quaternion, like rotate().
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> rotation_matrix(BasicQuaternion<T> unit) noexcept {
    const auto [x, y, z, w] = unit;
    return {{1 - 2 * y * y - 2 * z * z, 2 * x * y + 2 * z * w, 2 * x * z - 2 * y * w, 0, 2 * x * y - 2 * z * w,
             1 - 2 * x * x - 2 * z * z, 2 * y * z + 2 * x * w, 0, 2 * x * z + 2 * y * w, 2 * y * z - 2 * x * w,
             1 - 2 * x * x - 2 * y * y, 0, 0, 0, 0, 1}};
}
template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> trs_matrix(BasicVector<T, 3> position, BasicQuaternion<T> unit_rotation,
                                                   BasicVector<T, 3> scale) noexcept {
    auto result = rotation_matrix(unit_rotation);
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            result(row, column) *= scale[row];
    result[12] = position.x;
    result[13] = position.y;
    result[14] = position.z;
    return result;
}
template <Real T>
[[nodiscard]] bool is_finite(const BasicMatrix4<T>& value) noexcept {
    for (T component : value)
        if (!std::isfinite(component))
            return false;
    return true;
}
// Row equilibration prevents elimination overflow for large finite coefficients
// without discarding independently small rows. Partial pivoting uses no absolute
// determinant epsilon. Null means singular/nonfinite, numerical overflow during
// elimination, or a result outside T's finite range; success does not estimate
// the condition number or guarantee accuracy for ill-conditioned matrices.
template <Real T>
[[nodiscard]] std::optional<BasicMatrix4<T>> try_inverse(const BasicMatrix4<T>& value) noexcept {
    if (!is_finite(value))
        return std::nullopt;
    std::array<std::array<double, 8>, 4> rows{};
    std::array<double, 4> scales{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column)
            scales[row] = std::max(scales[row], std::abs(static_cast<double>(value(row, column))));
        if (scales[row] == 0)
            return std::nullopt;
        for (std::size_t column = 0; column < 4; ++column)
            rows[row][column] = static_cast<double>(value(row, column)) / scales[row];
        rows[row][4 + row] = 1;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
                pivot = row;
        if (rows[pivot][column] == 0 || !std::isfinite(rows[pivot][column]))
            return std::nullopt;
        if (pivot != column)
            std::swap(rows[pivot], rows[column]);
        const double divisor = rows[column][column];
        for (double& component : rows[column]) {
            component /= divisor;
            if (!std::isfinite(component))
                return std::nullopt;
        }
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column)
                continue;
            const double factor = rows[row][column];
            for (std::size_t k = 0; k < 8; ++k) {
                rows[row][k] -= factor * rows[column][k];
                if (!std::isfinite(rows[row][k]))
                    return std::nullopt;
            }
        }
    }
    BasicMatrix4<T> result;
    // B = D^-1 A, so A^-1 = B^-1 D^-1: undo the ORIGINAL row scales on
    // inverse columns. Keep scales independent from Gaussian pivot row swaps.
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column) {
            const double component = rows[row][4 + column] / scales[column];
            if (!std::isfinite(component) || std::abs(component) > std::numeric_limits<T>::max())
                return std::nullopt;
            result(row, column) = static_cast<T>(component);
        }
    // An affine inverse has this exact last column. Elimination can pivot through
    // the translated row and leave tiny projective terms; retain the input's
    // structural invariant so affine consumers need not accept projective data.
    if (value(0, 3) == 0 && value(1, 3) == 0 && value(2, 3) == 0 && value(3, 3) == 1) {
        result(0, 3) = result(1, 3) = result(2, 3) = T{};
        result(3, 3) = T{1};
    }
    return result;
}

// The normal transform a GPU consumes for an affine linear part (row-major 3x3,
// row vectors): sign(det) * cofactor matrix scaled by one common factor, which
// keeps the inverse-transpose row weights. Fails when the double determinant of
// the FP32 entries is zero (singular) or the scaled matrix becomes singular once
// FP32 flushes denormals to zero (unrepresentable). The renderer and producers of
// instance transforms share this test so neither accepts what the other rejects.
struct NormalTransform {
    std::array<float, 9> matrix;
    float mirror; // -1 when the linear part reflects, else 1.
};
enum class NormalTransformFailure { singular, unrepresentable };
[[nodiscard]] inline std::expected<NormalTransform, NormalTransformFailure>
normal_transform(const std::array<float, 9>& linear) noexcept {
    const double a = linear[0], b = linear[1], c = linear[2];
    const double d = linear[3], e = linear[4], f = linear[5];
    const double g = linear[6], h = linear[7], i = linear[8];
    // Double intermediates avoid overflow/underflow for finite float entries.
    const std::array<double, 9> cofactors{e * i - f * h, f * g - d * i, d * h - e * g, c * h - b * i, a * i - c * g,
                                          b * g - a * h, b * f - c * e, c * d - a * f, a * e - b * d};
    const double determinant = a * cofactors[0] + b * cofactors[1] + c * cofactors[2];
    if (determinant == 0)
        return std::unexpected(NormalTransformFailure::singular);
    double maximum = 0;
    for (double value : cofactors)
        maximum = std::max(maximum, std::abs(value));
    const double sign = determinant < 0 ? -1. : 1.;
    NormalTransform result{{}, determinant < 0 ? -1.f : 1.f};
    for (std::size_t n = 0; n < cofactors.size(); ++n) {
        result.matrix[n] = static_cast<float>(sign * cofactors[n] / maximum);
        if (std::abs(result.matrix[n]) < std::numeric_limits<float>::min())
            result.matrix[n] = 0;
    }
    const auto& m = result.matrix;
    const double gpu_determinant = double(m[0]) * (double(m[4]) * m[8] - double(m[5]) * m[7]) -
                                   double(m[1]) * (double(m[3]) * m[8] - double(m[5]) * m[6]) +
                                   double(m[2]) * (double(m[3]) * m[7] - double(m[4]) * m[6]);
    if (gpu_determinant == 0)
        return std::unexpected(NormalTransformFailure::unrepresentable);
    return result;
}

using Vector2 = BasicVector<float, 2>;
using Vector3 = BasicVector<float, 3>;
using Vector4 = BasicVector<float, 4>;
using Vector2d = BasicVector<double, 2>;
using Vector3d = BasicVector<double, 3>;
using Vector4d = BasicVector<double, 4>;
using Quaternion = BasicQuaternion<float>;
using Quaterniond = BasicQuaternion<double>;
using Matrix4 = BasicMatrix4<float>;
using Matrix4d = BasicMatrix4<double>;

template <class T>
inline constexpr Type type_of{};
template <Real T, std::size_t N>
    requires(N >= 2 && N <= 4)
inline constexpr Type type_of<BasicVector<T, N>>{N == 2   ? Shape::vector2
                                                 : N == 3 ? Shape::vector3
                                                          : Shape::vector4,
                                                 std::same_as<T, double> ? Precision::f64 : Precision::f32};
template <Real T>
inline constexpr Type type_of<BasicQuaternion<T>>{Shape::quaternion,
                                                  std::same_as<T, double> ? Precision::f64 : Precision::f32};
template <Real T>
inline constexpr Type type_of<BasicMatrix4<T>>{Shape::matrix4,
                                               std::same_as<T, double> ? Precision::f64 : Precision::f32};

static_assert(sizeof(Vector2) == 8 && sizeof(Vector3) == 12 && sizeof(Vector4) == 16);
static_assert(sizeof(Vector2d) == 16 && sizeof(Vector3d) == 24 && sizeof(Vector4d) == 32);
static_assert(alignof(Vector3) == alignof(float) && alignof(Vector3d) == alignof(double));
static_assert(sizeof(Quaternion) == 16 && sizeof(Quaterniond) == 32);
static_assert(sizeof(Matrix4) == 64 && sizeof(Matrix4d) == 128);
static_assert(std::is_standard_layout_v<Vector3> && std::is_trivially_copyable_v<Vector3>);
static_assert(std::is_standard_layout_v<Matrix4d> && std::is_trivially_copyable_v<Matrix4d>);

}

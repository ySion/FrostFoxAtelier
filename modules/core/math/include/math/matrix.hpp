#pragma once

#include "math/quaternion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <emmintrin.h>

namespace atelier::math {

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

    template <Real U>
        requires(!std::same_as<T, U>)
    explicit constexpr operator BasicMatrix4<U>() const noexcept {
        BasicMatrix4<U> result;
        for (std::size_t i = 0; i < 16; ++i)
            result[i] = static_cast<U>(elements[i]);
        return result;
    }

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
    if !consteval {
        if constexpr (std::same_as<T, float>) {
            const __m128 b0 = _mm_loadu_ps(&b[0]);
            const __m128 b1 = _mm_loadu_ps(&b[4]);
            const __m128 b2 = _mm_loadu_ps(&b[8]);
            const __m128 b3 = _mm_loadu_ps(&b[12]);
            for (std::size_t row = 0; row < 4; ++row) {
                __m128 acc = _mm_mul_ps(_mm_set1_ps(a(row, 0)), b0);
                acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(a(row, 1)), b1));
                acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(a(row, 2)), b2));
                acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(a(row, 3)), b3));
                _mm_storeu_ps(&result[row * 4], acc);
            }
            return result;
        } else {
            const __m128d b0_lo = _mm_loadu_pd(&b[0]);
            const __m128d b0_hi = _mm_loadu_pd(&b[2]);
            const __m128d b1_lo = _mm_loadu_pd(&b[4]);
            const __m128d b1_hi = _mm_loadu_pd(&b[6]);
            const __m128d b2_lo = _mm_loadu_pd(&b[8]);
            const __m128d b2_hi = _mm_loadu_pd(&b[10]);
            const __m128d b3_lo = _mm_loadu_pd(&b[12]);
            const __m128d b3_hi = _mm_loadu_pd(&b[14]);
            for (std::size_t row = 0; row < 4; ++row) {
                const __m128d a0 = _mm_set1_pd(a(row, 0));
                const __m128d a1 = _mm_set1_pd(a(row, 1));
                const __m128d a2 = _mm_set1_pd(a(row, 2));
                const __m128d a3 = _mm_set1_pd(a(row, 3));
                __m128d lo = _mm_add_pd(_mm_mul_pd(a0, b0_lo), _mm_mul_pd(a1, b1_lo));
                lo = _mm_add_pd(lo, _mm_mul_pd(a2, b2_lo));
                lo = _mm_add_pd(lo, _mm_mul_pd(a3, b3_lo));
                __m128d hi = _mm_add_pd(_mm_mul_pd(a0, b0_hi), _mm_mul_pd(a1, b1_hi));
                hi = _mm_add_pd(hi, _mm_mul_pd(a2, b2_hi));
                hi = _mm_add_pd(hi, _mm_mul_pd(a3, b3_hi));
                _mm_storeu_pd(&result[row * 4], lo);
                _mm_storeu_pd(&result[row * 4 + 2], hi);
            }
            return result;
        }
    }
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result(row, column) = ((a(row, 0) * b(0, column) + a(row, 1) * b(1, column)) + a(row, 2) * b(2, column)) +
                                  a(row, 3) * b(3, column);
    return result;
}

template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> transpose(const BasicMatrix4<T>& value) noexcept {
    BasicMatrix4<T> result;
    if !consteval {
        if constexpr (std::same_as<T, float>) {
            __m128 r0 = _mm_loadu_ps(&value[0]);
            __m128 r1 = _mm_loadu_ps(&value[4]);
            __m128 r2 = _mm_loadu_ps(&value[8]);
            __m128 r3 = _mm_loadu_ps(&value[12]);
            _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
            _mm_storeu_ps(&result[0], r0);
            _mm_storeu_ps(&result[4], r1);
            _mm_storeu_ps(&result[8], r2);
            _mm_storeu_ps(&result[12], r3);
            return result;
        } else {
            const __m128d r0_lo = _mm_loadu_pd(&value[0]);
            const __m128d r0_hi = _mm_loadu_pd(&value[2]);
            const __m128d r1_lo = _mm_loadu_pd(&value[4]);
            const __m128d r1_hi = _mm_loadu_pd(&value[6]);
            const __m128d r2_lo = _mm_loadu_pd(&value[8]);
            const __m128d r2_hi = _mm_loadu_pd(&value[10]);
            const __m128d r3_lo = _mm_loadu_pd(&value[12]);
            const __m128d r3_hi = _mm_loadu_pd(&value[14]);
            _mm_storeu_pd(&result[0], _mm_unpacklo_pd(r0_lo, r1_lo));
            _mm_storeu_pd(&result[2], _mm_unpacklo_pd(r2_lo, r3_lo));
            _mm_storeu_pd(&result[4], _mm_unpackhi_pd(r0_lo, r1_lo));
            _mm_storeu_pd(&result[6], _mm_unpackhi_pd(r2_lo, r3_lo));
            _mm_storeu_pd(&result[8], _mm_unpacklo_pd(r0_hi, r1_hi));
            _mm_storeu_pd(&result[10], _mm_unpacklo_pd(r2_hi, r3_hi));
            _mm_storeu_pd(&result[12], _mm_unpackhi_pd(r0_hi, r1_hi));
            _mm_storeu_pd(&result[14], _mm_unpackhi_pd(r2_hi, r3_hi));
            return result;
        }
    }
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            result(row, column) = value(column, row);
    return result;
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 4> operator*(BasicVector<T, 4> v, const BasicMatrix4<T>& m) noexcept {
    if !consteval {
        if constexpr (std::same_as<T, float>) {
            __m128 acc = _mm_mul_ps(_mm_set1_ps(v.x), _mm_loadu_ps(&m[0]));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(v.y), _mm_loadu_ps(&m[4])));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(v.z), _mm_loadu_ps(&m[8])));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(v.w), _mm_loadu_ps(&m[12])));
            BasicVector<float, 4> result;
            _mm_storeu_ps(&result.x, acc);
            return result;
        } else {
            const __m128d vx = _mm_set1_pd(v.x);
            const __m128d vy = _mm_set1_pd(v.y);
            const __m128d vz = _mm_set1_pd(v.z);
            const __m128d vw = _mm_set1_pd(v.w);
            __m128d lo = _mm_add_pd(_mm_mul_pd(vx, _mm_loadu_pd(&m[0])), _mm_mul_pd(vy, _mm_loadu_pd(&m[4])));
            lo = _mm_add_pd(lo, _mm_mul_pd(vz, _mm_loadu_pd(&m[8])));
            lo = _mm_add_pd(lo, _mm_mul_pd(vw, _mm_loadu_pd(&m[12])));
            __m128d hi = _mm_add_pd(_mm_mul_pd(vx, _mm_loadu_pd(&m[2])), _mm_mul_pd(vy, _mm_loadu_pd(&m[6])));
            hi = _mm_add_pd(hi, _mm_mul_pd(vz, _mm_loadu_pd(&m[10])));
            hi = _mm_add_pd(hi, _mm_mul_pd(vw, _mm_loadu_pd(&m[14])));
            BasicVector<double, 4> result;
            _mm_storeu_pd(&result.x, lo);
            _mm_storeu_pd(&result.z, hi);
            return result;
        }
    }
    return {((v.x * m[0] + v.y * m[4]) + v.z * m[8]) + v.w * m[12],
            ((v.x * m[1] + v.y * m[5]) + v.z * m[9]) + v.w * m[13],
            ((v.x * m[2] + v.y * m[6]) + v.z * m[10]) + v.w * m[14],
            ((v.x * m[3] + v.y * m[7]) + v.z * m[11]) + v.w * m[15]};
}

// Affine operations: projection requires an explicit homogeneous divide.
template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> transform_point(BasicVector<T, 3> point,
                                                          const BasicMatrix4<T>& affine) noexcept {
    if !consteval {
        if constexpr (std::same_as<T, float>) {
            __m128 acc = _mm_mul_ps(_mm_set1_ps(point.x), _mm_loadu_ps(&affine[0]));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(point.y), _mm_loadu_ps(&affine[4])));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(point.z), _mm_loadu_ps(&affine[8])));
            acc = _mm_add_ps(acc, _mm_loadu_ps(&affine[12]));
            alignas(16) float out[4];
            _mm_store_ps(out, acc);
            return {out[0], out[1], out[2]};
        } else {
            const __m128d px = _mm_set1_pd(point.x);
            const __m128d py = _mm_set1_pd(point.y);
            const __m128d pz = _mm_set1_pd(point.z);
            __m128d xy = _mm_add_pd(_mm_mul_pd(px, _mm_loadu_pd(&affine[0])), _mm_mul_pd(py, _mm_loadu_pd(&affine[4])));
            xy = _mm_add_pd(xy, _mm_mul_pd(pz, _mm_loadu_pd(&affine[8])));
            xy = _mm_add_pd(xy, _mm_loadu_pd(&affine[12]));
            alignas(16) double out[2];
            _mm_store_pd(out, xy);
            const double z = ((point.x * affine[2] + point.y * affine[6]) + point.z * affine[10]) + affine[14];
            return {out[0], out[1], z};
        }
    }
    return {((point.x * affine[0] + point.y * affine[4]) + point.z * affine[8]) + affine[12],
            ((point.x * affine[1] + point.y * affine[5]) + point.z * affine[9]) + affine[13],
            ((point.x * affine[2] + point.y * affine[6]) + point.z * affine[10]) + affine[14]};
}

template <Real T>
[[nodiscard]] constexpr BasicVector<T, 3> transform_direction(BasicVector<T, 3> direction,
                                                              const BasicMatrix4<T>& affine) noexcept {
    if !consteval {
        if constexpr (std::same_as<T, float>) {
            __m128 acc = _mm_mul_ps(_mm_set1_ps(direction.x), _mm_loadu_ps(&affine[0]));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(direction.y), _mm_loadu_ps(&affine[4])));
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(direction.z), _mm_loadu_ps(&affine[8])));
            alignas(16) float out[4];
            _mm_store_ps(out, acc);
            return {out[0], out[1], out[2]};
        } else {
            const __m128d dx = _mm_set1_pd(direction.x);
            const __m128d dy = _mm_set1_pd(direction.y);
            const __m128d dz = _mm_set1_pd(direction.z);
            __m128d xy = _mm_add_pd(_mm_mul_pd(dx, _mm_loadu_pd(&affine[0])), _mm_mul_pd(dy, _mm_loadu_pd(&affine[4])));
            xy = _mm_add_pd(xy, _mm_mul_pd(dz, _mm_loadu_pd(&affine[8])));
            alignas(16) double out[2];
            _mm_store_pd(out, xy);
            const double z = (direction.x * affine[2] + direction.y * affine[6]) + direction.z * affine[10];
            return {out[0], out[1], z};
        }
    }
    return {(direction.x * affine[0] + direction.y * affine[4]) + direction.z * affine[8],
            (direction.x * affine[1] + direction.y * affine[5]) + direction.z * affine[9],
            (direction.x * affine[2] + direction.y * affine[6]) + direction.z * affine[10]};
}

template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> translation_matrix(BasicVector<T, 3> offset) noexcept {
    return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, offset.x, offset.y, offset.z, 1}};
}

template <Real T>
[[nodiscard]] constexpr BasicMatrix4<T> scale_matrix(BasicVector<T, 3> scale) noexcept {
    return {{scale.x, 0, 0, 0, 0, scale.y, 0, 0, 0, 0, scale.z, 0, 0, 0, 0, 1}};
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

// Left-handed row-vector view matrix (+Z forward). Rejects nonfinite inputs and collinear forward/up.
template <Real T>
[[nodiscard]] std::optional<BasicMatrix4<T>> try_look_to_lh(BasicVector<T, 3> eye, BasicVector<T, 3> forward,
                                                            BasicVector<T, 3> up) noexcept {
    if (!is_finite(eye))
        return std::nullopt;
    const auto z = try_normalize(forward);
    if (!z)
        return std::nullopt;
    const auto x = try_normalize(cross(up, *z));
    if (!x)
        return std::nullopt;
    const auto y = cross(*z, *x);
    const T tx = -dot(eye, *x);
    const T ty = -dot(eye, y);
    const T tz = -dot(eye, *z);
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz))
        return std::nullopt;
    return BasicMatrix4<T>{{x->x, y.x, z->x, 0, x->y, y.y, z->y, 0, x->z, y.z, z->z, 0, tx, ty, tz, 1}};
}

template <Real T>
[[nodiscard]] std::optional<BasicMatrix4<T>> try_look_at_lh(BasicVector<T, 3> eye, BasicVector<T, 3> target,
                                                            BasicVector<T, 3> up) noexcept {
    if (!is_finite(target))
        return std::nullopt;
    return try_look_to_lh(eye, target - eye, up);
}

template <Real T>
[[nodiscard]] constexpr bool is_finite(const BasicMatrix4<T>& value) noexcept {
    for (T component : value)
        if (!__builtin_isfinite(component))
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
    const double a = linear[0];
    const double b = linear[1];
    const double c = linear[2];
    const double d = linear[3];
    const double e = linear[4];
    const double f = linear[5];
    const double g = linear[6];
    const double h = linear[7];
    const double i = linear[8];
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

using Matrix4 = BasicMatrix4<float>;

using Matrix4d = BasicMatrix4<double>;

template <Real T>
inline constexpr Type type_of<BasicMatrix4<T>>{Shape::matrix4,
                                               std::same_as<T, double> ? Precision::f64 : Precision::f32};

static_assert(sizeof(Matrix4) == 64 && sizeof(Matrix4d) == 128);

static_assert(std::is_standard_layout_v<Matrix4d> && std::is_trivially_copyable_v<Matrix4d>);

}

#include "world/math.hpp"
#include <cmath>
#include <algorithm>

namespace atelier::world {

Result<Trs> canonical_transform(Trs value) {
    const auto& q = value.rotation;
    if (!math::is_finite(value.position) || !math::is_finite(value.scale) || !math::is_finite(q))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Transform requires finite values."});
    // Rounding each component of a unit quaternion to float moves norm² (summed in double) by about 2·2^-24 = 2^-23;
    // 2^-22 doubles that. A value within it stays: normalizing it again can move a component by an ulp on every call.
    const double squared = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
    if (std::abs(squared - 1.) <= 0x1p-22)
        return value;
    const auto unit = math::try_normalize(math::Quaterniond(q));
    if (!unit)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Transform rotation must be a nonzero quaternion."});
    value.rotation = math::Quaternion(*unit);
    return value;
}

math::Matrix4 transform_matrix(const Trs& value) noexcept {
    // Re-normalize in double: storing the canonical quaternion in float can put
    // its squared norm just above one, overflowing a finite maximum scale.
    const auto& q = value.rotation;
    const double inverse_norm = 1. / std::sqrt(math::dot(math::Quaterniond(q), math::Quaterniond(q)));
    const math::Quaterniond unit{q.x * inverse_norm, q.y * inverse_norm, q.z * inverse_norm, q.w * inverse_norm};
    const auto wide = math::trs_matrix(math::Vector3d(value.position), unit, math::Vector3d(value.scale));
    return math::Matrix4(wide);
}

Result<math::Matrix4> multiply(const math::Matrix4& a, const math::Matrix4& b) {
    math::Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t col = 0; col < 4; ++col) {
            double value = 0;
            for (std::size_t k = 0; k < 4; ++k)
                value += double(a[row * 4 + k]) * b[k * 4 + col];
            result[row * 4 + col] = static_cast<float>(value);
            if (!std::isfinite(result[row * 4 + col]))
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "World transform exceeds finite matrix range."});
        }
    return result;
}

bool singular(const math::Matrix4& m) noexcept {
    const double a = m[0];
    const double b = m[1];
    const double c = m[2];
    const double d = m[4];
    const double e = m[5];
    const double f = m[6];
    const double g = m[8];
    const double h = m[9];
    const double i = m[10];
    return a * (e * i - f * h) + b * (f * g - d * i) + c * (d * h - e * g) == 0;
}

Result<Trs> relative_transform(const math::Matrix4& world, const math::Matrix4& parent, math::Vector3 preferred) {
    const auto invalid = [] {
        return std::unexpected(
            Error{ErrorCode::argument_mismatch,
                  "Keeping the world transform requires a finite, nonsingular local TRS without shear."});
    };
    for (float value : world)
        if (!std::isfinite(value))
            return invalid();
    for (float value : parent)
        if (!std::isfinite(value))
            return invalid();
    for (const auto* matrix : {&world, &parent})
        if ((*matrix)[3] != 0 || (*matrix)[7] != 0 || (*matrix)[11] != 0 || (*matrix)[15] != 1)
            return invalid();
    const double a = parent[0];
    const double b = parent[1];
    const double c = parent[2];
    const double d = parent[4];
    const double e = parent[5];
    const double f = parent[6];
    const double g = parent[8];
    const double h = parent[9];
    const double i = parent[10];
    const std::array<double, 9> cof{e * i - f * h, f * g - d * i, d * h - e * g, c * h - b * i, a * i - c * g,
                                    b * g - a * h, b * f - c * e, c * d - a * f, a * e - b * d};
    const double determinant = a * cof[0] + b * cof[1] + c * cof[2];
    if (determinant == 0)
        return invalid();
    std::array<double, 9> local{};
    std::array<double, 3> position{};
    std::array<double, 3> scale{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < 3; ++col)
            for (std::size_t k = 0; k < 3; ++k)
                local[row * 3 + col] += double(world[row * 4 + k]) * cof[col * 3 + k] / determinant;
    for (std::size_t col = 0; col < 3; ++col)
        for (std::size_t k = 0; k < 3; ++k)
            position[col] += (double(world[12 + k]) - parent[12 + k]) * cof[col * 3 + k] / determinant;
    const double handedness = local[0] * (local[4] * local[8] - local[5] * local[7]) -
                              local[1] * (local[3] * local[8] - local[5] * local[6]) +
                              local[2] * (local[3] * local[7] - local[4] * local[6]);
    // The rotation stays proper: a reflection is carried by an odd number of
    // negative scale axes, preferably the ones the caller already uses.
    std::array<double, 3> sign{preferred.x < 0 ? -1. : 1., preferred.y < 0 ? -1. : 1., preferred.z < 0 ? -1. : 1.};
    if ((sign[0] * sign[1] * sign[2] < 0) != (handedness < 0))
        sign = {handedness < 0 ? -1. : 1., 1., 1.};
    for (std::size_t row = 0; row < 3; ++row) {
        scale[row] = sign[row] * std::hypot(local[row * 3], local[row * 3 + 1], local[row * 3 + 2]);
        if (!std::isfinite(scale[row]) || scale[row] == 0)
            return invalid();
        for (std::size_t col = 0; col < 3; ++col)
            local[row * 3 + col] /= scale[row];
    }
    // Float storage permits rounding, but does not silently discard visible shear.
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t other = row + 1; other < 3; ++other) {
            double dot = 0;
            for (std::size_t col = 0; col < 3; ++col)
                dot += local[row * 3 + col] * local[other * 3 + col];
            if (std::abs(dot) > 1e-6)
                return invalid();
        }
    const double orientation = local[0] * (local[4] * local[8] - local[5] * local[7]) -
                               local[1] * (local[3] * local[8] - local[5] * local[6]) +
                               local[2] * (local[3] * local[7] - local[4] * local[6]);
    if (orientation <= 0)
        return invalid();
    // Transpose the row-vector rotation into the conventional column-vector form.
    const auto m = [&](int row, int col) { return local[std::size_t(col * 3 + row)]; };
    double x{};
    double y{};
    double z{};
    double w{};
    const double trace = m(0, 0) + m(1, 1) + m(2, 2);
    if (trace > 0) {
        const double s = 2 * std::sqrt(trace + 1);
        w = s / 4;
        x = (m(2, 1) - m(1, 2)) / s;
        y = (m(0, 2) - m(2, 0)) / s;
        z = (m(1, 0) - m(0, 1)) / s;
    } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
        const double s = 2 * std::sqrt(1 + m(0, 0) - m(1, 1) - m(2, 2));
        x = s / 4;
        y = (m(0, 1) + m(1, 0)) / s;
        z = (m(0, 2) + m(2, 0)) / s;
        w = (m(2, 1) - m(1, 2)) / s;
    } else if (m(1, 1) > m(2, 2)) {
        const double s = 2 * std::sqrt(1 + m(1, 1) - m(0, 0) - m(2, 2));
        y = s / 4;
        x = (m(0, 1) + m(1, 0)) / s;
        z = (m(1, 2) + m(2, 1)) / s;
        w = (m(0, 2) - m(2, 0)) / s;
    } else {
        const double s = 2 * std::sqrt(1 + m(2, 2) - m(0, 0) - m(1, 1));
        z = s / 4;
        x = (m(0, 2) + m(2, 0)) / s;
        y = (m(1, 2) + m(2, 1)) / s;
        w = (m(1, 0) - m(0, 1)) / s;
    }
    return canonical_transform({{float(position[0]), float(position[1]), float(position[2])},
                                {float(x), float(y), float(z), float(w)},
                                {float(scale[0]), float(scale[1]), float(scale[2])}});
}

}

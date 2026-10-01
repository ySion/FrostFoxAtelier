#include "world/math.hpp"
#include <cmath>
#include <algorithm>

namespace atelier::world {

Result<Trs> canonical_transform(Trs value) {
    const auto finite = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    const auto& q = value.rotation;
    if (!finite(value.position) || !finite(value.scale) ||
        value.scale.x <= 0.f || value.scale.y <= 0.f || value.scale.z <= 0.f ||
        !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Transform requires finite values and strictly positive scale."});
    const double norm = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
    if (norm == 0.)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Transform rotation must be a nonzero quaternion."});
    value.rotation = {float(q.x / norm), float(q.y / norm), float(q.z / norm), float(q.w / norm)};
    return value;
}

Matrix4 transform_matrix(const Trs& value) noexcept {
    // Re-normalize in double: storing the canonical quaternion in float can put
    // its squared norm just above one, overflowing a finite maximum scale.
    double x = value.rotation.x, y = value.rotation.y, z = value.rotation.z, w = value.rotation.w;
    const double inverse_norm = 1. / std::sqrt(x*x + y*y + z*z + w*w);
    x *= inverse_norm; y *= inverse_norm; z *= inverse_norm; w *= inverse_norm;
    const double sx = value.scale.x, sy = value.scale.y, sz = value.scale.z;
    return {
        float(sx * (1 - 2*y*y - 2*z*z)), float(sx * (2*x*y + 2*z*w)), float(sx * (2*x*z - 2*y*w)), 0.f,
        float(sy * (2*x*y - 2*z*w)), float(sy * (1 - 2*x*x - 2*z*z)), float(sy * (2*y*z + 2*x*w)), 0.f,
        float(sz * (2*x*z + 2*y*w)), float(sz * (2*y*z - 2*x*w)), float(sz * (1 - 2*x*x - 2*y*y)), 0.f,
        value.position.x, value.position.y, value.position.z, 1.f
    };
}

Result<Matrix4> multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t col = 0; col < 4; ++col) {
        double value = 0;
        for (std::size_t k = 0; k < 4; ++k) value += double(a[row * 4 + k]) * b[k * 4 + col];
        result[row * 4 + col] = static_cast<float>(value);
        if (!std::isfinite(result[row * 4 + col]))
            return std::unexpected(Error{ErrorCode::argument_mismatch, "World transform exceeds finite matrix range."});
    }
    return result;
}

Result<Trs> relative_transform(const Matrix4& world, const Matrix4& parent) {
    const auto invalid = [] { return std::unexpected(Error{ErrorCode::argument_mismatch,
        "Keeping the world transform requires a finite local TRS without shear or reflection."}); };
    for (float value : world) if (!std::isfinite(value)) return invalid();
    for (float value : parent) if (!std::isfinite(value)) return invalid();
    for (const auto* matrix : {&world, &parent})
        if ((*matrix)[3] != 0 || (*matrix)[7] != 0 || (*matrix)[11] != 0 || (*matrix)[15] != 1) return invalid();
    const double a = parent[0], b = parent[1], c = parent[2];
    const double d = parent[4], e = parent[5], f = parent[6];
    const double g = parent[8], h = parent[9], i = parent[10];
    const std::array<double, 9> cof{e*i-f*h, f*g-d*i, d*h-e*g,
        c*h-b*i, a*i-c*g, b*g-a*h, b*f-c*e, c*d-a*f, a*e-b*d};
    const double determinant = a*cof[0] + b*cof[1] + c*cof[2];
    if (determinant == 0) return invalid();
    std::array<double, 9> local{};
    std::array<double, 3> position{}, scale{};
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t col = 0; col < 3; ++col)
        for (std::size_t k = 0; k < 3; ++k)
            local[row * 3 + col] += double(world[row * 4 + k]) * cof[col * 3 + k] / determinant;
    for (std::size_t col = 0; col < 3; ++col) for (std::size_t k = 0; k < 3; ++k)
        position[col] += (double(world[12 + k]) - parent[12 + k]) * cof[col * 3 + k] / determinant;
    for (std::size_t row = 0; row < 3; ++row) {
        scale[row] = std::hypot(local[row * 3], local[row * 3 + 1], local[row * 3 + 2]);
        if (!std::isfinite(scale[row]) || scale[row] == 0) return invalid();
        for (std::size_t col = 0; col < 3; ++col) local[row * 3 + col] /= scale[row];
    }
    // Float storage permits rounding, but does not silently discard visible shear.
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t other = row + 1; other < 3; ++other) {
        double dot = 0;
        for (std::size_t col = 0; col < 3; ++col) dot += local[row * 3 + col] * local[other * 3 + col];
        if (std::abs(dot) > 1e-6) return invalid();
    }
    const double orientation = local[0]*(local[4]*local[8]-local[5]*local[7])
        -local[1]*(local[3]*local[8]-local[5]*local[6])+local[2]*(local[3]*local[7]-local[4]*local[6]);
    if (orientation <= 0) return invalid();
    // Transpose the row-vector rotation into the conventional column-vector form.
    const auto m = [&](int row, int col) { return local[std::size_t(col * 3 + row)]; };
    double x{}, y{}, z{}, w{};
    const double trace = m(0,0)+m(1,1)+m(2,2);
    if (trace > 0) {
        const double s = 2*std::sqrt(trace+1); w=s/4;
        x=(m(2,1)-m(1,2))/s; y=(m(0,2)-m(2,0))/s; z=(m(1,0)-m(0,1))/s;
    } else if (m(0,0)>m(1,1) && m(0,0)>m(2,2)) {
        const double s=2*std::sqrt(1+m(0,0)-m(1,1)-m(2,2)); x=s/4;
        y=(m(0,1)+m(1,0))/s; z=(m(0,2)+m(2,0))/s; w=(m(2,1)-m(1,2))/s;
    } else if (m(1,1)>m(2,2)) {
        const double s=2*std::sqrt(1+m(1,1)-m(0,0)-m(2,2)); y=s/4;
        x=(m(0,1)+m(1,0))/s; z=(m(1,2)+m(2,1))/s; w=(m(0,2)-m(2,0))/s;
    } else {
        const double s=2*std::sqrt(1+m(2,2)-m(0,0)-m(1,1)); z=s/4;
        x=(m(0,2)+m(2,0))/s; y=(m(1,2)+m(2,1))/s; w=(m(1,0)-m(0,1))/s;
    }
    return canonical_transform({{float(position[0]),float(position[1]),float(position[2])},
        {float(x),float(y),float(z),float(w)}, {float(scale[0]),float(scale[1]),float(scale[2])}});
}

}

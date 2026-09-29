#include "world/math.hpp"
#include <cmath>

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

}

#include "world/math.hpp"
#include <cmath>

namespace atelier::world {

Result<Trs> canonical_transform(Trs value) {
    const auto& q = value.rotation;
    if (!math::is_finite(value.position) || !math::is_finite(value.scale) ||
        value.scale.x <= 0.f || value.scale.y <= 0.f || value.scale.z <= 0.f ||
        !math::is_finite(q))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Transform requires finite values and strictly positive scale."});
    const auto unit = math::try_normalize(math::Quaterniond{q.x, q.y, q.z, q.w});
    if (!unit)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Transform rotation must be a nonzero quaternion."});
    value.rotation = {float(unit->x), float(unit->y), float(unit->z), float(unit->w)};
    return value;
}

math::Matrix4 transform_matrix(const Trs& value) noexcept {
    // Re-normalize in double: storing the canonical quaternion in float can put
    // its squared norm just above one, overflowing a finite maximum scale.
    const auto& q = value.rotation;
    const double inverse_norm = 1. / std::sqrt(double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w);
    const math::Quaterniond unit{q.x*inverse_norm, q.y*inverse_norm, q.z*inverse_norm, q.w*inverse_norm};
    const auto wide = math::trs_matrix(math::Vector3d{value.position.x, value.position.y, value.position.z}, unit,
        math::Vector3d{value.scale.x, value.scale.y, value.scale.z});
    math::Matrix4 result;
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<float>(wide[i]);
    return result;
}

}

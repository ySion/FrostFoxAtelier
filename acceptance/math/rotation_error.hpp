#pragma once
#include "math/quaternion.hpp"

namespace atelier::acceptance {
template <math::Real T>
double rotation_error(math::BasicQuaternion<T> expected, math::BasicQuaternion<T> actual) {
    const auto delta =
        math::conjugate(static_cast<math::Quaterniond>(expected)) * static_cast<math::Quaterniond>(actual);
    return 2.0 * std::atan2(math::length(math::Vector3d{delta.x, delta.y, delta.z}), std::abs(delta.w));
}
}

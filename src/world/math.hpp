#pragma once

#include "math/math.hpp"
#include "runtime/runtime.hpp"

namespace atelier::world {

struct Trs {
    math::Vector3 position;
    math::Quaternion rotation;
    math::Vector3 scale{1.f, 1.f, 1.f};
    bool operator==(const Trs&) const = default;
};
// World policy: finite position/rotation and strictly positive scale. Math
// values themselves have no World dependency or restricted scale convention.
[[nodiscard]] Result<Trs> canonical_transform(Trs);
// Requires a transform accepted by canonical_transform.
[[nodiscard]] math::Matrix4 transform_matrix(const Trs&) noexcept;
[[nodiscard]] Result<math::Matrix4> multiply(const math::Matrix4&, const math::Matrix4&);
// Fails on singular parents, overflow, reflection or shear that local TRS cannot represent.
[[nodiscard]] Result<Trs> relative_transform(const math::Matrix4& world, const math::Matrix4& parent);

}

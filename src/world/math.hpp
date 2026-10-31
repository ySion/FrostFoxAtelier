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
// World policy: finite position, rotation and scale. A negative scale axis
// mirrors; a zero axis is stored but makes the space degenerate.
[[nodiscard]] Result<Trs> canonical_transform(Trs);
// Requires a transform accepted by canonical_transform.
[[nodiscard]] math::Matrix4 transform_matrix(const Trs&) noexcept;
[[nodiscard]] Result<math::Matrix4> multiply(const math::Matrix4&, const math::Matrix4&);
// Fails on singular matrices, overflow or shear that local TRS cannot represent.
// A reflection takes the scale signs of `preferred` when their parity matches
// (keeping an object's own mirrored axes), otherwise it negates X.
[[nodiscard]] Result<Trs> relative_transform(const math::Matrix4& world, const math::Matrix4& parent,
                                             math::Vector3 preferred = {1.f, 1.f, 1.f});
// True when the 3x3 part has a zero determinant, as the renderer and physics require.
[[nodiscard]] bool singular(const math::Matrix4&) noexcept;

}

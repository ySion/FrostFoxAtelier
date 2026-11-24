#pragma once

#include "procedural/geometry.hpp"

namespace atelier::procedural::detail {

struct BevelPlane {
    math::Vector3d normal;

    double offset;
};

// Equal-angle stations. Reversing the edge and station index gives the same point.
math::Vector3d bevel_arc(math::Vector3d a, math::Vector3d b, std::uint32_t index, std::uint32_t segments) noexcept;

// Coordinates are on the unit sphere around the inward-offset corner. The caller
// supplies adjacent exterior face normals in winding order. The returned planes
// have dot(normal, p) <= offset on the retained side. An empty result means
// the incident strip planes already close this chamfer.
Result<Array<BevelPlane>> bevel_corner_planes(std::span<const math::Vector3d> normals, std::uint32_t segments);

}

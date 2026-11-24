#pragma once

#include "procedural/geometry.hpp"

namespace atelier::procedural::detail {

struct BevelOffsetCell {
    math::Vector3d center;
    Array<std::uint32_t> faces;
};

// Vertices of the inward-offset corner (n[i]·delta <= offsets[i]) and their
// incident support indices in the input fan order. Unit normals and distances
// are expressed relative to the original vertex. Tolerance is a plane distance.
Result<Array<BevelOffsetCell>> bevel_offset_cells(std::span<const math::Vector3d> normals,
                                                 std::span<const double> offsets, double tolerance);

}

#pragma once

#include "procedural/geometry.hpp"

namespace atelier::procedural::detail {

struct BevelFace {
    Array<std::uint32_t> points;
    std::uint32_t donor;
    bool original;
    std::uint32_t feature;
};

// A convex, outward-wound boundary in component-local coordinates. Point IDs survive
// all cuts, including the unused points left behind by a cut.
struct BevelHull {
    Array<math::Vector3d> points;
    Array<BevelFace> faces;

    // Keep dot(unit normal, point) <= offset. A supporting plane returns false.
    // Only a successful cut appends a cap. Discard the work hull on any error.
    Result<bool> clip(math::Vector3d normal, double offset, double tolerance, std::uint32_t donor,
                      std::uint32_t feature);
};

}

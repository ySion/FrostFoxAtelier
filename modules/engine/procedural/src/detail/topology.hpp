#pragma once

#include "procedural/geometry.hpp"

namespace atelier::procedural::detail {

// Every boundary edge has two opposite uses and each used point has one closed fan.
Result<void> validate_shells(const Geometry& surface, Array<std::uint32_t>* next_around_point = nullptr);

// Dense component IDs in point order, including isolated points.
Array<std::uint32_t> point_pieces(const Geometry& geometry);

}

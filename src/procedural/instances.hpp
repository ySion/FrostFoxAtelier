#pragma once

#include "procedural/geometry.hpp"

#include <array>
#include <memory>
#include <vector>

namespace atelier::procedural {

// Row-major 3x4 affine in the row-vector convention: the x, y and z axis rows, then
// the translation row, 3 floats each. The fourth column is (0, 0, 0, 1) by construction.
using Affine = std::array<float, 12>;

// The instances of one reference whose translations fall in one cube of the
// generator's local space, in instance order.
struct Collection {
    Reference reference;

    std::array<std::int32_t, 3> chunk;

    std::shared_ptr<const std::vector<Affine>> transforms; // Never empty.
};

struct Publication {
    std::vector<Collection> collections; // Ordered by reference key, then chunk.

    std::uint64_t dropped{}; // Degenerate instances left out.
};

// Groups the instance layer by reference and chunk (floor(translation / chunk_size)).
// An instance whose linear part fails math::normal_transform (zero scale, flattened) is
// dropped, so a degenerate local transform never makes the renderer reject a frame. A collection
// whose bytes equal the previous publication's for the same reference and chunk reuses
// its transforms pointer, so consumers keyed by that pointer keep their state. Fails for
// a chunk size that is not finite and positive, or a chunk index outside int32.
Result<Publication> publish(const Geometry& geometry, float chunk_size, const Publication* previous = nullptr);

}

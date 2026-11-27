#pragma once

#include "procedural/geometry.hpp"

namespace atelier::procedural::detail {

// Each face and point names an original primitive. Retained faces keep their own corner
// chart; a cut face uses its donor's chart and its own geometric normal.
Result<void> bevel_attributes(const Geometry& source, Geometry& output, Span<const std::uint32_t> primitive_sources,
                              Span<const std::uint8_t> original_faces, Span<const std::uint32_t> point_sources);

}

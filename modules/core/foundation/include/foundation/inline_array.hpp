#pragma once

#include "foundation/detail/inline_sequence.hpp"

namespace atelier {

// Starts empty in N inline slots, spills to heap, and returns inline on release
// or squeeze when the live prefix fits. Growth and moves invalidate borrows.
template <class T, std::size_t N>
using InlineArray = detail::InlineSequence<T, N, true>;

}

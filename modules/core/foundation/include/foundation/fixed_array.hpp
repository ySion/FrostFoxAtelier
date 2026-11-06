#pragma once

#include "foundation/detail/inline_sequence.hpp"

namespace atelier {

// Fixed capacity, variable length: only [0, size()) contains live T values.
// Never allocates; exceeding N is a contract violation, not a business error.
template <class T, std::size_t N>
using FixedArray = detail::InlineSequence<T, N, false>;

}

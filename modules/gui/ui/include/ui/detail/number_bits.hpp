#pragma once

#include <bit>
#include <cstdint>

namespace atelier::ui::detail {
// Snapshot identity preserves the sign of zero and the payload of a rejected NaN.
constexpr std::uint64_t number_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}
}

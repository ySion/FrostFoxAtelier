#include "foundation/hash.hpp"

#include <ankerl/unordered_dense.h>

namespace atelier {
std::size_t Hash<StringView>::operator()(StringView value) const noexcept {
    // Uses the pinned backend's public byte hash. No validation, allocation,
    // case folding or NUL scan: a query may contain arbitrary bytes.
    return ankerl::unordered_dense::hash<std::string_view>{}(value.std_view());
}
std::size_t Hash<StringView>::operator()(const char* value) const noexcept {
    return ankerl::unordered_dense::hash<std::string_view>{}(std::string_view{value ? value : ""});
}
}

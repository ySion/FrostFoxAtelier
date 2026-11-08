#pragma once
#include "memory/fatal.hpp"
#include <expected>
#include <string_view>
#include <utility>

namespace atelier::acceptance::helpers {
inline void require(bool condition, std::string_view message) noexcept {
    if (!condition)
        fatal(message);
}

template <class T, class E>
T take(std::expected<T, E> result) {
    if (!result)
        fatal(result.error().message);
    return std::move(*result);
}

template <class E>
void take(std::expected<void, E> result) {
    if (!result)
        fatal(result.error().message);
}
}

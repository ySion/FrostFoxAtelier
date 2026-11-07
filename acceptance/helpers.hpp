#pragma once
#include "runtime/runtime.hpp"
#include <stdexcept>
#include <utility>

namespace atelier::acceptance::helpers {
inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T, class E>
T take(std::expected<T, E> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

template <class E>
void take(std::expected<void, E> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
}

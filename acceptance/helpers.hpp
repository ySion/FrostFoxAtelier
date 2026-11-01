#pragma once
#include "runtime/runtime.hpp"
#include <stdexcept>
#include <utility>

namespace atelier::acceptance::helpers {
inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

inline void take(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
}

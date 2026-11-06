#pragma once
#include "runtime/runtime.hpp"

namespace atelier::editor::detail {
// Internal exception boundary for construction and graph assembly. Public
// Result APIs recover the owned Error without rebuilding it from what().
class Failure : public std::exception {
public:
    explicit Failure(Error value) : error(std::move(value)) {}

    const char* what() const noexcept override { return error.message.c_str(); }

    Error error;
};

inline Error caught_error(const std::exception& error, ErrorCode fallback) {
    if (const auto* failure = dynamic_cast<const Failure*>(&error))
        return failure->error;
    if (dynamic_cast<const std::bad_alloc*>(&error))
        return {ErrorCode::capacity_exceeded, error.what()};
    return {fallback, error.what()};
}
}

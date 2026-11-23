#pragma once

#include <expected>
#include <utility>

// Forward an error without changing its type or context. Evaluate once;
// borrow lvalues, move errors from temporaries. atelier_try_ is macro-local.
#define ATELIER_TRY(...)                                                                                               \
    do {                                                                                                               \
        if (auto&& atelier_try_ = (__VA_ARGS__); !atelier_try_)                                                        \
            return std::unexpected(std::forward<decltype(atelier_try_)>(atelier_try_).error());                        \
    } while (false)

// Keep the named Result and its lifetime, not just its value. Like a declaration,
// this macro needs a block when used under a conditional or loop.
#define ATELIER_TRY_BIND(name, ...)                                                                                    \
    auto name = (__VA_ARGS__);                                                                                         \
    if (!name)                                                                                                         \
    return std::unexpected(std::move(name).error())

// Evaluate the destination only after success. Move from a temporary Result, borrow from an lvalue.
#define ATELIER_TRY_ASSIGN(destination, ...)                                                                           \
    do {                                                                                                               \
        auto&& atelier_try_ = (__VA_ARGS__);                                                                           \
        if (!atelier_try_)                                                                                             \
            return std::unexpected(std::forward<decltype(atelier_try_)>(atelier_try_).error());                        \
        (destination) = *std::forward<decltype(atelier_try_)>(atelier_try_);                                           \
    } while (false)

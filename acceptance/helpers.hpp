#pragma once
#include "memory/fatal.hpp"
#include <concepts>
#include <expected>
#include <string_view>
#include <utility>

namespace atelier::acceptance::helpers {
struct RequireFn {
    void operator()(bool condition, std::string_view message) const noexcept {
        if (!condition)
            fatal(message);
    }

    template <class M>
        requires(!std::convertible_to<const M&, std::string_view>) && requires(const M& message) {
            { message.c_str() } -> std::convertible_to<const char*>;
            { message.size() } -> std::convertible_to<std::size_t>;
        }
    void operator()(bool condition, const M& message) const noexcept {
        if (!condition)
            fatal({message.c_str(), message.size()});
    }

    template <class M>
        requires(!std::convertible_to<const M&, std::string_view>) &&
                (!requires(const M& message) { message.c_str(); }) && requires(const M& message) {
                    { message.data() } -> std::convertible_to<const char*>;
                    { message.size() } -> std::convertible_to<std::size_t>;
                }
    void operator()(bool condition, const M& message) const noexcept {
        if (!condition)
            fatal({message.data(), message.size()});
    }
};

inline constexpr RequireFn require{};

template <class T, class E>
T take(std::expected<T, E> result) {
    if (!result) {
        if constexpr (requires { result.error().message; })
            fatal(result.error().message);
        else
            fatal("Unexpected error result in acceptance check.");
    }
    return std::move(*result);
}

template <class E>
void take(std::expected<void, E> result) {
    if (!result) {
        if constexpr (requires { result.error().message; })
            fatal(result.error().message);
        else
            fatal("Unexpected error result in acceptance check.");
    }
}
}

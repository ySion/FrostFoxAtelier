#pragma once

#include <functional>
#include <type_traits>
#include <utility>

namespace atelier {

// Owns one nonthrowing cleanup action. Moving transfers the obligation;
// release transfers it to the caller. Fatal termination does not run cleanup.
template <class F>
    requires std::is_nothrow_move_constructible_v<F> && std::is_nothrow_destructible_v<F> &&
             std::is_nothrow_invocable_v<F&>
class ScopeExit {
public:
    template <class Action>
        requires std::is_nothrow_constructible_v<F, Action>
    explicit ScopeExit(Action&& action) noexcept : action_(std::forward<Action>(action)) {}

    ScopeExit(const ScopeExit&) = delete;

    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept
        : action_(std::move(other.action_)), active_(std::exchange(other.active_, false)) {}

    ~ScopeExit() {
        if (active_)
            std::invoke(action_);
    }

    void release() noexcept { active_ = false; }

private:
    F action_;

    bool active_{true};
};

template <class F>
ScopeExit(F) -> ScopeExit<F>;

}

#pragma once

#include "memory/fatal.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace atelier {

// Parameter-only synchronous borrow. Temporary callables live through the full
// call expression. Never store this as a member or retain it after the call.
// Free functions are stored by value, without borrowing a pointer object.
template <class Signature>
class FunctionRef;

template <class R, class... Args, bool Noexcept>
class FunctionRef<R(Args...) noexcept(Noexcept)> {
    union Target {
        const volatile void* object;

        void (*function)();
    };

public:
    template <class F>
        requires std::is_function_v<F> && std::is_invocable_r_v<R, F*, Args...> &&
                 (!Noexcept || std::is_nothrow_invocable_r_v<R, F*, Args...>)
    FunctionRef(F* function) noexcept {
        if (!function)
            fatal("FunctionRef requires a callable.");
        // Only round-trip function-pointer casts; never convert a function to void*.
        target_.function = reinterpret_cast<void (*)()>(function);
        invoke_ = [](Target target, Args&&... args) noexcept(Noexcept) -> R {
            return std::invoke_r<R>(reinterpret_cast<F*>(target.function), std::forward<Args>(args)...);
        };
    }

    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, FunctionRef>) &&
                (!(std::is_pointer_v<std::decay_t<F>> && std::is_function_v<std::remove_pointer_t<std::decay_t<F>>>)) &&
                std::is_invocable_r_v<R, F&, Args...> && (!Noexcept || std::is_nothrow_invocable_r_v<R, F&, Args...>)
    FunctionRef(F&& function [[clang::lifetimebound]]) noexcept {
        using Callable = std::remove_reference_t<F>;
        target_.object = std::addressof(function);
        invoke_ = [](Target target, Args&&... args) noexcept(Noexcept) -> R {
            auto* callable = const_cast<Callable*>(static_cast<const volatile Callable*>(target.object));
            return std::invoke_r<R>(*callable, std::forward<Args>(args)...);
        };
    }

    R operator()(Args... args) const noexcept(Noexcept) { return invoke_(target_, std::forward<Args>(args)...); }

private:
    Target target_;

    R (*invoke_)(Target, Args&&...) noexcept(Noexcept);
};

}

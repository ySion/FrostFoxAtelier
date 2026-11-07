#pragma once

#include "memory/fatal.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace atelier {

template <class Signature>
class FunctionRef;

namespace detail {

template <class R, bool Noexcept, class... Args>
class FunctionReference {
    union Target {
        const volatile void* object;

        void (*function)();
    };

public:
    template <class F>
        requires(!std::is_base_of_v<FunctionReference, std::remove_cvref_t<F>>) &&
                std::is_invocable_r_v<R, F&, Args...> && (!Noexcept || std::is_nothrow_invocable_r_v<R, F&, Args...>)
    FunctionReference(F&& function) noexcept {
        using Callable = std::remove_reference_t<F>;
        using Pointer = std::decay_t<F>;
        if constexpr (std::is_pointer_v<Pointer> && std::is_function_v<std::remove_pointer_t<Pointer>>) {
            Pointer pointer = function;
            if (!pointer)
                fatal("FunctionRef requires a callable.");
            // Only round-trip function-pointer casts; never convert a function to void*.
            target_.function = reinterpret_cast<void (*)()>(pointer);
            invoke_ = [](Target target, Args&&... args) noexcept(Noexcept) -> R {
                return std::invoke_r<R>(reinterpret_cast<Pointer>(target.function), std::forward<Args>(args)...);
            };
        } else {
            target_.object = std::addressof(function);
            invoke_ = [](Target target, Args&&... args) noexcept(Noexcept) -> R {
                auto* function = const_cast<Callable*>(static_cast<const volatile Callable*>(target.object));
                return std::invoke_r<R>(*function, std::forward<Args>(args)...);
            };
        }
    }

    R operator()(Args... args) const noexcept(Noexcept) { return invoke_(target_, std::forward<Args>(args)...); }

private:
    Target target_;

    R (*invoke_)(Target, Args&&...) noexcept(Noexcept);
};

}

// Parameter-only synchronous borrow. Temporary callables live through the full
// call expression. Never store this as a member or retain it after the call.
// Free functions are stored by value, without borrowing a pointer object.
template <class R, class... Args>
class FunctionRef<R(Args...)> : public detail::FunctionReference<R, false, Args...> {
public:
    using detail::FunctionReference<R, false, Args...>::FunctionReference;
};

template <class R, class... Args>
class FunctionRef<R(Args...) noexcept> : public detail::FunctionReference<R, true, Args...> {
public:
    using detail::FunctionReference<R, true, Args...>::FunctionReference;
};

}

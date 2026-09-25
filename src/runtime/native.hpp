#pragma once

#include "runtime/runtime.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace atelier {
namespace detail {

template<class T>
concept ScalarType = std::same_as<T, float> || std::same_as<T, std::uint32_t>;

template<class> struct Member;
template<class V, class C> struct Member<V C::*> { using Value = V; using Owner = C; };

template<class> struct Method;
template<class R, class C, class... A>
struct Method<R (C::*)(A...)> {
    using Owner = C;
    using Arguments = std::tuple<A...>;
    static_assert(std::same_as<R, void>, "This slice supports void methods only.");
    static_assert((ScalarType<A> && ...), "Method arguments must be supported scalars passed by value.");
};
template<class R, class C, class... A>
struct Method<R (C::*)(A...) const> : Method<R (C::*)(A...)> {};
template<class R, class C, class... A>
struct Method<R (C::*)(A...) noexcept> : Method<R (C::*)(A...)> {};
template<class R, class C, class... A>
struct Method<R (C::*)(A...) const noexcept> : Method<R (C::*)(A...)> {};

}

// A producer of TypeDefinition, with no registry or object storage of its own.
template<class T>
class NativeRegistration {
    static_assert(std::default_initializable<T> && std::is_nothrow_destructible_v<T>);
public:
    NativeRegistration(Runtime& runtime, std::string name, TypeKind kind)
        : runtime_(runtime) {
        definition_.name = std::move(name);
        definition_.kind = kind;
        definition_.native = {
            sizeof(T), alignof(T), detail::representation<T>(),
            [](void* address) { std::construct_at(static_cast<T*>(address)); },
            [](void* address) noexcept { std::destroy_at(static_cast<T*>(address)); }
        };
        if (kind == TypeKind::record) {
            if constexpr (std::is_copy_constructible_v<T>) {
                definition_.native.copy_construct = [](void* destination, const void* source) {
                    std::construct_at(static_cast<T*>(destination), *static_cast<const T*>(source));
                };
            }
            if constexpr (std::is_copy_assignable_v<T>) {
                definition_.native.copy_assign = [](void* destination, const void* source) {
                    *static_cast<T*>(destination) = *static_cast<const T*>(source);
                };
            }
        }
    }

    template<auto Pointer>
    NativeRegistration& field(std::string name) {
        using Value = typename detail::Member<decltype(Pointer)>::Value;
        return field<Pointer>(std::move(name), {TypeReference::native<Value>()});
    }

    // Explicit descriptions allow references to definitions in the same batch.
    template<auto Pointer>
    NativeRegistration& field(std::string name, ValueDefinition value) {
        using Traits = detail::Member<decltype(Pointer)>;
        using Value = typename Traits::Value;
        static_assert(std::same_as<typename Traits::Owner, T>);
        static_assert(!std::is_const_v<Value> && !std::is_volatile_v<Value>);
        definition_.fields.push_back({std::move(name), std::move(value),
            [](void* object) noexcept -> void* { return std::addressof(static_cast<T*>(object)->*Pointer); },
            detail::representation<Value>()});
        return *this;
    }

    template<auto Pointer>
    NativeRegistration& hidden_field(std::string name, ValueDefinition value) {
        field<Pointer>(std::move(name), std::move(value));
        definition_.fields.back().exposed = false;
        return *this;
    }

    template<auto Pointer>
    NativeRegistration& hidden_field(std::string name) {
        using Value = typename detail::Member<decltype(Pointer)>::Value;
        return hidden_field<Pointer>(std::move(name), {TypeReference::native<Value>()});
    }

    template<auto Pointer>
    NativeRegistration& method(std::string name) {
        using Traits = detail::Method<decltype(Pointer)>;
        static_assert(std::same_as<typename Traits::Owner, T>);
        using Args = typename Traits::Arguments;
        MethodDefinition method;
        method.name = std::move(name);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (method.parameters.push_back({TypeReference::native<std::tuple_element_t<I, Args>>()}), ...);
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        method.invoke = [](CallContext& call) -> Result<void> {
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                std::invoke(Pointer, *static_cast<T*>(call.storage),
                    std::get<std::tuple_element_t<I, Args>>(call.arguments[I])...);
            }(std::make_index_sequence<std::tuple_size_v<Args>>{});
            return {};
        };
        definition_.methods.push_back(std::move(method));
        return *this;
    }

    // Consumes the producer; validation belongs to Runtime, including batch lookup.
    [[nodiscard]] TypeDefinition build() { return std::move(definition_); }

    [[nodiscard]] Result<TypeId> publish() {
        return runtime_.register_type(build());
    }

private:

    Runtime& runtime_;
    TypeDefinition definition_;
};

}

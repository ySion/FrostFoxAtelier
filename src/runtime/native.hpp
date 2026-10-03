#pragma once

#include "runtime/runtime.hpp"
#include "math/math.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace atelier {
namespace detail {

template<class T>
concept ScalarType = std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, std::uint32_t> ||
    std::same_as<T, std::int32_t> || (std::is_enum_v<T> && sizeof(T) == 4);

template<class T>
concept MathValueType = static_cast<bool>(math::type_of<T>);

template<class T>
concept NativeParameter = (ScalarType<std::remove_cvref_t<T>> || MathValueType<std::remove_cvref_t<T>> ||
    std::is_lvalue_reference_v<T>) && !std::is_rvalue_reference_v<T> &&
    !std::is_volatile_v<std::remove_reference_t<T>>;
static_assert(sizeof(int) == 4);

template<class> struct Member;
template<class V, class C> struct Member<V C::*> { using Value = V; using Owner = C; };

template<class> struct Method;
template<class R, class C, class... A>
struct Method<R (C::*)(A...)> {
    using Owner = C;
    using Arguments = std::tuple<A...>;
    using Return = R;
    static constexpr bool receiver_read_only = false;
    static_assert(std::same_as<R, void> || ScalarType<R> || MathValueType<R>,
        "Method results must be void or supported scalar/math values returned by value.");
    static_assert((NativeParameter<A> && ...), "Method parameters require values or non-volatile lvalue references.");
};
template<class R, class C, class... A>
struct Method<R (C::*)(A...) const> : Method<R (C::*)(A...)> {
    static constexpr bool receiver_read_only = true;
};
template<class R, class C, class... A>
struct Method<R (C::*)(A...) noexcept> : Method<R (C::*)(A...)> {};
template<class R, class C, class... A>
struct Method<R (C::*)(A...) const noexcept> : Method<R (C::*)(A...) const> {};

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
        definition_.native.value_operations_preserve_borrows = std::is_trivially_default_constructible_v<T> &&
            std::is_trivially_copy_constructible_v<T> && std::is_trivially_copy_assignable_v<T> && std::is_trivially_destructible_v<T> &&
            std::is_trivially_constructible_v<T, decltype(std::move_if_noexcept(std::declval<T&>()))>;
        if constexpr (std::is_enum_v<T>) {
            definition_.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::underlying_type_t<T>>(), {}};
        }
        if (kind != TypeKind::object) {
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

    template<class U = T> requires std::is_enum_v<U> && std::same_as<U, T>
    NativeRegistration& enumerator(std::string name, T value) {
        using Underlying = std::underlying_type_t<T>;
        static_assert(std::same_as<Underlying, std::int32_t> || std::same_as<Underlying, std::uint32_t>,
            "Reflected enums require an i32/u32 underlying type.");
        if (!definition_.enumeration)
            definition_.enumeration = TypeDefinition::Enumeration{TypeReference::native<Underlying>(), {}};
        definition_.enumeration->values.emplace_back(std::move(name), static_cast<std::uint32_t>(value));
        return *this;
    }

    template<class Base> requires std::derived_from<T, Base>
    NativeRegistration& base() {
        definition_.base = TypeReference::native<Base>();
        definition_.native.base_address = [](void* storage) noexcept -> void* {
            return static_cast<Base*>(static_cast<T*>(storage));
        };
        return *this;
    }

    template<auto Pointer>
    NativeRegistration& field(std::string name) {
        using Value = typename detail::Member<decltype(Pointer)>::Value;
        return field<Pointer>(std::move(name), {TypeReference::native<Value>()});
    }

    // Explicit descriptions allow references to definitions in the same batch.
    template<auto Pointer>
    NativeRegistration& field(std::string name, ValueDefinition value, std::optional<std::size_t> offset = std::nullopt) {
        using Traits = detail::Member<decltype(Pointer)>;
        using Value = typename Traits::Value;
        static_assert(std::same_as<typename Traits::Owner, T>);
        static_assert(!std::is_const_v<Value> && !std::is_volatile_v<Value>);
        definition_.fields.push_back({std::move(name), std::move(value),
            [](void* object) noexcept -> void* { return std::addressof(static_cast<T*>(object)->*Pointer); },
            detail::representation<Value>(), true, offset});
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
    NativeRegistration& method(std::string name, bool preserves_borrows = false) {
        using Traits = detail::Method<decltype(Pointer)>;
        static_assert(std::same_as<typename Traits::Owner, T>);
        using Args = typename Traits::Arguments;
        using Return = typename Traits::Return;
        using Receiver = std::conditional_t<Traits::receiver_read_only, const T, T>;
        MethodDefinition method;
        method.name = std::move(name);
        method.preserves_borrows = preserves_borrows;
        method.signature.receiver = ReceiverDefinition{TypeReference::native<T>(), Traits::receiver_read_only};
        if constexpr (!std::same_as<Return, void>)
            method.signature.result = ValueDefinition{TypeReference::native<Return>()};
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (method.signature.parameters.push_back({{TypeReference::native<std::remove_cvref_t<std::tuple_element_t<I, Args>>>()},
                parameter_passing<std::tuple_element_t<I, Args>>}), ...);
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        method.invoke = [](CallContext& call) -> Result<void> {
            const auto invoke = [&]<std::size_t... I>(std::index_sequence<I...>) -> Return {
                return std::invoke(Pointer, *static_cast<Receiver*>(call.storage),
                    call.arguments[I].get<std::tuple_element_t<I, Args>>()...);
            };
            if constexpr (std::same_as<Return, void>) invoke(std::make_index_sequence<std::tuple_size_v<Args>>{});
            else if constexpr (detail::MathValueType<Return>) {
                const auto type = call.runtime.native_type<Return>();
                if (!type) return std::unexpected(type.error());
                if (call.result_value.type != *type || !call.result_value.data)
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Native math result requires exact typed output storage."});
                *static_cast<Return*>(call.result_value.data) = invoke(std::make_index_sequence<std::tuple_size_v<Args>>{});
                call.result_written = true;
            }
            else if constexpr (std::is_enum_v<Return>) {
                const auto type = call.runtime.native_type<Return>();
                if (!type) return std::unexpected(type.error());
                call.result = EnumValue{*type, static_cast<std::uint32_t>(invoke(std::make_index_sequence<std::tuple_size_v<Args>>{}))};
            } else call.result = invoke(std::make_index_sequence<std::tuple_size_v<Args>>{});
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

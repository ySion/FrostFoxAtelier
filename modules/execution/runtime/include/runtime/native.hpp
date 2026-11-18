#pragma once

#include "runtime/execution.hpp"
#include "math/math.hpp"

#include <concepts>
#include <cstdio>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace atelier {
namespace detail {

template <class T>
concept ScalarType = std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, std::uint32_t> ||
                     std::same_as<T, std::int32_t> || std::same_as<T, bool> || (std::is_enum_v<T> && sizeof(T) == 4);

template <class T>
concept MathValueType = static_cast<bool>(math::type_of<T>);

template <class F, class T>
concept NativeValidator =
    std::same_as<F, Result<void> (*)(const T&)> || std::same_as<F, Result<void> (*)(const T&) noexcept> ||
    std::same_as<F, Result<void> (T::*)() const> || std::same_as<F, Result<void> (T::*)() const noexcept>;

template <class T>
concept PlainNativeValue =
    std::is_class_v<T> && std::is_standard_layout_v<T> && std::is_trivially_copy_constructible_v<T> &&
    std::is_trivially_copy_assignable_v<T> && std::is_trivially_destructible_v<T>;

template <class T>
concept NativeParameter = (ScalarType<std::remove_cvref_t<T>> || MathValueType<std::remove_cvref_t<T>> ||
                           PlainNativeValue<std::remove_cvref_t<T>> || std::is_lvalue_reference_v<T>) &&
                          !std::is_rvalue_reference_v<T> && !std::is_volatile_v<std::remove_reference_t<T>>;

static_assert(sizeof(int) == 4);

template <class>
struct Member;

template <class V, class C>
struct Member<V C::*> {
    using Value = V;

    using Owner = C;
};

template <class R>
struct NativeResult {
    using Value = R;

    static constexpr bool fallible = false;
};

template <class R>
struct NativeResult<Result<R>> {
    using Value = R;

    static constexpr bool fallible = true;
};

template <class>
struct Method;

template <class R, class C, class... A>
struct Method<R (C::*)(A...)> {
    using Owner = C;

    using Arguments = std::tuple<A...>;

    using Return = typename NativeResult<R>::Value;

    static constexpr bool fallible = NativeResult<R>::fallible;

    static constexpr bool receiver_read_only = false;

    static constexpr bool contextual = false;

    static_assert(!NativeResult<Return>::fallible && (std::same_as<Return, void> || ScalarType<Return> ||
                                                      MathValueType<Return> || PlainNativeValue<Return>),
                  "Method results must be void or supported scalar/plain record values, optionally in Result.");

    static_assert((NativeParameter<A> && ...), "Method parameters require values or non-volatile lvalue references.");
};

template <class R, class C, class... A>
struct Method<R (C::*)(A...) const> : Method<R (C::*)(A...)> {
    static constexpr bool receiver_read_only = true;
};

template <class R, class C, class... A>
struct Method<R (C::*)(A...) noexcept> : Method<R (C::*)(A...)> {};

template <class R, class C, class... A>
struct Method<R (C::*)(A...) const noexcept> : Method<R (C::*)(A...) const> {};

// Contextual methods share the same producer and ABI as C++ member functions.
// Runtime/ObjectId are engine context, not reflected business parameters.
template <class R, class C, class... A>
struct Method<Result<R> (*)(Runtime&, ObjectId, C&, A...)> : Method<R (std::remove_const_t<C>::*)(A...)> {
    static constexpr bool receiver_read_only = std::is_const_v<C>;

    static constexpr bool contextual = true;
};

template <class R, class C, class... A>
struct Method<Result<R> (*)(Runtime&, ObjectId, C&, A...) noexcept>
    : Method<Result<R> (*)(Runtime&, ObjectId, C&, A...)> {};

template <class T>
using NativeArgument = std::conditional_t<
    std::is_lvalue_reference_v<T>, std::remove_reference_t<T>*,
    std::conditional_t<!ScalarType<T>, const std::remove_reference_t<T>*,
                       std::conditional_t<std::same_as<T, bool> || std::is_enum_v<T>, std::uint32_t, T>>>;

template <class T>
decltype(auto) native_argument(NativeArgument<T> value) {
    if constexpr (std::is_lvalue_reference_v<T>)
        return static_cast<T>(*value);
    else if constexpr (!ScalarType<T>)
        return T(*value);
    else
        return static_cast<T>(value);
}

template <auto Pointer, class... A>
auto invoke_native(Runtime& runtime, ObjectId self, void* storage, A&&... arguments)
    -> Result<typename Method<decltype(Pointer)>::Return> {
    using Traits = Method<decltype(Pointer)>;
    using Receiver =
        std::conditional_t<Traits::receiver_read_only, const typename Traits::Owner, typename Traits::Owner>;
    if constexpr (Traits::contextual)
        return std::invoke(Pointer, runtime, self, *static_cast<Receiver*>(storage), std::forward<A>(arguments)...);
    else if constexpr (std::same_as<typename Traits::Return, void> && !Traits::fallible) {
        std::invoke(Pointer, *static_cast<Receiver*>(storage), std::forward<A>(arguments)...);
        return {};
    } else
        return std::invoke(Pointer, *static_cast<Receiver*>(storage), std::forward<A>(arguments)...);
}

template <auto Pointer, class>
struct NativeThunk;

template <auto Pointer, class... A>
struct NativeThunk<Pointer, std::tuple<A...>> {
    static std::uint32_t invoke(NativeContext* context, void* receiver, NativeArgument<A>... arguments, void* output,
                                NativeFailure* failure) noexcept {
        NativeCallback scope{*context};
        const auto fail = [&](ErrorCode code, const char* text) {
            failure->code = static_cast<std::uint32_t>(code);
            std::snprintf(failure->message, sizeof(failure->message), "%s", text);
            return 1u;
        };
        auto result =
            invoke_native<Pointer>(*context->runtime, context->self, receiver, native_argument<A>(arguments)...);
        if (!result)
            return fail(result.error().code, result.error().message.c_str());
        using Return = typename Method<decltype(Pointer)>::Return;
        if constexpr (std::same_as<Return, bool> || std::is_enum_v<Return>)
            *static_cast<std::uint32_t*>(output) = static_cast<std::uint32_t>(*result);
        else if constexpr (!std::same_as<Return, void>)
            *static_cast<Return*>(output) = *result;
        return 0;
    }
};

}

// Produces both dynamic reflection and fixed native entry from one callable.
// Aggregate value inputs cross the generated ABI in independent aligned live
// storage; only the C++ thunk performs the platform's real by-value call.
template <auto Pointer>
MethodDefinition native_method(std::string name, bool preserves_borrows = false, std::string description = {}) {
    using Traits = detail::Method<decltype(Pointer)>;
    using Args = typename Traits::Arguments;
    using Return = typename Traits::Return;
    MethodDefinition method;
    method.name = std::move(name);
    method.preserves_borrows = preserves_borrows;
    method.description = std::move(description);
    method.signature.receiver =
        ReceiverDefinition{TypeReference::native<typename Traits::Owner>(), Traits::receiver_read_only};
    if constexpr (!std::same_as<Return, void>)
        method.signature.result = ValueDefinition{TypeReference::native<Return>()};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (method.signature.parameters.push_back(
             {{TypeReference::native<std::remove_cvref_t<std::tuple_element_t<I, Args>>>()},
              parameter_passing<std::tuple_element_t<I, Args>>}),
         ...);
    }(std::make_index_sequence<std::tuple_size_v<Args>>{});
    method.invoke = [](CallContext& call) noexcept -> Result<void> {
        const auto invoke = [&]<std::size_t... I>(std::index_sequence<I...>) {
            return detail::invoke_native<Pointer>(call.runtime, call.self, call.storage,
                                                  call.arguments[I].get<std::tuple_element_t<I, Args>>()...);
        };
        ATELIER_TRY_BIND(result, invoke(std::make_index_sequence<std::tuple_size_v<Args>>{}));
        if constexpr (detail::MathValueType<Return> || detail::PlainNativeValue<Return>) {
            TypeId expected = call.expected_result_type;
            if (expected == TypeId{}) [[unlikely]] {
                const auto type = call.runtime.native_type<Return>();
                if (!type)
                    return std::unexpected(type.error());
                expected = *type;
            }
            if (call.result_value.type != expected || !call.result_value.data)
                return std::unexpected(Error{ErrorCode::invalid_storage,
                                             "Native plain value result requires exact typed output storage."});
            *static_cast<Return*>(call.result_value.data) = *result;
            call.result_written = true;
        } else if constexpr (std::is_enum_v<Return>) {
            TypeId enum_type = call.expected_result_type;
            if (enum_type == TypeId{}) [[unlikely]] {
                const auto type = call.runtime.native_type<Return>();
                if (!type)
                    return std::unexpected(type.error());
                enum_type = *type;
            }
            call.result = EnumValue{enum_type, static_cast<std::uint32_t>(*result)};
        } else if constexpr (!std::same_as<Return, void>)
            call.result = *result;
        return {};
    };
    method.native_entry = reinterpret_cast<std::uintptr_t>(&detail::NativeThunk<Pointer, Args>::invoke);
    return method;
}

// A producer of TypeDefinition, with no registry or object storage of its own.
template <class T>
class NativeRegistration {
    static_assert(std::default_initializable<T> && std::is_nothrow_destructible_v<T>);

public:
    NativeRegistration(Runtime& runtime, std::string name, TypeKind kind, std::string description = {})
        : runtime_(runtime) {
        definition_.name = std::move(name);
        definition_.kind = kind;
        definition_.description = std::move(description);
        definition_.native = {sizeof(T), alignof(T), detail::representation<T>(),
                              [](void* address) noexcept -> Result<void> {
                                  std::construct_at(static_cast<T*>(address));
                                  return {};
                              },
                              [](void* address) noexcept { std::destroy_at(static_cast<T*>(address)); }};
        definition_.native.value_operations_preserve_borrows =
            std::is_trivially_default_constructible_v<T> && std::is_trivially_copy_constructible_v<T> &&
            std::is_trivially_copy_assignable_v<T> && std::is_trivially_destructible_v<T> &&
            std::is_trivially_constructible_v<T, decltype(std::move_if_noexcept(std::declval<T&>()))>;
        definition_.native.plain_value = std::is_standard_layout_v<T> && std::is_trivially_copy_constructible_v<T> &&
                                         std::is_trivially_copy_assignable_v<T> && std::is_trivially_destructible_v<T>;
        definition_.native.trivially_destructible = std::is_trivially_destructible_v<T>;
        if constexpr (std::is_trivially_default_constructible_v<T> && std::is_trivially_copyable_v<T>)
            definition_.native.prototype_constructor = definition_.native.construct;
        if constexpr (std::is_enum_v<T>) {
            definition_.enumeration =
                TypeDefinition::Enumeration{TypeReference::native<std::underlying_type_t<T>>(), {}};
        }
        if (kind != TypeKind::object) {
            if constexpr (std::is_copy_constructible_v<T>) {
                definition_.native.copy_construct = [](void* destination, const void* source) noexcept -> Result<void> {
                    std::construct_at(static_cast<T*>(destination), *static_cast<const T*>(source));
                    return {};
                };
            }
            if constexpr (std::is_copy_assignable_v<T>) {
                definition_.native.copy_assign = [](void* destination, const void* source) noexcept -> Result<void> {
                    *static_cast<T*>(destination) = *static_cast<const T*>(source);
                    return {};
                };
            }
            if constexpr (std::is_nothrow_swappable_v<T>) {
                definition_.native.swap = [](void* first, void* second) noexcept {
                    using std::swap;
                    swap(*static_cast<T*>(first), *static_cast<T*>(second));
                };
            }
        }
    }

    template <class U = T>
        requires std::is_enum_v<U> && std::same_as<U, T>
    NativeRegistration& flags(bool value = true) {
        static_assert(std::same_as<std::underlying_type_t<T>, std::uint32_t>,
                      "Flags enums require a uint32_t underlying type.");
        if (!definition_.enumeration)
            definition_.enumeration = TypeDefinition::Enumeration{TypeReference::native<std::uint32_t>(), {}};
        definition_.enumeration->flags = value;
        return *this;
    }

    template <class U = T>
        requires std::is_enum_v<U> && std::same_as<U, T>
    NativeRegistration& enumerator(std::string name, T value, std::string label = {}, std::string description = {}) {
        using Underlying = std::underlying_type_t<T>;
        static_assert(std::same_as<Underlying, std::int32_t> || std::same_as<Underlying, std::uint32_t>,
                      "Reflected enums require an i32/u32 underlying type.");
        if (!definition_.enumeration)
            definition_.enumeration = TypeDefinition::Enumeration{TypeReference::native<Underlying>(), {}};
        auto& enumeration = *definition_.enumeration;
        if (!label.empty() || !description.empty() || !enumeration.metadata.empty()) {
            enumeration.metadata.resize(enumeration.values.size());
            enumeration.metadata.push_back({std::move(label), std::move(description)});
        }
        enumeration.values.emplace_back(std::move(name), static_cast<std::uint32_t>(value));
        return *this;
    }

    template <class Base>
        requires std::derived_from<T, Base>
    NativeRegistration& base(std::optional<std::size_t> offset = std::nullopt) {
        definition_.base = TypeReference::native<Base>();
        definition_.native.base_address = [](void* storage) noexcept -> void* {
            return static_cast<Base*>(static_cast<T*>(storage));
        };
        definition_.native.base_offset = offset;
        return *this;
    }

    template <auto Pointer>
    NativeRegistration& field(std::string name, std::string description = {}) {
        using Value = typename detail::Member<decltype(Pointer)>::Value;
        return field<Pointer>(std::move(name), {TypeReference::native<Value>()}, std::nullopt, {}, {},
                              std::move(description));
    }

    // Explicit descriptions allow references to definitions in the same batch.
    template <auto Pointer>
    NativeRegistration& field(std::string name, ValueDefinition value, std::optional<std::size_t> offset = std::nullopt,
                              std::string label = {}, std::string category = {}, std::string description = {}) {
        using Traits = detail::Member<decltype(Pointer)>;
        using Value = typename Traits::Value;
        static_assert(std::same_as<typename Traits::Owner, T>);
        static_assert(!std::is_const_v<Value> && !std::is_volatile_v<Value>);
        definition_.fields.push_back(
            {std::move(name),
             std::move(value),
             [](void* object) noexcept -> void* { return std::addressof(static_cast<T*>(object)->*Pointer); },
             detail::representation<Value>(),
             true,
             offset,
             {},
             std::move(label),
             std::move(category),
             std::move(description)});
        return *this;
    }

    template <auto Pointer>
    NativeRegistration& hidden_field(std::string name, ValueDefinition value,
                                     std::optional<std::size_t> offset = std::nullopt, std::string label = {},
                                     std::string category = {}, std::string description = {}) {
        field<Pointer>(std::move(name), std::move(value), offset, std::move(label), std::move(category),
                       std::move(description));
        definition_.fields.back().exposed = false;
        return *this;
    }

    template <auto Pointer>
    NativeRegistration& hidden_field(std::string name, std::string description = {}) {
        using Value = typename detail::Member<decltype(Pointer)>::Value;
        return hidden_field<Pointer>(std::move(name), {TypeReference::native<Value>()}, std::nullopt, {}, {},
                                     std::move(description));
    }

    // Applies to the most recently declared field.
    NativeRegistration& range(FieldRange range) {
        definition_.fields.back().range = std::move(range);
        return *this;
    }

    // Applies to the most recently declared field; Runtime validates and owns the values.
    NativeRegistration& metadata(std::vector<std::pair<std::string, std::string>> values) {
        definition_.fields.back().metadata = std::move(values);
        return *this;
    }

    // Applies to the most recently declared field.
    NativeRegistration& transient(bool value = true) {
        definition_.fields.back().transient = value;
        return *this;
    }

    // Applies to the most recently declared field.
    NativeRegistration& read_only(bool value = true) {
        definition_.fields.back().read_only = value;
        return *this;
    }

    // Validate is a free function or const member returning Result<void> for const T&.
    template <auto Validate>
        requires detail::NativeValidator<decltype(Validate), T>
    NativeRegistration& validate() {
        definition_.validate = [](const void* storage) noexcept -> Result<void> {
            return std::invoke(Validate, *static_cast<const T*>(storage));
        };
        return *this;
    }

    template <auto Pointer>
    NativeRegistration& method(std::string name, bool preserves_borrows = false, std::string description = {}) {
        using Traits = detail::Method<decltype(Pointer)>;
        static_assert(std::same_as<typename Traits::Owner, T>);
        definition_.methods.push_back(
            native_method<Pointer>(std::move(name), preserves_borrows, std::move(description)));
        return *this;
    }

    // Applies to the most recently declared method; one name per parameter.
    NativeRegistration& parameter_names(std::vector<std::string> names) {
        definition_.methods.back().parameter_names = std::move(names);
        return *this;
    }

    // Applies to the most recently declared method; one optional default per parameter.
    NativeRegistration& parameter_defaults(std::vector<std::optional<Scalar>> defaults) {
        definition_.methods.back().parameter_defaults = std::move(defaults);
        return *this;
    }

    // Consumes the producer; validation belongs to Runtime, including batch lookup.
    [[nodiscard]] TypeDefinition build() { return std::move(definition_); }

    [[nodiscard]] Result<TypeId> publish() { return runtime_.register_type(build()); }

private:
    Runtime& runtime_;

    TypeDefinition definition_;
};

}

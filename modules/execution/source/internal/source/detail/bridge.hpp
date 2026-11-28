#pragma once

// Private host/backend integration and acceptance observation. Engine callers
// include source/program.hpp; these types are not the public source API.
#include "source/program.hpp"
#include "jit/source_backend.hpp"
#include "language/declarations.hpp"

#include <array>
#include <bit>

namespace atelier::source::detail {
using CodeOwner = std::unique_ptr<BusinessCode, decltype(&atelier_destroy_business)>;

[[nodiscard]] inline String utf8_or_fallback(StringView text, String fallback = {}) noexcept {
    if (auto valid = String::from_utf8(text))
        return std::move(*valid);
    return fallback;
}

struct BoundField {
    String name;

    Array<FieldId> path;

    BusinessType kind;

    // Hard bounds apply here or to the containing math vector.
    bool constrained{};

    Array<BusinessAddressStep> access{};

    std::uint64_t minimum{};

    std::uint64_t maximum{};

    std::uint32_t bounds{};

    bool operator==(const BoundField&) const = default;
};

struct BoundMethod {
    String name;

    MethodId id;

    Array<BusinessParameter> parameters;

    BusinessType result{ValueKind::void_value};

    bool receiver_read_only{};

    bool preserves_borrows{};

    std::uintptr_t entry{};

    Array<BusinessAddressStep> receiver_access{};

    bool operator==(const BoundMethod&) const = default;
};

struct BoundEnum {
    TypeId type;

    String name;

    ValueKind underlying;

    Array<std::pair<String, std::uint32_t>> values;

    bool flags{};

    bool operator==(const BoundEnum&) const = default;
};

struct BoundValueField {
    String name;

    BusinessType type;

    std::size_t offset;

    bool operator==(const BoundValueField&) const = default;
};

struct BoundValue {
    TypeId type;

    String name;

    ValueKind kind;

    std::size_t size;

    std::size_t alignment;

    BusinessType element{ValueKind::void_value};

    Array<BoundValueField> fields;

    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept;

    bool can_own{};

    bool can_mutate{};

    bool standard_array_layout{};

    math::Type math{};

    bool plain_value{};

    bool zero_default{};

    Array<std::byte> initial_value{};

    bool operator==(const BoundValue&) const = default;
};

struct Binding {
    Runtime* runtime{};

    TypeId type;

    StringView name;

    Array<BoundField> fields;

    Array<BoundMethod> methods;

    Array<BoundEnum> enumerations{};

    Array<BoundValue> values{};

    TypeId f32{};

    TypeId f64{};

    TypeId u32{};

    TypeId i32{};

    TypeId boolean{};

    bool operator==(const Binding&) const = default;
};

Result<Binding> bind_type(Runtime&, TypeId, Span<const TypeInfo* const> catalog = {}) noexcept;

BusinessType value_kind(const Binding&, TypeId);

TypeId value_type_id(const Binding&, BusinessType);

Result<Array<TypeDefinition>> type_definitions(Span<const language::TypeDeclaration>,
                                               Span<const language::TypeName>, const Binding&);

struct EnvironmentView {
    Array<BusinessFieldBinding> fields;

    Array<BusinessMethodBinding> methods;

    Array<Array<BusinessEnumItem>> enum_items;

    Array<BusinessEnumBinding> enumerations;

    Array<Array<BusinessValueField>> value_fields;

    Array<BusinessValueBinding> values;

    explicit EnvironmentView(const Binding&) noexcept;

    BusinessEnvironment view(const char* object_type_name) const;
};

inline BusinessValue scalar(float value) {
    return {ValueKind::f32, std::bit_cast<std::uint32_t>(value)};
}

inline BusinessValue scalar(double value) {
    return {ValueKind::f64, std::bit_cast<std::uint64_t>(value)};
}

inline BusinessValue scalar(std::uint32_t value) {
    return {ValueKind::u32, value};
}

inline BusinessValue scalar(std::int32_t value) {
    return {ValueKind::i32, std::bit_cast<std::uint32_t>(value)};
}

inline BusinessValue scalar(bool value) {
    return {ValueKind::boolean, value ? 1u : 0u};
}

BusinessValue scalar(const Scalar&, const Binding&);

Optional<Scalar> scalar_value(BusinessValue, const Binding* = nullptr);

struct AbiArguments {
    InlineArray<BusinessValue, 8> values;

    PreparedArguments prepared;

    Result<void> status;

    AbiArguments(const Binding&, const FunctionSignature&, const BusinessSignature&, Span<const Scalar>);

    AbiArguments(const Binding&, const FunctionSignature&, const BusinessSignature&, Span<const Argument>);

    void initialize(const Binding&, const FunctionSignature&, const BusinessSignature&, Span<const Argument>);

    AbiArguments(const AbiArguments&) = delete;

    AbiArguments& operator=(const AbiArguments&) = delete;

    AbiArguments(AbiArguments&&) = delete;

    AbiArguments& operator=(AbiArguments&&) = delete;
};

struct Invocation {
    Runtime& runtime;

    const Binding& binding;

    bool uses_receiver_slot;

    InlineArray<ObjectRoot, 2> roots;

    InlineArray<ObjectReference, 4> references;

    InlineArray<ObjectReference, 2> observations;

    ExecutionFrame frame;

    InlineArray<NativeContext, 4> native;

    BusinessTrace* trace{};

    DebugSessionState* debugger{};

    BusinessDebugFrame* debug_top{};

    std::uint64_t next_debug_activation{};

    bool debug_aborted{};

    bool failed{};

    std::size_t live_values{};

    NativeFailure error_storage;

    Optional<StringView> external_error{};

    Invocation(Runtime&, const Binding&, BusinessFrameLayout, BusinessTrace* = nullptr, DebugSessionState* = nullptr,
               ExecutionScope* = nullptr);

    bool resources_cleared() const noexcept;

    void record_resources(bool cleared) const noexcept;

    void fail(const Error&) noexcept;

    void fail(ErrorCode, StringView, std::uint32_t element = no_failure_element, StringView layer = {}) noexcept;

    void fail_native(const NativeFailure&) noexcept;

    void fail_view(ErrorCode, StringView, std::uint32_t element = no_failure_element, StringView layer = {}) noexcept;

    ErrorCode error_code() const noexcept { return static_cast<ErrorCode>(error_storage.code); }

    StringView error_message() const noexcept {
        if (!failed)
            return {};
        return external_error ? *external_error : StringView{error_storage.message};
    }

    void invalidate_native() noexcept;
};

template <class Operation>
std::uint32_t bridge(Invocation& invocation, Operation&& operation) noexcept {
    auto result = operation();
    if (result)
        return 0;
    invocation.fail(result.error());
    return 1;
}

std::uint32_t create_slot(void*, std::uint32_t) noexcept;

std::uint32_t native_receiver(void*, std::uint32_t, NativeContext**) noexcept;

void native_failure(void*, const NativeFailure*) noexcept;

void native_segment(void*) noexcept;

Result<ObjectReference> reference_value(Invocation&, std::uint32_t);

std::uint32_t change_reference(void*, ReferenceOperation, std::uint32_t, std::uint32_t) noexcept;

std::uint32_t same_reference(void*, std::uint32_t, std::uint32_t, std::uint32_t*) noexcept;

void clear_slot(void*, std::uint32_t) noexcept;

extern const BusinessServices services;

bool same_environment(const Binding&, const BusinessEnvironment&);

StringView resolve_failure_layer_view(const Binding&, std::uint32_t packed_layer) noexcept;

Optional<ErrorElement> resolve_failure_element(const Binding&, const BusinessFailure&);

Error execution_error(const Invocation*, const Binding&, const BusinessFailure&,
                      const BusinessFailureMessage* failure_messages = nullptr, std::uint32_t failure_message_count = 0,
                      StringView fallback_message = "Source execution failed.");

bool try_bind_receiver(Invocation&, bool requires_receiver, ObjectId) noexcept;

Result<void> bind_receiver(Invocation&, bool requires_receiver, ObjectId);

Result<void> finish_invocation(Invocation&, std::uint32_t status, const BusinessFailure&,
                               std::uint32_t* failure_site = nullptr,
                               const BusinessFailureMessage* failure_messages = nullptr,
                               std::uint32_t failure_message_count = 0);

// Parameter transport varies; admission, Invocation lifetime and explicit
// cleanup are shared by generic and fixed-signature callers.
using InvocationBody = std::uint32_t (*)(void*, Invocation&, BusinessFailure&) noexcept;

Result<void> execute_invocation(ExecutionScope&, Runtime&, const Binding&, BusinessFrameLayout, bool requires_receiver,
                                ObjectId, void*, InvocationBody, BusinessTrace* = nullptr, DebugSessionState* = nullptr,
                                std::uint32_t* failure_site = nullptr,
                                const BusinessFailureMessage* failure_messages = nullptr,
                                std::uint32_t failure_message_count = 0);

Result<BusinessValue> execute_admitted(ExecutionScope&, Runtime&, const Binding&, const BusinessCodeInfo&,
                                       BusinessFrameLayout, bool requires_receiver, ObjectId,
                                       Span<const BusinessValue>, void* result_data = nullptr,
                                       BusinessTrace* = nullptr, DebugSessionState* = nullptr,
                                       std::uint32_t* failure_site = nullptr);

struct ProgramAccess {
    static std::uintptr_t address(const Program::Lease&) noexcept;

    static bool stateless_direct(const Program::Lease&) noexcept;
};
}

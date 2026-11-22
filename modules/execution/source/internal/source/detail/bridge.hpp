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

struct BoundField {
    std::string name;

    std::vector<FieldId> path;

    BusinessType kind;

    // Hard bounds apply here or to the containing math vector.
    bool constrained{};

    std::vector<BusinessAddressStep> access{};

    std::uint64_t minimum{};

    std::uint64_t maximum{};

    std::uint32_t bounds{};

    bool operator==(const BoundField&) const = default;
};

struct BoundMethod {
    std::string name;

    MethodId id;

    std::vector<BusinessParameter> parameters;

    BusinessType result{ValueKind::void_value};

    bool receiver_read_only{};

    bool preserves_borrows{};

    std::uintptr_t entry{};

    std::vector<BusinessAddressStep> receiver_access{};

    bool operator==(const BoundMethod&) const = default;
};

struct BoundEnum {
    TypeId type;

    std::string name;

    ValueKind underlying;

    std::vector<std::pair<std::string, std::uint32_t>> values;

    bool flags{};

    bool operator==(const BoundEnum&) const = default;
};

struct BoundValueField {
    std::string name;

    BusinessType type;

    std::size_t offset;

    bool operator==(const BoundValueField&) const = default;
};

struct BoundValue {
    TypeId type;

    std::string name;

    ValueKind kind;

    std::size_t size;

    std::size_t alignment;

    BusinessType element{ValueKind::void_value};

    std::vector<BoundValueField> fields;

    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept;

    bool can_own{};

    bool can_mutate{};

    math::Type math{};

    bool plain_value{};

    bool zero_default{};

    std::vector<std::byte> initial_value{};

    bool operator==(const BoundValue&) const = default;
};

struct Binding {
    Runtime* runtime{};

    TypeId type;

    std::string_view name;

    std::vector<BoundField> fields;

    std::vector<BoundMethod> methods;

    std::vector<BoundEnum> enumerations{};

    std::vector<BoundValue> values{};

    TypeId f32{};

    TypeId f64{};

    TypeId u32{};

    TypeId i32{};

    TypeId boolean{};

    bool operator==(const Binding&) const = default;
};

Result<Binding> bind_type(Runtime&, TypeId, std::span<const TypeInfo* const> catalog = {}) noexcept;

BusinessType value_kind(const Binding&, TypeId);

TypeId value_type_id(const Binding&, BusinessType);

Result<std::vector<TypeDefinition>> type_definitions(const std::vector<language::TypeDeclaration>&,
                                                     std::span<const language::TypeName>, const Binding&);

struct EnvironmentView {
    std::vector<BusinessFieldBinding> fields;

    std::vector<BusinessMethodBinding> methods;

    std::vector<std::vector<BusinessEnumItem>> enum_items;

    std::vector<BusinessEnumBinding> enumerations;

    std::vector<std::vector<BusinessValueField>> value_fields;

    std::vector<BusinessValueBinding> values;

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

std::optional<Scalar> scalar_value(BusinessValue, const Binding* = nullptr);

struct AbiArguments {
    InlineArray<BusinessValue, 8> values;

    PreparedArguments prepared;

    Result<void> status;

    AbiArguments(const Binding&, const FunctionSignature&, const BusinessSignature&, std::span<const Scalar>);

    AbiArguments(const Binding&, const FunctionSignature&, const BusinessSignature&, std::span<const Argument>);

    void initialize(const Binding&, const FunctionSignature&, const BusinessSignature&, std::span<const Argument>);

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

    ErrorCode error_code{ErrorCode::invocation_failed};

    char error[512];

    std::optional<std::string_view> external_error{};

    Invocation(Runtime&, const Binding&, BusinessFrameLayout, BusinessTrace* = nullptr, DebugSessionState* = nullptr,
               ExecutionScope* = nullptr);

    bool resources_cleared() const noexcept;

    void record_resources(bool cleared) const noexcept;

    void fail(ErrorCode, const char*) noexcept;

    void fail_view(ErrorCode, std::string_view) noexcept;

    std::string_view error_message() const noexcept {
        return external_error ? *external_error : std::string_view(error);
    }

    void invalidate_native() noexcept;
};

template <class Operation>
std::uint32_t bridge(Invocation& invocation, Operation&& operation) noexcept {
    auto result = operation();
    if (result)
        return 0;
    invocation.fail(result.error().code, result.error().message.c_str());
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
                                       std::span<const BusinessValue>, void* result_data = nullptr,
                                       BusinessTrace* = nullptr, DebugSessionState* = nullptr,
                                       std::uint32_t* failure_site = nullptr);

struct ProgramAccess {
    static std::uintptr_t address(const Program::Lease&) noexcept;

    static bool stateless_direct(const Program::Lease&) noexcept;
};
}

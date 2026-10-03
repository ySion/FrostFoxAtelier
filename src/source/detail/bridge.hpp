#pragma once

// Private host/backend integration and acceptance observation. Engine callers
// include source/program.hpp; these types are not the public source API.
#include "source/program.hpp"
#include "jit/source_backend.hpp"

#include <array>
#include <bit>
#include <exception>

namespace atelier::source::detail {
using CodeOwner = std::unique_ptr<BusinessCode, decltype(&atelier_destroy_business)>;
struct BoundField {
    std::string name;
    std::vector<FieldId> path;
    BusinessType kind;
    bool operator==(const BoundField&) const = default;
};
struct BoundMethod {
    std::string name;
    MethodId id;
    std::vector<BusinessParameter> parameters;
    BusinessType result{ValueKind::void_value};
    bool receiver_read_only{};
    bool preserves_borrows{};
    bool operator==(const BoundMethod&) const = default;
};
struct BoundEnum {
    TypeId type;
    std::string name;
    ValueKind underlying;
    std::vector<std::pair<std::string, std::uint32_t>> values;
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
    std::size_t size, alignment;
    BusinessType element{ValueKind::void_value};
    std::vector<BoundValueField> fields;
    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept;
    bool can_own{}, can_mutate{};
    math::Type math{};
    bool operator==(const BoundValue&) const = default;
};
struct Binding {
    Runtime* runtime{};
    TypeId type;
    std::string_view name;
    std::vector<BoundField> fields;
    std::vector<BoundMethod> methods;
    std::vector<TypeId> dependencies;
    std::vector<BoundEnum> enumerations{};
    std::vector<BoundValue> values{};
    TypeId f32{}, f64{}, u32{}, i32{};
};
Binding bind_type(Runtime&, TypeId, std::span<const TypeInfo* const> catalog = {});
BusinessType value_kind(const Binding&, TypeId);
TypeId value_type_id(const Binding&, BusinessType);
struct EnvironmentView {
    std::vector<BusinessFieldBinding> fields;
    std::vector<BusinessMethodBinding> methods;
    std::vector<std::vector<BusinessEnumItem>> enum_items;
    std::vector<BusinessEnumBinding> enumerations;
    std::vector<std::vector<BusinessValueField>> value_fields;
    std::vector<BusinessValueBinding> values;
    explicit EnvironmentView(const Binding&);
    BusinessEnvironment view(const char* object_type_name) const;
};
inline BusinessValue scalar(float value) { return {ValueKind::f32, std::bit_cast<std::uint32_t>(value)}; }
inline BusinessValue scalar(double value) { return {ValueKind::f64, std::bit_cast<std::uint64_t>(value)}; }
inline BusinessValue scalar(std::uint32_t value) { return {ValueKind::u32, value}; }
inline BusinessValue scalar(std::int32_t value) { return {ValueKind::i32, std::bit_cast<std::uint32_t>(value)}; }
BusinessValue scalar(const Scalar&, const Binding&);
std::optional<Scalar> scalar_value(BusinessValue, const Binding* = nullptr);
struct AbiArguments {
    std::array<BusinessValue, 8> inline_values;
    std::vector<BusinessValue> expanded;
    std::span<BusinessValue> values;
    PreparedArguments prepared;
    Result<void> status;
    AbiArguments(const Binding&, const FunctionSignature&, std::span<const Scalar>);
    AbiArguments(const Binding&, const FunctionSignature&, std::span<const Argument>);
    void initialize(const Binding&, const FunctionSignature&, std::span<const Argument>);
    AbiArguments(const AbiArguments&) = delete;
    AbiArguments& operator=(const AbiArguments&) = delete;
    AbiArguments(AbiArguments&&) = delete;
    AbiArguments& operator=(AbiArguments&&) = delete;
};

struct Invocation;
// All observer state is borrowed until invocation/frame destruction. Callbacks
// run inside the checked bridge; complete only observes teardown and cannot
// throw. No observer owns a second program or changes generated slot layout.
struct InvocationObserver {
    void* context{};
    Result<void> (*begin)(void*, Invocation&){};
    Result<void> (*poll)(void*, Invocation&, const CollectionProgress&){};
    Result<void> (*finish)(void*, Invocation&){};
    void (*complete)(void*, const Invocation&, bool success) noexcept{};
};
struct Invocation {
    Runtime& runtime;
    const Binding& binding;
    std::array<ObjectRoot, business_root_slots> inline_roots;
    std::vector<ObjectRoot> expanded_roots;
    std::span<ObjectRoot> roots;
    std::vector<ObjectReference> observations;
    ExecutionFrame frame;
    std::array<Argument, 8> inline_arguments;
    std::vector<Argument> expanded_arguments;
    std::span<Argument> method_arguments;
    CollectionBudget budget;
    InvocationObserver observer;
    bool failed{};
    std::size_t live_values{};
    ErrorCode error_code{ErrorCode::invocation_failed};
    char error[512]{};
    Invocation(Runtime&, const Binding&, BusinessFrameLayout, CollectionBudget, InvocationObserver = {});
    bool resources_cleared() const noexcept;
    void fail(ErrorCode, const char*) noexcept;
};
template<class Operation>
std::uint32_t bridge(Invocation& invocation, Operation&& operation) noexcept {
    try {
        auto result = operation();
        if (result) return 0;
        invocation.fail(result.error().code, result.error().message.c_str());
    } catch (const std::exception& error) {
        invocation.fail(ErrorCode::invocation_failed, error.what());
    } catch (...) {
        invocation.fail(ErrorCode::invocation_failed, "Unknown native service failure.");
    }
    return 1;
}
std::uint32_t create_slot(void*, std::uint32_t) noexcept;
std::uint32_t read_float(void*, std::uint32_t, std::uint32_t, float*) noexcept;
std::uint32_t write_float(void*, std::uint32_t, std::uint32_t, float) noexcept;
std::uint32_t read_double(void*, std::uint32_t, std::uint32_t, double*) noexcept;
std::uint32_t write_double(void*, std::uint32_t, std::uint32_t, double) noexcept;
std::uint32_t read_math(void*, std::uint32_t, std::uint32_t, void*) noexcept;
std::uint32_t write_math(void*, std::uint32_t, std::uint32_t, const void*) noexcept;
std::uint32_t read_uint(void*, std::uint32_t, std::uint32_t, std::uint32_t*) noexcept;
std::uint32_t write_uint(void*, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
std::uint32_t call_method(void*, std::uint32_t, std::uint32_t, const BusinessValue*, std::uint32_t, BusinessValue*) noexcept;
std::uint32_t begin(void*) noexcept;
std::uint32_t poll(void*) noexcept;
std::uint32_t finish(void*) noexcept;
Result<ObjectReference> reference_value(Invocation&, std::uint32_t);
std::uint32_t change_reference(void*, ReferenceOperation, std::uint32_t, std::uint32_t) noexcept;
std::uint32_t same_reference(void*, std::uint32_t, std::uint32_t, std::uint32_t*) noexcept;
void clear_slot(void*, std::uint32_t) noexcept;
extern const BusinessServices services;
bool same_environment(const Binding&, const BusinessEnvironment&);
Result<void> check_source_arguments(const Binding&, const BusinessSignature&, std::span<const BusinessValue>);
Result<void> check_source_receiver(const Binding&, ObjectId);
Result<BusinessValue> execute_admitted(ExecutionScope&, Runtime&, const Binding&, BusinessCode*,
    BusinessFrameLayout, bool requires_receiver, ObjectId, std::span<const BusinessValue>,
    bool request_collection, InvocationObserver = {}, CollectionBudget = {1}, void* result_data = nullptr);
Result<BusinessValue> execute_typed(Runtime&, const Binding&, BusinessCode*, ObjectId,
    std::span<const BusinessValue>, bool request_collection = false,
    InvocationObserver = {}, CollectionBudget = {1});

struct ProgramAccess {
    using Compiler = decltype(&atelier_compile_business);
    static Result<BusinessValue> invoke(const Program::Entry&, ObjectId, std::span<const BusinessValue>,
        InvocationObserver = {}, CollectionBudget = {1});
    static Result<BusinessValue> invoke(const Program::Lease&, ObjectId, std::span<const BusinessValue>,
        InvocationObserver = {}, CollectionBudget = {1});
    static Result<Coroutine> start(const Program::Entry&, ObjectId, std::span<const BusinessValue>,
        InvocationObserver = {}, CollectionBudget = {1});
    static Result<Coroutine> start(const Program::Lease&, ObjectId, std::span<const BusinessValue>,
        InvocationObserver = {}, CollectionBudget = {1});
    static BusinessFrameLayout layout(const Program::Lease&) noexcept;
    static Result<Program::Candidate> prepare(const Program::Request&, Compiler);
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
    static Result<void> compatible(const Program&, BusinessCode*, std::string_view);
    static Result<void> unchanged(const Program&);
    static void set_request_sequence(Program&, std::uint64_t) noexcept;
    static std::uint64_t published_sequence(const Program&) noexcept;
#endif
private:
    static Result<Coroutine> start_version(std::shared_ptr<const Program::Version>, ObjectId,
        std::span<const BusinessValue>, InvocationObserver, CollectionBudget);
};
}

#pragma once
#include "runtime/parameter.hpp"
#include "math/type.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace atelier::source::detail {

inline constexpr std::uint32_t receiver_slot = 0;
inline constexpr std::uint32_t business_root_slots = 1;
inline constexpr std::uint32_t observation_slot_bit = 0x80000000u;
inline constexpr std::uint32_t null_reference_slot = 0xffffffffu;

enum class ValueKind : std::uint32_t { void_value, f32, u32, i32, enumeration, record, array, string, f64 };
struct BusinessType {
    ValueKind kind;
    std::uint32_t type_index;
    BusinessType() = default;
    constexpr BusinessType(ValueKind value, std::uint32_t index = 0) : kind(value), type_index(index) {}
    bool operator==(const BusinessType&) const = default;
};
struct BusinessParameter {
    BusinessType type;
    ParameterPassing passing{ParameterPassing::value};
    bool operator==(const BusinessParameter&) const = default;
};
enum class ArgumentStorage : std::uint32_t { value, read_only, reference, field };
// Fixed-layout operand. Native values borrow original storage; bits only carry
// scalars. Addresses/field locators last through a synchronous call. Math value
// arguments point at independent input storage; math results use caller-owned
// exact typed storage. This is not an aggregate serialization format.
struct BusinessValue {
    BusinessType kind{};
    std::uint64_t bits{};
    ArgumentStorage storage{ArgumentStorage::value};
    const void* data{};
    std::uint32_t slot{}, field{};
};
static_assert(std::is_standard_layout_v<BusinessValue> && std::is_trivially_copyable_v<BusinessValue>);
static_assert(sizeof(BusinessValue) == 40 && alignof(BusinessValue) == 8 &&
    offsetof(BusinessValue, bits) == 8 && offsetof(BusinessValue, storage) == 16 &&
    offsetof(BusinessValue, data) == 24 && offsetof(BusinessValue, slot) == 32 && offsetof(BusinessValue, field) == 36);

struct BusinessFieldBinding { const char* name; BusinessType kind; };
struct BusinessMethodBinding {
    const char* name;
    const BusinessParameter* parameters;
    std::uint32_t parameter_count;
    BusinessType result{ValueKind::void_value};
    bool receiver_read_only{};
    bool preserves_borrows{};
};
struct BusinessEnumItem { const char* name; std::uint32_t bits; };
struct BusinessEnumBinding {
    const char* name;
    ValueKind underlying;
    const BusinessEnumItem* values;
    std::uint32_t value_count;
};
inline bool aggregate(BusinessType type) noexcept {
    return type.kind == ValueKind::record || type.kind == ValueKind::array || type.kind == ValueKind::string;
}
struct BusinessValueField { const char* name; BusinessType type; std::uint64_t offset; };
struct BusinessValueBinding {
    const char* name;
    ValueKind kind;
    std::uint64_t size, alignment;
    BusinessType element;
    const BusinessValueField* fields;
    std::uint32_t field_count;
    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept;
    bool can_own{}, can_mutate{};
    math::Type math{};
};
struct BusinessEnvironment {
    const BusinessFieldBinding* fields;
    std::uint32_t field_count;
    const BusinessMethodBinding* methods;
    std::uint32_t method_count;
    const char* object_type_name;
    const BusinessEnumBinding* enumerations{};
    std::uint32_t enumeration_count{};
    const BusinessValueBinding* values{};
    std::uint32_t value_count{};
};
struct BusinessSignature {
    const BusinessParameter* parameters;
    std::uint32_t parameter_count;
    BusinessType result;
    bool is_coroutine;
};

enum class ReferenceOperation : std::uint32_t { copy, observe, try_acquire };

// Private source-backend ABI. All semantic objects, roots and diagnostics stay in
// the host. Slots belong to one execution instance and remain stable across
// coroutine suspensions; they are not an object map.
// Strong slots are untagged; weak slots address a separate observation array.
// The null slot is an operand only, never a destination or cleanup target.
// Field/method operands are indices into the compiled environment's tables.
struct BusinessServices {
    std::uint32_t (*begin)(void*) noexcept;
    std::uint32_t (*create)(void*, std::uint32_t) noexcept;
    std::uint32_t (*read_float)(void*, std::uint32_t, std::uint32_t, float*) noexcept;
    std::uint32_t (*write_float)(void*, std::uint32_t, std::uint32_t, float) noexcept;
    std::uint32_t (*read_uint)(void*, std::uint32_t, std::uint32_t, std::uint32_t*) noexcept;
    std::uint32_t (*write_uint)(void*, std::uint32_t, std::uint32_t, std::uint32_t) noexcept;
    // Arguments/result borrow entry-allocated scratch through this synchronous
    // call. Non-void methods require result storage and commit its declared kind
    // and payload only on success; math output.data points to typed writable
    // scratch. Void methods may receive a null result pointer.
    std::uint32_t (*call)(void*, std::uint32_t, std::uint32_t, const BusinessValue*, std::uint32_t,
        BusinessValue* result) noexcept;
    std::uint32_t (*poll)(void*) noexcept;
    std::uint32_t (*finish)(void*) noexcept;
    // Cannot fail for this compiler's validated strong or tagged weak slots.
    void (*clear)(void*, std::uint32_t) noexcept;
    std::uint32_t (*reference)(void*, ReferenceOperation, std::uint32_t destination,
        std::uint32_t source) noexcept;
    std::uint32_t (*same_reference)(void*, std::uint32_t left, std::uint32_t right,
        std::uint32_t* output) noexcept;

    std::uint32_t (*construct_value)(void*, std::uint32_t, void*, const void*) noexcept{};
    std::uint32_t (*assign_value)(void*, std::uint32_t, void*, const void*) noexcept{};
    void (*destroy_value)(void*, std::uint32_t, void*) noexcept{};
    std::uint32_t (*mutate_sequence)(void*, std::uint32_t, SequenceMutation, void*, const void*, std::uint64_t) noexcept{};
    std::uint32_t (*read_double)(void*, std::uint32_t, std::uint32_t, double*) noexcept{};
    std::uint32_t (*write_double)(void*, std::uint32_t, std::uint32_t, double) noexcept{};
    std::uint32_t (*read_math)(void*, std::uint32_t, std::uint32_t, void*) noexcept{};
    std::uint32_t (*write_math)(void*, std::uint32_t, std::uint32_t, const void*) noexcept{};
    bool operator==(const BusinessServices&) const = default;
};

struct BusinessCodeInfo {
    // Names, tables and parameter arrays are code-owned, borrowed while an
    // owner or active invocation pin keeps this state alive. This view owns no
    // code; source inputs and host catalog storage need not remain alive.
    const char* function_name;
    const char* source_name;
    // Diagnostic/lifetime inspection only, not a durable callable handle.
    std::uintptr_t entry_address;
    // Exact callback values used for this compiled unit's symbol binding.
    BusinessServices services;
    BusinessSignature signature;
    BusinessEnvironment environment;
    bool requires_receiver;
};

struct BusinessFrameLayout {
    std::uint32_t strong_slots;
    std::uint32_t weak_slots;
    // Code-owned, null-terminated name; borrowed while a compiled-state owner
    // or active invocation pin is alive. Empty/null retains the generic binding
    // used by scalar-only programs.
    const char* object_type_name;
    // Maximum native-call arity across reachable functions, not the host catalog.
    std::uint32_t max_call_arguments;
};
static_assert(std::is_trivial_v<BusinessFrameLayout> && std::is_standard_layout_v<BusinessFrameLayout>);

struct BusinessCode;
struct BusinessCoroutine;

}

#ifdef ATELIER_SOURCE_BACKEND_BUILD
#define ATELIER_BUSINESS_BACKEND __declspec(dllexport)
#else
#define ATELIER_BUSINESS_BACKEND __declspec(dllimport)
#endif

// A module has one selected host entry and internal synchronous functions.
// entry_name may be omitted for a single function; results are scalar/enum or
// synchronous builtin math values.
// Environment members describe the host's validated Runtime binding; a null
// environment is empty. Code and LLVM state stay in their DLL; source bytes,
// catalog names and arrays are borrowed only for compile. Compiled metadata
// owns its signature, catalog, names and service snapshot.
// Diagnostics and the COFF file label abbreviate long display names with a
// bounded prefix/suffix, preserving trailing request identity and UTF-8 edges.
// Compiled source_name remains complete; display labels are not identity keys.
// Failure leaves *output unchanged, including an existing executable handle.
// Host callback code and this DLL must outlive all code owners and invocations.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_compile_business(
    const atelier::source::detail::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::source::detail::BusinessEnvironment* environment,
    atelier::source::detail::BusinessCode** output,
    char* error, std::uint32_t error_capacity, const char* entry_name = nullptr) noexcept;

// Actual compiled metadata, not caller-provided claims. Null code returns null
// names/tables, a zero address and null callbacks. Querying acquires no owner.
extern "C" ATELIER_BUSINESS_BACKEND void atelier_query_business_info(
    atelier::source::detail::BusinessCode* code, atelier::source::detail::BusinessCodeInfo* output) noexcept;
inline atelier::source::detail::BusinessCodeInfo atelier_business_info(atelier::source::detail::BusinessCode* code) noexcept {
    atelier::source::detail::BusinessCodeInfo output{};
    atelier_query_business_info(code, &output);
    return output;
}

// Query before allocating invocation storage. Null code returns zero counts.
extern "C" ATELIER_BUSINESS_BACKEND atelier::source::detail::BusinessFrameLayout atelier_business_layout(
    atelier::source::detail::BusinessCode* code) noexcept;

// One invocation pins its compiled state before entering generated code.
// Destroying its outer handle in a synchronous callback does not free active
// machine code. This does not authorize concurrent use/destruction or DLL unload.
// Arguments are borrowed and must remain stable through this synchronous call.
// Arity, kinds and required storage are checked before invoking any callbacks.
// A non-void result requires storage; void permits null. Only full success
// commits its declared value; a math result requires result->data to address an
// aligned value of its exact declared type. Failure leaves both the operand and
// its pointed-to result storage unchanged.
// Service failure status codes pass through unchanged; their first-cause text
// stays with the host invocation. Generated result-contract failures instead
// populate error after explicit cleanup, without replacing a host first cause.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_invoke_business(
    atelier::source::detail::BusinessCode* code, void* invocation,
    const atelier::source::detail::BusinessValue* arguments, std::uint32_t argument_count,
    atelier::source::detail::BusinessValue* result, char* error, std::uint32_t error_capacity) noexcept;

extern "C" ATELIER_BUSINESS_BACKEND void atelier_destroy_business(
    atelier::source::detail::BusinessCode* code) noexcept;

// Private same-thread coroutine ABI. Creation copies arguments and owns its
// code version without executing the body. invocation must remain stable until
// destruction. resume commits result only on completion; suspended is separate
// from error status. Destruction cancels without running the source continuation.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_create_business_coroutine(
    atelier::source::detail::BusinessCode* code, void* invocation,
    const atelier::source::detail::BusinessValue* arguments, std::uint32_t argument_count,
    atelier::source::detail::BusinessCoroutine** output, char* error, std::uint32_t error_capacity) noexcept;
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_resume_business_coroutine(
    atelier::source::detail::BusinessCoroutine* coroutine, bool* suspended,
    atelier::source::detail::BusinessValue* result, char* error, std::uint32_t error_capacity) noexcept;
extern "C" ATELIER_BUSINESS_BACKEND void atelier_destroy_business_coroutine(
    atelier::source::detail::BusinessCoroutine* coroutine) noexcept;

#undef ATELIER_BUSINESS_BACKEND

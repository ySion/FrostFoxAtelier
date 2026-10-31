#pragma once
#include "runtime/parameter.hpp"
#include "math/type.hpp"
#include "source/diagnostic_types.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace atelier::source::detail {

inline constexpr std::uint32_t no_diagnostic_location = 0xffffffffu;
// Same-thread, non-owning capture storage. Generated code only writes fixed POD
// fields; no observer callbacks or allocations occur while recording an event.
struct BusinessTrace {
    DiagnosticCounters calls;
    std::uint32_t current_location{no_diagnostic_location};
    std::uint32_t failure_location{no_diagnostic_location};
    std::uint32_t suspended_location{no_diagnostic_location};
    std::uint64_t peak_owned_values{};
    std::size_t remaining_owned_values{};
    bool resources_cleared{};
};
static_assert(std::is_standard_layout_v<BusinessTrace> && std::is_trivially_copyable_v<BusinessTrace>);
struct BusinessLocation {
    const char* function;
    std::uint64_t offset, line, column;
};
struct BusinessArtifacts {
    const char* entry_symbol;
    const char* target_triple;
    const char* cpu_features;
    const char* original_ir;
    std::size_t original_ir_size;
    const char* optimized_ir;
    std::size_t optimized_ir_size;
    const char* assembly;
    std::size_t assembly_size;
    const std::byte* object_code;
    std::size_t object_code_size;
};

inline constexpr std::uint32_t receiver_slot = 0;
inline constexpr std::uint32_t business_root_slots = 1;
inline constexpr std::uint32_t observation_slot_bit = 0x80000000u;
inline constexpr std::uint32_t null_reference_slot = 0xffffffffu;

enum class ValueKind : std::uint32_t { void_value, f32, u32, i32, enumeration, record, array, string, f64, boolean };
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
// scalars; boolean bits must be exactly 0 or 1. Addresses/field locators last through a synchronous call. Math and plain record value
// arguments point at independent input storage; aggregate results use caller-owned
// exact typed storage. This is not an aggregate serialization format.
struct BusinessValue {
    BusinessType kind{};
    std::uint64_t bits{};
    ArgumentStorage storage{ArgumentStorage::value};
    const void* data{};
    std::uint32_t slot{}, field{};
};
static_assert(std::is_standard_layout_v<BusinessValue> && std::is_trivially_copyable_v<BusinessValue>);
static_assert(sizeof(BusinessValue) == 40 && alignof(BusinessValue) == 8 && offsetof(BusinessValue, bits) == 8 &&
              offsetof(BusinessValue, storage) == 16 && offsetof(BusinessValue, data) == 24 &&
              offsetof(BusinessValue, slot) == 32 && offsetof(BusinessValue, field) == 36);

// `constrained`: Runtime hard bounds apply, so the field cannot be borrowed writably.
struct BusinessFieldBinding {
    const char* name;
    BusinessType kind;
    bool constrained{};
};
struct BusinessMethodBinding {
    const char* name;
    const BusinessParameter* parameters;
    std::uint32_t parameter_count;
    BusinessType result{ValueKind::void_value};
    bool receiver_read_only{};
    bool preserves_borrows{};
};
struct BusinessEnumItem {
    const char* name;
    std::uint32_t bits;
};
struct BusinessEnumBinding {
    const char* name;
    ValueKind underlying;
    const BusinessEnumItem* values;
    std::uint32_t value_count;
};
inline bool aggregate(BusinessType type) noexcept {
    return type.kind == ValueKind::record || type.kind == ValueKind::array || type.kind == ValueKind::string;
}
struct BusinessValueField {
    const char* name;
    BusinessType type;
    std::uint64_t offset;
};
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
    bool plain_value{};
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

enum class DebugStorage : std::uint32_t { value, strong_reference, weak_reference };
enum class DebugAvailability : std::uint32_t { out_of_scope, uninitialized, available };
enum class DebugPoint : std::uint32_t { statement, entry, call_return, error };
enum class EntryFailure : std::uint32_t {
    none,
    invalid_call_result,
    sequence_bounds,
    sequence_layout,
    sequence_size,
    integer_division,
    integer_shift,
    integer_conversion
};
inline const char* failure_message(EntryFailure reason) noexcept {
    switch (reason) {
    case EntryFailure::none:
        return nullptr;
    case EntryFailure::invalid_call_result:
        return "Native call result contract violation: successful callback omitted the result or returned an invalid value.";
    case EntryFailure::sequence_bounds:
        return "Borrowed Array/String index is out of range.";
    case EntryFailure::sequence_layout:
        return "Borrowed sequence has invalid storage, alignment or size beyond uint range.";
    case EntryFailure::sequence_size:
        return "Sequence size cannot be negative.";
    case EntryFailure::integer_division:
        return "Integer division or remainder requires a nonzero divisor and a representable quotient.";
    case EntryFailure::integer_shift:
        return "Integer shift count must be in the range 0 through 31.";
    case EntryFailure::integer_conversion:
        return "Integer conversion requires a finite value representable after truncation toward zero.";
    }
    return "Unknown source execution failure.";
}
// Written only on a generated failure exit: the reason, then the innermost
// failing site's index into the compiled location table.
struct BusinessFailure {
    EntryFailure reason{EntryFailure::none};
    std::uint32_t location{no_diagnostic_location};
};
static_assert(std::is_standard_layout_v<BusinessFailure> && sizeof(BusinessFailure) == 8 &&
              offsetof(BusinessFailure, location) == 4);
struct BusinessDebugVariable {
    const char* name;
    BusinessType type;
    DebugStorage storage;
    ParameterPassing passing;
};
struct BusinessDebugFunction {
    const BusinessDebugVariable* variables;
    std::uint32_t variable_count;
};
struct BusinessDebugLocal {
    const void* data;
    std::uint32_t slot;
    DebugAvailability availability;
};
struct BusinessDebugFrame {
    BusinessDebugFrame* previous;
    const BusinessDebugLocal* locals;
    std::uint64_t activation;
    std::uint32_t function, location;
};
static_assert(std::is_standard_layout_v<BusinessDebugFrame> && std::is_trivial_v<BusinessDebugFrame>);
static_assert(sizeof(BusinessDebugFrame) == 32 && sizeof(BusinessDebugLocal) == 16);

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
    // and payload only on success; aggregate output.data points to typed writable
    // scratch. Void methods may receive a null result pointer.
    std::uint32_t (*call)(void*, std::uint32_t, std::uint32_t, const BusinessValue*, std::uint32_t,
                          BusinessValue* result) noexcept;
    std::uint32_t (*poll)(void*) noexcept;
    std::uint32_t (*finish)(void*) noexcept;
    // Cannot fail for this compiler's validated strong or tagged weak slots.
    void (*clear)(void*, std::uint32_t) noexcept;
    std::uint32_t (*reference)(void*, ReferenceOperation, std::uint32_t destination, std::uint32_t source) noexcept;
    std::uint32_t (*same_reference)(void*, std::uint32_t left, std::uint32_t right, std::uint32_t* output) noexcept;

    std::uint32_t (*construct_value)(void*, std::uint32_t, void*, const void*) noexcept {};
    std::uint32_t (*assign_value)(void*, std::uint32_t, void*, const void*) noexcept {};
    void (*destroy_value)(void*, std::uint32_t, void*) noexcept {};
    std::uint32_t (*mutate_sequence)(void*, std::uint32_t, SequenceMutation, void*, const void*,
                                     std::uint64_t) noexcept {};
    std::uint32_t (*read_double)(void*, std::uint32_t, std::uint32_t, double*) noexcept {};
    std::uint32_t (*write_double)(void*, std::uint32_t, std::uint32_t, double) noexcept {};
    // Exact typed reads/writes for builtin math and recursively plain records.
    std::uint32_t (*read_value)(void*, std::uint32_t, std::uint32_t, void*) noexcept {};
    std::uint32_t (*write_value)(void*, std::uint32_t, std::uint32_t, const void*) noexcept {};
    // Numeric ABI operands are canonical 0/1; native bool storage remains one byte.
    std::uint32_t (*read_bool)(void*, std::uint32_t, std::uint32_t, std::uint32_t*) noexcept {};
    std::uint32_t (*write_bool)(void*, std::uint32_t, std::uint32_t, std::uint32_t) noexcept {};
    std::uint32_t (*debug_enter)(void*, BusinessDebugFrame*) noexcept {};
    std::uint32_t (*debug_point)(void*, BusinessDebugFrame*, std::uint32_t location, DebugPoint,
                                 EntryFailure) noexcept {};
    void (*debug_leave)(void*, BusinessDebugFrame*) noexcept {};
    // Pure material sampling: one invocation-owned binding, float2 in / float4 out.
    std::uint32_t (*sample_texture)(void*, std::uint32_t, const float*, float*) noexcept {};
    // Independent sampler-address query: same binding, float2 in / float2 out.
    std::uint32_t (*sample_orientation)(void*, std::uint32_t, const float*, float*) noexcept {};
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
    bool diagnostics;
    const BusinessLocation* locations;
    std::uint32_t location_count;
    bool debugging;
    const BusinessDebugFunction* debug_functions;
    std::uint32_t debug_function_count;
    const std::uint32_t* debug_sites; // Location IDs for bindable statement/entry sites.
    std::uint32_t debug_site_count;
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
struct CompileLocation {
    std::uint64_t offset{}, line{}, column{};
};
struct MaterialCode;
struct MaterialTextureInfo {
    const char* key;
    std::uint32_t slot;
    std::size_t offset;
};
struct MaterialCodeView {
    BusinessCode* shade;
    BusinessCode* defaults;
    const char* hlsl;
    const MaterialTextureInfo* textures;
    std::uint32_t texture_count, parameters, resources;
};

}

#ifdef ATELIER_SOURCE_BACKEND_BUILD
#define ATELIER_BUSINESS_BACKEND __declspec(dllexport)
#else
#define ATELIER_BUSINESS_BACKEND __declspec(dllimport)
#endif

// A module has one selected host entry and internal synchronous functions.
// entry_name may be omitted for a single function; results are scalar/enum or
// synchronous builtin math or plain record values.
// Environment members describe the host's validated Runtime binding; a null
// environment is empty. Code and LLVM state stay in their DLL; source bytes,
// catalog names and arrays are borrowed only for compile. Compiled metadata
// owns its signature, catalog, names and service snapshot.
// Diagnostics and the COFF file label abbreviate long display names with a
// bounded prefix/suffix, preserving trailing request identity and UTF-8 edges.
// Compiled source_name remains complete; display labels are not identity keys.
// Failure leaves *output unchanged, including an existing executable handle.
// Host callback code and this DLL must outlive all code owners and invocations.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t
atelier_compile_business(const atelier::source::detail::BusinessServices* services, const char* source_name,
                         const char* source, std::uint32_t source_size,
                         const atelier::source::detail::BusinessEnvironment* environment,
                         atelier::source::detail::BusinessCode** output, char* error, std::uint32_t error_capacity,
                         const char* entry_name = nullptr, atelier::source::CompileOptions options = {}) noexcept;

// One synchronous compilation with multiple typed host entries. Each output is
// a view owning the same JIT module; failure leaves every output untouched.
// Zero entries validates a declaration-only module without generating code.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_compile_business_entries(
    const atelier::source::detail::BusinessServices*, const char* source_name, const char* source,
    std::uint32_t source_size, const atelier::source::detail::BusinessEnvironment*, const char* const* entries,
    std::uint32_t entry_count, bool receiver, atelier::source::detail::BusinessCode** output,
    atelier::source::detail::CompileLocation*, char* error, std::uint32_t error_capacity,
    atelier::source::CompileOptions options = {}) noexcept;

// Detached pure-value compilation. Catalog/default capture is borrowed only
// during compile; neither the capture callback nor its context is retained.
// Code, HLSL and metadata are owned by MaterialCode, with no Runtime services.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t
atelier_compile_material(const char* name, const char* source, std::uint32_t size,
                         const atelier::source::detail::BusinessEnvironment*, void* capture_context,
                         const std::byte* (*capture_default)(void*, std::uint32_t) noexcept, std::uint32_t context,
                         std::uint32_t output_type, std::uint32_t texture_input,
                         std::uint32_t (*sample)(void*, std::uint32_t, const float*, float*) noexcept,
                         std::uint32_t (*orientation)(void*, std::uint32_t, const float*, float*) noexcept,
                         atelier::source::detail::MaterialCode** output, char* error, std::uint32_t capacity) noexcept;
extern "C" ATELIER_BUSINESS_BACKEND atelier::source::detail::MaterialCodeView
atelier_material_code_view(atelier::source::detail::MaterialCode*) noexcept;
extern "C" ATELIER_BUSINESS_BACKEND void atelier_destroy_material(atelier::source::detail::MaterialCode*) noexcept;

// Actual compiled metadata, not caller-provided claims. Null code returns null
// names/tables, a zero address and null callbacks. Querying acquires no owner.
extern "C" ATELIER_BUSINESS_BACKEND void
atelier_query_business_info(atelier::source::detail::BusinessCode* code,
                            atelier::source::detail::BusinessCodeInfo* output) noexcept;
// Borrowed views of optional compiler output, valid while code is owned.
extern "C" ATELIER_BUSINESS_BACKEND bool
atelier_query_business_artifacts(atelier::source::detail::BusinessCode* code,
                                 atelier::source::detail::BusinessArtifacts* output) noexcept;
inline atelier::source::detail::BusinessCodeInfo
atelier_business_info(atelier::source::detail::BusinessCode* code) noexcept {
    atelier::source::detail::BusinessCodeInfo output{};
    atelier_query_business_info(code, &output);
    return output;
}

// Query before allocating invocation storage. Null code returns zero counts.
extern "C" ATELIER_BUSINESS_BACKEND atelier::source::detail::BusinessFrameLayout
atelier_business_layout(atelier::source::detail::BusinessCode* code) noexcept;

// One invocation pins its compiled state before entering generated code.
// Destroying its outer handle in a synchronous callback does not free active
// machine code. This does not authorize concurrent use/destruction or DLL unload.
// Arguments are borrowed and must remain stable through this synchronous call.
// Arity, kinds and required storage are checked before invoking any callbacks.
// A non-void result requires storage; void permits null. Only full success
// commits its declared value; an aggregate result requires result->data to address an
// aligned value of its exact declared type. Failure leaves both the operand and
// its pointed-to result storage unchanged.
// Service failure status codes pass through unchanged; their first-cause text
// stays with the host invocation. Generated result-contract failures instead
// populate error after explicit cleanup, without replacing a host first cause.
// A generated failure exit also reports its innermost site in failure_location.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t
atelier_invoke_business(atelier::source::detail::BusinessCode* code, void* invocation,
                        const atelier::source::detail::BusinessValue* arguments, std::uint32_t argument_count,
                        atelier::source::detail::BusinessValue* result, char* error, std::uint32_t error_capacity,
                        atelier::source::detail::BusinessTrace* trace = nullptr,
                        std::uint32_t* failure_location = nullptr) noexcept;

extern "C" ATELIER_BUSINESS_BACKEND void atelier_destroy_business(atelier::source::detail::BusinessCode* code) noexcept;

// Private same-thread coroutine ABI. Creation copies arguments and owns its
// code version without executing the body. invocation must remain stable until
// destruction. resume commits result only on completion; suspended is separate
// from error status. Destruction cancels without running the source continuation.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t
atelier_create_business_coroutine(atelier::source::detail::BusinessCode* code, void* invocation,
                                  const atelier::source::detail::BusinessValue* arguments, std::uint32_t argument_count,
                                  atelier::source::detail::BusinessCoroutine** output, char* error,
                                  std::uint32_t error_capacity,
                                  atelier::source::detail::BusinessTrace* trace = nullptr) noexcept;
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t
atelier_resume_business_coroutine(atelier::source::detail::BusinessCoroutine* coroutine, bool* suspended,
                                  atelier::source::detail::BusinessValue* result, char* error,
                                  std::uint32_t error_capacity, std::uint32_t* failure_location = nullptr) noexcept;
extern "C" ATELIER_BUSINESS_BACKEND void
atelier_destroy_business_coroutine(atelier::source::detail::BusinessCoroutine* coroutine) noexcept;

#undef ATELIER_BUSINESS_BACKEND

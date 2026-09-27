#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace atelier::jit_acceptance {

inline constexpr std::uint32_t receiver_slot = 0;
inline constexpr std::uint32_t snapshot_slot = 1;
inline constexpr std::uint32_t business_root_slots = 2;
inline constexpr std::uint32_t observation_slot_bit = 0x80000000u;
inline constexpr std::uint32_t null_reference_slot = 0xffffffffu;

enum class ScalarKind : std::uint32_t { void_value, f32, u32 };
struct BusinessScalar {
    ScalarKind kind{ScalarKind::void_value};
    std::uint32_t bits{};
};
static_assert(std::is_standard_layout_v<BusinessScalar> && std::is_trivially_copyable_v<BusinessScalar>);
static_assert(sizeof(BusinessScalar) == 8 && alignof(BusinessScalar) == 4 && offsetof(BusinessScalar, bits) == 4);

struct BusinessFieldBinding { const char* name; ScalarKind kind; };
struct BusinessMethodBinding {
    const char* name;
    const ScalarKind* parameters;
    std::uint32_t parameter_count;
    ScalarKind result{ScalarKind::void_value};
    bool receiver_read_only{};
};
struct BusinessEnvironment {
    const BusinessFieldBinding* fields;
    std::uint32_t field_count;
    const BusinessMethodBinding* methods;
    std::uint32_t method_count;
    const char* object_type_name;
};
struct BusinessSignature {
    const ScalarKind* parameters;
    std::uint32_t parameter_count;
    ScalarKind result;
};

enum class ReferenceOperation : std::uint32_t { copy, observe, try_acquire };

// Private acceptance ABI. All semantic objects, roots and diagnostics stay in
// the host. Slots belong to one synchronous invocation, not an object map.
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
    // and bits only on success; void methods may receive a null result pointer.
    std::uint32_t (*call)(void*, std::uint32_t, std::uint32_t, const BusinessScalar*, std::uint32_t,
        BusinessScalar* result) noexcept;
    std::uint32_t (*poll)(void*) noexcept;
    std::uint32_t (*finish)(void*) noexcept;
    // Cannot fail for this compiler's validated strong or tagged weak slots.
    void (*clear)(void*, std::uint32_t) noexcept;
    std::uint32_t (*reference)(void*, ReferenceOperation, std::uint32_t destination,
        std::uint32_t source) noexcept;
    std::uint32_t (*same_reference)(void*, std::uint32_t left, std::uint32_t right,
        std::uint32_t* output) noexcept;

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
    // Maximum arity among calls in the compiled AST, not the host catalog.
    std::uint32_t max_call_arguments;
};
static_assert(std::is_trivial_v<BusinessFrameLayout> && std::is_standard_layout_v<BusinessFrameLayout>);

struct BusinessCode;

}

#ifdef ATELIER_JIT_PROBE_BUILD
#define ATELIER_BUSINESS_BACKEND __declspec(dllexport)
#else
#define ATELIER_BUSINESS_BACKEND __declspec(dllimport)
#endif

// Source has zero or more float/uint parameters and a void/float/uint result.
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
    const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::jit_acceptance::BusinessEnvironment* environment,
    atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity) noexcept;

// Acceptance-only: the same source compiler and real linker, but business_poll
// is deliberately left unbound. Failure preserves *output, including existing
// code; its diagnostic identifies the supplied source, phase and missing symbol.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_compile_business_without_poll_for_verification(
    const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::jit_acceptance::BusinessEnvironment* environment,
    atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity) noexcept;

// Actual compiled metadata, not caller-provided claims. Null code returns null
// names/tables, a zero address and null callbacks. Querying acquires no owner.
extern "C" ATELIER_BUSINESS_BACKEND atelier::jit_acceptance::BusinessCodeInfo atelier_business_info(
    atelier::jit_acceptance::BusinessCode* code) noexcept;

// Query before allocating invocation storage. Null code returns zero counts.
extern "C" ATELIER_BUSINESS_BACKEND atelier::jit_acceptance::BusinessFrameLayout atelier_business_layout(
    atelier::jit_acceptance::BusinessCode* code) noexcept;

// One invocation pins its compiled state before entering generated code.
// Destroying its outer handle in a synchronous callback does not free active
// machine code. This does not authorize concurrent use/destruction or DLL unload.
// Arguments are borrowed and must remain stable through this synchronous call.
// Arity, kinds and required storage are checked before invoking any callbacks.
// A non-void result requires storage; void permits null. Only full success
// commits {declared kind, bits}; failure leaves result entirely unchanged.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_invoke_business(
    atelier::jit_acceptance::BusinessCode* code, void* invocation,
    const atelier::jit_acceptance::BusinessScalar* arguments, std::uint32_t argument_count,
    atelier::jit_acceptance::BusinessScalar* result, char* error, std::uint32_t error_capacity) noexcept;

extern "C" ATELIER_BUSINESS_BACKEND void atelier_destroy_business(
    atelier::jit_acceptance::BusinessCode* code) noexcept;

// Acceptance-only fault injection: exercise the real compiler/linker with an
// omitted required service. Normal compilation never enables this path.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_verify_business_link_failure(
    const atelier::jit_acceptance::BusinessServices* services,
    char* error, std::uint32_t error_capacity) noexcept;

#undef ATELIER_BUSINESS_BACKEND

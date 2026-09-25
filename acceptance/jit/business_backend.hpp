#pragma once

#include <cstdint>

namespace atelier::jit_acceptance {

inline constexpr std::uint32_t receiver_slot = 0;
inline constexpr std::uint32_t snapshot_slot = 1;
inline constexpr std::uint32_t business_root_slots = 2;
inline constexpr std::uint32_t observation_slot_bit = 0x80000000u;
inline constexpr std::uint32_t null_reference_slot = 0xffffffffu;
inline constexpr std::uint32_t progress_field = 0;
inline constexpr std::uint32_t heat_field = 1;

enum class ReferenceOperation : std::uint32_t { copy, observe, try_acquire };

// Private acceptance ABI. All semantic objects, roots and diagnostics stay in
// the host. Slots belong to one synchronous invocation, not an object map.
// Strong slots are untagged; weak slots address a separate observation array.
// The null slot is an operand only, never a destination or cleanup target.
struct BusinessServices {
    std::uint32_t (*create)(void*, std::uint32_t) noexcept;
    std::uint32_t (*read_float)(void*, std::uint32_t, std::uint32_t, float*) noexcept;
    std::uint32_t (*write_float)(void*, std::uint32_t, std::uint32_t, float) noexcept;
    std::uint32_t (*read_cycles)(void*, std::uint32_t, std::uint32_t*) noexcept;
    std::uint32_t (*write_cycles)(void*, std::uint32_t, std::uint32_t) noexcept;
    std::uint32_t (*advance)(void*, std::uint32_t, float) noexcept;
    std::uint32_t (*poll)(void*) noexcept;
    std::uint32_t (*publish)(void*, std::uint32_t) noexcept;
    // Cannot fail for this compiler's validated strong or tagged weak slots.
    void (*clear)(void*, std::uint32_t) noexcept;
    std::uint32_t (*reference)(void*, ReferenceOperation, std::uint32_t destination,
        std::uint32_t source) noexcept;
    std::uint32_t (*same_reference)(void*, std::uint32_t left, std::uint32_t right,
        std::uint32_t* output) noexcept;

    bool operator==(const BusinessServices&) const = default;
};

struct BusinessCodeInfo {
    // Code-owned, null-terminated names; borrowed while an owner or active
    // invocation pin keeps this compiled state alive. This view owns no code.
    const char* function_name;
    const char* source_name;
    // Diagnostic/lifetime inspection only, not a durable callable handle.
    std::uintptr_t entry_address;
    // Exact callback values used for this compiled unit's symbol binding.
    BusinessServices services;
};

struct BusinessFrameLayout {
    std::uint32_t strong_slots;
    std::uint32_t weak_slots;
    // Code-owned, null-terminated name; borrowed while a compiled-state owner
    // or active invocation pin is alive. Empty/null retains the generic binding
    // used by scalar-only programs.
    const char* object_type_name;
};

struct BusinessCode;

}

#ifdef ATELIER_JIT_PROBE_BUILD
#define ATELIER_BUSINESS_BACKEND __declspec(dllexport)
#else
#define ATELIER_BUSINESS_BACKEND __declspec(dllimport)
#endif

// Source is a host-bound void function with float/uint parameters and implicit
// self. Names refer only to the Runtime-validated business adapter surface.
// Code and LLVM state stay in their DLL; source bytes and input names are only
// borrowed for compile. Compiled metadata owns its names and service snapshot.
// Diagnostics and the COFF file label abbreviate long display names with a
// bounded prefix/suffix, preserving trailing request identity and UTF-8 edges.
// Compiled source_name remains complete; display labels are not identity keys.
// A null object_type_name keeps the unnamed, generic scalar-only environment.
// Failure leaves *output unchanged, including an existing executable handle.
// Host callback code and this DLL must outlive all code owners and invocations.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_compile_business(
    const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const char* object_type_name, atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity) noexcept;

// Acceptance-only: the same source compiler and real linker, but business_poll
// is deliberately left unbound. Failure preserves *output, including existing
// code; its diagnostic identifies the supplied source, phase and missing symbol.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_compile_business_without_poll_for_verification(
    const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const char* object_type_name, atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity) noexcept;

// Actual compiled metadata, not caller-provided claims. Null code returns null
// names, a zero address and null callbacks. Querying does not acquire an owner.
extern "C" ATELIER_BUSINESS_BACKEND atelier::jit_acceptance::BusinessCodeInfo atelier_business_info(
    atelier::jit_acceptance::BusinessCode* code) noexcept;

// Query before allocating invocation storage. Null code returns zero counts.
extern "C" ATELIER_BUSINESS_BACKEND atelier::jit_acceptance::BusinessFrameLayout atelier_business_layout(
    atelier::jit_acceptance::BusinessCode* code) noexcept;

// One invocation pins its compiled state before entering generated code.
// Destroying its outer handle in a synchronous callback does not free active
// machine code. This does not authorize concurrent use/destruction or DLL unload.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_invoke_business(
    atelier::jit_acceptance::BusinessCode* code, void* invocation,
    float amount, std::uint32_t steps, char* error, std::uint32_t error_capacity) noexcept;

extern "C" ATELIER_BUSINESS_BACKEND void atelier_destroy_business(
    atelier::jit_acceptance::BusinessCode* code) noexcept;

// Acceptance-only fault injection: exercise the real compiler/linker with an
// omitted required service. Normal compilation never enables this path.
extern "C" ATELIER_BUSINESS_BACKEND std::uint32_t atelier_verify_business_link_failure(
    const atelier::jit_acceptance::BusinessServices* services,
    char* error, std::uint32_t error_capacity) noexcept;

#undef ATELIER_BUSINESS_BACKEND

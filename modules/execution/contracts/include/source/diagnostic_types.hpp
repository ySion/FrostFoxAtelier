#pragma once
#include <cstdint>

namespace atelier::source {
struct CompileOptions {
    bool diagnostics{};

    bool artifacts{};

    bool debugging{};

    bool operator==(const CompileOptions&) const = default;
};

enum class DiagnosticCall : std::uint32_t {
    field_read,
    field_write,
    native_call,
    object_create,
    reference,
    reference_compare,
    root_clear,
    value_construct,
    value_assign,
    value_destroy,
    sequence_mutation,
    sequence_view,
    coroutine_allocate,
    coroutine_free,
    source_call,
    debug_hook,
    count
};

struct DiagnosticCounters {
    std::uint64_t values[static_cast<std::uint32_t>(DiagnosticCall::count)]{};

    std::uint64_t operator[](DiagnosticCall kind) const noexcept { return values[static_cast<std::uint32_t>(kind)]; }
};
}

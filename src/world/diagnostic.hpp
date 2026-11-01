#pragma once

#include "runtime/runtime.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::world {

// The single vocabulary for object diagnostics. World execution, World
// consumers, physics and scene loading all produce this record; hosts add
// document identity and scene path from their own ObjectId mapping.
enum class Severity : std::uint8_t { info, warning, error };

enum class DiagnosticKind : std::uint8_t {
    behavior_failed,
    task_failed,
    invalid_value,
    degenerate_value,
    command_rejected,
    observer_failed,
    unknown_type,
    unknown_field,
    unknown_key,
    mismatch,
    converted,
    reference_dropped,
    missing_resource,
    unreadable_resource,
    capacity_exceeded,
};

enum class DiagnosticPhase : std::uint8_t {
    load,
    begin,
    tick,
    end,
    task,
    command,
    presentation,
    lighting,
    physics,
    lifecycle,
};

// Stable spellings for logs, reports and Control. Never localized.
[[nodiscard]] constexpr std::string_view name(Severity value) noexcept {
    switch (value) {
    case Severity::info:
        return "info";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::string_view name(DiagnosticKind value) noexcept {
    switch (value) {
    case DiagnosticKind::behavior_failed:
        return "behavior_failed";
    case DiagnosticKind::task_failed:
        return "task_failed";
    case DiagnosticKind::invalid_value:
        return "invalid_value";
    case DiagnosticKind::degenerate_value:
        return "degenerate_value";
    case DiagnosticKind::command_rejected:
        return "command_rejected";
    case DiagnosticKind::observer_failed:
        return "observer_failed";
    case DiagnosticKind::unknown_type:
        return "unknown_type";
    case DiagnosticKind::unknown_field:
        return "unknown_field";
    case DiagnosticKind::unknown_key:
        return "unknown_key";
    case DiagnosticKind::mismatch:
        return "mismatch";
    case DiagnosticKind::converted:
        return "converted";
    case DiagnosticKind::reference_dropped:
        return "reference_dropped";
    case DiagnosticKind::missing_resource:
        return "missing_resource";
    case DiagnosticKind::unreadable_resource:
        return "unreadable_resource";
    case DiagnosticKind::capacity_exceeded:
        return "capacity_exceeded";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::string_view name(DiagnosticPhase value) noexcept {
    switch (value) {
    case DiagnosticPhase::load:
        return "load";
    case DiagnosticPhase::begin:
        return "begin";
    case DiagnosticPhase::tick:
        return "tick";
    case DiagnosticPhase::end:
        return "end";
    case DiagnosticPhase::task:
        return "task";
    case DiagnosticPhase::command:
        return "command";
    case DiagnosticPhase::presentation:
        return "presentation";
    case DiagnosticPhase::lighting:
        return "lighting";
    case DiagnosticPhase::physics:
        return "physics";
    case DiagnosticPhase::lifecycle:
        return "lifecycle";
    }
    return "unknown";
}

struct Diagnostic {
    // Monotonic within its producer: a World's log or one scene load.
    std::uint64_t serial{};
    Severity severity{Severity::error};
    DiagnosticKind kind{};
    DiagnosticPhase phase{};
    // The failing Entity or Component and its owner Entity (equal for an
    // Entity). Both are null for document-level entries. An identity remains
    // meaningful as a value after its object is destroyed.
    ObjectId object, entity;
    std::string type; // Qualified type name, e.g. world.StaticMeshComponent.
    std::vector<std::string> field; // Field path names; empty when not field-specific.
    std::optional<SourceLocation> source; // Script failures, when the Source program recorded one.
    std::string value; // Truncated preview of the offending value; may be empty.
    std::string message;
    std::uint32_t repeats{1}; // Coalesced identical per-frame failures.
    // When forwarding a failed Result, message/source retain its original payload.
    std::optional<ErrorCode> error_code{};
};

}

#pragma once
#include "runtime/runtime.hpp"
#include "source/diagnostic_types.hpp"

namespace atelier::source {
namespace detail {
struct DiagnosticState;
struct DiagnosticAccess;
}
struct DiagnosticOptions {
    bool timing{};
};
enum class DiagnosticStatus { unused, ready, running, suspended, completed, failed, cancelled, rejected };
struct DiagnosticLocation {
    std::string function;
    SourceLocation source;
};
struct DiagnosticResources {
    std::size_t objects{}, roots{}, pending_reclamation{};
    CollectionPhase collection_phase{CollectionPhase::idle};
};
struct DiagnosticReport {
    std::uint64_t program_id{}, code_version{};
    std::string entry, source_name;
    DiagnosticStatus status{DiagnosticStatus::unused};
    DiagnosticCounters calls;
    std::uint64_t resumes{}, peak_owned_values{};
    std::size_t remaining_owned_values{};
    bool resources_cleared{};
    bool timing{};
    std::chrono::nanoseconds active_time{};
    DiagnosticResources before, after;
    std::optional<DiagnosticLocation> location;
    std::optional<Error> error;
};

// A same-thread, passive capture handle. One invocation may own a handle at a
// time; a terminal handle may be reused. Copies observe the same capture. An
// active coroutine retains its state even if the caller releases this handle.
// Snapshots own their text and never retain Runtime, executable code or frames.
class Diagnostics final {
    friend struct detail::DiagnosticAccess;
    std::shared_ptr<detail::DiagnosticState> state_;

public:
    explicit Diagnostics(DiagnosticOptions = {});
    [[nodiscard]] DiagnosticReport snapshot() const;
    [[nodiscard]] bool active() const noexcept;
};

struct CompilationArtifacts {
    std::string entry_symbol, target_triple, cpu_features;
    std::string original_ir, optimized_ir, assembly;
    // Actual compiler output before JIT relocation; section-relative symbols
    // and relocations are included in assembly. These bytes are not executable.
    std::vector<std::byte> object_code;
};
}

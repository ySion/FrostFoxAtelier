#pragma once
#include "source/diagnostics.hpp"

namespace atelier::source {
namespace detail {
struct DebugSessionState;

struct DebugAccess;
}

enum class DebugAction { continue_execution, step_into, step_over, step_out, abort };

enum class DebugStatus { unused, ready, running, paused, suspended, completed, failed, cancelled, rejected };

enum class DebugStopReason { entry, breakpoint, step, error };

enum class DebugValueState { available, uninitialized, out_of_scope, unavailable };

struct DebugCommand {
    std::uint64_t stop_id{};

    DebugAction action{DebugAction::continue_execution};
};

struct DebugBreakpoint {
    String path;

    std::uint64_t source_version{}; // Exact source identity, including a literal zero version.

    std::size_t line{};

    std::size_t column{}; // column == 0 binds every executable site on this exact line.

    DebugBreakpoint() noexcept = default;

    DebugBreakpoint(Utf8Text source_path, std::uint64_t version = 0, std::size_t source_line = 0,
                    std::size_t source_column = 0) noexcept
        : path(std::move(source_path).into_string()), source_version(version), line(source_line),
          column(source_column) {}
};

struct DebugValue {
    String name;

    String type_name;

    TypeId type;

    DebugValueState state{DebugValueState::unavailable};

    ParameterPassing passing{ParameterPassing::value};

    Optional<Scalar> scalar;

    ObjectId object;

    bool weak{};

    bool object_live{};

    String text;

    Array<DebugValue> children;

    std::size_t total_children{};

    bool truncated{};
};

struct DebugFrame {
    std::uint64_t activation_id{};

    DiagnosticLocation location;

    Array<DebugValue> locals;
};

struct DebugSnapshot {
    std::uint64_t program_id{};

    std::uint64_t code_version{};

    std::uint64_t stop_id{};

    DebugStopReason reason{DebugStopReason::entry};

    Array<DebugFrame> frames; // Innermost first. All data is owned; no live addresses or roots.

    Optional<Error> error;

    bool truncated{};

    DebugCommand command(DebugAction action) const noexcept { return {stop_id, action}; }
};

struct DebugOptions {
    bool break_on_entry{true};

    bool break_on_error{true};

    // Values, expanded children and copied text share one budget per snapshot.
    // Depth zero previews scalar roots but does not expand aggregate children.
    std::size_t max_frames{64};

    std::size_t max_values{256};

    std::size_t max_depth{4};

    std::size_t max_elements{32};

    std::size_t max_text_bytes{4096};
};

using DebugHandler = std::move_only_function<Result<DebugCommand>(const DebugSnapshot&) noexcept>;

// Same Runtime thread. A stop blocks invoke/resume on its existing native stack;
// returning a command resumes that call; failure ends it through normal cleanup.
// Handlers inspect owned snapshots only and never propagate C++ exceptions.
// One session belongs to one invocation/coroutine until terminal cleanup.
class Debugger final {
    friend struct detail::DebugAccess;

    std::shared_ptr<detail::DebugSessionState> state_;

public:
    explicit Debugger(DebugHandler = {}, DebugOptions = {});

    Result<void> set_breakpoints(Array<DebugBreakpoint>);

    [[nodiscard]] DebugStatus status() const noexcept;

    [[nodiscard]] bool active() const noexcept;

    [[nodiscard]] Optional<DebugSnapshot> last_stop() const;
};
}

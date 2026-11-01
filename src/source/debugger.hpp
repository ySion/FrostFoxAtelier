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
    std::string path;
    std::uint64_t source_version{}; // Exact source identity, including a literal zero version.
    std::size_t line{};
    std::size_t column{}; // column == 0 binds every executable site on this exact line.
};
struct DebugValue {
    std::string name;
    std::string type_name;
    TypeId type;
    DebugValueState state{DebugValueState::unavailable};
    ParameterPassing passing{ParameterPassing::value};
    std::optional<Scalar> scalar;
    ObjectId object;
    bool weak{};
    bool object_live{};
    std::string text;
    std::vector<DebugValue> children;
    std::size_t total_children{};
    bool truncated{};
};
struct DebugFrame {
    std::uint64_t activation_id{};
    DiagnosticLocation location;
    std::vector<DebugValue> locals;
};
struct DebugSnapshot {
    std::uint64_t program_id{};
    std::uint64_t code_version{};
    std::uint64_t stop_id{};
    DebugStopReason reason{DebugStopReason::entry};
    std::vector<DebugFrame> frames; // Innermost first. All data is owned; no live addresses or roots.
    std::optional<Error> error;
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
using DebugHandler = std::function<DebugCommand(const DebugSnapshot&)>;

// Same Runtime thread. A stop blocks invoke/resume on its existing native stack;
// returning a command resumes that call. Handlers inspect owned snapshots only.
// One session belongs to one invocation/coroutine until terminal cleanup.
class Debugger final {
    friend struct detail::DebugAccess;
    std::shared_ptr<detail::DebugSessionState> state_;

public:
    explicit Debugger(DebugHandler = {}, DebugOptions = {});
    Result<void> set_breakpoints(std::vector<DebugBreakpoint>);
    [[nodiscard]] DebugStatus status() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::optional<DebugSnapshot> last_stop() const;
};
}

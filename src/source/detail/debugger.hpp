#pragma once
#include "source/debugger.hpp"
#include "source/detail/diagnostics.hpp"

namespace atelier::source::detail {
struct DebugVariableMetadata {
    std::string name;
    BusinessType type;
    DebugStorage storage;
    ParameterPassing passing;
};
struct DebugFunctionMetadata { std::vector<DebugVariableMetadata> variables; };
struct DebugTables {
    std::vector<DebugFunctionMetadata> functions;
    std::vector<std::uint32_t> sites;
};
struct DebugMetadata {
    std::uint64_t program_id{}, code_version{};
    std::shared_ptr<const std::vector<DiagnosticLocation>> locations;
    std::shared_ptr<const DebugTables> tables;
};
struct DebugSessionState {
    DebugHandler handler;
    DebugOptions options;
    std::vector<DebugBreakpoint> breakpoints;
    std::shared_ptr<const DebugMetadata> metadata;
    std::vector<std::uint32_t> bound_sites;
    std::optional<DebugSnapshot> last_stop;
    DebugStatus status{DebugStatus::unused};
    DebugAction action{DebugAction::continue_execution};
    std::uint64_t next_stop_id{}, step_activation{};
    std::uint32_t failure_location{no_diagnostic_location};
    bool active{}, entry_stopped{}, error_stopped{}, handler_failed{};
};
std::shared_ptr<const DebugTables> copy_debug_tables(const BusinessCodeInfo&);
std::shared_ptr<const DebugMetadata> make_debug_metadata(const DiagnosticMetadata&,
    const BusinessCodeInfo&, std::shared_ptr<const DebugTables> = {});

struct DebugAccess {
    static Result<std::shared_ptr<DebugSessionState>> begin(Debugger*, const std::shared_ptr<const DebugMetadata>&, bool supported);
    static Error reject(Debugger*, Error);
    static void status(DebugSessionState&, DebugStatus) noexcept;
    static void finish(DebugSessionState&, DebugStatus) noexcept;
    static void locate_error(const DebugSessionState&, Error&);
};
// Invocation owns the active session; generated frames are borrowed only while
// on that invocation's native stack or in its LLVM coroutine frame.
std::uint32_t debug_enter(void*, BusinessDebugFrame*) noexcept;
std::uint32_t debug_point(void*, BusinessDebugFrame*, std::uint32_t, DebugPoint, EntryFailure) noexcept;
void debug_leave(void*, BusinessDebugFrame*) noexcept;

class DebugScope final {
    std::shared_ptr<DebugSessionState> state_;
    DebugStatus terminal_{DebugStatus::failed};
public:
    explicit DebugScope(std::shared_ptr<DebugSessionState> state) noexcept : state_(std::move(state)) {}
    ~DebugScope() { if (state_) DebugAccess::finish(*state_, terminal_); }
    std::shared_ptr<DebugSessionState> state() const noexcept { return state_; }
    template<class T> Result<T> complete(Result<T> result) {
        terminal_ = result ? DebugStatus::completed : result.error().code == ErrorCode::execution_aborted ?
            DebugStatus::cancelled : DebugStatus::failed;
        if (state_ && !result) DebugAccess::locate_error(*state_, result.error());
        return result;
    }
    void transfer(DebugStatus status) noexcept {
        if (state_) DebugAccess::status(*state_, status);
        state_.reset();
    }
};
}

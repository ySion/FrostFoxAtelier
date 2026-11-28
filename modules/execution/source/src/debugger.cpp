#include "detail/debugger.hpp"

#include <algorithm>
#include <cassert>

namespace atelier::source {
Debugger::Debugger(DebugHandler handler, DebugOptions options) : state_(std::make_shared<detail::DebugSessionState>()) {
    state_->handler = std::move(handler);
    state_->options = options;
}

Result<void> Debugger::set_breakpoints(Array<DebugBreakpoint> breakpoints) {
    if (active())
        return std::unexpected(Error{ErrorCode::busy, "Cannot change breakpoints during an active invocation."});
    for (const auto& breakpoint : breakpoints) {
        if (breakpoint.path.empty() || breakpoint.line == 0)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "A breakpoint requires an exact source path, version and line."});
    }
    if (!state_)
        state_ = std::make_shared<detail::DebugSessionState>();
    state_->breakpoints = std::move(breakpoints);
    return {};
}

DebugStatus Debugger::status() const noexcept {
    return state_ ? state_->status : DebugStatus::unused;
}

bool Debugger::active() const noexcept {
    return state_ && state_->active;
}

Optional<DebugSnapshot> Debugger::last_stop() const {
    return state_ ? state_->last_stop : Optional<DebugSnapshot>{};
}

namespace detail {
std::shared_ptr<const DebugTables> copy_debug_tables(const BusinessCodeInfo& info) {
    auto tables = std::make_shared<DebugTables>();
    if (!info.debugging)
        return tables;
    // The compiler owns these tables and publishes their actual storage/counts.
    assert((!info.debug_function_count || info.debug_functions) && (!info.debug_site_count || info.debug_sites));
    tables->functions.reserve(info.debug_function_count);
    for (std::uint32_t i = 0; i < info.debug_function_count; ++i) {
        const auto& input = info.debug_functions[i];
        assert(!input.variable_count || input.variables);
        auto& output = tables->functions.emplace();
        output.variables.reserve(input.variable_count);
        for (std::uint32_t j = 0; j < input.variable_count; ++j) {
            const auto& variable = input.variables[j];
            assert(variable.name);
            auto valid_name = String::from_utf8(StringView{variable.name});
            output.variables.add(
                {valid_name ? std::move(*valid_name) : String{}, variable.type, variable.storage, variable.passing});
        }
    }
    if (info.debug_site_count)
        tables->sites.assign(info.debug_sites, info.debug_sites + info.debug_site_count);
    assert(std::ranges::all_of(tables->sites, [&](const auto site) { return site < info.location_count; }));
    tables->failure_messages.reserve(info.failure_message_count);
    tables->failure_message_views.reserve(info.failure_message_count);
    for (std::uint32_t i = 0; i < info.failure_message_count; ++i) {
        if (info.failure_messages && info.failure_messages[i].data) {
            auto msg = String::from_utf8(StringView{info.failure_messages[i].data, info.failure_messages[i].size});
            tables->failure_messages.add(msg ? std::move(*msg) : "Source execution failed."_s);
        } else
            tables->failure_messages.emplace();
        const auto& stored = tables->failure_messages.back();
        tables->failure_message_views.add({stored.c_str(), stored.size()});
    }
    return tables;
}

std::shared_ptr<const DebugMetadata> make_debug_metadata(const DiagnosticMetadata& diagnostic,
                                                         const BusinessCodeInfo& info,
                                                         std::shared_ptr<const DebugTables> tables) {
    if (!tables)
        tables = copy_debug_tables(info);
    return std::make_shared<DebugMetadata>(
        DebugMetadata{diagnostic.program_id, diagnostic.code_version, diagnostic.locations, std::move(tables)});
}

namespace {
void reset_capture(DebugSessionState& state) noexcept {
    state.metadata.reset();
    state.bound_sites.clear();
    state.last_stop.reset();
    state.action = DebugAction::continue_execution;
    state.step_activation = 0;
    state.active = state.entry_stopped = state.error_stopped = state.handler_failed = false;
    state.status = DebugStatus::rejected;
    // Stop identities remain monotonic across reuse, so an old command never
    // becomes valid just because a second invocation reaches its first stop.
}
}

Result<std::shared_ptr<DebugSessionState>>
DebugAccess::begin(Debugger* debugger, const std::shared_ptr<const DebugMetadata>& metadata, bool supported) {
    if (!debugger)
        return std::shared_ptr<DebugSessionState>{};
    if (debugger->active())
        return std::unexpected(Error{ErrorCode::busy, "Debugger already belongs to an active invocation."});
    if (!debugger->state_)
        debugger->state_ = std::make_shared<DebugSessionState>();
    auto state = debugger->state_;
    reset_capture(*state);
    state->metadata = metadata;
    if (!supported)
        return std::unexpected(
            Error{ErrorCode::unsupported_operation, "This source version was compiled without debugging."});
    if (!state->metadata || !state->metadata->locations || !state->metadata->tables)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "This source version has incomplete debug metadata."});
    const auto& locations = *state->metadata->locations;
    for (const auto& breakpoint : state->breakpoints) {
        bool bound = false;
        for (const auto site : state->metadata->tables->sites) {
            const auto& source = locations[site].source;
            if (source.path == breakpoint.path && source.version == breakpoint.source_version &&
                source.line == breakpoint.line && (breakpoint.column == 0 || source.column == breakpoint.column)) {
                state->bound_sites.add(site);
                bound = true;
            }
        }
        if (!bound)
            return std::unexpected(Error{
                ErrorCode::invalid_member, "Breakpoint does not identify an executable site in this source version.",
                SourceLocation{breakpoint.path, breakpoint.source_version, 0, breakpoint.line, breakpoint.column}});
    }
    std::sort(state->bound_sites.begin(), state->bound_sites.end());
    state->bound_sites.deduplicate();
    state->active = true;
    state->status = DebugStatus::running;
    return state;
}

Error DebugAccess::reject(Debugger* debugger, Error error) {
    if (!debugger || debugger->active())
        return error;
    if (!debugger->state_)
        debugger->state_ = std::make_shared<DebugSessionState>();
    reset_capture(*debugger->state_);
    return error;
}

void DebugAccess::status(DebugSessionState& state, DebugStatus status) noexcept {
    if (state.active)
        state.status = status;
}

void DebugAccess::finish(DebugSessionState& state, DebugStatus status) noexcept {
    if (!state.active)
        return;
    state.status = status;
    state.active = false;
    state.action = DebugAction::continue_execution;
    state.step_activation = 0;
    state.bound_sites.clear();
    state.metadata.reset();
}
}
}

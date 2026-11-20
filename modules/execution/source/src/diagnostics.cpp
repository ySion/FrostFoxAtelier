#include "detail/diagnostics.hpp"
#include "foundation/string.hpp"
#include <algorithm>
#include <cstring>

namespace atelier::source {
namespace {
DiagnosticResources resources(const Runtime& runtime) noexcept {
    return {runtime.live_objects(), runtime.root_count(), runtime.pending_reclamation(), runtime.collection_phase()};
}

void pause_clock(detail::DiagnosticState& state) noexcept {
    if (!state.clock_running)
        return;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - state.started);
    state.active_time += std::min(elapsed, std::chrono::nanoseconds::max() - state.active_time);
    state.clock_running = false;
}

const DiagnosticLocation* location(const detail::DiagnosticState& state) noexcept {
    if (!state.metadata || !state.metadata->locations)
        return nullptr;
    auto index = state.trace.failure_location;
    if (index == detail::no_diagnostic_location &&
        (state.status == DiagnosticStatus::suspended || state.status == DiagnosticStatus::cancelled))
        index = state.trace.suspended_location;
    if (index == detail::no_diagnostic_location && state.status == DiagnosticStatus::running)
        index = state.trace.current_location;
    return index < state.metadata->locations->size() ? &(*state.metadata->locations)[index] : nullptr;
}
}

Diagnostics::Diagnostics(DiagnosticOptions options) : state_(std::make_shared<detail::DiagnosticState>()) {
    state_->options = options;
}

bool Diagnostics::active() const noexcept {
    return state_ && state_->active;
}

DiagnosticReport Diagnostics::snapshot() const {
    DiagnosticReport report;
    if (!state_)
        return report;
    const auto& state = *state_;
    if (state.metadata) {
        report.program_id = state.metadata->program_id;
        report.code_version = state.metadata->code_version;
        report.entry = state.metadata->entry;
        report.source_name = state.metadata->source_name;
    }
    report.status = state.status;
    report.calls = state.trace.calls;
    report.resumes = state.resumes;
    report.peak_owned_values = state.trace.peak_owned_values;
    report.remaining_owned_values = state.trace.remaining_owned_values;
    report.resources_cleared = state.trace.resources_cleared;
    report.timing = state.options.timing;
    report.active_time = state.active_time;
    if (state.clock_running) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - state.started);
        report.active_time += std::min(elapsed, std::chrono::nanoseconds::max() - report.active_time);
    }
    report.before = state.before;
    report.after = state.after;
    if (const auto* found = location(state))
        report.location = *found;
    if (state.failed) {
        report.error = Error{state.error_code, std::string(state.error, state.error_size)};
        if (report.location)
            report.error->source = report.location->source;
    }
    return report;
}

namespace detail {
Result<std::shared_ptr<DiagnosticState>>
DiagnosticAccess::begin(Diagnostics* capture, const std::shared_ptr<const DiagnosticMetadata>& metadata,
                        const Runtime& runtime, bool supported) {
    if (!capture)
        return std::shared_ptr<DiagnosticState>{};
    if (capture->active())
        return std::unexpected(Error{ErrorCode::busy, "Diagnostic capture already belongs to an active invocation."});
    if (!capture->state_)
        capture->state_ = std::make_shared<DiagnosticState>();
    auto state = capture->state_;
    const auto options = state->options;
    *state = {};
    state->options = options;
    state->metadata = metadata;
    state->before = state->after = resources(runtime);
    state->trace.resources_cleared = true;
    if (!supported) {
        Error error{ErrorCode::unsupported_operation, "This source version was compiled without diagnostics."};
        fail(*state, error);
        state->status = DiagnosticStatus::rejected;
        return std::unexpected(std::move(error));
    }
    state->active = true;
    running(*state);
    return state;
}

Error DiagnosticAccess::reject(Diagnostics* capture, Error error) {
    if (!capture || capture->active())
        return error;
    if (!capture->state_)
        capture->state_ = std::make_shared<DiagnosticState>();
    auto& state = *capture->state_;
    const auto options = state.options;
    state = {};
    state.options = options;
    state.trace.resources_cleared = true;
    state.status = DiagnosticStatus::rejected;
    fail(state, error);
    return error;
}

void DiagnosticAccess::running(DiagnosticState& state, bool resume) noexcept {
    state.status = DiagnosticStatus::running;
    if (resume) {
        diagnostic_add(state.resumes, 1);
        state.trace.suspended_location = no_diagnostic_location;
    }
    if (state.options.timing && !state.clock_running) {
        state.started = std::chrono::steady_clock::now();
        state.clock_running = true;
    }
}

void DiagnosticAccess::pause(DiagnosticState& state, const Runtime& runtime, DiagnosticStatus status) noexcept {
    pause_clock(state);
    state.after = resources(runtime);
    state.status = status;
}

void DiagnosticAccess::fail(DiagnosticState& state, const Error& error) noexcept {
    if (state.failed)
        return;
    state.failed = true;
    state.error_code = error.code;
    const auto prefix = StringView{error.message}.utf8_prefix(sizeof(state.error));
    std::memcpy(state.error, prefix.data(), prefix.size());
    state.error_size = prefix.size();
}

void DiagnosticAccess::finish(DiagnosticState& state, const Runtime& runtime, DiagnosticStatus status) noexcept {
    if (!state.active)
        return;
    pause(state, runtime, status);
    state.active = false;
}
}
}

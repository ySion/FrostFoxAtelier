#pragma once
#include "source/diagnostics.hpp"
#include "jit/source_backend.hpp"
#include <algorithm>

namespace atelier::source::detail {
struct DiagnosticMetadata {
    std::uint64_t program_id{};
    std::uint64_t code_version{};
    std::string entry;
    std::string source_name;
    std::shared_ptr<const std::vector<DiagnosticLocation>> locations;
};
struct DiagnosticState {
    DiagnosticOptions options;
    std::shared_ptr<const DiagnosticMetadata> metadata;
    BusinessTrace trace;
    DiagnosticStatus status{DiagnosticStatus::unused};
    bool active{};
    bool clock_running{};
    bool failed{};
    ErrorCode error_code{ErrorCode::invocation_failed};
    char error[512]{};
    std::uint64_t resumes{};
    std::chrono::steady_clock::time_point started;
    std::chrono::nanoseconds active_time{};
    DiagnosticResources before;
    DiagnosticResources after;
};
struct DiagnosticAccess {
    static Result<std::shared_ptr<DiagnosticState>>
    begin(Diagnostics*, const std::shared_ptr<const DiagnosticMetadata>&, const Runtime&, bool supported);
    static Error reject(Diagnostics*, Error);
    static void running(DiagnosticState&, bool resume = false) noexcept;
    static void pause(DiagnosticState&, const Runtime&, DiagnosticStatus) noexcept;
    static void fail(DiagnosticState&, const Error&) noexcept;
    static void finish(DiagnosticState&, const Runtime&, DiagnosticStatus) noexcept;
};
inline void diagnostic_add(std::uint64_t& target, std::uint64_t amount) noexcept {
    target += std::min(amount, UINT64_MAX - target);
}
// Covers the public call, including prepared arguments and their destruction.
// A coroutine transfers the same state to its persistent frame before return.
class DiagnosticScope final {
    std::shared_ptr<DiagnosticState> state_;
    const Runtime& runtime_;
    DiagnosticStatus terminal_{DiagnosticStatus::failed};
    bool completed_{};

public:
    DiagnosticScope(std::shared_ptr<DiagnosticState> state, const Runtime& runtime) noexcept
        : state_(std::move(state)), runtime_(runtime) {}
    ~DiagnosticScope() {
        if (!state_)
            return;
        if (!completed_) {
            // Unwinding must not allocate; preserve an already recorded cause.
            if (!state_->failed) {
                state_->failed = true;
                state_->error_code = ErrorCode::invocation_failed;
                constexpr char message[] = "Source invocation raised a native exception.";
                std::copy_n(message, sizeof(message), state_->error);
            }
        }
        DiagnosticAccess::finish(*state_, runtime_, terminal_);
    }
    BusinessTrace* trace() const noexcept { return state_ ? &state_->trace : nullptr; }
    std::shared_ptr<DiagnosticState> state() const noexcept { return state_; }
    template <class T>
    Result<T> complete(Result<T> result) {
        completed_ = true;
        terminal_ = result                                                ? DiagnosticStatus::completed
                    : result.error().code == ErrorCode::execution_aborted ? DiagnosticStatus::cancelled
                                                                          : DiagnosticStatus::failed;
        if (state_ && !result)
            DiagnosticAccess::fail(*state_, result.error());
        return result;
    }
    void transfer(DiagnosticStatus status) noexcept {
        if (state_)
            DiagnosticAccess::pause(*state_, runtime_, status);
        state_.reset();
    }
};
}

#pragma once

#include <gfx.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace atelier::rhi {

namespace detail {
struct ContextState;

struct PreparationJob;

class Preparer;
}

enum class Validation { disabled, debug_layer, gpu };

enum class PipelineKind { vertex, mesh, compute };

enum class ErrorCode {
    invalid_argument,
    unsupported,
    creation_failed,
    shader_failed,
    execution_failed,
    timeout,
    wrong_thread,
    busy,
    closed
};

struct Error {
    ErrorCode code = ErrorCode::execution_failed;

    GfxResult native_result = kGfxResult_NoError;

    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

struct ContextOptions {
    std::uint32_t width = 64;

    std::uint32_t height = 32;

    HWND window = nullptr;

    Validation validation = Validation::disabled;

    std::uint32_t wait_timeout_ms = 10000;

    bool rgba8_window = false; // Explicit SDR RGBA8 target instead of display auto-selection.

    // Window-only: one queued presentation, admitted with wait_frame before rendering.
    bool frame_latency_waitable = false;

    // Directory shared by processes for compiled shader stages; empty disables
    // it. Cache failures only cost recompilation.
    std::filesystem::path shader_cache{};
};

struct ContextInfo {
    std::uint32_t frames_in_flight = 0;

    bool debug_layer = false;

    bool gpu_validation = false;
};

struct TargetInfo {
    std::uint32_t width{};

    std::uint32_t height{};

    bool windowed{};

    bool drawable{};
};

using FrameCpuTimings = GfxFrameCpuTimings;

struct FrameInfo {
    TargetInfo rendered;

    TargetInfo next;

    bool occluded{};

    FrameCpuTimings cpu; // This submission's CPU wall time; GPU samples may describe an older frame.
};

// Source strings are borrowed only during prepare/submit. For file programs, optional
// absent stages retain gfx's file convention; any existing stage must compile.
struct PipelineDesc {
    PipelineKind kind = PipelineKind::compute;

    GfxProgramDesc source;

    const char* name = "pipeline";

    const char* file_name = nullptr;

    const char* file_path = nullptr;

    const char* entry_point = "main";

    const char* shader_model = "6_5";

    GfxDrawState draw_state;
};

class Pipeline {
public:
    Pipeline() = default;

    ~Pipeline();

    Pipeline(Pipeline&& other) noexcept;

    Pipeline& operator=(Pipeline&& other) noexcept;

    Pipeline(const Pipeline&) = delete;

    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] GfxProgram program() const noexcept;

    [[nodiscard]] GfxKernel kernel() const noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class Context;

    Pipeline(std::shared_ptr<detail::ContextState> state, GfxProgram program, GfxKernel kernel) noexcept;

    void reset() noexcept;

    std::shared_ptr<detail::ContextState> state_;

    GfxProgram program_;

    GfxKernel kernel_;
};

// A pipeline being prepared on Context workers. Dropping it, or closing the
// Context, cancels the work; it never holds the execution domain open.
class PipelineJob {
public:
    PipelineJob() = default;

    ~PipelineJob();

    PipelineJob(PipelineJob&& other) noexcept = default;

    PipelineJob& operator=(PipelineJob&& other) noexcept;

    PipelineJob(const PipelineJob&) = delete;

    PipelineJob& operator=(const PipelineJob&) = delete;

    // False once taken or for a default-constructed job.
    [[nodiscard]] explicit operator bool() const noexcept { return job_ != nullptr; }

private:
    friend class Context;

    void cancel() noexcept;

    std::shared_ptr<detail::Preparer> preparer_;

    std::shared_ptr<detail::PreparationJob> job_;
};

// All operations and final destruction occur on the opening thread. Borrowed
// gfx access is for ordinary resources, parameters and command encoding only;
// frame/finish/destroy/reloadAll/native import/submission remain owner-only.
class Context {
public:
    Context() = default;

    ~Context();

    Context(Context&& other) noexcept;

    Context& operator=(Context&& other) noexcept;

    Context(const Context&) = delete;

    Context& operator=(const Context&) = delete;

    [[nodiscard]] static Result<Context> open(const ContextOptions& options = {});

    [[nodiscard]] GfxContext borrow() const noexcept;

    [[nodiscard]] const ContextInfo& info() const noexcept;

    // Compiles, reflects and creates the root signature and PSO on this thread.
    // Under Validation::gpu it also records the pipeline once so GPU-based
    // validation patches it here instead of at its first real use.
    [[nodiscard]] Result<Pipeline> prepare(const PipelineDesc& desc);

    // The same preparation as prepare, run on up to eight Context workers. The
    // description is checked and copied before returning.
    [[nodiscard]] Result<PipelineJob> submit(const PipelineDesc& desc);

    // Empty while the job runs; then its Pipeline or its failure, exactly once.
    // A job cancelled by close returns ErrorCode::closed.
    [[nodiscard]] std::optional<Result<Pipeline>> take(PipelineJob& job);

    [[nodiscard]] Result<TargetInfo> target() const;

    // Owner-only, before recording the draw/execute callback. Synchronizes a
    // positive window client size even while hidden; returns the actual target.
    // A changed size drains pending work, but never presents or advances a frame.
    [[nodiscard]] Result<TargetInfo> sync_window_target();

    // Owner-only, including before the first frame. Waits for presentation capacity
    // or thread input, without dispatching messages. False means input/timeout,
    // not permission to render. A true result stays latched until frame; repeated
    // calls do not consume another signal. Non-waitable contexts return true.
    // timeout_ms is finite; zero is a single readiness check, not a polling loop.
    [[nodiscard]] Result<bool> wait_frame(std::uint32_t timeout_ms);

    // Details are published only after the entire frame succeeds. Positive DXGI
    // occlusion status is successful execution but not an accepted visible frame.
    // Requesting details also measures gfx frame stages; nullptr avoids clock reads.
    // A waitable window context must already hold its wait_frame permission.
    [[nodiscard]] Result<void> frame(bool vsync = false, FrameInfo* details = nullptr);

    [[nodiscard]] Result<void> finish();

    // Busy while Pipeline owners remain. Otherwise cancels queued preparation,
    // waits for running preparation steps, then releases the domain. Failure
    // retains the execution domain; close may drain submitted work but must
    // never resubmit it.
    [[nodiscard]] Result<void> close();

private:
    explicit Context(std::shared_ptr<detail::ContextState> state) noexcept;

    [[nodiscard]] Result<void> admit(const char* operation) const;

    [[nodiscard]] Result<std::shared_ptr<detail::PreparationJob>> job(const PipelineDesc& desc) const;

    [[nodiscard]] Result<Pipeline> adopt(detail::PreparationJob& job);

    std::shared_ptr<detail::ContextState> state_;
};

struct RecordedBinding {
    // Pointer fields are cleared after copying; opaque keys are diagnostic-only.
    GfxObservedBinding detail;

    std::array<char, 128> name{};

    std::array<char, 128> resource_name{};
};

struct RecordedCommand {
    std::array<char, 128> entry{};

    std::uint64_t kernel_key{};

    std::array<std::uint32_t, 3> groups{};

    std::uint32_t first_binding{};

    std::uint32_t binding_count{};

    bool indirect{};
};

struct CommandTraceInfo {
    std::uint64_t recording_sequence{};

    bool entered{};

    bool closed{};

    bool truncated{};

    bool unobserved_binding{};

    bool interrupted{};
};

// One-shot CPU snapshot, allocated before recording, with no GPU/Runtime owners.
// Storage is below 1 MiB (32 commands, 1024 bindings, bounded copied names).
// Recording never allocates, waits or throws. Limits mark the snapshot incomplete.
class CommandTrace {
public:
    CommandTrace();

    ~CommandTrace();

    CommandTrace(CommandTrace&&) noexcept;

    CommandTrace& operator=(CommandTrace&&) noexcept;

    [[nodiscard]] const CommandTraceInfo& info() const noexcept;

    [[nodiscard]] std::span<const RecordedCommand> commands() const noexcept;

    [[nodiscard]] std::span<const RecordedBinding> bindings() const noexcept;

private:
    friend class CommandTraceScope;

    struct Storage;

    std::unique_ptr<Storage> storage_;

    static void observe(void*, const GfxComputeObservation&) noexcept;
};

// Owner-thread borrow, strictly inside one render attempt. Context and snapshot
// must outlive the scope; neither may move while attached. No nested observers.
// Destruction detaches even when command recording fails. Disabled callers do
// not create a scope; gfx still pays observer checks, not a zero-cost guarantee.
class CommandTraceScope {
public:
    CommandTraceScope(Context&, CommandTrace&) noexcept;

    ~CommandTraceScope();

    CommandTraceScope(const CommandTraceScope&) = delete;

    CommandTraceScope& operator=(const CommandTraceScope&) = delete;

    // Mark only after the whole observed command range succeeds. A normal
    // early return without this call preserves the recorded prefix as interrupted.
    void complete() noexcept { completed_ = true; }

private:
    GfxContext context_;

    CommandTrace& trace_;

    bool completed_{};
};

} // namespace atelier::rhi

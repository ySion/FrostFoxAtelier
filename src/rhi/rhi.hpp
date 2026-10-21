#pragma once

#include <gfx.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace atelier::rhi {

namespace detail { struct ContextState; struct PreparationJob; class Preparer; }

enum class Validation { disabled, debug_layer, gpu };
enum class PipelineKind { vertex, mesh, compute };
enum class ErrorCode {
    invalid_argument, unsupported, creation_failed, shader_failed,
    execution_failed, timeout, wrong_thread, busy, closed
};

struct Error {
    ErrorCode code = ErrorCode::execution_failed;
    GfxResult native_result = kGfxResult_NoError;
    std::string message;
};

template<class T> using Result = std::expected<T, Error>;

struct ContextOptions {
    std::uint32_t width = 64;
    std::uint32_t height = 32;
    HWND window = nullptr;
    Validation validation = Validation::disabled;
    std::uint32_t wait_timeout_ms = 10000;
    bool rgba8_window = false; // Explicit SDR RGBA8 target instead of display auto-selection.
    // Directory shared by processes for compiled shader stages; empty disables
    // it. Cache failures only cost recompilation.
    std::filesystem::path shader_cache{};
};

struct ContextInfo {
    std::string adapter;
    std::uint32_t vendor_id = 0;
    std::uint32_t frames_in_flight = 0;
    bool debug_layer = false;
    bool gpu_validation = false;
};

struct TargetInfo {
    std::uint32_t width{}, height{};
    bool windowed{}, drawable{};
};
using FrameCpuTimings = GfxFrameCpuTimings;
struct FrameInfo {
    TargetInfo rendered, next;
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
    Pipeline(std::shared_ptr<detail::ContextState> state, GfxProgram program,
             GfxKernel kernel) noexcept;
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
    // Details are published only after the entire frame succeeds. Positive DXGI
    // occlusion status is successful execution but not an accepted visible frame.
    // Requesting details also measures gfx frame stages; nullptr avoids clock reads.
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

} // namespace atelier::rhi

#pragma once

#include <gfx.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace atelier::rhi {

namespace detail { struct ContextState; }

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
};

struct ContextInfo {
    std::string adapter;
    std::uint32_t vendor_id = 0;
    std::uint32_t frames_in_flight = 0;
    bool mesh_shader = false;
    bool debug_layer = false;
    bool gpu_validation = false;
};

// Source strings are borrowed only during prepare. For file programs, optional
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
    [[nodiscard]] Result<Pipeline> prepare(const PipelineDesc& desc);
    [[nodiscard]] Result<void> frame(bool vsync = false);
    [[nodiscard]] Result<void> finish();
    // Busy while Pipeline owners remain. Failure retains the execution domain;
    // close may drain submitted work but must never resubmit it.
    [[nodiscard]] Result<void> close();

private:
    explicit Context(std::shared_ptr<detail::ContextState> state) noexcept;
    std::shared_ptr<detail::ContextState> state_;
};

} // namespace atelier::rhi

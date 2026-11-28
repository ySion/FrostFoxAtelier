#pragma once

#include "foundation/string.hpp"

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <type_traits>

struct HWND__;

namespace atelier::rhi {

using NativeWindow = HWND__*;

struct BackendAccess;

namespace detail {
struct DeviceState;

struct SurfaceState;

struct PipelineState;

struct PreparationJob;

class Preparer;
} // namespace detail

enum class Validation : std::uint8_t { disabled, debug_layer, gpu };

enum class QueueKind : std::uint8_t { graphics = 0, compute = 1, copy = 2 };

enum class PipelineKind : std::uint8_t { vertex, mesh, compute };

enum class ErrorCode : std::uint8_t {
    invalid_argument,
    unsupported,
    creation_failed,
    shader_failed,
    out_of_memory,
    device_lost,
    execution_failed,
    timeout,
    wrong_thread,
    busy,
    closed
};

struct Error {
    Error() = default;

    Error(ErrorCode error_code, std::int32_t native = 0, Utf8Text text = {})
        : code(error_code), native_result(native), message(std::move(text).into_string()) {}

    template <std::same_as<StringView> SV>
    Error(ErrorCode error_code, std::int32_t native, SV text)
        : code(error_code), native_result(native),
          message(String::from_utf8(text).value_or("RHI error"_s)) {}

    template <std::same_as<const char*> P>
    Error(ErrorCode error_code, std::int32_t native, P text)
        : code(error_code), native_result(native),
          message(text ? String::from_utf8(StringView{text}).value_or("RHI error"_s) : String{}) {}

    ErrorCode code = ErrorCode::execution_failed;

    std::int32_t native_result = 0;

    String message;
};

template <class T>
using Result = std::expected<T, Error>;

struct DeviceOptions {
    Validation validation = Validation::disabled;

    std::uint32_t wait_timeout_ms = 10000;

    // Directory shared by processes for compiled DXIL stages; empty disables it.
    std::filesystem::path shader_cache{};
};

struct DeviceInfo {
    String adapter_name;

    std::uint64_t dedicated_video_memory = 0;

    std::uint32_t frames_in_flight = 3;

    bool debug_layer = false;

    bool gpu_validation = false;

    bool async_compute = false;

    bool copy_queue = false;
};

struct SurfaceOptions {
    std::uint32_t width = 64;

    std::uint32_t height = 32;

    bool srgb = true;

    // One queued presentation, admitted with wait_frame before rendering.
    bool frame_latency_waitable = false;
};

struct TargetInfo {
    std::uint32_t width{};

    std::uint32_t height{};

    bool windowed{};

    bool drawable{};
};

struct FrameCpuTimings {
    double submit_ms{};

    double present_ms{};

    double wait_ms{};

    double recycle_ms{};
};

struct FrameInfo {
    TargetInfo rendered;

    TargetInfo next;

    bool occluded{};

    FrameCpuTimings cpu;
};

// Move-only handle to a compiled graphics, mesh, or compute PSO.
// Retains the underlying execution domain until reset on the owner thread.
class Pipeline {
public:
    Pipeline() = default;

    ~Pipeline();

    Pipeline(Pipeline&& other) noexcept;

    Pipeline& operator=(Pipeline&& other) noexcept;

    Pipeline(const Pipeline&) = delete;

    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return pipeline_ != nullptr; }

    void reset() noexcept;

private:
    friend class Device;

    friend struct BackendAccess;

    friend struct detail::DeviceState;

    Pipeline(std::shared_ptr<detail::DeviceState> state, detail::PipelineState* pipeline) noexcept;

    std::shared_ptr<detail::DeviceState> state_;

    detail::PipelineState* pipeline_{};
};

// Asynchronous PSO compilation job running on Device worker threads.
// Never holds the execution domain open; dropping or closing cancels queued work.
class PipelineJob {
public:
    PipelineJob() = default;

    ~PipelineJob();

    PipelineJob(PipelineJob&& other) noexcept = default;

    PipelineJob& operator=(PipelineJob&& other) noexcept;

    PipelineJob(const PipelineJob&) = delete;

    PipelineJob& operator=(const PipelineJob&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return job_ != nullptr; }

private:
    friend class Device;

    friend struct BackendAccess;

    void cancel() noexcept;

    std::shared_ptr<detail::Preparer> preparer_;

    std::shared_ptr<detail::PreparationJob> job_;
};

// Lightweight per-HWND swapchain surface sharing a single process-wide Device.
class WindowSurface {
public:
    WindowSurface() noexcept;

    ~WindowSurface();

    WindowSurface(WindowSurface&& other) noexcept;

    WindowSurface& operator=(WindowSurface&& other) noexcept;

    WindowSurface(const WindowSurface&) = delete;

    WindowSurface& operator=(const WindowSurface&) = delete;

    [[nodiscard]] static Result<WindowSurface> open(Device& device, NativeWindow window,
                                                    const SurfaceOptions& options = {});

    [[nodiscard]] explicit operator bool() const noexcept { return surface_ != nullptr; }

    [[nodiscard]] Result<TargetInfo> target() const;

    // Synchronizes positive client dimensions in-place via ResizeBuffers without presenting.
    [[nodiscard]] Result<TargetInfo> sync_target();

    // Waits for frame-latency capacity or Win32 input without dispatching messages.
    [[nodiscard]] Result<bool> wait_frame(std::uint32_t timeout_ms);

    [[nodiscard]] Result<void> close();

private:
    friend class Device;

    friend struct BackendAccess;

    friend struct detail::DeviceState;

    WindowSurface(std::shared_ptr<detail::DeviceState> device, std::unique_ptr<detail::SurfaceState> surface) noexcept;

    std::shared_ptr<detail::DeviceState> device_;

    std::unique_ptr<detail::SurfaceState> surface_;
};

// Process-wide explicit GPU device (Graphics + Async Compute + Copy queues + SM 6.6 Bindless heap).
// All state-mutating calls and final destruction occur on the opening owner thread.
class Device {
public:
    Device() = default;

    ~Device();

    Device(Device&& other) noexcept;

    Device& operator=(Device&& other) noexcept;

    Device(const Device&) = delete;

    Device& operator=(const Device&) = delete;

    [[nodiscard]] static Result<Device> open(const DeviceOptions& options = {});

    [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    [[nodiscard]] const DeviceInfo& info() const noexcept;

    [[nodiscard]] std::uint32_t wait_timeout_ms() const noexcept;

    // Drains all three queues within wait_timeout_ms and reclaims retired resources.
    [[nodiscard]] Result<void> wait_idle();

    // Busy while any WindowSurface or Pipeline remains alive.
    [[nodiscard]] Result<void> close();

private:
    friend class WindowSurface;

    friend class Pipeline;

    friend struct BackendAccess;

    friend struct detail::DeviceState;

    explicit Device(std::shared_ptr<detail::DeviceState> state) noexcept;

    std::shared_ptr<detail::DeviceState> state_;
};

} // namespace atelier::rhi

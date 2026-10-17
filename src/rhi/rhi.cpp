#include "rhi.hpp"

#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>

namespace atelier::rhi {
namespace {
using Microsoft::WRL::ComPtr;

[[noreturn]] void lifetime_failure(const char* message) noexcept {
    std::fprintf(stderr, "RHI lifetime violation: %s\n", message);
    std::abort();
}

Error execution_error(GfxResult result) {
    return {result == kGfxResult_Timeout ? ErrorCode::timeout : ErrorCode::execution_failed,
            result, "gfx execution failed; only owner cleanup/close remains permitted"};
}

bool valid_shader_model(const char* value) {
    if (!value) return false;
    const std::string_view model(value);
    // gfx prefixes the suffix with up to four bytes ("lib_") in a 16-byte profile.
    if (model.size() > 11) return false;
    const auto separator = model.find('_');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == model.size()) return false;
    for (std::size_t i = 0; i < model.size(); ++i)
        if (i != separator && (model[i] < '0' || model[i] > '9')) return false;
    return true;
}
} // namespace

namespace detail {
struct ContextState {
    GfxContext context;
    ContextInfo info;
    const std::thread::id owner = std::this_thread::get_id();
    std::uint32_t pipelines = 0;
    GfxResult failure = kGfxResult_NoError;

    [[nodiscard]] bool on_owner() const noexcept { return owner == std::this_thread::get_id(); }
    ~ContextState() {
        if (!context) return;
        if (!on_owner()) lifetime_failure("final context destruction on a foreign thread");
        if (gfxDestroyContext(context) != kGfxResult_NoError)
            lifetime_failure("final context destruction cannot prove GPU completion; use close first");
    }
};
} // namespace detail

Pipeline::Pipeline(std::shared_ptr<detail::ContextState> state, GfxProgram program,
                   GfxKernel kernel) noexcept
    : state_(std::move(state)), program_(program), kernel_(kernel) {
    ++state_->pipelines;
}
Pipeline::~Pipeline() { reset(); }
Pipeline::Pipeline(Pipeline&& other) noexcept
    : state_(std::move(other.state_)), program_(std::exchange(other.program_, {})),
      kernel_(std::exchange(other.kernel_, {})) {}
Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        program_ = std::exchange(other.program_, {});
        kernel_ = std::exchange(other.kernel_, {});
    }
    return *this;
}
void Pipeline::reset() noexcept {
    if (!state_) return;
    if (!state_->on_owner()) lifetime_failure("pipeline destruction on a foreign thread");
    if (gfxDestroyKernel(state_->context, kernel_) != kGfxResult_NoError ||
        gfxDestroyProgram(state_->context, program_) != kGfxResult_NoError)
        lifetime_failure("pipeline cleanup failed");
    kernel_ = {};
    program_ = {};
    --state_->pipelines;
    state_.reset();
}
GfxProgram Pipeline::program() const noexcept {
    return state_ && state_->on_owner() && state_->failure == kGfxResult_NoError ? program_ : GfxProgram{};
}
GfxKernel Pipeline::kernel() const noexcept {
    return state_ && state_->on_owner() && state_->failure == kGfxResult_NoError ? kernel_ : GfxKernel{};
}
Pipeline::operator bool() const noexcept { return static_cast<bool>(kernel()); }

Context::Context(std::shared_ptr<detail::ContextState> state) noexcept : state_(std::move(state)) {}
Context::~Context() = default;
Context::Context(Context&& other) noexcept = default;
Context& Context::operator=(Context&& other) noexcept = default;

Result<Context> Context::open(const ContextOptions& options) {
    if (!options.width || !options.height || options.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        options.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !options.wait_timeout_ms || options.wait_timeout_ms == INFINITE ||
        (options.window && !IsWindow(options.window)))
        return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter,
                                     "invalid dimensions, window or finite wait budget"});
    if (options.validation != Validation::disabled && options.validation != Validation::debug_layer &&
        options.validation != Validation::gpu)
        return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "invalid validation mode"});

    const bool debug = options.validation != Validation::disabled;
    if (debug) {
        ComPtr<ID3D12Debug> controller;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&controller))))
            return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                                         "requested D3D12 debug layer is unavailable"});
        controller->EnableDebugLayer();
        if (options.validation == Validation::gpu) {
            ComPtr<ID3D12Debug1> gpu;
            if (FAILED(controller.As(&gpu)))
                return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                                             "requested GPU validation is unavailable"});
            gpu->SetEnableGPUBasedValidation(TRUE);
        }
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory))))
        return std::unexpected(Error{ErrorCode::creation_failed, kGfxResult_InternalError, "DXGI factory creation failed"});
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT result = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                   IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result))
            return std::unexpected(Error{ErrorCode::creation_failed, kGfxResult_InternalError, "DXGI adapter enumeration failed"});
        DXGI_ADAPTER_DESC1 desc{};
        ComPtr<ID3D12Device> candidate_device;
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 capabilities{};
        if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&candidate_device))) &&
            SUCCEEDED(candidate_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                &capabilities, sizeof(capabilities))) && capabilities.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            adapter = std::move(candidate);
            break;
        }
    }
    if (!adapter)
        return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                                     "no hardware adapter with FL12_1 and Mesh Shader support"});
    auto state = std::make_shared<detail::ContextState>();
    const GfxCreateContextFlags flags = (debug ? kGfxCreateContextFlag_EnableDebugLayer : 0) |
        (options.validation == Validation::gpu ? kGfxCreateContextFlag_EnableGPUBasedValidation : 0) |
        (options.rgba8_window ? kGfxCreateContextFlag_ForceRGBA8SwapChain : 0);
    state->context = options.window ? gfxCreateContext(options.window, flags, adapter.Get())
        : gfxCreateContext(options.width, options.height, flags, adapter.Get());
    if (!state->context)
        return std::unexpected(Error{ErrorCode::creation_failed, kGfxResult_DeviceError, "gfx context creation failed"});
    const GfxResult wait_result = gfxSetWaitTimeout(state->context, options.wait_timeout_ms);
    if (wait_result != kGfxResult_NoError) return std::unexpected(execution_error(wait_result));
    state->info.adapter = state->context.getName();
    state->info.vendor_id = state->context.getVendorId();
    state->info.frames_in_flight = gfxGetBackBufferCount(state->context);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 capabilities{};
    auto* device = gfxGetDevice(state->context);
    state->info.mesh_shader = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
        &capabilities, sizeof(capabilities))) && capabilities.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    if (!state->info.mesh_shader)
        return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation, "created device lacks Mesh Shader support"});
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_5};
    state->info.inline_ray_query = gfxIsRaytracingSupported(state->context) &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) &&
        shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    ComPtr<ID3D12InfoQueue> queue;
    state->info.debug_layer = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)));
    if (debug && !state->info.debug_layer)
        return std::unexpected(Error{ErrorCode::creation_failed, kGfxResult_DeviceError, "requested validation did not activate"});
    state->info.gpu_validation = options.validation == Validation::gpu;
    return Context(std::move(state));
}

GfxContext Context::borrow() const noexcept {
    return state_ && state_->on_owner() && state_->failure == kGfxResult_NoError ? state_->context : GfxContext{};
}
const ContextInfo& Context::info() const noexcept {
    static const ContextInfo empty;
    return state_ ? state_->info : empty;
}
Result<Pipeline> Context::prepare(const PipelineDesc& desc) {
    if (!state_ || !state_->context)
        return std::unexpected(Error{ErrorCode::closed, kGfxResult_InvalidOperation, "context is closed"});
    if (!state_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation, "prepare requires owner thread"});
    if (state_->failure != kGfxResult_NoError) return std::unexpected(execution_error(state_->failure));
    if (!desc.entry_point || !*desc.entry_point || !valid_shader_model(desc.shader_model) ||
        (desc.file_name && (!*desc.file_name || (desc.file_path && !*desc.file_path))))
        return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "invalid program description"});
    if (desc.kind != PipelineKind::compute && desc.kind != PipelineKind::vertex && desc.kind != PipelineKind::mesh)
        return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "invalid pipeline kind"});
    if (desc.kind == PipelineKind::mesh && !state_->info.mesh_shader)
        return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation, "mesh shaders are unsupported"});
    if (!desc.file_name) {
        const char* primary = desc.kind == PipelineKind::compute ? desc.source.cs
            : desc.kind == PipelineKind::vertex ? desc.source.vs : desc.source.ms;
        if (!primary || !*primary)
            return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "required source stage is missing"});
        const bool mismatched = desc.source.lib ||
            (desc.kind == PipelineKind::compute && (desc.source.as || desc.source.ms || desc.source.vs || desc.source.gs || desc.source.ps)) ||
            (desc.kind == PipelineKind::vertex && (desc.source.cs || desc.source.as || desc.source.ms)) ||
            (desc.kind == PipelineKind::mesh && (desc.source.cs || desc.source.vs || desc.source.gs));
        if (mismatched)
            return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "stage does not belong to pipeline kind"});
        for (const char* stage : {desc.source.cs, desc.source.as, desc.source.ms, desc.source.vs, desc.source.gs, desc.source.ps})
            if (stage && !*stage)
                return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "explicit source stage is empty"});
    }
    const GfxContext context = state_->context;
    GfxProgram program = desc.file_name
        ? gfxCreateProgram(context, desc.file_name, desc.file_path, desc.shader_model)
        : gfxCreateProgram(context, desc.source, desc.name, desc.shader_model);
    if (!program)
        return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter, "program creation failed"});
    GfxKernel kernel;
    switch (desc.kind) {
    case PipelineKind::compute: kernel = gfxCreateComputeKernel(context, program, desc.entry_point); break;
    case PipelineKind::vertex: kernel = gfxCreateGraphicsKernel(context, program, desc.draw_state, desc.entry_point); break;
    case PipelineKind::mesh: kernel = gfxCreateMeshKernel(context, program, desc.draw_state, desc.entry_point); break;
    }
    if (!kernel || !gfxKernelGetRootSignature(context, kernel) || !gfxKernelGetPipelineState(context, kernel)) {
        gfxDestroyKernel(context, kernel);
        gfxDestroyProgram(context, program);
        return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter,
                                     "pipeline preparation failed; existing pipelines are unchanged"});
    }
    return Pipeline(state_, program, kernel);
}
Result<TargetInfo> Context::target() const {
    if (!state_ || !state_->context)
        return std::unexpected(Error{ErrorCode::closed, kGfxResult_InvalidOperation, "context is closed"});
    if (!state_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation, "target requires owner thread"});
    if (state_->failure != kGfxResult_NoError) return std::unexpected(execution_error(state_->failure));
    const auto context = state_->context;
    const HWND window = gfxGetWindowHandle(context);
    RECT client{};
    const bool drawable = !window || (IsWindowVisible(window) && !IsIconic(window) &&
        GetClientRect(window, &client) && client.right > 0 && client.bottom > 0);
    return TargetInfo{gfxGetBackBufferWidth(context), gfxGetBackBufferHeight(context), window != nullptr, drawable};
}
Result<TargetInfo> Context::sync_window_target() {
    const auto current = target(); // Shared owner, closed and failure admission.
    if (!current) return std::unexpected(current.error());
    const GfxResult result = gfxSyncWindowTarget(state_->context);
    if (result == kGfxResult_NoError) return target();
    state_->failure = result;
    return std::unexpected(execution_error(result));
}
Result<void> Context::frame(bool vsync, FrameInfo* details) {
    if (!state_ || !state_->context)
        return std::unexpected(Error{ErrorCode::closed, kGfxResult_InvalidOperation, "context is closed"});
    if (!state_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation, "frame requires owner thread"});
    if (state_->failure != kGfxResult_NoError) return std::unexpected(execution_error(state_->failure));
    const auto rendered = target();
    if (!rendered) return std::unexpected(rendered.error());
    FrameCpuTimings cpu;
    const GfxResult result = gfxFrame(state_->context, vsync, details ? &cpu : nullptr);
    if (result == kGfxResult_NoError) {
        if (details) *details = {*rendered, *target(), gfxGetLastPresentResult(state_->context) == DXGI_STATUS_OCCLUDED, cpu};
        return {};
    }
    state_->failure = result;
    return std::unexpected(execution_error(result));
}
Result<void> Context::finish() {
    if (!state_ || !state_->context)
        return std::unexpected(Error{ErrorCode::closed, kGfxResult_InvalidOperation, "context is closed"});
    if (!state_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation, "finish requires owner thread"});
    if (state_->failure != kGfxResult_NoError) return std::unexpected(execution_error(state_->failure));
    const GfxResult result = gfxFinish(state_->context);
    if (result == kGfxResult_NoError) return {};
    state_->failure = result;
    return std::unexpected(execution_error(result));
}
Result<void> Context::close() {
    if (!state_ || !state_->context) return {};
    if (!state_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation, "close requires owner thread"});
    if (state_->pipelines)
        return std::unexpected(Error{ErrorCode::busy, kGfxResult_InvalidOperation, "release pipeline owners before close"});
    const GfxResult result = gfxDestroyContext(state_->context);
    if (result != kGfxResult_NoError) {
        state_->failure = result;
        return std::unexpected(execution_error(result));
    }
    state_->context = {};
    state_.reset();
    return {};
}
} // namespace atelier::rhi

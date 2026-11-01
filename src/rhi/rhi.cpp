#include "rhi.hpp"
#include "shader_compiler.hpp"

#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace atelier::rhi {
namespace {
using Microsoft::WRL::ComPtr;

[[noreturn]] void lifetime_failure(const char* message) noexcept {
    std::fprintf(stderr, "RHI lifetime violation: %s\n", message);
    std::abort();
}

Error execution_error(GfxResult result) {
    return {result == kGfxResult_Timeout ? ErrorCode::timeout : ErrorCode::execution_failed, result,
            "gfx execution failed; only owner cleanup/close remains permitted"};
}

bool valid_shader_model(const char* value) {
    if (!value)
        return false;
    const std::string_view model(value);
    // gfx prefixes the suffix with up to four bytes ("lib_") in a 16-byte profile.
    if (model.size() > 11)
        return false;
    const auto separator = model.find('_');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == model.size())
        return false;
    for (std::size_t i = 0; i < model.size(); ++i)
        if (i != separator && (model[i] < '0' || model[i] > '9'))
            return false;
    return true;
}

std::wstring widen(std::string_view text) {
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

bool meets_hardware_floor(ID3D12Device& device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 mesh{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 raytracing{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 barycentrics{};
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    return SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &mesh, sizeof(mesh))) &&
           mesh.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &raytracing, sizeof(raytracing))) &&
           raytracing.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &barycentrics, sizeof(barycentrics))) &&
           barycentrics.BarycentricsSupported &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
           model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
}
} // namespace

namespace detail {
struct PreparationJob {
    PipelineKind kind{};

    std::string name;

    std::string model;

    std::vector<std::pair<GfxShaderStage, ShaderSource>> stages;

    GfxKernelBuild* build{};

    std::atomic<bool> cancel{};

    // Guarded by Preparer::mutex_ once submitted; a worker owns build while running.
    enum class Phase { queued, running, done, cancelled } phase{Phase::queued};

    std::optional<Error> error;
};

// Runs preparation jobs. Owner-thread prepare uses the same steps inline.
// Workers touch only their job, its kernel build and the device, so stop()
// must finish before the gfx context is destroyed.
class Preparer {
public:
    struct Config {
        ID3D12Device* device{};

        bool gpu_validation{};

        std::filesystem::path cache;
    };

    explicit Preparer(Config config) : config_(std::move(config)) {}

    ~Preparer() { stop(); }

    Preparer(const Preparer&) = delete;

    Preparer& operator=(const Preparer&) = delete;

    Result<void> submit(std::shared_ptr<PreparationJob> job) {
        std::lock_guard lock(mutex_);
        if (stopped_)
            return std::unexpected(Error{ErrorCode::closed, kGfxResult_InvalidOperation, "context is closed"});
        try {
            for (auto count = std::clamp(std::thread::hardware_concurrency(), 1u, 8u); threads_.size() < count;)
                threads_.emplace_back([this] { work(); });
        } catch (const std::system_error&) {
            if (threads_.empty())
                return std::unexpected(
                    Error{ErrorCode::creation_failed, kGfxResult_InternalError, "preparation thread creation failed"});
        }
        live_.push_back(job);
        queue_.push_back(std::move(job));
        wake_.notify_one();
        return {};
    }

    void run_inline(PreparationJob& job) { run(job, owner_); }

    // True once the job left the workers; it is then done or cancelled.
    bool finished(PreparationJob& job) {
        std::lock_guard lock(mutex_);
        if (job.phase == PreparationJob::Phase::queued || job.phase == PreparationJob::Phase::running)
            return false;
        std::erase_if(live_, [&](const auto& item) { return item.get() == &job; });
        return true;
    }

    void cancel(PreparationJob& job) noexcept {
        std::lock_guard lock(mutex_);
        std::erase_if(live_, [&](const auto& item) { return item.get() == &job; });
        if (job.phase == PreparationJob::Phase::running)
            job.cancel = true;
        else
            discard(job);
    }

    // Cancels queued jobs, lets running steps finish, then releases every
    // build not yet taken. Idempotent.
    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            for (auto& job : queue_)
                discard(*job);
            queue_.clear();
            for (auto& job : live_)
                job->cancel = true;
        }
        wake_.notify_all();
        for (auto& thread : threads_)
            thread.join();
        threads_.clear();
        std::lock_guard lock(mutex_);
        for (auto& job : live_)
            discard(*job);
        live_.clear();
        owner_.release();
    }

private:
    struct Worker {
        std::optional<ShaderCompiler> compiler;

        ComPtr<ID3D12CommandAllocator> allocator;

        ComPtr<ID3D12GraphicsCommandList> list;

        void release() noexcept {
            if (list)
                list->Close();
            list.Reset();
            allocator.Reset();
            compiler.reset();
        }
    };

    static void discard(PreparationJob& job) noexcept {
        if (job.phase == PreparationJob::Phase::running)
            return;
        gfxKernelBuildDestroy(std::exchange(job.build, nullptr));
        job.phase = PreparationJob::Phase::cancelled;
    }

    void work() noexcept {
        Worker worker;
        for (;;) {
            std::shared_ptr<PreparationJob> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
                if (queue_.empty())
                    break;
                job = std::move(queue_.front());
                queue_.pop_front();
                if (job->phase != PreparationJob::Phase::queued)
                    continue;
                job->phase = PreparationJob::Phase::running;
            }
            run(*job, worker);
            std::lock_guard lock(mutex_);
            job->phase = PreparationJob::Phase::done;
            if (job->cancel)
                discard(*job);
        }
        worker.release();
    }

    // Failures become the job's error value; nothing escapes to the thread.
    void run(PreparationJob& job, Worker& worker) noexcept {
        try {
            if (!worker.compiler) {
                auto compiler = ShaderCompiler::open(config_.cache);
                if (!compiler) {
                    job.error = compiler.error();
                    return;
                }
                worker.compiler.emplace(std::move(*compiler));
            }
            for (const auto& [stage, source] : job.stages) {
                if (job.cancel)
                    return;
                auto compiled = worker.compiler->compile(source);
                if (!compiled) {
                    job.error = std::move(compiled.error());
                    return;
                }
                if (gfxKernelBuildSetStage(job.build, stage, compiled->bytecode.Get(), compiled->reflection.Get()) !=
                    kGfxResult_NoError) {
                    job.error = Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter,
                                      "duplicate shader stage for `" + job.name + "'"};
                    return;
                }
            }
            if (job.cancel)
                return;
            if (gfxKernelBuildFinish(job.build) != kGfxResult_NoError || !gfxKernelBuildGetRootSignature(job.build) ||
                !gfxKernelBuildGetPipelineState(job.build)) {
                job.error = Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter,
                                  "root signature or pipeline state creation failed for `" + job.name + "'"};
                return;
            }
            if (config_.gpu_validation && !job.cancel)
                warm(job, worker);
        } catch (...) {
            job.error =
                Error{ErrorCode::creation_failed, kGfxResult_OutOfMemory, "pipeline preparation failed to allocate"};
        }
    }

    // GPU-based validation creates its patched pipeline at the first
    // SetPipelineState of any command list. Recording one that never executes
    // moves that cost here; validation of real use is unchanged.
    void warm(const PreparationJob& job, Worker& worker) noexcept {
        if (!worker.list &&
            (FAILED(config_.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           IID_PPV_ARGS(&worker.allocator))) ||
             FAILED(config_.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, worker.allocator.Get(),
                                                      nullptr, IID_PPV_ARGS(&worker.list))))) {
            worker.release();
            return;
        }
        auto* signature = gfxKernelBuildGetRootSignature(job.build);
        if (job.kind == PipelineKind::compute)
            worker.list->SetComputeRootSignature(signature);
        else
            worker.list->SetGraphicsRootSignature(signature);
        worker.list->SetPipelineState(gfxKernelBuildGetPipelineState(job.build));
        if (FAILED(worker.list->Close()) || FAILED(worker.allocator->Reset()) ||
            FAILED(worker.list->Reset(worker.allocator.Get(), nullptr))) {
            worker.list.Reset();
            worker.allocator.Reset();
        }
    }

    const Config config_;

    std::mutex mutex_;

    std::condition_variable wake_;

    std::deque<std::shared_ptr<PreparationJob>> queue_;

    std::vector<std::shared_ptr<PreparationJob>> live_; // Submitted and not yet taken or cancelled.

    std::vector<std::thread> threads_;

    bool stopped_{};

    Worker owner_;
};

struct ContextState {
    GfxContext context;

    ContextInfo info;

    const std::thread::id owner = std::this_thread::get_id();

    std::uint32_t pipelines = 0;

    GfxResult failure = kGfxResult_NoError;

    std::shared_ptr<Preparer> preparer;

    [[nodiscard]] bool on_owner() const noexcept { return owner == std::this_thread::get_id(); }

    ~ContextState() {
        if (!context)
            return;
        if (!on_owner())
            lifetime_failure("final context destruction on a foreign thread");
        if (preparer)
            preparer->stop();
        if (gfxDestroyContext(context) != kGfxResult_NoError)
            lifetime_failure("final context destruction cannot prove GPU completion; use close first");
    }
};
} // namespace detail

Pipeline::Pipeline(std::shared_ptr<detail::ContextState> state, GfxProgram program, GfxKernel kernel) noexcept
    : state_(std::move(state)), program_(program), kernel_(kernel) {
    ++state_->pipelines;
}

Pipeline::~Pipeline() {
    reset();
}

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
    if (!state_)
        return;
    if (!state_->on_owner())
        lifetime_failure("pipeline destruction on a foreign thread");
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

Pipeline::operator bool() const noexcept {
    return static_cast<bool>(kernel());
}

Context::Context(std::shared_ptr<detail::ContextState> state) noexcept : state_(std::move(state)) {}

Context::~Context() = default;

Context::Context(Context&& other) noexcept = default;

Context& Context::operator=(Context&& other) noexcept = default;

Result<Context> Context::open(const ContextOptions& options) {
    if (!options.width || !options.height || options.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        options.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !options.wait_timeout_ms ||
        options.wait_timeout_ms == INFINITE || (options.window && !IsWindow(options.window)))
        return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter,
                                     "invalid dimensions, window or finite wait budget"});
    if (options.validation != Validation::disabled && options.validation != Validation::debug_layer &&
        options.validation != Validation::gpu)
        return std::unexpected(
            Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "invalid validation mode"});
    // Process-wide switch: debug lowers a GPU-based validation request to the debug layer; unset or gbv keeps it.
    Validation validation = options.validation;
    char switch_value[8]{};
    if (const DWORD length = GetEnvironmentVariableA("ATELIER_GPU_VALIDATION", switch_value, sizeof(switch_value))) {
        const std::string_view value(switch_value, length < sizeof(switch_value) ? length : 0);
        if (value != "gbv" && value != "debug")
            return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter,
                                         "ATELIER_GPU_VALIDATION must be gbv or debug"});
        if (value == "debug" && validation == Validation::gpu) {
            validation = Validation::debug_layer;
            std::fputs("RHI: ATELIER_GPU_VALIDATION=debug, GPU-based validation replaced by the D3D12 debug layer\n",
                       stderr);
        }
    }

    const bool debug = validation != Validation::disabled;
    if (debug) {
        ComPtr<ID3D12Debug> controller;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&controller))))
            return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                                         "requested D3D12 debug layer is unavailable"});
        controller->EnableDebugLayer();
        if (validation == Validation::gpu) {
            ComPtr<ID3D12Debug1> gpu;
            if (FAILED(controller.As(&gpu)))
                return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                                             "requested GPU validation is unavailable"});
            gpu->SetEnableGPUBasedValidation(TRUE);
        }
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory))))
        return std::unexpected(
            Error{ErrorCode::creation_failed, kGfxResult_InternalError, "DXGI factory creation failed"});
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT result =
            factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(result))
            return std::unexpected(
                Error{ErrorCode::creation_failed, kGfxResult_InternalError, "DXGI adapter enumeration failed"});
        DXGI_ADAPTER_DESC1 desc{};
        ComPtr<ID3D12Device> candidate_device;
        if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&candidate_device))) &&
            meets_hardware_floor(*candidate_device.Get())) {
            adapter = std::move(candidate);
            break;
        }
    }
    if (!adapter)
        return std::unexpected(
            Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                  "no hardware adapter with FL12_1, Mesh Shader and DXR 1.1 inline RayQuery support"});
    auto state = std::make_shared<detail::ContextState>();
    const GfxCreateContextFlags flags =
        (debug ? kGfxCreateContextFlag_EnableDebugLayer : 0) |
        (validation == Validation::gpu ? kGfxCreateContextFlag_EnableGPUBasedValidation : 0) |
        (options.rgba8_window ? kGfxCreateContextFlag_ForceRGBA8SwapChain : 0);
    state->context = options.window ? gfxCreateContext(options.window, flags, adapter.Get())
                                    : gfxCreateContext(options.width, options.height, flags, adapter.Get());
    if (!state->context)
        return std::unexpected(
            Error{ErrorCode::creation_failed, kGfxResult_DeviceError, "gfx context creation failed"});
    const GfxResult wait_result = gfxSetWaitTimeout(state->context, options.wait_timeout_ms);
    if (wait_result != kGfxResult_NoError)
        return std::unexpected(execution_error(wait_result));
    state->info.adapter = state->context.getName();
    state->info.vendor_id = state->context.getVendorId();
    state->info.frames_in_flight = gfxGetBackBufferCount(state->context);
    auto* device = gfxGetDevice(state->context);
    // gfx drops its DXR device when it cannot obtain the DXR interfaces.
    if (!meets_hardware_floor(*device) || !gfxIsRaytracingSupported(state->context))
        return std::unexpected(Error{ErrorCode::unsupported, kGfxResult_InvalidOperation,
                                     "created device lacks Mesh Shader or DXR 1.1 inline RayQuery support"});
    ComPtr<ID3D12InfoQueue> queue;
    state->info.debug_layer = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)));
    if (debug && !state->info.debug_layer)
        return std::unexpected(
            Error{ErrorCode::creation_failed, kGfxResult_DeviceError, "requested validation did not activate"});
    state->info.gpu_validation = validation == Validation::gpu;
    state->preparer = std::make_shared<detail::Preparer>(
        detail::Preparer::Config{device, state->info.gpu_validation, options.shader_cache});
    return Context(std::move(state));
}

GfxContext Context::borrow() const noexcept {
    return state_ && state_->on_owner() && state_->failure == kGfxResult_NoError ? state_->context : GfxContext{};
}

const ContextInfo& Context::info() const noexcept {
    static const ContextInfo empty;
    return state_ ? state_->info : empty;
}

Result<void> Context::admit(const char* operation) const {
    if (!state_ || !state_->context)
        return std::unexpected(Error{ErrorCode::closed, kGfxResult_InvalidOperation, "context is closed"});
    if (!state_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation,
                                     std::string(operation) + " requires owner thread"});
    if (state_->failure != kGfxResult_NoError)
        return std::unexpected(execution_error(state_->failure));
    return {};
}

// Checks and copies the description on the owner. File stages are read here,
// so workers never touch the file system except through #include.
Result<std::shared_ptr<detail::PreparationJob>> Context::job(const PipelineDesc& desc) const {
    if (!desc.entry_point || !*desc.entry_point || !valid_shader_model(desc.shader_model) ||
        (desc.file_name && (!*desc.file_name || (desc.file_path && !*desc.file_path))))
        return std::unexpected(
            Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "invalid program description"});
    if (desc.kind != PipelineKind::compute && desc.kind != PipelineKind::vertex && desc.kind != PipelineKind::mesh)
        return std::unexpected(
            Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "invalid pipeline kind"});
    if (!desc.file_name) {
        const char* primary = desc.kind == PipelineKind::compute  ? desc.source.cs
                              : desc.kind == PipelineKind::vertex ? desc.source.vs
                                                                  : desc.source.ms;
        if (!primary || !*primary)
            return std::unexpected(
                Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "required source stage is missing"});
        const bool mismatched =
            desc.source.lib ||
            (desc.kind == PipelineKind::compute &&
             (desc.source.as || desc.source.ms || desc.source.vs || desc.source.gs || desc.source.ps)) ||
            (desc.kind == PipelineKind::vertex && (desc.source.cs || desc.source.as || desc.source.ms)) ||
            (desc.kind == PipelineKind::mesh && (desc.source.cs || desc.source.vs || desc.source.gs));
        if (mismatched)
            return std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter,
                                         "stage does not belong to pipeline kind"});
        for (const char* stage :
             {desc.source.cs, desc.source.as, desc.source.ms, desc.source.vs, desc.source.gs, desc.source.ps})
            if (stage && !*stage)
                return std::unexpected(
                    Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter, "explicit source stage is empty"});
    }
    struct Stage {
        GfxShaderStage stage;

        const char* source;

        const wchar_t* profile;

        const wchar_t* extension;

        bool primary;
    };
    const auto& s = desc.source;
    const std::vector<Stage> stages =
        desc.kind == PipelineKind::compute  ? std::vector<Stage>{{kGfxShaderStage_CS, s.cs, L"cs_", L".comp", true}}
        : desc.kind == PipelineKind::vertex ? std::vector<Stage>{{kGfxShaderStage_VS, s.vs, L"vs_", L".vert", true},
                                                                 {kGfxShaderStage_GS, s.gs, L"gs_", L".geom", false},
                                                                 {kGfxShaderStage_PS, s.ps, L"ps_", L".frag", false}}
                                            : std::vector<Stage>{{kGfxShaderStage_AS, s.as, L"as_", L".task", false},
                                                                 {kGfxShaderStage_MS, s.ms, L"ms_", L".mesh", true},
                                                                 {kGfxShaderStage_PS, s.ps, L"ps_", L".frag", false}};
    auto job = std::make_shared<detail::PreparationJob>();
    job->kind = desc.kind;
    job->model = desc.shader_model;
    job->name = desc.file_name ? desc.file_name : desc.name ? desc.name : "pipeline";
    const std::wstring model = widen(desc.shader_model);
    const std::wstring entry = widen(desc.entry_point);
    std::error_code error;
    const auto directory = desc.file_name ? std::filesystem::absolute(desc.file_path ? desc.file_path : ".", error)
                                          : std::filesystem::path{};
    if (error)
        return std::unexpected(
            Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter, "shader directory is unusable"});
    for (const auto& stage : stages) {
        detail::ShaderSource source{{}, {}, stage.profile + model, entry};
        if (desc.file_name) {
            // Like gfx: an absent optional stage file is skipped; any other access failure is an error.
            const auto path = directory / (widen(desc.file_name) + stage.extension);
            const bool exists = std::filesystem::exists(path, error);
            if (error || (!exists && stage.primary))
                return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter,
                                             "cannot open shader file " + path.string()});
            if (!exists)
                continue;
            std::ifstream file(path, std::ios::binary);
            source.text.assign(std::istreambuf_iterator<char>(file), {});
            if (!file && !file.eof())
                return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter,
                                             "cannot read shader file " + path.string()});
            source.name = path.wstring();
        } else {
            if (!stage.source)
                continue;
            source.text = stage.source;
            source.name = widen(job->name) + stage.extension;
        }
        job->stages.emplace_back(stage.stage, std::move(source));
    }
    const auto type = desc.kind == PipelineKind::compute  ? kGfxKernelBuildType_Compute
                      : desc.kind == PipelineKind::vertex ? kGfxKernelBuildType_Graphics
                                                          : kGfxKernelBuildType_Mesh;
    job->build = gfxKernelBuildCreate(state_->context, type, desc.draw_state, job->name.c_str(), desc.entry_point);
    if (!job->build)
        return std::unexpected(
            Error{ErrorCode::creation_failed, kGfxResult_InternalError, "kernel build creation failed"});
    return job;
}

// Owner-only registration of a finished job; the build is consumed either way.
Result<Pipeline> Context::adopt(detail::PreparationJob& job) {
    if (job.phase == detail::PreparationJob::Phase::cancelled)
        return std::unexpected(
            Error{ErrorCode::closed, kGfxResult_InvalidOperation, "pipeline preparation was cancelled"});
    if (job.error) {
        gfxKernelBuildDestroy(std::exchange(job.build, nullptr));
        return std::unexpected(std::move(*job.error));
    }
    const GfxContext context = state_->context;
    const GfxProgram program = gfxCreateProgram(context, GfxProgramDesc{}, job.name.c_str(), job.model.c_str());
    if (!program) {
        gfxKernelBuildDestroy(std::exchange(job.build, nullptr));
        return std::unexpected(Error{ErrorCode::creation_failed, kGfxResult_InternalError, "program creation failed"});
    }
    const GfxKernel kernel = gfxKernelBuildAdopt(context, program, std::exchange(job.build, nullptr));
    if (!kernel) {
        gfxDestroyProgram(context, program);
        return std::unexpected(
            Error{ErrorCode::creation_failed, kGfxResult_InternalError, "kernel registration failed"});
    }
    return Pipeline(state_, program, kernel);
}

Result<Pipeline> Context::prepare(const PipelineDesc& desc) {
    if (const auto admitted = admit("prepare"); !admitted)
        return std::unexpected(admitted.error());
    auto job = this->job(desc);
    if (!job)
        return std::unexpected(std::move(job.error()));
    state_->preparer->run_inline(**job);
    return adopt(**job);
}

Result<PipelineJob> Context::submit(const PipelineDesc& desc) {
    if (const auto admitted = admit("submit"); !admitted)
        return std::unexpected(admitted.error());
    auto job = this->job(desc);
    if (!job)
        return std::unexpected(std::move(job.error()));
    if (auto queued = state_->preparer->submit(*job); !queued) {
        gfxKernelBuildDestroy(std::exchange((*job)->build, nullptr));
        return std::unexpected(std::move(queued.error()));
    }
    PipelineJob handle;
    handle.preparer_ = state_->preparer;
    handle.job_ = std::move(*job);
    return handle;
}

std::optional<Result<Pipeline>> Context::take(PipelineJob& job) {
    if (const auto admitted = admit("take"); !admitted)
        return Result<Pipeline>(std::unexpected(admitted.error()));
    if (!job.job_ || job.preparer_ != state_->preparer)
        return Result<Pipeline>(std::unexpected(Error{ErrorCode::invalid_argument, kGfxResult_InvalidParameter,
                                                      "job was already taken or belongs to another context"}));
    if (!state_->preparer->finished(*job.job_))
        return std::nullopt;
    const auto finished = std::exchange(job.job_, {});
    job.preparer_.reset();
    return adopt(*finished);
}

PipelineJob::~PipelineJob() {
    cancel();
}

PipelineJob& PipelineJob::operator=(PipelineJob&& other) noexcept {
    if (this != &other) {
        cancel();
        preparer_ = std::move(other.preparer_);
        job_ = std::move(other.job_);
    }
    return *this;
}

void PipelineJob::cancel() noexcept {
    if (job_)
        preparer_->cancel(*job_);
    job_.reset();
    preparer_.reset();
}

Result<TargetInfo> Context::target() const {
    if (const auto admitted = admit("target"); !admitted)
        return std::unexpected(admitted.error());
    const auto context = state_->context;
    const HWND window = gfxGetWindowHandle(context);
    RECT client{};
    const bool drawable = !window || (IsWindowVisible(window) && !IsIconic(window) && GetClientRect(window, &client) &&
                                      client.right > 0 && client.bottom > 0);
    return TargetInfo{gfxGetBackBufferWidth(context), gfxGetBackBufferHeight(context), window != nullptr, drawable};
}

Result<TargetInfo> Context::sync_window_target() {
    const auto current = target(); // Shared owner, closed and failure admission.
    if (!current)
        return std::unexpected(current.error());
    const GfxResult result = gfxSyncWindowTarget(state_->context);
    if (result == kGfxResult_NoError)
        return target();
    state_->failure = result;
    return std::unexpected(execution_error(result));
}

Result<void> Context::frame(bool vsync, FrameInfo* details) {
    if (const auto admitted = admit("frame"); !admitted)
        return std::unexpected(admitted.error());
    const auto rendered = target();
    if (!rendered)
        return std::unexpected(rendered.error());
    FrameCpuTimings cpu;
    const GfxResult result = gfxFrame(state_->context, vsync, details ? &cpu : nullptr);
    if (result == kGfxResult_NoError) {
        if (details)
            *details = {*rendered, *target(), gfxGetLastPresentResult(state_->context) == DXGI_STATUS_OCCLUDED, cpu};
        return {};
    }
    state_->failure = result;
    return std::unexpected(execution_error(result));
}

Result<void> Context::finish() {
    if (const auto admitted = admit("finish"); !admitted)
        return std::unexpected(admitted.error());
    const GfxResult result = gfxFinish(state_->context);
    if (result == kGfxResult_NoError)
        return {};
    state_->failure = result;
    return std::unexpected(execution_error(result));
}

Result<void> Context::close() {
    if (!state_ || !state_->context)
        return {};
    if (!state_->on_owner())
        return std::unexpected(
            Error{ErrorCode::wrong_thread, kGfxResult_InvalidOperation, "close requires owner thread"});
    if (state_->pipelines)
        return std::unexpected(
            Error{ErrorCode::busy, kGfxResult_InvalidOperation, "release pipeline owners before close"});
    state_->preparer->stop();
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

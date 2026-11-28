#include "rhi/backend.hpp"
#include "detail/shader_compiler.hpp"
#include "foundation/result.hpp"
#include "memory/fatal.hpp"
#include "platform/utf.hpp"

#include <mimalloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace atelier::rhi {
namespace {
using Microsoft::WRL::ComPtr;

void* NRI_CALL nri_allocate(void*, std::size_t size, std::size_t alignment) {
    const std::size_t align = std::max<std::size_t>(alignment, sizeof(void*));
    void* ptr = mi_malloc_aligned(std::max<std::size_t>(size, 1), align);
    if (!ptr)
        atelier::fatal("NRI host memory allocation exhausted");
    return ptr;
}

void* NRI_CALL nri_reallocate(void*, void* memory, std::size_t size, std::size_t alignment) {
    const std::size_t align = std::max<std::size_t>(alignment, sizeof(void*));
    if (size == 0) {
        mi_free(memory);
        return nullptr;
    }
    void* ptr = mi_realloc_aligned(memory, size, align);
    if (!ptr)
        atelier::fatal("NRI host memory reallocation exhausted");
    return ptr;
}

void NRI_CALL nri_free(void*, void* memory) {
    mi_free(memory);
}

void NRI_CALL nri_noop_abort(void*) {}

void NRI_CALL nri_message_callback(nri::Message type, const char* file, std::uint32_t line, const char* message,
                                   void*) {
    if (type == nri::Message::ERROR || (message && std::strstr(message, "[DeviceLost]") != nullptr))
        std::fprintf(stderr, "[NRI %s %s:%u] %s\n", type == nri::Message::ERROR ? "ERROR" : "DRED", file ? file : "?",
                     line, message ? message : "");
}

Error map_nri_error(nri::Result res, std::string_view context) {
    ErrorCode code = ErrorCode::execution_failed;
    switch (res) {
    case nri::Result::INVALID_ARGUMENT:
        code = ErrorCode::invalid_argument;
        break;
    case nri::Result::UNSUPPORTED:
        code = ErrorCode::unsupported;
        break;
    case nri::Result::OUT_OF_MEMORY:
        code = ErrorCode::out_of_memory;
        break;
    case nri::Result::DEVICE_LOST:
        code = ErrorCode::device_lost;
        break;
    default:
        code = ErrorCode::execution_failed;
        break;
    }
    return {code, static_cast<std::int32_t>(res), std::string(context)};
}

bool valid_shader_model(const char* value) {
    if (!value)
        return false;
    const std::string_view model(value);
    if (model.size() < 3 || model.size() > 11)
        return false;
    const auto sep = model.find('_');
    if (sep == std::string_view::npos || sep == 0 || sep + 1 == model.size())
        return false;
    for (std::size_t i = 0; i < model.size(); ++i)
        if (i != sep && (model[i] < '0' || model[i] > '9'))
            return false;
    return true;
}

const wchar_t* stage_prefix(nri::StageBits stage) {
    switch (stage) {
    case nri::StageBits::VERTEX_SHADER:
        return L"vs_";
    case nri::StageBits::FRAGMENT_SHADER:
        return L"ps_";
    case nri::StageBits::TASK_SHADER:
        return L"as_";
    case nri::StageBits::MESH_SHADER:
        return L"ms_";
    case nri::StageBits::COMPUTE_SHADER:
        return L"cs_";
    default:
        return nullptr;
    }
}

Result<Validation> resolve_validation_env(Validation requested) {
    wchar_t value[32]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD len = GetEnvironmentVariableW(L"ATELIER_GPU_VALIDATION", value, 32);
    if (len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return requested;
    if (len == 0 || len >= 32)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "ATELIER_GPU_VALIDATION must be gbv or debug"});
    const std::wstring_view text(value, len);
    if (text != L"gbv" && text != L"debug")
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "ATELIER_GPU_VALIDATION must be gbv or debug"});
    if (text == L"debug" && requested == Validation::gpu) {
        std::fputs("RHI: ATELIER_GPU_VALIDATION=debug, GPU-based validation replaced by the D3D12 debug layer\n",
                   stderr);
        return Validation::debug_layer;
    }
    return requested;
}

bool check_hardware_floor(ID3D12Device& device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 mesh{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 bary{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 enhanced{};
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    return SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &mesh, sizeof(mesh))) &&
           mesh.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &rt, sizeof(rt))) &&
           rt.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &bary, sizeof(bary))) &&
           bary.BarycentricsSupported &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &enhanced, sizeof(enhanced))) &&
           enhanced.EnhancedBarriersSupported &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
           sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6 &&
           SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))) &&
           opts.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
}

} // namespace

namespace detail {

// ============================================================================
// BindlessHeap
// ============================================================================

Result<void> BindlessHeap::initialize(nri::Device& device, const nri::CoreInterface& core,
                                      const nri::DescriptorHeapInterface& heap_iface) {
    heap_iface_ = &heap_iface;
    bool initialized_ok = false;
    struct CleanupGuard {
        BindlessHeap* self;

        const nri::CoreInterface* core;

        const nri::DescriptorHeapInterface* heap_iface;

        bool* ok;

        ~CleanupGuard() {
            if (!*ok)
                self->shutdown(*core, *heap_iface);
        }
    } guard{this, &core, &heap_iface, &initialized_ok};

    nri::DescriptorHeapDesc heap_desc{};
    heap_desc.resourceDescriptorNum = bindless_resource_capacity;
    heap_desc.samplerDescriptorNum = bindless_sampler_capacity;
    nri::Result res = heap_iface.CreateDescriptorHeap(device, heap_desc, heap_);
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "Failed to create global Bindless descriptor heap"));

    // Populate reserved Bindless slot 0 with a valid zeroed buffer SRV sentinel so out-of-range clamped descriptors
    // never touch uninitialized heap memory.
    nri::BufferDesc sentinel_desc{};
    sentinel_desc.size = 1024u;
    sentinel_desc.byteAddress = true;
    sentinel_desc.usage = nri::BufferUsageBits::SHADER_RESOURCE;
    res = core.CreateCommittedBuffer(device, nri::MemoryLocation::HOST_UPLOAD, 0.0f, sentinel_desc, sentinel_buffer_);
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "Failed to create Bindless slot-0 sentinel buffer"));
    void* sentinel_mapped = core.MapBuffer(*sentinel_buffer_, 0u, sentinel_desc.size);
    if (!sentinel_mapped)
        return std::unexpected(Error{ErrorCode::out_of_memory, 0, "Failed to map Bindless slot-0 sentinel buffer"});
    std::memset(sentinel_mapped, 0, static_cast<std::size_t>(sentinel_desc.size));
    core.UnmapBuffer(*sentinel_buffer_);

    nri::BufferViewDesc sentinel_view{};
    sentinel_view.buffer = sentinel_buffer_;
    sentinel_view.type = nri::BufferView::BYTE_ADDRESS_BUFFER;
    sentinel_view.offset = 0u;
    sentinel_view.size = nri::WHOLE_SIZE;
    res = core.CreateBufferView(sentinel_view, sentinel_srv_);
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "Failed to create Bindless slot-0 sentinel SRV"));
    nri::WriteResourceDescriptorsDesc sentinel_write{sentinel_srv_, 0u};
    res = heap_iface.WriteResourceDescriptors(*heap_, &sentinel_write, 1u);
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "Failed to write Bindless slot-0 sentinel SRV"));

    auto make_sampler = [&](nri::Filter mag_min, nri::Filter mip, nri::AddressMode addr, std::uint8_t aniso,
                            nri::CompareOp cmp) -> Result<nri::Descriptor*> {
        nri::SamplerDesc desc{};
        desc.filters = {mag_min, mag_min, mip, nri::FilterOp::AVERAGE};
        desc.anisotropy = aniso;
        desc.mipMax = 16.f;
        desc.addressModes = {addr, addr, addr};
        desc.compareOp = cmp;
        nri::Descriptor* out{};
        const nri::Result r = core.CreateSampler(device, desc, out);
        if (r != nri::Result::SUCCESS)
            return std::unexpected(map_nri_error(r, "Failed to create immutable static sampler"));
        return out;
    };

    ATELIER_TRY_ASSIGN(static_samplers_[0], make_sampler(nri::Filter::LINEAR, nri::Filter::LINEAR,
                                                         nri::AddressMode::CLAMP_TO_EDGE, 1, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[1], make_sampler(nri::Filter::LINEAR, nri::Filter::LINEAR,
                                                         nri::AddressMode::REPEAT, 1, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[2], make_sampler(nri::Filter::LINEAR, nri::Filter::LINEAR,
                                                         nri::AddressMode::MIRRORED_REPEAT, 1, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[3], make_sampler(nri::Filter::NEAREST, nri::Filter::NEAREST,
                                                         nri::AddressMode::CLAMP_TO_EDGE, 1, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[4], make_sampler(nri::Filter::NEAREST, nri::Filter::NEAREST,
                                                         nri::AddressMode::REPEAT, 1, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[5], make_sampler(nri::Filter::LINEAR, nri::Filter::LINEAR,
                                                         nri::AddressMode::REPEAT, 16, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[6], make_sampler(nri::Filter::LINEAR, nri::Filter::LINEAR,
                                                         nri::AddressMode::CLAMP_TO_EDGE, 16, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[7], make_sampler(nri::Filter::LINEAR, nri::Filter::LINEAR,
                                                         nri::AddressMode::MIRRORED_REPEAT, 16, nri::CompareOp::NONE));
    ATELIER_TRY_ASSIGN(static_samplers_[8],
                       make_sampler(nri::Filter::LINEAR, nri::Filter::NEAREST, nri::AddressMode::CLAMP_TO_EDGE, 1,
                                    nri::CompareOp::GREATER_EQUAL));

    std::array<nri::WriteSamplerDescriptorsDesc, static_cast<std::size_t>(StaticSampler::count)> sampler_writes{};
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(static_samplers_.size()); ++i)
        sampler_writes[i] = {static_samplers_[i], i};
    res = heap_iface.WriteSamplerDescriptors(*heap_, sampler_writes.data(),
                                             static_cast<std::uint32_t>(sampler_writes.size()));
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "Failed to write static samplers into Bindless heap"));

    nri::RootConstantDesc root_consts{};
    root_consts.registerIndex = 0;
    root_consts.size = root_constant_byte_size;
    root_consts.shaderStages = nri::StageBits::ALL_SHADERS;

    nri::PipelineLayoutDesc layout_desc{};
    layout_desc.rootRegisterSpace = 0;
    layout_desc.rootConstants = &root_consts;
    layout_desc.rootConstantNum = 1;
    layout_desc.shaderStages = nri::StageBits::ALL_SHADERS;
    layout_desc.flags = nri::PipelineLayoutBits::RESOURCE_HEAP_DIRECTLY_INDEXED |
                        nri::PipelineLayoutBits::SAMPLER_HEAP_DIRECTLY_INDEXED;
    res = core.CreatePipelineLayout(device, layout_desc, layout_);
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "Failed to create universal SM 6.6 Bindless PipelineLayout"));

    next_index_ = 1; // Reserve index 0 as zeroed sentinel.
    initialized_ok = true;
    return {};
}

void BindlessHeap::shutdown(const nri::CoreInterface& core, const nri::DescriptorHeapInterface& heap_iface) noexcept {
    if (layout_) {
        core.DestroyPipelineLayout(layout_);
        layout_ = nullptr;
    }
    if (sentinel_srv_) {
        core.DestroyDescriptor(sentinel_srv_);
        sentinel_srv_ = nullptr;
    }
    if (sentinel_buffer_) {
        core.DestroyBuffer(sentinel_buffer_);
        sentinel_buffer_ = nullptr;
    }
    for (auto*& sampler : static_samplers_) {
        if (sampler) {
            core.DestroyDescriptor(sampler);
            sampler = nullptr;
        }
    }
    if (heap_) {
        heap_iface.DestroyDescriptorHeap(heap_);
        heap_ = nullptr;
    }
    free_indices_.clear();
    next_index_ = 0;
}

Result<std::uint32_t> BindlessHeap::allocate_resource(nri::Descriptor* descriptor) {
    if (!descriptor)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "Null descriptor passed to BindlessHeap"});
    std::uint32_t index = bindless_invalid_index;
    {
        std::lock_guard lock(mutex_);
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
        } else {
            if (next_index_ >= bindless_resource_capacity)
                return std::unexpected(
                    Error{ErrorCode::out_of_memory, 0, "Global Bindless resource descriptor heap exhausted"});
            index = next_index_++;
        }
    }
    update_resource(index, descriptor);
    return index;
}

void BindlessHeap::update_resource(std::uint32_t index, nri::Descriptor* descriptor) noexcept {
    nri::WriteResourceDescriptorsDesc write_desc{descriptor, index};
    static_cast<void>(heap_iface_->WriteResourceDescriptors(*heap_, &write_desc, 1u));
}

void BindlessHeap::free_resource(std::uint32_t index) noexcept {
    if (index == 0 || index == bindless_invalid_index)
        return;
    std::lock_guard lock(mutex_);
    free_indices_.push_back(index);
}

void BindlessHeap::bind(nri::CommandBuffer& cmd, const nri::CoreInterface& core,
                        const nri::DescriptorHeapInterface& heap_iface, QueueKind resolved_queue) const noexcept {
    if (resolved_queue == QueueKind::copy)
        return;
    heap_iface.CmdSetDescriptorHeap(cmd, *heap_);
    if (resolved_queue == QueueKind::graphics)
        core.CmdSetPipelineLayout(cmd, nri::BindPoint::GRAPHICS, *layout_);
    core.CmdSetPipelineLayout(cmd, nri::BindPoint::COMPUTE, *layout_);
}

// ============================================================================
// QueueTimeline
// ============================================================================

QueueTimeline::~QueueTimeline() {
    for (auto& lane : lanes_) {
        if (lane.event) {
            CloseHandle(lane.event);
            lane.event = nullptr;
        }
    }
}

Result<void> QueueTimeline::initialize(nri::Device& device, ID3D12Device& d3d12_device, const nri::CoreInterface& core,
                                       const nri::WrapperD3D12Interface& wrapper, bool has_async_compute,
                                       bool has_copy_queue) {
    nri_device_ = &device;
    d3d12_device_ = &d3d12_device;
    core_ = &core;
    has_compute_ = has_async_compute;
    has_copy_ = has_copy_queue;

    bool initialized_ok = false;
    struct CleanupGuard {
        QueueTimeline* self;

        const nri::CoreInterface* core;

        bool* ok;

        ~CleanupGuard() {
            if (!*ok)
                self->shutdown(*core);
        }
    } guard{this, &core, &initialized_ok};

    const std::array<std::pair<nri::QueueType, bool>, 3> types{{
        {nri::QueueType::GRAPHICS, true},
        {nri::QueueType::COMPUTE, has_async_compute},
        {nri::QueueType::COPY, has_copy_queue},
    }};

    for (std::size_t i = 0; i < 3; ++i) {
        if (!types[i].second)
            continue;
        auto& lane = lanes_[i];
        const nri::Result qr = core.GetQueue(device, types[i].first, 0, lane.queue);
        if (qr != nri::Result::SUCCESS || !lane.queue)
            return std::unexpected(map_nri_error(qr, "Failed to retrieve NRI hardware queue"));
        lane.d3d12_queue = static_cast<ID3D12CommandQueue*>(core.GetQueueNativeObject(lane.queue));
        HRESULT hr = d3d12_device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lane.d3d12_fence));
        if (FAILED(hr))
            return std::unexpected(Error{ErrorCode::creation_failed, hr, "ID3D12Device::CreateFence failed"});
        nri::FenceD3D12Desc fdesc{};
        fdesc.d3d12Fence = lane.d3d12_fence.Get();
        const nri::Result fr = wrapper.CreateFenceD3D12(device, fdesc, lane.nri_fence);
        if (fr != nri::Result::SUCCESS)
            return std::unexpected(map_nri_error(fr, "CreateFenceD3D12 failed"));
        lane.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!lane.event)
            return std::unexpected(
                Error{ErrorCode::creation_failed, static_cast<std::int32_t>(GetLastError()), "CreateEventW failed"});
        if (lane.d3d12_queue)
            static_cast<void>(lane.d3d12_queue->GetTimestampFrequency(&lane.frequency));
    }
    initialized_ok = true;
    return {};
}

void QueueTimeline::shutdown(const nri::CoreInterface& core) noexcept {
    for (auto& lane : lanes_) {
        if (lane.nri_fence) {
            core.DestroyFence(lane.nri_fence);
            lane.nri_fence = nullptr;
        }
        lane.d3d12_fence.Reset();
        if (lane.event) {
            CloseHandle(lane.event);
            lane.event = nullptr;
        }
        lane.queue = nullptr;
        lane.d3d12_queue = nullptr;
        lane.last_signaled = 0;
        lane.frequency = 0;
    }
}

QueueKind QueueTimeline::resolve_kind(QueueKind requested) const noexcept {
    if (requested == QueueKind::compute && !has_compute_)
        return QueueKind::graphics;
    if (requested == QueueKind::copy && !has_copy_)
        return QueueKind::graphics;
    return requested;
}

nri::Queue* QueueTimeline::queue(QueueKind kind) const noexcept {
    return lanes_[static_cast<std::size_t>(resolve_kind(kind))].queue;
}

nri::Fence* QueueTimeline::fence(QueueKind kind) const noexcept {
    return lanes_[static_cast<std::size_t>(resolve_kind(kind))].nri_fence;
}

std::uint64_t QueueTimeline::current_value(QueueKind kind) const noexcept {
    return lanes_[static_cast<std::size_t>(resolve_kind(kind))].last_signaled;
}

FencePoint QueueTimeline::current_point() const noexcept {
    return {{lanes_[0].last_signaled, lanes_[1].last_signaled, lanes_[2].last_signaled}};
}

bool QueueTimeline::is_device_lost() const noexcept {
    if (!d3d12_device_)
        return false;
    if (d3d12_device_->GetDeviceRemovedReason() != S_OK)
        return true;
    for (const auto& lane : lanes_)
        if (lane.d3d12_fence && lane.d3d12_fence->GetCompletedValue() == UINT64_MAX)
            return true;
    return false;
}

void QueueTimeline::report_dred_once() const noexcept {
    if (!nri_device_ || dred_reported_.exchange(true))
        return;
    nri::DeviceLostDump dump{};
    static_cast<void>(nri::nriReportDeviceLostInfo(*nri_device_, dump));
}

Result<std::uint64_t> QueueTimeline::completed_value(QueueKind kind) const {
    const auto idx = static_cast<std::size_t>(resolve_kind(kind));
    const auto& lane = lanes_[idx];
    if (!lane.d3d12_fence)
        return 0u;
    const std::uint64_t val = lane.d3d12_fence->GetCompletedValue();
    if (val == UINT64_MAX || (d3d12_device_ && d3d12_device_->GetDeviceRemovedReason() != S_OK)) {
        report_dred_once();
        return std::unexpected(
            Error{ErrorCode::device_lost,
                  d3d12_device_ ? static_cast<std::int32_t>(d3d12_device_->GetDeviceRemovedReason()) : E_FAIL,
                  "GPU device lost detected on timeline fence query"});
    }
    return val;
}

Result<FencePoint> QueueTimeline::completed_point() const {
    FencePoint out{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!lanes_[i].d3d12_fence)
            continue;
        ATELIER_TRY_ASSIGN(out.values[i], completed_value(static_cast<QueueKind>(i)));
    }
    return out;
}

Result<std::uint64_t> QueueTimeline::submit(QueueKind kind, std::span<nri::CommandBuffer* const> command_buffers,
                                            std::span<const std::pair<QueueKind, std::uint64_t>> waits) {
    auto& lane = lanes_[static_cast<std::size_t>(resolve_kind(kind))];
    std::vector<nri::FenceSubmitDesc> wait_descs;
    wait_descs.reserve(waits.size());
    for (const auto& [wait_kind, wait_val] : waits) {
        if (wait_val == 0)
            continue;
        const auto resolved = resolve_kind(wait_kind);
        if (resolved == resolve_kind(kind))
            continue;
        nri::FenceSubmitDesc fd{};
        fd.fence = lanes_[static_cast<std::size_t>(resolved)].nri_fence;
        fd.value = wait_val;
        fd.stages = nri::StageBits::ALL;
        wait_descs.push_back(fd);
    }

    const std::uint64_t sig_val = lane.last_signaled + 1u;
    nri::FenceSubmitDesc sig_desc{};
    sig_desc.fence = lane.nri_fence;
    sig_desc.value = sig_val;
    sig_desc.stages = nri::StageBits::ALL;

    nri::QueueSubmitDesc sub{};
    sub.waitFences = wait_descs.data();
    sub.waitFenceNum = static_cast<std::uint32_t>(wait_descs.size());
    sub.commandBuffers = command_buffers.data();
    sub.commandBufferNum = static_cast<std::uint32_t>(command_buffers.size());
    sub.signalFences = &sig_desc;
    sub.signalFenceNum = 1;

    const nri::Result res = core_->QueueSubmit(*lane.queue, sub);
    if (res != nri::Result::SUCCESS) {
        if (res == nri::Result::DEVICE_LOST)
            report_dred_once();
        return std::unexpected(map_nri_error(res, "QueueSubmit failed"));
    }
    lane.last_signaled = sig_val;
    return sig_val;
}

Result<void> QueueTimeline::wait(QueueKind kind, std::uint64_t value, std::uint32_t timeout_ms) const {
    if (value == 0)
        return {};
    const auto& lane = lanes_[static_cast<std::size_t>(resolve_kind(kind))];
    if (!lane.d3d12_fence)
        return {};
    if (value > lane.last_signaled)
        return std::unexpected(
            Error{ErrorCode::invalid_argument, 0, "Fence wait target exceeds last signaled timeline value"});
    ATELIER_TRY_BIND(done, completed_value(kind));
    if (*done >= value)
        return {};
    if (timeout_ms == 0)
        return std::unexpected(Error{ErrorCode::timeout, WAIT_TIMEOUT, "GPU timeline fence wait timed out (0 ms)"});
    ResetEvent(lane.event);
    const HRESULT hr = lane.d3d12_fence->SetEventOnCompletion(value, lane.event);
    if (FAILED(hr))
        return std::unexpected(Error{ErrorCode::execution_failed, hr, "SetEventOnCompletion failed"});
    const DWORD wait_res = WaitForSingleObjectEx(lane.event, timeout_ms, FALSE);
    ATELIER_TRY_BIND(after, completed_value(kind));
    if (*after >= value)
        return {};
    if (wait_res == WAIT_TIMEOUT)
        return std::unexpected(Error{ErrorCode::timeout, WAIT_TIMEOUT, "GPU timeline fence wait timed out"});
    return std::unexpected(
        Error{ErrorCode::execution_failed, static_cast<std::int32_t>(wait_res), "WaitForSingleObjectEx failed"});
}

Result<void> QueueTimeline::wait_all(const FencePoint& point, std::uint32_t timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (std::size_t i = 0; i < 3; ++i) {
        if (!lanes_[i].d3d12_fence || point.values[i] == 0)
            continue;
        std::uint32_t rem_ms = 0u;
        if (timeout_ms > 0u) {
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                rem_ms = static_cast<std::uint32_t>(std::min<std::int64_t>(
                    timeout_ms, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
                rem_ms = std::max(1u, rem_ms);
            }
        }
        ATELIER_TRY(wait(static_cast<QueueKind>(i), point.values[i], rem_ms));
    }
    return {};
}

Result<void> QueueTimeline::drain_all(std::uint32_t timeout_ms) {
    FencePoint targets{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!lanes_[i].queue)
            continue;
        ATELIER_TRY_ASSIGN(targets.values[i], submit(static_cast<QueueKind>(i), {}));
    }
    return wait_all(targets, timeout_ms);
}

ClockCalibration QueueTimeline::calibration(QueueKind kind) const noexcept {
    const auto& lane = lanes_[static_cast<std::size_t>(resolve_kind(kind))];
    ClockCalibration cal{};
    cal.gpu_frequency = lane.frequency;
    if (lane.d3d12_queue)
        static_cast<void>(lane.d3d12_queue->GetClockCalibration(&cal.gpu_timestamp, &cal.cpu_counter));
    return cal;
}

// ============================================================================
// DeferredReclaimer
// ============================================================================

void DeferredReclaimer::retire(Entry entry) {
    entries_.push_back(entry);
}

Result<void> DeferredReclaimer::reclaim(const QueueTimeline& timeline, BindlessHeap& bindless,
                                        const nri::CoreInterface& core, const nri::RayTracingInterface* rt) {
    if (entries_.empty())
        return {};
    ATELIER_TRY_BIND(done, timeline.completed_point());
    std::size_t write = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        auto& e = entries_[i];
        if (done->values[0] >= e.fence.values[0] && done->values[1] >= e.fence.values[1] &&
            done->values[2] >= e.fence.values[2]) {
            if (e.bindless_index != bindless_invalid_index)
                bindless.free_resource(e.bindless_index);
            if (e.descriptor)
                core.DestroyDescriptor(e.descriptor);
            if (e.as && rt)
                rt->DestroyAccelerationStructure(e.as);
            if (e.buffer)
                core.DestroyBuffer(e.buffer);
            if (e.texture)
                core.DestroyTexture(e.texture);
            if (e.pipeline)
                core.DestroyPipeline(e.pipeline);
            if (e.command_buffer)
                core.DestroyCommandBuffer(e.command_buffer);
            if (e.command_allocator)
                core.DestroyCommandAllocator(e.command_allocator);
            if (e.memory)
                core.FreeMemory(e.memory);
        } else {
            if (write != i)
                entries_[write] = e;
            ++write;
        }
    }
    entries_.resize(write);
    return {};
}

void DeferredReclaimer::force_release_all(BindlessHeap& bindless, const nri::CoreInterface& core,
                                          const nri::RayTracingInterface* rt) noexcept {
    for (auto& e : entries_) {
        if (e.bindless_index != bindless_invalid_index)
            bindless.free_resource(e.bindless_index);
        if (e.descriptor)
            core.DestroyDescriptor(e.descriptor);
        if (e.as && rt)
            rt->DestroyAccelerationStructure(e.as);
        if (e.buffer)
            core.DestroyBuffer(e.buffer);
        if (e.texture)
            core.DestroyTexture(e.texture);
        if (e.pipeline)
            core.DestroyPipeline(e.pipeline);
        if (e.command_buffer)
            core.DestroyCommandBuffer(e.command_buffer);
        if (e.command_allocator)
            core.DestroyCommandAllocator(e.command_allocator);
        if (e.memory)
            core.FreeMemory(e.memory);
    }
    entries_.clear();
}

// ============================================================================
// Preparer (8-worker SM 6.6 DXC + NRI Pipeline Creation + GBV Warmup)
// ============================================================================

struct PreparationJob {
    PipelineKind kind{};

    std::string debug_name;

    std::vector<std::pair<nri::StageBits, ShaderSource>> stages;

    GraphicsPipelineStateDesc graphics{};

    nri::Pipeline* created_pipeline{};

    std::atomic<bool> cancel{};

    enum class Phase { queued, running, done, cancelled } phase{Phase::queued};

    std::optional<Error> error;
};

class Preparer {
public:
    struct Config {
        nri::Device* nri_device{};

        ID3D12Device* d3d12_device{};

        const nri::CoreInterface* core{};

        const nri::WrapperD3D12Interface* wrapper_d3d12{};

        nri::PipelineLayout* layout{};

        bool gpu_validation{};

        std::filesystem::path cache;
    };

    explicit Preparer(Config config) : config_(std::move(config)) {}

    ~Preparer() { stop(); }

    Preparer(const Preparer&) = delete;

    Preparer& operator=(const Preparer&) = delete;

    void submit(std::shared_ptr<PreparationJob> job) noexcept {
        std::lock_guard lock(mutex_);
        drain_cancelled_locked();
        for (auto count = std::clamp(std::thread::hardware_concurrency(), 1u, 8u); threads_.size() < count;)
            threads_.emplace_back([this] { work(); });
        live_.push_back(job);
        queue_.push_back(std::move(job));
        wake_.notify_one();
    }

    void run_inline(PreparationJob& job) {
        {
            std::lock_guard lock(mutex_);
            drain_cancelled_locked();
        }
        run(job, owner_);
    }

    bool finished(PreparationJob& job) {
        std::lock_guard lock(mutex_);
        drain_cancelled_locked();
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
            discard_locked(job);
        drain_cancelled_locked();
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            for (auto& job : queue_)
                discard_locked(*job);
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
            discard_locked(*job);
        live_.clear();
        drain_cancelled_locked();
        owner_.release();
    }

private:
    struct Worker {
        std::optional<ShaderCompiler> compiler;

        ComPtr<ID3D12CommandAllocator> allocator;

        ComPtr<ID3D12GraphicsCommandList> list;

        bool list_open = false;

        void release() noexcept {
            if (list && list_open) {
                list->Close();
                list_open = false;
            }
            list.Reset();
            allocator.Reset();
            compiler.reset();
        }
    };

    void discard_locked(PreparationJob& job) noexcept {
        if (job.phase == PreparationJob::Phase::running)
            return;
        if (job.created_pipeline) {
            cancelled_pipelines_.push_back(std::exchange(job.created_pipeline, nullptr));
        }
        job.phase = PreparationJob::Phase::cancelled;
    }

    void drain_cancelled_locked() noexcept {
        for (nri::Pipeline* p : cancelled_pipelines_) {
            if (p)
                config_.core->DestroyPipeline(p);
        }
        cancelled_pipelines_.clear();
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
                discard_locked(*job);
        }
        worker.release();
    }

    void run(PreparationJob& job, Worker& worker) noexcept {
        if (!worker.compiler) {
            auto compiler = ShaderCompiler::open(config_.cache);
            if (!compiler) {
                job.error = compiler.error();
                return;
            }
            worker.compiler.emplace(std::move(*compiler));
        }

        std::vector<CompiledShader> blobs;
        std::vector<nri::ShaderDesc> shader_descs;
        std::vector<std::string> entries;
        blobs.reserve(job.stages.size());
        shader_descs.reserve(job.stages.size());
        entries.reserve(job.stages.size());

        for (const auto& [stage, source] : job.stages) {
            if (job.cancel)
                return;
            auto compiled = worker.compiler->compile(source);
            if (!compiled) {
                job.error = std::move(compiled.error());
                return;
            }
            auto utf8_entry = platform::utf16_to_utf8(source.entry);
            entries.push_back(utf8_entry ? std::string(utf8_entry->view().std_view()) : std::string("main"));
            nri::ShaderDesc sd{};
            sd.stage = stage;
            sd.bytecode = compiled->bytecode->GetBufferPointer();
            sd.size = compiled->bytecode->GetBufferSize();
            sd.entryPointName = entries.back().c_str();
            shader_descs.push_back(sd);
            blobs.push_back(std::move(*compiled));
        }
        if (job.cancel)
            return;

        nri::Result res = nri::Result::FAILURE;
        if (job.kind == PipelineKind::compute) {
            if (shader_descs.size() != 1) {
                job.error = Error{ErrorCode::invalid_argument, 0, "Compute pipeline requires exactly 1 CS stage"};
                return;
            }
            nri::ComputePipelineDesc cdesc{};
            cdesc.pipelineLayout = config_.layout;
            cdesc.shader = shader_descs[0];
            res = config_.core->CreateComputePipeline(*config_.nri_device, cdesc, job.created_pipeline);
        } else {
            std::array<nri::ColorAttachmentDesc, 8> colors{};
            for (std::uint32_t i = 0; i < job.graphics.color_attachment_count; ++i) {
                colors[i].format = job.graphics.color_formats[i];
                colors[i].colorWriteMask =
                    job.graphics.color_write ? nri::ColorWriteBits::RGBA : nri::ColorWriteBits::NONE;
                if (job.graphics.premultiplied_blend) {
                    colors[i].blendEnabled = true;
                    colors[i].colorBlend = {nri::BlendFactor::ONE, nri::BlendFactor::ONE_MINUS_SRC_ALPHA,
                                            nri::BlendOp::ADD};
                    colors[i].alphaBlend = {nri::BlendFactor::ONE, nri::BlendFactor::ONE_MINUS_SRC_ALPHA,
                                            nri::BlendOp::ADD};
                } else if (job.graphics.alpha_blend) {
                    colors[i].blendEnabled = true;
                    colors[i].colorBlend = {nri::BlendFactor::SRC_ALPHA, nri::BlendFactor::ONE_MINUS_SRC_ALPHA,
                                            nri::BlendOp::ADD};
                    colors[i].alphaBlend = {nri::BlendFactor::ONE, nri::BlendFactor::ONE_MINUS_SRC_ALPHA,
                                            nri::BlendOp::ADD};
                }
            }
            nri::GraphicsPipelineDesc gdesc{};
            gdesc.pipelineLayout = config_.layout;
            gdesc.shaders = shader_descs.data();
            gdesc.shaderNum = static_cast<std::uint32_t>(shader_descs.size());
            gdesc.inputAssembly.topology = job.graphics.topology;
            gdesc.rasterization.fillMode = job.graphics.fill_mode;
            gdesc.rasterization.cullMode = job.graphics.cull_mode;
            gdesc.rasterization.depthClamp = job.graphics.depth_clamp;
            gdesc.rasterization.depthBias.constant = job.graphics.depth_bias_constant;
            gdesc.rasterization.depthBias.slope = job.graphics.depth_bias_slope;
            gdesc.rasterization.depthBias.clamp = job.graphics.depth_bias_clamp;
            gdesc.outputMerger.colors = colors.data();
            gdesc.outputMerger.colorNum = job.graphics.color_attachment_count;
            gdesc.outputMerger.depthStencilFormat = job.graphics.depth_format;
            gdesc.outputMerger.depth.write = job.graphics.depth_write;
            gdesc.outputMerger.depth.compareOp =
                job.graphics.depth_test ? job.graphics.depth_compare : nri::CompareOp::NONE;
            res = config_.core->CreateGraphicsPipeline(*config_.nri_device, gdesc, job.created_pipeline);
        }

        if (res != nri::Result::SUCCESS || !job.created_pipeline) {
            job.error = map_nri_error(res, "NRI pipeline state creation failed for `" + job.debug_name + "'");
            if (job.error->code != ErrorCode::device_lost && job.error->code != ErrorCode::out_of_memory &&
                job.error->code != ErrorCode::unsupported) {
                job.error->code = ErrorCode::shader_failed;
            }
            return;
        }
        config_.core->SetDebugName(reinterpret_cast<nri::Object*>(job.created_pipeline), job.debug_name.c_str());
        if (config_.gpu_validation && !job.cancel)
            warm_gbv(*job.created_pipeline, worker);
    }

    void warm_gbv(nri::Pipeline& pipeline, Worker& worker) noexcept {
        if (!worker.list) {
            if (FAILED(config_.d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                    IID_PPV_ARGS(&worker.allocator))) ||
                FAILED(config_.d3d12_device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, worker.allocator.Get(), nullptr, IID_PPV_ARGS(&worker.list)))) {
                worker.release();
                return;
            }
            worker.list_open = true;
        }
        if (config_.wrapper_d3d12) {
            nri::CommandBufferD3D12Desc wrap_desc{};
            wrap_desc.d3d12CommandList = worker.list.Get();
            wrap_desc.d3d12CommandAllocator = worker.allocator.Get();
            nri::CommandBuffer* cmd{};
            if (config_.wrapper_d3d12->CreateCommandBufferD3D12(*config_.nri_device, wrap_desc, cmd) ==
                    nri::Result::SUCCESS &&
                cmd) {
                config_.core->CmdSetPipeline(*cmd, pipeline);
                config_.core->DestroyCommandBuffer(cmd);
            }
        }
        if (FAILED(worker.list->Close())) {
            worker.list_open = false;
            worker.release();
            return;
        }
        worker.list_open = false;
        if (FAILED(worker.allocator->Reset()) || FAILED(worker.list->Reset(worker.allocator.Get(), nullptr))) {
            worker.release();
            return;
        }
        worker.list_open = true;
    }

    const Config config_;

    std::mutex mutex_;

    std::condition_variable wake_;

    std::deque<std::shared_ptr<PreparationJob>> queue_;

    std::vector<std::shared_ptr<PreparationJob>> live_;

    std::vector<nri::Pipeline*> cancelled_pipelines_;

    std::vector<std::thread> threads_;

    bool stopped_{};

    Worker owner_;
};

// ============================================================================
// DeviceState & SurfaceState
// ============================================================================

std::atomic<std::uint32_t> g_live_d3d12_devices{0};

std::atomic<Validation> g_active_validation{Validation::disabled};

struct DeviceState {
    nri::Device* device{};

    ID3D12Device* d3d12_device{};

    ComPtr<IDXGIFactory6> dxgi_factory;

    nri::CoreInterface core{};

    nri::DescriptorHeapInterface descriptor_heap{};

    nri::MeshShaderInterface mesh_shader{};

    nri::RayTracingInterface ray_tracing{};

    nri::WrapperD3D12Interface wrapper_d3d12{};

    DeviceInfo info{};

    std::uint32_t wait_timeout_ms = 10000;

    const std::thread::id owner = std::this_thread::get_id();

    BindlessHeap bindless;

    QueueTimeline timeline;

    DeferredReclaimer reclaimer;

    std::shared_ptr<Preparer> preparer;

    std::uint32_t live_pipelines = 0;

    std::uint32_t live_surfaces = 0;

    bool allow_tearing = false;

    bool closed = false;

    [[nodiscard]] bool on_owner() const noexcept { return owner == std::this_thread::get_id(); }

    [[nodiscard]] Result<void> admit() const {
        if (!on_owner())
            return std::unexpected(Error{ErrorCode::wrong_thread, 0, "RHI call made from non-owner thread"});
        if (closed || !device)
            return std::unexpected(Error{ErrorCode::closed, 0, "RHI device is closed"});
        if (timeline.is_device_lost())
            return std::unexpected(Error{ErrorCode::device_lost, DXGI_ERROR_DEVICE_REMOVED, "GPU device lost"});
        return {};
    }

    Result<void> shutdown() {
        if (closed || !device) {
            closed = true;
            return {};
        }
        if (!on_owner())
            return std::unexpected(Error{ErrorCode::wrong_thread, 0, "RHI device close called on foreign thread"});
        if (live_pipelines > 0 || live_surfaces > 0)
            return std::unexpected(
                Error{ErrorCode::busy, 0, "Cannot close RHI device while pipelines or surfaces remain alive"});
        if (preparer)
            preparer->stop();
        const auto drained = timeline.drain_all(wait_timeout_ms);
        if (!drained && drained.error().code != ErrorCode::device_lost)
            return std::unexpected(drained.error());
        reclaimer.force_release_all(bindless, core, &ray_tracing);
        bindless.shutdown(core, descriptor_heap);
        timeline.shutdown(core);
        dxgi_factory.Reset();
        nriDestroyDevice(device);
        --g_live_d3d12_devices;
        device = nullptr;
        d3d12_device = nullptr;
        closed = true;
        return {};
    }

    ~DeviceState() {
        if (!device || closed)
            return;
        if (!on_owner())
            atelier::fatal("RHI lifetime violation: final device destruction on a foreign thread");
        if (auto res = shutdown(); !res)
            atelier::fatal("RHI lifetime violation: final device destruction failed to drain GPU safely");
    }
};

struct SurfaceState {
    static constexpr std::uint32_t buffer_count = 3;

    struct BackbufferSlot {
        ComPtr<ID3D12Resource> resource;

        nri::Texture* texture{};

        nri::Descriptor* rtv{};

        bool ever_used = false;
    };

    HWND window{};

    SurfaceOptions options{};

    ComPtr<IDXGISwapChain4> swapchain;

    HANDLE waitable_handle{};

    UINT swapchain_flags = 0;

    std::uint32_t width = 0;

    std::uint32_t height = 0;

    bool frame_latched = false;

    bool buffers_valid = false;

    bool closed = false;

    void release_buffers(DeviceState& dev) noexcept {
        buffers_valid = false;
        for (auto& slot : buffers_) {
            if (slot.rtv) {
                dev.core.DestroyDescriptor(slot.rtv);
                slot.rtv = nullptr;
            }
            if (slot.texture) {
                dev.core.DestroyTexture(slot.texture);
                slot.texture = nullptr;
            }
            slot.resource.Reset();
            slot.ever_used = false;
        }
    }

    Result<void> acquire_buffers(DeviceState& dev) {
        bool ok = false;
        struct Guard {
            SurfaceState* self;

            DeviceState* dev;

            bool* ok;

            ~Guard() {
                if (!*ok)
                    self->release_buffers(*dev);
            }
        } guard{this, &dev, &ok};

        for (std::uint32_t i = 0; i < buffer_count; ++i) {
            auto& slot = buffers_[i];
            HRESULT hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&slot.resource));
            if (FAILED(hr))
                return std::unexpected(Error{ErrorCode::creation_failed, hr, "IDXGISwapChain::GetBuffer failed"});
            nri::TextureD3D12Desc tdesc{};
            tdesc.d3d12Resource = slot.resource.Get();
            tdesc.format = options.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            nri::Result res = dev.wrapper_d3d12.CreateTextureD3D12(*dev.device, tdesc, slot.texture);
            if (res != nri::Result::SUCCESS)
                return std::unexpected(map_nri_error(res, "CreateTextureD3D12 for swapchain backbuffer failed"));

            nri::TextureViewDesc vdesc{};
            vdesc.texture = slot.texture;
            vdesc.type = nri::TextureView::COLOR_ATTACHMENT;
            vdesc.format = options.srgb ? nri::Format::RGBA8_SRGB : nri::Format::RGBA8_UNORM;
            vdesc.mipNum = 1;
            vdesc.layerNum = 1;
            res = dev.core.CreateTextureView(vdesc, slot.rtv);
            if (res != nri::Result::SUCCESS)
                return std::unexpected(map_nri_error(res, "CreateTextureView RTV for swapchain backbuffer failed"));
        }
        buffers_valid = true;
        ok = true;
        return {};
    }

    std::array<BackbufferSlot, buffer_count>& slots() noexcept { return buffers_; }

private:
    std::array<BackbufferSlot, buffer_count> buffers_{};
};

} // namespace detail

// ============================================================================
// Public Pipeline & PipelineJob
// ============================================================================

Pipeline::Pipeline(std::shared_ptr<detail::DeviceState> state, detail::PipelineState* pipeline) noexcept
    : state_(std::move(state)), pipeline_(pipeline) {
    if (state_ && pipeline_)
        ++state_->live_pipelines;
}

Pipeline::~Pipeline() {
    reset();
}

Pipeline::Pipeline(Pipeline&& other) noexcept
    : state_(std::move(other.state_)), pipeline_(std::exchange(other.pipeline_, nullptr)) {}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        pipeline_ = std::exchange(other.pipeline_, nullptr);
    }
    return *this;
}

void Pipeline::reset() noexcept {
    if (!state_ || !pipeline_)
        return;
    if (!state_->on_owner())
        atelier::fatal("RHI lifetime violation: pipeline destruction on a foreign thread");
    if (pipeline_->pipeline && state_->device && !state_->closed) {
        detail::DeferredReclaimer::Entry retired{};
        retired.fence = state_->timeline.current_point();
        retired.pipeline = pipeline_->pipeline;
        state_->reclaimer.retire(retired);
    }
    delete pipeline_;
    pipeline_ = nullptr;
    --state_->live_pipelines;
    state_.reset();
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
    if (preparer_ && job_)
        preparer_->cancel(*job_);
    job_.reset();
    preparer_.reset();
}

// ============================================================================
// Public WindowSurface
// ============================================================================

WindowSurface::WindowSurface(std::shared_ptr<detail::DeviceState> device,
                             std::unique_ptr<detail::SurfaceState> surface) noexcept
    : device_(std::move(device)), surface_(std::move(surface)) {
    if (device_ && surface_)
        ++device_->live_surfaces;
}

WindowSurface::WindowSurface() noexcept = default;

WindowSurface::~WindowSurface() {
    if (!surface_ || surface_->closed)
        return;
    if (!device_ || !device_->on_owner())
        atelier::fatal("RHI lifetime violation: WindowSurface destroyed on foreign thread");
    if (auto res = close(); !res && res.error().code != ErrorCode::device_lost)
        atelier::fatal("RHI lifetime violation: WindowSurface destruction could not drain GPU safely");
}

WindowSurface::WindowSurface(WindowSurface&& other) noexcept
    : device_(std::move(other.device_)), surface_(std::move(other.surface_)) {}

WindowSurface& WindowSurface::operator=(WindowSurface&& other) noexcept {
    if (this != &other) {
        if (surface_ && !surface_->closed) {
            if (auto res = close(); !res && res.error().code != ErrorCode::device_lost)
                atelier::fatal("RHI lifetime violation: WindowSurface assignment cleanup failed");
        }
        device_ = std::move(other.device_);
        surface_ = std::move(other.surface_);
    }
    return *this;
}

Result<WindowSurface> WindowSurface::open(Device& device, NativeWindow window, const SurfaceOptions& options) {
    if (!device.state_)
        return std::unexpected(Error{ErrorCode::closed, 0, "Device is not open"});
    ATELIER_TRY(device.state_->admit());
    if (!window || !IsWindow(window))
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "Invalid HWND for WindowSurface::open"});

    RECT client{};
    GetClientRect(window, &client);
    const std::uint32_t init_w = std::max<std::uint32_t>(
        1u, client.right > client.left ? static_cast<std::uint32_t>(client.right - client.left) : options.width);
    const std::uint32_t init_h = std::max<std::uint32_t>(
        1u, client.bottom > client.top ? static_cast<std::uint32_t>(client.bottom - client.top) : options.height);

    auto state = std::make_unique<detail::SurfaceState>();
    state->window = window;
    state->options = options;
    state->width = init_w;
    state->height = init_h;
    state->swapchain_flags = 0;
    if (options.frame_latency_waitable)
        state->swapchain_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (device.state_->allow_tearing)
        state->swapchain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = init_w;
    sc_desc.Height = init_h;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc = {1, 0};
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = detail::SurfaceState::buffer_count;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sc_desc.Flags = state->swapchain_flags;

    auto* d3d12_queue = static_cast<ID3D12CommandQueue*>(
        device.state_->core.GetQueueNativeObject(device.state_->timeline.queue(QueueKind::graphics)));
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr =
        device.state_->dxgi_factory->CreateSwapChainForHwnd(d3d12_queue, window, &sc_desc, nullptr, nullptr, &sc1);
    if (FAILED(hr))
        return std::unexpected(Error{ErrorCode::creation_failed, hr, "CreateSwapChainForHwnd failed"});
    static_cast<void>(device.state_->dxgi_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
    hr = sc1.As(&state->swapchain);
    if (FAILED(hr))
        return std::unexpected(Error{ErrorCode::creation_failed, hr, "IDXGISwapChain4 query failed"});
    static_cast<void>(state->swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));

    bool open_ok = false;
    struct WaitableGuard {
        detail::SurfaceState* s;

        bool* ok;

        ~WaitableGuard() {
            if (!*ok && s->waitable_handle) {
                CloseHandle(s->waitable_handle);
                s->waitable_handle = nullptr;
            }
        }
    } waitable_guard{state.get(), &open_ok};

    if (options.frame_latency_waitable) {
        hr = state->swapchain->SetMaximumFrameLatency(1);
        if (FAILED(hr))
            return std::unexpected(Error{ErrorCode::creation_failed, hr, "SetMaximumFrameLatency(1) failed"});
        state->waitable_handle = state->swapchain->GetFrameLatencyWaitableObject();
        if (!state->waitable_handle)
            return std::unexpected(
                Error{ErrorCode::creation_failed, E_FAIL, "GetFrameLatencyWaitableObject returned null"});
    }

    ATELIER_TRY(state->acquire_buffers(*device.state_));
    open_ok = true;
    return WindowSurface(device.state_, std::move(state));
}

Result<TargetInfo> WindowSurface::target() const {
    if (!device_ || !surface_ || surface_->closed)
        return std::unexpected(Error{ErrorCode::closed, 0, "WindowSurface is closed"});
    ATELIER_TRY(device_->admit());
    RECT client{};
    const bool valid_win = IsWindow(surface_->window) && GetClientRect(surface_->window, &client);
    const std::uint32_t cw =
        valid_win && client.right > client.left ? static_cast<std::uint32_t>(client.right - client.left) : 0u;
    const std::uint32_t ch =
        valid_win && client.bottom > client.top ? static_cast<std::uint32_t>(client.bottom - client.top) : 0u;
    const bool drawable =
        valid_win && IsWindowVisible(surface_->window) && !IsIconic(surface_->window) && cw > 0 && ch > 0;
    return TargetInfo{surface_->width, surface_->height, true, drawable};
}

Result<TargetInfo> WindowSurface::sync_target() {
    if (!device_ || !surface_ || surface_->closed)
        return std::unexpected(Error{ErrorCode::closed, 0, "WindowSurface is closed"});
    ATELIER_TRY(device_->admit());
    RECT client{};
    if (!IsWindow(surface_->window) || !GetClientRect(surface_->window, &client))
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "Surface HWND is no longer valid"});
    const std::uint32_t cw = client.right > client.left ? static_cast<std::uint32_t>(client.right - client.left) : 0u;
    const std::uint32_t ch = client.bottom > client.top ? static_cast<std::uint32_t>(client.bottom - client.top) : 0u;
    const bool drawable = IsWindowVisible(surface_->window) && !IsIconic(surface_->window) && cw > 0 && ch > 0;
    if (cw > 0 && ch > 0 && (!surface_->buffers_valid || cw != surface_->width || ch != surface_->height)) {
        ATELIER_TRY(device_->timeline.drain_all(device_->wait_timeout_ms));
        ATELIER_TRY(
            device_->reclaimer.reclaim(device_->timeline, device_->bindless, device_->core, &device_->ray_tracing));
        surface_->release_buffers(*device_);
        const HRESULT hr = surface_->swapchain->ResizeBuffers(detail::SurfaceState::buffer_count, cw, ch,
                                                              DXGI_FORMAT_R8G8B8A8_UNORM, surface_->swapchain_flags);
        if (FAILED(hr))
            return std::unexpected(Error{hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                                             ? ErrorCode::device_lost
                                             : ErrorCode::execution_failed,
                                         hr, "IDXGISwapChain4::ResizeBuffers failed"});
        ATELIER_TRY(surface_->acquire_buffers(*device_));
        surface_->width = cw;
        surface_->height = ch;
    }
    return TargetInfo{surface_->width, surface_->height, true, drawable};
}

Result<bool> WindowSurface::wait_frame(std::uint32_t timeout_ms) {
    if (!device_ || !surface_ || surface_->closed)
        return std::unexpected(Error{ErrorCode::closed, 0, "WindowSurface is closed"});
    ATELIER_TRY(device_->admit());
    if (!surface_->options.frame_latency_waitable)
        return true;
    if (surface_->frame_latched)
        return true;
    const HANDLE handles[1] = {surface_->waitable_handle};
    const DWORD res = timeout_ms == 0
                          ? WaitForSingleObjectEx(surface_->waitable_handle, 0, FALSE)
                          : MsgWaitForMultipleObjectsEx(1, handles, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (res == WAIT_OBJECT_0) {
        surface_->frame_latched = true;
        return true;
    }
    if (res == WAIT_OBJECT_0 + 1 || res == WAIT_TIMEOUT)
        return false;
    return std::unexpected(
        Error{ErrorCode::execution_failed, static_cast<std::int32_t>(GetLastError()), "wait_frame failed"});
}

Result<void> WindowSurface::close() {
    if (!surface_)
        return {};
    if (surface_->closed) {
        surface_.reset();
        return {};
    }
    if (!device_ || !device_->on_owner())
        return std::unexpected(Error{ErrorCode::wrong_thread, 0, "WindowSurface::close called on foreign thread"});
    if (!device_->closed && device_->device) {
        const auto drained = device_->timeline.drain_all(device_->wait_timeout_ms);
        static_cast<void>(
            device_->reclaimer.reclaim(device_->timeline, device_->bindless, device_->core, &device_->ray_tracing));
        surface_->release_buffers(*device_);
        if (surface_->waitable_handle) {
            CloseHandle(surface_->waitable_handle);
            surface_->waitable_handle = nullptr;
        }
        surface_->swapchain.Reset();
        surface_->closed = true;
        --device_->live_surfaces;
        device_.reset();
        surface_.reset();
        if (!drained)
            return std::unexpected(drained.error());
        return {};
    }
    if (surface_->waitable_handle) {
        CloseHandle(surface_->waitable_handle);
        surface_->waitable_handle = nullptr;
    }
    surface_->swapchain.Reset();
    surface_->closed = true;
    if (device_) {
        --device_->live_surfaces;
        device_.reset();
    }
    surface_.reset();
    return {};
}

// ============================================================================
// Public Device
// ============================================================================

Device::Device(std::shared_ptr<detail::DeviceState> state) noexcept : state_(std::move(state)) {}

Device::~Device() = default;

Device::Device(Device&& other) noexcept = default;

Device& Device::operator=(Device&& other) noexcept = default;

Result<Device> Device::open(const DeviceOptions& options) {
    if (options.wait_timeout_ms == 0 || options.wait_timeout_ms == INFINITE)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "wait_timeout_ms must be finite and positive"});

    ATELIER_TRY_BIND(effective_val_res, resolve_validation_env(options.validation));
    const Validation effective_val = *effective_val_res;

    std::uint32_t adapter_count = 0;
    nri::Result res = nriEnumerateAdapters(nullptr, adapter_count, nri::GraphicsAPI::D3D12);
    if (res != nri::Result::SUCCESS || adapter_count == 0)
        return std::unexpected(
            Error{ErrorCode::unsupported, static_cast<std::int32_t>(res), "No D3D12 hardware adapters enumerated"});
    std::vector<nri::AdapterDesc> adapters(adapter_count);
    res = nriEnumerateAdapters(adapters.data(), adapter_count, nri::GraphicsAPI::D3D12);
    if (res != nri::Result::SUCCESS)
        return std::unexpected(map_nri_error(res, "nriEnumerateAdapters failed"));

    auto state = std::make_shared<detail::DeviceState>();
    state->wait_timeout_ms = options.wait_timeout_ms;

    // Probe adapters via IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE) so primary display-attached
    // hardware adapters win over headless IddCx mirror LUIDs that NRI's unstable qsort may reorder ahead of Adapter[0].
    ComPtr<IDXGIFactory6> probe_factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&probe_factory))))
        return std::unexpected(Error{ErrorCode::creation_failed, 0, "DXGI adapter probe factory creation failed"});

    const nri::AdapterDesc* chosen = nullptr;
    const nri::AdapterDesc* headless_fallback = nullptr;
    for (UINT idx = 0;; ++idx) {
        ComPtr<IDXGIAdapter1> dxgi_adapter;
        const HRESULT enum_hr = probe_factory->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                          IID_PPV_ARGS(&dxgi_adapter));
        if (enum_hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(enum_hr))
            continue;
        DXGI_ADAPTER_DESC1 desc1{};
        if (FAILED(dxgi_adapter->GetDesc1(&desc1)) || (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;
        const nri::AdapterDesc* matched = nullptr;
        for (const auto& candidate : adapters) {
            if (candidate.architecture == nri::Architecture::SOFTWARE)
                continue;
            LUID cand_luid{};
            std::memcpy(&cand_luid, &candidate.uid.low, sizeof(LUID));
            if (cand_luid.LowPart == desc1.AdapterLuid.LowPart && cand_luid.HighPart == desc1.AdapterLuid.HighPart) {
                matched = &candidate;
                break;
            }
        }
        if (!matched)
            continue;
        ComPtr<ID3D12Device> probe_dev;
        if (FAILED(D3D12CreateDevice(dxgi_adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&probe_dev))) ||
            !check_hardware_floor(*probe_dev.Get()))
            continue;
        bool has_desktop_output = false;
        for (UINT out_idx = 0;; ++out_idx) {
            ComPtr<IDXGIOutput> out;
            if (dxgi_adapter->EnumOutputs(out_idx, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC out_desc{};
            if (out && SUCCEEDED(out->GetDesc(&out_desc)) && out_desc.AttachedToDesktop) {
                has_desktop_output = true;
                break;
            }
        }
        if (has_desktop_output) {
            chosen = matched;
            break;
        }
        if (!headless_fallback)
            headless_fallback = matched;
    }
    if (!chosen)
        chosen = headless_fallback;
    if (!chosen)
        return std::unexpected(Error{
            ErrorCode::unsupported, 0,
            "No hardware GPU supports D3D12 Ultimate (Mesh Shader, DXR 1.1, SV_Barycentrics, Enhanced Barriers, SM 6.6)"});

    std::vector<nri::QueueFamilyDesc> queue_families;
    queue_families.push_back({nullptr, 1, nri::QueueType::GRAPHICS});
    const bool want_compute = chosen->queueNum[static_cast<std::size_t>(nri::QueueType::COMPUTE)] > 0;
    const bool want_copy = chosen->queueNum[static_cast<std::size_t>(nri::QueueType::COPY)] > 0;
    if (want_compute)
        queue_families.push_back({nullptr, 1, nri::QueueType::COMPUTE});
    if (want_copy)
        queue_families.push_back({nullptr, 1, nri::QueueType::COPY});

    const bool has_live_device = detail::g_live_d3d12_devices.load() > 0u;
    if (has_live_device && effective_val != detail::g_active_validation.load()) {
        return std::unexpected(
            Error{ErrorCode::busy, 0, "Cannot change D3D12 validation mode while another Device is open"});
    }

    nri::DeviceCreationDesc create_desc{};
    create_desc.graphicsAPI = nri::GraphicsAPI::D3D12;
    create_desc.adapterDesc = chosen;
    create_desc.deviceLostInfoLevel = nri::DeviceLostInfoLevel::VERBOSE;
    create_desc.callbackInterface = {nri_message_callback, nri_noop_abort, nullptr};
    create_desc.allocationCallbacks = {nri_allocate, nri_reallocate, nri_free, nullptr, false};
    create_desc.queueFamilies = queue_families.data();
    create_desc.queueFamilyNum = static_cast<std::uint32_t>(queue_families.size());
    create_desc.enableGraphicsAPIValidation = !has_live_device && (effective_val != Validation::disabled);
    create_desc.enableD3D12GpuBasedValidation = !has_live_device && (effective_val == Validation::gpu);
    create_desc.disableD3D12EnhancedBarriers = false;
    create_desc.disableD3D12BreakOnError = true;

    res = nriCreateDevice(create_desc, state->device);
    if (res != nri::Result::SUCCESS || !state->device)
        return std::unexpected(map_nri_error(res, "nriCreateDevice failed"));

    if (!has_live_device)
        detail::g_active_validation.store(effective_val);
    ++detail::g_live_d3d12_devices;

    bool open_ok = false;
    struct DeviceOpenGuard {
        detail::DeviceState* s;

        bool* ok;

        ~DeviceOpenGuard() {
            if (*ok)
                return;
            if (s->device) {
                s->bindless.shutdown(s->core, s->descriptor_heap);
                s->timeline.shutdown(s->core);
                s->dxgi_factory.Reset();
                nriDestroyDevice(s->device);
                --detail::g_live_d3d12_devices;
                s->device = nullptr;
                s->d3d12_device = nullptr;
                s->closed = true;
            }
        }
    } open_guard{state.get(), &open_ok};

    if (nriGetInterface(*state->device, NRI_INTERFACE(nri::CoreInterface), &state->core) != nri::Result::SUCCESS ||
        nriGetInterface(*state->device, NRI_INTERFACE(nri::DescriptorHeapInterface), &state->descriptor_heap) !=
            nri::Result::SUCCESS ||
        nriGetInterface(*state->device, NRI_INTERFACE(nri::MeshShaderInterface), &state->mesh_shader) !=
            nri::Result::SUCCESS ||
        nriGetInterface(*state->device, NRI_INTERFACE(nri::RayTracingInterface), &state->ray_tracing) !=
            nri::Result::SUCCESS ||
        nriGetInterface(*state->device, NRI_INTERFACE(nri::WrapperD3D12Interface), &state->wrapper_d3d12) !=
            nri::Result::SUCCESS) {
        return std::unexpected(
            Error{ErrorCode::unsupported, 0, "Required NRI D3D12 Ultimate extension interface unavailable"});
    }

    state->d3d12_device = static_cast<ID3D12Device*>(state->core.GetDeviceNativeObject(state->device));
    if (!state->d3d12_device || !check_hardware_floor(*state->d3d12_device)) {
        return std::unexpected(
            Error{ErrorCode::unsupported, 0, "Created D3D12 device failed Ultimate capability check"});
    }
    if (effective_val != Validation::disabled) {
        ComPtr<ID3D12InfoQueue> info_queue;
        if (SUCCEEDED(state->d3d12_device->QueryInterface(IID_PPV_ARGS(&info_queue))) && info_queue) {
            // Suppress Agility SDK 1.620+ CheckFeatureSupport1 informational deprecation notice (ID 989).
            D3D12_MESSAGE_ID deny_ids[] = {static_cast<D3D12_MESSAGE_ID>(989)};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = 1u;
            filter.DenyList.pIDList = deny_ids;
            static_cast<void>(info_queue->AddStorageFilterEntries(&filter));
            info_queue->ClearStoredMessages();
        }
    }

    const UINT dxgi_flags = effective_val != Validation::disabled ? DXGI_CREATE_FACTORY_DEBUG : 0u;
    const HRESULT fhr = CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&state->dxgi_factory));
    if (FAILED(fhr))
        return std::unexpected(Error{ErrorCode::creation_failed, fhr, "CreateDXGIFactory2 failed"});
    BOOL tearing = FALSE;
    if (SUCCEEDED(
            state->dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
        state->allow_tearing = tearing == TRUE;

    ATELIER_TRY(state->timeline.initialize(*state->device, *state->d3d12_device, state->core, state->wrapper_d3d12,
                                           want_compute, want_copy));
    ATELIER_TRY(state->bindless.initialize(*state->device, state->core, state->descriptor_heap));

    state->info.adapter_name = chosen->name;
    state->info.dedicated_video_memory = chosen->videoMemorySize;
    state->info.frames_in_flight = 3;
    state->info.debug_layer = effective_val != Validation::disabled;
    state->info.gpu_validation = effective_val == Validation::gpu;
    state->info.async_compute = want_compute;
    state->info.copy_queue = want_copy;

    state->preparer = std::make_shared<detail::Preparer>(detail::Preparer::Config{
        .nri_device = state->device,
        .d3d12_device = state->d3d12_device,
        .core = &state->core,
        .wrapper_d3d12 = &state->wrapper_d3d12,
        .layout = state->bindless.pipeline_layout(),
        .gpu_validation = state->info.gpu_validation,
        .cache = options.shader_cache,
    });

    open_ok = true;
    return Device(std::move(state));
}

const DeviceInfo& Device::info() const noexcept {
    static const DeviceInfo empty{};
    return state_ ? state_->info : empty;
}

std::uint32_t Device::wait_timeout_ms() const noexcept {
    return state_ ? state_->wait_timeout_ms : 0u;
}

Result<void> Device::wait_idle() {
    if (!state_)
        return std::unexpected(Error{ErrorCode::closed, 0, "Device is closed"});
    ATELIER_TRY(state_->admit());
    ATELIER_TRY(state_->timeline.drain_all(state_->wait_timeout_ms));
    return state_->reclaimer.reclaim(state_->timeline, state_->bindless, state_->core, &state_->ray_tracing);
}

Result<void> Device::close() {
    if (!state_)
        return {};
    ATELIER_TRY(state_->shutdown());
    state_.reset();
    return {};
}

// ============================================================================
// Internal BackendAccess
// ============================================================================

namespace {
Result<std::shared_ptr<detail::PreparationJob>> make_job(const PipelineCompileDesc& desc) {
    if (!valid_shader_model(desc.shader_model) || desc.stages.empty())
        return std::unexpected(
            Error{ErrorCode::invalid_argument, 0, "Invalid PipelineCompileDesc shader model or stages"});
    const auto wmodel = platform::utf8_to_utf16(desc.shader_model);
    if (!wmodel)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "Invalid UTF-8 shader_model"});
    auto job = std::make_shared<detail::PreparationJob>();
    job->kind = desc.kind;
    job->debug_name = desc.debug_name ? desc.debug_name : "pipeline";
    job->graphics = desc.graphics;
    job->stages.reserve(desc.stages.size());
    for (const auto& s : desc.stages) {
        const auto* prefix = stage_prefix(s.stage);
        if (!prefix || !s.source || !s.entry_point)
            return std::unexpected(
                Error{ErrorCode::invalid_argument, 0, "Invalid ShaderStageSource in PipelineCompileDesc"});
        const auto wname = platform::utf8_to_utf16(
            s.name ? StringView{s.name} : StringView{std::string_view{job->debug_name}});
        const auto wentry = platform::utf8_to_utf16(StringView{s.entry_point});
        if (!wname || !wentry)
            return std::unexpected(Error{ErrorCode::invalid_argument, 0, "Invalid UTF-8 shader stage name or entry"});
        job->stages.push_back(
            {s.stage, detail::ShaderSource{*wname, std::string(s.source), std::wstring(prefix) + *wmodel, *wentry}});
    }
    return job;
}
} // namespace

detail::DeviceState& BackendAccess::state(Device& device) noexcept {
    return *device.state_;
}

const detail::DeviceState& BackendAccess::state(const Device& device) noexcept {
    return *device.state_;
}

nri::Device& BackendAccess::nri_device(Device& device) noexcept {
    return *device.state_->device;
}

ID3D12Device& BackendAccess::d3d12_device(Device& device) noexcept {
    return *device.state_->d3d12_device;
}

const nri::CoreInterface& BackendAccess::core(const Device& device) noexcept {
    return device.state_->core;
}

const nri::DescriptorHeapInterface& BackendAccess::descriptor_heap(const Device& device) noexcept {
    return device.state_->descriptor_heap;
}

const nri::MeshShaderInterface& BackendAccess::mesh_shader(const Device& device) noexcept {
    return device.state_->mesh_shader;
}

const nri::RayTracingInterface& BackendAccess::ray_tracing(const Device& device) noexcept {
    return device.state_->ray_tracing;
}

const nri::WrapperD3D12Interface& BackendAccess::wrapper_d3d12(const Device& device) noexcept {
    return device.state_->wrapper_d3d12;
}

detail::BindlessHeap& BackendAccess::bindless(Device& device) noexcept {
    return device.state_->bindless;
}

detail::QueueTimeline& BackendAccess::timeline(Device& device) noexcept {
    return device.state_->timeline;
}

detail::DeferredReclaimer& BackendAccess::reclaimer(Device& device) noexcept {
    return device.state_->reclaimer;
}

Result<Pipeline> BackendAccess::compile_pipeline(Device& device, const PipelineCompileDesc& desc) {
    if (!device.state_)
        return std::unexpected(Error{ErrorCode::closed, 0, "Device is closed"});
    ATELIER_TRY(device.state_->admit());
    ATELIER_TRY_BIND(job_res, make_job(desc));
    auto& job = *job_res;
    device.state_->preparer->run_inline(*job);
    if (job->error)
        return std::unexpected(std::move(*job->error));
    auto* pstate = new detail::PipelineState{std::exchange(job->created_pipeline, nullptr), job->kind};
    return Pipeline(device.state_, pstate);
}

Result<PipelineJob> BackendAccess::submit_pipeline(Device& device, const PipelineCompileDesc& desc) {
    if (!device.state_)
        return std::unexpected(Error{ErrorCode::closed, 0, "Device is closed"});
    ATELIER_TRY(device.state_->admit());
    ATELIER_TRY_BIND(job_res, make_job(desc));
    PipelineJob handle;
    handle.preparer_ = device.state_->preparer;
    handle.job_ = *job_res;
    device.state_->preparer->submit(std::move(*job_res));
    return handle;
}

std::optional<Result<Pipeline>> BackendAccess::take_pipeline(Device& device, PipelineJob& job) {
    if (!job.job_ || !job.preparer_)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "PipelineJob is empty"});
    if (!device.state_ || device.state_->closed) {
        job.cancel();
        return std::unexpected(Error{ErrorCode::closed, 0, "Device is closed"});
    }
    if (job.preparer_ != device.state_->preparer)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "PipelineJob belongs to another Device"});
    if (auto adm = device.state_->admit(); !adm) {
        if (adm.error().code == ErrorCode::device_lost || adm.error().code == ErrorCode::closed)
            job.cancel();
        return std::unexpected(adm.error());
    }
    if (!job.preparer_->finished(*job.job_))
        return std::nullopt;
    auto done = std::move(job.job_);
    job.preparer_.reset();
    if (done->phase == detail::PreparationJob::Phase::cancelled)
        return std::unexpected(Error{ErrorCode::closed, 0, "PipelineJob was cancelled"});
    if (done->error)
        return std::unexpected(std::move(*done->error));
    auto* pstate = new detail::PipelineState{std::exchange(done->created_pipeline, nullptr), done->kind};
    return Pipeline(device.state_, pstate);
}

nri::Pipeline* BackendAccess::nri_pipeline(const Pipeline& pipeline) noexcept {
    return pipeline.pipeline_ ? pipeline.pipeline_->pipeline : nullptr;
}

PipelineKind BackendAccess::pipeline_kind(const Pipeline& pipeline) noexcept {
    return pipeline.pipeline_ ? pipeline.pipeline_->kind : PipelineKind::compute;
}

Result<AcquiredBackbuffer> BackendAccess::acquire_backbuffer(WindowSurface& surface) {
    if (!surface.device_ || !surface.surface_ || surface.surface_->closed)
        return std::unexpected(Error{ErrorCode::closed, 0, "WindowSurface is closed"});
    ATELIER_TRY(surface.device_->admit());
    if (!surface.surface_->buffers_valid)
        return std::unexpected(
            Error{ErrorCode::execution_failed, 0, "WindowSurface backbuffers unavailable after failed resize"});
    if (surface.surface_->options.frame_latency_waitable && !surface.surface_->frame_latched)
        return std::unexpected(Error{ErrorCode::invalid_argument, 0, "Waitable WindowSurface lacks wait_frame latch"});
    const std::uint32_t idx = surface.surface_->swapchain->GetCurrentBackBufferIndex();
    auto& slot = surface.surface_->slots()[idx];
    if (!slot.texture || !slot.rtv)
        return std::unexpected(Error{ErrorCode::execution_failed, 0, "WindowSurface backbuffer slot is invalid"});
    const bool was_used = std::exchange(slot.ever_used, true);
    return AcquiredBackbuffer{
        .texture = slot.texture,
        .rtv = slot.rtv,
        .format = surface.surface_->options.srgb ? nri::Format::RGBA8_SRGB : nri::Format::RGBA8_UNORM,
        .width = surface.surface_->width,
        .height = surface.surface_->height,
        .buffer_index = idx,
        .ever_used = was_used,
    };
}

Result<void> BackendAccess::present(WindowSurface& surface, bool vsync, FrameInfo* details) {
    if (!surface.device_ || !surface.surface_ || surface.surface_->closed)
        return std::unexpected(Error{ErrorCode::closed, 0, "WindowSurface is closed"});
    ATELIER_TRY(surface.device_->admit());
    auto& s = *surface.surface_;
    const std::uint32_t rendered_w = s.width;
    const std::uint32_t rendered_h = s.height;
    const bool rendered_drawable =
        IsWindowVisible(s.window) && !IsIconic(s.window) && rendered_w > 0u && rendered_h > 0u;
    const auto t0 = std::chrono::steady_clock::now();
    const UINT sync_interval = vsync ? 1u : 0u;
    const UINT present_flags =
        (!vsync && (s.swapchain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT hr = s.swapchain->Present(sync_interval, present_flags);
    s.frame_latched = false;
    const auto t1 = std::chrono::steady_clock::now();
    if (FAILED(hr))
        return std::unexpected(Error{hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                                         ? ErrorCode::device_lost
                                         : ErrorCode::execution_failed,
                                     hr, "IDXGISwapChain4::Present failed"});
    const auto t2 = std::chrono::steady_clock::now();
    RECT client{};
    const bool valid_win = IsWindow(s.window) && GetClientRect(s.window, &client);
    const std::uint32_t nw =
        valid_win && client.right > client.left ? static_cast<std::uint32_t>(client.right - client.left) : 0u;
    const std::uint32_t nh =
        valid_win && client.bottom > client.top ? static_cast<std::uint32_t>(client.bottom - client.top) : 0u;
    if (nw > 0u && nh > 0u && (!s.buffers_valid || nw != s.width || nh != s.height)) {
        ATELIER_TRY(surface.device_->timeline.drain_all(surface.device_->wait_timeout_ms));
        ATELIER_TRY(surface.device_->reclaimer.reclaim(surface.device_->timeline, surface.device_->bindless,
                                                       surface.device_->core, &surface.device_->ray_tracing));
        s.release_buffers(*surface.device_);
        const HRESULT rhr = s.swapchain->ResizeBuffers(detail::SurfaceState::buffer_count, nw, nh,
                                                       DXGI_FORMAT_R8G8B8A8_UNORM, s.swapchain_flags);
        if (FAILED(rhr))
            return std::unexpected(Error{rhr == DXGI_ERROR_DEVICE_REMOVED || rhr == DXGI_ERROR_DEVICE_RESET
                                             ? ErrorCode::device_lost
                                             : ErrorCode::execution_failed,
                                         rhr, "IDXGISwapChain4::ResizeBuffers after Present failed"});
        ATELIER_TRY(s.acquire_buffers(*surface.device_));
        s.width = nw;
        s.height = nh;
    } else {
        ATELIER_TRY(surface.device_->reclaimer.reclaim(surface.device_->timeline, surface.device_->bindless,
                                                       surface.device_->core, &surface.device_->ray_tracing));
    }
    const auto t3 = std::chrono::steady_clock::now();
    if (details) {
        details->rendered = {rendered_w, rendered_h, true, rendered_drawable};
        details->next = {s.width, s.height, true,
                         valid_win && IsWindowVisible(s.window) && !IsIconic(s.window) && nw > 0u && nh > 0u};
        details->occluded = (hr == DXGI_STATUS_OCCLUDED);
        details->cpu.present_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        details->cpu.recycle_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    }
    return {};
}

Result<bool> BackendAccess::test_occluded(WindowSurface& surface) {
    if (!surface.device_ || !surface.surface_ || surface.surface_->closed)
        return std::unexpected(Error{ErrorCode::closed, 0, "WindowSurface is closed"});
    ATELIER_TRY(surface.device_->admit());
    const HRESULT hr = surface.surface_->swapchain->Present(0u, DXGI_PRESENT_TEST);
    if (FAILED(hr)) {
        return std::unexpected(Error{hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                                         ? ErrorCode::device_lost
                                         : ErrorCode::execution_failed,
                                     hr, "IDXGISwapChain4::Present(DXGI_PRESENT_TEST) failed"});
    }
    return hr == DXGI_STATUS_OCCLUDED;
}

} // namespace atelier::rhi

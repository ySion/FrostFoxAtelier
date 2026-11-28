#pragma once

#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "rhi/rhi.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <NRI.h>
#include <Extensions/NRIDescriptorHeap.h>
#include <Extensions/NRIDeviceCreation.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIMeshShader.h>
#include <Extensions/NRIRayTracing.h>
#include <Extensions/NRIWrapperD3D12.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace atelier::rhi {

inline constexpr std::uint32_t bindless_invalid_index = 0xFFFFFFFFu;

inline constexpr std::uint32_t bindless_resource_capacity = 262144u;

inline constexpr std::uint32_t bindless_sampler_capacity = 256u;

inline constexpr std::uint32_t root_constant_dword_count = 16u;

inline constexpr std::uint32_t root_constant_byte_size = root_constant_dword_count * sizeof(std::uint32_t);

enum class StaticSampler : std::uint32_t {
    linear_clamp = 0,
    linear_wrap = 1,
    linear_mirror = 2,
    point_clamp = 3,
    point_wrap = 4,
    aniso_wrap = 5,
    aniso_clamp = 6,
    aniso_mirror = 7,
    shadow_cmp_greater_equal = 8,
    count = 9
};

struct FencePoint {
    std::array<std::uint64_t, 3> values{};
};

struct ClockCalibration {
    std::uint64_t gpu_frequency{};

    std::uint64_t gpu_timestamp{};

    std::uint64_t cpu_counter{};
};

struct ShaderStageSource {
    nri::StageBits stage = nri::StageBits::COMPUTE_SHADER;

    const char* source = nullptr;

    const char* entry_point = "main";

    const char* name = "shader";
};

struct GraphicsPipelineStateDesc {
    std::array<nri::Format, 8> color_formats{};

    std::uint32_t color_attachment_count = 0;

    nri::Format depth_format = nri::Format::UNKNOWN;

    bool depth_test = false;

    bool depth_write = false;

    nri::CompareOp depth_compare = nri::CompareOp::GREATER;

    nri::CullMode cull_mode = nri::CullMode::NONE;

    nri::FillMode fill_mode = nri::FillMode::SOLID;

    nri::Topology topology = nri::Topology::TRIANGLE_LIST;

    bool alpha_blend = false;

    bool premultiplied_blend = false;

    bool color_write = true;

    bool depth_clamp = false;

    float depth_bias_constant = 0.f;

    float depth_bias_slope = 0.f;

    float depth_bias_clamp = 0.f;
};

struct PipelineCompileDesc {
    PipelineKind kind = PipelineKind::compute;

    const char* debug_name = "pipeline";

    const char* shader_model = "6_6";

    Span<const ShaderStageSource> stages{};

    GraphicsPipelineStateDesc graphics{};
};

struct AcquiredBackbuffer {
    nri::Texture* texture{};

    nri::Descriptor* rtv{};

    nri::Format format = nri::Format::RGBA8_SRGB;

    std::uint32_t width{};

    std::uint32_t height{};

    std::uint32_t buffer_index{};

    bool ever_used{};
};

namespace detail {

struct PipelineState {
    nri::Pipeline* pipeline{};

    PipelineKind kind = PipelineKind::compute;
};

class BindlessHeap {
public:
    BindlessHeap() = default;

    ~BindlessHeap() = default;

    BindlessHeap(const BindlessHeap&) = delete;

    BindlessHeap& operator=(const BindlessHeap&) = delete;

    [[nodiscard]] Result<void> initialize(nri::Device& device, const nri::CoreInterface& core,
                                          const nri::DescriptorHeapInterface& heap_iface);

    void shutdown(const nri::CoreInterface& core, const nri::DescriptorHeapInterface& heap_iface) noexcept;

    [[nodiscard]] Result<std::uint32_t> allocate_resource(nri::Descriptor* descriptor);

    void update_resource(std::uint32_t index, nri::Descriptor* descriptor) noexcept;

    void free_resource(std::uint32_t index) noexcept;

    [[nodiscard]] nri::DescriptorHeap* descriptor_heap() const noexcept { return heap_; }

    [[nodiscard]] nri::PipelineLayout* pipeline_layout() const noexcept { return layout_; }

    void bind(nri::CommandBuffer& cmd, const nri::CoreInterface& core, const nri::DescriptorHeapInterface& heap_iface,
              QueueKind resolved_queue) const noexcept;

private:
    const nri::DescriptorHeapInterface* heap_iface_{};

    nri::DescriptorHeap* heap_{};

    nri::PipelineLayout* layout_{};

    nri::Buffer* sentinel_buffer_{};

    nri::Descriptor* sentinel_srv_{};

    std::array<nri::Descriptor*, static_cast<std::size_t>(StaticSampler::count)> static_samplers_{};

    std::mutex mutex_;

    Array<std::uint32_t> free_indices_;

    std::uint32_t next_index_ = 0;
};

class QueueTimeline {
public:
    QueueTimeline() = default;

    ~QueueTimeline();

    QueueTimeline(const QueueTimeline&) = delete;

    QueueTimeline& operator=(const QueueTimeline&) = delete;

    [[nodiscard]] Result<void> initialize(nri::Device& device, ID3D12Device& d3d12_device,
                                          const nri::CoreInterface& core, const nri::WrapperD3D12Interface& wrapper,
                                          bool has_async_compute, bool has_copy_queue);

    void shutdown(const nri::CoreInterface& core) noexcept;

    [[nodiscard]] nri::Queue* queue(QueueKind kind) const noexcept;

    [[nodiscard]] QueueKind resolve_kind(QueueKind requested) const noexcept;

    [[nodiscard]] nri::Fence* fence(QueueKind kind) const noexcept;

    [[nodiscard]] std::uint64_t current_value(QueueKind kind) const noexcept;

    [[nodiscard]] FencePoint current_point() const noexcept;

    [[nodiscard]] Result<std::uint64_t> completed_value(QueueKind kind) const;

    [[nodiscard]] Result<FencePoint> completed_point() const;

    [[nodiscard]] Result<std::uint64_t> submit(QueueKind kind, Span<nri::CommandBuffer* const> command_buffers,
                                               Span<const std::pair<QueueKind, std::uint64_t>> waits = {});

    [[nodiscard]] Result<void> wait(QueueKind kind, std::uint64_t value, std::uint32_t timeout_ms) const;

    [[nodiscard]] Result<void> wait_all(const FencePoint& point, std::uint32_t timeout_ms) const;

    [[nodiscard]] Result<void> drain_all(std::uint32_t timeout_ms);

    [[nodiscard]] ClockCalibration calibration(QueueKind kind) const noexcept;

    [[nodiscard]] bool is_device_lost() const noexcept;

    void report_dred_once() const noexcept;

private:
    struct Lane {
        nri::Queue* queue{};

        ID3D12CommandQueue* d3d12_queue{};

        Microsoft::WRL::ComPtr<ID3D12Fence> d3d12_fence;

        nri::Fence* nri_fence{};

        HANDLE event{};

        std::uint64_t last_signaled = 0;

        std::uint64_t frequency = 0;
    };

    nri::Device* nri_device_{};

    ID3D12Device* d3d12_device_{};

    const nri::CoreInterface* core_{};

    std::array<Lane, 3> lanes_{};

    mutable std::atomic<bool> dred_reported_{false};

    bool has_compute_{};

    bool has_copy_{};
};

class DeferredReclaimer {
public:
    struct Entry {
        FencePoint fence;

        nri::Buffer* buffer{};

        nri::Texture* texture{};

        nri::Descriptor* descriptor{};

        nri::Pipeline* pipeline{};

        nri::Memory* memory{};

        nri::AccelerationStructure* as{};

        nri::CommandBuffer* command_buffer{};

        nri::CommandAllocator* command_allocator{};

        std::uint32_t bindless_index = bindless_invalid_index;
    };

    void retire(Entry entry);

    [[nodiscard]] Result<void> reclaim(const QueueTimeline& timeline, BindlessHeap& bindless,
                                       const nri::CoreInterface& core, const nri::RayTracingInterface* rt);

    void force_release_all(BindlessHeap& bindless, const nri::CoreInterface& core,
                           const nri::RayTracingInterface* rt) noexcept;

    [[nodiscard]] std::size_t pending_count() const noexcept { return entries_.size(); }

private:
    Array<Entry> entries_;
};

} // namespace detail

// Internal render-domain accessors (strictly PRIVATE to rhi, rdg, renderer, and rhi_acceptance).
struct BackendAccess {
    [[nodiscard]] static detail::DeviceState& state(Device& device) noexcept;

    [[nodiscard]] static const detail::DeviceState& state(const Device& device) noexcept;

    [[nodiscard]] static nri::Device& nri_device(Device& device) noexcept;

    [[nodiscard]] static ID3D12Device& d3d12_device(Device& device) noexcept;

    [[nodiscard]] static const nri::CoreInterface& core(const Device& device) noexcept;

    [[nodiscard]] static const nri::DescriptorHeapInterface& descriptor_heap(const Device& device) noexcept;

    [[nodiscard]] static const nri::MeshShaderInterface& mesh_shader(const Device& device) noexcept;

    [[nodiscard]] static const nri::RayTracingInterface& ray_tracing(const Device& device) noexcept;

    [[nodiscard]] static const nri::WrapperD3D12Interface& wrapper_d3d12(const Device& device) noexcept;

    [[nodiscard]] static detail::BindlessHeap& bindless(Device& device) noexcept;

    [[nodiscard]] static detail::QueueTimeline& timeline(Device& device) noexcept;

    [[nodiscard]] static detail::DeferredReclaimer& reclaimer(Device& device) noexcept;

    [[nodiscard]] static Result<Pipeline> compile_pipeline(Device& device, const PipelineCompileDesc& desc);

    [[nodiscard]] static Result<PipelineJob> submit_pipeline(Device& device, const PipelineCompileDesc& desc);

    [[nodiscard]] static Optional<Result<Pipeline>> take_pipeline(Device& device, PipelineJob& job);

    [[nodiscard]] static nri::Pipeline* nri_pipeline(const Pipeline& pipeline) noexcept;

    [[nodiscard]] static PipelineKind pipeline_kind(const Pipeline& pipeline) noexcept;

    [[nodiscard]] static Result<AcquiredBackbuffer> acquire_backbuffer(WindowSurface& surface);

    [[nodiscard]] static Result<void> present(WindowSurface& surface, bool vsync, FrameInfo* details = nullptr);

    [[nodiscard]] static Result<bool> test_occluded(WindowSurface& surface);
};

} // namespace atelier::rhi

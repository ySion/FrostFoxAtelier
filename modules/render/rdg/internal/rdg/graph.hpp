#pragma once

#include "foundation/array.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "memory/fatal.hpp"
#include "rdg/rdg.hpp"
#include "rhi/backend.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace atelier::rdg {

inline constexpr std::uint32_t invalid_handle_index = 0xFFFFFFFFu;

struct TextureHandle {
    std::uint32_t index = invalid_handle_index;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_handle_index; }

    constexpr bool operator==(const TextureHandle&) const noexcept = default;
};

struct BufferHandle {
    std::uint32_t index = invalid_handle_index;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_handle_index; }

    constexpr bool operator==(const BufferHandle&) const noexcept = default;
};

struct TextureState {
    nri::AccessBits access = nri::AccessBits::NONE;

    nri::Layout layout = nri::Layout::UNDEFINED;

    nri::StageBits stages = nri::StageBits::NONE;
};

struct BufferState {
    nri::AccessBits access = nri::AccessBits::NONE;

    nri::StageBits stages = nri::StageBits::NONE;
};

struct TextureDesc {
    nri::TextureType type = nri::TextureType::TEXTURE_2D;

    nri::Format format = nri::Format::RGBA8_UNORM;

    std::uint32_t width = 1;

    std::uint32_t height = 1;

    std::uint32_t depth = 1;

    std::uint16_t mip_count = 1;

    std::uint16_t layer_count = 1;

    std::uint8_t sample_count = 1;

    nri::TextureUsageBits usage = nri::TextureUsageBits::NONE;

    nri::ClearValue clear_value{.color = {.f = {0.0f, 0.0f, 0.0f, 0.0f}}};
};

struct BufferDesc {
    std::uint64_t size = 0;

    std::uint32_t structure_stride = 0;

    bool byte_address = false;

    nri::BufferUsageBits usage = nri::BufferUsageBits::NONE;
};

struct SubresourceRange {
    std::uint16_t base_mip = 0;

    std::uint16_t mip_count = 0; // 0 == all remaining mips from base_mip

    [[nodiscard]] static constexpr SubresourceRange all() noexcept { return {0u, 0u}; }

    [[nodiscard]] static constexpr SubresourceRange single_mip(std::uint16_t mip) noexcept { return {mip, 1u}; }

    constexpr bool operator==(const SubresourceRange&) const noexcept = default;
};

struct ImportedTextureViews {
    nri::Descriptor* rtv = nullptr;

    nri::Descriptor* dsv = nullptr;

    nri::Descriptor* dsv_readonly = nullptr;

    std::uint32_t srv_bindless = rhi::bindless_invalid_index;

    std::uint32_t uav_bindless = rhi::bindless_invalid_index;

    Span<const std::uint32_t> mip_srv_bindless{};

    Span<const std::uint32_t> mip_uav_bindless{};
};

struct ImportedBufferViews {
    std::uint32_t srv_bindless = rhi::bindless_invalid_index;

    std::uint32_t uav_bindless = rhi::bindless_invalid_index;
};

// Frame-scoped bump allocator reset in O(1) at the start of every Graph build.
class LinearArena {
public:
    explicit LinearArena(std::size_t initial_capacity = 256u * 1024u);

    ~LinearArena();

    LinearArena(const LinearArena&) = delete;

    LinearArena& operator=(const LinearArena&) = delete;

    LinearArena(LinearArena&& other) noexcept;

    LinearArena& operator=(LinearArena&& other) noexcept;

    void reset() noexcept;

    [[nodiscard]] void* allocate_bytes(std::size_t size, std::size_t alignment);

    template <class T>
    [[nodiscard]] T* allocate(std::size_t count = 1) {
        if (count == 0)
            return nullptr;
        if (count > (~std::size_t{0}) / sizeof(T))
            atelier::fatal("LinearArena allocation overflow.");
        return static_cast<T*>(allocate_bytes(sizeof(T) * count, alignof(T)));
    }

    template <class T, class... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        void* storage = allocate_bytes(sizeof(T), alignof(T));
        T* object = std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            auto* dtor = static_cast<DestructorNode*>(allocate_bytes(sizeof(DestructorNode), alignof(DestructorNode)));
            dtor->object = object;
            dtor->destroy = [](void* ptr) noexcept { std::destroy_at(static_cast<T*>(ptr)); };
            dtor->next = destructors_;
            destructors_ = dtor;
        }
        return object;
    }

    [[nodiscard]] StringView copy_string(StringView text);

private:
    struct Block {
        std::byte* data = nullptr;

        std::size_t capacity = 0;

        std::size_t offset = 0;
    };

    struct DestructorNode {
        void* object = nullptr;

        void (*destroy)(void*) noexcept = nullptr;

        DestructorNode* next = nullptr;
    };

    void run_destructors() noexcept;

    Array<Block> blocks_;

    std::size_t current_block_ = 0;

    DestructorNode* destructors_ = nullptr;
};

class Graph;

class PassBuilder {
public:
    void read_texture(TextureHandle handle, nri::StageBits stages = nri::StageBits::COMPUTE_SHADER,
                      nri::Layout layout = nri::Layout::SHADER_RESOURCE,
                      SubresourceRange range = SubresourceRange::all());

    void write_texture(TextureHandle handle, nri::StageBits stages = nri::StageBits::COMPUTE_SHADER,
                       SubresourceRange range = SubresourceRange::all());

    void render_target(TextureHandle handle, nri::LoadOp load_op = nri::LoadOp::LOAD,
                       nri::StoreOp store_op = nri::StoreOp::STORE,
                       nri::ClearValue clear = {.color = {.f = {0.0f, 0.0f, 0.0f, 0.0f}}}, bool blend_read = false);

    void depth_target(TextureHandle handle, nri::LoadOp load_op = nri::LoadOp::LOAD,
                      nri::StoreOp store_op = nri::StoreOp::STORE, float clear_depth = 0.0f, bool read_only = false);

    void copy_source_texture(TextureHandle handle);

    void copy_dest_texture(TextureHandle handle);

    void read_buffer(BufferHandle handle, nri::AccessBits access = nri::AccessBits::SHADER_RESOURCE,
                     nri::StageBits stages = nri::StageBits::COMPUTE_SHADER);

    void write_buffer(BufferHandle handle, nri::AccessBits access = nri::AccessBits::SHADER_RESOURCE_STORAGE,
                      nri::StageBits stages = nri::StageBits::COMPUTE_SHADER);

    void copy_source_buffer(BufferHandle handle);

    void copy_dest_buffer(BufferHandle handle);

    void mark_side_effect() noexcept { side_effect_ = true; }

    void set_scope(StringView scope) noexcept;

private:
    friend class Graph;

    PassBuilder(Graph& graph, std::uint32_t pass_index) noexcept : graph_(graph), pass_index_(pass_index) {}

    Graph& graph_;

    std::uint32_t pass_index_ = 0;

    bool side_effect_ = false;
};

class PassContext {
public:
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_; }

    [[nodiscard]] nri::CommandBuffer& cmd() const noexcept { return *cmd_; }

    [[nodiscard]] const nri::CoreInterface& core() const noexcept { return *core_; }

    [[nodiscard]] const nri::MeshShaderInterface& mesh_shader() const noexcept { return *mesh_shader_; }

    [[nodiscard]] nri::Texture* texture(TextureHandle handle) const noexcept;

    [[nodiscard]] nri::Buffer* buffer(BufferHandle handle) const noexcept;

    [[nodiscard]] nri::Descriptor* rtv(TextureHandle handle) const noexcept;

    [[nodiscard]] nri::Descriptor* dsv(TextureHandle handle, bool read_only = false) const noexcept;

    [[nodiscard]] std::uint32_t srv(TextureHandle handle) const noexcept;

    [[nodiscard]] std::uint32_t srv(TextureHandle handle, std::uint16_t mip) const noexcept;

    [[nodiscard]] std::uint32_t uav(TextureHandle handle) const noexcept;

    [[nodiscard]] std::uint32_t uav(TextureHandle handle, std::uint16_t mip) const noexcept;

    [[nodiscard]] std::uint32_t srv(BufferHandle handle) const noexcept;

    [[nodiscard]] std::uint32_t uav(BufferHandle handle) const noexcept;

    // Begins/ends dynamic rendering from the render_target / depth_target attachments declared on PassBuilder.
    void begin_rendering() const noexcept;

    void end_rendering() const noexcept;

    void set_pipeline(const rhi::Pipeline& pipeline) const noexcept;

    void set_root_constants(const void* data, std::uint32_t byte_size, std::uint32_t byte_offset = 0,
                            nri::BindPoint bind_point = nri::BindPoint::INHERIT) const noexcept;

    template <class T>
        requires(std::is_trivially_copyable_v<T> && sizeof(T) <= rhi::root_constant_byte_size && (sizeof(T) % 4u) == 0u)
    void push_constants(const T& constants, nri::BindPoint bind_point = nri::BindPoint::INHERIT) const noexcept {
        set_root_constants(&constants, static_cast<std::uint32_t>(sizeof(T)), 0u, bind_point);
    }

private:
    friend class Executor;

    rhi::Device* device_ = nullptr;

    const Graph* graph_ = nullptr;

    std::uint32_t pass_index_ = 0;

    nri::CommandBuffer* cmd_ = nullptr;

    const nri::CoreInterface* core_ = nullptr;

    const nri::MeshShaderInterface* mesh_shader_ = nullptr;

    mutable nri::BindPoint current_bind_point_ = nri::BindPoint::COMPUTE;
};

namespace detail {
struct ExecutorState;
} // namespace detail

struct ExecuteOptions {
    std::uint64_t frame_id = 0;

    bool collect_timestamps = true;

    bool collect_pipeline_statistics = false;

    TraceSnapshot* trace = nullptr;

    StringView trace_scope{};
};

// Frame-local RDG builder allocated from LinearArena.
class Graph {
public:
    explicit Graph(LinearArena& arena) noexcept;

    Graph(const Graph&) = delete;

    Graph& operator=(const Graph&) = delete;

    [[nodiscard]] LinearArena& arena() noexcept { return arena_; }

    [[nodiscard]] TextureHandle create_texture(StringView name, const TextureDesc& desc);

    [[nodiscard]] BufferHandle create_buffer(StringView name, const BufferDesc& desc);

    [[nodiscard]] TextureHandle import_texture(StringView name, nri::Texture* texture, TextureState initial_state,
                                               TextureState final_state = {}, ImportedTextureViews views = {},
                                               TextureState* out_committed_state = nullptr);

    [[nodiscard]] BufferHandle import_buffer(StringView name, nri::Buffer* buffer, BufferState initial_state,
                                             BufferState final_state = {}, ImportedBufferViews views = {},
                                             BufferState* out_committed_state = nullptr);

    void export_texture(TextureHandle handle, TextureState final_state);

    void export_buffer(BufferHandle handle, BufferState final_state);

    template <class SetupFn, class ExecuteFn>
    void add_pass(StringView name, rhi::QueueKind queue, SetupFn&& setup, ExecuteFn&& execute) {
        using StoredFn = std::decay_t<ExecuteFn>;
        auto* stored = arena_.construct<StoredFn>(std::forward<ExecuteFn>(execute));
        const auto thunk = [](void* callable, const PassContext& ctx) -> rhi::Result<void> {
            return (*static_cast<StoredFn*>(callable))(ctx);
        };
        const std::uint32_t pass_index = begin_pass(name, queue, stored, thunk);
        PassBuilder builder(*this, pass_index);
        std::forward<SetupFn>(setup)(builder);
        finish_pass(pass_index, builder.side_effect_);
    }

private:
    friend class PassBuilder;

    friend class PassContext;

    friend class Executor;

    using PassThunk = rhi::Result<void> (*)(void*, const PassContext&);

    std::uint32_t begin_pass(StringView name, rhi::QueueKind queue, void* callable, PassThunk thunk);

    void finish_pass(std::uint32_t pass_index, bool side_effect);

    struct TextureNode;

    struct BufferNode;

    struct PassAccess;

    struct ColorTargetBinding;

    struct DepthTargetBinding;

    struct PassNode;

    LinearArena& arena_;

    Array<TextureNode*> textures_;

    Array<BufferNode*> buffers_;

    Array<PassNode*> passes_;
};

// Persistent per-view or per-renderer RDG executor owning the cross-frame Placed Resource
// aliasing pool, multi-queue command allocators, and calibrated GPU timestamp/statistics rings.
class Executor {
public:
    Executor() noexcept;

    ~Executor();

    Executor(Executor&& other) noexcept;

    Executor& operator=(Executor&& other) noexcept;

    Executor(const Executor&) = delete;

    Executor& operator=(const Executor&) = delete;

    [[nodiscard]] static rhi::Result<Executor> open(rhi::Device& device);

    [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    // Compiles, barriers, executes, and submits all live passes of `graph`.
    // Returns the multi-queue FencePoint signaled by this graph execution.
    [[nodiscard]] rhi::Result<rhi::FencePoint> execute(Graph& graph, const ExecuteOptions& options = {});

    // Releases transient placed heaps & cached descriptors (used on L2/L3 viewport dormancy).
    void release_transients() noexcept;

    [[nodiscard]] const FrameGpuProfile& latest_gpu_profile() const noexcept;

    [[nodiscard]] TransientStats transient_stats() const noexcept;

    [[nodiscard]] rhi::Result<void> close();

private:
    explicit Executor(std::unique_ptr<detail::ExecutorState> state) noexcept;

    std::unique_ptr<detail::ExecutorState> state_;
};

} // namespace atelier::rdg

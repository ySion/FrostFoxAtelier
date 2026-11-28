#include "rdg/graph.hpp"

#include "foundation/result.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

namespace atelier::rdg {

namespace {

[[nodiscard]] constexpr std::size_t align_up_size(std::size_t value, std::size_t alignment) noexcept {
    const std::size_t mask = alignment - 1u;
    return (value + mask) & ~mask;
}

[[nodiscard]] constexpr std::uint64_t align_down_u64(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (alignment <= 1u)
        return value;
    return (value / alignment) * alignment;
}

[[nodiscard]] constexpr std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (alignment <= 1u)
        return value;
    const std::uint64_t rem = value % alignment;
    return rem == 0u ? value : (value + (alignment - rem));
}

// NRI StageBits sentinel algebra: StageBits::ALL == 0u, StageBits::NONE == 0x7FFFFFFFu!
// Raw bitwise `|` or `&` without handling ALL and NONE corrupts stage masks.
[[nodiscard]] constexpr nri::StageBits merge_stages(nri::StageBits a, nri::StageBits b) noexcept {
    if (a == nri::StageBits::NONE)
        return b;
    if (b == nri::StageBits::NONE)
        return a;
    if (a == nri::StageBits::ALL || b == nri::StageBits::ALL)
        return nri::StageBits::ALL;
    return static_cast<nri::StageBits>(a | b);
}

[[nodiscard]] constexpr bool has_any_stage(nri::StageBits stages, nri::StageBits mask) noexcept {
    if (stages == nri::StageBits::NONE || mask == nri::StageBits::NONE)
        return false;
    if (stages == nri::StageBits::ALL || mask == nri::StageBits::ALL)
        return true;
    return static_cast<std::uint32_t>(stages & mask) != 0u;
}

[[nodiscard]] constexpr nri::StageBits filter_stages(nri::StageBits stages, nri::StageBits allowed,
                                                     nri::StageBits fallback_if_all) noexcept {
    if (stages == nri::StageBits::NONE)
        return nri::StageBits::NONE;
    if (stages == nri::StageBits::ALL)
        return fallback_if_all;
    const auto masked = static_cast<nri::StageBits>(stages & allowed);
    return (static_cast<std::uint32_t>(masked) != 0u) ? masked : nri::StageBits::NONE;
}

[[nodiscard]] constexpr nri::Layout merge_texture_layout(nri::Layout a, nri::Layout b,
                                                         nri::AccessBits merged_access) noexcept {
    if (a == nri::Layout::UNDEFINED)
        return b;
    if (b == nri::Layout::UNDEFINED || a == b)
        return a;
    if ((merged_access & nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE) != 0)
        return nri::Layout::DEPTH_STENCIL_ATTACHMENT;
    if ((merged_access & nri::AccessBits::COLOR_ATTACHMENT_WRITE) != 0)
        return nri::Layout::COLOR_ATTACHMENT;
    constexpr nri::AccessBits ro_depth_compatible = nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ |
                                                    nri::AccessBits::SHADER_RESOURCE | nri::AccessBits::COPY_SOURCE;
    if ((merged_access & nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ) != 0 &&
        (merged_access & ~ro_depth_compatible) == 0) {
        return nri::Layout::DEPTH_STENCIL_READONLY;
    }
    return nri::Layout::GENERAL;
}

[[nodiscard]] rhi::Result<void> check_nri(nri::Result result, rhi::ErrorCode code, const char* message) {
    if (result == nri::Result::SUCCESS)
        return {};
    rhi::ErrorCode mapped = code;
    if (result == nri::Result::DEVICE_LOST)
        mapped = rhi::ErrorCode::device_lost;
    else if (result == nri::Result::OUT_OF_MEMORY)
        mapped = rhi::ErrorCode::out_of_memory;
    else if (result == nri::Result::UNSUPPORTED)
        mapped = rhi::ErrorCode::unsupported;
    return std::unexpected(rhi::Error{mapped, static_cast<std::int32_t>(result), message});
}

[[nodiscard]] constexpr bool is_write_access(nri::AccessBits access) noexcept {
    constexpr nri::AccessBits write_mask =
        nri::AccessBits::SCRATCH_BUFFER | nri::AccessBits::COLOR_ATTACHMENT_WRITE |
        nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE | nri::AccessBits::ACCELERATION_STRUCTURE_WRITE |
        nri::AccessBits::MICROMAP_WRITE | nri::AccessBits::SHADER_RESOURCE_STORAGE | nri::AccessBits::COPY_DESTINATION |
        nri::AccessBits::RESOLVE_DESTINATION | nri::AccessBits::RESOLVE_ATTACHMENT_DESTINATION |
        nri::AccessBits::CLEAR_STORAGE | nri::AccessBits::HOST_WRITE | nri::AccessBits::VIDEO_DECODE_WRITE |
        nri::AccessBits::VIDEO_ENCODE_WRITE;
    return (access & write_mask) != 0;
}

[[nodiscard]] constexpr bool is_graphics_only_layout(nri::Layout layout) noexcept {
    switch (layout) {
    case nri::Layout::PRESENT:
    case nri::Layout::COLOR_ATTACHMENT:
    case nri::Layout::DEPTH_STENCIL_ATTACHMENT:
    case nri::Layout::DEPTH_READONLY_STENCIL_ATTACHMENT:
    case nri::Layout::DEPTH_ATTACHMENT_STENCIL_READONLY:
    case nri::Layout::DEPTH_STENCIL_READONLY:
    case nri::Layout::SHADING_RATE_ATTACHMENT:
    case nri::Layout::INPUT_ATTACHMENT:
    case nri::Layout::RESOLVE_SOURCE:
    case nri::Layout::RESOLVE_DESTINATION:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr nri::PlaneBits texture_planes_for_format(nri::Format format) noexcept {
    const nri::FormatProps* props = nri::nriGetFormatProps(format);
    if (!props)
        return nri::PlaneBits::ALL;
    if (props->isDepth && props->isStencil)
        return nri::PlaneBits::DEPTH | nri::PlaneBits::STENCIL;
    if (props->isDepth)
        return nri::PlaneBits::DEPTH;
    return nri::PlaneBits::COLOR;
}

[[nodiscard]] std::uint64_t hash_mix(std::uint64_t seed, std::uint64_t value) noexcept {
    value ^= value >> 33u;
    value *= 0xff51afd7ed558ccduLL;
    value ^= value >> 33u;
    value *= 0xc4ceb9fe1a85ec53uLL;
    value ^= value >> 33u;
    return seed ^ (value + 0x9e3779b97f4a7c15uLL + (seed << 6u) + (seed >> 2u));
}

} // namespace

// ============================================================================
// LinearArena
// ============================================================================

LinearArena::LinearArena(std::size_t initial_capacity) {
    const std::size_t cap = std::max<std::size_t>(initial_capacity, 4096u);
    auto* mem = static_cast<std::byte*>(::operator new(cap, std::align_val_t{64}));
    blocks_.add(Block{mem, cap, 0u});
}

LinearArena::~LinearArena() {
    run_destructors();
    for (Block& block : blocks_) {
        if (block.data)
            ::operator delete(block.data, std::align_val_t{64});
    }
}

LinearArena::LinearArena(LinearArena&& other) noexcept
    : blocks_(std::move(other.blocks_)), current_block_(std::exchange(other.current_block_, 0u)),
      destructors_(std::exchange(other.destructors_, nullptr)) {}

LinearArena& LinearArena::operator=(LinearArena&& other) noexcept {
    if (this != &other) {
        run_destructors();
        for (Block& block : blocks_) {
            if (block.data)
                ::operator delete(block.data, std::align_val_t{64});
        }
        blocks_ = std::move(other.blocks_);
        current_block_ = std::exchange(other.current_block_, 0u);
        destructors_ = std::exchange(other.destructors_, nullptr);
    }
    return *this;
}

void LinearArena::run_destructors() noexcept {
    DestructorNode* node = destructors_;
    destructors_ = nullptr;
    while (node) {
        DestructorNode* next = node->next;
        node->destroy(node->object);
        node = next;
    }
}

void LinearArena::reset() noexcept {
    run_destructors();
    if (blocks_.size() > 1u) {
        std::size_t total = 0u;
        for (const Block& block : blocks_) {
            if (block.capacity > (~std::size_t{0}) - 64u - total)
                atelier::fatal("LinearArena reset capacity overflow.");
            total += block.capacity;
        }
        const std::size_t merged = align_up_size(total, 64u);
        auto* mem = static_cast<std::byte*>(::operator new(merged, std::align_val_t{64}));
        for (Block& block : blocks_) {
            if (block.data)
                ::operator delete(block.data, std::align_val_t{64});
        }
        blocks_.clear();
        blocks_.add(Block{mem, merged, 0u});
    } else if (!blocks_.empty()) {
        blocks_[0].offset = 0u;
    }
    current_block_ = 0u;
}

void* LinearArena::allocate_bytes(std::size_t size, std::size_t alignment) {
    const std::size_t align = std::max<std::size_t>(alignment, alignof(std::max_align_t));
    if (align > 64u)
        atelier::fatal("LinearArena alignment exceeds 64-byte block alignment.");
    if (size > (~std::size_t{0}) - 4160u)
        atelier::fatal("LinearArena allocation size overflow.");
    while (current_block_ < blocks_.size()) {
        Block& block = blocks_[current_block_];
        if (block.offset <= block.capacity - (align - 1u)) {
            const std::size_t aligned = align_up_size(block.offset, align);
            if (aligned <= block.capacity && size <= block.capacity - aligned) {
                block.offset = aligned + size;
                return block.data + aligned;
            }
        }
        ++current_block_;
    }
    const std::size_t prev_cap = blocks_.empty() ? (256u * 1024u) : blocks_.back().capacity;
    const std::size_t doubled = (prev_cap <= (~std::size_t{0}) / 2u) ? (prev_cap * 2u) : (~std::size_t{0} & ~63u);
    const std::size_t grown = std::max(doubled, align_up_size(size + 64u, 4096u));
    auto* mem = static_cast<std::byte*>(::operator new(grown, std::align_val_t{64}));
    blocks_.add(Block{mem, grown, size});
    current_block_ = blocks_.size() - 1u;
    return mem;
}

StringView LinearArena::copy_string(StringView text) {
    if (text.empty())
        return {};
    auto* dst = allocate<char>(text.size() + 1u);
    std::memcpy(dst, text.data(), text.size());
    dst[text.size()] = '\0';
    return StringView{dst, text.size()};
}

// ============================================================================
// Graph Internal Nodes
// ============================================================================

struct MipTrackingState {
    TextureState state{};

    rhi::QueueKind queue = rhi::QueueKind::graphics;
};

struct Graph::TextureNode {
    StringView name{};

    TextureDesc desc{};

    bool imported = false;

    bool exported = false;

    nri::Texture* runtime_texture = nullptr;

    nri::Format resolved_format = nri::Format::UNKNOWN;

    std::uint16_t resolved_mip_count = 1u;

    MipTrackingState* mip_states = nullptr;

    TextureState initial_state{};

    TextureState final_state{};

    TextureState current_state{};

    TextureState* out_committed_state = nullptr;

    rhi::QueueKind current_queue = rhi::QueueKind::graphics;

    ImportedTextureViews views{};

    bool needed = false;

    bool simultaneous = false;

    std::uint32_t first_pass = invalid_handle_index;

    std::uint32_t last_pass = invalid_handle_index;

    std::uint8_t queue_mask = 0u;

    std::uint32_t heap_index = invalid_handle_index;

    std::uint64_t byte_offset = 0u;

    std::uint64_t byte_size = 0u;

    std::array<nri::AccessBits, 3> per_queue_accesses{};

    nri::AccessBits first_pass_access = nri::AccessBits::NONE;
};

struct Graph::BufferNode {
    StringView name{};

    BufferDesc desc{};

    bool imported = false;

    bool exported = false;

    nri::Buffer* runtime_buffer = nullptr;

    BufferState initial_state{};

    BufferState final_state{};

    BufferState current_state{};

    BufferState* out_committed_state = nullptr;

    rhi::QueueKind current_queue = rhi::QueueKind::graphics;

    ImportedBufferViews views{};

    bool needed = false;

    std::uint32_t first_pass = invalid_handle_index;

    std::uint32_t last_pass = invalid_handle_index;

    std::uint8_t queue_mask = 0u;

    std::uint32_t heap_index = invalid_handle_index;

    std::uint64_t byte_offset = 0u;

    std::uint64_t byte_size = 0u;

    std::array<nri::AccessBits, 3> per_queue_accesses{};

    nri::AccessBits first_pass_access = nri::AccessBits::NONE;
};

struct Graph::PassAccess {
    bool is_texture = true;

    std::uint32_t index = 0u;

    bool is_write = false;

    nri::AccessBits access = nri::AccessBits::NONE;

    nri::StageBits stages = nri::StageBits::NONE;

    nri::Layout layout = nri::Layout::UNDEFINED;

    SubresourceRange range{};

    PassAccess* next = nullptr;
};

struct Graph::ColorTargetBinding {
    TextureHandle handle{};

    nri::LoadOp load_op = nri::LoadOp::LOAD;

    nri::StoreOp store_op = nri::StoreOp::STORE;

    nri::ClearValue clear_value{};
};

struct Graph::DepthTargetBinding {
    TextureHandle handle{};

    nri::LoadOp load_op = nri::LoadOp::LOAD;

    nri::StoreOp store_op = nri::StoreOp::STORE;

    float clear_depth = 0.0f;

    bool read_only = false;
};

struct Graph::PassNode {
    StringView name{};

    StringView scope{};

    rhi::QueueKind requested_queue = rhi::QueueKind::graphics;

    rhi::QueueKind resolved_queue = rhi::QueueKind::graphics;

    void* callable = nullptr;

    PassThunk thunk = nullptr;

    bool side_effect = false;

    bool culled = false;

    StringView cull_reason{};

    PassAccess* first_access = nullptr;

    PassAccess* last_access = nullptr;

    std::array<ColorTargetBinding, 8> color_targets{};

    std::uint32_t color_target_count = 0u;

    DepthTargetBinding depth_target{};

    bool has_depth_target = false;
};

// ============================================================================
// Graph & PassBuilder
// ============================================================================

Graph::Graph(LinearArena& arena) noexcept : arena_(arena) {
    textures_.reserve(64u);
    buffers_.reserve(64u);
    passes_.reserve(64u);
}

TextureHandle Graph::create_texture(StringView name, const TextureDesc& desc) {
    auto* node = arena_.construct<TextureNode>();
    node->name = arena_.copy_string(name);
    node->desc = desc;
    node->imported = false;
    node->resolved_format = desc.format;
    node->initial_state = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
    node->current_state = node->initial_state;
    const auto index = static_cast<std::uint32_t>(textures_.size());
    textures_.add(node);
    return TextureHandle{index};
}

BufferHandle Graph::create_buffer(StringView name, const BufferDesc& desc) {
    auto* node = arena_.construct<BufferNode>();
    node->name = arena_.copy_string(name);
    node->desc = desc;
    node->imported = false;
    node->initial_state = {nri::AccessBits::NONE, nri::StageBits::NONE};
    node->current_state = node->initial_state;
    const auto index = static_cast<std::uint32_t>(buffers_.size());
    buffers_.add(node);
    return BufferHandle{index};
}

TextureHandle Graph::import_texture(StringView name, nri::Texture* texture, TextureState initial_state,
                                    TextureState final_state, ImportedTextureViews views,
                                    TextureState* out_committed_state) {
    if (!texture)
        atelier::fatal("Graph::import_texture requires a non-null nri::Texture.");
    auto* node = arena_.construct<TextureNode>();
    node->name = arena_.copy_string(name);
    node->imported = true;
    node->exported = (final_state.layout != nri::Layout::UNDEFINED);
    node->runtime_texture = texture;
    node->initial_state = initial_state;
    node->final_state = final_state;
    node->current_state = initial_state;
    node->out_committed_state = out_committed_state;
    node->views = views;
    const auto index = static_cast<std::uint32_t>(textures_.size());
    textures_.add(node);
    return TextureHandle{index};
}

BufferHandle Graph::import_buffer(StringView name, nri::Buffer* buffer, BufferState initial_state,
                                  BufferState final_state, ImportedBufferViews views,
                                  BufferState* out_committed_state) {
    if (!buffer)
        atelier::fatal("Graph::import_buffer requires a non-null nri::Buffer.");
    auto* node = arena_.construct<BufferNode>();
    node->name = arena_.copy_string(name);
    node->imported = true;
    node->exported = (final_state.stages != nri::StageBits::NONE || final_state.access != nri::AccessBits::NONE);
    node->runtime_buffer = buffer;
    node->initial_state = initial_state;
    node->final_state = final_state;
    node->current_state = initial_state;
    node->out_committed_state = out_committed_state;
    node->views = views;
    const auto index = static_cast<std::uint32_t>(buffers_.size());
    buffers_.add(node);
    return BufferHandle{index};
}

void Graph::export_texture(TextureHandle handle, TextureState final_state) {
    if (!handle.valid() || handle.index >= textures_.size())
        atelier::fatal("Graph::export_texture received invalid handle.");
    TextureNode& node = *textures_[handle.index];
    node.exported = true;
    node.final_state = final_state;
}

void Graph::export_buffer(BufferHandle handle, BufferState final_state) {
    if (!handle.valid() || handle.index >= buffers_.size())
        atelier::fatal("Graph::export_buffer received invalid handle.");
    BufferNode& node = *buffers_[handle.index];
    node.exported = true;
    node.final_state = final_state;
}

std::uint32_t Graph::begin_pass(StringView name, rhi::QueueKind queue, void* callable, PassThunk thunk) {
    auto* pass = arena_.construct<PassNode>();
    pass->name = arena_.copy_string(name);
    pass->requested_queue = queue;
    pass->resolved_queue = queue;
    pass->callable = callable;
    pass->thunk = thunk;
    const auto index = static_cast<std::uint32_t>(passes_.size());
    passes_.add(pass);
    return index;
}

void Graph::finish_pass(std::uint32_t pass_index, bool side_effect) {
    passes_[pass_index]->side_effect = side_effect;
}

namespace {

void append_access(LinearArena& arena, auto& pass, bool is_texture, std::uint32_t index, bool is_write,
                   nri::AccessBits access, nri::StageBits stages, nri::Layout layout,
                   SubresourceRange range = SubresourceRange::all()) {
    for (auto* existing = pass.first_access; existing != nullptr; existing = existing->next) {
        if (existing->is_texture == is_texture && existing->index == index &&
            (!is_texture || existing->range == range)) {
            existing->is_write = existing->is_write || is_write;
            existing->access = existing->access | access;
            existing->stages = merge_stages(existing->stages, stages);
            if (is_texture)
                existing->layout = merge_texture_layout(existing->layout, layout, existing->access);
            return;
        }
    }
    using AccessT = std::remove_pointer_t<decltype(pass.first_access)>;
    auto* entry = arena.construct<AccessT>();
    entry->is_texture = is_texture;
    entry->index = index;
    entry->is_write = is_write;
    entry->access = access;
    entry->stages = stages;
    entry->layout = layout;
    entry->range = range;
    if (!pass.first_access) {
        pass.first_access = entry;
        pass.last_access = entry;
    } else {
        pass.last_access->next = entry;
        pass.last_access = entry;
    }
}

} // namespace

void PassBuilder::read_texture(TextureHandle handle, nri::StageBits stages, nri::Layout layout,
                               SubresourceRange range) {
    if (!handle.valid() || handle.index >= graph_.textures_.size())
        atelier::fatal("PassBuilder::read_texture received invalid handle.");
    auto& tex = *graph_.textures_[handle.index];
    if (!tex.imported)
        tex.desc.usage = tex.desc.usage | nri::TextureUsageBits::SHADER_RESOURCE;
    append_access(graph_.arena_, *graph_.passes_[pass_index_], true, handle.index, false,
                  nri::AccessBits::SHADER_RESOURCE, stages, layout, range);
}

void PassBuilder::write_texture(TextureHandle handle, nri::StageBits stages, SubresourceRange range) {
    if (!handle.valid() || handle.index >= graph_.textures_.size())
        atelier::fatal("PassBuilder::write_texture received invalid handle.");
    auto& tex = *graph_.textures_[handle.index];
    if (!tex.imported)
        tex.desc.usage = tex.desc.usage | nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
    append_access(graph_.arena_, *graph_.passes_[pass_index_], true, handle.index, true,
                  nri::AccessBits::SHADER_RESOURCE_STORAGE, stages, nri::Layout::SHADER_RESOURCE_STORAGE, range);
}

void PassBuilder::render_target(TextureHandle handle, nri::LoadOp load_op, nri::StoreOp store_op, nri::ClearValue clear,
                                bool blend_read) {
    if (!handle.valid() || handle.index >= graph_.textures_.size())
        atelier::fatal("PassBuilder::render_target received invalid handle.");
    auto& pass = *graph_.passes_[pass_index_];
    if (pass.color_target_count >= pass.color_targets.size())
        atelier::fatal("PassBuilder::render_target exceeded maximum 8 color attachments.");
    auto& tex = *graph_.textures_[handle.index];
    if (!tex.imported)
        tex.desc.usage = tex.desc.usage | nri::TextureUsageBits::COLOR_ATTACHMENT;
    pass.color_targets[pass.color_target_count++] = Graph::ColorTargetBinding{handle, load_op, store_op, clear};
    nri::AccessBits access = nri::AccessBits::COLOR_ATTACHMENT_WRITE;
    if (load_op == nri::LoadOp::LOAD || blend_read)
        access = access | nri::AccessBits::COLOR_ATTACHMENT_READ;
    append_access(graph_.arena_, pass, true, handle.index, true, access, nri::StageBits::COLOR_ATTACHMENT,
                  nri::Layout::COLOR_ATTACHMENT);
}

void PassBuilder::depth_target(TextureHandle handle, nri::LoadOp load_op, nri::StoreOp store_op, float clear_depth,
                               bool read_only) {
    if (!handle.valid() || handle.index >= graph_.textures_.size())
        atelier::fatal("PassBuilder::depth_target received invalid handle.");
    auto& pass = *graph_.passes_[pass_index_];
    auto& tex = *graph_.textures_[handle.index];
    if (!tex.imported)
        tex.desc.usage = tex.desc.usage | nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
    pass.depth_target = Graph::DepthTargetBinding{handle, load_op, store_op, clear_depth, read_only};
    pass.has_depth_target = true;
    const nri::AccessBits access =
        read_only ? nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ
                  : (nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_READ | nri::AccessBits::DEPTH_STENCIL_ATTACHMENT_WRITE);
    const nri::Layout layout = read_only ? nri::Layout::DEPTH_STENCIL_READONLY : nri::Layout::DEPTH_STENCIL_ATTACHMENT;
    append_access(graph_.arena_, pass, true, handle.index, !read_only, access, nri::StageBits::DEPTH_STENCIL_ATTACHMENT,
                  layout);
}

void PassBuilder::copy_source_texture(TextureHandle handle) {
    if (!handle.valid() || handle.index >= graph_.textures_.size())
        atelier::fatal("PassBuilder::copy_source_texture received invalid handle.");
    append_access(graph_.arena_, *graph_.passes_[pass_index_], true, handle.index, false, nri::AccessBits::COPY_SOURCE,
                  nri::StageBits::COPY, nri::Layout::COPY_SOURCE);
}

void PassBuilder::copy_dest_texture(TextureHandle handle) {
    if (!handle.valid() || handle.index >= graph_.textures_.size())
        atelier::fatal("PassBuilder::copy_dest_texture received invalid handle.");
    append_access(graph_.arena_, *graph_.passes_[pass_index_], true, handle.index, true,
                  nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::Layout::COPY_DESTINATION);
}

void PassBuilder::read_buffer(BufferHandle handle, nri::AccessBits access, nri::StageBits stages) {
    if (!handle.valid() || handle.index >= graph_.buffers_.size())
        atelier::fatal("PassBuilder::read_buffer received invalid handle.");
    auto& buf = *graph_.buffers_[handle.index];
    if (!buf.imported) {
        if ((access & nri::AccessBits::SHADER_RESOURCE) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::SHADER_RESOURCE;
        if ((access & nri::AccessBits::INDEX_BUFFER) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::INDEX;
        if ((access & nri::AccessBits::VERTEX_BUFFER) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::VERTEX;
        if ((access & nri::AccessBits::CONSTANT_BUFFER) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::CONSTANT;
        if ((access & nri::AccessBits::ARGUMENT_BUFFER) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::ARGUMENT;
    }
    append_access(graph_.arena_, *graph_.passes_[pass_index_], false, handle.index, false, access, stages,
                  nri::Layout::UNDEFINED);
}

void PassBuilder::write_buffer(BufferHandle handle, nri::AccessBits access, nri::StageBits stages) {
    if (!handle.valid() || handle.index >= graph_.buffers_.size())
        atelier::fatal("PassBuilder::write_buffer received invalid handle.");
    auto& buf = *graph_.buffers_[handle.index];
    if (!buf.imported) {
        if ((access & nri::AccessBits::SHADER_RESOURCE_STORAGE) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
        if ((access & nri::AccessBits::SCRATCH_BUFFER) != 0)
            buf.desc.usage = buf.desc.usage | nri::BufferUsageBits::SCRATCH;
    }
    append_access(graph_.arena_, *graph_.passes_[pass_index_], false, handle.index, true, access, stages,
                  nri::Layout::UNDEFINED);
}

void PassBuilder::copy_source_buffer(BufferHandle handle) {
    read_buffer(handle, nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY);
}

void PassBuilder::copy_dest_buffer(BufferHandle handle) {
    write_buffer(handle, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
}

void PassBuilder::set_scope(StringView scope) noexcept {
    graph_.passes_[pass_index_]->scope = graph_.arena_.copy_string(scope);
}

// ============================================================================
// PassContext
// ============================================================================

nri::Texture* PassContext::texture(TextureHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return nullptr;
    return graph_->textures_[handle.index]->runtime_texture;
}

nri::Buffer* PassContext::buffer(BufferHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->buffers_.size())
        return nullptr;
    return graph_->buffers_[handle.index]->runtime_buffer;
}

nri::Descriptor* PassContext::rtv(TextureHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return nullptr;
    return graph_->textures_[handle.index]->views.rtv;
}

nri::Descriptor* PassContext::dsv(TextureHandle handle, bool read_only) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return nullptr;
    const auto& views = graph_->textures_[handle.index]->views;
    return (read_only && views.dsv_readonly) ? views.dsv_readonly : views.dsv;
}

std::uint32_t PassContext::srv(TextureHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return rhi::bindless_invalid_index;
    return graph_->textures_[handle.index]->views.srv_bindless;
}

std::uint32_t PassContext::srv(TextureHandle handle, std::uint16_t mip) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return rhi::bindless_invalid_index;
    const auto& tex = *graph_->textures_[handle.index];
    if (mip < tex.views.mip_srv_bindless.size())
        return tex.views.mip_srv_bindless[mip];
    if (mip == 0u && tex.resolved_mip_count == 1u)
        return tex.views.srv_bindless;
    return rhi::bindless_invalid_index;
}

std::uint32_t PassContext::uav(TextureHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return rhi::bindless_invalid_index;
    return graph_->textures_[handle.index]->views.uav_bindless;
}

std::uint32_t PassContext::uav(TextureHandle handle, std::uint16_t mip) const noexcept {
    if (!handle.valid() || handle.index >= graph_->textures_.size())
        return rhi::bindless_invalid_index;
    const auto& tex = *graph_->textures_[handle.index];
    if (mip < tex.views.mip_uav_bindless.size())
        return tex.views.mip_uav_bindless[mip];
    if (mip == 0u)
        return tex.views.uav_bindless;
    return rhi::bindless_invalid_index;
}

std::uint32_t PassContext::srv(BufferHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->buffers_.size())
        return rhi::bindless_invalid_index;
    return graph_->buffers_[handle.index]->views.srv_bindless;
}

std::uint32_t PassContext::uav(BufferHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= graph_->buffers_.size())
        return rhi::bindless_invalid_index;
    return graph_->buffers_[handle.index]->views.uav_bindless;
}

void PassContext::begin_rendering() const noexcept {
    const auto& pass = *graph_->passes_[pass_index_];
    std::array<nri::AttachmentDesc, 8> colors{};
    for (std::uint32_t i = 0u; i < pass.color_target_count; ++i) {
        const auto& binding = pass.color_targets[i];
        colors[i].descriptor = rtv(binding.handle);
        colors[i].clearValue = binding.clear_value;
        colors[i].loadOp = binding.load_op;
        colors[i].storeOp = binding.store_op;
    }
    nri::RenderingDesc desc{};
    desc.colors = pass.color_target_count > 0u ? colors.data() : nullptr;
    desc.colorNum = pass.color_target_count;
    if (pass.has_depth_target) {
        desc.depth.descriptor = dsv(pass.depth_target.handle, pass.depth_target.read_only);
        desc.depth.clearValue.depthStencil.depth = pass.depth_target.clear_depth;
        desc.depth.clearValue.depthStencil.stencil = 0u;
        desc.depth.loadOp = pass.depth_target.load_op;
        desc.depth.storeOp = pass.depth_target.store_op;
    }
    core_->CmdBeginRendering(*cmd_, desc);
}

void PassContext::end_rendering() const noexcept {
    core_->CmdEndRendering(*cmd_);
}

void PassContext::set_pipeline(const rhi::Pipeline& pipeline) const noexcept {
    nri::Pipeline* raw = rhi::BackendAccess::nri_pipeline(pipeline);
    if (raw) {
        current_bind_point_ = (rhi::BackendAccess::pipeline_kind(pipeline) == rhi::PipelineKind::compute)
                                  ? nri::BindPoint::COMPUTE
                                  : nri::BindPoint::GRAPHICS;
        core_->CmdSetPipeline(*cmd_, *raw);
    }
}

void PassContext::set_root_constants(const void* data, std::uint32_t byte_size, std::uint32_t byte_offset,
                                     nri::BindPoint bind_point) const noexcept {
    nri::SetRootConstantsDesc desc{};
    desc.rootConstantIndex = 0u;
    desc.data = data;
    desc.size = byte_size;
    desc.offset = byte_offset;
    desc.bindPoint = (bind_point == nri::BindPoint::INHERIT) ? current_bind_point_ : bind_point;
    core_->CmdSetRootConstants(*cmd_, desc);
}

// ============================================================================
// ExecutorState: Placed Transient Aliasing Cache + Command Pools + Profilers
// ============================================================================

namespace detail {

struct TransientHeap {
    nri::MemoryType memory_type = 0u;

    nri::Memory* memory = nullptr;

    std::uint64_t capacity = 0u;

    std::uint64_t generation = 0u;

    std::uint64_t last_used_sequence = 0u;

    rhi::FencePoint prev_frame_fences{};

    std::uint8_t prev_frame_queue_mask = 0u;

    bool dedicated = false;

    bool allow_msaa = false;

    bool used_in_prior_frame = false;
};

struct CachedPlacedObject {
    std::uint64_t key_hash = 0u;

    std::uint32_t heap_index = 0u;

    std::uint64_t heap_generation = 0u;

    std::uint64_t byte_offset = 0u;

    bool is_texture = true;

    nri::TextureDesc tex_desc{};

    nri::BufferDesc buf_desc{};

    nri::Texture* texture = nullptr;

    nri::Buffer* buffer = nullptr;

    nri::Descriptor* srv = nullptr;

    nri::Descriptor* uav = nullptr;

    nri::Descriptor* rtv = nullptr;

    nri::Descriptor* dsv = nullptr;

    nri::Descriptor* dsv_readonly = nullptr;

    std::uint32_t srv_bindless = rhi::bindless_invalid_index;

    std::uint32_t uav_bindless = rhi::bindless_invalid_index;

    Array<nri::Descriptor*> mip_srvs;

    Array<std::uint32_t> mip_srv_bindless;

    Array<nri::Descriptor*> mip_uavs;

    Array<std::uint32_t> mip_uav_bindless;

    std::uint64_t last_used_sequence = 0u;
};

inline constexpr std::uint32_t command_ring_size = 4u;

inline constexpr std::uint32_t max_timed_passes_per_frame = 128u;

struct CommandSlice {
    rhi::FencePoint fence{};

    struct QueueCmdPool {
        nri::CommandAllocator* allocator = nullptr;

        Array<nri::CommandBuffer*> buffers;

        std::uint32_t used = 0u;
    };

    std::array<QueueCmdPool, 3> queues{};

    std::uint64_t frame_id = 0u;

    std::uint64_t sequence = 0u;

    bool has_queries = false;

    bool has_pipeline_stats = false;

    struct PendingTimestamp {
        String name;

        rhi::QueueKind queue = rhi::QueueKind::graphics;

        std::uint32_t query_begin = 0u;
    };

    struct PendingPipeStat {
        String name;

        std::uint32_t stat_index = 0u;
    };

    Array<PendingTimestamp> pending_timestamps;

    Array<PendingPipeStat> pending_stats;

    std::array<rhi::ClockCalibration, 3> calibrations{};
};

struct ExecutorState {
    rhi::Device* device = nullptr;

    Array<TransientHeap> heaps;

    Array<CachedPlacedObject> placed_cache;

    std::uint64_t sequence = 0u;

    std::uint64_t last_peak_aliased_bytes = 0u;

    std::uint64_t last_unaliased_bytes = 0u;

    std::array<CommandSlice, command_ring_size> slices{};

    std::uint32_t slice_cursor = 0u;

    nri::QueryPool* timestamp_pool = nullptr;

    nri::Buffer* timestamp_readback = nullptr;

    nri::QueryPool* stats_pool = nullptr;

    nri::Buffer* stats_readback = nullptr;

    std::uint32_t stats_stride = 0u;

    double cpu_qpc_freq = 10000000.0;

    FrameGpuProfile latest_profile{};

    rhi::FencePoint prev_frame_fences{};

    std::array<std::array<std::uint64_t, 3>, 3> satisfied_waits{};

    std::uint8_t prev_frame_queue_mask = 0u;

    bool has_prev_frame = false;

    bool closed = false;

    void retire_placed(CachedPlacedObject& entry, const rhi::FencePoint& fence) {
        auto& reclaimer = rhi::BackendAccess::reclaimer(*device);
        if (entry.srv || entry.srv_bindless != rhi::bindless_invalid_index) {
            reclaimer.retire({.fence = fence, .descriptor = entry.srv, .bindless_index = entry.srv_bindless});
            entry.srv = nullptr;
            entry.srv_bindless = rhi::bindless_invalid_index;
        }
        if (entry.uav || entry.uav_bindless != rhi::bindless_invalid_index) {
            reclaimer.retire({.fence = fence, .descriptor = entry.uav, .bindless_index = entry.uav_bindless});
            entry.uav = nullptr;
            entry.uav_bindless = rhi::bindless_invalid_index;
        }
        for (std::size_t m = 0u; m < entry.mip_srvs.size(); ++m) {
            const std::uint32_t idx =
                (m < entry.mip_srv_bindless.size()) ? entry.mip_srv_bindless[m] : rhi::bindless_invalid_index;
            if (entry.mip_srvs[m] || idx != rhi::bindless_invalid_index)
                reclaimer.retire({.fence = fence, .descriptor = entry.mip_srvs[m], .bindless_index = idx});
        }
        entry.mip_srvs.clear();
        entry.mip_srv_bindless.clear();
        for (std::size_t m = 0u; m < entry.mip_uavs.size(); ++m) {
            // Mip 0 UAV shares `entry.uav` / `entry.uav_bindless`, so `mip_uavs[0]` is nullptr!
            const std::uint32_t idx =
                (m > 0u && m < entry.mip_uav_bindless.size()) ? entry.mip_uav_bindless[m] : rhi::bindless_invalid_index;
            if (entry.mip_uavs[m] || idx != rhi::bindless_invalid_index)
                reclaimer.retire({.fence = fence, .descriptor = entry.mip_uavs[m], .bindless_index = idx});
        }
        entry.mip_uavs.clear();
        entry.mip_uav_bindless.clear();
        for (nri::Descriptor** host_desc : {&entry.rtv, &entry.dsv, &entry.dsv_readonly}) {
            if (*host_desc) {
                reclaimer.retire({.fence = fence, .descriptor = *host_desc});
                *host_desc = nullptr;
            }
        }
        if (entry.texture || entry.buffer) {
            reclaimer.retire({.fence = fence, .buffer = entry.buffer, .texture = entry.texture});
            entry.texture = nullptr;
            entry.buffer = nullptr;
        }
    }

    void release_transients() noexcept {
        if (!device || closed || !*device)
            return;
        auto& timeline = rhi::BackendAccess::timeline(*device);
        const rhi::FencePoint fence = timeline.current_point();
        auto& reclaimer = rhi::BackendAccess::reclaimer(*device);
        for (CachedPlacedObject& obj : placed_cache)
            retire_placed(obj, fence);
        placed_cache.clear();
        for (TransientHeap& heap : heaps) {
            if (heap.memory) {
                reclaimer.retire({.fence = fence, .memory = heap.memory});
                heap.memory = nullptr;
            }
        }
        heaps.clear();
        static_cast<void>(reclaimer.reclaim(timeline, rhi::BackendAccess::bindless(*device),
                                            rhi::BackendAccess::core(*device),
                                            &rhi::BackendAccess::ray_tracing(*device)));
        last_peak_aliased_bytes = 0u;
        last_unaliased_bytes = 0u;
    }

    [[nodiscard]] rhi::Result<void> close() {
        if (closed || !device) {
            closed = true;
            device = nullptr;
            return {};
        }
        if (!*device) {
            placed_cache.clear();
            heaps.clear();
            prev_frame_fences = {};
            satisfied_waits = {};
            prev_frame_queue_mask = 0u;
            has_prev_frame = false;
            closed = true;
            device = nullptr;
            return {};
        }
        const auto idle = device->wait_idle();
        if (!idle && idle.error().code != rhi::ErrorCode::device_lost && idle.error().code != rhi::ErrorCode::closed)
            return std::unexpected(idle.error());

        release_transients();
        prev_frame_fences = {};
        satisfied_waits = {};
        prev_frame_queue_mask = 0u;
        has_prev_frame = false;
        const auto& core = rhi::BackendAccess::core(*device);
        for (CommandSlice& slice : slices) {
            for (auto& pool : slice.queues) {
                for (nri::CommandBuffer* cb : pool.buffers) {
                    if (cb)
                        core.DestroyCommandBuffer(cb);
                }
                pool.buffers.clear();
                if (pool.allocator) {
                    core.DestroyCommandAllocator(pool.allocator);
                    pool.allocator = nullptr;
                }
            }
        }
        if (timestamp_readback) {
            core.DestroyBuffer(timestamp_readback);
            timestamp_readback = nullptr;
        }
        if (timestamp_pool) {
            core.DestroyQueryPool(timestamp_pool);
            timestamp_pool = nullptr;
        }
        if (stats_readback) {
            core.DestroyBuffer(stats_readback);
            stats_readback = nullptr;
        }
        if (stats_pool) {
            core.DestroyQueryPool(stats_pool);
            stats_pool = nullptr;
        }
        auto& timeline = rhi::BackendAccess::timeline(*device);
        auto& bindless = rhi::BackendAccess::bindless(*device);
        const auto& rt = rhi::BackendAccess::ray_tracing(*device);
        auto& reclaimer = rhi::BackendAccess::reclaimer(*device);
        if (!idle) {
            reclaimer.force_release_all(bindless, core, &rt);
            closed = true;
            device = nullptr;
            return std::unexpected(idle.error());
        }
        const auto rec_res = reclaimer.reclaim(timeline, bindless, core, &rt);
        closed = true;
        device = nullptr;
        return rec_res;
    }
};

} // namespace detail

namespace {

[[nodiscard]] nri::TextureDesc make_nri_texture_desc(const TextureDesc& desc, std::uint8_t queue_mask,
                                                     bool simultaneous) noexcept {
    nri::TextureDesc out{};
    out.type = desc.type;
    out.usage = desc.usage;
    out.format = desc.format;
    out.width = static_cast<nri::Dim_t>(std::clamp<std::uint32_t>(desc.width, 1u, 16384u));
    out.height = static_cast<nri::Dim_t>(std::clamp<std::uint32_t>(desc.height, 1u, 16384u));
    out.depth = static_cast<nri::Dim_t>(std::clamp<std::uint32_t>(desc.depth, 1u, 2048u));
    out.mipNum = std::max<nri::Dim_t>(1u, desc.mip_count);
    out.layerNum = std::max<nri::Dim_t>(1u, desc.layer_count);
    out.sampleNum = std::max<nri::Sample_t>(1u, desc.sample_count);
    // Single-queue transient textures use EXCLUSIVE sharing to preserve DCC/HTILE compression unless SIMULTANEOUS is needed.
    const bool single_queue = (queue_mask != 0u) && ((queue_mask & (queue_mask - 1u)) == 0u);
    out.sharingMode = simultaneous   ? nri::SharingMode::SIMULTANEOUS
                      : single_queue ? nri::SharingMode::EXCLUSIVE
                                     : nri::SharingMode::CONCURRENT;
    out.optimizedClearValue = desc.clear_value;
    return out;
}

[[nodiscard]] nri::BufferDesc make_nri_buffer_desc(const BufferDesc& desc) noexcept {
    nri::BufferDesc out{};
    const std::uint64_t stride = (!desc.byte_address && desc.structure_stride > 0u) ? desc.structure_stride : 4u;
    const std::uint64_t min_bytes = std::max<std::uint64_t>(16u, stride);
    out.size = align_up_u64(std::max<std::uint64_t>(min_bytes, desc.size), stride);
    out.structureStride = desc.byte_address ? 0u : desc.structure_stride;
    out.usage = desc.usage;
    out.byteAddress = desc.byte_address;
    return out;
}

[[nodiscard]] bool same_clear_value(const nri::TextureDesc& a, const nri::TextureDesc& b) noexcept {
    constexpr nri::TextureUsageBits clear_usages =
        nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
    if ((a.usage & clear_usages) == 0)
        return true;
    return std::memcmp(&a.optimizedClearValue, &b.optimizedClearValue, sizeof(nri::ClearValue)) == 0;
}

[[nodiscard]] bool same_tex_desc(const nri::TextureDesc& a, const nri::TextureDesc& b) noexcept {
    return a.type == b.type && a.usage == b.usage && a.format == b.format && a.width == b.width &&
           a.height == b.height && a.depth == b.depth && a.mipNum == b.mipNum && a.layerNum == b.layerNum &&
           a.sampleNum == b.sampleNum && a.sharingMode == b.sharingMode && same_clear_value(a, b);
}

[[nodiscard]] bool same_buf_desc(const nri::BufferDesc& a, const nri::BufferDesc& b) noexcept {
    return a.size == b.size && a.structureStride == b.structureStride && a.usage == b.usage &&
           a.byteAddress == b.byteAddress;
}

[[nodiscard]] std::uint64_t hash_placed_tex(std::uint32_t heap_idx, std::uint64_t heap_gen, std::uint64_t offset,
                                            const nri::TextureDesc& d) noexcept {
    std::uint64_t h = hash_mix(0x100000001b3uLL, heap_idx);
    h = hash_mix(h, heap_gen);
    h = hash_mix(h, offset);
    h = hash_mix(h, (static_cast<std::uint64_t>(d.type) << 48u) | (static_cast<std::uint64_t>(d.usage) << 32u) |
                        (static_cast<std::uint64_t>(d.format) << 16u) | static_cast<std::uint64_t>(d.sharingMode));
    h = hash_mix(h, (static_cast<std::uint64_t>(d.width) << 32u) | (static_cast<std::uint64_t>(d.height) << 16u) |
                        static_cast<std::uint64_t>(d.depth));
    h = hash_mix(h, (static_cast<std::uint64_t>(d.mipNum) << 32u) | (static_cast<std::uint64_t>(d.layerNum) << 16u) |
                        static_cast<std::uint64_t>(d.sampleNum));
    constexpr nri::TextureUsageBits clear_usages =
        nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
    if ((d.usage & clear_usages) != 0) {
        std::uint64_t c0 = 0u;
        std::uint64_t c1 = 0u;
        std::memcpy(&c0, &d.optimizedClearValue, sizeof(c0));
        std::memcpy(&c1, reinterpret_cast<const std::byte*>(&d.optimizedClearValue) + sizeof(c0), sizeof(c1));
        h = hash_mix(hash_mix(h, c0), c1);
    }
    return h;
}

[[nodiscard]] std::uint64_t hash_placed_buf(std::uint32_t heap_idx, std::uint64_t heap_gen, std::uint64_t offset,
                                            const nri::BufferDesc& d) noexcept {
    std::uint64_t h = hash_mix(0x200000002b7uLL, heap_idx);
    h = hash_mix(h, heap_gen);
    h = hash_mix(h, offset);
    h = hash_mix(h, d.size);
    h = hash_mix(h, (static_cast<std::uint64_t>(d.structureStride) << 32u) |
                        (static_cast<std::uint64_t>(d.usage) << 8u) | static_cast<std::uint64_t>(d.byteAddress));
    return h;
}

void harvest_completed_slice(detail::ExecutorState& state, detail::CommandSlice& slice) {
    const bool want_ts = slice.has_queries && state.timestamp_readback != nullptr;
    const bool want_stats =
        slice.has_pipeline_stats && state.stats_readback != nullptr && state.stats_stride >= sizeof(std::uint64_t) * 9u;
    if (!want_ts && !want_stats) {
        slice.has_queries = false;
        slice.has_pipeline_stats = false;
        slice.pending_timestamps.clear();
        slice.pending_stats.clear();
        return;
    }

    const auto& core = rhi::BackendAccess::core(*state.device);
    const std::uint32_t slice_idx = static_cast<std::uint32_t>(&slice - state.slices.data());

    FrameGpuProfile profile{};
    profile.frame_id = slice.frame_id;
    profile.sequence = slice.sequence;

    if (want_ts && !slice.pending_timestamps.empty()) {
        const std::uint32_t query_base = slice_idx * (detail::max_timed_passes_per_frame * 2u);
        const auto* raw_ts = static_cast<const std::uint64_t*>(
            core.MapBuffer(*state.timestamp_readback, query_base * sizeof(std::uint64_t),
                           slice.pending_timestamps.size() * 2u * sizeof(std::uint64_t)));
        if (raw_ts) {
            profile.valid = true;
            profile.passes.reserve(slice.pending_timestamps.size());

            const rhi::ClockCalibration& ref_cal = slice.calibrations[0];
            const double cpu_freq = state.cpu_qpc_freq;

            auto to_unified_ns = [&](rhi::QueueKind q, std::uint64_t ticks) noexcept -> std::uint64_t {
                const auto q_idx = static_cast<std::size_t>(q);
                const rhi::ClockCalibration& cal = slice.calibrations[q_idx];
                if (cal.gpu_frequency == 0u)
                    return 0u;
                const double delta_gpu_ticks = static_cast<double>(static_cast<std::int64_t>(ticks) -
                                                                   static_cast<std::int64_t>(cal.gpu_timestamp));
                const double delta_ns = (delta_gpu_ticks * 1.0e9) / static_cast<double>(cal.gpu_frequency);
                const double cpu_delta_ns = (static_cast<double>(static_cast<std::int64_t>(cal.cpu_counter) -
                                                                 static_cast<std::int64_t>(ref_cal.cpu_counter)) *
                                             1.0e9) /
                                            cpu_freq;
                const double shifted = 1.0e12 + cpu_delta_ns + delta_ns;
                return shifted > 0.0 ? static_cast<std::uint64_t>(shifted) : 0u;
            };

            Array<std::pair<std::uint64_t, std::uint64_t>> intervals;
            intervals.reserve(slice.pending_timestamps.size());

            for (std::size_t i = 0u; i < slice.pending_timestamps.size(); ++i) {
                auto& item = slice.pending_timestamps[i];
                const std::uint64_t t0 = raw_ts[i * 2u];
                const std::uint64_t t1 = raw_ts[i * 2u + 1u];
                const auto q_idx = static_cast<std::size_t>(item.queue);
                const std::uint64_t freq = slice.calibrations[q_idx].gpu_frequency;
                double ms = 0.0;
                if (freq > 0u && t1 >= t0)
                    ms = (static_cast<double>(t1 - t0) * 1000.0) / static_cast<double>(freq);
                const std::uint64_t ns0 = to_unified_ns(item.queue, t0);
                const std::uint64_t ns1 = std::max(ns0, to_unified_ns(item.queue, t1));
                profile.passes.add(PassTimingSample{std::move(item.name), item.queue, ms, ns0, ns1});
                if (ns1 > ns0)
                    intervals.emplace(ns0, ns1);
            }
            core.UnmapBuffer(*state.timestamp_readback);

            if (!intervals.empty()) {
                std::sort(intervals.begin(), intervals.end());
                std::uint64_t union_ns = 0u;
                std::uint64_t cur_start = intervals[0].first;
                std::uint64_t cur_end = intervals[0].second;
                for (std::size_t i = 1u; i < intervals.size(); ++i) {
                    if (intervals[i].first <= cur_end) {
                        cur_end = std::max(cur_end, intervals[i].second);
                    } else {
                        union_ns += (cur_end - cur_start);
                        cur_start = intervals[i].first;
                        cur_end = intervals[i].second;
                    }
                }
                union_ns += (cur_end - cur_start);
                profile.union_gpu_ms = static_cast<double>(union_ns) / 1.0e6;
            }
        }
    }

    if (want_stats && !slice.pending_stats.empty()) {
        const std::uint64_t byte_base =
            static_cast<std::uint64_t>(slice_idx) * detail::max_timed_passes_per_frame * state.stats_stride;
        const auto* raw_bytes = static_cast<const std::byte*>(
            core.MapBuffer(*state.stats_readback, byte_base,
                           slice.pending_stats.size() * static_cast<std::size_t>(state.stats_stride)));
        if (raw_bytes) {
            profile.valid = true;
            profile.pipeline_stats.reserve(slice.pending_stats.size());
            for (auto& item : slice.pending_stats) {
                nri::PipelineStatisticsDesc stat{};
                std::memcpy(&stat, raw_bytes + item.stat_index * state.stats_stride,
                            std::min<std::size_t>(sizeof(stat), state.stats_stride));
                PipelineStatsSample out{};
                out.pass_name = std::move(item.name);
                out.input_vertices = stat.inputVertexNum;
                out.input_primitives = stat.inputPrimitiveNum;
                out.vs_invocations = stat.vertexShaderInvocationNum;
                out.clipper_primitives = stat.rasterizerOutPrimitiveNum;
                out.ps_invocations = stat.fragmentShaderInvocationNum;
                out.cs_invocations = stat.computeShaderInvocationNum;
                out.task_invocations = stat.taskShaderInvocationNum;
                out.mesh_invocations = stat.meshShaderInvocationNum;
                out.mesh_primitives = stat.meshShaderPrimitiveNum;
                profile.pipeline_stats.add(std::move(out));
            }
            core.UnmapBuffer(*state.stats_readback);
        }
    }

    if (profile.valid)
        state.latest_profile = std::move(profile);
    slice.has_queries = false;
    slice.has_pipeline_stats = false;
    slice.pending_timestamps.clear();
    slice.pending_stats.clear();
}

[[nodiscard]] nri::AccessBits sanitize_d3d12_access(nri::AccessBits access, rhi::QueueKind resolved_queue) noexcept {
    if (resolved_queue == rhi::QueueKind::graphics)
        return access;
    if (resolved_queue == rhi::QueueKind::copy)
        return static_cast<nri::AccessBits>(access &
                                            (nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION));
    constexpr nri::AccessBits graphics_only_access =
        nri::AccessBits::INDEX_BUFFER | nri::AccessBits::VERTEX_BUFFER | nri::AccessBits::COLOR_ATTACHMENT |
        nri::AccessBits::DEPTH_STENCIL_ATTACHMENT | nri::AccessBits::SHADING_RATE_ATTACHMENT |
        nri::AccessBits::INPUT_ATTACHMENT | nri::AccessBits::RESOLVE_SOURCE | nri::AccessBits::RESOLVE_DESTINATION |
        nri::AccessBits::RESOLVE_ATTACHMENT_SOURCE | nri::AccessBits::RESOLVE_ATTACHMENT_DESTINATION;
    return static_cast<nri::AccessBits>(access & ~graphics_only_access);
}

[[nodiscard]] nri::StageBits sanitize_d3d12_stages(nri::StageBits stages, nri::AccessBits access,
                                                   rhi::QueueKind resolved_queue) noexcept {
    if (stages == nri::StageBits::NONE)
        return nri::StageBits::NONE;
    if (resolved_queue == rhi::QueueKind::copy)
        return (access != nri::AccessBits::NONE) ? nri::StageBits::COPY : nri::StageBits::NONE;
    if (stages == nri::StageBits::ALL)
        return nri::StageBits::ALL;
    if (access == nri::AccessBits::NONE)
        return nri::StageBits::NONE;

    if (resolved_queue == rhi::QueueKind::compute) {
        constexpr nri::StageBits compute_allowed =
            nri::StageBits::COMPUTE_SHADER | nri::StageBits::RAY_TRACING_SHADERS | nri::StageBits::COPY |
            nri::StageBits::CLEAR_STORAGE | nri::StageBits::ACCELERATION_STRUCTURE | nri::StageBits::MICROMAP |
            nri::StageBits::INDIRECT;
        stages = filter_stages(stages, compute_allowed, nri::StageBits::ALL);
        if (stages == nri::StageBits::NONE)
            return nri::StageBits::ALL;
        if (stages == nri::StageBits::ALL)
            return nri::StageBits::ALL;
    }

    // D3D12 Enhanced Barriers enforce strict Sync-to-Access compatibility; widen incompatible combinations to StageBits::ALL.
    unsigned categories = 0u;
    constexpr nri::StageBits vertex_shading_stages = nri::StageBits::VERTEX_SHADER |
                                                     nri::StageBits::TESSELLATION_SHADERS |
                                                     nri::StageBits::GEOMETRY_SHADER | nri::StageBits::MESH_SHADERS;
    constexpr nri::StageBits as_build_stages = nri::StageBits::ACCELERATION_STRUCTURE | nri::StageBits::MICROMAP;
    constexpr nri::AccessBits pure_shader_acc =
        nri::AccessBits::SHADER_RESOURCE | nri::AccessBits::SHADER_RESOURCE_STORAGE | nri::AccessBits::CONSTANT_BUFFER |
        nri::AccessBits::SHADER_BINDING_TABLE | nri::AccessBits::INPUT_ATTACHMENT;

    if ((access & nri::AccessBits::VERTEX_BUFFER) != 0) {
        if (filter_stages(stages, vertex_shading_stages, vertex_shading_stages) != stages)
            return nri::StageBits::ALL;
        ++categories;
    }
    if ((access & nri::AccessBits::SCRATCH_BUFFER) != 0) {
        if (filter_stages(stages, as_build_stages, as_build_stages) != stages)
            return nri::StageBits::ALL;
        ++categories;
    }
    if ((access & nri::AccessBits::SHADER_BINDING_TABLE) != 0 &&
        filter_stages(stages, nri::StageBits::RAY_TRACING_SHADERS, nri::StageBits::RAY_TRACING_SHADERS) != stages) {
        return nri::StageBits::ALL;
    }
    if ((access & nri::AccessBits::INPUT_ATTACHMENT) != 0 &&
        filter_stages(stages, nri::StageBits::FRAGMENT_SHADER, nri::StageBits::FRAGMENT_SHADER) != stages) {
        return nri::StageBits::ALL;
    }
    if ((access & pure_shader_acc) != 0 || has_any_stage(stages, nri::StageBits::ALL_SHADERS)) {
        if ((access & pure_shader_acc) != 0) {
            constexpr nri::StageBits allowed_for_pure_shader =
                nri::StageBits::ALL_SHADERS | as_build_stages | nri::StageBits::CLEAR_STORAGE;
            if (filter_stages(stages, allowed_for_pure_shader, allowed_for_pure_shader) != stages)
                return nri::StageBits::ALL;
            if (has_any_stage(stages, as_build_stages) &&
                (access & (nri::AccessBits::CONSTANT_BUFFER | nri::AccessBits::SHADER_BINDING_TABLE |
                           nri::AccessBits::INPUT_ATTACHMENT | nri::AccessBits::SHADER_RESOURCE_STORAGE)) != 0) {
                return nri::StageBits::ALL;
            }
            if (has_any_stage(stages, nri::StageBits::CLEAR_STORAGE) &&
                (access & (nri::AccessBits::SHADER_RESOURCE | nri::AccessBits::CONSTANT_BUFFER |
                           nri::AccessBits::SHADER_BINDING_TABLE | nri::AccessBits::INPUT_ATTACHMENT)) != 0) {
                return nri::StageBits::ALL;
            }
        }
        if ((access & nri::AccessBits::VERTEX_BUFFER) == 0)
            ++categories;
    }
    if ((access & nri::AccessBits::SHADING_RATE_ATTACHMENT) != 0 ||
        has_any_stage(stages, nri::StageBits::SHADING_RATE_ATTACHMENT)) {
        if ((access & nri::AccessBits::SHADING_RATE_ATTACHMENT) != 0 &&
            filter_stages(stages, nri::StageBits::SHADING_RATE_ATTACHMENT, nri::StageBits::SHADING_RATE_ATTACHMENT) !=
                stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    if ((access & nri::AccessBits::INDEX_BUFFER) != 0 || has_any_stage(stages, nri::StageBits::INDEX_INPUT)) {
        if ((access & nri::AccessBits::INDEX_BUFFER) != 0 &&
            filter_stages(stages, nri::StageBits::INDEX_INPUT, nri::StageBits::INDEX_INPUT) != stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    if ((access & nri::AccessBits::ARGUMENT_BUFFER) != 0 || has_any_stage(stages, nri::StageBits::INDIRECT)) {
        if ((access & nri::AccessBits::ARGUMENT_BUFFER) != 0 &&
            filter_stages(stages, nri::StageBits::INDIRECT, nri::StageBits::INDIRECT) != stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    constexpr nri::AccessBits copy_acc = nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION |
                                         nri::AccessBits::HOST_READ | nri::AccessBits::HOST_WRITE;
    if ((access & copy_acc) != 0 || has_any_stage(stages, nri::StageBits::COPY)) {
        if ((access & copy_acc) != 0 && filter_stages(stages, nri::StageBits::COPY, nri::StageBits::COPY) != stages)
            return nri::StageBits::ALL;
        ++categories;
    }
    if ((access & nri::AccessBits::CLEAR_STORAGE) != 0 || has_any_stage(stages, nri::StageBits::CLEAR_STORAGE)) {
        if ((access & nri::AccessBits::CLEAR_STORAGE) != 0 &&
            filter_stages(stages, nri::StageBits::CLEAR_STORAGE, nri::StageBits::CLEAR_STORAGE) != stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    if ((access & nri::AccessBits::COLOR_ATTACHMENT) != 0 || has_any_stage(stages, nri::StageBits::COLOR_ATTACHMENT)) {
        if ((access & nri::AccessBits::COLOR_ATTACHMENT) != 0 &&
            filter_stages(stages, nri::StageBits::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT) != stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    if ((access & nri::AccessBits::DEPTH_STENCIL_ATTACHMENT) != 0 ||
        has_any_stage(stages, nri::StageBits::DEPTH_STENCIL_ATTACHMENT)) {
        if ((access & nri::AccessBits::DEPTH_STENCIL_ATTACHMENT) != 0 &&
            filter_stages(stages, nri::StageBits::DEPTH_STENCIL_ATTACHMENT, nri::StageBits::DEPTH_STENCIL_ATTACHMENT) !=
                stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    constexpr nri::AccessBits resolve_acc = nri::AccessBits::RESOLVE_SOURCE | nri::AccessBits::RESOLVE_DESTINATION |
                                            nri::AccessBits::RESOLVE_ATTACHMENT_SOURCE |
                                            nri::AccessBits::RESOLVE_ATTACHMENT_DESTINATION;
    if ((access & resolve_acc) != 0 || has_any_stage(stages, nri::StageBits::ALL_RESOLVE)) {
        if ((access & resolve_acc) != 0 &&
            filter_stages(stages, nri::StageBits::ALL_RESOLVE, nri::StageBits::ALL_RESOLVE) != stages) {
            return nri::StageBits::ALL;
        }
        ++categories;
    }
    if ((access & (nri::AccessBits::ACCELERATION_STRUCTURE_READ | nri::AccessBits::ACCELERATION_STRUCTURE_WRITE |
                   nri::AccessBits::MICROMAP_READ | nri::AccessBits::MICROMAP_WRITE)) != 0) {
        if (categories > 0u)
            return nri::StageBits::ALL;
        if ((access & (nri::AccessBits::ACCELERATION_STRUCTURE_WRITE | nri::AccessBits::MICROMAP_WRITE)) != 0)
            return nri::StageBits::ACCELERATION_STRUCTURE;
        constexpr nri::StageBits valid_as_read = nri::StageBits::COMPUTE_SHADER | nri::StageBits::RAY_TRACING_SHADERS |
                                                 nri::StageBits::ACCELERATION_STRUCTURE;
        const nri::StageBits filtered = filter_stages(stages, valid_as_read, nri::StageBits::ALL);
        return (filtered != nri::StageBits::NONE) ? filtered : nri::StageBits::COMPUTE_SHADER;
    }

    if (categories > 1u)
        return nri::StageBits::ALL;
    return stages;
}

[[nodiscard]] rhi::Result<nri::CommandBuffer*>
acquire_command_buffer(detail::ExecutorState& state, detail::CommandSlice& slice, rhi::QueueKind queue) {
    auto& pool = slice.queues[static_cast<std::size_t>(queue)];
    const auto& core = rhi::BackendAccess::core(*state.device);
    const rhi::QueueKind resolved = rhi::BackendAccess::timeline(*state.device).resolve_kind(queue);
    if (!pool.allocator) {
        nri::Queue* nri_queue = rhi::BackendAccess::timeline(*state.device).queue(resolved);
        ATELIER_TRY(check_nri(core.CreateCommandAllocator(*nri_queue, pool.allocator), rhi::ErrorCode::creation_failed,
                              "RDG CreateCommandAllocator failed"));
    }
    if (pool.used >= pool.buffers.size()) {
        nri::CommandBuffer* cb = nullptr;
        ATELIER_TRY(check_nri(core.CreateCommandBuffer(*pool.allocator, cb), rhi::ErrorCode::creation_failed,
                              "RDG CreateCommandBuffer failed"));
        pool.buffers.add(cb);
    }
    nri::CommandBuffer* cb = pool.buffers[pool.used++];
    ATELIER_TRY(check_nri(core.BeginCommandBuffer(*cb, nullptr), rhi::ErrorCode::execution_failed,
                          "RDG BeginCommandBuffer failed"));
    if (resolved != rhi::QueueKind::copy) {
        rhi::BackendAccess::bindless(*state.device)
            .bind(*cb, core, rhi::BackendAccess::descriptor_heap(*state.device), resolved);
    }
    return cb;
}

} // namespace

// ============================================================================
// Executor Public Implementation
// ============================================================================

Executor::Executor() noexcept = default;

Executor::Executor(std::unique_ptr<detail::ExecutorState> state) noexcept : state_(std::move(state)) {}

Executor::~Executor() {
    if (state_ && !state_->closed) {
        if (auto res = state_->close(); !res && res.error().code != rhi::ErrorCode::device_lost)
            atelier::fatal("Executor destroyed without completing in-flight GPU work.");
    }
}

Executor::Executor(Executor&& other) noexcept = default;

Executor& Executor::operator=(Executor&& other) noexcept {
    if (this != &other) {
        if (state_ && !state_->closed) {
            if (auto res = state_->close(); !res && res.error().code != rhi::ErrorCode::device_lost)
                atelier::fatal("Executor overwritten without completing in-flight GPU work.");
        }
        state_ = std::move(other.state_);
    }
    return *this;
}

rhi::Result<Executor> Executor::open(rhi::Device& device) {
    if (!device)
        return std::unexpected(rhi::Error{rhi::ErrorCode::invalid_argument, 0, "Executor::open requires live Device"});
    auto state = std::make_unique<detail::ExecutorState>();
    state->device = &device;

    LARGE_INTEGER qpc_freq_li{};
    if (QueryPerformanceFrequency(&qpc_freq_li) && qpc_freq_li.QuadPart > 0)
        state->cpu_qpc_freq = static_cast<double>(qpc_freq_li.QuadPart);

    const auto& core = rhi::BackendAccess::core(device);
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);
    const nri::DeviceDesc& dev_desc = core.GetDeviceDesc(nri_dev);

    if (dev_desc.features.timestamp) {
        nri::QueryPoolDesc q_desc{};
        q_desc.queryType = nri::QueryType::TIMESTAMP;
        q_desc.capacity = detail::command_ring_size * detail::max_timed_passes_per_frame * 2u;
        if (core.CreateQueryPool(nri_dev, q_desc, state->timestamp_pool) == nri::Result::SUCCESS) {
            nri::BufferDesc rb_desc{};
            rb_desc.size = static_cast<std::uint64_t>(q_desc.capacity) * sizeof(std::uint64_t);
            if (core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::HOST_READBACK, 0.0f, rb_desc,
                                           state->timestamp_readback) != nri::Result::SUCCESS) {
                core.DestroyQueryPool(state->timestamp_pool);
                state->timestamp_pool = nullptr;
            }
        }
    }

    if (dev_desc.features.pipelineStatistics) {
        nri::QueryPoolDesc s_desc{};
        s_desc.queryType = nri::QueryType::PIPELINE_STATISTICS;
        s_desc.capacity = detail::command_ring_size * detail::max_timed_passes_per_frame;
        if (core.CreateQueryPool(nri_dev, s_desc, state->stats_pool) == nri::Result::SUCCESS) {
            state->stats_stride = core.GetQuerySize(*state->stats_pool);
            nri::BufferDesc rb_desc{};
            rb_desc.size = static_cast<std::uint64_t>(s_desc.capacity) * state->stats_stride;
            if (core.CreateCommittedBuffer(nri_dev, nri::MemoryLocation::HOST_READBACK, 0.0f, rb_desc,
                                           state->stats_readback) != nri::Result::SUCCESS) {
                core.DestroyQueryPool(state->stats_pool);
                state->stats_pool = nullptr;
                state->stats_stride = 0u;
            }
        }
    }

    return Executor(std::move(state));
}

void Executor::release_transients() noexcept {
    if (state_)
        state_->release_transients();
}

const FrameGpuProfile& Executor::latest_gpu_profile() const noexcept {
    static const FrameGpuProfile empty{};
    return state_ ? state_->latest_profile : empty;
}

TransientStats Executor::transient_stats() const noexcept {
    if (!state_)
        return {};
    TransientStats stats{};
    for (const auto& heap : state_->heaps) {
        stats.allocated_heap_bytes += heap.capacity;
        if (heap.memory)
            ++stats.active_heaps;
    }
    stats.peak_frame_aliased_bytes = state_->last_peak_aliased_bytes;
    stats.unaliased_frame_bytes = state_->last_unaliased_bytes;
    stats.cached_placed_resources = static_cast<std::uint32_t>(state_->placed_cache.size());
    return stats;
}

rhi::Result<void> Executor::close() {
    if (!state_)
        return {};
    return state_->close();
}

rhi::Result<rhi::FencePoint> Executor::execute(Graph& graph, const ExecuteOptions& options) {
    if (!state_ || state_->closed || !state_->device || !*state_->device)
        return std::unexpected(rhi::Error{rhi::ErrorCode::closed, 0, "RDG Executor is closed"});

    auto& state = *state_;
    rhi::Device& device = *state.device;
    nri::Device& nri_dev = rhi::BackendAccess::nri_device(device);
    const auto& core = rhi::BackendAccess::core(device);
    const auto& mesh_iface = rhi::BackendAccess::mesh_shader(device);
    auto& bindless = rhi::BackendAccess::bindless(device);
    auto& timeline = rhi::BackendAccess::timeline(device);
    auto& reclaimer = rhi::BackendAccess::reclaimer(device);
    const nri::DeviceDesc& dev_desc = core.GetDeviceDesc(nri_dev);

    const std::uint64_t current_seq = ++state.sequence;
    if (options.trace) {
        *options.trace = TraceSnapshot{};
        options.trace->recording_sequence = current_seq;
        options.trace->requested_scope = String::from_utf8(options.trace_scope).value_or(""_s);
        options.trace->entered = true;
    }

    // 1. Rotate command slice, wait for its previous FencePoint, reset allocators, harvest queries, reclaim retired objects.
    detail::CommandSlice& slice = state.slices[state.slice_cursor];
    state.slice_cursor = (state.slice_cursor + 1u) % detail::command_ring_size;
    ATELIER_TRY(timeline.wait_all(slice.fence, device.wait_timeout_ms()));
    harvest_completed_slice(state, slice);
    for (auto& pool : slice.queues) {
        if (pool.allocator)
            core.ResetCommandAllocator(*pool.allocator);
        pool.used = 0u;
    }
    const auto& rt_iface = rhi::BackendAccess::ray_tracing(device);
    ATELIER_TRY(reclaimer.reclaim(timeline, bindless, core, &rt_iface));

    // Cache imported texture format & mip count once, and initialize per-mip tracking arrays on Graph's LinearArena.
    for (auto* tex : graph.textures_) {
        if (tex->imported && tex->runtime_texture) {
            const nri::TextureDesc& td = core.GetTextureDesc(*tex->runtime_texture);
            if (tex->resolved_format == nri::Format::UNKNOWN)
                tex->resolved_format = td.format;
            tex->resolved_mip_count = std::max<std::uint16_t>(1u, td.mipNum);
        } else {
            tex->resolved_mip_count = std::max<std::uint16_t>(1u, tex->desc.mip_count);
        }
        tex->mip_states = graph.arena_.allocate<MipTrackingState>(tex->resolved_mip_count);
        for (std::uint16_t m = 0u; m < tex->resolved_mip_count; ++m)
            tex->mip_states[m] = MipTrackingState{tex->current_state, tex->current_queue};
    }

    // 2. Resolve hardware queues & run backward Dead-Pass Culling.
    for (auto* pass : graph.passes_)
        pass->resolved_queue = timeline.resolve_kind(pass->requested_queue);

    for (auto* tex : graph.textures_)
        tex->needed = tex->imported && tex->exported;
    for (auto* buf : graph.buffers_)
        buf->needed = buf->imported && buf->exported;

    for (std::size_t idx = graph.passes_.size(); idx > 0u; --idx) {
        auto& pass = *graph.passes_[idx - 1u];
        bool live = pass.side_effect;
        for (auto* acc = pass.first_access; acc != nullptr; acc = acc->next) {
            if (!acc->is_write)
                continue;
            if (acc->is_texture) {
                auto& tex = *graph.textures_[acc->index];
                if (tex.imported || tex.exported || tex.needed)
                    live = true;
            } else {
                auto& buf = *graph.buffers_[acc->index];
                if (buf.imported || buf.exported || buf.needed)
                    live = true;
            }
        }
        if (!live) {
            pass.culled = true;
            pass.cull_reason = "unreferenced_outputs";
            continue;
        }
        pass.culled = false;
        for (auto* acc = pass.first_access; acc != nullptr; acc = acc->next) {
            if (acc->is_texture)
                graph.textures_[acc->index]->needed = true;
            else
                graph.buffers_[acc->index]->needed = true;
        }
    }

    // 3. Compute live resource lifetime intervals [first_pass, last_pass], queue masks, and RAW/WAW/WAR DAG edges.
    struct HazardTracker {
        std::uint32_t last_writer = invalid_handle_index;

        std::array<std::uint32_t, 3> last_access_on_queue{invalid_handle_index, invalid_handle_index,
                                                          invalid_handle_index};
    };
    auto resolve_mip_bounds = [](const Graph::TextureNode& tex,
                                 SubresourceRange range) noexcept -> std::pair<std::uint16_t, std::uint16_t> {
        const std::uint16_t m0 = std::min<std::uint16_t>(range.base_mip, tex.resolved_mip_count - 1u);
        const std::uint16_t m1 =
            (range.mip_count == 0u)
                ? tex.resolved_mip_count
                : static_cast<std::uint16_t>(std::min<std::uint32_t>(
                      tex.resolved_mip_count, static_cast<std::uint32_t>(m0) + range.mip_count));
        return {m0, std::max<std::uint16_t>(static_cast<std::uint16_t>(m0 + 1u), m1)};
    };

    Array<std::uint32_t> tex_hazard_bases(graph.textures_.size(), 0u);
    std::uint32_t total_tex_mips = 0u;
    for (std::size_t ti = 0u; ti < graph.textures_.size(); ++ti) {
        tex_hazard_bases[ti] = total_tex_mips;
        total_tex_mips += graph.textures_[ti]->resolved_mip_count;
    }
    Array<HazardTracker> tex_mip_hazards(total_tex_mips);
    Array<HazardTracker> buf_hazards(graph.buffers_.size());
    Array<TraceEdge> dag_edges;

    // Cross-queue dependency matrix: pass_waits[p][q] = latest pass index on queue q that pass p depends on.
    Array<std::array<std::uint32_t, 3>> pass_waits(
        graph.passes_.size(), {invalid_handle_index, invalid_handle_index, invalid_handle_index});

    auto record_dep = [&](std::uint32_t pred, std::uint32_t dst_p, std::uint32_t res_id) {
        if (pred == invalid_handle_index || pred == dst_p)
            return;
        const rhi::QueueKind src_q = graph.passes_[pred]->resolved_queue;
        const rhi::QueueKind dst_q = graph.passes_[dst_p]->resolved_queue;
        const bool cross = (src_q != dst_q);
        dag_edges.add(TraceEdge{pred, dst_p, res_id, cross});
        if (cross) {
            auto& slot = pass_waits[dst_p][static_cast<std::size_t>(src_q)];
            slot = (slot == invalid_handle_index) ? pred : std::max(slot, pred);
        }
    };

    for (std::uint32_t p = 0u; p < static_cast<std::uint32_t>(graph.passes_.size()); ++p) {
        auto& pass = *graph.passes_[p];
        if (pass.culled)
            continue;
        const auto pq_idx = static_cast<std::size_t>(pass.resolved_queue);
        const std::uint8_t q_bit = static_cast<std::uint8_t>(1u << pq_idx);
        for (auto* acc = pass.first_access; acc != nullptr; acc = acc->next) {
            if (acc->is_texture) {
                auto& tex = *graph.textures_[acc->index];
                tex.needed = true;
                if (tex.imported && tex.runtime_texture &&
                    core.GetTextureDesc(*tex.runtime_texture).sharingMode == nri::SharingMode::SIMULTANEOUS) {
                    tex.simultaneous = true;
                }
                if (acc->layout == nri::Layout::GENERAL &&
                    (acc->access & nri::AccessBits::SHADER_RESOURCE_STORAGE) != 0) {
                    if (tex.imported && !tex.simultaneous) {
                        atelier::fatal("RDG imported texture uses Layout::GENERAL with SHADER_RESOURCE_STORAGE without "
                                       "SharingMode::SIMULTANEOUS.");
                    }
                    if (!tex.imported && ((tex.desc.usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0 ||
                                          tex.desc.sample_count > 1u)) {
                        atelier::fatal("RDG transient depth-stencil or MSAA texture cannot combine SRV and UAV in "
                                       "Layout::GENERAL.");
                    }
                    tex.simultaneous = true;
                }
                if (tex.first_pass == invalid_handle_index) {
                    tex.first_pass = p;
                    tex.first_pass_access = acc->access;
                } else if (tex.first_pass == p) {
                    tex.first_pass_access = tex.first_pass_access | acc->access;
                }
                tex.last_pass = p;
                tex.queue_mask |= q_bit;
                tex.per_queue_accesses[pq_idx] = tex.per_queue_accesses[pq_idx] | acc->access;

                const auto [m0, m1] = resolve_mip_bounds(tex, acc->range);
                const std::uint32_t hz_base = tex_hazard_bases[acc->index];
                for (std::uint16_t m = m0; m < m1; ++m) {
                    auto& hz = tex_mip_hazards[hz_base + m];
                    record_dep(hz.last_writer, p, acc->index);
                    // Writes and non-simultaneous cross-queue texture handoffs wait on prior queue users!
                    for (std::size_t q = 0u; q < 3u; ++q) {
                        if (hz.last_access_on_queue[q] != invalid_handle_index &&
                            hz.last_access_on_queue[q] != hz.last_writer &&
                            (acc->is_write || (!tex.simultaneous && q != pq_idx))) {
                            record_dep(hz.last_access_on_queue[q], p, acc->index);
                        }
                    }
                    if (acc->is_write) {
                        hz.last_writer = p;
                        hz.last_access_on_queue = {invalid_handle_index, invalid_handle_index, invalid_handle_index};
                    }
                    hz.last_access_on_queue[pq_idx] = p;
                }
            } else {
                auto& buf = *graph.buffers_[acc->index];
                buf.needed = true;
                if (buf.first_pass == invalid_handle_index) {
                    buf.first_pass = p;
                    buf.first_pass_access = acc->access;
                }
                buf.last_pass = p;
                buf.queue_mask |= q_bit;
                buf.per_queue_accesses[pq_idx] = buf.per_queue_accesses[pq_idx] | acc->access;

                const auto res_id = static_cast<std::uint32_t>(graph.textures_.size() + acc->index);
                auto& hz = buf_hazards[acc->index];
                record_dep(hz.last_writer, p, res_id);
                if (acc->is_write) {
                    for (std::size_t q = 0u; q < 3u; ++q) {
                        if (hz.last_access_on_queue[q] != invalid_handle_index &&
                            hz.last_access_on_queue[q] != hz.last_writer) {
                            record_dep(hz.last_access_on_queue[q], p, res_id);
                        }
                    }
                    hz.last_writer = p;
                    hz.last_access_on_queue = {invalid_handle_index, invalid_handle_index, invalid_handle_index};
                }
                hz.last_access_on_queue[pq_idx] = p;
            }
        }
    }

    // Compute transitive happens-before relation across queues for safe transient memory aliasing.
    const std::size_t pass_count = graph.passes_.size();
    const std::size_t word_count = (pass_count + 63u) / 64u;
    Array<std::uint64_t> ancestors(pass_count * word_count, 0u);
    std::array<std::uint32_t, 3> prev_on_queue{invalid_handle_index, invalid_handle_index, invalid_handle_index};
    for (std::uint32_t p = 0u; p < static_cast<std::uint32_t>(pass_count); ++p) {
        if (graph.passes_[p]->culled)
            continue;
        const auto q_idx = static_cast<std::size_t>(graph.passes_[p]->resolved_queue);
        auto merge_ancestors = [&](std::uint32_t pred) {
            if (pred == invalid_handle_index)
                return;
            ancestors[p * word_count + (pred / 64u)] |= (1uLL << (pred % 64u));
            for (std::size_t w = 0u; w < word_count; ++w)
                ancestors[p * word_count + w] |= ancestors[pred * word_count + w];
        };
        merge_ancestors(prev_on_queue[q_idx]);
        for (std::size_t q = 0u; q < 3u; ++q)
            merge_ancestors(pass_waits[p][q]);
        prev_on_queue[q_idx] = p;
    }

    auto happens_before = [&](std::uint32_t u, std::uint32_t v) noexcept -> bool {
        if (u >= pass_count || v >= pass_count || u >= v)
            return false;
        return (ancestors[v * word_count + (u / 64u)] & (1uLL << (u % 64u))) != 0u;
    };

    // 4. Plan Transient Memory Aliasing & Materialize Placed Resources + Cached Bindless Views.
    struct PlacedLease {
        bool is_texture = true;

        std::uint32_t node_index = 0u;

        std::uint32_t heap_index = 0u;

        std::uint64_t offset = 0u;

        std::uint64_t size = 0u;

        std::uint32_t first_pass = 0u;

        std::uint32_t last_pass = 0u;

        std::uint8_t queue_mask = 0u;

        nri::MemoryDesc mem_desc{};

        nri::TextureDesc nri_tex{};

        nri::BufferDesc nri_buf{};
    };

    Array<PlacedLease> leases;
    leases.reserve(graph.textures_.size() + graph.buffers_.size());
    std::uint64_t unaliased_sum = 0u;

    for (std::uint32_t i = 0u; i < static_cast<std::uint32_t>(graph.textures_.size()); ++i) {
        auto& tex = *graph.textures_[i];
        if (tex.imported || !tex.needed || tex.first_pass == invalid_handle_index)
            continue;
        PlacedLease lease{};
        lease.is_texture = true;
        lease.node_index = i;
        lease.first_pass = tex.first_pass;
        lease.last_pass = tex.last_pass;
        lease.queue_mask = tex.queue_mask;
        lease.nri_tex = make_nri_texture_desc(tex.desc, tex.queue_mask, tex.simultaneous);
        core.GetTextureMemoryDesc2(nri_dev, lease.nri_tex, nri::MemoryLocation::DEVICE, lease.mem_desc);
        lease.size = lease.mem_desc.size;
        tex.byte_size = lease.size;
        unaliased_sum += lease.size;
        leases.add(lease);
    }

    for (std::uint32_t i = 0u; i < static_cast<std::uint32_t>(graph.buffers_.size()); ++i) {
        auto& buf = *graph.buffers_[i];
        if (buf.imported || !buf.needed || buf.first_pass == invalid_handle_index)
            continue;
        PlacedLease lease{};
        lease.is_texture = false;
        lease.node_index = i;
        lease.first_pass = buf.first_pass;
        lease.last_pass = buf.last_pass;
        lease.queue_mask = buf.queue_mask;
        lease.nri_buf = make_nri_buffer_desc(buf.desc);
        core.GetBufferMemoryDesc2(nri_dev, lease.nri_buf, nri::MemoryLocation::DEVICE, lease.mem_desc);
        lease.size = lease.mem_desc.size;
        buf.byte_size = lease.size;
        unaliased_sum += lease.size;
        leases.add(lease);
    }

    // Sort descending by size so large GBuffer/HDR targets anchor stable base offsets across frames.
    std::sort(leases.begin(), leases.end(), [](const PlacedLease& a, const PlacedLease& b) {
        if (a.size != b.size)
            return a.size > b.size;
        return a.first_pass < b.first_pass;
    });

    const std::uint64_t granularity = std::max<std::uint64_t>(1u, dev_desc.memory.bufferTextureGranularity);
    Array<std::uint64_t> heap_watermarks(state.heaps.size(), 0u);
    Array<std::uint8_t> heap_used_queues(state.heaps.size(), 0u);
    Array<bool> heap_needs_msaa(state.heaps.size(), false);

    auto find_or_add_heap = [&](nri::MemoryType mem_type, bool dedicated, bool is_msaa,
                                std::uint64_t min_size) -> std::uint32_t {
        if (!dedicated) {
            for (std::uint32_t h = 0u; h < static_cast<std::uint32_t>(state.heaps.size()); ++h) {
                if (!state.heaps[h].dedicated && state.heaps[h].memory_type == mem_type &&
                    state.heaps[h].allow_msaa == is_msaa) {
                    return h;
                }
            }
        } else {
            for (std::uint32_t h = 0u; h < static_cast<std::uint32_t>(state.heaps.size()); ++h) {
                if (state.heaps[h].dedicated && state.heaps[h].memory_type == mem_type &&
                    state.heaps[h].allow_msaa == is_msaa && heap_watermarks[h] == 0u &&
                    state.heaps[h].capacity >= min_size) {
                    return h;
                }
            }
            for (std::uint32_t h = 0u; h < static_cast<std::uint32_t>(state.heaps.size()); ++h) {
                if (state.heaps[h].dedicated && state.heaps[h].memory_type == mem_type &&
                    state.heaps[h].allow_msaa == is_msaa && heap_watermarks[h] == 0u) {
                    return h;
                }
            }
        }
        detail::TransientHeap heap{};
        heap.memory_type = mem_type;
        heap.dedicated = dedicated;
        heap.allow_msaa = is_msaa;
        state.heaps.add(heap);
        heap_watermarks.add(0u);
        heap_used_queues.add(0u);
        heap_needs_msaa.add(is_msaa);
        return static_cast<std::uint32_t>(state.heaps.size() - 1u);
    };

    Array<std::pair<std::uint64_t, std::uint64_t>> blocked_ranges;
    blocked_ranges.reserve(leases.size());

    for (std::size_t i = 0u; i < leases.size(); ++i) {
        PlacedLease& cur = leases[i];
        const bool is_msaa = cur.is_texture && (cur.nri_tex.sampleNum > 1u);
        const std::uint32_t h_idx =
            find_or_add_heap(cur.mem_desc.type, cur.mem_desc.mustBeDedicated, is_msaa, cur.mem_desc.size);
        cur.heap_index = h_idx;
        heap_used_queues[h_idx] |= cur.queue_mask;
        if (dev_desc.graphicsAPI == nri::GraphicsAPI::D3D12 && cur.is_texture &&
            (cur.queue_mask & static_cast<std::uint8_t>(1u << static_cast<std::size_t>(rhi::QueueKind::copy))) != 0u) {
            heap_used_queues[h_idx] |=
                static_cast<std::uint8_t>(1u << static_cast<std::size_t>(rhi::QueueKind::graphics));
        }
        heap_needs_msaa[h_idx] = heap_needs_msaa[h_idx] || is_msaa;

        if (cur.mem_desc.mustBeDedicated) {
            cur.offset = 0u;
            heap_watermarks[h_idx] = std::max(heap_watermarks[h_idx], cur.size);
            continue;
        }

        const std::uint64_t align = std::max<std::uint64_t>(256u, cur.mem_desc.alignment);
        blocked_ranges.clear();
        const bool cur_copy_tex = (dev_desc.graphicsAPI == nri::GraphicsAPI::D3D12) && cur.is_texture &&
                                  (graph.passes_[cur.first_pass]->resolved_queue == rhi::QueueKind::copy);
        for (std::size_t j = 0u; j < i; ++j) {
            const PlacedLease& placed = leases[j];
            if (placed.heap_index != h_idx)
                continue;
            const bool placed_copy_tex = (dev_desc.graphicsAPI == nri::GraphicsAPI::D3D12) && placed.is_texture &&
                                         (graph.passes_[placed.first_pass]->resolved_queue == rhi::QueueKind::copy);
            const bool disjoint = (!cur_copy_tex && happens_before(placed.last_pass, cur.first_pass)) ||
                                  (!placed_copy_tex && happens_before(cur.last_pass, placed.first_pass));
            if (disjoint)
                continue;
            const std::uint64_t pad = (placed.is_texture != cur.is_texture) ? granularity : 1u;
            const std::uint64_t p_begin = align_down_u64(placed.offset, pad);
            const std::uint64_t p_end = align_up_u64(placed.offset + placed.size, pad);
            blocked_ranges.emplace(p_begin, p_end);
        }
        std::sort(blocked_ranges.begin(), blocked_ranges.end());

        std::uint64_t candidate = 0u;
        for (const auto& [b_start, b_end] : blocked_ranges) {
            candidate = align_up_u64(candidate, align);
            if (candidate + cur.size <= b_start)
                break;
            if (candidate < b_end)
                candidate = align_up_u64(b_end, align);
        }
        cur.offset = align_up_u64(candidate, align);
        heap_watermarks[h_idx] = std::max(heap_watermarks[h_idx], cur.offset + cur.size);
    }

    // Ensure backing nri::Memory* chunks are allocated for every active heap, and retire long-idle heaps (> 180 frames).
    const rhi::FencePoint retire_point = timeline.current_point();
    std::uint64_t peak_aliased_sum = 0u;
    for (std::uint32_t h = 0u; h < static_cast<std::uint32_t>(state.heaps.size()); ++h) {
        const std::uint64_t needed = heap_watermarks[h];
        detail::TransientHeap& heap = state.heaps[h];
        if (needed == 0u) {
            if (heap.memory && current_seq > heap.last_used_sequence + 180u) {
                for (std::size_t ci = 0u; ci < state.placed_cache.size();) {
                    if (state.placed_cache[ci].heap_index == h) {
                        state.retire_placed(state.placed_cache[ci], retire_point);
                        state.placed_cache.remove_at(ci);
                    } else {
                        ++ci;
                    }
                }
                reclaimer.retire({.fence = retire_point, .memory = heap.memory});
                heap.memory = nullptr;
                heap.capacity = 0u;
                heap.used_in_prior_frame = false;
                heap.prev_frame_queue_mask = 0u;
                heap.prev_frame_fences = {};
                ++heap.generation;
            }
            continue;
        }
        heap.last_used_sequence = current_seq;
        peak_aliased_sum += needed;
        if (heap.memory && heap.capacity >= needed)
            continue;

        // Retire cached placed objects bound to the old smaller heap allocation.
        for (std::size_t ci = 0u; ci < state.placed_cache.size();) {
            if (state.placed_cache[ci].heap_index == h) {
                state.retire_placed(state.placed_cache[ci], retire_point);
                state.placed_cache.remove_at(ci);
            } else {
                ++ci;
            }
        }
        if (heap.memory) {
            reclaimer.retire({.fence = retire_point, .memory = heap.memory});
            heap.memory = nullptr;
        }
        constexpr std::uint64_t chunk_step = 16uLL * 1024uLL * 1024uLL;
        const std::uint64_t alloc_bytes =
            heap.dedicated ? needed : std::max(align_up_u64(needed, chunk_step), chunk_step);
        nri::AllocateMemoryDesc alloc_desc{};
        alloc_desc.size = alloc_bytes;
        alloc_desc.type = heap.memory_type;
        alloc_desc.priority = 0.0f;
        alloc_desc.vma.enable = false;
        alloc_desc.allowMultisampleTextures = heap_needs_msaa[h];
        ATELIER_TRY(check_nri(core.AllocateMemory(nri_dev, alloc_desc, heap.memory), rhi::ErrorCode::out_of_memory,
                              "RDG AllocateMemory for transient heap failed"));
        heap.capacity = alloc_bytes;
        heap.used_in_prior_frame = false;
        heap.prev_frame_queue_mask = 0u;
        heap.prev_frame_fences = {};
        ++heap.generation;
    }
    state.last_peak_aliased_bytes = peak_aliased_sum;
    state.last_unaliased_bytes = unaliased_sum;

    // Resolve each lease against the cross-frame Placed Resource + Bindless View cache!
    for (std::size_t lease_idx = 0u; lease_idx < leases.size(); ++lease_idx) {
        const PlacedLease& lease = leases[lease_idx];
        const detail::TransientHeap& heap = state.heaps[lease.heap_index];
        bool has_intra_alias = false;
        for (std::size_t j = 0u; j < lease_idx; ++j) {
            const PlacedLease& prev_l = leases[j];
            if (prev_l.heap_index != lease.heap_index)
                continue;
            const std::uint64_t pad = (prev_l.is_texture != lease.is_texture) ? granularity : 1u;
            const std::uint64_t s_begin = align_down_u64(prev_l.offset, pad);
            const std::uint64_t s_end = align_up_u64(prev_l.offset + prev_l.size, pad);
            const std::uint64_t d_begin = align_down_u64(lease.offset, pad);
            const std::uint64_t d_end = align_up_u64(lease.offset + lease.size, pad);
            if (s_begin < d_end && d_begin < s_end) {
                has_intra_alias = true;
                break;
            }
        }
        if (lease.is_texture) {
            auto& tex = *graph.textures_[lease.node_index];
            tex.heap_index = lease.heap_index;
            tex.byte_offset = lease.offset;
            const std::uint64_t key_hash =
                hash_placed_tex(lease.heap_index, heap.generation, lease.offset, lease.nri_tex);
            detail::CachedPlacedObject* cached = nullptr;
            for (auto& entry : state.placed_cache) {
                if (entry.last_used_sequence != current_seq && entry.key_hash == key_hash && entry.is_texture &&
                    entry.heap_index == lease.heap_index && entry.heap_generation == heap.generation &&
                    entry.byte_offset == lease.offset && same_tex_desc(entry.tex_desc, lease.nri_tex)) {
                    cached = &entry;
                    break;
                }
            }
            if (!cached) {
                detail::CachedPlacedObject created{};
                created.key_hash = key_hash;
                created.heap_index = lease.heap_index;
                created.heap_generation = heap.generation;
                created.byte_offset = lease.offset;
                created.is_texture = true;
                created.tex_desc = lease.nri_tex;
                bool created_ok = false;
                struct PlacedGuard {
                    detail::ExecutorState* st;

                    detail::CachedPlacedObject* obj;

                    rhi::FencePoint fp;

                    bool* ok;

                    ~PlacedGuard() {
                        if (!*ok)
                            st->retire_placed(*obj, fp);
                    }
                } guard{&state, &created, retire_point, &created_ok};

                ATELIER_TRY(check_nri(
                    core.CreatePlacedTexture(nri_dev, heap.memory, lease.offset, lease.nri_tex, created.texture),
                    rhi::ErrorCode::creation_failed, "RDG CreatePlacedTexture failed"));
                if (!tex.name.empty())
                    core.SetDebugName(created.texture, tex.name.data());

                if ((lease.nri_tex.usage & nri::TextureUsageBits::SHADER_RESOURCE) != 0) {
                    nri::TextureViewDesc v_desc{};
                    v_desc.texture = created.texture;
                    v_desc.type = (lease.nri_tex.layerNum > 1u && lease.nri_tex.type == nri::TextureType::TEXTURE_2D)
                                      ? nri::TextureView::TEXTURE_ARRAY
                                      : nri::TextureView::TEXTURE;
                    v_desc.format = lease.nri_tex.format;
                    v_desc.mipNum = nri::REMAINING;
                    v_desc.layerNum = nri::REMAINING;
                    v_desc.sliceNum = nri::REMAINING;
                    ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, created.srv), rhi::ErrorCode::creation_failed,
                                          "RDG CreateTextureView SRV failed"));
                    ATELIER_TRY_ASSIGN(created.srv_bindless, bindless.allocate_resource(created.srv));
                    if (lease.nri_tex.mipNum > 1u) {
                        created.mip_srvs.resize(lease.nri_tex.mipNum, nullptr);
                        created.mip_srv_bindless.resize(lease.nri_tex.mipNum, rhi::bindless_invalid_index);
                        for (nri::Dim_t m = 0u; m < lease.nri_tex.mipNum; ++m) {
                            nri::TextureViewDesc mv = v_desc;
                            mv.mipOffset = m;
                            mv.mipNum = 1u;
                            ATELIER_TRY(check_nri(core.CreateTextureView(mv, created.mip_srvs[m]),
                                                  rhi::ErrorCode::creation_failed,
                                                  "RDG CreateTextureView per-mip SRV failed"));
                            ATELIER_TRY_ASSIGN(created.mip_srv_bindless[m],
                                               bindless.allocate_resource(created.mip_srvs[m]));
                        }
                    }
                }
                if ((lease.nri_tex.usage & nri::TextureUsageBits::SHADER_RESOURCE_STORAGE) != 0) {
                    nri::TextureViewDesc v_desc{};
                    v_desc.texture = created.texture;
                    v_desc.type = (lease.nri_tex.layerNum > 1u && lease.nri_tex.type == nri::TextureType::TEXTURE_2D)
                                      ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                                      : nri::TextureView::STORAGE_TEXTURE;
                    v_desc.format = lease.nri_tex.format;
                    v_desc.mipOffset = 0u;
                    v_desc.mipNum = 1u;
                    v_desc.layerNum = nri::REMAINING;
                    v_desc.sliceNum = nri::REMAINING;
                    ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, created.uav), rhi::ErrorCode::creation_failed,
                                          "RDG CreateTextureView UAV failed"));
                    ATELIER_TRY_ASSIGN(created.uav_bindless, bindless.allocate_resource(created.uav));
                    if (lease.nri_tex.mipNum > 1u) {
                        created.mip_uavs.resize(lease.nri_tex.mipNum, nullptr);
                        created.mip_uav_bindless.resize(lease.nri_tex.mipNum, rhi::bindless_invalid_index);
                        created.mip_uav_bindless[0] = created.uav_bindless;
                        for (nri::Dim_t m = 1u; m < lease.nri_tex.mipNum; ++m) {
                            nri::TextureViewDesc mv = v_desc;
                            mv.mipOffset = m;
                            mv.mipNum = 1u;
                            ATELIER_TRY(check_nri(core.CreateTextureView(mv, created.mip_uavs[m]),
                                                  rhi::ErrorCode::creation_failed,
                                                  "RDG CreateTextureView per-mip UAV failed"));
                            ATELIER_TRY_ASSIGN(created.mip_uav_bindless[m],
                                               bindless.allocate_resource(created.mip_uavs[m]));
                        }
                    }
                }
                if ((lease.nri_tex.usage & nri::TextureUsageBits::COLOR_ATTACHMENT) != 0) {
                    nri::TextureViewDesc v_desc{};
                    v_desc.texture = created.texture;
                    v_desc.type = nri::TextureView::COLOR_ATTACHMENT;
                    v_desc.format = lease.nri_tex.format;
                    v_desc.mipOffset = 0u;
                    v_desc.mipNum = 1u;
                    v_desc.layerNum = 1u;
                    ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, created.rtv), rhi::ErrorCode::creation_failed,
                                          "RDG CreateTextureView RTV failed"));
                }
                if ((lease.nri_tex.usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0) {
                    nri::TextureViewDesc v_desc{};
                    v_desc.texture = created.texture;
                    v_desc.type = nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
                    v_desc.format = lease.nri_tex.format;
                    v_desc.mipOffset = 0u;
                    v_desc.mipNum = 1u;
                    v_desc.layerNum = 1u;
                    ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, created.dsv), rhi::ErrorCode::creation_failed,
                                          "RDG CreateTextureView DSV failed"));
                    v_desc.planes = nri::PlaneBits::NONE;
                    v_desc.layout = nri::Layout::DEPTH_STENCIL_READONLY;
                    ATELIER_TRY(check_nri(core.CreateTextureView(v_desc, created.dsv_readonly),
                                          rhi::ErrorCode::creation_failed, "RDG CreateTextureView DSV RO failed"));
                }
                created_ok = true;
                state.placed_cache.add(std::move(created));
                cached = &state.placed_cache.back();
            }
            // Synchronize against any prior-frame or intra-frame heap occupancy even when this CachedPlacedObject itself is brand new!
            const nri::StageBits init_sync =
                (heap.used_in_prior_frame || has_intra_alias) ? nri::StageBits::ALL : nri::StageBits::NONE;
            cached->last_used_sequence = current_seq;
            tex.runtime_texture = cached->texture;
            tex.views.rtv = cached->rtv;
            tex.views.dsv = cached->dsv;
            tex.views.dsv_readonly = cached->dsv_readonly;
            tex.views.srv_bindless = cached->srv_bindless;
            tex.views.uav_bindless = cached->uav_bindless;
            tex.views.mip_srv_bindless = cached->mip_srv_bindless;
            tex.views.mip_uav_bindless = cached->mip_uav_bindless;
            // Force Layout::UNDEFINED at start of lease so first barrier emits D3D12_TEXTURE_BARRIER_FLAG_DISCARD!
            tex.current_state = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, init_sync};
            for (std::uint16_t m = 0u; m < tex.resolved_mip_count; ++m)
                tex.mip_states[m] = MipTrackingState{tex.current_state, rhi::QueueKind::graphics};
        } else {
            auto& buf = *graph.buffers_[lease.node_index];
            buf.heap_index = lease.heap_index;
            buf.byte_offset = lease.offset;
            const std::uint64_t key_hash =
                hash_placed_buf(lease.heap_index, heap.generation, lease.offset, lease.nri_buf);
            detail::CachedPlacedObject* cached = nullptr;
            for (auto& entry : state.placed_cache) {
                if (entry.last_used_sequence != current_seq && entry.key_hash == key_hash && !entry.is_texture &&
                    entry.heap_index == lease.heap_index && entry.heap_generation == heap.generation &&
                    entry.byte_offset == lease.offset && same_buf_desc(entry.buf_desc, lease.nri_buf)) {
                    cached = &entry;
                    break;
                }
            }
            if (!cached) {
                detail::CachedPlacedObject created{};
                created.key_hash = key_hash;
                created.heap_index = lease.heap_index;
                created.heap_generation = heap.generation;
                created.byte_offset = lease.offset;
                created.is_texture = false;
                created.buf_desc = lease.nri_buf;
                bool created_ok = false;
                struct PlacedGuard {
                    detail::ExecutorState* st;

                    detail::CachedPlacedObject* obj;

                    rhi::FencePoint fp;

                    bool* ok;

                    ~PlacedGuard() {
                        if (!*ok)
                            st->retire_placed(*obj, fp);
                    }
                } guard{&state, &created, retire_point, &created_ok};

                ATELIER_TRY(check_nri(
                    core.CreatePlacedBuffer(nri_dev, heap.memory, lease.offset, lease.nri_buf, created.buffer),
                    rhi::ErrorCode::creation_failed, "RDG CreatePlacedBuffer failed"));
                if (!buf.name.empty())
                    core.SetDebugName(created.buffer, buf.name.data());

                if ((lease.nri_buf.usage & nri::BufferUsageBits::SHADER_RESOURCE) != 0 &&
                    (lease.nri_buf.byteAddress || lease.nri_buf.structureStride > 0u)) {
                    nri::BufferViewDesc v_desc{};
                    v_desc.buffer = created.buffer;
                    v_desc.type = lease.nri_buf.byteAddress ? nri::BufferView::BYTE_ADDRESS_BUFFER
                                                            : nri::BufferView::STRUCTURED_BUFFER;
                    v_desc.offset = 0u;
                    v_desc.size = nri::WHOLE_SIZE;
                    v_desc.structureStride = lease.nri_buf.structureStride;
                    ATELIER_TRY(check_nri(core.CreateBufferView(v_desc, created.srv), rhi::ErrorCode::creation_failed,
                                          "RDG CreateBufferView SRV failed"));
                    ATELIER_TRY_ASSIGN(created.srv_bindless, bindless.allocate_resource(created.srv));
                }
                if ((lease.nri_buf.usage & nri::BufferUsageBits::SHADER_RESOURCE_STORAGE) != 0 &&
                    (lease.nri_buf.byteAddress || lease.nri_buf.structureStride > 0u)) {
                    nri::BufferViewDesc v_desc{};
                    v_desc.buffer = created.buffer;
                    v_desc.type = lease.nri_buf.byteAddress ? nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER
                                                            : nri::BufferView::STORAGE_STRUCTURED_BUFFER;
                    v_desc.offset = 0u;
                    v_desc.size = nri::WHOLE_SIZE;
                    v_desc.structureStride = lease.nri_buf.structureStride;
                    ATELIER_TRY(check_nri(core.CreateBufferView(v_desc, created.uav), rhi::ErrorCode::creation_failed,
                                          "RDG CreateBufferView UAV failed"));
                    ATELIER_TRY_ASSIGN(created.uav_bindless, bindless.allocate_resource(created.uav));
                }
                created_ok = true;
                state.placed_cache.add(std::move(created));
                cached = &state.placed_cache.back();
            }
            cached->last_used_sequence = current_seq;
            buf.runtime_buffer = cached->buffer;
            buf.views.srv_bindless = cached->srv_bindless;
            buf.views.uav_bindless = cached->uav_bindless;
            buf.current_state = {nri::AccessBits::NONE, nri::StageBits::NONE};
        }
    }

    // Trim placed cache entries unused for > 180 executions to prevent unbounded growth across resizes.
    for (std::size_t ci = 0u; ci < state.placed_cache.size();) {
        if (current_seq > state.placed_cache[ci].last_used_sequence + 180u) {
            state.retire_placed(state.placed_cache[ci], retire_point);
            state.placed_cache.remove_at(ci);
        } else {
            ++ci;
        }
    }

    // Build per-pass intra-frame & cross-frame aliasing GlobalBarriers (strictly queue-native and non-NO_ACCESS!).
    Array<nri::GlobalBarrierDesc> pass_alias_globals(graph.passes_.size());
    Array<bool> pass_has_alias_global(graph.passes_.size(), false);
    for (std::size_t i = 0u; i < leases.size(); ++i) {
        const PlacedLease& dst = leases[i];
        const rhi::QueueKind pass_q = graph.passes_[dst.first_pass]->resolved_queue;
        const auto pq_idx = static_cast<std::size_t>(pass_q);
        nri::AccessBits before_access = nri::AccessBits::NONE;

        for (std::size_t j = 0u; j < leases.size(); ++j) {
            if (i == j)
                continue;
            const PlacedLease& src = leases[j];
            if (src.heap_index != dst.heap_index)
                continue;
            const std::uint64_t pad = (src.is_texture != dst.is_texture) ? granularity : 1u;
            const std::uint64_t s_begin = align_down_u64(src.offset, pad);
            const std::uint64_t s_end = align_up_u64(src.offset + src.size, pad);
            const std::uint64_t d_begin = align_down_u64(dst.offset, pad);
            const std::uint64_t d_end = align_up_u64(dst.offset + dst.size, pad);
            const bool overlaps = (s_begin < d_end) && (d_begin < s_end);
            if (!overlaps || !happens_before(src.last_pass, dst.first_pass))
                continue;
            // Cross-queue predecessors are already synchronized by ID3D12Fence Wait; accumulate same-queue accesses (including reads for WAR!).
            if (src.is_texture)
                before_access = before_access | graph.textures_[src.node_index]->per_queue_accesses[pq_idx];
            else
                before_access = before_access | graph.buffers_[src.node_index]->per_queue_accesses[pq_idx];
        }

        // Placed buffers have no Layout::UNDEFINED discard barrier, so also guard cross-frame same-queue heap reuse!
        const detail::TransientHeap& heap = state.heaps[dst.heap_index];
        if (!dst.is_texture && before_access == nri::AccessBits::NONE && heap.used_in_prior_frame &&
            (heap.prev_frame_queue_mask & static_cast<std::uint8_t>(1u << pq_idx)) != 0u) {
            before_access = (pass_q == rhi::QueueKind::copy) ? nri::AccessBits::COPY_DESTINATION
                                                             : nri::AccessBits::SHADER_RESOURCE_STORAGE;
        }

        before_access = sanitize_d3d12_access(before_access, pass_q);
        const nri::AccessBits raw_after = dst.is_texture ? graph.textures_[dst.node_index]->first_pass_access
                                                         : graph.buffers_[dst.node_index]->first_pass_access;
        const nri::AccessBits after_access = sanitize_d3d12_access(raw_after, pass_q);
        if (before_access != nri::AccessBits::NONE && after_access != nri::AccessBits::NONE) {
            const nri::StageBits sync_stage =
                (pass_q == rhi::QueueKind::copy) ? nri::StageBits::COPY : nri::StageBits::ALL;
            auto& g_bar = pass_alias_globals[dst.first_pass];
            g_bar.before.access = sanitize_d3d12_access(g_bar.before.access | before_access, pass_q);
            g_bar.before.stages = sync_stage;
            g_bar.after.access = sanitize_d3d12_access(g_bar.after.access | after_access, pass_q);
            g_bar.after.stages = sync_stage;
            pass_has_alias_global[dst.first_pass] = true;
        }
    }

    // 5. Record & Submit Passes across Queues with Enhanced Barriers, Most-Capable-Queue Handoff, Timestamps, and DRED.
    const std::uint32_t slice_idx = (state.slice_cursor + detail::command_ring_size - 1u) % detail::command_ring_size;
    const std::uint32_t ts_pool_base = slice_idx * (detail::max_timed_passes_per_frame * 2u);
    const std::uint32_t stat_pool_base = slice_idx * detail::max_timed_passes_per_frame;
    const bool can_time = options.collect_timestamps && (state.timestamp_pool != nullptr);
    const bool can_stat = options.collect_pipeline_statistics && (state.stats_pool != nullptr);

    slice.frame_id = options.frame_id;
    slice.sequence = current_seq;
    slice.pending_timestamps.clear();
    slice.pending_stats.clear();

    std::array<nri::CommandBuffer*, 3> open_cmds{nullptr, nullptr, nullptr};
    struct OpenCmdsAbortGuard {
        const nri::CoreInterface* core;

        std::array<nri::CommandBuffer*, 3>* cmds;

        ~OpenCmdsAbortGuard() {
            for (nri::CommandBuffer*& cb : *cmds) {
                if (cb) {
                    static_cast<void>(core->EndCommandBuffer(*cb));
                    cb = nullptr;
                }
            }
        }
    } open_cmds_guard{&core, &open_cmds};

    Array<std::uint64_t> pass_signal_fence(graph.passes_.size(), 0u);
    std::array<std::uint32_t, 3> next_unsignaled_pass{0u, 0u, 0u};
    auto satisfied_waits = state.satisfied_waits;

    // Compute cross-frame cross-queue fence waits for any TransientHeap reused across different hardware queues!
    std::array<std::array<std::uint64_t, 3>, 3> pending_heap_waits{};
    for (std::uint32_t h = 0u; h < static_cast<std::uint32_t>(state.heaps.size()); ++h) {
        const detail::TransientHeap& heap = state.heaps[h];
        if (!heap.used_in_prior_frame || heap_watermarks[h] == 0u)
            continue;
        for (std::size_t dst_q = 0u; dst_q < 3u; ++dst_q) {
            if ((heap_used_queues[h] & (1u << dst_q)) == 0u)
                continue;
            for (std::size_t src_q = 0u; src_q < 3u; ++src_q) {
                if (src_q == dst_q || (heap.prev_frame_queue_mask & (1u << src_q)) == 0u)
                    continue;
                pending_heap_waits[dst_q][src_q] =
                    std::max(pending_heap_waits[dst_q][src_q], heap.prev_frame_fences.values[src_q]);
            }
        }
    }

    if (state.has_prev_frame) {
        auto add_imported_cross_frame_waits = [&](std::uint8_t q_mask) {
            for (std::size_t dst_q = 0u; dst_q < 3u; ++dst_q) {
                if ((q_mask & (1u << dst_q)) == 0u)
                    continue;
                for (std::size_t src_q = 0u; src_q < 3u; ++src_q) {
                    if (src_q == dst_q || (state.prev_frame_queue_mask & (1u << src_q)) == 0u)
                        continue;
                    pending_heap_waits[dst_q][src_q] =
                        std::max(pending_heap_waits[dst_q][src_q], state.prev_frame_fences.values[src_q]);
                }
            }
        };
        for (const auto* tex : graph.textures_) {
            if (!tex->needed || !tex->imported)
                continue;
            if (tex->initial_state.layout != nri::Layout::UNDEFINED ||
                tex->initial_state.stages != nri::StageBits::NONE ||
                tex->initial_state.access != nri::AccessBits::NONE) {
                add_imported_cross_frame_waits(tex->queue_mask);
            }
        }
        for (const auto* buf : graph.buffers_) {
            if (!buf->needed || !buf->imported)
                continue;
            if (buf->initial_state.stages != nri::StageBits::NONE ||
                buf->initial_state.access != nri::AccessBits::NONE) {
                add_imported_cross_frame_waits(buf->queue_mask);
            }
        }
    }

    std::uint8_t frame_submitted_queues = 0u;
    auto flush_queue =
        [&](rhi::QueueKind q, std::uint32_t up_to_pass,
            Span<const std::pair<rhi::QueueKind, std::uint64_t>> waits = {}) -> rhi::Result<std::uint64_t> {
        const auto q_idx = static_cast<std::size_t>(q);
        nri::CommandBuffer* cb = open_cmds[q_idx];
        if (!cb)
            return timeline.current_value(q);
        open_cmds[q_idx] = nullptr;
        ATELIER_TRY(
            check_nri(core.EndCommandBuffer(*cb), rhi::ErrorCode::execution_failed, "RDG EndCommandBuffer failed"));
        std::array<nri::CommandBuffer*, 1> cbs{cb};
        ATELIER_TRY_BIND(sig, timeline.submit(q, cbs, waits));
        frame_submitted_queues |= static_cast<std::uint8_t>(1u << q_idx);
        while (next_unsignaled_pass[q_idx] < up_to_pass) {
            const std::uint32_t k = next_unsignaled_pass[q_idx];
            if (!graph.passes_[k]->culled && graph.passes_[k]->resolved_queue == q && pass_signal_fence[k] == 0u)
                pass_signal_fence[k] = *sig;
            ++next_unsignaled_pass[q_idx];
        }
        for (const auto& [wq_kind, wq_val] : waits) {
            const auto wq = static_cast<std::size_t>(wq_kind);
            satisfied_waits[q_idx][wq] = std::max(satisfied_waits[q_idx][wq], wq_val);
            state.satisfied_waits[q_idx][wq] = satisfied_waits[q_idx][wq];
        }
        return *sig;
    };

    auto get_cmd = [&](rhi::QueueKind q) -> rhi::Result<nri::CommandBuffer*> {
        const auto q_idx = static_cast<std::size_t>(q);
        if (!open_cmds[q_idx]) {
            ATELIER_TRY_ASSIGN(open_cmds[q_idx], acquire_command_buffer(state, slice, q));
        }
        return open_cmds[q_idx];
    };

    Array<nri::GlobalBarrierDesc> globals;
    Array<nri::BufferBarrierDesc> buf_barriers;
    Array<nri::TextureBarrierDesc> tex_barriers;
    globals.reserve(4u);
    buf_barriers.reserve(32u);
    tex_barriers.reserve(32u);

    const bool is_d3d12 = (dev_desc.graphicsAPI == nri::GraphicsAPI::D3D12);

    for (std::uint32_t p = 0u; p < static_cast<std::uint32_t>(graph.passes_.size()); ++p) {
        auto& pass = *graph.passes_[p];
        if (pass.culled)
            continue;
        const rhi::QueueKind dst_q = pass.resolved_queue;
        const auto dst_q_idx = static_cast<std::size_t>(dst_q);

        // Pre-transition resources on source/graphics queue when Most-Capable-Queue law requires transition BEFORE signal!
        std::array<bool, 3> pre_transition_recorded{false, false, false};
        for (auto* acc = pass.first_access; acc != nullptr; acc = acc->next) {
            if (!acc->is_texture)
                continue;
            auto& tex = *graph.textures_[acc->index];
            const auto [m0, m1] = resolve_mip_bounds(tex, acc->range);
            const nri::PlaneBits planes = texture_planes_for_format(tex.resolved_format);
            const nri::Layout target_on_dst =
                (tex.simultaneous || (is_d3d12 && dst_q == rhi::QueueKind::copy)) ? nri::Layout::GENERAL : acc->layout;

            for (std::uint16_t m = m0; m < m1;) {
                const MipTrackingState ms = tex.mip_states[m];
                std::uint16_t run_end = static_cast<std::uint16_t>(m + 1u);
                while (run_end < m1 && tex.mip_states[run_end].queue == ms.queue &&
                       tex.mip_states[run_end].state.layout == ms.state.layout &&
                       tex.mip_states[run_end].state.access == ms.state.access &&
                       tex.mip_states[run_end].state.stages == ms.state.stages) {
                    ++run_end;
                }
                const bool full_mips = (m == 0u && run_end == tex.resolved_mip_count);
                // D3D12 Copy queues cannot transition Placed Textures out of Layout::UNDEFINED; Graphics must initialize COMMON first!
                if (is_d3d12 && dst_q == rhi::QueueKind::copy && ms.state.layout == nri::Layout::UNDEFINED) {
                    ATELIER_TRY_BIND(gfx_cb, get_cmd(rhi::QueueKind::graphics));
                    nri::TextureBarrierDesc tb{};
                    tb.texture = tex.runtime_texture;
                    tb.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, ms.state.stages};
                    tb.after = {nri::AccessBits::NONE, nri::Layout::GENERAL, nri::StageBits::NONE};
                    tb.mipOffset = m;
                    tb.mipNum = full_mips ? nri::REMAINING : static_cast<nri::Dim_t>(run_end - m);
                    tb.layerNum = nri::REMAINING;
                    tb.planes = planes;
                    nri::BarrierDesc bd{};
                    bd.textures = &tb;
                    bd.textureNum = 1u;
                    core.CmdBarrier(**gfx_cb, bd);
                    for (std::uint16_t k = m; k < run_end; ++k) {
                        tex.mip_states[k] = MipTrackingState{
                            {nri::AccessBits::NONE, nri::Layout::GENERAL, nri::StageBits::NONE},
                            rhi::QueueKind::graphics};
                    }
                    pre_transition_recorded[static_cast<std::size_t>(rhi::QueueKind::graphics)] = true;
                } else if (ms.state.layout != nri::Layout::UNDEFINED && ms.queue != dst_q) {
                    const rhi::QueueKind src_q = ms.queue;
                    if ((src_q == rhi::QueueKind::graphics && is_graphics_only_layout(ms.state.layout)) ||
                        (is_d3d12 && dst_q == rhi::QueueKind::copy && ms.state.layout != nri::Layout::GENERAL)) {
                        ATELIER_TRY_BIND(src_cb, get_cmd(src_q));
                        const nri::AccessBits b_acc = sanitize_d3d12_access(ms.state.access, src_q);
                        nri::TextureBarrierDesc tb{};
                        tb.texture = tex.runtime_texture;
                        tb.before = {b_acc, ms.state.layout, sanitize_d3d12_stages(ms.state.stages, b_acc, src_q)};
                        tb.after = {nri::AccessBits::NONE, target_on_dst, nri::StageBits::NONE};
                        tb.mipOffset = m;
                        tb.mipNum = full_mips ? nri::REMAINING : static_cast<nri::Dim_t>(run_end - m);
                        tb.layerNum = nri::REMAINING;
                        tb.planes = planes;
                        nri::BarrierDesc bd{};
                        bd.textures = &tb;
                        bd.textureNum = 1u;
                        core.CmdBarrier(**src_cb, bd);
                        for (std::uint16_t k = m; k < run_end; ++k) {
                            tex.mip_states[k].state = {nri::AccessBits::NONE, target_on_dst, nri::StageBits::NONE};
                        }
                        pre_transition_recorded[static_cast<std::size_t>(src_q)] = true;
                    }
                }
                m = run_end;
            }
            tex.current_state = tex.mip_states[0].state;
            tex.current_queue = tex.mip_states[0].queue;
        }

        // Collect cross-queue fence waits needed by pass `p` (including prior-frame heap waits & pre-transitions!).
        std::array<std::pair<rhi::QueueKind, std::uint64_t>, 2> waits{};
        std::size_t wait_count = 0u;
        for (std::size_t q = 0u; q < 3u; ++q) {
            if (q == dst_q_idx)
                continue;
            const auto src_q = static_cast<rhi::QueueKind>(q);
            const std::uint32_t dep_pass = pass_waits[p][q];
            std::uint64_t required_val = pending_heap_waits[dst_q_idx][q];
            pending_heap_waits[dst_q_idx][q] = 0u;

            if (pre_transition_recorded[q] || (dep_pass != invalid_handle_index && pass_signal_fence[dep_pass] == 0u)) {
                std::array<std::pair<rhi::QueueKind, std::uint64_t>, 2> src_waits{};
                std::size_t src_wait_count = 0u;
                for (std::size_t oq = 0u; oq < 3u; ++oq) {
                    if (oq == q)
                        continue;
                    const std::uint64_t hw = pending_heap_waits[q][oq];
                    pending_heap_waits[q][oq] = 0u;
                    if (hw > satisfied_waits[q][oq])
                        src_waits[src_wait_count++] = {static_cast<rhi::QueueKind>(oq), hw};
                }
                std::uint64_t sig = 0u;
                ATELIER_TRY_ASSIGN(sig, flush_queue(src_q, p, Span{src_waits.data(), src_wait_count}));
                if (pre_transition_recorded[q])
                    required_val = std::max(required_val, sig);
            }
            if (dep_pass != invalid_handle_index)
                required_val = std::max(required_val, pass_signal_fence[dep_pass]);

            if (required_val > satisfied_waits[dst_q_idx][q])
                waits[wait_count++] = {src_q, required_val};
        }

        // If `dst_q` already has an open command buffer and now needs a cross-queue wait, flush `dst_q` first!
        if (wait_count > 0u && open_cmds[dst_q_idx] != nullptr) {
            ATELIER_TRY(flush_queue(dst_q, p));
        }

        ATELIER_TRY_BIND(cb_res, get_cmd(dst_q));
        nri::CommandBuffer& cb = **cb_res;

        // Synthesize Enhanced Barriers for pass `p`!
        globals.clear();
        buf_barriers.clear();
        tex_barriers.clear();

        if (pass_has_alias_global[p])
            globals.add(pass_alias_globals[p]);

        for (auto* acc = pass.first_access; acc != nullptr; acc = acc->next) {
            if (acc->is_texture) {
                auto& tex = *graph.textures_[acc->index];
                const auto [m0, m1] = resolve_mip_bounds(tex, acc->range);
                const nri::PlaneBits planes = texture_planes_for_format(tex.resolved_format);
                const nri::Layout desired_layout = (tex.simultaneous || (is_d3d12 && dst_q == rhi::QueueKind::copy))
                                                       ? nri::Layout::GENERAL
                                                       : acc->layout;
                const nri::AccessBits a_acc = sanitize_d3d12_access(acc->access, dst_q);
                const nri::StageBits a_stg = sanitize_d3d12_stages(acc->stages, a_acc, dst_q);

                for (std::uint16_t m = m0; m < m1;) {
                    const MipTrackingState ms = tex.mip_states[m];
                    std::uint16_t run_end = static_cast<std::uint16_t>(m + 1u);
                    while (run_end < m1 && tex.mip_states[run_end].queue == ms.queue &&
                           tex.mip_states[run_end].state.layout == ms.state.layout &&
                           tex.mip_states[run_end].state.access == ms.state.access &&
                           tex.mip_states[run_end].state.stages == ms.state.stages) {
                        ++run_end;
                    }
                    const bool cross_q = (ms.state.layout != nri::Layout::UNDEFINED) && (ms.queue != dst_q);
                    const TextureState effective_prev =
                        cross_q ? TextureState{nri::AccessBits::NONE, ms.state.layout, nri::StageBits::NONE} : ms.state;

                    const nri::AccessBits b_acc = sanitize_d3d12_access(effective_prev.access, dst_q);
                    const nri::StageBits b_stg =
                        (effective_prev.layout == nri::Layout::UNDEFINED && b_acc == nri::AccessBits::NONE)
                            ? effective_prev.stages
                            : sanitize_d3d12_stages(effective_prev.stages, b_acc, dst_q);

                    const bool need_barrier = (effective_prev.layout != desired_layout) || is_write_access(b_acc) ||
                                              acc->is_write || (b_acc != a_acc && b_stg != nri::StageBits::NONE);

                    if (need_barrier &&
                        !(is_d3d12 && dst_q == rhi::QueueKind::copy && effective_prev.layout == nri::Layout::GENERAL &&
                          !acc->is_write && !is_write_access(b_acc))) {
                        const bool full_mips = (m == 0u && run_end == tex.resolved_mip_count);
                        nri::TextureBarrierDesc tb{};
                        tb.texture = tex.runtime_texture;
                        tb.before = {b_acc, effective_prev.layout, b_stg};
                        tb.after = {a_acc, desired_layout, a_stg};
                        tb.mipOffset = m;
                        tb.mipNum = full_mips ? nri::REMAINING : static_cast<nri::Dim_t>(run_end - m);
                        tb.layerNum = nri::REMAINING;
                        tb.planes = planes;
                        tex_barriers.add(tb);
                    }
                    for (std::uint16_t k = m; k < run_end; ++k)
                        tex.mip_states[k] = MipTrackingState{{a_acc, desired_layout, a_stg}, dst_q};
                    m = run_end;
                }
                tex.current_state = tex.mip_states[0].state;
                tex.current_queue = tex.mip_states[0].queue;
            } else {
                auto& buf = *graph.buffers_[acc->index];
                const bool cross_q = (buf.current_state.stages != nri::StageBits::NONE) && (buf.current_queue != dst_q);
                const BufferState effective_prev =
                    cross_q ? BufferState{nri::AccessBits::NONE, nri::StageBits::NONE} : buf.current_state;
                const nri::AccessBits b_acc = sanitize_d3d12_access(effective_prev.access, dst_q);
                const nri::AccessBits a_acc = sanitize_d3d12_access(acc->access, dst_q);
                const nri::StageBits b_stg = sanitize_d3d12_stages(effective_prev.stages, b_acc, dst_q);
                const nri::StageBits a_stg = sanitize_d3d12_stages(acc->stages, a_acc, dst_q);
                const bool need_barrier = (b_stg != nri::StageBits::NONE) && (is_write_access(b_acc) || acc->is_write);
                if (need_barrier) {
                    nri::BufferBarrierDesc bb{};
                    bb.buffer = buf.runtime_buffer;
                    bb.before = {b_acc, b_stg};
                    bb.after = {a_acc, a_stg};
                    buf_barriers.add(bb);
                }
                buf.current_state = {a_acc, a_stg};
                buf.current_queue = dst_q;
            }
        }

        if (!globals.empty() || !buf_barriers.empty() || !tex_barriers.empty()) {
            nri::BarrierDesc bd{};
            bd.globals = globals.empty() ? nullptr : globals.data();
            bd.globalNum = static_cast<std::uint32_t>(globals.size());
            bd.buffers = buf_barriers.empty() ? nullptr : buf_barriers.data();
            bd.bufferNum = static_cast<std::uint32_t>(buf_barriers.size());
            bd.textures = tex_barriers.empty() ? nullptr : tex_barriers.data();
            bd.textureNum = static_cast<std::uint32_t>(tex_barriers.size());
            core.CmdBarrier(cb, bd);
        }

        // DRED / PIX Breadcrumb + Optional GPU Timestamp & Graphics PipelineStatistics1!
        core.CmdBeginAnnotation(cb, pass.name.empty() ? "rdg_pass" : pass.name.data(), nri::BGRA_UNUSED);

        const bool time_this_pass = can_time && (dst_q != rhi::QueueKind::copy) &&
                                    (slice.pending_timestamps.size() < detail::max_timed_passes_per_frame);
        std::uint32_t q_begin = 0u;
        if (time_this_pass) {
            q_begin = ts_pool_base + static_cast<std::uint32_t>(slice.pending_timestamps.size()) * 2u;
            core.CmdResetQueries(cb, *state.timestamp_pool, q_begin, 2u);
            core.CmdEndQuery(cb, *state.timestamp_pool, q_begin);
        }

        const bool stat_this_pass = can_stat && (dst_q == rhi::QueueKind::graphics) &&
                                    (slice.pending_stats.size() < detail::max_timed_passes_per_frame);
        std::uint32_t stat_slot = 0u;
        if (stat_this_pass) {
            stat_slot = static_cast<std::uint32_t>(slice.pending_stats.size());
            core.CmdResetQueries(cb, *state.stats_pool, stat_pool_base + stat_slot, 1u);
            core.CmdBeginQuery(cb, *state.stats_pool, stat_pool_base + stat_slot);
        }

        PassContext ctx{};
        ctx.device_ = &device;
        ctx.graph_ = &graph;
        ctx.pass_index_ = p;
        ctx.cmd_ = &cb;
        ctx.core_ = &core;
        ctx.mesh_shader_ = &mesh_iface;
        ctx.current_bind_point_ =
            (dst_q == rhi::QueueKind::compute) ? nri::BindPoint::COMPUTE : nri::BindPoint::GRAPHICS;

        auto pass_res = pass.thunk(pass.callable, ctx);
        if (stat_this_pass)
            core.CmdEndQuery(cb, *state.stats_pool, stat_pool_base + stat_slot);
        if (time_this_pass)
            core.CmdEndQuery(cb, *state.timestamp_pool, q_begin + 1u);
        core.CmdEndAnnotation(cb);

        if (!pass_res) {
            if (options.trace)
                options.trace->interrupted = true;
            return std::unexpected(std::move(pass_res).error());
        }

        if (time_this_pass) {
            slice.pending_timestamps.add(detail::CommandSlice::PendingTimestamp{
                String::from_utf8(pass.name).value_or("rdg_pass"_s), dst_q, q_begin});
        }
        if (stat_this_pass) {
            slice.pending_stats.add(detail::CommandSlice::PendingPipeStat{
                String::from_utf8(pass.name).value_or("rdg_pass"_s), stat_slot});
        }

        // If we had pending cross-queue waits for this batch, submit immediately with those waits so subsequent passes see them satisfied.
        if (wait_count > 0u) {
            ATELIER_TRY(flush_queue(dst_q, p + 1u, Span{waits.data(), wait_count}));
        }
    }

    // 6. Emit Epilogue Barriers for Imported / Exported Resources (e.g. Backbuffer -> Layout::PRESENT)!
    const auto all_passes_end = static_cast<std::uint32_t>(graph.passes_.size());
    tex_barriers.clear();
    buf_barriers.clear();
    bool need_aux_queue_sync = false;

    for (auto* tex : graph.textures_) {
        if (tex->needed && tex->imported && (tex->queue_mask & 0b110u) != 0u)
            need_aux_queue_sync = true;

        bool diverged_tracked = false;
        if (tex->imported && tex->out_committed_state != nullptr && (!tex->exported || tex->final_state.layout == nri::Layout::UNDEFINED)) {
            const MipTrackingState& m0_st = tex->mip_states[0];
            for (std::uint16_t m = 1u; m < tex->resolved_mip_count; ++m) {
                if (tex->mip_states[m].queue != m0_st.queue ||
                    tex->mip_states[m].state.layout != m0_st.state.layout ||
                    tex->mip_states[m].state.access != m0_st.state.access) {
                    diverged_tracked = true;
                    break;
                }
            }
        }
        if ((!tex->exported || tex->final_state.layout == nri::Layout::UNDEFINED) && !diverged_tracked)
            continue;

        TextureState desired_final = (tex->exported && tex->final_state.layout != nri::Layout::UNDEFINED)
                                         ? tex->final_state
                                         : tex->mip_states[0].state;
        if (desired_final.layout == nri::Layout::UNDEFINED) {
            for (std::uint16_t m = 1u; m < tex->resolved_mip_count; ++m) {
                if (tex->mip_states[m].state.layout != nri::Layout::UNDEFINED) {
                    desired_final = tex->mip_states[m].state;
                    break;
                }
            }
        }
        if (desired_final.layout == nri::Layout::UNDEFINED)
            continue;
        const nri::Layout final_layout = (tex->simultaneous && desired_final.layout != nri::Layout::PRESENT)
                                             ? nri::Layout::GENERAL
                                             : desired_final.layout;
        const nri::AccessBits a_acc = sanitize_d3d12_access(desired_final.access, rhi::QueueKind::graphics);
        const nri::StageBits a_stg = sanitize_d3d12_stages(desired_final.stages, a_acc, rhi::QueueKind::graphics);
        const nri::PlaneBits planes = texture_planes_for_format(tex->resolved_format);

        if ((tex->queue_mask & 0b110u) != 0u)
            need_aux_queue_sync = true;

        for (std::uint16_t m = 0u; m < tex->resolved_mip_count;) {
            const MipTrackingState ms = tex->mip_states[m];
            std::uint16_t run_end = static_cast<std::uint16_t>(m + 1u);
            while (run_end < tex->resolved_mip_count && tex->mip_states[run_end].queue == ms.queue &&
                   tex->mip_states[run_end].state.layout == ms.state.layout &&
                   tex->mip_states[run_end].state.access == ms.state.access &&
                   tex->mip_states[run_end].state.stages == ms.state.stages) {
                ++run_end;
            }
            const bool cross_q = (ms.queue != rhi::QueueKind::graphics);
            if (cross_q)
                need_aux_queue_sync = true;
            if (cross_q || ms.state.layout != final_layout || ms.state.access != desired_final.access) {
                const nri::AccessBits b_acc =
                    cross_q ? nri::AccessBits::NONE : sanitize_d3d12_access(ms.state.access, rhi::QueueKind::graphics);
                const nri::StageBits b_stg =
                    (ms.state.layout == nri::Layout::UNDEFINED && b_acc == nri::AccessBits::NONE)
                        ? ms.state.stages
                        : (cross_q ? nri::StageBits::NONE
                                   : sanitize_d3d12_stages(ms.state.stages, b_acc, rhi::QueueKind::graphics));
                if (ms.state.layout != final_layout || (b_stg != nri::StageBits::NONE && a_stg != nri::StageBits::NONE)) {
                    const bool full_mips = (m == 0u && run_end == tex->resolved_mip_count);
                    nri::TextureBarrierDesc tb{};
                    tb.texture = tex->runtime_texture;
                    tb.before = {b_acc, ms.state.layout, b_stg};
                    tb.after = {a_acc, final_layout, a_stg};
                    tb.mipOffset = m;
                    tb.mipNum = full_mips ? nri::REMAINING : static_cast<nri::Dim_t>(run_end - m);
                    tb.layerNum = nri::REMAINING;
                    tb.planes = planes;
                    tex_barriers.add(tb);
                }
            }
            for (std::uint16_t k = m; k < run_end; ++k)
                tex->mip_states[k] = MipTrackingState{{a_acc, final_layout, a_stg}, rhi::QueueKind::graphics};
            m = run_end;
        }
        tex->current_state = {a_acc, final_layout, a_stg};
        tex->current_queue = rhi::QueueKind::graphics;
    }

    for (auto* buf : graph.buffers_) {
        if (buf->needed && buf->imported && (buf->queue_mask & 0b110u) != 0u)
            need_aux_queue_sync = true;
        if (!buf->exported)
            continue;
        const bool cross_q = (buf->current_queue != rhi::QueueKind::graphics);
        if (cross_q || (buf->queue_mask & 0b110u) != 0u)
            need_aux_queue_sync = true;
        const bool is_host_final =
            (buf->final_state.access & (nri::AccessBits::HOST_READ | nri::AccessBits::HOST_WRITE)) != 0 ||
            (buf->final_state.stages != nri::StageBits::ALL &&
             has_any_stage(buf->final_state.stages, nri::StageBits::HOST));
        if (is_host_final) {
            // Host readback happens after fence completion; ensure aux queues join Graphics FencePoint if needed.
            buf->current_state = {nri::AccessBits::NONE, nri::StageBits::NONE};
            buf->current_queue = rhi::QueueKind::graphics;
            continue;
        }
        if (!cross_q && buf->current_state.access == buf->final_state.access &&
            buf->current_state.stages == buf->final_state.stages) {
            continue;
        }
        const nri::AccessBits b_acc = cross_q
                                          ? nri::AccessBits::NONE
                                          : sanitize_d3d12_access(buf->current_state.access, rhi::QueueKind::graphics);
        const nri::StageBits b_stg =
            cross_q ? nri::StageBits::NONE
                    : sanitize_d3d12_stages(buf->current_state.stages, b_acc, rhi::QueueKind::graphics);
        const nri::AccessBits a_acc = sanitize_d3d12_access(buf->final_state.access, rhi::QueueKind::graphics);
        const nri::StageBits a_stg = sanitize_d3d12_stages(buf->final_state.stages, a_acc, rhi::QueueKind::graphics);
        if (b_stg != nri::StageBits::NONE && a_stg != nri::StageBits::NONE) {
            nri::BufferBarrierDesc bb{};
            bb.buffer = buf->runtime_buffer;
            bb.before = {b_acc, b_stg};
            bb.after = {a_acc, a_stg};
            buf_barriers.add(bb);
        }
        buf->current_state = {a_acc, a_stg};
        buf->current_queue = rhi::QueueKind::graphics;
    }

    const bool has_epilogue_work = need_aux_queue_sync || !tex_barriers.empty() || !buf_barriers.empty() ||
                                   !slice.pending_timestamps.empty() || !slice.pending_stats.empty();

    if (has_epilogue_work) {
        // Flush open Compute/Copy command buffers AND wait on ALL unsynchronized Compute/Copy signals from this frame!
        std::array<std::pair<rhi::QueueKind, std::uint64_t>, 2> final_waits{};
        std::size_t final_wait_count = 0u;
        for (rhi::QueueKind aux_q : {rhi::QueueKind::compute, rhi::QueueKind::copy}) {
            const auto aux_resolved = timeline.resolve_kind(aux_q);
            if (aux_resolved == rhi::QueueKind::graphics)
                continue;
            const auto aux_idx = static_cast<std::size_t>(aux_resolved);
            if (open_cmds[aux_idx] != nullptr) {
                ATELIER_TRY(flush_queue(aux_resolved, all_passes_end));
            }
            const std::uint64_t latest_aux = timeline.current_value(aux_resolved);
            if (latest_aux > satisfied_waits[0][aux_idx]) {
                final_waits[final_wait_count++] = {aux_resolved, latest_aux};
            }
        }
        if (final_wait_count > 0u && open_cmds[0] != nullptr) {
            ATELIER_TRY(flush_queue(rhi::QueueKind::graphics, all_passes_end));
        }

        if (!tex_barriers.empty() || !buf_barriers.empty() || !slice.pending_timestamps.empty() ||
            !slice.pending_stats.empty() || final_wait_count > 0u) {
            ATELIER_TRY_BIND(gfx_cb, get_cmd(rhi::QueueKind::graphics));
            if (!tex_barriers.empty() || !buf_barriers.empty()) {
                nri::BarrierDesc bd{};
                bd.buffers = buf_barriers.empty() ? nullptr : buf_barriers.data();
                bd.bufferNum = static_cast<std::uint32_t>(buf_barriers.size());
                bd.textures = tex_barriers.empty() ? nullptr : tex_barriers.data();
                bd.textureNum = static_cast<std::uint32_t>(tex_barriers.size());
                core.CmdBarrier(**gfx_cb, bd);
            }
            if (!slice.pending_timestamps.empty() && state.timestamp_pool && state.timestamp_readback) {
                const std::uint32_t count = static_cast<std::uint32_t>(slice.pending_timestamps.size()) * 2u;
                core.CmdCopyQueries(**gfx_cb, *state.timestamp_pool, ts_pool_base, count, *state.timestamp_readback,
                                    static_cast<std::uint64_t>(ts_pool_base) * sizeof(std::uint64_t));
                slice.has_queries = true;
                slice.calibrations[0] = timeline.calibration(rhi::QueueKind::graphics);
                slice.calibrations[1] = timeline.calibration(rhi::QueueKind::compute);
                slice.calibrations[2] = timeline.calibration(rhi::QueueKind::copy);
            }
            if (!slice.pending_stats.empty() && state.stats_pool && state.stats_readback) {
                const std::uint32_t count = static_cast<std::uint32_t>(slice.pending_stats.size());
                core.CmdCopyQueries(**gfx_cb, *state.stats_pool, stat_pool_base, count, *state.stats_readback,
                                    static_cast<std::uint64_t>(stat_pool_base) * state.stats_stride);
                slice.has_pipeline_stats = true;
            }
            ATELIER_TRY(
                flush_queue(rhi::QueueKind::graphics, all_passes_end, Span{final_waits.data(), final_wait_count}));
        }
    }

    // Flush any remaining open command buffers on all queues.
    for (rhi::QueueKind q : {rhi::QueueKind::copy, rhi::QueueKind::compute, rhi::QueueKind::graphics}) {
        if (open_cmds[static_cast<std::size_t>(q)] != nullptr) {
            ATELIER_TRY(flush_queue(q, all_passes_end));
        }
    }

    slice.fence = timeline.current_point();

    // Commit persistent resource states & heap occupancy ONLY after all queue submissions have succeeded!
    if (frame_submitted_queues != 0u) {
        state.prev_frame_fences = slice.fence;
        state.prev_frame_queue_mask |= frame_submitted_queues;
        state.has_prev_frame = true;
    }
    for (std::uint32_t h = 0u; h < static_cast<std::uint32_t>(state.heaps.size()); ++h) {
        if (heap_watermarks[h] > 0u) {
            state.heaps[h].prev_frame_fences = slice.fence;
            state.heaps[h].prev_frame_queue_mask = heap_used_queues[h];
            state.heaps[h].used_in_prior_frame = true;
        }
    }
    for (auto* tex : graph.textures_) {
        if (tex->out_committed_state)
            *tex->out_committed_state = tex->current_state;
    }
    for (auto* buf : graph.buffers_) {
        if (buf->out_committed_state)
            *buf->out_committed_state = buf->current_state;
    }

    // 7. Populate owned CPU TraceSnapshot if requested (`render.trace`).
    if (options.trace) {
        TraceSnapshot& snap = *options.trace;
        snap.transient_heap_bytes = peak_aliased_sum;
        snap.unaliased_transient_bytes = unaliased_sum;
        snap.resources.reserve(graph.textures_.size() + graph.buffers_.size());

        for (std::uint32_t i = 0u; i < static_cast<std::uint32_t>(graph.textures_.size()); ++i) {
            const auto& tex = *graph.textures_[i];
            TraceResource tr{};
            tr.index = i;
            tr.name = String::from_utf8(tex.name).value_or(""_s);
            tr.is_texture = true;
            tr.imported = tex.imported;
            if (tex.imported && tex.runtime_texture) {
                const nri::TextureDesc& td = core.GetTextureDesc(*tex.runtime_texture);
                tr.extent = {td.width, std::max<std::uint32_t>(1u, td.height), std::max<std::uint32_t>(1u, td.depth)};
                tr.dxgi_format = nri::nriConvertNRIFormatToDXGI(td.format);
                tr.mip_count = std::max<std::uint32_t>(1u, td.mipNum);
                tr.layer_count = std::max<std::uint32_t>(1u, td.layerNum);
            } else {
                tr.extent = {tex.desc.width, tex.desc.height, tex.desc.depth};
                tr.dxgi_format = nri::nriConvertNRIFormatToDXGI(tex.desc.format);
                tr.mip_count = tex.desc.mip_count;
                tr.layer_count = tex.desc.layer_count;
            }
            tr.heap_index = tex.heap_index;
            tr.byte_offset = tex.byte_offset;
            tr.byte_size = tex.byte_size;
            tr.first_pass = tex.first_pass;
            tr.last_pass = tex.last_pass;
            snap.resources.add(std::move(tr));
        }

        const auto tex_count = static_cast<std::uint32_t>(graph.textures_.size());
        for (std::uint32_t i = 0u; i < static_cast<std::uint32_t>(graph.buffers_.size()); ++i) {
            const auto& buf = *graph.buffers_[i];
            TraceResource tr{};
            tr.index = tex_count + i;
            tr.name = String::from_utf8(buf.name).value_or(""_s);
            tr.is_texture = false;
            tr.imported = buf.imported;
            const std::uint64_t sz =
                (buf.imported && buf.runtime_buffer) ? core.GetBufferDesc(*buf.runtime_buffer).size : buf.desc.size;
            tr.extent = {static_cast<std::uint32_t>(std::min<std::uint64_t>(sz, 0xFFFFFFFFuLL)), 1u, 1u};
            tr.heap_index = buf.heap_index;
            tr.byte_offset = buf.byte_offset;
            tr.byte_size = buf.byte_size > 0u ? buf.byte_size : sz;
            tr.first_pass = buf.first_pass;
            tr.last_pass = buf.last_pass;
            snap.resources.add(std::move(tr));
        }

        snap.passes.reserve(graph.passes_.size());
        for (std::uint32_t p = 0u; p < static_cast<std::uint32_t>(graph.passes_.size()); ++p) {
            const auto& pass = *graph.passes_[p];
            TracePass tp{};
            tp.ordinal = p;
            tp.name = String::from_utf8(pass.name).value_or(""_s);
            tp.scope = String::from_utf8(pass.scope).value_or(""_s);
            tp.queue = pass.resolved_queue;
            tp.culled = pass.culled;
            tp.cull_reason = String::from_utf8(pass.cull_reason).value_or(""_s);
            tp.in_requested_scope = options.trace_scope.empty() || (pass.scope == options.trace_scope);
            for (auto* acc = pass.first_access; acc != nullptr; acc = acc->next) {
                TraceAccess ta{};
                ta.resource_index = acc->is_texture ? acc->index : (tex_count + acc->index);
                ta.is_write = acc->is_write;
                ta.access_bits = static_cast<std::uint32_t>(acc->access);
                ta.stage_bits = static_cast<std::uint32_t>(acc->stages);
                ta.layout = static_cast<std::uint32_t>(acc->layout);
                if (acc->is_texture) {
                    const auto& views = graph.textures_[acc->index]->views;
                    ta.bindless_index = acc->is_write ? views.uav_bindless : views.srv_bindless;
                } else {
                    const auto& views = graph.buffers_[acc->index]->views;
                    ta.bindless_index = acc->is_write ? views.uav_bindless : views.srv_bindless;
                }
                tp.accesses.add(ta);
            }
            snap.passes.add(std::move(tp));
        }
        snap.edges = std::move(dag_edges);
        snap.closed = true;
    }

    return slice.fence;
}

} // namespace atelier::rdg

#pragma once

#include "foundation/array.hpp"
#include "rhi/rhi.hpp"

#include <array>
#include <cstdint>

namespace atelier::rdg {

struct PassTimingSample {
    String name;

    rhi::QueueKind queue = rhi::QueueKind::graphics;

    double duration_ms = 0.0;

    std::uint64_t start_ns = 0;

    std::uint64_t end_ns = 0;
};

struct PipelineStatsSample {
    String pass_name;

    std::uint64_t input_vertices = 0;

    std::uint64_t input_primitives = 0;

    std::uint64_t vs_invocations = 0;

    std::uint64_t clipper_primitives = 0;

    std::uint64_t ps_invocations = 0;

    std::uint64_t cs_invocations = 0;

    std::uint64_t task_invocations = 0;

    std::uint64_t mesh_invocations = 0;

    std::uint64_t mesh_primitives = 0;
};

struct FrameGpuProfile {
    bool valid = false;

    std::uint64_t frame_id = 0;

    std::uint64_t sequence = 0;

    // Interval-union GPU time across Graphics, Async Compute, and Copy queues.
    double union_gpu_ms = 0.0;

    Array<PassTimingSample> passes;

    Array<PipelineStatsSample> pipeline_stats;
};

struct TransientStats {
    std::uint64_t allocated_heap_bytes = 0;

    std::uint64_t peak_frame_aliased_bytes = 0;

    std::uint64_t unaliased_frame_bytes = 0;

    std::uint32_t cached_placed_resources = 0;

    std::uint32_t active_heaps = 0;
};

struct TraceResource {
    std::uint32_t index = 0;

    String name;

    bool is_texture = true;

    bool imported = false;

    std::array<std::uint32_t, 3> extent{1, 1, 1};

    std::uint32_t dxgi_format = 0;

    std::uint32_t mip_count = 1;

    std::uint32_t layer_count = 1;

    std::uint32_t heap_index = 0xFFFFFFFFu;

    std::uint64_t byte_offset = 0;

    std::uint64_t byte_size = 0;

    std::uint32_t first_pass = 0xFFFFFFFFu;

    std::uint32_t last_pass = 0xFFFFFFFFu;
};

struct TraceAccess {
    std::uint32_t resource_index = 0;

    bool is_write = false;

    std::uint32_t access_bits = 0;

    std::uint32_t stage_bits = 0;

    std::uint32_t layout = 0;

    std::uint32_t bindless_index = 0xFFFFFFFFu;
};

struct TracePass {
    std::uint32_t ordinal = 0;

    String name;

    String scope;

    rhi::QueueKind queue = rhi::QueueKind::graphics;

    bool culled = false;

    String cull_reason;

    bool in_requested_scope = true;

    Array<TraceAccess> accesses;
};

struct TraceEdge {
    std::uint32_t from_pass = 0;

    std::uint32_t to_pass = 0;

    std::uint32_t resource_index = 0;

    bool cross_queue = false;
};

// Owned CPU-only snapshot of one RDG compilation and recording attempt.
// Carries zero native GPU handles, Runtime roots, or device borrows.
struct TraceSnapshot {
    std::uint64_t recording_sequence = 0;

    String requested_scope;

    bool entered = false;

    bool closed = false;

    bool interrupted = false;

    std::uint64_t transient_heap_bytes = 0;

    std::uint64_t unaliased_transient_bytes = 0;

    Array<TraceResource> resources;

    Array<TracePass> passes;

    Array<TraceEdge> edges;
};

} // namespace atelier::rdg

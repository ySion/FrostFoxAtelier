#include "rhi/rhi.hpp"
#include "support.hpp"
#include "shaders.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace rhi_acceptance {
namespace {
using namespace atelier::rhi;

template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void complete(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

Context open_context() {
    ContextOptions options;
    options.validation = Validation::gpu;
    return take(Context::open(options));
}

void root_budget_graphics() {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.kind = PipelineKind::vertex;
    // Each stage has only 32 DWORDs, but together with the SRV table costs 65.
    // The shared field deliberately has different reflected offsets per stage.
    desc.source.vs = R"(
uint4 shared_value; uint4 vertex_values[7];
struct Output { float4 position : SV_Position; nointerpolation uint value : COLOR; };
Output main(uint id : SV_VertexID) {
    Output output;
    float2 p = id == 0 ? float2(-1,-1) : id == 1 ? float2(-1,3) : float2(3,-1);
    output.position = float4(p,0.25,1);
    output.value = vertex_values[6].x + shared_value.x;
    return output;
}
)";
    desc.source.ps = R"(
uint4 pixel_values[7]; uint4 shared_value;
StructuredBuffer<uint> extra;
float4 main(float4 position : SV_Position, nointerpolation uint value : COLOR) : SV_Target {
    return float4(float(value), float(pixel_values[6].x + shared_value.y), float(extra[0]), 255) / 255.0;
}
)";
    check(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM));
    check(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    auto pipeline = take(owner.prepare(desc));
    const std::uint32_t blue = 37;
    auto extra = gfxCreateBuffer<std::uint32_t>(gfx, 1, &blue);
    auto readback = gfxCreateBuffer(gfx, 64 * 32 * 4, nullptr, kGfxCpuAccess_Read);
    auto texture = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    require(extra && readback && texture, "root budget graphics resources unavailable");
    check(gfxProgramSetParameter(gfx, pipeline.program(), "extra", extra));
    for (std::uint32_t iteration = 0; iteration < 4; ++iteration) {
        const std::array<std::uint32_t, 4> shared = {iteration + 3, iteration + 7, 0, 0};
        std::array<std::uint32_t, 28> vertex_values{};
        std::array<std::uint32_t, 28> pixel_values{};
        vertex_values[24] = 61 + iteration;
        pixel_values[24] = 103 + iteration;
        check(gfxProgramSetParameter(gfx, pipeline.program(), "shared_value", shared));
        check(gfxProgramSetParameter(gfx, pipeline.program(), "vertex_values", vertex_values));
        check(gfxProgramSetParameter(gfx, pipeline.program(), "pixel_values", pixel_values));
        check(gfxCommandBindColorTarget(gfx, 0, texture));
        check(gfxCommandBindKernel(gfx, pipeline.kernel()));
        check(gfxCommandDraw(gfx, 3));
        check(gfxCommandCopyTextureToBuffer(gfx, readback, texture));
        complete(owner.finish());
        const auto* pixels = gfxBufferGetData<std::uint8_t>(gfx, readback);
        require(pixels != nullptr, "root budget graphics readback unavailable");
        for (std::size_t i = 0; i < 64 * 32; ++i)
            require(pixels[4 * i] == vertex_values[24] + shared[0] &&
                        pixels[4 * i + 1] == pixel_values[24] + shared[1] && pixels[4 * i + 2] == blue &&
                        pixels[4 * i + 3] == 255,
                    "cross-stage root constant fallback corrupted GPU pixels");
    }
    check(gfxDestroyBuffer(gfx, extra));
    check(gfxDestroyBuffer(gfx, readback));
    check(gfxDestroyTexture(gfx, texture));
    pipeline = {};
    complete(owner.close());
    check_debug(debug.Get());
    std::puts("root-budget-graphics: VS/PS-globals=32+32 SRV-table=1 shared-offsets/updates/pixels=PASS");
}

void root_budget_compute() {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.source.cs = R"(
uint4 values[16];
RWStructuredBuffer<uint> output_value;
[numthreads(1,1,1)] void main() { output_value[0] = values[15].x; }
)";
    auto pipeline = take(owner.prepare(desc));
    auto output = gfxCreateBuffer<std::uint32_t>(gfx, 1);
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, 1, nullptr, kGfxCpuAccess_Read);
    require(output && readback, "root budget compute resources unavailable");
    check(gfxProgramSetParameter(gfx, pipeline.program(), "output_value", output));
    for (std::uint32_t iteration = 0; iteration < 4; ++iteration) {
        std::array<std::uint32_t, 64> values{};
        values[60] = 121 + iteration;
        check(gfxProgramSetParameter(gfx, pipeline.program(), "values", values));
        check(gfxCommandBindKernel(gfx, pipeline.kernel()));
        check(gfxCommandDispatch(gfx, 1, 1, 1));
        check(gfxCommandCopyBuffer(gfx, readback, output));
        complete(owner.finish());
        const auto* value = gfxBufferGetData<std::uint32_t>(gfx, readback);
        require(value && *value == values[60], "256-byte globals plus UAV fallback corrupted GPU output");
    }
    check(gfxDestroyBuffer(gfx, output));
    check(gfxDestroyBuffer(gfx, readback));
    pipeline = {};
    complete(owner.close());
    check_debug(debug.Get());
    std::puts("root-budget-compute: globals=64 UAV-table=1 updates/GPU-values=PASS");
}

void raw_buffer_views() {
    auto owner = open_context();
    require(owner.info().gpu_validation, "RAW SRV regression requires GPU validation");
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.source.cs = R"(
ByteAddressBuffer raw_full, raw_range, raw_tail;
StructuredBuffer<uint3> structured_full, structured_range;
RWStructuredBuffer<uint4> result;
[numthreads(1,1,1)] void main() {
    result[0] = raw_full.Load4(4);
    result[1] = uint4(raw_full.Load3(32), raw_full.Load(80));
    result[2] = raw_range.Load4(4);
    result[3] = uint4(raw_range.Load3(0), raw_range.Load(20));
    result[4] = uint4(structured_full[6], structured_full[0].y);
    result[5] = uint4(structured_range[1], structured_range[0].z);
    uint full_bytes, range_bytes, count, stride;
    raw_full.GetDimensions(full_bytes);
    raw_range.GetDimensions(range_bytes);
    structured_range.GetDimensions(count, stride);
    result[6] = uint4(full_bytes, range_bytes, count, stride);
    uint tail_bytes;
    raw_tail.GetDimensions(tail_bytes);
    result[7] = uint4(raw_tail.Load(0), tail_bytes, 0, 0);
}
)";
    auto pipeline = take(owner.prepare(desc));
    using Triple = std::array<std::uint32_t, 3>;
    using Quad = std::array<std::uint32_t, 4>;
    const std::array<Triple, 7> data{{{11, 23, 47},
                                      {71, 101, 149},
                                      {211, 307, 401},
                                      {503, 601, 709},
                                      {809, 907, 1009},
                                      {1103, 1201, 1301},
                                      {1409, 1511, 1601}}};
    // Both views deliberately retain a logical stride of 12. RAW addressing
    // uses four-byte words, with the view base aligned to 16 bytes for D3D12.
    auto full = gfxCreateBuffer<Triple>(gfx, 7, data.data());
    auto range = gfxCreateBufferRange(gfx, full, 48, 24);
    auto tail = gfxCreateBufferRange(gfx, full, 80, 4); // One RAW word, smaller than the logical stride.
    auto result = gfxCreateBuffer<Quad>(gfx, 8);
    auto readback = gfxCreateBuffer<Quad>(gfx, 8, nullptr, kGfxCpuAccess_Read);
    require(full && range && tail && result && readback, "RAW/Structured SRV resources unavailable");
    check(gfxProgramSetParameter(gfx, pipeline.program(), "raw_full", full));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "raw_range", range));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "raw_tail", tail));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "structured_full", full));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "structured_range", range));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "result", result));
    check(gfxCommandBindKernel(gfx, pipeline.kernel()));
    check(gfxCommandDispatch(gfx, 1, 1, 1));
    check(gfxCommandCopyBuffer(gfx, readback, result));
    const auto finished = owner.finish();
    if (!finished)
        check_debug(debug.Get());
    complete(finished);
    const auto* values = gfxBufferGetData<Quad>(gfx, readback);
    require(values != nullptr, "RAW/Structured SRV readback unavailable");
    const std::array<Quad, 8> expected{{{23, 47, 71, 101},
                                        {401, 503, 601, 1601},
                                        {907, 1009, 1103, 1201},
                                        {809, 907, 1009, 1301},
                                        {1409, 1511, 1601, 23},
                                        {1103, 1201, 1301, 1009},
                                        {84, 24, 2, 12},
                                        {1601, 4, 0, 0}}};
    bool correct = true;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        std::printf("raw-buffer-views[%zu]: actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n", i, values[i][0], values[i][1],
                    values[i][2], values[i][3], expected[i][0], expected[i][1], expected[i][2], expected[i][3]);
        correct &= values[i] == expected[i];
    }
    for (auto buffer : {tail, range, full, result, readback})
        check(gfxDestroyBuffer(gfx, buffer));
    pipeline = {};
    complete(owner.close());
    check_debug(debug.Get());
    require(correct, "RAW byte addressing, range bounds or Structured control disagreed");
    std::puts(
        "raw-buffer-views: stride=12 range-offset=48 range-bytes=24 tail-offset=80 tail-bytes=4 raw/structured GPU-values/GBV=PASS");
}

void indirect_constants() {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.source.cs = R"(
uint gfx_DispatchID;
cbuffer LargeData { uint4 large[32]; };
RWStructuredBuffer<uint> output_value;
[numthreads(1,1,1)] void main() { output_value[0] = large[31].x + gfx_DispatchID; }
)";
    auto pipeline = take(owner.prepare(desc));
    const std::array<std::uint32_t, 3> dispatch = {1, 1, 1};
    auto args = gfxCreateBuffer(gfx, sizeof(dispatch), dispatch.data(), kGfxCpuAccess_Write);
    auto output = gfxCreateBuffer<std::uint32_t>(gfx, 1);
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, 1, nullptr, kGfxCpuAccess_Read);
    auto filler = gfxCreateBuffer(gfx, 4, nullptr, kGfxCpuAccess_Write);
    require(args && output && readback && filler, "indirect resources unavailable");
    // Four live buffers fill the pinned gfx table before its first constant pool.
    // Every following finish must make that pool reusable without advancing frames.
    for (std::uint32_t iteration = 0; iteration < 64; ++iteration) {
        std::array<std::uint32_t, 128> large{};
        large[124] = 73 + iteration;
        check(gfxProgramSetParameter(gfx, pipeline.program(), "LargeData", large));
        check(gfxProgramSetParameter(gfx, pipeline.program(), "gfx_DispatchID", std::uint32_t{0}));
        check(gfxProgramSetParameter(gfx, pipeline.program(), "output_value", output));
        check(gfxCommandBindKernel(gfx, pipeline.kernel()));
        if (iteration % 2 == 0)
            check(gfxCommandDispatchIndirect(gfx, args));
        else
            check(gfxCommandMultiDispatchIndirect(gfx, args, 1));
        check(gfxCommandCopyBuffer(gfx, readback, output));
        complete(owner.finish());
        const auto* value = gfxBufferGetData<std::uint32_t>(gfx, readback);
        require(value && *value == large[124], "indirect constants or finish-only reuse corrupted output");
    }
    for (auto buffer : {args, output, readback, filler})
        check(gfxDestroyBuffer(gfx, buffer));
    pipeline = {};
    complete(owner.close());
    check_debug(debug.Get());
    std::puts("indirect-constants: single/multi-dispatch=PASS finish-only-iterations=64 GPU-values=PASS");
}

void indirect_graphics(PipelineKind kind, bool indexed) {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.kind = kind;
    if (kind == PipelineKind::mesh)
        desc.source.ms = mesh_source;
    else
        desc.source.vs = vertex_source;
    desc.source.ps = R"(
uint4 large[32];
float4 main() : SV_Target { return float4(float(large[31].x) / 255.0, 0, 0, 1); }
)";
    check(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM));
    check(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    auto pipeline = take(owner.prepare(desc));
    const std::array<std::uint32_t, 5> arguments = kind == PipelineKind::mesh
                                                       ? std::array<std::uint32_t, 5>{1, 1, 1, 0, 0}
                                                       : std::array<std::uint32_t, 5>{3, 1, 0, 0, 0};
    const std::array<std::uint32_t, 3> triangle = {0, 1, 2};
    auto args = gfxCreateBuffer(gfx, sizeof(arguments), arguments.data(), kGfxCpuAccess_Write);
    auto indices = gfxCreateBuffer<std::uint32_t>(gfx, 3, triangle.data(), kGfxCpuAccess_Write);
    auto readback = gfxCreateBuffer(gfx, 64 * 32 * 4, nullptr, kGfxCpuAccess_Read);
    auto filler = gfxCreateBuffer(gfx, 4, nullptr, kGfxCpuAccess_Write);
    auto texture = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    require(args && indices && readback && filler && texture, "indirect graphics resources unavailable");
    std::array<std::uint32_t, 128> large{};
    large[124] = 91;
    check(gfxProgramSetParameter(gfx, pipeline.program(), "large", large));
    check(gfxCommandClearTexture(gfx, texture));
    check(gfxCommandBindColorTarget(gfx, 0, texture));
    check(gfxCommandBindKernel(gfx, pipeline.kernel()));
    if (kind == PipelineKind::mesh)
        check(gfxCommandDrawMeshIndirect(gfx, args));
    else if (indexed) {
        check(gfxCommandBindIndexBuffer(gfx, indices));
        check(gfxCommandMultiDrawIndexedIndirect(gfx, args, 1));
    } else
        check(gfxCommandMultiDrawIndirect(gfx, args, 1));
    check(gfxCommandCopyTextureToBuffer(gfx, readback, texture));
    complete(owner.finish());
    const auto* pixels = gfxBufferGetData<std::uint8_t>(gfx, readback);
    require(pixels != nullptr, "indirect graphics readback unavailable");
    for (std::size_t i = 0; i < 64 * 32; ++i)
        require(pixels[4 * i] == 91 && pixels[4 * i + 1] == 0 && pixels[4 * i + 2] == 0 && pixels[4 * i + 3] == 255,
                "indirect graphics large constants mismatch");
    for (auto buffer : {args, indices, readback, filler})
        check(gfxDestroyBuffer(gfx, buffer));
    check(gfxDestroyTexture(gfx, texture));
    pipeline = {};
    complete(owner.close());
    check_debug(debug.Get());
    std::printf("indirect-graphics: kind=%s indexed=%d pixels=2048 PASS\n",
                kind == PipelineKind::mesh ? "mesh" : "vertex", indexed);
}

void shared_buffer_reads_and_writes() {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc raster;
    raster.kind = PipelineKind::vertex;
    raster.source.vs = R"(
struct Output { float4 position : SV_Position; nointerpolation uint value : COLOR; };
Output main(uint id : SV_VertexID, uint value : DATA) {
    Output o;
    o.position = float4(id == 0 ? float2(-1,-1) : id == 1 ? float2(-1,3) : float2(3,-1), 0, 1);
    o.value = value; return o;
})";
    raster.source.ps = R"(
StructuredBuffer<uint> indices, values;
float4 main(float4 position : SV_Position, nointerpolation uint value : COLOR) : SV_Target {
    return float4(value, values[2], indices[0] + 1, 255) / 255.0;
})";
    check(gfxDrawStateSetColorTarget(raster.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM));
    check(gfxDrawStateSetCullMode(raster.draw_state, D3D12_CULL_MODE_NONE));
    auto draw = take(owner.prepare(raster));
    PipelineDesc compute;
    compute.source.cs = R"(
StructuredBuffer<uint> indices, values;
RWStructuredBuffer<uint> result;
[numthreads(1,1,1)] void main() { result[0] = values[indices[0]] + values[2] * 256; }
)";
    auto read = take(owner.prepare(compute));
    compute.source.cs = R"(
RWStructuredBuffer<uint> indices, values;
[numthreads(1,1,1)] void main() {
    for (uint i = 0; i < 3; ++i) { indices[i] = (indices[i] + 1) % 3; values[i] += 1; }
})";
    auto write = take(owner.prepare(compute));
    const std::array<std::uint32_t, 3> triangle{2, 0, 1};
    const std::array<std::uint32_t, 3> data{17, 31, 43};
    auto indices = gfxCreateBuffer<std::uint32_t>(gfx, 3, triangle.data());
    auto values = gfxCreateBuffer<std::uint32_t>(gfx, 3, data.data());
    auto index_view = gfxCreateBufferRange(gfx, indices, 0, sizeof(triangle));
    auto value_view = gfxCreateBufferRange(gfx, values, 0, sizeof(data));
    auto upload = gfxCreateBuffer<std::uint32_t>(gfx, 3, data.data(), kGfxCpuAccess_Write);
    auto other = gfxCreateBuffer<std::uint32_t>(gfx, 3, triangle.data(), kGfxCpuAccess_Write);
    auto result = gfxCreateBuffer<std::uint32_t>(gfx, 1);
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, 1, nullptr, kGfxCpuAccess_Read);
    auto pixels = gfxCreateBuffer(gfx, 64 * 32 * 4, nullptr, kGfxCpuAccess_Read);
    auto target = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    require(indices && values && index_view && value_view && upload && other && result && readback && pixels && target,
            "shared IA/SRV buffer resources unavailable");
    for (auto p : {draw.program(), read.program(), write.program()}) {
        check(gfxProgramSetParameter(gfx, p, "indices", index_view));
        check(gfxProgramSetParameter(gfx, p, "values", value_view));
    }
    check(gfxProgramSetParameter(gfx, read.program(), "result", result));
    check(gfxCommandBindIndexBuffer(gfx, indices));
    check(gfxCommandBindVertexBuffer(gfx, values));
    for (std::uint32_t iteration = 0; iteration < 4; ++iteration) {
        // Install A before writing through its range aliases in this same list.
        // finish() between these draws would mask a missing IA reinstall.
        check(gfxCommandBindColorTarget(gfx, 0, target));
        check(gfxCommandBindKernel(gfx, draw.kernel()));
        check(gfxCommandDrawIndexed(gfx, 3));
        if (iteration % 2 != 0) {
            // Only change the logical binds to B; no draw installs B in IA.
            check(gfxCommandBindIndexBuffer(gfx, other));
            check(gfxCommandBindVertexBuffer(gfx, other));
        }
        check(gfxCommandCopyBuffer(gfx, value_view, upload));
        check(gfxCommandBindKernel(gfx, write.kernel()));
        check(gfxCommandDispatch(gfx, 1, 1, 1));
        check(gfxCommandDispatch(gfx, 1, 1, 1)); // Ordered UAV read-modify-write.
        check(gfxCommandBindIndexBuffer(gfx, indices));
        check(gfxCommandBindVertexBuffer(gfx, values));
        check(gfxCommandBindKernel(gfx, draw.kernel()));
        check(gfxCommandDrawIndexed(gfx, 3));
        check(gfxCommandBindKernel(gfx, read.kernel()));
        check(gfxCommandDispatch(gfx, 1, 1, 1));
        check(gfxCommandCopyBuffer(gfx, readback, result));
        check(gfxCommandBindKernel(gfx, draw.kernel()));
        check(gfxCommandDrawIndexed(gfx, 3)); // IA and pixel reads after a compute read.
        check(gfxCommandCopyTextureToBuffer(gfx, pixels, target));
        complete(owner.finish());
        const auto first = (2 + 2 * (iteration + 1)) % 3;
        const auto red = data[first] + 2;
        const auto green = data[2] + 2;
        const auto blue = first + 1;
        const auto* color = gfxBufferGetData<std::uint8_t>(gfx, pixels);
        const auto* word = gfxBufferGetData<std::uint32_t>(gfx, readback);
        require(color && word && *word == red + green * 256, "shared buffer compute read saw stale writes");
        for (std::size_t i = 0; i < 64 * 32; ++i)
            require(color[4 * i] == red && color[4 * i + 1] == green && color[4 * i + 2] == blue &&
                        color[4 * i + 3] == 255,
                    "shared IA/SRV reads or post-write IA reinstall corrupted pixels");
    }
    for (auto buffer : {index_view, value_view, indices, values, upload, other, result, readback, pixels})
        check(gfxDestroyBuffer(gfx, buffer));
    check(gfxDestroyTexture(gfx, target));
    draw = {};
    read = {};
    write = {};
    complete(owner.close());
    check_debug(debug.Get());
    std::puts(
        "shared-buffer-reads: same-list draw/bind-B/alias-write/rebind-A/draw COPY_DEST/UAV/UAV-to-read pixels=PASS");
}

void texture_ranges() {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    constexpr std::uint32_t bytes = 64 * 32 * 4;
    constexpr std::uint32_t source_offset = 256;
    constexpr std::uint32_t target_offset = 512;
    constexpr std::uint32_t trailer = 512;
    std::vector<std::uint8_t> upload_data(source_offset + bytes, 0x5a);
    for (std::uint32_t i = 0; i < bytes; ++i)
        upload_data[source_offset + i] = static_cast<std::uint8_t>((i * 13 + 7) % 251);
    auto upload = gfxCreateBuffer(gfx, upload_data.size(), upload_data.data(), kGfxCpuAccess_Write);
    auto source = gfxCreateBufferRange(gfx, upload, source_offset, bytes);
    auto filler_a = gfxCreateBuffer(gfx, 4, nullptr, kGfxCpuAccess_Write);
    auto filler_b = gfxCreateBuffer(gfx, 4, nullptr, kGfxCpuAccess_Write);
    auto texture = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM);
    require(upload && source && filler_a && filler_b && texture, "range upload resources unavailable");
    // The unaligned range uses upload scratch on hardware requiring aligned footprints.
    check(gfxCommandCopyBufferToTexture(gfx, texture, source));
    const std::vector<std::uint8_t> sentinel(target_offset + bytes + trailer, 0x5a);
    auto sentinel_upload = gfxCreateBuffer(gfx, sentinel.size(), sentinel.data(), kGfxCpuAccess_Write);
    auto readback = gfxCreateBuffer(gfx, sentinel.size(), nullptr, kGfxCpuAccess_Read);
    auto target = gfxCreateBufferRange(gfx, readback, target_offset, bytes);
    require(sentinel_upload && readback && target, "range readback resources unavailable");
    check(gfxCommandCopyBuffer(gfx, readback, sentinel_upload));
    check(gfxCommandCopyTextureToBuffer(gfx, target, texture));
    complete(owner.finish());
    const auto* data = gfxBufferGetData<std::uint8_t>(gfx, readback);
    require(data != nullptr, "range readback unavailable");
    for (std::uint32_t i = 0; i < sentinel.size(); ++i) {
        const auto expected = i < target_offset || i >= target_offset + bytes
                                  ? std::uint8_t{0x5a}
                                  : upload_data[source_offset + i - target_offset];
        require(data[i] == expected, "range copy corrupted image or surrounding sentinel");
    }
    for (auto buffer : {source, upload, filler_a, filler_b, target, readback, sentinel_upload})
        check(gfxDestroyBuffer(gfx, buffer));
    check(gfxDestroyTexture(gfx, texture));
    complete(owner.close());
    check_debug(debug.Get());
    std::puts("texture-ranges: upload-offset=256 readback-offset=512 image-bytes=8192 prefix/trailer=PASS");
}

void window_target_sync() {
    // This fixture owns its window and never activates or touches another one.
    struct Window {
        HINSTANCE instance = GetModuleHandleW(nullptr);

        HWND handle{};

        Window() {
            WNDCLASSW type{};
            type.hInstance = instance;
            type.lpfnWndProc = DefWindowProcW;
            type.lpszClassName = L"AtelierRhiTargetSyncAcceptance";
            require(RegisterClassW(&type) != 0, "window class registration failed");
            handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName,
                                     L"RHI target synchronization acceptance", WS_POPUP, 32, 32, 160, 96, nullptr,
                                     nullptr, instance, nullptr);
            require(handle != nullptr, "acceptance window creation failed");
        }

        ~Window() {
            DestroyWindow(handle);
            UnregisterClassW(L"AtelierRhiTargetSyncAcceptance", instance);
        }

        void size(int width, int height) const {
            require(SetWindowPos(handle, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) !=
                        FALSE,
                    "window resize failed");
        }
    } window;
    ContextOptions options;
    options.window = window.handle;
    options.rgba8_window = true;
    options.validation = Validation::gpu;
    auto owner = take(Context::open(options));
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.kind = PipelineKind::vertex;
    desc.source.vs = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 0 ? float2(-1,-1) : id == 1 ? float2(-1,3) : float2(3,-1), 0, 1);
}
)";
    desc.source.ps = "float4 main() : SV_Target { return float4(1,0,1,1); }";
    check(gfxDrawStateSetColorTarget(desc.draw_state, 0, gfxGetBackBufferFormat(gfx)));
    check(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    auto pipeline = take(owner.prepare(desc));
    auto automatic =
        gfxCreateTexture2D(gfx, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    require(static_cast<bool>(automatic), "auto-sized texture creation failed");
    struct Capture {
        GfxBuffer buffer;

        std::uint32_t width;

        std::uint32_t height;

        std::uint32_t pitch;
    };
    std::vector<Capture> captures;
    constexpr std::array sizes{std::pair{641u, 73u}, std::pair{257u, 181u}, std::pair{703u, 97u},
                               std::pair{320u, 96u}, std::pair{641u, 73u},  std::pair{257u, 181u}};
    for (const auto [width, height] : sizes) {
        ShowWindow(window.handle, SW_HIDE);
        window.size(static_cast<int>(width), static_cast<int>(height));
        const auto sequence = gfxGetRecordingFrameSequence(gfx);
        const auto last_present = gfxGetLastPresentResult(gfx);
        const auto target = take(owner.sync_window_target());
        require(target.windowed && !target.drawable && target.width == width && target.height == height,
                "hidden window did not synchronize before its first draw");
        require(gfxGetRecordingFrameSequence(gfx) == sequence && gfxGetLastPresentResult(gfx) == last_present,
                "target synchronization presented or advanced frame identity");
        const auto resource_desc = gfxTextureGetResource(gfx, automatic)->GetDesc();
        require(resource_desc.Width == width && resource_desc.Height == height,
                "auto-sized texture did not follow pre-draw synchronization");
        const auto repeated = take(owner.sync_window_target());
        require(repeated.width == width && repeated.height == height, "same-size synchronization changed the target");
        ShowWindow(window.handle, SW_SHOWNOACTIVATE);
        require(take(owner.sync_window_target()).drawable, "shown acceptance window is not drawable");

        const auto pitch = (width * 4 + 255) & ~255u;
        auto readback = gfxCreateBuffer(gfx, static_cast<std::uint64_t>(pitch) * height, nullptr, kGfxCpuAccess_Read);
        require(static_cast<bool>(readback), "window readback creation failed");
        captures.push_back({readback, width, height, pitch});
        check(gfxCommandClearTexture(gfx, automatic)); // Previous texture may still be in flight at the next resize.
        check(gfxCommandClearBackBuffer(gfx));
        check(gfxCommandBindKernel(gfx, pipeline.kernel()));
        check(gfxCommandDraw(gfx, 3));
        check(gfxCommandCopyBackBufferToBuffer(gfx, readback, pitch));
        FrameInfo frame;
        complete(owner.frame(false, &frame));
        const auto& cpu = frame.cpu;
        double stage_sum = 0;
        for (double us : {cpu.prepare_us, cpu.submit_us, cpu.present_us, cpu.wait_us, cpu.resize_us, cpu.recycle_us,
                          cpu.reclaim_us}) {
            require(std::isfinite(us) && us >= 0, "frame CPU timing is invalid");
            stage_sum += us;
        }
        require(cpu.frame_sequence == sequence && std::isfinite(cpu.total_us) &&
                    std::abs(stage_sum - cpu.total_us) <= 1e-3,
                "frame CPU timings do not partition their submitted frame");
        require(frame.rendered.width == width && frame.rendered.height == height && frame.next.width == width &&
                    frame.next.height == height && gfxGetRecordingFrameSequence(gfx) == sequence + 1,
                "first Present after resize used an obsolete target");
    }
    complete(owner.finish());
    std::uint64_t pixels = 0;
    for (const auto& capture : captures) {
        const auto* bytes = gfxBufferGetData<std::uint8_t>(gfx, capture.buffer);
        require(bytes != nullptr, "window readback unavailable");
        for (std::uint32_t y = 0; y < capture.height; ++y)
            for (std::uint32_t x = 0; x < capture.width; ++x) {
                const auto* pixel = bytes + y * capture.pitch + x * 4;
                require(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255,
                        "resized viewport did not fill the first presented image");
                ++pixels;
            }
        check(gfxDestroyBuffer(gfx, capture.buffer));
    }

    // A blocked real queue makes any accidental no-op drain fail deterministically.
    ComPtr<ID3D12Fence> gate;
    check_hr(gfxGetDevice(gfx)->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    struct ReleaseGate {
        ID3D12Fence* fence;

        ~ReleaseGate() { fence->Signal(1); }
    } release{gate.Get()};
    check_hr(gfxGetCommandQueue(gfx)->Wait(gate.Get(), 1));
    check(gfxSetWaitTimeout(gfx, 5));
    const auto sequence = gfxGetRecordingFrameSequence(gfx);
    for (unsigned repeat = 0; repeat < 2; ++repeat)
        (void)take(owner.sync_window_target());
    ShowWindow(window.handle, SW_HIDE);
    require(!take(owner.sync_window_target()).drawable, "hidden no-op target became drawable");
    window.size(0, 0);
    const auto zero = take(owner.sync_window_target());
    require(!zero.drawable && zero.width == sizes.back().first && zero.height == sizes.back().second,
            "zero client size resized the swap chain");
    window.size(401, 109);
    ShowWindow(window.handle, SW_SHOWMINNOACTIVE);
    require(IsIconic(window.handle) != FALSE && !take(owner.sync_window_target()).drawable,
            "minimized target synchronization did not skip resize");
    require(gfxGetRecordingFrameSequence(gfx) == sequence, "no-op synchronization advanced a frame");
    ShowWindow(window.handle, SW_SHOWNOACTIVATE);
    ShowWindow(window.handle, SW_HIDE);
    window.size(401, 109);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        const auto failed = owner.sync_window_target();
        require(!failed && failed.error().code == ErrorCode::timeout, "resize timeout did not retain terminal failure");
    }
    require(!owner.borrow(), "failed resize remained borrowable");
    check_hr(gate->Signal(1));
    check(gfxSetWaitTimeout(gfx, 10000)); // Cleanup budget only; does not clear failure.
    check(gfxDestroyTexture(gfx, automatic));
    pipeline = {};
    complete(owner.close());
    complete(owner.close());
    check_debug(debug.Get());
    std::printf(
        "window-target-sync: hidden-resizes=6 single-present=PASS image-pixels=%llu no-op/no-wait=PASS resize-timeout/drain-only=PASS debug-warnings=0 debug-errors=0\n",
        static_cast<unsigned long long>(pixels));
}
} // namespace

void resource_cases() {
    root_budget_graphics();
    root_budget_compute();
    raw_buffer_views();
    indirect_constants();
    indirect_graphics(PipelineKind::vertex, false);
    indirect_graphics(PipelineKind::vertex, true);
    indirect_graphics(PipelineKind::mesh, false);
    shared_buffer_reads_and_writes();
    texture_ranges();
    window_target_sync();
}
} // namespace rhi_acceptance

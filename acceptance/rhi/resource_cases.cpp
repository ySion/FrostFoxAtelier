#include "rhi/rhi.hpp"
#include "support.hpp"
#include "shaders.hpp"

#include <array>
#include <utility>

namespace rhi_acceptance {
namespace {
using namespace atelier::rhi;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void complete(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
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
        std::array<std::uint32_t, 28> vertex_values{}, pixel_values{};
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
                    pixels[4 * i + 1] == pixel_values[24] + shared[1] &&
                    pixels[4 * i + 2] == blue && pixels[4 * i + 3] == 255,
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
        if (iteration % 2 == 0) check(gfxCommandDispatchIndirect(gfx, args));
        else check(gfxCommandMultiDispatchIndirect(gfx, args, 1));
        check(gfxCommandCopyBuffer(gfx, readback, output));
        complete(owner.finish());
        const auto* value = gfxBufferGetData<std::uint32_t>(gfx, readback);
        require(value && *value == large[124], "indirect constants or finish-only reuse corrupted output");
    }
    for (auto buffer : {args, output, readback, filler}) check(gfxDestroyBuffer(gfx, buffer));
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
    if (kind == PipelineKind::mesh) desc.source.ms = mesh_source;
    else desc.source.vs = vertex_source;
    desc.source.ps = R"(
uint4 large[32];
float4 main() : SV_Target { return float4(float(large[31].x) / 255.0, 0, 0, 1); }
)";
    check(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM));
    check(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    auto pipeline = take(owner.prepare(desc));
    const std::array<std::uint32_t, 5> arguments = kind == PipelineKind::mesh
        ? std::array<std::uint32_t, 5>{1, 1, 1, 0, 0} : std::array<std::uint32_t, 5>{3, 1, 0, 0, 0};
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
    if (kind == PipelineKind::mesh) check(gfxCommandDrawMeshIndirect(gfx, args));
    else if (indexed) {
        check(gfxCommandBindIndexBuffer(gfx, indices));
        check(gfxCommandMultiDrawIndexedIndirect(gfx, args, 1));
    } else check(gfxCommandMultiDrawIndirect(gfx, args, 1));
    check(gfxCommandCopyTextureToBuffer(gfx, readback, texture));
    complete(owner.finish());
    const auto* pixels = gfxBufferGetData<std::uint8_t>(gfx, readback);
    require(pixels != nullptr, "indirect graphics readback unavailable");
    for (std::size_t i = 0; i < 64 * 32; ++i)
        require(pixels[4 * i] == 91 && pixels[4 * i + 1] == 0 && pixels[4 * i + 2] == 0 && pixels[4 * i + 3] == 255,
                "indirect graphics large constants mismatch");
    for (auto buffer : {args, indices, readback, filler}) check(gfxDestroyBuffer(gfx, buffer));
    check(gfxDestroyTexture(gfx, texture));
    pipeline = {};
    complete(owner.close());
    check_debug(debug.Get());
    std::printf("indirect-graphics: kind=%s indexed=%d pixels=2048 PASS\n",
                kind == PipelineKind::mesh ? "mesh" : "vertex", indexed);
}

void texture_ranges() {
    auto owner = open_context();
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    constexpr std::uint32_t bytes = 64 * 32 * 4, source_offset = 256, target_offset = 512, trailer = 512;
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
            ? std::uint8_t{0x5a} : upload_data[source_offset + i - target_offset];
        require(data[i] == expected, "range copy corrupted image or surrounding sentinel");
    }
    for (auto buffer : {source, upload, filler_a, filler_b, target, readback, sentinel_upload})
        check(gfxDestroyBuffer(gfx, buffer));
    check(gfxDestroyTexture(gfx, texture));
    complete(owner.close());
    check_debug(debug.Get());
    std::puts("texture-ranges: upload-offset=256 readback-offset=512 image-bytes=8192 prefix/trailer=PASS");
}
} // namespace

void resource_cases() {
    root_budget_graphics();
    root_budget_compute();
    indirect_constants();
    indirect_graphics(PipelineKind::vertex, false);
    indirect_graphics(PipelineKind::vertex, true);
    indirect_graphics(PipelineKind::mesh, false);
    texture_ranges();
}
} // namespace rhi_acceptance

#include "rhi/rhi.hpp"
#include "support.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace rhi_acceptance {
namespace {
using namespace atelier::rhi;

constexpr char plane_vs[] = R"(

float plane_base; float plane_slope;
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = id == 0 ? float2(-1,-1) : id == 1 ? float2(-1,3) : float2(3,-1);
    return float4(p, plane_base + plane_slope * (p.x * .5 + .5), 1);
})";
PipelineDesc depth_desc() {
    PipelineDesc desc;
    desc.kind = PipelineKind::vertex;
    desc.source.vs = plane_vs;
    check(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT));
    check(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_ALWAYS));
    check(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    return desc;
}

void set_plane(GfxContext gfx, const Pipeline& pipeline, float base, float slope) {
    check(gfxProgramSetParameter(gfx, pipeline.program(), "plane_base", base));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "plane_slope", slope));
    check(gfxCommandBindKernel(gfx, pipeline.kernel()));
}

bool contains(const D3D12_RECT& rect, std::uint32_t x, std::uint32_t y) {
    return x >= static_cast<std::uint32_t>(rect.left) && x < static_cast<std::uint32_t>(rect.right) &&
           y >= static_cast<std::uint32_t>(rect.top) && y < static_cast<std::uint32_t>(rect.bottom);
}

void atlas_rectangles(Context& owner) {
    const auto gfx = owner.borrow();
    auto depth_draw = take(owner.prepare(depth_desc()));
    PipelineDesc color_desc;
    color_desc.kind = PipelineKind::vertex;
    color_desc.source.vs = plane_vs;
    color_desc.source.ps = "float4 main():SV_Target{return float4(.125,.25,.375,.5);}";
    check(gfxDrawStateSetColorTarget(color_desc.draw_state, 0, DXGI_FORMAT_R32G32B32A32_FLOAT));
    check(gfxDrawStateSetCullMode(color_desc.draw_state, D3D12_CULL_MODE_NONE));
    auto color_draw = take(owner.prepare(color_desc));
    const std::array<float, 4> depth_clear{1, 0, 0, 0};
    const std::array<float, 4> color_clear{.75f, .5f, .125f, 1};
    auto depth = gfxCreateTexture2DArray(gfx, 64, 32, 2, DXGI_FORMAT_D32_FLOAT, 3, depth_clear.data(),
                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto color = gfxCreateTexture2DArray(gfx, 64, 32, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, color_clear.data(),
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    require(depth && color, "shadow atlas allocation failed");
    // Slice zero is the untouched neighbor of the cleared page at mip two.
    constexpr std::uint32_t mip = 2;
    for (std::uint32_t slice = 0; slice < 2; ++slice) {
        check(gfxCommandSetViewport(gfx, 0, 0, float(64u >> mip), float(32u >> mip)));
        check(gfxCommandSetScissorRect(gfx, 0, 0, 64u >> mip, 32u >> mip));
        check(gfxCommandBindColorTarget(gfx, 0, {}));
        check(gfxCommandBindDepthStencilTarget(gfx, depth, mip, slice));
        set_plane(gfx, depth_draw, .25f, 0);
        check(gfxCommandDraw(gfx, 3));
        check(gfxCommandBindDepthStencilTarget(gfx, {}));
        check(gfxCommandBindColorTarget(gfx, 0, color, mip, slice));
        set_plane(gfx, color_draw, .25f, 0);
        check(gfxCommandDraw(gfx, 3));
    }
    check(gfxCommandBindColorTarget(gfx, 0, {}));
    // Clear rectangles are independent of the raster scissor and viewport.
    check(gfxCommandSetViewport(gfx, 0, 0, 1, 1));
    check(gfxCommandSetScissorRect(gfx, 0, 0, 1, 1));
    constexpr std::array<D3D12_RECT, 3> rects{{{2, 3, 9, 8}, {10, 0, 16, 2}, {15, 7, 16, 8}}};
    check(gfxCommandClearImageRects(gfx, depth, rects.data(), std::uint32_t(rects.size()), 2, 1));
    check(gfxCommandClearImageRects(gfx, color, rects.data(), std::uint32_t(rects.size()), 2, 1));
    PipelineDesc sample_desc;
    sample_desc.source.cs = R"(
Texture2DArray<float> depth_image; Texture2DArray<float4> color_image;
SamplerComparisonState shadow_sampler; RWStructuredBuffer<uint> values; uint4 view;
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=view.xy))return;
    uint i=view.w+(id.y*view.x+id.x)*6;
    float depth=depth_image.Load(int4(id.xy,view.z,0));
    float4 color=color_image.Load(int4(id.xy,view.z,0));
    float comparison=depth_image.SampleCmpLevelZero(shadow_sampler,float3((id.xy+.5)/view.xy,view.z),.5);
    values[i]=asuint(depth);values[i+1]=asuint(comparison);
    values[i+2]=asuint(color.x);values[i+3]=asuint(color.y);values[i+4]=asuint(color.z);values[i+5]=asuint(color.w);
})";
    auto sample = take(owner.prepare(sample_desc));
    auto sampler =
        gfxCreateSamplerState(gfx, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_COMPARISON_FUNC_LESS_EQUAL);
    constexpr std::uint32_t words = 2 * 16 * 8 * 6;
    auto values = gfxCreateBuffer<std::uint32_t>(gfx, words);
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, words, nullptr, kGfxCpuAccess_Read);
    require(sampler && values && readback, "shadow atlas readback allocation failed");
    check(gfxProgramSetParameter(gfx, sample.program(), "shadow_sampler", sampler));
    check(gfxProgramSetParameter(gfx, sample.program(), "values", values));
    std::uint32_t offset = 0;
    for (std::uint32_t slice = 0; slice < 2; ++slice) {
        const std::array<std::uint32_t, 4> view{64u >> mip, 32u >> mip, slice, offset};
        check(gfxProgramSetParameter(gfx, sample.program(), "view", view));
        check(gfxProgramSetParameter(gfx, sample.program(), "depth_image", depth, mip));
        check(gfxProgramSetParameter(gfx, sample.program(), "color_image", color, mip));
        check(gfxCommandBindKernel(gfx, sample.kernel()));
        check(gfxCommandDispatch(gfx, (view[0] + 7) / 8, (view[1] + 7) / 8, 1));
        offset += view[0] * view[1] * 6;
    }
    check(gfxCommandCopyBuffer(gfx, readback, values));
    check(gfxDestroyTexture(gfx, depth));
    check(gfxDestroyTexture(gfx, color));
    check(gfxDestroySamplerState(gfx, sampler));
    check(gfxDestroyBuffer(gfx, values));
    sample = {};
    depth_draw = {};
    color_draw = {};
    take(owner.finish());
    const auto* data = gfxBufferGetData<std::uint32_t>(gfx, readback);
    require(data != nullptr, "shadow atlas readback is unavailable");
    offset = 0;
    for (std::uint32_t slice = 0; slice < 2; ++slice)
        for (std::uint32_t y = 0; y < (32u >> mip); ++y)
            for (std::uint32_t x = 0; x < (64u >> mip); ++x) {
                bool cleared = false;
                if (slice == 1)
                    for (const auto& rect : rects)
                        cleared |= contains(rect, x, y);
                const auto expected = cleared ? color_clear : std::array<float, 4>{.125f, .25f, .375f, .5f};
                require(data[offset++] == std::bit_cast<std::uint32_t>(cleared ? 1.f : .25f),
                        "tile depth or outside sentinel mismatch");
                require(data[offset++] == std::bit_cast<std::uint32_t>(cleared ? 1.f : 0.f),
                        "shadow comparison sample mismatch");
                for (float channel : expected)
                    require(data[offset++] == std::bit_cast<std::uint32_t>(channel),
                            "tile color or outside sentinel mismatch");
            }
    check(gfxDestroyBuffer(gfx, readback));
    std::printf("shadow atlas: words=%u mip=2 target/neighbor half-open/comparison/retirement=PASS\n", words);
}

void raster_bias(Context& owner) {
    const auto gfx = owner.borrow();
    auto desc = depth_desc();
    check(gfxDrawStateSetDepthBias(desc.draw_state, 262144, 2, .001f));
    auto pipeline = take(owner.prepare(desc));
    auto readback = gfxCreateBuffer<float>(gfx, 64 * 32, nullptr, kGfxCpuAccess_Read);
    require(bool(readback), "depth bias readback allocation failed");
    // Prepared kernels own snapshots; later edits must not affect earlier PSOs.
    check(gfxDrawStateSetDepthBias(desc.draw_state, 777777, 7, 0));
    const std::array<float, 4> clear{1, 0, 0, 0};
    auto depth = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_D32_FLOAT, 1, clear.data(),
                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    require(bool(depth), "depth bias target allocation failed");
    check(gfxCommandSetViewport(gfx, 0, 0, 64, 32));
    check(gfxCommandSetScissorRect(gfx, 0, 0, 64, 32));
    check(gfxCommandBindColorTarget(gfx, 0, {}));
    check(gfxCommandBindDepthStencilTarget(gfx, depth));
    set_plane(gfx, pipeline, .25f, .125f);
    check(gfxCommandDraw(gfx, 3));
    check(gfxCommandBindDepthStencilTarget(gfx, {}));
    check(gfxCommandCopyTextureToBuffer(gfx, readback, depth));
    check(gfxDestroyTexture(gfx, depth));
    pipeline = {};
    take(owner.finish());
    const auto* data = gfxBufferGetData<float>(gfx, readback);
    require(data != nullptr, "depth bias readback is unavailable");
    for (std::size_t p = 0; p < 64 * 32; ++p) {
        const float expected = .25f + .125f * (float(p % 64) + .5f) / 64 + .001f;
        require(std::fabs(data[p] - expected) < 1e-6f, "positive depth bias clamp or PSO snapshot mismatch");
    }
    check(gfxDestroyBuffer(gfx, readback));
    std::puts("shadow bias: vertex pixels=2048 positive-clamp/snapshot=PASS");
}
}

void shadow_cases() {
    ContextOptions options;
    options.validation = Validation::gpu;
    auto owner = take(Context::open(options));
    auto debug = debug_queue(owner.borrow());
    atlas_rectangles(owner);
    raster_bias(owner);
    take(owner.finish());
    check_debug(debug.Get());
    take(owner.close());
    check_debug(debug.Get());
    std::puts("Shadow atlas RHI acceptance: PASS");
}
}

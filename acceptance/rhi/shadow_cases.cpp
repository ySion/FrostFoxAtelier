#include "rhi/rhi.hpp"
#include "support.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
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
constexpr char plane_vs[] = R"(
float plane_base; float plane_slope;
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = id == 0 ? float2(-1,-1) : id == 1 ? float2(-1,3) : float2(3,-1);
    return float4(p, plane_base + plane_slope * (p.x * .5 + .5), 1);
})";
constexpr char plane_ms[] = R"(
float plane_base; float plane_slope;
struct Vertex { float4 position : SV_Position; };
[outputtopology("triangle")][numthreads(1,1,1)]
void main(out vertices Vertex verts[3], out indices uint3 triangles[1]) {
    SetMeshOutputCounts(3,1);
    float2 p[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    for (uint i=0;i<3;i++) verts[i].position=float4(p[i],plane_base+plane_slope*(p[i].x*.5+.5),1);
    triangles[0]=uint3(0,1,2);
})";
PipelineDesc depth_desc(PipelineKind kind) {
    PipelineDesc desc;
    desc.kind = kind;
    if (kind == PipelineKind::vertex)
        desc.source.vs = plane_vs;
    else
        desc.source.ms = plane_ms;
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
    auto depth_draw = take(owner.prepare(depth_desc(PipelineKind::vertex)));
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
    for (std::uint32_t mip = 0; mip < 3; ++mip)
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
    check(gfxCommandClearImageRects(gfx, depth, nullptr, 0, 2, 1));
    check(gfxCommandClearImageRects(gfx, color, nullptr, 0, 2, 1));
    constexpr std::array<D3D12_RECT, 7> invalid{{{-1, 0, 1, 1},
                                                 {0, -1, 1, 1},
                                                 {0, 0, 17, 1},
                                                 {0, 0, 1, 9},
                                                 {1, 0, 1, 1},
                                                 {2, 0, 1, 1},
                                                 {0, 0, std::numeric_limits<LONG>::max(), 1}}};
    for (const auto& bad : invalid) {
        const std::array<D3D12_RECT, 2> batch{{{0, 0, 1, 1}, bad}};
        require(gfxCommandClearImageRects(gfx, depth, batch.data(), 2, 2, 1) == kGfxResult_InvalidParameter,
                "invalid rectangle batch was accepted");
    }
    require(gfxCommandClearImageRects(gfx, depth, nullptr, 1, 2, 1) == kGfxResult_InvalidParameter,
            "null nonempty rectangle list was accepted");
    require(gfxCommandClearImageRects(gfx, depth, rects.data(), 1, 3, 0) != kGfxResult_NoError,
            "invalid clear mip was accepted");
    require(gfxCommandClearImageRects(gfx, depth, rects.data(), 1, 2, 2) != kGfxResult_NoError,
            "invalid clear slice was accepted");
    // The old whole-image entry still clears exactly the selected view.
    check(gfxCommandClearImage(gfx, depth, 1, 0));
    check(gfxCommandClearImage(gfx, color, 1, 0));
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
    constexpr std::uint32_t words = 2 * (64 * 32 + 32 * 16 + 16 * 8) * 6;
    auto values = gfxCreateBuffer<std::uint32_t>(gfx, words);
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, words, nullptr, kGfxCpuAccess_Read);
    require(sampler && values && readback, "shadow atlas readback allocation failed");
    check(gfxProgramSetParameter(gfx, sample.program(), "shadow_sampler", sampler));
    check(gfxProgramSetParameter(gfx, sample.program(), "values", values));
    std::uint32_t offset = 0;
    for (std::uint32_t mip = 0; mip < 3; ++mip)
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
    complete(owner.finish());
    const auto* data = gfxBufferGetData<std::uint32_t>(gfx, readback);
    require(data != nullptr, "shadow atlas readback is unavailable");
    offset = 0;
    for (std::uint32_t mip = 0; mip < 3; ++mip)
        for (std::uint32_t slice = 0; slice < 2; ++slice)
            for (std::uint32_t y = 0; y < (32u >> mip); ++y)
                for (std::uint32_t x = 0; x < (64u >> mip); ++x) {
                    bool cleared = mip == 1 && slice == 0;
                    if (mip == 2 && slice == 1)
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
    std::printf(
        "shadow atlas: words=%u mip-slice=6 half-open=PASS invalid-batch=PASS empty=PASS comparison=PASS retirement=PASS\n",
        words);
}
void raster_bias(Context& owner, PipelineKind kind) {
    const auto gfx = owner.borrow();
    struct Bias {
        std::int32_t constant;
        float slope;
        float clamp;
    };
    constexpr std::array<Bias, 7> biases{
        {{0, 0, 0}, {0, 0, 0}, {262144, 0, 0}, {-262144, 0, 0}, {0, 2, 0}, {262144, 2, .001f}, {-262144, -2, -.001f}}};
    auto desc = depth_desc(kind);
    std::array<Pipeline, biases.size()> pipelines;
    std::array<GfxBuffer, biases.size()> readbacks;
    for (std::size_t i = 0; i < biases.size(); ++i) {
        const auto b = biases[i];
        if (i)
            check(gfxDrawStateSetDepthBias(desc.draw_state, b.constant, b.slope, b.clamp));
        if (i == 2) {
            require(gfxDrawStateSetDepthBias(desc.draw_state, 0, std::numeric_limits<float>::quiet_NaN(), 0) ==
                        kGfxResult_InvalidParameter,
                    "NaN bias was accepted");
            require(gfxDrawStateSetDepthBias(desc.draw_state, 0, 0, std::numeric_limits<float>::infinity()) ==
                        kGfxResult_InvalidParameter,
                    "infinite bias was accepted");
        }
        pipelines[i] = take(owner.prepare(desc));
        readbacks[i] = gfxCreateBuffer<float>(gfx, 64 * 32, nullptr, kGfxCpuAccess_Read);
        require(bool(readbacks[i]), "depth bias readback allocation failed");
    }
    // Prepared kernels own snapshots; later edits must not affect earlier PSOs.
    check(gfxDrawStateSetDepthBias(desc.draw_state, 777777, 7, 0));
    const std::array<float, 4> clear{1, 0, 0, 0};
    auto depth = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_D32_FLOAT, 1, clear.data(),
                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    require(bool(depth), "depth bias target allocation failed");
    check(gfxCommandSetViewport(gfx, 0, 0, 64, 32));
    check(gfxCommandSetScissorRect(gfx, 0, 0, 64, 32));
    check(gfxCommandBindColorTarget(gfx, 0, {}));
    for (std::size_t i = 0; i < pipelines.size(); ++i) {
        check(gfxCommandBindDepthStencilTarget(gfx, depth));
        set_plane(gfx, pipelines[i], .25f, .125f);
        if (kind == PipelineKind::vertex)
            check(gfxCommandDraw(gfx, 3));
        else
            check(gfxCommandDrawMesh(gfx, 1, 1, 1));
        check(gfxCommandBindDepthStencilTarget(gfx, {}));
        check(gfxCommandCopyTextureToBuffer(gfx, readbacks[i], depth));
    }
    check(gfxDestroyTexture(gfx, depth));
    for (auto& pipeline : pipelines)
        pipeline = {};
    complete(owner.finish());
    std::array<const float*, biases.size()> data;
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = gfxBufferGetData<float>(gfx, readbacks[i]);
        require(data[i] != nullptr, "depth bias readback is unavailable");
    }
    for (std::size_t p = 0; p < 64 * 32; ++p) {
        const float expected = .25f + .125f * (float(p % 64) + .5f) / 64;
        require(std::fabs(data[0][p] - expected) < 1e-6f, "default depth bias changed raster depth");
        require(std::fabs(data[1][p] - data[0][p]) < 1e-6f, "explicit zero bias differs from default");
        const float positive = data[2][p] - data[0][p];
        const float negative = data[3][p] - data[0][p];
        require(positive > .0005f && positive < .05f && negative < -.0005f && negative > -.05f,
                "constant depth bias sign/magnitude mismatch");
        require(std::fabs((data[4][p] - data[0][p]) - (2 * .125f / 64)) < 1e-5f, "slope-scaled depth bias mismatch");
        require(std::fabs((data[5][p] - data[0][p]) - .001f) < 1e-6f, "positive depth bias clamp mismatch");
        require(std::fabs((data[6][p] - data[0][p]) + .001f) < 1e-6f, "negative depth bias clamp mismatch");
    }
    for (auto readback : readbacks)
        check(gfxDestroyBuffer(gfx, readback));
    std::printf(
        "shadow bias: stage=%s variants=%zu pixels=%u defaults=PASS constant=PASS slope=PASS clamp=PASS snapshot=PASS\n",
        kind == PipelineKind::vertex ? "vertex" : "mesh", biases.size(), 64 * 32);
}
}
void shadow_cases() {
    ContextOptions options;
    options.validation = Validation::gpu;
    auto owner = take(Context::open(options));
    auto debug = debug_queue(owner.borrow());
    atlas_rectangles(owner);
    raster_bias(owner, PipelineKind::vertex);
    raster_bias(owner, PipelineKind::mesh);
    complete(owner.finish());
    check_debug(debug.Get());
    complete(owner.close());
    check_debug(debug.Get());
    std::puts("Shadow atlas RHI acceptance: PASS");
}
}

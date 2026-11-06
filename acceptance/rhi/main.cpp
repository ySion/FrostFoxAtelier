#include "memory/fatal.hpp"
#include "rhi/rhi.hpp"
#include "support.hpp"
#include "shaders.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

using namespace atelier::rhi;

using namespace rhi_acceptance;

namespace rhi_acceptance {
void resource_cases();

void ray_query_cases();

void scene_lit_cases();

void shadow_cases();

void preparation_cases();
}

namespace {
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void check(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

PipelineDesc graphics_desc(PipelineKind kind) {
    PipelineDesc desc;
    desc.kind = kind;
    if (kind == PipelineKind::mesh) {
        desc.source.ms = mesh_source;
        desc.source.ps = mesh_pixel_source;
    } else {
        desc.source.vs = vertex_source;
        desc.source.ps = vertex_pixel_source;
    }
    rhi_acceptance::check(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM));
    rhi_acceptance::check(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT));
    rhi_acceptance::check(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_LESS));
    rhi_acceptance::check(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    return desc;
}

void scene(Context& context, Pipeline& compute, Pipeline& mesh, Pipeline& vertex) {
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 32;
    constexpr std::uint32_t frames = 6;
    constexpr std::uint32_t image_bytes = width * height * 4;
    const GfxContext gfx = context.borrow();
    const float depth_clear[4] = {1, 0, 0, 0};
    auto color = gfxCreateTexture2D(gfx, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = gfxCreateTexture2D(gfx, width, height, DXGI_FORMAT_D32_FLOAT, 1, depth_clear,
                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto mesh_color = gfxCreateBuffer(gfx, image_bytes, nullptr, kGfxCpuAccess_Read);
    auto vertex_color = gfxCreateBuffer(gfx, image_bytes, nullptr, kGfxCpuAccess_Read);
    auto depth_readback = gfxCreateBuffer(gfx, image_bytes, nullptr, kGfxCpuAccess_Read);
    require(color && depth && mesh_color && vertex_color && depth_readback, "render target/readback creation failed");
    std::array<GfxBuffer, frames> outputs;
    std::array<std::uint32_t, 128> large{};
    large[124] = 7;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto& out = outputs[frame];
        out = gfxCreateBuffer<std::uint32_t>(gfx, 15, nullptr, kGfxCpuAccess_Read);
        auto a = gfxCreateBuffer<std::uint32_t>(gfx, 4);
        auto b = gfxCreateBuffer<std::uint32_t>(gfx, 4);
        auto mesh_args = gfxCreateBuffer<std::uint32_t>(gfx, 3);
        auto draw_args = gfxCreateBuffer<std::uint32_t>(gfx, 4);
        require(out && a && b && mesh_args && draw_args, "scene buffer creation failed");
        rhi_acceptance::check(gfxCommandBindKernel(gfx, compute.kernel()));
        rhi_acceptance::check(gfxProgramSetParameter(gfx, compute.program(), "large", large));
        rhi_acceptance::check(gfxProgramSetParameter(gfx, compute.program(), "mesh_args", mesh_args));
        rhi_acceptance::check(gfxProgramSetParameter(gfx, compute.program(), "draw_args", draw_args));
        for (std::uint32_t pass = 0; pass < 2; ++pass) {
            const std::uint32_t seed = (pass == 0 ? 40u : 80u) + frame - 7;
            rhi_acceptance::check(gfxProgramSetParameter(gfx, compute.program(), "seed", seed));
            rhi_acceptance::check(gfxProgramSetParameter(gfx, compute.program(), "values", pass == 0 ? a : b));
            rhi_acceptance::check(gfxCommandDispatch(gfx, 1, 1, 1));
        }
        rhi_acceptance::check(gfxCommandClearTexture(gfx, color));
        rhi_acceptance::check(gfxCommandClearTexture(gfx, depth));
        rhi_acceptance::check(gfxCommandBindColorTarget(gfx, 0, color));
        rhi_acceptance::check(gfxCommandBindDepthStencilTarget(gfx, depth));
        rhi_acceptance::check(gfxProgramSetParameter(gfx, mesh.program(), "values", a));
        rhi_acceptance::check(gfxCommandBindKernel(gfx, mesh.kernel()));
        rhi_acceptance::check(gfxCommandDrawMeshIndirect(gfx, mesh_args));
        if (frame + 1 == frames)
            rhi_acceptance::check(gfxCommandCopyTextureToBuffer(gfx, mesh_color, color));
        rhi_acceptance::check(gfxProgramSetParameter(gfx, vertex.program(), "values", b));
        rhi_acceptance::check(gfxCommandBindKernel(gfx, vertex.kernel()));
        rhi_acceptance::check(gfxCommandMultiDrawIndirect(gfx, draw_args, 1));
        if (frame + 1 == frames) {
            rhi_acceptance::check(gfxCommandCopyTextureToBuffer(gfx, vertex_color, color));
            rhi_acceptance::check(gfxCommandCopyTextureToBuffer(gfx, depth_readback, depth));
        }
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out, 0, a, 0, 16));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out, 16, b, 0, 16));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out, 32, mesh_args, 0, 12));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out, 44, draw_args, 0, 16));
        for (auto buffer : {a, b, mesh_args, draw_args})
            rhi_acceptance::check(gfxDestroyBuffer(gfx, buffer));
        check(context.frame());
        if (frame == 1)
            vertex = take(context.prepare(graphics_desc(PipelineKind::vertex)));
    }
    check(context.finish());
    const auto* red = gfxBufferGetData<std::uint8_t>(gfx, mesh_color);
    const auto* green = gfxBufferGetData<std::uint8_t>(gfx, vertex_color);
    const auto* z = gfxBufferGetData<float>(gfx, depth_readback);
    require(red && green && z, "image readback mapping failed");
    for (std::uint32_t pixel = 0; pixel < width * height; ++pixel) {
        require(red[pixel * 4] == 40 + frames - 1 && red[pixel * 4 + 1] == 0 && red[pixel * 4 + 2] == 0 &&
                    red[pixel * 4 + 3] == 255,
                "mesh pixel result mismatch");
        require(green[pixel * 4] == 0 && green[pixel * 4 + 1] == 80 + frames - 1 && green[pixel * 4 + 2] == 0 &&
                    green[pixel * 4 + 3] == 255,
                "vertex pixel result mismatch");
        require(std::fabs(z[pixel] - 0.25f) < 0.00001f, "depth result mismatch");
    }
    // Each submission keeps its own parameter result across retirement and slot reuse.
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto* parameters = gfxBufferGetData<std::uint32_t>(gfx, outputs[frame]);
        require(parameters != nullptr, "parameter readback mapping failed");
        const std::array<std::uint32_t, 15> expected = {40 + frame, 0, 0, 1, 80 + frame, 0, 0, 1, 1, 1, 1, 3, 1, 0, 0};
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(parameters[i] == expected[i], "compute/indirect parameter mismatch");
        rhi_acceptance::check(gfxDestroyBuffer(gfx, outputs[frame]));
    }
    for (auto buffer : {mesh_color, vertex_color, depth_readback})
        rhi_acceptance::check(gfxDestroyBuffer(gfx, buffer));
    rhi_acceptance::check(gfxDestroyTexture(gfx, color));
    rhi_acceptance::check(gfxDestroyTexture(gfx, depth));
    std::printf("scene: frames=%u color-pixels=%u depth-pixels=%u parameter-words=%u mismatches=0\n", frames,
                width * height * 2, width * height, frames * 15);
    std::puts("same-frame-rebinding=PASS cross-frame-constants=PASS early-release=PASS pipeline-replacement=PASS");
}
} // namespace

int main() {
    atelier::install_fatal_handler();
    try {
        ContextOptions options;
        options.validation = Validation::gpu;
        auto context = take(Context::open(options));
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            const auto target = take(context.sync_window_target());
            require(!target.windowed && target.drawable && target.width == options.width &&
                        target.height == options.height,
                    "headless target synchronization changed the target");
            require(gfxGetRecordingFrameSequence(context.borrow()) == 1, "headless synchronization advanced a frame");
        }
        require(context.info().debug_layer && context.info().gpu_validation == expected_gpu_validation(),
                "validation differs from the requested mode");
        print_adapter(context.borrow());
        auto debug = debug_queue(context.borrow());
        std::printf("debug-layer=%u gpu-validation=%u\n", unsigned(context.info().debug_layer),
                    unsigned(context.info().gpu_validation));
        PipelineDesc compute_desc;
        compute_desc.source.cs = compute_source;
        auto compute = take(context.prepare(compute_desc));
        auto mesh = take(context.prepare(graphics_desc(PipelineKind::mesh)));
        auto vertex = take(context.prepare(graphics_desc(PipelineKind::vertex)));
        scene(context, compute, mesh, vertex);
        compute = {};
        mesh = {};
        vertex = {};
        check(context.finish());
        check_debug(debug.Get());
        check(context.close());
        check_debug(debug.Get());
        debug.Reset();
        Pipeline survivor;
        GfxContext retained;
        ComPtr<ID3D12InfoQueue> retained_debug;
        {
            auto temporary = take(Context::open(options));
            retained = temporary.borrow();
            retained_debug = debug_queue(retained);
            survivor = take(temporary.prepare(compute_desc));
        }
        require(survivor && gfxKernelGetPipelineState(retained, survivor.kernel()),
                "pipeline did not keep its execution domain alive");
        survivor = {};
        check_debug(retained_debug.Get());
        retained_debug.Reset();
        resource_cases();
        ray_query_cases();
        scene_lit_cases();
        shadow_cases();
        preparation_cases();
        std::puts("pipeline-retains-domain=PASS debug-warnings=0 debug-errors=0");
        std::puts("RHI acceptance: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RHI acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}

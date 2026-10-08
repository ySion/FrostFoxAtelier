#include "rhi/rhi.hpp"
#include "support.hpp"
#include "shaders.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

using namespace atelier::rhi;
using namespace rhi_acceptance;
namespace rhi_acceptance {
void resource_cases();
void ray_query_cases();
void scene_lit_cases();
}
namespace {
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void check(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void fixture(const std::filesystem::path& path, const char* source) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << source;
    require(file.good(), "shader fixture write failed");
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
void preparation_matrix(Context& context, Pipeline& live_vertex) {
    const auto old_program = live_vertex.program();
    const auto old_kernel = live_vertex.kernel();
    std::uint32_t rejected = 0;
    auto reject = [&](const PipelineDesc& desc) {
        auto candidate = context.prepare(desc);
        require(!candidate, "invalid shader candidate unexpectedly became ready");
        require(live_vertex.program() == old_program && live_vertex.kernel() == old_kernel,
                "failed preparation modified the live pipeline");
        ++rejected;
    };
    PipelineDesc invalid_input;
    invalid_input.source.cs = increment_source;
    const auto reject_input = [&](const PipelineDesc& desc) {
        auto candidate = context.prepare(desc);
        require(!candidate && candidate.error().code == ErrorCode::invalid_argument,
                "malformed description was not rejected at the input boundary");
        require(live_vertex.program() == old_program && live_vertex.kernel() == old_kernel,
                "invalid description changed the live pipeline");
        ++rejected;
    };
    for (std::size_t length : {11u, 12u, 128u, 8192u}) {
        const std::string model(length, 'A');
        invalid_input.shader_model = model.c_str();
        reject_input(invalid_input);
    }
    for (const char* model : {"", "6", "_5", "6_", "6__5", "6_a", "1234567890_1"}) {
        invalid_input.shader_model = model;
        reject_input(invalid_input);
    }
    invalid_input.shader_model = nullptr;
    reject_input(invalid_input);
    invalid_input.shader_model = "6_5";
    invalid_input.file_name = "file_compute";
    invalid_input.file_path = "";
    reject_input(invalid_input);
    PipelineDesc compute;
    compute.source.cs = invalid_source;
    reject(compute);
    compute.source.cs = increment_source;
    compute.entry_point = "missing_entry";
    reject(compute);
    auto vertex = graphics_desc(PipelineKind::vertex);
    vertex.source.ps = invalid_source;
    reject(vertex);
    vertex = graphics_desc(PipelineKind::vertex);
    vertex.source.gs = invalid_source;
    reject(vertex);
    auto mesh = graphics_desc(PipelineKind::mesh);
    mesh.source.as = invalid_source;
    reject(mesh);
    mesh = graphics_desc(PipelineKind::mesh);
    mesh.source.ps = invalid_source;
    reject(mesh);

    std::filesystem::create_directories("rhi-shaders");
    fixture("rhi-shaders/file_vertex.vert", vertex_source);
    fixture("rhi-shaders/file_vertex.frag", vertex_pixel_source);
    // The optional GS is intentionally absent; remove our previous-run fixture.
    std::filesystem::remove("rhi-shaders/file_vertex.geom");
    vertex = graphics_desc(PipelineKind::vertex);
    vertex.file_name = "file_vertex";
    vertex.file_path = "rhi-shaders";
    auto valid_file_vertex = take(context.prepare(vertex));
    require(static_cast<bool>(valid_file_vertex), "file vertex pipeline failed without optional GS");
    fixture("rhi-shaders/file_vertex.geom", invalid_source);
    reject(vertex);
    std::filesystem::remove("rhi-shaders/file_vertex.geom");
    fixture("rhi-shaders/file_vertex.frag", invalid_source);
    reject(vertex);
    fixture("rhi-shaders/file_mesh.mesh", mesh_source);
    fixture("rhi-shaders/file_mesh.frag", mesh_pixel_source);
    std::filesystem::remove("rhi-shaders/file_mesh.task");
    mesh = graphics_desc(PipelineKind::mesh);
    mesh.file_name = "file_mesh";
    mesh.file_path = "rhi-shaders";
    auto valid_file_mesh = take(context.prepare(mesh));
    require(static_cast<bool>(valid_file_mesh), "file mesh pipeline failed without optional AS");
    fixture("rhi-shaders/file_mesh.task", invalid_source);
    reject(mesh);
    fixture("rhi-shaders/file_compute.comp", invalid_source);
    compute = PipelineDesc{};
    compute.file_name = "file_compute";
    compute.file_path = "rhi-shaders";
    reject(compute);
    for (auto kind : {PipelineKind::vertex, PipelineKind::mesh, PipelineKind::compute}) {
        auto missing = graphics_desc(kind);
        missing.file_name = "absent_primary_stage";
        missing.file_path = "rhi-shaders";
        reject(missing);
    }
    std::printf("preparation: rejected=%u optional-file-stages=PASS old-pipeline-preserved=PASS\n", rejected);
}
struct Readbacks {
    GfxBuffer mesh_color;
    GfxBuffer vertex_color;
    GfxBuffer depth;
    GfxBuffer parameters;
};
void scene(Context& context, Pipeline& compute, Pipeline& mesh, Pipeline& vertex) {
    constexpr std::uint32_t width = 64, height = 32, frames = 6;
    constexpr std::uint32_t image_bytes = width * height * 4;
    const GfxContext gfx = context.borrow();
    const float depth_clear[4] = {1, 0, 0, 0};
    auto color = gfxCreateTexture2D(gfx, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = gfxCreateTexture2D(gfx, width, height, DXGI_FORMAT_D32_FLOAT, 1, depth_clear, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    require(color && depth, "render target creation failed");
    std::array<Readbacks, frames> outputs;
    std::array<std::uint32_t, 128> large{};
    large[124] = 7;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto& out = outputs[frame];
        out.mesh_color = gfxCreateBuffer(gfx, image_bytes, nullptr, kGfxCpuAccess_Read);
        out.vertex_color = gfxCreateBuffer(gfx, image_bytes, nullptr, kGfxCpuAccess_Read);
        out.depth = gfxCreateBuffer(gfx, image_bytes, nullptr, kGfxCpuAccess_Read);
        out.parameters = gfxCreateBuffer<std::uint32_t>(gfx, 15, nullptr, kGfxCpuAccess_Read);
        auto a = gfxCreateBuffer<std::uint32_t>(gfx, 4);
        auto b = gfxCreateBuffer<std::uint32_t>(gfx, 4);
        auto mesh_args = gfxCreateBuffer<std::uint32_t>(gfx, 3);
        auto draw_args = gfxCreateBuffer<std::uint32_t>(gfx, 4);
        require(out.mesh_color && out.vertex_color && out.depth && out.parameters && a && b && mesh_args && draw_args,
                "scene buffer creation failed");
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
        rhi_acceptance::check(gfxCommandCopyTextureToBuffer(gfx, out.mesh_color, color));
        rhi_acceptance::check(gfxProgramSetParameter(gfx, vertex.program(), "values", b));
        rhi_acceptance::check(gfxCommandBindKernel(gfx, vertex.kernel()));
        rhi_acceptance::check(gfxCommandMultiDrawIndirect(gfx, draw_args, 1));
        rhi_acceptance::check(gfxCommandCopyTextureToBuffer(gfx, out.vertex_color, color));
        rhi_acceptance::check(gfxCommandCopyTextureToBuffer(gfx, out.depth, depth));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out.parameters, 0, a, 0, 16));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out.parameters, 16, b, 0, 16));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out.parameters, 32, mesh_args, 0, 12));
        rhi_acceptance::check(gfxCommandCopyBuffer(gfx, out.parameters, 44, draw_args, 0, 16));
        for (auto buffer : {a, b, mesh_args, draw_args}) rhi_acceptance::check(gfxDestroyBuffer(gfx, buffer));
        check(context.frame());
        if (frame == 1) vertex = take(context.prepare(graphics_desc(PipelineKind::vertex)));
    }
    check(context.finish());
    std::uint64_t color_samples = 0, depth_samples = 0, parameter_samples = 0;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto& out = outputs[frame];
        const auto* red = gfxBufferGetData<std::uint8_t>(gfx, out.mesh_color);
        const auto* green = gfxBufferGetData<std::uint8_t>(gfx, out.vertex_color);
        const auto* z = gfxBufferGetData<float>(gfx, out.depth);
        const auto* parameters = gfxBufferGetData<std::uint32_t>(gfx, out.parameters);
        require(red && green && z && parameters, "readback mapping failed");
        for (std::uint32_t pixel = 0; pixel < width * height; ++pixel) {
            require(red[pixel * 4] == 40 + frame && red[pixel * 4 + 1] == 0 && red[pixel * 4 + 2] == 0 && red[pixel * 4 + 3] == 255,
                    "mesh pixel result mismatch");
            require(green[pixel * 4] == 0 && green[pixel * 4 + 1] == 80 + frame && green[pixel * 4 + 2] == 0 && green[pixel * 4 + 3] == 255,
                    "vertex pixel result mismatch");
            require(std::fabs(z[pixel] - 0.25f) < 0.00001f, "depth result mismatch");
            color_samples += 2;
            ++depth_samples;
        }
        const std::array<std::uint32_t, 15> expected = {40 + frame, 0, 0, 1, 80 + frame, 0, 0, 1, 1, 1, 1, 3, 1, 0, 0};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require(parameters[i] == expected[i], "compute/indirect parameter mismatch");
            ++parameter_samples;
        }
        for (auto buffer : {out.mesh_color, out.vertex_color, out.depth, out.parameters}) rhi_acceptance::check(gfxDestroyBuffer(gfx, buffer));
    }
    rhi_acceptance::check(gfxDestroyTexture(gfx, color));
    rhi_acceptance::check(gfxDestroyTexture(gfx, depth));
    std::printf("scene: frames=%u color-pixels=%llu depth-pixels=%llu parameter-words=%llu mismatches=0\n",
        frames, static_cast<unsigned long long>(color_samples), static_cast<unsigned long long>(depth_samples),
        static_cast<unsigned long long>(parameter_samples));
    std::puts("same-frame-rebinding=PASS cross-frame-constants=PASS early-release=PASS pipeline-replacement=PASS");
}
} // namespace

int main() {
    try {
        for (unsigned test = 0; test < 4; ++test) {
            ContextOptions invalid;
            if (test == 0) invalid.width = 0;
            if (test == 1) invalid.wait_timeout_ms = 0;
            if (test == 2) invalid.wait_timeout_ms = INFINITE;
            if (test == 3) invalid.validation = static_cast<Validation>(999);
            auto rejected = Context::open(invalid);
            require(!rejected && rejected.error().code == ErrorCode::invalid_argument,
                    "invalid context input was not rejected before creation");
        }
        ContextOptions options;
        options.validation = Validation::gpu;
        auto context = take(Context::open(options));
        require(context.info().debug_layer && context.info().gpu_validation, "validation not enabled");
        require(context.info().mesh_shader, "acceptance requires hardware mesh shader support");
        print_adapter(context.borrow());
        auto debug = debug_queue(context.borrow());
        std::puts("debug-layer=1 gpu-validation=1");
        PipelineDesc compute_desc;
        compute_desc.source.cs = compute_source;
        auto compute = take(context.prepare(compute_desc));
        auto mesh = take(context.prepare(graphics_desc(PipelineKind::mesh)));
        auto vertex = take(context.prepare(graphics_desc(PipelineKind::vertex)));
        auto busy = context.close();
        require(!busy && busy.error().code == ErrorCode::busy, "close did not preserve live pipeline owners");
        bool wrong_thread = false;
        std::thread worker([&] {
            auto result = context.finish();
            wrong_thread = !result && result.error().code == ErrorCode::wrong_thread && !context.borrow();
        });
        worker.join();
        require(wrong_thread, "foreign thread entered the execution domain");
        preparation_matrix(context, vertex);
        scene(context, compute, mesh, vertex);
        compute = {};
        mesh = {};
        vertex = {};
        check(context.finish());
        check_debug(debug.Get());
        check(context.close());
        require(!context.borrow(), "closed context remained borrowable");
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
        std::puts("invalid-context-inputs=4 PASS pipeline-retains-domain=PASS");
        std::puts("owner-thread=PASS close-busy=PASS close-idempotent=PASS debug-warnings=0 debug-errors=0");
        std::puts("RHI acceptance: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RHI acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}

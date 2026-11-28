#include "memory/fatal.hpp"
#include "rdg/graph.hpp"
#include "rhi/rhi.hpp"
#include "shaders.hpp"
#include "support.hpp"

#include <array>
#include <cmath>
#include <cstdint>

using namespace atelier::rhi;

using namespace rhi_acceptance;

namespace {

Pipeline prepare_graphics(Device& device, PipelineKind kind) {
    std::array<ShaderStageSource, 2> stages{};
    if (kind == PipelineKind::mesh) {
        stages[0] = {nri::StageBits::MESH_SHADER, mesh_source, "main", "rhi_mesh_ms"};
        stages[1] = {nri::StageBits::FRAGMENT_SHADER, mesh_pixel_source, "main", "rhi_mesh_ps"};
    } else {
        stages[0] = {nri::StageBits::VERTEX_SHADER, vertex_source, "main", "rhi_vertex_vs"};
        stages[1] = {nri::StageBits::FRAGMENT_SHADER, vertex_pixel_source, "main", "rhi_vertex_ps"};
    }
    PipelineCompileDesc desc{};
    desc.kind = kind;
    desc.debug_name = kind == PipelineKind::mesh ? "rhi_acceptance_mesh" : "rhi_acceptance_vertex";
    desc.stages = stages;
    desc.graphics.color_formats[0] = nri::Format::RGBA8_UNORM;
    desc.graphics.color_attachment_count = 1u;
    desc.graphics.depth_format = nri::Format::D32_SFLOAT;
    desc.graphics.depth_test = true;
    desc.graphics.depth_write = true;
    desc.graphics.depth_compare = nri::CompareOp::LESS;
    desc.graphics.cull_mode = nri::CullMode::NONE;
    return take(BackendAccess::compile_pipeline(device, desc));
}

Pipeline prepare_compute(Device& device) {
    const std::array<ShaderStageSource, 1> stages{{
        {nri::StageBits::COMPUTE_SHADER, compute_source, "main", "rhi_compute_cs"},
    }};
    PipelineCompileDesc desc{};
    desc.kind = PipelineKind::compute;
    desc.debug_name = "rhi_acceptance_compute";
    desc.stages = stages;
    return take(BackendAccess::compile_pipeline(device, desc));
}

nri::Buffer* create_readback_buffer(Device& device, std::uint64_t byte_size) {
    nri::BufferDesc desc{};
    desc.size = byte_size;
    nri::Buffer* buf = nullptr;
    check_nri(BackendAccess::core(device).CreateCommittedBuffer(BackendAccess::nri_device(device),
                                                                nri::MemoryLocation::HOST_READBACK, 0.0f, desc, buf));
    return buf;
}

void scene(Device& device, atelier::rdg::Executor& executor, Pipeline& compute, Pipeline& mesh, Pipeline& vertex) {
    constexpr std::uint32_t width = 64u;
    constexpr std::uint32_t height = 32u;
    constexpr std::uint32_t row_pitch = 256u; // 64 * 4 == 256-byte row alignment
    constexpr std::uint32_t image_bytes = row_pitch * height;

    const auto& core = BackendAccess::core(device);
    nri::Buffer* mesh_color = create_readback_buffer(device, image_bytes);
    nri::Buffer* vertex_color = create_readback_buffer(device, image_bytes);
    nri::Buffer* depth_readback = create_readback_buffer(device, image_bytes);
    nri::Buffer* out_buf = create_readback_buffer(device, 15u * sizeof(std::uint32_t));

    atelier::rdg::LinearArena arena;
    atelier::rdg::Graph graph(arena);

    atelier::rdg::TextureDesc color_desc{};
    color_desc.format = nri::Format::RGBA8_UNORM;
    color_desc.width = width;
    color_desc.height = height;
    const auto color_tex = graph.create_texture("rhi_color", color_desc);

    atelier::rdg::TextureDesc depth_desc{};
    depth_desc.format = nri::Format::D32_SFLOAT;
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.clear_value.depthStencil = {1.0f, 0u};
    const auto depth_tex = graph.create_texture("rhi_depth", depth_desc);

    const auto buf_a = graph.create_buffer("buf_a", {.size = 16u, .structure_stride = 4u});
    const auto buf_b = graph.create_buffer("buf_b", {.size = 16u, .structure_stride = 4u});
    const auto mesh_args = graph.create_buffer("mesh_args", {.size = 12u, .structure_stride = 4u});
    const auto draw_args = graph.create_buffer("draw_args", {.size = 16u, .structure_stride = 4u});

    const auto rb_mesh =
        graph.import_buffer("rb_mesh", mesh_color, {}, {nri::AccessBits::HOST_READ, nri::StageBits::HOST});
    const auto rb_vert =
        graph.import_buffer("rb_vert", vertex_color, {}, {nri::AccessBits::HOST_READ, nri::StageBits::HOST});
    const auto rb_depth =
        graph.import_buffer("rb_depth", depth_readback, {}, {nri::AccessBits::HOST_READ, nri::StageBits::HOST});
    const auto rb_out = graph.import_buffer("rb_out", out_buf, {}, {nri::AccessBits::HOST_READ, nri::StageBits::HOST});

    struct ComputeConsts {
        std::uint32_t seed;

        std::uint32_t bias;

        std::uint32_t values_uav;

        std::uint32_t mesh_args_uav;

        std::uint32_t draw_args_uav;
    };

    graph.add_pass(
        "ComputeSetup", QueueKind::compute,
        [&](atelier::rdg::PassBuilder& b) {
            b.write_buffer(buf_a);
            b.write_buffer(buf_b);
            b.write_buffer(mesh_args);
            b.write_buffer(draw_args);
        },
        [&](const atelier::rdg::PassContext& ctx) -> Result<void> {
            ctx.set_pipeline(compute);
            for (std::uint32_t pass = 0u; pass < 2u; ++pass) {
                ComputeConsts c{pass == 0u ? 33u : 73u, 7u, ctx.uav(pass == 0u ? buf_a : buf_b), ctx.uav(mesh_args),
                                ctx.uav(draw_args)};
                ctx.push_constants(c, nri::BindPoint::COMPUTE);
                ctx.core().CmdDispatch(ctx.cmd(), {1u, 1u, 1u});
            }
            return {};
        });

    graph.add_pass(
        "MeshDraw", QueueKind::graphics,
        [&](atelier::rdg::PassBuilder& b) {
            b.read_buffer(buf_a, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER);
            b.read_buffer(mesh_args, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
            b.render_target(color_tex, nri::LoadOp::CLEAR, nri::StoreOp::STORE, {});
            b.depth_target(depth_tex, nri::LoadOp::CLEAR, nri::StoreOp::STORE, 1.0f);
        },
        [&](const atelier::rdg::PassContext& ctx) -> Result<void> {
            const nri::Viewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                                   1.0f, false};
            const nri::Rect sc{0, 0, static_cast<nri::Dim_t>(width), static_cast<nri::Dim_t>(height)};
            ctx.core().CmdSetViewports(ctx.cmd(), &vp, 1u);
            ctx.core().CmdSetScissors(ctx.cmd(), &sc, 1u);
            ctx.begin_rendering();
            ctx.set_pipeline(mesh);
            const std::uint32_t srv = ctx.srv(buf_a);
            ctx.push_constants(srv, nri::BindPoint::GRAPHICS);
            ctx.mesh_shader().CmdDrawMeshTasksIndirect(ctx.cmd(), *ctx.buffer(mesh_args), 0u, 1u,
                                                       sizeof(nri::DrawMeshTasksDesc), nullptr, 0u);
            ctx.end_rendering();
            return {};
        });

    graph.add_pass(
        "CopyMeshColor", QueueKind::graphics,
        [&](atelier::rdg::PassBuilder& b) {
            b.copy_source_texture(color_tex);
            b.copy_dest_buffer(rb_mesh);
        },
        [&](const atelier::rdg::PassContext& ctx) -> Result<void> {
            nri::TextureDataLayoutDesc layout{0u, row_pitch, image_bytes};
            nri::TextureRegionDesc region{0u, 0u, 0u, width, height, 1u, 0u, 0u, nri::PlaneBits::COLOR};
            ctx.core().CmdReadbackTextureToBuffer(ctx.cmd(), *ctx.buffer(rb_mesh), layout, *ctx.texture(color_tex),
                                                  region);
            return {};
        });

    graph.add_pass(
        "VertexDraw", QueueKind::graphics,
        [&](atelier::rdg::PassBuilder& b) {
            b.read_buffer(buf_b, nri::AccessBits::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER);
            b.read_buffer(draw_args, nri::AccessBits::ARGUMENT_BUFFER, nri::StageBits::INDIRECT);
            b.render_target(color_tex, nri::LoadOp::LOAD, nri::StoreOp::STORE, {});
            b.depth_target(depth_tex, nri::LoadOp::LOAD, nri::StoreOp::STORE, 1.0f);
        },
        [&](const atelier::rdg::PassContext& ctx) -> Result<void> {
            const nri::Viewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                                   1.0f, false};
            const nri::Rect sc{0, 0, static_cast<nri::Dim_t>(width), static_cast<nri::Dim_t>(height)};
            ctx.core().CmdSetViewports(ctx.cmd(), &vp, 1u);
            ctx.core().CmdSetScissors(ctx.cmd(), &sc, 1u);
            ctx.begin_rendering();
            ctx.set_pipeline(vertex);
            const std::uint32_t srv = ctx.srv(buf_b);
            ctx.push_constants(srv, nri::BindPoint::GRAPHICS);
            ctx.core().CmdDrawIndirect(ctx.cmd(), *ctx.buffer(draw_args), 0u, 1u, sizeof(nri::DrawDesc), nullptr, 0u);
            ctx.end_rendering();
            return {};
        });

    graph.add_pass(
        "FinalReadbacks", QueueKind::graphics,
        [&](atelier::rdg::PassBuilder& b) {
            b.copy_source_texture(color_tex);
            b.copy_source_texture(depth_tex);
            b.copy_source_buffer(buf_a);
            b.copy_source_buffer(buf_b);
            b.copy_source_buffer(mesh_args);
            b.copy_source_buffer(draw_args);
            b.copy_dest_buffer(rb_vert);
            b.copy_dest_buffer(rb_depth);
            b.copy_dest_buffer(rb_out);
        },
        [&](const atelier::rdg::PassContext& ctx) -> Result<void> {
            nri::TextureDataLayoutDesc layout{0u, row_pitch, image_bytes};
            nri::TextureRegionDesc color_reg{0u, 0u, 0u, width, height, 1u, 0u, 0u, nri::PlaneBits::COLOR};
            nri::TextureRegionDesc depth_reg{0u, 0u, 0u, width, height, 1u, 0u, 0u, nri::PlaneBits::DEPTH};
            ctx.core().CmdReadbackTextureToBuffer(ctx.cmd(), *ctx.buffer(rb_vert), layout, *ctx.texture(color_tex),
                                                  color_reg);
            ctx.core().CmdReadbackTextureToBuffer(ctx.cmd(), *ctx.buffer(rb_depth), layout, *ctx.texture(depth_tex),
                                                  depth_reg);
            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_out), 0u, *ctx.buffer(buf_a), 0u, 16u);
            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_out), 16u, *ctx.buffer(buf_b), 0u, 16u);
            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_out), 32u, *ctx.buffer(mesh_args), 0u, 12u);
            ctx.core().CmdCopyBuffer(ctx.cmd(), *ctx.buffer(rb_out), 44u, *ctx.buffer(draw_args), 0u, 16u);
            return {};
        });

    atelier::rdg::TraceSnapshot trace;
    take(executor.execute(graph, {.frame_id = 1u, .collect_timestamps = true, .trace = &trace}));
    take(device.wait_idle());
    require(trace.entered && trace.closed && !trace.interrupted && trace.passes.size() == 5u,
            "RDG trace snapshot mismatch");

    const auto* red = static_cast<const std::uint8_t*>(core.MapBuffer(*mesh_color, 0u, image_bytes));
    const auto* green = static_cast<const std::uint8_t*>(core.MapBuffer(*vertex_color, 0u, image_bytes));
    const auto* z = static_cast<const float*>(core.MapBuffer(*depth_readback, 0u, image_bytes));
    require(red && green && z, "image readback mapping failed");
    for (std::uint32_t pixel = 0u; pixel < width * height; ++pixel) {
        require(red[pixel * 4u] == 40u && red[pixel * 4u + 1u] == 0u && red[pixel * 4u + 2u] == 0u &&
                    red[pixel * 4u + 3u] == 255u,
                "mesh pixel result mismatch");
        require(green[pixel * 4u] == 0u && green[pixel * 4u + 1u] == 80u && green[pixel * 4u + 2u] == 0u &&
                    green[pixel * 4u + 3u] == 255u,
                "vertex pixel result mismatch");
        require(std::fabs(z[pixel] - 0.25f) < 0.00001f, "depth result mismatch");
    }
    core.UnmapBuffer(*mesh_color);
    core.UnmapBuffer(*vertex_color);
    core.UnmapBuffer(*depth_readback);

    const auto* parameters =
        static_cast<const std::uint32_t*>(core.MapBuffer(*out_buf, 0u, 15u * sizeof(std::uint32_t)));
    require(parameters != nullptr, "parameter readback mapping failed");
    const std::array<std::uint32_t, 15> expected = {40u, 0u, 0u, 1u, 80u, 0u, 0u, 1u, 1u, 1u, 1u, 3u, 1u, 0u, 0u};
    for (std::size_t i = 0u; i < expected.size(); ++i)
        require(parameters[i] == expected[i], "compute/indirect parameter mismatch");
    core.UnmapBuffer(*out_buf);

    for (nri::Buffer* buf : {mesh_color, vertex_color, depth_readback, out_buf})
        core.DestroyBuffer(buf);

    std::printf("scene: color-pixels=%u depth-pixels=%u parameter-words=15 mismatches=0\n", width * height * 2u,
                width * height);
    std::puts("compute/draw/readback=PASS");
}

} // namespace

int main() {
    atelier::install_fatal_handler();
    DeviceOptions options{};
    options.validation = Validation::gpu;
    auto device = take(Device::open(options));
    require(device.info().debug_layer && device.info().gpu_validation == expected_gpu_validation(),
            "validation differs from the requested mode");
    print_adapter(device);
    auto debug = debug_queue(BackendAccess::d3d12_device(device));
    std::printf("debug-layer=%u gpu-validation=%u\n", unsigned(device.info().debug_layer),
                unsigned(device.info().gpu_validation));

    auto executor = take(atelier::rdg::Executor::open(device));
    auto compute = prepare_compute(device);
    auto mesh = prepare_graphics(device, PipelineKind::mesh);
    auto vertex = prepare_graphics(device, PipelineKind::vertex);

    scene(device, executor, compute, mesh, vertex);

    compute.reset();
    mesh.reset();
    vertex.reset();
    take(executor.close());
    take(device.wait_idle());
    check_debug(debug.Get());
    take(device.close());
    check_debug(debug.Get());
    debug.Reset();

    Pipeline survivor;
    ComPtr<ID3D12InfoQueue> retained_debug;
    {
        auto temporary = take(Device::open(options));
        retained_debug = debug_queue(BackendAccess::d3d12_device(temporary));
        survivor = prepare_compute(temporary);
    }
    require(survivor && BackendAccess::nri_pipeline(survivor) != nullptr,
            "pipeline did not keep its execution domain alive");
    survivor.reset();
    check_debug(retained_debug.Get());
    retained_debug.Reset();

    std::puts("pipeline-retains-domain=PASS debug-warnings=0 debug-errors=0");
    std::puts("RHI acceptance: PASS");
    return 0;
}

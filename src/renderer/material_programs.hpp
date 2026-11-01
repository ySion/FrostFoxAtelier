#pragma once

#include "renderer.hpp"

#include <optional>
#include <thread>

namespace atelier::renderer::detail {

enum class PipelineStyle { ui, surface, cavity_surface, depth, visible_lines, all_lines, background, grid, grid_depth };
rhi::PipelineDesc studio_pipeline(const char* name, const char* vs, const char* ps, PipelineStyle);
rhi::PipelineDesc gbuffer_pipeline(const char* vs, const char* ps);
rhi::PipelineDesc shadow_pipeline(const char* vs, const char* ps);

// These are the existing material consumers, not arbitrary shader stages.
enum class MaterialPass : std::size_t {
    studio,
    cavity,
    depth,
    visible_lines,
    all_lines,
    gbuffer,
    shadow,
    compose,
    sun_trace,
    probe_trace,
    probe_update,
    probe_resolve,
    probe_upsample,
    probe_exact,
    inspect,
    inspect_update,
    probe_coverage,
    count
};
struct ProgramSurfaceRecord {
    std::uint32_t program{};
    std::uint32_t parameter_base{};
    std::uint32_t input_base{};
    std::array<std::uint32_t, 3> flags{}; // Masked, double sided, two-sided emission.
};
static_assert(sizeof(ProgramSurfaceRecord) == 24);
struct ProgramInputRecord {
    std::uint32_t slot{};
    std::uint32_t texture{~0u};
    std::uint32_t address{};
    std::uint32_t count{};
    math::Vector4 empty_value{};
};
static_assert(sizeof(ProgramInputRecord) == 32);
// Shared with the existing image cache. A ready but not yet adopted batch pins
// its uploads independently of which images the last rendered frame used.
struct MaterialImageResource {
    material::ImageRef owner;
    GfxContext retirement_context;
    GfxTexture texture{};
    MaterialImageResource(material::ImageRef image, GfxContext gfx)
        : owner(std::move(image)), retirement_context(gfx) {}
    ~MaterialImageResource();
    MaterialImageResource(const MaterialImageResource&) = delete;
    MaterialImageResource& operator=(const MaterialImageResource&) = delete;
};
struct MaterialProgramSet {
    // Canonical program order, independent of MI count and slot order.
    // Definitions retain the exact code without borrowing a compilation Runtime.
    std::vector<material::DefinitionRef> definitions;
    // One bit per program: any Surface using it is Mask, across all definitions.
    // This classification is part of the compiled pipeline-set identity.
    std::vector<bool> masked_programs;
    std::array<rhi::Pipeline, static_cast<std::size_t>(MaterialPass::count)> pipelines;
    struct Pending {
        MaterialPass pass;
        rhi::PipelineJob job;
    };
    std::vector<Pending> pending;
    std::optional<Error> failure;
    void submit(rhi::Context&);
    Result<Preparation> preparation(rhi::Context&);
};

struct MaterialBatchState {
    rhi::Context& context;
    // Renderer cannot close with a live batch. Keep the original borrow only
    // for retirement: Context::borrow() stops admitting work after failure.
    const GfxContext retirement_context;
    std::size_t& live_batches;
    const std::thread::id owner = std::this_thread::get_id();
    std::vector<material::ProgramSurfaceRef> surfaces;
    std::vector<ProgramSurfaceRecord> records;
    std::vector<std::byte> parameters;
    std::vector<ProgramInputRecord> inputs;
    std::vector<std::pair<material::ImageRef, bool>> images;
    std::vector<std::shared_ptr<MaterialImageResource>> image_owners;
    std::vector<GfxTexture> textures;
    std::shared_ptr<MaterialProgramSet> code;
    std::optional<Error> failure;
    GfxBuffer surface_buffer{};
    GfxBuffer parameter_buffer{};
    GfxBuffer input_buffer{};
    bool uploading{};
    bool ready{};

    MaterialBatchState(rhi::Context&, std::size_t&);
    ~MaterialBatchState();
    MaterialBatchState(const MaterialBatchState&) = delete;
    MaterialBatchState& operator=(const MaterialBatchState&) = delete;
    void submit(std::span<const material::ProgramSurfaceRef>, std::vector<std::weak_ptr<MaterialProgramSet>>&);
    void upload(); // Called only after pipelines and Renderer-owned images are ready.
    const rhi::Pipeline& pipeline(MaterialPass pass) const { return code->pipelines[static_cast<std::size_t>(pass)]; }
};

} // namespace atelier::renderer::detail

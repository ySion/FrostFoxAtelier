#pragma once

#include "assets/atx_texture.hpp"
#include "gpu_resources.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace atelier::renderer::detail {

// 32B GPU surface table record indexed by `material_slot_map[inst.material_slot_offset + face_material_slot]`.
struct alignas(16) GpuSurfaceRecord {
    std::uint32_t archetype_index = 0u; // Index into active MaterialProgramSet's archetypes[]

    std::uint32_t parameter_base = 0u; // Byte offset inside `parameter_buffer`

    std::uint32_t input_base = 0u; // Element offset inside `input_buffer`

    std::uint32_t flags = 0u; // bit 0: masked, bit 1: double_sided, bit 2: emission_two_sided, bit 3: fully_discarded

    std::uint32_t masked_bin_index = 0xFFFFFFFFu; // Compact index into masked_archetype_indices (0xFFFFFFFF if opaque)

    std::uint32_t masked_cov_index =
        0xFFFFFFFFu; // Deduplicated slice index into MaskedCoverageSet (0xFFFFFFFF if opaque)

    std::uint32_t pad1 = 0u;

    std::uint32_t pad2 = 0u;
};

static_assert(sizeof(GpuSurfaceRecord) == 32);

// 32B GPU texture input binding record.
struct alignas(16) GpuTextureInputRecord {
    std::uint32_t slot = 0u;

    std::uint32_t texture_srv = 0xFFFFFFFFu;

    std::uint32_t address_mode = 0u; // 0: repeat, 1: clamp, 2: mirror

    std::uint32_t input_count_and_role =
        0u; // bits 0..15: count, bit 16: bc5_rg, bit 17: bc4_scalar, bit 18: has_toksvig, bits 24..31: toksvig_u8

    std::array<float, 4> empty_value{};
};

static_assert(sizeof(GpuTextureInputRecord) == 32);

struct ResolvedMaterialImage {
    std::uint32_t srv = 0xFFFFFFFFu;

    std::uint32_t role_flags = 0u;

    float toksvig_slope = 0.0f;
};

struct MaterialImageResource {
    material::ImageRef source;

    assets::AtxRole role = assets::AtxRole::linear_rgba;

    assets::AtxFormat format = assets::AtxFormat::rgba8_unorm;

    std::shared_ptr<rhi::Device> device;

    OwnedTexture gpu;

    float toksvig_slope = 0.0f; // Mean mip-1..N normal length falloff for BC5 Toksvig Specular AA

    MaterialImageResource(material::ImageRef src, assets::AtxRole r, std::shared_ptr<rhi::Device> dev) noexcept
        : source(std::move(src)), role(r), device(std::move(dev)) {}

    ~MaterialImageResource() {
        if (device && *device)
            gpu.retire(*device);
    }
};

// Cached GPU pipelines for ONE single `.source` MaterialProgram!
// Editing a `.source` recompiles ONLY that archetype's own PSOs — zero 17-PSO megashader coupling!
struct ProgramArchetype {
    material::DefinitionRef definition;

    bool base_submitted = false;

    bool masked_submitted = false;

    enum class Stage : std::uint8_t {
        shade_bin_cs = 0, // [numthreads(64,1,1)] wrapping hlsl_footprint -> writes 3x8B OpenPBR Slab
        studio_bin_cs = 1, // [numthreads(64,1,1)] wrapping hlsl_footprint -> writes Studio/Cavity color+normal
        gi_hit_cs = 2, // [numthreads(64,1,1)] wrapping hlsl_gi_hit -> evaluates secondary GI hits
        masked_vis_ms = 3, // Mesh + PS wrapping hlsl_coverage -> writes VisBuffer + D32F
        masked_vsm_ms = 4, // Mesh + PS wrapping hlsl_coverage -> writes D32F VSM Physical Page
        count = 5
    };

    struct Pending {
        Stage stage{};

        rhi::PipelineJob job;
    };

    std::vector<Pending> base_pending;

    std::optional<Error> base_failure;

    rhi::PipelineJob masked_job;

    rhi::PipelineJob masked_vsm_job;

    std::optional<Error> masked_failure;

    std::array<rhi::Pipeline, static_cast<std::size_t>(Stage::count)> pipelines{};

    [[nodiscard]] Result<void> submit(rhi::Device& device, bool compile_masked);

    [[nodiscard]] Result<Preparation> poll(rhi::Device& device, bool use_masked);
};

// Independently cached Masked RayQuery PSO keyed strictly by deduplicated canonical coverage slices!
// Adding or editing opaque materials never recompiles `shadow_ray_masked_cs`.
struct MaskedCoverageSet {
    std::vector<std::string> canonical_coverage_slices;

    rhi::PipelineJob job;

    rhi::Pipeline pso;

    std::optional<Error> failure;

    [[nodiscard]] Result<Preparation> poll(rhi::Device& device);
};

// Lightweight set of `ProgramArchetype`s referenced by a `MaterialBatch` + shared `MaskedCoverageSet`.
struct MaterialProgramSet {
    std::vector<std::shared_ptr<ProgramArchetype>> archetypes;

    std::vector<bool> masked_flags;

    std::vector<std::uint32_t> masked_archetype_indices;

    std::shared_ptr<MaskedCoverageSet> masked_coverage;

    std::optional<Error> failure;

    [[nodiscard]] const rhi::Pipeline& ray_shadow_masked_pso() const noexcept {
        static const rhi::Pipeline empty{};
        return masked_coverage ? masked_coverage->pso : empty;
    }

    [[nodiscard]] Result<Preparation> preparation(rhi::Device& device);
};

struct MaterialBatchState {
    const std::thread::id owner = std::this_thread::get_id();

    const void* owner_renderer = nullptr;

    std::shared_ptr<rhi::Device> device;

    std::size_t& live_batches;

    std::vector<material::ProgramSurfaceRef> surfaces;

    std::vector<GpuSurfaceRecord> records;

    std::vector<std::byte> parameters;

    std::vector<GpuTextureInputRecord> inputs;

    struct ImageRequest {
        material::ImageRef image;

        assets::AtxRole role = assets::AtxRole::linear_rgba;
    };

    std::vector<ImageRequest> image_requests;

    std::vector<std::shared_ptr<MaterialImageResource>> image_owners;

    std::shared_ptr<MaterialProgramSet> program_set;

    OwnedBuffer surface_buffer;

    OwnedBuffer parameter_buffer;

    OwnedBuffer input_buffer;

    bool ready = false;

    std::optional<Error> failure;

    MaterialBatchState(const void* renderer_tag, std::shared_ptr<rhi::Device> dev, std::size_t& count) noexcept
        : owner_renderer(renderer_tag), device(std::move(dev)), live_batches(count) {
        ++live_batches;
    }

    ~MaterialBatchState();

    [[nodiscard]] Result<void>
    submit(std::span<const material::ProgramSurfaceRef> input,
           std::map<const source::MaterialProgram*, std::weak_ptr<ProgramArchetype>>& archetype_cache,
           std::vector<std::weak_ptr<MaterialProgramSet>>& set_cache,
           std::vector<std::weak_ptr<MaskedCoverageSet>>& masked_coverage_cache);

    [[nodiscard]] Result<void> stage_upload(std::span<const ResolvedMaterialImage> resolved_images, UploadBatch& batch);
};

} // namespace atelier::renderer::detail

#include "material_archetypes.hpp"
#include "memory/fatal.hpp"
#include "shader_source.hpp"

#include <cmath>
#include <limits>

namespace atelier::renderer::detail {
namespace {

assets::AtxRole classify_texture_role(bool is_normal_map, material::TextureColor color,
                                      const material::Image* img) noexcept {
    if (is_normal_map)
        return assets::AtxRole::normal_bc5;
    if (color == material::TextureColor::srgb)
        return assets::AtxRole::color_srgb;
    if (img && img->is_grayscale())
        return assets::AtxRole::scalar_bc4;
    return assets::AtxRole::linear_rgba;
}

std::string make_material_bridge_hlsl() {
    return R"hlsl(
#ifndef ATELIER_SAFE_DESC_DEFINED
#define ATELIER_SAFE_DESC_DEFINED 1
uint atelier_safe_desc(uint idx) {
    return (idx < 262144u) ? idx : 0u;
}
#endif

struct MaterialInputRecord {
    uint slot;
    uint texture_srv;
    uint address_mode;
    uint count_and_role;
    float4 empty_value;
};

cbuffer ArchetypeRootConstants : register(b0, space0) {
    uint g_frame_cb_srv;
    uint g_frame_cb_idx;
    uint g_material_inputs_srv;
    uint g_material_params_srv;
    uint g_material_surfaces_srv;
    uint g_archetype_index;
    uint g_bin_offset_or_cluster_srv;
    uint g_aux0_srv;
    uint g_aux1_srv;
    uint g_aux2_uav;
    uint g_aux3_uav;
    uint g_aux4_uav;
    uint g_aux5_uav;
    uint g_pass_flags;
    uint g_viewport_packed0;
    uint g_viewport_packed1;
};

static float g_accum_toksvig_var = 0.0;

MaterialInputRecord atelier_find_input(uint environment, uint local_slot) {
    StructuredBuffer<MaterialInputRecord> tbl = ResourceDescriptorHeap[atelier_safe_desc(g_material_inputs_srv)];
    uint count = tbl[environment].count_and_role & 0xFFFFu;
    uint idx = (local_slot < count) ? (environment + local_slot) : environment;
    return tbl[idx];
}

uint atelier_sampler_for_address(uint addr) {
    return addr == 1u ? 6u : (addr == 2u ? 7u : 5u);
}

float4 atelier_decode_sample(MaterialInputRecord rec, float4 raw) {
    // bit 16: BC5_RG_UNORM stores X, Y in [0,1] with B=0, A=1; reconstruct +Z in [0.5, 1.0] so `texel*2-1` yields exact 3D tangent normal!
    if ((rec.count_and_role & 0x10000u) != 0u) {
        float2 xy = raw.xy * 2.0 - 1.0;
        float z = sqrt(saturate(1.0 - dot(xy, xy)));
        return float4(raw.x, raw.y, z * 0.5 + 0.5, 1.0);
    }
    // bit 17: BC4_R_UNORM stores scalar in R with G=0, B=0, A=1; splat R across RGB for lossless grayscale channel swizzles!
    if ((rec.count_and_role & 0x20000u) != 0u) {
        return float4(raw.xxx, 1.0);
    }
    return raw;
}

float4 atelier_material_sample(uint environment, uint local_slot, float2 uv) {
    MaterialInputRecord rec = atelier_find_input(environment, local_slot);
    if (rec.texture_srv == 0xFFFFFFFFu) return rec.empty_value;
    Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(rec.texture_srv))];
    SamplerState smp = SamplerDescriptorHeap[NonUniformResourceIndex(atelier_sampler_for_address(rec.address_mode))];
    return atelier_decode_sample(rec, tex.SampleLevel(smp, uv, 0.0));
}

float2 atelier_material_sample_orientation(uint environment, uint local_slot, float2 uv) {
    MaterialInputRecord rec = atelier_find_input(environment, local_slot);
    if (rec.address_mode != 2u) return float2(1.0, 1.0);
    precise float2 tile = floor(uv * 0.5);
    precise float2 folded = uv - tile * 2.0;
    return float2(folded.x > 1.0 ? -1.0 : 1.0, folded.y > 1.0 ? -1.0 : 1.0);
}

float3 atelier_material_sample_normal(uint environment, uint local_slot, float2 uv) {
    float4 raw = atelier_material_sample(environment, local_slot, uv);
    precise float2 orient = atelier_material_sample_orientation(environment, local_slot, uv);
    precise float ex = (raw.x * 2.0 - 1.0) * orient.x;
    precise float ey = (raw.y * 2.0 - 1.0) * orient.y;
    precise float ez = sqrt(clamp(1.0 - ex * ex - ey * ey, 0.0, 1.0));
    return float3(ex, ey, ez);
}

bool atelier_finite2(float2 v) { return all((asuint(v) & 0x7FFFFFFFu) < 0x7F800000u); }

float4 atelier_material_sample_grad_impl(uint environment, uint local_slot, float2 uv, float2 d0, float2 d1, bool accum_toksvig) {
    MaterialInputRecord rec = atelier_find_input(environment, local_slot);
    if (rec.texture_srv == 0xFFFFFFFFu) return rec.empty_value;
    Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(atelier_safe_desc(rec.texture_srv))];
    uint width, height, levels;
    tex.GetDimensions(0, width, height, levels);
    precise float2 t0 = d0 * float2(width, height);
    precise float2 t1 = d1 * float2(width, height);
    SamplerState smp = SamplerDescriptorHeap[NonUniformResourceIndex(atelier_sampler_for_address(rec.address_mode))];
    if (!atelier_finite2(d0) || !atelier_finite2(d1) || !atelier_finite2(t0) || !atelier_finite2(t1)) {
        return atelier_decode_sample(rec, tex.SampleLevel(smp, uv, float(levels - 1u)));
    }
    if (accum_toksvig && (rec.count_and_role & 0x40000u) != 0u) {
        float rho2 = max(dot(t0, t0), dot(t1, t1));
        float mip = clamp(0.5 * log2(max(rho2, 1.0)), 0.0, float(levels - 1u));
        float tok_slope = float((rec.count_and_role >> 24u) & 0xFFu) * (1.0 / 255.0);
        g_accum_toksvig_var = max(g_accum_toksvig_var, saturate(mip * 0.25) * tok_slope);
    }
    return atelier_decode_sample(rec, tex.SampleGrad(smp, uv, d0, d1));
}

float4 atelier_material_sample_grad(uint environment, uint local_slot, float2 uv, float2 d0, float2 d1) {
    return atelier_material_sample_grad_impl(environment, local_slot, uv, d0, d1, true);
}

#define ATELIER_MATERIAL_SAMPLE_GRAD_OFFSET_DEFINED 1
float4 atelier_material_sample_grad_offset(uint environment, uint local_slot, float2 uv, float2 d0, float2 d1) {
    return atelier_material_sample_grad_impl(environment, local_slot, uv, d0, d1, false);
}
)hlsl";
}

} // namespace

Result<void> ProgramArchetype::submit(rhi::Device& device, bool compile_masked) {
    if (base_failure)
        return std::unexpected(*base_failure);
    if (compile_masked && masked_failure)
        return std::unexpected(*masked_failure);
    if (base_submitted && (!compile_masked || masked_submitted))
        return {};

    // Pre-validate masked coverage slice BEFORE submitting base PSOs so deterministic coverage errors never spawn orphan jobs!
    std::optional<std::string> cov_hlsl;
    if (compile_masked && !masked_submitted) {
        auto cov = definition->program().hlsl_coverage("ArchCov");
        if (!cov) {
            masked_failure = make_error(rhi::ErrorCode::invalid_argument, std::move(cov.error().message));
            return std::unexpected(*masked_failure);
        }
        cov_hlsl = std::string(cov->view().std_view());
    }

    const std::string bridge = make_material_bridge_hlsl();

    if (!base_submitted) {
        auto fail_base = [&](Error err) -> Result<void> {
            base_failure = std::move(err);
            base_pending.clear();
            return std::unexpected(*base_failure);
        };

        auto fp = definition->program().hlsl_footprint("Arch");
        if (!fp)
            return fail_base(make_error(rhi::ErrorCode::invalid_argument, std::move(fp.error().message)));

        auto gi = definition->program().hlsl_gi_hit("ArchGi");
        if (!gi)
            return fail_base(make_error(rhi::ErrorCode::invalid_argument, std::move(gi.error().message)));

        // 1. OpenPBR Canonical Slab Shade Bin CS
        {
            const std::string src =
                bridge + fp->c_str() + "\n#define ARCH_STUDIO_MODE 0\n" + shaders::material_shade_bin_cs;
            const std::array<rhi::ShaderStageSource, 1> stages{
                {{nri::StageBits::COMPUTE_SHADER, src.c_str(), "main", "material_shade_bin_cs"}}};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::compute;
            desc.debug_name = "arch_shade_bin_cs";
            desc.stages = stages;
            auto job = rhi::BackendAccess::submit_pipeline(device, desc);
            if (!job)
                return fail_base(Error{job.error(), false});
            base_pending.push_back({Stage::shade_bin_cs, std::move(*job)});
        }

        // 2. Studio / Cavity Bin CS
        {
            const std::string src =
                bridge + fp->c_str() + "\n#define ARCH_STUDIO_MODE 1\n" + shaders::material_shade_bin_cs;
            const std::array<rhi::ShaderStageSource, 1> stages{
                {{nri::StageBits::COMPUTE_SHADER, src.c_str(), "main", "studio_shade_bin_cs"}}};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::compute;
            desc.debug_name = "arch_studio_bin_cs";
            desc.stages = stages;
            auto job = rhi::BackendAccess::submit_pipeline(device, desc);
            if (!job)
                return fail_base(Error{job.error(), false});
            base_pending.push_back({Stage::studio_bin_cs, std::move(*job)});
        }

        // 3. Secondary GI Hit CS (pruned normal_texture/TBN + OpenPBR §3.10 effective albedo & foliage transmission)
        {
            const std::string src = bridge + gi->c_str() + "\n" + shaders::secondary_gi_hit_cs;
            const std::array<rhi::ShaderStageSource, 1> stages{
                {{nri::StageBits::COMPUTE_SHADER, src.c_str(), "main", "secondary_gi_hit_cs"}}};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::compute;
            desc.debug_name = "arch_gi_hit_cs";
            desc.stages = stages;
            auto job = rhi::BackendAccess::submit_pipeline(device, desc);
            if (!job)
                return fail_base(Error{job.error(), false});
            base_pending.push_back({Stage::gi_hit_cs, std::move(*job)});
        }
        base_submitted = true;
    }

    // 4. Optional Masked VisBuffer + Masked VSM Mesh+PS PSOs (isolated from base Opaque PSOs!)
    if (compile_masked && !masked_submitted) {
        if (!pipelines[static_cast<std::size_t>(Stage::masked_vis_ms)] && !masked_job) {
            const std::string ms_vis = bridge + "\n#define VIS_MASKED 1\n" + shaders::visbuffer_mesh;
            const std::string ps_vis = bridge + *cov_hlsl + "\n#define VIS_MASKED 1\n" + shaders::visbuffer_masked_ps;
            const std::array<rhi::ShaderStageSource, 2> stages{{
                {nri::StageBits::MESH_SHADER, ms_vis.c_str(), "vis_ms_main", "masked_vis_ms"},
                {nri::StageBits::FRAGMENT_SHADER, ps_vis.c_str(), "vis_ps_main", "masked_vis_ps"},
            }};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::mesh;
            desc.debug_name = "arch_masked_vis_pso";
            desc.stages = stages;
            desc.graphics.color_formats[0] = nri::Format::R32_UINT;
            desc.graphics.color_attachment_count = 1u;
            desc.graphics.depth_format = nri::Format::D32_SFLOAT;
            desc.graphics.depth_test = true;
            desc.graphics.depth_write = true;
            desc.graphics.depth_compare = nri::CompareOp::GREATER;
            desc.graphics.cull_mode = nri::CullMode::NONE;
            auto job = rhi::BackendAccess::submit_pipeline(device, desc);
            if (!job) {
                masked_failure = Error{job.error(), false};
                return std::unexpected(*masked_failure);
            }
            masked_job = std::move(*job);
        }
        if (!pipelines[static_cast<std::size_t>(Stage::masked_vsm_ms)] && !masked_vsm_job) {
            const std::string ms_vsm = bridge + "\n#define VSM_MASKED 1\n" + shaders::vsm_mesh;
            const std::string ps_vsm = bridge + *cov_hlsl + "\n#define VSM_MASKED 1\n" + shaders::visbuffer_masked_ps;
            const std::array<rhi::ShaderStageSource, 2> stages{{
                {nri::StageBits::MESH_SHADER, ms_vsm.c_str(), "vsm_ms_main", "masked_vsm_ms"},
                {nri::StageBits::FRAGMENT_SHADER, ps_vsm.c_str(), "vsm_ps_main", "masked_vsm_ps"},
            }};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::mesh;
            desc.debug_name = "arch_masked_vsm_pso";
            desc.stages = stages;
            desc.graphics.color_attachment_count = 0u;
            desc.graphics.depth_format = nri::Format::D32_SFLOAT;
            desc.graphics.depth_test = true;
            desc.graphics.depth_write = true;
            desc.graphics.depth_compare = nri::CompareOp::GREATER;
            desc.graphics.depth_clamp = true;
            desc.graphics.cull_mode = nri::CullMode::NONE;
            auto job = rhi::BackendAccess::submit_pipeline(device, desc);
            if (!job) {
                masked_job = {};
                masked_failure = Error{job.error(), false};
                return std::unexpected(*masked_failure);
            }
            masked_vsm_job = std::move(*job);
        }
        masked_submitted = true;
    }

    return {};
}

Result<Preparation> ProgramArchetype::poll(rhi::Device& device, bool use_masked) {
    if (base_failure)
        return std::unexpected(*base_failure);
    for (auto it = base_pending.begin(); it != base_pending.end();) {
        auto taken = rhi::BackendAccess::take_pipeline(device, it->job);
        if (!taken) {
            ++it;
            continue;
        }
        if (!*taken) {
            base_failure = Error{taken->error(), false};
            base_pending.clear();
            return std::unexpected(*base_failure);
        }
        pipelines[static_cast<std::size_t>(it->stage)] = std::move(**taken);
        it = base_pending.erase(it);
    }

    std::uint32_t total = 3u;
    std::uint32_t done = 3u - std::min<std::uint32_t>(3u, static_cast<std::uint32_t>(base_pending.size()));
    bool ready = base_pending.empty();

    if (use_masked) {
        if (masked_failure)
            return std::unexpected(*masked_failure);
        total += 2u;
        auto poll_one = [&](Stage st, rhi::PipelineJob& jb) -> Result<void> {
            if (pipelines[static_cast<std::size_t>(st)]) {
                ++done;
            } else if (jb) {
                auto taken = rhi::BackendAccess::take_pipeline(device, jb);
                if (!taken) {
                    ready = false;
                } else if (!*taken) {
                    masked_failure = Error{taken->error(), false};
                    masked_job = {};
                    masked_vsm_job = {};
                    return std::unexpected(*masked_failure);
                } else {
                    pipelines[static_cast<std::size_t>(st)] = std::move(**taken);
                    ++done;
                }
            } else {
                ready = false;
            }
            return {};
        };
        ATELIER_TRY(poll_one(Stage::masked_vis_ms, masked_job));
        ATELIER_TRY(poll_one(Stage::masked_vsm_ms, masked_vsm_job));
    }

    return Preparation{done, total, ready};
}

Result<Preparation> MaskedCoverageSet::poll(rhi::Device& device) {
    if (failure)
        return std::unexpected(*failure);
    if (pso)
        return Preparation{1u, 1u, true};
    if (!job)
        return Preparation{0u, 1u, false};
    auto taken = rhi::BackendAccess::take_pipeline(device, job);
    if (!taken)
        return Preparation{0u, 1u, false};
    if (!*taken) {
        failure = Error{taken->error(), false};
        return std::unexpected(*failure);
    }
    pso = std::move(**taken);
    return Preparation{1u, 1u, true};
}

Result<Preparation> MaterialProgramSet::preparation(rhi::Device& device) {
    if (failure)
        return std::unexpected(*failure);

    std::uint32_t done = 0u;
    std::uint32_t total = 0u;
    bool all_ready = true;

    for (std::size_t i = 0; i < archetypes.size(); ++i) {
        auto p = archetypes[i]->poll(device, masked_flags[i]);
        if (!p) {
            failure = p.error();
            return p;
        }
        done += p->done;
        total += p->total;
        all_ready &= p->ready;
    }

    if (masked_coverage) {
        auto mp = masked_coverage->poll(device);
        if (!mp) {
            failure = mp.error();
            return mp;
        }
        done += mp->done;
        total += mp->total;
        all_ready &= mp->ready;
    }

    return Preparation{done, std::max(1u, total), all_ready};
}

MaterialBatchState::~MaterialBatchState() {
    if (owner != std::this_thread::get_id())
        atelier::fatal("Material batch destruction on a foreign thread");
    if (device && *device) {
        surface_buffer.retire(*device);
        parameter_buffer.retire(*device);
        input_buffer.retire(*device);
    }
    --live_batches;
}

Result<void>
MaterialBatchState::submit(std::span<const material::ProgramSurfaceRef> input,
                           std::map<const source::MaterialProgram*, std::weak_ptr<ProgramArchetype>>& archetype_cache,
                           std::vector<std::weak_ptr<MaterialProgramSet>>& set_cache,
                           std::vector<std::weak_ptr<MaskedCoverageSet>>& masked_coverage_cache) {
    if (input.empty() || input.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(GpuSurfaceRecord)) {
        return std::unexpected(make_error(rhi::ErrorCode::invalid_argument,
                                          "Material batch needs a representable nonempty Surface table"));
    }

    surfaces.assign(input.begin(), input.end());

    for (auto it = archetype_cache.begin(); it != archetype_cache.end();) {
        if (it->second.expired())
            it = archetype_cache.erase(it);
        else
            ++it;
    }

    struct ProgramUse {
        material::DefinitionRef definition;

        bool masked = false;
    };
    std::map<const source::MaterialProgram*, ProgramUse> unique_progs;
    for (const auto& surface : surfaces) {
        if (!surface)
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Material batch has a missing Surface"));
        if (surface->inputs().size() > 0xFFFFu)
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Material surface exceeds 65535 texture inputs"));
        auto& use = unique_progs.try_emplace(&surface->program(), ProgramUse{surface->definition()}).first->second;
        use.masked |= (surface->policy().coverage == material::Coverage::mask);
    }

    std::vector<std::shared_ptr<ProgramArchetype>> archetypes;
    std::vector<bool> masked_flags;
    std::vector<std::uint32_t> masked_arch_indices;
    std::vector<std::string> canonical_cov_slices;
    std::vector<std::string> unique_numbered_cov_slices;
    std::map<const source::MaterialProgram*, std::uint32_t> prog_to_arch;
    std::map<const source::MaterialProgram*, std::uint32_t> prog_to_masked_bin;
    std::map<const source::MaterialProgram*, std::uint32_t> prog_to_masked_cov;

    for (const auto& [prog_ptr, use] : unique_progs) {
        std::shared_ptr<ProgramArchetype> arch;
        bool insert_into_cache = false;
        if (auto it = archetype_cache.find(prog_ptr); it != archetype_cache.end())
            arch = it->second.lock();
        if (!arch || arch->base_failure) {
            arch = std::make_shared<ProgramArchetype>();
            arch->definition = use.definition;
            insert_into_cache = true;
        } else if (use.masked && arch->masked_failure) {
            arch->masked_failure.reset();
            arch->masked_submitted = false;
            arch->masked_job = {};
            arch->masked_vsm_job = {};
        }
        ATELIER_TRY(arch->submit(*device, use.masked));
        if (insert_into_cache)
            archetype_cache[prog_ptr] = arch;
        const auto arch_idx = static_cast<std::uint32_t>(archetypes.size());
        prog_to_arch.emplace(prog_ptr, arch_idx);
        if (use.masked) {
            const auto raster_bin_idx = static_cast<std::uint32_t>(masked_arch_indices.size());
            masked_arch_indices.push_back(arch_idx);
            auto canon = arch->definition->program().hlsl_coverage("RayCovCanon");
            if (!canon)
                return std::unexpected(make_error(rhi::ErrorCode::invalid_argument, std::move(canon.error().message)));
            const std::string canon_str(canon->view().std_view());
            std::uint32_t cov_idx = 0u;
            const auto found_cov = std::ranges::find(canonical_cov_slices, canon_str);
            if (found_cov != canonical_cov_slices.end()) {
                cov_idx = static_cast<std::uint32_t>(found_cov - canonical_cov_slices.begin());
            } else {
                cov_idx = static_cast<std::uint32_t>(canonical_cov_slices.size());
                const String ray_ns = String::concat("RayCov", cov_idx);
                auto numbered = arch->definition->program().hlsl_coverage(ray_ns.view());
                if (!numbered)
                    return std::unexpected(
                        make_error(rhi::ErrorCode::invalid_argument, std::move(numbered.error().message)));
                canonical_cov_slices.push_back(canon_str);
                unique_numbered_cov_slices.push_back(std::string(numbered->view().std_view()));
            }
            prog_to_masked_bin.emplace(prog_ptr, raster_bin_idx);
            prog_to_masked_cov.emplace(prog_ptr, cov_idx);
        }
        archetypes.push_back(std::move(arch));
        masked_flags.push_back(use.masked);
    }

    // Resolve or compile the independent MaskedCoverageSet (0 recompiles when only opaque archetypes change!).
    std::shared_ptr<MaskedCoverageSet> masked_cov_set;
    if (!canonical_cov_slices.empty()) {
        for (auto it = masked_coverage_cache.begin(); it != masked_coverage_cache.end();) {
            auto existing = it->lock();
            if (!existing) {
                it = masked_coverage_cache.erase(it);
                continue;
            }
            if (!existing->failure && existing->canonical_coverage_slices == canonical_cov_slices)
                masked_cov_set = existing;
            ++it;
        }
        if (!masked_cov_set) {
            masked_cov_set = std::make_shared<MaskedCoverageSet>();
            masked_cov_set->canonical_coverage_slices = std::move(canonical_cov_slices);

            std::string ray_cov = make_material_bridge_hlsl() + "\n#define RAY_HAS_MASKED 1\n";
            for (const auto& slice_src : unique_numbered_cov_slices)
                ray_cov += slice_src + "\n";

            ray_cov +=
                "float eval_masked_ray_coverage(uint masked_bin, uint input_base, uint param_base, float2 uv, float3 pos_w, float3 norm_w, float3 geom_w, float4 tan_w) {\n"
                "    ByteAddressBuffer material_parameters = ResourceDescriptorHeap[atelier_safe_desc(g_material_params_srv)];\n"
                "    switch (masked_bin) {\n";
            for (std::size_t m = 0; m < unique_numbered_cov_slices.size(); ++m) {
                const std::string ns = "RayCov" + std::to_string(m);
                ray_cov += "    case " + std::to_string(m) + ": {\n        " + ns +
                           "::ms_Context c;\n"
                           "        c.ms_uv = uv; c.ms_position_world = pos_w; c.ms_normal_world = norm_w;\n"
                           "        c.ms_geometric_normal_world = geom_w; c.ms_tangent_world = tan_w;\n"
                           "        return " +
                           ns + "::ms_coverage(input_base, c, " + ns +
                           "::ms_load_parameters(material_parameters, param_base));\n    }\n";
            }
            ray_cov += "    }\n    return 1.0;\n}\n";
            ray_cov += shaders::shadow_ray_query_cs;

            const std::array<rhi::ShaderStageSource, 1> stages{
                {{nri::StageBits::COMPUTE_SHADER, ray_cov.c_str(), "main", "shadow_ray_masked_cs"}}};
            rhi::PipelineCompileDesc desc{};
            desc.kind = rhi::PipelineKind::compute;
            desc.debug_name = "shadow_ray_masked_cs";
            desc.stages = stages;
            auto job = rhi::BackendAccess::submit_pipeline(*device, desc);
            if (!job)
                return std::unexpected(Error{job.error(), false});
            masked_cov_set->job = std::move(*job);
            masked_coverage_cache.push_back(masked_cov_set);
        }
    }

    // Reuse an identical MaterialProgramSet if still alive.
    for (auto it = set_cache.begin(); it != set_cache.end();) {
        auto existing = it->lock();
        if (!existing) {
            it = set_cache.erase(it);
            continue;
        }
        if (!existing->failure && existing->masked_flags == masked_flags && existing->archetypes == archetypes &&
            existing->masked_coverage == masked_cov_set) {
            program_set = existing;
        }
        ++it;
    }

    if (!program_set) {
        program_set = std::make_shared<MaterialProgramSet>();
        program_set->archetypes = std::move(archetypes);
        program_set->masked_flags = std::move(masked_flags);
        program_set->masked_archetype_indices = std::move(masked_arch_indices);
        program_set->masked_coverage = std::move(masked_cov_set);
        set_cache.push_back(program_set);
    }

    std::map<std::pair<const material::Image*, assets::AtxRole>, std::uint32_t> img_map;
    for (const auto& surface : surfaces) {
        const auto& policy = surface->policy();
        if (parameters.size() + surface->parameters().size() + 3u > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Material parameter buffer exceeds byte addressing"));
        }
        if (inputs.size() + surface->inputs().size() >
            std::numeric_limits<std::uint32_t>::max() / sizeof(GpuTextureInputRecord)) {
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Material input table exceeds addressing"));
        }

        GpuSurfaceRecord rec{};
        rec.archetype_index = prog_to_arch.at(&surface->program());
        rec.parameter_base = static_cast<std::uint32_t>(parameters.size());
        rec.input_base = static_cast<std::uint32_t>(inputs.size());
        rec.flags = (policy.coverage == material::Coverage::mask ? 1u : 0u) | (policy.double_sided ? 2u : 0u) |
                    (policy.emission_two_sided ? 4u : 0u);
        if (policy.coverage == material::Coverage::mask) {
            rec.masked_bin_index = prog_to_masked_bin.at(&surface->program());
            rec.masked_cov_index = prog_to_masked_cov.at(&surface->program());

            const bool no_bound_images =
                std::ranges::none_of(surface->inputs(), [](const auto& b) { return static_cast<bool>(b.image); });
            if (no_bound_images && masked_cov_set &&
                rec.masked_cov_index < masked_cov_set->canonical_coverage_slices.size()) {
                const std::string& cov_src = masked_cov_set->canonical_coverage_slices[rec.masked_cov_index];
                if (cov_src.find("ms_base_color") != std::string::npos &&
                    cov_src.find("ms_alpha_cutoff") != std::string::npos &&
                    cov_src.find("ms_position_world") == std::string::npos &&
                    cov_src.find("ms_normal_world") == std::string::npos) {
                    const auto param_bytes = surface->parameters();
                    std::optional<float> base_alpha;
                    std::optional<float> alpha_cutoff;
                    for (const auto& mp : surface->program().parameters()) {
                        if (mp.key == "base_color" && mp.components == 4u && mp.offset + 16u <= param_bytes.size()) {
                            float w = 1.0f;
                            std::memcpy(&w, param_bytes.data() + mp.offset + 12u, sizeof(float));
                            base_alpha = w;
                        } else if (mp.key == "alpha_cutoff" && mp.components == 1u &&
                                   mp.offset + 4u <= param_bytes.size()) {
                            float c = 0.5f;
                            std::memcpy(&c, param_bytes.data() + mp.offset, sizeof(float));
                            alpha_cutoff = c;
                        }
                    }
                    if (base_alpha && alpha_cutoff && *base_alpha <= 0.0f && *alpha_cutoff > 0.0f)
                        rec.flags |= 8u;
                }
            }
        }
        records.push_back(rec);

        parameters.insert(parameters.end(), surface->parameters().begin(), surface->parameters().end());
        parameters.resize((parameters.size() + 3u) & ~std::size_t{3u});

        const auto prog_tex = surface->program().textures();
        if (prog_tex.size() != surface->inputs().size()) {
            return std::unexpected(
                make_error(rhi::ErrorCode::invalid_argument, "Material surface texture count mismatch"));
        }
        for (std::size_t i = 0; i < surface->inputs().size(); ++i) {
            if (prog_tex[i].slot != i)
                atelier::fatal("Material program texture slots must be contiguous [0..count)");
            const auto& binding = surface->inputs()[i];
            const assets::AtxRole role =
                classify_texture_role(prog_tex[i].is_normal_map, binding.color, binding.image.get());
            GpuTextureInputRecord in_rec{};
            in_rec.slot = prog_tex[i].slot;
            in_rec.texture_srv = 0xFFFFFFFFu;
            in_rec.address_mode = static_cast<std::uint32_t>(binding.address);
            in_rec.input_count_and_role = static_cast<std::uint32_t>(surface->inputs().size()) & 0xFFFFu;
            in_rec.empty_value = {binding.empty_value.x, binding.empty_value.y, binding.empty_value.z,
                                  binding.empty_value.w};
            if (binding.image) {
                const auto [at, added] =
                    img_map.try_emplace({binding.image.get(), role}, static_cast<std::uint32_t>(image_requests.size()));
                if (added)
                    image_requests.push_back({binding.image, role});
                in_rec.texture_srv = at->second;
            }
            inputs.push_back(in_rec);
        }
    }
    if (parameters.empty())
        parameters.resize(4u);
    if (inputs.empty())
        inputs.emplace_back();
    return {};
}

Result<void> MaterialBatchState::stage_upload(std::span<const ResolvedMaterialImage> resolved_images,
                                              UploadBatch& batch) {
    for (auto& in_rec : inputs) {
        if (in_rec.texture_srv != 0xFFFFFFFFu) {
            const auto& res = resolved_images[in_rec.texture_srv];
            in_rec.texture_srv = res.srv;
            const auto q_tok =
                static_cast<std::uint32_t>(std::clamp(std::lrint(res.toksvig_slope * 255.0f), 0l, 255l)) & 0xFFu;
            in_rec.input_count_and_role |= res.role_flags | (q_tok << 24u);
        }
    }
    ATELIER_TRY_ASSIGN(surface_buffer,
                       batch.stage_buffer(records.data(), records.size() * sizeof(GpuSurfaceRecord),
                                          sizeof(GpuSurfaceRecord), false, nri::BufferUsageBits::NONE, "mat_surfaces"));
    ATELIER_TRY_ASSIGN(parameter_buffer, batch.stage_buffer(parameters.data(), parameters.size(), 0u, true,
                                                            nri::BufferUsageBits::NONE, "mat_parameters"));
    ATELIER_TRY_ASSIGN(input_buffer, batch.stage_buffer(inputs.data(), inputs.size() * sizeof(GpuTextureInputRecord),
                                                        sizeof(GpuTextureInputRecord), false,
                                                        nri::BufferUsageBits::NONE, "mat_inputs"));
    return {};
}

} // namespace atelier::renderer::detail

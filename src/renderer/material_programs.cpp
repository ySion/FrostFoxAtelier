#include "material_programs.hpp"
#include "shader_source.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>

namespace atelier::renderer::detail {
namespace {
void require(GfxResult result, const char* message, bool terminal = false) {
    if (result)
        throw Error{{rhi::ErrorCode::execution_failed, result, message}, terminal};
}
void invalid(bool valid, const char* message) {
    if (!valid)
        throw Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, message}, false};
}
}

rhi::PipelineDesc studio_pipeline(const char* name, const char* vs, const char* ps, PipelineStyle style) {
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.name = name;
    desc.source.vs = vs;
    desc.source.ps = ps;
    require(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), "Set window color target");
    if (style == PipelineStyle::cavity_surface)
        require(gfxDrawStateSetColorTarget(desc.draw_state, 1, DXGI_FORMAT_R16G16B16A16_FLOAT),
                "Set cavity normal target");
    require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set culling");
    const bool surface = style == PipelineStyle::surface || style == PipelineStyle::cavity_surface;
    const bool with_depth = surface || style == PipelineStyle::depth || style == PipelineStyle::visible_lines ||
                            style == PipelineStyle::all_lines || style == PipelineStyle::grid_depth;
    const bool write_depth = surface || style == PipelineStyle::depth;
    if (with_depth) {
        require(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT), "Set depth target");
        require(gfxDrawStateSetDepthFunction(
                    desc.draw_state, style == PipelineStyle::all_lines       ? D3D12_COMPARISON_FUNC_ALWAYS
                                     : style == PipelineStyle::visible_lines ? D3D12_COMPARISON_FUNC_GREATER_EQUAL
                                                                             : D3D12_COMPARISON_FUNC_GREATER),
                "Set depth comparison");
    }
    require(gfxDrawStateSetDepthWriteMask(desc.draw_state,
                                          write_depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO),
            "Set depth writes");
    if (!surface && style != PipelineStyle::background)
        require(gfxDrawStateSetBlendMode(
                    desc.draw_state, style == PipelineStyle::depth ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
                    style == PipelineStyle::depth ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
                    style == PipelineStyle::depth ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
                    style == PipelineStyle::depth ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD),
                "Set pass blending");
    return desc;
}
rhi::PipelineDesc gbuffer_pipeline(const char* vs, const char* ps) {
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.source.vs = vs;
    desc.source.ps = ps;
    desc.name = "Scene Lit GBuffer";
    for (std::uint32_t i = 0; i < 4; ++i)
        require(gfxDrawStateSetColorTarget(desc.draw_state, i, DXGI_FORMAT_R16G16B16A16_FLOAT), "Set GBuffer format");
    require(gfxDrawStateSetColorTarget(desc.draw_state, 4, DXGI_FORMAT_R32_FLOAT), "Set previous surface depth format");
    require(gfxDrawStateSetColorTarget(desc.draw_state, 5, DXGI_FORMAT_R16G16_FLOAT), "Set geometric normal format");
    require(gfxDrawStateSetColorTarget(desc.draw_state, 6, DXGI_FORMAT_R32G32_UINT),
            "Set stable surface identity format");
    require(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT), "Set GBuffer depth");
    require(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_GREATER), "Set GBuffer reversed depth");
    require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set GBuffer culling");
    return desc;
}
rhi::PipelineDesc shadow_pipeline(const char* vs, const char* ps) {
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.source.vs = vs;
    desc.source.ps = ps;
    desc.name = "Cached shadow depth";
    require(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT), "Set cached shadow precision");
    require(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_GREATER), "Set shadow reversed depth");
    require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set two-sided shadow coverage");
    return desc;
}

MaterialBatchState::MaterialBatchState(rhi::Context& c, std::size_t& count)
    : context(c), retirement_context(c.borrow()), live_batches(count) {
    ++live_batches;
}
MaterialImageResource::~MaterialImageResource() {
    if (texture && gfxDestroyTexture(retirement_context, texture)) {
        std::fputs("Material image retirement failed\n", stderr);
        std::abort();
    }
}
MaterialBatchState::~MaterialBatchState() {
    if (owner != std::this_thread::get_id())
        std::abort();
    for (auto buffer : {surface_buffer, parameter_buffer, input_buffer})
        if (buffer)
            if (gfxDestroyBuffer(retirement_context, buffer)) {
                std::fputs("Material batch buffer retirement failed\n", stderr);
                std::abort();
            }
    --live_batches;
}
void MaterialBatchState::submit(std::span<const material::ProgramSurfaceRef> input,
                                std::vector<std::weak_ptr<MaterialProgramSet>>& sets) {
    invalid(!input.empty() && input.size() <= std::numeric_limits<std::uint32_t>::max() / sizeof(ProgramSurfaceRecord),
            "Material batch needs a representable nonempty Surface table");
    surfaces.assign(input.begin(), input.end());
    struct ProgramUse {
        material::DefinitionRef definition;
        bool masked{};
    };
    std::map<const source::MaterialProgram*, ProgramUse> unique;
    for (const auto& surface : surfaces) {
        invalid(bool(surface), "Material batch has a missing Surface");
        auto& use = unique.try_emplace(&surface->program(), ProgramUse{surface->definition()}).first->second;
        // Different definitions can share a program. Never let the first
        // (possibly Opaque) definition hide another instance's Mask policy.
        use.masked |= surface->policy().coverage == material::Coverage::mask;
    }
    std::vector<material::DefinitionRef> definitions;
    std::vector<bool> masked_programs;
    std::map<const source::MaterialProgram*, std::uint32_t> indices;
    for (const auto& [program, use] : unique) {
        indices.emplace(program, static_cast<std::uint32_t>(definitions.size()));
        definitions.push_back(use.definition);
        masked_programs.push_back(use.masked);
    }
    for (auto it = sets.begin(); it != sets.end();) {
        auto existing = it->lock();
        if (!existing) {
            it = sets.erase(it);
            continue;
        }
        if (!existing->failure && existing->masked_programs == masked_programs &&
            std::ranges::equal(existing->definitions, definitions,
                               [](const auto& a, const auto& b) { return &a->program() == &b->program(); }))
            code = std::move(existing);
        ++it;
    }
    std::map<std::pair<const material::Image*, bool>, std::uint32_t> image_indices;
    for (const auto& surface : surfaces) {
        const auto& policy = surface->policy();
        invalid(parameters.size() + surface->parameters().size() + 3 <= std::numeric_limits<std::uint32_t>::max(),
                "Material parameter buffer exceeds byte addressing");
        invalid(inputs.size() + surface->inputs().size() <=
                    std::numeric_limits<std::uint32_t>::max() / sizeof(ProgramInputRecord),
                "Material input table exceeds addressing");
        records.push_back({indices.at(&surface->program()),
                           static_cast<std::uint32_t>(parameters.size()),
                           static_cast<std::uint32_t>(inputs.size()),
                           {policy.coverage == material::Coverage::mask ? 1u : 0u, policy.double_sided ? 1u : 0u,
                            policy.emission_two_sided ? 1u : 0u}});
        parameters.insert(parameters.end(), surface->parameters().begin(), surface->parameters().end());
        parameters.resize((parameters.size() + 3) & ~std::size_t(3));
        for (std::size_t i = 0; i < surface->inputs().size(); ++i) {
            const auto& binding = surface->inputs()[i];
            ProgramInputRecord record{surface->program().textures()[i].slot, ~0u,
                                      static_cast<std::uint32_t>(binding.address),
                                      static_cast<std::uint32_t>(surface->inputs().size()), binding.empty_value};
            if (binding.image) {
                const bool srgb = binding.color == material::TextureColor::srgb;
                const auto [at, added] =
                    image_indices.try_emplace({binding.image.get(), srgb}, static_cast<std::uint32_t>(images.size()));
                if (added)
                    images.emplace_back(binding.image, srgb);
                record.texture = at->second;
            }
            inputs.push_back(record);
        }
    }
    if (parameters.empty())
        parameters.resize(4);
    if (inputs.empty())
        inputs.emplace_back();
    if (!code) {
        code = std::make_shared<MaterialProgramSet>();
        code->definitions = std::move(definitions);
        code->masked_programs = std::move(masked_programs);
        code->submit(context);
        sets.push_back(code);
    }
}
void MaterialProgramSet::submit(rhi::Context& context) {
    std::string prefix = R"hlsl(#define MATERIAL_PROGRAMS 1
struct MaterialInput { uint slot; uint texture_index; uint address; uint count; float4 empty_value; };
StructuredBuffer<MaterialInput> material_inputs;
Texture2D<float4> material_textures[] : register(t0, space1);
SamplerState material_repeat_sampler;
SamplerState material_clamp_sampler;
SamplerState material_mirror_sampler;
MaterialInput material_input(uint environment,uint local_slot) {
    uint count=material_inputs[environment].count;
    [loop] for(uint i=0;i<count;++i) {
        MaterialInput binding=material_inputs[environment+i];
        if(binding.slot==local_slot)return binding;
    }
    return material_inputs[environment]; // Complete bindings are validated by ProgramSurface.
}
float4 material_input_level(MaterialInput binding,float2 uv,float lod) {
    if(binding.texture_index==0xffffffffu)return binding.empty_value;
    uint index=NonUniformResourceIndex(binding.texture_index);
    if(binding.address==1)return material_textures[index].SampleLevel(material_clamp_sampler,uv,lod);
    if(binding.address==2)return material_textures[index].SampleLevel(material_mirror_sampler,uv,lod);
    return material_textures[index].SampleLevel(material_repeat_sampler,uv,lod);
}
float4 atelier_material_sample(uint environment,uint local_slot,float2 uv) {
    return material_input_level(material_input(environment,local_slot),uv,0);
}
float2 atelier_material_sample_orientation(uint environment,uint local_slot,float2 uv) {
    MaterialInput binding=material_input(environment,local_slot);
    if(binding.address!=2)return float2(1,1);
    // The real input owns addressing. At either exact mirror fold the selected
    // orientation is positive; the footprint lowering treats this as constant.
    precise float2 tile=floor(uv*.5);
    precise float2 folded=uv-tile*2;
    return float2(folded.x>1?-1:1,folded.y>1?-1:1);
}
bool material_finite2(float2 value) { return all((asuint(value)&0x7fffffffu)<0x7f800000u); }
float4 atelier_material_sample_grad(uint environment,uint local_slot,float2 uv,float2 d0,float2 d1) {
    MaterialInput binding=material_input(environment,local_slot);
    if(binding.texture_index==0xffffffffu)return binding.empty_value;
    uint index=NonUniformResourceIndex(binding.texture_index),width,height,levels;
    material_textures[index].GetDimensions(0,width,height,levels);
    // Integer classification survives DXC's ordinary finite-math assumptions.
    // Never pass unavailable or texel-scale-overflowed gradients to hardware.
    precise float2 t0=d0*float2(width,height),t1=d1*float2(width,height);
    if(!material_finite2(d0)||!material_finite2(d1)||!material_finite2(t0)||!material_finite2(t1))
        return material_input_level(binding,uv,float(levels-1));
    if(binding.address==1)return material_textures[index].SampleGrad(material_clamp_sampler,uv,d0,d1);
    if(binding.address==2)return material_textures[index].SampleGrad(material_mirror_sampler,uv,d0,d1);
    return material_textures[index].SampleGrad(material_repeat_sampler,uv,d0,d1);
}
)hlsl";
    prefix += std::ranges::any_of(masked_programs, [](bool masked) { return masked; })
                  ? "#define MATERIAL_HAS_MASKED 1\n"
                  : "#define MATERIAL_HAS_MASKED 0\n";
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        auto hlsl = definitions[i]->program().hlsl("MaterialProgram" + std::to_string(i));
        if (!hlsl)
            throw Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, hlsl.error().message}, false};
        prefix += *hlsl;
        auto footprint = definitions[i]->program().hlsl_footprint("MaterialFootprint" + std::to_string(i));
        if (!footprint)
            throw Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, footprint.error().message},
                        false};
        prefix += *footprint;
        if (masked_programs[i]) {
            auto coverage = definitions[i]->program().hlsl_coverage("MaterialCoverage" + std::to_string(i));
            if (!coverage)
                throw Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, coverage.error().message},
                            false};
            prefix += *coverage;
        }
    }
    // Reuse the actual generated engine value declarations, not parameter or
    // Context packing assumptions. Each MS still has its own Params layout.
    prefix += R"hlsl(
typedef MaterialProgram0::ms_Context MaterialContext;
typedef MaterialProgram0::ms_Output MaterialOutput;
struct MaterialSurface { uint program; uint parameter_base; uint input_base; uint3 flags; };
StructuredBuffer<MaterialSurface> material_surfaces;
StructuredBuffer<uint> material_indices;
StructuredBuffer<uint> material_offsets;
ByteAddressBuffer material_parameters;
)hlsl";
    prefix += "float material_coverage(MaterialSurface surface,MaterialContext input) {\n"
              "if(surface.flags.x==0)return 1;\nswitch(surface.program) {\n";
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (!masked_programs[i])
            continue;
        const auto ns = "MaterialCoverage" + std::to_string(i);
        prefix += "case " + std::to_string(i) + ": {\n" + ns + "::ms_Context c;\n";
        for (const auto* field : {"uv", "position_world", "normal_world", "geometric_normal_world", "tangent_world"})
            prefix += "c.ms_" + std::string(field) + "=input.ms_" + field + ";\n";
        prefix += "return " + ns + "::ms_coverage(surface.input_base,c," + ns +
                  "::ms_load_parameters(material_parameters,surface.parameter_base)); }\n";
    }
    prefix += "}\nreturn 1;\n}\n";
    for (bool footprint : {false, true}) {
        prefix +=
            footprint
                ? "MaterialOutput material_shade_footprint(MaterialSurface surface,MaterialContext input,MaterialContext dx,MaterialContext dy) {\n"
                : "MaterialOutput material_shade(MaterialSurface surface,MaterialContext input) {\n";
        prefix += "switch(surface.program) {\n";
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            const auto ns = std::string(footprint ? "MaterialFootprint" : "MaterialProgram") + std::to_string(i);
            prefix += "case " + std::to_string(i) + ": {\n";
            for (const auto* name : {"input", "dx", "dy"}) {
                if (!footprint && std::string_view(name) != "input")
                    break;
                prefix += ns + "::ms_Context c_" + name + ";\n";
                for (const auto* field :
                     {"uv", "position_world", "normal_world", "geometric_normal_world", "tangent_world"})
                    prefix += "c_" + std::string(name) + ".ms_" + field + "=" + name + ".ms_" + field + ";\n";
            }
            prefix += ns + "::ms_Output o=" + ns +
                      (footprint ? "::ms_shade_footprint(surface.input_base,c_input,c_dx,c_dy,"
                                 : "::ms_shade(surface.input_base,c_input,") +
                      ns +
                      "::ms_load_parameters(material_parameters,surface.parameter_base));\nMaterialOutput result;\n";
            for (const auto* field : {"base_color", "emissive", "roughness", "metallic", "coverage", "normal_world"})
                prefix += "result.ms_" + std::string(field) + "=o.ms_" + field + ";\n";
            prefix += "return result; }\n";
        }
        prefix += "}\nreturn (MaterialOutput)0;\n}\n";
    }
    const auto submit = [&](MaterialPass pass, const rhi::PipelineDesc& desc) {
        auto job = context.submit(desc);
        if (!job)
            throw Error{job.error(), false};
        pending.push_back({pass, std::move(*job)});
    };
    const auto compute = [&](MaterialPass pass, const char* source, const char* entry) {
        // Only probe transport/inspection approximates Mask as solid geometry.
        // Camera direct lighting and receiver gathers retain LOD0 candidates.
        const bool gi_internal = pass == MaterialPass::probe_trace || pass == MaterialPass::probe_update ||
                                 pass == MaterialPass::inspect || pass == MaterialPass::inspect_update;
        const std::string text =
            prefix + (gi_internal ? "#define MATERIAL_DDGI_INTERNAL_FORCE_OPAQUE 1\n" : "") + source;
        rhi::PipelineDesc desc;
        desc.source.cs = text.c_str();
        desc.entry_point = entry;
        desc.name = entry;
        submit(pass, desc);
    };
    // Submit the costly ray consumers first, like the existing Scene Lit queue.
    compute(MaterialPass::probe_update, shaders::lit_ddgi, "DDGIUpdate");
    compute(MaterialPass::compose, shaders::lit_clustered, "lit_compose_cs");
    compute(MaterialPass::sun_trace, shaders::lit_clustered, "lit_sun_trace_cs");
    compute(MaterialPass::probe_trace, shaders::lit_ddgi, "DDGITrace");
    compute(MaterialPass::probe_resolve, shaders::lit_ddgi, "DDGIResolve");
    compute(MaterialPass::probe_upsample, shaders::lit_ddgi, "DDGIUpsample");
    compute(MaterialPass::probe_exact, shaders::lit_ddgi, "DDGIExact");
    compute(MaterialPass::inspect, shaders::lit_ddgi, "DDGIInspect");
    compute(MaterialPass::inspect_update, shaders::lit_ddgi, "DDGIUpdateInspect");
    const auto studio_vs = prefix + shaders::scene_vs;
    const auto studio_ps = prefix + shaders::scene_ps;
    const auto cavity_ps = prefix + shaders::cavity_surface_ps;
    for (const auto& [pass, style] : {std::pair{MaterialPass::studio, PipelineStyle::surface},
                                      {MaterialPass::cavity, PipelineStyle::cavity_surface},
                                      {MaterialPass::depth, PipelineStyle::depth},
                                      {MaterialPass::visible_lines, PipelineStyle::visible_lines},
                                      {MaterialPass::all_lines, PipelineStyle::all_lines}})
        submit(pass, studio_pipeline("Programmable studio", studio_vs.c_str(),
                                     pass == MaterialPass::cavity ? cavity_ps.c_str() : studio_ps.c_str(), style));
    const auto gbuffer = prefix + shaders::lit_gbuffer;
    const auto vs = gbuffer + "\nLitRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_gbuffer_vs(v,i);}";
    const auto ps =
        gbuffer +
        "\nLitGBuffer main(LitRaster i,uint p:SV_PrimitiveID,float3 b:SV_Barycentrics){return lit_gbuffer_ps(i,p,b);}";
    submit(MaterialPass::gbuffer, gbuffer_pipeline(vs.c_str(), ps.c_str()));
    const auto shadow = prefix + shaders::lit_shadow;
    const auto shadow_vs =
        shadow + "\nLitShadowRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_shadow_vs(v,i);}";
    const auto shadow_ps = shadow + "\nvoid main(LitShadowRaster i,uint p:SV_PrimitiveID){lit_shadow_ps(i,p);}";
    submit(MaterialPass::shadow, shadow_pipeline(shadow_vs.c_str(), shadow_ps.c_str()));
    const auto coverage = prefix + shaders::lit_ddgi_debug +
                          "\nfloat4 main(float4 p:SV_Position):SV_Target{return ddgi_debug_coverage_ps(p);}";
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.source.vs = shaders::viewport_vs;
    desc.source.ps = coverage.c_str();
    desc.name = "Programmable DDGI receiver coverage";
    require(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
            "Set DDGI debug color format");
    require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set DDGI debug culling");
    require(gfxDrawStateSetDepthWriteMask(desc.draw_state, D3D12_DEPTH_WRITE_MASK_ZERO),
            "Disable diagnostic surface depth writes");
    submit(MaterialPass::probe_coverage, desc);
}
Result<Preparation> MaterialProgramSet::preparation(rhi::Context& context) {
    if (failure)
        return std::unexpected(*failure);
    constexpr auto total = static_cast<std::uint32_t>(MaterialPass::count);
    for (auto it = pending.begin(); it != pending.end();) {
        auto result = context.take(it->job);
        if (!result) {
            ++it;
            continue;
        }
        if (!*result) {
            failure = Error{result->error(), false};
            pending.clear();
            return std::unexpected(*failure);
        }
        pipelines[static_cast<std::size_t>(it->pass)] = std::move(**result);
        it = pending.erase(it);
    }
    return Preparation{total - static_cast<std::uint32_t>(pending.size()), total, pending.empty()};
}
void MaterialBatchState::upload() {
    surface_buffer = gfxCreateBuffer<ProgramSurfaceRecord>(context.borrow(), static_cast<std::uint32_t>(records.size()),
                                                           records.data());
    parameter_buffer = gfxCreateBuffer(context.borrow(), parameters.size(), parameters.data());
    input_buffer =
        gfxCreateBuffer<ProgramInputRecord>(context.borrow(), static_cast<std::uint32_t>(inputs.size()), inputs.data());
    if (!surface_buffer || !parameter_buffer || !input_buffer)
        throw Error{{rhi::ErrorCode::creation_failed, kGfxResult_OutOfMemory, "Material batch table upload failed"},
                    true};
    ready = true;
}
} // namespace atelier::renderer::detail

namespace atelier::renderer {
MaterialBatch::MaterialBatch() = default;
MaterialBatch::~MaterialBatch() = default;
MaterialBatch::MaterialBatch(MaterialBatch&&) noexcept = default;
MaterialBatch& MaterialBatch::operator=(MaterialBatch&&) noexcept = default;
std::span<const material::ProgramSurfaceRef> MaterialBatch::surfaces() const noexcept {
    return state_ ? std::span<const material::ProgramSurfaceRef>(state_->surfaces)
                  : std::span<const material::ProgramSurfaceRef>{};
}
} // namespace atelier::renderer

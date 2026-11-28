#include "detail/material_programs.hpp"
#include "detail/result.hpp"
#include "shader_source.hpp"
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <limits>

namespace atelier::renderer::detail {
namespace {
Result<void> invalid(bool valid, Utf8Text message) {
    if (!valid)
        return std::unexpected(
            Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, std::move(message).into_string()},
                  false});
    return {};
}
}

rhi::PipelineDesc studio_pipeline(const char* name, const char* vs, const char* ps, PipelineStyle style) {
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.name = name;
    desc.source.vs = vs;
    desc.source.ps = ps;
    // These setters only copy CPU state: the owned draw state and constant target
    // indices satisfy their preconditions. GPU validation occurs at pipeline creation.
    static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
    if (style == PipelineStyle::cavity_surface)
        static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, 1, DXGI_FORMAT_R16G16B16A16_FLOAT));
    static_cast<void>(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    const bool surface = style == PipelineStyle::surface || style == PipelineStyle::cavity_surface;
    const bool with_depth = surface || style == PipelineStyle::depth || style == PipelineStyle::visible_lines ||
                            style == PipelineStyle::all_lines || style == PipelineStyle::grid_depth;
    const bool write_depth = surface || style == PipelineStyle::depth;
    if (with_depth) {
        static_cast<void>(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT));
        static_cast<void>(gfxDrawStateSetDepthFunction(
            desc.draw_state, style == PipelineStyle::all_lines       ? D3D12_COMPARISON_FUNC_ALWAYS
                             : style == PipelineStyle::visible_lines ? D3D12_COMPARISON_FUNC_GREATER_EQUAL
                                                                     : D3D12_COMPARISON_FUNC_GREATER));
    }
    static_cast<void>(gfxDrawStateSetDepthWriteMask(desc.draw_state, write_depth ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                                 : D3D12_DEPTH_WRITE_MASK_ZERO));
    if (!surface && style != PipelineStyle::background)
        static_cast<void>(gfxDrawStateSetBlendMode(
            desc.draw_state, style == PipelineStyle::depth ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
            style == PipelineStyle::depth ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
            style == PipelineStyle::depth ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
            style == PipelineStyle::depth ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD));
    return desc;
}

rhi::PipelineDesc gbuffer_pipeline(const char* vs, const char* ps) {
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.source.vs = vs;
    desc.source.ps = ps;
    desc.name = "Scene Lit GBuffer";
    for (std::uint32_t i = 0; i < 4; ++i)
        static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, i, DXGI_FORMAT_R16G16B16A16_FLOAT));
    static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, 4, DXGI_FORMAT_R32_FLOAT));
    static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, 5, DXGI_FORMAT_R16G16_FLOAT));
    static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, 6, DXGI_FORMAT_R32G32_UINT));
    static_cast<void>(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT));
    static_cast<void>(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_GREATER));
    static_cast<void>(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    return desc;
}

rhi::PipelineDesc shadow_pipeline(const char* vs, const char* ps) {
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.source.vs = vs;
    desc.source.ps = ps;
    desc.name = "Cached shadow depth";
    static_cast<void>(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT));
    static_cast<void>(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_GREATER));
    static_cast<void>(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    return desc;
}

MaterialBatchState::MaterialBatchState(rhi::Context& c, std::size_t& count)
    : context(c), retirement_context(c.borrow()), live_batches(count) {
    ++live_batches;
}

MaterialImageResource::~MaterialImageResource() {
    if (texture && gfxDestroyTexture(retirement_context, texture))
        fatal("Material image retirement failed");
}

MaterialBatchState::~MaterialBatchState() {
    if (owner != std::this_thread::get_id())
        fatal("Material batch destruction on a foreign thread");
    for (auto buffer : {surface_buffer, parameter_buffer, input_buffer})
        if (buffer)
            if (gfxDestroyBuffer(retirement_context, buffer))
                fatal("Material batch buffer retirement failed");
    --live_batches;
}

Result<void> MaterialBatchState::submit(Span<const material::ProgramSurfaceRef> input,
                                        Array<std::weak_ptr<MaterialProgramSet>>& sets) {
    ATELIER_TRY(invalid(!input.empty() &&
                            input.size() <= std::numeric_limits<std::uint32_t>::max() / sizeof(ProgramSurfaceRecord),
                        "Material batch needs a representable nonempty Surface table"));
    surfaces.assign(input);
    struct ProgramUse {
        material::DefinitionRef definition;

        bool masked{};

        std::uint32_t index{};
    };
    Map<const source::MaterialProgram*, ProgramUse> unique;
    Array<const source::MaterialProgram*> ordered_programs;
    for (const auto& surface : surfaces) {
        ATELIER_TRY(invalid(bool(surface), "Material batch has a missing Surface"));
        const auto* program = &surface->program();
        auto [use, inserted] = unique.try_emplace(program, ProgramUse{surface->definition()});
        if (inserted)
            ordered_programs.add(program);
        // Different definitions can share a program. Never let the first
        // (possibly Opaque) definition hide another instance's Mask policy.
        use->masked |= surface->policy().coverage == material::Coverage::mask;
    }
    std::ranges::sort(ordered_programs);
    Array<material::DefinitionRef> definitions;
    BitVector masked_programs;
    definitions.reserve(ordered_programs.size());
    masked_programs.reserve(ordered_programs.size());
    for (const auto* program : ordered_programs) {
        auto& use = unique.at(program);
        use.index = static_cast<std::uint32_t>(definitions.size());
        definitions.add(use.definition);
        masked_programs.add(use.masked);
    }
    sets.remove_if([&](const std::weak_ptr<MaterialProgramSet>& weak) {
        auto existing = weak.lock();
        if (!existing)
            return true;
        if (!existing->failure && existing->masked_programs == masked_programs &&
            std::ranges::equal(existing->definitions, definitions,
                               [](const auto& a, const auto& b) { return &a->program() == &b->program(); }))
            code = std::move(existing);
        return false;
    });
    Map<std::pair<const material::Image*, bool>, std::uint32_t> image_indices;
    for (const auto& surface : surfaces) {
        const auto& policy = surface->policy();
        ATELIER_TRY(
            invalid(parameters.size() + surface->parameters().size() + 3 <= std::numeric_limits<std::uint32_t>::max(),
                    "Material parameter buffer exceeds byte addressing"));
        ATELIER_TRY(invalid(inputs.size() + surface->inputs().size() <=
                                std::numeric_limits<std::uint32_t>::max() / sizeof(ProgramInputRecord),
                            "Material input table exceeds addressing"));
        records.add({unique.at(&surface->program()).index,
                     static_cast<std::uint32_t>(parameters.size()),
                     static_cast<std::uint32_t>(inputs.size()),
                     {policy.coverage == material::Coverage::mask ? 1u : 0u, policy.double_sided ? 1u : 0u,
                      policy.emission_two_sided ? 1u : 0u}});
        parameters.append(surface->parameters());
        parameters.resize((parameters.size() + 3) & ~std::size_t(3));
        for (std::size_t i = 0; i < surface->inputs().size(); ++i) {
            const auto& binding = surface->inputs()[i];
            ProgramInputRecord record{surface->program().textures()[i].slot, ~0u,
                                      static_cast<std::uint32_t>(binding.address),
                                      static_cast<std::uint32_t>(surface->inputs().size()), binding.empty_value};
            if (binding.image) {
                const bool srgb = binding.color == material::TextureColor::srgb;
                const auto [slot, added] =
                    image_indices.try_emplace({binding.image.get(), srgb}, static_cast<std::uint32_t>(images.size()));
                if (added)
                    images.emplace(binding.image, srgb);
                record.texture = *slot;
            }
            inputs.add(record);
        }
    }
    if (parameters.empty())
        parameters.resize(4);
    if (inputs.empty())
        inputs.emplace();
    if (!code) {
        code = std::make_shared<MaterialProgramSet>();
        code->definitions = std::move(definitions);
        code->masked_programs = std::move(masked_programs);
        ATELIER_TRY(code->submit(context));
        sets.add(code);
    }
    return {};
}

Result<void> MaterialProgramSet::submit(rhi::Context& context) {
    String prefix = String::literal<R"hlsl(#define MATERIAL_PROGRAMS 1
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
)hlsl">();
    if (masked_programs.any())
        prefix.append_literal<"#define MATERIAL_HAS_MASKED 1\n">();
    else
        prefix.append_literal<"#define MATERIAL_HAS_MASKED 0\n">();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto program_ns = String::concat("MaterialProgram", i);

        auto hlsl = definitions[i]->program().hlsl(program_ns);
        if (!hlsl)
            return std::unexpected(
                Error{{rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, hlsl.error().message}, false});
        prefix += *hlsl;
        const auto footprint_ns = String::concat("MaterialFootprint", i);

        auto footprint = definitions[i]->program().hlsl_footprint(footprint_ns);
        if (!footprint)
            return std::unexpected(Error{
                {rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, footprint.error().message}, false});
        prefix += *footprint;
        if (masked_programs[i]) {
            const auto coverage_ns = String::concat("MaterialCoverage", i);

            auto coverage = definitions[i]->program().hlsl_coverage(coverage_ns);
            if (!coverage)
                return std::unexpected(Error{
                    {rhi::ErrorCode::invalid_argument, kGfxResult_InvalidParameter, coverage.error().message}, false});
            prefix += *coverage;
        }
    }
    // Reuse the actual generated engine value declarations, not parameter or
    // Context packing assumptions. Each MS still has its own Params layout.
    prefix.append_literal<R"hlsl(
typedef MaterialProgram0::ms_Context MaterialContext;
typedef MaterialProgram0::ms_Output MaterialOutput;
struct MaterialSurface { uint program; uint parameter_base; uint input_base; uint3 flags; };
StructuredBuffer<MaterialSurface> material_surfaces;
StructuredBuffer<uint> material_indices;
StructuredBuffer<uint> material_offsets;
ByteAddressBuffer material_parameters;
)hlsl">();
    prefix.append_literal<"float material_coverage(MaterialSurface surface,MaterialContext input) {\n"
                          "if(surface.flags.x==0)return 1;\nswitch(surface.program) {\n">();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (!masked_programs[i])
            continue;
        const auto ns = String::concat("MaterialCoverage", i);
        prefix.append(String::concat("case ", i, ": {\n", ns, "::ms_Context c;\n"));
        for (const StringView field :
             {"uv", "position_world", "normal_world", "geometric_normal_world", "tangent_world"})
            prefix.append(String::concat("c.ms_", String::validated(field), "=input.ms_", String::validated(field), ";\n"));
        prefix.append(String::concat("return ", ns, "::ms_coverage(surface.input_base,c,", ns,
                                     "::ms_load_parameters(material_parameters,surface.parameter_base)); }\n"));
    }
    prefix.append_literal<"}\nreturn 1;\n}\n">();
    for (bool footprint : {false, true}) {
        if (footprint)
            prefix.append_literal<"MaterialOutput material_shade_footprint(MaterialSurface surface,MaterialContext input,MaterialContext dx,MaterialContext dy) {\n">();
        else
            prefix.append_literal<"MaterialOutput material_shade(MaterialSurface surface,MaterialContext input) {\n">();
        prefix.append_literal<"switch(surface.program) {\n">();
        for (std::size_t i = 0; i < definitions.size(); ++i) {
            const auto ns = String::concat(footprint ? "MaterialFootprint"_s : "MaterialProgram"_s, i);
            prefix.append(String::concat("case ", i, ": {\n"));
            for (const StringView name : {"input", "dx", "dy"}) {
                if (!footprint && name != "input")
                    break;
                prefix.append(String::concat(ns, "::ms_Context c_", String::validated(name), ";\n"));
                for (const StringView field :
                     {"uv", "position_world", "normal_world", "geometric_normal_world", "tangent_world"})
                    prefix.append(String::concat("c_", String::validated(name), ".ms_", String::validated(field), "=",
                                                 String::validated(name), ".ms_", String::validated(field), ";\n"));
            }
            prefix.append(String::concat(
                ns, "::ms_Output o=", ns,
                footprint ? "::ms_shade_footprint(surface.input_base,c_input,c_dx,c_dy,"_s
                          : "::ms_shade(surface.input_base,c_input,"_s,
                ns, "::ms_load_parameters(material_parameters,surface.parameter_base));\nMaterialOutput result;\n"));
            for (const StringView field :
                 {"base_color", "emissive", "roughness", "metallic", "coverage", "normal_world"})
                prefix.append(String::concat("result.ms_", String::validated(field), "=o.ms_", String::validated(field), ";\n"));
            prefix.append_literal<"return result; }\n">();
        }
        prefix.append_literal<"}\nreturn (MaterialOutput)0;\n}\n">();
    }
    const auto submit = [&](MaterialPass pass, const rhi::PipelineDesc& desc) -> Result<void> {
        auto job = context.submit(desc);
        if (!job)
            return std::unexpected(Error{job.error(), false});
        pending.add({pass, std::move(*job)});
        return {};
    };
    const auto compute = [&](MaterialPass pass, const String::ConcatPiece& source, const char* entry) {
        // Only probe transport/inspection approximates Mask as solid geometry.
        // Camera direct lighting and receiver gathers retain LOD0 candidates.
        const bool gi_internal = pass == MaterialPass::probe_trace || pass == MaterialPass::probe_update ||
                                 pass == MaterialPass::inspect || pass == MaterialPass::inspect_update;
        const auto text =
            String::concat(prefix, gi_internal ? "#define MATERIAL_DDGI_INTERNAL_FORCE_OPAQUE 1\n"_s : ""_s, source);
        rhi::PipelineDesc desc;
        desc.source.cs = text.c_str();
        desc.entry_point = entry;
        desc.name = entry;
        return submit(pass, desc);
    };
    // Submit the costly ray consumers first, like the existing Scene Lit queue.
    ATELIER_TRY(compute(MaterialPass::probe_update, shaders::lit_ddgi, "DDGIUpdate"));
    ATELIER_TRY(compute(MaterialPass::compose, shaders::lit_clustered, "lit_compose_cs"));
    ATELIER_TRY(compute(MaterialPass::sun_trace, shaders::lit_clustered, "lit_sun_trace_cs"));
    ATELIER_TRY(compute(MaterialPass::probe_trace, shaders::lit_ddgi, "DDGITrace"));
    ATELIER_TRY(compute(MaterialPass::probe_resolve, shaders::lit_ddgi, "DDGIResolve"));
    ATELIER_TRY(compute(MaterialPass::probe_upsample, shaders::lit_ddgi, "DDGIUpsample"));
    ATELIER_TRY(compute(MaterialPass::probe_exact, shaders::lit_ddgi, "DDGIExact"));
    ATELIER_TRY(compute(MaterialPass::inspect, shaders::lit_ddgi, "DDGIInspect"));
    ATELIER_TRY(compute(MaterialPass::inspect_update, shaders::lit_ddgi, "DDGIUpdateInspect"));
    const auto studio_vs = String::concat(prefix, shaders::scene_vs);
    const auto studio_ps = String::concat(prefix, shaders::scene_ps);
    const auto cavity_ps = String::concat(prefix, shaders::cavity_surface_ps);
    for (const auto& [pass, style] : {std::pair{MaterialPass::studio, PipelineStyle::surface},
                                      {MaterialPass::cavity, PipelineStyle::cavity_surface},
                                      {MaterialPass::depth, PipelineStyle::depth},
                                      {MaterialPass::visible_lines, PipelineStyle::visible_lines},
                                      {MaterialPass::all_lines, PipelineStyle::all_lines}})
        ATELIER_TRY(
            submit(pass, studio_pipeline("Programmable studio", studio_vs.c_str(),
                                         pass == MaterialPass::cavity ? cavity_ps.c_str() : studio_ps.c_str(), style)));
    const auto gbuffer = String::concat(prefix, shaders::lit_gbuffer);
    const auto vs =
        String::concat(gbuffer, "\nLitRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_gbuffer_vs(v,i);}");
    const auto ps = String::concat(
        gbuffer,
        "\nLitGBuffer main(LitRaster i,uint p:SV_PrimitiveID,float3 b:SV_Barycentrics){return lit_gbuffer_ps(i,p,b);}");
    ATELIER_TRY(submit(MaterialPass::gbuffer, gbuffer_pipeline(vs.c_str(), ps.c_str())));
    const auto shadow = String::concat(prefix, shaders::lit_shadow);
    const auto shadow_vs = String::concat(
        shadow, "\nLitShadowRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_shadow_vs(v,i);}");
    const auto shadow_ps =
        String::concat(shadow, "\nvoid main(LitShadowRaster i,uint p:SV_PrimitiveID){lit_shadow_ps(i,p);}");
    ATELIER_TRY(submit(MaterialPass::shadow, shadow_pipeline(shadow_vs.c_str(), shadow_ps.c_str())));
    const auto coverage = String::concat(
        prefix, shaders::lit_ddgi_debug,
        "\nfloat4 main(float4 p:SV_Position):SV_Target{return ddgi_debug_coverage_ps(p);}");
    rhi::PipelineDesc desc;
    desc.kind = rhi::PipelineKind::vertex;
    desc.source.vs = shaders::viewport_vs;
    desc.source.ps = coverage.c_str();
    desc.name = "Programmable DDGI receiver coverage";
    static_cast<void>(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
    static_cast<void>(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE));
    static_cast<void>(gfxDrawStateSetDepthWriteMask(desc.draw_state, D3D12_DEPTH_WRITE_MASK_ZERO));
    return submit(MaterialPass::probe_coverage, desc);
}

Result<Preparation> MaterialProgramSet::preparation(rhi::Context& context) {
    if (failure)
        return std::unexpected(*failure);
    constexpr auto total = static_cast<std::uint32_t>(MaterialPass::count);
    for (std::size_t i = 0; i < pending.size();) {
        auto result = context.take(pending[i].job);
        if (!result) {
            ++i;
            continue;
        }
        if (!*result) {
            failure = Error{result->error(), false};
            pending.clear();
            return std::unexpected(*failure);
        }
        pipelines[static_cast<std::size_t>(pending[i].pass)] = std::move(**result);
        pending.remove_at(i);
    }
    return Preparation{total - static_cast<std::uint32_t>(pending.size()), total, pending.empty()};
}

Result<void> MaterialBatchState::upload() {
    surface_buffer = gfxCreateBuffer<ProgramSurfaceRecord>(context.borrow(), static_cast<std::uint32_t>(records.size()),
                                                           records.data());
    ATELIER_TRY(allocation(bool(surface_buffer), "Material batch table upload failed"));
    parameter_buffer = gfxCreateBuffer(context.borrow(), parameters.size(), parameters.data());
    ATELIER_TRY(allocation(bool(parameter_buffer), "Material batch table upload failed"));
    input_buffer =
        gfxCreateBuffer<ProgramInputRecord>(context.borrow(), static_cast<std::uint32_t>(inputs.size()), inputs.data());
    ATELIER_TRY(allocation(bool(input_buffer), "Material batch table upload failed"));
    ready = true;
    return {};
}
} // namespace atelier::renderer::detail

namespace atelier::renderer {
MaterialBatch::MaterialBatch() = default;

MaterialBatch::~MaterialBatch() = default;

MaterialBatch::MaterialBatch(MaterialBatch&&) noexcept = default;

MaterialBatch& MaterialBatch::operator=(MaterialBatch&&) noexcept = default;

Span<const material::ProgramSurfaceRef> MaterialBatch::surfaces() const noexcept {
    return state_ ? state_->surfaces.view() : Span<const material::ProgramSurfaceRef>{};
}
} // namespace atelier::renderer

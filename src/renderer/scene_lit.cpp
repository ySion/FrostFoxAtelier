#include "scene_lit.hpp"
#include "light_frame.hpp"
#include "ddgi_volume.hpp"
#include "shader_source.hpp"
#include "smaa_lut.hpp"
#include "solid_geometry.hpp"
#include "raster_viewport.hpp"
#include "math/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <string>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <utility>

namespace atelier::renderer::detail {
namespace {
void require(GfxResult result,const char* operation) {
    if(result) throw Error{{rhi::ErrorCode::execution_failed,result,operation},true};
}
void allocation(bool valid) {
    if(!valid) throw Error{{rhi::ErrorCode::creation_failed,kGfxResult_OutOfMemory,"Scene Lit GPU allocation failed"},true};
}
void invalid(bool valid,const char* message) {
    if(!valid) throw Error{{rhi::ErrorCode::invalid_argument,kGfxResult_InvalidParameter,message},false};
}
math::Matrix4d matrix(const std::array<float,16>& value) {
    math::Matrix4d result;for(std::size_t i=0;i<16;i++)result.elements[i]=value[i];return result;
}
std::array<float,16> floats(const math::Matrix4d& value) {
    std::array<float,16> result;for(std::size_t i=0;i<16;i++)result[i]=static_cast<float>(value.elements[i]);return result;
}
float halton(std::uint32_t index,std::uint32_t base) {
    float value=0,factor=1;while(index){factor/=base;value+=factor*(index%base);index/=base;}return value;
}
struct GpuInstance {
    std::array<float,16> world,previous_world;
    std::array<float,4> normal_x,normal_y,normal_z;
    std::array<std::uint32_t,4> geometry; // RT offset, stable key low, history valid, stable key high.
    bool operator==(const GpuInstance&) const=default;
};
struct GpuRayTriangle {
    std::array<geometry::Vertex,3> vertices;
    TriangleVolume volume;
    std::uint32_t material_slot{};
    std::uint32_t closure_first{},closure_count{},reserved{};
};
// DDGIRayHit in lit_ddgi_common.hlsl. Per-dispatch scratch, never history.
struct ProbeRayHitStorage { std::array<std::uint32_t,16> words; };
static_assert(sizeof(GpuInstance)==192);
static_assert(sizeof(geometry::Vertex)==48 && sizeof(SceneTriangle)==176);
static_assert(sizeof(GpuRayTriangle)==176);
static_assert(sizeof(ProbeRayHitStorage)==64);
}

struct SceneLit::State {
    rhi::Context& context;
    GfxContext gfx;
    rhi::Pipeline gbuffer,di_initial,di_spatial,direct,direct_resolve,direct_filter,compose;
    rhi::Pipeline probe_copy,probe_trace,probe_update,probe_resolve,ao,ao_resolve,depth_first,depth_pyramid,ssr_reset,ssr_classify,ssr_prepare,ssr_trace;
    rhi::Pipeline reflection_resolve,reflection_filter,brdf,taa,bloom_downsample,bloom_blur,tone_map,tone;
    rhi::Pipeline smaa_edges_pass,smaa_weights_pass,smaa_neighborhood,smaa_resolve;
    rhi::Pipeline ssil_trace,ssil_resolve,probe_debug_surface,probe_debug_geometry;
    GfxTexture probe_age,probe_debug_depth;
    std::array<GfxTimestampQuery,16> queries{}; // Eight stages, two DDGI, four direct and two DDGI-update subscopes.
    std::uint32_t encoded_pass_mask{};
    GfxSamplerState sampler;
    ui::PixelExtent extent{};
    std::array<GfxTexture,7> gbuffer_textures{};
    GfxTexture hdr,indirect,ao_texture,ao_raw,reflection_trace,reflection_hit_trace,brdf_lut;
    struct DirectSignal { GfxTexture raw; std::array<GfxTexture,2> history{},moments{},scratch{}; };
    std::array<DirectSignal,2> direct_signals{}; // Diffuse/specular radiance with separate reconstruction support.
    std::array<GfxTexture,2> history_depth{},history_normal{},history_geometry_normal{},taa_history{},reflection_history{},reflection_hit_history{},reflection_moments{},reflection_scratch{},ao_history{},bloom{};
    GfxTexture tone_color,smaa_edges,smaa_weights,smaa_output,smaa_area,smaa_search;
    std::array<GfxTexture,2> smaa_spatial{},history_base_metallic{},history_surface_id{};
    std::array<GfxBuffer,2> di_reservoirs{};
    GfxBuffer di_scratch; // Distinct spatial input; neighbors never read an in-place update.
    GfxTexture ssil_raw,ssil_irradiance;
    std::array<GfxTexture,2> ssil_history{};
    std::array<GfxTexture,12> pyramid{};
    std::uint32_t pyramid_count{};
    std::array<GfxTexture,2> probe_irradiance{},probe_distance{},probe_offsets{};
    GfxTexture probe_target{}; // Each scheduled probe exclusively owns its sampled target texels.
    GfxBuffer probe_ray_hits;
    ProbeField probe_field;
    std::array<ProbeVolumeSettings,probe_volume_limit> previous_probe_settings{};
    GfxBuffer gpu_instances,gpu_sources,gpu_previous_sources,gpu_proposals,gpu_current_to_previous,gpu_previous_to_current;
    GfxBuffer rt_triangles,rt_closed_slots,reflection_work,reflection_counter,reflection_arguments;
    LightFrame light_frame;
    GfxBuffer ray_counters;
    static constexpr std::size_t readback_count=32;
    std::array<GfxBuffer,readback_count> ray_readback{};
    std::array<std::uint64_t,readback_count> readback_sequences{},readback_frame_ids{};
    std::array<std::uint32_t,readback_count> readback_pass_masks{};
    GfxTopLevelAccelerationStructure tlas;
    struct Geometry {
        geometry::MeshRef source;
        GfxBuffer positions;
        GfxGeometry geometry;
        GfxBottomLevelAccelerationStructure blas;
        std::vector<TriangleVolume> volumes;
        std::map<std::uint32_t,std::vector<std::uint32_t>> component_slots;
        std::array<float,3> minimum{},maximum{};
        std::uint32_t offset{};
        bool used{},opaque{true},next_opaque{true};
    };
    struct Object {
        GfxTopLevelAccelerationStructureInstance instance;
        const geometry::Mesh* geometry{};
        std::array<float,16> world{};
        std::array<float,4> normal_x{},normal_y{},normal_z{};
        std::vector<material::Description> surfaces;
        bool emitting{};
        bool used{};
    };
    struct ObjectKey {
        std::uint64_t value{};
        bool legacy{};
        bool operator==(const ObjectKey&) const=default;
    };
    struct ObjectKeyHash {
        std::size_t operator()(ObjectKey key) const noexcept {
            return std::hash<std::uint64_t>{}(key.value) ^ (key.legacy?std::size_t{0x9e3779b9u}:0);
        }
    };
    std::unordered_map<const geometry::Mesh*,Geometry> geometries;
    std::unordered_map<ObjectKey,Object,ObjectKeyHash> objects;
    std::vector<GpuInstance> instances;
    std::vector<GpuRayTriangle> packed_triangles;
    std::vector<std::uint32_t> packed_closed_slots;
    std::vector<ObjectKey> object_keys,previous_object_keys;
    std::array<float,16> view_projection{},inverse_view_projection{},previous_view_projection{},previous_inverse_view_projection{};
    std::array<float,4> viewport{},environment{},controls{},temporal{},jitter{},probe_history{},probe_controls{};
    std::array<float,4> probe_scene_min{},probe_scene_max{};
    std::array<std::uint32_t,4> size_frame{},source_counts{},di_settings{},probe_update_settings{};
    std::array<std::uint32_t,4> screen_dispatch{};
    std::array<float,4> raster_transform{1,1,0,0};
    std::uint32_t effect_flags{},di_history_valid{};
    std::uint64_t scene_identity{},scene_revision{},reset_events{},sequence{};
    std::uint32_t parity{},probe_parity{},probe_updates_pending{},probe_geometry_pending{},probe_sequence{};
    float previous_probe_bounce{.85f},previous_indirect_gain{1};
    bool previous_probe_feedback_visibility{true};
    std::uint32_t previous_effect_flags{},previous_direct_candidates{};
    bool previous_direct_reference{};
    std::uint32_t history_invalid{};
    Antialiasing previous_antialiasing{Antialiasing::none};
    bool history_valid{},enabled{},brdf_ready{},tlas_built{};
    bool light_changed{},material_changed{},coverage_changed{},emitter_changed{};
    const MaterialFrame* material_frame{};
    explicit State(rhi::Context& owner):context(owner),gfx(owner.borrow()){}

    template<class T> void bind(GfxProgram p,const char* name,const T& value) {
        require(gfxProgramSetParameter(gfx,p,name,value),name);
    }
    void retire(GfxTexture& texture) {if(texture)require(gfxDestroyTexture(gfx,std::exchange(texture,{})),"Retire Scene Lit texture");}
    void retire(GfxBuffer& buffer) {if(buffer)require(gfxDestroyBuffer(gfx,std::exchange(buffer,{})),"Retire Scene Lit buffer");}
    GfxTexture texture(std::uint32_t width,std::uint32_t height,DXGI_FORMAT format,std::uint32_t) {
        auto result=gfxCreateTexture2D(gfx,width,height,format);allocation(bool(result));
        return result;
    }
    void prepare(rhi::Pipeline& output,const char* source,const char* entry,const char* name) {
        rhi::PipelineDesc desc;desc.source.cs=source;desc.entry_point=entry;desc.name=name;
        auto result=context.prepare(desc);if(!result)throw Error{result.error(),true};output=std::move(*result);
    }
    void pipelines() {
        if(gbuffer)return;
        sampler=gfxCreateSamplerState(gfx,D3D12_FILTER_MIN_MAG_MIP_LINEAR);allocation(bool(sampler));
        for(auto& query:queries){query=gfxCreateTimestampQuery(gfx);allocation(bool(query));}
        ray_counters=gfxCreateBuffer<std::uint32_t>(gfx,8);allocation(bool(ray_counters));
        for(auto& readback:ray_readback){readback=gfxCreateBuffer(gfx,32,nullptr,kGfxCpuAccess_Read);allocation(bool(readback));}
        const std::string vs=std::string(shaders::lit_gbuffer)+"\nLitRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_gbuffer_vs(v,i);}";
        const std::string ps=std::string(shaders::lit_gbuffer)+"\nLitGBuffer main(LitRaster i){return lit_gbuffer_ps(i);}";
        rhi::PipelineDesc desc;desc.kind=rhi::PipelineKind::vertex;desc.source.vs=vs.c_str();desc.source.ps=ps.c_str();desc.name="Scene Lit GBuffer";
        for(std::uint32_t i=0;i<4;i++)require(gfxDrawStateSetColorTarget(desc.draw_state,i,DXGI_FORMAT_R16G16B16A16_FLOAT),"Set GBuffer format");
        require(gfxDrawStateSetColorTarget(desc.draw_state,4,DXGI_FORMAT_R32_FLOAT),"Set previous surface depth format");
        require(gfxDrawStateSetColorTarget(desc.draw_state,5,DXGI_FORMAT_R16G16_FLOAT),"Set geometric normal format");
        require(gfxDrawStateSetColorTarget(desc.draw_state,6,DXGI_FORMAT_R32G32_UINT),"Set stable surface identity format");
        require(gfxDrawStateSetDepthStencilTarget(desc.draw_state,DXGI_FORMAT_D32_FLOAT),"Set GBuffer depth");
        require(gfxDrawStateSetDepthFunction(desc.draw_state,D3D12_COMPARISON_FUNC_GREATER),"Set GBuffer reversed depth");
        require(gfxDrawStateSetCullMode(desc.draw_state,D3D12_CULL_MODE_NONE),"Set GBuffer culling");
        auto result=context.prepare(desc);if(!result)throw Error{result.error(),true};gbuffer=std::move(*result);
        prepare(di_initial,shaders::lit_restir,"lit_restir_initial_cs","ReSTIR DI initial and temporal sampling");
        prepare(di_spatial,shaders::lit_restir,"lit_restir_spatial_cs","ReSTIR DI spatial reuse");
        prepare(direct,shaders::lit_restir,"lit_restir_shade_cs","ReSTIR DI current-frame visibility");
        prepare(direct_resolve,shaders::lit_direct,"lit_direct_resolve_cs","Direct radiance temporal reconstruction");
        prepare(direct_filter,shaders::lit_direct,"lit_direct_filter_cs","Direct radiance spatial reconstruction");
        prepare(compose,shaders::lit_direct,"lit_compose_cs","Scene Lit HDR composition");
        prepare(probe_copy,shaders::lit_ddgi,"DDGICopy","DDGI history transfer");
        prepare(probe_trace,shaders::lit_ddgi,"DDGITrace","DDGI RT primary trace");
        prepare(probe_update,shaders::lit_ddgi,"DDGIUpdate","DDGI hit lighting and convolution");
        prepare(probe_resolve,shaders::lit_ddgi,"DDGIResolve","DDGI irradiance gather");
        prepare(ao,shaders::lit_post,"lit_gtao_cs","Scene Lit GTAO");
        prepare(ao_resolve,shaders::lit_post,"lit_gtao_resolve_cs","GTAO signal reconstruction");
        prepare(depth_first,shaders::lit_post,"lit_depth_first_cs","Scene Lit first depth reduction");
        prepare(depth_pyramid,shaders::lit_post,"lit_depth_pyramid_cs","Scene Lit depth hierarchy");
        prepare(ssr_reset,shaders::lit_post,"lit_ssr_reset_cs","SSSR work reset");
        prepare(ssr_classify,shaders::lit_post,"lit_ssr_classify_cs","SSSR classify");
        prepare(ssr_prepare,shaders::lit_post,"lit_ssr_prepare_cs","SSSR indirect work");
        prepare(ssr_trace,shaders::lit_post,"lit_ssr_trace_cs","SSSR hierarchy traversal");
        prepare(reflection_resolve,shaders::lit_post,"lit_reflection_resolve_cs","SSSR temporal resolve");
        prepare(reflection_filter,shaders::lit_post,"lit_reflection_filter_cs","SSSR spatial reconstruction");
        prepare(brdf,shaders::lit_post,"lit_brdf_lut_cs","Split sum BRDF integration");
        prepare(taa,shaders::lit_post,"lit_taa_cs","Scene Lit temporal antialiasing");
        prepare(bloom_downsample,shaders::lit_post,"lit_bloom_downsample_cs","Scene Lit bloom downsample");
        prepare(bloom_blur,shaders::lit_post,"lit_bloom_blur_cs","Scene Lit bloom filter");
        prepare(tone_map,shaders::lit_post,"lit_tonemap_cs","Scene Lit SDR scene resolve");
        prepare(smaa_edges_pass,shaders::lit_smaa,"lit_smaa_edges_cs","SMAA edges");
        prepare(smaa_weights_pass,shaders::lit_smaa,"lit_smaa_weights_cs","SMAA coverage weights");
        prepare(smaa_neighborhood,shaders::lit_smaa,"lit_smaa_neighborhood_cs","SMAA spatial reconstruction");
        prepare(smaa_resolve,shaders::lit_smaa,"lit_smaa_resolve_cs","SMAA two-frame reconstruction");
        prepare(ssil_trace,shaders::lit_ssil,"lit_ssil_trace_cs","Local indirect light sampling");
        prepare(ssil_resolve,shaders::lit_ssil,"lit_ssil_resolve_cs","Local indirect light reconstruction");
        const std::string tone_source=std::string(shaders::lit_post)+"\nfloat4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return lit_display_ps(p,uv);}";
        rhi::PipelineDesc tone_desc;tone_desc.kind=rhi::PipelineKind::vertex;tone_desc.source.vs=shaders::viewport_vs;
        tone_desc.source.ps=tone_source.c_str();tone_desc.name="Scene Lit display tone";
        require(gfxDrawStateSetColorTarget(tone_desc.draw_state,0,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),"Set Scene Lit display format");
        require(gfxDrawStateSetCullMode(tone_desc.draw_state,D3D12_CULL_MODE_NONE),"Set Scene Lit display culling");
        require(gfxDrawStateSetDepthWriteMask(tone_desc.draw_state,D3D12_DEPTH_WRITE_MASK_ZERO),"Disable tone depth writes");
        result=context.prepare(tone_desc);if(!result)throw Error{result.error(),true};tone=std::move(*result);
        brdf_lut=texture(64,64,DXGI_FORMAT_R16G16_FLOAT,4);
        const auto upload_lut=[&](GfxTexture& target,std::uint32_t width,std::uint32_t height,DXGI_FORMAT format,std::span<const unsigned char> bytes) {
            target=texture(width,height,format,0);
            auto upload=gfxCreateBuffer(gfx,bytes.size(),bytes.data(),kGfxCpuAccess_Write);allocation(bool(upload));
            const auto copied=gfxCommandCopyBufferToTexture(gfx,target,upload);
            const auto retired=gfxDestroyBuffer(gfx,upload);require(copied,"Upload SMAA lookup");require(retired,"Retire SMAA upload");
        };
        upload_lut(smaa_area,smaa_lut::area_width,smaa_lut::area_height,DXGI_FORMAT_R8G8_UNORM,smaa_lut::area_bytes());
        upload_lut(smaa_search,smaa_lut::search_width,smaa_lut::search_height,DXGI_FORMAT_R8_UNORM,smaa_lut::search_bytes());
    }
    void retire_targets() {
        for(auto& t:gbuffer_textures)retire(t);
        for(auto* t:{&hdr,&indirect,&ao_texture,&ao_raw,&reflection_trace,&reflection_hit_trace,&tone_color,&smaa_edges,&smaa_weights,&smaa_output,&ssil_raw,&ssil_irradiance})retire(*t);
        for(auto* pair:{&history_depth,&history_normal,&history_geometry_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_moments,&reflection_scratch,&ao_history,&bloom,&smaa_spatial,&ssil_history,&history_base_metallic,&history_surface_id})for(auto& t:*pair)retire(t);
        for(auto& signal:direct_signals){retire(signal.raw);for(auto* pair:{&signal.history,&signal.moments,&signal.scratch})for(auto& t:*pair)retire(t);}
        for(auto& t:pyramid)retire(t);
        for(auto* b:{&di_scratch,&di_reservoirs[0],&di_reservoirs[1],&reflection_work,&reflection_counter,&reflection_arguments})retire(*b);
    }
    void targets(ui::PixelExtent next) {
        if(extent==next)return;
        retire_targets();extent=next;
        const auto w=next.width,h=next.height;
        for(std::size_t i=0;i<gbuffer_textures.size();++i)gbuffer_textures[i]=texture(w,h,i==4?DXGI_FORMAT_R32_FLOAT:(i==5?DXGI_FORMAT_R16G16_FLOAT:(i==6?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R16G16B16A16_FLOAT)),i>=4?4:8);
        for(auto* t:{&hdr,&indirect,&reflection_trace,&tone_color,&smaa_output})*t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        reflection_hit_trace=texture(w,h,DXGI_FORMAT_R32G32_UINT,8);
        ao_texture=texture(w,h,DXGI_FORMAT_R16_FLOAT,2);
        ao_raw=texture(w,h,DXGI_FORMAT_R16_FLOAT,2);
        for(auto* pair:{&history_normal,&taa_history,&reflection_history,&reflection_scratch,&smaa_spatial,&history_base_metallic})for(auto& t:*pair)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        for(auto& t:reflection_hit_history)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        for(auto& t:reflection_moments)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        for(auto& t:history_surface_id)t=texture(w,h,DXGI_FORMAT_R32G32_UINT,8);
        for(auto& t:history_geometry_normal)t=texture(w,h,DXGI_FORMAT_R16G16_FLOAT,4);
        for(auto& t:ao_history)t=texture(w,h,DXGI_FORMAT_R16G16_FLOAT,4);
        smaa_edges=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4);smaa_weights=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4);
        for(auto& signal:direct_signals) {
            signal.raw=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
            for(auto* pair:{&signal.history,&signal.scratch})for(auto& t:*pair)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
            for(auto& t:signal.moments)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        }
        for(auto& t:history_depth)t=texture(w,h,DXGI_FORMAT_R32_FLOAT,4);
        for(auto& t:bloom)t=texture((w+3)/4,(h+3)/4,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        // Logical level zero is the existing FP32 primary depth, not a (z,z) copy.
        // Keep the original twelve logical levels, including the 1x1 fallback.
        std::uint32_t pw=(w+1)/2,ph=(h+1)/2;pyramid_count=0;
        while(pyramid_count<11){pyramid[pyramid_count++]=texture(pw,ph,DXGI_FORMAT_R32G32_FLOAT,8);if(pw==1&&ph==1)break;pw=(pw+1)/2;ph=(ph+1)/2;}
        // 24-byte reservoir, FP32 random coordinates, weights and confidence.
        struct ReservoirStorage { std::array<std::uint32_t,6> words; };
        for(auto& buffer:di_reservoirs){buffer=gfxCreateBuffer<ReservoirStorage>(gfx,std::uint64_t(w)*h);allocation(bool(buffer));}
        di_scratch=gfxCreateBuffer<ReservoirStorage>(gfx,std::uint64_t(w)*h);allocation(bool(di_scratch));
        reflection_work=gfxCreateBuffer<std::uint32_t>(gfx,w*h);reflection_counter=gfxCreateBuffer<std::uint32_t>(gfx,1);
        reflection_arguments=gfxCreateBuffer<std::uint32_t>(gfx,3);
        allocation(bool(reflection_work)&&bool(reflection_counter)&&bool(reflection_arguments));
        history_valid=false;reset_events++;
    }
    void probes(const SceneLitSettings& settings,const std::array<float,3>& focus,FrameStats& stats) {
        probe_field.describe(settings,focus);
        const auto w=probe_field.width*8,h=probe_field.height*8;
        if(probe_irradiance[0].getWidth()!=w || probe_irradiance[0].getHeight()!=h) {
            for(auto* pair:{&probe_irradiance,&probe_distance,&probe_offsets})for(auto& t:*pair)retire(t);
            retire(probe_target);
            retire(probe_age);
            for(auto& t:probe_irradiance)t=texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16);
            probe_target=texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16);
            for(auto& t:probe_distance)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
            // World-space relocation must retain the range of the supported grid.
            for(auto& t:probe_offsets)t=texture(probe_field.width,probe_field.height,DXGI_FORMAT_R32G32B32A32_FLOAT,16);
            probe_age=texture(probe_field.width,probe_field.height,DXGI_FORMAT_R32_UINT,4);
            probe_field.invalidate();probe_field.describe(settings,focus);
            probe_parity=0;probe_updates_pending=0;probe_geometry_pending=0;probe_sequence=0;
        }
        probe_field.plan(settings,stats);
        const auto rays=std::uint64_t(probe_field.scheduled)*settings.probe_rays;
        invalid(rays<=std::numeric_limits<std::uint32_t>::max(),"DDGI ray scratch exceeds shader index range");
        if(probe_ray_hits.getSize()<rays*sizeof(ProbeRayHitStorage)) {
            retire(probe_ray_hits);
            probe_ray_hits=gfxCreateBuffer<ProbeRayHitStorage>(gfx,rays);allocation(bool(probe_ray_hits));
        }
    }
    void bind_probes(GfxProgram p,bool newest) {
        bind(p,"lit_probe_volumes",newest?probe_field.current:probe_field.previous);
        bind(p,"lit_probe_current_volumes",probe_field.current);
        bind(p,"lit_probe_pool",std::array<std::uint32_t,4>{probe_field.width,probe_field.total,0,probe_field.valid?1u:0u});
        bind(p,"lit_probe_update",probe_update_settings);
        bind(p,"lit_probe_sequence",probe_sequence);bind(p,"lit_probe_age_output",probe_age);
        bind(p,"lit_probe_history",probe_history);bind(p,"lit_probe_controls",probe_controls);
        bind(p,"lit_probe_scene_min",probe_scene_min);bind(p,"lit_probe_scene_max",probe_scene_max);
        const auto read=newest?probe_parity:1-probe_parity;
        bind(p,"lit_probe_irradiance_previous",probe_irradiance[read]);bind(p,"lit_probe_distance_previous",probe_distance[read]);
        bind(p,"lit_probe_offsets_previous",probe_offsets[read]);bind(p,"lit_probe_target",probe_target);
        bind(p,"lit_probe_irradiance_output",probe_irradiance[probe_parity]);
        bind(p,"lit_probe_distance_output",probe_distance[probe_parity]);bind(p,"lit_probe_offsets_output",probe_offsets[probe_parity]);
    }
    void common(GfxProgram p,GfxTexture depth,std::uint32_t visibility_counter=0) {
        bind_materials(gfx,p,*material_frame);
        bind(p,"raster_viewport_transform",raster_transform);
        bind(p,"lit_size_frame",size_frame);bind(p,"lit_viewport",viewport);bind(p,"lit_screen_dispatch",screen_dispatch);bind(p,"lit_view_projection",view_projection);
        bind(p,"lit_inverse_view_projection",inverse_view_projection);bind(p,"lit_previous_view_projection",previous_view_projection);
        bind(p,"lit_previous_inverse_view_projection",previous_inverse_view_projection);
        bind(p,"lit_environment",environment);bind(p,"lit_controls",controls);
        bind(p,"lit_temporal",temporal);bind(p,"lit_jitter",jitter);bind(p,"lit_effect_flags",effect_flags);
        bind(p,"lit_source_counts",source_counts);bind(p,"lit_di_settings",di_settings);bind(p,"lit_di_history_valid",di_history_valid);
        bind(p,"lit_history_invalid",history_invalid);
        bind(p,"lit_instances",gpu_instances);bind(p,"lit_rt_triangles",rt_triangles);bind(p,"lit_closed_slots",rt_closed_slots);
        bind(p,"lit_sources",gpu_sources);bind(p,"lit_previous_sources",gpu_previous_sources);bind(p,"lit_light_proposals",gpu_proposals);
        bind(p,"lit_current_to_previous",gpu_current_to_previous);bind(p,"lit_previous_to_current",gpu_previous_to_current);
        bind(p,"lit_scene",tlas);bind(p,"lit_depth",depth);bind(p,"lit_normal_roughness",gbuffer_textures[0]);
        bind(p,"lit_ray_counters",ray_counters);
        bind(p,"lit_visibility_counter",visibility_counter);
        bind(p,"lit_base_metallic",gbuffer_textures[1]);bind(p,"lit_emissive",gbuffer_textures[2]);bind(p,"lit_motion",gbuffer_textures[3]);
        bind(p,"lit_previous_depth",gbuffer_textures[4]);bind(p,"lit_surface_id",gbuffer_textures[6]);
        bind(p,"lit_history_base_metallic",history_base_metallic[1-parity]);bind(p,"lit_history_surface_id",history_surface_id[1-parity]);
        bind(p,"lit_geometry_normal",gbuffer_textures[5]);bind(p,"lit_history_geometry_normal",history_geometry_normal[1-parity]);
        bind(p,"lit_linear_sampler",sampler);bind(p,"lit_history_depth",history_depth[1-parity]);bind(p,"lit_history_normal",history_normal[1-parity]);
        bind(p,"lit_hdr_source",hdr);bind(p,"lit_brdf_lut",brdf_lut);
    }
    void dispatch(const rhi::Pipeline& pipeline,GfxTexture depth,std::uint32_t x,std::uint32_t y=1,std::uint32_t z=1,std::uint32_t visibility_counter=0) {
        common(pipeline.program(),depth,visibility_counter);require(gfxCommandBindKernel(gfx,pipeline.kernel()),"Bind Scene Lit compute");
        require(gfxCommandDispatch(gfx,x,y,z),"Dispatch Scene Lit compute");
    }
    void dispatch_screen(const rhi::Pipeline& pipeline,GfxTexture depth,std::uint32_t visibility_counter=0) {
        const auto width=screen_dispatch[2]-screen_dispatch[0],height=screen_dispatch[3]-screen_dispatch[1];
        if(width&&height)dispatch(pipeline,depth,(width+7)/8,(height+7)/8,1,visibility_counter);
    }
    void begin(std::uint32_t pass,bool enabled=true) {
        if(enabled)encoded_pass_mask|=1u<<pass;
        require(gfxCommandBeginTimestampQuery(gfx,queries[pass]),"Begin Scene Lit timestamp");
    }
    void end(std::uint32_t pass) {require(gfxCommandEndTimestampQuery(gfx,queries[pass]),"End Scene Lit timestamp");}
    void reset(bool reset_probes=true) {
        if(history_valid||(reset_probes&&probe_field.valid)) {
            history_valid=false;if(reset_probes){probe_field.invalidate();probe_parity=0;probe_updates_pending=0;probe_geometry_pending=0;probe_sequence=0;}reset_events++;
        }
    }
    void publish_gpu_sample(FrameStats& stats) {
        const auto sample=gfxTimestampQueryGetSample(gfx,queries[0]);
        if(sample.frame_sequence==0)return;
        std::array<float,8> timings{};
        std::array<float,2> ddgi_timings{};
        std::array<float,4> direct_timings{};
        std::array<float,2> ddgi_update_timings{};
        for(std::size_t i=0;i<queries.size();i++) {
            const auto pass=gfxTimestampQueryGetSample(gfx,queries[i]);
            if(pass.frame_sequence!=sample.frame_sequence)return;
            if(i<timings.size())timings[i]=static_cast<float>(pass.duration_ms);
            else if(i<10)ddgi_timings[i-8]=static_cast<float>(pass.duration_ms);
            else if(i<14)direct_timings[i-10]=static_cast<float>(pass.duration_ms);
            else ddgi_update_timings[i-14]=static_cast<float>(pass.duration_ms);
        }
        for(std::size_t slot=0;slot<readback_count;slot++) {
            if(readback_sequences[slot]!=sample.frame_sequence)continue;
            const auto* counter=gfxBufferGetData<std::uint32_t>(gfx,ray_readback[slot]);
            if(!counter)return;
            stats.scene_lit_gpu_ms=timings;stats.scene_lit_ddgi_gpu_ms=ddgi_timings;stats.scene_lit_gpu_sample_sequence=sample.frame_sequence;
            stats.scene_lit_direct_gpu_ms=direct_timings;stats.scene_lit_ddgi_update_gpu_ms=ddgi_update_timings;
            stats.scene_lit_gpu_pass_mask=readback_pass_masks[slot];
            stats.scene_lit_gpu_sample_frame_id=readback_frame_ids[slot];
            const auto ray_count=[&](std::size_t i){return counter[i]|(std::uint64_t(counter[i+4])<<32);};
            stats.scene_lit_shadow_rays=ray_count(0);
            for(std::size_t i=0;i<ddgi_timings.size();++i){stats.scene_lit_ddgi_visibility_rays[i]=ray_count(i+2);stats.scene_lit_shadow_rays+=ray_count(i+2);}
            stats.scene_lit_probe_rays=ray_count(1);
            stats.scene_lit_gpu_timing_valid=true;return;
        }
    }
    std::uint64_t resource_bytes() const {
        std::uint64_t result=0;
        const auto add_texture=[&](GfxTexture t){if(!t)return;std::uint32_t bytes=8;
            if(t.getFormat()==DXGI_FORMAT_R32G32B32A32_FLOAT)bytes=16;
            else if(t.getFormat()==DXGI_FORMAT_R16_FLOAT)bytes=2;
            else if(t.getFormat()==DXGI_FORMAT_R8_UNORM)bytes=1;
            else if(t.getFormat()==DXGI_FORMAT_R8G8_UNORM)bytes=2;
            else if(t.getFormat()==DXGI_FORMAT_R32_FLOAT||t.getFormat()==DXGI_FORMAT_R32_UINT||t.getFormat()==DXGI_FORMAT_D32_FLOAT||t.getFormat()==DXGI_FORMAT_R16G16_FLOAT||t.getFormat()==DXGI_FORMAT_R8G8B8A8_UNORM)bytes=4;
            result+=std::uint64_t(t.getWidth())*t.getHeight()*t.getDepth()*bytes;};
        for(auto t:gbuffer_textures)add_texture(t);
        for(auto t:{hdr,indirect,ao_texture,ao_raw,reflection_trace,reflection_hit_trace,brdf_lut,tone_color,smaa_edges,smaa_weights,smaa_output,smaa_area,smaa_search,ssil_raw,ssil_irradiance,probe_target,probe_age,probe_debug_depth})add_texture(t);
        for(const auto* pair:{&history_depth,&history_normal,&history_geometry_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_moments,&reflection_scratch,&ao_history,&smaa_spatial,&ssil_history,&bloom,&history_base_metallic,&history_surface_id,&probe_irradiance,&probe_distance,&probe_offsets})for(auto t:*pair)add_texture(t);
        for(const auto& signal:direct_signals){add_texture(signal.raw);for(const auto* pair:{&signal.history,&signal.moments,&signal.scratch})for(auto t:*pair)add_texture(t);}
        for(auto t:pyramid)add_texture(t);
        for(auto b:{gpu_instances,gpu_sources,gpu_previous_sources,gpu_proposals,gpu_current_to_previous,gpu_previous_to_current,di_scratch,di_reservoirs[0],di_reservoirs[1],rt_triangles,rt_closed_slots,reflection_work,reflection_counter,reflection_arguments,probe_ray_hits,ray_counters})result+=b.getSize();
        for(auto b:ray_readback)result+=b.getSize();
        for(const auto& [key,g]:geometries){(void)key;result+=g.positions.getSize()+gfxBottomLevelAccelerationStructureGetDataSize(gfx,g.blas);}
        if(tlas)result+=gfxTopLevelAccelerationStructureGetDataSize(gfx,tlas);
        return result;
    }

    bool acceleration(const Frame& frame) {
        material_changed=false;coverage_changed=false;
        emitter_changed=false;
        bool geometry_changed=false,topology_changed=false,transform_changed=false;
        bool instances_changed=instances.size()!=frame.meshes.size();
        if(!tlas){tlas=gfxCreateTopLevelAccelerationStructure(gfx);allocation(bool(tlas));}
        for(auto& [key,g]:geometries){(void)key;g.used=false;g.next_opaque=true;}
        for(auto& [key,o]:objects){(void)key;o.used=false;}
        for(std::size_t i=0;i<frame.meshes.size();++i) {
            const auto& mesh=frame.meshes[i];bool opaque=true;
            for(std::size_t j=0;j<mesh.geometry->materials().size();++j)
                opaque &= material_frame->surface(i,j).parameters.coverage==material::Coverage::opaque;
            auto [at,added]=geometries.try_emplace(mesh.geometry.get());auto& g=at->second;
            if(added)g.source=mesh.geometry;
            g.used=true;g.next_opaque &= opaque;
        }
        // Aggregate coverage in the owning geometry cache. Repeated frames and
        // instances share it without allocating a second per-frame hash table.
        for(auto& [key,g]:geometries) {
            (void)key;if(!g.used)continue;
            if(!g.blas) {
                g.volumes=triangle_volumes(*g.source);const auto vertices=g.source->vertices();
                std::copy_n(vertices.front().position,3,g.minimum.begin());g.maximum=g.minimum;
                for(const auto& vertex:vertices)for(std::size_t axis=0;axis<3;++axis) {
                    g.minimum[axis]=std::min(g.minimum[axis],vertex.position[axis]);
                    g.maximum[axis]=std::max(g.maximum[axis],vertex.position[axis]);
                }
                for(std::size_t i=0;i<g.volumes.size();++i)if(g.volumes[i].closed)
                    g.component_slots[g.volumes[i].closed].push_back(g.source->material_slot(i));
                for(auto& [component,slots]:g.component_slots) {
                    (void)component;std::sort(slots.begin(),slots.end());slots.erase(std::unique(slots.begin(),slots.end()),slots.end());
                }
                g.positions=gfxCreateBuffer<geometry::Vertex>(gfx,static_cast<std::uint32_t>(vertices.size()),vertices.data());allocation(bool(g.positions));
                auto range=gfxCreateBufferRange(gfx,g.positions,0,g.positions.getSize());allocation(bool(range));
                g.geometry=gfxCreateGeometryTriangles(gfx,range,sizeof(geometry::Vertex));allocation(bool(g.geometry));
                g.opaque=g.next_opaque;
                require(gfxGeometrySetOpaque(gfx,g.geometry,g.opaque),"Set RT material coverage");
                g.blas=gfxCreateBottomLevelAccelerationStructure(gfx);allocation(bool(g.blas));
                require(gfxBottomLevelAccelerationStructureAddGeometry(gfx,g.blas,g.geometry),"Attach RT mesh geometry");
                const GfxBuildBottomLevelASFlags flags=kGfxBuildBottomLevelASFlag_FastTrace;
                require(gfxBuildBottomLevelAccelerationStructures(gfx,&g.blas,&flags,1),"Build static mesh BLAS");geometry_changed=true;
            } else if(g.opaque!=g.next_opaque) {
                g.opaque=g.next_opaque;
                require(gfxGeometrySetOpaque(gfx,g.geometry,g.opaque),"Change RT material coverage");
                const GfxBuildBottomLevelASFlags flags=kGfxBuildBottomLevelASFlag_FastTrace;
                require(gfxBuildBottomLevelAccelerationStructures(gfx,&g.blas,&flags,1),"Rebuild changed material coverage BLAS");topology_changed=true;
            }
        }
        object_keys.clear();object_keys.reserve(frame.meshes.size());instances.resize(frame.meshes.size());
        for(std::size_t index=0;index<frame.meshes.size();index++) {
            const auto& mesh=frame.meshes[index];const ObjectKey key{mesh.stable_key?mesh.stable_key:index,mesh.stable_key==0};
            object_keys.push_back(key);auto [found,inserted]=objects.try_emplace(key);auto& object=found->second;object.used=true;
            const bool correspondence=!inserted&&mesh.stable_key!=0&&object.geometry==mesh.geometry.get()&&history_valid;
            GpuInstance instance{};instance.world=mesh.world;instance.previous_world=correspondence?object.world:mesh.world;
            if(inserted||object.world!=mesh.world) {
                const auto inverse=math::try_inverse(matrix(mesh.world));allocation(bool(inverse));
                const auto& inv=inverse->elements;
                const double determinant=inv[0]*(inv[5]*inv[10]-inv[6]*inv[9])-inv[1]*(inv[4]*inv[10]-inv[6]*inv[8])+inv[2]*(inv[4]*inv[9]-inv[5]*inv[8]);
                object.normal_x[3]=determinant<0?-1.f:1.f;
                double maximum=0;for(std::size_t r=0;r<3;r++)for(std::size_t c=0;c<3;c++)maximum=std::max(maximum,std::abs(inverse->elements[c*4+r]));
                for(std::size_t c=0;c<3;c++){object.normal_x[c]=static_cast<float>(inverse->elements[c*4]/maximum);
                    object.normal_y[c]=static_cast<float>(inverse->elements[c*4+1]/maximum);object.normal_z[c]=static_cast<float>(inverse->elements[c*4+2]/maximum);}
            }
            instance.normal_x=object.normal_x;instance.normal_y=object.normal_y;instance.normal_z=object.normal_z;
            const auto descriptions=std::span(material_frame->descriptions).subspan(material_frame->offsets[index],mesh.geometry->materials().size());
            const bool surface_changed=!inserted&&!std::ranges::equal(object.surfaces,descriptions);
            material_changed|=surface_changed;
            if(surface_changed) {
                // Mask sampling changes ray geometry even when the shared BLAS
                // remains non-opaque. Color-only edits do not change distances.
                coverage_changed|=!std::ranges::equal(object.surfaces,descriptions,[](const auto& a,const auto& b) {
                    if(a.parameters.coverage!=b.parameters.coverage)return false;
                    if(a.parameters.coverage==material::Coverage::opaque)return true;
                    return a.parameters.base_color[3]==b.parameters.base_color[3] &&
                        a.parameters.alpha_cutoff==b.parameters.alpha_cutoff &&
                        a.texture(material::Input::base_color)==b.texture(material::Input::base_color) &&
                        a.texture(material::Input::opacity)==b.texture(material::Input::opacity);
                });
            }
            bool emitting=false;
            for(const auto& surface:descriptions) {
                const auto& e=surface.parameters.emissive;emitting|=e[0]>0||e[1]>0||e[2]>0;
            }
            if(emitting||object.emitting)
                emitter_changed|=inserted||object.geometry!=mesh.geometry.get()||object.world!=mesh.world||surface_changed;
            if(inserted||surface_changed)object.surfaces.assign(descriptions.begin(),descriptions.end());
            object.emitting=emitting;
            // Keep geometric motion, but edited material/radiance has no valid
            // previous shading observation, including when seen through SSR.
            instance.geometry={geometries.at(mesh.geometry.get()).offset,static_cast<std::uint32_t>(mesh.stable_key),correspondence&&!surface_changed?1u:0u,static_cast<std::uint32_t>(mesh.stable_key>>32)};
            instances_changed|=instances[index]!=instance;instances[index]=instance;
            if(inserted){object.instance=gfxCreateTopLevelAccelerationStructureInstance(gfx);allocation(bool(object.instance));
                require(gfxTopLevelAccelerationStructureAddInstance(gfx,tlas,object.instance),"Attach RT scene instance");topology_changed=true;}
            if(inserted||object.geometry!=mesh.geometry.get()) {
                require(gfxTopLevelAccelerationStructureInstanceSetBottomLevelAccelerationStructure(gfx,object.instance,geometries.at(mesh.geometry.get()).blas),"Set RT instance mesh");topology_changed=true;
            }
            if(inserted||object.world!=mesh.world) {
                std::array<float,12> transform{};for(std::size_t r=0;r<3;r++)for(std::size_t c=0;c<4;c++)transform[r*4+c]=mesh.world[c*4+r];
                require(gfxTopLevelAccelerationStructureInstanceSetTransform(gfx,object.instance,transform.data()),"Update rigid RT transform");transform_changed=true;
            }
            require(gfxTopLevelAccelerationStructureInstanceSetInstanceID(gfx,object.instance,static_cast<std::uint32_t>(index)),"Remap current RT snapshot");
            object.world=mesh.world;object.geometry=mesh.geometry.get();
        }
        for(auto it=objects.begin();it!=objects.end();) {
            if(it->second.used){++it;continue;}
            emitter_changed|=it->second.emitting;
            require(gfxTopLevelAccelerationStructureRemoveInstance(gfx,tlas,it->second.instance),"Detach removed RT instance");
            require(gfxDestroyTopLevelAccelerationStructureInstance(gfx,it->second.instance),"Retire removed RT instance");it=objects.erase(it);topology_changed=true;
        }
        for(auto it=geometries.begin();it!=geometries.end();) {
            if(it->second.used){++it;continue;}auto& g=it->second;
            require(gfxDestroyBottomLevelAccelerationStructure(gfx,g.blas),"Retire unused static BLAS");
            require(gfxDestroyGeometry(gfx,g.geometry),"Retire unused RT geometry");retire(g.positions);it=geometries.erase(it);geometry_changed=true;
        }
        if(geometry_changed) {
            packed_triangles.clear();packed_closed_slots.clear();
            for(auto& [key,g]:geometries) {
                (void)key;g.offset=static_cast<std::uint32_t>(packed_triangles.size());
                std::map<std::uint32_t,std::array<std::uint32_t,2>> ranges;
                for(const auto& [component,slots]:g.component_slots) {
                    // A single-slot component needs only the hit material's
                    // coverage flag. Multi-slot shells inspect their other slots.
                    if(slots.size()>1) {
                        ranges[component]={static_cast<std::uint32_t>(packed_closed_slots.size()),static_cast<std::uint32_t>(slots.size())};
                        packed_closed_slots.insert(packed_closed_slots.end(),slots.begin(),slots.end());
                    }
                }
                const auto vertices=g.source->vertices();
                for(std::size_t i=0;i<g.volumes.size();++i) {
                    const auto at=ranges.find(g.volumes[i].closed);
                    const auto range=at==ranges.end()?std::array<std::uint32_t,2>{}:at->second;
                    packed_triangles.push_back({{vertices[i*3],vertices[i*3+1],vertices[i*3+2]},g.volumes[i],g.source->material_slot(i),range[0],range[1],0});
                }
            }
            retire(rt_closed_slots);
            rt_closed_slots=gfxCreateBuffer<std::uint32_t>(gfx,std::max(1u,static_cast<std::uint32_t>(packed_closed_slots.size())),packed_closed_slots.empty()?nullptr:packed_closed_slots.data());allocation(bool(rt_closed_slots));
            retire(rt_triangles);rt_triangles=gfxCreateBuffer<GpuRayTriangle>(gfx,static_cast<std::uint32_t>(packed_triangles.size()),packed_triangles.data());allocation(bool(rt_triangles));
        }
        for(std::size_t i=0;i<instances.size();i++) {
            const auto offset=geometries.at(frame.meshes[i].geometry.get()).offset;
            instances_changed|=instances[i].geometry[0]!=offset;instances[i].geometry[0]=offset;
        }
        // Membership/order changes do not change the world-space probe domain.
        // New/replaced objects already have no surface correspondence above;
        // surviving stable objects retain it even if their GPU indices change.
        if(!tlas_built||topology_changed) {require(gfxTopLevelAccelerationStructureBuild(gfx,tlas),"Build scene TLAS");tlas_built=true;}
        else if(transform_changed||object_keys!=previous_object_keys)require(gfxTopLevelAccelerationStructureUpdate(gfx,tlas),"Update scene TLAS");
        emitter_changed|=object_keys!=previous_object_keys;
        previous_object_keys=object_keys;
        if(instances_changed||!gpu_instances) {
            retire(gpu_instances);gpu_instances=gfxCreateBuffer<GpuInstance>(gfx,static_cast<std::uint32_t>(instances.size()),instances.data());allocation(bool(gpu_instances));
        }
        if(geometry_changed||topology_changed||transform_changed) {
            std::array<double,3> low,high;low.fill(std::numeric_limits<double>::max());high.fill(-std::numeric_limits<double>::max());
            for(const auto& mesh:frame.meshes) {
                const auto& geometry=geometries.at(mesh.geometry.get());
                for(unsigned corner=0;corner<8;++corner)for(std::size_t axis=0;axis<3;++axis) {
                    double value=mesh.world[12+axis];
                    for(std::size_t a=0;a<3;++a)value+=double((corner&(1u<<a))?geometry.maximum[a]:geometry.minimum[a])*mesh.world[a*4+axis];
                    low[axis]=std::min(low[axis],value);high[axis]=std::max(high[axis],value);
                }
            }
            for(std::size_t axis=0;axis<3;++axis) {
                probe_scene_min[axis]=static_cast<float>(low[axis]);probe_scene_max[axis]=static_cast<float>(high[axis]);
            }
        }
        end(1);
        return geometry_changed||topology_changed||transform_changed;
    }

    void lighting(const Frame& frame) {
        light_frame.update(frame,*material_frame,sequence);
        light_changed=light_frame.changed;
        if(light_frame.changed||!gpu_sources) {
            const auto upload=[&]<class T>(GfxBuffer& buffer,const std::vector<T>& data) {
                retire(buffer);
                buffer=gfxCreateBuffer<T>(gfx,std::max(std::size_t{1},data.size()),data.empty()?nullptr:data.data());
                allocation(bool(buffer));
            };
            upload(gpu_sources,light_frame.sources);upload(gpu_previous_sources,light_frame.previous_sources);
            upload(gpu_proposals,light_frame.proposals);
            upload(gpu_current_to_previous,light_frame.current_to_previous);upload(gpu_previous_to_current,light_frame.previous_to_current);
        }
        source_counts={light_frame.local_count,light_frame.directional_count,light_frame.emitter_count,
            static_cast<std::uint32_t>(light_frame.previous_sources.size())};
    }
    void draw(const Frame& frame,ui::PixelExtent next,GfxTexture depth,std::span<const SceneLitDraw> draws,const MaterialFrame& materials,FrameStats& stats) {
        material_frame=&materials;
        pipelines();publish_gpu_sample(stats);encoded_pass_mask=0;begin(1);const auto& settings=frame.view.lit;targets(next);
        if(!enabled||scene_identity!=frame.scene_identity)reset();
        else if(frame.camera_cut)reset(false);
        const std::array<float,4> next_viewport{frame.scene_viewport.x,frame.scene_viewport.y,frame.scene_viewport.width,frame.scene_viewport.height};
        if(viewport!=next_viewport)reset(false);
        if(previous_probe_settings!=settings.probe_volumes || previous_indirect_gain!=settings.indirect_gain)reset(false);
        if(previous_probe_bounce!=settings.probe_bounce || previous_probe_feedback_visibility!=settings.probe_feedback_visibility)reset();
        previous_probe_settings=settings.probe_volumes;previous_indirect_gain=settings.indirect_gain;
        previous_probe_bounce=settings.probe_bounce;previous_probe_feedback_visibility=settings.probe_feedback_visibility;
        const std::uint32_t flags=(settings.shadows?1u:0u)|(settings.ddgi?2u:0u)|(settings.gtao&&!settings.ssil?4u:0u)|(settings.sssr?8u:0u)|(settings.antialiasing==Antialiasing::taa?16u:0u)|(settings.bloom?32u:0u)|(settings.ssil?64u:0u);
        const auto effect_changes=flags^previous_effect_flags;
        const bool shadow_bias_changed=settings.shadow_bias!=controls[1];
        const bool direct_changed=(effect_changes&1u)!=0||settings.direct_candidates!=previous_direct_candidates||settings.direct_reference!=previous_direct_reference||shadow_bias_changed;
        const bool reflection_changed=settings.reflection_distance!=controls[3]||settings.reflection_roughness!=temporal[1];
        const bool ao_changed=(effect_changes&4u)!=0||settings.ao_radius!=controls[2];
        // Invalidate each signal and its actual consumers when its input contract
        // changes. Continuous light edits still use each signal's local response.
        history_invalid=(direct_changed?1u:0u)|
            (direct_changed||reflection_changed||ao_changed||(effect_changes&(2u|4u|8u|64u))?2u:0u)|(ao_changed?4u:0u)|
            (direct_changed||reflection_changed||ao_changed||effect_changes?8u:0u)|
            (direct_changed||(effect_changes&(2u|64u))?16u:0u);
        if(settings.antialiasing!=previous_antialiasing)reset(false); // Projection sampling changed.
        previous_antialiasing=settings.antialiasing;
        previous_effect_flags=flags;previous_direct_candidates=settings.direct_candidates;previous_direct_reference=settings.direct_reference;
        scene_identity=frame.scene_identity;scene_revision=frame.scene_revision;enabled=true;
        probes(settings,frame.lighting_focus,stats);const bool transform_changed=acceleration(frame);lighting(frame);
        const bool visibility_changed=transform_changed||coverage_changed;
        const std::array<float,4> next_environment{frame.environment[0],frame.environment[1],frame.environment[2],1};
        const bool radiance_changed=light_changed||material_changed||emitter_changed||next_environment!=environment;
        // Budgeted updates retain the authoritative change hint until every probe has been visited.
        if(radiance_changed||transform_changed||shadow_bias_changed)probe_updates_pending=4*probe_field.total;
        if(visibility_changed)probe_geometry_pending=2*probe_field.total;
        auto projection=matrix(frame.camera.projection);const bool taa_enabled=settings.antialiasing==Antialiasing::taa;
        const bool smaa_temporal=settings.antialiasing==Antialiasing::smaa_t2x;
        const float smaa_jitter=sequence%2==0?.25f:-.25f;
        const float jx=taa_enabled?halton(static_cast<std::uint32_t>(sequence%8)+1,2)-.5f:(smaa_temporal?smaa_jitter:0);
        const float jy=taa_enabled?halton(static_cast<std::uint32_t>(sequence%8)+1,3)-.5f:(smaa_temporal?smaa_jitter:0);
        for(std::size_t row=0;row<4;row++){projection.elements[row*4]+=2*jx/frame.scene_viewport.width*projection.elements[row*4+3];
            projection.elements[row*4+1]-=2*jy/frame.scene_viewport.height*projection.elements[row*4+3];}
        const auto vp=matrix(frame.camera.view)*projection;view_projection=floats(vp);inverse_view_projection=floats(*math::try_inverse(vp));
        if(!history_valid){previous_view_projection=view_projection;previous_inverse_view_projection=inverse_view_projection;}
        size_frame={next.width,next.height,static_cast<std::uint32_t>(sequence),history_valid?1u:0u};
        require(gfxCommandClearBuffer(gfx,ray_counters),"Reset Scene Lit ray counters");
        viewport=next_viewport;
        const auto raster=raster_viewport(frame.scene_viewport);
        raster_transform=raster.clip_transform;
        // The validated logical rect lies in this window. Raster and receiver
        // dispatch share one pixel-center range, including fractional edges.
        screen_dispatch={static_cast<std::uint32_t>(raster.scissor.left),static_cast<std::uint32_t>(raster.scissor.top),
            static_cast<std::uint32_t>(raster.scissor.right),static_cast<std::uint32_t>(raster.scissor.bottom)};
        environment=next_environment;
        controls={std::exp2(settings.exposure_ev),settings.shadow_bias,settings.ao_radius,settings.reflection_distance};
        effect_flags=flags;di_settings={settings.direct_candidates,20u,settings.direct_reference?1u:0u,64u};
        di_history_valid=history_valid&&!direct_changed&&!settings.direct_reference?1u:0u;
        // Receiver motion alone cannot describe changing shadows or secondary
        // hits. Keep visibility changes distinct from authored radiance edits:
        // they shorten support without globally clamping stochastic observations.
        temporal={.9f,settings.reflection_roughness,radiance_changed?1.f:0.f,visibility_changed?1.f:0.f};jitter={jx,jy,jitter[0],jitter[1]};
        begin(0);for(std::uint32_t i=0;i<gbuffer_textures.size();i++){require(gfxCommandClearTexture(gfx,gbuffer_textures[i]),"Clear Scene Lit GBuffer");require(gfxCommandBindColorTarget(gfx,i,gbuffer_textures[i]),"Bind Scene Lit GBuffer");}
        require(gfxCommandBindDepthStencilTarget(gfx,depth),"Bind Scene Lit primary depth");common(gbuffer.program(),depth);
        require(gfxCommandBindKernel(gfx,gbuffer.kernel()),"Bind Scene Lit raster pipeline");
        for(const auto& draw:draws){bind(gbuffer.program(),"lit_triangles",draw.triangles);bind(gbuffer.program(),"lit_first_instance",draw.first);
            require(gfxCommandDraw(gfx,draw.vertex_count,draw.count),"Draw Scene Lit surface geometry");stats.scene_draw_calls++;}
        for(std::uint32_t i=0;i<gbuffer_textures.size();i++)require(gfxCommandBindColorTarget(gfx,i,{}),"Unbind Scene Lit GBuffer");
        require(gfxCommandBindDepthStencilTarget(gfx,{}),"Unbind Scene Lit primary depth");end(0);
        const auto dx=(next.width+7)/8,dy=(next.height+7)/8;
        begin(2);begin(10,!settings.direct_reference);
        if(!settings.direct_reference) {
            bind(di_initial.program(),"lit_di_previous",di_reservoirs[1-parity]);bind(di_initial.program(),"lit_di_output",di_scratch);
            dispatch_screen(di_initial,depth);
        }
        end(10);begin(11,!settings.direct_reference);
        if(!settings.direct_reference) {
            bind(di_spatial.program(),"lit_di_input",di_scratch);bind(di_spatial.program(),"lit_di_output",di_reservoirs[parity]);
            dispatch_screen(di_spatial,depth);
        }
        end(11);begin(12);
        bind(direct.program(),"lit_di_input",di_reservoirs[parity]);
        const std::array<const char*,2> raw_outputs{"lit_direct_diffuse_raw_output","lit_direct_specular_raw_output"},
            raw_inputs{"lit_direct_diffuse_raw","lit_direct_specular_raw"},previous_inputs{"lit_direct_diffuse_previous","lit_direct_specular_previous"},
            moments_previous{"lit_direct_diffuse_moments_previous","lit_direct_specular_moments_previous"},
            history_outputs{"lit_direct_diffuse_history_output","lit_direct_specular_history_output"},
            moments_outputs{"lit_direct_diffuse_moments_output","lit_direct_specular_moments_output"},
            filter_inputs{"lit_direct_diffuse_filter_input","lit_direct_specular_filter_input"},
            filter_outputs{"lit_direct_diffuse_filter_output","lit_direct_specular_filter_output"},
            filter_moments{"lit_direct_diffuse_moments","lit_direct_specular_moments"};
        for(std::size_t i=0;i<2;++i)bind(direct.program(),raw_outputs[i],direct_signals[i].raw);
        if(settings.direct_reference) {
            // Keep each expensive diagnostic dispatch bounded. Global pixel
            // coordinates preserve the 64 estimates and RNG across tile edges.
            constexpr std::uint32_t tile=64;
            common(direct.program(),depth);
            require(gfxCommandBindKernel(gfx,direct.kernel()),"Bind direct reference compute");
            for(std::uint32_t y=screen_dispatch[1];y<screen_dispatch[3];y+=tile)for(std::uint32_t x=screen_dispatch[0];x<screen_dispatch[2];x+=tile) {
                const auto right=std::min(x+tile,screen_dispatch[2]),bottom=std::min(y+tile,screen_dispatch[3]);
                bind(direct.program(),"lit_screen_dispatch",std::array<std::uint32_t,4>{x,y,right,bottom});
                require(gfxCommandDispatch(gfx,(right-x+7)/8,(bottom-y+7)/8,1),"Dispatch direct reference tile");
            }
        } else {
            dispatch_screen(direct,depth);
        }
        end(12);begin(13);
        for(std::size_t i=0;i<2;++i){const auto& signal=direct_signals[i];const auto p=direct_resolve.program();
            bind(p,raw_inputs[i],signal.raw);bind(p,previous_inputs[i],signal.history[1-parity]);bind(p,moments_previous[i],signal.moments[1-parity]);
            bind(p,history_outputs[i],signal.history[parity]);bind(p,moments_outputs[i],signal.moments[parity]);}
        dispatch_screen(direct_resolve,depth);
        const bool filter_direct=!settings.direct_reference&&!light_frame.sources.empty();
        if(filter_direct)for(std::uint32_t step=0;step<3;++step){const auto p=direct_filter.program();
            for(std::size_t i=0;i<2;++i){const auto& signal=direct_signals[i];
                bind(p,filter_inputs[i],step==0?signal.history[parity]:signal.scratch[(step-1)%2]);
                bind(p,filter_outputs[i],signal.scratch[step%2]);bind(p,filter_moments[i],signal.moments[parity]);}
            bind(p,"lit_direct_filter_step",1u<<step);dispatch_screen(direct_filter,depth);}
        const std::array<GfxTexture,2> direct_result{
            filter_direct?direct_signals[0].scratch[0]:direct_signals[0].history[parity],
            filter_direct?direct_signals[1].scratch[0]:direct_signals[1].history[parity]};
        end(13);end(2);
        begin(3,settings.ddgi);begin(8,settings.ddgi&&probe_field.total!=0);
        if(settings.ddgi) {
            const auto total=probe_field.total,budget=probe_field.scheduled;
            probe_history={probe_updates_pending?.8f:.93f,probe_geometry_pending?.85f:.95f,
                total?1.f-float(budget)/float(total):0,0};
            probe_controls={settings.probe_bounce,settings.probe_feedback_visibility?1.f:0.f,0,0};
            probe_update_settings={0,budget,settings.probe_rays,probe_field.valid?0u:1u};
            if(total) {
                bind_probes(probe_copy.program(),false);dispatch(probe_copy,depth,total);
            }
            begin(14,total&&budget);
            if(total&&budget) {
                bind_probes(probe_trace.program(),false);bind(probe_trace.program(),"lit_probe_work",probe_field.work);
                bind(probe_trace.program(),"lit_probe_ray_hits_output",probe_ray_hits);
                dispatch(probe_trace,depth,budget,1,1,2);
            }
            end(14);begin(15,total&&budget);
            if(total&&budget) {
                bind_probes(probe_update.program(),false);bind(probe_update.program(),"lit_probe_work",probe_field.work);
                // SRV consumers establish the UAV write/read dependencies in gfx.
                bind(probe_update.program(),"lit_probe_ray_hits",probe_ray_hits);
                bind(probe_update.program(),"lit_probe_offsets_current",probe_offsets[probe_parity]);
                dispatch(probe_update,depth,budget,1,1,2);
            }
            end(15);
            end(8);begin(9);
            bind_probes(probe_resolve.program(),true);bind(probe_resolve.program(),"lit_indirect",indirect);
            dispatch_screen(probe_resolve,depth,3);end(9);
            probe_field.commit(settings);probe_parity=1-probe_parity;
            // Warmup may repeatedly visit new slices. Only global traversal
            // progress proves that remote support received its response rounds.
            probe_updates_pending-=std::min(probe_updates_pending,probe_field.round_robin_progress);
            probe_geometry_pending-=std::min(probe_geometry_pending,probe_field.round_robin_progress);
            ++probe_sequence;
        } else {begin(14,false);end(14);begin(15,false);end(15);end(8);begin(9,false);end(9);}
        end(3);begin(4,settings.gtao||settings.ssil);if(settings.gtao&&!settings.ssil){bind(ao.program(),"lit_ao_output",ao_raw);dispatch_screen(ao,depth);
            bind(ao_resolve.program(),"lit_ao_raw",ao_raw);bind(ao_resolve.program(),"lit_ao_previous",ao_history[1-parity]);
            bind(ao_resolve.program(),"lit_ao_history_output",ao_history[parity]);bind(ao_resolve.program(),"lit_ao_resolved_output",ao_texture);dispatch_screen(ao_resolve,depth);}
        if(settings.ssil) {
            if(!ssil_raw){ssil_raw=texture(next.width,next.height,DXGI_FORMAT_R16G16B16A16_FLOAT,8);ssil_irradiance=texture(next.width,next.height,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
                for(auto& t:ssil_history)t=texture(next.width,next.height,DXGI_FORMAT_R16G16B16A16_FLOAT,8);history_invalid|=16u;}
            for(const auto* pipeline:{&ssil_trace,&ssil_resolve}){const auto p=pipeline->program();bind(p,"lit_direct_diffuse",direct_result[0]);
                bind(p,"lit_indirect_source",indirect);bind(p,"lit_ssil_controls",std::array<float,4>{1.2f,.3f,0,0});
                bind(p,"lit_ssil_raw_output",ssil_raw);bind(p,"lit_ssil_raw",ssil_raw);bind(p,"lit_ssil_previous",ssil_history[1-parity]);
                bind(p,"lit_ssil_history_output",ssil_history[parity]);bind(p,"lit_ssil_irradiance_output",ssil_irradiance);}
            dispatch_screen(ssil_trace,depth);dispatch_screen(ssil_resolve,depth);
        }
        end(4);
        begin(5);for(std::size_t i=0;i<2;++i)bind(compose.program(),filter_inputs[i],direct_result[i]);
        bind(compose.program(),"lit_indirect_source",settings.ssil?ssil_irradiance:indirect);bind(compose.program(),"lit_ao",ao_texture);
        bind(compose.program(),"lit_indirect_gain",settings.indirect_gain);
        bind(compose.program(),"lit_hdr_output",hdr);dispatch(compose,depth,dx,dy);
        if(!brdf_ready){bind(brdf.program(),"lit_brdf_output",brdf_lut);dispatch(brdf,depth,8,8);brdf_ready=true;}
        if(settings.sssr)for(std::uint32_t i=0;i<pyramid_count;i++) {
            // Specialize the first reduction so even a 1x1 hierarchy has no
            // unused SRV alias of its output UAV in the reflected bindings.
            const auto& reduce=i==0?depth_first:depth_pyramid;
            if(i)bind(reduce.program(),"lit_depth_input",pyramid[i-1]);
            bind(reduce.program(),"lit_depth_output",pyramid[i]);const std::array<std::uint32_t,2> size{pyramid[i].getWidth(),pyramid[i].getHeight()};
            bind(reduce.program(),"lit_depth_output_size",size);dispatch(reduce,depth,(size[0]+7)/8,(size[1]+7)/8);
        }
        auto pyramid_array=pyramid;for(std::uint32_t i=pyramid_count;i<12;i++)pyramid_array[i]=pyramid[pyramid_count-1];
        for(const auto* pipeline:{&ssr_reset,&ssr_classify,&ssr_prepare,&ssr_trace}) {
            const auto p=pipeline->program();bind(p,"lit_reflection_work",reflection_work);bind(p,"lit_reflection_counter",reflection_counter);bind(p,"lit_reflection_arguments",reflection_arguments);
            bind(p,"lit_reflection_output",reflection_trace);bind(p,"lit_reflection_hit_trace_output",reflection_hit_trace);
            require(gfxProgramSetParameter(gfx,p,"lit_depth_pyramid",pyramid_array.data(),12),"Bind separate depth hierarchy");bind(p,"lit_depth_level_count",next.width==1&&next.height==1?1u:pyramid_count+1);
        }
        if(settings.sssr)dispatch(ssr_reset,depth,1);
        dispatch(ssr_classify,depth,dx,dy);
        if(settings.sssr)dispatch(ssr_prepare,depth,1);
        if(settings.sssr){common(ssr_trace.program(),depth);require(gfxCommandBindKernel(gfx,ssr_trace.kernel()),"Bind SSSR work kernel");require(gfxCommandDispatchIndirect(gfx,reflection_arguments),"Dispatch SSSR classified work");}
        bind(reflection_resolve.program(),"lit_reflection_trace",reflection_trace);bind(reflection_resolve.program(),"lit_reflection_hit_trace",reflection_hit_trace);
        bind(reflection_resolve.program(),"lit_reflection_previous",reflection_history[1-parity]);bind(reflection_resolve.program(),"lit_reflection_hit_previous",reflection_hit_history[1-parity]);
        bind(reflection_resolve.program(),"lit_reflection_resolved_output",reflection_history[parity]);bind(reflection_resolve.program(),"lit_reflection_hit_output",reflection_hit_history[parity]);
        bind(reflection_resolve.program(),"lit_reflection_moments_previous",reflection_moments[1-parity]);bind(reflection_resolve.program(),"lit_reflection_moments_output",reflection_moments[parity]);
        dispatch(reflection_resolve,depth,dx,dy);
        if(settings.sssr)for(std::uint32_t step=0;step<3;++step){const auto p=reflection_filter.program();
            bind(p,"lit_reflection_filter_input",step==0?reflection_history[parity]:reflection_scratch[(step-1)%2]);
            bind(p,"lit_reflection_filter_output",reflection_scratch[step%2]);bind(p,"lit_reflection_moments",reflection_moments[parity]);
            bind(p,"lit_reflection_hit_trace",reflection_hit_trace);bind(p,"lit_reflection_filter_step",1u<<step);dispatch(reflection_filter,depth,dx,dy);}
        end(5);
        begin(6);bind(taa.program(),"lit_reflection",settings.sssr?reflection_scratch[0]:reflection_history[parity]);bind(taa.program(),"lit_reflection_moments",reflection_moments[parity]);bind(taa.program(),"lit_taa_previous",taa_history[1-parity]);bind(taa.program(),"lit_taa_output",taa_history[parity]);
        bind(taa.program(),"lit_history_depth_output",history_depth[parity]);bind(taa.program(),"lit_history_normal_output",history_normal[parity]);bind(taa.program(),"lit_history_geometry_normal_output",history_geometry_normal[parity]);
        bind(taa.program(),"lit_history_base_metallic_output",history_base_metallic[parity]);bind(taa.program(),"lit_history_surface_id_output",history_surface_id[parity]);dispatch(taa,depth,dx,dy);end(6);
        begin(7);const std::array<float,4> bloom_controls{1,.5f,settings.bloom?.04f:0.f,64};
        if(settings.bloom) {
        bind(bloom_downsample.program(),"lit_taa",taa_history[parity]);bind(bloom_downsample.program(),"lit_bloom_output",bloom[0]);bind(bloom_downsample.program(),"lit_bloom_controls",bloom_controls);
        dispatch(bloom_downsample,depth,(bloom[0].getWidth()+7)/8,(bloom[0].getHeight()+7)/8);
        for(std::uint32_t i=0;i<2;i++){bind(bloom_blur.program(),"lit_bloom_input",bloom[i]);bind(bloom_blur.program(),"lit_bloom_output",bloom[1-i]);
            bind(bloom_blur.program(),"lit_bloom_direction",std::array<std::int32_t,2>{i==0?1:0,i==0?0:1});dispatch(bloom_blur,depth,(bloom[0].getWidth()+7)/8,(bloom[0].getHeight()+7)/8);}
        }
        bind(tone_map.program(),"lit_taa",taa_history[parity]);bind(tone_map.program(),"lit_bloom",bloom[0]);bind(tone_map.program(),"lit_bloom_controls",bloom_controls);bind(tone_map.program(),"lit_tone_output",tone_color);dispatch(tone_map,depth,dx,dy);
        GfxTexture display=tone_color;
        if(settings.antialiasing==Antialiasing::smaa||smaa_temporal) {
            const float phase=sequence%2==0?1.f:2.f;
            const std::array<float,4> subsamples=smaa_temporal?std::array<float,4>{phase,phase,phase,0}:std::array<float,4>{};
            for(const auto* pipeline:{&smaa_edges_pass,&smaa_weights_pass,&smaa_neighborhood,&smaa_resolve}){const auto p=pipeline->program();
                bind(p,"lit_smaa_color",tone_color);bind(p,"lit_smaa_edges",smaa_edges);bind(p,"lit_smaa_edges_output",smaa_edges);
                bind(p,"lit_smaa_weights",smaa_weights);bind(p,"lit_smaa_weights_output",smaa_weights);
                bind(p,"lit_smaa_area",smaa_area);bind(p,"lit_smaa_search",smaa_search);bind(p,"lit_smaa_subsample_indices",subsamples);
                bind(p,"lit_smaa_spatial_output",smaa_spatial[parity]);bind(p,"lit_smaa_spatial",smaa_spatial[parity]);bind(p,"lit_smaa_previous",smaa_spatial[1-parity]);
                bind(p,"lit_smaa_output",smaa_output);bind(p,"lit_smaa_temporal_enabled",smaa_temporal?1u:0u);}
            dispatch(smaa_edges_pass,depth,dx,dy);dispatch(smaa_weights_pass,depth,dx,dy);dispatch(smaa_neighborhood,depth,dx,dy);
            display=smaa_spatial[parity];if(smaa_temporal){dispatch(smaa_resolve,depth,dx,dy);display=smaa_output;}
        }
        common(tone.program(),depth);bind(tone.program(),"lit_display_source",display);
        require(gfxCommandBindKernel(gfx,tone.kernel()),"Bind Scene Lit tone map");require(gfxCommandDraw(gfx,3),"Compose Scene Lit display");stats.viewport_draw_calls++;end(7);
        if(settings.probe_debug) draw_coverage(depth,settings,stats);
        stats.scene_lit_light_sources=static_cast<std::uint32_t>(light_frame.sources.size());
        stats.scene_lit_shadow_sources=settings.shadows?static_cast<std::uint32_t>(std::count_if(light_frame.sources.begin(),light_frame.sources.end(),[](const GpuLightSource& l){return (l.meta[1]&2u)!=0;})):0;
        stats.scene_lit_di_candidates=settings.direct_candidates;
        const auto readback_slot=sequence%readback_count;
        require(gfxCommandCopyBuffer(gfx,ray_readback[readback_slot],ray_counters),"Publish Scene Lit ray counters asynchronously");
        readback_sequences[readback_slot]=gfxGetRecordingFrameSequence(gfx);readback_frame_ids[readback_slot]=frame.ui_frame.id;
        readback_pass_masks[readback_slot]=encoded_pass_mask;
        stats.scene_lit_history_resets=reset_events;reset_events=0;previous_view_projection=view_projection;previous_inverse_view_projection=inverse_view_projection;history_valid=true;sequence++;parity=1-parity;
    }
    void prepare_probe_debug(rhi::Pipeline& pipeline,bool geometry) {
        if(pipeline)return;
        const std::string source=std::string(shaders::lit_ddgi_debug);
        const std::string vs=source+"\nDDGIDebugVertex main(uint v:SV_VertexID,uint i:SV_InstanceID){return ddgi_debug_vs(v,i);}";
        const std::string ps=source+(geometry?"\nfloat4 main(DDGIDebugVertex i):SV_Target{return ddgi_debug_ps(i);}":
            "\nfloat4 main(float4 p:SV_Position):SV_Target{return ddgi_debug_coverage_ps(p);}");
        rhi::PipelineDesc desc;desc.kind=rhi::PipelineKind::vertex;desc.source.vs=geometry?vs.c_str():shaders::viewport_vs;
        desc.source.ps=ps.c_str();desc.name=geometry?"DDGI spatial diagnostics":"DDGI receiver coverage";
        require(gfxDrawStateSetColorTarget(desc.draw_state,0,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),"Set DDGI debug color format");
        require(gfxDrawStateSetCullMode(desc.draw_state,D3D12_CULL_MODE_NONE),"Set DDGI debug culling");
        if(geometry) {
            require(gfxDrawStateSetDepthStencilTarget(desc.draw_state,DXGI_FORMAT_D32_FLOAT),"Set DDGI debug depth format");
            require(gfxDrawStateSetDepthFunction(desc.draw_state,D3D12_COMPARISON_FUNC_GREATER_EQUAL),"Set DDGI debug reversed depth");
        } else require(gfxDrawStateSetDepthWriteMask(desc.draw_state,D3D12_DEPTH_WRITE_MASK_ZERO),"Disable diagnostic surface depth writes");
        auto result=context.prepare(desc);if(!result)throw Error{result.error(),true};pipeline=std::move(*result);
    }
    void bind_probe_debug(GfxProgram p) {
        const auto read=1-probe_parity;
        bind(p,"lit_probe_irradiance_previous",probe_irradiance[read]);bind(p,"lit_probe_distance_previous",probe_distance[read]);
        bind(p,"lit_probe_offsets_previous",probe_offsets[read]);bind(p,"lit_probe_age",probe_age);
        bind(p,"lit_probe_volumes",probe_field.current);
        bind(p,"lit_probe_pool",std::array<std::uint32_t,4>{probe_field.width,probe_field.total,0,probe_field.valid?1u:0u});
    }
    void draw_coverage(GfxTexture depth,const SceneLitSettings& settings,FrameStats& stats) {
        prepare_probe_debug(probe_debug_surface,false);
        const auto p=probe_debug_surface.program();common(p,depth);bind_probe_debug(p);
        bind(p,"lit_probe_debug_options",std::array<std::uint32_t,4>{(effect_flags&2u)&&probe_field.valid?1u:0u,
            static_cast<std::uint32_t>(settings.probe_coverage),0,0});
        require(gfxCommandBindKernel(gfx,probe_debug_surface.kernel()),"Bind DDGI receiver diagnostic");
        require(gfxCommandDraw(gfx,3),"Draw DDGI receiver diagnostic");++stats.viewport_draw_calls;
    }
    void draw_debug(const Frame& frame,ui::PixelExtent size,GfxTexture depth,FrameStats& stats) {
        const auto& settings=frame.view.lit;
        if(frame.view.shading!=Shading::scene_lit||frame.view.wireframe==Wireframe::only||!settings.probe_overlay()) {retire(probe_debug_depth);stats.scene_lit_resource_bytes=resource_bytes();return;}
        prepare_probe_debug(probe_debug_geometry,true);
        if(probe_debug_depth.getWidth()!=size.width||probe_debug_depth.getHeight()!=size.height) {
            retire(probe_debug_depth);constexpr float clear[4]{};
            probe_debug_depth=gfxCreateTexture2D(gfx,size.width,size.height,DXGI_FORMAT_D32_FLOAT,1,clear);allocation(bool(probe_debug_depth));
        }
        require(gfxCommandBindDepthStencilTarget(gfx,{}),"Unbind scene depth for DDGI debug copy");
        if(settings.probe_xray||frame.meshes.empty())require(gfxCommandClearTexture(gfx,probe_debug_depth),"Clear DDGI X-Ray depth");
        else require(gfxCommandCopyTexture(gfx,probe_debug_depth,depth),"Copy scene depth for DDGI diagnostics");
        require(gfxCommandBindDepthStencilTarget(gfx,probe_debug_depth),"Bind private DDGI diagnostic depth");
        ProbeField preview;preview.describe(settings,frame.lighting_focus);
        const auto p=probe_debug_geometry.program();
        const bool ready=enabled&&settings.ddgi&&probe_field.valid&&scene_identity==frame.scene_identity&&probe_field.committed==settings.probe_volumes;
        const auto& v=frame.scene_viewport;
        bind(p,"raster_viewport_transform",raster_viewport(v).clip_transform);
        bind(p,"lit_view_projection",floats(matrix(frame.camera.view)*matrix(frame.camera.projection)));
        bind(p,"lit_viewport",std::array<float,4>{v.x,v.y,v.width,v.height});
        bind_probe_debug(p);bind(p,"lit_probe_volumes",preview.current);
        bind(p,"lit_probe_pool",std::array<std::uint32_t,4>{preview.width,preview.total,0,ready?1u:0u});
        const auto budget=std::min(preview.total,settings.probe_update_budget);
        bind(p,"lit_probe_debug_controls",std::array<float,4>{settings.probe_radius,std::exp2(settings.exposure_ev),
            budget?float((preview.total+budget-1)/budget-1):0,0});
        require(gfxCommandBindKernel(gfx,probe_debug_geometry.kernel()),"Bind DDGI spatial diagnostics");
        for(std::uint32_t slot=0;slot<probe_volume_limit;++slot) {
            const auto total=ProbeField::count(preview.current[slot]);
            if(!total || (settings.probe_debug_volume>=0 && std::uint32_t(settings.probe_debug_volume)!=slot))continue;
            const auto draw=[&](std::uint32_t kind,std::uint32_t vertices,std::uint32_t count) {
                bind(p,"lit_probe_debug_options",std::array<std::uint32_t,4>{ready?1u:0u,static_cast<std::uint32_t>(settings.probe_visualization),kind,slot});
                require(gfxCommandDraw(gfx,vertices,count),"Draw DDGI diagnostic geometry");++stats.viewport_draw_calls;
            };
            if(settings.probe_visualization!=ProbeVisualization::off)draw(0,12*8*6,total);
            if(settings.probe_relocation)draw(2,6,total);
            if(settings.probe_volume)draw(1,6,12);
        }
        require(gfxCommandBindDepthStencilTarget(gfx,depth),"Restore scene editing depth");
        stats.scene_lit_resource_bytes=resource_bytes();
    }
    void retire_scene() {
        for(auto& [key,o]:objects){(void)key;if(tlas)require(gfxTopLevelAccelerationStructureRemoveInstance(gfx,tlas,o.instance),"Detach Scene Lit instance");
            require(gfxDestroyTopLevelAccelerationStructureInstance(gfx,o.instance),"Retire Scene Lit instance");}objects.clear();
        if(tlas)require(gfxDestroyTopLevelAccelerationStructure(gfx,std::exchange(tlas,{})),"Retire Scene Lit TLAS");
        for(auto& [key,g]:geometries){(void)key;require(gfxDestroyBottomLevelAccelerationStructure(gfx,g.blas),"Retire Scene Lit BLAS");require(gfxDestroyGeometry(gfx,g.geometry),"Retire Scene Lit geometry");retire(g.positions);}geometries.clear();
        for(auto* b:{&gpu_instances,&gpu_sources,&gpu_previous_sources,&gpu_proposals,&gpu_current_to_previous,&gpu_previous_to_current,&rt_triangles,&rt_closed_slots,&probe_ray_hits})retire(*b);
        std::vector<GpuInstance>{}.swap(instances);light_frame.clear();
        std::vector<GpuRayTriangle>{}.swap(packed_triangles);std::vector<std::uint32_t>{}.swap(packed_closed_slots);
        object_keys.clear();previous_object_keys.clear();
        tlas_built=false;
    }
    void close() {
        retire_targets();for(auto* pair:{&probe_irradiance,&probe_distance,&probe_offsets})for(auto& t:*pair)retire(t);retire(probe_target);retire(brdf_lut);retire(smaa_area);retire(smaa_search);retire(probe_age);retire(probe_debug_depth);
        retire_scene();retire(ray_counters);
        for(auto& readback:ray_readback)retire(readback);
        for(auto& q:queries)if(q)require(gfxDestroyTimestampQuery(gfx,std::exchange(q,{})),"Retire Scene Lit timestamp");
        if(sampler)require(gfxDestroySamplerState(gfx,std::exchange(sampler,{})),"Retire Scene Lit sampler");
        for(auto* p:{&gbuffer,&di_initial,&di_spatial,&direct,&direct_resolve,&direct_filter,&compose,&probe_copy,&probe_trace,&probe_update,&probe_resolve,
                    &ao,&ao_resolve,&depth_first,&depth_pyramid,&ssr_reset,&ssr_classify,&ssr_prepare,&ssr_trace,&reflection_resolve,&reflection_filter,&brdf,&taa,
                    &bloom_downsample,&bloom_blur,&tone_map,&tone,&smaa_edges_pass,&smaa_weights_pass,&smaa_neighborhood,&smaa_resolve,&ssil_trace,&ssil_resolve,&probe_debug_surface,&probe_debug_geometry})*p={};
    }
};

SceneLit::SceneLit(rhi::Context& context):state_(std::make_unique<State>(context)){}
SceneLit::~SceneLit(){if(state_){const auto result=close();if(!result){std::fprintf(stderr,"Scene Lit cleanup failed: %s\n",result.error().cause.message.c_str());std::abort();}}}
void SceneLit::validate(const Frame& frame) const {
    invalid(state_->context.info().inline_ray_query,"Scene Lit requires hardware DXR 1.1 inline ray tracing");
    const auto view_inverse=math::try_inverse(matrix(frame.camera.view));
    const auto vp=matrix(frame.camera.view)*matrix(frame.camera.projection);const auto vp_inverse=math::try_inverse(vp);
    invalid(bool(view_inverse)&&bool(vp_inverse),"Scene Lit requires an invertible camera");
    for(const auto* value:{&vp,&*vp_inverse,&*view_inverse})for(double element:value->elements)
        invalid(std::isfinite(element)&&std::abs(element)<=std::numeric_limits<float>::max(),"Scene Lit camera exceeds GPU float representation");
    const auto& s=frame.view.lit;invalid(frame.lights.size()<no_light_source&&s.direct_candidates>=1&&s.direct_candidates<=32,"Invalid Scene Lit light count or candidate budget");
    const auto finite=[](float v){return std::isfinite(v);};
    invalid(finite(s.exposure_ev)&&s.exposure_ev>=-16&&s.exposure_ev<=16&&finite(s.shadow_bias)&&s.shadow_bias>0&&s.shadow_bias<=.1f&&finite(s.ao_radius)&&s.ao_radius>0&&
        finite(s.reflection_distance)&&s.reflection_distance>0&&finite(s.reflection_roughness)&&s.reflection_roughness>=0&&s.reflection_roughness<=1&&s.probe_update_budget>0&&s.probe_rays>=1&&s.probe_rays<=256&&s.antialiasing<=Antialiasing::smaa_t2x&&s.probe_visualization<=ProbeVisualization::distance&&finite(s.probe_radius)&&s.probe_radius>=.01f&&s.probe_radius<=.45f,"Invalid Scene Lit settings");
    for(float radiance:frame.environment)invalid(finite(radiance)&&radiance>=0&&radiance<=65504,"Invalid Scene Lit sky radiance");
    if(const auto* error=probe_focus_error(s,frame.lighting_focus))invalid(false,error);
    std::unordered_set<std::uint64_t> mesh_keys,light_keys;
    for(const auto& mesh:frame.meshes)if(mesh.stable_key)
        invalid(mesh_keys.insert(mesh.stable_key).second,"Duplicate stable mesh identity");
    for(const auto& l:frame.lights){invalid(l.kind<=LightKind::directional&&finite(l.intensity)&&l.intensity>=0&&finite(l.range)&&l.range>=0&&finite(l.source_radius)&&l.source_radius>=0&&finite(l.width)&&l.width>=0&&finite(l.height)&&l.height>=0,"Invalid Scene Lit light");
        invalid(l.kind==LightKind::directional||l.range>0,"Local light range must be positive");
        invalid(l.kind!=LightKind::rectangle||(l.width>0&&l.height>0),"Rectangle dimensions must be positive");
        float direction_length=0,right_length=0,up_length=0,orthogonal=0;for(std::size_t i=0;i<3;i++){invalid(finite(l.position[i])&&finite(l.direction[i])&&finite(l.color[i])&&l.color[i]>=0&&finite(l.right[i])&&finite(l.up[i]),"Invalid Scene Lit light vectors");direction_length+=l.direction[i]*l.direction[i];right_length+=l.right[i]*l.right[i];up_length+=l.up[i]*l.up[i];orthogonal+=l.right[i]*l.up[i];}
        invalid(std::abs(direction_length-1)<.002f,"Light propagation direction must be unit length");
        if(l.kind==LightKind::rectangle) {
            float right_dot=0,up_dot=0;for(std::size_t i=0;i<3;i++){right_dot+=l.right[i]*l.direction[i];up_dot+=l.up[i]*l.direction[i];}
            invalid(std::abs(right_length-1)<.002f&&std::abs(up_length-1)<.002f&&std::abs(orthogonal)<.002f&&std::abs(right_dot)<.002f&&std::abs(up_dot)<.002f,"Rectangle light tangent axes and propagation direction must be orthonormal");
        }
        if(const auto* message=rendering::analytic_light_range_error(l.color,l.intensity,
            l.kind==LightKind::rectangle?double(l.width)*l.height:1))invalid(false,message);
        if(l.kind==LightKind::directional)invalid(double(l.source_radius)<std::numbers::pi_v<double>/2,"Directional source angular radius is too large");if(l.stable_key)invalid(light_keys.insert(l.stable_key).second,"Duplicate stable light identity");}
}
void SceneLit::draw(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,std::span<const SceneLitDraw> draws,const MaterialFrame& materials,FrameStats& stats){state_->draw(frame,extent,depth,draws,materials,stats);}
void SceneLit::draw_debug(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,FrameStats& stats){state_->draw_debug(frame,extent,depth,stats);}
void SceneLit::invalidate(bool empty_scene,FrameStats& stats) {
    state_->enabled=false;state_->reset();if(!empty_scene)state_->retire(state_->probe_debug_depth);if(empty_scene)state_->retire_scene();
    stats.scene_lit_resource_bytes=state_->resource_bytes();
}
Result<void> SceneLit::close(){if(!state_)return {};try{state_->close();state_.reset();return {};}catch(const Error& error){return std::unexpected(error);}}
}

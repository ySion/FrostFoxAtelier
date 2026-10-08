#include "scene_lit.hpp"
#include "shader_source.hpp"
#include "smaa_lut.hpp"
#include "math/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
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
    std::array<float,4> normal_x,normal_y,normal_z,base_roughness,emissive_metallic;
    std::array<std::uint32_t,4> geometry;
};
struct GpuLight {
    std::array<float,4> position_range,direction_radius,color_intensity,right_width,up_height;
    std::array<std::uint32_t,4> meta;
    bool operator==(const GpuLight&) const=default;
};
static_assert(sizeof(GpuInstance)==224 && sizeof(GpuLight)==96);
std::uint32_t key_hash(std::uint64_t key) {return static_cast<std::uint32_t>(key^(key>>32));}
}

struct SceneLit::State {
    rhi::Context& context;
    GfxContext gfx;
    rhi::Pipeline gbuffer,tiles,direct,direct_resolve,direct_filter,direct_combine,compose;
    rhi::Pipeline probe_copy,probe_update,probe_resolve,ao,ao_resolve,depth_pyramid,ssr_reset,ssr_classify,ssr_prepare,ssr_trace;
    rhi::Pipeline reflection_resolve,reflection_filter,brdf,taa,bloom_downsample,bloom_blur,tone_map,tone;
    rhi::Pipeline smaa_edges_pass,smaa_weights_pass,smaa_neighborhood,smaa_resolve;
    rhi::Pipeline ssil_trace,ssil_resolve;
    std::array<GfxTimestampQuery,8> queries{};
    GfxSamplerState sampler;
    ui::PixelExtent extent{};
    std::array<GfxTexture,5> gbuffer_textures{};
    GfxTexture direct_hdr,hdr,indirect,ao_texture,ao_raw,reflection_trace,reflection_hit_trace,brdf_lut;
    struct DirectSignal { GfxTexture raw; std::array<GfxTexture,2> history{},moments{},scratch{}; };
    std::array<DirectSignal,2> direct_signals{}; // Diffuse/specular radiance with separate reconstruction support.
    std::array<GfxTexture,2> history_depth{},history_normal{},taa_history{},reflection_history{},reflection_hit_history{},reflection_moments{},reflection_scratch{},ao_history{},bloom{};
    GfxTexture tone_color,smaa_edges,smaa_weights,smaa_output,smaa_area,smaa_search;
    std::array<GfxTexture,2> smaa_spatial{};
    GfxTexture ssil_raw,ssil_irradiance;
    std::array<GfxTexture,2> ssil_history{};
    std::array<GfxTexture,12> pyramid{};
    std::uint32_t pyramid_count{};
    std::array<GfxTexture,2> probe_irradiance{},probe_target{},probe_distance{},probe_offsets{};
    std::array<std::uint32_t,3> probe_counts{};
    std::array<float,3> probe_min{},probe_max{};
    GfxBuffer gpu_instances,gpu_lights,rt_vertices,tile_counts,tile_indices,reflection_work,reflection_counter,reflection_arguments;
    GfxBuffer ray_counters;
    static constexpr std::size_t readback_count=32;
    std::array<GfxBuffer,readback_count> ray_readback{};
    std::array<std::uint64_t,readback_count> readback_sequences{},readback_frame_ids{};
    GfxTopLevelAccelerationStructure tlas;
    struct Geometry {
        geometry::MeshRef source;
        GfxBuffer positions;
        GfxGeometry geometry;
        GfxBottomLevelAccelerationStructure blas;
        std::uint32_t offset{};
        bool used{};
    };
    struct Object {
        GfxTopLevelAccelerationStructureInstance instance;
        const geometry::Mesh* geometry{};
        std::array<float,16> world{};
        std::array<float,4> base_roughness{},emissive_metallic{};
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
    std::vector<GpuLight> lights;
    std::vector<GpuLight> previous_lights;
    std::vector<std::uint32_t> shadow_lights;
    std::vector<geometry::Vertex> packed_vertices;
    std::vector<ObjectKey> object_keys,previous_object_keys;
    std::vector<std::uint64_t> light_keys,previous_light_keys;
    std::array<float,16> view_projection{},inverse_view_projection{},previous_view_projection{};
    std::array<float,4> camera{},viewport{},environment{},controls{},temporal{},jitter{},probe_origin{},probe_spacing{},probe_history{};
    std::array<std::uint32_t,4> size_frame{},counts{},probe_grid{},probe_update_settings{};
    std::uint64_t scene_identity{},scene_revision{},reset_events{},sequence{};
    std::uint32_t parity{},probe_parity{},probe_cursor{},probe_updates_pending{},probe_geometry_pending{},probe_sequence{};
    std::uint32_t previous_effect_flags{},previous_shadow_samples{},previous_shadow_budget{};
    std::uint32_t history_invalid{};
    bool previous_probe_debug{};
    Antialiasing previous_antialiasing{Antialiasing::none};
    bool history_valid{},enabled{},probe_valid{},brdf_ready{},tlas_built{};
    bool light_changed{},material_changed{};
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
        ray_counters=gfxCreateBuffer<std::uint32_t>(gfx,4);allocation(bool(ray_counters));
        for(auto& readback:ray_readback){readback=gfxCreateBuffer(gfx,16,nullptr,kGfxCpuAccess_Read);allocation(bool(readback));}
        const std::string vs=std::string(shaders::lit_gbuffer)+"\nLitRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_gbuffer_vs(v,i);}";
        const std::string ps=std::string(shaders::lit_gbuffer)+"\nLitGBuffer main(LitRaster i){return lit_gbuffer_ps(i);}";
        rhi::PipelineDesc desc;desc.kind=rhi::PipelineKind::vertex;desc.source.vs=vs.c_str();desc.source.ps=ps.c_str();desc.name="Scene Lit GBuffer";
        for(std::uint32_t i=0;i<4;i++)require(gfxDrawStateSetColorTarget(desc.draw_state,i,DXGI_FORMAT_R16G16B16A16_FLOAT),"Set GBuffer format");
        require(gfxDrawStateSetColorTarget(desc.draw_state,4,DXGI_FORMAT_R32_FLOAT),"Set previous surface depth format");
        require(gfxDrawStateSetDepthStencilTarget(desc.draw_state,DXGI_FORMAT_D32_FLOAT),"Set GBuffer depth");
        require(gfxDrawStateSetDepthFunction(desc.draw_state,D3D12_COMPARISON_FUNC_GREATER),"Set GBuffer reversed depth");
        require(gfxDrawStateSetCullMode(desc.draw_state,D3D12_CULL_MODE_NONE),"Set GBuffer culling");
        auto result=context.prepare(desc);if(!result)throw Error{result.error(),true};gbuffer=std::move(*result);
        prepare(tiles,shaders::lit_direct,"lit_tiles_cs","Scene Lit light tiles");
        prepare(direct,shaders::lit_direct,"lit_direct_cs","Scene Lit direct PBR");
        prepare(direct_resolve,shaders::lit_direct,"lit_direct_resolve_cs","Direct radiance temporal reconstruction");
        prepare(direct_filter,shaders::lit_direct,"lit_direct_filter_cs","Direct radiance spatial reconstruction");
        prepare(direct_combine,shaders::lit_direct,"lit_direct_combine_cs","Direct radiance material resolve");
        prepare(compose,shaders::lit_direct,"lit_compose_cs","Scene Lit HDR composition");
        prepare(probe_copy,shaders::lit_ddgi,"DDGICopy","DDGI history transfer");
        prepare(probe_update,shaders::lit_ddgi,"DDGIUpdate","DDGI RT probe update");
        prepare(probe_resolve,shaders::lit_ddgi,"DDGIResolve","DDGI irradiance gather");
        prepare(ao,shaders::lit_post,"lit_gtao_cs","Scene Lit GTAO");
        prepare(ao_resolve,shaders::lit_post,"lit_gtao_resolve_cs","GTAO signal reconstruction");
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
        for(auto* t:{&direct_hdr,&hdr,&indirect,&ao_texture,&ao_raw,&reflection_trace,&reflection_hit_trace,&tone_color,&smaa_edges,&smaa_weights,&smaa_output,&ssil_raw,&ssil_irradiance})retire(*t);
        for(auto* pair:{&history_depth,&history_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_moments,&reflection_scratch,&ao_history,&bloom,&smaa_spatial,&ssil_history})for(auto& t:*pair)retire(t);
        for(auto& signal:direct_signals){retire(signal.raw);for(auto* pair:{&signal.history,&signal.moments,&signal.scratch})for(auto& t:*pair)retire(t);}
        for(auto& t:pyramid)retire(t);
        for(auto* b:{&tile_counts,&tile_indices,&reflection_work,&reflection_counter,&reflection_arguments})retire(*b);
    }
    void targets(ui::PixelExtent next) {
        if(extent==next)return;
        retire_targets();extent=next;
        const auto w=next.width,h=next.height;
        for(std::size_t i=0;i<gbuffer_textures.size();++i)gbuffer_textures[i]=texture(w,h,i==4?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT,i==4?4:8);
        for(auto* t:{&direct_hdr,&hdr,&indirect,&reflection_trace,&reflection_hit_trace,&tone_color,&smaa_output})*t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        ao_texture=texture(w,h,DXGI_FORMAT_R16_FLOAT,2);
        ao_raw=texture(w,h,DXGI_FORMAT_R16_FLOAT,2);
        for(auto* pair:{&history_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_scratch,&smaa_spatial})for(auto& t:*pair)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        for(auto& t:reflection_moments)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        for(auto& t:ao_history)t=texture(w,h,DXGI_FORMAT_R16G16_FLOAT,4);
        smaa_edges=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4);smaa_weights=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4);
        for(auto& signal:direct_signals) {
            signal.raw=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
            for(auto* pair:{&signal.history,&signal.scratch})for(auto& t:*pair)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
            for(auto& t:signal.moments)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        }
        for(auto& t:history_depth)t=texture(w,h,DXGI_FORMAT_R32_FLOAT,4);
        for(auto& t:bloom)t=texture((w+3)/4,(h+3)/4,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        std::uint32_t pw=w,ph=h;pyramid_count=0;
        while(pyramid_count<12){pyramid[pyramid_count++]=texture(pw,ph,DXGI_FORMAT_R32G32_FLOAT,8);if(pw==1&&ph==1)break;pw=(pw+1)/2;ph=(ph+1)/2;}
        const auto tile_total=((w+15)/16)*((h+15)/16);
        tile_counts=gfxCreateBuffer<std::uint32_t>(gfx,tile_total);tile_indices=gfxCreateBuffer<std::uint32_t>(gfx,tile_total*64);
        reflection_work=gfxCreateBuffer<std::uint32_t>(gfx,w*h);reflection_counter=gfxCreateBuffer<std::uint32_t>(gfx,1);
        reflection_arguments=gfxCreateBuffer<std::uint32_t>(gfx,3);
        allocation(bool(tile_counts)&&bool(tile_indices)&&bool(reflection_work)&&bool(reflection_counter)&&bool(reflection_arguments));
        history_valid=false;reset_events++;
    }
    void probes(const SceneLitSettings& settings) {
        if(probe_counts==settings.probe_counts&&probe_min==settings.probe_min&&probe_max==settings.probe_max)return;
        for(auto* pair:{&probe_irradiance,&probe_target,&probe_distance,&probe_offsets})for(auto& t:*pair)retire(t);
        probe_counts=settings.probe_counts;probe_min=settings.probe_min;probe_max=settings.probe_max;
        const auto w=probe_counts[0]*8,h=probe_counts[1]*probe_counts[2]*8;
        for(auto& t:probe_irradiance)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        for(auto& t:probe_target)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        for(auto& t:probe_distance)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        for(auto& t:probe_offsets)t=texture(probe_counts[0],probe_counts[1]*probe_counts[2],DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        probe_valid=false;probe_parity=0;probe_cursor=0;probe_updates_pending=0;probe_geometry_pending=0;probe_sequence=0;
    }
    void common(GfxProgram p,GfxTexture depth) {
        bind(p,"lit_size_frame",size_frame);bind(p,"lit_viewport",viewport);bind(p,"lit_view_projection",view_projection);
        bind(p,"lit_inverse_view_projection",inverse_view_projection);bind(p,"lit_previous_view_projection",previous_view_projection);
        bind(p,"lit_camera_position",camera);bind(p,"lit_environment",environment);bind(p,"lit_controls",controls);
        bind(p,"lit_temporal",temporal);bind(p,"lit_jitter",jitter);bind(p,"lit_counts",counts);
        bind(p,"lit_history_invalid",history_invalid);
        bind(p,"lit_instances",gpu_instances);bind(p,"lit_lights",gpu_lights);bind(p,"lit_rt_vertices",rt_vertices);
        bind(p,"lit_scene",tlas);bind(p,"lit_depth",depth);bind(p,"lit_normal_roughness",gbuffer_textures[0]);
        bind(p,"lit_ray_counters",ray_counters);
        bind(p,"lit_base_metallic",gbuffer_textures[1]);bind(p,"lit_emissive",gbuffer_textures[2]);bind(p,"lit_motion",gbuffer_textures[3]);
        bind(p,"lit_previous_depth",gbuffer_textures[4]);
        bind(p,"lit_linear_sampler",sampler);bind(p,"lit_history_depth",history_depth[1-parity]);bind(p,"lit_history_normal",history_normal[1-parity]);
        bind(p,"lit_hdr_source",hdr);bind(p,"lit_brdf_lut",brdf_lut);
        bind(p,"lit_temporal",temporal);
    }
    void dispatch(const rhi::Pipeline& pipeline,GfxTexture depth,std::uint32_t x,std::uint32_t y=1,std::uint32_t z=1) {
        common(pipeline.program(),depth);require(gfxCommandBindKernel(gfx,pipeline.kernel()),"Bind Scene Lit compute");
        require(gfxCommandDispatch(gfx,x,y,z),"Dispatch Scene Lit compute");
    }
    void begin(std::uint32_t pass) {require(gfxCommandBeginTimestampQuery(gfx,queries[pass]),"Begin Scene Lit timestamp");}
    void end(std::uint32_t pass) {require(gfxCommandEndTimestampQuery(gfx,queries[pass]),"End Scene Lit timestamp");}
    void reset(bool reset_probes=true) {
        if(history_valid||(reset_probes&&probe_valid)) {
            history_valid=false;if(reset_probes){probe_valid=false;probe_parity=0;probe_cursor=0;probe_updates_pending=0;probe_geometry_pending=0;probe_sequence=0;}reset_events++;
        }
    }
    void publish_gpu_sample(FrameStats& stats) {
        const auto sample=gfxTimestampQueryGetSample(gfx,queries[0]);
        if(sample.frame_sequence==0)return;
        std::array<float,8> timings{};
        for(std::size_t i=0;i<queries.size();i++) {
            const auto pass=gfxTimestampQueryGetSample(gfx,queries[i]);
            if(pass.frame_sequence!=sample.frame_sequence)return;
            timings[i]=static_cast<float>(pass.duration_ms);
        }
        for(std::size_t slot=0;slot<readback_count;slot++) {
            if(readback_sequences[slot]!=sample.frame_sequence)continue;
            const auto* counter=gfxBufferGetData<std::uint32_t>(gfx,ray_readback[slot]);
            if(!counter)return;
            stats.scene_lit_gpu_ms=timings;stats.scene_lit_gpu_sample_sequence=sample.frame_sequence;
            stats.scene_lit_gpu_sample_frame_id=readback_frame_ids[slot];
            stats.scene_lit_shadow_rays=counter[0]|(std::uint64_t(counter[2])<<32);
            stats.scene_lit_probe_rays=counter[1]|(std::uint64_t(counter[3])<<32);
            stats.scene_lit_gpu_timing_valid=true;return;
        }
    }
    std::uint64_t resource_bytes() const {
        std::uint64_t result=0;
        const auto add_texture=[&](GfxTexture t){if(!t)return;std::uint32_t bytes=8;
            if(t.getFormat()==DXGI_FORMAT_R16_FLOAT)bytes=2;
            else if(t.getFormat()==DXGI_FORMAT_R8_UNORM)bytes=1;
            else if(t.getFormat()==DXGI_FORMAT_R8G8_UNORM)bytes=2;
            else if(t.getFormat()==DXGI_FORMAT_R32_FLOAT||t.getFormat()==DXGI_FORMAT_R16G16_FLOAT||t.getFormat()==DXGI_FORMAT_R8G8B8A8_UNORM)bytes=4;
            result+=std::uint64_t(t.getWidth())*t.getHeight()*t.getDepth()*bytes;};
        for(auto t:gbuffer_textures)add_texture(t);
        for(auto t:{direct_hdr,hdr,indirect,ao_texture,ao_raw,reflection_trace,reflection_hit_trace,brdf_lut,tone_color,smaa_edges,smaa_weights,smaa_output,smaa_area,smaa_search,ssil_raw,ssil_irradiance})add_texture(t);
        for(const auto* pair:{&history_depth,&history_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_moments,&reflection_scratch,&ao_history,&smaa_spatial,&ssil_history,&bloom,&probe_irradiance,&probe_target,&probe_distance,&probe_offsets})for(auto t:*pair)add_texture(t);
        for(const auto& signal:direct_signals){add_texture(signal.raw);for(const auto* pair:{&signal.history,&signal.moments,&signal.scratch})for(auto t:*pair)add_texture(t);}
        for(auto t:pyramid)add_texture(t);
        for(auto b:{gpu_instances,gpu_lights,rt_vertices,tile_counts,tile_indices,reflection_work,reflection_counter,reflection_arguments,ray_counters})result+=b.getSize();
        for(auto b:ray_readback)result+=b.getSize();
        for(const auto& [key,g]:geometries){(void)key;result+=g.positions.getSize()+gfxBottomLevelAccelerationStructureGetDataSize(gfx,g.blas);}
        if(tlas)result+=gfxTopLevelAccelerationStructureGetDataSize(gfx,tlas);
        return result;
    }

    bool acceleration(const Frame& frame) {
        material_changed=false;
        bool geometry_changed=false,topology_changed=false,transform_changed=false,instance_moved=false;
        if(!tlas){tlas=gfxCreateTopLevelAccelerationStructure(gfx);allocation(bool(tlas));}
        for(auto& [key,g]:geometries){(void)key;g.used=false;}
        for(auto& [key,o]:objects){(void)key;o.used=false;}
        for(const auto& mesh:frame.meshes) {
            auto [found,inserted]=geometries.try_emplace(mesh.geometry.get());auto& g=found->second;g.used=true;
            if(inserted) {
                g.source=mesh.geometry;const auto vertices=g.source->vertices();
                g.positions=gfxCreateBuffer<geometry::Vertex>(gfx,static_cast<std::uint32_t>(vertices.size()),vertices.data());allocation(bool(g.positions));
                auto range=gfxCreateBufferRange(gfx,g.positions,0,g.positions.getSize());allocation(bool(range));
                g.geometry=gfxCreateGeometryTriangles(gfx,range,sizeof(geometry::Vertex));allocation(bool(g.geometry));
                require(gfxGeometrySetOpaque(gfx,g.geometry,true),"Set opaque RT geometry");
                g.blas=gfxCreateBottomLevelAccelerationStructure(gfx);allocation(bool(g.blas));
                require(gfxBottomLevelAccelerationStructureAddGeometry(gfx,g.blas,g.geometry),"Attach RT mesh geometry");
                const GfxBuildBottomLevelASFlags flags=kGfxBuildBottomLevelASFlag_FastTrace;
                require(gfxBuildBottomLevelAccelerationStructures(gfx,&g.blas,&flags,1),"Build static mesh BLAS");geometry_changed=true;
            }
        }
        object_keys.clear();instances.clear();object_keys.reserve(frame.meshes.size());instances.reserve(frame.meshes.size());
        for(std::size_t index=0;index<frame.meshes.size();index++) {
            const auto& mesh=frame.meshes[index];const ObjectKey key{mesh.stable_key?mesh.stable_key:index,mesh.stable_key==0};
            object_keys.push_back(key);auto [found,inserted]=objects.try_emplace(key);auto& object=found->second;object.used=true;
            const bool correspondence=!inserted&&mesh.stable_key!=0&&object.geometry==mesh.geometry.get()&&history_valid;
            GpuInstance instance{};instance.world=mesh.world;instance.previous_world=correspondence?object.world:mesh.world;
            const auto inverse=math::try_inverse(matrix(mesh.world));allocation(bool(inverse));
            double maximum=0;for(std::size_t r=0;r<3;r++)for(std::size_t c=0;c<3;c++)maximum=std::max(maximum,std::abs(inverse->elements[c*4+r]));
            for(std::size_t c=0;c<3;c++){instance.normal_x[c]=static_cast<float>(inverse->elements[c*4]/maximum);
                instance.normal_y[c]=static_cast<float>(inverse->elements[c*4+1]/maximum);instance.normal_z[c]=static_cast<float>(inverse->elements[c*4+2]/maximum);}
            instance.base_roughness={mesh.color.r,mesh.color.g,mesh.color.b,mesh.material.roughness};
            instance.emissive_metallic={mesh.material.emissive[0],mesh.material.emissive[1],mesh.material.emissive[2],mesh.material.metallic};
            material_changed|=!inserted&&(object.base_roughness!=instance.base_roughness||object.emissive_metallic!=instance.emissive_metallic);
            object.base_roughness=instance.base_roughness;object.emissive_metallic=instance.emissive_metallic;
            instance.geometry={0,static_cast<std::uint32_t>(mesh.geometry->vertices().size()),correspondence?1u:0u,key_hash(key.value)^(key.legacy?0x9e3779b9u:0u)};instances.push_back(instance);
            if(inserted){object.instance=gfxCreateTopLevelAccelerationStructureInstance(gfx);allocation(bool(object.instance));
                require(gfxTopLevelAccelerationStructureAddInstance(gfx,tlas,object.instance),"Attach RT scene instance");topology_changed=true;}
            if(inserted||object.geometry!=mesh.geometry.get()) {
                require(gfxTopLevelAccelerationStructureInstanceSetBottomLevelAccelerationStructure(gfx,object.instance,geometries.at(mesh.geometry.get()).blas),"Set RT instance mesh");topology_changed=true;
            }
            if(inserted||object.world!=mesh.world) {
                instance_moved|=!inserted;
                std::array<float,12> transform{};for(std::size_t r=0;r<3;r++)for(std::size_t c=0;c<4;c++)transform[r*4+c]=mesh.world[c*4+r];
                require(gfxTopLevelAccelerationStructureInstanceSetTransform(gfx,object.instance,transform.data()),"Update rigid RT transform");transform_changed=true;
            }
            require(gfxTopLevelAccelerationStructureInstanceSetInstanceID(gfx,object.instance,static_cast<std::uint32_t>(index)),"Remap current RT snapshot");
            object.world=mesh.world;object.geometry=mesh.geometry.get();
        }
        for(auto it=objects.begin();it!=objects.end();) {
            if(it->second.used){++it;continue;}
            require(gfxTopLevelAccelerationStructureRemoveInstance(gfx,tlas,it->second.instance),"Detach removed RT instance");
            require(gfxDestroyTopLevelAccelerationStructureInstance(gfx,it->second.instance),"Retire removed RT instance");it=objects.erase(it);topology_changed=true;
        }
        for(auto it=geometries.begin();it!=geometries.end();) {
            if(it->second.used){++it;continue;}auto& g=it->second;
            require(gfxDestroyBottomLevelAccelerationStructure(gfx,g.blas),"Retire unused static BLAS");
            require(gfxDestroyGeometry(gfx,g.geometry),"Retire unused RT geometry");retire(g.positions);it=geometries.erase(it);geometry_changed=true;
        }
        if(geometry_changed) {
            packed_vertices.clear();for(auto& [key,g]:geometries){(void)key;g.offset=static_cast<std::uint32_t>(packed_vertices.size());
                const auto vertices=g.source->vertices();packed_vertices.insert(packed_vertices.end(),vertices.begin(),vertices.end());}
            retire(rt_vertices);rt_vertices=gfxCreateBuffer<geometry::Vertex>(gfx,static_cast<std::uint32_t>(packed_vertices.size()),packed_vertices.data());allocation(bool(rt_vertices));
        }
        for(std::size_t i=0;i<instances.size();i++)instances[i].geometry[0]=geometries.at(frame.meshes[i].geometry.get()).offset;
        if(geometry_changed||topology_changed||object_keys!=previous_object_keys)reset();
        if(!history_valid)for(auto& i:instances)i.geometry[2]=0;
        if(!tlas_built||topology_changed) {require(gfxTopLevelAccelerationStructureBuild(gfx,tlas),"Build scene TLAS");tlas_built=true;}
        else if(transform_changed||object_keys!=previous_object_keys)require(gfxTopLevelAccelerationStructureUpdate(gfx,tlas),"Update scene TLAS");
        previous_object_keys=object_keys;
        retire(gpu_instances);gpu_instances=gfxCreateBuffer<GpuInstance>(gfx,static_cast<std::uint32_t>(instances.size()),instances.data());allocation(bool(gpu_instances));
        end(1);
        return instance_moved;
    }

    void lighting(const Frame& frame) {
        lights.clear();shadow_lights.clear();light_keys.clear();
        std::vector<const Light*> sorted;sorted.reserve(frame.lights.size());for(const auto& light:frame.lights)sorted.push_back(&light);
        std::stable_sort(sorted.begin(),sorted.end(),[](const Light* a,const Light* b){
            if((a->kind==LightKind::directional)!=(b->kind==LightKind::directional))return a->kind==LightKind::directional;
            return a->stable_key<b->stable_key;});
        for(const auto* source:sorted) {
            const auto& l=*source;light_keys.push_back(l.stable_key);GpuLight g{};
            g.position_range={l.position[0],l.position[1],l.position[2],l.range};
            g.direction_radius={l.direction[0],l.direction[1],l.direction[2],l.source_radius};
            g.color_intensity={l.color[0],l.color[1],l.color[2],l.intensity};
            g.right_width={l.right[0],l.right[1],l.right[2],l.width};g.up_height={l.up[0],l.up[1],l.up[2],l.height};
            const bool shadow=l.casts_shadow&&frame.view.lit.shadows&&shadow_lights.size()<frame.view.lit.shadow_light_budget;
            g.meta={static_cast<std::uint32_t>(l.kind),shadow?static_cast<std::uint32_t>(shadow_lights.size()):~0u,
                (l.two_sided?1u:0u)|(l.casts_shadow?2u:0u),key_hash(l.stable_key)};
            if(shadow)shadow_lights.push_back(static_cast<std::uint32_t>(lights.size()));lights.push_back(g);
        }
        previous_light_keys=light_keys;
        light_changed=lights!=previous_lights;previous_lights=lights;
        retire(gpu_lights);
        gpu_lights=gfxCreateBuffer<GpuLight>(gfx,std::max(1u,static_cast<std::uint32_t>(lights.size())),lights.empty()?nullptr:lights.data());
        allocation(bool(gpu_lights));
    }
    void draw(const Frame& frame,ui::PixelExtent next,GfxTexture depth,std::span<const SceneLitDraw> draws,FrameStats& stats) {
        pipelines();publish_gpu_sample(stats);begin(1);const auto& settings=frame.view.lit;lighting(frame);targets(next);
        if(!enabled||scene_identity!=frame.scene_identity)reset();
        else if(frame.camera_cut)reset(false);
        const std::array<float,4> next_viewport{frame.scene_viewport.x,frame.scene_viewport.y,frame.scene_viewport.width,frame.scene_viewport.height};
        if(viewport!=next_viewport)reset(false);
        if(probe_counts!=settings.probe_counts||probe_min!=settings.probe_min||probe_max!=settings.probe_max)reset();
        const std::uint32_t flags=(settings.shadows?1u:0u)|(settings.ddgi?2u:0u)|(settings.gtao&&!settings.ssil?4u:0u)|(settings.sssr?8u:0u)|(settings.antialiasing==Antialiasing::taa?16u:0u)|(settings.bloom?32u:0u)|(settings.ssil?64u:0u);
        const auto effect_changes=flags^previous_effect_flags;
        const bool direct_changed=(effect_changes&1u)!=0||settings.shadow_samples!=previous_shadow_samples||settings.shadow_light_budget!=previous_shadow_budget;
        // Invalidate each signal and its actual consumers when its input contract
        // changes. Continuous light edits still use each signal's local response.
        history_invalid=(direct_changed?1u:0u)|
            (direct_changed||(effect_changes&(2u|4u|8u|64u))?2u:0u)|(effect_changes&4u?4u:0u)|
            (direct_changed||effect_changes||settings.probe_debug!=previous_probe_debug?8u:0u)|
            (direct_changed||(effect_changes&(2u|64u))?16u:0u);
        if(settings.antialiasing!=previous_antialiasing)reset(false); // Projection sampling changed.
        previous_antialiasing=settings.antialiasing;
        previous_effect_flags=flags;previous_shadow_samples=settings.shadow_samples;previous_shadow_budget=settings.shadow_light_budget;previous_probe_debug=settings.probe_debug;
        scene_identity=frame.scene_identity;scene_revision=frame.scene_revision;enabled=true;
        probes(settings);const bool transform_changed=acceleration(frame);
        const std::array<float,4> next_environment{settings.environment[0],settings.environment[1],settings.environment[2],1};
        const bool radiance_changed=light_changed||material_changed||next_environment!=environment;
        // Budgeted updates retain the authoritative change hint until every probe has been visited.
        if(radiance_changed||transform_changed)probe_updates_pending=4*probe_counts[0]*probe_counts[1]*probe_counts[2];
        if(transform_changed)probe_geometry_pending=2*probe_counts[0]*probe_counts[1]*probe_counts[2];
        auto projection=matrix(frame.camera.projection);const bool taa_enabled=settings.antialiasing==Antialiasing::taa;
        const bool smaa_temporal=settings.antialiasing==Antialiasing::smaa_t2x;
        const float smaa_jitter=sequence%2==0?.25f:-.25f;
        const float jx=taa_enabled?halton(static_cast<std::uint32_t>(sequence%8)+1,2)-.5f:(smaa_temporal?smaa_jitter:0);
        const float jy=taa_enabled?halton(static_cast<std::uint32_t>(sequence%8)+1,3)-.5f:(smaa_temporal?smaa_jitter:0);
        for(std::size_t row=0;row<4;row++){projection.elements[row*4]+=2*jx/frame.scene_viewport.width*projection.elements[row*4+3];
            projection.elements[row*4+1]-=2*jy/frame.scene_viewport.height*projection.elements[row*4+3];}
        const auto vp=matrix(frame.camera.view)*projection;view_projection=floats(vp);inverse_view_projection=floats(*math::try_inverse(vp));
        if(!history_valid)previous_view_projection=view_projection;
        const auto inverse_view=*math::try_inverse(matrix(frame.camera.view));camera={static_cast<float>(inverse_view.elements[12]),static_cast<float>(inverse_view.elements[13]),static_cast<float>(inverse_view.elements[14]),1};
        size_frame={next.width,next.height,static_cast<std::uint32_t>(sequence),history_valid?1u:0u};
        require(gfxCommandClearBuffer(gfx,ray_counters),"Reset Scene Lit ray counters");
        viewport=next_viewport;
        environment=next_environment;
        controls={std::exp2(settings.exposure_ev),settings.shadow_bias,settings.ao_radius,settings.reflection_distance};
        counts={static_cast<std::uint32_t>(lights.size()),static_cast<std::uint32_t>(shadow_lights.size()),settings.shadow_samples,flags};
        temporal={.9f,settings.reflection_roughness,radiance_changed?1.f:0.f,0};jitter={jx,jy,jitter[0],jitter[1]};
        begin(0);for(std::uint32_t i=0;i<5;i++){require(gfxCommandClearTexture(gfx,gbuffer_textures[i]),"Clear Scene Lit GBuffer");require(gfxCommandBindColorTarget(gfx,i,gbuffer_textures[i]),"Bind Scene Lit GBuffer");}
        require(gfxCommandBindDepthStencilTarget(gfx,depth),"Bind Scene Lit primary depth");common(gbuffer.program(),depth);
        require(gfxCommandBindKernel(gfx,gbuffer.kernel()),"Bind Scene Lit raster pipeline");
        for(const auto& draw:draws){bind(gbuffer.program(),"lit_triangles",draw.triangles);bind(gbuffer.program(),"lit_first_instance",draw.first);
            require(gfxCommandDraw(gfx,draw.vertex_count,draw.count),"Draw Scene Lit opaque geometry");stats.scene_draw_calls++;}
        for(std::uint32_t i=0;i<5;i++)require(gfxCommandBindColorTarget(gfx,i,{}),"Unbind Scene Lit GBuffer");
        require(gfxCommandBindDepthStencilTarget(gfx,{}),"Unbind Scene Lit primary depth");end(0);
        const auto dx=(next.width+7)/8,dy=(next.height+7)/8;
        begin(2);bind(tiles.program(),"lit_tile_counts_output",tile_counts);bind(tiles.program(),"lit_tile_indices_output",tile_indices);
        dispatch(tiles,depth,(next.width+15)/16,(next.height+15)/16);
        bind(direct.program(),"lit_tile_counts",tile_counts);bind(direct.program(),"lit_tile_indices",tile_indices);
        const std::array<const char*,2> raw_outputs{"lit_direct_diffuse_raw_output","lit_direct_specular_raw_output"},
            raw_inputs{"lit_direct_diffuse_raw","lit_direct_specular_raw"},previous_inputs{"lit_direct_diffuse_previous","lit_direct_specular_previous"},
            moments_previous{"lit_direct_diffuse_moments_previous","lit_direct_specular_moments_previous"},
            history_outputs{"lit_direct_diffuse_history_output","lit_direct_specular_history_output"},
            moments_outputs{"lit_direct_diffuse_moments_output","lit_direct_specular_moments_output"},
            filter_inputs{"lit_direct_diffuse_filter_input","lit_direct_specular_filter_input"},
            filter_outputs{"lit_direct_diffuse_filter_output","lit_direct_specular_filter_output"},
            filter_moments{"lit_direct_diffuse_moments","lit_direct_specular_moments"};
        for(std::size_t i=0;i<2;++i)bind(direct.program(),raw_outputs[i],direct_signals[i].raw);
        dispatch(direct,depth,dx,dy);
        for(std::size_t i=0;i<2;++i){const auto& signal=direct_signals[i];const auto p=direct_resolve.program();
            bind(p,raw_inputs[i],signal.raw);bind(p,previous_inputs[i],signal.history[1-parity]);bind(p,moments_previous[i],signal.moments[1-parity]);
            bind(p,history_outputs[i],signal.history[parity]);bind(p,moments_outputs[i],signal.moments[parity]);}
        dispatch(direct_resolve,depth,dx,dy);
        for(std::uint32_t step=0;step<3;++step){const auto p=direct_filter.program();
            for(std::size_t i=0;i<2;++i){const auto& signal=direct_signals[i];
                bind(p,filter_inputs[i],step==0?signal.history[parity]:signal.scratch[(step-1)%2]);
                bind(p,filter_outputs[i],signal.scratch[step%2]);bind(p,filter_moments[i],signal.moments[parity]);}
            bind(p,"lit_direct_filter_step",1u<<step);dispatch(direct_filter,depth,dx,dy);}
        for(std::size_t i=0;i<2;++i)bind(direct_combine.program(),filter_inputs[i],direct_signals[i].scratch[0]);
        bind(direct_combine.program(),"lit_direct_output",direct_hdr);dispatch(direct_combine,depth,dx,dy);end(2);
        begin(3);
        if(settings.ddgi) {
            const auto total=probe_counts[0]*probe_counts[1]*probe_counts[2];float maximum_distance=0;
            for(std::size_t i=0;i<3;i++){const auto size=probe_max[i]-probe_min[i];probe_origin[i]=probe_min[i];probe_spacing[i]=size/(probe_counts[i]-1);maximum_distance+=size*size;}
            probe_origin[3]=std::sqrt(maximum_distance);probe_spacing[3]=0;
            probe_grid={probe_counts[0],probe_counts[1],probe_counts[2],total};const auto budget=std::min(settings.probe_update_budget,total);
            probe_history={probe_updates_pending?.8f:.93f,probe_geometry_pending?.85f:.95f,1.f-float(budget)/float(total),0};
            probe_update_settings={probe_cursor,budget,settings.probe_rays,(probe_valid?0u:1u)|(settings.probe_debug?2u:0u)};
            const auto bind_probes=[&](GfxProgram p,bool newest) {
                bind(p,"lit_probe_origin",probe_origin);bind(p,"lit_probe_spacing",probe_spacing);bind(p,"lit_probe_grid",probe_grid);bind(p,"lit_probe_update",probe_update_settings);
                bind(p,"lit_probe_sequence",probe_sequence);
                bind(p,"lit_probe_history",probe_history);
                const auto read=newest?probe_parity:1-probe_parity;bind(p,"lit_probe_irradiance_previous",probe_irradiance[read]);bind(p,"lit_probe_distance_previous",probe_distance[read]);bind(p,"lit_probe_offsets_previous",probe_offsets[read]);
                bind(p,"lit_probe_target_previous",probe_target[read]);bind(p,"lit_probe_target_output",probe_target[probe_parity]);
                bind(p,"lit_probe_irradiance_output",probe_irradiance[probe_parity]);bind(p,"lit_probe_distance_output",probe_distance[probe_parity]);bind(p,"lit_probe_offsets_output",probe_offsets[probe_parity]);
            };
            bind_probes(probe_copy.program(),false);dispatch(probe_copy,depth,(total*64+63)/64);
            bind_probes(probe_update.program(),false);dispatch(probe_update,depth,budget);
            bind_probes(probe_resolve.program(),true);bind(probe_resolve.program(),"lit_indirect",indirect);dispatch(probe_resolve,depth,dx,dy);
            probe_cursor=(probe_cursor+budget)%total;probe_valid=true;probe_parity=1-probe_parity;
            probe_updates_pending-=std::min(probe_updates_pending,budget);
            probe_geometry_pending-=std::min(probe_geometry_pending,budget);
            ++probe_sequence; // Sampling belongs to this probe field, independent of surface-frame history.
        }
        end(3);begin(4);if(settings.gtao&&!settings.ssil){bind(ao.program(),"lit_ao_output",ao_raw);dispatch(ao,depth,dx,dy);
            bind(ao_resolve.program(),"lit_ao_raw",ao_raw);bind(ao_resolve.program(),"lit_ao_previous",ao_history[1-parity]);
            bind(ao_resolve.program(),"lit_ao_history_output",ao_history[parity]);bind(ao_resolve.program(),"lit_ao_resolved_output",ao_texture);dispatch(ao_resolve,depth,dx,dy);}
        if(settings.ssil) {
            if(!ssil_raw){ssil_raw=texture(next.width,next.height,DXGI_FORMAT_R16G16B16A16_FLOAT,8);ssil_irradiance=texture(next.width,next.height,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
                for(auto& t:ssil_history)t=texture(next.width,next.height,DXGI_FORMAT_R16G16B16A16_FLOAT,8);history_invalid|=16u;}
            for(const auto* pipeline:{&ssil_trace,&ssil_resolve}){const auto p=pipeline->program();bind(p,"lit_direct_diffuse",direct_signals[0].scratch[0]);
                bind(p,"lit_indirect_source",indirect);bind(p,"lit_ssil_controls",std::array<float,4>{1.2f,.3f,0,0});
                bind(p,"lit_ssil_raw_output",ssil_raw);bind(p,"lit_ssil_raw",ssil_raw);bind(p,"lit_ssil_previous",ssil_history[1-parity]);
                bind(p,"lit_ssil_history_output",ssil_history[parity]);bind(p,"lit_ssil_irradiance_output",ssil_irradiance);}
            dispatch(ssil_trace,depth,dx,dy);dispatch(ssil_resolve,depth,dx,dy);
        }
        end(4);
        begin(5);bind(compose.program(),"lit_direct_source",direct_hdr);bind(compose.program(),"lit_indirect_source",settings.ssil?ssil_irradiance:indirect);bind(compose.program(),"lit_ao",ao_texture);
        bind(compose.program(),"lit_probe_debug",settings.probe_debug&&settings.ddgi?1u:0u);
        bind(compose.program(),"lit_background",frame.view.background==Background::gradient?1u:0u);
        bind(compose.program(),"lit_hdr_output",hdr);dispatch(compose,depth,dx,dy);
        if(!brdf_ready){bind(brdf.program(),"lit_brdf_output",brdf_lut);dispatch(brdf,depth,8,8);brdf_ready=true;}
        for(std::uint32_t i=0;i<pyramid_count;i++) {
            bind(depth_pyramid.program(),"lit_depth_source_is_device",i==0?1u:0u);bind(depth_pyramid.program(),"lit_depth_input",pyramid[i==0?pyramid_count-1:i-1]);
            bind(depth_pyramid.program(),"lit_depth_output",pyramid[i]);const std::array<std::uint32_t,2> size{pyramid[i].getWidth(),pyramid[i].getHeight()};
            bind(depth_pyramid.program(),"lit_depth_output_size",size);dispatch(depth_pyramid,depth,(size[0]+7)/8,(size[1]+7)/8);
        }
        auto pyramid_array=pyramid;for(std::uint32_t i=pyramid_count;i<12;i++)pyramid_array[i]=pyramid[pyramid_count-1];
        for(const auto* pipeline:{&ssr_reset,&ssr_classify,&ssr_prepare,&ssr_trace}) {
            const auto p=pipeline->program();bind(p,"lit_reflection_work",reflection_work);bind(p,"lit_reflection_counter",reflection_counter);bind(p,"lit_reflection_arguments",reflection_arguments);
            bind(p,"lit_reflection_output",reflection_trace);bind(p,"lit_reflection_hit_trace_output",reflection_hit_trace);
            require(gfxProgramSetParameter(gfx,p,"lit_depth_pyramid",pyramid_array.data(),12),"Bind separate depth hierarchy");bind(p,"lit_depth_level_count",pyramid_count);
        }
        dispatch(ssr_reset,depth,1);dispatch(ssr_classify,depth,dx,dy);dispatch(ssr_prepare,depth,1);
        if(settings.sssr){common(ssr_trace.program(),depth);require(gfxCommandBindKernel(gfx,ssr_trace.kernel()),"Bind SSSR work kernel");require(gfxCommandDispatchIndirect(gfx,reflection_arguments),"Dispatch SSSR classified work");}
        bind(reflection_resolve.program(),"lit_reflection_trace",reflection_trace);bind(reflection_resolve.program(),"lit_reflection_hit_trace",reflection_hit_trace);
        bind(reflection_resolve.program(),"lit_reflection_previous",reflection_history[1-parity]);bind(reflection_resolve.program(),"lit_reflection_hit_previous",reflection_hit_history[1-parity]);
        bind(reflection_resolve.program(),"lit_reflection_resolved_output",reflection_history[parity]);bind(reflection_resolve.program(),"lit_reflection_hit_output",reflection_hit_history[parity]);
        bind(reflection_resolve.program(),"lit_reflection_moments_previous",reflection_moments[1-parity]);bind(reflection_resolve.program(),"lit_reflection_moments_output",reflection_moments[parity]);
        dispatch(reflection_resolve,depth,dx,dy);
        for(std::uint32_t step=0;step<3;++step){const auto p=reflection_filter.program();
            bind(p,"lit_reflection_filter_input",step==0?reflection_history[parity]:reflection_scratch[(step-1)%2]);
            bind(p,"lit_reflection_filter_output",reflection_scratch[step%2]);bind(p,"lit_reflection_moments",reflection_moments[parity]);
            bind(p,"lit_reflection_hit_trace",reflection_hit_trace);bind(p,"lit_reflection_filter_step",1u<<step);dispatch(reflection_filter,depth,dx,dy);}end(5);
        begin(6);bind(taa.program(),"lit_reflection",reflection_scratch[0]);bind(taa.program(),"lit_reflection_moments",reflection_moments[parity]);bind(taa.program(),"lit_taa_previous",taa_history[1-parity]);bind(taa.program(),"lit_taa_output",taa_history[parity]);
        bind(taa.program(),"lit_history_depth_output",history_depth[parity]);bind(taa.program(),"lit_history_normal_output",history_normal[parity]);dispatch(taa,depth,dx,dy);end(6);
        begin(7);const std::array<float,4> bloom_controls{1,.5f,settings.bloom?.04f:0.f,64};
        bind(bloom_downsample.program(),"lit_taa",taa_history[parity]);bind(bloom_downsample.program(),"lit_bloom_output",bloom[0]);bind(bloom_downsample.program(),"lit_bloom_controls",bloom_controls);
        dispatch(bloom_downsample,depth,(bloom[0].getWidth()+7)/8,(bloom[0].getHeight()+7)/8);
        for(std::uint32_t i=0;i<2;i++){bind(bloom_blur.program(),"lit_bloom_input",bloom[i]);bind(bloom_blur.program(),"lit_bloom_output",bloom[1-i]);
            bind(bloom_blur.program(),"lit_bloom_direction",std::array<std::int32_t,2>{i==0?1:0,i==0?0:1});dispatch(bloom_blur,depth,(bloom[0].getWidth()+7)/8,(bloom[0].getHeight()+7)/8);}
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
                bind(p,"lit_smaa_output",smaa_output);bind(p,"lit_smaa_expected_previous_depth",gbuffer_textures[4]);bind(p,"lit_smaa_temporal_enabled",smaa_temporal?1u:0u);}
            dispatch(smaa_edges_pass,depth,dx,dy);dispatch(smaa_weights_pass,depth,dx,dy);dispatch(smaa_neighborhood,depth,dx,dy);
            display=smaa_spatial[parity];if(smaa_temporal){dispatch(smaa_resolve,depth,dx,dy);display=smaa_output;}
        }
        common(tone.program(),depth);bind(tone.program(),"lit_display_source",display);
        require(gfxCommandBindKernel(gfx,tone.kernel()),"Bind Scene Lit tone map");require(gfxCommandDraw(gfx,3),"Compose Scene Lit display");stats.viewport_draw_calls++;end(7);
        stats.scene_lit_resource_bytes=resource_bytes();stats.scene_lit_shadow_lights=counts[1];stats.scene_lit_unshadowed_lights=static_cast<std::uint32_t>(std::count_if(frame.lights.begin(),frame.lights.end(),[](const Light& l){return l.casts_shadow;}))-counts[1];
        const auto readback_slot=sequence%readback_count;
        require(gfxCommandCopyBuffer(gfx,ray_readback[readback_slot],ray_counters),"Publish Scene Lit ray counters asynchronously");
        readback_sequences[readback_slot]=gfxGetRecordingFrameSequence(gfx);readback_frame_ids[readback_slot]=frame.ui_frame.id;
        stats.scene_lit_history_resets=reset_events;reset_events=0;previous_view_projection=view_projection;history_valid=true;sequence++;parity=1-parity;
    }
    void close() {
        retire_targets();for(auto* pair:{&probe_irradiance,&probe_target,&probe_distance,&probe_offsets})for(auto& t:*pair)retire(t);retire(brdf_lut);retire(smaa_area);retire(smaa_search);
        for(auto& [key,o]:objects){(void)key;if(tlas)require(gfxTopLevelAccelerationStructureRemoveInstance(gfx,tlas,o.instance),"Detach Scene Lit instance");
            require(gfxDestroyTopLevelAccelerationStructureInstance(gfx,o.instance),"Retire Scene Lit instance");}objects.clear();
        if(tlas)require(gfxDestroyTopLevelAccelerationStructure(gfx,std::exchange(tlas,{})),"Retire Scene Lit TLAS");
        for(auto& [key,g]:geometries){(void)key;require(gfxDestroyBottomLevelAccelerationStructure(gfx,g.blas),"Retire Scene Lit BLAS");require(gfxDestroyGeometry(gfx,g.geometry),"Retire Scene Lit geometry");retire(g.positions);}geometries.clear();
        for(auto* b:{&gpu_instances,&gpu_lights,&rt_vertices,&ray_counters})retire(*b);
        for(auto& readback:ray_readback)retire(readback);
        for(auto& q:queries)if(q)require(gfxDestroyTimestampQuery(gfx,std::exchange(q,{})),"Retire Scene Lit timestamp");
        if(sampler)require(gfxDestroySamplerState(gfx,std::exchange(sampler,{})),"Retire Scene Lit sampler");
        for(auto* p:{&gbuffer,&tiles,&direct,&direct_resolve,&direct_filter,&direct_combine,&compose,&probe_copy,&probe_update,&probe_resolve,
                    &ao,&ao_resolve,&depth_pyramid,&ssr_reset,&ssr_classify,&ssr_prepare,&ssr_trace,&reflection_resolve,&reflection_filter,&brdf,&taa,
                    &bloom_downsample,&bloom_blur,&tone_map,&tone,&smaa_edges_pass,&smaa_weights_pass,&smaa_neighborhood,&smaa_resolve,&ssil_trace,&ssil_resolve})*p={};
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
    const auto& s=frame.view.lit;invalid(frame.lights.size()<=256&&s.shadow_light_budget<=16&&s.shadow_samples>=1&&s.shadow_samples<=64,"Invalid Scene Lit light or shadow budget");
    const auto finite=[](float v){return std::isfinite(v);};
    invalid(finite(s.exposure_ev)&&s.exposure_ev>=-16&&s.exposure_ev<=16&&finite(s.shadow_bias)&&s.shadow_bias>0&&s.shadow_bias<=.1f&&finite(s.ao_radius)&&s.ao_radius>0&&
        finite(s.reflection_distance)&&s.reflection_distance>0&&finite(s.reflection_roughness)&&s.reflection_roughness>=0&&s.reflection_roughness<=1&&s.probe_update_budget>0&&s.probe_rays>=1&&s.probe_rays<=256&&s.antialiasing<=Antialiasing::smaa_t2x,"Invalid Scene Lit settings");
    std::uint64_t total=1;for(std::size_t i=0;i<3;i++){invalid(finite(s.environment[i])&&s.environment[i]>=0&&finite(s.probe_min[i])&&finite(s.probe_max[i])&&s.probe_max[i]>s.probe_min[i]&&s.probe_counts[i]>=2&&s.probe_counts[i]<=32,"Invalid Scene Lit probe volume");total*=s.probe_counts[i];}
    invalid(total<=4096,"Scene Lit probe volume exceeds 4096 probes");
    std::unordered_set<std::uint64_t> mesh_keys,light_keys;
    for(const auto& mesh:frame.meshes){const auto& m=mesh.material;invalid(finite(m.roughness)&&m.roughness>=0&&m.roughness<=1&&finite(m.metallic)&&m.metallic>=0&&m.metallic<=1,"Invalid Scene Lit material");
        for(float e:m.emissive)invalid(finite(e)&&e>=0&&e<=65504,"Invalid Scene Lit emissive");if(mesh.stable_key)invalid(mesh_keys.insert(mesh.stable_key).second,"Duplicate stable mesh identity");}
    for(const auto& l:frame.lights){invalid(l.kind<=LightKind::directional&&finite(l.intensity)&&l.intensity>=0&&finite(l.range)&&l.range>0&&finite(l.source_radius)&&l.source_radius>=0&&finite(l.width)&&l.width>0&&finite(l.height)&&l.height>0,"Invalid Scene Lit light");
        float direction_length=0,right_length=0,up_length=0,orthogonal=0;for(std::size_t i=0;i<3;i++){invalid(finite(l.position[i])&&finite(l.direction[i])&&finite(l.color[i])&&l.color[i]>=0&&finite(l.right[i])&&finite(l.up[i]),"Invalid Scene Lit light vectors");direction_length+=l.direction[i]*l.direction[i];right_length+=l.right[i]*l.right[i];up_length+=l.up[i]*l.up[i];orthogonal+=l.right[i]*l.up[i];}
        invalid(std::abs(direction_length-1)<.002f,"Light propagation direction must be unit length");
        if(l.kind==LightKind::rectangle) {
            float right_dot=0,up_dot=0;for(std::size_t i=0;i<3;i++){right_dot+=l.right[i]*l.direction[i];up_dot+=l.up[i]*l.direction[i];}
            invalid(std::abs(right_length-1)<.002f&&std::abs(up_length-1)<.002f&&std::abs(orthogonal)<.002f&&std::abs(right_dot)<.002f&&std::abs(up_dot)<.002f,"Rectangle light tangent axes and propagation direction must be orthonormal");
        }
        if(l.kind==LightKind::directional)invalid(l.source_radius<1.5f,"Directional source angular radius is too large");if(l.stable_key)invalid(light_keys.insert(l.stable_key).second,"Duplicate stable light identity");}
}
void SceneLit::draw(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,std::span<const SceneLitDraw> draws,FrameStats& stats){state_->draw(frame,extent,depth,draws,stats);}
void SceneLit::invalidate() noexcept {state_->enabled=false;state_->reset();}
Result<void> SceneLit::close(){if(!state_)return {};try{state_->close();state_.reset();return {};}catch(const Error& error){return std::unexpected(error);}}
}

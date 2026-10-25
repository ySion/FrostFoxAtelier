#include "scene_lit.hpp"
#include "light_frame.hpp"
#include "shadow_cache.hpp"
#include "ddgi_volume.hpp"
#include "shader_source.hpp"
#include "ltc_lut.hpp"
#include "smaa_lut.hpp"
#include "solid_geometry.hpp"
#include "raster_viewport.hpp"
#include "frame_slots.hpp"
#include "math/math.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <utility>

namespace atelier::renderer::detail {
namespace {
// Secondary ray origins leave their surface by this many world meters.
constexpr float ray_offset=.002f;
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
static_assert(sizeof(ProbeContribution)==96);
static_assert(sizeof(ProbeInspection::Ray)==96);
}

struct SceneLit::State {
    rhi::Context& context;
    GfxContext gfx;
    rhi::Pipeline gbuffer,compose,sun_trace_prepare,sun_trace,cluster_build,cluster_inspect,shadow_depth;
    rhi::Pipeline probe_copy,probe_trace,probe_update,probe_present,probe_resolve,probe_upsample,probe_exact,ao,ao_depth_reduce,ao_resolve,depth_first,depth_pyramid,ssr_reset,ssr_classify,ssr_prepare,ssr_trace;
    rhi::Pipeline reflection_resolve,reflection_filter,brdf,taa,bloom_downsample,bloom_blur,tone_map,tone;
    rhi::Pipeline smaa_edges_pass,smaa_weights_pass,smaa_neighborhood,smaa_resolve;
    rhi::Pipeline probe_debug_surface,probe_debug_geometry,inspect_pass,inspect_update;
    GfxTexture probe_age,probe_sample_cursor,probe_debug_depth;
    std::array<GfxTimestampQuery,15> queries{}; // FrameStats::scene_lit_gpu_pass_mask scopes.
    std::uint32_t encoded_pass_mask{};
    GfxSamplerState sampler,shadow_sampler;
    ui::PixelExtent extent{};
    std::array<GfxTexture,7> gbuffer_textures{};
    GfxTexture indirect_half; // DDGI receiver irradiance at one representative per 2x2 block.
    GfxBuffer probe_fallback,probe_fallback_arguments; // Receivers without a matching representative: count, packed pixels; indirect dispatch.
    std::uint32_t probe_fallback_capacity{};
    GfxTexture hdr,indirect,ao_texture,ao_raw,reflection_trace,reflection_hit_trace,brdf_lut;
    GfxTexture ao_depth; // GTAO view depth with four reduced levels for distant horizon taps.
    std::array<float,4> ao_view_x{},ao_view_y{},ao_view_z{}; // Jittered projection terms for view-space reconstruction.
    std::array<GfxTexture,2> history_depth{},history_normal{},history_geometry_normal{},taa_history{},reflection_history{},reflection_hit_history{},reflection_moments{},reflection_scratch{},ao_history{},bloom{};
    GfxTexture tone_color,smaa_edges,smaa_weights,smaa_output,smaa_area,smaa_search;
    std::array<GfxTexture,2> smaa_spatial{},history_surface_id{};
    GfxBuffer cluster_depth,cluster_count,cluster_source;
    std::array<std::uint32_t,2> cluster_tiles{},cluster_origin{};
    ShadowCache shadow_cache;
    GfxTexture shadow_static,shadow_dynamic;
    struct ShadowTables {
        DeviceTable<GpuShadowFace> face_table;
        DeviceTable<GpuShadowLight> light_table;
        DeviceTable<std::uint32_t> instance_table;
        GfxBuffer faces,lights,instances; // This frame's tables.
        std::vector<GpuShadowFace> previous_faces;
        std::vector<GpuShadowLight> previous_lights;
        std::vector<std::uint32_t> previous_instances;
    } shadow_tables;
    std::vector<ShadowCaster> shadow_casters;
    std::vector<ShadowChange> shadow_changes;
    std::vector<std::uint32_t> shadow_instance_indices;
    bool shadows_cached{};
    struct ShadowSample { ShadowPlanStats plan;std::uint32_t draws{}; };
    std::array<GfxTexture,12> pyramid{};
    std::uint32_t pyramid_count{};
    std::array<GfxTexture,2> probe_irradiance{},probe_distance{},probe_offsets{};
    GfxTexture probe_presentation{}; // Display-only cache; never a transport feedback input.
    GfxTexture probe_response; // Per directional texel: author-change epoch and effective response visits.
    GfxBuffer probe_ray_hits;
    GfxBuffer probe_visibility_cache; // Per receiver block: 32 LOS bits and 32 validity bits.
    bool probe_visibility_valid{};
    ProbeField probe_field;
    std::array<ProbeVolumeSettings,probe_volume_limit> previous_probe_settings{};
    DeviceTable<GpuInstance> instance_table;
    GfxBuffer gpu_instances; // This frame's instance table.
    struct LightTables {
        DeviceTable<GpuLightSource> sources;
        DeviceTable<GpuLightProposal> proposals;
        DeviceTable<std::uint32_t> spatial_source_slots;
        DeviceTable<GpuLightCell> cells;
        DeviceTable<GpuSpatialLightProposal> spatial_proposals;
        template<class Self,class F> void each(this Self& self,F&& f) {
            f(self.sources);f(self.proposals);f(self.spatial_source_slots);f(self.cells);f(self.spatial_proposals);
        }
    } light_tables;
    // This frame's light tables.
    GfxBuffer gpu_sources,gpu_proposals,gpu_light_cells,gpu_spatial_proposals,gpu_spatial_source_slots;
    GfxBuffer rt_triangles,rt_closed_slots,reflection_work,reflection_counter,reflection_arguments;
    GfxBuffer sun_trace_list,sun_trace_arguments; // Pixels with a sun beyond every cascade, one entry each.
    LightFrame light_frame;
    GfxBuffer ray_counters;
    GfxBuffer ltc; // Rectangle-light LTC table, ltc_lut::size squared cells.
    static constexpr std::size_t readback_count=32;
    std::array<GfxBuffer,readback_count> ray_readback{};
    std::array<std::uint64_t,readback_count> readback_sequences{},readback_frame_ids{};
    // Receiver depth read back from another camera or scene must not fit cascades.
    std::array<std::uint64_t,readback_count> readback_depth_epochs{};
    std::uint64_t depth_epoch{};
    std::array<std::uint32_t,readback_count> readback_pass_masks{};
    std::array<ShadowSample,readback_count> readback_shadows{};
    struct InspectionState {
        GfxBuffer output,rays;
        std::array<GfxBuffer,readback_count> readback{},ray_readback{};
        std::array<ProbeInspection,readback_count> metadata{};
        std::shared_ptr<const ProbeInspection> published;
        std::uint32_t traced_probe{~0u};
    };
    std::unique_ptr<InspectionState> inspection;
    std::uint64_t completed_sequence{};
    GfxTopLevelAccelerationStructure tlas;
    struct Geometry {
        geometry::MeshRef source;
        GfxBuffer positions;
        GfxGeometry geometry;
        GfxBottomLevelAccelerationStructure blas;
        std::vector<TriangleVolume> volumes;
        std::map<std::uint32_t,std::vector<std::uint32_t>> component_slots;
        std::uint32_t offset{};
        bool used{},opaque{true},next_opaque{true};
    };
    struct Object {
        GfxTopLevelAccelerationStructureInstance instance;
        const geometry::Mesh* geometry{};
        std::array<float,16> world{};
        std::array<float,4> normal_x{},normal_y{},normal_z{};
        std::vector<material::Description> surfaces;
        ShadowBounds shadow_bounds;
        bool shadow_dynamic{},shadow_tracked{};
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
    // Per snapshot index, the object and mesh entries the last complete pass
    // resolved. Only a complete pass retires entries, after rewriting every
    // slot, so slots are trusted only while resolved_complete holds.
    struct Slot { ObjectKey key; Object* object{}; Geometry* geometry{}; };
    std::vector<Slot> resolved;
    bool resolved_complete{};
    std::uint64_t material_revision{}; // MaterialFrame revision the complete pass compared surfaces against.
    std::vector<GpuInstance> instances;
    std::vector<GpuRayTriangle> packed_triangles;
    std::vector<std::uint32_t> packed_closed_slots;
    std::array<float,16> view_projection{},inverse_view_projection{},previous_view_projection{},previous_inverse_view_projection{};
    std::array<float,16> camera_view{};
    float far_depth{};
    std::array<float,4> viewport{},environment{},controls{},temporal{},jitter{},probe_history{},probe_controls{};
    // Holding the map keeps its address from naming a newer map while these textures live.
    rendering::SkyMapRef sky_source;
    GfxTexture sky_background,sky_cube;
    GfxSamplerState sky_sampler;
    std::array<float,4> sky{},sky_radiance{}; // cos/sin of the yaw, 2^k, bound; sky_color * sky_intensity.
    float sky_yaw{};
    std::array<float,4> probe_scene_min{},probe_scene_max{};
    std::array<std::uint32_t,4> size_frame{},source_counts{},probe_update_settings{};
    std::uint32_t hit_candidates{};
    std::array<std::uint32_t,4> light_grid_counts_gpu{};
    bool spatial_uploaded{};
    std::array<std::uint32_t,4> screen_dispatch{};
    std::array<float,4> raster_transform{1,1,0,0};
    std::uint32_t effect_flags{};
    std::uint64_t scene_identity{},reset_events{},sequence{};
    std::uint32_t parity{},probe_parity{},probe_response_epoch{},probe_geometry_pending{},probe_sequence{};
    std::uint32_t probe_history_mode{};
    float previous_probe_bounce{.85f},previous_indirect_gain{1};
    bool previous_probe_feedback_visibility{true};
    std::uint32_t previous_effect_flags{};
    std::uint32_t history_invalid{};
    Antialiasing previous_antialiasing{Antialiasing::none};
    bool history_valid{},enabled{},brdf_ready{},tlas_built{};
    std::uint32_t full_window_frames{};
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
    // Pipeline groups by the settings that bind them. draw() runs only after
    // the frame's groups are ready, so it never prepares or waits.
    enum Group : std::uint32_t {
        group_base=1,group_shadow_cache=2,group_cluster_inspect=4,group_probe_inspection=8,group_probe_coverage=16,group_probe_overlay=32,group_end=64
    };
    struct PendingPipeline { rhi::Pipeline* output; rhi::PipelineJob job; Group group; };
    std::vector<PendingPipeline> pending;
    std::array<std::uint32_t,6> group_sizes{};
    std::uint32_t submitted{};
    std::optional<Error> preparation_failure;
    static std::uint32_t groups(const ViewSettings& view) {
        const auto& s=view.lit;
        return group_base|(s.shadows?group_shadow_cache:0u)|(s.direct_cluster_inspect?group_cluster_inspect:0u)|
            (s.ddgi&&s.probe_inspect?group_probe_inspection:0u)|(s.probe_debug?group_probe_coverage:0u)|
            (view.wireframe!=Wireframe::only&&s.probe_overlay()?group_probe_overlay:0u);
    }
    void submit(Group group,rhi::Pipeline& output,const rhi::PipelineDesc& desc) {
        auto job=context.submit(desc);if(!job)throw Error{job.error(),false};
        pending.push_back({&output,std::move(*job),group});++group_sizes[std::countr_zero(std::uint32_t(group))];
    }
    void submit(Group group,rhi::Pipeline& output,const char* source,const char* entry,const char* name) {
        rhi::PipelineDesc desc;desc.source.cs=source;desc.entry_point=entry;desc.name=name;submit(group,output,desc);
    }
    void submit_probe_debug(Group group,rhi::Pipeline& pipeline,bool geometry) {
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
        submit(group,pipeline,desc);
    }
    // Workers start jobs in submission order. Under GPU-based validation each
    // job also pays its pipeline's first-use patching, so the costliest
    // measured pipelines go first.
    void submit_group(Group group) {
        switch(group) {
        case group_base: {
            submit(group,probe_update,shaders::lit_ddgi,"DDGIUpdate","DDGI hit lighting and convolution");
            submit(group,compose,shaders::lit_clustered,"lit_compose_cs","Scene Lit direct lighting and HDR composition");
            submit(group,sun_trace,shaders::lit_clustered,"lit_sun_trace_cs","Sun receivers beyond the cascades");
            submit(group,sun_trace_prepare,shaders::lit_clustered,"lit_sun_trace_prepare_cs","Sun trace indirect arguments");
            submit(group,probe_trace,shaders::lit_ddgi,"DDGITrace","DDGI RT primary trace");
            submit(group,reflection_resolve,shaders::lit_post,"lit_reflection_resolve_cs","SSSR temporal resolve");
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
            submit(group,gbuffer,desc);
            submit(group,cluster_build,shaders::lit_clustered,"lit_cluster_build_cs","Conservative local light lists");
            submit(group,probe_copy,shaders::lit_ddgi,"DDGICopy","DDGI history transfer");
            submit(group,probe_present,shaders::lit_ddgi,"DDGIPresent","DDGI presentation smoothing");
            submit(group,probe_resolve,shaders::lit_ddgi,"DDGIResolve","DDGI irradiance gather");
            submit(group,probe_upsample,shaders::lit_ddgi,"DDGIUpsample","DDGI irradiance upsampling");
            submit(group,probe_exact,shaders::lit_ddgi,"DDGIExact","DDGI exact receiver gather");
            submit(group,ao,shaders::lit_post,"lit_gtao_cs","Scene Lit GTAO");
            submit(group,ao_depth_reduce,shaders::lit_post,"lit_gtao_depth_cs","GTAO view depth levels");
            submit(group,ao_resolve,shaders::lit_post,"lit_gtao_resolve_cs","GTAO signal reconstruction");
            submit(group,depth_first,shaders::lit_post,"lit_depth_first_cs","Scene Lit first depth reduction");
            submit(group,depth_pyramid,shaders::lit_post,"lit_depth_pyramid_cs","Scene Lit depth hierarchy");
            submit(group,ssr_reset,shaders::lit_post,"lit_ssr_reset_cs","SSSR work reset");
            submit(group,ssr_classify,shaders::lit_post,"lit_ssr_classify_cs","SSSR classify");
            submit(group,ssr_prepare,shaders::lit_post,"lit_ssr_prepare_cs","SSSR indirect work");
            submit(group,ssr_trace,shaders::lit_post,"lit_ssr_trace_cs","SSSR hierarchy traversal");
            submit(group,reflection_filter,shaders::lit_post,"lit_reflection_filter_cs","SSSR spatial reconstruction");
            submit(group,brdf,shaders::lit_post,"lit_brdf_lut_cs","Split sum BRDF integration");
            submit(group,taa,shaders::lit_post,"lit_taa_cs","Scene Lit temporal antialiasing");
            submit(group,bloom_downsample,shaders::lit_post,"lit_bloom_downsample_cs","Scene Lit bloom downsample");
            submit(group,bloom_blur,shaders::lit_post,"lit_bloom_blur_cs","Scene Lit bloom filter");
            submit(group,tone_map,shaders::lit_post,"lit_tonemap_cs","Scene Lit SDR scene resolve");
            submit(group,smaa_edges_pass,shaders::lit_smaa,"lit_smaa_edges_cs","SMAA edges");
            submit(group,smaa_weights_pass,shaders::lit_smaa,"lit_smaa_weights_cs","SMAA coverage weights");
            submit(group,smaa_neighborhood,shaders::lit_smaa,"lit_smaa_neighborhood_cs","SMAA spatial reconstruction");
            submit(group,smaa_resolve,shaders::lit_smaa,"lit_smaa_resolve_cs","SMAA two-frame reconstruction");
            const std::string tone_source=std::string(shaders::lit_post)+"\nfloat4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return lit_display_ps(p,uv);}";
            rhi::PipelineDesc tone_desc;tone_desc.kind=rhi::PipelineKind::vertex;tone_desc.source.vs=shaders::viewport_vs;
            tone_desc.source.ps=tone_source.c_str();tone_desc.name="Scene Lit display tone";
            require(gfxDrawStateSetColorTarget(tone_desc.draw_state,0,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),"Set Scene Lit display format");
            require(gfxDrawStateSetCullMode(tone_desc.draw_state,D3D12_CULL_MODE_NONE),"Set Scene Lit display culling");
            require(gfxDrawStateSetDepthWriteMask(tone_desc.draw_state,D3D12_DEPTH_WRITE_MASK_ZERO),"Disable tone depth writes");
            submit(group,tone,tone_desc);
            break;
        }
        case group_shadow_cache: {
            const std::string vs=std::string(shaders::lit_shadow)+"\nLitShadowRaster main(uint v:SV_VertexID,uint i:SV_InstanceID){return lit_shadow_vs(v,i);}";
            const std::string ps=std::string(shaders::lit_shadow)+"\nvoid main(LitShadowRaster i){lit_shadow_ps(i);}";
            rhi::PipelineDesc desc;desc.kind=rhi::PipelineKind::vertex;desc.source.vs=vs.c_str();desc.source.ps=ps.c_str();desc.name="Cached shadow depth";
            require(gfxDrawStateSetDepthStencilTarget(desc.draw_state,DXGI_FORMAT_D32_FLOAT),"Set cached shadow precision");
            require(gfxDrawStateSetDepthFunction(desc.draw_state,D3D12_COMPARISON_FUNC_GREATER),"Set shadow reversed depth");
            require(gfxDrawStateSetCullMode(desc.draw_state,D3D12_CULL_MODE_NONE),"Set two-sided shadow coverage");
            submit(group,shadow_depth,desc);
            break;
        }
        case group_cluster_inspect: submit(group,cluster_inspect,shaders::lit_clustered,"lit_cluster_inspect_cs","Direct receiver support inspection");break;
        case group_probe_inspection:
            submit(group,inspect_update,shaders::lit_ddgi,"DDGIUpdateInspect","DDGI sampled-hit inspection");
            submit(group,inspect_pass,shaders::lit_ddgi,"DDGIInspect","DDGI receiver inspection");
            break;
        case group_probe_coverage: submit_probe_debug(group,probe_debug_surface,false);break;
        case group_probe_overlay: submit_probe_debug(group,probe_debug_geometry,true);break;
        case group_end: break;
        }
        submitted|=group;
    }
    Result<Preparation> preparation(const ViewSettings& view) {
        if(preparation_failure)return std::unexpected(*preparation_failure);
        const auto wanted=groups(view);
        try {
            for(std::uint32_t group=group_base;group!=group_end;group<<=1)
                if((wanted&group)&&!(submitted&group))submit_group(static_cast<Group>(group));
            for(auto it=pending.begin();it!=pending.end();) {
                auto taken=context.take(it->job);
                if(!taken){++it;continue;}
                if(!*taken)throw Error{taken->error(),false};
                *it->output=std::move(**taken);it=pending.erase(it);
            }
        } catch(const Error& error) {
            // Built-in sources do not change within a process; retrying cannot succeed.
            pending.clear();preparation_failure=error;preparation_failure->terminal=false;
            return std::unexpected(*preparation_failure);
        }
        Preparation result;
        for(std::uint32_t i=0;i<group_sizes.size();++i)if(wanted&(1u<<i))result.total+=group_sizes[i];
        result.done=result.total-static_cast<std::uint32_t>(std::ranges::count_if(pending,[&](const PendingPipeline& item){return (wanted&item.group)!=0;}));
        result.ready=result.done==result.total;
        return result;
    }
    // Owner-thread resources shared by every Lit frame, created at the first encoded frame.
    void resources() {
        if(sampler)return;
        sampler=gfxCreateSamplerState(gfx,D3D12_FILTER_MIN_MAG_MIP_LINEAR);allocation(bool(sampler));
        sky_sampler=gfxCreateSamplerState(gfx,D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_WRAP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        allocation(bool(sky_sampler));
        shadow_sampler=gfxCreateSamplerState(gfx,D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT,D3D12_COMPARISON_FUNC_GREATER_EQUAL);
        allocation(bool(shadow_sampler));
        for(auto& query:queries){query=gfxCreateTimestampQuery(gfx);allocation(bool(query));}
        // Rays[0..7], cluster statistics[8..10], optional histogram[11..77], shadow missing taps[78], plane fallbacks[81],
        // receiver view-Z minimum (complemented order) and maximum (order)[82..83], sun trace pixels[84].
        ray_counters=gfxCreateBuffer<std::uint32_t>(gfx,85);allocation(bool(ray_counters));
        for(auto& readback:ray_readback){readback=gfxCreateBuffer(gfx,85*sizeof(std::uint32_t),nullptr,kGfxCpuAccess_Read);allocation(bool(readback));}
        brdf_lut=texture(64,64,DXGI_FORMAT_R16G16_FLOAT,4);
        const auto upload_lut=[&](GfxTexture& target,std::uint32_t width,std::uint32_t height,DXGI_FORMAT format,std::span<const unsigned char> bytes) {
            target=texture(width,height,format,0);
            auto upload=gfxCreateBuffer(gfx,bytes.size(),bytes.data(),kGfxCpuAccess_Write);allocation(bool(upload));
            const auto copied=gfxCommandCopyBufferToTexture(gfx,target,upload);
            const auto retired=gfxDestroyBuffer(gfx,upload);require(copied,"Upload SMAA lookup");require(retired,"Retire SMAA upload");
        };
        upload_lut(smaa_area,smaa_lut::area_width,smaa_lut::area_height,DXGI_FORMAT_R8G8_UNORM,smaa_lut::area_bytes());
        upload_lut(smaa_search,smaa_lut::search_width,smaa_lut::search_height,DXGI_FORMAT_R8_UNORM,smaa_lut::search_bytes());
        struct LtcCell { float inverse[4],amplitude[2]; };
        static_assert(sizeof(LtcCell)*ltc_lut::size*ltc_lut::size==sizeof(ltc_lut::table));
        ltc=gfxCreateBuffer<LtcCell>(gfx,ltc_lut::size*ltc_lut::size,ltc_lut::table.data());allocation(bool(ltc));
    }
    void retire_targets() {
        retire(probe_visibility_cache);probe_visibility_valid=false;
        for(auto& t:gbuffer_textures)retire(t);
        for(auto* t:{&indirect_half,&hdr,&indirect,&ao_texture,&ao_raw,&ao_depth,&reflection_trace,&reflection_hit_trace,&tone_color,&smaa_edges,&smaa_weights,&smaa_output})retire(*t);
        for(auto* pair:{&history_depth,&history_normal,&history_geometry_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_moments,&reflection_scratch,&ao_history,&bloom,&smaa_spatial,&history_surface_id})for(auto& t:*pair)retire(t);
        for(auto& t:pyramid)retire(t);
        for(auto* b:{&cluster_depth,&cluster_count,&cluster_source,&reflection_work,&reflection_counter,&reflection_arguments,&probe_fallback,&probe_fallback_arguments,
                     &sun_trace_list,&sun_trace_arguments})retire(*b);
    }
    void clusters() {
        cluster_origin={screen_dispatch[0],screen_dispatch[1]};
        cluster_tiles={(screen_dispatch[2]-screen_dispatch[0]+15)/16,(screen_dispatch[3]-screen_dispatch[1]+15)/16};
        const auto tiles=std::uint64_t(cluster_tiles[0])*cluster_tiles[1];
        if(!tiles || cluster_count.getSize()>=tiles*4*sizeof(std::uint32_t))return;
        retire(cluster_depth);retire(cluster_count);retire(cluster_source);
        cluster_depth=gfxCreateBuffer<std::array<float,2>>(gfx,tiles);allocation(bool(cluster_depth));
        cluster_count=gfxCreateBuffer<std::uint32_t>(gfx,tiles*4);allocation(bool(cluster_count));
        cluster_source=gfxCreateBuffer<std::uint32_t>(gfx,tiles*4*64);allocation(bool(cluster_source));
    }
    template<class T> GfxBuffer shadow_upload(DeviceTable<T>& table,std::vector<T>& previous,const std::vector<T>& values) {
        const bool changed=previous.size()!=values.size()||
            (!values.empty()&&std::memcmp(previous.data(),values.data(),values.size()*sizeof(T))!=0);
        if(changed)previous=values;
        return table.write(gfx,context.info().frames_in_flight,values,changed,"Shadow batch");
    }
    // The newest completed receiver depth of the current camera epoch. gfx
    // records frame N only after frame N - frames_in_flight completed.
    std::optional<ShadowReceiverDepth> receiver_depth() const {
        const auto recording=gfxGetRecordingFrameSequence(gfx);const std::uint64_t in_flight=context.info().frames_in_flight;
        if(recording<=in_flight)return {};
        std::size_t newest=readback_count;
        for(std::size_t slot=0;slot<readback_count;++slot)if(const auto s=readback_sequences[slot];s&&s<=recording-in_flight&&
            (newest==readback_count||s>readback_sequences[newest]))newest=slot;
        if(newest==readback_count||readback_depth_epochs[newest]!=depth_epoch)return {};
        const auto* counter=gfxBufferGetData<std::uint32_t>(gfx,ray_readback[newest]);
        if(!counter||!counter[82]||!counter[83])return {};
        const auto decode=[](std::uint32_t v){return std::bit_cast<float>(v^((v&0x80000000u)!=0?0x80000000u:~0u));};
        return ShadowReceiverDepth{decode(~counter[82]),decode(counter[83])};
    }
    void prepare_shadows() {
        if(!shadow_static||!shadow_dynamic) {
            shadow_cache.invalidate();
            constexpr float clear[4]{};
            for(auto* t:{&shadow_static,&shadow_dynamic})if(!*t){
                *t=gfxCreateTexture2D(gfx,shadow_atlas_extent,shadow_atlas_extent,DXGI_FORMAT_D32_FLOAT,1,clear);allocation(bool(*t));}
        }
    }
    void shadow_update(const ShadowFramePlan& plan,const SceneLitDraws& scene_draws,ShadowSample& sample) {
        auto& gpu=shadow_tables;
        gpu.faces=shadow_upload(gpu.face_table,gpu.previous_faces,plan.faces);
        gpu.lights=shadow_upload(gpu.light_table,gpu.previous_lights,plan.lights);
        if(plan.updates.empty())return;
        const auto& instance_draws=scene_draws.instance_draws;
        shadow_instance_indices.clear();
        for(const auto& job:plan.updates) {
            const auto first=shadow_instance_indices.size();
            for(const auto index:job.instances) {
                invalid(index<instance_draws.size(),"Shadow caster missing from current geometry snapshot");
                shadow_instance_indices.push_back(index);
            }
            // Depth-only rasterization is order independent: group each job's casters
            // by geometry so every run of one mesh is a single instanced draw.
            std::sort(shadow_instance_indices.begin()+first,shadow_instance_indices.end(),[&](std::uint32_t a,std::uint32_t b) {
                return std::pair{instance_draws[a],a}<std::pair{instance_draws[b],b};
            });
        }
        gpu.instances=shadow_upload(gpu.instance_table,gpu.previous_instances,shadow_instance_indices);
        const auto p=shadow_depth.program();bind_materials(gfx,p,*material_frame);
        bind(p,"lit_instances",gpu_instances);bind(p,"lit_shadow_instances",gpu.instances);
        require(gfxCommandBindKernel(gfx,shadow_depth.kernel()),"Bind cached shadow raster");
        std::size_t first=0;
        for(const auto& job:plan.updates) {
            const auto target=job.layer==ShadowLayer::stationary?shadow_static:shadow_dynamic;
            const auto& r=job.rect;
            const D3D12_RECT rect{static_cast<LONG>(r[0]),static_cast<LONG>(r[1]),static_cast<LONG>(r[0]+r[2]),static_cast<LONG>(r[1]+r[3])};
            require(gfxCommandClearImageRects(gfx,target,&rect,1),"Clear invalidated shadow tile");
            require(gfxCommandBindDepthStencilTarget(gfx,target),"Bind shadow tile depth");
            require(gfxCommandSetViewport(gfx,float(r[0]),float(r[1]),float(r[2]),float(r[3])),"Set shadow tile viewport");
            require(gfxCommandSetScissorRect(gfx,r[0],r[1],r[2],r[3]),"Set shadow tile scissor");
            bind(p,"lit_shadow_view_projection",job.view_projection);
            const auto last=first+job.instances.size();
            for(std::size_t at=first;at<last;) {
                const auto draw_index=instance_draws[shadow_instance_indices[at]];
                const auto& draw=scene_draws.draws[draw_index];std::size_t end=at+1;
                while(end<last&&instance_draws[shadow_instance_indices[end]]==draw_index)++end;
                bind(p,"lit_triangles",draw.triangles);bind(p,"lit_shadow_first_instance",static_cast<std::uint32_t>(at));
                require(gfxCommandDraw(gfx,draw.vertex_count,static_cast<std::uint32_t>(end-at)),"Draw invalidated shadow casters");
                ++sample.draws;at=end;
            }
            first=last;
        }
        require(gfxCommandBindDepthStencilTarget(gfx,{}),"Unbind cached shadow atlas before sampling");
    }
    void bind_shadows(GfxProgram p) {
        bind(p,"lit_shadow_sampler",shadow_sampler);bind(p,"lit_shadow_atlas_extent",shadow_atlas_extent);
        // Without a caster nothing reads the atlas; empty handles drop bindings to retired atlases.
        const bool on=shadows_cached;
        bind(p,"lit_shadow_faces",on?shadow_tables.faces:GfxBuffer{});bind(p,"lit_shadow_lights",on?shadow_tables.lights:GfxBuffer{});
        bind(p,"lit_shadow_static",on?shadow_static:GfxTexture{});bind(p,"lit_shadow_dynamic",on?shadow_dynamic:GfxTexture{});
    }
    void targets(ui::PixelExtent next) {
        if(extent==next)return;
        retire_targets();extent=next;
        const auto w=next.width,h=next.height;
        for(std::size_t i=0;i<gbuffer_textures.size();++i)gbuffer_textures[i]=texture(w,h,i==4?DXGI_FORMAT_R32_FLOAT:(i==5?DXGI_FORMAT_R16G16_FLOAT:(i==6?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R16G16B16A16_FLOAT)),i>=4?4:8);
        for(auto* t:{&hdr,&indirect,&reflection_trace,&tone_color,&smaa_output})*t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        indirect_half=texture((w+1)/2,(h+1)/2,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        // Overflow beyond a quarter of the receivers gathers inline in the upsampling pass.
        probe_fallback_capacity=std::uint32_t((std::uint64_t(w)*h+3)/4);
        probe_fallback=gfxCreateBuffer<std::uint32_t>(gfx,std::uint64_t(probe_fallback_capacity)+1);
        probe_fallback_arguments=gfxCreateBuffer<std::uint32_t>(gfx,3);
        allocation(bool(probe_fallback)&&bool(probe_fallback_arguments));
        sun_trace_list=gfxCreateBuffer<std::uint32_t>(gfx,std::uint64_t(w)*h);sun_trace_arguments=gfxCreateBuffer<std::uint32_t>(gfx,3);
        allocation(bool(sun_trace_list)&&bool(sun_trace_arguments));
        reflection_hit_trace=texture(w,h,DXGI_FORMAT_R32G32_UINT,8);
        ao_texture=texture(w,h,DXGI_FORMAT_R16_FLOAT,2);
        ao_raw=texture(w,h,DXGI_FORMAT_R16_FLOAT,2);
        ao_depth=gfxCreateTexture2D(gfx,w,h,DXGI_FORMAT_R32_FLOAT,5);allocation(bool(ao_depth));
        for(auto* pair:{&history_normal,&taa_history,&reflection_history,&reflection_scratch,&smaa_spatial})for(auto& t:*pair)t=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        for(auto& t:reflection_hit_history)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        for(auto& t:reflection_moments)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
        for(auto& t:history_surface_id)t=texture(w,h,DXGI_FORMAT_R32G32_UINT,8);
        for(auto& t:history_geometry_normal)t=texture(w,h,DXGI_FORMAT_R16G16_FLOAT,4);
        for(auto& t:ao_history)t=texture(w,h,DXGI_FORMAT_R16G16_FLOAT,4);
        smaa_edges=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4);smaa_weights=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,4);
        for(auto& t:history_depth)t=texture(w,h,DXGI_FORMAT_R32_FLOAT,4);
        for(auto& t:bloom)t=texture((w+3)/4,(h+3)/4,DXGI_FORMAT_R16G16B16A16_FLOAT,8);
        // Logical level zero is the existing FP32 primary depth, not a (z,z) copy.
        // Keep the original twelve logical levels, including the 1x1 fallback.
        std::uint32_t pw=(w+1)/2,ph=(h+1)/2;pyramid_count=0;
        while(pyramid_count<11){pyramid[pyramid_count++]=texture(pw,ph,DXGI_FORMAT_R32G32_FLOAT,8);if(pw==1&&ph==1)break;pw=(pw+1)/2;ph=(ph+1)/2;}
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
            retire(probe_presentation);
            retire(probe_response);
            retire(probe_age);
            retire(probe_sample_cursor);
            for(auto& t:probe_irradiance)t=texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16);
            probe_presentation=texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,16);
            probe_response=texture(w,h,DXGI_FORMAT_R32G32_UINT,8);
            for(auto& t:probe_distance)t=texture(w,h,DXGI_FORMAT_R32G32_FLOAT,8);
            // World-space relocation must retain the range of the supported grid.
            for(auto& t:probe_offsets)t=texture(probe_field.width,probe_field.height,DXGI_FORMAT_R32G32B32A32_FLOAT,16);
            probe_age=texture(probe_field.width,probe_field.height,DXGI_FORMAT_R32_UINT,4);
            probe_sample_cursor=texture(probe_field.width,probe_field.height,DXGI_FORMAT_R32G32_UINT,8);
            probe_field.invalidate();probe_field.describe(settings,focus);
            probe_parity=0;probe_response_epoch=0;probe_geometry_pending=0;probe_sequence=0;
        }
        probe_field.plan(settings,stats);
        const auto rays=std::uint64_t(probe_field.scheduled)*settings.probe_rays;
        invalid(rays<=std::numeric_limits<std::uint32_t>::max(),"DDGI ray scratch exceeds shader index range");
        if(probe_ray_hits.getSize()<rays*sizeof(ProbeRayHitStorage)) {
            retire(probe_ray_hits);
            probe_ray_hits=gfxCreateBuffer<ProbeRayHitStorage>(gfx,rays);allocation(bool(probe_ray_hits));
        }
    }
    void bind_probes(GfxProgram p,bool newest,bool presentation=false) {
        bind(p,"lit_probe_volumes",newest?probe_field.current:probe_field.previous);
        bind(p,"lit_probe_current_volumes",probe_field.current);
        bind(p,"lit_probe_pool",std::array<std::uint32_t,4>{probe_field.width,probe_field.total,0,probe_field.valid?1u:0u});
        bind(p,"lit_probe_update",probe_update_settings);
        bind(p,"lit_probe_history_mode",probe_history_mode);
        bind(p,"lit_probe_sequence",probe_sequence);bind(p,"lit_probe_age_output",probe_age);
        bind(p,"lit_probe_sample_cursor",probe_sample_cursor);
        bind(p,"lit_probe_response",probe_response);bind(p,"lit_probe_response_epoch",probe_response_epoch);
        bind(p,"lit_probe_history",probe_history);bind(p,"lit_probe_controls",probe_controls);
        bind(p,"lit_probe_scene_min",probe_scene_min);bind(p,"lit_probe_scene_max",probe_scene_max);
        const auto read=newest?probe_parity:1-probe_parity;
        bind(p,"lit_probe_irradiance_previous",presentation?probe_presentation:probe_irradiance[read]);bind(p,"lit_probe_distance_previous",probe_distance[read]);
        bind(p,"lit_probe_offsets_previous",probe_offsets[read]);bind(p,"lit_probe_presentation",probe_presentation);
        bind(p,"lit_probe_irradiance_output",probe_irradiance[probe_parity]);
        bind(p,"lit_probe_distance_output",probe_distance[probe_parity]);bind(p,"lit_probe_offsets_output",probe_offsets[probe_parity]);
    }
    void common(GfxProgram p,GfxTexture depth,std::uint32_t visibility_counter=0) {
        bind_materials(gfx,p,*material_frame);
        bind(p,"raster_viewport_transform",raster_transform);
        bind(p,"lit_size_frame",size_frame);bind(p,"lit_viewport",viewport);bind(p,"lit_screen_dispatch",screen_dispatch);bind(p,"lit_view_projection",view_projection);
        bind(p,"lit_inverse_view_projection",inverse_view_projection);bind(p,"lit_previous_view_projection",previous_view_projection);
        bind(p,"lit_previous_inverse_view_projection",previous_inverse_view_projection);
        bind(p,"lit_far_depth",far_depth);
        bind(p,"lit_camera_view",camera_view);bind(p,"lit_cluster_tiles",cluster_tiles);bind(p,"lit_cluster_origin",cluster_origin);
        bind(p,"lit_environment",environment);bind(p,"lit_controls",controls);
        bind(p,"lit_sky",sky);bind(p,"lit_sky_radiance",sky_radiance);bind(p,"lit_sky_background",sky_background);
        bind(p,"lit_sky_cube",sky_cube);bind(p,"lit_sky_sampler",sky_sampler);
        bind(p,"lit_temporal",temporal);bind(p,"lit_jitter",jitter);bind(p,"lit_effect_flags",effect_flags);
        bind(p,"lit_source_counts",source_counts);bind(p,"lit_hit_candidates",hit_candidates);
        bind(p,"lit_history_invalid",history_invalid);
        bind(p,"lit_instances",gpu_instances);bind(p,"lit_rt_triangles",rt_triangles);bind(p,"lit_closed_slots",rt_closed_slots);
        bind(p,"lit_sources",gpu_sources);bind(p,"lit_light_proposals",gpu_proposals);
        bind(p,"lit_light_cells",gpu_light_cells);bind(p,"lit_spatial_light_proposals",gpu_spatial_proposals);
        bind(p,"lit_spatial_source_slots",gpu_spatial_source_slots);bind(p,"lit_light_grid_origin",light_frame.spatial_origin);
        bind(p,"lit_light_grid_counts",light_grid_counts_gpu);
        bind(p,"lit_scene",tlas);bind(p,"lit_depth",depth);bind(p,"lit_normal_roughness",gbuffer_textures[0]);
        bind(p,"lit_ray_counters",ray_counters);
        bind(p,"lit_visibility_counter",visibility_counter);
        bind(p,"lit_base_metallic",gbuffer_textures[1]);bind(p,"lit_emissive",gbuffer_textures[2]);bind(p,"lit_motion",gbuffer_textures[3]);
        bind(p,"lit_previous_depth",gbuffer_textures[4]);bind(p,"lit_surface_id",gbuffer_textures[6]);
        bind(p,"lit_history_surface_id",history_surface_id[1-parity]);
        bind(p,"lit_geometry_normal",gbuffer_textures[5]);bind(p,"lit_history_geometry_normal",history_geometry_normal[1-parity]);
        bind(p,"lit_linear_sampler",sampler);bind(p,"lit_history_depth",history_depth[1-parity]);bind(p,"lit_history_normal",history_normal[1-parity]);
        bind(p,"lit_hdr_source",hdr);bind(p,"lit_brdf_lut",brdf_lut);
    }
    void dispatch(const rhi::Pipeline& pipeline,GfxTexture depth,std::uint32_t x,std::uint32_t y=1,std::uint32_t z=1,std::uint32_t visibility_counter=0) {
        common(pipeline.program(),depth,visibility_counter);
        require(gfxCommandBindKernel(gfx,pipeline.kernel()),"Bind Scene Lit compute");
        require(gfxCommandDispatch(gfx,x,y,z),"Dispatch Scene Lit compute");
    }
    void dispatch_screen(const rhi::Pipeline& pipeline,GfxTexture depth,std::uint32_t visibility_counter=0) {
        const auto width=screen_dispatch[2]-screen_dispatch[0],height=screen_dispatch[3]-screen_dispatch[1];
        if(width&&height)dispatch(pipeline,depth,(width+7)/8,(height+7)/8,1,visibility_counter);
    }
    // After a history reset both parities cover the whole window once, so texels outside the
    // viewport keep the zero/far values neighbor reads expect; later frames touch only the viewport.
    void dispatch_window(const rhi::Pipeline& pipeline,GfxTexture depth) {
        if(!full_window_frames){dispatch_screen(pipeline,depth);return;}
        const auto viewport_rect=screen_dispatch;screen_dispatch={0,0,size_frame[0],size_frame[1]};
        dispatch_screen(pipeline,depth);screen_dispatch=viewport_rect;
    }
    void begin(std::uint32_t pass,bool enabled=true) {
        if(enabled)encoded_pass_mask|=1u<<pass;
        require(gfxCommandBeginTimestampQuery(gfx,queries[pass]),"Begin Scene Lit timestamp");
    }
    void end(std::uint32_t pass) {require(gfxCommandEndTimestampQuery(gfx,queries[pass]),"End Scene Lit timestamp");}
    void reset(bool reset_probes=true) {
        probe_visibility_valid=false;
        if(history_valid||(reset_probes&&probe_field.valid)) {
            history_valid=false;if(reset_probes){probe_field.invalidate();probe_parity=0;probe_response_epoch=0;probe_geometry_pending=0;probe_sequence=0;}reset_events++;
        }
    }
    void publish_gpu_sample(FrameStats& stats) {
        const auto sample=gfxTimestampQueryGetSample(gfx,queries[0]);
        if(sample.frame_sequence==0)return;
        std::array<float,8> timings{};
        std::array<float,2> ddgi_timings{},ddgi_update_timings{};
        float cluster_timing{},shadow_timing{},shading_timing{};
        for(std::size_t i=0;i<queries.size();i++) {
            const auto pass=gfxTimestampQueryGetSample(gfx,queries[i]);
            if(pass.frame_sequence!=sample.frame_sequence)return;
            const auto ms=static_cast<float>(pass.duration_ms);
            if(i<timings.size())timings[i]=ms;
            else if(i<10)ddgi_timings[i-8]=ms;
            else if(i<12)ddgi_update_timings[i-10]=ms;
            else if(i==12)cluster_timing=ms;
            else if(i==13)shadow_timing=ms;
            else shading_timing=ms;
        }
        for(std::size_t slot=0;slot<readback_count;slot++) {
            if(readback_sequences[slot]!=sample.frame_sequence)continue;
            const auto* counter=gfxBufferGetData<std::uint32_t>(gfx,ray_readback[slot]);
            if(!counter)return;
            const auto& shadow=readback_shadows[slot];
            stats.scene_lit_gpu_ms=timings;stats.scene_lit_ddgi_gpu_ms=ddgi_timings;stats.scene_lit_gpu_sample_sequence=sample.frame_sequence;
            stats.scene_lit_ddgi_update_gpu_ms=ddgi_update_timings;
            stats.scene_lit_direct_cluster_gpu_ms=cluster_timing;stats.scene_lit_direct_shading_gpu_ms=shading_timing;
            stats.scene_lit_direct_cluster_cells=counter[8];stats.scene_lit_direct_cluster_overflows=counter[9];
            stats.scene_lit_direct_cluster_max_sources=counter[10];
            std::copy_n(counter+11,stats.scene_lit_direct_receiver_histogram.size(),stats.scene_lit_direct_receiver_histogram.begin());
            stats.scene_lit_direct_receiver_max_sources=counter[77];
            stats.scene_lit_shadow_cache_gpu_ms=shadow_timing;
            stats.scene_lit_shadow_cache_faces=shadow.plan.requested_faces;stats.scene_lit_shadow_cache_reused_faces=shadow.plan.reused_faces;
            stats.scene_lit_shadow_cache_static_updates=shadow.plan.static_updates;stats.scene_lit_shadow_cache_dynamic_updates=shadow.plan.dynamic_updates;
            stats.scene_lit_shadow_cache_draws=shadow.draws;stats.scene_lit_shadow_cache_resolution_downgrades=shadow.plan.resolution_downgrades;
            stats.scene_lit_shadow_cache_dropped_lights=shadow.plan.dropped_lights;stats.scene_lit_shadow_cache_near_plane_faces=shadow.plan.near_plane_faces;
            stats.scene_lit_shadow_cache_missing_taps=counter[78];
            stats.scene_lit_shadow_cache_deferred_faces=shadow.plan.deferred_faces;stats.scene_lit_shadow_cache_deferral_frames=shadow.plan.deferral_frames;
            stats.scene_lit_shadow_cache_atlas_cells=shadow.plan.atlas_cells;stats.scene_lit_shadow_cache_pending_lights=shadow.plan.pending_lights;
            stats.scene_lit_shadow_cache_plane_fallbacks=counter[81];
            stats.scene_lit_gpu_pass_mask=readback_pass_masks[slot];
            stats.scene_lit_gpu_sample_frame_id=readback_frame_ids[slot];
            const auto ray_count=[&](std::size_t i){return counter[i]|(std::uint64_t(counter[i+4])<<32);};
            stats.scene_lit_shadow_rays=ray_count(0);
            for(std::size_t i=0;i<ddgi_timings.size();++i){stats.scene_lit_ddgi_visibility_rays[i]=ray_count(i+2);stats.scene_lit_shadow_rays+=ray_count(i+2);}
            stats.scene_lit_probe_rays=ray_count(1);
            stats.scene_lit_gpu_timing_valid=true;
            completed_sequence=std::max(completed_sequence,sample.frame_sequence);
            if(inspection && inspection->metadata[slot].sequence==sample.frame_sequence) {
                auto& state=*inspection;
                if(!state.published || state.published->sequence!=sample.frame_sequence) {
                    if(const auto* data=gfxBufferGetData<ProbeContribution>(gfx,state.readback[slot])) {
                        auto value=std::make_shared<ProbeInspection>(state.metadata[slot]);
                        std::copy_n(data,value->probes.size(),value->probes.begin());value->receiver=data[value->probes.size()];
                        if(const auto* rays=gfxBufferGetData<ProbeInspection::Ray>(gfx,state.ray_readback[slot]))
                            std::copy_n(rays,value->ray_samples.size(),value->ray_samples.begin());
                        state.published=std::move(value);
                    }
                }
                stats.scene_lit_probe_inspection=state.published;
            }
            return;
        }
    }
    void prepare_inspection(const Frame& frame) {
        const auto& settings=frame.view.lit;
        if(!settings.probe_inspect || !settings.ddgi || !probe_field.total)return;
        if(!inspection) {
            inspection=std::make_unique<InspectionState>();
            constexpr auto bytes=sizeof(ProbeContribution)*(probe_volume_limit*8+1);
            inspection->output=gfxCreateBuffer<ProbeContribution>(gfx,probe_volume_limit*8+1);allocation(bool(inspection->output));
            for(auto& buffer:inspection->readback) {
                buffer=gfxCreateBuffer(gfx,bytes,nullptr,kGfxCpuAccess_Read);allocation(bool(buffer));
            }
            inspection->rays=gfxCreateBuffer<ProbeInspection::Ray>(gfx,256);allocation(bool(inspection->rays));
            for(auto& buffer:inspection->ray_readback) {
                buffer=gfxCreateBuffer(gfx,256*sizeof(ProbeInspection::Ray),nullptr,kGfxCpuAccess_Read);allocation(bool(buffer));
            }
        }
        inspection->traced_probe=~0u;
        const auto& previous=inspection->published;
        float largest_weight=0;
        if(settings.probe_inspect_source>=0) {
            const auto requested=static_cast<std::uint32_t>(settings.probe_inspect_source);
            if(requested<probe_field.total)inspection->traced_probe=requested;
        } else if(previous && previous->scene_identity==frame.scene_identity && previous->pixel==settings.probe_inspect_pixel)
            for(const auto& probe:previous->probes)if(probe.identity[2]==6 && probe.identity[1]<probe_field.total && probe.weights[3]>largest_weight) {
                inspection->traced_probe=probe.identity[1];largest_weight=probe.weights[3];
            }
        require(gfxCommandClearBuffer(gfx,inspection->rays),"Clear optional probe-hit records");
    }
    void inspect_probe(const Frame& frame,GfxTexture depth,const FrameStats& stats) {
        const auto& settings=frame.view.lit;
        if(!settings.probe_inspect || !settings.ddgi || !probe_field.total)return;
        const auto slot=sequence%readback_count;
        auto& state=*inspection;
        // An unavailable slot drops this diagnostic sample; never wait for it.
        if(state.metadata[slot].sequence>completed_sequence)return;
        const auto p=inspect_pass.program();bind_probes(p,true,true);
        bind(p,"lit_probe_transport",probe_irradiance[probe_parity]);
        bind(p,"lit_probe_age",probe_age);bind(p,"lit_probe_inspect_pixel",settings.probe_inspect_pixel);
        bind(p,"lit_probe_inspection",state.output);
        dispatch(inspect_pass,depth,1,1,1,3);
        require(gfxCommandCopyBuffer(gfx,state.readback[slot],state.output),"Publish DDGI inspection asynchronously");
        require(gfxCommandCopyBuffer(gfx,state.ray_readback[slot],state.rays),"Publish DDGI hit samples asynchronously");
        auto& metadata=state.metadata[slot];metadata={};
        metadata.sequence=gfxGetRecordingFrameSequence(gfx);metadata.frame_id=frame.ui_frame.id;
        metadata.scene_identity=frame.scene_identity;metadata.pixel=settings.probe_inspect_pixel;
        metadata.history=settings.probe_history;metadata.rays=settings.probe_rays;metadata.updates=probe_field.scheduled;
        metadata.requested_probe=settings.probe_inspect_source;
        metadata.volumes=stats.scene_lit_probe_volumes;
        metadata.traced_probe=state.traced_probe;
    }
    std::uint64_t resource_bytes() const {
        std::uint64_t result=0;
        const auto add_texture=[&](GfxTexture t){if(!t)return;std::uint32_t bytes=8;
            if(t.getFormat()==DXGI_FORMAT_R32G32B32A32_FLOAT)bytes=16;
            else if(t.getFormat()==DXGI_FORMAT_R16_FLOAT)bytes=2;
            else if(t.getFormat()==DXGI_FORMAT_R8_UNORM||t.getFormat()==DXGI_FORMAT_R8_UINT)bytes=1;
            else if(t.getFormat()==DXGI_FORMAT_R8G8_UNORM)bytes=2;
            else if(t.getFormat()==DXGI_FORMAT_R32_FLOAT||t.getFormat()==DXGI_FORMAT_R32_UINT||t.getFormat()==DXGI_FORMAT_D32_FLOAT||t.getFormat()==DXGI_FORMAT_R16G16_FLOAT||t.getFormat()==DXGI_FORMAT_R8G8B8A8_UNORM)bytes=4;
            for(std::uint32_t level=0;level<t.getMipLevels();++level)
                result+=std::uint64_t(std::max(t.getWidth()>>level,1u))*std::max(t.getHeight()>>level,1u)*t.getDepth()*bytes;};
        for(auto t:gbuffer_textures)add_texture(t);
        for(auto t:{indirect_half,hdr,indirect,ao_texture,ao_raw,ao_depth,reflection_trace,reflection_hit_trace,brdf_lut,tone_color,smaa_edges,smaa_weights,smaa_output,smaa_area,smaa_search,probe_presentation,probe_response,probe_age,probe_sample_cursor,probe_debug_depth,shadow_static,shadow_dynamic,sky_background,sky_cube})add_texture(t);
        for(const auto* pair:{&history_depth,&history_normal,&history_geometry_normal,&taa_history,&reflection_history,&reflection_hit_history,&reflection_moments,&reflection_scratch,&ao_history,&smaa_spatial,&bloom,&history_surface_id,&probe_irradiance,&probe_distance,&probe_offsets})for(auto t:*pair)add_texture(t);
        for(auto t:pyramid)add_texture(t);
        result+=instance_table.bytes();light_tables.each([&](const auto& table){result+=table.bytes();});
        for(auto b:{cluster_depth,cluster_count,cluster_source,probe_fallback,probe_fallback_arguments,rt_triangles,rt_closed_slots,reflection_work,reflection_counter,reflection_arguments,probe_ray_hits,probe_visibility_cache,ray_counters,ltc,
                    sun_trace_list,sun_trace_arguments})result+=b.getSize();
        for(auto b:ray_readback)result+=b.getSize();
        result+=shadow_tables.face_table.bytes()+shadow_tables.light_table.bytes()+shadow_tables.instance_table.bytes();
        if(inspection) {result+=inspection->output.getSize()+inspection->rays.getSize();for(auto b:inspection->readback)result+=b.getSize();for(auto b:inspection->ray_readback)result+=b.getSize();}
        for(const auto& [key,g]:geometries){(void)key;result+=g.positions.getSize()+gfxBottomLevelAccelerationStructureGetDataSize(gfx,g.blas);}
        if(tlas)result+=gfxTopLevelAccelerationStructureGetDataSize(gfx,tlas);
        return result;
    }

    bool acceleration(const Frame& frame) {
        const auto count=frame.meshes.size();
        shadow_casters.clear();shadow_changes.clear();
        if(shadows_cached)shadow_casters.reserve(count);
        material_changed=false;coverage_changed=false;
        emitter_changed=false;
        bool geometry_changed=false,topology_changed=false,transform_changed=false;
        // Indices whose GpuInstance changed; only this range is copied to the GPU.
        std::size_t dirty_first=instances.size()!=count?0:count,dirty_last=instances.size()!=count?count:0;
        const auto dirty=[&](std::size_t index){dirty_first=std::min(dirty_first,index);dirty_last=std::max(dirty_last,index+1);};
        if(!tlas){tlas=gfxCreateTopLevelAccelerationStructure(gfx);allocation(bool(tlas));}
        const auto key_of=[&](std::size_t index){const auto stable=frame.meshes[index].stable_key;return ObjectKey{stable?stable:index,stable==0};};
        // A slot is reused only for the same key and mesh at the same index. Its
        // surfaces were compared against material_revision, and the MaterialFrame
        // keeps a revision only while every description stays the same.
        const bool reuse=std::exchange(resolved_complete,false);
        const bool materials_same=reuse&&material_frame->revision==material_revision;
        bool remapped=!reuse||resolved.size()!=count;
        if(reuse)resolved.resize(count);else resolved.assign(count,{});
        // A steady frame has the same objects, meshes and materials at every index
        // as the last complete pass: used flags, coverage and offsets stay valid
        // and nothing retires.
        bool steady=materials_same&&!remapped;
        for(std::size_t i=0;steady&&i<count;++i)
            steady=resolved[i].key==key_of(i)&&resolved[i].geometry->source==frame.meshes[i].geometry;
        if(!steady) {
            for(auto& [key,g]:geometries){(void)key;g.used=false;g.next_opaque=true;}
            for(auto& [key,o]:objects){(void)key;o.used=false;}
            for(std::size_t i=0;i<count;++i) {
                const auto& mesh=frame.meshes[i];auto& slot=resolved[i];const auto key=key_of(i);
                if(!slot.object||slot.key!=key||slot.geometry->source!=mesh.geometry) {
                    remapped|=slot.key!=key;
                    auto [at,added]=geometries.try_emplace(mesh.geometry.get());
                    if(added)at->second.source=mesh.geometry;
                    slot={key,nullptr,&at->second};
                }
                bool opaque=true;
                for(std::size_t j=0;j<mesh.geometry->materials().size();++j)
                    opaque &= material_frame->surface(i,j).parameters.coverage==material::Coverage::opaque;
                slot.geometry->used=true;slot.geometry->next_opaque &= opaque;
            }
        }
        // Aggregate coverage in the owning geometry cache. Repeated frames and
        // instances share it without allocating a second per-frame hash table.
        if(!steady)for(auto& [key,g]:geometries) {
            (void)key;if(!g.used)continue;
            if(!g.blas) {
                g.volumes=triangle_volumes(*g.source);const auto vertices=g.source->vertices();
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
        instances.resize(count);
        for(std::size_t index=0;index<count;index++) {
            const auto& mesh=frame.meshes[index];auto& slot=resolved[index];
            // Only a reused slot keeps its object; the coverage pass cleared every other one.
            const bool same_slot=slot.object!=nullptr;
            bool inserted=false;
            if(!same_slot){auto [found,added]=objects.try_emplace(slot.key);slot.object=&found->second;inserted=added;}
            auto& object=*slot.object;const auto& g=*slot.geometry;object.used=true;
            const bool correspondence=!inserted&&mesh.stable_key!=0&&object.geometry==mesh.geometry.get()&&history_valid;
            const bool surfaces_same=materials_same&&same_slot;
            // Unchanged object at an unchanged index: its instance, RT state and caster bounds stand.
            if(surfaces_same&&object.world==mesh.world&&(!shadows_cached||object.shadow_tracked)&&
                instances[index].previous_world==mesh.world&&instances[index].geometry[2]==(correspondence?1u:0u)) {
                if(shadows_cached)shadow_casters.push_back({static_cast<std::uint32_t>(index),object.shadow_bounds,object.shadow_dynamic});
                else object.shadow_tracked=false;
                continue;
            }
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
            const bool surface_changed=!inserted&&!surfaces_same&&!std::ranges::equal(object.surfaces,descriptions);
            material_changed|=surface_changed;
            bool object_coverage_changed=false;
            if(surface_changed) {
                // Mask sampling changes ray geometry even when the shared BLAS
                // remains non-opaque. Color-only edits do not change distances.
                object_coverage_changed=!std::ranges::equal(object.surfaces,descriptions,[](const auto& a,const auto& b) {
                    if(a.parameters.coverage!=b.parameters.coverage)return false;
                    if(a.parameters.coverage==material::Coverage::opaque)return true;
                    return a.parameters.base_color[3]==b.parameters.base_color[3] &&
                        a.parameters.alpha_cutoff==b.parameters.alpha_cutoff &&
                        a.texture(material::Input::base_color)==b.texture(material::Input::base_color) &&
                        a.texture(material::Input::opacity)==b.texture(material::Input::opacity);
                });
                coverage_changed|=object_coverage_changed;
            }
            const bool shadow_changed=inserted||object.world!=mesh.world||object.geometry!=mesh.geometry.get()||object_coverage_changed;
            const bool was_dynamic=object.shadow_dynamic;
            if(!inserted&&shadow_changed)object.shadow_dynamic=true;
            if(shadows_cached) {
                if(!object.shadow_tracked||shadow_changed) {
                    ShadowBounds bounds;
                    bounds.minimum.fill(std::numeric_limits<float>::max());bounds.maximum.fill(-std::numeric_limits<float>::max());
                    const auto& local=mesh.geometry->bounds();
                    for(unsigned corner=0;corner<8;++corner)for(std::size_t axis=0;axis<3;++axis) {
                        double value=mesh.world[12+axis];
                        for(std::size_t a=0;a<3;++a)value+=double((corner&(1u<<a))?local.maximum[a]:local.minimum[a])*mesh.world[a*4+axis];
                        invalid(std::isfinite(value)&&std::abs(value)<std::numeric_limits<float>::max(),"Shadow caster bounds exceed GPU float representation");
                        bounds.minimum[axis]=std::min(bounds.minimum[axis],std::nextafter(static_cast<float>(value),-std::numeric_limits<float>::infinity()));
                        bounds.maximum[axis]=std::max(bounds.maximum[axis],std::nextafter(static_cast<float>(value),std::numeric_limits<float>::infinity()));
                    }
                    shadow_changes.push_back({object.shadow_bounds,bounds,object.shadow_tracked,true,
                        (object.shadow_tracked&&!was_dynamic)||!object.shadow_dynamic,was_dynamic||object.shadow_dynamic});
                    object.shadow_bounds=bounds;object.shadow_tracked=true;
                }
                shadow_casters.push_back({static_cast<std::uint32_t>(index),object.shadow_bounds,object.shadow_dynamic});
            } else object.shadow_tracked=false;
            bool emitting=object.emitting;
            if(!surfaces_same) {
                emitting=false;
                for(const auto& surface:descriptions) {
                    const auto& e=surface.parameters.emissive;emitting|=e[0]>0||e[1]>0||e[2]>0;
                }
            }
            if(emitting||object.emitting)
                emitter_changed|=inserted||object.geometry!=mesh.geometry.get()||object.world!=mesh.world||surface_changed;
            if(inserted||surface_changed)object.surfaces.assign(descriptions.begin(),descriptions.end());
            object.emitting=emitting;
            // Keep geometric motion, but edited material/radiance has no valid
            // previous shading observation, including when seen through SSR.
            instance.geometry={g.offset,static_cast<std::uint32_t>(mesh.stable_key),correspondence&&!surface_changed?1u:0u,static_cast<std::uint32_t>(mesh.stable_key>>32)};
            if(instances[index]!=instance){instances[index]=instance;dirty(index);}
            if(inserted){object.instance=gfxCreateTopLevelAccelerationStructureInstance(gfx);allocation(bool(object.instance));
                require(gfxTopLevelAccelerationStructureAddInstance(gfx,tlas,object.instance),"Attach RT scene instance");topology_changed=true;}
            if(inserted||object.geometry!=mesh.geometry.get()) {
                require(gfxTopLevelAccelerationStructureInstanceSetBottomLevelAccelerationStructure(gfx,object.instance,g.blas),"Set RT instance mesh");topology_changed=true;
            }
            if(inserted||object.world!=mesh.world) {
                std::array<float,12> transform{};for(std::size_t r=0;r<3;r++)for(std::size_t c=0;c<4;c++)transform[r*4+c]=mesh.world[c*4+r];
                require(gfxTopLevelAccelerationStructureInstanceSetTransform(gfx,object.instance,transform.data()),"Update rigid RT transform");transform_changed=true;
            }
            require(gfxTopLevelAccelerationStructureInstanceSetInstanceID(gfx,object.instance,static_cast<std::uint32_t>(index)),"Remap current RT snapshot");
            object.world=mesh.world;object.geometry=mesh.geometry.get();
        }
        if(!steady)for(auto it=objects.begin();it!=objects.end();) {
            if(it->second.used){++it;continue;}
            if(shadows_cached&&it->second.shadow_tracked)shadow_changes.push_back({it->second.shadow_bounds,{},true,false,
                !it->second.shadow_dynamic,it->second.shadow_dynamic});
            emitter_changed|=it->second.emitting;
            require(gfxTopLevelAccelerationStructureRemoveInstance(gfx,tlas,it->second.instance),"Detach removed RT instance");
            require(gfxDestroyTopLevelAccelerationStructureInstance(gfx,it->second.instance),"Retire removed RT instance");it=objects.erase(it);topology_changed=true;
        }
        if(!steady)for(auto it=geometries.begin();it!=geometries.end();) {
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
        if(!steady)for(std::size_t i=0;i<count;i++) {
            const auto offset=resolved[i].geometry->offset;
            if(instances[i].geometry[0]!=offset){instances[i].geometry[0]=offset;dirty(i);}
        }
        // Membership/order changes do not change the world-space probe domain.
        // New/replaced objects already have no surface correspondence above;
        // surviving stable objects retain it even if their GPU indices change.
        if(!tlas_built||topology_changed) {require(gfxTopLevelAccelerationStructureBuild(gfx,tlas),"Build scene TLAS");tlas_built=true;}
        else if(transform_changed||remapped)require(gfxTopLevelAccelerationStructureUpdate(gfx,tlas),"Update scene TLAS");
        gpu_instances=instance_table.write(gfx,context.info().frames_in_flight,instances,dirty_first,dirty_last,"Scene Lit instance");
        if(geometry_changed||topology_changed||transform_changed) {
            std::array<double,3> low,high;low.fill(std::numeric_limits<double>::max());high.fill(-std::numeric_limits<double>::max());
            for(const auto& mesh:frame.meshes) {
                const auto& local=mesh.geometry->bounds();
                for(unsigned corner=0;corner<8;++corner)for(std::size_t axis=0;axis<3;++axis) {
                    double value=mesh.world[12+axis];
                    for(std::size_t a=0;a<3;++a)value+=double((corner&(1u<<a))?local.maximum[a]:local.minimum[a])*mesh.world[a*4+axis];
                    low[axis]=std::min(low[axis],value);high[axis]=std::max(high[axis],value);
                }
            }
            for(std::size_t axis=0;axis<3;++axis) {
                probe_scene_min[axis]=static_cast<float>(low[axis]);probe_scene_max[axis]=static_cast<float>(high[axis]);
            }
        }
        material_revision=material_frame->revision;resolved_complete=true;
        end(1);
        return geometry_changed||topology_changed||transform_changed;
    }

    void lighting(const Frame& frame) {
        light_frame.update(frame,sequence);
        light_changed=light_frame.radiance_changed;
        const auto slots=context.info().frames_in_flight;
        const auto upload=[&]<class T>(DeviceTable<T>& target,const std::vector<T>& data,bool changed) {
            return target.write(gfx,slots,data,changed,"Scene Lit light table");
        };
        const bool changed=light_frame.changed;
        gpu_sources=upload(light_tables.sources,light_frame.sources,changed);
        gpu_proposals=upload(light_tables.proposals,light_frame.proposals,changed);
        spatial_uploaded=light_frame.spatial_changed||!light_tables.cells.bytes();
        gpu_light_cells=upload(light_tables.cells,light_frame.spatial_cells,light_frame.spatial_changed);
        gpu_spatial_proposals=upload(light_tables.spatial_proposals,light_frame.spatial_proposals,light_frame.spatial_changed);
        gpu_spatial_source_slots=upload(light_tables.spatial_source_slots,light_frame.spatial_source_slots,light_frame.spatial_changed);
        light_grid_counts_gpu={light_grid_counts[0],light_grid_counts[1],light_grid_counts[2],
            frame.view.lit.spatial_light_sampling&&!light_frame.spatial_cells.empty()?1u:0u};
        source_counts={light_frame.local_count,light_frame.directional_count,0,0};
    }
    void draw(const Frame& frame,ui::PixelExtent next,GfxTexture depth,const SceneLitDraws& scene_draws,const MaterialFrame& materials,FrameStats& stats) {
        material_frame=&materials;
        resources();publish_gpu_sample(stats);encoded_pass_mask=0;begin(1);const auto& settings=frame.view.lit;targets(next);
        if(!settings.probe_inspect || !settings.ddgi || (stats.scene_lit_probe_inspection &&
            (stats.scene_lit_probe_inspection->scene_identity!=frame.scene_identity ||
             stats.scene_lit_probe_inspection->pixel!=settings.probe_inspect_pixel ||
             stats.scene_lit_probe_inspection->requested_probe!=settings.probe_inspect_source)))stats.scene_lit_probe_inspection.reset();
        if(!enabled||scene_identity!=frame.scene_identity||frame.camera_cut)++depth_epoch;
        if(!enabled||scene_identity!=frame.scene_identity) {
            reset();shadow_cache.invalidate();
            for(auto& [key,object]:objects){(void)key;object.shadow_tracked=false;if(scene_identity!=frame.scene_identity)object.shadow_dynamic=false;}
        }
        else if(frame.camera_cut)reset(false);
        far_depth=frame.camera.far_depth;
        const std::array<float,4> next_viewport{frame.scene_viewport.x,frame.scene_viewport.y,frame.scene_viewport.width,frame.scene_viewport.height};
        if(viewport!=next_viewport)reset(false);
        if(previous_probe_settings!=settings.probe_volumes || previous_indirect_gain!=settings.indirect_gain)reset(false);
        if(previous_probe_bounce!=settings.probe_bounce || previous_probe_feedback_visibility!=settings.probe_feedback_visibility)reset();
        previous_probe_settings=settings.probe_volumes;previous_indirect_gain=settings.indirect_gain;
        previous_probe_bounce=settings.probe_bounce;previous_probe_feedback_visibility=settings.probe_feedback_visibility;
        const std::uint32_t flags=(settings.shadows?1u:0u)|(settings.ddgi?2u:0u)|(settings.gtao?4u:0u)|(settings.sssr?8u:0u)|(settings.antialiasing==Antialiasing::taa?16u:0u)|(settings.bloom?32u:0u);
        const auto effect_changes=flags^previous_effect_flags;
        // Every lit shadow caster reads the atlas, so it exists whenever one can contribute.
        const bool next_shadows_cached=settings.shadows&&std::ranges::any_of(frame.lights,[](const Light& light) {
            return light.casts_shadow&&light.intensity>0&&std::ranges::any_of(light.color,[](float channel){return channel>0;});
        });
        const bool shadow_backend_changed=shadows_cached!=next_shadows_cached;
        if(shadow_backend_changed)shadow_cache.invalidate();
        shadows_cached=next_shadows_cached;
        ShadowSample shadow_sample;
        if(shadows_cached)prepare_shadows();
        const bool direct_changed=(effect_changes&1u)!=0||shadow_backend_changed;
        const bool reflection_changed=settings.reflection_distance!=controls[3]||settings.reflection_roughness!=temporal[1];
        const bool ao_changed=(effect_changes&4u)!=0||settings.ao_radius!=controls[2];
        // Invalidate each signal and its actual consumers when its input contract
        // changes. Continuous light edits still use each signal's local response.
        history_invalid=(direct_changed||reflection_changed||ao_changed||(effect_changes&(2u|4u|8u))?2u:0u)|(ao_changed?4u:0u)|
            (direct_changed||reflection_changed||ao_changed||effect_changes?8u:0u);
        if(settings.antialiasing!=previous_antialiasing)reset(false); // Projection sampling changed.
        previous_antialiasing=settings.antialiasing;
        previous_effect_flags=flags;
        scene_identity=frame.scene_identity;enabled=true;
        const bool sky_changed=prepare_sky(settings);
        probes(settings,frame.lighting_focus,stats);
        const bool transform_changed=acceleration(frame);lighting(frame);
        const bool visibility_changed=transform_changed||coverage_changed;
        // Uniform consumers (DDGI-off composition, SSSR fallback) read the map's mean.
        const auto mean=sky_source?sky_source->mean():std::array<float,3>{1,1,1};
        const std::array<float,4> next_environment{frame.environment[0]*mean[0],frame.environment[1]*mean[1],frame.environment[2]*mean[2],1};
        sky_radiance={frame.environment[0],frame.environment[1],frame.environment[2],0};
        const bool radiance_changed=light_changed||material_changed||emitter_changed||sky_changed||next_environment!=environment;
        // The epoch describes an authored change, never stochastic radiance.
        // GPU directions keep their own progress even when sparsely scheduled.
        if(radiance_changed||transform_changed)
            if(++probe_response_epoch==0)require(gfxCommandClearTexture(gfx,probe_response),"Reset wrapped DDGI response epoch");
        if(visibility_changed)probe_geometry_pending=2*probe_field.total;
        auto projection=matrix(frame.camera.projection);const bool taa_enabled=settings.antialiasing==Antialiasing::taa;
        const bool smaa_temporal=settings.antialiasing==Antialiasing::smaa_t2x;
        const float smaa_jitter=sequence%2==0?.25f:-.25f;
        const float jx=taa_enabled?halton(static_cast<std::uint32_t>(sequence%8)+1,2)-.5f:(smaa_temporal?smaa_jitter:0);
        const float jy=taa_enabled?halton(static_cast<std::uint32_t>(sequence%8)+1,3)-.5f:(smaa_temporal?smaa_jitter:0);
        for(std::size_t row=0;row<4;row++){projection.elements[row*4]+=2*jx/frame.scene_viewport.width*projection.elements[row*4+3];
            projection.elements[row*4+1]-=2*jy/frame.scene_viewport.height*projection.elements[row*4+3];}
        camera_view=frame.camera.view;
        {   // Row-vector projection without skew: view x = ndc.x*(z*a+b)+(z*c+d), likewise y; z from device depth.
            const auto& e=projection.elements;
            ao_view_x={float(e[11]/e[0]),float(e[15]/e[0]),float(-e[8]/e[0]),float(-e[12]/e[0])};
            ao_view_y={float(e[11]/e[5]),float(e[15]/e[5]),float(-e[9]/e[5]),float(-e[13]/e[5])};
            ao_view_z={float(e[10]),float(e[14]),float(e[11]),float(e[15])};
        }
        const auto vp=matrix(camera_view)*projection;view_projection=floats(vp);inverse_view_projection=floats(*math::try_inverse(vp));
        if(!history_valid){previous_view_projection=view_projection;previous_inverse_view_projection=inverse_view_projection;}
        size_frame={next.width,next.height,static_cast<std::uint32_t>(sequence),history_valid?1u:0u};
        require(gfxCommandClearBuffer(gfx,ray_counters),"Reset Scene Lit ray counters");
        viewport=next_viewport;
        const auto raster=raster_viewport(frame.scene_viewport);
        raster_transform=raster.clip_transform;
        // The validated logical rect lies in this window. Receiver dispatch ends with the raster
        // pixel-center range; its origin rounds down to 16 so thread groups align with texture
        // tiles, and lit_inside rejects the added margin.
        screen_dispatch={static_cast<std::uint32_t>(raster.scissor.left)&~15u,static_cast<std::uint32_t>(raster.scissor.top)&~15u,
            static_cast<std::uint32_t>(raster.scissor.right),static_cast<std::uint32_t>(raster.scissor.bottom)};
        clusters();
        environment=next_environment;
        controls={std::exp2(settings.exposure_ev),ray_offset,settings.ao_radius,settings.reflection_distance};
        effect_flags=flags;hit_candidates=settings.direct_candidates;
        // Receiver motion alone cannot describe changing shadows or secondary
        // hits. Keep visibility changes distinct from authored radiance edits:
        // they shorten support without globally clamping stochastic observations.
        temporal={.9f,settings.reflection_roughness,radiance_changed?1.f:0.f,visibility_changed?1.f:0.f};jitter={jx,jy,jitter[0],jitter[1]};
        begin(0);for(std::uint32_t i=0;i<gbuffer_textures.size();i++){require(gfxCommandClearTexture(gfx,gbuffer_textures[i]),"Clear Scene Lit GBuffer");require(gfxCommandBindColorTarget(gfx,i,gbuffer_textures[i]),"Bind Scene Lit GBuffer");}
        require(gfxCommandBindDepthStencilTarget(gfx,depth),"Bind Scene Lit primary depth");common(gbuffer.program(),depth);
        require(gfxCommandBindKernel(gfx,gbuffer.kernel()),"Bind Scene Lit raster pipeline");
        bind(gbuffer.program(),"lit_draw_instances",scene_draws.draw_instances);
        for(const auto& draw:scene_draws.draws) {
            if(!draw.count)continue;
            bind(gbuffer.program(),"lit_triangles",draw.triangles);bind(gbuffer.program(),"lit_first_instance",draw.first);
            require(gfxCommandDraw(gfx,draw.vertex_count,draw.count),"Draw Scene Lit surface geometry");++stats.scene_draw_calls;
        }
        for(std::uint32_t i=0;i<gbuffer_textures.size();i++)require(gfxCommandBindColorTarget(gfx,i,{}),"Unbind Scene Lit GBuffer");
        require(gfxCommandBindDepthStencilTarget(gfx,{}),"Unbind Scene Lit primary depth");end(0);
        begin(3,settings.ddgi);begin(8,settings.ddgi&&probe_field.total!=0);
        if(settings.ddgi) {
            prepare_inspection(frame);
            probe_history_mode=static_cast<std::uint32_t>(settings.probe_history);
            const auto total=probe_field.total,budget=probe_field.scheduled;
            probe_history={.6f,probe_geometry_pending?.85f:.95f,
                std::exp(-frame.render_delta_seconds/.15f),.93f};
            probe_controls={settings.probe_bounce,settings.probe_feedback_visibility?1.f:0.f,0,0};
            probe_update_settings={0,budget,settings.probe_rays,probe_field.valid?0u:1u};
            if(total) {
                bind_probes(probe_copy.program(),false);dispatch(probe_copy,depth,total);
            }
            begin(10,total&&budget);
            if(total&&budget) {
                bind_probes(probe_trace.program(),false);bind(probe_trace.program(),"lit_probe_work",probe_field.work);
                bind(probe_trace.program(),"lit_probe_ray_hits_output",probe_ray_hits);
                dispatch(probe_trace,depth,budget,1,1,2);
            }
            end(10);begin(11,total&&budget);
            if(total&&budget) {
                const auto& update=settings.probe_inspect?inspect_update:probe_update;
                const auto p=update.program();
                bind_probes(p,false);bind(p,"lit_probe_work",probe_field.work);
                if(settings.probe_inspect){bind(p,"lit_probe_inspect_source",inspection->traced_probe);bind(p,"lit_probe_inspected_rays",inspection->rays);}
                // SRV consumers establish the UAV write/read dependencies in gfx.
                bind(p,"lit_probe_ray_hits",probe_ray_hits);
                bind(p,"lit_probe_offsets_current",probe_offsets[probe_parity]);
                dispatch(update,depth,budget,1,1,2);
            }
            end(11);
            if(total){bind_probes(probe_present.program(),true);dispatch(probe_present,depth,total);}
            end(8);begin(9);
            // Jitter changes every receiver endpoint each frame. Do not pay a
            // full-screen cache allocation/write for a mode that cannot reuse it.
            const bool cache_visibility=!taa_enabled&&!smaa_temporal;
            const auto cache_count=cache_visibility?std::uint64_t((next.width+1)/2)*((next.height+1)/2):1u;
            if(probe_visibility_cache.getSize()<cache_count*2*sizeof(std::uint32_t)) {
                retire(probe_visibility_cache);
                probe_visibility_cache=gfxCreateBuffer<std::array<std::uint32_t,2>>(gfx,cache_count);
                allocation(bool(probe_visibility_cache));probe_visibility_valid=false;
            }
            // Reuse only the exact segment answer. Lighting and moment weights
            // remain live. The shader also checks the receiver and each probe's
            // actual offset; geometry/Mask edits invalidate every segment.
            const bool reuse_visibility=cache_visibility&&probe_visibility_valid&&history_valid&&probe_field.valid&&!visibility_changed&&
                view_projection==previous_view_projection&&inverse_view_projection==previous_inverse_view_projection&&
                std::memcmp(probe_field.current.data(),probe_field.previous.data(),sizeof(probe_field.current))==0;
            const auto resolve=probe_resolve.program();
            bind_probes(resolve,true,true);bind(resolve,"lit_indirect_half_output",indirect_half);
            bind(resolve,"lit_probe_visibility_cache",probe_visibility_cache);
            bind(resolve,"lit_probe_visibility_cache_mode",cache_visibility?(reuse_visibility?2u:1u):0u);
            bind(resolve,"lit_probe_visibility_offsets",probe_offsets[1-probe_parity]);
            const auto blocks_x=(screen_dispatch[2]-screen_dispatch[0]+1)/2,blocks_y=(screen_dispatch[3]-screen_dispatch[1]+1)/2;
            if(blocks_x&&blocks_y)dispatch(probe_resolve,depth,(blocks_x+7)/8,(blocks_y+7)/8,1,3);
            require(gfxCommandClearBuffer(gfx,probe_fallback),"Reset DDGI exact receiver list");
            require(gfxCommandClearBuffer(gfx,probe_fallback_arguments),"Reset DDGI exact receiver dispatch");
            const auto upsample=probe_upsample.program();
            bind_probes(upsample,true,true);bind(upsample,"lit_indirect_half",indirect_half);bind(upsample,"lit_indirect",indirect);
            bind(upsample,"lit_probe_fallback_output",probe_fallback);bind(upsample,"lit_probe_fallback_arguments",probe_fallback_arguments);
            bind(upsample,"lit_probe_fallback_capacity",probe_fallback_capacity);
            dispatch_screen(probe_upsample,depth,3);
            const auto exact=probe_exact.program();
            bind_probes(exact,true,true);bind(exact,"lit_indirect",indirect);
            bind(exact,"lit_probe_fallback",probe_fallback);bind(exact,"lit_probe_fallback_capacity",probe_fallback_capacity);
            common(exact,depth,3);require(gfxCommandBindKernel(gfx,probe_exact.kernel()),"Bind DDGI exact receiver gather");
            require(gfxCommandDispatchIndirect(gfx,probe_fallback_arguments),"Dispatch listed DDGI exact receivers");
            end(9);
            inspect_probe(frame,depth,stats);
            probe_field.commit(settings);probe_parity=1-probe_parity;
            // Warmup may repeatedly visit new slices. Only global traversal
            // progress proves that remote support received its response rounds.
            probe_geometry_pending-=std::min(probe_geometry_pending,probe_field.round_robin_progress);
            ++probe_sequence;
        } else {begin(10,false);end(10);begin(11,false);end(11);end(8);begin(9,false);end(9);}
        end(3);begin(4,settings.gtao);if(settings.gtao){
            const auto reduce=ao_depth_reduce.program();
            constexpr std::array levels{"lit_ao_depth_output0","lit_ao_depth_output1","lit_ao_depth_output2","lit_ao_depth_output3","lit_ao_depth_output4"};
            for(std::uint32_t level=0;level<levels.size();++level)
                require(gfxProgramSetTexture(gfx,reduce,levels[level],ao_depth,level),"Bind GTAO view depth level");
            for(const auto& p:{reduce,ao.program()}){bind(p,"lit_ao_view_x",ao_view_x);bind(p,"lit_ao_view_y",ao_view_y);bind(p,"lit_ao_view_z",ao_view_z);}
            dispatch(ao_depth_reduce,depth,(next.width+15)/16,(next.height+15)/16);
            bind(ao.program(),"lit_ao_depth",ao_depth);bind(ao.program(),"lit_ao_output",ao_raw);dispatch_screen(ao,depth);
            bind(ao_resolve.program(),"lit_ao_raw",ao_raw);bind(ao_resolve.program(),"lit_ao_previous",ao_history[1-parity]);
            bind(ao_resolve.program(),"lit_ao_history_output",ao_history[parity]);bind(ao_resolve.program(),"lit_ao_resolved_output",ao_texture);dispatch_screen(ao_resolve,depth);}
        end(4);
        // Direct light reads every receiver signal above and composes HDR in one pass.
        begin(2);begin(12);
        if(cluster_tiles[0]&&cluster_tiles[1]) {
            const auto& build=settings.direct_cluster_inspect?cluster_inspect:cluster_build;
            const auto p=build.program();bind(p,"lit_cluster_depth_output",cluster_depth);
            bind(p,"lit_cluster_count_output",cluster_count);bind(p,"lit_cluster_source_output",cluster_source);
            dispatch(build,depth,cluster_tiles[0],cluster_tiles[1]);
        }
        end(12);begin(13,false);
        if(shadows_cached) {
            const auto& plan=shadow_cache.prepare(frame,light_frame,shadow_casters,shadow_changes,receiver_depth(),sequence);
            shadow_sample.plan=plan.stats;
            if(!plan.updates.empty())encoded_pass_mask|=1u<<13;
            shadow_update(plan,scene_draws,shadow_sample);
            require(gfxCommandSetViewport(gfx,raster.hardware.x,raster.hardware.y,raster.hardware.width,raster.hardware.height),"Restore Scene Lit viewport after shadow updates");
            require(gfxCommandSetScissorRect(gfx,raster.scissor.left,raster.scissor.top,raster.scissor.right-raster.scissor.left,raster.scissor.bottom-raster.scissor.top),"Restore Scene Lit scissor after shadow updates");
        }
        end(13);
        if(!history_valid)full_window_frames=2;
        begin(14);
        {const auto p=compose.program();
            bind(p,"lit_cluster_depth",cluster_depth);bind(p,"lit_cluster_count",cluster_count);bind(p,"lit_cluster_source",cluster_source);
            bind_shadows(p);bind(p,"lit_ltc",ltc);
            bind(p,"lit_indirect_source",indirect);bind(p,"lit_ao",ao_texture);
            bind(p,"lit_indirect_gain",settings.indirect_gain);bind(p,"lit_hdr_output",hdr);bind(p,"lit_motion_output",gbuffer_textures[3]);bind(p,"lit_sun_trace_list",sun_trace_list);}
        dispatch_window(compose,depth);
        // Usually an empty list: an indirect dispatch of zero groups.
        bind(sun_trace_prepare.program(),"lit_sun_trace_arguments",sun_trace_arguments);dispatch(sun_trace_prepare,depth,1);
        {const auto p=sun_trace.program();bind_shadows(p);bind(p,"lit_sun_trace_list",sun_trace_list);bind(p,"lit_hdr_output",hdr);
            common(p,depth);require(gfxCommandBindKernel(gfx,sun_trace.kernel()),"Bind sun trace kernel");
            require(gfxCommandDispatchIndirect(gfx,sun_trace_arguments),"Dispatch sun receivers beyond the cascades");}
        if(shadows_cached)shadow_cache.commit();
        end(14);end(2);
        begin(5);
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
        dispatch_window(ssr_classify,depth);
        if(settings.sssr)dispatch(ssr_prepare,depth,1);
        if(settings.sssr){common(ssr_trace.program(),depth);require(gfxCommandBindKernel(gfx,ssr_trace.kernel()),"Bind SSSR work kernel");require(gfxCommandDispatchIndirect(gfx,reflection_arguments),"Dispatch SSSR classified work");}
        bind(reflection_resolve.program(),"lit_reflection_trace",reflection_trace);bind(reflection_resolve.program(),"lit_reflection_hit_trace",reflection_hit_trace);
        bind(reflection_resolve.program(),"lit_reflection_previous",reflection_history[1-parity]);bind(reflection_resolve.program(),"lit_reflection_hit_previous",reflection_hit_history[1-parity]);
        bind(reflection_resolve.program(),"lit_reflection_resolved_output",reflection_history[parity]);bind(reflection_resolve.program(),"lit_reflection_hit_output",reflection_hit_history[parity]);
        bind(reflection_resolve.program(),"lit_reflection_moments_previous",reflection_moments[1-parity]);bind(reflection_resolve.program(),"lit_reflection_moments_output",reflection_moments[parity]);
        dispatch_window(reflection_resolve,depth);
        if(settings.sssr)for(std::uint32_t step=0;step<3;++step){const auto p=reflection_filter.program();
            bind(p,"lit_reflection_filter_input",step==0?reflection_history[parity]:reflection_scratch[(step-1)%2]);
            bind(p,"lit_reflection_filter_output",reflection_scratch[step%2]);bind(p,"lit_reflection_moments",reflection_moments[parity]);
            bind(p,"lit_reflection_hit_trace",reflection_hit_trace);bind(p,"lit_reflection_filter_step",1u<<step);dispatch_window(reflection_filter,depth);}
        end(5);
        begin(6);bind(taa.program(),"lit_reflection",settings.sssr?reflection_scratch[0]:reflection_history[parity]);bind(taa.program(),"lit_reflection_moments",reflection_moments[parity]);bind(taa.program(),"lit_taa_previous",taa_history[1-parity]);bind(taa.program(),"lit_taa_output",taa_history[parity]);
        bind(taa.program(),"lit_history_depth_output",history_depth[parity]);bind(taa.program(),"lit_history_normal_output",history_normal[parity]);bind(taa.program(),"lit_history_geometry_normal_output",history_geometry_normal[parity]);
        bind(taa.program(),"lit_history_surface_id_output",history_surface_id[parity]);dispatch_window(taa,depth);end(6);
        begin(7);const std::array<float,4> bloom_controls{1,.5f,settings.bloom?.04f:0.f,64};
        if(settings.bloom) {
        bind(bloom_downsample.program(),"lit_taa",taa_history[parity]);bind(bloom_downsample.program(),"lit_bloom_output",bloom[0]);bind(bloom_downsample.program(),"lit_bloom_controls",bloom_controls);
        dispatch(bloom_downsample,depth,(bloom[0].getWidth()+7)/8,(bloom[0].getHeight()+7)/8);
        for(std::uint32_t i=0;i<2;i++){bind(bloom_blur.program(),"lit_bloom_input",bloom[i]);bind(bloom_blur.program(),"lit_bloom_output",bloom[1-i]);
            bind(bloom_blur.program(),"lit_bloom_direction",std::array<std::int32_t,2>{i==0?1:0,i==0?0:1});dispatch(bloom_blur,depth,(bloom[0].getWidth()+7)/8,(bloom[0].getHeight()+7)/8);}
        }
        bind(tone_map.program(),"lit_taa",taa_history[parity]);bind(tone_map.program(),"lit_bloom",bloom[0]);bind(tone_map.program(),"lit_bloom_controls",bloom_controls);bind(tone_map.program(),"lit_tone_output",tone_color);dispatch_window(tone_map,depth);
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
            dispatch_window(smaa_edges_pass,depth);dispatch_window(smaa_weights_pass,depth);dispatch_window(smaa_neighborhood,depth);
            display=smaa_spatial[parity];if(smaa_temporal){dispatch_window(smaa_resolve,depth);display=smaa_output;}
        }
        common(tone.program(),depth);bind(tone.program(),"lit_display_source",display);
        require(gfxCommandBindKernel(gfx,tone.kernel()),"Bind Scene Lit tone map");require(gfxCommandDraw(gfx,3),"Compose Scene Lit display");stats.viewport_draw_calls++;end(7);
        if(settings.probe_debug) draw_coverage(depth,settings,stats);
        stats.scene_lit_light_sources=static_cast<std::uint32_t>(light_frame.sources.size());
        stats.scene_lit_spatial_sources=static_cast<std::uint32_t>(light_frame.spatial_proposals.size());
        stats.scene_lit_spatial_cells=light_frame.spatial_nonempty_cells;
        const auto logical=[](const auto& values){return std::max<std::size_t>(1,values.size())*sizeof(values[0]);};
        stats.scene_lit_spatial_bytes=logical(light_frame.spatial_cells)+logical(light_frame.spatial_proposals)+logical(light_frame.spatial_source_slots);
        stats.scene_lit_spatial_changed=spatial_uploaded;
        stats.scene_lit_shadow_sources=settings.shadows?static_cast<std::uint32_t>(std::count_if(light_frame.sources.begin(),light_frame.sources.end(),[](const GpuLightSource& l){return (l.meta[1]&2u)!=0;})):0;
        const auto readback_slot=sequence%readback_count;
        require(gfxCommandCopyBuffer(gfx,ray_readback[readback_slot],ray_counters),"Publish Scene Lit ray counters asynchronously");
        readback_sequences[readback_slot]=gfxGetRecordingFrameSequence(gfx);readback_frame_ids[readback_slot]=frame.ui_frame.id;
        readback_pass_masks[readback_slot]=encoded_pass_mask;readback_depth_epochs[readback_slot]=depth_epoch;
        readback_shadows[readback_slot]=shadow_sample;
        probe_visibility_valid=settings.ddgi&&!taa_enabled&&!smaa_temporal;
        stats.scene_lit_history_resets=reset_events;reset_events=0;previous_view_projection=view_projection;previous_inverse_view_projection=inverse_view_projection;history_valid=true;sequence++;parity=1-parity;
        if(full_window_frames)--full_window_frames;
    }
    void bind_probe_debug(GfxProgram p) {
        const auto read=1-probe_parity;
        bind(p,"lit_probe_irradiance_previous",probe_presentation);bind(p,"lit_probe_distance_previous",probe_distance[read]);
        bind(p,"lit_probe_offsets_previous",probe_offsets[read]);bind(p,"lit_probe_age",probe_age);
        bind(p,"lit_probe_volumes",probe_field.current);
        bind(p,"lit_probe_pool",std::array<std::uint32_t,4>{probe_field.width,probe_field.total,0,probe_field.valid?1u:0u});
    }
    void draw_coverage(GfxTexture depth,const SceneLitSettings& settings,FrameStats& stats) {
        const auto p=probe_debug_surface.program();common(p,depth);bind_probe_debug(p);
        bind(p,"lit_probe_debug_options",std::array<std::uint32_t,4>{(effect_flags&2u)&&probe_field.valid?1u:0u,
            static_cast<std::uint32_t>(settings.probe_coverage),0,0});
        require(gfxCommandBindKernel(gfx,probe_debug_surface.kernel()),"Bind DDGI receiver diagnostic");
        require(gfxCommandDraw(gfx,3),"Draw DDGI receiver diagnostic");++stats.viewport_draw_calls;
    }
    void draw_debug(const Frame& frame,ui::PixelExtent size,GfxTexture depth,FrameStats& stats) {
        const auto& settings=frame.view.lit;
        if(frame.view.shading!=Shading::scene_lit||frame.view.wireframe==Wireframe::only||!settings.probe_overlay()) {retire(probe_debug_depth);stats.scene_lit_resource_bytes=resource_bytes();return;}
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
        bind(p,"lit_far_depth",frame.camera.far_depth);
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
    // Returns whether the map or its yaw changed; tint changes reach the probes through environment.
    bool prepare_sky(const SceneLitSettings& settings) {
        const bool replaced=settings.sky_map!=sky_source,turned=settings.sky_map&&settings.sky_rotation!=sky_yaw;
        sky_yaw=settings.sky_rotation;
        if(replaced) {
            retire(sky_background);retire(sky_cube);sky_source=settings.sky_map;
            if(sky_source) {
                const auto& map=*sky_source;
                // Mip generation writes the levels as UAVs; without the flag gfx re-creates and copies the whole texture first.
                sky_background=gfxCreateTexture2D(gfx,map.width(),map.height(),DXGI_FORMAT_R16G16B16A16_FLOAT,gfxCalculateMipCount(map.width(),map.height()),
                    nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                allocation(bool(sky_background));
                const auto upload=[&](std::span<const std::byte> bytes,auto&& copy) {
                    auto buffer=gfxCreateBuffer(gfx,bytes.size(),bytes.data(),kGfxCpuAccess_Write);allocation(bool(buffer));
                    const auto copied=copy(buffer);const auto retired=gfxDestroyBuffer(gfx,buffer);
                    require(copied,"Upload sky map");require(retired,"Retire sky map upload");
                };
                upload(std::as_bytes(map.pixels()),[&](GfxBuffer buffer){return gfxCommandCopyBufferToTexture(gfx,sky_background,buffer);});
                require(gfxCommandGenerateMips(gfx,sky_background),"Generate sky map mipmaps");
                sky_cube=gfxCreateTextureCube(gfx,rendering::SkyMap::cube_size,DXGI_FORMAT_R32G32B32A32_FLOAT);allocation(bool(sky_cube));
                // gfx copies cube faces at the placed-footprint row pitch, so rows are padded to it.
                constexpr std::size_t size=rendering::SkyMap::cube_size,row=size*4*sizeof(float);
                constexpr std::size_t pitch=(row+D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1)/D3D12_TEXTURE_DATA_PITCH_ALIGNMENT*D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
                std::vector<std::byte> face(size*pitch);
                const auto texels=std::as_bytes(map.cube());
                for(std::uint32_t i=0;i<6;++i) {
                    for(std::size_t y=0;y<size;++y)std::copy_n(texels.begin()+(i*size+y)*row,row,face.begin()+y*pitch);
                    upload(face,[&](GfxBuffer buffer){return gfxCommandCopyBufferToCubeFace(gfx,sky_cube,buffer,i);});
                }
            }
        }
        const double radians=double(settings.sky_rotation)*std::numbers::pi/180;
        sky=sky_source?std::array<float,4>{float(std::cos(radians)),float(std::sin(radians)),std::ldexp(1.f,sky_source->scale_exponent()),1}:std::array<float,4>{};
        return replaced||turned;
    }
    void retire_scene() {
        retire(sky_background);retire(sky_cube);sky_source.reset();sky={};
        resolved.clear();resolved_complete=false;
        shadow_cache.invalidate();shadows_cached=false;shadow_casters.clear();shadow_changes.clear();
        retire(shadow_static);retire(shadow_dynamic);
        auto& tables=shadow_tables;
        for(auto r:{tables.face_table.retire(gfx),tables.light_table.retire(gfx),tables.instance_table.retire(gfx)})require(r,"Retire shadow tables");
        tables.faces={};tables.lights={};tables.instances={};
        tables.previous_faces.clear();tables.previous_lights.clear();tables.previous_instances.clear();
        shadow_instance_indices.clear();
        for(auto& [key,o]:objects){(void)key;if(tlas)require(gfxTopLevelAccelerationStructureRemoveInstance(gfx,tlas,o.instance),"Detach Scene Lit instance");
            require(gfxDestroyTopLevelAccelerationStructureInstance(gfx,o.instance),"Retire Scene Lit instance");}objects.clear();
        if(tlas)require(gfxDestroyTopLevelAccelerationStructure(gfx,std::exchange(tlas,{})),"Retire Scene Lit TLAS");
        for(auto& [key,g]:geometries){(void)key;require(gfxDestroyBottomLevelAccelerationStructure(gfx,g.blas),"Retire Scene Lit BLAS");require(gfxDestroyGeometry(gfx,g.geometry),"Retire Scene Lit geometry");retire(g.positions);}geometries.clear();
        for(auto* b:{&rt_triangles,&rt_closed_slots,&probe_ray_hits})retire(*b);
        require(instance_table.retire(gfx),"Retire Scene Lit instance table");
        light_tables.each([&](auto& table){require(table.retire(gfx),"Retire Scene Lit light tables");});
        for(auto* b:{&gpu_instances,&gpu_light_cells,&gpu_spatial_proposals,&gpu_spatial_source_slots,&gpu_sources,&gpu_proposals})*b={};
        std::vector<GpuInstance>{}.swap(instances);light_frame.clear();
        std::vector<GpuRayTriangle>{}.swap(packed_triangles);std::vector<std::uint32_t>{}.swap(packed_closed_slots);
        tlas_built=false;
    }
    void close() {
        pending.clear();
        if(inspection){retire(inspection->output);retire(inspection->rays);for(auto& buffer:inspection->readback)retire(buffer);for(auto& buffer:inspection->ray_readback)retire(buffer);inspection.reset();}
        retire_targets();for(auto* pair:{&probe_irradiance,&probe_distance,&probe_offsets})for(auto& t:*pair)retire(t);retire(probe_presentation);retire(probe_response);retire(brdf_lut);retire(smaa_area);retire(smaa_search);retire(probe_age);retire(probe_sample_cursor);retire(probe_debug_depth);
        retire_scene();retire(ray_counters);retire(ltc);
        for(auto& readback:ray_readback)retire(readback);
        for(auto& q:queries)if(q)require(gfxDestroyTimestampQuery(gfx,std::exchange(q,{})),"Retire Scene Lit timestamp");
        for(auto* s:{&sampler,&shadow_sampler,&sky_sampler})if(*s)require(gfxDestroySamplerState(gfx,std::exchange(*s,{})),"Retire Scene Lit sampler");
        for(auto* p:{&gbuffer,&cluster_build,&cluster_inspect,&shadow_depth,&compose,&sun_trace_prepare,&sun_trace,&probe_copy,&probe_trace,&probe_update,&probe_present,&probe_resolve,&probe_upsample,&probe_exact,
                    &ao,&ao_depth_reduce,&ao_resolve,&depth_first,&depth_pyramid,&ssr_reset,&ssr_classify,&ssr_prepare,&ssr_trace,&reflection_resolve,&reflection_filter,&brdf,&taa,
                    &bloom_downsample,&bloom_blur,&tone_map,&tone,&smaa_edges_pass,&smaa_weights_pass,&smaa_neighborhood,&smaa_resolve,&probe_debug_surface,&probe_debug_geometry,&inspect_pass,&inspect_update})*p={};
    }
};

SceneLit::SceneLit(rhi::Context& context):state_(std::make_unique<State>(context)){}
SceneLit::~SceneLit(){if(state_){const auto result=close();if(!result){std::fprintf(stderr,"Scene Lit cleanup failed: %s\n",result.error().cause.message.c_str());std::abort();}}}
void SceneLit::validate(const Frame& frame,bool duplicate_mesh_keys) const {
    const auto view_inverse=math::try_inverse(matrix(frame.camera.view));
    const auto vp=matrix(frame.camera.view)*matrix(frame.camera.projection);const auto vp_inverse=math::try_inverse(vp);
    invalid(bool(view_inverse)&&bool(vp_inverse),"Scene Lit requires an invertible camera");
    for(const auto* value:{&vp,&*vp_inverse,&*view_inverse})for(double element:value->elements)
        invalid(std::isfinite(element)&&std::abs(element)<=std::numeric_limits<float>::max(),"Scene Lit camera exceeds GPU float representation");
    const auto& s=frame.view.lit;invalid(frame.lights.size()<no_light_source&&s.direct_candidates>=1&&s.direct_candidates<=32,"Invalid Scene Lit light count or candidate budget");
    const auto finite=[](float v){return std::isfinite(v);};
    invalid(finite(s.exposure_ev)&&s.exposure_ev>=-16&&s.exposure_ev<=16&&finite(s.ao_radius)&&s.ao_radius>0&&
        finite(s.reflection_distance)&&s.reflection_distance>0&&finite(s.reflection_roughness)&&s.reflection_roughness>=0&&s.reflection_roughness<=1&&s.probe_update_budget>0&&s.probe_rays>=1&&s.probe_rays<=256&&s.antialiasing<=Antialiasing::smaa_t2x&&s.probe_visualization<=ProbeVisualization::distance&&finite(s.probe_radius)&&s.probe_radius>=.01f&&s.probe_radius<=.45f,"Invalid Scene Lit settings");
    for(float radiance:frame.environment)invalid(finite(radiance)&&radiance>=0&&radiance<=65504,"Invalid Scene Lit sky radiance");
    invalid(finite(s.sky_rotation)&&s.sky_rotation>=-180&&s.sky_rotation<=180,"Invalid Scene Lit sky rotation");
    if(const auto* error=probe_focus_error(s,frame.lighting_focus))invalid(false,error);
    invalid(!duplicate_mesh_keys,"Duplicate stable mesh identity");
    std::unordered_set<std::uint64_t> light_keys;
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
void SceneLit::draw(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,const SceneLitDraws& scene_draws,const MaterialFrame& materials,FrameStats& stats){state_->draw(frame,extent,depth,scene_draws,materials,stats);}
Result<Preparation> SceneLit::preparation(const ViewSettings& view){return state_->preparation(view);}
void SceneLit::draw_debug(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,FrameStats& stats){state_->draw_debug(frame,extent,depth,stats);}
GfxTexture SceneLit::motion() const{return state_->gbuffer_textures[3];}
void SceneLit::invalidate(bool empty_scene,FrameStats& stats) {
    state_->enabled=false;state_->reset();if(!empty_scene)state_->retire(state_->probe_debug_depth);if(empty_scene)state_->retire_scene();
    stats.scene_lit_resource_bytes=state_->resource_bytes();
}
Result<void> SceneLit::close(){if(!state_)return {};try{state_->close();state_.reset();return {};}catch(const Error& error){return std::unexpected(error);}}
}

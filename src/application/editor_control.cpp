#include "application/editor_control.hpp"
#include "application/application.hpp"
#include "application/control_artifacts.hpp"
#include "editor/view_options.hpp"
#include "editor/json_format.hpp"
#include "editor/material_format.hpp"
#include "platform/windows_path.hpp"
#include "atelier/build_info.hpp"
#include <deque>
#include <future>
#include <limits>
#include <set>
#include <unordered_set>

namespace atelier::application {
namespace {
using control::Json;using control::Failure;
constexpr unsigned repro_view_schema=2;
template<class T> T take(Result<T> value) {if(!value) throw Failure("operation_failed",value.error().message);return std::move(*value);}
void take(Result<void> value) {if(!value) throw Failure("operation_failed",value.error().message);}
void committed(Result<world::Commit> value) {auto commit=take(std::move(value));if(commit.notification_error) throw Failure("operation_failed",commit.notification_error->message);}
std::string text(const Json& value,std::size_t maximum=4096) {
    if(!value.is_string()) throw Failure("invalid_request","Expected a string");auto result=value.get<std::string>();
    if(result.size()>maximum || result.find('\0')!=result.npos) throw Failure("invalid_request","Invalid string length or NUL");
    static_cast<void>(control::wide(result));return result;
}
double number(const Json& value) {if(!value.is_number()) throw Failure("invalid_request","Expected a number");const auto n=value.get<double>();if(!std::isfinite(n)) throw Failure("invalid_request","Expected a finite number");return n;}
unsigned integer(const Json& value,unsigned low,unsigned high) {
    const auto n=number(value);if(n<low || n>high || std::floor(n)!=n) throw Failure("invalid_request","Integer outside supported range");return static_cast<unsigned>(n);
}
math::Vector3 vector3(const Json& value) {
    if(!value.is_array() || value.size()!=3) throw Failure("invalid_request","Expected three vector components");
    const auto f=[](const Json& n) {const auto v=number(n);if(std::abs(v)>100000000) throw Failure("invalid_request","Vector exceeds authoring range");return static_cast<float>(v);};
    return {f(value[0]),f(value[1]),f(value[2])};
}
Json vector3(math::Vector3 v) {return {v.x,v.y,v.z};}
Json camera_json(const editor::CameraState& c) {return {{"view",static_cast<unsigned>(c.view)},{"target",vector3(c.target)},{"yaw",c.yaw},{"pitch",c.pitch},{"distance",c.distance}};}
editor::CameraState camera_state(const Json& args) {
    control::fields(args,{"view","target","yaw","pitch","distance"});
    return {static_cast<editor::CameraView>(integer(args.at("view"),0,static_cast<unsigned>(editor::CameraView::left))),vector3(args.at("target")),
        static_cast<float>(number(args.at("yaw"))),static_cast<float>(number(args.at("pitch"))),static_cast<float>(number(args.at("distance")))};
}
Json view_json(const renderer::ViewSettings& view) {
    Json result=Json::object();
    for(const auto& option:editor::view_options()) {
        const auto value=editor::view_value(view,option.option);
        if(option.kind==editor::ViewValueKind::enumeration) result[option.name]=option.choices[static_cast<std::size_t>(value)];
        else if(option.kind==editor::ViewValueKind::boolean) result[option.name]=value!=0;
        else result[option.name]=value;
    }
    return result;
}
Json probe_volumes_json(const std::array<renderer::ProbeVolumeStats,renderer::probe_volume_limit>& volumes) {
    auto result=Json::array();
    for(std::size_t i=0;i<volumes.size();++i) {
        const auto& volume=volumes[i];
        result.push_back({{"slot",i},{"enabled",volume.enabled},{"rolling",volume.rolling},
            {"minimum",volume.minimum},{"maximum",volume.maximum},{"spacing",volume.spacing},{"counts",volume.counts},
            {"resident",volume.resident},{"updated",volume.updated},{"reset_probes",volume.reset_probes}});
    }
    return result;
}
Json probe_inspection_json(const std::shared_ptr<const renderer::ProbeInspection>& value) {
    if(!value)return nullptr;
    const auto& v=*value;const auto& r=v.receiver;
    constexpr std::array modes{"live","hold_irradiance","hold_distance"};
    const auto selection=v.requested_probe>=0?(v.traced_probe==~0u?"explicit_out_of_range":"explicit"):
        (v.traced_probe==~0u?"automatic_pending":"automatic");
    Json result{{"sequence",v.sequence},{"frame_id",v.frame_id},{"scene_identity",v.scene_identity},{"pixel",v.pixel},
        {"history",modes[static_cast<unsigned>(v.history)]},{"rays",v.rays},{"updates",v.updates},
        {"volumes",probe_volumes_json(v.volumes)},
        {"receiver",{{"status",r.identity[0]},{"volume",r.identity[1]},{"contributors",r.identity[2]},
            {"irradiance_coverage",r.irradiance},{"weight_sum",r.weights[0]},{"dominance",r.weights[1]},{"position_valid",r.position}}},
        {"probes",Json::array()},{"requested_probe",v.requested_probe},{"probe_selection",selection},
        {"traced_probe",v.traced_probe},{"ray_samples",Json::array()}};
    for(const auto& p:v.probes)if(p.identity[2])result["probes"].push_back({
        {"volume",p.identity[0]},{"probe",p.identity[1]},{"support_stage",p.identity[2]},{"age_frames",p.identity[3]},
        {"irradiance_observations",p.irradiance},{"spatial_facing_visibility_weight",p.weights},{"position_distance",p.position},
        {"transport_observations",p.transport},{"response_epoch_min_max_visits",p.response}});
    for(const auto& ray:v.ray_samples)if(ray.identity[3])result["ray_samples"].push_back({
        {"identity",ray.identity},{"direct_luminance",ray.direct},{"surface_roughness",ray.surface},{"sampling",ray.sampling},
        {"light_endpoint_distance",ray.endpoint_distance},{"source_radius_range_area_min_distance_squared",ray.source_parameters}});
    return result;
}
Json performance_json(const editor::RenderPerformance& performance) {
    const auto& sample=performance.snapshot();
    constexpr std::array states{"unavailable","waiting","ready"};
    constexpr std::array aa_names{"off","taa","smaa","smaa_t2x"};
    Json result{{"state",states[static_cast<unsigned>(sample.state)]},{"window_capacity",editor::RenderPerformance::window_size},
        {"publish_interval_ms",250},{"samples",sample.samples},{"sequence",sample.sequence},{"frame_id",sample.frame_id},
        {"scene_identity",sample.scene_identity},{"domain_first_frame",sample.first_frame},
        {"extent",{sample.extent.width,sample.extent.height}},
        {"viewport",{sample.viewport.x,sample.viewport.y,sample.viewport.width,sample.viewport.height}},
        {"configuration",{{"antialiasing",aa_names[static_cast<unsigned>(sample.antialiasing)]},{"direct_candidates",sample.direct_candidates},
            {"direct_reference",sample.direct_reference},{"ddgi",sample.ddgi}}},
        {"logical_resource_bytes",sample.resource_bytes?Json(*sample.resource_bytes):Json(nullptr)},
        {"resource_semantics","Current logical Scene Lit allocation; excludes retired resources and physical residency"},
        {"total_semantics","Sum of eight measured main intervals; excludes UI, guides, probe overlays, capture and CPU Present"},
        {"passes",Json::array()}};
    const auto definitions=editor::RenderPerformance::passes();
    for(std::size_t i=0;i<definitions.size();++i) {
        const auto& pass=definitions[i];const auto& timing=sample.timings[i];const bool active=timing && timing->active_samples;
        result["passes"].push_back({{"id",pass.key},{"title",pass.title},{"parent",pass.parent<0?Json(nullptr):Json(definitions[pass.parent].key)},
            {"depth",pass.depth},{"included_in_total",pass.in_total},
            {"state",!timing?"unavailable":timing->current_active?"measured":"skipped"},
            {"active_samples",timing?timing->active_samples:0},
            {"current_ms",timing && timing->current_active?Json(timing->current):Json(nullptr)},
            {"mean_ms",active?Json(timing->mean):Json(nullptr)},{"p95_ms",active?Json(timing->p95):Json(nullptr)},
            {"share_percent",active && timing->share?Json(*timing->share):Json(nullptr)}});
    }
    return result;
}
renderer::ViewSettings patched_view(renderer::ViewSettings view,const Json& patch,std::array<float,3> focus) {
    if(!patch.is_object()) throw Failure("invalid_request","Display patch must be an object");
    if(patch.value("gtao",false) && patch.value("ssil",false)) throw Failure("invalid_request","GTAO and Local GI are mutually exclusive");
    std::set<editor::ViewOption> assigned;
    for(const auto& [name,value]:patch.items()) {
        const auto* option=editor::view_option(name);if(!option) throw Failure("invalid_request","Unknown display option: "+name);
        if(!assigned.insert(option->option).second) throw Failure("invalid_request","Display option specified through multiple names: "+name);
        double scalar{};
        if(option->kind==editor::ViewValueKind::enumeration) {
            const auto choice=text(value);const auto found=std::ranges::find(option->choices,choice);
            if(found==option->choices.end()) throw Failure("invalid_request","Unsupported display choice: "+choice);
            scalar=static_cast<double>(found-option->choices.begin());
        } else if(option->kind==editor::ViewValueKind::boolean) {
            if(!value.is_boolean()) throw Failure("invalid_request","Display toggle requires a boolean");scalar=value.get<bool>()?1:0;
        } else scalar=number(value);
        take(editor::set_view_option(view,option->option,scalar));
    }
    take(editor::validate_view_options(view,focus));return view;
}
renderer::ViewSettings repro_view(const Json& snapshot,bool legacy_shadow_settings,std::array<float,3> focus) {
    if(!legacy_shadow_settings || !snapshot.is_object())return patched_view({},snapshot,focus);
    auto migrated=snapshot;
    // Only persisted repro input accepts retired settings. Validate the old
    // authoring ranges, then retain today's defaults rather than infer reference mode.
    for(const auto* name:{"shadow_light_budget","shadow_samples"}) {
        const auto found=migrated.find(name);if(found==migrated.end())continue;
        if(migrated.contains("direct_candidates"))throw Failure("invalid_request","Legacy shadow settings cannot be combined with direct_candidates");
        const bool samples=std::string_view(name)=="shadow_samples";
        static_cast<void>(integer(*found,samples?1u:0u,samples?64u:16u));
        migrated.erase(found);
    }
    return patched_view({},migrated,focus);
}
Json scalar_json(const Scalar& scalar,const Runtime& runtime) {
    return std::visit([&](const auto& value)->Json {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,EnumValue>) {
            const auto* type=take(runtime.describe(value.type));
            if(type->enumeration) for(const auto& [name,bits]:type->enumeration->values) if(bits==value.bits) return name;
            return value.bits;
        } else return value;
    },scalar);
}
Scalar scalar_value(const Json& json,const Scalar& current,const Runtime& runtime) {
    return std::visit([&](const auto& value)->Scalar {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,EnumValue>) {
            const auto name=text(json);const auto* type=take(runtime.describe(value.type));
            if(type->enumeration) for(const auto& [choice,bits]:type->enumeration->values) if(name==choice) return EnumValue{value.type,bits};
            throw Failure("invalid_request","Unknown enum choice");
        } else if constexpr(std::is_same_v<T,bool>) {if(!json.is_boolean()) throw Failure("invalid_request","Expected boolean property");return json.get<bool>();}
        else {
            const auto n=number(json);
            if(n<std::numeric_limits<T>::lowest() || n>std::numeric_limits<T>::max() || (std::is_integral_v<T> && std::floor(n)!=n)) throw Failure("invalid_request","Property outside scalar range");
            return static_cast<T>(n);
        }
    },current);
}
struct Operation {std::string_view name,description;bool changes;Json properties;std::vector<std::string> required;};
const std::vector<Operation>& operations() {
    static const Json str{{"type","string"}},obj{{"type","object"}},arr{{"type","array"}};
    static const std::vector<Operation> values{
        {"capabilities","Discover operations, arguments, limits and display options",false,{},{}},
        {"editor.inspect","Read editor state; detail=summary omits object and asset enumeration",false,{{"detail",{{"enum",{"full","summary"}}}}},{}},
        {"project.open","Open an existing project; refuses unsaved scene or source drafts",true,{{"path",str}},{"path"}},
        {"scene.open","Open a project-relative scene; refuses unsaved scene",true,{{"path",str}},{"path"}},
        {"scene.save","Save the current scene through the normal project save command",true,{{"path",str}},{}},
        {"scene.create","Create one entity, mesh, point/rectangle/directional/sky light",true,{{"kind",{{"enum",{"entity","mesh","point","rectangle","directional","sky"}}}},{"mesh",str}},{"kind"}},
        {"scene.select","Select entity handles; optional inspected object must belong to active entity",true,{{"entities",arr},{"active",str},{"object",str}},{"entities"}},
        {"scene.rename","Rename one entity through document history; components use their type names",true,{{"object",str},{"name",str}},{"object","name"}},
        {"scene.set_property","Write one reflected property with its exact scalar/enum type",true,{{"object",str},{"property",str},{"value",Json::object()}},{"object","property","value"}},
        {"material.create","Create a shared material asset under Content",true,{{"path",str}},{"path"}},
        {"texture.import","Import a texture with a Project GUID",true,{{"path",str}},{"path"}},
        {"material.inspect","Read a shared asset or instance object and its resolved reflected parameters",false,{{"asset",str},{"object",str}},{}},
        {"material.set_property","Edit one reflected shared or instance material parameter",true,{{"object",str},{"property",str},{"value",Json::object()}},{"object","property","value"}},
        {"material.set_texture","Assign a texture asset to one shared material input",true,{{"object",str},{"input",str},{"texture",str}},{"object","input","texture"}},
        {"material.assign","Assign a shared material to a component slot; empty asset restores its mesh default",true,{{"component",str},{"slot",str},{"asset",str}},{"component","slot","asset"}},
        {"material.override","Enable or clear explicit instance parameter inheritance override",true,{{"component",str},{"slot",str},{"property",str},{"enabled",{{"type","boolean"}}}},{"component","slot","property","enabled"}},
        {"material.save","Save a shared material asset",true,{{"asset",str}},{"asset"}},
        {"material.undo","Undo one shared material asset edit",true,{{"asset",str}},{"asset"}},
        {"material.redo","Redo one shared material asset edit",true,{{"asset",str}},{"asset"}},
        {"scene.transform","Set or add complete Entity TRS; one undo entry",true,{{"object",str},{"position",arr},{"rotation",arr},{"scale",arr}},{"object","position","rotation","scale"}},
        {"scene.remove_transform","Remove optional Entity space through document history",true,{{"object",str}},{"object"}},
        {"scene.set_space_reference","Set Entity spatial origin independently of composition; null inherits, empty string is world space",true,{{"object",str},{"origin",{{"type",{"string","null"}}}},{"keep_world",{{"type","boolean"}}}},{"object","origin"}},
        {"scene.reparent","Reparent Entity composition with explicit transform preservation",true,{{"object",str},{"parent",{{"type",{"string","null"}}}},{"keep_world",{{"type","boolean"}}},{"index",{{"type","integer"},{"minimum",0}}}},{"object"}},
        {"scene.add_component","Add one discovered component type to the selected entity",true,{{"type",str}},{"type"}},
        {"scene.remove_component","Remove a component through document history",true,{{"object",str}},{"object"}},
        {"scene.delete","Delete selected entities through document history",true,{},{}},
        {"scene.undo","Undo one document operation",true,{},{}},
        {"scene.redo","Redo one document operation",true,{},{}},
        {"camera.set","Restore an exact camera state from editor.inspect",true,{{"state",obj}},{"state"}},
        {"camera.look_at","Aim a perspective camera at a world-space target (metres, +Y up)",true,{{"eye",arr},{"target",arr}},{"eye","target"}},
        {"view.set","Atomically apply a typed display patch using the same rules as menus",true,{{"patch",obj}},{"patch"}},
        {"view.layout","Apply an explicit DDGI coverage layout; retain propagation and display settings",true,{{"layout",{{"enum",{"fit_scene","scene_focus"}}}}},{"layout"}},
        {"repro.export","Export an independent project from the current memory scene; does not save the original",true,{},{}},
        {"repro.open","Open a completed repro bundle and restore its camera, display and entity selection",true,{{"directory",str}},{"directory"}},
        {"render.prepare","Prepare Scene Lit pipelines through a real accepted frame; restores base display settings",true,{{"patch",obj},{"timeout_ms",{{"type","integer"}}}},{}},
        {"render.capture","Capture at accepted-frame ordinals using current display settings",true,{{"sample_frames",arr},{"timeout_ms",{{"type","integer"}}}},{}},
        {"render.compare","Capture sequential temporary display cases; settings never enter document/undo",true,{{"cases",arr},{"sample_frames",arr},{"timeout_ms",{{"type","integer"}}}},{"cases"}},
        {"job.status","Read a bounded asynchronous job; polling does not mutate editor state",false,{{"job_id",str}},{"job_id"}},
        {"job.cancel","Cancel a job idempotently; temporary display is removed at this boundary",false,{{"job_id",str}},{"job_id"}}
    };return values;
}
Json capabilities() {
    auto result=Json{{"operations",Json::array()},{"display_options",Json::object()},{"limits",{{"message_bytes",control::maximum_message},{"request_history",256},{"request_history_bytes",8*1024*1024},{"job_history",16},{"captures",16},{"capture_bytes",256*1024*1024},{"timeout_ms",control::maximum_timeout_ms}}}};
    for(const auto& op:operations()) result["operations"].push_back({{"op",op.name},{"description",op.description},{"requires_state_token",op.changes},
        {"args",{{"type","object"},{"properties",op.properties.is_null()?Json::object():op.properties},{"required",op.required},{"additionalProperties",false}}}});
    for(const auto& option:editor::view_options()) {
        Json schema;
        if(option.kind==editor::ViewValueKind::enumeration) {schema={{"type","string"},{"enum",Json::array()}};for(auto choice:option.choices) schema["enum"].push_back(choice);}
        else if(option.kind==editor::ViewValueKind::boolean) schema={{"type","boolean"}};
        else schema={{"type",option.kind==editor::ViewValueKind::integer?"integer":"number"},{"minimum",option.minimum},{"maximum",option.maximum}};
        if(option.option==editor::ViewOption::grid_plane) schema["availability"]="render.compare only; persistent value follows camera";
        schema["scope"]=editor::world_option(option.option)?"scene":"view";
        schema["label"]=option.label;schema["category"]=option.category;schema["description"]=option.description;
        result["display_options"][option.name]=std::move(schema);
    }
    result["display_aliases"]=Json::object();
    for(const auto& alias:editor::view_option_aliases()) result["display_aliases"][alias.name]=editor::view_option(alias.name)->name;
    result["probe_volume_stats_semantics"]="CPU accepted-frame layout and update schedule; not GPU completion or valid-support counts";
    result["coordinates"]="metres, left-handed +Y up, row-major matrices, row vectors; TRS rotation is quaternion xyzw";
    result["history_policy"]="Renderer histories evolve naturally across cases; no probe reset or GPU-history restoration is promised";
    return result;
}
}

struct EditorControl::Impl {
    Application& app;
    control::Server server;
    std::uint64_t version{1},document_generation{1},input_revision{},next_handle{1},edit_id{1ull<<62};
    struct Observation {
        std::uint64_t document{},session{},input{},project{},code{},view_revision{};
        ui::PixelExtent extent;float dpi{};
        editor::CameraState camera;renderer::ViewSettings view;bool editing{},running{};
        bool operator==(const Observation&) const=default;
    } observed;
    std::unordered_map<ObjectId,std::string> handles;
    std::unordered_map<std::string,ObjectId> objects;
    std::uint64_t pruned_revision{};
    struct Cached {std::string id,request;Json response;std::size_t bytes{};};std::deque<Cached> cache;
    std::size_t cache_bytes{};
    struct Case {std::string name;renderer::ViewSettings view;};
    struct Job {
        std::string id,token;std::shared_ptr<control::Peer> peer;
        Observation context;
        std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);
        std::chrono::steady_clock::time_point deadline,started;
        std::vector<Case> cases;std::vector<unsigned> points;
        std::size_t case_index{},point_index{};unsigned accepted{},skipped{};
        ui::Rect bounds;std::filesystem::path directory;
        std::vector<control_artifacts::Sample> samples;Json manifest,response;
        std::future<Json> writer;
        bool rendering{},preparing{},complete{},recorded{};
        std::uint64_t history_resets{};
    };
    std::deque<std::unique_ptr<Job>> jobs;
    struct AcceptedFrame {
        std::uint64_t id{},count{};ui::Rect bounds;ui::PixelExtent extent;renderer::Camera camera;
        renderer::ViewSettings view;std::array<float,3> environment,lighting_focus;renderer::FrameStats stats;
    };
    std::optional<AcceptedFrame> last_frame;
    Json frame_json() const {
        if(!last_frame) return Json::object();const auto& f=*last_frame;const auto& s=f.stats;
        return {{"frame_id",f.id},{"accepted_count",f.count},{"viewport",{f.bounds.x,f.bounds.y,f.bounds.width,f.bounds.height}},
            {"camera",{{"view",f.camera.view},{"projection",f.camera.projection},{"projection_convention","editor_unjittered"}}},{"effective_view",view_json(f.view)},
            {"environment",f.environment},{"lighting_focus",f.lighting_focus},{"extent",{f.extent.width,f.extent.height}},
            {"frame_stats",{{"history_resets",s.scene_lit_history_resets},{"light_sources",s.scene_lit_light_sources},{"shadow_sources",s.scene_lit_shadow_sources},{"di_candidates",s.scene_lit_di_candidates},
                {"spatial_light_sampling",{{"retained_sources",s.scene_lit_spatial_sources},{"nonempty_cells",s.scene_lit_spatial_cells},{"resource_bytes",s.scene_lit_spatial_bytes},{"uploaded",s.scene_lit_spatial_changed}}},
                {"probe_volumes",probe_volumes_json(s.scene_lit_probe_volumes)},{"probe_volumes_source","cpu_layout_and_schedule"},
                {"probe_inspection",probe_inspection_json(s.scene_lit_probe_inspection)}}},
            {"gpu_sample",{{"valid",s.scene_lit_gpu_timing_valid},{"frame_id",s.scene_lit_gpu_sample_frame_id},{"sequence",s.scene_lit_gpu_sample_sequence},{"pass_ms",s.scene_lit_gpu_ms},
            {"ddgi_pass_ms",s.scene_lit_ddgi_gpu_ms},{"direct_pass_ms",s.scene_lit_direct_gpu_ms},{"ddgi_update_pass_ms",s.scene_lit_ddgi_update_gpu_ms},
            {"direct_reconstruction_pass_ms",s.scene_lit_direct_reconstruction_gpu_ms},
            {"pass_mask",s.scene_lit_gpu_pass_mask},{"ddgi_visibility_rays",s.scene_lit_ddgi_visibility_rays},
            {"resource_bytes",s.scene_lit_resource_bytes},{"resource_bytes_source","current_cpu_logical"},
            {"shadow_rays",s.scene_lit_shadow_rays},{"probe_rays",s.scene_lit_probe_rays}}}};
    }
    Impl(Application& owner,const std::filesystem::path& directory):app(owner),server(directory,{{"revision",build_info::revision},{"configuration",build_info::configuration}}) {
        std::filesystem::create_directories(platform::windows_file_path(server.directory()/"artifacts"/server.identity().at("instance_id").get<std::string>()));observe();
    }
    ~Impl() {server.stop();for(auto& job:jobs) {job->cancel->store(true);if(job->writer.valid()) job->writer.wait();}}
    void observe() {
        Observation current{document_generation,app.document_->revision(),input_revision,app.project_?app.project_->revision():0,
            app.project_code_ && app.project_code_->report()?app.project_code_->report()->published_version:0,app.workspace_.view_revision(),
            app.surface_.extent,app.surface_.dpi_scale,app.camera_.state(),app.workspace_.view,app.document_->editing(),app.play_.running()};
        if(current!=observed) {observed=current;++version;}
    }
    std::string token() const {return server.identity().at("instance_id").get<std::string>()+":"+std::to_string(version);}
    auto busy_states() const {
        const auto ui=app.ui_.interaction_state();
        using State=std::pair<std::string_view,bool>;
        return std::array{State{"document_edit",app.document_->editing()},State{"viewport_tool",app.viewport_tool_.active()},
            State{"camera_orbit",app.camera_drag_ && !app.camera_drag_->pan},State{"camera_pan",app.camera_drag_ && app.camera_drag_->pan},
            State{"camera_fly",app.camera_fly_.has_value()},State{"view_edit",app.workspace_.lighting.editing()},State{"menu",app.workspace_.menu.has_value()},
            State{"ui_pointer",ui.pointer},State{"ui_key",ui.keyboard},State{"text_draft",ui.text_draft},
            State{"ime",ui.composition},State{"clipboard",ui.clipboard},State{"ui_popup",ui.popup}};
    }
    bool busy() const {return std::ranges::any_of(busy_states(),[](const auto& state){return state.second;});}
    Json busy_reasons() const {
        auto reasons=Json::array();for(const auto& [name,active]:busy_states()) if(active) reasons.push_back(name);
        return reasons;
    }
    void idle() const {if(busy()) throw Failure("busy","Finish the current text, menu or viewport interaction first");}
    void replaceable() const {
        idle();if(app.document_->dirty()) throw Failure("unsaved_changes","Save the current scene before replacing it");
        if(!app.document_->dirty_materials().empty()) throw Failure("unsaved_changes","Save the open material assets before replacing the document");
        for(const auto& file:app.workspace_.source_editor.files()) if(file->dirty()) throw Failure("unsaved_changes","Source drafts are still open");
        if(app.play_.running()) throw Failure("busy","Stop the project before replacing its document");
    }
    Job* active() {for(auto& job:jobs) if(!job->complete) return job.get();return nullptr;}
    void cancel(Job& job,std::string_view code,std::string_view message) {
        if(job.complete || job.cancel->exchange(true)) return;job.rendering=false;job.preparing=false;
        job.response=control::failure(code,message);job.response["job_id"]=job.id;job.samples.clear();
        if(!job.writer.valid()) job.complete=true;
    }
    void trim_cache() {
        while(!cache.empty() && (cache.size()>256 || cache_bytes>8*1024*1024)) {cache_bytes-=cache.front().bytes;cache.pop_front();}
    }
    void record_completions() {
        for(auto& job:jobs) if(job->complete && !job->recorded) {
            job->recorded=true;job->response["state_token"]=token();job->response["job_id"]=job->id;
            for(auto& entry:cache) if(entry.id==job->id) {
                cache_bytes-=entry.bytes;entry.response=job->response;
                entry.bytes=entry.request.size()+entry.response.dump().size();cache_bytes+=entry.bytes;break;
            }
        }
        trim_cache();
    }
    void checkpoint() {
        observe();
        if(auto* job=active()) {
            if(!job->peer->connected.load()) cancel(*job,"cancelled","Control connection ended");
            else if(std::chrono::steady_clock::now()>=job->deadline) cancel(*job,"timeout","Control job deadline exceeded");
            else if(job->rendering && (job->token!=token() || busy())) {
                auto changes=Json::array();const auto& before=job->context;
                if(before.document!=observed.document) changes.push_back("document");
                if(before.session!=observed.session) changes.push_back("scene_or_selection");
                if(before.input!=observed.input) changes.push_back("user_input");
                if(before.extent!=observed.extent || before.dpi!=observed.dpi) changes.push_back("surface");
                if(before.project!=observed.project) changes.push_back("project_files");
                if(before.code!=observed.code) changes.push_back("published_code");
                if(before.camera!=observed.camera) changes.push_back("camera");
                if(before.view!=observed.view || before.view_revision!=observed.view_revision) changes.push_back("display_preferences");
                if(before.running!=observed.running) changes.push_back("play_state");
                if(busy()) changes.push_back("active_interaction");
                cancel(*job,"state_changed","Editor changed during the render experiment");job->response["error"]["changes"]=std::move(changes);
                job->response["error"]["previous_extent"]={before.extent.width,before.extent.height};job->response["error"]["current_extent"]={observed.extent.width,observed.extent.height};
            }
            if(job->writer.valid() && job->writer.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
                try {auto result=job->writer.get();if(!job->cancel->load()) job->response={{"status","ok"},{"result",std::move(result)}};}
                catch(const Failure& error) {if(!job->cancel->load()) job->response=control::failure(error.code,error.what());}
                catch(const std::exception& error) {if(!job->cancel->load()) job->response=control::failure("storage_error",error.what());}
                job->response["job_id"]=job->id;job->complete=true;
            }
        }
        record_completions();
    }
    std::string handle(ObjectId object) {
        if(object==ObjectId{}) return {};
        if(handles.size()>editor::Session::maximum_entities+editor::Session::maximum_components && pruned_revision!=app.document_->revision()) prune_handles();
        auto [found,added]=handles.try_emplace(object);
        if(added) {found->second=std::to_string(document_generation)+"/"+std::to_string(next_handle++);objects.emplace(found->second,object);}
        return found->second;
    }
    void prune_handles() {
        pruned_revision=app.document_->revision();
        std::unordered_set<ObjectId> live;
        for(const auto& entity:take(app.document_->world().members())) {live.insert(entity.id());for(const auto& component:take(world::components(app.document_->world().runtime(),entity.id()))) live.insert(component.id());}
        for(const auto id:app.document_->material_objects()) live.insert(id);
        const auto objects_snapshot=live;
        for(const auto id:objects_snapshot) if(auto bindings=world::read_material_bindings(app.document_->world().runtime(),id);bindings)
            for(const auto& binding:*bindings) live.insert(binding.resource.id());
        std::erase_if(handles,[&](const auto& entry) {if(live.contains(entry.first)) return false;objects.erase(entry.second);return true;});
    }
    bool member(ObjectId object) const {
        const auto materials=app.document_->material_objects();if(std::ranges::find(materials,object)!=materials.end()) return true;
        auto entities=app.document_->world().members();if(!entities) return false;
        for(const auto& entity:*entities) {if(entity.id()==object) return true;auto components=world::components(app.document_->world().runtime(),entity.id());
            if(components) for(const auto& component:*components) {
                if(component.id()==object) return true;
                if(auto bindings=world::read_material_bindings(app.document_->world().runtime(),component.id());bindings)
                    for(const auto& binding:*bindings) if(binding.resource.id()==object) return true;
            }}
        return false;
    }
    ObjectId object(const Json& value) {
        const auto name=text(value,80);const auto found=objects.find(name);
        if(found==objects.end() || !member(found->second)) throw Failure("stale_object","Object handle is no longer a document member");return found->second;
    }
    Json object_json(ObjectId id) {
        auto& runtime=app.document_->world().runtime();const auto* type=take(runtime.describe(take(runtime.type_of(id))));
        Json result{{"handle",handle(id)},{"name",control::utf8(app.document_->name(id))},{"type",type->name},
            {"type_description",type->description},{"properties",Json::array()}};
        for(const auto& property:take(app.document_->properties(id))) {
            const auto* prop_type=take(runtime.describe(property.type));
            Json field{{"name",property.name},{"label",property.label},{"category",property.category},{"description",property.description},
                {"type",prop_type->name},{"value",scalar_json(property.value,runtime)}};
            if(prop_type->enumeration) {field["choices"]=Json::array();for(const auto& [name,bits]:prop_type->enumeration->values) {static_cast<void>(bits);field["choices"].push_back(name);}}
            result["properties"].push_back(std::move(field));
        }
        if(take(runtime.is_a(take(runtime.type_of(id)),app.document_->types().entity))) {
            const auto origin=take(world::space_reference(runtime,id));result["space_reference"]=origin?Json(*origin==ObjectId{}?"":handle(*origin)):Json(nullptr);
        }
        if(auto transform=world::read_transform(runtime,id)) result["transform"]={{"position",vector3(transform->position)},
            {"rotation",{transform->rotation.x,transform->rotation.y,transform->rotation.z,transform->rotation.w}},{"scale",vector3(transform->scale)}};
        if(auto bindings=world::read_material_bindings(runtime,id);bindings) {
            result["materials"]=Json::array();
            for(const auto& binding:*bindings) {
                const auto state=take(material::read_material(runtime,binding.resource.id()));
                result["materials"].push_back({{"slot",binding.slot},{"object",handle(binding.resource.id())},{"source",state.source},
                    {"parent",state.parent.registered()?take(material::read_material(runtime,state.parent.id())).source:""},
                    {"parameter_mask",state.overrides},{"assignment_override",binding.overridden}});
            }
        }
        return result;
    }
    Json inspect(bool summary=false) {
        Json result{{"identity",server.identity()},{"project",app.project_?control::utf8(app.project_->root().generic_wstring()):""},
            {"scene_path",control::utf8(app.scene_path_.generic_wstring())},{"dirty",app.document_->dirty()},{"busy",busy()},
            {"busy_reasons",busy_reasons()},{"text_focus",app.ui_.wants_keyboard()},
            {"undo",app.document_->undo_count()},{"redo",app.document_->redo_count()},{"camera",camera_json(app.camera_.state())},
            {"view",view_json(app.workspace_.view)},{"accepted_frames",app.accepted_frames_},{"last_frame",frame_json()}};
        result["performance"]=performance_json(app.workspace_.lighting.performance());
        if(auto* job=active()) result["active_job"]={{"job_id",job->id},{"rendering",job->rendering},{"cancel_requested",job->cancel->load()}};
        if(summary) {result["detail"]="summary";return result;}
        for(const char* field:{"entities","selection","assets","component_types"}) result[field]=Json::array();
        for(const auto& entity:take(app.document_->world().members())) {
            auto value=object_json(entity.id());value["components"]=Json::array();
            for(const auto& component:take(world::components(app.document_->world().runtime(),entity.id()))) value["components"].push_back(object_json(component.id()));
            const auto parent=take(world::parent_entity(app.document_->world().runtime(),entity.id()));value["parent"]=parent!=ObjectId{}?handle(parent):"";
            value["children"]=Json::array();for(const auto& child:take(world::children(app.document_->world().runtime(),entity.id()))) value["children"].push_back(handle(child.id()));
            result["entities"].push_back(std::move(value));
        }
        // Released IDs never retain Runtime objects, and are bounded by current membership.
        prune_handles();
        for(auto entity:app.document_->selected_entities()) result["selection"].push_back(handle(entity));
        result["inspected_object"]=handle(app.document_->selection().object);
        result["dirty_materials"]=app.document_->dirty_materials();
        for(const auto& type:take(app.document_->component_types())) result["component_types"].push_back(type.name);
        for(const auto& asset:editor::mesh_assets()) result["assets"].push_back({{"key",asset.key},{"name",control::utf8(asset.name)},{"kind","mesh"}});
        if(app.project_) for(const auto& entry:app.project_->entries()) result["assets"].push_back({{"key",entry.key},{"path",control::utf8(entry.path.generic_wstring())},{"kind",static_cast<unsigned>(entry.kind)}});
        return result;
    }
    Job& new_job(const std::string& id,const std::shared_ptr<control::Peer>& peer) {
        if(active()) throw Failure("busy","Another control job is still active");
        if(std::ranges::any_of(jobs,[&](const auto& job){return job->id==id;})) throw Failure("request_id_expired","Request payload expired; query the retained job using job.status");
        record_completions();
        while(jobs.size()>=16) jobs.pop_front();
        auto job=std::make_unique<Job>();job->id=id;job->peer=peer;job->token=token();job->context=observed;job->started=std::chrono::steady_clock::now();job->deadline=job->started+std::chrono::seconds(120);
        job->directory=server.directory()/"artifacts"/server.identity().at("instance_id").get<std::string>()/control::unique_id();
        job->manifest={{"protocol",control::protocol_version},{"identity",server.identity()},{"state_token",token()},{"request_id",id},
            {"camera",camera_json(app.camera_.state())},{"view_schema",repro_view_schema},{"view",view_json(app.workspace_.view)}};
        jobs.push_back(std::move(job));return *jobs.back();
    }
    Json start_export(const std::string& id,const std::shared_ptr<control::Peer>& peer) {
        control_artifacts::Repro data;data.scene=take(app.document_->serialize());
        data.materials=take(app.document_->material_assets());
        if(app.project_) {
            if(!app.project_code_ || !app.project_code_->published_build() || !app.workspace_.scene_available) throw Failure("not_ready","Repair the project and load its scene before exporting a repro");
            data.project=app.project_->root();const auto& published=*app.project_code_->published_build();
            data.sources=published.sources;data.ui_source=published.ui_source;data.publication_version=published.published_version;
        }
        data.drafts=Json::array();for(const auto& file:app.workspace_.source_editor.files()) if(file->dirty())
            data.drafts.push_back({{"path",control::utf8(file->path.generic_wstring())},{"text",control::utf8(file->document->text())},{"conflict",file->conflict}});
        for(const auto& entity:take(app.document_->world().members())) for(const auto& component:take(world::components(app.document_->world().runtime(),entity.id())))
            if(auto mesh=world::read_static_mesh(app.document_->world().runtime(),component.id());mesh && mesh->mesh.resolved)
                if(std::ranges::none_of(data.meshes,[&](const auto& entry){return entry.first==mesh->mesh.source;})) data.meshes.emplace_back(mesh->mesh.source,mesh->mesh.resolved);
        auto& job=new_job(id,peer);data.manifest=job.manifest;data.manifest["kind"]="repro";
        data.manifest["original_scene_path"]=control::utf8(app.scene_path_.generic_wstring());data.manifest["original_dirty"]=app.document_->dirty();
        data.manifest["selection_indices"]=Json::array();const auto members=take(app.document_->world().members());
        for(auto selected:app.document_->selected_entities()) for(std::size_t i=0;i<members.size();++i) if(members[i].id()==selected) data.manifest["selection_indices"].push_back(i);
        const auto selection=app.document_->selection();
        if(selection.object!=selection.entity && selection.object!=ObjectId{}) for(std::size_t i=0;i<members.size();++i) if(members[i].id()==selection.entity) {
            const auto components=take(world::components(app.document_->world().runtime(),selection.entity));
            for(std::size_t j=0;j<components.size();++j) if(components[j].id()==selection.object) data.manifest["inspection"]={{"entity",i},{"component",j}};
        }
        data.manifest["source_version"]=data.sources?data.sources->version:0;
        data.manifest["ui_source_version"]=data.ui_source?data.ui_source->version:0;
        data.manifest["publication_version"]=data.publication_version;
        try {job.writer=std::async(std::launch::async,[path=job.directory,data=std::move(data),cancel=job.cancel]() mutable {return control_artifacts::export_repro(path,std::move(data),*cancel);});}
        catch(const std::exception& error) {cancel(job,"storage_error",error.what());return job.response;}
        return {{"status","pending"},{"job_id",id}};
    }
    Json start_render(const std::string& id,const std::shared_ptr<control::Peer>& peer,const Json& args,bool compare,bool prepare=false) {
        if(!app.surface_.extent.width || !app.surface_.extent.height || !app.accepted_view_ || app.accepted_view_->bounds.width<=0 || app.accepted_view_->bounds.height<=0) throw Failure("not_ready","A visible accepted viewport is required");
        std::vector<Case> cases;
        if(compare) {
            const auto& input=args.at("cases");if(!input.is_array() || input.empty() || input.size()>8) throw Failure("invalid_request","Expected 1 to 8 cases");std::set<std::string> names;
            for(const auto& item:input) {control::fields(item,{"name","patch"});auto name=text(item.at("name"),80);
                if(name.empty() || !names.insert(name).second) throw Failure("invalid_request","Case names must be nonempty and unique");
                cases.push_back({name,patched_view(app.workspace_.view,item.at("patch"),app.lighting_focus())});}
        } else if(prepare) {
            auto patch=args.value("patch",Json::object());
            if(!patch.is_object()) throw Failure("invalid_request","Preparation patch must be an object");
            if(!patch.contains("shading")) patch["shading"]="scene_lit";
            if(!patch.contains("wireframe")) patch["wireframe"]="off";
            auto view=patched_view(app.workspace_.view,patch,app.lighting_focus());
            if(view.shading!=renderer::Shading::scene_lit || view.wireframe==renderer::Wireframe::only || app.scene_.meshes.empty()) throw Failure("not_ready","Scene Lit preparation requires a filled view and a mesh instance");
            cases.push_back({"prepare",view});
        } else cases.push_back({"current",app.workspace_.view});
        auto points_json=args.value("sample_frames",Json::array({1}));std::vector<unsigned> points;
        if(!points_json.is_array() || points_json.empty() || points_json.size()*cases.size()>16) throw Failure("invalid_request","Expected 1 to 16 total sample points");
        for(const auto& point:points_json) {auto n=integer(point,1,4096);if(!points.empty() && n<=points.back()) throw Failure("invalid_request","Sample ordinals must increase strictly");points.push_back(n);}
        const auto extent=app.surface_.extent;
        if(std::uint64_t(extent.width)*extent.height*8*points.size()*cases.size()>256*1024*1024) throw Failure("resource_limit","Captured color/depth would exceed 256 MiB");
        const auto timeout=integer(args.value("timeout_ms",Json(prepare?control::maximum_timeout_ms:120000)),1,control::maximum_timeout_ms);
        auto& job=new_job(id,peer);job.cases=std::move(cases);job.points=std::move(points);job.rendering=true;job.preparing=prepare;job.bounds=app.accepted_view_->bounds;
        job.deadline=job.started+std::chrono::milliseconds(timeout);job.manifest["kind"]=prepare?"render.prepare":compare?"render.compare":"render.capture";
        job.manifest["history_policy"]="natural renderer transitions, no full history reset or GPU-history restoration; accepted ordinals exclude skipped frames";
        return {{"status","pending"},{"job_id",id},{"result",{{"phase",prepare?"preparing":"rendering"}}}};
    }
    Json dispatch(std::string_view op,const Json& args,const std::string& id,const std::shared_ptr<control::Peer>& peer) {
        auto& doc=*app.document_;auto& runtime=doc.world().runtime();
        if(op=="capabilities") return capabilities();
        if(op=="editor.inspect") {
            const auto detail=args.contains("detail")?text(args.at("detail")):std::string{"full"};
            if(detail!="full" && detail!="summary") throw Failure("invalid_request","Unsupported editor inspection detail");
            return inspect(detail=="summary");
        }
        if(op=="material.inspect") {
            if(!app.project_) throw Failure("not_ready","No project is open");
            if(args.contains("asset")==args.contains("object")) throw Failure("invalid_request","Specify exactly one material asset or object");
            const auto target=args.contains("asset")?take(doc.material_resource(*app.project_,text(args.at("asset")))).id():object(args.at("object"));
            if(!doc.is_material(target)) throw Failure("invalid_request","Expected a material object");
            const auto state=take(material::read_material(runtime,target));
            auto result=object_json(target);result["asset"]=state.source;
            result["parent"]=state.parent.registered()?take(material::read_material(runtime,state.parent.id())).source:"";
            result["parameter_mask"]=state.overrides;
            result["dirty"]=state.source.empty()?doc.dirty():doc.material_dirty(state.source);return result;
        }
        if(op=="job.status" || op=="job.cancel") {
            const auto job_id=text(args.at("job_id"),80);for(auto& job:jobs) if(job->id==job_id) {
                if(op=="job.cancel") cancel(*job,"cancelled","Cancelled by control client");
                if(job->complete) return job->response;
                return {{"status","pending"},{"job_id",job->id},{"result",{{"phase",job->preparing?"preparing":job->rendering?"rendering":"writing"},{"case_index",job->case_index},{"accepted_frames",job->accepted},{"cancel_requested",job->cancel->load()}}}};
            }
            throw Failure("unknown_job","Job expired or belongs to another instance");
        }
        idle();
        if(op.starts_with("material.") || op=="texture.import") {
            if(!app.project_) throw Failure("not_ready","No project is open");
            auto& project=*app.project_;
            if(op=="material.create") return {{"asset",take(project.create_material(control::wide(text(args.at("path")))))}};
            if(op=="texture.import") return {{"asset",take(project.import_texture(control::wide(text(args.at("path")))))}};
            if(op=="material.save") {
                take(doc.save_material(project,text(args.at("asset"))));const auto refreshed=project.refresh();
                Json result{{"saved",true}};if(!refreshed) result["catalog_warning"]=refreshed.error().message;return result;
            }
            if(op=="material.undo" || op=="material.redo") {take(doc.material_undo(text(args.at("asset")),op=="material.redo"));return Json::object();}
            if(op=="material.assign") {
                const auto component=object(args.at("component"));const auto slot=text(args.at("slot")),asset=text(args.at("asset"));
                if(asset.empty()) take(doc.clear_material(component,slot));else take(doc.assign_material(project,component,slot,asset));return Json::object();
            }
            if(op=="material.set_texture") {
                const auto input=text(args.at("input"));const auto at=std::ranges::find(editor::material_format::names,input);
                if(at==editor::material_format::names.end()) throw Failure("invalid_request","Unknown material input");
                take(doc.set_material_texture(project,object(args.at("object")),static_cast<material::Input>(at-editor::material_format::names.begin()),text(args.at("texture"))));return Json::object();
            }
            auto target=op=="material.override"?ObjectId{}:object(args.at("object"));
            ObjectId component;std::string slot;
            if(op=="material.override") {
                component=object(args.at("component"));slot=text(args.at("slot"));
                const auto bindings=take(world::read_material_bindings(runtime,component));const auto at=std::ranges::find(bindings,slot,&world::MaterialBinding::slot);
                if(at==bindings.end()) throw Failure("invalid_request","Assign a material to this slot first");target=at->resource.id();
            }
            if(!doc.is_material(target)) throw Failure("invalid_request","Expected a material object");
            const auto properties=take(doc.properties(target));const auto name=text(args.at("property"));const auto found=std::ranges::find(properties,name,&editor::Property::name);
            if(found==properties.end()) throw Failure("unknown_property","Unknown reflected material property");
            if(op=="material.override") take(doc.set_material_override(component,slot,found->path,args.at("enabled").get<bool>()));
            else take(doc.set_property(target,found->path,scalar_value(args.at("value"),found->value,runtime),++edit_id));
            return Json::object();
        }
        if(op=="project.open") {replaceable();take(app.open_project(control::wide(text(args.at("path")))));}
        else if(op=="scene.open") {replaceable();take(app.open_scene(control::wide(text(args.at("path")))));}
        else if(op=="scene.save") take(app.save_scene(control::wide(text(args.value("path",Json(""))))));
        else if(op=="scene.create") {
            const auto kind=text(args.at("kind"),40);editor::Command command;
            if(kind=="entity") command=editor::Command::create_entity;else if(kind=="mesh") command=editor::Command::create_static_mesh;
            else if(kind=="point") command=editor::Command::create_point_light;else if(kind=="rectangle") command=editor::Command::create_rectangle_light;
            else if(kind=="directional") command=editor::Command::create_directional_light;else if(kind=="sky") command=editor::Command::create_sky_light;
            else throw Failure("invalid_request","Unknown entity creation kind");
            if(!app.workspace_.command_enabled(command,doc)) throw Failure("unavailable","Creation is unavailable in this document");
            take(app.execute(command,text(args.value("mesh",Json("")))));return {{"object",handle(doc.selection().entity)}};
        } else if(op=="scene.select") {
            const auto& values=args.at("entities");if(!values.is_array() || values.size()>editor::Session::maximum_entities) throw Failure("invalid_request","Invalid entity selection");
            std::vector<ObjectId> selected;for(const auto& value:values) selected.push_back(object(value));
            const auto active_id=args.contains("active")?object(args["active"]):(selected.empty()?ObjectId{}:selected.back());
            if(active_id!=ObjectId{} && std::ranges::find(selected,active_id)==selected.end()) throw Failure("invalid_request","Active entity must be included in the selection");
            const auto inspected=args.contains("object")?object(args["object"]):active_id;
            if(inspected!=active_id) {const auto status=take(world::inspect_component(runtime,inspected));if(!status.valid || status.owner!=active_id || selected.size()!=1) throw Failure("invalid_request","Inspected component must belong to the sole selected entity");}
            take(doc.select_entities(selected,active_id));if(inspected!=active_id) take(doc.select(inspected));app.workspace_.inspecting_asset=false;
        } else if(op=="scene.rename") take(doc.rename(object(args.at("object")),control::wide(text(args.at("name"),1024))));
        else if(op=="scene.set_property") {
            const auto target=object(args.at("object"));const auto name=text(args.at("property"),256);const auto properties=take(doc.properties(target));
            const auto found=std::ranges::find(properties,name,&editor::Property::name);if(found==properties.end()) throw Failure("invalid_request","Unknown reflected property");
            take(doc.set_property(target,found->path,scalar_value(args.at("value"),found->value,runtime),++edit_id));
        } else if(op=="scene.transform") {
            const auto target=object(args.at("object"));
            const auto& rotation=args.at("rotation");if(!rotation.is_array() || rotation.size()!=4) throw Failure("invalid_request","Rotation requires quaternion xyzw");
            world::Trs value{vector3(args.at("position")),{static_cast<float>(number(rotation[0])),static_cast<float>(number(rotation[1])),static_cast<float>(number(rotation[2])),static_cast<float>(number(rotation[3]))},vector3(args.at("scale"))};
            take(doc.set_transform(target,value));
        } else if(op=="scene.remove_transform") take(doc.remove_transform(object(args.at("object"))));
        else if(op=="scene.set_space_reference") {
            std::optional<ObjectId> origin;
            if(!args.at("origin").is_null()) origin=text(args.at("origin"),80).empty()?ObjectId{}:object(args.at("origin"));
            committed(doc.set_space_reference(object(args.at("object")),origin,args.value("keep_world",true)?world::ReparentRule::keep_world:world::ReparentRule::keep_local));
        } else if(op=="scene.reparent") {
            const auto parent=args.contains("parent") && !args.at("parent").is_null()?object(args.at("parent")):ObjectId{};
            const auto rule=args.value("keep_world",true)?world::ReparentRule::keep_world:world::ReparentRule::keep_local;
            committed(doc.reparent(object(args.at("object")),parent,rule,args.value("index",world::append)));
        } else if(op=="scene.add_component") {
            const auto name=text(args.at("type"),256);const auto types=take(doc.component_types());const auto found=std::ranges::find(types,name,&editor::ComponentType::name);
            if(found==types.end()) throw Failure("invalid_request","Component type is not available");committed(doc.add_component(found->type));return {{"object",handle(doc.selection().object)}};
        } else if(op=="scene.remove_component") committed(doc.remove_component(object(args.at("object"))));
        else if(op=="scene.delete") committed(doc.erase_selected());
        else if(op=="scene.undo") {committed(doc.undo());app.sync_world_view();}
        else if(op=="scene.redo") {committed(doc.redo());app.sync_world_view();}
        else if(op=="camera.set" || op=="camera.look_at") {
            auto camera=app.camera_;
            const auto valid=op=="camera.set"?camera.set_state(camera_state(args.at("state"))):camera.look_at(vector3(args.at("eye")),vector3(args.at("target")));
            if(!valid) throw Failure("invalid_request","Invalid camera state or eye/target");
            take(app.set_camera(camera,true));
        }
        else if(op=="view.layout") {
            idle();const auto layout=text(args.at("layout"));
            if(layout!="fit_scene" && layout!="scene_focus") throw Failure("invalid_request","Unsupported DDGI coverage layout");
            take(app.execute(layout=="fit_scene"?editor::Command::fit_probe_scene:editor::Command::fit_probe_scene_focus));
        }
        else if(op=="view.set") {
            if(args.at("patch").contains("grid_plane")) throw Failure("invalid_request","Persistent grid plane follows camera view; temporary render cases may override it");
            auto view=patched_view(app.workspace_.view,args.at("patch"),app.lighting_focus());take(app.workspace_.set_view_settings(*app.document_,view,app.lighting_focus()));app.workspace_.viewport_settings.grid_spacing=view.grid_spacing;
        } else if(op=="repro.export") return start_export(id,peer);
        else if(op=="render.capture" || op=="render.compare" || op=="render.prepare") return start_render(id,peer,args,op=="render.compare",op=="render.prepare");
        else if(op=="repro.open") {
            replaceable();const std::filesystem::path root=control::wide(text(args.at("directory")));
            const auto manifest=control_artifacts::validate_repro(root);
            const auto view_schema=manifest.contains("view_schema")?integer(manifest.at("view_schema"),1,repro_view_schema):1u;
            auto camera=app.camera_;if(!camera.set_state(camera_state(manifest.at("camera")))) throw Failure("invalid_request","Invalid repro camera");
            const auto focus=camera.state().target;
            auto view=repro_view(manifest.at("view"),view_schema==1,{focus.x,focus.y,focus.z});
            auto project=take(editor::Project::open(root/"Project"));
            const auto scene_text=take(editor::read_file(take(project.resolve(project.startup_scene()))));
            const auto scene=editor::json_format::parse(scene_text);
            const auto scene_version=editor::json_format::identity(scene.at("version"));
            if(scene_version>=4) {
                const auto authored=repro_view(scene.at("world"),scene_version==4,{focus.x,focus.y,focus.z});
                if(static_cast<const rendering::SceneLightingSettings&>(view.lit)!=static_cast<const rendering::SceneLightingSettings&>(authored.lit))
                    throw Failure("dependency_mismatch","Repro frame snapshot disagrees with its Scene World settings");
            }
            const bool legacy_scene=scene_version<6;
            const auto& frozen_entities=scene.at(legacy_scene?"actors":"entities");std::vector<unsigned> indices;
            for(const auto& index:manifest.at("selection_indices")) {
                const auto i=integer(index,0,editor::Session::maximum_entities);
                if(i>=frozen_entities.size() || std::ranges::find(indices,i)!=indices.end()) throw Failure("dependency_mismatch","Invalid repro selection");indices.push_back(i);
            }
            std::optional<std::pair<unsigned,unsigned>> inspection;
            if(manifest.contains("inspection")) {
                const auto& value=manifest["inspection"];
                if(legacy_scene) control::fields(value,{"actor","component"});else control::fields(value,{"entity","component"});
                auto a=integer(value.at(legacy_scene?"actor":"entity"),0,editor::Session::maximum_entities),c=integer(value.at("component"),0,editor::Session::maximum_components);
                if(indices.size()!=1 || indices[0]!=a || c>=frozen_entities[a].at("components").size()) throw Failure("dependency_mismatch","Invalid repro component inspection");inspection={{a,c}};
            }
            std::vector<std::pair<std::uint64_t,std::uint64_t>> legacy_objects;
            if(legacy_scene) {
                std::vector<std::pair<std::size_t,std::optional<std::size_t>>> requests;
                for(const auto index:indices) requests.emplace_back(index,std::nullopt);
                if(inspection) requests.emplace_back(inspection->first,inspection->second);
                // Validate and resolve all old identities before replacing the current document.
                legacy_objects=take(editor::Session::legacy_selection(scene_text,requests));
            }
            take(app.open_project(root/"Project",false,ProjectOpenPolicy::require_scene));
            take(app.workspace_.set_view_settings(*app.document_,view,{focus.x,focus.y,focus.z}));app.document_->mark_saved();app.camera_=camera;app.camera_cut_=true;app.workspace_.viewport_settings.grid_spacing=view.grid_spacing;
            const auto members=take(app.document_->world().members());std::vector<ObjectId> selected;
            for(std::size_t index=0;index<indices.size();++index) {
                if(legacy_scene) selected.push_back(take(app.document_->local_object(legacy_objects[index].first)));
                else {const auto i=indices[index];if(i>=members.size()) throw Failure("dependency_mismatch","Repro selection exceeds scene size");selected.push_back(members[i].id());}
            }
            take(app.document_->select_entities(selected,selected.empty()?ObjectId{}:selected.back()));
            if(inspection) {
                if(legacy_scene) take(app.document_->select(take(app.document_->local_object(legacy_objects.back().second))));
                else {const auto components=take(world::components(app.document_->world().runtime(),members[inspection->first].id()));take(app.document_->select(components.at(inspection->second).id()));}
            }
        } else throw Failure("unknown_operation","Operation has no implementation");
        return Json::object();
    }
    Json request(const Json& request,const std::shared_ptr<control::Peer>& peer) {
        app.sync_world_view();
        Json response;std::string id,canonical;bool remember{};
        try {
            control::fields(request,{"protocol","instance_id","request_id","op","args","state_token"});
            if(integer(request.at("protocol"),1,1)!=control::protocol_version || request.at("instance_id")!=server.identity().at("instance_id")) throw Failure("protocol_mismatch","Wrong protocol or editor instance");
            id=text(request.at("request_id"),80);if(id.empty()) throw Failure("invalid_request","Empty request identity");canonical=request.dump();
            for(const auto& entry:cache) if(entry.id==id) {if(entry.request!=canonical) throw Failure("request_id_conflict","Request identity was used for different content");return entry.response;}
            if(std::ranges::any_of(jobs,[&](const auto& job){return job->id==id;})) throw Failure("request_id_expired","Request payload expired; query the retained job using job.status");
            const auto name=text(request.at("op"),80);const auto& known=operations();const auto op=std::ranges::find(known,name,&Operation::name);
            if(op==known.end()) throw Failure("unknown_operation","Operation is not exposed by this editor");
            const auto args=request.value("args",Json::object());if(!args.is_object()) throw Failure("invalid_request","args must be an object");
            for(const auto& item:args.items()) if(!op->properties.contains(item.key())) throw Failure("invalid_request","Unknown argument: "+item.key());
            for(const auto& field:op->required) if(!args.contains(field)) throw Failure("invalid_request","Missing argument: "+field);
            observe();remember=op->changes;
            if(op->changes) {
                if(request.value("state_token","")!=token()) throw Failure("stale_state","Inspect the editor again before changing its state");
            }
            auto result=dispatch(name,args,id,peer);
            if(result.contains("status")) response=std::move(result);else response={{"status","ok"},{"result",std::move(result)}};
        } catch(const Failure& error) {
            response=control::failure(error.code,error.what());
            if(error.code=="busy" && busy()) response["error"]["busy_reasons"]=busy_reasons();
        }
          catch(const Json::exception& error) {response=control::failure("invalid_request",error.what());}
          catch(const std::exception& error) {response=control::failure("operation_failed",error.what());}
        observe();response["state_token"]=token();
        if(remember) {
            const auto bytes=canonical.size()+response.dump().size();
            cache_bytes+=bytes;cache.push_back({id,std::move(canonical),response,bytes});
            trim_cache();
        }
        return response;
    }
};
EditorControl::EditorControl(Application& app,const std::filesystem::path& directory):impl_(std::make_unique<Impl>(app,directory)) {}
EditorControl::~EditorControl()=default;
void EditorControl::poll() {impl_->checkpoint();impl_->server.poll([&](const auto& request,const auto& peer){return impl_->request(request,peer);});impl_->checkpoint();}
void EditorControl::document_replaced() {++impl_->document_generation;impl_->handles.clear();impl_->objects.clear();impl_->last_frame.reset();}
void EditorControl::input(UINT message,WPARAM wparam) {
    if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN || message==WM_CHAR || message==WM_MOUSEWHEEL || message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN ||
        (message==WM_MOUSEMOVE && (wparam&(MK_LBUTTON|MK_RBUTTON|MK_MBUTTON)))) ++impl_->input_revision;
}
renderer::ViewSettings EditorControl::view() const {const auto* job=impl_->active();return job && job->rendering?job->cases[job->case_index].view:impl_->app.workspace_.view;}
bool EditorControl::wants_capture() {impl_->checkpoint();const auto* job=impl_->active();return job && job->rendering && !job->preparing && job->accepted+1==job->points[job->point_index];}
void EditorControl::render_failed(std::string_view message) {if(auto* job=impl_->active();job && job->rendering) impl_->cancel(*job,"render_failure",message);}
void EditorControl::rendered(const renderer::Frame& frame,const renderer::FrameResult& result,renderer::Capture* capture) {
    auto& state=*impl_;state.checkpoint();auto* job=state.active();
    if(job && job->rendering) job->history_resets+=result.stats.scene_lit_history_resets;
    if(result.status!=renderer::FrameStatus::accepted) {if(job && job->rendering) ++job->skipped;return;}
    const auto& bounds=frame.scene_viewport;
    state.last_frame=Impl::AcceptedFrame{result.accepted_id,state.app.accepted_frames_,bounds,result.accepted_extent,frame.camera,frame.view,frame.environment,frame.lighting_focus,result.stats};
    if(!job || !job->rendering) return;
    if(bounds!=job->bounds) {state.cancel(*job,"state_changed","Viewport layout changed");return;}
    ++job->accepted;
    if(job->preparing) {
        job->rendering=false;job->preparing=false;job->complete=true;
        job->response={{"status","ok"},{"job_id",job->id},{"result",{{"phase","ready"},{"frame",state.frame_json()},
            {"preparation_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-job->started).count()}}}};
        return;
    }
    if(job->accepted!=job->points[job->point_index]) return;
    if(!capture || capture->frame_id!=result.accepted_id || capture->extent!=result.accepted_extent) {state.cancel(*job,"render_failure","Capture does not belong to the accepted frame");return;}
    auto metadata=state.frame_json();metadata["case"]=job->cases[job->case_index].name;metadata["accepted_ordinal"]=job->accepted;
    metadata["warmup_accepted_frames"]=job->accepted-1;metadata["skipped_frames"]=job->skipped;
    metadata["case_reported_history_resets"]=job->history_resets;
    job->samples.push_back({std::move(*capture),std::move(metadata)});
    if(++job->point_index==job->points.size()) {
        job->point_index=0;job->accepted=0;job->skipped=0;job->history_resets=0;
        if(++job->case_index==job->cases.size()) {
            job->rendering=false;
            try {job->writer=std::async(std::launch::async,[path=job->directory,samples=std::move(job->samples),manifest=job->manifest,cancel=job->cancel]() mutable {
                return control_artifacts::write_samples(path,std::move(samples),std::move(manifest),*cancel);
            });} catch(const std::exception& error) {state.cancel(*job,"storage_error",error.what());}
        }
    }
}
}

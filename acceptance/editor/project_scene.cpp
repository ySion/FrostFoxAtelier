#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/material_format.hpp"
#include "editor/properties.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include "physics/physics.hpp"
#include <nlohmann/json.hpp>
#include <bit>
#include <chrono>
#include <fstream>
#include <limits>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using Json=nlohmann::json;
const char* acceptance_phase="document initialization";
template<class T> T take(Result<T> value) { if (!value) throw std::runtime_error(value.error().message); return std::move(*value); }
void take(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
void changed(Result<world::Commit> value) { auto commit=take(std::move(value)); if (commit.notification_error) throw std::runtime_error(commit.notification_error->message); }
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Entity : world::Entity { std::int32_t health{100}; };
struct Move : world::Component { float speed{2}; double mass{3.5}; std::uint32_t count{7}; bool allowed{true}; };
struct Spatial : world::Component { float offset{4}; };
struct QueryProbe : physics::SceneQueryComponent { bool observed{};std::uint32_t count{};float distance{};math::Vector3 point{}; };
struct Caption { String text; float size{1}; std::uint32_t lines{1}; };
struct Label : world::Component { String title; Caption caption; };
struct Tags : world::Component { Array<String> names; Array<float> weights; Array<std::uint32_t> counts; };
struct DerivedEnvironment : presentation::EnvironmentComponent {};
struct Evidence { unsigned prepared{},destroyed{},begins{},ends{}; bool fail_tick{},fail_begin{},fail_end{},live_runtime_on_release{true}; };
struct Code {
    Runtime& runtime;
    source::Program program;
    std::shared_ptr<Evidence> evidence;
    Code(Runtime& r,TypeId type,std::shared_ptr<Evidence> e):runtime(r),program(r,type),evidence(std::move(e)) {}
    ~Code() { evidence->live_runtime_on_release &= !runtime.types().empty(); ++evidence->destroyed; }
};
editor::Session::Prepare prepare(std::shared_ptr<Evidence> evidence) {
    return [evidence](world::World& world)->Result<std::shared_ptr<void>> {
        auto& runtime=world.runtime(); ++evidence->prepared;
        take(world.set_failure_policy(world::FailurePolicy::halt));
        take(NativeRegistration<Entity>{runtime,"game.Entity",TypeKind::object}.base<world::Entity>().field<&Entity::health>("health").publish());
        const auto move=take(NativeRegistration<Move>{runtime,"game.Move",TypeKind::object}.base<world::Component>()
            .field<&Move::speed>("speed").field<&Move::mass>("mass").field<&Move::count>("count").field<&Move::allowed>("allowed").publish());
        take(NativeRegistration<Spatial>{runtime,"game.Spatial",TypeKind::object}.base<world::Component>().field<&Spatial::offset>("offset").publish());
        take(NativeRegistration<Caption>{runtime,"game.Caption",TypeKind::record}.field<&Caption::text>("text")
            .field<&Caption::size>("size").range({.hard_min=Scalar{0.f},.hard_max=Scalar{10.f}}).field<&Caption::lines>("lines").publish());
        take(NativeRegistration<Label>{runtime,"game.Label",TypeKind::object}.base<world::Component>()
            .field<&Label::title>("title").field<&Label::caption>("caption").publish());
        take(NativeRegistration<DerivedEnvironment>{runtime,"game.Environment",TypeKind::object}.base<presentation::EnvironmentComponent>().publish());
        auto code=std::make_shared<Code>(runtime,move,evidence);
        auto candidate=take(source::Program::prepare(take(code->program.request("Move.source",
            "void tick(float dt) { self.translate_owner(self.speed * dt, 0.0f, 0.0f); }"))));
        take(code->program.try_publish(candidate));
        auto lease=take(code->program.pin());
        take(world.bind_behavior(move,{
            [evidence](Runtime&,ObjectId)->Result<void>{++evidence->begins; if (evidence->fail_begin) return std::unexpected(Error{ErrorCode::invocation_failed,"begin failed"}); return {};},
            [lease,evidence](Runtime&,ObjectId object,float dt)->Result<void>{
                const std::array<Scalar,1> arguments{dt};
                auto called=lease.invoke(object,arguments); if (!called) return std::unexpected(called.error());
                if (evidence->fail_tick) return std::unexpected(Error{ErrorCode::invocation_failed,"tick failed after enqueue"});
                return {};
            },
            [evidence](Runtime&,ObjectId)->Result<void>{++evidence->ends; if (evidence->fail_end) return std::unexpected(Error{ErrorCode::invocation_failed,"end failed"}); return {};}
        }));
        return std::static_pointer_cast<void>(code);
    };
}
// The scene types plus a component of inline Arrays whose weights and counts are bounded per element.
editor::Session::Prepare prepare_tags(std::shared_ptr<Evidence> evidence) {
    return [base=prepare(std::move(evidence))](world::World& world)->Result<std::shared_ptr<void>> {
        auto code=base(world);if(!code) return code;
        auto& runtime=world.runtime();
        take(runtime.register_types({array_definition<String>(runtime,"game.Strings"),array_definition<float>(runtime,"game.Floats"),
            array_definition<std::uint32_t>(runtime,"game.Counts")}));
        take(NativeRegistration<Tags>{runtime,"game.Tags",TypeKind::object}.base<world::Component>().field<&Tags::names>("names")
            .field<&Tags::weights>("weights").range({.hard_min=Scalar{0.f},.hard_max=Scalar{1.f}})
            .field<&Tags::counts>("counts").range({.hard_min=Scalar{std::uint32_t{0}},.hard_max=Scalar{std::uint32_t{3}}}).publish());
        return code;
    };
}
ObjectId member(editor::Session& session,TypeId type) {
    for (const auto& component:take(world::components(session.world().runtime(),session.selection().entity)))
        if (take(session.world().runtime().type_of(component.id()))==type) return component.id();
    throw std::runtime_error("Missing component");
}
world::FieldPath field(editor::Session& session,TypeId type,std::string_view name) { return {take(session.world().runtime().find_field(type,name))}; }
// A flat Radiance RGBE sky map, 2 * height by height texels of radiance 2^(exponent - 129).
std::string radiance(std::uint32_t height,char exponent) {
    auto bytes="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y "+std::to_string(height)+" +X "+std::to_string(2*height)+"\n";
    for(std::uint32_t i=0;i<2*height*height;++i) bytes+=std::string{char(128),char(128),char(128),exponent};
    return bytes;
}
// Writes a sky map into the project's Content and returns its catalog key.
std::string sky_asset(editor::Project& project,const std::filesystem::path& relative,const std::string& bytes) {
    std::filesystem::create_directories((project.root()/relative).parent_path());
    take(editor::write_file(project.root()/relative,bytes));take(project.refresh());
    const auto at=std::ranges::find_if(project.entries(),[&](const auto& entry){return entry.path.filename()==relative.filename();});
    require(at!=project.entries().end() && at->kind==editor::AssetKind::sky,"A .hdr file was not catalogued as a sky asset.");
    return at->key;
}
void verify_primitive_source(editor::Project& project) {
    acceptance_phase="Primitive Source reflection and persistence";
    editor::Session document;
    auto& runtime=document.world().runtime();
    source::Program program(runtime,document.presentation_types().static_mesh_component);
    auto candidate=take(source::Program::prepare(take(program.request("Primitive.source",
        "void configure() { self.body.enabled = true; self.body.half_extents.x = 2.0f; self.body.material.friction = 0.6f; }"))));
    take(program.try_publish(candidate));
    changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));
    const auto component=member(document,document.presentation_types().static_mesh_component);
    auto lease=take(program.pin());take(lease.invoke(component,{}));
    const auto settings=take(physics::read_settings(runtime,component));
    require(settings.enabled && settings.half_extents.x==2.f && settings.material.friction==.6f,
        "Source did not write the Primitive's shared reflected body configuration.");
    const auto saved=take(document.serialize());
    const auto data=Json::parse(saved);
    require(data["entities"][0]["components"].size()==1 &&
        data["entities"][0]["components"][0]["properties"]["body"]["enabled"]==true &&
        !data["entities"][0]["components"][0].contains("body"),
        "Primitive physics acquired a separate component or a second serialization path.");
    auto reopened=take(editor::Session::deserialize(saved,project));
    changed(reopened->start());take(reopened->update(1.f/60.f));changed(reopened->stop());
    require(take(reopened->serialize())==saved,"Jolt startup changed the authored Primitive configuration.");
    changed(document.close());changed(reopened->close());
    std::puts("Primitive Source/Scene: PASS (generated reflection, JIT fields, generic properties, Jolt lifecycle)");
}
void verify_query_source(editor::Project& project) {
    acceptance_phase="Source query loading";
    auto evidence=std::make_shared<Evidence>();
    const editor::Session::Prepare query_code=[&](world::World& world)->Result<std::shared_ptr<void>> {
        auto& runtime=world.runtime();++evidence->prepared;
        const auto type=take(NativeRegistration<QueryProbe>{runtime,"game.QueryProbe",TypeKind::object}
            .base<physics::SceneQueryComponent>().field<&QueryProbe::observed>("observed").field<&QueryProbe::count>("count")
            .field<&QueryProbe::distance>("distance").field<&QueryProbe::point>("point").publish());
        auto code=std::make_shared<Code>(runtime,type,evidence);
        auto candidate=take(source::Program::prepare(take(code->program.request("QueryProbe.source",
            "void tick(float dt) { self.observed = self.query_ready() && self.has_hit(); self.count = self.hit_count(); self.distance = self.hit_distance(); self.point = self.hit_point(); self.request_query(); }"))));
        take(code->program.try_publish(candidate));const auto lease=take(code->program.pin());
        take(world.bind_behavior(type,{.tick=[lease](Runtime&,ObjectId object,float dt)->Result<void>{
            const std::array<Scalar,1> arguments{dt};auto called=lease.invoke(object,arguments);
            if(!called) return std::unexpected(called.error());return {};
        }}));
        return std::static_pointer_cast<void>(code);
    };
    auto scene=Json::parse(take(editor::read_file(editor::asset_directory()/"examples/Examples/Physic/Scenes/Raycasts.scene")));
    unsigned authored_probes{};
    for(auto& entity:scene["entities"]) {
        for (auto& component:entity["components"]) if (component["type"]=="physics.SceneQueryComponent" &&
            component["properties"]["settings"]["mode"]=="single") {
            component["type"]="game.QueryProbe";component["properties"]["automatic"]=false;++authored_probes;
        }
    }
    require(authored_probes==1,"Raycast fixture must contain exactly one single-query capability to become the Source probe.");
    auto document=take(editor::Session::deserialize(scene.dump(),project,query_code));
    ObjectId probe{};
    for(const auto& entity:take(document->world().members())) for(const auto& component:take(world::components(document->world().runtime(),entity.id())))
        if(take(document->world().runtime().type_of(component.id()))==take(document->world().runtime().native_type<QueryProbe>())) probe=component.id();
    require(probe!=ObjectId{},"Source query probe was not loaded.");
    acceptance_phase="Source query request and result";
    changed(document->start());take(document->update(1.f/60));take(document->update(1.f/60));
    take(document->world().runtime().with_native<const QueryProbe>(probe,[](const auto& value)->Result<void>{
        require(value.observed && value.count==1 && std::abs(value.distance-6.5f)<.01f && std::abs(value.point.z-2.5f)<.01f,
            "Source could not request a trace and read the completed bool/count/distance/vector result.");return {};
    }));
    acceptance_phase="Source query persistence while active";
    const auto saved=take(document->serialize());changed(document->stop());changed(document->close());document.reset();
    auto reopened=take(editor::Session::deserialize(saved,project,query_code));
    for(const auto& entity:take(reopened->world().members())) for(const auto& component:take(world::components(reopened->world().runtime(),entity.id()))) {
        auto ready=reopened->world().runtime().with_native<const physics::SceneQueryComponent>(component.id(),[](const auto& q)->Result<bool>{return q.query_ready();});
        if(ready) require(!*ready,"Scene persistence restored a transient query result.");
    }
    changed(reopened->close());reopened.reset();
    require(evidence->prepared==evidence->destroyed,"Query script lease survived its Session.");
    std::puts("Query Source acceptance: PASS (native inherited methods, fixed-step request, scalar/vector result, transient reset)");
}
void edit(editor::Session& session,ObjectId object,const world::FieldPath& path,Scalar value,std::uint64_t key) {
    take(session.begin_edit(object,path,key)); take(session.preview_edit(key,value)); take(session.commit_edit(key));
}
void verify_stop_reentry() {
    acceptance_phase="Session stop reentry";
    editor::Session* current{};
    ObjectId query;
    bool fail_tick{};
    unsigned ends{}, live_ends{};
    editor::Session document([&](world::World& world)->Result<std::shared_ptr<void>> {
        take(world.set_failure_policy(world::FailurePolicy::halt));
        take(world.bind_behavior(world.types().entity, {
            .tick=[&](Runtime&,ObjectId,float)->Result<void> {
                if (fail_tick) return std::unexpected(Error{ErrorCode::invocation_failed,"reentry tick failure"});
                return {};
            },
            .end=[&](Runtime& runtime,ObjectId)->Result<void> {
                ++ends;
                const auto stopped=current->stop();
                require(!stopped && stopped.error().code==ErrorCode::busy,"End reentered Session stop.");
                // The completed query is real PhysicsScene-owned transient state.
                // Its destructor clears this state, so this detects premature
                // release without relying on a freed allocation still being readable.
                const bool alive=take(runtime.with_native<const physics::SceneQueryComponent>(query,[](const auto& value)->Result<bool> {
                    return value.query_ready();
                }));
                if (alive) ++live_ends;
                require(alive,"Rejected stop destroyed the physics service before End returned.");
                return {};
            }
        }));
        return std::shared_ptr<void>{};
    });
    current=&document;
    changed(document.add_entity());
    take(document.set_transform(document.selection().entity,{}));
    changed(document.add_component(document.physics_types().scene_query_component));
    query=document.selection().object;
    const auto ready=[&] {
        return take(document.world().runtime().with_native<const physics::SceneQueryComponent>(query,[](const auto& value)->Result<bool> {
            return value.query_ready();
        }));
    };
    changed(document.start());take(document.update(1.f/60));require(ready(),"Physics query did not complete before End reentry.");
    fail_tick=true;
    const auto failed=document.update(1.f/60);
    require(!failed && failed.error().message=="reentry tick failure" && ends==1 && live_ends==1 && !document.world().running() && !ready(),
        "Failed Session update lost its error or retained physics participation.");
    changed(document.stop());changed(document.stop());require(ends==1,"Repeated stop repeated End after failure.");
    fail_tick=false;
    changed(document.start());take(document.update(1.f/60));require(ready(),"Physics service did not restart.");
    changed(document.stop());require(ends==2 && live_ends==2 && !ready(),"Normal stop did not release physics after End.");
    changed(document.close());changed(document.close());
    require(ends==2,"Close repeated an already completed End.");
    std::puts("Session stop reentry: PASS (failed Tick, live physics during End, rejected nested stop, restart, idempotent cleanup)");
}
void verify_codec_graph(editor::Project& project) {
    acceptance_phase="final spatial graph persistence";
    {
        editor::Session document;auto& runtime=document.world().runtime();
        changed(document.add_entity());const auto parent=document.selection().entity;take(document.set_transform(parent,{{3,0,0},{},{1,1,1}}));
        changed(document.add_entity());const auto child=document.selection().entity;take(document.set_transform(child,{{7,0,0},{},{1,1,1}}));
        changed(document.reparent(child,parent,world::ReparentRule::keep_local));
        changed(document.set_space_reference(child,ObjectId{},world::ReparentRule::keep_local));
        changed(document.set_space_reference(parent,child,world::ReparentRule::keep_local));
        const auto saved=take(document.serialize());auto reopened=take(editor::Session::deserialize(saved,project));
        require(take(reopened->serialize())==saved && take(world::world_matrix(runtime,parent))[12]==10,
            "A valid final spatial DAG was rejected because its restoration passed through a temporary cycle.");
        const auto parent_id=Json::parse(saved)["roots"][0].get<std::string>();
        const auto restored=take(reopened->local_object(std::stoull(parent_id)));
        require(take(world::world_matrix(reopened->world().runtime(),restored))[12]==10,
            "Final spatial origin plans were not published with membership.");
        auto invalid=Json::parse(saved);invalid["entities"][1]["space"]["origin"]=nullptr;
        require(!editor::Session::deserialize(invalid.dump(),project),"The final origin transaction accepted an actual spatial cycle.");
        auto extreme=Json::parse(saved);
        for(auto& entity:extreme["entities"]) {
            entity["space"]["origin"]="0";entity["space"]["scale"]=Json::array({1e30,1e30,1e30});
        }
        auto independent=take(editor::Session::deserialize(extreme.dump(),project));
        for(const auto& entity:take(independent->world().members())) require(take(world::world_matrix(independent->world().runtime(),entity.id()))[0]==1e30f,
            "Detached assembly rejected finite final spaces because temporary inherited scale multiplication overflowed.");
        require(Json::parse(take(independent->serialize()))==extreme,"Finite independent spaces changed during extreme-scale scene roundtrip.");
    }
    {
        editor::Session document;auto& runtime=document.world().runtime();
        changed(document.add_entity());const auto origin=document.selection().entity;take(document.set_transform(origin,{{7,0,0},{},{1,1,1}}));
        changed(document.add_entity());const auto follower=document.selection().entity;take(document.set_transform(follower,{{2,0,0},{},{1,1,1}}));
        changed(document.set_space_reference(follower,origin,world::ReparentRule::keep_local));
        const auto before=take(world::world_matrix(runtime,follower));take(document.select(follower));
        changed(document.duplicate_selected());const auto copied=document.selection().entity;
        require(copied!=follower && take(world::space_reference(runtime,copied))==std::optional<ObjectId>{origin} &&
            take(world::world_matrix(runtime,copied))==before,"Cloning a spatial follower lost its external origin or local/world pose.");
        changed(document.undo());require(take(world::inspect_entity(runtime,copied)).state==world::EntityState::detached &&
            take(world::space_reference(runtime,copied))==std::optional<ObjectId>{origin},"Clone Undo lost its authored external origin.");
        changed(document.redo());require(document.selection().entity==copied && take(world::world_matrix(runtime,copied))==before,
            "Clone Redo changed identity or failed to restore an outgoing external origin.");
        require(!document.serialize_subtree(copied),"Standalone export silently retained a scene-external spatial origin.");
    }
    acceptance_phase="material graph identity persistence";
    {
        editor::Session document;auto& runtime=document.world().runtime();
        std::array<ObjectId,3> entities,meshes;
        for(std::size_t i=0;i<entities.size();++i) {
            changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));entities[i]=document.selection().entity;
            meshes[i]=member(document,document.presentation_types().static_mesh_component);
        }
        material::State state;state.description.parameters.roughness=.3f;
        const auto shared=take(material::create_material(runtime,state)),independent=take(material::create_material(runtime,state));
        for(std::size_t i=0;i<meshes.size();++i) {
            const std::array<presentation::MaterialBinding,1> bindings{{{"slot/0",i==2?independent:shared,true}}};
            take(presentation::set_material_bindings(runtime,meshes[i],bindings));
        }
        const auto saved=take(document.serialize());const auto data=Json::parse(saved);
        require(data["materials"].size()==2,"Scene material definitions did not preserve shared and equal-but-independent identities.");
        auto reopened=take(editor::Session::deserialize(saved,project));
        require(take(reopened->serialize())==saved,"Material document identities changed after save/reload.");
        const auto bindings_of=[&](editor::Session& session,ObjectId entity) {
            take(session.select(entity));return take(presentation::read_material_bindings(session.world().runtime(),member(session,session.presentation_types().static_mesh_component))).front().resource;
        };
        const auto members=take(reopened->world().members());
        const auto a=bindings_of(*reopened,members[0].id()),b=bindings_of(*reopened,members[1].id()),c=bindings_of(*reopened,members[2].id());
        require(a.id()==b.id() && a.id()!=c.id(),"Scene reload split a shared inline material or merged equal independent materials.");
        auto updated=take(material::read_material(reopened->world().runtime(),a.id()));
        updated.description.parameters.roughness=.6f;take(material::update_material(reopened->world().runtime(),a.id(),updated));
        require(take(material::capture(reopened->world().runtime(),b.id()))->description().parameters.roughness==.6f &&
            take(material::capture(reopened->world().runtime(),c.id()))->description().parameters.roughness==.3f,
            "Restored shared material edits did not propagate according to object identity.");
        const auto key=take(project.create_material("Content/Materials/Codec-parent.material"));
        const auto parent=take(document.material_resource(project,key));const auto instance=take(material::create_instance(runtime,parent));
        for(std::size_t i=0;i<2;++i) {
            const std::array<presentation::MaterialBinding,1> bindings{{{"slot/0",instance,true}}};take(presentation::set_material_bindings(runtime,meshes[i],bindings));
        }
        auto instances=take(editor::Session::deserialize(take(document.serialize()),project));
        const auto instance_members=take(instances->world().members());
        const auto left=bindings_of(*instances,instance_members[0].id()),right=bindings_of(*instances,instance_members[1].id());
        require(left.id()==right.id() && take(material::read_material(instances->world().runtime(),left.id())).parent.id()==take(instances->material_resource(project,key)).id(),
            "Shared inline instance identity or its shared GUID parent was lost on reload.");
        const std::array selected{entities[0],entities[1]};take(document.select_entities(selected,entities[1]));changed(document.duplicate_selected());
        const std::vector<ObjectId> copies(document.selected_entities().begin(),document.selected_entities().end());
        const auto copy_a=bindings_of(document,copies[0]),copy_b=bindings_of(document,copies[1]);
        require(copy_a.id()==copy_b.id() && copy_a.id()!=instance.id() && take(material::read_material(runtime,copy_a.id())).parent.id()==parent.id(),
            "Forest clone lost shared instance aliasing or copied the shared material asset.");
    }
    acceptance_phase="failed graph resource rollback";
    {
        std::string bmp(70,'\0');bmp[0]='B';bmp[1]='M';bmp[2]=70;bmp[10]=54;bmp[14]=40;bmp[18]=2;bmp[22]=2;bmp[26]=1;bmp[28]=32;
        for(std::size_t i=54;i<bmp.size();i+=4) {bmp[i+2]=char(255);bmp[i+3]=char(255);}
        const auto path=project.root()/"Codec.bmp";take(editor::write_file(path,bmp));const auto texture=take(project.import_texture(path));
        editor::MaterialAsset asset;asset.textures[0]=texture;
        const auto incoming=take(project.create_material("Content/Materials/Codec-incoming.material",asset));
        const auto retained=take(project.create_material("Content/Materials/Codec-retained.material"));
        editor::Session source;changed(source.create_static_mesh(take(project.mesh("builtin/cube"))));
        take(source.assign_material(project,member(source,source.presentation_types().static_mesh_component),"slot/0",incoming));
        changed(source.add_entity());auto invalid=Json::parse(take(source.serialize()));
        invalid["entities"][1]["properties"]["unknown_business_field"]={{"$ref","999999"}};
        editor::Session target;changed(target.add_entity());const auto original=target.selection().entity;
        const auto resource=take(target.material_resource(project,retained));const auto properties=take(target.properties(resource.id()));
        const auto roughness=std::ranges::find(properties,std::string("parameters.roughness"),&editor::Property::name);
        require(roughness!=properties.end(),"Material rollback fixture lost roughness reflection.");take(target.set_property(resource.id(),roughness->path,Scalar{.2f},832));
        const auto before=take(target.serialize());target.mark_saved();const auto history=target.undo_count(),count=take(target.material_assets()).size();
        for(unsigned i=0;i<8;++i) take(target.collect_step());const auto objects=target.world().runtime().live_objects();
        require(!target.instantiate_subtree(invalid.dump(),project),"Invalid late business data unexpectedly published a graph.");
        auto cyclic=Json::parse(take(source.serialize()));
        cyclic["entities"][0]["space"]["origin"]=cyclic["entities"][1]["id"];
        cyclic["entities"][1]["space"]={{"position",Json::array({0,0,0})},{"rotation",Json::array({0,0,0,1})},
            {"scale",Json::array({1,1,1})},{"origin",cyclic["entities"][0]["id"]}};
        require(!target.instantiate_subtree(cyclic.dump(),project),"A candidate spatial cycle was partially published after resource preparation.");
        for(unsigned i=0;i<8;++i) take(target.collect_step());
        require(take(target.serialize())==before && target.selection().entity==original && target.undo_count()==history && !target.dirty() &&
            take(target.material_assets()).size()==count && target.loaded_material(incoming)==ObjectId{} && target.loaded_material(retained)==resource.id() &&
            target.material_dirty(retained) && target.world().runtime().live_objects()==objects,
            "Failed composition import changed document IDs/history/selection, leaked new material/texture roots, or discarded a dirty existing resource.");
        changed(target.instantiate_subtree(take(source.serialize()),project));const auto created=target.selected_entities().size();
        require(created==2 && target.loaded_material(incoming)!=ObjectId{},"Composition import could not retry after resource rollback.");
        changed(target.undo());changed(target.redo());require(target.selected_entities().size()==created,"Imported forest history did not restore the entire graph.");
    }
}
// Every value in expected exists in actual at the same path; actual may add defaults.
bool subset(const Json& actual,const Json& expected) {
    if(expected.is_object()) {
        if(!actual.is_object()) return false;
        for(const auto& [key,value]:expected.items()) if(!actual.contains(key) || !subset(actual.at(key),value)) return false;
        return true;
    }
    if(expected.is_array()) {
        if(!actual.is_array() || actual.size()!=expected.size()) return false;
        for(std::size_t i=0;i<expected.size();++i) if(!subset(actual[i],expected[i])) return false;
        return true;
    }
    return actual==expected;
}
std::size_t count(std::span<const world::Diagnostic> issues,world::DiagnosticKind kind) {
    return static_cast<std::size_t>(std::ranges::count(issues,kind,&world::Diagnostic::kind));
}
const Json* find_entity(const Json& scene,const std::string& id) {
    for(const auto& entity:scene["entities"]) if(entity["id"]==id) return &entity;
    return nullptr;
}
void verify_retained_data(editor::Project& project,const Json& baseline,const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase="retained scene data";
    using enum world::DiagnosticKind;
    auto scene=baseline;auto next=std::stoull(scene["next_id"].get<std::string>());
    const auto fresh=[&]{return std::to_string(next++);};
    auto& entity=scene["entities"][0];auto& move=entity["components"][1];
    const auto entity_id=entity["id"].get<std::string>(),move_id=move["id"].get<std::string>();
    move["properties"]["legacy_speed"]=9.5;
    move["properties"]["count"]=-1;
    move["properties"]["allowed"]="yes";
    entity["properties"]["health"]=75.0;
    entity["tags"]=Json::array({"a","b"});
    entity["space"]["pivot"]=Json::array({0,0,0});
    const auto missing_id=fresh(),mesh_id=fresh(),constraint_id=fresh(),gone_id=fresh();
    entity["components"].push_back({{"id",missing_id},{"type","game.Missing"},{"name",""},{"enabled",true},
        {"properties",{{"power",3},{"owner_ref",{{"$ref",entity_id}}}}}});
    entity["components"].push_back({{"id",mesh_id},{"type","game.MissingMesh"},{"name",""},{"enabled",true},{"properties",{{"glow",1.5}}},
        {"mesh","builtin/cube"},{"base_color",{1,0,0}},{"roughness",.5},{"metallic",0},{"emissive",{0,0,0}},{"materials",Json::array()}});
    entity["components"].push_back({{"id",constraint_id},{"type","physics.DistanceConstraintComponent"},{"name",""},{"enabled",true},
        {"properties",{{"target",{{"$ref",move_id}}}}}});
    scene["entities"].push_back({{"id",gone_id},{"type","game.Gone"},{"name","Gone"},{"enabled",true},{"space",nullptr},
        {"children",Json::array()},{"components",Json::array()},{"properties",{{"peer",{{"$ref",entity_id}}}}}});
    scene["roots"].push_back(gone_id);scene["next_id"]=std::to_string(next);
    scene["editor_meta"]={{"layout",1}};scene["world"]["future_option"]=3;
    const auto text=scene.dump();
    require(!editor::Session::deserialize(text,project,prepare(evidence),editor::LoadPolicy::strict),"Strict load accepted unresolved scene data.");

    auto document=take(editor::Session::deserialize(text,project,prepare(evidence)));
    auto& runtime=document->world().runtime();
    const auto issues=document->load_issues();
    require(issues.size()==16 && count(issues,converted)==1 && count(issues,unknown_field)==5 && count(issues,mismatch)==3 &&
        count(issues,unknown_key)==4 && count(issues,unknown_type)==3 && !document->dirty(),
        "Tolerant load did not report exactly the unresolved data, or marked the document dirty.");
    const auto move_type=take(runtime.find_type("game.Move")),entity_type=take(runtime.find_type("game.Entity"));
    const auto entity_object=take(document->local_object(std::stoull(entity_id))),move_object=take(document->local_object(std::stoull(move_id)));
    const auto mesh_object=take(document->local_object(std::stoull(mesh_id))),constraint=take(document->local_object(std::stoull(constraint_id)));
    require(std::get<std::uint32_t>(take(runtime.read(move_object,field(*document,move_type,"count"))))==7 &&
        std::get<bool>(take(runtime.read(move_object,field(*document,move_type,"allowed")))) &&
        std::get<float>(take(runtime.read(move_object,field(*document,move_type,"speed"))))==4 &&
        std::get<std::int32_t>(take(runtime.read(entity_object,field(*document,entity_type,"health"))))==75,
        "Mismatched values did not fall back to defaults, or applicable values were lost.");
    require(document->placeholder_count()==3 && take(presentation::read_static_mesh(runtime,mesh_object)).mesh.source=="builtin/cube" &&
        take(physics::constraint_target(runtime,constraint))==ObjectId{},
        "Placeholders did not keep their native base, or a mistyped reference was applied.");
    {
        // Serials and display order follow document identity; document-level data comes last.
        std::vector<std::uint64_t> order;
        std::uint64_t serial{};
        for(const auto& issue:document->issues()) {
            require(issue.serial==++serial,"Current issue serials did not follow display order.");
            order.push_back(issue.object==ObjectId{}?std::numeric_limits<std::uint64_t>::max():*document->document_identity(issue.object));
        }
        std::vector<std::uint64_t> expected_order{std::stoull(entity_id),std::stoull(move_id),std::stoull(missing_id),
            std::stoull(mesh_id),std::stoull(constraint_id),std::stoull(gone_id)};
        std::ranges::sort(expected_order);expected_order.push_back(std::numeric_limits<std::uint64_t>::max());
        auto objects=order;objects.erase(std::ranges::unique(objects).begin(),objects.end());
        require(std::ranges::is_sorted(order) && objects==expected_order,
            "Current issues did not follow document identity order with document data last.");
    }
    const auto view=take(document->retained(move_object));
    require(view.type.empty() && view.entries.size()==3 && std::ranges::count(view.entries,unknown_field,&editor::RetainedEntry::kind)==1 &&
        std::ranges::count(view.entries,mismatch,&editor::RetainedEntry::kind)==2,"Retained view misclassified saved data.");

    auto expected=scene;expected["entities"][0]["properties"]["health"]=75;
    const auto first=Json::parse(take(document->serialize()));
    require(subset(first,expected),"Saving dropped or altered retained scene data.");
    auto reopened=take(editor::Session::deserialize(first.dump(),project,prepare(evidence)));
    require(reopened->load_issues().size()==15 && Json::parse(take(reopened->serialize()))==first,"Retained data did not round-trip stably.");

    acceptance_phase="retained data released by edits";
    take(document->select(entity_object));
    edit(*document,move_object,field(*document,move_type,"count"),Scalar{std::uint32_t{9}},901);
    const auto count_of=[&]{return Json::parse(take(document->serialize()))["entities"][0]["components"][1]["properties"]["count"];};
    require(count_of()==9,"An edit did not replace the retained value.");
    changed(document->undo());require(count_of()==-1,"Undo did not restore the retained value.");
    changed(document->redo());require(count_of()==9,"Redo did not release the retained value again.");
    take(document->discard_retained(move_object));
    auto discarded=Json::parse(take(document->serialize()))["entities"][0]["components"][1]["properties"];
    require(!discarded.contains("legacy_speed") && discarded["allowed"]==true && discarded["count"]==9,"Discard did not drop the object's retained data.");
    changed(document->undo());discarded=Json::parse(take(document->serialize()))["entities"][0]["components"][1]["properties"];
    require(discarded["legacy_speed"]==9.5 && discarded["allowed"]=="yes","Undo did not restore discarded data.");

    acceptance_phase="retained data in duplicates";
    {
        auto copy=take(editor::Session::deserialize(text,project,prepare(evidence)));
        const auto source=take(copy->local_object(std::stoull(entity_id)));
        take(copy->select(source));changed(copy->duplicate_selected());
        const auto duplicate=copy->selected_entities().back();const auto duplicate_id=std::to_string(*copy->document_identity(duplicate));
        const auto saved=Json::parse(take(copy->serialize()));const auto* record=find_entity(saved,duplicate_id);
        require(record && (*record)["components"].size()==5 && (*record)["components"][2]["type"]=="game.Missing" &&
            (*record)["components"][2]["properties"]["owner_ref"]["$ref"]==duplicate_id && (*record)["components"][3]["type"]=="game.MissingMesh" &&
            (*record)["tags"]==Json::array({"a","b"}),"Duplicate lost retained data or did not remap a reference inside the copy.");
        require(copy->load_issues().size()==16,"Duplicating reported its retained data as a new load.");
    }

    acceptance_phase="retained references to deleted objects";
    {
        auto deleted=take(editor::Session::deserialize(text,project,prepare(evidence)));
        const auto target=take(deleted->local_object(std::stoull(entity_id)));
        const std::array targets{target};take(deleted->select_entities(targets,target));changed(deleted->erase_entities());
        const auto saved=take(deleted->serialize());
        require((*find_entity(Json::parse(saved),gone_id))["properties"]["peer"].is_null() && count(deleted->issues(),reference_dropped)==1,
            "A retained reference to a deleted object was not written as null and reported.");
        require(editor::Session::deserialize(saved,project,prepare(evidence)).has_value(),"Saving after a deletion produced an unloadable scene.");
        changed(deleted->undo());
        require((*find_entity(Json::parse(take(deleted->serialize())),gone_id))["properties"]["peer"]["$ref"]==entity_id,
            "Undoing the deletion did not restore the retained reference.");
    }

    acceptance_phase="structural damage still rejects";
    const auto rejects=[&](Json damaged,const char* message) {
        require(!editor::Session::deserialize(damaged.dump(),project,prepare(evidence)),message);
    };
    auto damaged=scene;damaged["entities"][0]["components"][4]["properties"]["target"]={{"$ref","999999"}};
    rejects(damaged,"A dangling reference in a known field was accepted.");
    damaged=scene;damaged["entities"][0]["components"][1]["properties"]["legacy_speed"]={{"$ref","999999"}};
    rejects(damaged,"A dangling reference in retained data was accepted.");
    damaged=scene;damaged["entities"][0]["components"][1]["properties"]["legacy_speed"]={{"$ref",5}};
    rejects(damaged,"A malformed reference identity was accepted.");
    damaged=scene;damaged["entities"][1]["id"]=entity_id;
    rejects(damaged,"A duplicate document identity was accepted.");
    damaged=scene;damaged["entities"][1]["children"]=Json::array({gone_id});
    rejects(damaged,"A composition cycle was accepted.");
    damaged=scene;damaged["version"]=10;
    rejects(damaged,"A newer scene version was accepted.");
    damaged=scene;damaged["version"]=8;
    rejects(damaged,"An older scene version was accepted.");
}
// String leaves use the scene codec's shared leaf decoder, which also decodes operator parameters strictly.
void verify_string_properties(editor::Project& project,const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase="String properties";
    using enum world::DiagnosticKind;
    editor::Session document(prepare(evidence));auto& runtime=document.world().runtime();
    const auto label_type=take(runtime.find_type("game.Label")),caption_type=take(runtime.find_type("game.Caption"));
    const auto string_type=take(runtime.native_type<String>());
    changed(document.add_entity());const auto entity=document.selection().entity;
    changed(document.add_component(label_type));const auto label=member(document,label_type);
    const auto caption=take(runtime.find_field(label_type,"caption"));
    const world::FieldPath title{take(runtime.find_field(label_type,"title"))},text{caption,take(runtime.find_field(caption_type,"text"))};
    const world::FieldPath size{caption,take(runtime.find_field(caption_type,"size"))},lines{caption,take(runtime.find_field(caption_type,"lines"))};
    const auto set=[&](ObjectId object,const world::FieldPath& path,std::string_view value) {
        String stored{StringView{value}};take(runtime.write(object,path,ValueInput{string_type,&stored}));
    };
    // Each session owns a Runtime with its own field identities, so paths resolve by name per session.
    const auto path=[](editor::Session& session,std::string_view leaf)->world::FieldPath {
        auto& owner=session.world().runtime();const auto type=take(owner.find_type("game.Label"));
        if(leaf=="title") return {take(owner.find_field(type,leaf))};
        return {take(owner.find_field(type,"caption")),take(owner.find_field(take(owner.find_type("game.Caption")),leaf))};
    };
    const auto get=[&](editor::Session& session,ObjectId object,std::string_view leaf) {
        auto& owner=session.world().runtime();String value;
        take(owner.read(object,path(session,leaf),ValueOutput{take(owner.native_type<String>()),&value}));return std::string{value.c_str(),value.size()};
    };
    const auto label_json=[](Json& scene)->Json& {
        for(auto& part:scene["entities"][0]["components"]) if(part["type"]=="game.Label") return part;
        throw std::runtime_error("The saved scene lost its Label component.");
    };
    const std::string gate="Gate \"north\" \xE5\x8C\x97\xE9\x97\xA8";
    set(label,title,gate);set(label,text,"line\nbreak");
    auto saved=Json::parse(take(document.serialize()));
    const auto label_id=std::stoull(label_json(saved)["id"].get<std::string>());
    require(subset(label_json(saved)["properties"],{{"title",gate},{"caption",{{"text","line\nbreak"},{"size",1.0},{"lines",1}}}}),
        "String fields were not saved as JSON strings beside their record siblings.");
    {
        auto reopened=take(editor::Session::deserialize(saved.dump(),project,prepare(evidence)));
        const auto copy=take(reopened->local_object(label_id));
        require(reopened->load_issues().empty() && get(*reopened,copy,"title")==gate && get(*reopened,copy,"text")=="line\nbreak" &&
            Json::parse(take(reopened->serialize()))==saved,"String fields did not round-trip through a scene load.");
    }
    {
        auto damaged=saved;auto& properties=label_json(damaged)["properties"];
        properties["title"]=5;properties["caption"]["size"]=50;properties["caption"]["lines"]=2.0;
        require(!editor::Session::deserialize(damaged.dump(),project,prepare(evidence),editor::LoadPolicy::strict),
            "Strict load accepted a mistyped String or a clamped value.");
        auto tolerant=take(editor::Session::deserialize(damaged.dump(),project,prepare(evidence)));
        const auto copy=take(tolerant->local_object(label_id));const auto issues=tolerant->load_issues();
        auto& owner=tolerant->world().runtime();
        require(issues.size()==3 && count(issues,mismatch)==2 && count(issues,converted)==1 && get(*tolerant,copy,"title").empty() &&
            std::get<float>(take(owner.read(copy,path(*tolerant,"size"))))==10 && std::get<std::uint32_t>(take(owner.read(copy,path(*tolerant,"lines"))))==2,
            "A mistyped String did not keep its default, or its record siblings were not decoded.");
        auto resaved_scene=Json::parse(take(tolerant->serialize()));const auto& resaved=label_json(resaved_scene)["properties"];
        require(resaved["title"]==5 && resaved["caption"]["size"]==50 && resaved["caption"]["text"]=="line\nbreak",
            "The saved values of mismatched leaves were not written back.");
    }

    acceptance_phase="strict properties decoding";
    alignas(Caption) std::array<std::byte,sizeof(Caption)> storage{};take(runtime.initialize_value(caption_type,storage));
    const editor::PropertyTarget record{caption_type,{},storage};
    const auto value_of=[&](const world::FieldPath& path){return take(runtime.read(caption_type,storage,std::span(path).subspan(1)));};
    take(editor::decode_properties(runtime,record,{{"text","hi"},{"size",3},{"lines",2.0}}));
    String decoded;take(runtime.read(caption_type,storage,std::span(text).subspan(1),ValueOutput{string_type,&decoded}));
    require(std::string{decoded.c_str(),decoded.size()}=="hi" && value_of(size)==Scalar{3.f} && value_of(lines)==Scalar{std::uint32_t{2}},
        "Strict decoding did not write a caller-owned record, or rejected an exact integral conversion.");
    const auto rejects=[&](const Json& args,std::string_view message) {
        const auto result=editor::decode_properties(runtime,record,args);
        require(!result && result.error().code==ErrorCode::argument_mismatch && result.error().message.starts_with(message),
            "Strict decoding accepted or misreported an invalid leaf.");
    };
    rejects({{"size",50}},"size: Outside the field's hard bounds.");
    rejects({{"text",5}},"text: Expected a string property.");
    rejects({{"lines",2.5}},"lines: Expected an integer property.");
    rejects({{"nope",1}},"nope: Unknown field.");
    rejects({{"size",{{"x",1}}}},"size: Expected a numeric property.");
    require(value_of(size)==Scalar{3.f},"A rejected strict decode clamped and wrote its value.");
    take(runtime.destroy_value(caption_type,storage));

    // A write hook turns an object decode into a patch without touching the object.
    std::vector<std::pair<world::FieldPath,editor::PropertyLeaf>> patch;
    take(editor::decode_properties(runtime,{label_type,label},{{"title","patched"},{"caption",{{"size",4}}}},{},
        [&](std::span<const FieldId> path,const editor::PropertyLeaf& leaf)->Result<void> {patch.emplace_back(world::FieldPath(path.begin(),path.end()),leaf);return {};}));
    const auto leaf=[&](const world::FieldPath& path)->const editor::PropertyLeaf* {
        const auto at=std::ranges::find(patch,path,&std::pair<world::FieldPath,editor::PropertyLeaf>::first);return at==patch.end()?nullptr:&at->second;
    };
    require(patch.size()==2 && leaf(title) && std::get<String>(*leaf(title))==String{StringView{"patched"}} &&
        leaf(size) && std::get<Scalar>(*leaf(size))==Scalar{4.f} && get(document,label,"title")==gate,
        "A collecting write hook did not receive the decoded String and scalar leaves, or the object changed.");
    require(!editor::decode_properties(runtime,{caption_type,label},Json::object()),"A decode accepted an object of another type.");
    const auto unresolved=[](const Json&,TypeId,ValueForm)->std::optional<ObjectReference>{return std::nullopt;};
    const auto hooked=editor::decode_properties(runtime,{label_type,label},Json::object(),unresolved,
        [](std::span<const FieldId>,const editor::PropertyLeaf&)->Result<void>{return {};});
    require(!hooked && hooked.error().code==ErrorCode::argument_mismatch,"A write hook was combined with reference resolution.");

    acceptance_phase="String properties in duplicates";
    take(document.select(entity));changed(document.duplicate_selected());
    const auto duplicate=document.selection().entity;ObjectId copied{};
    for(const auto& part:take(world::components(runtime,duplicate))) if(take(runtime.type_of(part.id()))==label_type) copied=part.id();
    require(duplicate!=entity && copied!=ObjectId{} && get(document,copied,"title")==gate && get(document,copied,"text")=="line\nbreak",
        "A duplicate did not copy its String fields.");
}
// Inline Arrays save as JSON arrays and decode into a value of their own, stored as one whole leaf.
void verify_array_properties(editor::Project& project,const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase="Array properties";
    using enum world::DiagnosticKind;
    // Each session owns a Runtime with its own identities, so types and fields resolve by name per session.
    struct Fields {TypeId tags,strings,floats;world::FieldPath names,weights;};
    const auto fields=[](const Runtime& runtime) {
        const auto tags=take(runtime.find_type("game.Tags"));
        return Fields{tags,take(runtime.find_type("game.Strings")),take(runtime.find_type("game.Floats")),
            {take(runtime.find_field(tags,"names"))},{take(runtime.find_field(tags,"weights"))}};
    };
    const auto names_of=[&](const Runtime& runtime,ObjectId object) {
        const auto f=fields(runtime);Array<String> value;take(runtime.read(object,f.names,ValueOutput{f.strings,&value}));
        std::vector<std::string> result;for(const auto& text:value) result.emplace_back(text.c_str(),text.size());
        return result;
    };
    const auto weights_of=[&](const Runtime& runtime,ObjectId object) {
        const auto f=fields(runtime);Array<float> value;take(runtime.read(object,f.weights,ValueOutput{f.floats,&value}));
        return std::vector<float>(value.begin(),value.end());
    };
    const auto counts_of=[](const Runtime& runtime,ObjectId object) {
        const auto tags=take(runtime.find_type("game.Tags"));const world::FieldPath counts{take(runtime.find_field(tags,"counts"))};Array<std::uint32_t> value;
        take(runtime.read(object,counts,ValueOutput{take(runtime.find_type("game.Counts")),&value}));
        return std::vector<std::uint32_t>(value.begin(),value.end());
    };
    const auto tags_json=[](Json& scene)->Json& {
        for(auto& part:scene["entities"][0]["components"]) if(part["type"]=="game.Tags") return part;
        throw std::runtime_error("The saved scene lost its Tags component.");
    };
    editor::Session document(prepare_tags(evidence));auto& runtime=document.world().runtime();const auto f=fields(runtime);
    changed(document.add_entity());changed(document.add_component(f.tags));const auto tags=member(document,f.tags);
    const std::vector<std::string> names{"north","\xE5\x8C\x97"};const std::vector<float> weights{.25f,1.f,.5f};
    {
        const Array<String> stored{String{StringView{names[0]}},String{StringView{names[1]}}};const Array<float> bounded{.25f,1.f,.5f};
        take(runtime.write(tags,f.names,ValueInput{f.strings,&stored}));take(runtime.write(tags,f.weights,ValueInput{f.floats,&bounded}));
    }
    auto saved=Json::parse(take(document.serialize()));
    const auto tags_id=std::stoull(tags_json(saved)["id"].get<std::string>());
    require(subset(tags_json(saved)["properties"],{{"names",Json::array({names[0],names[1]})},{"weights",Json::array({.25,1.0,.5})}}),
        "Inline Arrays were not saved as JSON arrays of their elements.");
    {
        auto reopened=take(editor::Session::deserialize(saved.dump(),project,prepare_tags(evidence)));
        const auto copy=take(reopened->local_object(tags_id));auto& owner=reopened->world().runtime();
        require(reopened->load_issues().empty() && names_of(owner,copy)==names && weights_of(owner,copy)==weights &&
            Json::parse(take(reopened->serialize()))==saved,"Inline Arrays did not round-trip through a scene load.");
    }
    {
        auto damaged=saved;auto& properties=tags_json(damaged)["properties"];
        properties["names"]=Json::array({"kept",5});properties["weights"]=Json::array({.5,2.0,-1.0});properties["counts"]=Json::array({2.0,5.0});
        require(!editor::Session::deserialize(damaged.dump(),project,prepare_tags(evidence),editor::LoadPolicy::strict),
            "Strict load accepted a mistyped element or clamped an Array.");
        auto tolerant=take(editor::Session::deserialize(damaged.dump(),project,prepare_tags(evidence)));
        const auto copy=take(tolerant->local_object(tags_id));const auto issues=tolerant->load_issues();auto& owner=tolerant->world().runtime();
        // A clamped Array is reported once, as a mismatch, even when its elements also converted.
        require(issues.size()==3 && count(issues,mismatch)==3 && names_of(owner,copy).empty() && weights_of(owner,copy)==std::vector<float>{.5f,1.f,0.f} &&
            counts_of(owner,copy)==std::vector<std::uint32_t>{2,3},
            "A mistyped element did not leave its Array at the default, or elements outside the hard bounds were not clamped once.");
        require(Json::parse(take(tolerant->serialize()))==damaged,"The saved values of mismatched Arrays were not written back unchanged.");
    }

    acceptance_phase="Array properties decoding";
    const auto unchanged=[&]{return names_of(runtime,tags)==names && weights_of(runtime,tags)==weights;};
    const auto rejects=[&](const Json& record,std::string_view message) {
        const auto result=editor::decode_properties(runtime,{f.tags,tags},record);
        require(!result && result.error().code==ErrorCode::argument_mismatch && result.error().message.starts_with(message) && unchanged(),
            "Strict decoding accepted, misreported or partly wrote an invalid Array.");
    };
    rejects({{"weights",Json::array({.5,2.0})}},"weights: An element is outside the field's hard bounds.");
    rejects({{"names",Json::array({"a",1})}},"names: Expected an array of UTF-8 strings.");
    rejects({{"weights",.5}},"weights: Expected an array property.");
    // A write hook receives each Array as one whole leaf and leaves the object as it was.
    std::vector<std::pair<world::FieldPath,editor::PropertyLeaf>> patch;
    take(editor::decode_properties(runtime,{f.tags,tags},{{"names",Json::array({"patched"})},{"weights",Json::array({.75})}},{},
        [&](std::span<const FieldId> path,const editor::PropertyLeaf& leaf)->Result<void> {patch.emplace_back(world::FieldPath(path.begin(),path.end()),leaf);return {};}));
    const auto leaf=[&](const world::FieldPath& path)->const editor::ArrayLeaf* {
        const auto at=std::ranges::find(patch,path,&std::pair<world::FieldPath,editor::PropertyLeaf>::first);
        return at==patch.end()?nullptr:std::get_if<editor::ArrayLeaf>(&at->second);
    };
    const auto* patched_names=leaf(f.names);const auto* patched_weights=leaf(f.weights);
    require(patch.size()==2 && patched_names && patched_weights && patched_names->type()==f.strings && patched_weights->type()==f.floats &&
        std::ranges::equal(*reinterpret_cast<const Array<String>*>(patched_names->storage().data()),std::array{String{StringView{"patched"}}}) &&
        std::ranges::equal(*reinterpret_cast<const Array<float>*>(patched_weights->storage().data()),std::array{.75f}) && unchanged(),
        "A collecting write hook did not receive whole Array leaves, or the object changed.");
    // A tolerant decode reports from the record, applies the clamped Array and reports an exact conversion.
    std::vector<std::vector<std::string>> kept,conversions;
    const editor::PropertyReport report{
        [&](std::vector<std::string> at,const Json&,world::DiagnosticKind kind,const std::string&) {if(kind==mismatch) kept.push_back(std::move(at));},
        [&](std::vector<std::string> at,const Json&) {conversions.push_back(std::move(at));}};
    take(editor::decode_properties(runtime,{f.tags,tags},take(editor::detail::author_fields(runtime,f.tags)),
        {{"names",Json::array({"a",3})},{"weights",Json::array({.75,2.0})},{"counts",Json::array({1.0})}},report));
    require(kept==std::vector<std::vector<std::string>>{{"names"},{"weights"}} && conversions==std::vector<std::vector<std::string>>{{"counts"}} &&
        names_of(runtime,tags)==names && weights_of(runtime,tags)==std::vector<float>{.75f,1.f} && counts_of(runtime,tags)==std::vector<std::uint32_t>{1},
        "A tolerant decode did not report its leaves from the record, or did not apply the clamped and converted Arrays.");

    acceptance_phase="Array properties in Play";
    auto play_project=take(editor::Project::open(project.root()));
    auto b=take(editor::Session::deserialize(saved.dump(),play_project,prepare_tags(evidence)));b->enter_play();
    auto& rb=b->world().runtime();const auto b_tags=take(b->local_object(tags_id));
    const auto apply=[&](Json properties) {
        const Json op={{"op","set"},{"id",std::to_string(tags_id)},{"properties",std::move(properties)}};
        return take(b->apply_live(Json{{"ops",Json::array({op})}}.dump(),play_project));
    };
    require(apply({{"weights",Json::array({.5})}}).empty() && weights_of(rb,b_tags)==std::vector<float>{.5f},
        "A Play set did not shorten an Array to its one element.");
    take(const_cast<world::World&>(b->world()).add_admission(fields(rb).tags,[&](const world::World& world,std::span<const ObjectId> candidates)->Result<void> {
        for(const auto object:candidates)
            if(std::ranges::contains(names_of(world.runtime(),object),std::string{"Closed"})) return std::unexpected(Error{ErrorCode::invalid_member,"Tags cannot name Closed."});
        return {};
    }));
    const auto rejected=apply({{"names",Json::array({"open","Closed"})}});
    require(rejected.size()==1 && rejected[0].severity==world::Severity::error && rejected[0].kind==command_rejected && names_of(rb,b_tags)==names,
        "A Play Array change the type's rules reject did not keep the whole previous Array.");
    const auto clamped=apply({{"weights",Json::array({.25,2.0})}});
    require(clamped.size()==1 && clamped[0].kind==command_rejected && weights_of(rb,b_tags)==std::vector<float>{.5f},
        "A Play Array change outside the hard bounds was clamped and applied.");
    std::puts("Array properties: PASS (save/reopen, strict and tolerant elements, clamp, write hook, Play shorten and rejected sets)");
}
// Editor document A authors; its Play copy B follows A's changes while it runs.
void verify_play_changes(editor::Project& project,const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase="Play change batches";
    using enum world::DiagnosticKind;
    editor::Session a(prepare(evidence));auto& ra=a.world().runtime();
    const auto entity_type=take(ra.find_type("game.Entity")),move_type=take(ra.find_type("game.Move")),spatial_type=take(ra.find_type("game.Spatial"));
    const auto added=[&](math::Vector3 position) {
        changed(a.add_entity(entity_type));const auto entity=a.selection().entity;take(a.set_transform(entity,{position,{},{1,1,1}}));return entity;
    };
    const auto mover=added({});changed(a.add_component(move_type));const auto move=member(a,move_type);
    const auto label_type=take(ra.find_type("game.Label"));changed(a.add_component(label_type));const auto label=member(a,label_type);
    const auto doomed=added({0,0,4});
    const auto falling=added({0,5,0});
    const auto body_type=take(ra.find_type("world.CollisionComponent"));changed(a.add_component(body_type));const auto body=member(a,body_type);
    physics::BodySettings settings;settings.shape=physics::ShapeKind::sphere;settings.motion=physics::MotionKind::dynamic;settings.radius=.5f;
    take(a.configure_body(body,settings));
    auto lighting=take(a.presentation().environment());lighting.exposure_ev=1.5f;take(a.set_environment(lighting));const auto environment=a.environment();
    const auto saved=Json::parse(take(a.serialize()));
    const auto id=[&](ObjectId object){return std::to_string(*a.document_identity(object));};
    require(find_entity(saved,id(falling))->at("components")[0]["properties"]["body"]["motion"]=="dynamic","Body motion is not saved by member name.");

    auto play_project=take(editor::Project::open(project.root()));
    auto b=take(editor::Session::deserialize(saved.dump(),play_project,prepare(evidence)));b->enter_play();
    auto& rb=b->world().runtime();
    const auto in_b=[&](ObjectId object){return take(b->local_object(*a.document_identity(object)));};
    const auto apply=[&](Json ops){return take(b->apply_live(Json{{"ops",std::move(ops)}}.dump(),play_project));};
    const auto applied=[&](Json ops){
        const auto issues=apply(std::move(ops));if(!issues.empty()) throw std::runtime_error(issues.front().message);
    };
    const auto b_move_type=take(rb.find_type("game.Move"));
    const world::FieldPath speed{take(rb.find_field(b_move_type,"speed"))},count{take(rb.find_field(b_move_type,"count"))};
    const auto position=[&](ObjectId object){return take(world::read_transform(rb,object)).position;};
    const auto step=[&]{take(b->update(1.f/60));};
    require(!a.apply_live(R"({"ops":[]})",project),"An editor document accepted Play changes.");
    require(!b->apply_live(R"({"ops":{}})",play_project),"A malformed change batch was accepted.");
    changed(b->start());for(int frame=0;frame<10;++frame) step();

    // A field edit replaces only its own leaf; the game's write to a sibling stays.
    const auto b_move=in_b(move),b_mover=in_b(mover);
    take(rb.write(b_move,count,Scalar{std::uint32_t{42}}));
    applied({{{"op","set"},{"id",id(move)},{"properties",{{"speed",5.f}}},{"enabled",false}},{{"op","set"},{"id",id(mover)},{"name","Runner"}}});
    require(std::get<float>(take(rb.read(b_move,speed)))==5 && std::get<std::uint32_t>(take(rb.read(b_move,count)))==42 && b->name(b_mover)==L"Runner",
        "A Play field or name change was lost, or overwrote a runtime value.");
    const auto paused=position(b_mover).x;step();
    require(position(b_mover).x==paused,"A disabled component kept ticking.");
    applied({{{"op","set"},{"id",id(move)},{"enabled",true}}});step();
    require(std::abs(position(b_mover).x-paused-5.f/60)<1e-5f,"A re-enabled component did not tick with its new speed.");
    // String leaves apply as whole values; an admission that rejects one restores the whole previous String.
    const auto b_label_type=take(rb.find_type("game.Label")),b_string=take(rb.native_type<String>());
    const world::FieldPath title{take(rb.find_field(b_label_type,"title"))};
    const auto title_of=[&](const world::World& world,ObjectId object) {
        String value;take(world.runtime().read(object,title,ValueOutput{b_string,&value}));return std::string{value.c_str(),value.size()};
    };
    const auto b_label=in_b(label);
    applied({{{"op","set"},{"id",id(label)},{"properties",{{"title","North gate"}}}}});
    require(title_of(b->world(),b_label)=="North gate","A Play String field change was not applied.");
    take(const_cast<world::World&>(b->world()).add_admission(b_label_type,[&](const world::World& world,std::span<const ObjectId> candidates)->Result<void> {
        for(const auto object:candidates)
            if(title_of(world,object)=="Closed") return std::unexpected(Error{ErrorCode::invalid_member,"A Label cannot be titled Closed."});
        return {};
    }));
    const auto closed=apply({{{"op","set"},{"id",id(label)},{"properties",{{"title","Closed"}}}}});
    require(closed.size()==1 && closed[0].severity==world::Severity::error && closed[0].kind==command_rejected &&
        title_of(b->world(),b_label)=="North gate","A rejected Play String change did not restore the whole previous value.");

    // An authored pose replaces the simulated one, and body settings reach physics.
    const auto b_falling=in_b(falling);
    require(position(b_falling).y<5,"The dynamic body did not fall.");
    applied({{{"op","set"},{"id",id(falling)},{"space",{{"position",{3,10,0}}}}}});step();
    const auto teleported=position(b_falling);
    require(std::abs(teleported.x-3)<1e-4f && teleported.y>9.5f && teleported.y<10,"A moved dynamic body snapped back to its simulated pose.");
    applied({{{"op","set"},{"id",id(body)},{"properties",{{"body",{{"motion","stationary"}}}}}}});
    step();const auto resting=position(b_falling);step();step();
    require(position(b_falling)==resting,"A Play body setting did not reach physics.");

    // Scalar leaves of one set are one group: moving a probe volume past its old bounds fails leaf by leaf in either order.
    applied({{{"op","set"},{"id",id(environment)},{"properties",{{"volume0",{{"minimum",{{"x",-20},{"y",10}}},{"maximum",{{"x",-10},{"y",20}}}}}}}}});
    const auto moved_volume=take(b->presentation().environment()).probe_volumes[0];
    require(moved_volume.minimum[0]==-20 && moved_volume.maximum[0]==-10 && moved_volume.minimum[1]==10 && moved_volume.maximum[1]==20 &&
        take(b->presentation().environment()).exposure_ev==1.5f,
        "A Play set did not write a moved probe volume as one group.");
    require(!apply({{{"op","set"},{"id",id(environment)},{"properties",{{"volume0",{{"maximum",{{"x",-30}}}}}}}}}).empty() &&
        take(b->presentation().environment()).probe_volumes[0].maximum[0]==-10,"A Play set that breaks the Environment's rules was applied.");
    const auto key=take(project.create_material("Content/Materials/Live.material"));
    require(b->loaded_material(key)==ObjectId{},"Play loaded an unreferenced material.");
    editor::MaterialAsset asset;asset.description.parameters.roughness=.25f;
    applied({{{"op","resource"},{"key",key},{"material",editor::material_format::encode(asset)}}});
    const auto loaded=b->loaded_material(key);
    require(loaded!=ObjectId{} && take(material::read_material(rb,loaded)).description.parameters.roughness==.25f,"A new material asset did not reach Play.");
    asset.description.parameters.roughness=.75f;
    applied({{{"op","resource"},{"key",key},{"material",editor::material_format::encode(asset)}}});
    require(b->loaded_material(key)==loaded && take(material::read_material(rb,loaded)).description.parameters.roughness==.75f,
        "A material asset edit did not update the loaded material in place.");

    // Added objects Begin once with their editor identity; removal Ends them and frees the identity.
    const auto runner=added({0,0,8});changed(a.add_component(move_type));
    const Json add={{"op","add"},{"parent",nullptr},{"index",nullptr},{"graph",Json::parse(take(a.serialize_live_subtree(runner)))}};
    auto begins=evidence->begins;applied({add});
    require(evidence->begins==begins+1,"An added Entity did not Begin exactly once.");
    step();require(position(in_b(runner)).x>0,"An added Entity did not tick.");
    const auto ends=evidence->ends;applied({{{"op","remove"},{"id",id(runner)}}});
    require(evidence->ends==ends+1 && !b->local_object(*a.document_identity(runner)),"A removed Entity did not End or stayed alive.");
    begins=evidence->begins;applied({add});
    require(evidence->begins==begins+1 && b->local_object(*a.document_identity(runner)),"Restoring a removed identity failed.");
    const auto members=take(b->world().members()).size();const auto rejected=apply({add});
    require(rejected.size()==1 && rejected[0].severity==world::Severity::error && rejected[0].kind==command_rejected &&
        take(b->world().members()).size()==members,"An add of an identity Play already has created a second object.");

    take(a.select(mover));changed(a.add_component(spatial_type));const auto spatial=member(a,spatial_type);
    edit(a,spatial,field(a,spatial_type,"offset"),Scalar{9.f},31);
    applied({{{"op","attach"},{"entity",id(mover)},{"index",nullptr},{"graph",Json::parse(take(a.serialize_live_components(mover,std::array{spatial})))}}});
    const auto b_spatial=in_b(spatial);
    require(take(world::inspect_component(rb,b_spatial)).owner==b_mover &&
        std::get<float>(take(rb.read(b_spatial,{take(rb.find_field(take(rb.find_type("game.Spatial")),"offset"))})))==9,"An attached component lost its owner or values.");
    changed(a.reparent(runner,mover));
    applied({{{"op","move"},{"id",id(runner)},{"parent",id(mover)},{"index",nullptr},
        {"space",Json::parse(take(a.serialize_live_subtree(runner)))["entities"][0]["space"]}}});
    require(take(world::parent_entity(rb,in_b(runner)))==b_mover && take(world::read_transform(rb,in_b(runner)))==take(world::read_transform(ra,runner)),
        "A Play reparent did not keep the editor's parent and local transform.");

    // An index is the final position in the destination list (roots for a null parent); Play clamps one past its own list.
    const auto roots=[&]{
        std::vector<ObjectId> result;
        for(const auto& item:take(b->world().members())) if(take(world::parent_entity(rb,item.id()))==ObjectId{}) result.push_back(item.id());
        return result;
    };
    const auto children_of=[&](ObjectId parent){std::vector<ObjectId> result;for(const auto& child:take(world::children(rb,parent))) result.push_back(child.id());return result;};
    const auto placed=[&](Json parent,Json index){
        const auto entity=added({});
        applied({{{"op","add"},{"parent",parent},{"index",index},{"graph",Json::parse(take(a.serialize_live_subtree(entity)))}}});
        return entity;
    };
    const auto b_doomed=in_b(doomed),b_runner=in_b(runner),b_environment=take(world::inspect_component(rb,in_b(environment))).owner;
    // Member position 1 is the mover's child, so a root ordinal read as a member position fails here.
    const auto second=placed(nullptr,1),last=placed(nullptr,99),first_child=placed(id(mover),0),last_child=placed(id(mover),99);
    require(roots()==std::vector{b_mover,in_b(second),b_doomed,b_falling,b_environment,in_b(last)} && children_of(b_mover)==std::vector{in_b(first_child),b_runner,in_b(last_child)},
        "An add index is not the final position in its destination list, or did not clamp.");
    applied({{{"op","move"},{"id",id(last)},{"parent",nullptr},{"index",0},{"space",nullptr}},
        {{"op","move"},{"id",id(second)},{"parent",nullptr},{"index",99},{"space",nullptr}},
        {{"op","move"},{"id",id(first_child)},{"parent",id(mover)},{"index",99},{"space",nullptr}}});
    require(roots()==std::vector{in_b(last),b_mover,b_doomed,b_falling,b_environment,in_b(second)} && children_of(b_mover)==std::vector{b_runner,in_b(last_child),in_b(first_child)},
        "A move index is not the final position in its destination list, or did not clamp.");

    // A mesh imported after Play opened its own Project still resolves.
    const auto obj=project.root().parent_path()/"Imported.obj";
    {std::ofstream file(obj);file<<"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";}
    changed(a.create_static_mesh(take(project.mesh(take(project.import_mesh(obj))))));const auto imported=a.selection().entity;
    physics::BodySettings triangle;triangle.shape=physics::ShapeKind::triangle_mesh;
    take(a.configure_body(member(a,take(ra.find_type("world.StaticMeshComponent"))),triangle));
    applied({{{"op","add"},{"parent",nullptr},{"index",nullptr},{"graph",Json::parse(take(a.serialize_live_subtree(imported)))}}});
    require(std::ranges::any_of(take(b->presentation().render_items()),[&](const auto& item){return item.entity==in_b(imported);}),
        "A mesh imported during Play did not resolve in the Play copy.");
    // A rescale outgrows the shape cooked at load; physics cooks the new scale once instead of dropping the body.
    auto& simulation=*b->physics_scene();
    applied({{{"op","set"},{"id",id(imported)},{"space",{{"position",{20,0,0}},{"rotation",{0,0,0,1}},{"scale",{2,2,2}}}}}});
    step();const auto cooks=simulation.last_step().runtime_cooks;step();
    const auto hit=take(simulation.raycast({21.5f,.2f,5},{21.5f,.2f,-5}));
    require(cooks==1 && simulation.last_step().runtime_cooks==0 && !hit.hits.empty() && hit.hits.front().body.entity==in_b(imported),
        "A rescaled triangle body was not rebuilt at its new scale, or was cooked more than once.");

    // The game's own destruction skips that item only.
    changed(world::destroy_entity(rb,in_b(doomed)));
    const auto skipped=apply({{{"op","set"},{"id",id(doomed)},{"name","Gone"}},{{"op","set"},{"id",id(mover)},{"name","Still here"}}});
    require(skipped.size()==1 && skipped[0].severity==world::Severity::warning && skipped[0].kind==reference_dropped && b->name(b_mover)==L"Still here",
        "A change to an object the game destroyed was not skipped and reported alone.");
    // A reference whose target the game destroyed is null; the rest of the object arrives.
    const auto constraint_type=take(ra.find_type("physics.DistanceConstraintComponent"));
    const auto tethered=added({0,0,12});changed(a.add_component(constraint_type));const auto constraint=member(a,constraint_type);
    auto tether=Json::parse(take(a.serialize_live_subtree(tethered)));
    auto& tether_properties=tether["entities"][0]["components"][0]["properties"];
    tether_properties["target"]={{"$ref",id(doomed)}};tether_properties["minimum"]=2.f;tether_properties["maximum"]=3.f;
    const auto dropped=apply({{{"op","add"},{"parent",nullptr},{"index",nullptr},{"graph",tether}}});
    const auto b_constraint=in_b(constraint);const auto b_constraint_type=take(rb.find_type("physics.DistanceConstraintComponent"));
    require(dropped.size()==1 && dropped[0].severity==world::Severity::warning && dropped[0].kind==reference_dropped &&
        take(rb.read_reference(b_constraint,{take(rb.find_field(b_constraint_type,"target"))})).id()==ObjectId{} &&
        std::get<float>(take(rb.read(b_constraint,{take(rb.find_field(b_constraint_type,"maximum"))})))==3,
        "A reference to an object the game destroyed was not nulled and reported, or stopped the rest of the add.");

    // A Play copy never assigns document identities, so they cannot collide with the editor's.
    changed(b->stop());take(b->select(b_mover));
    const auto identity_rejected=[](const auto& result,const char* message) {
        if(result) throw std::runtime_error(message);
        if(result.error().message.find("does not assign document identities")==std::string::npos) throw std::runtime_error(result.error().message);
    };
    identity_rejected(b->duplicate_selected(),"A Play copy duplicated with new document identities.");
    identity_rejected(b->instantiate_subtree(take(a.serialize_subtree(mover)),play_project),"A Play copy imported with new document identities.");
    std::unordered_set<std::string> ids;
    const auto collect=[&](const Json& scene,auto&& visit) {
        for(const auto& entity:scene["entities"]) {visit(entity["id"]);for(const auto& part:entity["components"]) visit(part["id"]);}
    };
    collect(Json::parse(take(a.serialize())),[&](const Json& value){ids.insert(value.get<std::string>());});
    collect(Json::parse(take(b->serialize())),[&](const Json& value){
        if(!ids.contains(value.get<std::string>())) throw std::runtime_error("A Play copy wrote an identity the editor never assigned.");
    });
    changed(b->close());changed(a.close());

    // A Ragdoll follows its controller, so Play cannot follow an authored move of it.
    const auto ragdoll_scene=take(editor::read_file(editor::asset_directory()/"examples/Examples/Physic/Scenes/Ragdoll.scene"));
    std::string ragdoll;
    for(const auto& entity:Json::parse(ragdoll_scene)["entities"])
        for(const auto& part:entity["components"]) if(part["type"]=="physics.RagdollComponent") ragdoll=entity["id"];
    auto ragdolls=take(editor::Session::deserialize(ragdoll_scene,play_project));ragdolls->enter_play();
    const auto ragdoll_root=take(ragdolls->local_object(std::stoull(ragdoll)));
    changed(ragdolls->start());for(int frame=0;frame<3;++frame) take(ragdolls->update(1.f/60));
    const auto posed=take(world::read_transform(ragdolls->world().runtime(),ragdoll_root));
    const auto refused=take(ragdolls->apply_live(Json{{"ops",{{{"op","set"},{"id",ragdoll},{"space",{{"position",{1,2,3}}}}}}}}.dump(),play_project));
    require(refused.size()==1 && refused[0].severity==world::Severity::warning && refused[0].kind==command_rejected &&
        take(world::read_transform(ragdolls->world().runtime(),ragdoll_root))==posed,"An authored move Play cannot follow was applied or reported as divergence.");
    changed(ragdolls->close());
}
// Editor document A forwards its own changes; applying each batch to its Play copy B keeps B equal to A.
void verify_live_forwarding(editor::Project& project,const std::shared_ptr<Evidence>& evidence) {
    acceptance_phase="Play change forwarding";
    const auto material_key=take(project.create_material("Content/Materials/Forwarded.material"));
    editor::Session a(prepare(evidence));auto& ra=a.world().runtime();
    const auto entity_type=take(ra.find_type("game.Entity")),move_type=take(ra.find_type("game.Move")),spatial_type=take(ra.find_type("game.Spatial"));
    const auto added=[&](math::Vector3 position) {
        changed(a.add_entity(entity_type));const auto entity=a.selection().entity;take(a.set_transform(entity,{position,{},{1,1,1}}));return entity;
    };
    const auto first=added({1,2,3});changed(a.add_component(move_type));const auto move=member(a,move_type);
    const auto second=added({0,0,4});
    changed(a.create_static_mesh(take(project.mesh("builtin/cube")),{0,1,0}));const auto mesh=member(a,a.presentation_types().static_mesh_component);
    take(a.assign_material(project,mesh,"slot/0",material_key));
    const auto property=[&](ObjectId object,std::string_view name) {
        const auto properties=take(a.properties(object));const auto at=std::ranges::find(properties,name,&editor::Property::name);
        require(at!=properties.end(),"A forwarded property is missing.");return at->path;
    };
    const auto shared=take(presentation::read_material_bindings(ra,mesh)).front().resource.id();
    const auto roughness=property(shared,"parameters.roughness"),mesh_roughness=property(mesh,"material.roughness");
    const auto speed=field(a,move_type,"speed"),mass=field(a,move_type,"mass");

    auto play_project=take(editor::Project::open(project.root()));
    auto b=take(editor::Session::deserialize(take(a.serialize()),play_project,prepare(evidence)));b->enter_play();
    auto& rb=b->world().runtime();
    const auto in_b=[&](ObjectId object){return take(b->local_object(*a.document_identity(object)));};
    require(take(a.take_live_ops()).empty(),"A document recorded Play changes before forwarding started.");
    a.set_live_forwarding(true);
    const auto scene=[](const editor::Session& session){auto value=Json::parse(take(session.serialize()));value.erase("next_id");return value;};
    const auto sync=[&](const std::string& step,bool compare=true) {
        Json ops=Json::array();for(const auto& op:take(a.take_live_ops())) ops.push_back(Json::parse(op));
        const auto issues=take(b->apply_live(Json{{"ops",ops}}.dump(),play_project));
        if(!issues.empty()) throw std::runtime_error(step+": "+issues.front().message);
        if(compare && scene(a)!=scene(*b)) throw std::runtime_error(step+": the Play copy differs from the document.");
        return ops.size();
    };
    const auto each=[&](const std::string& step,auto&& change) {
        change();if(!sync(step)) throw std::runtime_error(step+": the change sent nothing.");
        changed(a.undo());sync(step+" undo");changed(a.redo());sync(step+" redo");
    };
    // The first World Settings edit reaches Play as one add carrying its values; undoing it removes the Environment.
    const rendering::SceneLightingSettings defaults{};
    auto lighting=defaults;lighting.exposure_ev=1.5f;lighting.sky_enabled=true;
    take(a.set_environment(lighting));
    require(sync("World settings creation")==1 && b->environment()!=ObjectId{} && take(b->presentation().environment())==lighting,
        "The first World Settings edit did not reach Play as one add with its values.");
    changed(a.undo());sync("World settings creation undo");
    require(b->environment()==ObjectId{} && take(b->presentation().environment())==defaults,"Undoing the first World Settings edit left Play with an Environment.");
    changed(a.redo());sync("World settings creation redo");
    require(take(b->presentation().environment())==lighting,"Redoing the first World Settings edit did not restore Play's Environment.");
    lighting.exposure_ev=2.5f;lighting.probe_volumes[0].minimum[0]=7;lighting.probe_volumes[0].maximum[0]=9;
    each("World settings",[&]{take(a.set_environment(lighting));});
    require(take(b->presentation().environment())==lighting,"A later World Settings edit did not reach Play.");
    // A sky map reaches Play as its key, which Play resolves in its own Project, refreshed for a file added after Play opened it.
    const auto sky_key=sky_asset(project,"Content/Sky/Forwarded.hdr",radiance(1,char(130)));
    const auto play_sky=[&]{return take(b->presentation().environment()).sky_map;};
    each("Sky map",[&]{take(a.set_sky_map(take(project.sky_map(sky_key))));});
    require(play_sky() && play_sky()==take(play_project.sky_map(sky_key)).map,"A sky map did not reach Play through its own Project.");
    const auto environment_entity=take(world::inspect_component(ra,a.environment())).owner;
    take(a.select(environment_entity));changed(a.erase_selected());sync("Environment removal");
    changed(a.undo());sync("Environment restoration");
    require(play_sky()==take(play_project.sky_map(sky_key)).map,"An added Environment did not carry its sky map key to Play.");
    // A key Play cannot resolve only warns, as the document keeps it unresolved too; a malformed one is rejected.
    const auto environment_id=std::to_string(*a.document_identity(a.environment()));
    const auto set_play_sky=[&](Json value) {
        return take(b->apply_live(Json{{"ops",Json::array({Json{{"op","set"},{"id",environment_id},{"sky_map",std::move(value)}}})}}.dump(),play_project));
    };
    const auto missing_sky=set_play_sky("project/{00000000-0000-0000-0000-000000000000}");
    require(missing_sky.size()==1 && missing_sky[0].severity==world::Severity::warning && missing_sky[0].kind==world::DiagnosticKind::missing_resource &&
        !play_sky() && take(presentation::read_sky_map(rb,b->environment())).source=="project/{00000000-0000-0000-0000-000000000000}",
        "An unresolvable Play sky map was not one warning that keeps its key.");
    const auto malformed_sky=set_play_sky(5);
    require(malformed_sky.size()==1 && malformed_sky[0].severity==world::Severity::error && malformed_sky[0].kind==world::DiagnosticKind::command_rejected,
        "A malformed Play sky map key was not rejected.");
    require(set_play_sky(sky_key).empty() && scene(a)==scene(*b) && play_sky(),"Play did not return to the document's sky map.");
    // An add reads a key Play failed to read before again: the editor may send it once the file is restored.
    const auto late_key=sky_asset(project,"Content/Sky/Late.hdr","broken\n");
    require(set_play_sky(late_key).size()==1 && b->unavailable(editor::AssetKind::sky,late_key),"Play resolved a corrupt sky map.");
    take(a.select(environment_entity));changed(a.erase_selected());sync("Environment removal before a restored sky map");
    take(editor::write_file(project.root()/"Content/Sky/Late.hdr",radiance(1,char(131))));take(project.refresh());
    take(a.set_sky_map(take(project.sky_map(late_key))));sync("Environment creation with a restored sky map");
    require(play_sky() && play_sky()==take(play_project.sky_map(late_key)).map,"Play kept an earlier failure for a restored sky map.");
    changed(a.undo());sync("Restored sky map undo");changed(a.undo());sync("Environment removal undo");
    require(play_sky()==take(play_project.sky_map(sky_key)).map,"Undoing back to the original Environment lost its sky map in Play.");
    each("Name",[&]{take(a.rename(first,L"Runner"));});
    each("Field",[&]{edit(a,move,speed,Scalar{5.f},201);});
    each("Field group",[&]{
        take(a.begin_group_edit(move,std::array{speed,mass},202));take(a.preview_group_edit(202,std::array{Scalar{6.f},Scalar{4.5}}));take(a.commit_edit(202));
    });
    each("Transform field",[&]{const auto& x=a.types().position_fields[0];edit(a,first,world::FieldPath{x.begin(),x.end()},Scalar{7.f},203);});
    each("Transform",[&]{
        take(a.begin_transform(std::array{first,second},204));
        take(a.preview_transform(204,std::array{world::Trs{{1,1,1},{},{2,2,2}},world::Trs{{0,0,5},{},{1,1,1}}}));take(a.commit_edit(204));
    });
    each("Transform removal",[&]{take(a.remove_transform(second));});
    each("Transform addition",[&]{take(a.set_transform(second,{{0,3,0},{},{1,1,1}}));});
    each("Space reference",[&]{changed(a.set_space_reference(second,ObjectId{}));});
    each("Reparent",[&]{changed(a.reparent(second,first));});
    ObjectId plain;each("Entity",[&]{changed(a.add_entity(entity_type));plain=a.selection().entity;});
    each("Duplicate",[&]{take(a.select(first));changed(a.duplicate_selected());});
    each("Deletion",[&]{take(a.select(second));changed(a.erase_selected());});
    each("Component",[&]{take(a.select(plain));changed(a.add_component(spatial_type));});
    const auto body_type=take(ra.find_type("world.CollisionComponent"));ObjectId body;
    each("Component with a Transform",[&]{take(a.select(plain));changed(a.add_component(body_type));body=member(a,body_type);});
    physics::BodySettings settings;settings.shape=physics::ShapeKind::sphere;settings.radius=.75f;
    each("Body",[&]{take(a.configure_body(body,settings));});
    each("Mesh material field",[&]{take(a.set_property(mesh,mesh_roughness,Scalar{.3f},205));});
    each("Material override",[&]{take(a.set_material_override(mesh,"slot/0",roughness,true));});
    const auto instance=take(presentation::read_material_bindings(ra,mesh)).front().resource.id();
    require(instance!=shared,"The override did not create a material instance.");
    each("Material instance",[&]{take(a.set_property(instance,roughness,Scalar{.8f},206));});
    const auto shared_roughness=[&](editor::Session& session) {
        return take(material::read_material(session.world().runtime(),session.loaded_material(material_key))).description.parameters.roughness;
    };
    take(a.set_property(shared,roughness,Scalar{.6f},207));sync("Material asset");
    require(shared_roughness(*b)==.6f,"A material asset edit did not reach Play.");
    take(a.material_undo(material_key));sync("Material asset undo");
    require(shared_roughness(*b)==shared_roughness(a) && shared_roughness(a)!=.6f,"A material asset undo did not reach Play.");

    acceptance_phase="Play preview forwarding";
    const auto pose=[&](const editor::Session& session,ObjectId entity){return take(world::read_transform(session.world().runtime(),entity));};
    // A leaf a preview returns to its start is still sent: Play saw the earlier preview.
    take(a.begin_transform(std::array{first},208));const auto start=pose(a,first);
    auto stretched=start;stretched.scale={5,5,5};take(a.preview_transform(208,std::array{stretched}));sync("Scale preview",false);
    auto moved=start;moved.position.x+=3;take(a.preview_transform(208,std::array{moved}));
    moved.position.x+=1;take(a.preview_transform(208,std::array{moved}));sync("Move preview",false);
    require(pose(*b,in_b(first))==pose(a,first),"A preview that returned a leaf to its start left Play at the earlier preview.");
    moved.position.y+=1;take(a.preview_transform(208,std::array{moved}));moved.position.y+=1;take(a.preview_transform(208,std::array{moved}));
    require(take(a.take_live_ops()).size()==2,"Previews of one leaf did not keep only the latest value.");
    take(a.commit_edit(208));sync("Preview commit");
    take(a.begin_edit(move,speed,209));take(a.preview_edit(209,Scalar{9.f}));sync("Field preview",false);
    require(std::get<float>(take(rb.read(in_b(move),field(*b,take(rb.find_type("game.Move")),"speed"))))==9,"A field preview did not reach Play.");
    take(a.cancel_edit());sync("Field cancel");
    each("Component removal",[&]{take(a.select(first));changed(a.remove_component(move));});

    acceptance_phase="Play forwarding failure";
    auto procedural=take(project.mesh("builtin/cube"));procedural.key.clear();
    changed(a.create_static_mesh(procedural));
    require(!a.take_live_ops() && !a.take_live_ops(),"A change Play cannot follow did not stop forwarding.");
    a.set_live_forwarding(false);require(take(a.take_live_ops()).empty(),"Stopping forwarding kept its failure.");
    std::puts("Play change forwarding: PASS (every change kind, undo and redo, previews, cancel, material assets, failure)");
}
// A document's World lighting is its Environment. Load initialization, as an example
// opens, creates one without history or a modified mark, and only when there is none.
void verify_environment_document(editor::Project& project) {
    acceptance_phase="Environment document";
    using enum world::DiagnosticKind;
    const auto settings=[](const editor::Session& session){return take(session.presentation().environment());};
    const Json empty_scene={{"version",9},{"next_id","1"},{"roots",Json::array()},{"entities",Json::array()},{"materials",Json::array()}};
    const auto load=[&](const Json& data){return take(editor::Session::deserialize(data.dump(),project));};
    auto opened=load(empty_scene);
    require(opened->environment()==ObjectId{} && opened->load_issues().empty() && settings(*opened)==rendering::SceneLightingSettings{},
        "A document without an Environment did not use the default World settings.");
    auto fitted=settings(*opened);fitted.probe_volumes[0].minimum={-3,-1,-4};fitted.probe_volumes[0].maximum={3,4,4};
    fitted.probe_volumes[0].counts={6,5,8};fitted.probe_update_budget=60;
    take(opened->initialize_environment(fitted));
    const auto owner=take(world::inspect_component(opened->world().runtime(),opened->environment())).owner;
    require(settings(*opened)==fitted && !opened->dirty() && opened->undo_count()==0 && opened->redo_count()==0 &&
        take(opened->world().members()).size()==1 && opened->name(owner)==L"Environment" && !take(world::has_transform(opened->world().runtime(),owner)),
        "Load initialization did not leave a clean document with one spaceless Environment root.");
    auto refitted=fitted;refitted.probe_update_budget=61;
    require(!opened->initialize_environment(refitted) && settings(*opened)==fitted,"Load initialization overwrote an existing Environment.");
    const auto saved=Json::parse(take(opened->serialize()));
    auto reopened=load(saved);
    require(saved.at("version")==9 && settings(*reopened)==fitted && reopened->load_issues().empty(),"A saved Environment did not reopen with the same settings.");
    {
        acceptance_phase="Environment sky map";
        const auto environment_record=[](Json& data,const std::string& type="world.EnvironmentComponent")->Json& {
            for(auto& node:data.at("entities")) for(auto& part:node.at("components")) if(part.at("type")==type) return part;
            throw std::runtime_error("The scene has no Environment record.");
        };
        auto current=saved;
        require(environment_record(current).at("sky_map")=="","A saved Environment did not write an empty sky map key.");
        auto derived=current;environment_record(derived)["type"]="game.Environment";
        auto derived_document=take(editor::Session::deserialize(derived.dump(),project,prepare(std::make_shared<Evidence>()),editor::LoadPolicy::strict));
        require(derived_document->environment()!=ObjectId{} && Json::parse(take(derived_document->serialize()))==derived,
            "An Environment of a derived type did not load as the Environment or did not save unchanged.");
        // A key resolves through the Project's cache; a missing one renders without its map and is saved unchanged.
        const auto key=sky_asset(project,"Content/Sky/Scene.hdr",radiance(1,char(129)));const auto map=take(project.sky_map(key)).map;
        auto mapped=current;environment_record(mapped)["sky_map"]=key;
        auto bound=load(mapped);
        require(map && settings(*bound).sky_map==map && bound->load_issues().empty() && Json::parse(take(bound->serialize()))==mapped,
            "A sky map key did not resolve to the Project's map or did not save unchanged.");
        auto missing=current;environment_record(missing)["sky_map"]="project/{00000000-0000-0000-0000-000000000000}";
        require(!editor::Session::deserialize(missing.dump(),project,{},editor::LoadPolicy::strict),"Strict loading accepted a missing sky map.");
        auto unresolved=load(missing);const auto& issues=unresolved->load_issues();
        require(issues.size()==1 && issues[0].kind==missing_resource && issues[0].severity==world::Severity::warning &&
            issues[0].field==std::vector<std::string>{"sky_map"} && !settings(*unresolved).sky_map && Json::parse(take(unresolved->serialize()))==missing,
            "A missing sky map was not one warning, or its key did not save unchanged.");
        auto malformed=current;environment_record(malformed)["sky_map"]=5;
        auto kept=load(malformed);
        require(kept->load_issues().size()==1 && kept->load_issues()[0].kind==mismatch && Json::parse(take(kept->serialize()))==malformed &&
            take(kept->retained(kept->environment())).entries.at(0).kind==mismatch,"A malformed sky map key was not reported and kept as a mismatch.");
        // set_sky_map is one history item that releases the kept value; undo and redo restore the stored map.
        const auto asset=take(project.sky_map(key));
        take(kept->set_sky_map(asset));
        require(settings(*kept).sky_map==map && kept->undo_count()==1 && Json::parse(take(kept->serialize()))==mapped,
            "Setting a sky map did not bind it as one history item or kept the malformed value.");
        changed(kept->undo());require(!settings(*kept).sky_map && Json::parse(take(kept->serialize()))==malformed,"Undoing a sky map did not restore the kept value.");
        changed(kept->redo());require(settings(*kept).sky_map==map,"Redoing a sky map did not restore the same map.");
        take(kept->set_sky_map({}));auto cleared=Json::parse(take(kept->serialize()));
        require(!settings(*kept).sky_map && environment_record(cleared).at("sky_map")=="" && kept->undo_count()==2,"An empty sky asset did not clear the sky map.");
        changed(kept->undo());require(settings(*kept).sky_map==map,"Undoing a cleared sky map did not restore the same map.");
        take(kept->set_sky_map(asset));require(kept->undo_count()==1 && kept->redo_count()==1,"An unchanged sky map created history.");
        // Without an Environment the first sky map creates one in the same history item.
        auto empty=load(empty_scene);
        take(empty->set_sky_map({}));require(empty->environment()==ObjectId{} && empty->undo_count()==0,"Clearing an absent sky map created an Environment.");
        take(empty->set_sky_map(asset));
        require(empty->environment()!=ObjectId{} && settings(*empty).sky_map==map && empty->undo_count()==1,"The first sky map did not create its Environment.");
        changed(empty->undo());require(empty->environment()==ObjectId{} && take(empty->world().members()).empty(),"Undoing the first sky map left an Environment.");
    }
    std::puts("Environment document: PASS (load initialization only without an Environment, derived Environment type, sky map keys and history)");
}
void run() {
    verify_stop_reentry();
    acceptance_phase="Source document fields and history";
    const auto directory=std::filesystem::current_path()/"project-scene-evidence"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project=take(editor::Project::create(directory));
    verify_codec_graph(project);
    acceptance_phase="Source document fields and history";
    auto evidence=std::make_shared<Evidence>();
    editor::Session document(prepare(evidence)); auto& runtime=document.world().runtime();
    const auto entity_type=take(runtime.find_type("game.Entity")), move_type=take(runtime.find_type("game.Move")), spatial_type=take(runtime.find_type("game.Spatial"));
    require(take(document.component_types()).size()>=5,"Derived component discovery failed.");
    changed(document.add_entity(entity_type)); const auto entity=document.selection().entity;
    take(document.set_transform(entity,{}));changed(document.add_component(spatial_type));
    changed(document.add_component(move_type)); const auto move=member(document,move_type);
    require(take(world::has_transform(runtime,entity)) && take(world::parent_entity(runtime,entity))==ObjectId{},"Non-spatial business component altered the root.");
    const auto speed=field(document,move_type,"speed"), health=field(document,entity_type,"health");
    require(std::get<float>(take(document.read_property(move,speed)))==2 && take(document.properties(move)).size()==4,"Default values or scalar discovery failed.");
    edit(document,move,speed,Scalar{4.f},11); edit(document,entity,health,Scalar{std::int32_t{75}},12);
    changed(document.undo()); require(std::get<std::int32_t>(take(document.read_property(entity,health)))==100,"Entity scalar undo failed.");
    changed(document.redo());
    take(document.begin_edit(move,speed,13)); require(!document.preview_edit(13,Scalar{true}),"Scalar edit accepted a different type.");
    take(document.preview_edit(13,6.f)); take(document.cancel_edit()); take(document.cancel_edit());
    require(std::get<float>(take(document.read_property(move,speed)))==4,"Cancel failed to restore scalar value.");
    const auto saved=take(document.serialize()); const auto saved_json=Json::parse(saved);
    require(saved_json["version"]==9 && saved_json["entities"][0]["properties"]["health"]==75,"Scene did not save real Entity properties.");
    auto reopened=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    require(Json::parse(take(reopened->serialize()))==saved_json,"Derived scene roundtrip changed type, fields or composition.");
    {
        // A float field is written as its shortest decimal and reads back bit for bit; a double keeps its full value.
        const auto mass=field(document,move_type,"mass");
        const std::array values{std::bit_cast<float>(1u),std::numeric_limits<float>::max(),.1f,-0.f,16777215.f,16777216.f,16777218.f,std::bit_cast<float>(0x15ae43fdu)};
        for(const float value:values) {
            take(runtime.write(move,speed,Scalar{value}));take(runtime.write(move,mass,Scalar{double(.1f)}));
            const auto text=take(document.serialize());const auto written=Json::parse(text).at("entities").at(0).at("components").at(1).at("properties");
            auto loaded=take(editor::Session::deserialize(text,project,prepare(evidence)));
            take(loaded->select(take(loaded->world().members()).front().id()));
            const auto loaded_type=take(loaded->world().runtime().find_type("game.Move"));const auto loaded_move=member(*loaded,loaded_type);
            const auto back=std::get<float>(take(loaded->read_property(loaded_move,field(*loaded,loaded_type,"speed"))));
            require(std::bit_cast<std::uint32_t>(back)==std::bit_cast<std::uint32_t>(value) &&
                std::get<double>(take(loaded->read_property(loaded_move,field(*loaded,loaded_type,"mass"))))==double(.1f) &&
                (value!=.1f || (written["speed"].dump()=="0.1" && written["mass"].dump()=="0.10000000149011612")),
                "A float field was not saved as its shortest decimal, or a float or double field changed across save and reload.");
        }
        take(runtime.write(move,speed,Scalar{4.f}));take(runtime.write(move,mass,Scalar{3.5}));
    }
    auto defaults=saved_json; defaults["entities"][0]["components"][1]["properties"].erase("speed");
    auto defaulted=take(editor::Session::deserialize(defaults.dump(),project,prepare(evidence)));
    const auto default_entity=take(defaulted->world().members()).front().id(); take(defaulted->select(default_entity));
    const auto default_type=take(defaulted->world().runtime().find_type("game.Move"));
    require(std::get<float>(take(defaulted->read_property(member(*defaulted,default_type),field(*defaulted,default_type,"speed"))))==2,"Absent field did not preserve the constructor default.");
    auto bad=saved_json; bad["entities"][0]["components"][1]["properties"]["deleted_field"]=1;
    const auto releases=evidence->destroyed;
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence),editor::LoadPolicy::strict),"Strict load accepted an unknown saved property.");
    require(evidence->destroyed==releases+1 && evidence->live_runtime_on_release,"Failed load released Runtime before its prepared code.");
    bad=saved_json; bad["entities"][0]["type"]="game.Missing";
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence),editor::LoadPolicy::strict),"Strict load accepted a missing project type.");
    bad=saved_json; bad["entities"][0]["components"][1]["properties"]["count"]=-1;
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence),editor::LoadPolicy::strict),"Unsigned field silently wrapped a negative value.");
    require(take(document.serialize())==saved,"Failed candidate load changed the edit document.");
    verify_retained_data(project,saved_json,evidence);
    verify_play_changes(project,evidence);
    verify_string_properties(project,evidence);
    verify_array_properties(project,evidence);
    verify_live_forwarding(project,evidence);
    verify_environment_document(project);
    acceptance_phase="Source play and stop lifecycle";
    const auto history=document.undo_count();
    for (int pass=0;pass<3;++pass) {
        auto running=take(editor::Session::deserialize(saved,project,prepare(evidence)));
        const auto run_entity=take(running->world().members()).front().id();
        const auto run_root=run_entity;
        require(run_entity!=entity,"Play reused editing Runtime identity.");
        changed(running->start());changed(running->start());take(running->update(.5f));
        require(take(world::read_transform(running->world().runtime(),run_root)).position.x==2,"Source native action did not run after JIT returned.");
        changed(running->stop());changed(running->stop());changed(running->close());changed(running->close());
    }
    require(take(document.serialize())==saved && document.undo_count()==history,"Play/Stop changed editing state/history.");
    auto failure=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    const auto failure_root=take(failure->world().members()).front().id();
    const auto ends=evidence->ends;
    changed(failure->start());evidence->fail_tick=true;require(!failure->update(.5f),"Tick failure was lost.");evidence->fail_tick=false;
    require(!failure->world().running() && evidence->ends==ends+1 && take(world::read_transform(failure->world().runtime(),failure_root)).position.x==0,"Failed callback applied queued movement or skipped End.");
    changed(failure->stop());changed(failure->stop());require(evidence->ends==ends+1,"Repeated stop duplicated End.");
    changed(failure->start());take(failure->update(.5f));require(take(world::read_transform(failure->world().runtime(),failure_root)).position.x==2,"Restart replayed a failed translation batch.");
    evidence->fail_end=true;auto stopped=take(failure->stop());evidence->fail_end=false;
    require(stopped.notification_error.has_value() && !failure->world().running(),"End error lost the committed stop state.");changed(failure->stop());
    evidence->fail_begin=true;auto started=take(failure->start());evidence->fail_begin=false;
    require(started.notification_error.has_value() && !failure->world().running(),"Begin error left World running.");changed(failure->close());
    acceptance_phase="Source and physics pose ownership";
    auto physical=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    const auto physical_entity=take(physical->world().members()).front().id();take(physical->select(physical_entity));
    auto& physical_runtime=physical->world().runtime();
    const auto body_type=take(physical_runtime.find_type("world.CollisionComponent"));changed(physical->add_component(body_type));
    const auto body=member(*physical,body_type);
    physics::BodySettings settings;
    settings.shape=physics::ShapeKind::sphere;settings.motion=physics::MotionKind::dynamic;
    settings.half_extents={1,2,3};settings.radius=.75f;
    take(physical->configure_body(body,settings));
    auto physical_reopened=take(editor::Session::deserialize(take(physical->serialize()),project,prepare(evidence)));
    take(physical_reopened->select(take(physical_reopened->world().members()).front().id()));
    const auto reopened_body_type=take(physical_reopened->world().runtime().find_type("world.CollisionComponent"));
    require(take(physics::read_settings(physical_reopened->world().runtime(),member(*physical_reopened,reopened_body_type)))==
        take(physics::read_settings(physical_runtime,body)),"Scene lost reflected Primitive body settings.");
    changed(physical_reopened->close());
    changed(physical->start()); require(!physical->update(.01f) && !physical->world().running(),"translate_owner overrode physics ownership.");
    changed(physical->close());
    for(const auto* controller:{"physics.CharacterComponent","physics.RagdollComponent"}) {
        auto controlled=take(editor::Session::deserialize(saved,project,prepare(evidence)));
        take(controlled->select(take(controlled->world().members()).front().id()));
        changed(controlled->add_component(take(controlled->world().runtime().find_type(controller))));
        changed(controlled->start());
        const auto moved=controlled->update(.01f);
        require(!moved && moved.error().message.find("translate_owner")!=std::string::npos && !controlled->world().running(),
            "translate_owner overrode a Character or Ragdoll component.");
        changed(controlled->close());
    }
    changed(document.erase_selected());require(!document.properties(move),"Detached history object remained editable.");
    changed(document.undo());require(document.selection().entity==entity && std::get<float>(take(document.read_property(move,speed)))==4,"Deletion undo lost business identity/fields.");
    changed(document.close());changed(reopened->close());changed(defaulted->close());
    verify_query_source(project);
    verify_primitive_source(project);
    require(evidence->prepared==evidence->destroyed && evidence->live_runtime_on_release,"Prepared code leaked or outlived its Runtime.");
    std::puts("Project Scene acceptance: PASS (real derived types, typed history, defaults, retained data, load cleanup, Play change batches, String properties and strict decoding, Source actions, repeated play/stop, Jolt ownership)");
}
}
int main() { try { run(); return 0; } catch (const std::exception& error) { std::fprintf(stderr,"Project Scene acceptance: FAIL [%s]: %s\n",acceptance_phase,error.what()); return 1; } }

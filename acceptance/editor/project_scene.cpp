#include "editor/session.hpp"
#include "editor/project.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include "physics/physics.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
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
        take(NativeRegistration<Entity>{runtime,"game.Entity",TypeKind::object}.base<world::Entity>().field<&Entity::health>("health").publish());
        const auto move=take(NativeRegistration<Move>{runtime,"game.Move",TypeKind::object}.base<world::Component>()
            .field<&Move::speed>("speed").field<&Move::mass>("mass").field<&Move::count>("count").field<&Move::allowed>("allowed").publish());
        take(NativeRegistration<Spatial>{runtime,"game.Spatial",TypeKind::object}.base<world::Component>().field<&Spatial::offset>("offset").publish());
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
ObjectId member(editor::Session& session,TypeId type) {
    for (const auto& component:take(world::components(session.world().runtime(),session.selection().entity)))
        if (take(session.world().runtime().type_of(component.id()))==type) return component.id();
    throw std::runtime_error("Missing component");
}
world::FieldPath field(editor::Session& session,TypeId type,std::string_view name) { return {take(session.world().runtime().find_field(type,name))}; }
void verify_primitive_source(editor::Project& project) {
    acceptance_phase="Primitive Source reflection and persistence";
    editor::Session document;
    auto& runtime=document.world().runtime();
    source::Program program(runtime,document.types().static_mesh_component);
    auto candidate=take(source::Program::prepare(take(program.request("Primitive.source",
        "void configure() { self.body.enabled = true; self.body.half_extents.x = 2.0f; self.body.material.friction = 0.6f; }"))));
    take(program.try_publish(candidate));
    changed(document.create_static_mesh(take(project.mesh("builtin/cube"))));
    const auto component=member(document,document.types().static_mesh_component);
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
            entity["space"]["origin"]="0";entity["space"]["scale"]=Json::array({1e30f,1e30f,1e30f});
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
            meshes[i]=member(document,document.types().static_mesh_component);
        }
        material::State state;state.description.parameters.roughness=.3f;
        const auto shared=take(material::create_material(runtime,state)),independent=take(material::create_material(runtime,state));
        for(std::size_t i=0;i<meshes.size();++i) {
            const std::array<world::MaterialBinding,1> bindings{{{"slot/0",i==2?independent:shared,true}}};
            take(world::set_material_bindings(runtime,meshes[i],bindings));
        }
        const auto saved=take(document.serialize());const auto data=Json::parse(saved);
        require(data["materials"].size()==2,"Scene material definitions did not preserve shared and equal-but-independent identities.");
        auto reopened=take(editor::Session::deserialize(saved,project));
        require(take(reopened->serialize())==saved,"Material document identities changed after save/reload.");
        const auto bindings_of=[&](editor::Session& session,ObjectId entity) {
            take(session.select(entity));return take(world::read_material_bindings(session.world().runtime(),member(session,session.types().static_mesh_component))).front().resource;
        };
        const auto members=take(reopened->world().members());
        const auto a=bindings_of(*reopened,members[0].id()),b=bindings_of(*reopened,members[1].id()),c=bindings_of(*reopened,members[2].id());
        require(a.id()==b.id() && a.id()!=c.id(),"Scene reload split a shared inline material or merged equal independent materials.");
        auto updated=take(material::read_material(reopened->world().runtime(),a.id()));
        updated.description.parameters.roughness=.6f;take(material::update_material(reopened->world().runtime(),a.id(),updated));
        require(take(material::capture(reopened->world().runtime(),b.id()))->description().parameters.roughness==.6f &&
            take(material::capture(reopened->world().runtime(),c.id()))->description().parameters.roughness==.3f,
            "Restored shared material edits did not propagate according to object identity.");
        // Previously generated v6 used per-binding resource payloads. It remains
        // readable, but has no identity information with which to infer aliasing.
        auto old=data;
        for(auto& entity:old["entities"]) for(auto& component:entity["components"]) for(auto& binding:component["materials"]) {
            const auto id=binding.at("resource");
            const auto at=std::ranges::find_if(data["materials"],[&](const auto& definition){return definition.at("id")==id;});
            require(at!=data["materials"].end(),"Missing material definition in compatibility fixture.");
            for(const auto& [name,value]:at->items()) if(name!="id") binding[name]=value;
            binding.erase("resource");
        }
        old.erase("materials");require(editor::Session::deserialize(old.dump(),project).has_value(),"Previously generated inline-resource v6 input became unreadable.");
        const auto key=take(project.create_material("Content/Materials/Codec-parent.material"));
        const auto parent=take(document.material_resource(project,key));const auto instance=take(material::create_instance(runtime,parent));
        for(std::size_t i=0;i<2;++i) {
            const std::array<world::MaterialBinding,1> bindings{{{"slot/0",instance,true}}};take(world::set_material_bindings(runtime,meshes[i],bindings));
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
        take(source.assign_material(project,member(source,source.types().static_mesh_component),"slot/0",incoming));
        changed(source.add_entity());auto invalid=Json::parse(take(source.serialize()));invalid["entities"][1]["properties"]["unknown_business_field"]=true;
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
    require(saved_json["version"]==6 && saved_json["entities"][0]["properties"]["health"]==75,"Scene did not save real Entity properties.");
    auto reopened=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    require(Json::parse(take(reopened->serialize()))==saved_json,"Derived scene roundtrip changed type, fields or composition.");
    auto defaults=saved_json; defaults["entities"][0]["components"][1]["properties"].erase("speed");
    auto defaulted=take(editor::Session::deserialize(defaults.dump(),project,prepare(evidence)));
    const auto default_entity=take(defaulted->world().members()).front().id(); take(defaulted->select(default_entity));
    const auto default_type=take(defaulted->world().runtime().find_type("game.Move"));
    require(std::get<float>(take(defaulted->read_property(member(*defaulted,default_type),field(*defaulted,default_type,"speed"))))==2,"Absent field did not preserve the constructor default.");
    auto bad=saved_json; bad["entities"][0]["components"][1]["properties"]["deleted_field"]=1;
    const auto releases=evidence->destroyed;
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence)),"Unknown saved property was silently discarded.");
    require(evidence->destroyed==releases+1 && evidence->live_runtime_on_release,"Failed load released Runtime before its prepared code.");
    bad=saved_json; bad["entities"][0]["type"]="game.Missing";
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence)),"Missing project type was accepted.");
    bad=saved_json; bad["entities"][0]["components"][1]["properties"]["count"]=-1;
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence)),"Unsigned field silently wrapped a negative value.");
    require(take(document.serialize())==saved,"Failed candidate load changed the edit document.");
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
    const std::string legacy=R"({"version":1,"actors":[{"type":"world.StaticMeshActor","name":"Legacy","enabled":true,"root":1,"components":[
        {"id":1,"type":"world.StaticMeshComponent","enabled":true,"parent":0,"position":[1,2,3],"rotation":[0,0,0,1],"scale":[2,2,2],"mesh":"builtin/cube","base_color":[0.2,0.3,0.4]}
    ]}]})";
    auto migrated=take(editor::Session::deserialize(legacy,project));
    auto upgraded=Json::parse(legacy);upgraded["version"]=3;
    upgraded["actors"][0]["components"][0]["materials"]=Json::array();
    upgraded["actors"][0]["properties"]=Json::object();upgraded["actors"][0]["components"][0]["properties"]=Json::object();
    // JSON source decimals round through the authored float storage.
    const auto migrated_json=Json::parse(take(migrated->serialize()));
    upgraded["actors"][0]["components"][0]["base_color"]=migrated_json["entities"][0]["components"][0]["base_color"];
    upgraded["actors"][0]["components"][0]["roughness"]=world::Material{}.roughness;
    upgraded["actors"][0]["components"][0]["metallic"]=0.f;
    upgraded["actors"][0]["components"][0]["emissive"]=Json::array({0.f,0.f,0.f});
    require(migrated_json["version"]==6 && migrated_json["entities"][0]["components"][0]["properties"]["body"]["enabled"]==false,
        "Legacy visual mesh unexpectedly enabled physics or failed to migrate to the Entity graph.");
    auto upgraded_document=take(editor::Session::deserialize(upgraded.dump(),project));
    require(take(upgraded_document->serialize())==take(migrated->serialize()),"Legacy v1/v3 migration disagreed on complete authored values.");
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
    std::puts("Project Scene acceptance: PASS (real derived types, typed history, v1/v2, defaults, load cleanup, Source actions, repeated play/stop, Jolt ownership)");
}
}
int main() { try { run(); return 0; } catch (const std::exception& error) { std::fprintf(stderr,"Project Scene acceptance: FAIL [%s]: %s\n",acceptance_phase,error.what()); return 1; } }

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
template<class T> T take(Result<T> value) { if (!value) throw std::runtime_error(value.error().message); return std::move(*value); }
void take(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
void changed(Result<world::Commit> value) { auto commit=take(std::move(value)); if (commit.notification_error) throw std::runtime_error(commit.notification_error->message); }
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Actor : world::Actor { std::int32_t health{100}; };
struct Move : world::Component { float speed{2}; double mass{3.5}; std::uint32_t count{7}; bool allowed{true}; };
struct Spatial : world::SceneComponent { float offset{4}; };
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
        take(NativeRegistration<Actor>{runtime,"game.Actor",TypeKind::object}.base<world::Actor>().field<&Actor::health>("health").publish());
        const auto move=take(NativeRegistration<Move>{runtime,"game.Move",TypeKind::object}.base<world::Component>()
            .field<&Move::speed>("speed").field<&Move::mass>("mass").field<&Move::count>("count").field<&Move::allowed>("allowed").publish());
        take(NativeRegistration<Spatial>{runtime,"game.Spatial",TypeKind::object}.base<world::SceneComponent>().field<&Spatial::offset>("offset").publish());
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
    for (const auto& component:take(world::components(session.world().runtime(),session.selection().actor)))
        if (take(session.world().runtime().type_of(component.id()))==type) return component.id();
    throw std::runtime_error("Missing component");
}
world::FieldPath field(editor::Session& session,TypeId type,std::string_view name) { return {take(session.world().runtime().find_field(type,name))}; }
void verify_query_source(editor::Project& project) {
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
    for(auto& actor:scene["actors"]) if(actor["name"]=="Scene query emitter") {
        auto& component=actor["components"][3];component["type"]="game.QueryProbe";component["properties"]["automatic"]=false;
    }
    auto document=take(editor::Session::deserialize(scene.dump(),project,query_code));
    ObjectId probe{};
    for(const auto& actor:take(document->world().members())) for(const auto& component:take(world::components(document->world().runtime(),actor.id())))
        if(take(document->world().runtime().type_of(component.id()))==take(document->world().runtime().native_type<QueryProbe>())) probe=component.id();
    require(probe!=ObjectId{},"Source query probe was not loaded.");
    changed(document->start());take(document->update(1.f/60));take(document->update(1.f/60));
    take(document->world().runtime().with_native<const QueryProbe>(probe,[](const auto& value)->Result<void>{
        require(value.observed && value.count==1 && std::abs(value.distance-6.5f)<.01f && std::abs(value.point.z-2.5f)<.01f,
            "Source could not request a trace and read the completed bool/count/distance/vector result.");return {};
    }));
    const auto saved=take(document->serialize());changed(document->stop());changed(document->close());document.reset();
    auto reopened=take(editor::Session::deserialize(saved,project,query_code));
    for(const auto& actor:take(reopened->world().members())) for(const auto& component:take(world::components(reopened->world().runtime(),actor.id()))) {
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
    editor::Session* current{};
    ObjectId query;
    bool fail_tick{};
    unsigned ends{}, live_ends{};
    editor::Session document([&](world::World& world)->Result<std::shared_ptr<void>> {
        take(world.bind_behavior(world.types().actor, {
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
    changed(document.add_actor());
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
void run() {
    verify_stop_reentry();
    const auto directory=std::filesystem::current_path()/"project-scene-evidence"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto project=take(editor::Project::create(directory));
    auto evidence=std::make_shared<Evidence>();
    editor::Session document(prepare(evidence)); auto& runtime=document.world().runtime();
    const auto actor_type=take(runtime.find_type("game.Actor")), move_type=take(runtime.find_type("game.Move")), spatial_type=take(runtime.find_type("game.Spatial"));
    require(take(document.component_types()).size()>=5,"Derived component discovery failed.");
    changed(document.add_actor(actor_type)); const auto actor=document.selection().actor;
    changed(document.add_component(spatial_type)); const auto root=member(document,spatial_type);
    changed(document.add_component(move_type)); const auto move=member(document,move_type);
    require(take(world::root_component(runtime,actor))==root && take(world::scene_parent(runtime,root))==ObjectId{},"Non-spatial business component altered the root.");
    const auto speed=field(document,move_type,"speed"), health=field(document,actor_type,"health");
    require(std::get<float>(take(document.read_property(move,speed)))==2 && take(document.properties(move)).size()==4,"Default values or scalar discovery failed.");
    edit(document,move,speed,Scalar{4.f},11); edit(document,actor,health,Scalar{std::int32_t{75}},12);
    changed(document.undo()); require(std::get<std::int32_t>(take(document.read_property(actor,health)))==100,"Actor scalar undo failed.");
    changed(document.redo());
    take(document.begin_edit(move,speed,13)); require(!document.preview_edit(13,Scalar{true}),"Scalar edit accepted a different type.");
    take(document.preview_edit(13,6.f)); take(document.cancel_edit()); take(document.cancel_edit());
    require(std::get<float>(take(document.read_property(move,speed)))==4,"Cancel failed to restore scalar value.");
    const auto saved=take(document.serialize()); const auto saved_json=Json::parse(saved);
    require(saved_json["version"]==3 && saved_json["actors"][0]["properties"]["health"]==75,"Scene did not save real Actor properties.");
    auto reopened=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    require(Json::parse(take(reopened->serialize()))==saved_json,"Derived scene roundtrip changed type, fields or composition.");
    auto defaults=saved_json; defaults["actors"][0]["components"][1]["properties"].erase("speed");
    auto defaulted=take(editor::Session::deserialize(defaults.dump(),project,prepare(evidence)));
    const auto default_actor=take(defaulted->world().members()).front().id(); take(defaulted->select(default_actor));
    const auto default_type=take(defaulted->world().runtime().find_type("game.Move"));
    require(std::get<float>(take(defaulted->read_property(member(*defaulted,default_type),field(*defaulted,default_type,"speed"))))==2,"Absent field did not preserve the constructor default.");
    auto bad=saved_json; bad["actors"][0]["components"][1]["properties"]["deleted_field"]=1;
    const auto releases=evidence->destroyed;
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence)),"Unknown saved property was silently discarded.");
    require(evidence->destroyed==releases+1 && evidence->live_runtime_on_release,"Failed load released Runtime before its prepared code.");
    bad=saved_json; bad["actors"][0]["type"]="game.Missing";
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence)),"Missing project type was accepted.");
    bad=saved_json; bad["actors"][0]["components"][1]["properties"]["count"]=-1;
    require(!editor::Session::deserialize(bad.dump(),project,prepare(evidence)),"Unsigned field silently wrapped a negative value.");
    require(take(document.serialize())==saved,"Failed candidate load changed the edit document.");
    const auto history=document.undo_count();
    for (int pass=0;pass<3;++pass) {
        auto running=take(editor::Session::deserialize(saved,project,prepare(evidence)));
        const auto run_actor=take(running->world().members()).front().id();
        const auto run_root=*take(world::root_component(running->world().runtime(),run_actor));
        require(run_actor!=actor,"Play reused editing Runtime identity.");
        changed(running->start());changed(running->start());take(running->update(.5f));
        require(take(world::read_transform(running->world().runtime(),run_root)).position.x==2,"Source native action did not run after JIT returned.");
        changed(running->stop());changed(running->stop());changed(running->close());changed(running->close());
    }
    require(take(document.serialize())==saved && document.undo_count()==history,"Play/Stop changed editing state/history.");
    auto failure=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    const auto failure_root=*take(world::root_component(failure->world().runtime(),take(failure->world().members()).front().id()));
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
    upgraded["actors"][0]["components"][0]["base_color"]=migrated_json["actors"][0]["components"][0]["base_color"];
    upgraded["actors"][0]["components"][0]["roughness"]=world::Material{}.roughness;
    upgraded["actors"][0]["components"][0]["metallic"]=0.f;
    upgraded["actors"][0]["components"][0]["emissive"]=Json::array({0.f,0.f,0.f});
    require(migrated_json==upgraded,"Legacy scene did not preserve spatial/resource data while upgrading to v2.");
    auto physical=take(editor::Session::deserialize(saved,project,prepare(evidence)));
    const auto physical_actor=take(physical->world().members()).front().id();take(physical->select(physical_actor));
    auto& physical_runtime=physical->world().runtime();
    const auto body_type=take(physical_runtime.find_type("physics.BodyComponent"));changed(physical->add_component(body_type));
    const auto body=member(*physical,body_type);
    physics::BodySettings settings;
    settings.shape=physics::ShapeKind::sphere;settings.motion=physics::MotionKind::dynamic;
    settings.half_extents={1,2,3};settings.radius=.75f;
    take(physical->configure_body(body,settings));
    auto physical_reopened=take(editor::Session::deserialize(take(physical->serialize()),project,prepare(evidence)));
    take(physical_reopened->select(take(physical_reopened->world().members()).front().id()));
    const auto reopened_body_type=take(physical_reopened->world().runtime().find_type("physics.BodyComponent"));
    require(take(physics::read_settings(physical_reopened->world().runtime(),member(*physical_reopened,reopened_body_type)))==
        take(physics::read_settings(physical_runtime,body)),"Scene lost non-reflected physical body settings.");
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
    changed(document.undo());require(document.selection().actor==actor && std::get<float>(take(document.read_property(move,speed)))==4,"Deletion undo lost business identity/fields.");
    changed(document.close());changed(reopened->close());changed(defaulted->close());
    verify_query_source(project);
    require(evidence->prepared==evidence->destroyed && evidence->live_runtime_on_release,"Prepared code leaked or outlived its Runtime.");
    std::puts("Project Scene acceptance: PASS (real derived types, typed history, v1/v2, defaults, load cleanup, Source actions, repeated play/stop, Jolt ownership)");
}
}
int main() { try { run(); return 0; } catch (const std::exception& error) { std::fprintf(stderr,"Project Scene acceptance: FAIL: %s\n",error.what()); return 1; } }

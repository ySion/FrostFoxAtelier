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
struct Evidence { unsigned prepared{},destroyed{},begins{},ends{}; bool fail_tick{},fail_begin{},fail_end{},live_runtime_on_release{true}; };
struct Code {
    Runtime& runtime;
    source::Program program;
    std::shared_ptr<Evidence> evidence;
    Code(Runtime& r,TypeId type,std::shared_ptr<Evidence> e):runtime(r),program(r,type),evidence(std::move(e)) {}
    ~Code() { evidence->live_runtime_on_release &= !runtime.types().empty(); ++evidence->destroyed; }
};
editor::Session::Prepare prepare(std::shared_ptr<Evidence> evidence,bool physical=false) {
    return [evidence,physical](world::World& world)->Result<std::shared_ptr<void>> {
        auto& runtime=world.runtime(); ++evidence->prepared;
        take(NativeRegistration<Actor>{runtime,"game.Actor",TypeKind::object}.base<world::Actor>().field<&Actor::health>("health").publish());
        const auto move=take(NativeRegistration<Move>{runtime,"game.Move",TypeKind::object}.base<world::Component>()
            .field<&Move::speed>("speed").field<&Move::mass>("mass").field<&Move::count>("count").field<&Move::allowed>("allowed").publish());
        take(NativeRegistration<Spatial>{runtime,"game.Spatial",TypeKind::object}.base<world::SceneComponent>().field<&Spatial::offset>("offset").publish());
        if (physical) take(physics::register_types(runtime));
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
void edit(editor::Session& session,ObjectId object,const world::FieldPath& path,Scalar value,std::uint64_t key) {
    take(session.begin_edit(object,path,key)); take(session.preview_edit(key,value)); take(session.commit_edit(key));
}
void run() {
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
    require(saved_json["version"]==2 && saved_json["actors"][0]["properties"]["health"]==75,"Scene v2 did not save real Actor properties.");
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
    auto upgraded=Json::parse(legacy);upgraded["version"]=2;
    upgraded["actors"][0]["properties"]=Json::object();upgraded["actors"][0]["components"][0]["properties"]=Json::object();
    // JSON source decimals round through the authored float storage.
    const auto migrated_json=Json::parse(take(migrated->serialize()));
    upgraded["actors"][0]["components"][0]["base_color"]=migrated_json["actors"][0]["components"][0]["base_color"];
    require(migrated_json==upgraded,"Legacy scene did not preserve spatial/resource data while upgrading to v2.");
    auto physical=take(editor::Session::deserialize(saved,project,prepare(evidence,true)));
    const auto physical_actor=take(physical->world().members()).front().id();take(physical->select(physical_actor));
    auto& physical_runtime=physical->world().runtime();
    const auto body_type=take(physical_runtime.find_type("physics.BodyComponent"));changed(physical->add_component(body_type));
    const auto body=member(*physical,body_type);
    take(physics::configure(physical_runtime,body,{physics::ShapeKind::sphere,physics::MotionKind::dynamic,{1,2,3},.75f}));
    auto physical_reopened=take(editor::Session::deserialize(take(physical->serialize()),project,prepare(evidence,true)));
    take(physical_reopened->select(take(physical_reopened->world().members()).front().id()));
    const auto reopened_body_type=take(physical_reopened->world().runtime().find_type("physics.BodyComponent"));
    require(take(physics::read_settings(physical_reopened->world().runtime(),member(*physical_reopened,reopened_body_type)))==
        take(physics::read_settings(physical_runtime,body)),"Scene lost non-reflected physical body settings.");
    changed(physical_reopened->close());
    changed(physical->start()); require(!physical->update(.01f) && !physical->world().running(),"translate_owner overrode physics ownership.");
    changed(physical->close());
    changed(document.erase_selected());require(!document.properties(move),"Detached history object remained editable.");
    changed(document.undo());require(document.selection().actor==actor && std::get<float>(take(document.read_property(move,speed)))==4,"Deletion undo lost business identity/fields.");
    changed(document.close());changed(reopened->close());changed(defaulted->close());
    require(evidence->prepared==evidence->destroyed && evidence->live_runtime_on_release,"Prepared code leaked or outlived its Runtime.");
    std::puts("Project Scene acceptance: PASS (real derived types, typed history, v1/v2, defaults, load cleanup, Source actions, repeated play/stop, Jolt ownership)");
}
}
int main() { try { run(); return 0; } catch (const std::exception& error) { std::fprintf(stderr,"Project Scene acceptance: FAIL: %s\n",error.what()); return 1; } }

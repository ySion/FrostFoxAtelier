#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/json_format.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace atelier::editor {
namespace {
using json_format::Json;
using json_format::fields;
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(*value);
}
void committed(Result<world::Commit> value) {
    auto result=take(std::move(value)); if (result.notification_error) throw std::runtime_error(result.notification_error->message);
}
Json vector(math::Vector3 value) { return Json::array({value.x,value.y,value.z}); }
math::Vector3 vector(const Json& value) {
    const auto v=json_format::vector<3>(value); return {v[0],v[1],v[2]};
}
Json body_settings(const physics::BodySettings& value) {
    Json responses=Json::array();
    for (auto response:value.responses) responses.push_back(static_cast<unsigned>(response));
    return {{"shape",static_cast<unsigned>(value.shape)},
        {"motion",static_cast<unsigned>(value.motion)},
        {"half_extents",vector(value.half_extents)},
        {"radius",value.radius},{"capsule_half_height",value.capsule_half_height},
        {"category",value.category},{"responses",std::move(responses)},
        {"sensor",value.sensor},{"friction",value.material.friction},
        {"restitution",value.material.restitution}};
}
physics::BodySettings body_settings(const Json& value) {
    fields(value,{"shape","motion","half_extents","radius","capsule_half_height",
        "category","responses","sensor","friction","restitution"});
    physics::BodySettings settings;
    const auto shape=json_format::identity(value.at("shape"));
    const auto motion=json_format::identity(value.at("motion"));
    if (shape>static_cast<unsigned>(physics::ShapeKind::capsule) ||
        motion>static_cast<unsigned>(physics::MotionKind::dynamic))
        throw std::runtime_error("Invalid physics shape or motion.");
    settings.shape=static_cast<physics::ShapeKind>(shape);
    settings.motion=static_cast<physics::MotionKind>(motion);
    settings.half_extents=vector(value.at("half_extents"));
    const auto number=[&](const char* key) {
        const auto& item=value.at(key);
        if (!item.is_number()) throw std::runtime_error(std::string("Expected numeric physics field: ")+key);
        return item.get<float>();
    };
    settings.radius=number("radius");
    settings.capsule_half_height=number("capsule_half_height");
    const auto category=json_format::identity(value.at("category"));
    if (category>=settings.responses.size()) throw std::runtime_error("Invalid physics category.");
    settings.category=static_cast<std::uint8_t>(category);
    const auto& responses=value.at("responses");
    if (!responses.is_array() || responses.size()!=settings.responses.size())
        throw std::runtime_error("Physics responses require 16 entries.");
    for (std::size_t i=0;i<settings.responses.size();++i) {
        const auto response=json_format::identity(responses[i]);
        if (response>static_cast<unsigned>(physics::Response::block))
            throw std::runtime_error("Invalid physics collision response.");
        settings.responses[i]=static_cast<physics::Response>(response);
    }
    settings.sensor=value.at("sensor").get<bool>();
    settings.material={number("friction"),number("restitution")};
    return settings;
}
std::string type_name(Runtime& runtime,TypeId type) { return std::string(take(runtime.describe(type))->name); }
auto failed(const std::exception& error) { return std::unexpected(Error{ErrorCode::invalid_storage,error.what()}); }
void validate_color(math::Vector3 color) {
    if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) || color.x<0 || color.y<0 || color.z<0 || color.x>1 || color.y>1 || color.z>1)
        throw std::runtime_error("Invalid scene material color.");
}
}
Result<std::string> Session::serialize() const {
    try {
        if (active_) throw std::runtime_error("Finish the active field edit before saving.");
        auto& runtime=world_.runtime();
        Json actors=Json::array();
        const auto members=take(world_.members());
        if (members.size()>maximum_actors) throw std::runtime_error("Scene exceeds the Actor count limit.");
        for (const auto& member:members) {
            const auto actor=take(world::inspect_actor(runtime,member.id()));
            if (actor.type!=types_.actor && actor.type!=types_.static_mesh_actor) throw std::runtime_error("Scene format does not support Actor type "+type_name(runtime,actor.type));
            auto components=take(world::components(runtime,member.id()));
            if (components.size()>maximum_components) throw std::runtime_error("Scene exceeds the Component count limit.");
            const auto root=take(world::root_component(runtime,member.id())).value_or(ObjectId{});
            std::unordered_map<ObjectId,std::size_t> identities;
            identities.reserve(components.size());
            for (std::size_t i=0;i<components.size();++i) identities.emplace(components[i].id(),i+1);
            const auto object_key=[&](ObjectId id) -> std::size_t {
                if (id==ObjectId{}) return 0;
                const auto found=identities.find(id);
                if (found==identities.end()) throw std::runtime_error("Scene relationship references a component outside its Actor.");
                return found->second;
            };
            Json values=Json::array();
            for (const auto& component:components) {
                const auto state=take(world::inspect_component(runtime,component.id()));
                if (state.type!=types_.component && state.type!=types_.scene_component &&
                    state.type!=types_.static_mesh_component && state.type!=physics_types_.body_component)
                    throw std::runtime_error("Scene format does not support Component type "+type_name(runtime,state.type));
                Json value={{"id",object_key(component.id())},{"type",type_name(runtime,state.type)},{"enabled",state.enabled}};
                if (state.type!=types_.component && state.type!=physics_types_.body_component) {
                    const auto trs=take(world::read_transform(runtime,component.id()));
                    value["position"]=vector(trs.position); value["scale"]=vector(trs.scale);
                    value["rotation"]=Json::array({trs.rotation.x,trs.rotation.y,trs.rotation.z,trs.rotation.w});
                    value["parent"]=object_key(take(world::scene_parent(runtime,component.id())));
                }
                if (state.type==types_.static_mesh_component) {
                    const auto mesh=take(world::read_static_mesh(runtime,component.id()));
                    validate_color(mesh.material.base_color);
                    if (mesh.mesh.resolved && mesh.mesh.source.empty()) throw std::runtime_error("A procedural mesh has no persistent resource source. Save/import the mesh first.");
                    value["mesh"]=mesh.mesh.source; value["base_color"]=vector(mesh.material.base_color);
                }
                if (state.type==physics_types_.body_component)
                    value["physics"]=body_settings(take(physics::read_settings(runtime,component.id())));
                values.push_back(std::move(value));
            }
            actors.push_back({{"type",type_name(runtime,actor.type)},{"name",utf8(name(member.id()))},{"enabled",actor.enabled},
                {"root",object_key(root)},{"components",std::move(values)}});
        }
        auto text=Json{{"version",1},{"actors",std::move(actors)}}.dump(2)+"\n";
        if (text.size()>json_format::maximum_bytes) throw std::runtime_error("Scene exceeds the 16 MiB file limit.");
        return text;
    } catch (const std::exception& error) { return failed(error); }
}
Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text,Project& project) {
    try {
        const auto data=json_format::parse(text);
        fields(data,{"version","actors"});
        if (json_format::identity(data.at("version"))!=1) throw std::runtime_error("Unsupported scene format version.");
        const auto& actors=data.at("actors");
        if (!actors.is_array() || actors.size()>maximum_actors) throw std::runtime_error("Invalid scene Actor count.");
        auto session=std::make_unique<Session>(); auto& world=session->world_; auto& runtime=session->runtime_;
        const auto& types=session->types_;
        for (const auto& actor:actors) {
            fields(actor,{"type","name","enabled","root","components"});
            const auto type=actor.at("type").get<std::string>();
            const auto actual=type=="world.Actor"?types.actor:type=="world.StaticMeshActor"?types.static_mesh_actor:TypeId{};
            if (actual==TypeId{}) throw std::runtime_error("Unsupported scene Actor type: "+type);
            const auto label=wide(actor.at("name").get<std::string>());
            if (auto valid=validate_name(label);!valid) throw std::runtime_error(valid.error().message);
            auto root=take(world::create_actor(runtime,actual));
            const auto& components=actor.at("components");
            if (!components.is_array() || components.size()>maximum_components) throw std::runtime_error("Invalid scene Component count.");
            std::vector<ObjectRoot> roots; roots.reserve(components.size());
            std::unordered_map<std::size_t,ObjectId> ids;
            ids.reserve(components.size());
            for (const auto& component:components) {
                const auto name=component.at("type").get<std::string>();
                const auto component_type=name=="world.Component"?types.component:name=="world.SceneComponent"?types.scene_component:
                    name=="world.StaticMeshComponent"?types.static_mesh_component:
                    name=="physics.BodyComponent"?session->physics_types_.body_component:TypeId{};
                if (component_type==TypeId{}) throw std::runtime_error("Unsupported scene Component type: "+name);
                if (component_type==types.component) fields(component,{"id","type","enabled"});
                else if (component_type==session->physics_types_.body_component)
                    fields(component,{"id","type","enabled","physics"});
                else if (component_type==types.scene_component) fields(component,{"id","type","enabled","position","rotation","scale","parent"});
                else fields(component,{"id","type","enabled","position","rotation","scale","parent","mesh","base_color"});
                auto value=take(world::create_component(runtime,component_type));
                const auto key=json_format::identity(component.at("id"));
                if (!key || !ids.emplace(key,value.id()).second) throw std::runtime_error("Duplicate or empty scene Component identity.");
                roots.push_back(std::move(value));
            }
            const auto resolve=[&](std::size_t key) { return key?ids.at(key):ObjectId{}; };
            const auto scene_root=resolve(json_format::identity(actor.at("root")));
            std::vector<world::Attachment> attachments;
            attachments.reserve(components.size());
            for (std::size_t i=0;i<components.size();++i) {
                const auto& component=components[i]; const auto id=roots[i].id();
                const auto component_type=take(runtime.type_of(id)); ObjectId parent;
                if (component_type!=types.component && component_type!=session->physics_types_.body_component) {
                    const auto rotation=json_format::vector<4>(component.at("rotation"));
                    take(world.set_transform(id,{vector(component.at("position")),{rotation[0],rotation[1],rotation[2],rotation[3]},vector(component.at("scale"))}));
                    parent=resolve(json_format::identity(component.at("parent")));
                }
                if (component_type==types.static_mesh_component) {
                    const auto source=component.at("mesh").get<std::string>();
                    geometry::MeshRef mesh; if (!source.empty()) mesh=take(project.mesh(source)).mesh;
                    take(world::set_mesh_binding(runtime,id,{source,std::move(mesh)}));
                    const auto color=vector(component.at("base_color"));
                    validate_color(color);
                    take(runtime.with_native<world::StaticMeshComponent>(id,[&](auto& value)->Result<void>{ value.material.base_color=color; return {}; }));
                }
                committed(world.set_enabled(id,component.at("enabled").get<bool>()));
                attachments.push_back({id,parent,id==scene_root});
            }
            for (std::size_t i=0;i<components.size();++i) {
                const auto& component=components[i]; const auto id=roots[i].id();
                const auto component_type=take(runtime.type_of(id));
                if (component_type==session->physics_types_.body_component) {
                    auto settings=body_settings(component.at("physics"));
                    if (settings.shape==physics::ShapeKind::triangle_mesh) {
                        if (scene_root==ObjectId{}) throw std::runtime_error("Triangle collision needs a root mesh.");
                        const auto mesh=take(world::read_static_mesh(runtime,scene_root));
                        if (mesh.mesh.source.empty()) throw std::runtime_error("Triangle collision needs a persistent mesh source.");
                        if (mesh.mesh.source.starts_with("project/"))
                            settings.cooked_shape=take(project.collision(mesh.mesh.source,
                                take(world::read_transform(runtime,scene_root)).scale));
                        else if (!mesh.mesh.source.starts_with("builtin/"))
                            throw std::runtime_error("Triangle collision has an unsupported mesh source.");
                    }
                    take(physics::configure(runtime,id,std::move(settings)));
                }
            }
            if (!attachments.empty()) committed(world.attach_components(root.id(),attachments));
            committed(world.set_enabled(root.id(),actor.at("enabled").get<bool>()));
            committed(world.attach(root.id()));
            session->names_.emplace(root.id(),label);
        }
        session->next_name_=actors.size()+1; return session;
    } catch (const std::exception& error) { return failed(error); }
}
}

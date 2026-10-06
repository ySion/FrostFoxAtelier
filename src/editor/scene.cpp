#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/json_format.hpp"
#include "editor/fields.hpp"
#include "physics/physics.hpp"
#include <limits>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace atelier::editor {
namespace {
using json_format::Json;
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
std::string type_name(Runtime& runtime,TypeId type) { return std::string(take(runtime.describe(type))->name); }
auto failed(const std::exception& error) { return std::unexpected(Error{ErrorCode::invalid_storage,error.what()}); }
using json_format::fields;
void validate_color(math::Vector3 color) {
    if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) || color.x<0 || color.y<0 || color.z<0 || color.x>1 || color.y>1 || color.z>1)
        throw std::runtime_error("Invalid scene material color.");
}
bool derives(Runtime& runtime,TypeId type,TypeId base) { return take(runtime.is_a(type,base)); }
bool physical_type(Runtime& runtime,TypeId type) {
    const auto body=runtime.find_type("physics.BodyComponent");
    return body && derives(runtime,type,*body);
}
std::vector<detail::ScalarField> business_fields(Runtime& runtime,const world::Types& types,TypeId type) {
    auto fields=take(detail::scalar_fields(runtime,type));
    std::vector<FieldId> reserved;
    if (derives(runtime,type,types.scene_component))
        for (const auto name:{"position","rotation","scale"}) reserved.push_back(take(runtime.find_field(types.scene_component,name)));
    if (derives(runtime,type,types.static_mesh_component)) reserved.push_back(take(runtime.find_field(types.static_mesh_component,"material")));
    std::erase_if(fields,[&](const auto& field) { return std::ranges::find(reserved,field.path.front())!=reserved.end(); });
    return fields;
}
Json write_body(Runtime& runtime,ObjectId object) {
    const auto settings=take(physics::read_settings(runtime,object));
    const auto shape=settings.shape==physics::ShapeKind::box?"box":settings.shape==physics::ShapeKind::sphere?"sphere":
        settings.shape==physics::ShapeKind::triangle_mesh?"triangle_mesh":nullptr;
    const auto motion=settings.motion==physics::MotionKind::stationary?"stationary":settings.motion==physics::MotionKind::kinematic?"kinematic":
        settings.motion==physics::MotionKind::dynamic?"dynamic":nullptr;
    if (!shape || !motion) throw std::runtime_error("Unknown physical body shape or motion.");
    if (!std::isfinite(settings.radius) || settings.radius<=0 ||
        !std::isfinite(settings.half_extents.x) || !std::isfinite(settings.half_extents.y) || !std::isfinite(settings.half_extents.z) ||
        settings.half_extents.x<=0 || settings.half_extents.y<=0 || settings.half_extents.z<=0 ||
        (settings.shape==physics::ShapeKind::triangle_mesh && settings.motion!=physics::MotionKind::stationary))
        throw std::runtime_error("Invalid physical body settings.");
    return {{"shape",shape},{"motion",motion},{"half_extents",vector(settings.half_extents)},{"radius",settings.radius}};
}
void read_body(Runtime& runtime,ObjectId object,const Json& value) {
    fields(value,{"shape","motion","half_extents","radius"});
    const auto shape=value.at("shape").get<std::string>(), motion=value.at("motion").get<std::string>();
    physics::BodySettings settings;
    if (shape=="box") settings.shape=physics::ShapeKind::box;
    else if (shape=="sphere") settings.shape=physics::ShapeKind::sphere;
    else if (shape=="triangle_mesh") settings.shape=physics::ShapeKind::triangle_mesh;
    else throw std::runtime_error("Unknown physical body shape.");
    if (motion=="stationary") settings.motion=physics::MotionKind::stationary;
    else if (motion=="kinematic") settings.motion=physics::MotionKind::kinematic;
    else if (motion=="dynamic") settings.motion=physics::MotionKind::dynamic;
    else throw std::runtime_error("Unknown physical body motion.");
    settings.half_extents=vector(value.at("half_extents"));
    if (!value.at("radius").is_number()) throw std::runtime_error("Expected a numeric body radius.");
    settings.radius=value.at("radius").get<float>();
    take(physics::configure(runtime,object,settings));
}
Json encode_scalar(Runtime& runtime,const Scalar& value) {
    if (!detail::finite_scalar(value)) throw std::runtime_error("Scene property values must be finite.");
    return std::visit([&](const auto& v)->Json {
        if constexpr (std::same_as<std::decay_t<decltype(v)>,EnumValue>) {
            const auto info=take(runtime.describe(v.type));
            for (const auto& [name,bits]:info->enumeration->values) if (bits==v.bits) return name;
            throw std::runtime_error("Scene enum value has no declared name.");
        } else return v;
    },value);
}
Scalar decode_scalar(Runtime& runtime,const Scalar& prototype,const Json& value) {
    return std::visit([&](const auto& original)->Scalar {
        using T=std::decay_t<decltype(original)>;
        if constexpr (std::same_as<T,bool>) {
            if (!value.is_boolean()) throw std::runtime_error("Expected a boolean property.");
            return value.get<bool>();
        } else if constexpr (std::same_as<T,EnumValue>) {
            if (!value.is_string()) throw std::runtime_error("Expected a declared enum member name.");
            const auto info=take(runtime.describe(original.type));
            for (const auto& [name,bits]:info->enumeration->values) if (value.get_ref<const std::string&>()==name) return EnumValue{original.type,bits};
            throw std::runtime_error("Unknown scene enum member.");
        } else if constexpr (std::is_integral_v<T>) {
            if (!value.is_number_integer()) throw std::runtime_error("Expected an integer property.");
            if (value.is_number_unsigned()) {
                const auto number=value.get<std::uint64_t>();
                if (number>static_cast<std::uint64_t>(std::numeric_limits<T>::max())) throw std::runtime_error("Integer property is out of range.");
                return static_cast<T>(number);
            }
            const auto number=value.get<std::int64_t>();
            if (number<static_cast<std::int64_t>(std::numeric_limits<T>::min()) || number>static_cast<std::int64_t>(std::numeric_limits<T>::max()))
                throw std::runtime_error("Integer property is out of range.");
            return static_cast<T>(number);
        } else {
            if (!value.is_number()) throw std::runtime_error("Expected a numeric property.");
            const auto number=value.get<T>();
            if (!std::isfinite(number)) throw std::runtime_error("Scene property values must be finite and representable.");
            return number;
        }
    },prototype);
}
Json write_properties(Runtime& runtime,const world::Types& types,ObjectId object,TypeId type) {
    Json result=Json::object();
    for (const auto& field:business_fields(runtime,types,type)) {
        auto* at=&result;
        for (const auto& name:field.names) at=&(*at)[name];
        *at=encode_scalar(runtime,take(runtime.read(object,field.path)));
    }
    return result;
}
void read_properties(Runtime& runtime,const world::Types& types,ObjectId object,TypeId type,const Json& properties) {
    const auto schema=business_fields(runtime,types,type);
    const auto visit=[&](auto&& self,const Json& value,std::vector<std::string> path)->void {
        if (!value.is_object()) throw std::runtime_error("Expected a scene property record.");
        for (const auto& [name,item]:value.items()) {
            auto next=path;next.push_back(name);
            const auto field=std::ranges::find_if(schema,[&](const auto& f){return f.names==next;});
            if (field!=schema.end()) {
                const auto prototype=take(runtime.read(object,field->path));
                take(runtime.write(object,field->path,decode_scalar(runtime,prototype,item)));
            } else {
                const bool parent=std::ranges::any_of(schema,[&](const auto& f){return f.names.size()>next.size() && std::equal(next.begin(),next.end(),f.names.begin());});
                if (!parent) throw std::runtime_error("Unknown or unsupported scene property: "+name);
                self(self,item,std::move(next));
            }
        }
    };
    visit(visit,properties,{});
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
            if (!derives(runtime,actor.type,types_.actor)) throw std::runtime_error("Scene format does not support Actor type "+type_name(runtime,actor.type));
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
                if (!derives(runtime,state.type,types_.component))
                    throw std::runtime_error("Scene format does not support Component type "+type_name(runtime,state.type));
                Json value={{"id",object_key(component.id())},{"type",type_name(runtime,state.type)},{"enabled",state.enabled},
                    {"properties",write_properties(runtime,types_,component.id(),state.type)}};
                if (derives(runtime,state.type,types_.scene_component)) {
                    const auto trs=take(world::read_transform(runtime,component.id()));
                    value["position"]=vector(trs.position); value["scale"]=vector(trs.scale);
                    value["rotation"]=Json::array({trs.rotation.x,trs.rotation.y,trs.rotation.z,trs.rotation.w});
                    value["parent"]=object_key(take(world::scene_parent(runtime,component.id())));
                }
                if (derives(runtime,state.type,types_.static_mesh_component)) {
                    const auto mesh=take(world::read_static_mesh(runtime,component.id()));
                    validate_color(mesh.material.base_color);
                    if (mesh.mesh.resolved && mesh.mesh.source.empty()) throw std::runtime_error("A procedural mesh has no persistent resource source. Save/import the mesh first.");
                    value["mesh"]=mesh.mesh.source; value["base_color"]=vector(mesh.material.base_color);
                }
                if (physical_type(runtime,state.type)) value["body"]=write_body(runtime,component.id());
                values.push_back(std::move(value));
            }
            actors.push_back({{"type",type_name(runtime,actor.type)},{"name",utf8(name(member.id()))},{"enabled",actor.enabled},
                {"root",object_key(root)},{"components",std::move(values)},
                {"properties",write_properties(runtime,types_,member.id(),actor.type)}});
        }
        auto text=Json{{"version",2},{"actors",std::move(actors)}}.dump(2)+"\n";
        if (text.size()>json_format::maximum_bytes) throw std::runtime_error("Scene exceeds the 16 MiB file limit.");
        return text;
    } catch (const std::exception& error) { return failed(error); }
}
Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text,Project& project,Prepare prepare) {
    try {
        const auto data=json_format::parse(text);
        fields(data,{"version","actors"});
        const auto version=json_format::identity(data.at("version"));
        if (version!=1 && version!=2) throw std::runtime_error("Unsupported scene format version.");
        const auto& actors=data.at("actors");
        if (!actors.is_array() || actors.size()>maximum_actors) throw std::runtime_error("Invalid scene Actor count.");
        auto session=std::make_unique<Session>(std::move(prepare)); auto& world=session->world_; auto& runtime=session->runtime_;
        const auto& types=session->types_;
        for (const auto& actor:actors) {
            if (version==1) fields(actor,{"type","name","enabled","root","components"});
            else fields(actor,{"type","name","enabled","root","components","properties"});
            const auto type=actor.at("type").get<std::string>();
            const auto actual=take(runtime.find_type(type));
            if (!derives(runtime,actual,types.actor)) throw std::runtime_error("Unsupported scene Actor type: "+type);
            const auto label=wide(actor.at("name").get<std::string>());
            if (auto valid=validate_name(label);!valid) throw std::runtime_error(valid.error().message);
            auto root=take(world::create_actor(runtime,actual));
            read_properties(runtime,types,root.id(),actual,actor.value("properties",Json::object()));
            const auto& components=actor.at("components");
            if (!components.is_array() || components.size()>maximum_components) throw std::runtime_error("Invalid scene Component count.");
            std::vector<ObjectRoot> roots; roots.reserve(components.size());
            std::unordered_map<std::size_t,ObjectId> ids;
            ids.reserve(components.size());
            for (const auto& component:components) {
                const auto name=component.at("type").get<std::string>();
                const auto component_type=take(runtime.find_type(name));
                if (!derives(runtime,component_type,types.component)) throw std::runtime_error("Scene type is not a Component: "+name);
                const bool spatial=derives(runtime,component_type,types.scene_component);
                const bool mesh=derives(runtime,component_type,types.static_mesh_component);
                Json structural=component;
                if (version==2) structural.erase("properties");
                const bool physical=physical_type(runtime,component_type);
                if (version==2 && physical) structural.erase("body");
                if (mesh) fields(structural,{"id","type","enabled","position","rotation","scale","parent","mesh","base_color"});
                else if (spatial) fields(structural,{"id","type","enabled","position","rotation","scale","parent"});
                else fields(structural,{"id","type","enabled"});
                auto value=take(world::create_component(runtime,component_type));
                read_properties(runtime,types,value.id(),component_type,component.value("properties",Json::object()));
                if (physical) read_body(runtime,value.id(),component.at("body"));
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
                if (derives(runtime,component_type,types.scene_component)) {
                    const auto rotation=json_format::vector<4>(component.at("rotation"));
                    take(world.set_transform(id,{vector(component.at("position")),{rotation[0],rotation[1],rotation[2],rotation[3]},vector(component.at("scale"))}));
                    parent=resolve(json_format::identity(component.at("parent")));
                }
                if (derives(runtime,component_type,types.static_mesh_component)) {
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
            if (!attachments.empty()) committed(world.attach_components(root.id(),attachments));
            committed(world.set_enabled(root.id(),actor.at("enabled").get<bool>()));
            committed(world.attach(root.id()));
            session->names_.emplace(root.id(),label);
        }
        session->next_name_=actors.size()+1; return session;
    } catch (const std::exception& error) { return failed(error); }
}
}

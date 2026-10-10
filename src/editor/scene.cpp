#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/json_format.hpp"
#include "editor/fields.hpp"
#include "editor/material_format.hpp"
#include "physics/physics.hpp"
#include <limits>
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
    constexpr std::array shapes{"box","sphere","triangle_mesh","capsule"};
    constexpr std::array motions{"stationary","kinematic","dynamic"};
    if (static_cast<unsigned>(value.shape)>=shapes.size() || static_cast<unsigned>(value.motion)>=motions.size() ||
        !math::is_finite(value.half_extents) || value.half_extents.x<=0 || value.half_extents.y<=0 || value.half_extents.z<=0 ||
        !std::isfinite(value.radius) || value.radius<=0 || !std::isfinite(value.capsule_half_height) || value.capsule_half_height<=0 ||
        value.category>=value.responses.size() || !std::isfinite(value.material.friction) || value.material.friction<0 ||
        !std::isfinite(value.material.restitution) || value.material.restitution<0 || value.material.restitution>1 ||
        (value.shape==physics::ShapeKind::triangle_mesh && value.motion!=physics::MotionKind::stationary) ||
        (value.cooked_shape && value.shape!=physics::ShapeKind::triangle_mesh))
        throw std::runtime_error("Invalid physical body settings.");
    Json responses=Json::array();
    for (auto response:value.responses) {
        if (static_cast<unsigned>(response)>static_cast<unsigned>(physics::Response::block))
            throw std::runtime_error("Invalid physics collision response.");
        responses.push_back(static_cast<unsigned>(response));
    }
    return {{"shape",shapes[static_cast<unsigned>(value.shape)]},
        {"motion",motions[static_cast<unsigned>(value.motion)]},
        {"half_extents",vector(value.half_extents)},
        {"radius",value.radius},{"capsule_half_height",value.capsule_half_height},
        {"category",value.category},{"responses",std::move(responses)},
        {"sensor",value.sensor},{"friction",value.material.friction},
        {"restitution",value.material.restitution}};
}
physics::BodySettings body_settings(const Json& value,bool legacy) {
    fields(value,{"shape","motion","half_extents","radius","capsule_half_height",
        "category","responses","sensor","friction","restitution"});
    if (legacy) for (const auto key:{"capsule_half_height","category","responses","sensor","friction","restitution"})
        (void)value.at(key);
    physics::BodySettings settings;
    const auto enumeration=[&](const char* key,auto names) {
        if (legacy) return json_format::identity(value.at(key));
        const auto name=value.at(key).get<std::string>();
        const auto found=std::ranges::find(names,name);
        if (found==names.end()) throw std::runtime_error(std::string("Unknown physics ")+key);
        return static_cast<std::size_t>(found-names.begin());
    };
    const auto shape=enumeration("shape",std::array{"box","sphere","triangle_mesh","capsule"});
    const auto motion=enumeration("motion",std::array{"stationary","kinematic","dynamic"});
    if (shape>static_cast<unsigned>(physics::ShapeKind::capsule) ||
        motion>static_cast<unsigned>(physics::MotionKind::dynamic))
        throw std::runtime_error("Invalid physics shape or motion.");
    settings.shape=static_cast<physics::ShapeKind>(shape);
    settings.motion=static_cast<physics::MotionKind>(motion);
    settings.half_extents=vector(value.at("half_extents"));
    const auto number=[&](const char* key,float fallback) {
        const auto found=value.find(key);
        if (found==value.end()) return fallback;
        const auto& item=*found;
        if (!item.is_number()) throw std::runtime_error(std::string("Expected numeric physics field: ")+key);
        return item.get<float>();
    };
    if (!value.contains("radius")) throw std::runtime_error("Missing physics radius.");
    settings.radius=number("radius",settings.radius);
    settings.capsule_half_height=number("capsule_half_height",settings.capsule_half_height);
    const auto category=value.contains("category")?json_format::identity(value.at("category")):settings.category;
    if (category>=settings.responses.size()) throw std::runtime_error("Invalid physics category.");
    settings.category=static_cast<std::uint8_t>(category);
    if (value.contains("responses")) {
        const auto& responses=value.at("responses");
        if (!responses.is_array() || responses.size()!=settings.responses.size())
            throw std::runtime_error("Physics responses require 16 entries.");
        for (std::size_t i=0;i<settings.responses.size();++i) {
            const auto response=json_format::identity(responses[i]);
            if (response>static_cast<unsigned>(physics::Response::block))
                throw std::runtime_error("Invalid physics collision response.");
            settings.responses[i]=static_cast<physics::Response>(response);
        }
    }
    settings.sensor=value.value("sensor",settings.sensor);
    settings.material={number("friction",settings.material.friction),number("restitution",settings.material.restitution)};
    return settings;
}
std::string type_name(Runtime& runtime,TypeId type) { return std::string(take(runtime.describe(type))->name); }
auto failed(const std::exception& error) { return std::unexpected(Error{ErrorCode::invalid_storage,error.what()}); }
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
Result<world::Commit> Session::duplicate_selected() {
    ActorChange change; change.added=true;
    std::vector<std::pair<ObjectId,std::wstring>> labels;
    std::vector<ObjectId> attached;
    try {
        if (editing()) throw std::runtime_error("Finish the active edit before duplicating.");
        if (selected_actors_.empty()) return world::Commit{};
        if (take(world_.members()).size()+selected_actors_.size()>maximum_actors)
            throw std::runtime_error("Editor Actor limit reached.");
        const auto copy_fields=[&](ObjectId from,ObjectId to,TypeId type) {
            for (const auto& field:business_fields(runtime_,types_,type)) {
                const auto value=take(runtime_.read(from,field.path));
                if (!detail::finite_scalar(value)) throw std::runtime_error("Cannot duplicate non-finite scene properties.");
                take(runtime_.write(to,field.path,value));
            }
        };
        // Prepare the complete detached graph before publishing any membership.
        // Runtime identities and component ownership are never copied as bytes.
        for (const auto source:selected_actors_) {
            take(admit_actor_edit(source));
            const auto actor=take(world::inspect_actor(runtime_,source));
            auto root=take(world::create_actor(runtime_,actor.type));
            copy_fields(source,root.id(),actor.type);
            const auto parts=take(world::components(runtime_,source));
            if (parts.size()>maximum_components) throw std::runtime_error("Editor Component limit reached.");
            const auto source_root=take(world::root_component(runtime_,source));
            std::vector<ObjectRoot> roots; roots.reserve(parts.size());
            std::unordered_map<ObjectId,ObjectId> ids;
            for (const auto& part:parts) {
                auto copy=take(world::create_component(runtime_,take(runtime_.type_of(part.id()))));
                ids.emplace(part.id(),copy.id()); roots.push_back(std::move(copy));
            }
            std::vector<world::Attachment> attachments; attachments.reserve(parts.size());
            for (std::size_t i=0;i<parts.size();++i) {
                const auto from=parts[i].id(),to=roots[i].id();
                const auto state=take(world::inspect_component(runtime_,from));
                copy_fields(from,to,state.type);
                ObjectId parent;
                if (derives(runtime_,state.type,types_.scene_component)) {
                    take(world_.set_transform(to,take(world::read_transform(runtime_,from))));
                    const auto old_parent=take(world::scene_parent(runtime_,from));
                    if (old_parent!=ObjectId{}) parent=ids.at(old_parent);
                }
                if (derives(runtime_,state.type,types_.static_mesh_component)) {
                    const auto mesh=take(world::read_static_mesh(runtime_,from));
                    take(world::validate_material(mesh.material));
                    take(world::set_mesh_binding(runtime_,to,mesh.mesh));
                    auto materials=take(world::read_material_bindings(runtime_,from));
                    for(auto& binding:materials) {
                        const auto state=take(material::read_material(runtime_,binding.resource.id()));
                        if(state.source.empty()) binding.resource=take(material::create_material(runtime_,state));
                    }
                    take(world::set_material_bindings(runtime_,to,materials));
                    take(runtime_.with_native<world::StaticMeshComponent>(to,[&](auto& value)->Result<void> {
                        value.material=mesh.material;return {};
                    }));
                }
                if (physical_type(runtime_,state.type)) take(physics::configure(runtime_,to,take(physics::read_settings(runtime_,from))));
                if (derives(runtime_,state.type,types_.light_component)) take(world::validate_light(take(world::read_light(runtime_,to))));
                committed(world_.set_enabled(to,state.enabled));
                attachments.push_back({to,parent,source_root && *source_root==from});
                if (!name(from).empty()) labels.emplace_back(to,name(from));
            }
            if (!attachments.empty()) committed(world_.attach_components(root.id(),attachments));
            committed(world_.set_enabled(root.id(),actor.enabled));
            std::wstring label;
            for (std::size_t suffix=1;;++suffix) {
                const auto tail=L" Copy"+(suffix==1?std::wstring{}:L" "+std::to_wstring(suffix));
                auto prefix=std::wstring(name(source).substr(0,256-tail.size()));
                if (!prefix.empty() && prefix.back()>=0xd800 && prefix.back()<=0xdbff) prefix.pop_back();
                label=prefix+tail;
                if (std::ranges::none_of(names_,[&](const auto& item){return item.second==label;}) &&
                    std::ranges::none_of(labels,[&](const auto& item){return item.second==label;})) break;
            }
            take(validate_name(label));
            labels.emplace_back(root.id(),std::move(label));
            change.removals.emplace_back().root=std::move(root);
        }
        world::Commit result;
        for (auto& item:change.removals) {
            auto added=take(world_.attach(item.id()));
            if (added.changed) attached.push_back(item.id());
            result.changed|=added.changed;
            if (added.notification_error && !result.notification_error) result.notification_error=std::move(added.notification_error);
        }
        for (auto& [id,label]:labels) names_.emplace(id,std::move(label));
        if (attached.empty()) return result;
        take(select_actors(attached,attached.back()));
        remember(std::move(change));
        return result;
    } catch (const std::exception& error) {
        // Membership is reversible in authoring mode. Keep any exceptional
        // failed rollback owned by history rather than losing an attached graph.
        bool rollback_failed{};
        for (auto it=attached.rbegin();it!=attached.rend();++it) if (!world_.detach(*it)) rollback_failed=true;
        if (rollback_failed) remember(std::move(change));
        return failed(error);
    }
}
Result<std::string> Session::serialize() const {
    try {
        if (editing()) throw std::runtime_error("Finish the active edit before saving.");
        auto& runtime=world_.runtime();
        Json actors=Json::array();
        const auto members=take(world_.members());
        std::unordered_map<ObjectId,std::size_t> actor_keys;
        for(std::size_t i=0;i<members.size();++i) actor_keys.emplace(members[i].id(),i+1);
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
                    take(world::validate_material(mesh.material));
                    if (mesh.mesh.resolved && mesh.mesh.source.empty()) throw std::runtime_error("A procedural mesh has no persistent resource source. Save/import the mesh first.");
                    value["mesh"]=mesh.mesh.source; value["base_color"]=vector(mesh.material.base_color);
                    value["roughness"]=mesh.material.roughness;value["metallic"]=mesh.material.metallic;
                    value["emissive"]=vector(mesh.material.emissive);
                    auto bindings=Json::array();
                    for(const auto& binding:take(world::read_material_bindings(runtime,component.id()))) {
                        const auto asset=take(material_asset(binding.resource.id()));
                        Json entry{{"slot",binding.slot},{"source",asset.key},{"overridden",binding.overridden}};
                        if(asset.key.empty()) entry["inline"]=material_format::encode(asset);
                        const auto state=take(material::read_material(runtime,binding.resource.id()));
                        if(state.parent.registered()) {
                            entry["parent"]=take(material::read_material(runtime,state.parent.id())).source;
                            auto overrides=Json::array();
                            for(std::size_t i=0;i<material::parameter_names.size();++i) if(state.overrides & (1u<<i)) overrides.push_back(material::parameter_names[i]);
                            entry["overrides"]=std::move(overrides);
                        }
                        bindings.push_back(std::move(entry));
                    }
                    value["materials"]=std::move(bindings);
                }
                if(derives(runtime,state.type,types_.light_component)) take(world::validate_light(take(world::read_light(runtime,component.id()))));
                if (physical_type(runtime,state.type)) value["body"]=body_settings(take(physics::read_settings(runtime,component.id())));
                if(derives(runtime,state.type,physics_types_.distance_constraint_component)) {
                    const auto target=take(physics::constraint_target(runtime,component.id()));
                    if(target!=ObjectId{} && !actor_keys.contains(target)) throw std::runtime_error("Constraint target must belong to this scene.");
                    value["constraint_target"]=target==ObjectId{}?0:actor_keys.at(target);
                }
                values.push_back(std::move(value));
            }
            actors.push_back({{"type",type_name(runtime,actor.type)},{"name",utf8(name(member.id()))},{"enabled",actor.enabled},
                {"root",object_key(root)},{"components",std::move(values)},
                {"properties",write_properties(runtime,types_,member.id(),actor.type)}});
        }
        auto text=Json{{"version",3},{"actors",std::move(actors)}}.dump(2)+"\n";
        if (text.size()>json_format::maximum_bytes) throw std::runtime_error("Scene exceeds the 16 MiB file limit.");
        return text;
    } catch (const std::exception& error) { return failed(error); }
}
Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text,Project& project,Prepare prepare) {
    try {
        const auto data=json_format::parse(text);
        fields(data,{"version","actors"});
        const auto version=json_format::identity(data.at("version"));
        if (version<1 || version>3) throw std::runtime_error("Unsupported scene format version.");
        const auto& actors=data.at("actors");
        if (!actors.is_array() || actors.size()>maximum_actors) throw std::runtime_error("Invalid scene Actor count.");
        auto session=std::make_unique<Session>(std::move(prepare)); auto& world=session->world_; auto& runtime=session->runtime_;
        const auto& types=session->types_;
        std::vector<ObjectId> actor_ids;
        std::vector<std::pair<ObjectId,std::size_t>> constraint_targets;
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
                if (version>=2) structural.erase("properties");
                if(mesh && version>=3) structural.erase("materials");
                const bool physical=physical_type(runtime,component_type);
                const bool constraint=derives(runtime,component_type,session->physics_types_.distance_constraint_component);
                if(constraint) structural.erase("constraint_target");
                if (physical) structural.erase(version==1?"physics":"body");
                if (mesh) fields(structural,{"id","type","enabled","position","rotation","scale","parent","mesh","base_color","roughness","metallic","emissive"});
                else if (spatial) fields(structural,{"id","type","enabled","position","rotation","scale","parent"});
                else fields(structural,{"id","type","enabled"});
                auto value=take(world::create_component(runtime,component_type));
                if(constraint) constraint_targets.emplace_back(value.id(),json_format::identity(component.at("constraint_target")));
                read_properties(runtime,types,value.id(),component_type,component.value("properties",Json::object()));
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
                    world::Material material;material.base_color=vector(component.at("base_color"));
                    if(component.contains("roughness")) material.roughness=std::get<float>(decode_scalar(runtime,Scalar{material.roughness},component.at("roughness")));
                    if(component.contains("metallic")) material.metallic=std::get<float>(decode_scalar(runtime,Scalar{material.metallic},component.at("metallic")));
                    if(component.contains("emissive")) material.emissive=vector(component.at("emissive"));
                    take(world::validate_material(material));
                    take(runtime.with_native<world::StaticMeshComponent>(id,[&](auto& value)->Result<void>{ value.material=material; return {}; }));
                    if(version>=3) {
                        const auto& entries=component.at("materials");
                        if(!entries.is_array() || entries.size()>1024) throw std::runtime_error("Invalid material binding table.");
                        std::vector<world::MaterialBinding> bindings;
                        for(const auto& entry:entries) {
                            fields(entry,{"slot","source","overridden","inline","parent","overrides"});
                            const auto key=entry.at("source").get<std::string>();
                            if(!key.empty() && entry.contains("inline")) throw std::runtime_error("A material binding must have one source.");
                            ObjectRoot resource;
                            if(entry.contains("parent")) {
                                if(!key.empty()) throw std::runtime_error("A material instance cannot own an asset identity.");
                                auto parent=take(session->material_resource(project,entry.at("parent").get<std::string>()));
                                auto state=take(material::read_material(runtime,parent.id()));state.source.clear();state.parent=parent;state.overrides=0;
                                state.description=material_format::decode(entry.at("inline")).description;
                                const auto& names=entry.at("overrides");if(!names.is_array() || names.size()>material::parameter_names.size()) throw std::runtime_error("Invalid parameter overrides.");
                                for(const auto& name:names) {
                                    const auto found=std::ranges::find(material::parameter_names,name.get<std::string>());
                                    if(found==material::parameter_names.end()) throw std::runtime_error("Unknown parameter override.");
                                    const auto bit=1u<<static_cast<unsigned>(found-material::parameter_names.begin());
                                    if(state.overrides & bit) throw std::runtime_error("Duplicate parameter override.");state.overrides|=bit;
                                }
                                resource=take(material::create_material(runtime,state));
                            } else {
                                if(entry.contains("overrides")) throw std::runtime_error("Parameter overrides require a parent material.");
                                resource=key.empty()?take(session->material_resource(project,material_format::decode(entry.at("inline")))):
                                    take(session->material_resource(project,key));
                            }
                            bindings.push_back({entry.at("slot").get<std::string>(),std::move(resource),entry.at("overridden").get<bool>()});
                        }
                        take(world::set_material_bindings(runtime,id,bindings));
                    }
                }
                if(derives(runtime,component_type,types.light_component)) take(world::validate_light(take(world::read_light(runtime,id))));
                committed(world.set_enabled(id,component.at("enabled").get<bool>()));
                attachments.push_back({id,parent,id==scene_root});
            }
            for (std::size_t i=0;i<components.size();++i) {
                const auto& component=components[i]; const auto id=roots[i].id();
                const auto component_type=take(runtime.type_of(id));
                if (physical_type(runtime,component_type)) {
                    auto settings=body_settings(component.at(version==1?"physics":"body"),version==1);
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
            actor_ids.push_back(root.id());
        }
        for(const auto& [component,key]:constraint_targets) {
            if(key>actor_ids.size()) throw std::runtime_error("Constraint target is outside the scene.");
            take(physics::set_constraint_target(runtime,component,key?actor_ids[key-1]:ObjectId{}));
        }
        session->next_name_=actors.size()+1; return session;
    } catch (const std::exception& error) { return failed(error); }
}
}

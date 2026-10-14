#include "editor/session.hpp"
#include "editor/project.hpp"
#include "editor/json_format.hpp"
#include "editor/fields.hpp"
#include "editor/view_options.hpp"
#include "editor/material_format.hpp"
#include "physics/physics.hpp"
#include <limits>
#include <charconv>
#include <utility>
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
// Persistence and UI/Control share the same author-option directory. Labels and
// diagnostic preferences never become keys in a scene document.
Json world_settings(const rendering::SceneLightingSettings& settings) {
    renderer::ViewSettings view;static_cast<rendering::SceneLightingSettings&>(view.lit)=settings;
    Json result=Json::object();
    for(const auto& option:view_options()) if(world_option(option.option)) {
        const auto value=view_value(view,option.option);
        switch(option.kind) {
        case ViewValueKind::boolean:result[option.name]=value!=0;break;
        case ViewValueKind::enumeration:result[option.name]=option.choices[static_cast<std::size_t>(value)];break;
        case ViewValueKind::integer:result[option.name]=static_cast<std::int64_t>(value);break;
        case ViewValueKind::number:result[option.name]=value;break;
        }
    }
    return result;
}
rendering::SceneLightingSettings world_settings(const Json& data,bool legacy_shadow_settings) {
    if(!data.is_object()) throw std::runtime_error("Expected World settings object.");
    renderer::ViewSettings candidate;
    for(const auto& [name,value]:data.items()) {
        if(legacy_shadow_settings && (name=="shadow_light_budget" || name=="shadow_samples")) {
            // Version 4 described per-light tracing. Validate that old contract,
            // then use the new candidate default; reference is never authoring data.
            if(data.contains("direct_candidates")) throw std::runtime_error("Legacy shadow settings cannot be combined with direct_candidates.");
            if(!value.is_number()) throw std::runtime_error("Expected a numeric legacy World option: "+name);
            const auto number=value.get<double>();
            const double minimum=name=="shadow_samples"?1:0,maximum=name=="shadow_samples"?64:16;
            if(!std::isfinite(number) || number!=std::floor(number) || number<minimum || number>maximum)
                throw std::runtime_error("Invalid legacy World option: "+name);
            continue;
        }
        const auto* option=view_option(name);
        if(!option || option->name!=name || !world_option(option->option))
            throw std::runtime_error("Unknown World setting: "+name);
        double number{};
        if(option->kind==ViewValueKind::boolean) number=value.get<bool>()?1:0;
        else if(option->kind==ViewValueKind::enumeration) {
            const auto text=value.get<std::string>();const auto at=std::ranges::find(option->choices,text);
            if(at==option->choices.end()) throw std::runtime_error("Unknown World option value: "+text);
            number=static_cast<double>(at-option->choices.begin());
        } else {
            if(!value.is_number()) throw std::runtime_error("Expected a numeric World option.");
            number=value.get<double>();
        }
        take(set_view_option(candidate,option->option,number));
    }
    // Reject contradictory input instead of letting the interactive mutual-
    // exclusion setter choose according to JSON object iteration order.
    if(data.value("gtao",true) && data.value("ssil",false)) throw std::runtime_error("GTAO and Local GI are mutually exclusive.");
    if(const auto* error=rendering::scene_lighting_error(candidate.lit)) throw std::runtime_error(error);
    return candidate.lit;
}
Json vector(math::Vector3 value) { return Json::array({value.x,value.y,value.z}); }
math::Vector3 vector(const Json& value) {
    const auto v=json_format::vector<3>(value); return {v[0],v[1],v[2]};
}
physics::BodySettings body_settings(const Json& value,bool legacy) {
    fields(value,{"shape","motion","half_extents","radius","capsule_half_height",
        "category","responses","sensor","friction","restitution","enabled"});
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
    if (category>=16) throw std::runtime_error("Invalid physics category.");
    settings.category=static_cast<std::uint32_t>(category);
    if (value.contains("responses")) {
        const auto& responses=value.at("responses");
        if (!responses.is_array() || responses.size()!=16)
            throw std::runtime_error("Physics responses require 16 entries.");
        for (std::size_t i=0;i<16;++i) {
            const auto response=json_format::identity(responses[i]);
            if (response>static_cast<unsigned>(physics::Response::block))
                throw std::runtime_error("Invalid physics collision response.");
            take(settings.set_response(static_cast<std::uint32_t>(i),static_cast<physics::Response>(response)));
        }
    }
    settings.enabled=value.value("enabled",true);
    settings.sensor=value.value("sensor",settings.sensor);
    settings.material={number("friction",settings.material.friction),number("restitution",settings.material.restitution)};
    return settings;
}
std::string type_name(Runtime& runtime,TypeId type) { return std::string(take(runtime.describe(type))->name); }
auto failed(const std::exception& error) { return std::unexpected(Error{ErrorCode::invalid_storage,error.what()}); }
bool derives(Runtime& runtime,TypeId type,TypeId base) { return take(runtime.is_a(type,base)); }
bool physical_type(Runtime& runtime,TypeId type) {
    const auto body=runtime.native_type<world::PrimitiveComponent>();
    return body && derives(runtime,type,*body);
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

std::vector<detail::ScalarField> business_fields(Runtime& runtime,const world::Types& types,TypeId type) {
    auto result=take(detail::author_fields(runtime,type));std::vector<FieldId> reserved;
    if(derives(runtime,type,types.entity))
        for(const auto key:{"position","rotation","scale"}) reserved.push_back(take(runtime.find_field(types.entity,key)));
    if(derives(runtime,type,types.static_mesh_component)) reserved.push_back(take(runtime.find_field(types.static_mesh_component,"material")));
    std::erase_if(result,[&](const auto& field){return std::ranges::find(reserved,field.path.front())!=reserved.end();});
    return result;
}
std::uint64_t local_key(const Json& value,bool allow_null=false) {
    if(!value.is_string()) throw std::runtime_error("Document object identity requires a decimal string.");
    const auto& text=value.get_ref<const std::string&>();std::uint64_t result{};
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
    if(text.empty() || error!=std::errc{} || end!=text.data()+text.size() || std::to_string(result)!=text || (!allow_null && result==0))
        throw std::runtime_error("Invalid document object identity: "+text);
    return result;
}
Json ref_key(std::uint64_t id) {return Json{{"$ref",std::to_string(id)}};}
Json trs_json(world::Trs value) {
    value=take(world::canonical_transform(value));
    return {{"position",vector(value.position)},{"rotation",Json::array({value.rotation.x,value.rotation.y,value.rotation.z,value.rotation.w})},{"scale",vector(value.scale)},{"origin",nullptr}};
}
world::Trs json_trs(const Json& value) {
    const auto q=json_format::vector<4>(value.at("rotation"));
    return take(world::canonical_transform({vector(value.at("position")),{q[0],q[1],q[2],q[3]},vector(value.at("scale"))}));
}
Json body_properties(const physics::BodySettings& body) {
    constexpr std::array shapes{"box","sphere","triangle_mesh","capsule"};
    constexpr std::array motions{"stationary","kinematic","dynamic"};take(world::validate_body_settings(body));
    return {{"enabled",body.enabled},{"shape",shapes[static_cast<std::size_t>(body.shape)]},{"motion",motions[static_cast<std::size_t>(body.motion)]},
        {"half_extents",{{"x",body.half_extents.x},{"y",body.half_extents.y},{"z",body.half_extents.z}}},{"radius",body.radius},
        {"capsule_half_height",body.capsule_half_height},{"category",body.category},{"ignore_categories",body.ignore_categories},
        {"overlap_categories",body.overlap_categories},{"sensor",body.sensor},{"material",{{"friction",body.material.friction},{"restitution",body.material.restitution}}}};
}
std::string legacy_type(std::string name) {
    if(name=="world.Actor") return "world.Entity";
    if(name=="world.StaticMeshActor") return "world.StaticMeshEntity";
    return name;
}
// Migration transforms descriptions before creating any Runtime objects.
struct LegacyActorSelection {std::uint64_t entity{};std::vector<std::pair<std::uint64_t,std::uint64_t>> components;};
Json migrate_scene(Json source,std::vector<LegacyActorSelection>* selections=nullptr) {
    const auto version=json_format::identity(source.at("version"));
    if(version==6) return source;
    if(version<1 || version>5) throw std::runtime_error("Unsupported scene format version.");
    fields(source,{"version","actors","world"});
    if(!source.contains("actors")) throw std::runtime_error("Legacy scene requires an Actor directory.");
    if(version<4 && source.contains("world")) throw std::runtime_error("World settings require scene version 4.");
    const auto& old=source.at("actors");
    if(!old.is_array() || old.size()>Session::maximum_entities) throw std::runtime_error("Invalid legacy Actor count.");
    std::uint64_t next=1;Json roots=Json::array(),entities=Json::array();std::vector<std::string> actor_ids;
    for(std::size_t i=0;i<old.size();++i) actor_ids.push_back(std::to_string(next++));
    const std::unordered_set<std::string> spatial_types{"world.SceneComponent","world.PrimitiveComponent","world.StaticMeshComponent","world.CollisionComponent","world.LightComponent","physics.RagdollPartComponent","physics.VehicleWheelComponent","physics.SceneQueryComponent"};
    for(std::size_t a=0;a<old.size();++a) {
        auto actor=old[a];fields(actor,{"type","name","enabled","root","components","properties"});
        if(version==1 && actor.contains("properties")) throw std::runtime_error("Actor business properties require scene version 2.");
        auto parts=actor.at("components");if(!parts.is_array() || parts.size()>Session::maximum_components) throw std::runtime_error("Invalid legacy Component count.");
        std::vector<std::size_t> original_parts;
        std::unordered_set<std::size_t> original_ids;
        for(const auto& part:parts) {
            const auto key=json_format::identity(part.at("id"));
            if(!key || !original_ids.insert(key).second) throw std::runtime_error("Duplicate legacy Component identity.");
            if(selections) original_parts.push_back(key);
        }
        std::unordered_map<std::size_t,std::pair<std::uint64_t,std::uint64_t>> migrated_parts;
        std::optional<std::size_t> old_body;
        const auto root_key=json_format::identity(actor.at("root"));
        auto body=std::ranges::find_if(parts,[](const auto& part){return part.at("type")=="physics.BodyComponent";});
        if(body!=parts.end()) {
            old_body=json_format::identity(body->at("id"));
            if(std::ranges::count_if(parts,[](const auto& part){return part.at("type")=="physics.BodyComponent";})!=1) throw std::runtime_error("Legacy multiple BodyComponents require explicit migration.");
            fields(*body,{"id","type","enabled","physics","body","properties"});
            if(body->contains("properties") && !body->at("properties").empty()) throw std::runtime_error("Legacy BodyComponent has unsupported business properties.");
            auto spatial=std::ranges::find_if(parts,[&](const auto& part){return json_format::identity(part.at("id"))==root_key;});
            if(spatial==parts.end()) throw std::runtime_error("Legacy BodyComponent has no spatial root.");
            if(spatial->at("type")=="world.SceneComponent") (*spatial)["type"]="world.CollisionComponent";
            const auto name=spatial->at("type").get<std::string>();
            if(name!="world.CollisionComponent" && name!="world.StaticMeshComponent" && name!="world.PrimitiveComponent") throw std::runtime_error("Legacy physical root type requires explicit migration: "+name);
            if(spatial->contains("body") || spatial->contains("physics") || spatial->value("properties",Json::object()).contains("body")) throw std::runtime_error("Legacy root has competing body definitions.");
            (*spatial)[version==1?"physics":"body"]=body->at(version==1?"physics":"body");
            (*spatial)[version==1?"physics":"body"]["enabled"]=body->at("enabled");parts.erase(body);
        }
        Json base={{"id",actor_ids[a]},{"type",legacy_type(actor.at("type").get<std::string>())},{"name",actor.at("name")},{"enabled",actor.at("enabled")},
            {"space",nullptr},{"children",Json::array()},{"components",Json::array()},{"properties",actor.value("properties",Json::object())}};
        std::unordered_map<std::size_t,std::size_t> positions;std::unordered_map<std::size_t,std::string> spaces;std::unordered_map<std::size_t,std::size_t> parents;
        std::vector<Json> nodes{base};std::unordered_map<std::string,std::size_t> node_positions{{actor_ids[a],0}};
        for(std::size_t i=0;i<parts.size();++i) {
            const auto& part=parts[i];const auto key=json_format::identity(part.at("id"));
            if(!key || !positions.emplace(key,i).second) throw std::runtime_error("Duplicate legacy Component identity.");
            const auto name=part.at("type").get<std::string>();
            if(part.contains("position") || part.contains("rotation") || part.contains("scale") || spatial_types.contains(name)) {
                if(!spatial_types.contains(name)) throw std::runtime_error("Legacy spatial type "+name+" at Actor "+std::to_string(a+1)+", Component "+std::to_string(key)+" requires an explicit owner/space migration; fields cannot infer behavior intent.");
                spaces.emplace(key,key==root_key?actor_ids[a]:std::to_string(next++));parents.emplace(key,json_format::identity(part.at("parent")));
            }
        }
        if(!spaces.empty() && (!spaces.contains(root_key) || parents.at(root_key)!=0)) throw std::runtime_error("Legacy spatial tree requires an explicit unparented root.");
        if(spaces.empty() && root_key!=0) throw std::runtime_error("Legacy root is not spatial.");
        for(const auto& part:parts) {
            const auto key=json_format::identity(part.at("id"));if(!spaces.contains(key)) continue;const auto& id=spaces.at(key);
            auto node=key;std::unordered_set<std::size_t> seen;
            while(node!=root_key) {if(!seen.insert(node).second || !parents.contains(node) || parents.at(node)==0) throw std::runtime_error("Legacy spatial hierarchy is cyclic or outside its Actor.");node=parents.at(node);}
            auto space=Json{{"position",part.at("position")},{"rotation",part.at("rotation")},{"scale",part.at("scale")},{"origin",nullptr}};
            if(key==root_key) nodes[0]["space"]=std::move(space);
            else {
                const auto name=part.at("type").get<std::string>();const auto label=name.substr(name.find_last_of('.')+1)+" "+std::to_string(key);
                // The old spatial parent carried no enable gate for its peers or
                // descendants. Only the capability retains its own enabled bit.
                node_positions.emplace(id,nodes.size());nodes.push_back({{"id",id},{"type","world.Entity"},{"name",label},{"enabled",true},
                    {"space",std::move(space)},{"children",Json::array()},{"components",Json::array()},{"properties",Json::object()}});
            }
        }
        for(const auto& part:parts) {
            const auto key=json_format::identity(part.at("id"));if(!spaces.contains(key) || key==root_key) continue;
            nodes[node_positions.at(spaces.at(parents.at(key)))]["children"].push_back(spaces.at(key));
        }
        Json ignored=Json::array();std::unordered_set<std::string> ignored_ids;
        for(const auto& part:parts) {
            const auto type=part.at("type").get<std::string>();
            if(type=="world.PrimitiveComponent" || type=="world.StaticMeshComponent" || type=="world.CollisionComponent" ||
                type=="physics.RagdollPartComponent" || type=="physics.VehicleWheelComponent") {
                const auto id=spaces.at(json_format::identity(part.at("id")));
                if(ignored_ids.insert(id).second) ignored.push_back(ref_key(std::stoull(id)));
            }
        }
        Json ragdoll=Json::array(),wheels=Json::array();
        for(auto part:parts) {
            const auto key=json_format::identity(part.at("id"));const auto name=part.at("type").get<std::string>();
            fields(part,{"id","type","enabled","properties","position","rotation","scale","parent","mesh","base_color","roughness","metallic","emissive","materials","physics","body","constraint_target"});
            auto props=part.value("properties",Json::object());
            if(version==1 && part.contains("properties")) throw std::runtime_error("Component business properties require scene version 2.");
            if(name=="physics.SceneQueryComponent" && props.value("ignore_self",true)) {
                if(props.contains("ignored_entities")) throw std::runtime_error("Legacy query has competing self-ignore scopes.");
                props["ignored_entities"]=ignored;
            }
            if(part.contains("body") || part.contains("physics")) {
                if(props.contains("body")) throw std::runtime_error("Legacy body has competing definitions.");
                props["body"]=body_properties(body_settings(part.at(version==1?"physics":"body"),version==1));
            }
            if(name=="physics.DistanceConstraintComponent") {
                if(props.contains("target")) throw std::runtime_error("Legacy constraint has competing targets.");
                const auto target=json_format::identity(part.at("constraint_target"));
                if(target>actor_ids.size()) throw std::runtime_error("Legacy constraint target is outside the scene.");
                props["target"]=target?ref_key(std::stoull(actor_ids[target-1])):Json(nullptr);
            } else if(part.contains("constraint_target")) throw std::runtime_error("Constraint target requires a DistanceConstraintComponent.");
            if(name=="world.SceneComponent") {
                if(!props.empty()) throw std::runtime_error("Legacy SceneComponent has unsupported business fields.");
                const auto entity=std::stoull(spaces.at(key));migrated_parts.emplace(key,std::pair{entity,entity});continue;
            }
            const auto owner=spaces.contains(key)?spaces.at(key):actor_ids[a];
            const bool special=name=="physics.RagdollPartComponent" || name=="physics.VehicleWheelComponent";
            Json payload={{"id",std::to_string(next++)},{"type",name},{"name",""},{"enabled",part.at("enabled")},{"properties",props}};
            migrated_parts.emplace(key,std::pair<std::uint64_t,std::uint64_t>{std::stoull(owner),std::stoull(payload.at("id").get<std::string>())});
            if(name=="world.StaticMeshComponent" || special) {
                for(const auto field:{"mesh","base_color","roughness","metallic","emissive","materials"}) if(part.contains(field)) payload[field]=part.at(field);
                if(!payload.contains("materials")) payload["materials"]=Json::array();
            } else for(const auto field:{"mesh","base_color","roughness","metallic","emissive","materials"})
                if(part.contains(field)) throw std::runtime_error("Legacy mesh payload requires an exact builtin mesh type: "+name);
            if(special) {
                if(key==root_key || parents.at(key)!=root_key) throw std::runtime_error("Legacy physics part must directly descend from its controller root.");
                auto metadata=payload;metadata["properties"].erase("body");for(const auto field:{"mesh","base_color","roughness","metallic","emissive","materials"}) metadata.erase(field);
                auto mesh=payload;mesh["id"]=std::to_string(next++);mesh["type"]="world.StaticMeshComponent";mesh["properties"]=Json::object();
                if(props.contains("body")) {mesh["properties"]["body"]=props.at("body");if(props.at("body").value("enabled",false)) throw std::runtime_error("Legacy ragdoll/wheel Primitive conflicts with its pose driver.");}
                else mesh["properties"]["body"]={{"enabled",false}};
                nodes[node_positions.at(owner)]["components"].push_back(std::move(metadata));nodes[node_positions.at(owner)]["components"].push_back(std::move(mesh));
                (name=="physics.RagdollPartComponent"?ragdoll:wheels).push_back(ref_key(std::stoull(owner)));
            } else nodes[node_positions.at(owner)]["components"].push_back(std::move(payload));
        }
        unsigned ragdoll_drivers{},vehicle_drivers{};
        for(auto& component:nodes[0]["components"]) {
            if(component.at("type")=="physics.RagdollComponent") {component["properties"]["parts"]=ragdoll;++ragdoll_drivers;}
            if(component.at("type")=="physics.VehicleComponent") {component["properties"]["wheels"]=wheels;++vehicle_drivers;}
        }
        if(ragdoll_drivers>1 || vehicle_drivers>1 || (!ragdoll.empty() && ragdoll_drivers!=1) || (!wheels.empty() && vehicle_drivers!=1)) throw std::runtime_error("Legacy physics parts require one identifiable controller.");
        if(selections) {
            LegacyActorSelection selection;selection.entity=std::stoull(actor_ids[a]);selection.components.reserve(original_parts.size());
            for(const auto key:original_parts) selection.components.push_back(migrated_parts.at(old_body && key==*old_body?root_key:key));
            selections->push_back(std::move(selection));
        }
        roots.push_back(actor_ids[a]);for(auto& node:nodes) entities.push_back(std::move(node));
    }
    Json lighting=source.value("world",Json::object());if(version==4) lighting=world_settings(world_settings(lighting,true));
    return {{"version",6},{"next_id",std::to_string(next)},{"world",std::move(lighting)},{"roots",std::move(roots)},{"entities",std::move(entities)}};
}
}


struct Session::GraphCodec {
    struct MeshCopy {world::StaticMeshState state;std::vector<world::MaterialBinding> bindings;};
    struct Graph {
        Json data;
        std::unordered_map<std::string,MeshCopy> meshes;
        std::unordered_map<std::string,physics::BodySettings> bodies;
        std::unordered_map<std::string,ObjectRoot> materials;
        std::unordered_map<std::string,ObjectRoot> external;
        std::unordered_map<std::string,ObjectId> parents;
    };
    struct Prepared {
        Session* session{};
        std::vector<ObjectRoot> objects;
        std::vector<ObjectId> roots;
        std::vector<world::SpaceReference> spaces;
        std::vector<ObjectId> triangle_meshes;
        std::vector<ObjectRoot> resources;
        Project* project{};
        std::unordered_map<ObjectId,std::wstring> labels;
        std::unordered_set<std::string> material_keys,texture_keys;
        std::unordered_map<ObjectId,std::uint64_t> identities_before;
        std::uint64_t next_before{};
        bool published{};
        explicit Prepared(Session& owner):session(&owner),identities_before(owner.local_ids_),next_before(owner.next_local_id_) {
            for(const auto& [key,value]:owner.materials_) {static_cast<void>(value);material_keys.insert(key);}
            for(const auto& [key,value]:owner.textures_) {static_cast<void>(value);texture_keys.insert(key);}
        }
        Prepared(const Prepared&)=delete;
        Prepared& operator=(const Prepared&)=delete;
        Prepared(Prepared&& other) noexcept:session(std::exchange(other.session,nullptr)),objects(std::move(other.objects)),
            roots(std::move(other.roots)),spaces(std::move(other.spaces)),triangle_meshes(std::move(other.triangle_meshes)),resources(std::move(other.resources)),
            project(other.project),labels(std::move(other.labels)),material_keys(std::move(other.material_keys)),texture_keys(std::move(other.texture_keys)),
            identities_before(std::move(other.identities_before)),
            next_before(other.next_before),published(other.published) {}
        ~Prepared() {
            if(!session || published) return;
            for(const auto& object:objects) {
                session->names_.erase(object.id());session->local_ids_.erase(object.id());
                auto type=session->runtime_.type_of(object.id());if(!type) continue;
                auto entity=session->runtime_.is_a(*type,session->types_.entity);
                if(entity && *entity) (void)world::destroy_entity(session->runtime_,object.id());
            }
            for(const auto& object:objects) {
                if(session->runtime_.type_of(object.id())) (void)world::destroy_component(session->runtime_,object.id());
            }
            std::erase_if(session->materials_,[&](const auto& entry){return !material_keys.contains(entry.first);});
            std::erase_if(session->textures_,[&](const auto& entry){return !texture_keys.contains(entry.first);});
            // Unpublished objects never acquired a document identity visible to
            // history or authoring. Failed construction must leave metadata intact.
            session->next_local_id_=next_before;
            session->local_ids_.swap(identities_before);
        }
    };
    static std::uint64_t identity(const Session& session,ObjectId object) {
        if(auto at=session.local_ids_.find(object);at!=session.local_ids_.end()) return at->second;
        if(session.next_local_id_==std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Document object identity space is exhausted.");
        const auto id=session.next_local_id_++;session.local_ids_.emplace(object,id);return id;
    }
    static Graph capture(const Session& session,std::span<const ObjectId> requested,bool persistent) {
        struct IdentityRollback {
            const Session& session;
            std::unordered_map<ObjectId,std::uint64_t> before;
            std::uint64_t next;
            bool complete{};
            ~IdentityRollback() {if(!complete) {session.local_ids_.swap(before);session.next_local_id_=next;}}
        } identities{session,session.local_ids_,session.next_local_id_};
        auto& runtime=session.world_.runtime();Graph graph;std::vector<ObjectId> roots,nodes;
        std::unordered_set<ObjectId> members;
        for(const auto& member:take(session.world_.members())) members.insert(member.id());
        std::erase_if(session.local_ids_,[&](const auto& entry){return !runtime.type_of(entry.first);});
        std::unordered_set<ObjectId> selected(requested.begin(),requested.end()),seen,root_seen;
        root_seen.reserve(requested.size());
        for(const auto object:requested) {
            const auto state=take(world::inspect_entity(runtime,object));
            if((!persistent && !state.can_edit) || !session.names_.contains(object) || !members.contains(object))
                throw std::runtime_error("The Entity is not editable in this document.");
            bool covered{};
            for(auto parent=take(world::parent_entity(runtime,object));parent!=ObjectId{};parent=take(world::parent_entity(runtime,parent)))
                if(selected.contains(parent)) {covered=true;break;}
            if(!covered && root_seen.insert(object).second) roots.push_back(object);
        }
        const auto walk=[&](auto&& self,ObjectId object)->void {
            if(!seen.insert(object).second) throw std::runtime_error("Entity graph contains a repeated identity.");
            nodes.push_back(object);if(nodes.size()>maximum_entities) throw std::runtime_error("Scene exceeds the Entity limit.");
            for(const auto& child:take(world::children(runtime,object))) self(self,child.id());
        };
        for(auto root:roots) walk(walk,root);
        std::unordered_map<ObjectId,std::uint64_t> ids;
        for(auto object:nodes) {
            ids.emplace(object,identity(session,object));const auto parts=take(world::components(runtime,object));
            if(parts.size()>maximum_components) throw std::runtime_error("Scene exceeds the Component limit.");
            for(const auto& part:parts) if(!ids.emplace(part.id(),identity(session,part.id())).second) throw std::runtime_error("Component identity appears twice.");
        }
        Json material_values=Json::array();
        const auto archive_material=[&](ObjectId object)->std::uint64_t {
            if(auto at=ids.find(object);at!=ids.end()) return at->second;
            const auto state=take(material::read_material(runtime,object));
            const auto key=identity(session,object);ids.emplace(object,key);
            graph.materials.emplace(std::to_string(key),take(runtime.hold(object)));
            Json value={{"id",std::to_string(key)},{"source",state.source}};
            if(state.source.empty() && persistent) {
                value["inline"]=material_format::encode(take(session.material_asset(object)));
                if(state.parent.registered()) {
                    const auto parent_source=take(material::read_material(runtime,state.parent.id())).source;
                    if(parent_source.empty()) throw std::runtime_error("Save the parent material asset before archiving its instance.");
                    value["parent"]=parent_source;auto overrides=Json::array();
                    for(std::size_t i=0;i<material::parameter_names.size();++i) if(state.overrides&(1u<<i)) overrides.push_back(material::parameter_names[i]);
                    value["overrides"]=std::move(overrides);
                }
            }
            material_values.push_back(std::move(value));return key;
        };
        for(auto object:nodes) for(const auto& part:take(world::components(runtime,object)))
            if(derives(runtime,take(runtime.type_of(part.id())),session.types_.static_mesh_component))
                for(const auto& binding:take(world::read_material_bindings(runtime,part.id()))) (void)archive_material(binding.resource.id());
        const auto reference=[&](ObjectId object)->Json {
            if(object==ObjectId{}) return nullptr;take(runtime.type_of(object));
            if(auto at=ids.find(object);at!=ids.end()) return ref_key(at->second);
            if(persistent) throw std::runtime_error("Object reference leaves the captured graph; supply an explicit binding or include its target.");
            const auto key=std::to_string(identity(session,object));
            if(!graph.external.contains(key)) graph.external.emplace(key,take(runtime.hold(object)));
            return Json{{"$external",key}};
        };
        const auto properties=[&](ObjectId object,TypeId type) {
            Json result=Json::object();
            for(const auto& field:business_fields(runtime,session.types_,type)) {
                Json value;
                if(field.form==ValueForm::inline_value) value=encode_scalar(runtime,take(runtime.read(object,field.path)));
                else if(field.form==ValueForm::strong_sequence) {
                    value=Json::array();const auto locator=take(runtime.reference_field(object,field.path));const auto count=take(runtime.sequence_size(locator));
                    for(std::size_t i=0;i<count;++i) value.push_back(reference(take(runtime.sequence_read(locator,i)).id()));
                } else value=reference(take(runtime.read_reference(object,field.path)).id());
                auto* at=&result;for(const auto& name:field.names) at=&(*at)[name];*at=std::move(value);
            }
            return result;
        };
        Json values=Json::array(),root_keys=Json::array();
        for(auto object:roots) {
            const auto key=std::to_string(ids.at(object));root_keys.push_back(key);graph.parents.emplace(key,take(world::parent_entity(runtime,object)));
        }
        for(auto object:nodes) {
            const auto status=take(world::inspect_entity(runtime,object));Json space=nullptr;
            if(take(world::has_transform(runtime,object))) {
                space=trs_json(take(world::read_transform(runtime,object)));const auto origin=take(world::space_reference(runtime,object));
                if(origin) {
                    if(*origin==ObjectId{}) space["origin"]="0";
                    else {const auto ref=reference(*origin);space["origin"]=ref.contains("$ref")?ref.at("$ref"):ref;}
                }
            }
            Json children=Json::array(),parts=Json::array();
            for(const auto& child:take(world::children(runtime,object))) children.push_back(std::to_string(ids.at(child.id())));
            for(const auto& part:take(world::components(runtime,object))) {
                const auto state=take(world::inspect_component(runtime,part.id()));const auto key=std::to_string(ids.at(part.id()));
                if(derives(runtime,state.type,session.types_.light_component)) take(world::validate_light(take(world::read_light(runtime,part.id()))));
                if(physical_type(runtime,state.type)) {
                    auto body=take(physics::read_settings(runtime,part.id()));take(world::validate_body_settings(body));
                    graph.bodies.emplace(key,std::move(body));
                }
                Json value={{"id",key},{"type",type_name(runtime,state.type)},{"name",utf8(session.name(part.id()))},{"enabled",state.enabled},{"properties",properties(part.id(),state.type)}};
                if(derives(runtime,state.type,session.types_.static_mesh_component)) {
                    auto mesh=take(world::read_static_mesh(runtime,part.id()));take(world::validate_material(mesh.material));
                    if(persistent && mesh.mesh.resolved && mesh.mesh.source.empty()) throw std::runtime_error("A procedural mesh has no persistent source; save/import it before archiving.");
                    auto bindings=take(world::read_material_bindings(runtime,part.id()));graph.meshes.emplace(key,MeshCopy{mesh,bindings});
                    value["mesh"]=mesh.mesh.source;value["base_color"]=vector(mesh.material.base_color);value["roughness"]=mesh.material.roughness;
                    value["metallic"]=mesh.material.metallic;value["emissive"]=vector(mesh.material.emissive);auto entries=Json::array();
                    for(const auto& binding:bindings) {
                        Json entry={{"slot",binding.slot},{"resource",std::to_string(ids.at(binding.resource.id()))},{"overridden",binding.overridden}};
                        entries.push_back(std::move(entry));
                    }
                    value["materials"]=std::move(entries);
                }
                parts.push_back(std::move(value));
            }
            const auto label=session.name(object);
            const auto name=label.empty()?type_name(runtime,status.type)+" "+std::to_string(ids.at(object)):utf8(label);
            values.push_back({{"id",std::to_string(ids.at(object))},{"type",type_name(runtime,status.type)},{"name",name},{"enabled",status.enabled},
                {"space",std::move(space)},{"children",std::move(children)},{"components",std::move(parts)},{"properties",properties(object,status.type)}});
        }
        graph.data={{"version",6},{"next_id",std::to_string(session.next_local_id_)},{"world",world_settings(session.world_.lighting_settings())},
            {"roots",std::move(root_keys)},{"entities",std::move(values)},{"materials",std::move(material_values)}};
        identities.complete=true;
        return graph;
    }

    static Prepared construct(Session& session,const Graph& graph,Project* project,bool keep_ids) {
        const auto& data=graph.data;fields(data,{"version","next_id","world","roots","entities","materials"});
        if(json_format::identity(data.at("version"))!=6) throw std::runtime_error("Object graph requires scene format version 6.");
        const auto next=local_key(data.at("next_id"));const auto& entities=data.at("entities");const auto& root_values=data.at("roots");
        if(!entities.is_array() || entities.size()>maximum_entities || !root_values.is_array() || root_values.size()>entities.size())
            throw std::runtime_error("Invalid Entity graph directory.");
        if(!keep_ids && take(session.world_.members()).size()+entities.size()>maximum_entities) throw std::runtime_error("Editor Entity limit reached.");
        Prepared result{session};result.project=project;auto& runtime=session.runtime_;auto& world=session.world_;
        std::unordered_map<std::uint64_t,ObjectId> ids;std::unordered_map<std::uint64_t,const Json*> descriptors;
        std::unordered_set<std::uint64_t> entity_ids;std::uint64_t largest{};
        const auto create=[&](const Json& value,bool entity) {
            const auto key=local_key(value.at("id"));
            if(ids.contains(key)) throw std::runtime_error("Duplicate graph object identity: "+std::to_string(key));
            largest=std::max(largest,key);const auto name=value.at("type").get<std::string>();const auto type=take(runtime.find_type(name));
            auto root=entity?take(world::create_entity(runtime,type)):take(world::create_component(runtime,type));
            const auto object=root.id();ids.emplace(key,object);descriptors.emplace(key,&value);result.objects.push_back(std::move(root));
            if(keep_ids) session.local_ids_.emplace(object,key);else (void)identity(session,object);
            const auto label=wide(value.value("name",std::string{}));
            if(entity || !label.empty()) {take(validate_name(label));result.labels.emplace(object,label);}
            if(entity) entity_ids.insert(key);
        };
        for(const auto& entity:entities) {
            fields(entity,{"id","type","name","enabled","space","children","components","properties"});create(entity,true);
            const auto& parts=entity.at("components");
            if(!parts.is_array() || parts.size()>maximum_components) throw std::runtime_error("Invalid Component count.");
            for(const auto& part:parts) {
                fields(part,{"id","type","name","enabled","properties","mesh","base_color","roughness","metallic","emissive","materials"});create(part,false);
            }
        }
        const auto restore_material=[&](const Json& entry)->ObjectRoot {
            if(!project) throw std::runtime_error("Material restoration requires a Project.");
            const auto source=entry.at("source").get<std::string>();
            if(!source.empty() && entry.contains("inline")) throw std::runtime_error("Material binding has competing sources.");
            if(entry.contains("parent")) {
                if(!source.empty()) throw std::runtime_error("A material instance cannot own an asset identity.");
                auto parent=take(session.material_resource(*project,entry.at("parent").get<std::string>()));
                auto state=take(material::read_material(runtime,parent.id()));state.source.clear();state.parent=parent;state.overrides=0;
                state.description=material_format::decode(entry.at("inline")).description;
                const auto& names=entry.at("overrides");
                if(!names.is_array() || names.size()>material::parameter_names.size()) throw std::runtime_error("Invalid material overrides.");
                for(const auto& name:names) {
                    const auto at=std::ranges::find(material::parameter_names,name.get<std::string>());
                    if(at==material::parameter_names.end()) throw std::runtime_error("Unknown material override.");
                    const auto bit=1u<<static_cast<unsigned>(at-material::parameter_names.begin());
                    if(state.overrides&bit) throw std::runtime_error("Duplicate material override.");state.overrides|=bit;
                }
                return take(material::create_material(runtime,state));
            }
            if(entry.contains("overrides")) throw std::runtime_error("Overrides require a parent material.");
            return source.empty()?take(session.material_resource(*project,material_format::decode(entry.at("inline")))):
                take(session.material_resource(*project,source));
        };
        std::unordered_map<std::uint64_t,ObjectRoot> material_ids;
        std::unordered_set<ObjectId> material_objects;
        std::unordered_map<ObjectId,ObjectRoot> copied_materials;
        std::unordered_map<ObjectId,std::uint64_t> copied_material_keys;
        std::unordered_set<std::uint64_t> used_materials;
        const auto definitions=data.value("materials",Json::array());
        if(!definitions.is_array() || definitions.size()>maximum_entities*maximum_components) throw std::runtime_error("Invalid scene material directory.");
        for(const auto& definition:definitions) {
            fields(definition,{"id","source","inline","parent","overrides"});
            const auto key=local_key(definition.at("id"));
            if(ids.contains(key)) throw std::runtime_error("Duplicate graph object identity: "+std::to_string(key));
            largest=std::max(largest,key);ObjectRoot resource;
            if(auto at=graph.materials.find(std::to_string(key));at!=graph.materials.end()) {
                const auto state=take(material::read_material(runtime,at->second.id()));
                resource=state.source.empty()?take(material::create_material(runtime,state)):at->second;
                copied_materials.emplace(at->second.id(),resource);
                copied_material_keys.emplace(at->second.id(),key);
            } else resource=restore_material(definition);
            if(!material_objects.insert(resource.id()).second) throw std::runtime_error("One material asset has multiple document identities.");
            ids.emplace(key,resource.id());material_ids.emplace(key,resource);result.resources.push_back(resource);
            if(keep_ids) {
                if(!session.local_ids_.emplace(resource.id(),key).second) throw std::runtime_error("One material asset has multiple document identities.");
            } else (void)identity(session,resource.id());
        }
        if(next<=largest) throw std::runtime_error("next_id must exceed every document object identity.");
        if(keep_ids) session.next_local_id_=next;
        std::unordered_map<std::uint64_t,std::uint64_t> owners;
        for(const auto& entity:entities) {
            const auto key=local_key(entity.at("id"));const auto object=ids.at(key);std::vector<world::Attachment> parts;
            for(const auto& part:entity.at("components")) parts.push_back({ids.at(local_key(part.at("id")))});
            if(!parts.empty()) committed(world.attach_components(object,parts));
            const auto& space=entity.at("space");
            if(!space.is_null()) {fields(space,{"position","rotation","scale","origin"});take(world.set_transform(object,json_trs(space)));}
            const auto& children=entity.at("children");
            if(!children.is_array() || children.size()>entities.size()) throw std::runtime_error("Invalid child directory.");
            for(const auto& child:children) {
                const auto id=local_key(child);
                if(!entity_ids.contains(id) || !owners.emplace(id,key).second) throw std::runtime_error("Entity child has a wrong type or multiple parents.");
            }
        }
        std::unordered_set<std::uint64_t> roots,visited;
        const auto visit=[&](auto&& self,std::uint64_t key)->void {
            if(!visited.insert(key).second) throw std::runtime_error("Entity composition contains a cycle or duplicate.");
            for(const auto& child:descriptors.at(key)->at("children")) self(self,local_key(child));
        };
        for(const auto& value:root_values) {
            const auto key=local_key(value);
            if(!entity_ids.contains(key) || owners.contains(key) || !roots.insert(key).second) throw std::runtime_error("Invalid graph root.");
            visit(visit,key);result.roots.push_back(ids.at(key));
        }
        if(visited.size()!=entity_ids.size()) throw std::runtime_error("Entity graph has unreachable nodes or a cycle.");
        // All identities exist before relations or business references are restored.
        for(const auto& entity:entities) for(const auto& child:entity.at("children"))
            committed(world.reparent(ids.at(local_key(child)),ids.at(local_key(entity.at("id"))),world::ReparentRule::keep_local));
        const auto resolve=[&](const Json& value,TypeId target,ValueForm form)->ObjectReference {
            if(value.is_null()) return take(runtime.null_reference(target,form==ValueForm::weak_reference?form:ValueForm::strong_reference));
            ObjectId object;
            if(value.is_object() && value.contains("$ref")) {
                fields(value,{"$ref"});const auto key=local_key(value.at("$ref"));
                if(!ids.contains(key)) throw std::runtime_error("Reference target is outside this graph: "+std::to_string(key));object=ids.at(key);
                if(material_ids.contains(key)) used_materials.insert(key);
            } else if(value.is_object() && value.contains("$external")) {
                fields(value,{"$external"});const auto key=value.at("$external").get<std::string>();const auto at=graph.external.find(key);
                if(at==graph.external.end()) throw std::runtime_error("External object references cannot be loaded from a file.");object=at->second.id();
            } else throw std::runtime_error("Object reference requires null or a $ref identity.");
            auto held=take(runtime.hold(object,target));return form==ValueForm::weak_reference?take(runtime.observe(held.reference())):held.reference();
        };
        for(const auto& entity:entities) if(!entity.at("space").is_null()) {
            const auto& origin=entity.at("space").at("origin");std::optional<ObjectId> target;
            if(!origin.is_null()) {
                if(origin.is_string()) {
                    const auto key=local_key(origin,true);
                    if(key && !entity_ids.contains(key)) throw std::runtime_error("Space reference must target an Entity in this graph.");
                    target=key?ids.at(key):ObjectId{};
                } else target=resolve(origin,session.types_.entity,ValueForm::weak_reference).id();
            }
            result.spaces.push_back({ids.at(local_key(entity.at("id"))),target});
        }
        const auto properties=[&](const Json& value,ObjectId object) {
            const auto type=take(runtime.type_of(object));const auto schema=business_fields(runtime,session.types_,type);
            const auto read=[&](auto&& self,const Json& record,std::vector<std::string> path)->void {
                if(!record.is_object()) throw std::runtime_error("Expected an author property record.");
                for(const auto& [name,item]:record.items()) {
                    auto next_path=path;next_path.push_back(name);
                    const auto field=std::ranges::find_if(schema,[&](const auto& f){return f.names==next_path;});
                    if(field==schema.end()) {
                        const bool prefix=std::ranges::any_of(schema,[&](const auto& f){return f.names.size()>next_path.size() && std::equal(next_path.begin(),next_path.end(),f.names.begin());});
                        if(!prefix) throw std::runtime_error("Unknown or unsupported author property on "+type_name(runtime,type)+": "+name);
                        self(self,item,std::move(next_path));continue;
                    }
                    if(field->form==ValueForm::inline_value)
                        take(runtime.write(object,field->path,decode_scalar(runtime,take(runtime.read(object,field->path)),item)));
                    else if(field->form==ValueForm::strong_sequence) {
                        if(!item.is_array() || item.size()>maximum_entities*maximum_components) throw std::runtime_error("Invalid object reference sequence.");
                        std::vector<ObjectReference> refs;refs.reserve(item.size());
                        for(const auto& ref:item) refs.push_back(resolve(ref,field->type,ValueForm::strong_reference));
                        const auto locator=take(runtime.reference_field(object,field->path));
                        const ReferenceChange change=SequenceSplice{locator,0,take(runtime.sequence_size(locator)),refs};
                        take(runtime.apply_reference_changes({&change,1}));
                    } else take(runtime.write_reference(object,field->path,resolve(item,field->type,field->form)));
                }
            };
            read(read,value.value("properties",Json::object()),{});committed(world.set_enabled(object,value.at("enabled").get<bool>()));
        };
        for(const auto& entity:entities) {
            properties(entity,ids.at(local_key(entity.at("id"))));
            for(const auto& part:entity.at("components")) {
                const auto object=ids.at(local_key(part.at("id")));const auto type=take(runtime.type_of(object));properties(part,object);
                if(derives(runtime,type,session.types_.static_mesh_component)) {
                    const auto key=part.at("id").get<std::string>();
                    if(auto at=graph.meshes.find(key);at!=graph.meshes.end()) {
                        const auto& copy=at->second;take(world::set_mesh_binding(runtime,object,copy.state.mesh));
                        take(world.set_material(object,copy.state.material));auto bindings=copy.bindings;
                        for(auto& binding:bindings) {
                            used_materials.insert(copied_material_keys.at(binding.resource.id()));
                            binding.resource=copied_materials.at(binding.resource.id());
                        }
                        take(world::set_material_bindings(runtime,object,bindings));
                    } else {
                        if(!project) throw std::runtime_error("Material and mesh restoration requires a Project.");
                        const auto source=part.at("mesh").get<std::string>();geometry::MeshRef mesh;
                        if(!source.empty()) mesh=take(project->mesh(source)).mesh;
                        take(world::set_mesh_binding(runtime,object,{source,std::move(mesh)}));
                        world::Material material;material.base_color=vector(part.at("base_color"));
                        if(part.contains("roughness")) material.roughness=std::get<float>(decode_scalar(runtime,Scalar{material.roughness},part.at("roughness")));
                        if(part.contains("metallic")) material.metallic=std::get<float>(decode_scalar(runtime,Scalar{material.metallic},part.at("metallic")));
                        if(part.contains("emissive")) material.emissive=vector(part.at("emissive"));take(world.set_material(object,material));
                        const auto entries=part.value("materials",Json::array());
                        if(!entries.is_array() || entries.size()>1024) throw std::runtime_error("Invalid material binding table.");
                        std::vector<world::MaterialBinding> bindings;
                        for(const auto& entry:entries) {
                            fields(entry,{"slot","resource","source","overridden","inline","parent","overrides"});
                            ObjectRoot resource;
                            if(entry.contains("resource")) {
                                fields(entry,{"slot","resource","overridden"});
                                const auto key=local_key(entry.at("resource"));
                                const auto at=material_ids.find(key);
                                if(at==material_ids.end()) throw std::runtime_error("Material binding references an unknown scene resource.");
                                used_materials.insert(key);resource=at->second;
                            } else resource=restore_material(entry);
                            bindings.push_back({entry.at("slot").get<std::string>(),std::move(resource),entry.at("overridden").get<bool>()});
                        }
                        take(world::set_material_bindings(runtime,object,bindings));
                    }
                } else for(const auto key:{"mesh","base_color","roughness","metallic","emissive","materials"})
                    if(part.contains(key)) throw std::runtime_error("Mesh payload belongs to a StaticMeshComponent.");
                if(derives(runtime,type,session.types_.light_component)) take(world::validate_light(take(world::read_light(runtime,object))));
                if(physical_type(runtime,type)) {
                    auto settings=take(physics::read_settings(runtime,object));take(world::validate_body_settings(settings));
                    if(auto at=graph.bodies.find(part.at("id").get<std::string>());at!=graph.bodies.end())
                        settings.cooked_shape=at->second.cooked_shape;
                    if(settings.enabled && settings.shape==physics::ShapeKind::triangle_mesh) {
                        const auto mesh=take(world::read_static_mesh(runtime,object));
                        if(mesh.mesh.source.empty()) {if(project) throw std::runtime_error("Triangle collision needs a persistent mesh source.");}
                        if(mesh.mesh.source.starts_with("project/")) result.triangle_meshes.push_back(object);
                    }
                    take(physics::configure(runtime,object,std::move(settings)));
                }
            }
        }
        for(const auto& object:result.objects) {
            const auto type=take(runtime.type_of(object.id()));
            if(derives(runtime,type,session.physics_types_.ragdoll_component))
                take(physics::set_ragdoll_parts(runtime,object.id(),take(physics::ragdoll_parts(runtime,object.id()))));
            if(derives(runtime,type,session.physics_types_.vehicle_component))
                take(physics::set_vehicle_wheels(runtime,object.id(),take(physics::vehicle_wheels(runtime,object.id()))));
            if(derives(runtime,type,session.physics_types_.scene_query_component))
                take(physics::set_query_ignored_entities(runtime,object.id(),take(physics::query_ignored_entities(runtime,object.id()))));
        }
        if(used_materials.size()!=material_ids.size()) throw std::runtime_error("Scene material directory contains an unreferenced resource.");
        return result;
    }

    static std::vector<world::EntityAttachment> attachments(const Prepared& prepared,const Graph& graph,ObjectId parent,bool duplicate) {
        std::vector<world::EntityAttachment> attachments;
        attachments.reserve(prepared.roots.size());
        for(std::size_t i=0;i<prepared.roots.size();++i) {
            auto target=parent;if(duplicate) target=graph.parents.at(graph.data.at("roots")[i].get<std::string>());
            attachments.push_back({prepared.roots[i],target,world::append});
        }
        return attachments;
    }
    static void prepare_cook(Session& session,Prepared& prepared,std::span<const world::EntityAttachment> attachments) {
        if(prepared.triangle_meshes.empty()) return;
        const auto matrices=take(session.world_.attachment_world_matrices(attachments,prepared.spaces));
        std::unordered_map<ObjectId,math::Matrix4> worlds;worlds.reserve(matrices.size());
        for(const auto& [object,matrix]:matrices) worlds.emplace(object,matrix);
        for(const auto object:prepared.triangle_meshes) {
            const auto mesh=take(world::read_static_mesh(session.runtime_,object));
            const auto owner=take(world::inspect_component(session.runtime_,object)).owner;
            const auto trs=take(world::relative_transform(worlds.at(owner),world::transform_matrix({})));
            auto settings=take(physics::read_settings(session.runtime_,object));
            if(prepared.project) settings.cooked_shape=take(prepared.project->collision(mesh.mesh.source,trs.scale));
            else if(settings.cooked_shape) take(physics::validate_cooked_triangle_mesh(*settings.cooked_shape,mesh.mesh.resolved,trs.scale,mesh.mesh.source));
            take(physics::configure(session.runtime_,object,std::move(settings)));
        }
    }
    static world::Commit publish(Session& session,Prepared& prepared,const Graph& graph,ObjectId parent,bool duplicate) {
        EntityChange history;history.added=true;history.removals.reserve(prepared.roots.size());
        for(auto root:prepared.roots) {world::EntityRemoval removal;removal.root=take(session.runtime_.hold(root));history.removals.push_back(std::move(removal));}
        auto plans=attachments(prepared,graph,parent,duplicate);prepare_cook(session,prepared,plans);
        auto selection=prepared.roots;
        session.history_.reserve(session.history_.size()+1);
        for(const auto& [object,label]:prepared.labels) session.names_.emplace(object,label);
        world::Commit result;
        try {
            result=take(session.world_.attach_batch(plans,prepared.spaces));
        } catch(...) {
            const auto state=world::inspect_entity(session.runtime_,prepared.roots.front());
            const bool attached=state && (state->state==world::EntityState::attached_inactive || state->state==world::EntityState::active);
            const bool retained=attached && !session.world_.detach_batch(prepared.roots);
            if(retained) {prepared.published=true;session.remember(std::move(history));}
            throw;
        }
        if(result.changed) {
            // Selection and history storage were prepared before World publication.
            // No admission, allocation or callback can fail between these commits.
            prepared.published=true;session.selected_entities_.swap(selection);
            session.selection_={prepared.roots.back(),prepared.roots.back()};++session.revision_;
            session.remember(std::move(history));
        }
        return result;
    }
};

Result<std::string> Session::serialize() const {
    try {
        if(editing()) throw std::runtime_error("Finish the active edit before saving.");std::vector<ObjectId> roots;
        for(const auto& member:take(world_.members()))
            if(take(world::parent_entity(world_.runtime(),member.id()))==ObjectId{}) roots.push_back(member.id());
        auto graph=GraphCodec::capture(*this,roots,true);auto text=graph.data.dump(2)+"\n";
        if(text.size()>json_format::maximum_bytes) throw std::runtime_error("Scene exceeds the 16 MiB file limit.");return text;
    } catch(const std::exception& error) {return failed(error);}
}
Result<std::string> Session::serialize_subtree(ObjectId root) const {
    try {
        if(editing()) throw std::runtime_error("Finish the active edit before saving a composition.");
        auto graph=GraphCodec::capture(*this,{&root,1},true);auto text=graph.data.dump(2)+"\n";
        if(text.size()>json_format::maximum_bytes) throw std::runtime_error("Composition exceeds the 16 MiB file limit.");return text;
    } catch(const std::exception& error) {return failed(error);}
}
Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text,Project& project,Prepare prepare) {
    try {
        GraphCodec::Graph graph;graph.data=migrate_scene(json_format::parse(text));auto session=std::make_unique<Session>(std::move(prepare));
        take(session->world_.set_lighting_settings(world_settings(graph.data.at("world"),false)));
        auto prepared=GraphCodec::construct(*session,graph,&project,true);
        for(const auto& [object,label]:prepared.labels) session->names_.emplace(object,label);
        if(!prepared.roots.empty()) {
            const auto plans=GraphCodec::attachments(prepared,graph,{},false);GraphCodec::prepare_cook(*session,prepared,plans);
            const auto commit=take(session->world_.attach_batch(plans,prepared.spaces));prepared.published=commit.changed;
            if(commit.notification_error) throw std::runtime_error(commit.notification_error->message);
        }
        prepared.published=true;
        session->next_name_=graph.data.at("entities").size()+1;return session;
    } catch(const std::exception& error) {return failed(error);}
}
Result<world::Commit> Session::instantiate_subtree(std::string_view text,Project& project,ObjectId parent) {
    try {
        if(editing() || world_.running()) throw std::runtime_error("Composition import requires a stopped document without an active edit.");
        if(parent!=ObjectId{}) take(admit_entity_edit(parent));
        GraphCodec::Graph graph;graph.data=migrate_scene(json_format::parse(text));
        auto prepared=GraphCodec::construct(*this,graph,&project,false);
        if(prepared.roots.empty()) return world::Commit{};return GraphCodec::publish(*this,prepared,graph,parent,false);
    } catch(const std::exception& error) {return failed(error);}
}
Result<world::Commit> Session::duplicate_selected() {
    try {
        if(editing()) throw std::runtime_error("Finish the active edit before duplicating.");if(selected_entities_.empty()) return world::Commit{};
        auto graph=GraphCodec::capture(*this,selected_entities_,false);auto prepared=GraphCodec::construct(*this,graph,nullptr,false);
        for(auto root:prepared.roots) {
            const auto original=prepared.labels.at(root);
            for(std::size_t suffix=1;;++suffix) {
                const auto tail=L" Copy"+(suffix==1?std::wstring{}:L" "+std::to_wstring(suffix));
                auto prefix=original.substr(0,256-tail.size());if(!prefix.empty() && prefix.back()>=0xd800 && prefix.back()<=0xdbff) prefix.pop_back();auto label=prefix+tail;
                const auto matches=[&](const auto& item){return item.first!=root && item.second==label;};
                if(std::ranges::none_of(names_,matches) && std::ranges::none_of(prepared.labels,matches)) {
                    take(validate_name(label));prepared.labels[root]=std::move(label);break;
                }
            }
        }
        return GraphCodec::publish(*this,prepared,graph,{},true);
    } catch(const std::exception& error) {return failed(error);}
}
Result<std::vector<std::pair<std::uint64_t,std::uint64_t>>> Session::legacy_selection(std::string_view text,
    std::span<const std::pair<std::size_t,std::optional<std::size_t>>> requests) {
    try {
        auto data=json_format::parse(text);
        if(json_format::identity(data.at("version"))>=6) throw std::runtime_error("Legacy selection requires scene version 1 through 5.");
        std::vector<LegacyActorSelection> selections;
        (void)migrate_scene(std::move(data),&selections);
        std::vector<std::pair<std::uint64_t,std::uint64_t>> result;result.reserve(requests.size());
        for(const auto& [actor,component]:requests) {
            if(actor>=selections.size()) throw std::runtime_error("Legacy selection Actor index is outside the scene.");
            const auto& selection=selections[actor];
            if(component) {
                if(*component>=selection.components.size()) throw std::runtime_error("Legacy inspection Component index is outside its Actor.");
                result.push_back(selection.components[*component]);
            } else result.emplace_back(selection.entity,selection.entity);
        }
        return result;
    } catch(const std::exception& error) {return failed(error);}
}
Result<ObjectId> Session::local_object(std::uint64_t identity) const {
    try {
        if(!identity) throw std::runtime_error("Document identity cannot be empty.");
        const auto at=std::ranges::find_if(local_ids_,[&](const auto& value){return value.second==identity;});
        if(at==local_ids_.end()) throw std::runtime_error("Unknown document object identity.");
        for(const auto& entity:take(world_.members())) {
            if(entity.id()==at->first) return at->first;
            for(const auto& component:take(world::components(world_.runtime(),entity.id()))) if(component.id()==at->first) return at->first;
        }
        throw std::runtime_error("Document identity does not belong to a live Entity or Component.");
    } catch(const std::exception& error) {return failed(error);}
}
}

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
#include <functional>
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
        const auto number=item.get<float>();
        if (!std::isfinite(number)) throw std::runtime_error(std::string("Physics field must be finite: ")+key);
        return number;
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
// A saved value that does not fit the current schema. Unlike other exceptions it
// does not reject the scene: the value is kept and the field keeps its default.
struct Mismatch : std::runtime_error {using std::runtime_error::runtime_error;};
Scalar decode_scalar(Runtime& runtime,const Scalar& prototype,const Json& value,bool* converted=nullptr) {
    return std::visit([&](const auto& original)->Scalar {
        using T=std::decay_t<decltype(original)>;
        if constexpr (std::same_as<T,bool>) {
            if (!value.is_boolean()) throw Mismatch("Expected a boolean property.");
            return value.get<bool>();
        } else if constexpr (std::same_as<T,EnumValue>) {
            if (!value.is_string()) throw Mismatch("Expected a declared enum member name.");
            const auto info=take(runtime.describe(original.type));
            for (const auto& [name,bits]:info->enumeration->values) if (value.get_ref<const std::string&>()==name) return EnumValue{original.type,bits};
            throw Mismatch("Unknown scene enum member.");
        } else if constexpr (std::is_integral_v<T>) {
            if (value.is_number_float()) {
                // Lossless only: an integral value written by an earlier float field.
                const auto number=value.get<double>();
                if (!std::isfinite(number) || std::trunc(number)!=number ||
                    number<static_cast<double>(std::numeric_limits<T>::min()) || number>static_cast<double>(std::numeric_limits<T>::max()))
                    throw Mismatch("Expected an integer property.");
                if (converted) *converted=true;
                return static_cast<T>(number);
            }
            if (!value.is_number_integer()) throw Mismatch("Expected an integer property.");
            if (value.is_number_unsigned()) {
                const auto number=value.get<std::uint64_t>();
                if (number>static_cast<std::uint64_t>(std::numeric_limits<T>::max())) throw Mismatch("Integer property is out of range.");
                return static_cast<T>(number);
            }
            const auto number=value.get<std::int64_t>();
            if (number<static_cast<std::int64_t>(std::numeric_limits<T>::min()) || number>static_cast<std::int64_t>(std::numeric_limits<T>::max()))
                throw Mismatch("Integer property is out of range.");
            return static_cast<T>(number);
        } else {
            if (!value.is_number()) throw Mismatch("Expected a numeric property.");
            const auto number=value.get<T>();
            if (!std::isfinite(number)) throw Mismatch("Scene property values must be finite and representable.");
            return number;
        }
    },prototype);
}
// Retained records are sparse scene JSON. On save a retained leaf replaces the
// emitted value at its path; retained records merge into emitted records.
void overlay(Json& target,const Json& retained) {
    if(!target.is_object() || !retained.is_object()) {target=retained;return;}
    for(const auto& [key,value]:retained.items()) {
        if(auto at=target.find(key);at!=target.end()) overlay(*at,value);
        else target[key]=value;
    }
}
// Removes the retained subtree at path, or a retained leaf on its prefix.
bool erase_path(Json& record,std::span<const std::string> path) {
    std::vector<Json*> trail{&record};
    for(std::size_t i=0;i<path.size();++i) {
        auto& node=*trail.back();
        const auto at=node.find(path[i]);if(at==node.end()) return false;
        if(i+1==path.size() || !at->is_object()) {
            node.erase(at);
            for(std::size_t up=trail.size()-1;up>0 && trail[up]->empty();--up) trail[up-1]->erase(path[up-1]);
            return true;
        }
        trail.push_back(&*at);
    }
    return false;
}
bool reference_value(const Json& value) {return value.is_object() && (value.contains("$ref") || value.contains("$external"));}
template<class F> void each_reference(Json& value,F&& visit) {
    if(reference_value(value)) {visit(value);return;}
    if(value.is_object() || value.is_array()) for(auto& item:value) each_reference(item,visit);
}
std::string preview(const Json& value) {
    auto text=value.dump(-1,' ',false,Json::error_handler_t::replace);
    if(text.size()>96) {
        std::size_t cut=93;while(cut && (static_cast<unsigned char>(text[cut])&0xC0)==0x80) --cut;
        text.resize(cut);text+="...";
    }
    return text;
}
std::string dotted(std::span<const std::string> path) {
    std::string text;
    for(const auto& name:path) {if(!text.empty()) text+='.';text+=name;}
    return text;
}
std::string dump_retained(const Json& value) {return value.empty()?std::string{}:value.dump();}

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
    constexpr std::array motions{"stationary","kinematic","dynamic"};
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
    // One load's diagnostics and the saved data it could not apply.
    struct Load {
        LoadPolicy policy{LoadPolicy::tolerant};
        bool document{}; // Loading a whole document, not importing into one.
        // Applying an editor graph to a Play copy: references outside the graph
        // name live objects, and targets the game destroyed are dropped.
        bool live{};
        std::vector<std::uint64_t> dropped;
        std::vector<world::Diagnostic> issues;
        std::unordered_map<ObjectId,Json> pending; // {"type","record","slots"} with file identities.
        std::unordered_map<ObjectId,ObjectId> owners;
        std::unordered_map<ObjectId,std::string> types;
        std::unordered_map<ObjectId,std::uint64_t> keys;
        Json top=Json::object();
        std::vector<ObjectRoot> unreferenced;
        std::unordered_map<ObjectId,std::vector<ObjectRoot>> materials; // Named by a kept binding table.
        std::unordered_map<ObjectId,Retained> retained;
        std::string subject(ObjectId object) const {
            if(object==ObjectId{}) return "Scene";
            std::string text=types.contains(object)?types.at(object):std::string("Object");
            if(const auto at=keys.find(object);at!=keys.end()) text+=" #"+std::to_string(at->second);
            return text;
        }
        void report(world::Severity severity,world::DiagnosticKind kind,ObjectId object,std::vector<std::string> field,const Json* value,const std::string& detail) {
            auto message=subject(object)+(field.empty()?std::string{}:" "+dotted(field))+": "+detail;
            if(policy==LoadPolicy::strict && severity!=world::Severity::info) throw std::runtime_error(message);
            world::Diagnostic item;
            item.serial=issues.size()+1;item.severity=severity;item.kind=kind;item.phase=world::DiagnosticPhase::load;item.object=object;
            if(object!=ObjectId{}) item.entity=owners.contains(object)?owners.at(object):object;
            if(const auto at=types.find(object);at!=types.end()) item.type=at->second;
            item.field=std::move(field);if(value) item.value=preview(*value);item.message=std::move(message);
            issues.push_back(std::move(item));
        }
        void store(ObjectId object,std::span<const std::string> path,const Json& value) {
            Json* node=object==ObjectId{}?&top:&pending[object]["record"];
            for(std::size_t i=0;i+1<path.size();++i) {if(!node->is_object()) *node=Json::object();node=&(*node)[path[i]];}
            if(!node->is_object()) *node=Json::object();
            (*node)[path.back()]=value;
        }
        void keep(ObjectId object,std::vector<std::string> path,const Json& value,world::DiagnosticKind kind,const std::string& detail) {
            report(world::Severity::warning,kind,object,path,&value,detail);
            store(object,path,value);
        }
        void keep_slot(ObjectId object,const std::string& slot,const std::string& key,const Json& value,world::DiagnosticKind kind,const std::string& detail) {
            report(world::Severity::warning,kind,object,{"materials",slot,key},&value,detail);
            pending[object]["slots"][slot][key]=value;
        }
        void extra(ObjectId object,const Json& value,std::initializer_list<std::string_view> allowed,std::vector<std::string> prefix={}) {
            for(const auto& [name,item]:value.items()) if(std::ranges::find(allowed,name)==allowed.end()) {
                auto path=prefix;path.push_back(name);
                keep(object,std::move(path),item,world::DiagnosticKind::unknown_key,"Unknown key kept from the saved scene.");
            }
        }
    };
    struct Prepared {
        Session* session{};
        std::vector<ObjectRoot> objects;
        std::vector<ObjectId> roots;
        std::vector<world::SpaceReference> spaces;
        std::vector<ObjectId> triangle_meshes;
        std::vector<ObjectRoot> resources;
        // Live graphs only: new components of an existing Entity, attached on publication.
        std::vector<std::pair<ObjectId,std::vector<world::Attachment>>> anchored;
        Project* project{};
        std::unordered_map<ObjectId,std::wstring> labels;
        std::unordered_set<std::string> material_keys,texture_keys;
        std::unordered_map<ObjectId,std::uint64_t> identities_before;
        std::unordered_map<std::string,MissingResource> unavailable_before;
        std::uint64_t next_before{};
        bool published{};
        explicit Prepared(Session& owner):session(&owner),identities_before(owner.local_ids_),unavailable_before(owner.unavailable_),next_before(owner.next_local_id_) {
            for(const auto& [key,value]:owner.materials_) {static_cast<void>(value);material_keys.insert(key);}
            for(const auto& [key,value]:owner.textures_) {static_cast<void>(value);texture_keys.insert(key);}
        }
        Prepared(const Prepared&)=delete;
        Prepared& operator=(const Prepared&)=delete;
        Prepared(Prepared&& other) noexcept:session(std::exchange(other.session,nullptr)),objects(std::move(other.objects)),
            roots(std::move(other.roots)),spaces(std::move(other.spaces)),triangle_meshes(std::move(other.triangle_meshes)),resources(std::move(other.resources)),
            anchored(std::move(other.anchored)),project(other.project),labels(std::move(other.labels)),material_keys(std::move(other.material_keys)),texture_keys(std::move(other.texture_keys)),
            identities_before(std::move(other.identities_before)),unavailable_before(std::move(other.unavailable_before)),
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
            session->unavailable_.swap(unavailable_before);
            // Unpublished objects never acquired a document identity visible to
            // history or authoring. Failed construction must leave metadata intact.
            session->next_local_id_=next_before;
            session->local_ids_.swap(identities_before);
        }
    };
    static std::uint64_t identity(const Session& session,ObjectId object) {
        if(auto at=session.local_ids_.find(object);at!=session.local_ids_.end()) return at->second;
        // The editor allocates every identity; Play objects without one use runtime handles.
        if(session.play_) throw std::runtime_error("A Play copy does not assign document identities.");
        if(session.next_local_id_==std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Document object identity space is exhausted.");
        const auto id=session.next_local_id_++;session.local_ids_.emplace(object,id);return id;
    }
    // Saved World settings apply option by option; data that does not fit is kept.
    static rendering::SceneLightingSettings load_world(const Json& data,Load& load) {
        renderer::ViewSettings candidate;
        const auto found=data.find("world");
        if(found==data.end()) {
            load.report(world::Severity::warning,world::DiagnosticKind::mismatch,{},{"world"},nullptr,"World settings are missing; defaults are used.");
            return candidate.lit;
        }
        const auto& source=*found;
        if(!source.is_object()) {
            load.keep({},{"world"},source,world::DiagnosticKind::mismatch,"Expected a World settings record. Defaults are used and the saved value is kept.");
            return candidate.lit;
        }
        for(const auto& [name,value]:source.items()) {
            const auto* option=view_option(name);
            if(!option || option->name!=name || !world_option(option->option)) {
                load.keep({},{"world",name},value,world::DiagnosticKind::unknown_key,"Unknown World setting kept from the saved scene.");
                continue;
            }
            try {
                double number{};
                if(option->kind==ViewValueKind::boolean) {
                    if(!value.is_boolean()) throw Mismatch("Expected a boolean World option.");
                    number=value.get<bool>()?1:0;
                } else if(option->kind==ViewValueKind::enumeration) {
                    if(!value.is_string()) throw Mismatch("Expected a World option name.");
                    const auto at=std::ranges::find(option->choices,value.get<std::string>());
                    if(at==option->choices.end()) throw Mismatch("Unknown World option value.");
                    number=static_cast<double>(at-option->choices.begin());
                } else {
                    if(!value.is_number()) throw Mismatch("Expected a numeric World option.");
                    number=value.get<double>();
                }
                if(auto set=set_view_option(candidate,option->option,number);!set) throw Mismatch(set.error().message);
            } catch(const Mismatch& error) {
                load.keep({},{"world",name},value,world::DiagnosticKind::mismatch,std::string(error.what())+" The default is used and the saved value is kept.");
            }
        }
        // Contradictory settings are not resolved by JSON iteration order.
        const auto flag=[&](const char* key,bool fallback) {
            const auto at=source.find(key);return at!=source.end() && at->is_boolean()?at->get<bool>():fallback;
        };
        const char* conflict=flag("gtao",true) && flag("ssil",false)?"GTAO and Local GI are mutually exclusive.":rendering::scene_lighting_error(candidate.lit);
        if(conflict) {
            load.keep({},{"world"},source,world::DiagnosticKind::mismatch,std::string(conflict)+" Defaults are used and the saved values are kept.");
            return renderer::ViewSettings{}.lit;
        }
        return candidate.lit;
    }
    static Graph capture(const Session& session,std::span<const ObjectId> requested,bool persistent,bool document=false,bool live=false) {
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
        const auto reference=[&](ObjectId object)->Json {
            if(object==ObjectId{}) return nullptr;take(runtime.type_of(object));
            if(auto at=ids.find(object);at!=ids.end()) return ref_key(at->second);
            if(live) return ref_key(identity(session,object));
            if(persistent) throw std::runtime_error("Object reference leaves the captured graph; supply an explicit binding or include its target.");
            const auto key=std::to_string(identity(session,object));
            if(!graph.external.contains(key)) graph.external.emplace(key,take(runtime.hold(object)));
            return Json{{"$external",key}};
        };
        // Retained data refers to document identities. A target that left the
        // document is written as null; a live target follows the ordinary rules.
        std::unordered_map<std::uint64_t,ObjectId> by_key;
        if(!session.retained_.empty() || !session.retained_document_.record.empty())
            for(const auto& [object,key]:session.local_ids_) by_key.emplace(key,object);
        const auto present=[&](ObjectId object) {
            if(!runtime.type_of(object)) return false;
            if(members.contains(object) || session.is_material(object)) return true;
            const auto component=world::inspect_component(runtime,object);
            return component && component->valid && members.contains(component->owner);
        };
        std::function<std::uint64_t(ObjectId)> archive_material;
        const auto restore_references=[&](Json value) {
            each_reference(value,[&](Json& node) {
                ObjectId object;
                if(const auto at=node.find("$ref");at!=node.end())
                    if(const auto found=by_key.find(local_key(*at));found!=by_key.end()) object=found->second;
                if(object==ObjectId{} || !present(object)) {node=nullptr;return;}
                if(session.is_material(object)) static_cast<void>(archive_material(object));
                node=reference(object);
            });
            return value;
        };
        const auto merge_retained=[&](Json& value,ObjectId object) {
            const auto at=session.retained_.find(object);if(at==session.retained_.end()) return;
            const auto& kept=at->second;
            for(const auto& material:kept.materials) if(runtime.type_of(material.id())) static_cast<void>(archive_material(material.id()));
            if(!kept.type.empty()) value["type"]=kept.type;
            if(!kept.record.empty()) overlay(value,restore_references(Json::parse(kept.record)));
            if(const auto entries=value.find("materials");!kept.slots.empty() && entries!=value.end() && entries->is_array()) {
                const auto slots=restore_references(Json::parse(kept.slots));
                for(auto& entry:*entries)
                    if(const auto extra=slots.find(entry.at("slot").get<std::string>());extra!=slots.end()) overlay(entry,*extra);
            }
        };
        archive_material=[&](ObjectId object)->std::uint64_t {
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
            merge_retained(value,object);
            material_values.push_back(std::move(value));return key;
        };
        for(auto object:nodes) for(const auto& part:take(world::components(runtime,object)))
            if(derives(runtime,take(runtime.type_of(part.id())),session.types_.static_mesh_component))
                for(const auto& binding:take(world::read_material_bindings(runtime,part.id()))) (void)archive_material(binding.resource.id());
        if(document) for(const auto& material:session.retained_document_.materials)
            if(runtime.type_of(material.id())) static_cast<void>(archive_material(material.id()));
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
                if(derives(runtime,state.type,session.types_.light_component)) take(world::validate_light(runtime,take(world::read_light(runtime,part.id()))));
                if(physical_type(runtime,state.type)) {
                    auto body=take(physics::read_settings(runtime,part.id()));take(world::validate_body_settings(runtime,body));
                    graph.bodies.emplace(key,std::move(body));
                }
                Json value={{"id",key},{"type",type_name(runtime,state.type)},{"name",utf8(session.name(part.id()))},{"enabled",state.enabled},{"properties",properties(part.id(),state.type)}};
                if(derives(runtime,state.type,session.types_.static_mesh_component)) {
                    auto mesh=take(world::read_static_mesh(runtime,part.id()));take(world::validate_material(runtime,mesh.material));
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
                merge_retained(value,part.id());
                parts.push_back(std::move(value));
            }
            const auto label=session.name(object);
            const auto name=label.empty()?type_name(runtime,status.type)+" "+std::to_string(ids.at(object)):utf8(label);
            Json value={{"id",std::to_string(ids.at(object))},{"type",type_name(runtime,status.type)},{"name",name},{"enabled",status.enabled},
                {"space",std::move(space)},{"children",std::move(children)},{"components",std::move(parts)},{"properties",properties(object,status.type)}};
            merge_retained(value,object);
            values.push_back(std::move(value));
        }
        graph.data={{"version",6},{"next_id",std::to_string(session.next_local_id_)},{"world",world_settings(session.world_.lighting_settings())},
            {"roots",std::move(root_keys)},{"entities",std::move(values)},{"materials",std::move(material_values)}};
        if(document && !session.retained_document_.record.empty())
            overlay(graph.data,restore_references(Json::parse(session.retained_document_.record)));
        identities.complete=true;
        return graph;
    }

    using Resolve=std::function<std::optional<ObjectReference>(const Json&,TypeId,ValueForm)>;
    using WriteScalar=std::function<Result<void>(ObjectId,std::span<const FieldId>,Scalar)>;
    // Scene and Play changes decode a properties record leaf by leaf through this one path.
    static void read_properties(Session& session,ObjectId object,const Json& record,Load& load,const Resolve& resolve,const WriteScalar& write) {
        auto& runtime=session.runtime_;
        const auto type=take(runtime.type_of(object));const auto schema=business_fields(runtime,session.types_,type);
        const auto read=[&](auto&& self,const Json& value,const std::vector<std::string>& path)->void {
            for(const auto& [name,item]:value.items()) {
                auto next_path=path;next_path.push_back(name);
                std::vector<std::string> at{"properties"};at.insert(at.end(),next_path.begin(),next_path.end());
                const auto field=std::ranges::find_if(schema,[&](const auto& f){return f.names==next_path;});
                if(field==schema.end()) {
                    const bool prefix=std::ranges::any_of(schema,[&](const auto& f){return f.names.size()>next_path.size() && std::equal(next_path.begin(),next_path.end(),f.names.begin());});
                    if(!prefix) load.keep(object,std::move(at),item,world::DiagnosticKind::unknown_field,"Unknown field kept from the saved scene.");
                    else if(!item.is_object()) load.keep(object,std::move(at),item,world::DiagnosticKind::mismatch,"Expected a record. Defaults are used and the saved value is kept.");
                    else self(self,item,next_path);
                    continue;
                }
                bool converted{};std::string problem;std::optional<Scalar> clamped;
                try {
                    if(field->form==ValueForm::inline_value) {
                        const auto scalar=decode_scalar(runtime,take(runtime.read(object,field->path)),item,&converted);
                        auto stored=runtime.constrain(type,field->path,scalar);if(!stored) throw Mismatch(stored.error().message);
                        if(*stored!=scalar) clamped=*stored;
                        if(auto written=write(object,field->path,*stored);!written) throw Mismatch(written.error().message);
                    } else if(field->form==ValueForm::strong_sequence) {
                        if(!item.is_array() || item.size()>maximum_entities*maximum_components) throw Mismatch("Expected an object reference sequence.");
                        std::vector<ObjectReference> refs;refs.reserve(item.size());
                        for(const auto& ref:item) {
                            auto resolved=resolve(ref,field->type,ValueForm::strong_reference);
                            if(!resolved) throw Mismatch("A sequence element is not a reference of the field's type.");
                            refs.push_back(std::move(*resolved));
                        }
                        const auto locator=take(runtime.reference_field(object,field->path));
                        const ReferenceChange change=SequenceSplice{locator,0,take(runtime.sequence_size(locator)),refs};
                        if(auto applied=runtime.apply_reference_changes({&change,1});!applied) throw Mismatch(applied.error().message);
                    } else {
                        auto resolved=resolve(item,field->type,field->form);
                        if(!resolved) throw Mismatch("Expected a reference of the field's type.");
                        if(auto written=runtime.write_reference(object,field->path,*resolved);!written) throw Mismatch(written.error().message);
                    }
                } catch(const Mismatch& error) {problem=error.what();}
                if(!problem.empty()) load.keep(object,std::move(at),item,world::DiagnosticKind::mismatch,problem+" The default is used and the saved value is kept.");
                else if(clamped) load.keep(object,std::move(at),item,world::DiagnosticKind::mismatch,
                    "Outside the field's hard bounds; "+encode_scalar(runtime,*clamped).dump()+" is used and the saved value is kept.");
                else if(converted) load.report(world::Severity::info,world::DiagnosticKind::converted,object,std::move(at),&item,"Integral number converted exactly to the field's integer type.");
            }
        };
        read(read,record,{});
    }
    // Scene material definition or inline binding entry to a material object.
    static ObjectRoot restore_material(Session& session,Project* project,const Json& entry,std::vector<MissingResource>* missing=nullptr) {
        auto& runtime=session.runtime_;
        if(!project) throw std::runtime_error("Material restoration requires a Project.");
        const auto source=entry.at("source").get<std::string>();
        if(!source.empty() && entry.contains("inline")) throw std::runtime_error("Material binding has competing sources.");
        if(entry.contains("parent")) {
            if(!source.empty()) throw std::runtime_error("A material instance cannot own an asset identity.");
            auto parent=take(session.material_resource(*project,entry.at("parent").get<std::string>(),missing));
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
        return source.empty()?take(session.material_resource(*project,material_format::decode(entry.at("inline")),missing)):
            take(session.material_resource(*project,source,missing));
    }
    // A Play copy's object for a document identity. Unknown identities mean Play
    // diverged; identities whose object the game destroyed are skipped.
    struct Gone:std::runtime_error {using std::runtime_error::runtime_error;};
    static std::optional<ObjectId> live_object(const Session& session,std::uint64_t key) {
        const auto at=session.live_objects_.find(key);
        if(at==session.live_objects_.end()) return std::nullopt;
        return session.runtime_.type_of(at->second)?std::optional(at->second):std::optional(ObjectId{});
    }
    static ObjectId live_target(const Session& session,const Json& id) {
        const auto key=local_key(id);const auto object=live_object(session,key);
        if(!object) throw std::runtime_error("Document identity #"+std::to_string(key)+" does not exist in Play.");
        if(*object==ObjectId{}) throw Gone("#"+std::to_string(key)+" was destroyed by the game.");
        return *object;
    }
    // A reference leaving a live graph; nullopt when the game destroyed its target.
    static std::optional<ObjectId> live_reference(const Session& session,std::uint64_t key,Load& load) {
        const auto object=live_object(session,key);
        if(!object) throw std::runtime_error("Reference target #"+std::to_string(key)+" does not exist in Play.");
        if(*object==ObjectId{}) {load.dropped.push_back(key);return std::nullopt;}
        return *object;
    }

    static Prepared construct(Session& session,const Graph& graph,Project* project,bool keep_ids,Load& load) {
        const auto& data=graph.data;
        if(!data.is_object()) throw std::runtime_error("Expected a JSON record.");
        if(json_format::identity(data.at("version"))!=6) throw std::runtime_error("Object graph requires scene format version 6.");
        for(const auto& [name,value]:data.items())
            if(name!="version" && name!="next_id" && name!="world" && name!="roots" && name!="entities" && name!="materials") {
                if(load.document) load.keep({},{name},value,world::DiagnosticKind::unknown_key,"Unknown scene key kept from the saved scene.");
                else load.report(world::Severity::warning,world::DiagnosticKind::unknown_key,{},{name},&value,"Unknown scene key is not imported.");
            }
        const auto next=local_key(data.at("next_id"));const auto& entities=data.at("entities");const auto& root_values=data.at("roots");
        if(!entities.is_array() || entities.size()>maximum_entities || !root_values.is_array() || root_values.size()>entities.size())
            throw std::runtime_error("Invalid Entity graph directory.");
        if(!keep_ids && take(session.world_.members()).size()+entities.size()>maximum_entities) throw std::runtime_error("Editor Entity limit reached.");
        Prepared result{session};result.project=project;auto& runtime=session.runtime_;auto& world=session.world_;
        std::unordered_map<std::uint64_t,ObjectId> ids;std::unordered_map<std::uint64_t,const Json*> descriptors;
        std::unordered_map<ObjectId,const Json*> records;
        std::unordered_set<std::uint64_t> entity_ids;std::uint64_t largest{};
        // An unregistered type keeps identity, composition and saved data in a
        // placeholder. A mesh payload is format evidence of a StaticMeshComponent base.
        const auto create=[&](const Json& value,bool entity,ObjectId owner)->ObjectId {
            if(!value.is_object()) throw std::runtime_error("Expected a JSON record.");
            const auto key=local_key(value.at("id"));
            if(ids.contains(key)) throw std::runtime_error("Duplicate graph object identity: "+std::to_string(key));
            if(load.live) if(const auto live=live_object(session,key);live && *live!=ObjectId{})
                throw std::runtime_error("Document identity #"+std::to_string(key)+" already exists in Play.");
            largest=std::max(largest,key);const auto name=value.at("type").get<std::string>();
            const auto base=entity?session.types_.entity:session.types_.component;
            const auto found=runtime.find_type(name);
            const auto matches=found?runtime.is_a(*found,base):Result<bool>{false};
            const bool placeholder=!matches || !*matches;
            const auto type=!placeholder?*found:!entity && value.contains("mesh")?session.types_.static_mesh_component:base;
            auto root=entity?take(world::create_entity(runtime,type)):take(world::create_component(runtime,type));
            const auto object=root.id();ids.emplace(key,object);descriptors.emplace(key,&value);records.emplace(object,&value);result.objects.push_back(std::move(root));
            if(keep_ids) session.local_ids_.emplace(object,key);else (void)identity(session,object);
            load.owners.emplace(object,entity?object:owner);load.types.emplace(object,name);load.keys.emplace(object,key);
            if(placeholder) {
                load.report(world::Severity::warning,world::DiagnosticKind::unknown_type,object,{},nullptr,
                    std::string(entity?"Entity":"Component")+" type is not registered; a placeholder keeps its saved data.");
                load.pending[object]["type"]=name;
            }
            std::wstring label;
            if(const auto at=value.find("name");at!=value.end()) {
                std::string problem;
                try {
                    if(!at->is_string()) throw Mismatch("Expected a name string.");
                    label=wide(at->get<std::string>());
                    if(entity || !label.empty()) if(auto valid=validate_name(label);!valid) throw Mismatch(valid.error().message);
                } catch(const std::exception& error) {problem=error.what();}
                if(!problem.empty()) {label.clear();load.keep(object,{"name"},*at,world::DiagnosticKind::mismatch,problem+" A generated name is used and the saved value is kept.");}
            }
            if(entity && label.empty()) {
                const auto dot=name.find_last_of('.');
                label=wide(dot==name.npos?name:name.substr(dot+1))+L" "+std::to_wstring(key);
                if(!validate_name(label)) label=L"Entity "+std::to_wstring(key);
            }
            if(entity || !label.empty()) result.labels.emplace(object,label);
            if(entity) entity_ids.insert(key);
            return object;
        };
        // A live record {"id","existing":true,"children":[],"components"} adds
        // components to an Entity Play already has; the Entity itself is untouched.
        std::unordered_set<std::uint64_t> anchors;
        for(const auto& entity:entities) {
            ObjectId object;
            if(load.live && entity.is_object() && entity.value("existing",false)) {
                fields(entity,{"id","existing","children","components"});
                const auto key=local_key(entity.at("id"));
                if(ids.contains(key)) throw std::runtime_error("Duplicate graph object identity: "+std::to_string(key));
                if(!entity.at("children").empty()) throw std::runtime_error("An existing Entity record cannot add children.");
                object=live_target(session,entity.at("id"));largest=std::max(largest,key);anchors.insert(key);
                ids.emplace(key,object);descriptors.emplace(key,&entity);records.emplace(object,&entity);
                load.owners.emplace(object,object);load.types.emplace(object,type_name(runtime,take(runtime.type_of(object))));load.keys.emplace(object,key);
            } else {
                object=create(entity,true,{});
                load.extra(object,entity,{"id","type","name","enabled","space","children","components","properties"});
            }
            const auto& parts=entity.at("components");
            if(!parts.is_array() || parts.size()>maximum_components) throw std::runtime_error("Invalid Component count.");
            for(const auto& part:parts) {
                const auto component=create(part,false,object);
                if(derives(runtime,take(runtime.type_of(component)),session.types_.static_mesh_component))
                    load.extra(component,part,{"id","type","name","enabled","properties","mesh","base_color","roughness","metallic","emissive","materials"});
                else load.extra(component,part,{"id","type","name","enabled","properties"});
            }
        }
        // A stand-in keeps the key, so saving writes the reference unchanged.
        const auto report_missing=[&](ObjectId object,std::vector<std::string> base,const Json& entry,const std::vector<MissingResource>& missing) {
            for(const auto& item:missing) {
                auto at=base;const Json value=item.key;
                if(item.input) {at.push_back("textures");at.emplace_back(material_format::names[*item.input]);}
                else at.emplace_back(entry.contains("parent")?"parent":"source");
                load.report(world::Severity::warning,item.kind,object,std::move(at),&value,item.reason+
                    (item.input?" The input has no texture":" A default material stands in")+"; the reference is saved unchanged.");
            }
        };
        std::unordered_map<std::uint64_t,ObjectRoot> material_ids;
        std::unordered_set<ObjectId> material_objects;
        std::unordered_map<ObjectId,ObjectRoot> copied_materials;
        std::unordered_map<ObjectId,std::uint64_t> copied_material_keys;
        std::unordered_set<std::uint64_t> used_materials;
        const auto definitions=data.value("materials",Json::array());
        if(!definitions.is_array() || definitions.size()>maximum_entities*maximum_components) throw std::runtime_error("Invalid scene material directory.");
        for(const auto& definition:definitions) {
            if(!definition.is_object()) throw std::runtime_error("Expected a JSON record.");
            const auto key=local_key(definition.at("id"));
            if(ids.contains(key)) throw std::runtime_error("Duplicate graph object identity: "+std::to_string(key));
            largest=std::max(largest,key);ObjectRoot resource;std::vector<MissingResource> missing;
            // Play keeps a material it already has; its value changes travel separately.
            const auto live=load.live?live_object(session,key):std::nullopt;
            const bool reused=live && *live!=ObjectId{};
            if(reused) {
                if(!session.is_material(*live)) throw std::runtime_error("Document identity #"+std::to_string(key)+" is not a material in Play.");
                resource=take(runtime.hold(*live));
            } else if(auto at=graph.materials.find(std::to_string(key));at!=graph.materials.end()) {
                const auto state=take(material::read_material(runtime,at->second.id()));
                resource=state.source.empty()?take(material::create_material(runtime,state)):at->second;
                copied_materials.emplace(at->second.id(),resource);
                copied_material_keys.emplace(at->second.id(),key);
            } else resource=restore_material(session,project,definition,&missing);
            if(!material_objects.insert(resource.id()).second) throw std::runtime_error("One material asset has multiple document identities.");
            ids.emplace(key,resource.id());material_ids.emplace(key,resource);result.resources.push_back(resource);
            if(keep_ids && !reused) {
                if(!session.local_ids_.emplace(resource.id(),key).second) throw std::runtime_error("One material asset has multiple document identities.");
            } else if(!keep_ids) (void)identity(session,resource.id());
            load.types.emplace(resource.id(),"material.Material");load.keys.emplace(resource.id(),key);
            report_missing(resource.id(),{},definition,missing);
            load.extra(resource.id(),definition,{"id","source","inline","parent","overrides"});
        }
        if(next<=largest) throw std::runtime_error("next_id must exceed every document object identity.");
        if(keep_ids && !load.live) session.next_local_id_=next;
        std::unordered_map<std::uint64_t,std::uint64_t> owners;
        for(const auto& entity:entities) {
            const auto key=local_key(entity.at("id"));const auto object=ids.at(key);std::vector<world::Attachment> parts;
            for(const auto& part:entity.at("components")) parts.push_back({ids.at(local_key(part.at("id")))});
            // A running Entity begins new components on attachment, after they are restored.
            if(anchors.contains(key)) result.anchored.emplace_back(object,std::move(parts));
            else if(!parts.empty()) committed(world.attach_components(object,parts));
            if(const auto space=entity.find("space");space!=entity.end() && !space->is_null()) {
                world::Trs trs;
                if(!space->is_object()) load.keep(object,{"space"},*space,world::DiagnosticKind::mismatch,"Expected a space record. The identity transform is used and the saved value is kept.");
                else {
                    load.extra(object,*space,{"position","rotation","scale","origin"},{"space"});
                    std::string problem;
                    try {trs=json_trs(*space);}
                    catch(const std::exception& error) {problem=error.what();}
                    if(!problem.empty()) {
                        // The shared canonical check decides validity; origin stays structural.
                        load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"space"},&*space,problem+" The identity transform is used and the saved values are kept.");
                        trs={};
                        for(const auto name:{"position","rotation","scale"})
                            if(const auto at=space->find(name);at!=space->end()) load.store(object,std::array{std::string("space"),std::string(name)},*at);
                    }
                }
                take(world.set_transform(object,trs));
            }
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
        // Dangling or malformed identities are structural damage. A target whose
        // type does not satisfy the field is schema drift: nullopt, not an error.
        const auto resolve=[&](const Json& value,TypeId target,ValueForm form)->std::optional<ObjectReference> {
            if(value.is_null()) return take(runtime.null_reference(target,form==ValueForm::weak_reference?form:ValueForm::strong_reference));
            ObjectId object;
            if(value.is_object() && value.contains("$ref")) {
                fields(value,{"$ref"});const auto key=local_key(value.at("$ref"));
                if(ids.contains(key)) object=ids.at(key);
                else if(!load.live) throw std::runtime_error("Reference target is outside this graph: "+std::to_string(key));
                else if(const auto live=live_reference(session,key,load)) object=*live;
                else return take(runtime.null_reference(target,form==ValueForm::weak_reference?form:ValueForm::strong_reference));
                if(material_ids.contains(key)) used_materials.insert(key);
            } else if(value.is_object() && value.contains("$external")) {
                fields(value,{"$external"});const auto key=value.at("$external").get<std::string>();const auto at=graph.external.find(key);
                if(at==graph.external.end()) throw std::runtime_error("External object references cannot be loaded from a file.");object=at->second.id();
            } else return std::nullopt;
            auto held=runtime.hold(object,target);if(!held) return std::nullopt;
            return form==ValueForm::weak_reference?take(runtime.observe(held->reference())):held->reference();
        };
        for(const auto& entity:entities) {
            const auto space=entity.find("space");
            if(space==entity.end() || space->is_null()) continue;
            std::optional<ObjectId> target;
            if(space->is_object()) if(const auto origin=space->find("origin");origin!=space->end() && !origin->is_null()) {
                if(origin->is_string()) {
                    const auto key=local_key(*origin,true);
                    if(!key || entity_ids.contains(key)) target=key?ids.at(key):ObjectId{};
                    else if(!load.live) throw std::runtime_error("Space reference must target an Entity in this graph.");
                    else target=live_reference(session,key,load);
                } else {
                    const auto resolved=resolve(*origin,session.types_.entity,ValueForm::weak_reference);
                    if(!resolved) throw std::runtime_error("Space reference must target an Entity in this graph.");
                    target=resolved->id();
                }
            }
            result.spaces.push_back({ids.at(local_key(entity.at("id"))),target});
        }
        const auto properties=[&](const Json& value,ObjectId object) {
            const auto write=[&](ObjectId target,std::span<const FieldId> path,Scalar scalar){return runtime.write(target,path,scalar);};
            if(const auto at=value.find("properties");at!=value.end()) {
                if(at->is_object()) read_properties(session,object,*at,load,resolve,write);
                else load.keep(object,{"properties"},*at,world::DiagnosticKind::mismatch,"Expected a property record. Defaults are used and the saved value is kept.");
            }
            bool enabled=true;
            if(const auto at=value.find("enabled");at==value.end())
                load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"enabled"},nullptr,"The enabled flag is missing; the object is enabled.");
            else if(!at->is_boolean()) load.keep(object,{"enabled"},*at,world::DiagnosticKind::mismatch,"Expected a boolean. The object is enabled and the saved value is kept.");
            else enabled=at->get<bool>();
            committed(world.set_enabled(object,enabled));
        };
        // A record that fails its combined validation returns to the type defaults.
        using Defaults=std::vector<std::pair<world::FieldPath,Scalar>>;
        const auto snapshot=[&](ObjectId object,std::string_view prefix) {
            Defaults values;
            for(const auto& field:business_fields(runtime,session.types_,take(runtime.type_of(object))))
                if(field.form==ValueForm::inline_value && !field.names.empty() && field.names.front()==prefix)
                    values.emplace_back(field.path,take(runtime.read(object,field.path)));
            return values;
        };
        const auto saved_property=[&](ObjectId object,std::string_view name)->const Json* {
            const auto& record=*records.at(object);const auto at=record.find("properties");
            if(at==record.end() || !at->is_object()) return nullptr;
            const auto found=at->find(name);return found==at->end()?nullptr:&*found;
        };
        const auto invalid_record=[&](ObjectId object,const std::string& name,const Defaults& defaults,const std::string& problem) {
            if(const auto* value=saved_property(object,name)) load.keep(object,{"properties",name},*value,world::DiagnosticKind::mismatch,problem+" Defaults are used and the saved value is kept.");
            else load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"properties",name},nullptr,problem+" Defaults are used.");
            for(const auto& [path,value]:defaults) take(runtime.write(object,path,value));
        };
        // Without its mesh a component has no slots. Its binding table is kept verbatim,
        // with scene material references renamed to document identities kept alive by it.
        const auto keep_bindings=[&](ObjectId object,const Json& entries) {
            if(entries.empty()) return;
            if(entries.size()>1024) throw std::runtime_error("Invalid material binding table.");
            auto kept=entries;auto& held=load.materials[object];
            for(auto& entry:kept) {
                if(!entry.is_object()) throw std::runtime_error("Expected a JSON record.");
                const auto at=entry.find("resource");if(at==entry.end()) continue;
                const auto key=local_key(*at);const auto found=material_ids.find(key);
                if(found==material_ids.end()) throw std::runtime_error("Material binding references an unknown scene resource.");
                used_materials.insert(key);held.push_back(found->second);*at=std::to_string(identity(session,found->second.id()));
            }
            load.store(object,std::array{std::string("materials")},kept);
        };
        for(const auto& entity:entities) {
            if(const auto key=local_key(entity.at("id"));!anchors.contains(key)) properties(entity,ids.at(key));
            for(const auto& part:entity.at("components")) {
                const auto object=ids.at(local_key(part.at("id")));const auto type=take(runtime.type_of(object));
                const bool light=derives(runtime,type,session.types_.light_component),physical=physical_type(runtime,type);
                const auto light_defaults=light?snapshot(object,"settings"):Defaults{};
                const auto body_defaults=physical?snapshot(object,"body"):Defaults{};
                properties(part,object);
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
                        if(const auto entries=part.find("materials");!copy.state.mesh.resolved && !copy.state.mesh.source.empty() &&
                            entries!=part.end() && entries->is_array()) keep_bindings(object,*entries);
                    } else {
                        if(!project) throw std::runtime_error("Material and mesh restoration requires a Project.");
                        std::string source;
                        if(const auto mesh_at=part.find("mesh");mesh_at==part.end())
                            load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"mesh"},nullptr,"The mesh source is missing; the component has no mesh.");
                        else if(!mesh_at->is_string()) load.keep(object,{"mesh"},*mesh_at,world::DiagnosticKind::mismatch,"Expected a mesh source key. The component has no mesh and the saved value is kept.");
                        else source=mesh_at->get<std::string>();
                        geometry::MeshRef mesh;
                        if(!source.empty()) {
                            if(auto loaded=project->mesh(source)) mesh=loaded->mesh;
                            else {
                                const auto& item=session.remember_unavailable(*project,source,loaded.error());const Json value=source;
                                load.report(world::Severity::warning,item.kind,object,{"mesh"},&value,item.reason+
                                    " The component draws nothing; the reference and its material bindings are saved unchanged.");
                            }
                        }
                        const bool unresolved=!source.empty() && !mesh;
                        take(world::set_mesh_binding(runtime,object,{source,std::move(mesh)}));
                        world::Material material;std::vector<std::string> applied;
                        const auto vector_value=[&](const char* name,math::Vector3& target) {
                            const auto found=part.find(name);if(found==part.end()) return;
                            std::string problem;
                            try {target=vector(*found);applied.emplace_back(name);} catch(const std::exception& error) {problem=error.what();}
                            if(!problem.empty()) load.keep(object,{name},*found,world::DiagnosticKind::mismatch,problem+" The default is used and the saved value is kept.");
                        };
                        const auto number_value=[&](const char* name,float& target) {
                            const auto found=part.find(name);if(found==part.end()) return;
                            std::string problem;
                            try {target=std::get<float>(decode_scalar(runtime,Scalar{target},*found));applied.emplace_back(name);} catch(const Mismatch& error) {problem=error.what();}
                            if(!problem.empty()) load.keep(object,{name},*found,world::DiagnosticKind::mismatch,problem+" The default is used and the saved value is kept.");
                        };
                        vector_value("base_color",material.base_color);number_value("roughness",material.roughness);
                        number_value("metallic",material.metallic);vector_value("emissive",material.emissive);
                        if(auto set=world.set_material(object,material);!set) {
                            load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"material"},nullptr,set.error().message+" Defaults are used and the saved values are kept.");
                            for(const auto& name:applied) load.store(object,std::array{name},part.at(name));
                            take(world.set_material(object,world::Material{}));
                        }
                        std::vector<world::MaterialBinding> bindings;
                        if(const auto entries=part.find("materials");entries!=part.end() && !entries->is_array())
                            load.keep(object,{"materials"},*entries,world::DiagnosticKind::mismatch,"Expected a material binding table. Bindings are empty and the saved value is kept.");
                        else if(entries!=part.end() && unresolved) keep_bindings(object,*entries);
                        else if(entries!=part.end()) {
                            if(entries->size()>1024) throw std::runtime_error("Invalid material binding table.");
                            for(const auto& entry:*entries) {
                                if(!entry.is_object()) throw std::runtime_error("Expected a JSON record.");
                                const auto slot=entry.at("slot").get<std::string>();
                                for(const auto& [name,item]:entry.items())
                                    if(name!="slot" && name!="resource" && name!="source" && name!="overridden" && name!="inline" && name!="parent" && name!="overrides")
                                        load.keep_slot(object,slot,name,item,world::DiagnosticKind::unknown_key,"Unknown binding key kept from the saved scene.");
                                ObjectRoot resource;
                                if(entry.contains("resource")) {
                                    for(const auto name:{"source","inline","parent","overrides"})
                                        if(entry.contains(name)) throw std::runtime_error(std::string("Unsupported field: ")+name);
                                    const auto key=local_key(entry.at("resource"));
                                    const auto at=material_ids.find(key);
                                    if(at==material_ids.end()) throw std::runtime_error("Material binding references an unknown scene resource.");
                                    used_materials.insert(key);resource=at->second;
                                } else {
                                    std::vector<MissingResource> missing;resource=restore_material(session,project,entry,&missing);
                                    report_missing(object,{"materials",slot},entry,missing);
                                }
                                bool overridden{};
                                if(const auto flag=entry.find("overridden");flag==entry.end())
                                    load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"materials",slot,"overridden"},nullptr,"The override flag is missing; the slot is not overridden.");
                                else if(!flag->is_boolean()) load.keep_slot(object,slot,"overridden",*flag,world::DiagnosticKind::mismatch,"Expected a boolean. The slot is not overridden and the saved value is kept.");
                                else overridden=flag->get<bool>();
                                bindings.push_back({slot,std::move(resource),overridden});
                            }
                        }
                        take(world::set_material_bindings(runtime,object,bindings));
                    }
                }
                if(light) if(auto valid=world::validate_light(runtime,take(world::read_light(runtime,object)));!valid)
                    invalid_record(object,"settings",light_defaults,valid.error().message);
                if(physical) {
                    auto settings=take(physics::read_settings(runtime,object));std::string problem;
                    if(auto valid=world::validate_body_settings(runtime,settings);!valid) problem=valid.error().message;
                    else if(settings.enabled && settings.shape==physics::ShapeKind::triangle_mesh && project) {
                        const auto mesh=world::read_static_mesh(runtime,object);
                        if(!mesh) problem=mesh.error().message;
                        else if(mesh->mesh.source.empty()) problem="Triangle collision needs a persistent mesh source.";
                    }
                    if(!problem.empty()) {invalid_record(object,"body",body_defaults,problem);settings=take(physics::read_settings(runtime,object));}
                    if(auto at=graph.bodies.find(part.at("id").get<std::string>());at!=graph.bodies.end())
                        settings.cooked_shape=at->second.cooked_shape;
                    if(settings.enabled && settings.shape==physics::ShapeKind::triangle_mesh) {
                        const auto mesh=take(world::read_static_mesh(runtime,object));
                        if(!mesh.mesh.resolved && !mesh.mesh.source.empty()) {
                            const auto* item=session.unavailable(mesh.mesh.source);
                            load.report(world::Severity::warning,item?item->kind:world::DiagnosticKind::missing_resource,object,{"properties","body","shape"},nullptr,
                                "Triangle collision has no mesh to cook; Play cannot create this body until the mesh is restored.");
                        } else if(mesh.mesh.source.starts_with("project/")) result.triangle_meshes.push_back(object);
                    }
                    if(auto configured=physics::configure(runtime,object,settings);!configured) {
                        invalid_record(object,"body",body_defaults,configured.error().message);std::erase(result.triangle_meshes,object);
                        take(physics::configure(runtime,object,take(physics::read_settings(runtime,object))));
                    }
                }
            }
        }
        // Controller bindings validate their entity sequences as a whole.
        const auto bound=[&](ObjectId object,const char* name,Result<void> applied,auto clear) {
            if(applied) return;
            if(const auto* value=saved_property(object,name)) load.keep(object,{"properties",name},*value,world::DiagnosticKind::mismatch,applied.error().message+" The binding is empty and the saved value is kept.");
            else load.report(world::Severity::warning,world::DiagnosticKind::mismatch,object,{"properties",name},nullptr,applied.error().message+" The binding is empty.");
            take(clear());
        };
        for(const auto& root:result.objects) {
            const auto object=root.id();const auto type=take(runtime.type_of(object));
            if(derives(runtime,type,session.physics_types_.ragdoll_component))
                bound(object,"parts",physics::set_ragdoll_parts(runtime,object,take(physics::ragdoll_parts(runtime,object))),[&]{return physics::set_ragdoll_parts(runtime,object,{});});
            if(derives(runtime,type,session.physics_types_.vehicle_component))
                bound(object,"wheels",physics::set_vehicle_wheels(runtime,object,take(physics::vehicle_wheels(runtime,object))),[&]{return physics::set_vehicle_wheels(runtime,object,{});});
            if(derives(runtime,type,session.physics_types_.scene_query_component))
                bound(object,"ignored_entities",physics::set_query_ignored_entities(runtime,object,take(physics::query_ignored_entities(runtime,object))),
                    [&]{return physics::set_query_ignored_entities(runtime,object,{});});
        }
        // Retained references bind to document identities, which stay stable for
        // the life of the document and are re-encoded on save.
        for(auto& [object,entry]:load.pending) {
            Retained kept;
            if(const auto at=entry.find("type");at!=entry.end()) kept.type=at->get<std::string>();
            for(const auto name:{"record","slots"}) if(auto at=entry.find(name);at!=entry.end()) {
                each_reference(*at,[&](Json& node) {
                    ObjectId target;
                    if(node.contains("$ref")) {
                        fields(node,{"$ref"});const auto key=local_key(node.at("$ref"));
                        // A live graph's outward references already name document identities.
                        if(!ids.contains(key) && load.live) return;
                        if(!ids.contains(key)) throw std::runtime_error("Reference target is outside this graph: "+std::to_string(key));
                        target=ids.at(key);if(material_ids.contains(key)) used_materials.insert(key);
                    } else {
                        fields(node,{"$external"});const auto found=graph.external.find(node.at("$external").get<std::string>());
                        if(found==graph.external.end()) throw std::runtime_error("External object references cannot be loaded from a file.");
                        target=found->second.id();
                    }
                    node=ref_key(identity(session,target));
                });
                (std::string_view(name)=="record"?kept.record:kept.slots)=dump_retained(*at);
            }
            if(auto at=load.materials.find(object);at!=load.materials.end()) kept.materials=std::move(at->second);
            load.retained.emplace(object,std::move(kept));
        }
        for(const auto& [key,resource]:material_ids) if(!used_materials.contains(key) && !load.live) {
            if(load.document) {
                load.report(world::Severity::warning,world::DiagnosticKind::unknown_key,resource.id(),{},nullptr,"The material definition is not referenced; it is kept for the next save.");
                load.unreferenced.push_back(resource);
            } else load.report(world::Severity::warning,world::DiagnosticKind::unknown_key,resource.id(),{},nullptr,"The material definition is not referenced and is not imported.");
        }
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
    static void prepare_cook(Session& session,Prepared& prepared,std::span<const world::EntityAttachment> attachments,Load& load) {
        if(prepared.triangle_meshes.empty()) return;
        std::unordered_map<ObjectId,math::Matrix4> worlds;
        if(!attachments.empty()) for(const auto& [object,matrix]:take(session.world_.attachment_world_matrices(attachments,prepared.spaces))) worlds.emplace(object,matrix);
        std::unordered_map<ObjectId,ObjectId> anchored;
        for(const auto& [entity,parts]:prepared.anchored) for(const auto& part:parts) anchored.emplace(part.component,entity);
        for(const auto object:prepared.triangle_meshes) {
            const auto mesh=take(world::read_static_mesh(session.runtime_,object));
            const auto at=anchored.find(object);
            const auto owner=at!=anchored.end()?at->second:take(world::inspect_component(session.runtime_,object)).owner;
            // Components added to a live Entity take its current space.
            const auto found=worlds.find(owner);
            const auto matrix=found!=worlds.end()?found->second:take(world::world_matrix(session.runtime_,owner));
            const auto trs=take(world::relative_transform(matrix,world::transform_matrix({})));
            auto settings=take(physics::read_settings(session.runtime_,object));
            if(prepared.project) {
                auto cooked=prepared.project->collision(mesh.mesh.source,trs.scale);
                if(!cooked) {
                    load.report(world::Severity::warning,world::DiagnosticKind::unreadable_resource,object,{"properties","body","shape"},nullptr,
                        cooked.error().message+" Play cannot create this body until its mesh can be cooked.");
                    continue;
                }
                settings.cooked_shape=std::move(*cooked);
            } else if(settings.cooked_shape) take(physics::validate_cooked_triangle_mesh(*settings.cooked_shape,mesh.mesh.resolved,trs.scale,mesh.mesh.source));
            take(physics::configure(session.runtime_,object,std::move(settings)));
        }
    }
    static world::Commit publish(Session& session,Prepared& prepared,const Graph& graph,ObjectId parent,bool duplicate,Load& load) {
        EntityChange history;history.added=true;history.removals.reserve(prepared.roots.size());
        for(auto root:prepared.roots) {world::EntityRemoval removal;removal.root=take(session.runtime_.hold(root));history.removals.push_back(std::move(removal));}
        auto plans=attachments(prepared,graph,parent,duplicate);prepare_cook(session,prepared,plans,load);
        auto selection=prepared.roots;
        session.history_.reserve(session.history_.size()+1);
        session.retained_.reserve(session.retained_.size()+load.retained.size());
        if(!duplicate) session.load_issues_.reserve(session.load_issues_.size()+load.issues.size());
        for(const auto& [object,label]:prepared.labels) session.names_.emplace(object,label);
        // Retained data travels with its objects, which history keeps alive.
        const auto adopt=[&] {
            session.retained_.merge(load.retained);
            if(!duplicate) std::ranges::move(load.issues,std::back_inserter(session.load_issues_));
        };
        world::Commit result;
        try {
            result=take(session.world_.attach_batch(plans,prepared.spaces));
        } catch(...) {
            const auto state=world::inspect_entity(session.runtime_,prepared.roots.front());
            const bool attached=state && (state->state==world::EntityState::attached_inactive || state->state==world::EntityState::active);
            const bool retained=attached && !session.world_.detach_batch(prepared.roots);
            if(retained) {prepared.published=true;session.remember(std::move(history));adopt();}
            throw;
        }
        if(result.changed) {
            // Selection and history storage were prepared before World publication.
            // No admission, allocation or callback can fail between these commits.
            prepared.published=true;session.selected_entities_.swap(selection);
            session.selection_={prepared.roots.back(),prepared.roots.back()};++session.revision_;
            session.remember(std::move(history));adopt();
        }
        return result;
    }
    // Play publication keeps document identities and records no history.
    static world::Commit publish_live(Session& session,Prepared& prepared,ObjectId parent,std::size_t index,Load& load) {
        std::vector<world::EntityAttachment> plans;
        for(const auto root:prepared.roots) plans.push_back({root,parent,index});
        prepare_cook(session,prepared,plans,load);
        for(const auto& [object,label]:prepared.labels) session.names_.insert_or_assign(object,label);
        world::Commit result;
        if(!plans.empty()) result=take(session.world_.attach_batch(plans,prepared.spaces));
        for(const auto& [entity,parts]:prepared.anchored) result=take(session.world_.attach_components(entity,parts,index));
        prepared.published=true;
        session.retained_.merge(load.retained);
        for(const auto& object:prepared.objects) session.live_objects_.insert_or_assign(session.local_ids_.at(object.id()),object.id());
        for(const auto& resource:prepared.resources) session.live_objects_.insert_or_assign(session.local_ids_.at(resource.id()),resource.id());
        return result;
    }
    struct Unsupported:std::runtime_error {using std::runtime_error::runtime_error;};
    // A simulation that cannot follow an authored pose leaves the edit unapplied.
    static void relocated(Result<void> value) {
        if(!value && value.error().code==ErrorCode::unsupported_operation) throw Unsupported(value.error().message);
        take(std::move(value));
    }
    static std::size_t live_index(const Json& value) {return value.is_null()?world::append:json_format::identity(value);}
    static float live_number(const Json& value) {
        if(!value.is_number()) throw std::runtime_error("Expected a number.");
        const auto number=value.get<float>();if(!std::isfinite(number)) throw std::runtime_error("Numbers must be finite.");
        return number;
    }
    // One op of a Play change batch; see Session::apply_live.
    static void apply_change(Session& session,Project& project,const Json& op,std::size_t index,std::vector<world::Diagnostic>& out) {
        auto& runtime=session.runtime_;auto& world=session.world_;
        std::string subject="Play change "+std::to_string(index);ObjectId object;
        const auto note=[&](world::Severity severity,world::DiagnosticKind kind,const std::string& detail) {
            world::Diagnostic item;item.serial=out.size()+1;item.severity=severity;item.kind=kind;item.phase=world::DiagnosticPhase::command;
            item.object=object;
            if(const auto type=runtime.type_of(object)) {
                item.type=type_name(runtime,*type);
                if(derives(runtime,*type,session.types_.entity)) item.entity=object;
                else if(const auto component=world::inspect_component(runtime,object)) item.entity=component->owner;
            }
            item.message=subject+": "+detail;out.push_back(std::move(item));
        };
        const auto target=[&](const Json& id) {
            const auto key=local_key(id);subject+=" #"+std::to_string(key);
            if(const auto at=session.live_objects_.find(key);at!=session.live_objects_.end()) object=at->second;
            return live_target(session,id);
        };
        const auto commit=[&](Result<world::Commit> value) {
            const auto result=take(std::move(value));
            if(result.notification_error) note(world::Severity::warning,world::DiagnosticKind::observer_failed,result.notification_error->message);
        };
        Load load;load.live=true;
        try {
            if(!op.is_object()) throw std::runtime_error("Expected a change record.");
            const auto kind=op.at("op").get<std::string>();
            if(kind=="add" || kind=="attach") {
                const bool add=kind=="add";
                if(add) fields(op,{"op","parent","index","graph"});else fields(op,{"op","entity","index","graph"});
                ObjectId parent;
                if(add) {
                    if(const auto& roots=op.at("graph").at("roots");roots.is_array() && roots.size()==1) subject+=" #"+std::to_string(local_key(roots[0]));
                    if(!op.at("parent").is_null()) parent=live_target(session,op.at("parent"));
                }
                const auto entity=add?ObjectId{}:target(op.at("entity"));
                Graph graph;graph.data=op.at("graph");
                // A resource imported after Play opened its Project is not catalogued yet.
                std::optional<Prepared> prepared;
                for(bool refreshed=false;;refreshed=true) {
                    load=Load{};load.live=true;
                    prepared.emplace(construct(session,graph,&project,true,load));
                    if(refreshed || std::ranges::none_of(load.issues,[](const auto& issue){return issue.kind==world::DiagnosticKind::missing_resource;})) break;
                    prepared.reset();take(project.refresh());
                }
                if(add?prepared->roots.size()!=1 || !prepared->anchored.empty():!prepared->roots.empty() || prepared->anchored.size()!=1 || prepared->anchored.front().first!=entity)
                    throw std::runtime_error(add?"An add carries exactly one root Entity.":"An attach carries only new components of its Entity.");
                if(add) object=prepared->roots.front();
                commit(publish_live(session,*prepared,parent,live_index(op.at("index")),load));
                // The document carries the same unresolved data; Play reports it without diverging.
                for(auto issue:load.issues) if(issue.severity!=world::Severity::info) {
                    issue.serial=out.size()+1;issue.phase=world::DiagnosticPhase::command;issue.message=subject+": "+issue.message;out.push_back(std::move(issue));
                }
            } else if(kind=="remove") {
                fields(op,{"op","id"});const auto removed=target(op.at("id"));const auto type=take(runtime.type_of(removed));
                if(derives(runtime,type,session.types_.entity)) commit(world::destroy_entity(runtime,removed));
                else if(derives(runtime,type,session.types_.component)) commit(world::destroy_component(runtime,removed));
                else throw std::runtime_error("Only an Entity or Component can be removed.");
            } else if(kind=="set") {
                fields(op,{"op","id","name","enabled","space","properties","base_color","roughness","metallic","emissive","materials"});
                const auto edited=target(op.at("id"));const auto type=take(runtime.type_of(edited));
                const bool entity=derives(runtime,type,session.types_.entity);
                load.policy=LoadPolicy::strict;
                if(const auto at=op.find("name");at!=op.end()) {
                    auto label=wide(at->get<std::string>());
                    if(entity || !label.empty()) take(validate_name(label));
                    if(label.empty()) session.names_.erase(edited);else session.names_.insert_or_assign(edited,std::move(label));
                }
                if(const auto at=op.find("enabled");at!=op.end()) {
                    if(!at->is_boolean()) throw std::runtime_error("Expected a boolean enabled flag.");
                    commit(world.set_enabled(edited,at->get<bool>()));
                }
                if(const auto at=op.find("properties");at!=op.end()) {
                    if(!at->is_object()) throw std::runtime_error("Expected a property record.");
                    const Resolve resolve=[&](const Json& value,TypeId field_type,ValueForm form)->std::optional<ObjectReference> {
                        const auto null=[&]{return take(runtime.null_reference(field_type,form==ValueForm::weak_reference?form:ValueForm::strong_reference));};
                        if(value.is_null()) return null();
                        if(!value.is_object() || !value.contains("$ref")) return std::nullopt;
                        fields(value,{"$ref"});
                        const auto live=live_reference(session,local_key(value.at("$ref")),load);if(!live) return null();
                        auto held=runtime.hold(*live,field_type);if(!held) return std::nullopt;
                        return form==ValueForm::weak_reference?take(runtime.observe(held->reference())):held->reference();
                    };
                    read_properties(session,edited,*at,load,resolve,[&](ObjectId written,std::span<const FieldId> path,Scalar scalar){return world.write(written,path,scalar);});
                }
                if(op.contains("base_color") || op.contains("roughness") || op.contains("metallic") || op.contains("emissive")) {
                    auto material=take(world::read_static_mesh(runtime,edited)).material;
                    if(const auto at=op.find("base_color");at!=op.end()) material.base_color=vector(*at);
                    if(const auto at=op.find("roughness");at!=op.end()) material.roughness=live_number(*at);
                    if(const auto at=op.find("metallic");at!=op.end()) material.metallic=live_number(*at);
                    if(const auto at=op.find("emissive");at!=op.end()) material.emissive=vector(*at);
                    take(world.set_material(edited,material));
                }
                if(const auto at=op.find("materials");at!=op.end()) {
                    if(!at->is_array() || at->size()>1024) throw std::runtime_error("Expected a material binding table.");
                    std::vector<world::MaterialBinding> bindings;
                    for(const auto& entry:*at) {
                        fields(entry,{"slot","resource","overridden"});
                        const auto key=local_key(entry.at("resource"));const auto live=live_object(session,key);
                        // Materials are document resources; the editor sends a missing one first.
                        if(!live || *live==ObjectId{} || !session.is_material(*live)) throw std::runtime_error("Material #"+std::to_string(key)+" does not exist in Play.");
                        bindings.push_back({entry.at("slot").get<std::string>(),take(runtime.hold(*live)),entry.at("overridden").get<bool>()});
                    }
                    take(world::set_material_bindings(runtime,edited,bindings));
                }
                if(const auto at=op.find("space");at!=op.end()) {
                    if(!entity) throw std::runtime_error("Only an Entity has a space.");
                    if(at->is_null()) take(world.remove_transform(edited));
                    else {
                        fields(*at,{"position","rotation","scale","origin"});
                        const bool spatial=take(world::has_transform(runtime,edited));
                        auto trs=spatial?take(world::read_transform(runtime,edited)):world::Trs{};
                        if(const auto v=at->find("position");v!=at->end()) trs.position=vector(*v);
                        if(const auto v=at->find("rotation");v!=at->end()) {const auto q=json_format::vector<4>(*v);trs.rotation={q[0],q[1],q[2],q[3]};}
                        if(const auto v=at->find("scale");v!=at->end()) trs.scale=vector(*v);
                        trs=take(world::canonical_transform(trs));
                        if(const auto origin=at->find("origin");origin!=at->end()) {
                            std::optional<ObjectId> reference;
                            if(!origin->is_null()) {const auto key=local_key(*origin,true);reference=key?live_reference(session,key,load):std::optional(ObjectId{});}
                            if(!spatial) relocated(world.set_transform(edited,trs));
                            commit(world.set_space_reference(edited,reference,world::ReparentRule::keep_local));
                        }
                        relocated(world.set_transform(edited,trs));
                    }
                }
            } else if(kind=="move") {
                fields(op,{"op","id","parent","index","space"});
                const auto moved=target(op.at("id"));
                const auto parent=op.at("parent").is_null()?ObjectId{}:live_target(session,op.at("parent"));
                commit(world.reparent(moved,parent,world::ReparentRule::keep_world,live_index(op.at("index"))));
                if(const auto& space=op.at("space");!space.is_null()) {
                    fields(space,{"position","rotation","scale","origin"});
                    relocated(world.set_transform(moved,json_trs(space)));
                }
            } else if(kind=="world") {
                fields(op,{"op","world"});load.policy=LoadPolicy::strict;
                take(world.set_lighting_settings(load_world(Json{{"world",op.at("world")}},load)));
            } else if(kind=="materials") {
                fields(op,{"op","materials"});
                const auto& definitions=op.at("materials");
                if(!definitions.is_array()) throw std::runtime_error("Expected a material definition list.");
                for(const auto& definition:definitions) {
                    fields(definition,{"id","source","inline","parent","overrides"});
                    const auto key=local_key(definition.at("id"));
                    auto restored=restore_material(session,&project,definition);
                    if(const auto live=live_object(session,key);live && *live!=ObjectId{}) {
                        if(!session.is_material(*live)) throw std::runtime_error("Document identity #"+std::to_string(key)+" is not a material in Play.");
                        if(restored.id()!=*live) take(material::update_material(runtime,*live,take(material::read_material(runtime,restored.id()))));
                    } else {
                        if(!session.local_ids_.emplace(restored.id(),key).second) throw std::runtime_error("One material asset has multiple document identities.");
                        session.live_objects_.insert_or_assign(key,restored.id());session.live_materials_.push_back(std::move(restored));
                    }
                }
            } else if(kind=="resource") {
                fields(op,{"op","key","material"});
                const auto key=op.at("key").get<std::string>();subject+=" "+key;
                auto asset=material_format::decode(op.at("material"));asset.key=key;
                const auto catalogued=[&](const std::string& texture){return std::ranges::find(project.entries(),texture,&ProjectEntry::key)!=project.entries().end();};
                if(std::ranges::any_of(asset.textures,[&](const auto& texture){return !texture.empty() && !catalogued(texture);})) take(project.refresh());
                if(const auto at=session.materials_.find(key);at!=session.materials_.end()) {
                    material::State state;state.source=key;state.description=asset.description;
                    for(std::size_t i=0;i<state.textures.size();++i) if(!asset.textures[i].empty()) {
                        state.textures[i]=take(session.texture_resource(project,asset.textures[i]));
                        state.description.textures[i].image=take(material::read_texture(runtime,state.textures[i].id())).image;
                    }
                    take(material::update_material(runtime,at->second.root.id(),state));
                    session.unavailable_.erase(key);
                } else take(session.material_resource(project,asset));
            } else throw std::runtime_error("Unknown change: "+kind);
            for(const auto key:load.dropped) note(world::Severity::warning,world::DiagnosticKind::reference_dropped,
                "#"+std::to_string(key)+" was destroyed by the game; the reference is null.");
        } catch(const Gone& error) {
            note(world::Severity::warning,world::DiagnosticKind::reference_dropped,std::string(error.what())+" The change is skipped.");
        } catch(const Unsupported& error) {
            note(world::Severity::warning,world::DiagnosticKind::command_rejected,std::string(error.what())+" The change is skipped.");
        } catch(const std::exception& error) {
            note(world::Severity::error,world::DiagnosticKind::command_rejected,error.what());
        }
    }
};

Result<std::string> Session::serialize() const {
    try {
        if(editing()) throw std::runtime_error("Finish the active edit before saving.");std::vector<ObjectId> roots;
        for(const auto& member:take(world_.members()))
            if(take(world::parent_entity(world_.runtime(),member.id()))==ObjectId{}) roots.push_back(member.id());
        auto graph=GraphCodec::capture(*this,roots,true,true);auto text=graph.data.dump(2)+"\n";
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
Result<std::string> Session::serialize_live_subtree(ObjectId root) const {
    try {
        auto graph=GraphCodec::capture(*this,{&root,1},true,false,true);auto text=graph.data.dump();
        if(text.size()>json_format::maximum_bytes) throw std::runtime_error("The Entity exceeds the 16 MiB change limit.");return text;
    } catch(const std::exception& error) {return failed(error);}
}
Result<std::string> Session::serialize_live_components(ObjectId entity,std::span<const ObjectId> components) const {
    try {
        for(const auto component:components)
            if(take(world::inspect_component(world_.runtime(),component)).owner!=entity) throw std::runtime_error("The Component does not belong to the Entity.");
        auto graph=GraphCodec::capture(*this,{&entity,1},true,false,true);auto& data=graph.data;
        std::unordered_set<std::string> wanted;
        for(const auto component:components) wanted.insert(std::to_string(local_ids_.at(component)));
        const auto key=std::to_string(local_ids_.at(entity));
        const auto& records=data.at("entities");
        const auto record=std::ranges::find_if(records,[&](const Json& value){return value.at("id")==key;});
        Json parts=Json::array();std::unordered_set<std::string> used;
        for(auto part:record->at("components")) if(wanted.contains(part.at("id").get<std::string>())) {
            if(const auto at=part.find("materials");at!=part.end()) for(const auto& entry:*at) used.insert(entry.at("resource").get<std::string>());
            auto references=part;each_reference(references,[&](Json& node){if(const auto at=node.find("$ref");at!=node.end()) used.insert(at->get<std::string>());});
            parts.push_back(std::move(part));
        }
        if(parts.size()!=components.size()) throw std::runtime_error("Each listed Component must appear once on the Entity.");
        Json materials=Json::array();
        for(const auto& definition:data.at("materials")) if(used.contains(definition.at("id").get<std::string>())) materials.push_back(definition);
        data["roots"]=Json::array();data["materials"]=std::move(materials);
        data["entities"]=Json::array({Json{{"id",key},{"existing",true},{"children",Json::array()},{"components",std::move(parts)}}});
        auto text=data.dump();
        if(text.size()>json_format::maximum_bytes) throw std::runtime_error("The Components exceed the 16 MiB change limit.");return text;
    } catch(const std::exception& error) {return failed(error);}
}
void Session::enter_play() {
    play_=true;
    for(const auto& [object,key]:local_ids_) if(runtime_.type_of(object)) live_objects_.insert_or_assign(key,object);
}
Result<std::vector<world::Diagnostic>> Session::apply_live(std::string_view text,Project& project) {
    try {
        if(!play_) throw std::runtime_error("Editor changes apply only to a Play copy.");
        if(world_.closed()) throw std::runtime_error("The Play World is closed.");
        const auto batch=json_format::parse(text);fields(batch,{"ops"});
        const auto& ops=batch.at("ops");if(!ops.is_array()) throw std::runtime_error("Expected a change list.");
        std::vector<world::Diagnostic> result;
        for(std::size_t i=0;i<ops.size();++i) GraphCodec::apply_change(*this,project,ops[i],i,result);
        return result;
    } catch(const std::exception& error) {return failed(error);}
}
Result<std::unique_ptr<Session>> Session::deserialize(std::string_view text,Project& project,Prepare prepare,LoadPolicy policy) {
    try {
        GraphCodec::Graph graph;graph.data=migrate_scene(json_format::parse(text));auto session=std::make_unique<Session>(std::move(prepare));
        GraphCodec::Load load;load.policy=policy;load.document=true;
        take(session->world_.set_lighting_settings(GraphCodec::load_world(graph.data,load)));
        auto prepared=GraphCodec::construct(*session,graph,&project,true,load);
        for(const auto& [object,label]:prepared.labels) session->names_.emplace(object,label);
        if(!prepared.roots.empty()) {
            const auto plans=GraphCodec::attachments(prepared,graph,{},false);GraphCodec::prepare_cook(*session,prepared,plans,load);
            const auto commit=take(session->world_.attach_batch(plans,prepared.spaces));prepared.published=commit.changed;
            if(commit.notification_error) throw std::runtime_error(commit.notification_error->message);
        }
        prepared.published=true;
        session->retained_=std::move(load.retained);
        session->retained_document_.record=dump_retained(load.top);session->retained_document_.materials=std::move(load.unreferenced);
        session->load_issues_=std::move(load.issues);
        session->next_name_=graph.data.at("entities").size()+1;return session;
    } catch(const std::exception& error) {return failed(error);}
}
Result<world::Commit> Session::instantiate_subtree(std::string_view text,Project& project,ObjectId parent) {
    try {
        if(editing() || world_.running()) throw std::runtime_error("Composition import requires a stopped document without an active edit.");
        if(parent!=ObjectId{}) take(admit_entity_edit(parent));
        GraphCodec::Graph graph;graph.data=migrate_scene(json_format::parse(text));
        GraphCodec::Load load;
        auto prepared=GraphCodec::construct(*this,graph,&project,false,load);
        if(prepared.roots.empty()) return world::Commit{};return GraphCodec::publish(*this,prepared,graph,parent,false,load);
    } catch(const std::exception& error) {return failed(error);}
}
Result<world::Commit> Session::duplicate_selected() {
    try {
        if(editing()) throw std::runtime_error("Finish the active edit before duplicating.");if(selected_entities_.empty()) return world::Commit{};
        auto graph=GraphCodec::capture(*this,selected_entities_,false);GraphCodec::Load load;
        auto prepared=GraphCodec::construct(*this,graph,nullptr,false,load);
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
        return GraphCodec::publish(*this,prepared,graph,{},true,load);
    } catch(const std::exception& error) {return failed(error);}
}
std::vector<Session::RetainedSwap> Session::release(const Change& change) {
    std::vector<RetainedSwap> swaps;
    if(retained_.empty() && retained_document_.record.empty()) return swaps;
    const auto entry_of=[&](ObjectId object)->Retained* {
        if(object==ObjectId{}) return &retained_document_;
        const auto at=retained_.find(object);return at==retained_.end()?nullptr:&at->second;
    };
    const auto drop=[&](ObjectId object,std::span<const std::string> path) {
        auto* entry=entry_of(object);if(!entry || entry->record.empty()) return;
        auto record=Json::parse(entry->record);if(!erase_path(record,path)) return;
        auto after=*entry;after.record=dump_retained(record);
        swaps.push_back({object,*entry,after});*entry=std::move(after);
    };
    const auto drop_slots=[&](ObjectId object) {
        auto* entry=entry_of(object);if(!entry || entry->slots.empty()) return;
        auto after=*entry;after.slots.clear();
        swaps.push_back({object,*entry,after});*entry=std::move(after);
    };
    // Field paths map to scene record paths: business fields live under
    // properties; a StaticMeshComponent's material fields are mesh payload keys.
    const auto field=[&](ObjectId object,std::span<const FieldId> path) {
        if(!retained_.contains(object)) return;
        const auto type=runtime_.type_of(object);if(!type || path.empty()) return;
        std::vector<std::string> names{"properties"};auto owner=*type;
        for(const auto id:path) {
            const auto info=runtime_.describe(owner);if(!info) return;
            const auto at=std::ranges::find((*info)->fields,id,&FieldInfo::id);if(at==(*info)->fields.end()) return;
            names.emplace_back(at->name);owner=at->value.type;
        }
        const auto mesh=runtime_.is_a(*type,types_.static_mesh_component);
        if(mesh && *mesh && names[1]=="material" && names.size()>2) drop(object,std::span(names).subspan(2,1));
        else drop(object,names);
    };
    static const std::array<std::string,1> space{"space"},name{"name"},world_path{"world"};
    static const std::array<std::string,2> body{"properties","body"};
    std::visit([&](const auto& item) {
        using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,LightingChange>) drop({},world_path);
        else if constexpr(std::is_same_v<T,FieldChange>) {
            if(std::holds_alternative<world::Trs>(item.after)) drop(item.root.id(),space);else field(item.root.id(),item.path);
        } else if constexpr(std::is_same_v<T,FieldGroupChange>) {for(const auto& path:item.paths) field(item.root.id(),path);}
        else if constexpr(std::is_same_v<T,NameChange>) drop(item.root.id(),name);
        else if constexpr(std::is_same_v<T,BodyChange>) drop(item.root.id(),body);
        else if constexpr(std::is_same_v<T,TransformChange>) {for(const auto& entry:item.entries) drop(entry.root.id(),space);}
        else if constexpr(std::is_same_v<T,ReparentChange>) {drop(item.root.id(),space);for(const auto& entry:item.transforms) drop(entry.root.id(),space);}
        else if constexpr(std::is_same_v<T,TransformPresenceChange> || std::is_same_v<T,SpaceReferenceChange>) drop(item.root.id(),space);
        else if constexpr(std::is_same_v<T,MaterialBindingChange>) drop_slots(item.root.id());
    },change);
    return swaps;
}
void Session::restore(std::span<const RetainedSwap> swaps,bool forward) {
    const auto apply=[&](const RetainedSwap& swap) {
        const auto& value=forward?swap.after:swap.before;
        if(swap.object==ObjectId{}) retained_document_=value;
        else if(value.empty()) retained_.erase(swap.object);
        else retained_[swap.object]=value;
    };
    if(forward) for(const auto& swap:swaps) apply(swap);
    else for(auto at=swaps.rbegin();at!=swaps.rend();++at) apply(*at);
}
Result<void> Session::discard_retained(ObjectId object) {
    const auto reject=[](const char* message){return std::unexpected(Error{ErrorCode::invalid_definition,message});};
    if(editing()) return reject("Finish the active edit before discarding saved data.");
    if(world_.running()) return reject("Stop the World before discarding saved data.");
    Retained before;
    if(object==ObjectId{}) before=retained_document_;
    else {
        if(auto ready=admit_property_edit(object);!ready) return std::unexpected(ready.error());
        const auto at=retained_.find(object);if(at==retained_.end()) return {};
        before=at->second;
    }
    if(before.record.empty() && before.slots.empty() && before.materials.empty()) return {};
    Retained after;after.type=before.type;
    RetainedSwap swap{object,std::move(before),std::move(after)};
    restore({&swap,1},true);
    remember(RetainedChange{});history_.back().retained.push_back(std::move(swap));
    return {};
}
bool Session::has_retained(ObjectId object) const noexcept {
    if(object==ObjectId{}) return !retained_document_.record.empty() || !retained_document_.materials.empty();
    return retained_.contains(object);
}
std::size_t Session::placeholder_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(retained_,[&](const auto& item){
        return !item.second.type.empty() && runtime_.type_of(item.first).has_value();}));
}
std::optional<std::uint64_t> Session::document_identity(ObjectId object) const {
    const auto at=local_ids_.find(object);
    return at==local_ids_.end()?std::nullopt:std::optional(at->second);
}
Result<RetainedView> Session::retained(ObjectId object) const {
    try {
        RetainedView view;
        const Retained* entry=&retained_document_;
        if(object!=ObjectId{}) {const auto at=retained_.find(object);if(at==retained_.end()) return view;entry=&at->second;}
        view.type=entry->type;
        const bool document=object==ObjectId{},material=!document && is_material(object);
        std::vector<detail::ScalarField> schema;
        if(!document && !material) schema=business_fields(world_.runtime(),types_,take(runtime_.type_of(object)));
        const MissingResource* mesh_unavailable{};
        if(!document && !material) if(const auto mesh=world::read_static_mesh(world_.runtime(),object); mesh && !mesh->mesh.resolved)
            mesh_unavailable=unavailable(mesh->mesh.source);
        // Descends into records the current schema still declares; anything else is one entry.
        const auto classify=[&](std::span<const std::string> path)->std::pair<bool,world::DiagnosticKind> {
            using enum world::DiagnosticKind;
            if(document) {
                if(path.front()!="world") return {false,unknown_key};
                if(path.size()==1) return {true,mismatch};
                const auto* option=view_option(path[1]);
                return {false,option && option->name==path[1] && world_option(option->option)?mismatch:unknown_key};
            }
            if(material) return {false,unknown_key};
            if(path.front()=="properties") {
                if(path.size()==1) return {true,mismatch};
                const auto names=path.subspan(1);bool exact{},prefix{};
                for(const auto& field:schema) {
                    if(std::ranges::equal(field.names,names)) exact=true;
                    else if(field.names.size()>names.size() && std::equal(names.begin(),names.end(),field.names.begin())) prefix=true;
                }
                return {!exact && prefix,exact || prefix?mismatch:unknown_field};
            }
            if(path.front()=="space") {
                if(path.size()==1) return {true,mismatch};
                return {false,path[1]=="position" || path[1]=="rotation" || path[1]=="scale" || path[1]=="origin"?mismatch:unknown_key};
            }
            if(path.front()=="materials" && mesh_unavailable) return {false,mesh_unavailable->kind};
            constexpr std::array known{"name","enabled","mesh","base_color","roughness","metallic","emissive","materials"};
            return {false,std::ranges::find(known,path.front())!=known.end()?mismatch:unknown_key};
        };
        std::vector<std::string> path;
        const auto walk=[&](auto&& self,const Json& value)->void {
            for(const auto& [name,item]:value.items()) {
                path.push_back(name);
                const auto [record,kind]=classify(path);
                if(record && item.is_object() && !reference_value(item)) self(self,item);
                else view.entries.push_back({path,preview(item),kind});
                path.pop_back();
            }
        };
        if(!entry->record.empty()) walk(walk,Json::parse(entry->record));
        if(!entry->slots.empty()) for(const auto& [slot,keys]:Json::parse(entry->slots).items())
            for(const auto& [key,value]:keys.items())
                view.entries.push_back({{"materials",slot,key},preview(value),key=="overridden"?world::DiagnosticKind::mismatch:world::DiagnosticKind::unknown_key});
        if(document) for(const auto& resource:entry->materials) if(runtime_.type_of(resource.id())) {
            const auto key=document_identity(resource.id());
            view.entries.push_back({{"materials",key?std::to_string(*key):std::string("?")},
                preview(take(material::read_material(world_.runtime(),resource.id())).source),world::DiagnosticKind::unknown_key});
        }
        return view;
    } catch(const std::exception& error) {return failed(error);}
}
std::vector<world::Diagnostic> Session::issues() const {
    std::vector<world::Diagnostic> result;
    std::unordered_set<ObjectId> members;
    if(const auto list=world_.members()) for(const auto& member:*list) members.insert(member.id());
    const auto owner=[&](ObjectId object)->ObjectId {
        if(members.contains(object)) return object;
        const auto component=world::inspect_component(world_.runtime(),object);
        return component && component->valid && members.contains(component->owner)?component->owner:ObjectId{};
    };
    std::unordered_map<std::uint64_t,ObjectId> by_key;
    for(const auto& [object,key]:local_ids_) by_key.emplace(key,object);
    const auto add=[&](ObjectId object,std::string type,world::DiagnosticKind kind,std::vector<std::string> field,std::string value,std::string detail) {
        world::Diagnostic item;item.serial=result.size()+1;item.severity=world::Severity::warning;item.kind=kind;item.phase=world::DiagnosticPhase::load;
        item.object=object;item.entity=object==ObjectId{}?ObjectId{}:owner(object);item.type=std::move(type);
        auto subject=object==ObjectId{}?std::string("Scene"):item.type;
        if(const auto key=document_identity(object);object!=ObjectId{} && key) subject+=" #"+std::to_string(*key);
        item.message=subject+(field.empty()?std::string{}:" "+dotted(field))+": "+detail;
        item.field=std::move(field);item.value=std::move(value);result.push_back(std::move(item));
    };
    const auto report=[&](ObjectId object,const Retained& entry,std::string type) {
        if(!entry.type.empty()) add(object,type,world::DiagnosticKind::unknown_type,{},{},"The type is not registered; a placeholder keeps its saved data.");
        const auto view=retained(object);if(!view) return;
        for(const auto& item:view->entries) add(object,type,item.kind,item.path,item.value,
            item.kind==world::DiagnosticKind::mismatch?"The saved value does not fit the current type; it is kept until edited or discarded.":
            item.kind==world::DiagnosticKind::missing_resource || item.kind==world::DiagnosticKind::unreadable_resource?
            "The resource it needs is unavailable; the saved value is kept unchanged.":"Saved data kept for the next save.");
        for(const auto* text:{&entry.record,&entry.slots}) if(!text->empty()) {
            auto value=Json::parse(*text);
            each_reference(value,[&](Json& node) {
                const auto at=node.find("$ref");if(at==node.end()) return;
                const auto found=by_key.find(local_key(*at));
                const bool live=found!=by_key.end() && runtime_.type_of(found->second) && (owner(found->second)!=ObjectId{} || is_material(found->second));
                if(!live) add(object,type,world::DiagnosticKind::reference_dropped,{},node.dump(),"A saved reference targets a deleted object; it will be saved as null.");
            });
        }
    };
    try {
        for(const auto& [object,entry]:retained_) {
            if(!runtime_.type_of(object) || (owner(object)==ObjectId{} && !is_material(object))) continue;
            report(object,entry,entry.type.empty()?type_name(world_.runtime(),take(runtime_.type_of(object))):entry.type);
        }
        report({},retained_document_,{});
    } catch(const std::exception&) {}
    return result;
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

#include "editor/session.hpp"
#include "editor/fields.hpp"
#include "physics/physics.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace atelier::editor {
namespace {
auto invalid(const char* message) { return std::unexpected(Error{ErrorCode::invalid_definition, message}); }
bool contains_member(const world::World& world, ObjectId entity) {
    const auto members=world.members();
    return members && std::ranges::any_of(*members,[&](const auto& member){return member.id()==entity;});
}
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
}
Session::Session(Prepare prepare) : types_(take(world::register_types(runtime_))),
    physics_types_(take(physics::register_types(runtime_))), world_(runtime_, types_),
    parameters_{Parameter{"X", "m", types_.position_fields[0]}, Parameter{"Y", "m", types_.position_fields[1]},
                Parameter{"Z", "m", types_.position_fields[2]}} {
    static_assert(std::is_nothrow_move_constructible_v<Change> && std::is_nothrow_move_assignable_v<Change>);
    history_.reserve(history_limit + 1);
    if (prepare) {
        auto ready = prepare(world_);
        if (!ready) throw std::runtime_error(ready.error().message);
        prepared_ = std::move(*ready);
    }
}
Session::~Session() { (void)world_.close(); }
void Session::remember(Change change) {
    ++revision_;
    if (saved_cursor_ && *saved_cursor_>cursor_) saved_cursor_.reset();
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    auto released=release(change);
    history_.push_back(Entry{std::move(change),std::move(released)});
    if (history_.size() > history_limit) {
        history_.erase(history_.begin());
        if (saved_cursor_) { if (*saved_cursor_) --*saved_cursor_; else saved_cursor_.reset(); }
    }
    cursor_ = history_.size();
}
std::wstring_view Session::name(ObjectId entity) const noexcept {
    const auto at=names_.find(entity);
    return at==names_.end()?std::wstring_view{}:at->second;
}
Result<void> Session::admit_entity_edit(ObjectId entity) const {
    auto state = world::inspect_entity(world_.runtime(), entity);
    if (!state) return std::unexpected(state.error());
    if (!state->can_edit || !names_.contains(entity)) return invalid("The Entity is not editable in this document.");
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    if (std::ranges::none_of(*members, [entity](const auto& member) { return member.id() == entity; }))
        return invalid("The Entity is not a member of this document.");
    return {};
}
Result<world::ComponentStatus> Session::admit_component_edit(ObjectId component) const {
    auto state = world::inspect_component(world_.runtime(), component);
    if (!state) return std::unexpected(state.error());
    if (!state->valid || state->owner == ObjectId{}) return invalid("The Component is not attached to an editable Entity.");
    if (auto ready = admit_entity_edit(state->owner); !ready) return std::unexpected(ready.error());
    return state;
}
Result<void> Session::validate_name(std::wstring_view name) {
    if (name.empty() || name.size()>256 || std::all_of(name.begin(),name.end(),[](wchar_t c){return c==L' ';}) ||
        std::any_of(name.begin(),name.end(),[](wchar_t c){return c<32 || c==127;}))
        return invalid("Use a name with 1–256 visible characters.");
    return {};
}
Result<void> Session::set_world_lighting(const rendering::SceneLightingSettings& settings) {
    const auto before=world_.lighting_settings();
    if(before==settings) return world_.set_lighting_settings(settings);
    if(editing()) return invalid("Finish the current property edit before changing World settings.");
    if(auto changed=world_.set_lighting_settings(settings);!changed) return changed;
    remember(LightingChange{before,settings});return {};
}
Result<void> Session::rename(ObjectId entity, std::wstring name) {
    if (auto valid=validate_name(name);!valid) return valid;
    if (auto ready = admit_entity_edit(entity); !ready) return ready;
    const auto at=names_.find(entity);
    if (at==names_.end()) return invalid("The Entity does not belong to this document.");
    if (at->second==name) return {};
    auto root=runtime_.hold(entity);
    if (!root) return std::unexpected(root.error());
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    NameChange change{std::move(*root),at->second,name};
    at->second=std::move(name); remember(std::move(change));
    return {};
}
void Session::set_selection(Selection next) {
    if(selection_!=next || selected_entities_.size()!=(next.entity==ObjectId{}?0u:1u)) ++revision_;
    selection_=next;
    selected_entities_.clear();
    if (next.entity!=ObjectId{}) selected_entities_.push_back(next.entity);
}
void Session::forget_entity(ObjectId entity) noexcept {
    const auto old_size=selected_entities_.size();
    std::erase_if(selected_entities_,[&](ObjectId id){return id==entity || !contains_member(world_,id);});
    if(old_size!=selected_entities_.size() || selection_.entity==entity) ++revision_;
    if (selection_.entity==entity || (selection_.entity!=ObjectId{} && !contains_member(world_,selection_.entity))) {
        selection_={};
        if (!selected_entities_.empty()) selection_={selected_entities_.back(),selected_entities_.back()};
    }
}
Result<void> Session::select(ObjectId object) {
    Selection next;
    if (object != ObjectId{}) {
        auto entity=admit_property_edit(object);if(!entity) return std::unexpected(entity.error());
        next={*entity,object};
    }
    if(next==selection_ && selected_entities_.size()==(next.entity==ObjectId{}?0u:1u)) return {};
    auto cancelled = cancel_edit();
    if (!cancelled) return cancelled;
    set_selection(next);
    return {};
}
Result<void> Session::select_entities(std::span<const ObjectId> entities, ObjectId active) {
    std::vector<ObjectId> next;
    next.reserve(entities.size());
    for (const auto entity:entities) {
        if (std::ranges::find(next,entity)!=next.end()) continue;
        if (auto ready=admit_entity_edit(entity); !ready) return ready;
        next.push_back(entity);
    }
    if (active==ObjectId{} || std::ranges::find(next,active)==next.end()) active=next.empty()?ObjectId{}:next.back();
    else { std::erase(next,active); next.push_back(active); }
    if (next==selected_entities_ && selection_==Selection{active,active}) return {};
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    selected_entities_=std::move(next);
    selection_={active,active};
    ++revision_;
    return {};
}
Result<world::Commit> Session::add_entity(TypeId actual) {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    if (members->size() >= maximum_entities) return invalid("Editor Entity limit reached.");
    auto root = world::create_entity(runtime_, actual == TypeId{} ? types_.entity : actual);
    if (!root) return std::unexpected(root.error());
    return attach_created(std::move(*root),L"Entity");
}
Result<void> Session::set_transform(ObjectId entity,world::Trs value) {
    if(auto valid=admit_entity_edit(entity);!valid) return valid;
    auto canonical=world::canonical_transform(value);if(!canonical) return std::unexpected(canonical.error());
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    const bool spatial=take(world::has_transform(runtime_,entity));
    const auto before=spatial?std::optional(take(world::read_transform(runtime_,entity))):std::nullopt;
    const auto before_origin=take(world::space_reference(runtime_,entity));
    if(before && *before==*canonical) return {};
    auto root=runtime_.hold(entity);if(!root) return std::unexpected(root.error());
    if(auto written=world_.set_transform(entity,*canonical);!written) return written;
    remember(TransformPresenceChange{std::move(*root),before,*canonical,before_origin,take(world::space_reference(runtime_,entity))});return {};
}
Result<void> Session::remove_transform(ObjectId entity) {
    if(auto valid=admit_entity_edit(entity);!valid) return valid;
    if(auto cancelled=cancel_edit();!cancelled) return cancelled;
    if(!take(world::has_transform(runtime_,entity))) return {};
    const auto before=take(world::read_transform(runtime_,entity));
    const auto before_origin=take(world::space_reference(runtime_,entity));
    auto root=runtime_.hold(entity);if(!root) return std::unexpected(root.error());
    if(auto removed=world_.remove_transform(entity);!removed) return removed;
    remember(TransformPresenceChange{std::move(*root),before,{},before_origin,take(world::space_reference(runtime_,entity))});return {};
}
Result<world::Commit> Session::set_space_reference(ObjectId entity,std::optional<ObjectId> origin,world::ReparentRule rule) {
    if(auto valid=admit_entity_edit(entity);!valid) return std::unexpected(valid.error());
    if(origin && *origin!=ObjectId{}) if(auto valid=admit_entity_edit(*origin);!valid) return std::unexpected(valid.error());
    if(auto cancelled=cancel_edit();!cancelled) return std::unexpected(cancelled.error());
    SpaceReferenceChange change;change.root=take(runtime_.hold(entity));change.before_origin=take(world::space_reference(runtime_,entity));
    change.spatial=take(world::has_transform(runtime_,entity));if(change.spatial) change.before=take(world::read_transform(runtime_,entity));
    auto changed=world_.set_space_reference(entity,origin,rule);if(!changed) return changed;
    if(changed->changed) {
        change.after_origin=take(world::space_reference(runtime_,entity));if(change.spatial) change.after=take(world::read_transform(runtime_,entity));
        set_selection({entity,entity});remember(std::move(change));
    }
    return changed;
}
Result<world::Commit> Session::reparent(ObjectId entity,ObjectId parent,world::ReparentRule rule,std::size_t index) {
    if(auto valid=admit_entity_edit(entity);!valid) return std::unexpected(valid.error());
    if(parent!=ObjectId{}) if(auto valid=admit_entity_edit(parent);!valid) return std::unexpected(valid.error());
    if(auto cancelled=cancel_edit();!cancelled) return std::unexpected(cancelled.error());
    const auto sibling_index=[&](ObjectId target,ObjectId owner) {
        std::size_t result{};
        if(owner!=ObjectId{}) for(const auto& child:take(world::children(runtime_,owner))) {
            if(child.id()==target) return result;++result;
        } else for(const auto& member:take(world_.members())) if(take(world::parent_entity(runtime_,member.id()))==ObjectId{}) {
            if(member.id()==target) return result;++result;
        }
        return result;
    };
    ReparentChange change;change.root=take(runtime_.hold(entity));change.before_parent=take(world::parent_entity(runtime_,entity));
    change.before_index=sibling_index(entity,change.before_parent);
    if(rule==world::ReparentRule::keep_world) {
        std::vector<ObjectId> pending{entity};
        while(!pending.empty()) {
            const auto object=pending.back();pending.pop_back();
            if(take(world::has_transform(runtime_,object))) {
                const auto before=take(world::read_transform(runtime_,object));
                change.transforms.push_back({take(runtime_.hold(object)),before,before});
            }
            for(const auto& child:take(world::children(runtime_,object))) pending.push_back(child.id());
        }
    }
    auto changed=world_.reparent(entity,parent,rule,index);if(!changed) return changed;
    if(changed->changed) {
        change.after_parent=take(world::parent_entity(runtime_,entity));change.after_index=sibling_index(entity,change.after_parent);
        for(auto& transform:change.transforms) transform.after=take(world::read_transform(runtime_,transform.root.id()));
        std::erase_if(change.transforms,[](const auto& transform){return transform.before==transform.after;});
        set_selection({entity,entity});remember(std::move(change));
    }
    return changed;
}
Result<world::Commit> Session::create_static_mesh(const MeshAsset& asset, math::Vector3 position) {
    auto cancelled=cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto members=world_.members();
    if (!members) return std::unexpected(members.error());
    if (members->size()>=maximum_entities) return invalid("Editor Entity limit reached.");
    world::Material material;
    if(asset.mesh && asset.mesh->has_materials()) material.base_color={1,1,1};
    auto root=world::create_static_mesh_entity(world_,{asset.mesh,{position,{},{1.f,1.f,1.f}},material,asset.key});
    if (!root) return std::unexpected(root.error());
    return attach_created(std::move(*root),asset.name);
}
Result<world::Commit> Session::create_light(world::LightKind kind,math::Vector3 position) {
    auto cancelled=cancel_edit();if(!cancelled) return std::unexpected(cancelled.error());
    auto members=world_.members();if(!members) return std::unexpected(members.error());
    if(members->size()>=maximum_entities) return invalid("Editor Entity limit reached.");
    auto entity=world::create_entity(runtime_);if(!entity) return std::unexpected(entity.error());
    auto light=world::create_component(runtime_,types_.light_component);if(!light) return std::unexpected(light.error());
    auto configured=runtime_.with_native<world::LightComponent>(light->id(),[&](auto& value)->Result<void> {
        value.settings.kind=kind;
        if(kind==world::LightKind::sky) value.settings.intensity=.03f;
        return world::validate_light(value.settings);
    });
    if(!configured) return std::unexpected(configured.error());
    if(kind!=world::LightKind::sky) {
        auto positioned=world_.set_transform(entity->id(),{position,{},{1,1,1}});if(!positioned) return std::unexpected(positioned.error());
    }
    const std::array parts{world::Attachment{light->id()}};
    auto attached=world_.attach_components(entity->id(),parts);if(!attached) return attached;
    if(attached->notification_error) return std::unexpected(*attached->notification_error);
    const auto label=kind==world::LightKind::point?L"Point Light":kind==world::LightKind::rectangle?L"Rectangle Light":
        kind==world::LightKind::directional?L"Directional Light":L"Sky Light";
    return attach_created(std::move(*entity),label);
}
Result<world::Commit> Session::attach_created(ObjectRoot root,std::wstring_view label) {
    EntityChange change;
    change.added=true; change.removals.emplace_back().root=std::move(root);
    const auto id=change.removals.front().id();
    names_.emplace(id,std::wstring(label)+L" "+std::to_wstring(next_name_++));
    auto attached = world_.attach(id);
    if (!attached) { names_.erase(id); return attached; }
    if (attached->changed) {
        set_selection({id,id});
        remember(std::move(change));
    }
    return attached;
}
Result<world::Commit> Session::erase_selected() {
    if(selection_.object==ObjectId{}) return world::Commit{};
    if(selection_.object!=selection_.entity) return remove_component(selection_.object);
    return erase_entities();
}
Result<world::Commit> Session::erase_entities() {
    if (selected_entities_.empty()) return world::Commit{};
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    std::vector<ObjectId> targets;
    for(const auto entity:selected_entities_) {
        auto parent=take(world::parent_entity(runtime_,entity));bool included{};
        while(parent!=ObjectId{}) {if(is_selected(parent)) {included=true;break;}parent=take(world::parent_entity(runtime_,parent));}
        if(!included) targets.push_back(entity);
    }
    auto removed=world_.detach_batch(targets);
    if(!removed) return std::unexpected(removed.error());
    EntityChange change;
    change.removals=std::move(*removed);
    world::Commit result;
    for (auto& removal:change.removals) {
        result.changed|=removal.commit.changed;
        if (removal.commit.notification_error && !result.notification_error) result.notification_error=std::move(removal.commit.notification_error);
        removal.commit.notification_error.reset();
    }
    if (result.changed) {
        remember(std::move(change));
        set_selection({});
    }
    return result;
}
Result<world::Commit> Session::add_component(TypeId type) {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    const auto entity=selection_.entity;
    if (auto ready = admit_entity_edit(entity); !ready) return std::unexpected(ready.error());
    auto supported = runtime_.is_a(type, types_.component);
    if (!supported) return std::unexpected(supported.error());
    if (!*supported) return invalid("Adding a component requires a Component type.");
    const auto members=world::components(runtime_,entity);
    if (!members) return std::unexpected(members.error());
    if (members->size()>=maximum_components) return invalid("Editor Component limit reached.");
    auto root = world::create_component(runtime_, type);
    if (!root) return std::unexpected(root.error());
    ComponentChange change;
    change.added = true;
    change.removal.root = std::move(*root);
    change.removal.owner = entity;
    const auto spatial=world::component_requires_space(runtime_,change.removal.id());
    if(!spatial) return std::unexpected(spatial.error());
    auto has_space=world::has_transform(runtime_,entity);if(!has_space) return std::unexpected(has_space.error());
    if(*spatial && !*has_space) {
        auto positioned=world_.set_transform(entity,{});if(!positioned) return std::unexpected(positioned.error());
        change.created_transform=true;
    }
    const std::array attachments{world::Attachment{change.removal.id()}};
    auto attached = world_.attach_components(entity, attachments);
    if (!attached) {
        if(change.created_transform) (void)world_.remove_transform(entity);
        return attached;
    }
    if (attached->changed) {set_selection({entity,change.removal.id()});remember(std::move(change));}
    return attached;
}
Result<world::Commit> Session::remove_component(ObjectId component) {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (auto ready = admit_component_edit(component); !ready) return std::unexpected(ready.error());
    auto removed = world_.remove_component(component);
    if (!removed) return std::unexpected(removed.error());
    auto result = std::move(removed->commit);
    if (result.changed) {
        if(selection_.object==component) set_selection({removed->owner,removed->owner});
        remember(ComponentChange{false, std::move(*removed)});
    }
    return result;
}
Result<ObjectId> Session::admit_property_edit(ObjectId object) const {
    if (world_.running() || world_.closed()) return invalid("Properties require an open, stopped document.");
    auto type = runtime_.type_of(object);
    if (!type) return std::unexpected(type.error());
    auto entity = runtime_.is_a(*type,types_.entity);
    if (!entity) return std::unexpected(entity.error());
    if (*entity) {
        if (auto ready=admit_entity_edit(object); !ready) return std::unexpected(ready.error());
        return object;
    }
    auto component=admit_component_edit(object);
    if (!component) return std::unexpected(component.error());
    return component->owner;
}
Result<std::vector<ComponentType>> Session::component_types() const {
    std::vector<ComponentType> result;
    for (const auto type:runtime_.types()) {
        auto info=runtime_.describe(type); if (!info) return std::unexpected(info.error());
        if ((*info)->kind!=TypeKind::object) continue;
        auto matches=runtime_.is_a(type,types_.component);
        if (!matches) return std::unexpected(matches.error());
        if (*matches) result.push_back({type,std::string((*info)->name)});
    }
    return result;
}
Result<std::vector<Property>> Session::properties(ObjectId object) const {
    if (!is_material(object)) if (auto ready=admit_property_edit(object); !ready) return std::unexpected(ready.error());
    auto type=runtime_.type_of(object); if (!type) return std::unexpected(type.error());
    auto fields=detail::scalar_fields(world_.runtime(),*type); if (!fields) return std::unexpected(fields.error());
    std::vector<Property> result;
    const bool entity=take(runtime_.is_a(*type,types_.entity));
    const bool spatial=entity && take(world::has_transform(world_.runtime(),object));
    for (auto& field:*fields) {
        if(entity && !spatial && detail::transform_path(types_,field.path)) continue;
        auto value=read_property(object,field.path); if (!value) return std::unexpected(value.error());
        std::string name;
        for (const auto& part:field.names) { if (!name.empty()) name+='.'; name+=part; }
        result.push_back({std::move(name),std::move(field.path),field.type,std::move(*value),std::move(field.label),std::move(field.category),std::move(field.description)});
    }
    return result;
}
Result<Scalar> Session::read_property(ObjectId object,std::span<const FieldId> path) const {
    if(is_material(object)) {
        auto captured=material::capture(world_.runtime(),object);if(!captured) return std::unexpected(captured.error());
        return world_.runtime().read(object,path);
    }
    if (auto ready=admit_property_edit(object); !ready) return std::unexpected(ready.error());
    return world_.read(object,path);
}
Result<void> Session::configure_body(ObjectId component, physics::BodySettings settings) {
    if (auto cancelled=cancel_edit(); !cancelled) return cancelled;
    auto state=admit_component_edit(component);
    if (!state) return std::unexpected(state.error());
    auto physical=runtime_.is_a(state->type,types_.primitive_component);
    if (!physical) return std::unexpected(physical.error());
    if (!*physical)
        return invalid("Selected Component is not a physics body.");
    auto before=physics::read_settings(runtime_,component);
    if (!before) return std::unexpected(before.error());
    if (*before==settings) return {};
    auto held=runtime_.hold(component);
    if (!held) return std::unexpected(held.error());
    if (auto changed=physics::configure(runtime_,component,settings); !changed) return changed;
    remember(BodyChange{std::move(*held),state->owner,std::move(*before),std::move(settings)});
    return {};
}
Result<void> Session::begin_edit(ObjectId object, std::span<const FieldId> path, std::uint64_t control_session) {
    if (editing()) return invalid("A field edit is already active.");
    if (!control_session) return invalid("A field edit requires a control session.");
    if(is_material(object)) {
        auto value=runtime_.read(object,path);if(!value) return std::unexpected(value.error());
        auto begun=begin_material_edit(object,control_session);if(!begun) return begun;
        material_edit_->paths={{path.begin(),path.end()}};return {};
    }
    auto entity=admit_property_edit(object); if (!entity) return std::unexpected(entity.error());
    auto before=world_.read(object,path); if (!before) return std::unexpected(before.error());
    auto root=runtime_.hold(object); if (!root) return std::unexpected(root.error());
    std::variant<world::Trs,Scalar> value{*before};
    if (detail::transform_path(types_,path)) {
        auto transform=world::read_transform(runtime_,object);
        if (!transform) return std::unexpected(transform.error());
        value=*transform;
    }
    active_.emplace(FieldChange{std::move(*root),*entity,{path.begin(),path.end()},control_session,value,value});
    return {};
}
Result<void> Session::begin_group_edit(ObjectId object,std::span<const world::FieldPath> paths,std::uint64_t control_session) {
    if (editing()) return invalid("A field edit is already active.");
    if (!control_session || paths.empty()) return invalid("A group edit requires a control session and fields.");
    if(is_material(object)) {
        for(const auto& path:paths) {
            auto value=read_property(object,path);if(!value) return std::unexpected(value.error());
        }
        if(auto begun=begin_material_edit(object,control_session);!begun) return begun;
        material_edit_->paths.assign(paths.begin(),paths.end());return {};
    }
    auto entity=admit_property_edit(object); if (!entity) return std::unexpected(entity.error());
    auto root=runtime_.hold(object); if (!root) return std::unexpected(root.error());
    FieldGroupChange change{std::move(*root),*entity,{paths.begin(),paths.end()},control_session,{},{}};
    for (const auto& path:paths) {
        if (detail::transform_path(types_,path)) return invalid("Transform fields use the transform edit.");
        auto value=world_.read(object,path); if (!value) return std::unexpected(value.error());
        change.before.push_back(*value);
    }
    change.after=change.before;group_.emplace(std::move(change));return {};
}
Result<void> Session::preview_group_edit(std::uint64_t control_session,std::span<const Scalar> values) {
    if(material_editing(control_session)) {
        auto written=write_group(material_edit_->root.id(),material_edit_->paths,values);
        if(written) ++revision_;return written;
    }
    if (!group_ || group_->control_session!=control_session) return invalid("No matching group edit.");
    if (values.size()!=group_->paths.size()) return invalid("A group edit needs one value per field.");
    auto entity=admit_property_edit(group_->root.id()); if (!entity) return std::unexpected(entity.error());
    if (*entity!=group_->entity) return invalid("The property's owning Entity changed.");
    if(auto written=write_group(group_->root.id(),group_->paths,values);!written) return written;
    ++revision_;return {};
}
Result<void> Session::write_group(ObjectId object,std::span<const world::FieldPath> paths,std::span<const Scalar> values) {
    if(paths.empty() || values.size()!=paths.size()) return invalid("A group edit needs one value per field.");
    std::vector<Scalar> previous;previous.reserve(paths.size());
    for(std::size_t i=0;i<paths.size();++i) {
        if(!detail::finite_scalar(values[i])) return invalid("Property values must be finite.");
        auto value=read_property(object,paths[i]);if(!value) return std::unexpected(value.error());
        if(value->index()!=values[i].index()) return invalid("A group edit value changed its field type.");
        previous.push_back(*value);
    }
    // Material rollback also restores override bits and the inherited surface.
    if(is_material(object)) {
        auto before=material::read_material(runtime_,object);if(!before) return std::unexpected(before.error());
        for(std::size_t i=0;i<paths.size();++i) if(auto written=material::write_property(runtime_,object,paths[i],values[i]);!written) {
            auto restored=material::update_material(runtime_,object,*before);return restored?written:restored;
        }
        return {};
    }
    for(std::size_t i=0;i<paths.size();++i) if(auto written=world_.write(object,paths[i],values[i]);!written) {
        // Each World write rejects its own value without changing it. Restore
        // earlier lanes in reverse order, including repeated paths.
        for(auto lane=i;lane>0;) {
            --lane;if(auto restored=world_.write(object,paths[lane],previous[lane]);!restored) return restored;
        }
        return written;
    }
    return {};
}
Result<void> Session::apply_group(const FieldGroupChange& change,bool forward) {
    auto entity=admit_property_edit(change.root.id()); if (!entity) return std::unexpected(entity.error());
    if (*entity!=change.entity) return invalid("The property's owning Entity changed.");
    const auto& values=forward?change.after:change.before;
    return write_group(change.root.id(),change.paths,values);
}
bool Session::owns_edit(std::uint64_t control_session) const noexcept {
    return (group_ && group_->control_session == control_session) || (active_ && active_->control_session == control_session) ||
        (transform_ && transform_->control_session == control_session) || material_editing(control_session);
}
Result<void> Session::begin_transform(std::span<const ObjectId> entities, std::uint64_t control_session, bool amend_creation) {
    if (editing()) return invalid("A field edit is already active.");
    if (!control_session) return invalid("A transform edit requires a control session.");
    if (entities.empty()) return invalid("Select an Entity with a Transform.");
    TransformChange change{{},control_session,selection_};
    change.entries.reserve(entities.size());
    for (const auto entity:entities) {
        if (std::ranges::any_of(change.entries,[&](const auto& entry){return entry.root.id()==entity;}))
            return invalid("A transform edit names an Entity twice.");
        auto state=admit_entity_edit(entity);
        if (!state) return std::unexpected(state.error());
        auto value=world::read_transform(runtime_,entity);
        if (!value) return std::unexpected(value.error());
        auto root=runtime_.hold(entity);
        if (!root) return std::unexpected(root.error());
        change.entries.push_back({std::move(*root),entity,*value,*value});
    }
    if (amend_creation) {
        const auto* creation=cursor_ && cursor_==history_.size()?std::get_if<EntityChange>(&history_.back().change):nullptr;
        if (!creation || !creation->added || saved_cursor_==cursor_ ||
            std::ranges::any_of(change.entries,[&](const auto& entry) {
                return std::ranges::none_of(creation->removals,[&](const auto& item){return item.id()==entry.entity;});
            })) return invalid("Initial placement must belong to the current unsaved creation.");
        change.amend_creation=true;
    }
    transform_=std::move(change);
    return {};
}
Result<world::Trs> Session::original_transform(std::uint64_t control_session,ObjectId component) const {
    if(!transform_ || transform_->control_session!=control_session) return invalid("No matching transform edit.");
    for(const auto& entry:transform_->entries) if(entry.root.id()==component) return entry.before;
    return invalid("The Entity does not belong to this transform edit.");
}
Result<void> Session::preview_transform(std::uint64_t control_session, std::span<const world::Trs> values) {
    if (!transform_ || transform_->control_session!=control_session) return invalid("No matching transform edit.");
    for(const auto& value:values) for(std::size_t axis=0;axis<3;++axis)
        if(value.position[axis]<parameters_[axis].minimum || value.position[axis]>parameters_[axis].maximum ||
            value.scale[axis]<minimum_scale || value.scale[axis]>maximum_scale)
            return invalid("Transform value is outside the editor range.");
    return write_transforms(*transform_,values);
}
Result<void> Session::write_transforms(const TransformChange& change,std::span<const world::Trs> values) {
    if (values.size()!=change.entries.size()) return invalid("Transform preview does not match its targets.");
    std::vector<world::Trs> previous;previous.reserve(values.size());
    for (std::size_t i=0;i<values.size();++i) {
        const auto& entry=change.entries[i];
        const auto canonical=world::canonical_transform(values[i]);if(!canonical) return std::unexpected(canonical.error());
        auto entity=admit_property_edit(entry.root.id());
        if (!entity) return std::unexpected(entity.error());
        if (*entity!=entry.entity) return invalid("The transform's owning Entity changed.");
        auto value=world::read_transform(runtime_,entry.root.id());
        if (!value) return std::unexpected(value.error());previous.push_back(*value);
    }
    if(!std::ranges::equal(previous,values)) ++revision_;
    for (std::size_t i=0;i<values.size();++i) {
        if (auto written=world_.set_transform(change.entries[i].root.id(),values[i]); !written) {
            // A refused member cannot leave earlier members at a new preview.
            for(std::size_t j=0;j<i;++j) {
                auto restored=world_.set_transform(change.entries[j].root.id(),previous[j]);
                if(!restored) return restored;
            }
            return written;
        }
    }
    return {};
}
Result<void> Session::set_property(ObjectId object,std::span<const FieldId> path,Scalar value,std::uint64_t control_session) {
    auto begun=begin_edit(object,path,control_session);if(!begun) return begun;
    auto changed=preview_edit(control_session,std::move(value));
    if(changed) changed=commit_edit(control_session);
    if(changed) return {};
    auto restored=cancel_edit();return restored?changed:restored;
}
Result<void> Session::preview_edit(std::uint64_t control_session,float value) {
    return preview_edit(control_session,Scalar{value});
}
Result<void> Session::preview_edit(std::uint64_t control_session,Scalar value) {
    if(material_editing(control_session)) {
        if(material_edit_->paths.size()!=1) return invalid("No single material field is being edited.");
        if(!detail::finite_scalar(value)) return invalid("Material fields must be finite.");
        auto changed=material::write_property(runtime_,material_edit_->root.id(),material_edit_->paths.front(),std::move(value));
        if(changed) ++revision_;return changed;
    }
    if (!active_ || active_->control_session!=control_session) return invalid("No matching field edit.");
    auto entity=admit_property_edit(active_->root.id());
    if (!entity) return std::unexpected(entity.error());
    if (*entity!=active_->entity) return invalid("The property's owning Entity changed.");
    if (!detail::finite_scalar(value)) return invalid("Property values must be finite.");
    if(const auto* enumerated=std::get_if<EnumValue>(&value)) {
        const auto type=runtime_.describe(enumerated->type);
        if(!type || !(*type)->enumeration || std::ranges::none_of((*type)->enumeration->values,
            [&](const auto& member){return member.second==enumerated->bits;}))
            return invalid("Choose a declared enum value so the scene remains serializable.");
    }
    const auto parameter=std::find_if(parameters_.begin(),parameters_.end(),[&](const auto& item) {
        return std::ranges::equal(item.path,active_->path);
    });
    if (parameter!=parameters_.end()) {
        const auto* number=std::get_if<float>(&value);
        if (!number || *number<parameter->minimum || *number>parameter->maximum)
            return invalid("Field value is outside the editor range.");
    }
    for(const auto& path:types_.scale_fields) if(std::ranges::equal(path,active_->path)) {
        const auto* number=std::get_if<float>(&value);
        if(!number || *number<minimum_scale || *number>maximum_scale) return invalid("Scale is outside the editor range.");
    }
    const auto before=world_.read(active_->root.id(),active_->path);
    if(!before) return std::unexpected(before.error());
    auto result=world_.write(active_->root.id(),active_->path,value);
    if(result && *before!=value) ++revision_;
    return result;
}
Result<void> Session::commit_edit(std::uint64_t control_session) {
    if(material_editing(control_session)) return commit_material_edit(control_session);
    if (!owns_edit(control_session)) return invalid("No matching field edit.");
    if (group_) {
        for (std::size_t i=0;i<group_->paths.size();++i) {
            auto value=world_.read(group_->root.id(),group_->paths[i]); if (!value) return std::unexpected(value.error());
            group_->after[i]=*value;
        }
        if (group_->before!=group_->after) remember(std::move(*group_));
        group_.reset(); return {};
    }
    if (transform_) {
        bool changed{};
        for (auto& entry:transform_->entries) {
            auto value=world::read_transform(runtime_,entry.root.id());
            if (!value) return std::unexpected(value.error());
            entry.after=*value; changed|=entry.after!=entry.before;
        }
        if (changed && !transform_->amend_creation) remember(std::move(*transform_));
        transform_.reset(); return {};
    }
    auto entity=admit_property_edit(active_->root.id());
    if (!entity) return std::unexpected(entity.error());
    if (*entity!=active_->entity) return invalid("The property's owning Entity changed.");
    if (std::holds_alternative<world::Trs>(active_->before)) {
        auto value=world::read_transform(runtime_,active_->root.id());
        if (!value) return std::unexpected(value.error());
        active_->after=*value;
    } else {
        auto value=world_.read(active_->root.id(),active_->path);
        if (!value) return std::unexpected(value.error());
        active_->after=*value;
    }
    if (active_->before!=active_->after) remember(std::move(*active_));
    active_.reset(); return {};
}
Result<void> Session::apply_field(const FieldChange& change,bool forward) {
    auto entity=admit_property_edit(change.root.id());
    if (!entity) return std::unexpected(entity.error());
    if (*entity!=change.entity) return invalid("The property's owning Entity changed.");
    const auto& value=forward?change.after:change.before;
    if (const auto* transform=std::get_if<world::Trs>(&value)) return world_.set_transform(change.root.id(),*transform);
    return world_.write(change.root.id(),change.path,std::get<Scalar>(value));
}
Result<void> Session::cancel_edit() {
    if(auto cancelled=cancel_material_edit();!cancelled) return cancelled;
    if (group_) {
        auto restored=apply_group(*group_,false); if (!restored) return restored;
        ++revision_; group_.reset();
    }
    if (transform_) {
        // Restore every target even if one fails; report the first failure.
        Result<void> first;
        for (const auto& entry:transform_->entries) {
            const auto current=world::read_transform(runtime_,entry.root.id());
            if (!current) {if (first) first=std::unexpected(current.error());continue;}
            auto restored=world_.set_transform(entry.root.id(),entry.before);
            if (restored && *current!=entry.before) ++revision_;
            if (!restored && first) first=restored;
        }
        if (!first) return first;
        transform_.reset();
    }
    if (!active_) return {};
    bool changed{};
    if (const auto* before=std::get_if<world::Trs>(&active_->before)) {
        const auto current=world::read_transform(runtime_,active_->root.id());
        if (!current) return std::unexpected(current.error());
        changed=*current!=*before;
    } else {
        const auto current=world_.read(active_->root.id(),active_->path);
        if (!current) return std::unexpected(current.error());
        changed=*current!=std::get<Scalar>(active_->before);
    }
    auto restored=apply_field(*active_,false); if (!restored) return restored;
    if (changed) ++revision_;
    active_.reset(); return {};
}
Result<void> Session::reset_position(ObjectId component) {
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    auto before=world::read_transform(runtime_,component);
    if (!before) return std::unexpected(before.error());
    auto state=admit_entity_edit(component);
    if (!state) return std::unexpected(state.error());
    auto root=runtime_.hold(component);
    if (!root) return std::unexpected(root.error());
    auto after=*before;
    after.position={parameters_[0].default_value,parameters_[1].default_value,parameters_[2].default_value};
    if (after==*before) return {};
    FieldChange change{std::move(*root),component,{},0,*before,after};
    auto applied=world_.set_transform(component,after);
    if (!applied) return applied;
    remember(std::move(change));
    return {};
}
Result<world::Commit> Session::replay(Change& change, bool forward) {
    return std::visit([&](auto& item) -> Result<world::Commit> {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, LightingChange>) {
            auto changed=world_.set_lighting_settings(forward?item.after:item.before);
            if(!changed) return std::unexpected(changed.error());
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, NameChange>) {
            if (auto ready = admit_entity_edit(item.root.id()); !ready) return std::unexpected(ready.error());
            names_.at(item.root.id())=forward?item.after:item.before;
            set_selection({item.root.id(),item.root.id()});
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, TransformPresenceChange>) {
            if(auto valid=admit_entity_edit(item.root.id());!valid) return std::unexpected(valid.error());
            const auto& value=forward?item.after:item.before;
            auto changed=value?world_.set_transform(item.root.id(),*value):world_.remove_transform(item.root.id());
            if(!changed) return std::unexpected(changed.error());
            if(value) {
                auto restored=world_.set_space_reference(item.root.id(),forward?item.after_origin:item.before_origin,world::ReparentRule::keep_local);
                if(!restored) return std::unexpected(restored.error());
            }
            set_selection({item.root.id(),item.root.id()});return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, SpaceReferenceChange>) {
            if(auto valid=admit_entity_edit(item.root.id());!valid) return std::unexpected(valid.error());
            auto changed=world_.set_space_reference(item.root.id(),forward?item.after_origin:item.before_origin,world::ReparentRule::keep_local);
            if(!changed) return changed;
            if(item.spatial) if(auto restored=world_.set_transform(item.root.id(),forward?item.after:item.before);!restored) return std::unexpected(restored.error());
            set_selection({item.root.id(),item.root.id()});return world::Commit{true,changed->notification_error};
        } else if constexpr (std::is_same_v<T, ReparentChange>) {
            if(auto valid=admit_entity_edit(item.root.id());!valid) return std::unexpected(valid.error());
            auto changed=world_.reparent(item.root.id(),forward?item.after_parent:item.before_parent,
                world::ReparentRule::keep_local,forward?item.after_index:item.before_index);
            if(!changed) return changed;
            for(const auto& transform:item.transforms) {
                auto restored=world_.set_transform(transform.root.id(),forward?transform.after:transform.before);
                if(!restored) return std::unexpected(restored.error());
            }
            set_selection({item.root.id(),item.root.id()});return world::Commit{true,changed->notification_error};
        } else if constexpr (std::is_same_v<T, FieldGroupChange>) {
            auto written=apply_group(item,forward);
            if (!written) return std::unexpected(written.error());
            set_selection({item.entity,item.root.id()});
            return world::Commit{true, {}};
        } else if constexpr (std::is_same_v<T, FieldChange>) {
            auto written=apply_field(item,forward);
            if (!written) return std::unexpected(written.error());
            set_selection({item.entity,item.root.id()});
            return world::Commit{true, {}};
        } else if constexpr (std::is_same_v<T, TransformChange>) {
            std::vector<world::Trs> values;values.reserve(item.entries.size());
            for(const auto& entry:item.entries) values.push_back(forward?entry.after:entry.before);
            auto written=write_transforms(item,values);if(!written) return std::unexpected(written.error());
            std::vector<ObjectId> entities;
            for (const auto& entry:item.entries) {
                entities.push_back(entry.entity);
            }
            // Retain the document-history policy: the edited objects become the selection.
            if (auto selected=select_entities(entities,entities.empty()?ObjectId{}:entities.back()); !selected) return std::unexpected(selected.error());
            if(item.inspection.entity==selection_.entity) {
                const auto owner=admit_property_edit(item.inspection.object);
                if(owner && *owner==selection_.entity) selection_.object=item.inspection.object;
            }
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, MaterialValueChange>) {
            auto state=admit_component_edit(item.component);if(!state) return std::unexpected(state.error());
            if(state->owner!=item.entity) return invalid("The material instance's owner changed.");
            auto changed=material::update_material(runtime_,item.root.id(),forward?item.after:item.before);
            if(!changed) return std::unexpected(changed.error());
            set_selection({item.entity,item.component});return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, MaterialBindingChange>) {
            auto state=admit_component_edit(item.root.id());if(!state) return std::unexpected(state.error());
            if(state->owner!=item.entity) return invalid("The material binding's owning Entity changed.");
            auto changed=world::set_material_bindings(runtime_,item.root.id(),forward?item.after:item.before);
            if(!changed) return std::unexpected(changed.error());
            set_selection({item.entity,item.root.id()});return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, BodyChange>) {
            auto state=admit_component_edit(item.root.id());
            if (!state) return std::unexpected(state.error());
            if (state->owner != item.entity)
                return invalid("The physics body's owning Entity changed.");
            auto changed=physics::configure(runtime_,item.root.id(),forward?item.after:item.before);
            if (!changed) return std::unexpected(changed.error());
            set_selection({item.entity,item.root.id()});
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, RetainedChange>) {
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, EntityChange>) {
            world::Commit result;
            const auto merge=[&](world::Commit& commit) {
                result.changed|=commit.changed;
                if (commit.notification_error && !result.notification_error) result.notification_error=std::move(commit.notification_error);
                commit.notification_error.reset();
            };
            if (item.added == forward) {
                auto restored=world_.restore(item.removals);
                if(!restored) return std::unexpected(restored.error());
                merge(*restored);
                std::vector<ObjectId> restored_ids;
                for(const auto& removal:item.removals) restored_ids.push_back(removal.id());
                if (result.changed) {
                    if (auto selected=select_entities(restored_ids,restored_ids.empty()?ObjectId{}:restored_ids.back()); !selected)
                        return std::unexpected(selected.error());
                }
                return result;
            }
            std::vector<ObjectId> targets;
            for(const auto& removal:item.removals) targets.push_back(removal.id());
            auto removed=world_.detach_batch(targets);
            if(!removed) return std::unexpected(removed.error());
            item.removals=std::move(*removed);
            for(auto& removal:item.removals) merge(removal.commit);
            if(result.changed) for(const auto target:targets) forget_entity(target);
            return result;
        } else {
            const bool attach = item.added == forward;
            if (attach) {
                if(item.created_transform) {auto added=world_.set_transform(item.removal.owner,{});if(!added) return std::unexpected(added.error());}
                auto restored = world_.restore(item.removal);
                if (!restored) {if(item.created_transform) (void)world_.remove_transform(item.removal.owner);return restored;}
                if(restored->changed) set_selection({item.removal.owner,item.removal.id()});
                return restored;
            }
            auto removed = world_.remove_component(item.removal.id());
            if (!removed) return std::unexpected(removed.error());
            item.removal = std::move(*removed);
            if(item.created_transform && item.removal.commit.changed) {
                auto removed_space=world_.remove_transform(item.removal.owner);if(!removed_space) return std::unexpected(removed_space.error());
            }
            if(item.removal.commit.changed && selection_.object==item.removal.id())
                set_selection({item.removal.owner,item.removal.owner});
            auto result = std::move(item.removal.commit);
            item.removal.commit.notification_error.reset();
            return result;
        }
    }, change);
}
Result<world::Commit> Session::undo() {
    if(cursor_) ++revision_;
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (!cursor_) return world::Commit{};
    auto result = replay(history_[cursor_ - 1].change, false);
    if (result) {restore(history_[cursor_ - 1].retained,false);--cursor_;}
    return result;
}
Result<world::Commit> Session::redo() {
    if(cursor_<history_.size()) ++revision_;
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (cursor_ == history_.size()) return world::Commit{};
    auto result = replay(history_[cursor_].change, true);
    if (result) {restore(history_[cursor_].retained,true);++cursor_;}
    return result;
}
Result<world::Commit> Session::close() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto closed = world_.close();
    if (!closed) return closed;
    physics_.reset();
    history_.clear(); names_.clear(); retained_.clear(); retained_document_={}; cursor_ = 0; set_selection({});
    materials_.clear();textures_.clear();
    prepared_.reset();
    return closed;
}
Result<world::Commit> Session::start() {
    if (editing()) return invalid("Finish the active edit before starting the World.");
    if (world_.running() || world_.closed()) return world_.start();
    auto scene=physics::PhysicsScene::create(world_,physics_types_);
    if (!scene) return std::unexpected(scene.error());
    physics_=std::move(*scene);
    auto started=world_.start();
    if (!world_.running()) physics_.reset();
    return started;
}
Result<void> Session::update(float amount) {
    auto result=world_.update(amount);
    if (!result) (void)stop();
    return result;
}
Result<world::Commit> Session::stop() {
    auto result=world_.stop();
    if (!result) return result;
    if (!world_.running()) physics_.reset();
    return result;
}
Result<void> Session::collect_step() {
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        auto started = runtime_.begin_collection();
        if (!started) return started;
    }
    auto progress = runtime_.collect_step({32});
    if (!progress) return std::unexpected(progress.error());
    if (runtime_.collection_phase()==CollectionPhase::idle) {
        std::erase_if(names_,[&](const auto& item){return !runtime_.type_of(item.first);});
        std::erase_if(retained_,[&](const auto& item){return !runtime_.type_of(item.first);});
    }
    return {};
}
}

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
bool contains_member(const world::World& world, ObjectId actor) {
    const auto members=world.members();
    return members && std::ranges::any_of(*members,[&](const auto& member){return member.id()==actor;});
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
    history_.push_back(std::move(change));
    if (history_.size() > history_limit) {
        history_.erase(history_.begin());
        if (saved_cursor_) { if (*saved_cursor_) --*saved_cursor_; else saved_cursor_.reset(); }
    }
    cursor_ = history_.size();
}
std::wstring_view Session::name(ObjectId actor) const noexcept {
    const auto at=names_.find(actor);
    return at==names_.end()?std::wstring_view{}:at->second;
}
Result<void> Session::admit_actor_edit(ObjectId actor) const {
    auto state = world::inspect_actor(world_.runtime(), actor);
    if (!state) return std::unexpected(state.error());
    if (!state->can_edit || !names_.contains(actor)) return invalid("The Actor is not editable in this document.");
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    if (std::ranges::none_of(*members, [actor](const auto& member) { return member.id() == actor; }))
        return invalid("The Actor is not a member of this document.");
    return {};
}
Result<world::ComponentStatus> Session::admit_component_edit(ObjectId component) const {
    auto state = world::inspect_component(world_.runtime(), component);
    if (!state) return std::unexpected(state.error());
    if (!state->valid || state->owner == ObjectId{}) return invalid("The Component is not attached to an editable Actor.");
    if (auto ready = admit_actor_edit(state->owner); !ready) return std::unexpected(ready.error());
    return state;
}
Result<void> Session::validate_name(std::wstring_view name) {
    if (name.empty() || name.size()>256 || std::all_of(name.begin(),name.end(),[](wchar_t c){return c==L' ';}) ||
        std::any_of(name.begin(),name.end(),[](wchar_t c){return c<32 || c==127;}))
        return invalid("Use a name with 1–256 visible characters.");
    return {};
}
Result<void> Session::rename(ObjectId actor, std::wstring name) {
    if (auto valid=validate_name(name);!valid) return valid;
    if (auto ready = admit_actor_edit(actor); !ready) return ready;
    const auto at=names_.find(actor);
    if (at==names_.end()) return invalid("The Actor does not belong to this document.");
    if (at->second==name) return {};
    auto root=runtime_.hold(actor);
    if (!root) return std::unexpected(root.error());
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    NameChange change{std::move(*root),at->second,name};
    at->second=std::move(name); remember(std::move(change));
    return {};
}
void Session::set_selection(Selection next) {
    if(selection_!=next || selected_actors_.size()!=(next.actor==ObjectId{}?0u:1u)) ++revision_;
    selection_=next;
    selected_actors_.clear();
    if (next.actor!=ObjectId{}) selected_actors_.push_back(next.actor);
}
void Session::forget_actor(ObjectId actor) noexcept {
    if(is_selected(actor) || selection_.actor==actor) ++revision_;
    std::erase(selected_actors_,actor);
    if (selection_.actor==actor) {
        selection_={};
        if (!selected_actors_.empty()) selection_={selected_actors_.back(),selected_actors_.back()};
    }
}
Result<void> Session::select(ObjectId object) {
    Selection next;
    if (object != ObjectId{}) {
        auto actor=admit_property_edit(object);if(!actor) return std::unexpected(actor.error());
        next={*actor,object};
    }
    if(next==selection_ && selected_actors_.size()==(next.actor==ObjectId{}?0u:1u)) return {};
    auto cancelled = cancel_edit();
    if (!cancelled) return cancelled;
    set_selection(next);
    return {};
}
Result<void> Session::select_actors(std::span<const ObjectId> actors, ObjectId active) {
    std::vector<ObjectId> next;
    next.reserve(actors.size());
    for (const auto actor:actors) {
        if (std::ranges::find(next,actor)!=next.end()) continue;
        if (auto ready=admit_actor_edit(actor); !ready) return ready;
        next.push_back(actor);
    }
    if (active==ObjectId{} || std::ranges::find(next,active)==next.end()) active=next.empty()?ObjectId{}:next.back();
    else { std::erase(next,active); next.push_back(active); }
    if (next==selected_actors_ && selection_==Selection{active,active}) return {};
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    selected_actors_=std::move(next);
    selection_={active,active};
    ++revision_;
    return {};
}
Result<world::Commit> Session::add_actor(TypeId actual) {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    if (members->size() >= maximum_actors) return invalid("Editor Actor limit reached.");
    auto root = world::create_actor(runtime_, actual == TypeId{} ? types_.actor : actual);
    if (!root) return std::unexpected(root.error());
    return attach_created(std::move(*root),L"Actor");
}
Result<world::Commit> Session::create_static_mesh(const MeshAsset& asset, math::Vector3 position) {
    auto cancelled=cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto members=world_.members();
    if (!members) return std::unexpected(members.error());
    if (members->size()>=maximum_actors) return invalid("Editor Actor limit reached.");
    world::Material material;
    if(asset.mesh && asset.mesh->has_materials()) material.base_color={1,1,1};
    auto root=world::create_static_mesh_actor(world_,{asset.mesh,{position,{},{1.f,1.f,1.f}},material,asset.key});
    if (!root) return std::unexpected(root.error());
    return attach_created(std::move(*root),asset.name);
}
Result<world::Commit> Session::create_light(world::LightKind kind,math::Vector3 position) {
    auto cancelled=cancel_edit();if(!cancelled) return std::unexpected(cancelled.error());
    auto members=world_.members();if(!members) return std::unexpected(members.error());
    if(members->size()>=maximum_actors) return invalid("Editor Actor limit reached.");
    auto actor=world::create_actor(runtime_);if(!actor) return std::unexpected(actor.error());
    auto light=world::create_component(runtime_,types_.light_component);if(!light) return std::unexpected(light.error());
    auto configured=runtime_.with_native<world::LightComponent>(light->id(),[&](auto& value)->Result<void> {
        value.settings.kind=kind;
        if(kind==world::LightKind::sky) value.settings.intensity=.03f;
        return world::validate_light(value.settings);
    });
    if(!configured) return std::unexpected(configured.error());
    auto positioned=world_.set_transform(light->id(),{position,{},{1,1,1}});if(!positioned) return std::unexpected(positioned.error());
    const std::array parts{world::Attachment{light->id(),{},true}};
    auto attached=world_.attach_components(actor->id(),parts);if(!attached) return attached;
    if(attached->notification_error) return std::unexpected(*attached->notification_error);
    const auto label=kind==world::LightKind::point?L"Point Light":kind==world::LightKind::rectangle?L"Rectangle Light":
        kind==world::LightKind::directional?L"Directional Light":L"Sky Light";
    return attach_created(std::move(*actor),label);
}
Result<world::Commit> Session::attach_created(ObjectRoot root,std::wstring_view label) {
    ActorChange change;
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
    if(selection_.object!=selection_.actor) return remove_component(selection_.object);
    return erase_actors();
}
Result<world::Commit> Session::erase_actors() {
    if (selected_actors_.empty()) return world::Commit{};
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    const auto targets=selected_actors_.empty()?std::vector<ObjectId>{selection_.actor}:selected_actors_;
    ActorChange change;
    world::Commit result;
    for (const auto actor:targets) {
        auto removed = world_.detach(actor);
        if (!removed) {
            // Earlier Actors already left the World; keep them undoable.
            if (!change.removals.empty()) remember(std::move(change));
            for (const auto& item:targets) if (!contains_member(world_,item)) forget_actor(item);
            return std::unexpected(removed.error());
        }
        if (removed->commit.notification_error && !result.notification_error) result.notification_error=removed->commit.notification_error;
        if (!removed->commit.changed) continue;
        result.changed=true;
        change.removals.push_back(std::move(*removed));
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
    const auto actor=selection_.actor;
    if (auto ready = admit_actor_edit(actor); !ready) return std::unexpected(ready.error());
    auto supported = runtime_.is_a(type, types_.component);
    if (!supported) return std::unexpected(supported.error());
    if (!*supported) return invalid("Adding a component requires a Component type.");
    const auto members=world::components(runtime_,actor);
    if (!members) return std::unexpected(members.error());
    if (members->size()>=maximum_components) return invalid("Editor Component limit reached.");
    auto root = world::create_component(runtime_, type);
    if (!root) return std::unexpected(root.error());
    auto actor_root = world::root_component(runtime_, actor);
    if (!actor_root) return std::unexpected(actor_root.error());
    ComponentChange change;
    change.added = true;
    change.removal.root = std::move(*root);
    change.removal.owner = actor;
    auto spatial = runtime_.is_a(type,types_.scene_component);
    if (!spatial) return std::unexpected(spatial.error());
    const std::array attachments{world::Attachment{change.removal.id(), *spatial ? actor_root->value_or(ObjectId{}) : ObjectId{}, *spatial && !actor_root->has_value()}};
    auto attached = world_.attach_components(actor, attachments);
    if (!attached) return attached;
    if (attached->changed) {set_selection({actor,change.removal.id()});remember(std::move(change));}
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
    auto actor = runtime_.is_a(*type,types_.actor);
    if (!actor) return std::unexpected(actor.error());
    if (*actor) {
        if (auto ready=admit_actor_edit(object); !ready) return std::unexpected(ready.error());
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
    for (auto& field:*fields) {
        auto value=read_property(object,field.path); if (!value) return std::unexpected(value.error());
        std::string name;
        for (const auto& part:field.names) { if (!name.empty()) name+='.'; name+=part; }
        result.push_back({std::move(name),std::move(field.path),field.type,std::move(*value)});
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
        material_edit_->path.assign(path.begin(),path.end());return {};
    }
    auto actor=admit_property_edit(object); if (!actor) return std::unexpected(actor.error());
    auto before=world_.read(object,path); if (!before) return std::unexpected(before.error());
    auto root=runtime_.hold(object); if (!root) return std::unexpected(root.error());
    std::variant<world::Trs,Scalar> value{*before};
    if (detail::transform_path(types_,path)) {
        auto transform=world::read_transform(runtime_,object);
        if (!transform) return std::unexpected(transform.error());
        value=*transform;
    }
    active_.emplace(FieldChange{std::move(*root),*actor,{path.begin(),path.end()},control_session,value,value});
    return {};
}
bool Session::owns_edit(std::uint64_t control_session) const noexcept {
    return (active_ && active_->control_session == control_session) ||
        (transform_ && transform_->control_session == control_session) || material_editing(control_session);
}
Result<void> Session::begin_transform(std::span<const ObjectId> components, std::uint64_t control_session, bool amend_creation) {
    if (editing()) return invalid("A field edit is already active.");
    if (!control_session) return invalid("A transform edit requires a control session.");
    if (components.empty()) return invalid("Select an Actor with a SceneComponent root.");
    TransformChange change{{},control_session,selection_};
    change.entries.reserve(components.size());
    for (const auto component:components) {
        if (std::ranges::any_of(change.entries,[&](const auto& entry){return entry.root.id()==component;}))
            return invalid("A transform edit names a component twice.");
        auto state=admit_component_edit(component);
        if (!state) return std::unexpected(state.error());
        auto spatial=runtime_.is_a(state->type,types_.scene_component);
        if (!spatial) return std::unexpected(spatial.error());
        if (!*spatial) return invalid("Viewport transforms require SceneComponents.");
        auto value=world::read_transform(runtime_,component);
        if (!value) return std::unexpected(value.error());
        auto root=runtime_.hold(component);
        if (!root) return std::unexpected(root.error());
        change.entries.push_back({std::move(*root),state->owner,*value,*value});
    }
    if (amend_creation) {
        const auto* creation=cursor_ && cursor_==history_.size()?std::get_if<ActorChange>(&history_.back()):nullptr;
        if (!creation || !creation->added || saved_cursor_==cursor_ ||
            std::ranges::any_of(change.entries,[&](const auto& entry) {
                return std::ranges::none_of(creation->removals,[&](const auto& item){return item.id()==entry.actor;});
            })) return invalid("Initial placement must belong to the current unsaved creation.");
        change.amend_creation=true;
    }
    transform_=std::move(change);
    return {};
}
Result<world::Trs> Session::original_transform(std::uint64_t control_session,ObjectId component) const {
    if(!transform_ || transform_->control_session!=control_session) return invalid("No matching transform edit.");
    for(const auto& entry:transform_->entries) if(entry.root.id()==component) return entry.before;
    return invalid("The component does not belong to this transform edit.");
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
        auto actor=admit_property_edit(entry.root.id());
        if (!actor) return std::unexpected(actor.error());
        if (*actor!=entry.actor) return invalid("The transform's owning Actor changed.");
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
        if(material_edit_->path.empty()) return invalid("No material field is being edited.");
        if(!detail::finite_scalar(value)) return invalid("Material fields must be finite.");
        auto changed=material::write_property(runtime_,material_edit_->root.id(),material_edit_->path,std::move(value));
        if(changed) ++revision_;return changed;
    }
    if (!active_ || active_->control_session!=control_session) return invalid("No matching field edit.");
    auto actor=admit_property_edit(active_->root.id());
    if (!actor) return std::unexpected(actor.error());
    if (*actor!=active_->actor) return invalid("The property's owning Actor changed.");
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
    auto actor=admit_property_edit(active_->root.id());
    if (!actor) return std::unexpected(actor.error());
    if (*actor!=active_->actor) return invalid("The property's owning Actor changed.");
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
    auto actor=admit_property_edit(change.root.id());
    if (!actor) return std::unexpected(actor.error());
    if (*actor!=change.actor) return invalid("The property's owning Actor changed.");
    const auto& value=forward?change.after:change.before;
    if (const auto* transform=std::get_if<world::Trs>(&value)) return world_.set_transform(change.root.id(),*transform);
    return world_.write(change.root.id(),change.path,std::get<Scalar>(value));
}
Result<void> Session::cancel_edit() {
    if(auto cancelled=cancel_material_edit();!cancelled) return cancelled;
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
    auto state=admit_component_edit(component);
    if (!state) return std::unexpected(state.error());
    auto root=runtime_.hold(component);
    if (!root) return std::unexpected(root.error());
    auto after=*before;
    after.position={parameters_[0].default_value,parameters_[1].default_value,parameters_[2].default_value};
    if (after==*before) return {};
    FieldChange change{std::move(*root),state->owner,{},0,*before,after};
    auto applied=world_.set_transform(component,after);
    if (!applied) return applied;
    remember(std::move(change));
    return {};
}
Result<world::Commit> Session::replay(Change& change, bool forward) {
    return std::visit([&](auto& item) -> Result<world::Commit> {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, NameChange>) {
            if (auto ready = admit_actor_edit(item.root.id()); !ready) return std::unexpected(ready.error());
            names_.at(item.root.id())=forward?item.after:item.before;
            set_selection({item.root.id(),item.root.id()});
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, FieldChange>) {
            auto written=apply_field(item,forward);
            if (!written) return std::unexpected(written.error());
            set_selection({item.actor,item.root.id()});
            return world::Commit{true, {}};
        } else if constexpr (std::is_same_v<T, TransformChange>) {
            std::vector<world::Trs> values;values.reserve(item.entries.size());
            for(const auto& entry:item.entries) values.push_back(forward?entry.after:entry.before);
            auto written=write_transforms(item,values);if(!written) return std::unexpected(written.error());
            std::vector<ObjectId> actors;
            for (const auto& entry:item.entries) {
                actors.push_back(entry.actor);
            }
            // Retain the document-history policy: the edited objects become the selection.
            if (auto selected=select_actors(actors,actors.empty()?ObjectId{}:actors.back()); !selected) return std::unexpected(selected.error());
            if(item.inspection.actor==selection_.actor) {
                const auto owner=admit_property_edit(item.inspection.object);
                if(owner && *owner==selection_.actor) selection_.object=item.inspection.object;
            }
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, MaterialValueChange>) {
            auto state=admit_component_edit(item.component);if(!state) return std::unexpected(state.error());
            if(state->owner!=item.actor) return invalid("The material instance's owner changed.");
            auto changed=material::update_material(runtime_,item.root.id(),forward?item.after:item.before);
            if(!changed) return std::unexpected(changed.error());
            set_selection({item.actor,item.component});return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, MaterialBindingChange>) {
            auto state=admit_component_edit(item.root.id());if(!state) return std::unexpected(state.error());
            if(state->owner!=item.actor) return invalid("The material binding's owning Actor changed.");
            auto changed=world::set_material_bindings(runtime_,item.root.id(),forward?item.after:item.before);
            if(!changed) return std::unexpected(changed.error());
            set_selection({item.actor,item.root.id()});return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, BodyChange>) {
            auto state=admit_component_edit(item.root.id());
            if (!state) return std::unexpected(state.error());
            if (state->owner != item.actor)
                return invalid("The physics body's owning Actor changed.");
            auto changed=physics::configure(runtime_,item.root.id(),forward?item.after:item.before);
            if (!changed) return std::unexpected(changed.error());
            set_selection({item.actor,item.root.id()});
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, ActorChange>) {
            world::Commit result;
            const auto merge=[&](world::Commit& commit) {
                result.changed|=commit.changed;
                if (commit.notification_error && !result.notification_error) result.notification_error=std::move(commit.notification_error);
                commit.notification_error.reset();
            };
            if (item.added == forward) {
                std::vector<ObjectId> restored_ids;
                for (auto at=item.removals.rbegin(); at!=item.removals.rend(); ++at) {
                    auto restored = world_.restore(*at);
                    if (!restored) return std::unexpected(restored.error());
                    merge(*restored);
                    if (restored->changed) restored_ids.push_back(at->id());
                }
                if (result.changed) {
                    std::ranges::reverse(restored_ids);
                    if (auto selected=select_actors(restored_ids,restored_ids.empty()?ObjectId{}:restored_ids.back()); !selected)
                        return std::unexpected(selected.error());
                }
                return result;
            }
            for (std::size_t index=0; index<item.removals.size(); ++index) {
                auto removed = world_.detach(item.removals[index].id());
                if (!removed) {
                    // Put back the Actors this step already removed, newest first.
                    for (std::size_t back=index; back>0; --back) (void)world_.restore(item.removals[back-1]);
                    return std::unexpected(removed.error());
                }
                item.removals[index] = std::move(*removed);
                merge(item.removals[index].commit);
                if (item.removals[index].commit.changed) forget_actor(item.removals[index].id());
            }
            return result;
        } else {
            const bool attach = item.added == forward;
            if (attach) {
                auto restored = world_.restore(item.removal);
                if (!restored) return restored;
                if(restored->changed) set_selection({item.removal.owner,item.removal.id()});
                return restored;
            }
            auto removed = world_.remove_component(item.removal.id());
            if (!removed) return std::unexpected(removed.error());
            item.removal = std::move(*removed);
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
    auto result = replay(history_[cursor_ - 1], false);
    if (result) --cursor_;
    return result;
}
Result<world::Commit> Session::redo() {
    if(cursor_<history_.size()) ++revision_;
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (cursor_ == history_.size()) return world::Commit{};
    auto result = replay(history_[cursor_], true);
    if (result) ++cursor_;
    return result;
}
Result<world::Commit> Session::close() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto closed = world_.close();
    if (!closed) return closed;
    physics_.reset();
    history_.clear(); names_.clear(); cursor_ = 0; set_selection({});
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
    auto result=[&]()->Result<void> {
        if (!physics_) return world_.update(amount);
        auto stepped=physics_->update(amount);
        if (!stepped) return std::unexpected(stepped.error());
        return {};
    }();
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
    if (runtime_.collection_phase()==CollectionPhase::idle)
        std::erase_if(names_,[&](const auto& item){return !runtime_.type_of(item.first);});
    return {};
}
}

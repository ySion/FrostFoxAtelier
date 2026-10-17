#include "world/detail.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<void> World::Impl::admit(bool mutation) const {
    if (closed || (closing && mutation)) return error(ErrorCode::unsupported_operation, closing ? "World is closing." : "World is closed.");
    if (transitioning || (mutation && dispatch_depth)) return error(ErrorCode::busy, "World operation cannot reenter this phase.");
    if (mutation) return runtime.check_code_publication();
    return {};
}
Result<std::size_t> World::Impl::member(ObjectId object) const {
    auto entity = detail::entity_data(runtime, object);
    if (!entity) return std::unexpected(entity.error());
    if (entity->world != &world)
        return error(ErrorCode::invalid_member, entity->world ? "Entity belongs to another World." : "Entity is not a World member.");
    const auto* found = positions.find(object);
    if (!found) return error(ErrorCode::invalid_member, "World member index is inconsistent.");
    return *found;
}
Result<ObjectId> World::Impl::component_owner(ObjectId object) const {
    auto component = detail::component_data(runtime, object);
    if (!component) return std::unexpected(component.error());
    if (component->owner == ObjectId{}) return error(ErrorCode::invalid_member, "Component is not attached.");
    auto found = member(component->owner);
    if (!found) return std::unexpected(found.error());
    return component->owner;
}
Result<detail::EntityData> World::Impl::composition_entity(ObjectId object) const {
    auto entity = detail::entity_data(runtime, object);
    if (!entity) return std::unexpected(entity.error());
    if (entity->transitioning) return error(ErrorCode::busy, "Entity is transitioning.");
    if (entity->world && entity->world != &world) return error(ErrorCode::invalid_member, "Entity belongs to another World.");
    return entity;
}
Result<detail::ComponentData> World::Impl::composition_component(ObjectId object) const {
    auto component = detail::component_data(runtime, object);
    if (!component) return std::unexpected(component.error());
    if (component->owner != ObjectId{}) {
        auto entity = composition_entity(component->owner);
        if (!entity) return std::unexpected(entity.error());
    }
    return component;
}
Result<void> World::Impl::admit_edit(ObjectId object) const {
    if (auto ready = admit(); !ready) return ready;
    auto entity = detail::is(runtime, object, types.entity);
    if (!entity) return std::unexpected(entity.error());
    World* owner{};
    if (*entity) {
        auto data = composition_entity(object);
        if (!data) return std::unexpected(data.error());
        owner = data->world;
    } else {
        auto data = composition_component(object);
        if (!data) return std::unexpected(data.error());
        if (data->owner != ObjectId{}) {
            auto parent = composition_entity(data->owner);
            if (!parent) return std::unexpected(parent.error());
            owner = parent->world;
        }
    }
    if (running && owner) return error(ErrorCode::busy, "Stop the World before editing its members.");
    return {};
}
void World::Impl::changed(bool spatial) noexcept {
    schedule_dirty = buckets_dirty = true;
    if (spatial) space.clear();
}
World::Impl::MemberState* World::Impl::state_of(ObjectId object) noexcept {
    const auto* found = positions.find(object);
    return found ? &states[*found] : nullptr;
}
const World::Impl::MemberState* World::Impl::state_of(ObjectId object) const noexcept {
    const auto* found = positions.find(object);
    return found ? &states[*found] : nullptr;
}
World::Impl::ComponentState* World::Impl::component_state(ObjectId entity, ObjectId component) noexcept {
    auto* owner = state_of(entity);
    if (!owner) return nullptr;
    const auto found = std::ranges::find(owner->components, component, &ComponentState::id);
    return found == owner->components.end() ? nullptr : &*found;
}
Result<std::vector<World::Impl::ComponentState>> World::Impl::derive_components(ObjectId entity) const {
    auto group = components(runtime, entity);
    if (!group) return std::unexpected(group.error());
    std::vector<ComponentState> result;
    result.reserve(group->size());
    for (const auto& root : *group) {
        const auto object = root.id();
        auto type = runtime.type_of(object);
        if (!type) return std::unexpected(type.error());
        auto binding = behavior(*type);
        if (!binding) return std::unexpected(binding.error());
        auto data = detail::component_data(runtime, object);
        if (!data) return std::unexpected(data.error());
        ComponentState item{object, *type, *binding, Presentation::none, data->enabled, false};
        auto mesh = runtime.is_a(*type, types.static_mesh_component);
        if (!mesh) return std::unexpected(mesh.error());
        if (*mesh) {
            auto present = runtime.with_native<StaticMeshComponent>(object, [](StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
            if (!present) return std::unexpected(present.error());
            if (*present) item.presentation = Presentation::mesh;
        } else {
            auto light = runtime.is_a(*type, types.light_component);
            if (!light) return std::unexpected(light.error());
            if (*light) item.presentation = Presentation::light;
        }
        result.push_back(item);
    }
    return result;
}
Result<World::Impl::MemberState> World::Impl::derive(ObjectId entity, ObjectId parent, ObjectId origin) const {
    auto type = runtime.type_of(entity);
    if (!type) return std::unexpected(type.error());
    auto binding = behavior(*type);
    if (!binding) return std::unexpected(binding.error());
    auto data = detail::entity_data(runtime, entity);
    if (!data) return std::unexpected(data.error());
    auto group = derive_components(entity);
    if (!group) return std::unexpected(group.error());
    MemberState state;
    state.type = *type; state.behavior = *binding; state.parent = parent;
    state.origin = origin;
    state.enabled = data->enabled;
    state.components = std::move(*group);
    return state;
}
Result<void> World::Impl::rederive(ObjectId entity) {
    auto next = derive_components(entity);
    if (!next) return std::unexpected(next.error());
    auto* state = state_of(entity);
    if (!state) return error(ErrorCode::invalid_member, "World member index is inconsistent.");
    for (auto& item : *next) {
        const auto previous = std::ranges::find(state->components, item.id, &ComponentState::id);
        if (previous != state->components.end()) item.begun = previous->begun;
    }
    state->components = std::move(*next);
    changed(false);
    return {};
}
void World::Impl::reindex(std::size_t first, std::size_t last) noexcept {
    for (auto i = first; i < last; ++i) {
        const auto [value, inserted] = positions.try_emplace(members[i].id(), i);
        if (!inserted) *value = i;
    }
}
void World::Impl::resize_ancestors(ObjectId parent, std::ptrdiff_t delta) noexcept {
    for (auto id = parent; id != ObjectId{};) {
        auto& state = states[*positions.find(id)];
        state.size = std::size_t(std::ptrdiff_t(state.size) + delta);
        id = state.parent;
    }
}
void World::Impl::refresh_eligibility(std::size_t first, std::size_t count) noexcept {
    // Preorder: every parent outside the range is already current.
    for (auto i = first; i < first + count; ++i) {
        auto& state = states[i];
        state.eligible = state.enabled && (state.parent == ObjectId{} || states[*positions.find(state.parent)].eligible);
    }
}
void World::Impl::rebuild_schedule() {
    updates.clear();
    schedule_dirty = false;
    if (!running) return;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& state = states[i];
        if (!state.eligible) continue;
        const auto id = members[i].id();
        if (state.begun && state.behavior && state.behavior->tick) updates.push_back({id, id, state.behavior});
        for (const auto& item : state.components)
            if (item.begun && item.enabled && item.behavior && item.behavior->tick) updates.push_back({item.id, id, item.behavior});
    }
}
void World::Impl::rebuild_buckets() {
    for (auto& bucket : buckets) bucket.objects.clear();
    const auto add = [&](TypeId type, ObjectId object) {
        auto found = std::ranges::find(buckets, type, &Bucket::type);
        if (found == buckets.end()) { buckets.push_back({type, {}}); found = std::prev(buckets.end()); }
        found->objects.push_back(object);
    };
    for (std::size_t i = 0; i < members.size(); ++i) {
        add(states[i].type, members[i].id());
        for (const auto& item : states[i].components) add(item.type, item.id);
    }
    buckets_dirty = false;
}
void World::Impl::cancel_tasks(ObjectId target, bool entity) noexcept {
    std::erase_if(tasks, [&](Task& task) {
        if (target != ObjectId{} && (entity ? task.entity : task.object) != target) return false;
        task.coroutine.cancel();
        return true;
    });
}
void World::Impl::halt() noexcept {
    running = false; updates.clear(); translations.clear(); cancel_tasks();
    changed(false);
}
std::optional<Error> World::Impl::notify(Lifecycle event, ObjectId object) {
    if (!observer && subscriptions.empty()) return {};
    auto scope = ExecutionScope::enter(runtime);
    if (!scope) return scope.error();
    detail::DispatchScope dispatch{dispatch_depth};
    std::optional<Error> failure;
    const auto invoke = [&](const LifecycleObserver& callback) {
        if (!callback) return;
        try {
            auto result = callback(world, event, object);
            if (!result && !failure) failure = result.error();
        } catch (const std::exception& exception) {
            if (!failure) failure = Error{ErrorCode::invocation_failed, exception.what()};
        } catch (...) {
            if (!failure) failure = Error{ErrorCode::invocation_failed, "World lifecycle observer threw an unknown exception."};
        }
    };
    invoke(observer);
    for (const auto& entry : subscriptions) {
        // A callback may unsubscribe itself; this reference keeps it alive until it returns.
        const auto callback = entry.observer;
        if (callback) invoke(*callback);
    }
    return failure;
}
Commit World::Impl::finish(bool changed, std::optional<Error> failure) {
    if (!failure && running) {
        auto applied=apply_translations();
        if (!applied) failure=applied.error();
    }
    if (failure) (void)shutdown();
    return {changed, std::move(failure)};
}

World::World(Runtime& runtime, const Types& types) : impl_(std::make_unique<Impl>(runtime, types, *this)) {
    const auto registered = runtime.native_type<Entity>();
    if (!registered || *registered != types.entity) throw std::invalid_argument("World types must be registered in its Runtime.");
}
World::~World() {
    if (!impl_->closed) {
        const auto result = close();
        // Destruction during a dispatch would leave dangling member_world links.
        if (!result) std::terminate();
    }
}
Runtime& World::runtime() const noexcept { return impl_->runtime; }
const rendering::SceneLightingSettings& World::lighting_settings() const noexcept { return impl_->lighting; }
Result<void> World::set_lighting_settings(const rendering::SceneLightingSettings& candidate) {
    if(auto ready=impl_->admit();!ready) return ready;
    if(impl_->running) return error(ErrorCode::unsupported_operation,"World lighting is editable only while stopped.");
    if(const auto* message=rendering::scene_lighting_error(candidate)) return error(ErrorCode::invalid_definition,message);
    impl_->lighting=candidate;return {};
}
const Types& World::types() const noexcept { return impl_->types; }
bool World::running() const noexcept { return impl_->running; }
bool World::closed() const noexcept { return impl_->closed; }
Activity World::activity() const noexcept {
    const auto& state = *impl_;
    Activity result{0, 0, state.tasks.size()};
    for (const auto& member : state.states) {
        result.update_registrations += state.running && member.begun && member.eligible && member.behavior && member.behavior->tick;
        for (const auto& item : member.components) {
            result.update_registrations += state.running && item.begun && member.eligible && item.enabled && item.behavior && item.behavior->tick;
            result.presentation_registrations += item.presentation != Impl::Presentation::none;
        }
    }
    return result;
}
bool World::contains(ObjectId entity) const noexcept { return impl_->positions.contains(entity); }
Result<std::span<const Member>> World::members() const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    return std::span<const Member>{impl_->members};
}
Result<std::vector<ObjectId>> World::query(TypeId type, bool include_derived) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto info = runtime().describe(type);
    if (!info) return std::unexpected(info.error());
    if (impl_->buckets_dirty) impl_->rebuild_buckets();
    std::vector<ObjectId> result;
    for (const auto& bucket : impl_->buckets) {
        auto matches = include_derived ? runtime().is_a(bucket.type, type) : Result<bool>{bucket.type == type};
        if (!matches) return std::unexpected(matches.error());
        if (*matches) result.insert(result.end(), bucket.objects.begin(), bucket.objects.end());
    }
    return result;
}

Result<Commit> World::attach_components(ObjectId entity_id, std::span<const Attachment> incoming, std::size_t index) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto entity = state.composition_entity(entity_id);
    if (!entity) return std::unexpected(entity.error());
    auto existing = components(runtime(), entity_id);
    if (!existing) return std::unexpected(existing.error());
    if (index == append) index = existing->size();
    if (index > existing->size()) return error(ErrorCode::argument_mismatch, "Component insertion index is out of range.");
    if (incoming.empty()) return Commit{};
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> references;
    Set<ObjectId> seen;
    roots.reserve(incoming.size()); references.reserve(incoming.size());
    for (const auto& item : *existing) seen.add(item.id());
    for (const auto& item : incoming) {
        if (!seen.add(item.component)) return error(ErrorCode::invalid_member, "Component is already present or duplicated.");
        auto data = detail::component_data(runtime(), item.component);
        if (!data) return std::unexpected(data.error());
        if (data->owner != ObjectId{}) return error(ErrorCode::invalid_member, "Component already has an owner.");
        auto held = runtime().hold(item.component, types().component);
        if (!held) return std::unexpected(held.error());
        references.push_back(held->reference()); roots.push_back(std::move(*held));
    }
    // Detached composition is the preparation phase: capabilities may be added
    // before their owner's Transform. Complete spatial requirements are checked
    // by attach_batch before any member, observer or behavior is published.
    if (entity->world)
        if (auto valid = detail::requires_space(runtime(), entity_id, roots); !valid) return std::unexpected(valid.error());
    if (entity->world) if (auto sky = state.validate_sky_light(roots); !sky) return std::unexpected(sky.error());
    auto held = runtime().hold(entity_id, types().entity);
    if (!held) return std::unexpected(held.error());
    auto changed = runtime().with_native<Entity>(entity_id, [&](Entity& value) -> Result<void> {
    auto sequence = runtime().reference_field(entity_id, Access::sequence(value));
    if (!sequence) return std::unexpected(sequence.error());
    std::vector<ReferenceChange> changes;
    changes.reserve(incoming.size() + 1);
    changes.push_back(SequenceSplice{*sequence, index, 0, references});
    for (const auto& item : incoming) {
        auto owner = runtime().with_native<Component>(item.component, [&](Component& value) { return runtime().reference_field(item.component, Access::owner(value)); });
        if (!owner) return std::unexpected(owner.error());
        changes.push_back(ReferenceWrite{*owner, held->reference()});
    }
    return runtime().apply_reference_changes(changes);
    });
    if (!changed) return std::unexpected(changed.error());
    if (!entity->world) return Commit{true, {}};
    if (auto rederived = state.rederive(entity_id); !rederived) return state.finish(true, rederived.error());
    std::optional<Error> failure;
    for (const auto& item : incoming) {
        auto notification = state.notify(Lifecycle::attached, item.component);
        if (!failure) failure = std::move(notification);
    }
    if (!failure && state.running) {
        auto at = state.member(entity_id);
        failure = at ? state.begin(*at, *at + 1) : std::optional<Error>{at.error()};
    }
    return state.finish(true, std::move(failure));
}
Result<ComponentRemoval> World::remove_component(ObjectId object) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto data = state.composition_component(object);
    if (!data) return std::unexpected(data.error());
    const auto owner = data->owner;
    if (owner == ObjectId{}) return error(ErrorCode::invalid_member, "Component is not attached.");
    auto entity = state.composition_entity(owner);
    if (!entity) return std::unexpected(entity.error());
    const bool registered = entity->world != nullptr;
    auto group = components(runtime(), owner);
    if (!group) return std::unexpected(group.error());
    const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
    if (at == group->end()) return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
    auto root = runtime().hold(object, types().component);
    if (!root) return std::unexpected(root.error());
    auto status = inspect_component(runtime(), object);
    if (!status) return std::unexpected(status.error());
    ComponentRemoval result;
    result.root = std::move(*root); result.owner = owner;
    result.index = std::size_t(at - group->begin()); result.origin_ = state.lifetime;
    if (registered) state.cancel_tasks(object, false);
    std::optional<Error> failure;
    if (registered) {
        auto position = state.member(owner);
        failure = position ? state.end(*position, *position + 1, object) : std::optional<Error>{position.error()};
    }
    if (status->active) { auto notification = state.notify(Lifecycle::deactivated, object); if (!failure) failure = std::move(notification); }
    {
        detail::FlagScope transition{state.transitioning};
        auto removed = detail::clear_component(runtime(), owner, object, result.index);
        if (!removed) { if (registered) state.halt(); return std::unexpected(removed.error()); }
        if (registered) {
            if (auto* member = state.state_of(owner)) std::erase_if(member->components, [&](const Impl::ComponentState& item) { return item.id == object; });
            state.changed(false);
        }
    }
    if (!registered) { result.commit = Commit{true, {}}; return result; }
    auto detached = state.notify(Lifecycle::detached, object);
    if (!failure) failure = std::move(detached);
    result.commit = state.finish(true, std::move(failure));
    return result;
}
Result<Commit> World::restore(const ComponentRemoval& removal) {
    auto origin = removal.origin_.lock();
    if (!origin || !origin->open || origin != impl_->lifetime)
        return error(ErrorCode::invalid_member, "Component removal does not belong to this open World's history.");
    const std::array incoming{Attachment{removal.id()}};
    return attach_components(removal.owner, incoming, removal.index);
}

namespace detail {
Result<void> queue_translation(Runtime& runtime,ObjectId object,math::Vector3 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
        return error(ErrorCode::argument_mismatch,"Translation must be finite.");
    auto component=component_data(runtime,object); if (!component) return std::unexpected(component.error());
    if (component->owner==ObjectId{}) return error(ErrorCode::invalid_member,"Translation requires an owned Component.");
    auto entity=entity_data(runtime,component->owner); if (!entity) return std::unexpected(entity.error());
    if (!entity->world) return error(ErrorCode::invalid_member,"Translation requires World membership.");
    auto& state=Access::state(*entity->world);
    if (!state.running || state.closing || state.transitioning || !state.dispatch_depth)
        return error(ErrorCode::busy,"translate_owner requires a running World behavior phase.");
    auto spatial=has_transform(runtime,component->owner); if (!spatial) return std::unexpected(spatial.error());
    if (!*spatial) return error(ErrorCode::invalid_member,"Translation requires an Entity Transform.");
    state.translations.push_back({object,component->owner,delta});
    return {};
}
}
Result<void> World::Impl::apply_translations() {
    if (translations.empty()) return {};
    // No callbacks execute while applying. Retain buffers between frames, and
    // discard the batch on every exit so a rejected action cannot replay later.
    struct Cleanup { Impl& state; ~Cleanup() { state.translations.clear(); state.translation_changes.clear(); } } cleanup{*this};
    auto& changes=translation_changes; changes.reserve(translations.size());
    const std::array physical_types{runtime.find_type("physics.CharacterComponent"),runtime.find_type("physics.RagdollComponent")};
    for (const auto& command:translations) {
        auto owner=component_owner(command.component);
        if (!owner) return std::unexpected(owner.error());
        if (*owner!=command.entity) return error(ErrorCode::invalid_member,"Translation receiver changed owner.");
        {
            auto group=components(runtime,command.entity); if (!group) return std::unexpected(group.error());
            for (const auto& component:*group) {
                auto type=runtime.type_of(component.id()); if (!type) return std::unexpected(type.error());
                auto primitive=runtime.is_a(*type,types.primitive_component);
                if (!primitive) return std::unexpected(primitive.error());
                if (*primitive) {
                    auto physical=runtime.with_native<const PrimitiveComponent>(component.id(),[](const auto& p)->Result<bool>{return p.body.enabled;});
                    if (!physical) return std::unexpected(physical.error());
                    if (*physical) return error(ErrorCode::unsupported_operation,"translate_owner cannot move a physics-controlled Entity; use physics actions.");
                }
                for(const auto& physical_type:physical_types) if(physical_type) {
                    auto physical=runtime.is_a(*type,*physical_type); if (!physical) return std::unexpected(physical.error());
                    if (*physical) return error(ErrorCode::unsupported_operation,"translate_owner cannot move a physics-controlled Entity; use physics actions.");
                }
            }
        }
        auto found=std::ranges::find(changes,command.entity,&TranslationChange::entity);
        if (found==changes.end()) {
            auto matrix=world_matrix(runtime,command.entity); if (!matrix) return std::unexpected(matrix.error());
            auto value=relative_transform(*matrix,math::Matrix4::identity()); if (!value) return std::unexpected(value.error());
            changes.push_back({command.entity,*value}); found=std::prev(changes.end());
        }
        auto& value=found->value;
        value.position.x+=command.delta.x; value.position.y+=command.delta.y; value.position.z+=command.delta.z;
        auto canonical=canonical_transform(value); if (!canonical) return std::unexpected(canonical.error());
        value=*canonical;
    }
    std::vector<SimulationPose> poses;
    poses.reserve(changes.size());
    for (const auto& change:changes) poses.push_back({change.entity,change.value.position,change.value.rotation});
    return world.apply_simulation_transforms(poses);
}

Result<Scalar> World::read(ObjectId component, std::span<const FieldId> path) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto entity = detail::is(runtime(),component,types().entity);
    if (!entity) return std::unexpected(entity.error());
    if (*entity) { auto data=impl_->composition_entity(component); if (!data) return std::unexpected(data.error()); }
    else { auto data=impl_->composition_component(component); if (!data) return std::unexpected(data.error()); }
    return runtime().read(component, path);
}
Result<void> World::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    if (auto ready = impl_->admit_edit(object); !ready) return ready;
    auto lighting=detail::write_lighting_property(*this,object,path,value);
    if (!lighting) return std::unexpected(lighting.error());
    if (*lighting) return {};
    auto primitive=detail::is(runtime(),object,types().primitive_component);
    if (!primitive) return std::unexpected(primitive.error());
    if (*primitive && !path.empty()) {
        auto body_field=runtime().find_field(types().primitive_component,"body");
        if (!body_field) return std::unexpected(body_field.error());
        if (path.front()==*body_field) {
            auto previous=runtime().with_native<const PrimitiveComponent>(object,[](const auto& p)->Result<BodySettings>{return p.body;});
            if (!previous) return std::unexpected(previous.error());
            if (auto written=runtime().write(object,path,value);!written) return written;
            return runtime().with_native<PrimitiveComponent>(object,[&](auto& p)->Result<void>{
                if (p.body.shape!=previous->shape) p.body.cooked_shape.reset();
                auto valid=validate_body_settings(p.body);
                if (!valid) p.body=*previous;
                return valid;
            });
        }
    }
    auto transform = detail::is(runtime(), object, types().entity);
    if (!transform) return std::unexpected(transform.error());
    if (*transform) {
        const auto match = [&](const auto& paths) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (std::ranges::equal(path, paths[i])) return i;
            return {};
        };
        const auto position = match(types().position_fields), rotation = match(types().rotation_fields), scale = match(types().scale_fields);
        if (!position && !rotation && !scale) return runtime().write(object, path, value);
        auto current = read_transform(runtime(), object);
        if (!current) return std::unexpected(current.error());
        if (!std::holds_alternative<float>(value)) return error(ErrorCode::type_mismatch, "Transform fields require float values.");
        if (auto i = match(types().position_fields)) {
            std::array fields{&current->position.x, &current->position.y, &current->position.z}; *fields[*i] = std::get<float>(value);
        } else if (auto i = match(types().rotation_fields)) {
            std::array fields{&current->rotation.x, &current->rotation.y, &current->rotation.z, &current->rotation.w}; *fields[*i] = std::get<float>(value);
        } else if (auto i = match(types().scale_fields)) {
            std::array fields{&current->scale.x, &current->scale.y, &current->scale.z}; *fields[*i] = std::get<float>(value);
        }
        return set_transform(object, *current);
    }
    return runtime().write(object, path, value);
}
Result<std::vector<RenderItem>> World::render_items() const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    auto& state = *impl_;
    std::vector<RenderItem> result;
    std::size_t count{};
    for (const auto& member : state.states)
        for (const auto& component : member.components) count += component.presentation == Impl::Presentation::mesh;
    if (count) result.reserve(count);
    state.space.begin_pass();
    for (std::size_t i = 0; i < state.members.size(); ++i) {
        const auto owner = state.members[i].id();
        for (const auto& component : state.states[i].components) {
            if (component.presentation != Impl::Presentation::mesh) continue;
            const auto object = component.id;
            auto matrix = state.space.matrix(owner);
            if (!matrix) return std::unexpected(matrix.error());
            auto item = runtime().with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& value) -> Result<RenderItem> {
                if (auto valid = validate_material(value.material); !valid) return std::unexpected(valid.error());
                return RenderItem{owner, object, *matrix, Access::geometry(value), value.material};
            });
            if (!item) return std::unexpected(item.error());
            auto surfaces=material_surfaces(runtime(),object);if(!surfaces) return std::unexpected(surfaces.error());
            item->surfaces=std::move(*surfaces);
            result.push_back(std::move(*item));
        }
    }
    return result;
}

}

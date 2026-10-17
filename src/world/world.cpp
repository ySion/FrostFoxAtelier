#include "world/detail.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace atelier::world {
using detail::Access;
using detail::error;

bool isolatable(const Error& failure) noexcept {
    switch (failure.code) {
    case ErrorCode::busy: case ErrorCode::rebuild_required: case ErrorCode::execution_aborted: case ErrorCode::capacity_exceeded:
        return false;
    default:
        return true;
    }
}
std::size_t World::Impl::DiagnosticKeyHash::operator()(const DiagnosticKey& key) const noexcept {
    auto value = std::hash<ObjectId>{}(key.object);
    value ^= (std::size_t(key.kind) << 8 | std::size_t(key.phase)) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    return value ^ (std::hash<std::string>{}(key.field) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2));
}
void World::Impl::begin_pass(DiagnosticPhase phase) noexcept {
    const auto pass = ++passes[std::size_t(phase)];
    std::erase_if(recent, [&](const auto& entry) { return entry.first.phase == phase && entry.second.pass + 1 < pass; });
}
void World::Impl::report(Diagnostic diagnostic) {
    if (diagnostic.type.empty() && diagnostic.object != ObjectId{})
        if (auto type = runtime.type_of(diagnostic.object))
            if (auto info = runtime.describe(*type)) diagnostic.type = (*info)->name;
    // Behavior failures fault their object and are reported once per incident.
    const auto phase = diagnostic.phase;
    const bool consumer = phase == DiagnosticPhase::presentation || phase == DiagnosticPhase::lighting ||
        phase == DiagnosticPhase::physics || phase == DiagnosticPhase::command;
    std::optional<DiagnosticKey> key;
    if (consumer) {
        std::string field;
        for (const auto& name : diagnostic.field) { if (!field.empty()) field += '.'; field += name; }
        key.emplace(DiagnosticKey{diagnostic.object, diagnostic.kind, phase, std::move(field)});
        const auto pass = passes[std::size_t(phase)];
        // `repeats` counts consecutive passes; dependents failing on one cause count once per pass.
        if (auto found = recent.find(*key); found != recent.end() && found->second.pass + 1 >= pass) {
            if (found->second.pass == pass) return;
            found->second.pass = pass;
            const auto pending = std::ranges::lower_bound(diagnostics, found->second.serial, {}, &Diagnostic::serial);
            if (pending != diagnostics.end() && pending->serial == found->second.serial && pending->repeats != UINT32_MAX) ++pending->repeats;
            return;
        }
    }
    if (diagnostics.size() >= diagnostic_capacity) { ++dropped_diagnostics; return; }
    diagnostic.serial = next_diagnostic++;
    if (key) recent.insert_or_assign(std::move(*key), Recent{diagnostic.serial, passes[std::size_t(phase)]});
    diagnostics.push_back(std::move(diagnostic));
}
bool World::Impl::contain(const Error& failure, Diagnostic diagnostic) {
    if (invariant_failed || !isolatable(failure)) return false;
    diagnostic.source = failure.source; diagnostic.message = failure.message;
    report(std::move(diagnostic));
    return policy == FailurePolicy::isolate;
}
bool World::Impl::contain(const Error& failure, DiagnosticKind kind, DiagnosticPhase phase, ObjectId object, ObjectId entity,
    std::vector<std::string> field, Severity severity) {
    Diagnostic diagnostic;
    diagnostic.severity = severity; diagnostic.kind = kind; diagnostic.phase = phase;
    diagnostic.object = object; diagnostic.entity = entity; diagnostic.field = std::move(field);
    return contain(failure, std::move(diagnostic));
}
World::Impl::Fault* World::Impl::fault_of(ObjectId object, ObjectId entity) noexcept {
    auto* member = state_of(entity);
    if (!member) return nullptr;
    if (object == entity) return &member->fault;
    const auto found = std::ranges::find(member->components, object, &ComponentState::id);
    return found == member->components.end() ? nullptr : &found->fault;
}
void World::Impl::fault_tick(ObjectId object, ObjectId entity) noexcept {
    if (auto* fault = fault_of(object, entity)) *fault = Fault::tick;
    schedule_dirty = true;
    cancel_tasks(object, false);
    std::erase_if(translations, [&](const Translation& command) { return command.component == object; });
}

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
    if (!found) return std::unexpected(inconsistent("World member index is inconsistent."));
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
        auto presentation = presentation_of(object, *type);
        if (!presentation) return std::unexpected(presentation.error());
        result.push_back({object, *type, *binding, *presentation, data->enabled, false});
    }
    return result;
}
Result<std::uint32_t> World::Impl::presentation_of(ObjectId component, TypeId type) const {
    std::uint32_t result{};
    for (std::uint32_t i = 0; i < presentations.size(); ++i) {
        const auto& registration = presentations[i];
        auto matches = runtime.is_a(type, registration.type);
        if (!matches) return std::unexpected(matches.error());
        if (!*matches) continue;
        if (registration.predicate) {
            auto present = registration.predicate(runtime, component);
            if (!present) return std::unexpected(present.error());
            if (!*present) continue;
        }
        result |= 1u << i;
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
    if (!state) return std::unexpected(inconsistent("World member index is inconsistent."));
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
    for (auto& phase : updates) phase.clear();
    schedule_dirty = false;
    if (!running) return;
    struct Node { Scheduled item; bool ticks; };
    // Every participant takes part in ordering, so prerequisites hold
    // transitively through objects that do not tick in a phase.
    std::vector<Node> order;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& state = states[i];
        const auto id = members[i].id();
        order.push_back({{id, id, state.behavior}, state.eligible && state.begun && state.fault == Fault::none});
        for (const auto& item : state.components)
            order.push_back({{item.id, id, item.behavior}, state.eligible && item.enabled && item.begun && item.fault == Fault::none});
    }
    if (!prerequisites.empty()) {
        // Kahn's algorithm; ties keep composition preorder, so the result is deterministic.
        std::unordered_map<ObjectId, std::size_t> index;
        index.reserve(order.size());
        for (std::size_t i = 0; i < order.size(); ++i) index.emplace(order[i].item.object, i);
        std::vector<std::size_t> pending(order.size());
        for (const auto& [object, before] : prerequisites) pending[index.at(object)] = before.size();
        std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
        for (std::size_t i = 0; i < order.size(); ++i) if (!pending[i]) ready.push(i);
        std::vector<Node> sorted;
        sorted.reserve(order.size());
        while (!ready.empty()) {
            const auto next = ready.top();
            ready.pop();
            sorted.push_back(order[next]);
            if (const auto found = dependents.find(order[next].item.object); found != dependents.end())
                for (const auto object : found->second) if (!--pending[index.at(object)]) ready.push(index.at(object));
        }
        order = std::move(sorted);
    }
    for (const auto& node : order) {
        const auto* behavior = node.item.behavior;
        if (!node.ticks || !behavior) continue;
        if (behavior->tick) updates[std::size_t(TickPhase::pre_physics)].push_back(node.item);
        if (behavior->post_physics_tick) updates[std::size_t(TickPhase::post_physics)].push_back(node.item);
        if (behavior->late_tick) updates[std::size_t(TickPhase::late)].push_back(node.item);
    }
}
void World::Impl::forget_prerequisites(ObjectId object) noexcept {
    const auto unlink = [&](auto& from, auto& to) {
        const auto found = from.find(object);
        if (found == from.end()) return;
        for (const auto other : found->second) {
            auto& back = to.at(other);
            std::erase(back, object);
            if (back.empty()) to.erase(other);
        }
        from.erase(found);
        schedule_dirty = true;
    };
    unlink(prerequisites, dependents);
    unlink(dependents, prerequisites);
}
void World::Impl::rebuild_buckets() {
    for (auto& bucket : buckets) bucket.objects.clear();
    const auto add = [&](TypeId type, ObjectId object) {
        auto found = std::ranges::find(buckets, type, &Bucket::type);
        if (found == buckets.end()) { buckets.push_back({type, {}}); found = std::prev(buckets.end()); }
        found->objects.push_back(object);
    };
    presented.assign(presentations.size(), {});
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto id = members[i].id();
        add(states[i].type, id);
        for (const auto& item : states[i].components) {
            add(item.type, item.id);
            for (std::uint32_t mask = item.presentation; mask; mask &= mask - 1)
                presented[std::countr_zero(mask)].push_back({id, item.id, states[i].eligible && item.enabled});
        }
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
    running = false; translations.clear(); cancel_tasks();
    for (auto& phase : updates) phase.clear();
    // Faults belong to one run; the next start begins every member again.
    for (auto& member : states) {
        member.fault = Fault::none;
        for (auto& item : member.components) item.fault = Fault::none;
    }
    changed(false);
}
std::optional<Error> World::Impl::notify(Lifecycle event, ObjectId object) {
    if (!observer && subscriptions.empty()) return {};
    auto scope = ExecutionScope::enter(runtime);
    if (!scope) return scope.error();
    detail::DispatchScope dispatch{dispatch_depth};
    std::optional<Error> failure;
    const auto owner = [&] {
        if (positions.contains(object)) return object;
        auto component = detail::component_data(runtime, object);
        return component ? component->owner : ObjectId{};
    };
    const auto invoke = [&](const LifecycleObserver& callback) {
        if (!callback) return;
        std::optional<Error> error;
        try {
            auto result = callback(world, event, object);
            if (!result) error = result.error();
        } catch (const std::bad_alloc&) {
            error = Error{ErrorCode::capacity_exceeded, "World lifecycle observer ran out of memory."};
        } catch (const std::exception& exception) {
            error = Error{ErrorCode::invocation_failed, exception.what()};
        } catch (...) {
            error = Error{ErrorCode::invocation_failed, "World lifecycle observer threw an unknown exception."};
        }
        if (error && !contain(*error, DiagnosticKind::observer_failed, DiagnosticPhase::lifecycle, object, owner()) && !failure)
            failure = std::move(error);
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
    // The world library presents its own mesh and light components; a mesh needs geometry.
    impl_->meshes = {0};
    impl_->presentations.push_back({types.static_mesh_component, [](Runtime& runtime, ObjectId object) {
        return runtime.with_native<StaticMeshComponent>(object, [](StaticMeshComponent& value) -> Result<bool> { return bool(Access::geometry(value)); });
    }});
    impl_->lights = {1};
    impl_->presentations.push_back({types.light_component, {}});
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
        const auto ticks = [](const Behavior* behavior) {
            return behavior && (behavior->tick || behavior->post_physics_tick || behavior->late_tick);
        };
        result.update_registrations += state.running && member.begun && member.fault == Impl::Fault::none && member.eligible &&
            ticks(member.behavior);
        for (const auto& item : member.components) {
            result.update_registrations += state.running && item.begun && item.fault == Impl::Fault::none && member.eligible &&
                item.enabled && ticks(item.behavior);
            result.presentation_registrations += item.presentation != 0;
        }
    }
    return result;
}
bool World::contains(ObjectId entity) const noexcept { return impl_->positions.contains(entity); }
FailurePolicy World::failure_policy() const noexcept { return impl_->policy; }
Result<void> World::set_failure_policy(FailurePolicy policy) {
    if (auto ready = impl_->admit(); !ready) return ready;
    if (impl_->running) return error(ErrorCode::busy, "Stop the World before changing its failure policy.");
    if (policy != FailurePolicy::isolate && policy != FailurePolicy::halt) return error(ErrorCode::argument_mismatch, "Unknown failure policy.");
    impl_->policy = policy;
    return {};
}
std::vector<Diagnostic> World::take_diagnostics() { return std::exchange(impl_->diagnostics, {}); }
std::uint64_t World::dropped_diagnostics() const noexcept { return impl_->dropped_diagnostics; }
void World::report(Diagnostic diagnostic) { impl_->report(std::move(diagnostic)); }
bool World::contain(const Error& failure, Diagnostic diagnostic) { return impl_->contain(failure, std::move(diagnostic)); }
Result<std::span<const Member>> World::members() const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    return std::span<const Member>{impl_->members};
}
Result<PresentationId> World::register_presentation(TypeId type, PresentationPredicate predicate) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (state.presentations.size() >= 32) return error(ErrorCode::capacity_exceeded, "A World supports at most 32 presentation registrations.");
    auto component = runtime().is_a(type, types().component);
    if (!component) return std::unexpected(component.error());
    if (!*component) return error(ErrorCode::type_mismatch, "Presentation requires a Component type.");
    const auto id = PresentationId{std::uint32_t(state.presentations.size())};
    state.presentations.push_back({type, std::move(predicate)});
    // Evaluate the new registration for current members before publishing any bit.
    std::vector<bool> present;
    for (const auto& member : state.states)
        for (const auto& item : member.components) {
            auto value = state.presentation_of(item.id, item.type);
            if (!value) { state.presentations.pop_back(); return std::unexpected(value.error()); }
            present.push_back(*value & Impl::bit(id));
        }
    auto next = present.begin();
    for (auto& member : state.states)
        for (auto& item : member.components) if (*next++) item.presentation |= Impl::bit(id);
    state.changed(false);
    return id;
}
Result<std::span<const PresentedComponent>> World::presented(PresentationId id) const {
    if (auto ready = impl_->admit(false); !ready) return std::unexpected(ready.error());
    if (id.value >= impl_->presentations.size()) return error(ErrorCode::argument_mismatch, "Unknown presentation registration.");
    if (impl_->buckets_dirty) impl_->rebuild_buckets();
    return std::span<const PresentedComponent>{impl_->presented[id.value]};
}
Result<void> World::reevaluate_presentation(ObjectId component) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return ready;
    auto owner = state.component_owner(component);
    if (!owner) return std::unexpected(owner.error());
    return state.rederive(*owner);
}
Result<std::uint64_t> World::add_simulation(Simulation simulation) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (!simulation.step || !simulation.writes_pose) return error(ErrorCode::argument_mismatch, "A simulation needs step and writes_pose.");
    std::erase_if(state.simulations, [](const Impl::SimulationEntry& entry) { return !entry.simulation; });
    const auto id = state.next_simulation++;
    state.simulations.push_back({id, std::make_shared<const Simulation>(std::move(simulation))});
    return id;
}
void World::remove_simulation(std::uint64_t id) noexcept {
    for (auto& entry : impl_->simulations) if (entry.id == id) entry.simulation.reset();
    if (!impl_->updating) std::erase_if(impl_->simulations, [](const Impl::SimulationEntry& entry) { return !entry.simulation; });
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
            state.forget_prerequisites(object);
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
Result<void> queue_request(Runtime& runtime,ObjectId object,RequestKind kind,bool enabled) {
    auto owner=object;
    if (kind!=RequestKind::destroy_entity && kind!=RequestKind::entity_enabled) {
        auto component=component_data(runtime,object); if (!component) return std::unexpected(component.error());
        owner=component->owner;
    }
    auto entity=owner==ObjectId{}?Result<EntityData>{EntityData{}}:entity_data(runtime,owner);
    if (!entity) return std::unexpected(entity.error());
    if (!entity->world) return error(ErrorCode::invalid_member,"World requests require World membership.");
    Request request;
    request.kind=kind; request.target=object; request.enabled=enabled;
    auto queued=entity->world->request(request);
    if (!queued) return std::unexpected(queued.error());
    return {};
}
}
Result<void> World::Impl::apply_translations() {
    if (translations.empty()) return {};
    // No callbacks execute while applying. Retain buffers between frames, and
    // discard the batch on every exit so a rejected action cannot replay later.
    struct Cleanup { Impl& state; ~Cleanup() { state.translations.clear(); state.translation_changes.clear(); } } cleanup{*this};
    auto& changes=translation_changes; changes.reserve(translations.size());
    // Each command is checked alone: a rejected one is reported and dropped.
    const auto check=[&](const Translation& command)->Result<void> {
        auto owner=component_owner(command.component);
        if (!owner) return std::unexpected(owner.error());
        if (*owner!=command.entity) return error(ErrorCode::invalid_member,"Translation receiver changed owner.");
        for (const auto& entry:simulations)
            if (entry.simulation && entry.simulation->writes_pose(command.entity))
                return error(ErrorCode::unsupported_operation,"translate_owner cannot move an Entity whose pose a simulation writes; use that system's actions.");
        auto found=std::ranges::find(changes,command.entity,&TranslationChange::entity);
        if (found==changes.end()) {
            auto matrix=world_matrix(runtime,command.entity); if (!matrix) return std::unexpected(matrix.error());
            auto value=relative_transform(*matrix,math::Matrix4::identity()); if (!value) return std::unexpected(value.error());
            changes.push_back({command.entity,*value}); found=std::prev(changes.end());
        }
        auto value=found->value;
        value.position.x+=command.delta.x; value.position.y+=command.delta.y; value.position.z+=command.delta.z;
        auto canonical=canonical_transform(value); if (!canonical) return std::unexpected(canonical.error());
        found->value=*canonical;
        return {};
    };
    for (const auto& command:translations)
        if (auto checked=check(command); !checked &&
            !contain(checked.error(),DiagnosticKind::command_rejected,DiagnosticPhase::command,command.component,command.entity,{},Severity::warning))
            return checked;
    std::vector<SimulationPose> poses;
    poses.reserve(changes.size());
    for (const auto& change:changes) poses.push_back({change.entity,change.value.position,change.value.rotation});
    return apply_poses(poses,DiagnosticPhase::command);
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
    state.begin_pass(DiagnosticPhase::presentation);
    auto meshes = presented(state.meshes);
    if (!meshes) return std::unexpected(meshes.error());
    result.reserve(meshes->size());
    state.space.begin_pass();
    {
        for (const auto& [owner, object, active] : *meshes) {
            (void)active;
            // Isolated items are skipped: an unusable space is reported once on
            // its Entity, an invalid material on its component.
            auto matrix = state.space.matrix(owner);
            if (!matrix) {
                const auto at = state.space.failed;
                if (!state.contain(matrix.error(), DiagnosticKind::invalid_value, DiagnosticPhase::presentation, at, at)) return std::unexpected(matrix.error());
                continue;
            }
            // A degenerate space is not a failure: its meshes are skipped under every policy.
            if (singular(*matrix)) {
                Diagnostic diagnostic;
                diagnostic.severity = Severity::warning; diagnostic.kind = DiagnosticKind::degenerate_value;
                diagnostic.phase = DiagnosticPhase::presentation;
                diagnostic.object = diagnostic.entity = state.space.degenerate_root(owner); diagnostic.field = {"scale"};
                diagnostic.message = "A zero scale axis makes this space degenerate; meshes in it are not drawn.";
                state.report(std::move(diagnostic));
                continue;
            }
            auto item = runtime().with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& value) -> Result<RenderItem> {
                if (auto valid = validate_material(value.material); !valid) return std::unexpected(valid.error());
                return RenderItem{owner, object, *matrix, Access::geometry(value), value.material};
            });
            if (!item) {
                if (!state.contain(item.error(), DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner, {"material"}))
                    return std::unexpected(item.error());
                continue;
            }
            auto surfaces=material_surfaces(runtime(),object);
            if (!surfaces) {
                if (!state.contain(surfaces.error(), DiagnosticKind::invalid_value, DiagnosticPhase::presentation, object, owner, {"materials"}))
                    return std::unexpected(surfaces.error());
                continue;
            }
            item->surfaces=std::move(*surfaces);
            result.push_back(std::move(*item));
        }
    }
    return result;
}

}

#include "world/detail.hpp"
#include "foundation/set.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <queue>
#include <stdexcept>
#include <unordered_set>

namespace atelier::world {
using detail::Access;

using detail::error;

bool isolatable(const Error& failure) noexcept {
    switch (failure.code) {
    case ErrorCode::busy:
    case ErrorCode::rebuild_required:
    case ErrorCode::execution_aborted:
    case ErrorCode::capacity_exceeded:
        return false;
    default:
        return true;
    }
}

Diagnostic validation_diagnostic(const ValidationFailure& failure, DiagnosticPhase phase, ObjectId object,
                                 ObjectId entity, std::vector<std::string> record) {
    Diagnostic diagnostic;
    diagnostic.kind = DiagnosticKind::invalid_value;
    diagnostic.phase = phase;
    diagnostic.object = object;
    diagnostic.entity = entity;
    diagnostic.field = std::move(record);
    diagnostic.field.insert(diagnostic.field.end(), failure.names.begin(), failure.names.end());
    if (failure.value)
        diagnostic.value = std::visit(
            [](const auto& value) -> std::string {
                if constexpr (std::same_as<std::decay_t<decltype(value)>, EnumValue>)
                    return std::to_string(value.bits);
                else
                    return std::format("{}", value);
            },
            *failure.value);
    diagnostic.message = failure.error.message;
    return diagnostic;
}

std::size_t World::Impl::DiagnosticKeyHash::operator()(const DiagnosticKey& key) const noexcept {
    auto value = std::hash<ObjectId>{}(key.object);
    value ^=
        (std::size_t(key.kind) << 8 | std::size_t(key.phase)) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
    return value ^ (std::hash<std::string>{}(key.field) + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2));
}

void World::Impl::begin_pass(DiagnosticPhase phase) noexcept {
    const auto pass = ++passes[std::size_t(phase)];
    std::erase_if(recent,
                  [&](const auto& entry) { return entry.first.phase == phase && entry.second.pass + 1 < pass; });
}

void World::Impl::report(Diagnostic diagnostic) {
    if (diagnostic.type.empty() && diagnostic.object != ObjectId{})
        if (auto type = runtime.type_of(diagnostic.object))
            if (auto info = runtime.describe(*type))
                diagnostic.type = (*info)->name;
    // Behavior failures fault their object and are reported once per incident.
    const auto phase = diagnostic.phase;
    const bool consumer = phase == DiagnosticPhase::presentation || phase == DiagnosticPhase::lighting ||
                          phase == DiagnosticPhase::physics || phase == DiagnosticPhase::command;
    std::optional<DiagnosticKey> key;
    if (consumer) {
        std::string field;
        for (const auto& name : diagnostic.field) {
            if (!field.empty())
                field += '.';
            field += name;
        }
        key.emplace(DiagnosticKey{diagnostic.object, diagnostic.kind, phase, std::move(field)});
        const auto pass = passes[std::size_t(phase)];
        // `repeats` counts consecutive passes; dependents failing on one cause count once per pass.
        if (auto found = recent.find(*key); found != recent.end() && found->second.pass + 1 >= pass) {
            if (found->second.pass == pass)
                return;
            found->second.pass = pass;
            const auto pending = std::ranges::lower_bound(diagnostics, found->second.serial, {}, &Diagnostic::serial);
            if (pending != diagnostics.end() && pending->serial == found->second.serial &&
                pending->repeats != UINT32_MAX)
                ++pending->repeats;
            return;
        }
    }
    if (diagnostics.size() >= diagnostic_capacity) {
        ++dropped_diagnostics;
        return;
    }
    diagnostic.serial = next_diagnostic++;
    if (key)
        recent.insert_or_assign(std::move(*key), Recent{diagnostic.serial, passes[std::size_t(phase)]});
    diagnostics.push_back(std::move(diagnostic));
}

bool World::Impl::contain(const Error& failure, Diagnostic diagnostic) {
    if (invariant_failed || !isolatable(failure))
        return false;
    diagnostic.source = failure.source;
    diagnostic.message = failure.message;
    report(std::move(diagnostic));
    return policy == FailurePolicy::isolate;
}

bool World::Impl::contain(const Error& failure, DiagnosticKind kind, DiagnosticPhase phase, ObjectId object,
                          ObjectId entity, std::vector<std::string> field, Severity severity) {
    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = kind;
    diagnostic.phase = phase;
    diagnostic.object = object;
    diagnostic.entity = entity;
    diagnostic.field = std::move(field);
    return contain(failure, std::move(diagnostic));
}

World::Impl::Fault* World::Impl::fault_of(ObjectId object, ObjectId entity) noexcept {
    auto* member = state_of(entity);
    if (!member)
        return nullptr;
    if (object == entity)
        return &member->fault;
    const auto found = std::ranges::find(member->components, object, &ComponentState::id);
    return found == member->components.end() ? nullptr : &found->fault;
}

void World::Impl::fault_tick(ObjectId object, ObjectId entity) noexcept {
    if (auto* fault = fault_of(object, entity))
        *fault = Fault::tick;
    schedule_dirty = true;
    cancel_tasks(object, false);
    std::erase_if(translations, [&](const Translation& command) { return command.component == object; });
}

Result<void> World::Impl::admit(bool mutation) const {
    if (closed || (closing && mutation))
        return error(ErrorCode::unsupported_operation, closing ? "World is closing." : "World is closed.");
    if (transitioning || (mutation && (dispatch_depth || relocating)))
        return error(ErrorCode::busy, "World operation cannot reenter this phase.");
    if (mutation)
        return runtime.check_code_publication();
    return {};
}

Result<std::size_t> World::Impl::member(ObjectId object) const {
    ATELIER_TRY_BIND(entity, detail::entity_data(runtime, object));
    if (entity->world != &world)
        return error(ErrorCode::invalid_member,
                     entity->world ? "Entity belongs to another World." : "Entity is not a World member.");
    const auto* found = positions.find(object);
    if (!found)
        return std::unexpected(inconsistent("World member index is inconsistent."));
    return *found;
}

Result<ObjectId> World::Impl::component_owner(ObjectId object) const {
    ATELIER_TRY_BIND(component, detail::component_data(runtime, object));
    if (component->owner == ObjectId{})
        return error(ErrorCode::invalid_member, "Component is not attached.");
    ATELIER_TRY_BIND(found, member(component->owner));
    return component->owner;
}

Result<detail::EntityData> World::Impl::composition_entity(ObjectId object) const {
    ATELIER_TRY_BIND(entity, detail::entity_data(runtime, object));
    if (entity->transitioning)
        return error(ErrorCode::busy, "Entity is transitioning.");
    if (entity->world && entity->world != &world)
        return error(ErrorCode::invalid_member, "Entity belongs to another World.");
    return entity;
}

Result<detail::ComponentData> World::Impl::composition_component(ObjectId object) const {
    ATELIER_TRY_BIND(component, detail::component_data(runtime, object));
    if (component->owner != ObjectId{}) {
        ATELIER_TRY_BIND(entity, composition_entity(component->owner));
    }
    return component;
}

Result<void> World::Impl::admit_edit(ObjectId object) const {
    ATELIER_TRY(admit());
    ATELIER_TRY_BIND(entity, detail::is(runtime, object, types.entity));
    if (*entity) {
        ATELIER_TRY_BIND(data, composition_entity(object));
    } else {
        ATELIER_TRY_BIND(data, composition_component(object));
    }
    return {};
}

Result<void> World::Impl::relocate(ObjectId edited) try {
    const auto systems = simulations;
    for (const auto& entry : systems) {
        if (!entry.simulation || !entry.simulation->relocated)
            continue;
        std::vector<ObjectId> moved;
        for (const auto& member : members) {
            if (!entry.simulation->writes_pose(member.id()))
                continue;
            for (auto node = member.id(); node != ObjectId{};) {
                if (node == edited) {
                    moved.push_back(member.id());
                    break;
                }
                ATELIER_TRY_BIND(parent, spatial_parent(runtime, node));
                node = *parent;
            }
        }
        if (moved.empty())
            continue;
        detail::FlagScope relocating_scope{relocating};
        ATELIER_TRY_BIND(relocated, entry.simulation->relocated(moved));
    }
    return {};
} catch (const std::bad_alloc&) {
    return error(ErrorCode::capacity_exceeded, "World simulation relocation ran out of memory.");
} catch (const std::exception& exception) {
    return error(ErrorCode::invocation_failed, exception.what());
} catch (...) {
    return error(ErrorCode::invocation_failed, "World simulation relocation threw an unknown exception.");
}

void World::Impl::changed(bool spatial) noexcept {
    schedule_dirty = buckets_dirty = true;
    ++revision;
    if (spatial)
        space.clear();
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
    if (!owner)
        return nullptr;
    const auto found = std::ranges::find(owner->components, component, &ComponentState::id);
    return found == owner->components.end() ? nullptr : &*found;
}

Result<std::vector<World::Impl::ComponentState>> World::Impl::derive_components(ObjectId entity) const {
    ATELIER_TRY_BIND(group, components(runtime, entity));
    std::vector<ComponentState> result;
    result.reserve(group->size());
    for (const auto& root : *group) {
        const auto object = root.id();
        ATELIER_TRY_BIND(type, runtime.type_of(object));
        ATELIER_TRY_BIND(binding, behavior(*type));
        ATELIER_TRY_BIND(data, detail::component_data(runtime, object));
        ATELIER_TRY_BIND(capabilities, capabilities_of(object, *type));
        result.push_back({object, *type, *binding, *capabilities, data->enabled, false});
    }
    return result;
}

Result<std::uint32_t> World::Impl::capabilities_of(ObjectId component, TypeId type) const {
    std::uint32_t result{};
    for (std::uint32_t i = 0; i < registrations.size(); ++i) {
        const auto& registration = registrations[i];
        ATELIER_TRY_BIND(matches, runtime.is_a(type, registration.type));
        if (!*matches)
            continue;
        if (registration.predicate) {
            ATELIER_TRY_BIND(present, registration.predicate(runtime, component));
            if (!*present)
                continue;
        }
        result |= 1u << i;
    }
    return result;
}

Result<World::Impl::MemberState> World::Impl::derive(ObjectId entity, ObjectId parent, ObjectId origin) const {
    ATELIER_TRY_BIND(type, runtime.type_of(entity));
    ATELIER_TRY_BIND(binding, behavior(*type));
    ATELIER_TRY_BIND(data, detail::entity_data(runtime, entity));
    ATELIER_TRY_BIND(group, derive_components(entity));
    MemberState state;
    state.type = *type;
    state.behavior = *binding;
    state.parent = parent;
    state.origin = origin;
    state.enabled = data->enabled;
    state.components = std::move(*group);
    return state;
}

Result<void> World::Impl::rederive(ObjectId entity) {
    ATELIER_TRY_BIND(next, derive_components(entity));
    auto* state = state_of(entity);
    if (!state)
        return std::unexpected(inconsistent("World member index is inconsistent."));
    for (auto& item : *next) {
        const auto previous = std::ranges::find(state->components, item.id, &ComponentState::id);
        if (previous != state->components.end())
            item.begun = previous->begun;
    }
    state->components = std::move(*next);
    changed(false);
    return {};
}

void World::Impl::reindex(std::size_t first, std::size_t last) noexcept {
    for (auto i = first; i < last; ++i) {
        const auto [value, inserted] = positions.try_emplace(members[i].id(), i);
        if (!inserted)
            *value = i;
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
        state.eligible =
            state.enabled && (state.parent == ObjectId{} || states[*positions.find(state.parent)].eligible);
    }
}

void World::Impl::rebuild_schedule() {
    for (auto& phase : updates)
        phase.clear();
    schedule_dirty = false;
    if (!running)
        return;
    struct Node {
        Scheduled item;

        bool ticks;
    };
    // Every participant takes part in ordering, so prerequisites hold
    // transitively through objects that do not tick in a phase.
    std::vector<Node> order;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& state = states[i];
        const auto id = members[i].id();
        order.push_back({{id, id, state.behavior}, state.eligible && state.begun && state.fault == Fault::none});
        for (const auto& item : state.components)
            order.push_back({{item.id, id, item.behavior},
                             state.eligible && item.enabled && item.begun && item.fault == Fault::none});
    }
    if (!prerequisites.empty()) {
        // Kahn's algorithm; ties keep composition preorder, so the result is deterministic.
        std::unordered_map<ObjectId, std::size_t> index;
        index.reserve(order.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            index.emplace(order[i].item.object, i);
        std::vector<std::size_t> pending(order.size());
        for (const auto& [object, before] : prerequisites)
            pending[index.at(object)] = before.size();
        std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
        for (std::size_t i = 0; i < order.size(); ++i)
            if (!pending[i])
                ready.push(i);
        std::vector<Node> sorted;
        sorted.reserve(order.size());
        while (!ready.empty()) {
            const auto next = ready.top();
            ready.pop();
            sorted.push_back(order[next]);
            if (const auto found = dependents.find(order[next].item.object); found != dependents.end())
                for (const auto object : found->second)
                    if (!--pending[index.at(object)])
                        ready.push(index.at(object));
        }
        order = std::move(sorted);
    }
    for (const auto& node : order) {
        const auto* behavior = node.item.behavior;
        if (!node.ticks || !behavior)
            continue;
        if (behavior->tick)
            updates[std::size_t(TickPhase::pre_physics)].push_back(node.item);
        if (behavior->post_physics_tick)
            updates[std::size_t(TickPhase::post_physics)].push_back(node.item);
        if (behavior->late_tick)
            updates[std::size_t(TickPhase::late)].push_back(node.item);
    }
}

void World::Impl::forget_prerequisites(ObjectId object) noexcept {
    const auto unlink = [&](auto& from, auto& to) {
        const auto found = from.find(object);
        if (found == from.end())
            return;
        for (const auto other : found->second) {
            auto& back = to.at(other);
            std::erase(back, object);
            if (back.empty())
                to.erase(other);
        }
        from.erase(found);
        schedule_dirty = true;
    };
    unlink(prerequisites, dependents);
    unlink(dependents, prerequisites);
}

void World::Impl::rebuild_buckets() {
    for (auto& bucket : buckets)
        bucket.objects.clear();
    const auto add = [&](TypeId type, ObjectId object) {
        auto found = std::ranges::find(buckets, type, &Bucket::type);
        if (found == buckets.end()) {
            buckets.push_back({type, {}});
            found = std::prev(buckets.end());
        }
        found->objects.push_back(object);
    };
    capable.assign(registrations.size(), {});
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto id = members[i].id();
        add(states[i].type, id);
        for (const auto& item : states[i].components) {
            add(item.type, item.id);
            for (std::uint32_t mask = item.capabilities; mask; mask &= mask - 1)
                capable[std::countr_zero(mask)].push_back({id, item.id, states[i].eligible && item.enabled});
        }
    }
    buckets_dirty = false;
}

void World::Impl::cancel_tasks(ObjectId target, bool entity) noexcept {
    std::erase_if(tasks, [&](Task& task) {
        if (target != ObjectId{} && (entity ? task.entity : task.object) != target)
            return false;
        task.coroutine.cancel();
        return true;
    });
}

void World::Impl::halt() noexcept {
    running = false;
    translations.clear();
    cancel_tasks();
    for (auto& phase : updates)
        phase.clear();
    // Faults belong to one run; the next start begins every member again.
    for (auto& member : states) {
        member.fault = Fault::none;
        for (auto& item : member.components)
            item.fault = Fault::none;
    }
    changed(false);
}

std::optional<Error> World::Impl::notify(Lifecycle event, ObjectId object) {
    if (!observer && subscriptions.empty())
        return {};
    auto scope = ExecutionScope::enter(runtime);
    if (!scope)
        return scope.error();
    detail::DispatchScope dispatch{dispatch_depth};
    std::optional<Error> failure;
    const auto owner = [&] {
        if (positions.contains(object))
            return object;
        auto component = detail::component_data(runtime, object);
        return component ? component->owner : ObjectId{};
    };
    const auto invoke = [&](const LifecycleObserver& callback) {
        if (!callback)
            return;
        auto error = detail::callback_error([&] { return callback(world, event, object); },
                                            "World lifecycle observer ran out of memory.",
                                            "World lifecycle observer threw an unknown exception.");
        if (error && !contain(*error, DiagnosticKind::observer_failed, DiagnosticPhase::lifecycle, object, owner()) &&
            !failure)
            failure = std::move(error);
    };
    invoke(observer);
    for (const auto& entry : subscriptions) {
        // A callback may unsubscribe itself; this reference keeps it alive until it returns.
        const auto callback = entry.observer;
        if (callback)
            invoke(*callback);
    }
    return failure;
}

Commit World::Impl::finish(bool changed, std::optional<Error> failure) {
    if (!failure && running) {
        auto applied = apply_translations();
        if (!applied)
            failure = applied.error();
    }
    if (failure)
        (void)shutdown();
    return {changed, std::move(failure)};
}

World::World(Runtime& runtime, const Types& types) : impl_(std::make_unique<Impl>(runtime, types, *this)) {
    const auto registered = runtime.native_type<Entity>();
    if (!registered || *registered != types.entity)
        throw std::invalid_argument("World types must be registered in its Runtime.");
}

World::~World() {
    if (!impl_->closed) {
        const auto result = close();
        // Destruction during a dispatch would leave dangling member_world links.
        if (!result)
            std::terminate();
    }
}

Runtime& World::runtime() const noexcept {
    return impl_->runtime;
}

const Types& World::types() const noexcept {
    return impl_->types;
}

bool World::running() const noexcept {
    return impl_->running;
}

bool World::closed() const noexcept {
    return impl_->closed;
}

std::uint64_t World::revision() const noexcept {
    return impl_->revision;
}

Activity World::activity() const noexcept {
    const auto& state = *impl_;
    Activity result{0, state.tasks.size()};
    for (const auto& member : state.states) {
        const auto ticks = [](const Behavior* behavior) {
            return behavior && (behavior->tick || behavior->post_physics_tick || behavior->late_tick);
        };
        result.update_registrations += state.running && member.begun && member.fault == Impl::Fault::none &&
                                       member.eligible && ticks(member.behavior);
        for (const auto& item : member.components) {
            result.update_registrations += state.running && item.begun && item.fault == Impl::Fault::none &&
                                           member.eligible && item.enabled && ticks(item.behavior);
        }
    }
    return result;
}

bool World::contains(ObjectId entity) const noexcept {
    return impl_->positions.contains(entity);
}

FailurePolicy World::failure_policy() const noexcept {
    return impl_->policy;
}

Result<void> World::set_failure_policy(FailurePolicy policy) {
    ATELIER_TRY(impl_->admit());
    if (impl_->running)
        return error(ErrorCode::busy, "Stop the World before changing its failure policy.");
    if (policy != FailurePolicy::isolate && policy != FailurePolicy::halt)
        return error(ErrorCode::argument_mismatch, "Unknown failure policy.");
    impl_->policy = policy;
    return {};
}

std::vector<Diagnostic> World::take_diagnostics() {
    return std::exchange(impl_->diagnostics, {});
}

std::uint64_t World::dropped_diagnostics() const noexcept {
    return impl_->dropped_diagnostics;
}

void World::report(Diagnostic diagnostic) {
    impl_->report(std::move(diagnostic));
}

bool World::contain(const Error& failure, Diagnostic diagnostic) {
    return impl_->contain(failure, std::move(diagnostic));
}

Result<std::span<const Member>> World::members() const {
    ATELIER_TRY(impl_->admit(false));
    return std::span<const Member>{impl_->members};
}

Result<CapabilityId> World::register_capability(TypeId type, CapabilityPredicate predicate) {
    auto& state = *impl_;
    ATELIER_TRY(state.admit());
    if (!predicate)
        for (std::uint32_t i = 0; i < state.registrations.size(); ++i)
            if (state.registrations[i].type == type && !state.registrations[i].predicate)
                return CapabilityId{i};
    if (state.registrations.size() >= 32)
        return error(ErrorCode::capacity_exceeded, "A World supports at most 32 capability registrations.");
    ATELIER_TRY_BIND(component, runtime().is_a(type, types().component));
    if (!*component)
        return error(ErrorCode::type_mismatch, "A capability requires a Component type.");
    const auto id = CapabilityId{std::uint32_t(state.registrations.size())};
    state.registrations.push_back({type, std::move(predicate)});
    // Evaluate the new registration for current members before publishing any bit.
    std::vector<bool> present;
    for (const auto& member : state.states)
        for (const auto& item : member.components) {
            auto value = state.capabilities_of(item.id, item.type);
            if (!value) {
                state.registrations.pop_back();
                return std::unexpected(value.error());
            }
            present.push_back(*value & Impl::bit(id));
        }
    auto next = present.begin();
    for (auto& member : state.states)
        for (auto& item : member.components)
            if (*next++)
                item.capabilities |= Impl::bit(id);
    state.changed(false);
    return id;
}

Result<std::span<const CapabilityMember>> World::members(CapabilityId id) const {
    ATELIER_TRY(impl_->admit(false));
    if (id.value >= impl_->registrations.size())
        return error(ErrorCode::argument_mismatch, "Unknown capability registration.");
    if (impl_->buckets_dirty)
        impl_->rebuild_buckets();
    return std::span<const CapabilityMember>{impl_->capable[id.value]};
}

Result<void> World::reevaluate_capabilities(ObjectId component) {
    auto& state = *impl_;
    ATELIER_TRY(state.admit());
    ATELIER_TRY_BIND(owner, state.component_owner(component));
    return state.rederive(*owner);
}

Result<void> World::add_admission(TypeId type, Admission admission) {
    auto& state = *impl_;
    ATELIER_TRY(state.admit());
    if (!admission)
        return error(ErrorCode::argument_mismatch, "An admission needs a check.");
    ATELIER_TRY_BIND(component, runtime().is_a(type, types().component));
    if (!*component)
        return error(ErrorCode::type_mismatch, "Admission requires a Component type.");
    state.admissions.push_back({type, std::move(admission)});
    return {};
}

Result<void> World::Impl::check_admissions(std::span<const ObjectId> candidates, ObjectId proposed_owner) {
    // Admissions may read the World; edits and registrations stay busy until they return.
    detail::DispatchScope dispatch{dispatch_depth};
    std::vector<AdmissionCandidate> selected;
    for (const auto& [type, admission] : admissions) {
        selected.clear();
        for (const auto object : candidates) {
            ATELIER_TRY_BIND(object_type, runtime.type_of(object));
            ATELIER_TRY_BIND(matches, runtime.is_a(*object_type, type));
            if (*matches) {
                auto owner = proposed_owner;
                if (owner == ObjectId{}) {
                    ATELIER_TRY_BIND(data, detail::component_data(runtime, object));
                    owner = data->owner;
                }
                selected.push_back({object, owner});
            }
        }
        if (selected.empty())
            continue;
        Result<void> admitted;
        try {
            admitted = admission(world, selected);
        } catch (const std::bad_alloc&) {
            return error(ErrorCode::capacity_exceeded, "A World admission ran out of memory.");
        } catch (const std::exception& exception) {
            return error(ErrorCode::invocation_failed, exception.what());
        } catch (...) {
            return error(ErrorCode::invocation_failed, "A World admission threw an unknown exception.");
        }
        if (!admitted)
            return admitted;
    }
    return {};
}

Result<void> World::Impl::check_admissions(std::span<const ObjectRoot> candidates, ObjectId proposed_owner) {
    if (admissions.empty())
        return {};
    std::vector<ObjectId> ids;
    ids.reserve(candidates.size());
    for (const auto& root : candidates)
        ids.push_back(root.id());
    return check_admissions(std::span<const ObjectId>{ids}, proposed_owner);
}

Result<bool> World::Impl::admitted(ObjectId component) const {
    if (admissions.empty())
        return false;
    ATELIER_TRY_BIND(data, detail::component_data(runtime, component));
    if (data->owner == ObjectId{} || !positions.contains(data->owner))
        return false;
    ATELIER_TRY_BIND(type, runtime.type_of(component));
    for (const auto& entry : admissions) {
        ATELIER_TRY_BIND(matches, runtime.is_a(*type, entry.type));
        if (*matches)
            return true;
    }
    return false;
}

Result<void> World::Impl::check_edit(ObjectId component) {
    ATELIER_TRY_BIND(checked, admitted(component));
    if (!*checked)
        return {};
    return check_admissions(std::span<const ObjectId>{&component, 1});
}

Result<World::SpatialPass> World::spatial_pass(DiagnosticPhase phase) const {
    ATELIER_TRY(impl_->admit(false));
    impl_->begin_pass(phase);
    impl_->space.begin_pass();
    return SpatialPass{*impl_, phase};
}

Result<std::optional<math::Matrix4>> World::SpatialPass::matrix(ObjectId entity) {
    auto& state = *impl_;
    if (!state.positions.contains(entity))
        return error(ErrorCode::invalid_member, "A spatial pass reads only member Entities.");
    auto matrix = state.space.matrix(entity);
    if (matrix)
        return *matrix;
    const auto at = state.space.failed;
    if (!state.contain(matrix.error(), DiagnosticKind::invalid_value, phase_, at, at))
        return std::unexpected(matrix.error());
    return std::nullopt;
}

bool World::SpatialPass::degenerate(ObjectId entity, const math::Matrix4& world, std::string_view message) {
    if (!singular(world) && !impl_->space.entry(entity)->flat)
        return false;
    Diagnostic diagnostic;
    diagnostic.severity = Severity::warning;
    diagnostic.kind = DiagnosticKind::degenerate_value;
    diagnostic.phase = phase_;
    diagnostic.object = diagnostic.entity = impl_->space.degenerate_root(entity);
    diagnostic.field = {"scale"};
    diagnostic.message = message;
    impl_->report(std::move(diagnostic));
    return true;
}

Result<ObjectId> World::SpatialPass::parent(ObjectId entity) {
    if (!impl_->positions.contains(entity))
        return error(ErrorCode::invalid_member, "A spatial pass reads only member Entities.");
    return impl_->space.parent(entity);
}

Result<std::optional<World::Orientation>> World::SpatialPass::orientation(ObjectId entity) {
    ATELIER_TRY_BIND(matrix, this->matrix(entity));
    if (!*matrix)
        return std::nullopt;
    // The chain was validated in this pass. An odd sign pattern is -1 times a
    // pattern with two negative axes, a half turn about the remaining positive one.
    Orientation result;
    const auto composed = [&]() -> Result<void> {
        for (auto node = entity; node != ObjectId{};) {
            const auto* entry = impl_->space.entry(node);
            ATELIER_TRY_BIND(canonical, canonical_transform(entry->local));
            const auto& q = canonical->rotation;
            const auto& s = canonical->scale;
            std::array negative{s.x < 0, s.y < 0, s.z < 0};
            const bool odd = negative[0] ^ negative[1] ^ negative[2];
            if (odd)
                for (auto& axis : negative)
                    axis = !axis;
            result.mirrored ^= odd;
            math::Quaterniond local{q.x, q.y, q.z, q.w};
            if (negative[0] || negative[1] || negative[2]) {
                math::Quaterniond turn{0, 0, 0, 0};
                if (!negative[0])
                    turn.x = 1;
                else if (!negative[1])
                    turn.y = 1;
                else
                    turn.z = 1;
                local = local * turn;
            }
            const auto next = math::try_normalize(local * result.rotation);
            if (!next)
                return error(ErrorCode::argument_mismatch, "Space orientation is not finite.");
            result.rotation = *next;
            node = entry->parent;
        }
        return {};
    }();
    if (composed)
        return result;
    if (!impl_->contain(composed.error(), DiagnosticKind::invalid_value, phase_, entity, entity, {"rotation"}))
        return std::unexpected(composed.error());
    return std::nullopt;
}

Result<std::optional<math::Quaterniond>> World::SpatialPass::rotation(ObjectId entity) {
    ATELIER_TRY_BIND(matrix, this->matrix(entity));
    if (!*matrix)
        return std::nullopt;
    math::Quaterniond result;
    // matrix() validated and cached the actual spatial chain, including explicit origins.
    for (auto node = entity; node != ObjectId{};) {
        const auto* entry = impl_->space.entry(node);
        const auto& q = entry->local.rotation;
        const auto local = math::try_normalize(math::Quaterniond{q.x, q.y, q.z, q.w});
        const auto next = local ? math::try_normalize(*local * result) : std::nullopt;
        if (!next) {
            const auto failure = Error{ErrorCode::argument_mismatch, "Space rotation is not finite or normalizable."};
            if (!impl_->contain(failure, DiagnosticKind::invalid_value, phase_, node, node, {"rotation"}))
                return std::unexpected(failure);
            return std::nullopt;
        }
        result = *next;
        node = entry->parent;
    }
    return result;
}

Result<std::uint64_t> World::add_simulation(Simulation simulation) {
    auto& state = *impl_;
    ATELIER_TRY(state.admit());
    if (!simulation.step || !simulation.writes_pose)
        return error(ErrorCode::argument_mismatch, "A simulation needs step and writes_pose.");
    std::erase_if(state.simulations, [](const Impl::SimulationEntry& entry) { return !entry.simulation; });
    const auto id = state.next_simulation++;
    state.simulations.push_back({id, std::make_shared<const Simulation>(std::move(simulation))});
    return id;
}

void World::remove_simulation(std::uint64_t id) noexcept {
    for (auto& entry : impl_->simulations)
        if (entry.id == id)
            entry.simulation.reset();
    if (!impl_->updating)
        std::erase_if(impl_->simulations, [](const Impl::SimulationEntry& entry) { return !entry.simulation; });
}

Result<std::vector<ObjectId>> World::query(TypeId type, bool include_derived) const {
    ATELIER_TRY(impl_->admit(false));
    ATELIER_TRY_BIND(info, runtime().describe(type));
    if (impl_->buckets_dirty)
        impl_->rebuild_buckets();
    std::vector<ObjectId> result;
    for (const auto& bucket : impl_->buckets) {
        ATELIER_TRY_BIND(matches,
                         include_derived ? runtime().is_a(bucket.type, type) : Result<bool>{bucket.type == type});
        if (*matches)
            result.insert(result.end(), bucket.objects.begin(), bucket.objects.end());
    }
    return result;
}

Result<Commit> World::attach_components(ObjectId entity_id, std::span<const Attachment> incoming, std::size_t index) {
    auto& state = *impl_;
    ATELIER_TRY(state.admit());
    ATELIER_TRY_BIND(entity, state.composition_entity(entity_id));
    ATELIER_TRY_BIND(existing, components(runtime(), entity_id));
    if (index == append)
        index = existing->size();
    if (index > existing->size())
        return error(ErrorCode::argument_mismatch, "Component insertion index is out of range.");
    if (incoming.empty())
        return Commit{};
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> references;
    Set<ObjectId> seen;
    roots.reserve(incoming.size());
    references.reserve(incoming.size());
    for (const auto& item : *existing)
        seen.add(item.id());
    for (const auto& item : incoming) {
        if (!seen.add(item.component))
            return error(ErrorCode::invalid_member, "Component is already present or duplicated.");
        ATELIER_TRY_BIND(data, detail::component_data(runtime(), item.component));
        if (data->owner != ObjectId{})
            return error(ErrorCode::invalid_member, "Component already has an owner.");
        ATELIER_TRY_BIND(held, runtime().hold(item.component, types().component));
        references.push_back(held->reference());
        roots.push_back(std::move(*held));
    }
    // Detached composition is the preparation phase: capabilities may be added
    // before their owner's Transform. Complete spatial requirements are checked
    // by attach_batch before any member, observer or behavior is published.
    if (entity->world)
        ATELIER_TRY(detail::requires_space(runtime(), entity_id, roots));
    if (entity->world)
        ATELIER_TRY(state.check_admissions(roots, entity_id));
    ATELIER_TRY_BIND(held, runtime().hold(entity_id, types().entity));
    auto changed = runtime().with_native<Entity>(entity_id, [&](Entity& value) -> Result<void> {
        ATELIER_TRY_BIND(sequence, runtime().reference_field(entity_id, Access::sequence(value)));
        std::vector<ReferenceChange> changes;
        changes.reserve(incoming.size() + 1);
        changes.push_back(SequenceSplice{*sequence, index, 0, references});
        for (const auto& item : incoming) {
            auto owner = runtime().with_native<Component>(item.component, [&](Component& value) {
                return runtime().reference_field(item.component, Access::owner(value));
            });
            if (!owner)
                return std::unexpected(owner.error());
            changes.push_back(ReferenceWrite{*owner, held->reference()});
        }
        return runtime().apply_reference_changes(changes);
    });
    if (!changed)
        return std::unexpected(changed.error());
    if (!entity->world)
        return Commit{true, {}};
    if (auto rederived = state.rederive(entity_id); !rederived)
        return state.finish(true, rederived.error());
    std::optional<Error> failure;
    for (const auto& item : incoming) {
        auto notification = state.notify(Lifecycle::attached, item.component);
        if (!failure)
            failure = std::move(notification);
    }
    if (!failure && state.running) {
        auto at = state.member(entity_id);
        failure = at ? state.begin(*at, *at + 1) : std::optional<Error>{at.error()};
    }
    return state.finish(true, std::move(failure));
}

Result<ComponentRemoval> World::remove_component(ObjectId object) {
    auto& state = *impl_;
    ATELIER_TRY(state.admit());
    ATELIER_TRY_BIND(data, state.composition_component(object));
    const auto owner = data->owner;
    if (owner == ObjectId{})
        return error(ErrorCode::invalid_member, "Component is not attached.");
    ATELIER_TRY_BIND(entity, state.composition_entity(owner));
    const bool registered = entity->world != nullptr;
    ATELIER_TRY_BIND(group, components(runtime(), owner));
    const auto at = std::ranges::find_if(*group, [&](const ObjectRoot& item) { return item.id() == object; });
    if (at == group->end())
        return error(ErrorCode::invalid_member, "Component is absent from its owner's composition.");
    ATELIER_TRY_BIND(root, runtime().hold(object, types().component));
    ATELIER_TRY_BIND(status, inspect_component(runtime(), object));
    ComponentRemoval result;
    result.root = std::move(*root);
    result.owner = owner;
    result.index = std::size_t(at - group->begin());
    result.origin_ = state.lifetime;
    if (registered)
        state.cancel_tasks(object, false);
    std::optional<Error> failure;
    if (registered) {
        auto position = state.member(owner);
        failure = position ? state.end(*position, *position + 1, object) : std::optional<Error>{position.error()};
    }
    if (status->active) {
        auto notification = state.notify(Lifecycle::deactivated, object);
        if (!failure)
            failure = std::move(notification);
    }
    {
        detail::FlagScope transition{state.transitioning};
        auto removed = detail::clear_component(runtime(), owner, object, result.index);
        if (!removed) {
            if (registered)
                state.halt();
            return std::unexpected(removed.error());
        }
        if (registered) {
            if (auto* member = state.state_of(owner))
                std::erase_if(member->components, [&](const Impl::ComponentState& item) { return item.id == object; });
            state.forget_prerequisites(object);
            state.changed(false);
        }
    }
    if (!registered) {
        result.commit = Commit{true, {}};
        return result;
    }
    auto detached = state.notify(Lifecycle::detached, object);
    if (!failure)
        failure = std::move(detached);
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
Result<void> queue_translation(Runtime& runtime, ObjectId object, math::Vector3 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
        return error(ErrorCode::argument_mismatch, "Translation must be finite.");
    ATELIER_TRY_BIND(component, component_data(runtime, object));
    if (component->owner == ObjectId{})
        return error(ErrorCode::invalid_member, "Translation requires an owned Component.");
    ATELIER_TRY_BIND(entity, entity_data(runtime, component->owner));
    if (!entity->world)
        return error(ErrorCode::invalid_member, "Translation requires World membership.");
    auto& state = Access::state(*entity->world);
    if (!state.running || state.closing || state.transitioning || !state.dispatch_depth)
        return error(ErrorCode::busy, "translate_owner requires a running World behavior phase.");
    ATELIER_TRY_BIND(spatial, has_transform(runtime, component->owner));
    if (!*spatial)
        return error(ErrorCode::invalid_member, "Translation requires an Entity Transform.");
    state.translations.push_back({object, component->owner, delta});
    return {};
}

Result<void> queue_request(Runtime& runtime, ObjectId object, RequestKind kind, bool enabled) {
    auto owner = object;
    if (kind != RequestKind::destroy_entity && kind != RequestKind::entity_enabled) {
        ATELIER_TRY_BIND(component, component_data(runtime, object));
        owner = component->owner;
    }
    ATELIER_TRY_BIND(entity, owner == ObjectId{} ? Result<EntityData>{EntityData{}} : entity_data(runtime, owner));
    if (!entity->world)
        return error(ErrorCode::invalid_member, "World requests require World membership.");
    Request request;
    request.kind = kind;
    request.target = object;
    request.enabled = enabled;
    ATELIER_TRY_BIND(queued, entity->world->request(request));
    return {};
}
}

Result<void> World::Impl::apply_translations() {
    if (translations.empty())
        return {};
    // No callbacks execute while applying. Retain buffers between frames, and
    // discard the batch on every exit so a rejected action cannot replay later.
    struct Cleanup {
        Impl& state;

        ~Cleanup() {
            state.translations.clear();
            state.translation_changes.clear();
        }
    } cleanup{*this};
    auto& changes = translation_changes;
    changes.reserve(translations.size());
    // Each command is checked alone: a rejected one is reported and dropped.
    const auto check = [&](const Translation& command) -> Result<void> {
        ATELIER_TRY_BIND(owner, component_owner(command.component));
        if (*owner != command.entity)
            return error(ErrorCode::invalid_member, "Translation receiver changed owner.");
        for (const auto& entry : simulations)
            if (entry.simulation && entry.simulation->writes_pose(command.entity))
                return error(
                    ErrorCode::unsupported_operation,
                    "translate_owner cannot move an Entity whose pose a simulation writes; use that system's actions.");
        auto found = std::ranges::find(changes, command.entity, &TranslationChange::entity);
        if (found == changes.end()) {
            ATELIER_TRY_BIND(matrix, world_matrix(runtime, command.entity));
            ATELIER_TRY_BIND(value, relative_transform(*matrix, math::Matrix4::identity()));
            changes.push_back({command.entity, *value});
            found = std::prev(changes.end());
        }
        auto value = found->value;
        value.position.x += command.delta.x;
        value.position.y += command.delta.y;
        value.position.z += command.delta.z;
        ATELIER_TRY_BIND(canonical, canonical_transform(value));
        found->value = *canonical;
        return {};
    };
    for (const auto& command : translations)
        if (auto checked = check(command);
            !checked && !contain(checked.error(), DiagnosticKind::command_rejected, DiagnosticPhase::command,
                                 command.component, command.entity, {}, Severity::warning))
            return checked;
    std::vector<SimulationPose> poses;
    poses.reserve(changes.size());
    for (const auto& change : changes)
        poses.push_back({change.entity, change.value.position, change.value.rotation});
    return apply_poses(poses, DiagnosticPhase::command);
}

// An edit may not make a valid object fail its type rules or an admission; a
// rejected edit restores the previous value. Objects already invalid stay
// editable so their faults can be repaired one field at a time.
template <class Write, class Restore>
Result<void> World::Impl::checked_write(ObjectId object, bool entity, Write&& write, Restore&& restore) {
    ATELIER_TRY_BIND(before, runtime.validate(object));
    ATELIER_TRY_BIND(candidate, entity ? Result<bool>{false} : admitted(object));
    ATELIER_TRY(write());
    const auto reject = [&](Error failure) -> Result<void> {
        if (auto restored = restore(); !restored)
            return error(restored.error().code, "The rejected value could not be restored (" +
                                                    restored.error().message +
                                                    "); it was rejected because: " + failure.message);
        return std::unexpected(std::move(failure));
    };
    if (!*before) {
        auto after = runtime.validate(object);
        if (!after)
            return reject(after.error());
        if (*after)
            return reject((*after)->error);
    }
    if (*candidate)
        if (auto valid = check_admissions(std::span<const ObjectId>{&object, 1}); !valid)
            return reject(valid.error());
    return {};
}

// Exchanges a scalar leaf with `old`, the exact value read before a write.
static Result<void> restore_scalar(Runtime& runtime, ObjectId object, std::span<const FieldId> path, Scalar& old) {
    return std::visit(
        [&]<class T>(T& value) -> Result<void> {
            if constexpr (std::is_same_v<T, EnumValue>)
                return runtime.restore(object, path, ValueOutput{value.type, &value.bits});
            else {
                ATELIER_TRY_BIND(type, runtime.native_type<T>());
                return runtime.restore(object, path, ValueOutput{*type, &value});
            }
        },
        old);
}

Result<Scalar> World::read(ObjectId component, std::span<const FieldId> path) const {
    ATELIER_TRY(impl_->admit(false));
    ATELIER_TRY_BIND(entity, detail::is(runtime(), component, types().entity));
    if (*entity) {
        ATELIER_TRY_BIND(data, impl_->composition_entity(component));
    } else {
        ATELIER_TRY_BIND(data, impl_->composition_component(component));
    }
    return runtime().read(component, path);
}

Result<void> World::write(ObjectId object, std::span<const FieldId> path, Scalar value) {
    ATELIER_TRY(impl_->admit_edit(object));
    ATELIER_TRY_BIND(transform, detail::is(runtime(), object, types().entity));
    const auto match = [&](const auto& paths) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < paths.size(); ++i)
            if (std::ranges::equal(path, paths[i]))
                return i;
        return {};
    };
    const auto position = match(types().position_fields);
    const auto rotation = match(types().rotation_fields);
    const auto scale = match(types().scale_fields);
    if (*transform && (position || rotation || scale)) {
        ATELIER_TRY_BIND(current, read_transform(runtime(), object));
        if (!std::holds_alternative<float>(value))
            return error(ErrorCode::type_mismatch, "Transform fields require float values.");
        if (position) {
            std::array fields{&current->position.x, &current->position.y, &current->position.z};
            *fields[*position] = std::get<float>(value);
        } else if (rotation) {
            std::array fields{&current->rotation.x, &current->rotation.y, &current->rotation.z, &current->rotation.w};
            *fields[*rotation] = std::get<float>(value);
        } else if (scale) {
            std::array fields{&current->scale.x, &current->scale.y, &current->scale.z};
            *fields[*scale] = std::get<float>(value);
        }
        return set_transform(object, *current);
    }
    ATELIER_TRY_BIND(previous, runtime().read(object, path));
    return impl_->checked_write(
        object, *transform, [&] { return runtime().write(object, path, value); },
        [&] { return restore_scalar(runtime(), object, path, *previous); });
}

Result<void> World::write(ObjectId object, std::span<const FieldPath> paths, std::span<const Scalar> values) {
    ATELIER_TRY(impl_->admit_edit(object));
    if (paths.empty() || paths.size() != values.size())
        return error(ErrorCode::argument_mismatch, "A group write needs one value per field.");
    ATELIER_TRY_BIND(entity, detail::is(runtime(), object, types().entity));
    const auto transform = [&](const FieldPath& path, const auto& leaves) {
        return std::ranges::any_of(leaves,
                                   [&](const auto& leaf) { return !path.empty() && leaf.front() == path.front(); });
    };
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (*entity && (transform(paths[i], types().position_fields) || transform(paths[i], types().rotation_fields) ||
                        transform(paths[i], types().scale_fields)))
            return error(ErrorCode::unsupported_operation,
                         "A group write cannot contain Transform fields; use set_transform.");
        for (std::size_t j = 0; j < i; ++j)
            if (paths[j] == paths[i])
                return error(ErrorCode::argument_mismatch, "A group write names the same field more than once.");
    }
    std::vector<Scalar> previous;
    previous.reserve(paths.size());
    for (const auto& path : paths) {
        ATELIER_TRY_BIND(value, runtime().read(object, path));
        previous.push_back(*value);
    }
    std::size_t written = 0;
    const auto restore = [&]() -> Result<void> {
        while (written > 0) {
            --written;
            ATELIER_TRY(restore_scalar(runtime(), object, paths[written], previous[written]));
        }
        return {};
    };
    return impl_->checked_write(
        object, *entity,
        [&]() -> Result<void> {
            for (; written < paths.size(); ++written)
                if (auto leaf = runtime().write(object, paths[written], values[written]); !leaf) {
                    if (auto restored = restore(); !restored)
                        return error(restored.error().code,
                                     "The group's earlier values could not be restored (" + restored.error().message +
                                         "); a field was rejected because: " + leaf.error().message);
                    return leaf;
                }
            return {};
        },
        restore);
}

Result<void> World::write(ObjectId object, std::span<const FieldId> path, ValueInput value) {
    ATELIER_TRY(impl_->admit_edit(object));
    ATELIER_TRY_BIND(entity, detail::is(runtime(), object, types().entity));
    const auto transform = [&](const auto& leaves) {
        return std::ranges::any_of(leaves,
                                   [&](const auto& leaf) { return !path.empty() && leaf.front() == path.front(); });
    };
    if (*entity &&
        (transform(types().position_fields) || transform(types().rotation_fields) || transform(types().scale_fields)))
        return error(ErrorCode::unsupported_operation,
                     "Transform fields are written as scalars or with set_transform.");
    ATELIER_TRY_BIND(info, runtime().describe(value.type));
    struct Previous {
        Runtime& runtime;

        TypeId type;

        std::align_val_t alignment;

        std::span<std::byte> storage;

        bool live{};

        Previous(Runtime& owner, const TypeInfo& info)
            : runtime(owner), type(info.id), alignment{info.alignment},
              storage{static_cast<std::byte*>(::operator new(info.size, alignment)), info.size} {}

        Previous(const Previous&) = delete;

        Previous& operator=(const Previous&) = delete;

        ~Previous() {
            if (live)
                (void)runtime.destroy_value(type, storage);
            ::operator delete(storage.data(), alignment);
        }
    } previous{runtime(), **info};
    ATELIER_TRY(runtime().initialize_value(value.type, previous.storage));
    previous.live = true;
    ATELIER_TRY(runtime().read(object, path, ValueOutput{value.type, previous.storage.data()}));
    return impl_->checked_write(
        object, *entity, [&] { return runtime().write(object, path, value); },
        [&] { return runtime().restore(object, path, ValueOutput{value.type, previous.storage.data()}); });
}

}

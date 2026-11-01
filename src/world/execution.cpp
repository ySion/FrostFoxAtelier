#include "world/detail.hpp"
#include "world/timing.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace atelier::world {
using detail::Access;

using detail::error;

Result<Commit> World::set_enabled(ObjectId object, bool enabled) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    auto entity_kind = detail::is(runtime(), object, types().entity);
    if (!entity_kind)
        return std::unexpected(entity_kind.error());
    if (*entity_kind) {
        auto data = state.composition_entity(object);
        if (!data)
            return std::unexpected(data.error());
        if (data->enabled == enabled)
            return Commit{};
        if (!data->world) {
            auto owned = detail::subtree(runtime(), object);
            if (!owned)
                return std::unexpected(owned.error());
            for (const auto& root : *owned) {
                auto member = state.composition_entity(root.id());
                if (!member)
                    return std::unexpected(member.error());
                if (member->world)
                    return error(ErrorCode::invalid_member, "Subtree has inconsistent World membership.");
            }
        }
        std::size_t first{};
        std::size_t count{};
        std::vector<bool> before;
        if (data->world) {
            auto at = state.member(object);
            if (!at)
                return std::unexpected(at.error());
            first = *at;
            count = state.states[first].size;
            before.reserve(count);
            for (auto i = first; i < first + count; ++i)
                before.push_back(state.states[i].eligible);
        }
        {
            detail::FlagScope transition{state.transitioning};
            auto changed = runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
                Access::enabled(entity) = enabled;
                return {};
            });
            if (!changed)
                return std::unexpected(changed.error());
            if (data->world) {
                state.states[first].enabled = enabled;
                state.refresh_eligibility(first, count);
                state.changed(false);
            }
        }
        if (!data->world)
            return Commit{true, {}};
        std::optional<Error> failure;
        // Entity notifications cover its capability registrations. Emit only
        // actual effective transitions, without repeating Begin/End.
        for (std::size_t i = count; i-- > 0;) {
            if (!before[i] || state.states[first + i].eligible)
                continue;
            const auto id = state.members[first + i].id();
            state.cancel_tasks(id);
            if (state.running) {
                auto notified = state.notify(Lifecycle::deactivated, id);
                if (!failure)
                    failure = std::move(notified);
            }
        }
        if (state.running)
            for (std::size_t i = 0; i < count; ++i) {
                if (before[i] || !state.states[first + i].eligible)
                    continue;
                auto notified = state.notify(Lifecycle::activated, state.members[first + i].id());
                if (!failure)
                    failure = std::move(notified);
            }
        return state.finish(true, std::move(failure));
    }
    auto data = state.composition_component(object);
    if (!data)
        return std::unexpected(data.error());
    if (data->enabled == enabled)
        return Commit{};
    bool registered{};
    bool owner_enabled{};
    if (data->owner != ObjectId{}) {
        auto owner = state.composition_entity(data->owner);
        if (!owner)
            return std::unexpected(owner.error());
        registered = owner->world != nullptr;
        auto eligible = effective_enabled(runtime(), data->owner);
        if (!eligible)
            return std::unexpected(eligible.error());
        owner_enabled = *eligible;
    }
    {
        detail::FlagScope transition{state.transitioning};
        auto changed = runtime().with_native<Component>(object, [&](Component& component) -> Result<void> {
            Access::enabled(component) = enabled;
            return {};
        });
        if (!changed)
            return std::unexpected(changed.error());
        if (!enabled)
            state.cancel_tasks(object, false);
        if (registered) {
            if (auto* item = state.component_state(data->owner, object))
                item->enabled = enabled;
            state.changed(false);
        }
    }
    if (!registered)
        return Commit{true, {}};
    return state.finish(true, state.running && owner_enabled
                                  ? state.notify(enabled ? Lifecycle::activated : Lifecycle::deactivated, object)
                                  : std::nullopt);
}

std::optional<Error> World::Impl::invoke(const Scheduled& item, bool beginning) {
    auto root = runtime.hold(item.object);
    if (!root)
        return root.error();
    auto execution = ExecutionScope::enter(runtime);
    if (!execution)
        return execution.error();
    detail::DispatchScope dispatch{dispatch_depth};
    try {
        if (item.behavior) {
            const auto& callback = beginning ? item.behavior->begin : item.behavior->end;
            if (callback) {
                auto result = callback(runtime, item.object);
                if (!result)
                    return result.error();
            }
        }
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity_exceeded, "World object lifecycle callback ran out of memory."};
    } catch (const std::exception& exception) {
        return Error{ErrorCode::invocation_failed, exception.what()};
    } catch (...) {
        return Error{ErrorCode::invocation_failed, "World object lifecycle callback threw an unknown exception."};
    }
    return {};
}

// Callbacks cannot change membership or registrations (dispatch guard), so
// state references stay valid across each invocation. An isolated Begin
// failure leaves only that object unbegun; its components and children continue.
std::optional<Error> World::Impl::begin(std::size_t first, std::size_t last, ObjectId component) {
    const auto begin_one = [&](ObjectId object, ObjectId entity, const Behavior* behavior, bool& begun,
                               Fault& fault) -> std::optional<Error> {
        if (begun || fault != Fault::none)
            return {};
        if (auto failure = invoke({object, entity, behavior}, true)) {
            if (!contain(*failure, DiagnosticKind::behavior_failed, DiagnosticPhase::begin, object, entity))
                return failure;
            fault = Fault::begin;
            return {};
        }
        begun = true;
        schedule_dirty = true;
        return {};
    };
    for (auto i = first; i < last; ++i) {
        const auto entity = members[i].id();
        if (component == ObjectId{})
            if (auto failure = begin_one(entity, entity, states[i].behavior, states[i].begun, states[i].fault))
                return failure;
        for (auto& item : states[i].components) {
            if (component != ObjectId{} && item.id != component)
                continue;
            if (auto failure = begin_one(item.id, entity, item.behavior, item.begun, item.fault))
                return failure;
        }
    }
    return {};
}

std::optional<Error> World::Impl::end(std::size_t first, std::size_t last, ObjectId component) {
    std::optional<Error> failure;
    // Clear before invoking: End is attempted exactly once, including failures.
    for (auto i = last; i-- > first;) {
        const auto entity = members[i].id();
        auto& group = states[i].components;
        for (auto it = group.rbegin(); it != group.rend(); ++it) {
            if ((component != ObjectId{} && it->id != component) || !it->begun)
                continue;
            it->begun = false;
            schedule_dirty = true;
            auto ended = invoke({it->id, entity, it->behavior}, false);
            if (ended && !contain(*ended, DiagnosticKind::behavior_failed, DiagnosticPhase::end, it->id, entity) &&
                !failure)
                failure = std::move(ended);
        }
        if (component == ObjectId{} && states[i].begun) {
            states[i].begun = false;
            schedule_dirty = true;
            auto ended = invoke({entity, entity, states[i].behavior}, false);
            if (ended && !contain(*ended, DiagnosticKind::behavior_failed, DiagnosticPhase::end, entity, entity) &&
                !failure)
                failure = std::move(ended);
        }
    }
    return failure;
}

std::optional<Error> World::Impl::shutdown() {
    const bool was_running = running;
    halt(); // Cancel tasks before End and suppress all further Tick calls.
    auto failure = end();
    // Keep the graph and registration indexes available to cleanup observers.
    // Reverse the current composition order, including after running reparent.
    if (was_running)
        for (auto i = members.size(); i-- > 0;) {
            if (!states[i].eligible)
                continue;
            auto notified = notify(Lifecycle::deactivated, members[i].id());
            if (!failure)
                failure = std::move(notified);
        }
    return failure;
}

Result<Commit> World::start() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    if (state.running)
        return Commit{};
    state.running = true;
    state.changed(false);
    auto failure = state.begin();
    for (std::size_t i = 0; !failure && i < state.members.size(); ++i)
        if (state.states[i].eligible)
            failure = state.notify(Lifecycle::activated, state.members[i].id());
    return state.finish(true, std::move(failure));
}

Result<Commit> World::clear_faults() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    bool cleared = false;
    const auto clear = [&](Impl::Fault& fault) {
        cleared |= fault != Impl::Fault::none;
        fault = Impl::Fault::none;
    };
    for (auto& member : state.states) {
        clear(member.fault);
        for (auto& item : member.components)
            clear(item.fault);
    }
    if (!cleared)
        return Commit{};
    state.schedule_dirty = true;
    return state.finish(true, state.begin());
}

Result<Commit> World::stop() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    if (!state.running)
        return Commit{};
    auto failure = state.shutdown();
    return state.finish(true, std::move(failure));
}

Result<Commit> World::close() {
    auto& state = *impl_;
    if (state.closed)
        return Commit{};
    if (auto ready = state.admit(); !ready)
        return std::unexpected(ready.error());
    state.closing = true;
    state.lifetime->open = false;
    // closing blocks mutation, but deactivated observers still need read-only
    // access to the complete registered graph to release subsystem state.
    auto failure = state.shutdown();
    std::vector<ObjectId> exiting;
    for (auto i = state.members.size(); i-- > 0;) {
        const auto& group = state.states[i].components;
        for (auto it = group.rbegin(); it != group.rend(); ++it)
            exiting.push_back(it->id);
        exiting.push_back(state.members[i].id());
    }
    {
        detail::FlagScope transition{state.transitioning};
        for (const auto& member : state.members) {
            auto cleared = runtime().with_native<Entity>(member.id(), [&](Entity& entity) -> Result<void> {
                Access::member_world(entity) = nullptr;
                return {};
            });
            if (!cleared && !failure)
                failure = cleared.error();
        }
        state.states.clear();
        state.buckets.clear();
        state.positions.clear();
        state.prerequisites.clear();
        state.dependents.clear();
        state.changed();
    }
    // Member roots retain every object until all reverse deregistrations finish.
    // The public directory is already empty and every object is Detached.
    auto roots = std::move(state.members);
    state.members.clear();
    for (const auto object : exiting) {
        auto notified = state.notify(Lifecycle::detached, object);
        if (!failure)
            failure = std::move(notified);
    }
    state.closed = true;
    state.closing = false;
    roots.clear();
    // Pending requests hold identities/conditions only; flush returns failures.
    return {Commit{true, std::move(failure)}};
}

Result<const World::Behavior*> World::Impl::behavior(TypeId type) const {
    for (;;) {
        const auto found = std::ranges::find(behaviors, type, &Binding::type);
        if (found != behaviors.end())
            return &found->behavior;
        auto info = runtime.describe(type);
        if (!info)
            return std::unexpected(info.error());
        if (!(*info)->base)
            return nullptr;
        type = *(*info)->base;
    }
}

Result<void> World::bind_behavior(TypeId type, Behavior binding) {
    if (auto ready = impl_->admit(); !ready)
        return ready;
    auto component = runtime().is_a(type, types().component);
    if (!component)
        return std::unexpected(component.error());
    auto entity = runtime().is_a(type, types().entity);
    if (!entity)
        return std::unexpected(entity.error());
    if (!*component && !*entity)
        return error(ErrorCode::type_mismatch, "Behavior requires an Entity or Component type.");
    auto& state = *impl_;
    auto found = std::ranges::find(state.behaviors, type, &Impl::Binding::type);
    if (found != state.behaviors.end()) {
        // Same address and type resolution; only the callbacks change.
        found->behavior = std::move(binding);
        state.changed(false);
        return {};
    }
    // Bindings keep their addresses, so a failed re-resolution leaves every registration valid.
    state.behaviors.push_back({type, std::move(binding)});
    std::vector<const Behavior*> resolved;
    for (const auto& member : state.states) {
        auto own = state.behavior(member.type);
        if (!own) {
            state.behaviors.pop_back();
            return std::unexpected(own.error());
        }
        resolved.push_back(*own);
        for (const auto& item : member.components) {
            auto bound = state.behavior(item.type);
            if (!bound) {
                state.behaviors.pop_back();
                return std::unexpected(bound.error());
            }
            resolved.push_back(*bound);
        }
    }
    auto next = resolved.begin();
    for (auto& member : state.states) {
        member.behavior = *next++;
        for (auto& item : member.components)
            item.behavior = *next++;
    }
    state.changed(false);
    return {};
}

Result<void> World::add_tick_prerequisite(ObjectId object, ObjectId prerequisite) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return ready;
    if (object == prerequisite)
        return error(ErrorCode::argument_mismatch, "An object cannot be its own tick prerequisite.");
    for (const auto id : {object, prerequisite}) {
        auto entity = detail::is(runtime(), id, types().entity);
        if (!entity)
            return std::unexpected(entity.error());
        if (*entity) {
            if (auto at = state.member(id); !at)
                return std::unexpected(at.error());
        } else if (auto owner = state.component_owner(id); !owner)
            return std::unexpected(owner.error());
    }
    if (const auto found = state.prerequisites.find(object);
        found != state.prerequisites.end() && std::ranges::find(found->second, prerequisite) != found->second.end())
        return {};
    // The edge closes a cycle when `object` already precedes `prerequisite`.
    std::vector<ObjectId> stack{object};
    std::unordered_set<ObjectId> seen{object};
    while (!stack.empty()) {
        const auto next = stack.back();
        stack.pop_back();
        if (next == prerequisite)
            return error(ErrorCode::argument_mismatch, "Tick prerequisites would form a cycle.");
        if (const auto found = state.dependents.find(next); found != state.dependents.end())
            for (const auto later : found->second)
                if (seen.insert(later).second)
                    stack.push_back(later);
    }
    state.prerequisites[object].push_back(prerequisite);
    state.dependents[prerequisite].push_back(object);
    state.schedule_dirty = true;
    return {};
}

Result<void> World::observe_lifecycle(LifecycleObserver observer) {
    if (auto ready = impl_->admit(); !ready)
        return ready;
    impl_->observer = std::move(observer);
    return {};
}

Result<std::uint64_t> World::subscribe_lifecycle(LifecycleObserver observer) {
    if (auto ready = impl_->admit(); !ready)
        return std::unexpected(ready.error());
    if (!observer)
        return error(ErrorCode::argument_mismatch, "Lifecycle subscription is empty.");
    if (impl_->next_subscription == std::numeric_limits<std::uint64_t>::max())
        return error(ErrorCode::capacity_exceeded, "Lifecycle subscription identity space is exhausted.");
    std::erase_if(impl_->subscriptions, [](const Impl::Subscription& entry) { return !entry.observer; });
    const auto id = impl_->next_subscription++;
    impl_->subscriptions.push_back({id, std::make_shared<LifecycleObserver>(std::move(observer))});
    return id;
}

void World::unsubscribe_lifecycle(std::uint64_t id) noexcept {
    if (impl_->dispatch_depth) {
        for (auto& entry : impl_->subscriptions) {
            if (entry.id != id)
                continue;
            entry.observer.reset();
            return;
        }
    }
    std::erase_if(impl_->subscriptions, [id](const Impl::Subscription& entry) { return entry.id == id; });
}

Result<void> World::start_task(ObjectId object, const TaskStart& starter) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return ready;
    if (!starter)
        return error(ErrorCode::argument_mismatch, "Task starter is empty.");
    auto entity_kind = detail::is(runtime(), object, types().entity);
    if (!entity_kind)
        return std::unexpected(entity_kind.error());
    ObjectId entity_id;
    if (*entity_kind) {
        auto member = state.member(object);
        if (!member)
            return std::unexpected(member.error());
        auto status = inspect_entity(runtime(), object);
        if (!status)
            return std::unexpected(status.error());
        if (status->state != EntityState::active)
            return error(ErrorCode::invalid_member, "Tasks require an active Entity.");
        entity_id = object;
    } else {
        auto owner = state.component_owner(object);
        if (!owner)
            return std::unexpected(owner.error());
        auto status = inspect_component(runtime(), object);
        if (!status)
            return std::unexpected(status.error());
        if (!status->valid || !status->active)
            return error(ErrorCode::invalid_member, "Tasks require an active Component.");
        entity_id = *owner;
    }
    if (const auto* fault = state.fault_of(object, entity_id); fault && *fault != Impl::Fault::none)
        return error(ErrorCode::invalid_member, "Tasks require an object without an isolated failure.");
    auto root = runtime().hold(object);
    if (!root)
        return std::unexpected(root.error());
    state.tasks.reserve(state.tasks.size() + 1);
    auto execution = ExecutionScope::enter(runtime());
    if (!execution)
        return std::unexpected(execution.error());
    detail::DispatchScope dispatch{state.dispatch_depth};
    try {
        auto task = starter(runtime(), object);
        if (!task)
            return std::unexpected(task.error());
        if (task->state() != CoroutineState::ready && task->state() != CoroutineState::suspended)
            return error(ErrorCode::argument_mismatch, "World accepts only ready or suspended tasks.");
        state.tasks.push_back({object, entity_id, std::move(*task)});
    } catch (const std::exception& exception) {
        return error(ErrorCode::invocation_failed, exception.what());
    } catch (...) {
        return error(ErrorCode::invocation_failed, "Task starter threw an unknown exception.");
    }
    return {};
}

std::optional<Error> World::Impl::run_phase(TickPhase phase, float amount) {
    struct PhaseMarkers {
        const timing::Marker& phase;

        const timing::Marker& tick;

        const timing::Marker& commands;

        const timing::Marker& flush;
    };
    static constexpr std::array<PhaseMarkers, 3> markers{
        {{phases::pre_physics, phases::pre_physics_tick, phases::pre_physics_commands, phases::pre_physics_flush},
         {phases::post_physics, phases::post_physics_tick, phases::post_physics_commands, phases::post_physics_flush},
         {phases::late, phases::late_tick, phases::late_commands, phases::late_flush}}};
    const auto& marked = markers[std::size_t(phase)];
    if (schedule_dirty) {
        timing::Scope scheduling{phases::schedule};
        rebuild_schedule();
    }
    timing::Scope timed{marked.phase};
    std::optional<Error> failure;
    std::optional<timing::Scope> resuming;
    {
        std::optional<timing::Scope> ticking{std::in_place, marked.tick};
        auto execution = ExecutionScope::enter(runtime);
        if (!execution)
            return execution.error();
        detail::DispatchScope dispatch{dispatch_depth};
        // Registration arrays cannot change under this guard. Member roots keep
        // their composition alive and ExecutionScope prevents GC, but a previous
        // native Tick may still explicitly destroy the next receiver.
        // An isolated failure stops that object's Tick from the next schedule.
        for (const auto& item : updates[std::size_t(phase)]) {
            const auto object = item.object;
            auto type = runtime.type_of(object);
            if (!type) {
                failure = type.error();
                break;
            }
            const auto& tick = phase == TickPhase::pre_physics    ? item.behavior->tick
                               : phase == TickPhase::post_physics ? item.behavior->post_physics_tick
                                                                  : item.behavior->late_tick;
            std::optional<Error> error;
            try {
                auto invoked = tick(runtime, object, amount);
                if (!invoked)
                    error = invoked.error();
            } catch (const std::bad_alloc&) {
                error = Error{ErrorCode::capacity_exceeded, "World object Tick ran out of memory."};
            } catch (const std::exception& exception) {
                error = Error{ErrorCode::invocation_failed, exception.what()};
            } catch (...) {
                error = Error{ErrorCode::invocation_failed, "World object Tick threw an unknown exception."};
            }
            if (!error)
                continue;
            if (!contain(*error, DiagnosticKind::behavior_failed, DiagnosticPhase::tick, object, item.entity)) {
                failure = std::move(error);
                break;
            }
            fault_tick(object, item.entity);
        }
        ticking.reset();
        if (!failure && phase == TickPhase::pre_physics) {
            resuming.emplace(phases::pre_physics_tasks);
            // Cancelling erases tasks, so faults apply after the resume loop.
            std::vector<std::pair<ObjectId, ObjectId>> faulted;
            for (auto& task : tasks) {
                if (std::ranges::any_of(faulted, [&](const auto& other) { return other.first == task.object; }))
                    continue;
                auto stepped = task.coroutine.resume();
                if (stepped)
                    continue;
                if (!contain(stepped.error(), DiagnosticKind::task_failed, DiagnosticPhase::task, task.object,
                             task.entity)) {
                    failure = stepped.error();
                    break;
                }
                faulted.emplace_back(task.object, task.entity);
            }
            for (const auto& [object, entity] : faulted)
                fault_tick(object, entity);
        }
    }
    if (failure)
        return failure;
    if (phase == TickPhase::pre_physics)
        std::erase_if(tasks, [](const Task& task) {
            const auto state = task.coroutine.state();
            return state == CoroutineState::completed || state == CoroutineState::cancelled ||
                   state == CoroutineState::failed;
        });
    resuming.reset();
    {
        timing::Scope commanding{marked.commands};
        if (auto applied = apply_translations(); !applied)
            return applied.error();
    }
    timing::Scope flushing{marked.flush};
    return flush_requests();
}

Result<void> World::update(float amount) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready)
        return ready;
    if (state.updating)
        return error(ErrorCode::busy, "World update cannot be nested.");
    if (!std::isfinite(amount) || amount < 0.f)
        return error(ErrorCode::argument_mismatch, "World update amount must be finite and nonnegative.");
    if (!state.running)
        return {};
    timing::Scope timed{phases::update};
    detail::FlagScope updating{state.updating};
    state.begin_pass(DiagnosticPhase::physics);
    state.begin_pass(DiagnosticPhase::command);
    const auto fail = [&](Error failure) -> Result<void> {
        (void)state.shutdown();
        return std::unexpected(std::move(failure));
    };
    if (auto failure = state.run_phase(TickPhase::pre_physics, amount))
        return fail(std::move(*failure));
    if (!state.running)
        return {};
    std::optional<timing::Scope> simulating{std::in_place, phases::simulation};
    std::vector<std::shared_ptr<const Simulation>> systems;
    for (const auto& entry : state.simulations)
        if (entry.simulation)
            systems.push_back(entry.simulation);
    for (const auto& system : systems) {
        if (!state.running)
            return {};
        if (std::ranges::none_of(state.simulations, [&](const auto& entry) { return entry.simulation == system; }))
            continue;
        std::optional<Error> failure;
        try {
            auto stepped = system->step(amount);
            if (!stepped)
                failure = stepped.error();
        } catch (const std::bad_alloc&) {
            failure = Error{ErrorCode::capacity_exceeded, "World simulation step ran out of memory."};
        } catch (const std::exception& exception) {
            failure = Error{ErrorCode::invocation_failed, exception.what()};
        } catch (...) {
            failure = Error{ErrorCode::invocation_failed, "World simulation step threw an unknown exception."};
        }
        if (failure)
            return fail(std::move(*failure));
    }
    simulating.reset();
    for (const auto phase : {TickPhase::post_physics, TickPhase::late}) {
        if (!state.running)
            return {};
        if (auto failure = state.run_phase(phase, amount))
            return fail(std::move(*failure));
    }
    return {};
}

Result<std::uint64_t> World::request(Request command) {
    auto& state = *impl_;
    if (state.closing || state.closed)
        return error(ErrorCode::unsupported_operation, "World is closing or closed.");
    if (state.next_request == std::numeric_limits<std::uint64_t>::max())
        return error(ErrorCode::capacity_exceeded, "World request identity space is exhausted.");
    Impl::Pending pending{state.next_request, command, {}, false};
    switch (command.kind) {
    case RequestKind::attach_entity: {
        auto entity = detail::entity_data(runtime(), command.target);
        if (!entity)
            return std::unexpected(entity.error());
        if (entity->world)
            return error(ErrorCode::invalid_member, "Queued attach requires a detached Entity.");
        break;
    }
    case RequestKind::detach_entity:
    case RequestKind::destroy_entity:
    case RequestKind::entity_enabled: {
        auto member = state.member(command.target);
        if (!member)
            return std::unexpected(member.error());
        pending.member_required = true;
        break;
    }
    case RequestKind::attach_component: {
        auto member = state.member(command.entity);
        if (!member)
            return std::unexpected(member.error());
        auto component = detail::component_data(runtime(), command.target);
        if (!component)
            return std::unexpected(component.error());
        if (component->owner != ObjectId{})
            return error(ErrorCode::invalid_member, "Queued component attach requires an unowned component.");
        pending.owner = command.entity;
        break;
    }
    case RequestKind::remove_component:
    case RequestKind::destroy_component:
    case RequestKind::component_enabled: {
        auto owner = state.component_owner(command.target);
        if (!owner)
            return std::unexpected(owner.error());
        pending.owner = *owner;
        pending.member_required = true;
        break;
    }
    case RequestKind::start:
    case RequestKind::stop:
    case RequestKind::close:
        break;
    default:
        return error(ErrorCode::argument_mismatch, "Unknown World request.");
    }
    state.pending.push_back(std::move(pending));
    return state.next_request++;
}

Result<Commit> World::Impl::apply_request(const Pending& item) {
    const auto& command = item.request;
    if (closed || closing) {
        if (command.kind == RequestKind::close)
            return Commit{};
        return error(ErrorCode::unsupported_operation, "World closed before the queued operation committed.");
    }
    if (item.member_required) {
        if (item.owner != ObjectId{}) {
            auto owner = component_owner(command.target);
            if (!owner)
                return std::unexpected(owner.error());
            if (*owner != item.owner)
                return error(ErrorCode::invalid_member, "Queued component operation has a different owner.");
        } else {
            auto at = member(command.target);
            if (!at)
                return std::unexpected(at.error());
        }
    }
    switch (command.kind) {
    case RequestKind::attach_entity:
        return world.attach(command.target, command.index, command.parent);
    case RequestKind::detach_entity: {
        auto removed = world.detach(command.target);
        if (!removed)
            return std::unexpected(removed.error());
        return std::move(removed->commit);
    }
    case RequestKind::attach_component: {
        auto at = member(item.owner);
        if (!at)
            return std::unexpected(at.error());
        const std::array added{Attachment{command.target}};
        return world.attach_components(item.owner, added, command.index);
    }
    case RequestKind::remove_component: {
        auto removed = world.remove_component(command.target);
        if (!removed)
            return std::unexpected(removed.error());
        return std::move(removed->commit);
    }
    case RequestKind::destroy_entity:
        return world::destroy_entity(runtime, command.target);
    case RequestKind::destroy_component:
        return world::destroy_component(runtime, command.target);
    case RequestKind::entity_enabled:
    case RequestKind::component_enabled:
        return world.set_enabled(command.target, command.enabled);
    case RequestKind::start:
        return world.start();
    case RequestKind::stop:
        return world.stop();
    case RequestKind::close:
        return world.close();
    }
    return error(ErrorCode::argument_mismatch, "Unknown World request.");
}

std::optional<Error> World::Impl::flush_requests() {
    if (pending.empty())
        return {};
    if (auto ready = runtime.check_code_publication(); !ready)
        return ready.error();
    auto batch = std::move(pending);
    pending.clear();
    detail::FlagScope scope{flushing};
    for (const auto& item : batch) {
        auto result = apply_request(item);
        // A committed failure already stopped the World.
        if (result) {
            if (result->notification_error)
                return result->notification_error;
            continue;
        }
        const auto target = item.request.target;
        if (target == ObjectId{})
            return result.error();
        const auto entity = item.owner != ObjectId{} ? item.owner : target;
        if (!contain(result.error(), DiagnosticKind::command_rejected, DiagnosticPhase::command, target, entity, {},
                     Severity::warning))
            return result.error();
    }
    return {};
}

Result<std::vector<RequestCompletion>> World::flush() {
    auto& state = *impl_;
    if (state.dispatch_depth || state.transitioning || state.flushing)
        return error(ErrorCode::busy, "Cannot recursively flush World requests.");
    if (auto ready = runtime().check_code_publication(); !ready)
        return std::unexpected(ready.error());
    std::vector<RequestCompletion> completed;
    completed.reserve(state.pending.size());
    auto pending = std::move(state.pending);
    state.pending.clear();
    detail::FlagScope flushing{state.flushing};
    for (const auto& item : pending)
        completed.push_back({item.serial, state.apply_request(item)});
    return completed;
}

}

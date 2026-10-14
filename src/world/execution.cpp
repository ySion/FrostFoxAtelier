#include "world/detail.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<Commit> World::set_enabled(ObjectId object, bool enabled) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto entity_kind = detail::is(runtime(), object, types().entity);
    if (!entity_kind) return std::unexpected(entity_kind.error());
    if (*entity_kind) {
        auto data = state.composition_entity(object);
        if (!data) return std::unexpected(data.error());
        if (data->enabled == enabled) return Commit{};
        auto owned = detail::subtree(runtime(), object);
        if (!owned) return std::unexpected(owned.error());
        std::vector<bool> before;
        before.reserve(owned->size());
        for (const auto& root : *owned) {
            auto member = state.composition_entity(root.id());
            if (!member) return std::unexpected(member.error());
            if (member->world != data->world) return error(ErrorCode::invalid_member, "Subtree has inconsistent World membership.");
            auto eligible = effective_enabled(runtime(), root.id());
            if (!eligible) return std::unexpected(eligible.error());
            before.push_back(*eligible);
        }
        if (data->world) state.prepare_activity();
        {
            detail::FlagScope transition{state.transitioning};
            auto changed = runtime().with_native<Entity>(object, [&](Entity& entity) -> Result<void> {
                Access::enabled(entity) = enabled; return {};
            });
            if (!changed) return std::unexpected(changed.error());
        }
        if (!data->world) return Commit{true, {}};
        if (auto refreshed = state.refresh(); !refreshed) return state.finish(true, refreshed.error());
        std::optional<Error> failure;
        // Entity notifications cover its capability registrations. Emit only
        // actual effective transitions, without repeating Begin/End.
        for (std::size_t i = owned->size(); i-- > 0;) {
            auto eligible = effective_enabled(runtime(), (*owned)[i].id());
            if (!eligible) { if (!failure) failure = eligible.error(); continue; }
            if (before[i] && !*eligible) {
                state.cancel_tasks((*owned)[i].id());
                if (state.running) {
                    auto notified = state.notify(Lifecycle::deactivated, (*owned)[i].id());
                    if (!failure) failure = std::move(notified);
                }
            }
        }
        if (state.running) for (std::size_t i = 0; i < owned->size(); ++i) {
            auto eligible = effective_enabled(runtime(), (*owned)[i].id());
            if (!eligible) { if (!failure) failure = eligible.error(); continue; }
            if (!before[i] && *eligible) {
                auto notified = state.notify(Lifecycle::activated, (*owned)[i].id());
                if (!failure) failure = std::move(notified);
            }
        }
        return state.finish(true, std::move(failure));
    }
    auto data = state.composition_component(object);
    if (!data) return std::unexpected(data.error());
    if (data->enabled == enabled) return Commit{};
    bool registered{}, owner_enabled{};
    if (data->owner != ObjectId{}) {
        auto owner = state.composition_entity(data->owner);
        if (!owner) return std::unexpected(owner.error());
        registered = owner->world != nullptr;
        auto eligible = effective_enabled(runtime(), data->owner);
        if (!eligible) return std::unexpected(eligible.error());
        owner_enabled = *eligible;
    }
    if (registered) state.prepare_activity();
    {
        detail::FlagScope transition{state.transitioning};
        auto changed = runtime().with_native<Component>(object, [&](Component& component) -> Result<void> {
            Access::enabled(component) = enabled; return {};
        });
        if (!changed) return std::unexpected(changed.error());
        if (!enabled) state.cancel_tasks(object, false);
    }
    if (!registered) return Commit{true, {}};
    if (auto refreshed = state.refresh(); !refreshed) return state.finish(true, refreshed.error());
    return state.finish(true, state.running && owner_enabled
        ? state.notify(enabled ? Lifecycle::activated : Lifecycle::deactivated, object) : std::nullopt);
}

std::optional<Error> World::Impl::invoke(const Scheduled& item, bool beginning) {
    auto root = runtime.hold(item.object);
    if (!root) return root.error();
    auto execution = ExecutionScope::enter(runtime);
    if (!execution) return execution.error();
    detail::DispatchScope dispatch{dispatch_depth};
    try {
        if (item.behavior) {
            const auto& callback = beginning ? item.behavior->begin : item.behavior->end;
            if (callback) { auto result = callback(runtime, item.object); if (!result) return result.error(); }
        }
    } catch (const std::exception& exception) {
        return Error{ErrorCode::invocation_failed, exception.what()};
    } catch (...) { return Error{ErrorCode::invocation_failed, "World object lifecycle callback threw an unknown exception."}; }
    return {};
}
Result<bool> World::Impl::begun(const Scheduled& item) const {
    if (item.object == item.entity) {
        auto data = detail::entity_data(runtime, item.object);
        if (!data) return std::unexpected(data.error());
        return data->begun;
    }
    auto data = detail::component_data(runtime, item.object);
    if (!data) return std::unexpected(data.error());
    return data->begun;
}
Result<void> World::Impl::mark_begun(const Scheduled& item, bool value) {
    const auto mark = [value](auto& object) -> Result<void> { Access::begun(object) = value; return {}; };
    return item.object == item.entity ? runtime.with_native<Entity>(item.object, mark)
                                    : runtime.with_native<Component>(item.object, mark);
}
std::optional<Error> World::Impl::begin(ObjectId entity, ObjectId component) {
    for (const auto& item : ordered) {
        if ((entity != ObjectId{} && item.entity != entity) || (component != ObjectId{} && item.object != component)) continue;
        auto started = begun(item);
        if (!started) return started.error();
        if (*started) continue;
        if (auto failure = invoke(item, true)) return failure;
        auto marked = mark_begun(item, true);
        if (!marked) return marked.error();
    }
    return {};
}
std::optional<Error> World::Impl::end(ObjectId entity, ObjectId component) {
    std::optional<Error> failure;
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        if ((entity != ObjectId{} && it->entity != entity) || (component != ObjectId{} && it->object != component)) continue;
        auto started = begun(*it);
        if (!started) { if (!failure) failure = started.error(); continue; }
        if (!*started) continue;
        // Clear before invoking: End is attempted exactly once, including failures.
        auto marked = mark_begun(*it, false);
        if (!marked) { if (!failure) failure = marked.error(); continue; }
        auto ended = invoke(*it, false);
        if (!failure) failure = std::move(ended);
    }
    return failure;
}
std::optional<Error> World::Impl::shutdown() {
    const bool was_running = running;
    halt(); // Cancel tasks before End and suppress all further Tick calls.
    auto failure = end();
    // Keep the graph and registration indexes available to cleanup observers.
    // Reverse the current composition order, including after running reparent.
    if (was_running) for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        if (it->object != it->entity) continue;
        auto eligible = effective_enabled(runtime, it->entity);
        if (!eligible) { if (!failure) failure = eligible.error(); continue; }
        if (*eligible) {
            auto notified = notify(Lifecycle::deactivated, it->entity);
            if (!failure) failure = std::move(notified);
        }
    }
    return failure;
}
Result<Commit> World::start() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (state.running) return Commit{};
    state.prepare_activity();
    state.running = true;
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    auto failure = state.begin();
    if (!failure) { auto active = state.refresh(); if (!active) failure = active.error(); }
    if (!failure) for (const auto& item : state.ordered) {
        if (item.object != item.entity) continue;
        auto eligible = effective_enabled(runtime(), item.entity);
        if (!eligible) { failure = eligible.error(); break; }
        if (*eligible) {
            failure = state.notify(Lifecycle::activated, item.entity);
            if (failure) break;
        }
    }
    return state.finish(true, std::move(failure));
}
Result<Commit> World::stop() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (!state.running) return Commit{};
    auto failure = state.shutdown();
    return state.finish(true, std::move(failure));
}
Result<Commit> World::close() {
    auto& state = *impl_;
    if (state.closed) return Commit{};
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    state.closing = true;
    state.lifetime->open = false;
    // closing blocks mutation, but deactivated observers still need read-only
    // access to the complete registered graph to release subsystem state.
    auto failure = state.shutdown();
    {
        detail::FlagScope transition{state.transitioning};
        for (const auto& member : state.members) {
            auto cleared = runtime().with_native<Entity>(member.id(), [&](Entity& entity) -> Result<void> {
                Access::member_world(entity) = nullptr; return {};
            });
            if (!cleared && !failure) failure = cleared.error();
        }
        state.presentation.clear();state.buckets.clear();state.positions.clear();
    }
    // Member roots retain every object until all reverse deregistrations finish.
    // The public directory is already empty and every object is Detached.
    auto roots = std::move(state.members);
    auto exiting = std::move(state.ordered);
    for (auto it = exiting.rbegin(); it != exiting.rend(); ++it) {
        auto notified = state.notify(Lifecycle::detached, it->object);
        if (!failure) failure = std::move(notified);
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
        if (found != behaviors.end()) return &found->behavior;
        auto info = runtime.describe(type);
        if (!info) return std::unexpected(info.error());
        if (!(*info)->base) return nullptr;
        type = *(*info)->base;
    }
}
Result<void> World::bind_behavior(TypeId type, Behavior binding) {
    if (auto ready = impl_->admit(); !ready) return ready;
    if (impl_->running) return error(ErrorCode::busy, "Stop the World before replacing a behavior binding.");
    auto component = runtime().is_a(type, types().component);
    if (!component) return std::unexpected(component.error());
    auto entity = runtime().is_a(type, types().entity);
    if (!entity) return std::unexpected(entity.error());
    if (!*component && !*entity) return error(ErrorCode::type_mismatch, "Behavior requires an Entity or Component type.");
    auto found = std::ranges::find(impl_->behaviors, type, &Impl::Binding::type);
    if (found == impl_->behaviors.end()) impl_->behaviors.push_back({type, std::move(binding)});
    else found->behavior = std::move(binding);
    return impl_->refresh();
}
Result<void> World::observe_lifecycle(LifecycleObserver observer) {
    if (auto ready = impl_->admit(); !ready) return ready;
    impl_->observer = std::move(observer);
    return {};
}
Result<std::uint64_t> World::subscribe_lifecycle(LifecycleObserver observer) {
    if (auto ready = impl_->admit(); !ready) return std::unexpected(ready.error());
    if (!observer) return error(ErrorCode::argument_mismatch, "Lifecycle subscription is empty.");
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
            if (entry.id != id) continue;
            entry.observer.reset();
            return;
        }
    }
    std::erase_if(impl_->subscriptions, [id](const Impl::Subscription& entry) { return entry.id == id; });
}
Result<void> World::start_task(ObjectId object, const TaskStart& starter) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return ready;
    if (!starter) return error(ErrorCode::argument_mismatch, "Task starter is empty.");
    auto entity_kind = detail::is(runtime(), object, types().entity);
    if (!entity_kind) return std::unexpected(entity_kind.error());
    ObjectId entity_id;
    if (*entity_kind) {
        auto member = state.member(object);
        if (!member) return std::unexpected(member.error());
        auto status = inspect_entity(runtime(), object);
        if (!status) return std::unexpected(status.error());
        if (status->state != EntityState::active) return error(ErrorCode::invalid_member, "Tasks require an active Entity.");
        entity_id = object;
    } else {
        auto owner = state.component_owner(object);
        if (!owner) return std::unexpected(owner.error());
        auto status = inspect_component(runtime(), object);
        if (!status) return std::unexpected(status.error());
        if (!status->valid || !status->active) return error(ErrorCode::invalid_member, "Tasks require an active Component.");
        entity_id = *owner;
    }
    auto root = runtime().hold(object);
    if (!root) return std::unexpected(root.error());
    state.tasks.reserve(state.tasks.size() + 1);
    auto execution = ExecutionScope::enter(runtime());
    if (!execution) return std::unexpected(execution.error());
    detail::DispatchScope dispatch{state.dispatch_depth};
    try {
        auto task = starter(runtime(), object);
        if (!task) return std::unexpected(task.error());
        if (task->state() != CoroutineState::ready && task->state() != CoroutineState::suspended)
            return error(ErrorCode::argument_mismatch, "World accepts only ready or suspended tasks.");
        state.tasks.push_back({object, entity_id, std::move(*task)});
    } catch (const std::exception& exception) {
        return error(ErrorCode::invocation_failed, exception.what());
    } catch (...) { return error(ErrorCode::invocation_failed, "Task starter threw an unknown exception."); }
    return {};
}

Result<void> World::update(float amount) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return ready;
    if (!std::isfinite(amount) || amount < 0.f) return error(ErrorCode::argument_mismatch, "World update amount must be finite and nonnegative.");
    if (!state.running) return {};
    std::optional<Error> failure;
    {
        auto execution = ExecutionScope::enter(runtime());
        if (!execution) return std::unexpected(execution.error());
        detail::DispatchScope dispatch{state.dispatch_depth};
        // Registration arrays cannot change under this guard. Member roots keep
        // their composition alive; each invocation also owns its receiver root.
        for (const auto& item : state.updates) {
            const auto object = item.object;
            auto root = runtime().hold(object);
            if (!root) { failure = root.error(); break; }
            try {
                auto invoked = item.behavior->tick(runtime(), object, amount);
                if (!invoked) { failure = invoked.error(); break; }
            } catch (const std::exception& exception) {
                failure = Error{ErrorCode::invocation_failed, exception.what()}; break;
            } catch (...) { failure = Error{ErrorCode::invocation_failed, "World object Tick threw an unknown exception."}; break; }
        }
        if (!failure) {
            for (auto& task : state.tasks) {
                auto stepped = task.coroutine.resume();
                if (!stepped) { failure = stepped.error(); break; }
            }
        }
    }
    if (!failure) {
        auto applied=state.apply_translations();
        if (!applied) failure=applied.error();
    }
    if (failure) { (void)state.shutdown(); return std::unexpected(std::move(*failure)); }
    std::erase_if(state.tasks, [](const Impl::Task& task) {
        const auto phase = task.coroutine.state();
        return phase == CoroutineState::completed || phase == CoroutineState::cancelled || phase == CoroutineState::failed;
    });
    return {};
}

Result<std::uint64_t> World::request(Request command) {
    auto& state = *impl_;
    if (state.closing || state.closed) return error(ErrorCode::unsupported_operation, "World is closing or closed.");
    if (state.next_request == std::numeric_limits<std::uint64_t>::max()) return error(ErrorCode::capacity_exceeded, "World request identity space is exhausted.");
    Impl::Pending pending{state.next_request, command, {}, false};
    switch (command.kind) {
    case RequestKind::attach_entity: {
        auto entity = detail::entity_data(runtime(), command.target);
        if (!entity) return std::unexpected(entity.error());
        if (entity->world) return error(ErrorCode::invalid_member, "Queued attach requires a detached Entity.");
        break;
    }
    case RequestKind::detach_entity:
    case RequestKind::destroy_entity:
    case RequestKind::entity_enabled: {
        auto member = state.member(command.target);
        if (!member) return std::unexpected(member.error());
        pending.member_required = true;
        break;
    }
    case RequestKind::attach_component: {
        auto member = state.member(command.entity);
        if (!member) return std::unexpected(member.error());
        auto component = detail::component_data(runtime(), command.target);
        if (!component) return std::unexpected(component.error());
        if (component->owner != ObjectId{}) return error(ErrorCode::invalid_member, "Queued component attach requires an unowned component.");
        pending.owner = command.entity;
        break;
    }
    case RequestKind::remove_component:
    case RequestKind::destroy_component:
    case RequestKind::component_enabled: {
        auto owner = state.component_owner(command.target);
        if (!owner) return std::unexpected(owner.error());
        pending.owner = *owner; pending.member_required = true;
        break;
    }
    case RequestKind::start: case RequestKind::stop: case RequestKind::close: break;
    default: return error(ErrorCode::argument_mismatch, "Unknown World request.");
    }
    state.pending.push_back(std::move(pending));
    return state.next_request++;
}

Result<std::vector<RequestCompletion>> World::flush() {
    auto& state = *impl_;
    if (state.dispatch_depth || state.transitioning || state.flushing)
        return error(ErrorCode::busy, "Cannot recursively flush World requests.");
    if (auto ready = runtime().check_code_publication(); !ready) return std::unexpected(ready.error());
    std::vector<RequestCompletion> completed;
    completed.reserve(state.pending.size());
    auto pending = std::move(state.pending);
    state.pending.clear();
    detail::FlagScope flushing{state.flushing};
    for (const auto& item : pending) {
        const auto apply = [&]() -> Result<Commit> {
            const auto& command = item.request;
            if (state.closed || state.closing) {
                if (command.kind == RequestKind::close) return Commit{};
                return error(ErrorCode::unsupported_operation, "World closed before the queued operation committed.");
            }
            if (item.member_required) {
                if (item.owner != ObjectId{}) {
                    auto owner = state.component_owner(command.target);
                    if (!owner) return std::unexpected(owner.error());
                    if (*owner != item.owner) return error(ErrorCode::invalid_member, "Queued component operation has a different owner.");
                } else {
                    auto member = state.member(command.target);
                    if (!member) return std::unexpected(member.error());
                }
            }
            switch (command.kind) {
            case RequestKind::attach_entity: return attach(command.target, command.index, command.parent);
            case RequestKind::detach_entity: {
                auto removed = detach(command.target);
                if (!removed) return std::unexpected(removed.error());
                return std::move(removed->commit);
            }
            case RequestKind::attach_component: {
                auto member = state.member(item.owner);
                if (!member) return std::unexpected(member.error());
                const std::array added{Attachment{command.target}};
                return attach_components(item.owner, added, command.index);
            }
            case RequestKind::remove_component: {
                auto removed = remove_component(command.target);
                if (!removed) return std::unexpected(removed.error());
                return std::move(removed->commit);
            }
            case RequestKind::destroy_entity: return world::destroy_entity(runtime(), command.target);
            case RequestKind::destroy_component: return world::destroy_component(runtime(), command.target);
            case RequestKind::entity_enabled: case RequestKind::component_enabled: return set_enabled(command.target, command.enabled);
            case RequestKind::start: return start();
            case RequestKind::stop: return stop();
            case RequestKind::close: return close();
            }
            return error(ErrorCode::argument_mismatch, "Unknown World request.");
        };
        completed.push_back({item.serial, apply()});
    }
    return completed;
}

}

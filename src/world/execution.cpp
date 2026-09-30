#include "world/detail.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::world {
using detail::Access;
using detail::error;

Result<Commit> World::set_enabled(ObjectId object, bool enabled) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    auto actor_kind = detail::is(runtime(), object, types().actor);
    if (!actor_kind) return std::unexpected(actor_kind.error());
    bool before{}, owner_enabled{true};
    if (*actor_kind) {
        auto member = state.member(object);
        if (!member) return std::unexpected(member.error());
        auto data = detail::actor_data(runtime(), object);
        if (!data) return std::unexpected(data.error());
        before = data->enabled;
    } else {
        auto owner = state.component_owner(object);
        if (!owner) return std::unexpected(owner.error());
        auto data = detail::component_data(runtime(), object);
        if (!data) return std::unexpected(data.error());
        before = data->enabled;
        auto actor = detail::actor_data(runtime(), *owner);
        if (!actor) return std::unexpected(actor.error());
        owner_enabled = actor->enabled;
    }
    if (before == enabled) return Commit{};
    state.prepare_activity();
    {
        detail::FlagScope transition{state.transitioning};
        if (!enabled) state.cancel_tasks(object, *actor_kind);
        auto changed = *actor_kind
            ? runtime().with_native<Actor>(object, [&](Actor& actor) -> Result<void> { Access::enabled(actor) = enabled; return {}; })
            : runtime().with_native<Component>(object, [&](Component& component) -> Result<void> { Access::enabled(component) = enabled; return {}; });
        if (!changed) return std::unexpected(changed.error());
    }
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    const bool activation_changed = state.running && owner_enabled;
    return state.finish(true, activation_changed ? state.notify(enabled ? Lifecycle::activated : Lifecycle::deactivated, object) : std::nullopt);
}

Result<Commit> World::start() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (state.running) return Commit{};
    state.prepare_activity();
    state.running = true;
    auto refreshed = state.refresh();
    if (!refreshed) return state.finish(true, refreshed.error());
    std::optional<Error> failure;
    for (const auto& member : state.members) {
        auto data = detail::actor_data(runtime(), member.id());
        if (!data) { failure = data.error(); break; }
        if (data->enabled) {
            failure = state.notify(Lifecycle::activated, member.id());
            if (failure) break;
        }
    }
    return state.finish(true, std::move(failure));
}
Result<Commit> World::stop() {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    if (!state.running) return Commit{};
    std::optional<Error> failure;
    {
        detail::FlagScope transition{state.transitioning};
        state.halt();
        for (const auto& member : state.members) {
            auto data = detail::actor_data(runtime(), member.id());
            if (!data) { if (!failure) failure = data.error(); continue; }
            if (data->enabled) {
                auto notification = state.notify(Lifecycle::deactivated, member.id());
                if (!failure) failure = std::move(notification);
            }
        }
    }
    return state.finish(true, std::move(failure));
}
Result<Commit> World::close() {
    auto& state = *impl_;
    if (state.closed) return Commit{};
    if (auto ready = state.admit(); !ready) return std::unexpected(ready.error());
    state.closing = true;
    const bool was_running = state.running;
    state.lifetime->open = false;
    state.halt();
    state.presentation.clear();
    std::optional<Error> failure;
    {
        detail::FlagScope transition{state.transitioning};
        for (const auto& member : state.members) {
            auto marked = runtime().with_native<Actor>(member.id(), [&](Actor& actor) -> Result<void> {
                Access::transitioning(actor) = true;
                return {};
            });
            if (!marked && !failure) failure = marked.error();
        }
        for (const auto& member : state.members) {
            auto data = detail::actor_data(runtime(), member.id());
            if (!data) { if (!failure) failure = data.error(); continue; }
            if (was_running && data->enabled) {
                auto notification = state.notify(Lifecycle::deactivated, member.id());
                if (!failure) failure = std::move(notification);
            }
            auto cleared = runtime().with_native<Actor>(member.id(), [&](Actor& actor) -> Result<void> {
                Access::member_world(actor) = nullptr;
                return {};
            });
            if (!cleared && !failure) failure = cleared.error();
            auto notification = state.notify(Lifecycle::detached, member.id());
            if (!failure) failure = std::move(notification);
        }
        for (const auto& member : state.members) {
            auto cleared = runtime().with_native<Actor>(member.id(), [&](Actor& actor) -> Result<void> {
                Access::transitioning(actor) = false;
                return {};
            });
            if (!cleared && !failure) failure = cleared.error();
        }
        state.buckets.clear(); state.positions.clear(); state.members.clear();
    }
    state.closed = true;
    state.closing = false;
    // Pending requests remain available to flush as failed receipts. They contain
    // only identities/conditions and never keep an Actor or the World alive.
    return {Commit{true, std::move(failure)}};
}

Result<void> World::counter_update(CounterUpdate update) {
    if (auto ready = impl_->admit(); !ready) return ready;
    if (impl_->running) return error(ErrorCode::busy, "Stop the World before replacing its Counter updater.");
    impl_->counter_update = std::move(update);
    return {};
}
Result<void> World::observe_lifecycle(LifecycleObserver observer) {
    if (auto ready = impl_->admit(); !ready) return ready;
    impl_->observer = std::move(observer);
    return {};
}
Result<void> World::start_task(ObjectId object, const TaskStart& starter) {
    auto& state = *impl_;
    if (auto ready = state.admit(); !ready) return ready;
    if (!starter) return error(ErrorCode::argument_mismatch, "Task starter is empty.");
    auto status = inspect_component(runtime(), object);
    if (!status) return std::unexpected(status.error());
    if (!status->valid || !status->active) return error(ErrorCode::invalid_member, "Tasks require an active component.");
    auto owner = state.component_owner(object);
    if (!owner) return std::unexpected(owner.error());
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
        state.tasks.push_back({object, *owner, std::move(*task)});
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
        for (const auto object : state.updates) {
            auto root = runtime().hold(object);
            if (!root) { failure = root.error(); break; }
            try {
                Result<void> invoked;
                if (state.counter_update) invoked = state.counter_update(runtime(), object, amount);
                else { const std::array args{Scalar{amount}}; invoked = runtime().call(object, types().counter_advance, args); }
                if (!invoked) { failure = invoked.error(); break; }
            } catch (const std::exception& exception) {
                failure = Error{ErrorCode::invocation_failed, exception.what()}; break;
            } catch (...) { failure = Error{ErrorCode::invocation_failed, "Counter updater threw an unknown exception."}; break; }
        }
        if (!failure) {
            for (auto& task : state.tasks) {
                auto stepped = task.coroutine.resume();
                if (!stepped) { failure = stepped.error(); break; }
            }
        }
    }
    if (failure) { state.halt(); return std::unexpected(std::move(*failure)); }
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
    case RequestKind::attach_actor: {
        auto actor = detail::actor_data(runtime(), command.target);
        if (!actor) return std::unexpected(actor.error());
        if (actor->world) return error(ErrorCode::invalid_member, "Queued attach requires a detached Actor.");
        break;
    }
    case RequestKind::detach_actor:
    case RequestKind::destroy_actor:
    case RequestKind::actor_enabled: {
        auto member = state.member(command.target);
        if (!member) return std::unexpected(member.error());
        pending.member_required = true;
        break;
    }
    case RequestKind::attach_component: {
        auto member = state.member(command.actor);
        if (!member) return std::unexpected(member.error());
        auto component = detail::component_data(runtime(), command.target);
        if (!component) return std::unexpected(component.error());
        if (component->owner != ObjectId{}) return error(ErrorCode::invalid_member, "Queued component attach requires an unowned component.");
        pending.owner = command.actor;
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
            case RequestKind::attach_actor: return attach(command.target, command.index);
            case RequestKind::detach_actor: {
                auto removed = detach(command.target);
                if (!removed) return std::unexpected(removed.error());
                return std::move(removed->commit);
            }
            case RequestKind::attach_component: {
                auto member = state.member(item.owner);
                if (!member) return std::unexpected(member.error());
                const std::array added{Attachment{command.target, command.transform}};
                return attach_components(item.owner, added, command.index);
            }
            case RequestKind::remove_component: {
                auto removed = remove_component(command.target);
                if (!removed) return std::unexpected(removed.error());
                return std::move(removed->commit);
            }
            case RequestKind::destroy_actor: return world::destroy_actor(runtime(), command.target);
            case RequestKind::destroy_component: return world::destroy_component(runtime(), command.target);
            case RequestKind::actor_enabled: case RequestKind::component_enabled: return set_enabled(command.target, command.enabled);
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

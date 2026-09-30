// Acceptance-owned observations around the actual source host bridge.
// Program, version, invocation and generated-code ownership stay in the library.
using PollHook = void (*)(void*, Invocation&, const CollectionProgress&);
struct ExecutionStats {
    std::size_t polls{}, completed_cycles{}, reclaimed{};
    bool entered{}, explicit_cleanup{};
};
struct FixtureInvocation {
    bool make_snapshot{};
    PollHook hook{};
    void* hook_state{};
    ExecutionStats stats;
    ObjectRoot snapshot;
    ObjectRoot output;
    bool failed_output{};
    std::array<std::span<const FieldId>, 3> snapshot_fields;

    FixtureInvocation(bool snapshot_requested = false, PollHook callback = nullptr, void* context = nullptr)
        : make_snapshot(snapshot_requested), hook(callback), hook_state(context) {}

    Result<void> refresh(Invocation& invocation) {
        if (!make_snapshot) return {};
        auto receiver = invocation.frame.object(receiver_slot);
        if (!receiver) return std::unexpected(std::move(receiver.error()));
        for (const auto path : snapshot_fields) {
            auto value = invocation.runtime.read(*receiver, path);
            if (!value) return std::unexpected(std::move(value.error()));
            auto copied = invocation.runtime.write(snapshot.id(), path, *value);
            if (!copied) return copied;
        }
        return {};
    }
    InvocationObserver observer() {
        return {this,
            [](void* context, Invocation& invocation) -> Result<void> {
                auto& state = *static_cast<FixtureInvocation*>(context);
                state.stats.entered = true;
                if (!state.make_snapshot) return {};
                const std::array names{"progress", "state.heat", "state.cycles"};
                for (std::size_t i = 0; i < names.size(); ++i)
                    state.snapshot_fields[i] = fixture_field(invocation.binding, names[i]);
                auto created = invocation.runtime.create(invocation.binding.type);
                if (!created) return std::unexpected(std::move(created.error()));
                state.snapshot = std::move(*created);
                return {};
            },
            [](void* context, Invocation& invocation, const CollectionProgress& progress) -> Result<void> {
                auto& state = *static_cast<FixtureInvocation*>(context);
                ++state.stats.polls;
                state.stats.reclaimed += progress.reclaimed;
                if (progress.work_done != 0 && progress.phase == CollectionPhase::idle)
                    ++state.stats.completed_cycles;
                if (state.hook) state.hook(state.hook_state, invocation, progress);
                return state.refresh(invocation);
            },
            [](void* context, Invocation& invocation) -> Result<void> {
                auto& state = *static_cast<FixtureInvocation*>(context);
                if (!state.make_snapshot) return {};
                auto refreshed = state.refresh(invocation);
                if (!refreshed) return refreshed;
                require(!state.output.registered(), "Business output was published twice.");
                state.output = std::move(state.snapshot);
                return {};
            },
            [](void* context, const Invocation& invocation, bool success) noexcept {
                auto& state = *static_cast<FixtureInvocation*>(context);
                state.stats.explicit_cleanup = invocation.resources_cleared();
                if (!success) {
                    state.failed_output = state.output.registered();
                    // Keep failed fixture roots inside execution teardown. A
                    // finish handoff followed by failure is a protocol error.
                    state.output.reset();
                    state.snapshot.reset();
                }
            }};
    }
};
FixtureInvocation& fixture(Invocation& invocation) {
    return *static_cast<FixtureInvocation*>(invocation.observer.context);
}

struct ExecutionResult {
    BusinessValue value;
    ObjectRoot snapshot;
};
Result<ExecutionResult> finish_fixture(Result<BusinessValue> result, FixtureInvocation& state,
    ExecutionStats* statistics) {
    if (statistics) *statistics = state.stats;
    if (!result) {
        if (state.stats.explicit_cleanup && state.failed_output)
            return std::unexpected(Error{ErrorCode::invocation_failed, "Failed generated code published an output."});
        return std::unexpected(std::move(result.error()));
    }
    if (state.make_snapshot != state.output.registered())
        return std::unexpected(Error{ErrorCode::invocation_failed, "Generated execution violated the host snapshot policy."});
    return ExecutionResult{*result, std::move(state.output)};
}
Result<ExecutionResult> execute_fixture(Runtime& runtime, const Binding& binding, BusinessCode* code,
    ObjectId receiver, std::span<const BusinessValue> arguments, bool request_collection = false,
    ExecutionStats* statistics = nullptr, PollHook hook = nullptr, void* hook_state = nullptr,
    CollectionBudget budget = {1}, bool make_snapshot = false) {
    if (statistics) *statistics = {};
    FixtureInvocation state{make_snapshot, hook, hook_state};
    auto result = source_detail::execute_typed(runtime, binding, code, receiver, arguments,
        request_collection, state.observer(), budget);
    return finish_fixture(std::move(result), state, statistics);
}
Result<ObjectRoot> execute_machine(Runtime& runtime, const Binding& binding, BusinessCode* code,
    ObjectId receiver, float amount, std::uint32_t steps, bool request_collection = false,
    ExecutionStats* statistics = nullptr, PollHook hook = nullptr, void* hook_state = nullptr,
    CollectionBudget budget = {1}) {
    const auto arguments = machine_arguments(amount, steps);
    auto result = execute_fixture(runtime, binding, code, receiver, arguments, request_collection,
        statistics, hook, hook_state, budget, true);
    if (!result) return std::unexpected(std::move(result.error()));
    return std::move(result->snapshot);
}
template<class Entry>
Result<ExecutionResult> invoke_fixture(const Entry& entry, ObjectId receiver,
    std::span<const BusinessValue> arguments, ExecutionStats* statistics = nullptr,
    PollHook hook = nullptr, void* context = nullptr, bool make_snapshot = false) {
    if (statistics) *statistics = {};
    FixtureInvocation state{make_snapshot, hook, context};
    auto result = ProgramAccess::invoke(entry, receiver, arguments, state.observer());
    return finish_fixture(std::move(result), state, statistics);
}
template<class Entry>
Result<ObjectRoot> invoke_machine(const Entry& entry, ObjectId receiver, float amount, std::uint32_t steps,
    ExecutionStats* statistics = nullptr, PollHook hook = nullptr, void* context = nullptr) {
    const auto arguments = machine_arguments(amount, steps);
    auto result = invoke_fixture(entry, receiver, arguments, statistics, hook, context, true);
    if (!result) return std::unexpected(std::move(result.error()));
    return std::move(result->snapshot);
}
template<class Entry>
Result<Coroutine> start_fixture(const Entry& entry, ObjectId receiver,
    std::span<const BusinessValue> arguments) {
    return ProgramAccess::start(entry, receiver, arguments);
}

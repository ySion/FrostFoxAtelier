#include "foundation/tasks.hpp"
#include "foundation/frame_timing.hpp"
#include "memory/fatal.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <new>
#include <thread>

namespace atelier::tasks {
namespace {

constinit thread_local std::size_t body_depth = 0;

class BodyScope {
public:
    BodyScope() noexcept { ++body_depth; }

    ~BodyScope() { --body_depth; }

    BodyScope(const BodyScope&) = delete;

    BodyScope& operator=(const BodyScope&) = delete;

private:
    timing::Suspend timing_;
};

class Process {
public:
    Process() noexcept { arena_.initialize(); }

    ~Process() {
        // Callers and groups must already have joined. No static destructor may
        // submit more work; terminating an arena is not a substitute for join.
        arena_.terminate();
        if (!oneapi::tbb::finalize(scheduler_, std::nothrow))
            fatal("Tasks: the process scheduler could not be finalized.");
    }

    template <class Operation>
    auto execute(Operation&& operation) noexcept {
        // Even same-arena execute switches to its default context. Nested work
        // must retain the executing parent's cancellation chain instead.
        if (in_parallel_body())
            return operation();
        return arena_.execute(std::forward<Operation>(operation));
    }

private:
    // Construct first and keep alive through arena termination and scheduler finalize.
    oneapi::tbb::global_control termination_{oneapi::tbb::global_control::terminate_on_exception, 1};

    const unsigned parallelism_ = std::min(8u, std::max(1u, std::thread::hardware_concurrency() / 2));

    oneapi::tbb::global_control limit_{oneapi::tbb::global_control::max_allowed_parallelism, parallelism_};

    // TBB adds a worker slot when P=1 reserves its only slot for a caller.
    // Leave that slot unreserved so two external callers cannot exceed P=1.
    oneapi::tbb::task_arena arena_{static_cast<int>(parallelism_), parallelism_ == 1 ? 0u : 1u};

    oneapi::tbb::task_scheduler_handle scheduler_{oneapi::tbb::attach{}};
};

Process& process() noexcept {
    static Process owner;
    return owner;
}

struct Cancel {
    oneapi::tbb::task_group_context& context;

    void operator()() const noexcept { context.cancel_group_execution(); }
};

// Only the first failing body writes the pair; other bodies never read it.
// completion() may only run after join, which publishes the body writes to the host.
class FailureSlot {
public:
    void record(Failure failure, oneapi::tbb::task_group_context& context) noexcept {
        if (!failure.code)
            fatal("Tasks: a failure code must be nonzero.");
        if (!claimed_.test_and_set(std::memory_order_relaxed))
            failure_ = failure;
        context.cancel_group_execution();
    }

    std::expected<Completion, Failure> completion(bool cancelled) const noexcept {
        if (failure_.code)
            return std::unexpected(failure_);
        return cancelled ? Completion::cancelled : Completion::completed;
    }

private:
    std::atomic_flag claimed_{};

    Failure failure_{};
};

}

bool in_parallel_body() noexcept {
    return body_depth != 0;
}

std::expected<Completion, Failure>
detail::parallel_for(Range range, const void* body, std::expected<void, Failure> (*invoke)(const void*, Range) noexcept,
                     std::size_t grain, std::stop_token stop) noexcept {
    if (range.begin > range.end || grain == 0)
        fatal("Parallel range must be ordered and grain must be positive.");
    if (range.begin == range.end)
        return stop.stop_requested() ? Completion::cancelled : Completion::completed;
    auto& owner = process();
    oneapi::tbb::task_group_context context{in_parallel_body() ? oneapi::tbb::task_group_context::bound
                                                               : oneapi::tbb::task_group_context::isolated};
    // This invocation's FP settings, not those at the arena's first use. All
    // bodies go through the TBB dispatcher, including P=1 and a single chunk.
    context.capture_fp_settings();
    FailureSlot failure;
    owner.execute([&] noexcept {
        oneapi::tbb::parallel_for(
            oneapi::tbb::blocked_range<std::size_t>{range.begin, range.end, grain},
            [&](const oneapi::tbb::blocked_range<std::size_t>& chunk) noexcept {
                // First binding copies the parent's cancellation bit. Register
                // only on the bound task stack, including an already-stopped token.
                std::stop_callback cancel{stop, Cancel{context}};
                if (context.is_group_execution_cancelled())
                    return;
                BodyScope scope;
                const auto result = invoke(body, {chunk.begin(), chunk.end()});
                if (!result)
                    failure.record(result.error(), context);
            },
            oneapi::tbb::auto_partitioner{}, context);
    });
    // All callbacks have unregistered before join returns. A token stopped
    // while no task was executing still makes the operation cancelled.
    return failure.completion(context.is_group_execution_cancelled() || stop.stop_requested());
}

struct JoinGroup::State {
    explicit State(std::stop_token token) noexcept : stop(token) { context.capture_fp_settings(); }

    Process& owner = process();

    oneapi::tbb::task_group_context context{in_parallel_body() ? oneapi::tbb::task_group_context::bound
                                                               : oneapi::tbb::task_group_context::isolated};

    oneapi::tbb::task_group group{context};

    const std::thread::id thread = std::this_thread::get_id();

    const std::stop_token stop;

    // Appending does not invalidate pointers to earlier callables. Workers never
    // touch the deque itself; its elements remain owned by the submitting thread.
    std::deque<std::move_only_function<std::expected<void, Failure>() const noexcept>> bodies;

    FailureSlot failure;

    // Explicit cancellation must survive first binding and wait's context reset.
    std::atomic<bool> cancelled = false;

    bool joined = false;
};

JoinGroup::JoinGroup(std::stop_token stop) noexcept : state_(std::make_unique<State>(stop)) {}

JoinGroup::~JoinGroup() noexcept {
    cancel();
    if (!state_->joined)
        (void)join(); // Explicit join delivers task failures; abandonment only drains them.
}

void JoinGroup::submit(std::move_only_function<std::expected<void, Failure>() const noexcept> body) noexcept {
    auto& state = *state_;
    if (state.thread != std::this_thread::get_id())
        fatal("Tasks: only the submitting thread may operate a group.");
    if (state.joined)
        fatal("A joined group cannot accept more work.");
    const auto* stored = &state.bodies.emplace_back(std::move(body));
    state.owner.execute([&] noexcept {
        state.group.run([&state, stored] noexcept {
            // task_group::wait resets its context. Keeping cancellation callbacks
            // on executing task stacks guarantees they have all unregistered
            // before the last task completes and wait can reset the context.
            std::stop_callback cancel{state.stop, Cancel{state.context}};
            if (state.cancelled.load(std::memory_order_relaxed))
                state.context.cancel_group_execution();
            if (state.context.is_group_execution_cancelled())
                return;
            BodyScope scope;
            const auto result = (*stored)();
            if (!result)
                state.failure.record(result.error(), state.context);
        });
    });
}

void JoinGroup::cancel() noexcept {
    if (state_->thread != std::this_thread::get_id())
        fatal("Tasks: only the submitting thread may operate a group.");
    if (!state_->joined) {
        state_->cancelled.store(true, std::memory_order_relaxed);
        state_->context.cancel_group_execution();
    }
}

std::expected<Completion, Failure> JoinGroup::join() noexcept {
    auto& state = *state_;
    if (state.thread != std::this_thread::get_id())
        fatal("Tasks: only the submitting thread may operate a group.");
    if (state.joined)
        fatal("A task group can only be joined once.");
    // A backend fault is fatal, never a normal return that could release live captures.
    const bool cancelled = state.owner.execute([&] noexcept { return state.group.wait() == oneapi::tbb::canceled; });
    state.joined = true;
    state.bodies.clear();
    return state.failure.completion(cancelled || state.cancelled.load(std::memory_order_relaxed) ||
                                    state.stop.stop_requested());
}

}

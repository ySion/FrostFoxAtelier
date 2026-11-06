#include "foundation/tasks.hpp"
#include "foundation/frame_timing.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <deque>
#include <exception>
#include <stdexcept>
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
    Process() { arena_.initialize(); }

    ~Process() {
        // Callers and groups must already have joined. No static destructor may
        // submit more work; terminating an arena is not a substitute for join.
        arena_.terminate();
        if (!oneapi::tbb::finalize(scheduler_, std::nothrow)) {
            std::fputs("Tasks: the process scheduler could not be finalized.\n", stderr);
            std::terminate();
        }
    }

    template <class Operation>
    auto execute(Operation&& operation) {
        // Even same-arena execute switches to its default context. Nested work
        // must retain the executing parent's cancellation chain instead.
        if (in_parallel_body())
            return operation();
        return arena_.execute(std::forward<Operation>(operation));
    }

private:
    const unsigned parallelism_ = std::min(8u, std::max(1u, std::thread::hardware_concurrency() / 2));

    oneapi::tbb::global_control limit_{oneapi::tbb::global_control::max_allowed_parallelism, parallelism_};

    // TBB adds a worker slot when P=1 reserves its only slot for a caller.
    // Leave that slot unreserved so two external callers cannot exceed P=1.
    oneapi::tbb::task_arena arena_{static_cast<int>(parallelism_), parallelism_ == 1 ? 0u : 1u};

    oneapi::tbb::task_scheduler_handle scheduler_{oneapi::tbb::attach{}};
};

Process& process() {
    static Process owner;
    return owner;
}

struct Cancel {
    oneapi::tbb::task_group_context& context;

    void operator()() const noexcept { context.cancel_group_execution(); }
};

}

bool in_parallel_body() noexcept {
    return body_depth != 0;
}

Completion detail::parallel_for(Range range, const void* body, void (*invoke)(const void*, Range), std::size_t grain,
                                std::stop_token stop) {
    if (range.begin > range.end || grain == 0)
        throw std::invalid_argument("Parallel range must be ordered and grain must be positive.");
    if (range.begin == range.end)
        return stop.stop_requested() ? Completion::cancelled : Completion::completed;
    auto& owner = process();
    oneapi::tbb::task_group_context context{in_parallel_body() ? oneapi::tbb::task_group_context::bound
                                                               : oneapi::tbb::task_group_context::isolated};
    // This invocation's FP settings, not those at the arena's first use. All
    // bodies go through the TBB dispatcher, including P=1 and a single chunk.
    context.capture_fp_settings();
    owner.execute([&] {
        oneapi::tbb::parallel_for(
            oneapi::tbb::blocked_range<std::size_t>{range.begin, range.end, grain},
            [&](const oneapi::tbb::blocked_range<std::size_t>& chunk) {
                // First binding copies the parent's cancellation bit. Register
                // only on the bound task stack, including an already-stopped token.
                std::stop_callback cancel{stop, Cancel{context}};
                if (context.is_group_execution_cancelled())
                    return;
                BodyScope scope;
                invoke(body, {chunk.begin(), chunk.end()});
            },
            oneapi::tbb::auto_partitioner{}, context);
    });
    // All callbacks have unregistered before join returns. A token stopped
    // while no task was executing still makes the operation cancelled.
    return context.is_group_execution_cancelled() || stop.stop_requested() ? Completion::cancelled
                                                                           : Completion::completed;
}

struct JoinGroup::State {
    explicit State(std::stop_token token) : stop(token) { context.capture_fp_settings(); }

    Process& owner = process();

    oneapi::tbb::task_group_context context{in_parallel_body() ? oneapi::tbb::task_group_context::bound
                                                               : oneapi::tbb::task_group_context::isolated};

    oneapi::tbb::task_group group{context};

    const std::thread::id thread = std::this_thread::get_id();

    const std::stop_token stop;

    // Appending does not invalidate pointers to earlier callables. Workers never
    // touch the deque itself; its elements remain owned by the submitting thread.
    std::deque<std::move_only_function<void() const>> bodies;

    // Explicit cancellation must survive first binding and wait's context reset.
    std::atomic<bool> cancelled = false;

    bool joined = false;
};

JoinGroup::JoinGroup(std::stop_token stop) : state_(std::make_unique<State>(stop)) {}

JoinGroup::~JoinGroup() {
    cancel_and_join();
}

void JoinGroup::submit(std::move_only_function<void() const> body) {
    auto& state = *state_;
    assert(state.thread == std::this_thread::get_id());
    if (state.joined)
        throw std::logic_error("A joined group cannot accept more work.");
    const auto* stored = &state.bodies.emplace_back(std::move(body));
    state.owner.execute([&] {
        state.group.run([&state, stored] {
            // task_group::wait resets its context. Keeping cancellation callbacks
            // on executing task stacks guarantees they have all unregistered
            // before the last task completes and wait can reset the context.
            std::stop_callback cancel{state.stop, Cancel{state.context}};
            if (state.cancelled.load(std::memory_order_relaxed))
                state.context.cancel_group_execution();
            if (state.context.is_group_execution_cancelled())
                return;
            BodyScope scope;
            (*stored)();
        });
    });
}

void JoinGroup::cancel() noexcept {
    assert(state_->thread == std::this_thread::get_id());
    if (!state_->joined) {
        state_->cancelled.store(true, std::memory_order_relaxed);
        state_->context.cancel_group_execution();
    }
}

Completion JoinGroup::join() {
    auto& state = *state_;
    assert(state.thread == std::this_thread::get_id());
    if (state.joined)
        throw std::logic_error("A task group can only be joined once.");
    std::exception_ptr error;
    bool cancelled = false;
    try {
        state.owner.execute([&] {
            try {
                cancelled = state.group.wait() == oneapi::tbb::canceled;
            } catch (...) {
                // wait has already drained and reset the context before throwing.
                error = std::current_exception();
            }
        });
    } catch (...) {
        // Failure to enter the arena cannot be reported as a completed join:
        // releasing captures here could race tasks that are still running.
        std::fputs("Tasks: could not enter the scheduler to drain a group.\n", stderr);
        std::terminate();
    }
    state.joined = true;
    state.bodies.clear();
    if (error)
        std::rethrow_exception(error);
    return cancelled || state.cancelled.load(std::memory_order_relaxed) || state.stop.stop_requested()
               ? Completion::cancelled
               : Completion::completed;
}

void JoinGroup::cancel_and_join() noexcept {
    if (state_->joined)
        return;
    cancel();
    try {
        (void)join();
    } catch (...) {
        // Explicit join delivers task errors; abandonment only drains them.
    }
}

}

#include "../helpers.hpp"
#include "foundation/frame_timing.hpp"
#include "foundation/shared_array.hpp"
#include "foundation/tasks.hpp"

#include <atomic>
#include <thread>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

struct OwnedInput {
    std::thread::id owner = std::this_thread::get_id();

    std::atomic<bool>& wrong_thread;

    ~OwnedInput() {
        if (std::this_thread::get_id() != owner)
            wrong_thread = true;
    }
};

struct UnwindCheck {
    std::atomic<bool>& outside_body;

    ~UnwindCheck() {
        if (!tasks::in_parallel_body())
            outside_body = true;
    }
};
}

void verify_tasks() {
    Array<std::int64_t> values;
    for (std::int64_t i = 0; i < 1009; ++i)
        values.add(i * i + 3 * i + 7);
    const SharedArray<std::int64_t> input{std::move(values)};
    const auto retained = input;
    auto output = input;
    const auto source = input.view();
    const auto destination = output.write().view();

    // Disjoint writes alone are insufficient: an in-place forward loop would
    // overwrite the next chunk's read-only neighbour. Do not execute that race.
    auto in_place = input.array();
    in_place[3] = in_place[3] + 2 * in_place[2];
    in_place[4] = in_place[4] + 2 * in_place[3];
    require(in_place[4] == 153 && in_place[4] != 85,
            "The aliasing counterexample no longer distinguishes the results.");

    timing::Recorder recorder;
    {
        timing::Recorder::Frame frame{recorder};
        require(tasks::parallel_for(
                    {3, 1006},
                    [source, destination](tasks::Range range) {
                        require(tasks::in_parallel_body(), "An outer task did not enter the parallel body contract.");
                        timing::Scope suppressed{timing::frame};
                        require(tasks::parallel_for(
                                    range,
                                    [source, destination](tasks::Range chunk) {
                                        require(tasks::in_parallel_body(),
                                                "A nested task lost the parallel body contract.");
                                        timing::Scope suppressed{timing::frame};
                                        for (auto i = chunk.begin; i < chunk.end; ++i)
                                            destination[i] = source[i] + 2 * source[i - 1];
                                    },
                                    11) == tasks::Completion::completed,
                                "A successful nested operation was cancelled.");
                        require(tasks::in_parallel_body(), "Nested join cleared the outer task contract.");
                    },
                    97) == tasks::Completion::completed,
                "A successful operation was cancelled.");
    }
    require(!tasks::in_parallel_body() && recorder.last().count(timing::frame) == 1 &&
                recorder.last().structure_errors == 0,
            "Join did not restore the caller or task bodies wrote to its Recorder.");
    for (std::int64_t i = 0; i < 1009; ++i) {
        const auto original = i * i + 3 * i + 7;
        const auto expected = i >= 3 && i < 1006 ? 3 * i * i + 5 * i + 17 : original;
        require(output.view()[i] == expected && input.view()[i] == original && retained.view()[i] == original,
                "Parallel output lost a boundary element or changed the shared input snapshot.");
    }

    // Cancellation before a nested context's first binding must survive that
    // binding, skip user work and leave the uncancelled parent usable.
    std::atomic<unsigned> cancelled_calls = 0;
    require(tasks::parallel_for({0, 1},
                                [&](tasks::Range) {
                                    std::stop_source stopped;
                                    stopped.request_stop();
                                    require(tasks::parallel_for(
                                                {0, 1}, [&](tasks::Range) { ++cancelled_calls; }, 1,
                                                stopped.get_token()) == tasks::Completion::cancelled,
                                            "First binding lost a nested operation's cancellation.");
                                    tasks::JoinGroup group;
                                    group.cancel();
                                    group.run([&] { ++cancelled_calls; });
                                    require(group.join() == tasks::Completion::cancelled,
                                            "First binding lost a group's explicit cancellation.");
                                }) == tasks::Completion::completed &&
                cancelled_calls == 0,
            "Cancelled work executed or cancelled its parent.");

    // An already-running body can fail after requesting cancellation. The error
    // must win, all task/capture lifetimes must end, and the pool must stay usable.
    std::atomic<bool> wrong_thread = false;
    std::atomic<bool> outside_body = false;
    auto owned = std::make_shared<OwnedInput>(std::this_thread::get_id(), wrong_thread);
    const std::weak_ptr<OwnedInput> lifetime = owned;
    std::stop_source stop;
    bool failed = false;
    {
        tasks::JoinGroup group{stop.get_token()};
        group.run([owned = std::move(owned), &outside_body] {
            UnwindCheck unwind{outside_body};
            require(owned != nullptr && tasks::in_parallel_body(), "A group task lost its input or body scope.");
        });
        group.run([&] {
            UnwindCheck unwind{outside_body};
            stop.request_stop();
            throw std::runtime_error("The candidate failed after cancellation.");
        });
        try {
            (void)group.join();
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed && lifetime.expired() && !wrong_thread && !outside_body && !tasks::in_parallel_body(),
                "Failed join retained work, released a user capture on a worker, or lost the error/body scope.");
    }
    std::uint32_t result = 0;
    tasks::JoinGroup next;
    next.run([&] { result = 123; });
    require(next.join() == tasks::Completion::completed && result == 123,
            "A failed/cancelled group poisoned the next operation.");
}

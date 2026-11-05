#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace atelier::tasks {

struct Range {
    std::size_t begin;

    std::size_t end;
};

enum class Completion { completed, cancelled };

// Available in every configuration. Includes caller-executed and nested bodies,
// and their exception unwinding. This is a diagnostic boundary, not a proof that
// captures, their construction/destruction or transported errors are Runtime-free.
[[nodiscard]] bool in_parallel_body() noexcept;

namespace detail {
Completion parallel_for(Range, const void* body, void (*invoke)(const void*, Range), std::size_t grain,
                        std::stop_token);
}

// Synchronous fork/join over [begin, end). Grain is an auto-partitioner hint,
// not an upper bound. Borrowed body and buffers live through join, even on error
// or cancellation. Every body (including caller/inline execution) is restricted
// to ordinary values: no Runtime, its Source/JIT, GC, roots or World commands. Prepare
// COW/capacity first; outputs must overlap neither other outputs nor any input.
// Body copies are not dispatched: workers borrow this const-invocable object.
// Fail-fast cancellation joins all work before rethrowing one representative
// error. Partial output is not rolled back; publish only after completed.
template <class Body>
    requires std::is_object_v<Body> && std::invocable<const Body&, Range> &&
             std::same_as<std::invoke_result_t<const Body&, Range>, void>
[[nodiscard]] Completion parallel_for(Range range, const Body& body, std::size_t grain_hint = 1,
                                      std::stop_token stop = {}) {
    return detail::parallel_for(
        range, std::addressof(body),
        [](const void* value, Range chunk) { std::invoke(*static_cast<const Body*>(value), chunk); }, grain_hint, stop);
}

// A single-use structured group. Only its creating thread calls run/cancel/join;
// child bodies can create their own groups. User captures are stored/released on
// the submitting thread, never in a backend task's execute/cancel destructor.
// Normal paths must explicitly join. Destruction cancels and drains, discarding
// task errors during cleanup. Borrowed owners must be declared before the group.
// A failed run drains before propagating, but argument construction happens
// before run: catch such failures outside the group scope, not within it.
class JoinGroup {
public:
    explicit JoinGroup(std::stop_token stop = {});

    ~JoinGroup();

    JoinGroup(const JoinGroup&) = delete;

    JoinGroup& operator=(const JoinGroup&) = delete;

    template <class Body>
        requires std::invocable<const std::decay_t<Body>&> &&
                 std::same_as<std::invoke_result_t<const std::decay_t<Body>&>, void>
    void run(Body&& body) {
        try {
            submit(std::move_only_function<void() const>{std::forward<Body>(body)});
        } catch (...) {
            cancel_and_join();
            throw;
        }
    }

    void cancel() noexcept;

    [[nodiscard]] Completion join();

private:
    struct State;

    void submit(std::move_only_function<void() const>);

    void cancel_and_join() noexcept;

    std::unique_ptr<State> state_;
};

}

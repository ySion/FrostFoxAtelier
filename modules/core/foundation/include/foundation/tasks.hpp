#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
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

// Operation-local nonzero code; item is a logical work item, not a worker index.
struct Failure {
    std::uint32_t code;

    std::size_t item{SIZE_MAX};
};

// Available in every configuration. Includes caller-executed and nested bodies,
// through local destruction on return. This is a diagnostic boundary, not a proof
// that captures or their construction/destruction are Runtime-free.
[[nodiscard]] bool in_parallel_body() noexcept;

namespace detail {
std::expected<Completion, Failure> parallel_for(Range, const void* body,
                                                std::expected<void, Failure> (*invoke)(const void*, Range) noexcept,
                                                std::size_t grain, std::stop_token) noexcept;
}

// Synchronous fork/join over [begin, end). Grain is an auto-partitioner hint,
// not an upper bound. Borrowed body and buffers live through join, even on error
// or cancellation. Every body (including caller/inline execution) is restricted
// to ordinary values: no Runtime, its Source/JIT, GC, roots or World commands. Prepare
// COW/capacity first; outputs must overlap neither other outputs nor any input.
// Body copies are not dispatched: workers borrow this const-invocable object.
// A representative Failure wins over cancellation; all normal paths join first.
// Partial output is not rolled back; publish only after successful completed.
template <class Body>
    requires std::is_object_v<Body> && std::is_nothrow_invocable_v<const Body&, Range> &&
             std::is_nothrow_destructible_v<Body> &&
             std::same_as<std::invoke_result_t<const Body&, Range>, std::expected<void, Failure>>
[[nodiscard]] std::expected<Completion, Failure> parallel_for(Range range, const Body& body, std::size_t grain_hint = 1,
                                                              std::stop_token stop = {}) noexcept {
    return detail::parallel_for(
        range, std::addressof(body),
        [](const void* value, Range chunk) noexcept { return std::invoke(*static_cast<const Body*>(value), chunk); },
        grain_hint, stop);
}

// A single-use structured group. Only its creating thread calls run/cancel/join;
// child bodies can create their own groups. User captures are stored/released on
// the submitting thread, never in a backend task's execute/cancel destructor.
// Normal paths must explicitly join. Destruction cancels and drains, discarding
// task errors during cleanup. Borrowed owners must be declared before the group.
// Prepare fallible captures with Result before run; it only accepts nothrow closures.
class JoinGroup {
public:
    explicit JoinGroup(std::stop_token stop = {}) noexcept;

    ~JoinGroup() noexcept;

    JoinGroup(const JoinGroup&) = delete;

    JoinGroup& operator=(const JoinGroup&) = delete;

    template <class Body>
        requires std::is_nothrow_invocable_v<const std::decay_t<Body>&> &&
                 std::is_nothrow_constructible_v<std::decay_t<Body>, Body> &&
                 std::is_nothrow_move_constructible_v<std::decay_t<Body>> &&
                 std::is_nothrow_destructible_v<std::decay_t<Body>> &&
                 std::same_as<std::invoke_result_t<const std::decay_t<Body>&>, std::expected<void, Failure>>
    void run(Body&& body) noexcept {
        submit(std::move_only_function<std::expected<void, Failure>() const noexcept>{std::forward<Body>(body)});
    }

    void cancel() noexcept;

    [[nodiscard]] std::expected<Completion, Failure> join() noexcept;

private:
    struct State;

    void submit(std::move_only_function<std::expected<void, Failure>() const noexcept>) noexcept;

    std::unique_ptr<State> state_;
};

}

#pragma once

#include "runtime/runtime.hpp"

namespace atelier {

// Covers entry preparation, generated execution and frame teardown. Nested
// invocations in one Runtime prevent publication until the outermost returns.
// The Runtime must outlive the scope; moving transfers the same activation.
class ExecutionScope final {
public:
    [[nodiscard]] static Result<ExecutionScope> enter(Runtime& runtime);
    ~ExecutionScope();
    ExecutionScope(ExecutionScope&& other) noexcept;
    ExecutionScope(const ExecutionScope&) = delete;
    ExecutionScope& operator=(const ExecutionScope&) = delete;
    ExecutionScope& operator=(ExecutionScope&&) = delete;
private:
    explicit ExecutionScope(Runtime& runtime) noexcept : runtime_(&runtime) {}
    Runtime* runtime_;
};

// A synchronous execution owns registrations in caller-provided, stable slots.
// The Runtime and slot storage must outlive this frame. Slot numbers identify
// execution storage, never a second object identity. No value-record stack ABI.
class ExecutionFrame final {
public:
    ExecutionFrame(Runtime& runtime, std::span<ObjectRoot> slots) noexcept;
    ~ExecutionFrame();
    ExecutionFrame(const ExecutionFrame&) = delete;
    ExecutionFrame& operator=(const ExecutionFrame&) = delete;
    ExecutionFrame(ExecutionFrame&&) = delete;
    ExecutionFrame& operator=(ExecutionFrame&&) = delete;

    [[nodiscard]] Result<void> hold(std::uint32_t slot, ObjectId object);
    // Copy a typed holding value, including null or a preserved stale identity.
    [[nodiscard]] Result<void> hold(std::uint32_t slot, const ObjectReference& reference);
    [[nodiscard]] Result<ObjectReference> reference(std::uint32_t slot) const;
    [[nodiscard]] Result<void> create(std::uint32_t slot, TypeId type);
    // Identity only: subsequent Runtime access still validates liveness.
    [[nodiscard]] Result<ObjectId> object(std::uint32_t slot) const;
    // Move the existing registration; no unrooted return-value interval.
    [[nodiscard]] Result<ObjectRoot> take(std::uint32_t slot);
    [[nodiscard]] Result<void> clear(std::uint32_t slot);
    [[nodiscard]] bool empty() const noexcept;

    // A request coalesces with an existing cycle; it never starts GC itself.
    void request_collection() noexcept;
    // Host-cooperative point, outside native borrows. At most one budgeted step.
    [[nodiscard]] Result<CollectionProgress> poll(CollectionBudget budget);

private:
    Runtime& runtime_;
    std::span<ObjectRoot> slots_;
    bool collection_requested_{};
};

}

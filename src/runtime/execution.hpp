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

// Read-only observation inside an active generated invocation. Runtime mutations,
// new roots, native borrows, GC and generated reentry are rejected until this
// scope ends. Queries, nested reference visits and existing root release remain
// available. The scope must end before its execution activation and Runtime.
class ExecutionInspection final {
public:
    [[nodiscard]] static Result<ExecutionInspection> enter(Runtime& runtime);
    ~ExecutionInspection() noexcept;
    ExecutionInspection(ExecutionInspection&& other) noexcept;
    ExecutionInspection(const ExecutionInspection&) = delete;
    ExecutionInspection& operator=(const ExecutionInspection&) = delete;
    ExecutionInspection& operator=(ExecutionInspection&&) = delete;
private:
    explicit ExecutionInspection(Runtime& runtime) noexcept : runtime_(&runtime) {}
    Runtime* runtime_;
};

// An execution owns registrations in caller-provided, stable slots. Coroutine
// owners keep these slots across suspension. Runtime/storage outlive the frame.
// Slot numbers identify
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
    [[nodiscard]] Result<void> hold(std::uint32_t slot, ObjectId object, TypeId declared_type);
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

enum class CoroutineState { ready, running, suspended, completed, failed, cancelled };

struct CoroutineStep {
    // A body returns suspended (without a result) or completed. Other states
    // belong to the owner. Empty completion means void, not a zero scalar.
    CoroutineState state;
    std::optional<Scalar> result{};
};

// Compiler/host-owned stable frame. Destruction releases roots, frame storage
// and code; it must not resume user code, suspend, throw or start new work.
class CoroutineFrame {
public:
    virtual ~CoroutineFrame() = default;
    [[nodiscard]] virtual Result<CoroutineStep> resume() = 0;
    // The owner resolves failure/cancellation precedence before destruction.
    // This notification cannot resume, throw or change the terminal result.
    virtual void on_terminal(CoroutineState, const Error*) noexcept {}
protected:
    // Cooperatively cancel after a successful resume step. Execution failures
    // still win, and the owner discards any result before destroying the frame.
    void request_cancel() noexcept { cancel_requested_ = true; }
private:
    friend class Coroutine;
    bool cancel_requested_{};
};

// Same-thread cooperative execution, with no scheduler. Runtime must outlive
// every nonterminal frame. Each resume uses the ordinary execution admission;
// suspended frames retain their roots/code without retaining an active scope.
class Coroutine final {
public:
    Coroutine() = default;
    [[nodiscard]] static Result<Coroutine> create(Runtime& runtime, std::unique_ptr<CoroutineFrame> frame);
    ~Coroutine();
    Coroutine(Coroutine&& other) noexcept;
    Coroutine& operator=(Coroutine&& other) noexcept;
    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    [[nodiscard]] Result<CoroutineState> resume();
    // Ready/suspended cancellation cleans immediately. Running cancellation
    // is a request: an execution error wins; otherwise cancel wins over the
    // returned step and no result commits. Does not interrupt native code.
    void cancel() noexcept;
    [[nodiscard]] CoroutineState state() const noexcept;
    [[nodiscard]] Result<std::optional<Scalar>> result() const;
private:
    struct Control;
    explicit Coroutine(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}
    std::shared_ptr<Control> control_;
};

}

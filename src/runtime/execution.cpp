#include "runtime/execution.hpp"

#include <utility>
#include <exception>

namespace atelier {
namespace {
auto invalid_slot() {
    return std::unexpected(Error{ErrorCode::argument_mismatch, "Execution root slot is out of range."});
}
}

Result<ExecutionScope> ExecutionScope::enter(Runtime& runtime) {
    auto ready = runtime.enter_execution();
    if (!ready) return std::unexpected(std::move(ready.error()));
    return ExecutionScope(runtime);
}

ExecutionScope::~ExecutionScope() { if (runtime_) runtime_->leave_execution(); }

ExecutionScope::ExecutionScope(ExecutionScope&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)) {}

ExecutionFrame::ExecutionFrame(Runtime& runtime, std::span<ObjectRoot> slots) noexcept
    : runtime_(runtime), slots_(slots) {}

ExecutionFrame::~ExecutionFrame() {
    for (auto& root : slots_) root.reset();
}

Result<void> ExecutionFrame::hold(std::uint32_t slot, ObjectId object_id) {
    if (slot >= slots_.size()) return invalid_slot();
    auto root = runtime_.hold(object_id);
    if (!root) return std::unexpected(std::move(root.error()));
    slots_[slot] = std::move(*root);
    return {};
}

Result<void> ExecutionFrame::hold(std::uint32_t slot, const ObjectReference& reference) {
    if (slot >= slots_.size()) return invalid_slot();
    auto root = runtime_.hold(reference);
    if (!root) return std::unexpected(std::move(root.error()));
    slots_[slot] = std::move(*root);
    return {};
}

Result<void> ExecutionFrame::hold(std::uint32_t slot, ObjectId object, TypeId declared_type) {
    if (slot >= slots_.size()) return invalid_slot();
    auto root = runtime_.hold(object, declared_type);
    if (!root) return std::unexpected(std::move(root.error()));
    slots_[slot] = std::move(*root);
    return {};
}

Result<ObjectReference> ExecutionFrame::reference(std::uint32_t slot) const {
    if (slot >= slots_.size()) return invalid_slot();
    if (!slots_[slot].registered())
        return std::unexpected(Error{ErrorCode::invalid_object, "Execution root slot is empty."});
    return slots_[slot].reference();
}

Result<void> ExecutionFrame::create(std::uint32_t slot, TypeId type) {
    if (slot >= slots_.size()) return invalid_slot();
    auto root = runtime_.create(type);
    if (!root) return std::unexpected(std::move(root.error()));
    slots_[slot] = std::move(*root);
    return {};
}

Result<ObjectId> ExecutionFrame::object(std::uint32_t slot) const {
    if (slot >= slots_.size()) return invalid_slot();
    if (!slots_[slot].registered())
        return std::unexpected(Error{ErrorCode::invalid_object, "Execution root slot is empty."});
    return slots_[slot].id();
}

Result<ObjectRoot> ExecutionFrame::take(std::uint32_t slot) {
    const auto identity = object(slot);
    if (!identity) return std::unexpected(identity.error());
    const auto type = runtime_.type_of(*identity);
    if (!type) return std::unexpected(type.error());
    return std::move(slots_[slot]);
}

Result<void> ExecutionFrame::clear(std::uint32_t slot) {
    if (slot >= slots_.size()) return invalid_slot();
    slots_[slot].reset();
    return {};
}

bool ExecutionFrame::empty() const noexcept {
    for (const auto& root : slots_) if (root.registered()) return false;
    return true;
}

void ExecutionFrame::request_collection() noexcept { collection_requested_ = true; }

Result<CollectionProgress> ExecutionFrame::poll(CollectionBudget budget) {
    const auto ready = runtime_.check_execution_entry();
    if (!ready) return std::unexpected(ready.error());
    if (budget.max_work == 0 || budget.max_time < std::chrono::nanoseconds::zero())
        return std::unexpected(Error{ErrorCode::argument_mismatch,
            "Execution safe points require positive work and nonnegative time limits."});
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        if (!collection_requested_) return CollectionProgress{};
        const auto started = runtime_.begin_collection();
        if (!started) return std::unexpected(started.error());
    }
    collection_requested_ = false;
    return runtime_.collect_step(budget);
}

struct Coroutine::Control {
    Runtime& runtime;
    std::unique_ptr<CoroutineFrame> frame;
    CoroutineState state{CoroutineState::ready};
    bool cancel_requested{};
    std::optional<Scalar> result;
    std::optional<Error> error;
    Control(Runtime& owner, std::unique_ptr<CoroutineFrame> body) : runtime(owner), frame(std::move(body)) {}
};

Result<Coroutine> Coroutine::create(Runtime& runtime, std::unique_ptr<CoroutineFrame> frame) {
    if (!frame) return std::unexpected(Error{ErrorCode::invalid_definition, "Coroutine requires an owned frame."});
    return Coroutine(std::make_shared<Control>(runtime, std::move(frame)));
}

Coroutine::~Coroutine() { cancel(); }
Coroutine::Coroutine(Coroutine&& other) noexcept : control_(std::move(other.control_)) {}
Coroutine& Coroutine::operator=(Coroutine&& other) noexcept {
    if (this != &other) { cancel(); control_ = std::move(other.control_); }
    return *this;
}

CoroutineState Coroutine::state() const noexcept {
    return control_ ? control_->state : CoroutineState::cancelled;
}

void Coroutine::cancel() noexcept {
    const auto active = control_;
    if (!active) return;
    if (active->state == CoroutineState::running) { active->cancel_requested = true; return; }
    if (active->state != CoroutineState::ready && active->state != CoroutineState::suspended) return;
    active->state = CoroutineState::cancelled;
    active->frame.reset();
}

Result<CoroutineState> Coroutine::resume() {
    // A callback can destroy/move the public owner. Only this pin is used below.
    const auto active = control_;
    if (!active) return std::unexpected(Error{ErrorCode::invalid_object, "Coroutine is empty."});
    if (active->state == CoroutineState::running)
        return std::unexpected(Error{ErrorCode::busy, "Coroutine is already running."});
    if (active->state != CoroutineState::ready && active->state != CoroutineState::suspended)
        return std::unexpected(Error{ErrorCode::invalid_object, "Coroutine has already terminated."});
    auto scope = ExecutionScope::enter(active->runtime);
    if (!scope) return std::unexpected(std::move(scope.error()));
    active->state = CoroutineState::running;
    Result<CoroutineStep> step;
    try { step = active->frame->resume(); }
    catch (const std::exception& error) { step = std::unexpected(Error{ErrorCode::invocation_failed, error.what()}); }
    catch (...) { step = std::unexpected(Error{ErrorCode::invocation_failed, "Coroutine raised an unknown exception."}); }
    if (step && (step->state != CoroutineState::suspended && step->state != CoroutineState::completed))
        step = std::unexpected(Error{ErrorCode::invocation_failed, "Coroutine body returned an invalid state."});
    if (step && step->state == CoroutineState::suspended && step->result)
        step = std::unexpected(Error{ErrorCode::invocation_failed, "A suspended coroutine cannot publish a result."});
    if (!step) {
        active->state = CoroutineState::failed;
        active->error = std::move(step.error());
        active->frame.reset();
        return std::unexpected(*active->error);
    }
    if (active->cancel_requested) {
        active->state = CoroutineState::cancelled;
        active->frame.reset();
    } else {
        active->state = step->state;
        if (active->state == CoroutineState::completed) {
            active->result = std::move(step->result);
            active->frame.reset();
        }
    }
    return active->state;
}

Result<std::optional<Scalar>> Coroutine::result() const {
    if (control_ && control_->error) return std::unexpected(*control_->error);
    if (!control_ || control_->state != CoroutineState::completed)
        return std::unexpected(Error{ErrorCode::invalid_object, "Coroutine has no completed result."});
    return control_->result;
}

}

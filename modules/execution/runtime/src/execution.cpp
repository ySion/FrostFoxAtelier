#include "runtime/execution.hpp"

#include <cassert>
#include <utility>

namespace atelier {
namespace {
auto invalid_slot() {
    return std::unexpected(Error{ErrorCode::argument_mismatch, "Execution root slot is out of range."});
}
}

Result<ExecutionScope> ExecutionScope::enter(Runtime& runtime) {
    if (runtime.try_enter_execution())
        return ExecutionScope(&runtime);
    ATELIER_TRY(runtime.enter_execution());
    return ExecutionScope(&runtime);
}

ExecutionScope ExecutionScope::try_enter(Runtime& runtime) noexcept {
    return ExecutionScope(runtime.try_enter_execution() ? &runtime : nullptr);
}

ExecutionScope::~ExecutionScope() {
    if (runtime_)
        runtime_->leave_execution();
}

ExecutionScope::ExecutionScope(ExecutionScope&& other) noexcept : runtime_(std::exchange(other.runtime_, nullptr)) {}

Result<ExecutionInspection> ExecutionInspection::enter(Runtime& runtime) {
    ATELIER_TRY(runtime.enter_execution_inspection());
    return ExecutionInspection(runtime);
}

ExecutionInspection::~ExecutionInspection() noexcept {
    if (runtime_)
        runtime_->leave_execution_inspection();
}

ExecutionInspection::ExecutionInspection(ExecutionInspection&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)) {}

ExecutionFrame::ExecutionFrame(Runtime& runtime, std::span<ObjectRoot> slots) noexcept
    : runtime_(runtime), slots_(slots) {}

ExecutionFrame::ExecutionFrame(ExecutionScope& scope, std::span<ObjectReference> slots) noexcept
    : runtime_(*scope.runtime_), references_(slots) {}

ExecutionFrame::~ExecutionFrame() {
    detail::assert_serial_access();
    for (auto& root : slots_)
        root.reset();
    for (auto& reference : references_)
        reference = {};
}

Result<void> ExecutionFrame::hold(std::uint32_t slot, ObjectId object_id) {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    if (!references_.empty()) {
        ATELIER_TRY_BIND(reference, runtime_.strong_reference(object_id));
        references_[slot] = *reference;
        return {};
    }
    ATELIER_TRY_BIND(root, runtime_.hold(object_id));
    slots_[slot] = std::move(*root);
    return {};
}

Result<void> ExecutionFrame::hold(std::uint32_t slot, const ObjectReference& reference) {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    if (!references_.empty()) {
        ATELIER_TRY(runtime_.validate_reference(reference));
        if (reference.description().form != ValueForm::strong_reference)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Holding slots require a strong reference."});
        references_[slot] = reference;
        return {};
    }
    ATELIER_TRY_BIND(root, runtime_.hold(reference));
    slots_[slot] = std::move(*root);
    return {};
}

Result<void> ExecutionFrame::hold(std::uint32_t slot, ObjectId object, TypeId declared_type) {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    if (!references_.empty()) {
        ATELIER_TRY_BIND(reference, runtime_.strong_reference(object, declared_type));
        references_[slot] = *reference;
        return {};
    }
    ATELIER_TRY_BIND(root, runtime_.hold(object, declared_type));
    slots_[slot] = std::move(*root);
    return {};
}

Result<void> ExecutionFrame::try_acquire(std::uint32_t slot, const ObjectReference& reference) {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    ATELIER_TRY_BIND(strong, runtime_.try_acquire_reference(reference));
    if (!references_.empty()) {
        references_[slot] = *strong;
        return {};
    }
    ATELIER_TRY_BIND(root, runtime_.hold(*strong));
    slots_[slot] = std::move(*root);
    return {};
}

void ExecutionFrame::set_validated_reference(std::uint32_t slot, TypeId type, ObjectId object) noexcept {
    detail::assert_serial_access();
    assert(slot < references_.size());
    references_[slot] = ObjectReference{{type, ValueForm::strong_reference}, object};
}

Result<ObjectReference> ExecutionFrame::reference(std::uint32_t slot) const {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    if (!references_.empty()) {
        if (references_[slot].description().type == TypeId{})
            return std::unexpected(Error{ErrorCode::invalid_object, "Execution reference slot is empty."});
        return references_[slot];
    }
    if (!slots_[slot].registered())
        return std::unexpected(Error{ErrorCode::invalid_object, "Execution root slot is empty."});
    return slots_[slot].reference();
}

Result<void> ExecutionFrame::create(std::uint32_t slot, TypeId type) {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    ATELIER_TRY_BIND(root, runtime_.create(type));
    if (references_.empty())
        slots_[slot] = std::move(*root);
    else
        references_[slot] = root->reference();
    return {};
}

Result<ObjectId> ExecutionFrame::object(std::uint32_t slot) const {
    const auto value = reference(slot);
    if (!value)
        return std::unexpected(value.error());
    return value->id();
}

Result<ObjectRoot> ExecutionFrame::take(std::uint32_t slot) {
    const auto identity = object(slot);
    if (!identity)
        return std::unexpected(identity.error());
    const auto type = runtime_.type_of(*identity);
    if (!type)
        return std::unexpected(type.error());
    if (!references_.empty()) {
        auto root = runtime_.hold(references_[slot]);
        if (root)
            references_[slot] = {};
        return root;
    }
    return std::move(slots_[slot]);
}

Result<void> ExecutionFrame::clear(std::uint32_t slot) {
    detail::assert_serial_access();
    if (slot >= size())
        return invalid_slot();
    if (references_.empty())
        slots_[slot].reset();
    else
        references_[slot] = {};
    return {};
}

bool ExecutionFrame::empty() const noexcept {
    detail::assert_serial_access();
    for (const auto& root : slots_)
        if (root.registered())
            return false;
    for (const auto& reference : references_)
        if (reference.description().type != TypeId{})
            return false;
    return true;
}

struct Coroutine::Control {
    Runtime& runtime;

    std::unique_ptr<CoroutineFrame> frame;

    std::size_t references{1};

    CoroutineState state{CoroutineState::ready};

    bool cancel_requested{};

    std::optional<Scalar> result;

    std::optional<Error> error;

    Control(Runtime& owner, std::unique_ptr<CoroutineFrame> body) : runtime(owner), frame(std::move(body)) {}

    static void release(Control* control) noexcept {
        if (control) {
            assert(control->references != 0);
            if (--control->references == 0)
                delete control;
        }
    }

    struct Pin {
        Control* ptr;

        explicit Pin(Control* control) noexcept : ptr(control) { ++ptr->references; }

        ~Pin() { release(ptr); }

        Pin(const Pin&) = delete;

        Pin& operator=(const Pin&) = delete;
    };
};

Result<Coroutine> Coroutine::create(Runtime& runtime, std::unique_ptr<CoroutineFrame> frame) {
    detail::assert_serial_access();
    if (!frame)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Coroutine requires an owned frame."});
    return Coroutine(new Control(runtime, std::move(frame)));
}

Coroutine::~Coroutine() {
    cancel();
    Control::release(std::exchange(control_, nullptr));
}

Coroutine::Coroutine(Coroutine&& other) noexcept : control_(std::exchange(other.control_, nullptr)) {}

Coroutine& Coroutine::operator=(Coroutine&& other) noexcept {
    if (this != &other) {
        cancel();
        Control::release(control_);
        control_ = std::exchange(other.control_, nullptr);
    }
    return *this;
}

CoroutineState Coroutine::state() const noexcept {
    return control_ ? control_->state : CoroutineState::cancelled;
}

void Coroutine::cancel() noexcept {
    detail::assert_serial_access();
    if (!control_)
        return;
    const Control::Pin pin{control_};
    auto* const active = pin.ptr;
    if (active->state == CoroutineState::running) {
        active->cancel_requested = true;
        return;
    }
    if (active->state != CoroutineState::ready && active->state != CoroutineState::suspended)
        return;
    active->state = CoroutineState::cancelled;
    active->frame->on_terminal(active->state, nullptr);
    active->frame.reset();
}

Result<CoroutineState> Coroutine::resume() noexcept {
    detail::assert_serial_access();
    if (!control_)
        return std::unexpected(Error{ErrorCode::invalid_object, "Coroutine is empty."});
    // A callback can destroy/move the public owner. Only this pin is used below.
    const Control::Pin pin{control_};
    auto* const active = pin.ptr;
    if (active->state == CoroutineState::running)
        return std::unexpected(Error{ErrorCode::busy, "Coroutine is already running."});
    if (active->state != CoroutineState::ready && active->state != CoroutineState::suspended)
        return std::unexpected(Error{ErrorCode::invalid_object, "Coroutine has already terminated."});
    ATELIER_TRY_BIND(scope, ExecutionScope::enter(active->runtime));
    active->state = CoroutineState::running;
    auto step = active->frame->resume();
    if (step && (step->state != CoroutineState::suspended && step->state != CoroutineState::completed))
        step = std::unexpected(Error{ErrorCode::invocation_failed, "Coroutine body returned an invalid state."});
    if (step && step->state == CoroutineState::suspended && step->result)
        step = std::unexpected(Error{ErrorCode::invocation_failed, "A suspended coroutine cannot publish a result."});
    if (!step) {
        active->state = CoroutineState::failed;
        active->error = std::move(step.error());
        active->frame->on_terminal(active->state, &*active->error);
        active->frame.reset();
        return std::unexpected(*active->error);
    }
    active->cancel_requested |= active->frame->cancel_requested_;
    if (active->cancel_requested) {
        active->state = CoroutineState::cancelled;
        active->frame->on_terminal(active->state, nullptr);
        active->frame.reset();
    } else {
        active->state = step->state;
        if (active->state == CoroutineState::completed) {
            active->result = std::move(step->result);
            active->frame->on_terminal(active->state, nullptr);
            active->frame.reset();
        }
    }
    return active->state;
}

Result<std::optional<Scalar>> Coroutine::result() const {
    if (control_ && control_->error)
        return std::unexpected(*control_->error);
    if (!control_ || control_->state != CoroutineState::completed)
        return std::unexpected(Error{ErrorCode::invalid_object, "Coroutine has no completed result."});
    return control_->result;
}

}

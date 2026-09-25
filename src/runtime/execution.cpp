#include "runtime/execution.hpp"

#include <utility>

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

}

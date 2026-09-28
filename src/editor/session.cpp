#include "editor/session.hpp"
#include <algorithm>

namespace atelier::editor {
namespace {
auto invalid(const char* message) { return std::unexpected(Error{ErrorCode::invalid_definition, message}); }
}
Session::Session() { history_.reserve(history_limit + 1); }
void Session::remember(Change change) {
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    history_.push_back(std::move(change));
    if (history_.size() > history_limit) history_.erase(history_.begin());
    cursor_ = history_.size();
}
Result<void> Session::select(scene::ObjectKey key) {
    if (key && !scene_.find(key)) return invalid("Selection is not a scene member.");
    auto cancelled = cancel_position();
    if (!cancelled) return cancelled;
    selected_ = key;
    return {};
}
Result<scene::ObjectKey> Session::add(scene::Vec3 position) {
    auto cancelled = cancel_position();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto member = scene_.create(position);
    if (!member) return std::unexpected(member.error());
    Change change{Kind::add, std::move(*member), scene_.members().size()};
    auto inserted = scene_.insert(change.member, change.index);
    if (!inserted) return std::unexpected(inserted.error());
    selected_ = change.member.key;
    remember(std::move(change));
    return selected_;
}
Result<void> Session::erase_selected() {
    auto cancelled = cancel_position();
    if (!cancelled) return cancelled;
    const auto* member = scene_.find(selected_);
    if (!member) return invalid("Select an object to delete.");
    const auto members = scene_.members();
    const auto index = static_cast<std::size_t>(member - members.data());
    Change change{Kind::remove, *member, index}; // root first, then remove scene root
    auto removed = scene_.remove(selected_);
    if (!removed) return removed;
    selected_ = 0;
    remember(std::move(change));
    return {};
}
Result<void> Session::begin_position() {
    if (active_) return invalid("A position edit is already active.");
    const auto* member = scene_.find(selected_);
    if (!member) return invalid("Select an object to edit.");
    const auto before = scene_.position(selected_);
    if (!before) return std::unexpected(before.error());
    active_.emplace(Change{Kind::position, *member, 0, *before, *before});
    return {};
}
Result<void> Session::preview_position(std::size_t axis, float value) {
    if (!active_ || axis >= position_parameters.size()) return invalid("No matching position edit.");
    auto next = scene_.position(active_->member.key);
    if (!next) return std::unexpected(next.error());
    if (axis == 0) next->x = value;
    else if (axis == 1) next->y = value;
    else next->z = value;
    return scene_.position(active_->member.key, *next);
}
Result<void> Session::commit_position() {
    if (!active_) return {};
    const auto after = scene_.position(active_->member.key);
    if (!after) return std::unexpected(after.error());
    active_->after = *after;
    if (active_->before != active_->after) remember(std::move(*active_));
    active_.reset();
    return {};
}
Result<void> Session::cancel_position() {
    if (!active_) return {};
    auto restored = scene_.position(active_->member.key, active_->before);
    if (!restored) return restored;
    active_.reset();
    return {};
}
Result<void> Session::replay(const Change& change, bool forward) {
    if (change.kind == Kind::position) {
        auto written = scene_.position(change.member.key, forward ? change.after : change.before);
        if (written) selected_ = change.member.key;
        return written;
    }
    const bool insert = (change.kind == Kind::add) == forward;
    auto result = insert ? scene_.insert(change.member, change.index) : scene_.remove(change.member.key);
    if (!result) return result;
    if (insert) selected_ = change.member.key;
    else if (selected_ == change.member.key) selected_ = 0;
    return {};
}
Result<void> Session::undo() {
    auto cancelled = cancel_position();
    if (!cancelled) return cancelled;
    if (!cursor_) return {};
    auto result = replay(history_[cursor_ - 1], false);
    if (result) --cursor_;
    return result;
}
Result<void> Session::redo() {
    auto cancelled = cancel_position();
    if (!cancelled) return cancelled;
    if (cursor_ == history_.size()) return {};
    auto result = replay(history_[cursor_], true);
    if (result) ++cursor_;
    return result;
}
}

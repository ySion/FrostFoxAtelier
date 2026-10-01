#include "editor/session.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace atelier::editor {
namespace {
auto invalid(const char* message) { return std::unexpected(Error{ErrorCode::invalid_definition, message}); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
}
Session::Session() : types_(take(world::register_types(runtime_))), world_(runtime_, types_),
    parameters_{Parameter{"X", "m", types_.position_fields[0]}, Parameter{"Y", "m", types_.position_fields[1]},
                Parameter{"Z", "m", types_.position_fields[2]}} {
    static_assert(std::is_nothrow_move_constructible_v<Change> && std::is_nothrow_move_assignable_v<Change>);
    history_.reserve(history_limit + 1);
}
void Session::remember(Change change) {
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    history_.push_back(std::move(change));
    if (history_.size() > history_limit) history_.erase(history_.begin());
    cursor_ = history_.size();
}
std::wstring_view Session::name(ObjectId actor) const noexcept {
    const auto at=names_.find(actor);
    return at==names_.end()?std::wstring_view{}:at->second;
}
Result<void> Session::rename(ObjectId actor, std::wstring name) {
    if (name.empty() || name.size()>256 || std::all_of(name.begin(),name.end(),[](wchar_t c){return c==L' ';}) ||
        std::any_of(name.begin(),name.end(),[](wchar_t c){return c<32 || c==127;}))
        return invalid("Use a name with 1–256 visible characters.");
    auto state=world::inspect_actor(runtime_,actor);
    if (!state) return std::unexpected(state.error());
    if (!state->can_edit) return invalid("The Actor is not editable.");
    const auto at=names_.find(actor);
    if (at==names_.end()) return invalid("The Actor does not belong to this document.");
    if (at->second==name) return {};
    auto root=runtime_.hold(actor);
    if (!root) return std::unexpected(root.error());
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    NameChange change{std::move(*root),at->second,name};
    at->second=std::move(name); remember(std::move(change));
    return {};
}
Result<void> Session::select(ObjectId actor) {
    if (actor != ObjectId{}) {
        auto members = world_.members();
        if (!members) return std::unexpected(members.error());
        if (std::none_of(members->begin(), members->end(), [actor](const auto& member) { return member.id() == actor; }))
            return invalid("Selection is not a World member.");
    }
    auto cancelled = cancel_edit();
    if (!cancelled) return cancelled;
    selected_ = actor;
    return {};
}
Result<world::Commit> Session::add_actor() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto members = world_.members();
    if (!members) return std::unexpected(members.error());
    if (members->size() >= maximum_actors) return invalid("Editor Actor limit reached.");
    auto root = world::create_actor(runtime_, types_.actor);
    if (!root) return std::unexpected(root.error());
    ActorChange change;
    change.added = true;
    change.removal.root = std::move(*root);
    names_.emplace(change.removal.id(),L"Actor "+std::to_wstring(next_name_++));
    auto attached = world_.attach(change.removal.id());
    if (!attached) return attached;
    if (attached->changed) {
        selected_ = change.removal.id();
        remember(std::move(change));
    }
    return attached;
}
Result<world::Commit> Session::erase_selected() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto removed = world_.detach(selected_);
    if (!removed) return std::unexpected(removed.error());
    auto result = std::move(removed->commit);
    if (result.changed) {
        remember(ActorChange{false, std::move(*removed)});
        selected_ = {};
    }
    return result;
}
Result<world::Commit> Session::add_component(TypeId type) {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (type != types_.transform && type != types_.cube) return invalid("This editor does not expose that component type.");
    auto root = world::create_component(runtime_, type);
    if (!root) return std::unexpected(root.error());
    ObjectId transform;
    if (type == types_.cube) {
        auto target = world::root_transform(runtime_, selected_);
        if (!target) return std::unexpected(target.error());
        if (!*target) return invalid("Add a Transform before adding a Cube.");
        transform = **target;
    }
    ComponentChange change;
    change.added = true;
    change.removal.root = std::move(*root);
    change.removal.owner = selected_;
    const std::array attachments{world::Attachment{change.removal.id(), transform}};
    auto attached = world_.attach_components(selected_, attachments);
    if (!attached) return attached;
    if (attached->changed) remember(std::move(change));
    return attached;
}
Result<world::Commit> Session::remove_component(ObjectId component) {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto removed = world_.remove_component(component);
    if (!removed) return std::unexpected(removed.error());
    auto result = std::move(removed->commit);
    if (result.changed) remember(ComponentChange{false, std::move(*removed)});
    return result;
}
Result<void> Session::begin_edit(ObjectId component, std::span<const FieldId> path, std::uint64_t control_session) {
    if (active_) return invalid("A field edit is already active.");
    if (!control_session) return invalid("A field edit requires a control session.");
    const auto includes = [&](const auto& paths) {
        return std::any_of(paths.begin(), paths.end(), [&](const auto& value) {
            return std::equal(value.begin(), value.end(), path.begin(), path.end());
        });
    };
    if (!includes(types_.position_fields) && !includes(types_.rotation_fields) && !includes(types_.scale_fields))
        return invalid("The field is not supported by this editor.");
    auto before = world_.read(component, path);
    if (!before) return std::unexpected(before.error());
    const auto* number = std::get_if<float>(&*before);
    if (!number) return invalid("The field is not a float.");
    auto transform = world::read_transform(runtime_, component);
    if (!transform) return std::unexpected(transform.error());
    auto state = world::inspect_component(runtime_, component);
    if (!state) return std::unexpected(state.error());
    auto root = runtime_.hold(component);
    if (!root) return std::unexpected(root.error());
    active_.emplace(FieldChange{std::move(*root), state->owner, {path.begin(), path.end()}, control_session, *transform, *transform});
    return {};
}
bool Session::owns_edit(std::uint64_t control_session) const noexcept {
    return active_ && active_->control_session == control_session;
}
Result<void> Session::preview_edit(std::uint64_t control_session, float value) {
    if (!owns_edit(control_session)) return invalid("No matching field edit.");
    const auto parameter = std::find_if(parameters_.begin(), parameters_.end(), [&](const auto& item) {
        return std::equal(item.path.begin(), item.path.end(), active_->path.begin(), active_->path.end());
    });
    if (!std::isfinite(value) || (parameter != parameters_.end() && (value < parameter->minimum || value > parameter->maximum)))
        return invalid("Field value is outside the editor range.");
    return world_.write(active_->root.id(), active_->path, Scalar{value});
}
Result<void> Session::commit_edit(std::uint64_t control_session) {
    if (!owns_edit(control_session)) return invalid("No matching field edit.");
    auto value = world_.read(active_->root.id(), active_->path);
    if (!value) return std::unexpected(value.error());
    auto transform = world::read_transform(runtime_, active_->root.id());
    if (!transform) return std::unexpected(transform.error());
    active_->after = *transform;
    if (active_->before != active_->after) remember(std::move(*active_));
    active_.reset();
    return {};
}
Result<void> Session::cancel_edit() {
    if (!active_) return {};
    auto restored = world_.set_transform(active_->root.id(), active_->before);
    if (!restored) return restored;
    active_.reset();
    return {};
}
Result<void> Session::reset_position(ObjectId component) {
    auto cancelled=cancel_edit();
    if (!cancelled) return cancelled;
    auto before=world::read_transform(runtime_,component);
    if (!before) return std::unexpected(before.error());
    auto state=world::inspect_component(runtime_,component);
    if (!state) return std::unexpected(state.error());
    auto root=runtime_.hold(component);
    if (!root) return std::unexpected(root.error());
    auto after=*before;
    after.position={parameters_[0].default_value,parameters_[1].default_value,parameters_[2].default_value};
    if (after==*before) return {};
    FieldChange change{std::move(*root),state->owner,{},0,*before,after};
    auto applied=world_.set_transform(component,after);
    if (!applied) return applied;
    remember(std::move(change));
    return {};
}
Result<world::Commit> Session::replay(Change& change, bool forward) {
    return std::visit([&](auto& item) -> Result<world::Commit> {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, NameChange>) {
            auto state=world::inspect_actor(runtime_,item.root.id());
            if (!state) return std::unexpected(state.error());
            if (!state->can_edit) return invalid("The Actor is not editable.");
            names_.at(item.root.id())=forward?item.after:item.before;
            selected_=item.root.id();
            return world::Commit{true,{}};
        } else if constexpr (std::is_same_v<T, FieldChange>) {
            auto state = world::inspect_component(runtime_, item.root.id());
            if (!state) return std::unexpected(state.error());
            if (!state->valid || state->owner != item.actor) return invalid("The field's owning Actor changed.");
            auto written = world_.set_transform(item.root.id(), forward ? item.after : item.before);
            if (!written) return std::unexpected(written.error());
            selected_ = item.actor; // Retain the existing document-history selection policy.
            return world::Commit{true, {}};
        } else {
            const bool attach = item.added == forward;
            if (attach) {
                auto restored = world_.restore(item.removal);
                if (!restored) return restored;
                if constexpr (std::is_same_v<T, ActorChange>) if (restored->changed) selected_ = item.removal.id();
                return restored;
            }
            if constexpr (std::is_same_v<T, ActorChange>) {
                auto removed = world_.detach(item.removal.id());
                if (!removed) return std::unexpected(removed.error());
                item.removal = std::move(*removed);
                if (item.removal.commit.changed && selected_ == item.removal.id()) selected_ = {};
            } else {
                auto removed = world_.remove_component(item.removal.id());
                if (!removed) return std::unexpected(removed.error());
                item.removal = std::move(*removed);
            }
            auto result = std::move(item.removal.commit);
            item.removal.commit.notification_error.reset();
            return result;
        }
    }, change);
}
Result<world::Commit> Session::undo() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (!cursor_) return world::Commit{};
    auto result = replay(history_[cursor_ - 1], false);
    if (result) --cursor_;
    return result;
}
Result<world::Commit> Session::redo() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    if (cursor_ == history_.size()) return world::Commit{};
    auto result = replay(history_[cursor_], true);
    if (result) ++cursor_;
    return result;
}
Result<world::Commit> Session::close() {
    auto cancelled = cancel_edit();
    if (!cancelled) return std::unexpected(cancelled.error());
    auto closed = world_.close();
    if (!closed) return closed;
    history_.clear(); names_.clear(); cursor_ = 0; selected_ = {};
    return closed;
}
Result<void> Session::collect_step() {
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        auto started = runtime_.begin_collection();
        if (!started) return started;
    }
    auto progress = runtime_.collect_step({32});
    if (!progress) return std::unexpected(progress.error());
    if (runtime_.collection_phase()==CollectionPhase::idle)
        std::erase_if(names_,[&](const auto& item){return !runtime_.type_of(item.first);});
    return {};
}
}

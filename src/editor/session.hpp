#pragma once

#include "world/world.hpp"

namespace atelier::editor {
struct Parameter {
    const char* name;
    const char* unit;
    std::array<FieldId, 2> path;
    float minimum{-10000.f}, maximum{10000.f}, drag_step{.01f};
};

// One document writer. History retains ordinary roots and removal records;
// evicting history never explicitly destroys a game object.
class Session final {
public:
    Session();
    const world::World& world() const noexcept { return world_; }
    const world::Types& types() const noexcept { return types_; }
    std::span<const Parameter> parameters() const noexcept { return parameters_; }
    ObjectId selected() const noexcept { return selected_; }
    Result<void> select(ObjectId);
    Result<world::Commit> add_actor();
    Result<world::Commit> erase_selected();
    Result<world::Commit> add_component(TypeId);
    Result<world::Commit> remove_component(ObjectId);
    Result<void> begin_edit(ObjectId component, std::span<const FieldId>, std::uint64_t control_session);
    Result<void> preview_edit(std::uint64_t control_session, float);
    Result<void> commit_edit(std::uint64_t control_session);
    Result<void> cancel_edit();
    bool owns_edit(std::uint64_t control_session) const noexcept;
    Result<world::Commit> undo();
    Result<world::Commit> redo();
    Result<world::Commit> close();
    Result<void> collect_step();
    bool editing() const noexcept { return active_.has_value(); }
    std::size_t undo_count() const noexcept { return cursor_; }
    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }
    static constexpr std::size_t maximum_actors = 256;
private:
    struct ActorChange { bool added{}; world::ActorRemoval removal; };
    struct ComponentChange { bool added{}; world::ComponentRemoval removal; };
    struct FieldChange {
        ObjectRoot root;
        ObjectId actor;
        world::FieldPath path;
        std::uint64_t control_session{};
        world::Trs before, after; // Quaternion field writes may normalize other components.
    };
    using Change = std::variant<ActorChange, ComponentChange, FieldChange>;
    static constexpr std::size_t history_limit = 128;
    Runtime runtime_;
    world::Types types_;
    world::World world_;
    std::array<Parameter, 3> parameters_;
    std::vector<Change> history_;
    std::size_t cursor_{};
    ObjectId selected_;
    std::optional<FieldChange> active_;
    void remember(Change);
    Result<world::Commit> replay(Change&, bool forward);
};
}

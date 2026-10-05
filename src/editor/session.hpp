#pragma once

#include "world/world.hpp"
#include "editor/assets.hpp"
#include "physics/physics.hpp"
#include <unordered_map>

namespace atelier::editor {
class Project;
struct Parameter {
    const char* name;
    const char* unit;
    std::array<FieldId, 2> path;
    float minimum{-10000.f}, maximum{10000.f}, drag_step{.01f};
    float default_value{};
};

// One document writer. History retains ordinary roots and removal records;
// evicting history never explicitly destroys a game object.
class Session final {
public:
    Session();
    const world::World& world() const noexcept { return world_; }
    const world::Types& types() const noexcept { return types_; }
    const physics::Types& physics_types() const noexcept { return physics_types_; }
    std::span<const Parameter> parameters() const noexcept { return parameters_; }
    ObjectId selected() const noexcept { return selected_; }
    std::wstring_view name(ObjectId) const noexcept;
    Result<void> rename(ObjectId, std::wstring); // Document label; shares scene history and object identity.
    Result<void> select(ObjectId);
    Result<world::Commit> add_actor();
    Result<world::Commit> create_static_mesh(const MeshAsset&);
    Result<world::Commit> erase_selected();
    Result<world::Commit> add_component(TypeId);
    Result<world::Commit> remove_component(ObjectId);
    Result<void> configure_body(ObjectId component, physics::BodySettings);
    Result<void> begin_edit(ObjectId component, std::span<const FieldId>, std::uint64_t control_session);
    Result<void> preview_edit(std::uint64_t control_session, float);
    Result<void> commit_edit(std::uint64_t control_session);
    Result<void> cancel_edit();
    Result<void> reset_position(ObjectId component); // One group change and one history entry.
    bool owns_edit(std::uint64_t control_session) const noexcept;
    Result<world::Commit> undo();
    Result<world::Commit> redo();
    Result<world::Commit> close();
    Result<void> collect_step();
    Result<std::string> serialize() const;
    static Result<std::unique_ptr<Session>> deserialize(std::string_view,Project&);
    bool dirty() const noexcept { return !saved_cursor_ || *saved_cursor_!=cursor_ || active_.has_value(); }
    void mark_saved() noexcept { saved_cursor_=cursor_; }
    bool editing() const noexcept { return active_.has_value(); }
    std::size_t undo_count() const noexcept { return cursor_; }
    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }
    static constexpr std::size_t maximum_actors = 256;
    static constexpr std::size_t maximum_components = 1024;
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
    struct NameChange { ObjectRoot root; std::wstring before, after; };
    struct BodyChange { ObjectRoot root; ObjectId actor; physics::BodySettings before, after; };
    using Change = std::variant<ActorChange, ComponentChange, FieldChange, NameChange, BodyChange>;
    static constexpr std::size_t history_limit = 128;
    Runtime runtime_;
    world::Types types_;
    physics::Types physics_types_;
    world::World world_;
    std::array<Parameter, 3> parameters_;
    std::vector<Change> history_;
    std::size_t cursor_{};
    std::optional<std::size_t> saved_cursor_{0};
    ObjectId selected_;
    std::optional<FieldChange> active_;
    std::unordered_map<ObjectId,std::wstring> names_; // Non-owning IDs; prune after collection.
    std::uint64_t next_name_{1};
    Result<void> admit_actor_edit(ObjectId) const;
    Result<world::ComponentStatus> admit_component_edit(ObjectId) const;
    Result<world::Commit> attach_created(ObjectRoot,std::wstring_view);
    void remember(Change);
    static Result<void> validate_name(std::wstring_view);
    Result<world::Commit> replay(Change&, bool forward);
};
}

#pragma once

#include "world/world.hpp"
#include "editor/assets.hpp"
#include "physics/physics.hpp"
#include <unordered_map>

namespace atelier::editor {
class Project;
struct MaterialAsset;
struct Parameter {
    const char* name;
    const char* unit;
    std::array<FieldId, 2> path;
    float minimum{-10000.f}, maximum{10000.f}, drag_step{.01f};
    float default_value{};
};
struct ComponentType { TypeId type; std::string name; };
struct Property { std::string name; world::FieldPath path; TypeId type; Scalar value; };
struct Selection {
    ObjectId actor, object; // Actor context and inspected Actor/Component, in this document only.
    bool operator==(const Selection&) const = default;
};

// One document writer. History retains ordinary roots and removal records;
// evicting history never explicitly destroys a game object.
class Session final {
public:
    // Runs after engine registration, before any object exists. The returned
    // code owner is released after World End, always before its Runtime.
    using Prepare = std::function<Result<std::shared_ptr<void>>(world::World&)>;
    explicit Session(Prepare = {});
    ~Session();
    const world::World& world() const noexcept { return world_; }
    const world::Types& types() const noexcept { return types_; }
    const physics::Types& physics_types() const noexcept { return physics_types_; }
    // Borrowed running scene; invalidated by stop, close, or a failed update.
    physics::PhysicsScene* physics_scene() noexcept { return physics_.get(); }
    std::span<const Parameter> parameters() const noexcept { return parameters_; }
    Selection selection() const noexcept { return selection_; }
    // Actor-level multi-selection; the active Actor (selection().actor) is the
    // last entry when it is one of them. Selection is view state, not history.
    std::span<const ObjectId> selected_actors() const noexcept { return selected_actors_; }
    bool is_selected(ObjectId actor) const noexcept { return std::ranges::find(selected_actors_,actor)!=selected_actors_.end(); }
    std::wstring_view name(ObjectId) const noexcept;
    Result<void> rename(ObjectId, std::wstring); // Document label; shares scene history and object identity.
    Result<void> select(ObjectId);
    Result<void> select_actors(std::span<const ObjectId> actors, ObjectId active);
    Result<world::Commit> add_actor(TypeId actual = {});
    Result<world::Commit> create_static_mesh(const MeshAsset&, math::Vector3 position = {});
    Result<world::Commit> create_light(world::LightKind, math::Vector3 position = {});
    Result<world::Commit> erase_selected(); // All selected Actors form one history entry.
    Result<world::Commit> erase_actors(); // Viewport actions target Actors even while inspecting a Component.
    Result<world::Commit> duplicate_selected(); // Fresh Actor/Component identities; immutable resources stay shared.
    Result<world::Commit> add_component(TypeId);
    Result<world::Commit> remove_component(ObjectId);
    Result<std::vector<ComponentType>> component_types() const;
    Result<std::vector<Property>> properties(ObjectId) const;
    Result<Scalar> read_property(ObjectId, std::span<const FieldId>) const;
    Result<void> configure_body(ObjectId component, physics::BodySettings);
    Result<void> begin_edit(ObjectId component, std::span<const FieldId>, std::uint64_t control_session);
    // Whole-TRS edit of several SceneComponents (viewport handles, modal G/R/S,
    // Euler fields). Preview writes complete transforms in the same order;
    // commit_edit records one history entry, cancel_edit restores all.
    Result<void> begin_transform(std::span<const ObjectId> scene_components, std::uint64_t control_session, bool amend_creation = false);
    static constexpr float minimum_scale=.0001f, maximum_scale=10000.f;
    // Stable gesture origin; previews never feed their converted values back into the next preview.
    Result<world::Trs> original_transform(std::uint64_t control_session, ObjectId component) const;
    Result<void> preview_transform(std::uint64_t control_session, std::span<const world::Trs>);
    Result<void> set_property(ObjectId, std::span<const FieldId>, Scalar, std::uint64_t control_session);
    Result<void> preview_edit(std::uint64_t control_session, float);
    Result<void> preview_edit(std::uint64_t control_session, Scalar);
    Result<void> commit_edit(std::uint64_t control_session);
    Result<void> cancel_edit();
    Result<void> reset_position(ObjectId component); // One group change and one history entry.
    Result<ObjectRoot> material_resource(Project&,std::string_view key);
    Result<ObjectRoot> material_resource(Project&,const MaterialAsset&);
    Result<MaterialAsset> material_asset(ObjectId) const;
    Result<std::vector<MaterialAsset>> material_assets() const;
    std::vector<ObjectId> material_objects() const;
    Result<void> begin_material_edit(ObjectId,std::uint64_t control_session);
    Result<void> preview_material(std::uint64_t,const material::State&);
    Result<void> commit_material_edit(std::uint64_t);
    Result<void> cancel_material_edit();
    Result<void> set_material_texture(Project&,ObjectId,material::Input,std::string_view key);
    Result<void> assign_material(Project&,ObjectId component,std::string_view slot,std::string_view key);
    Result<void> clear_material(ObjectId component,std::string_view slot);
    Result<void> set_material_override(ObjectId component,std::string_view slot,std::span<const FieldId>,bool);
    Result<void> save_material(Project&,std::string_view key);
    Result<void> material_undo(std::string_view key,bool redo=false);
    bool material_dirty(std::string_view key) const;
    std::vector<std::string> dirty_materials() const;
    bool material_editing(std::uint64_t control_session) const noexcept;
    ObjectId loaded_material(std::string_view key) const;
    bool is_material(ObjectId) const;
    bool material_can_undo(std::string_view key,bool redo=false) const;
    bool owns_edit(std::uint64_t control_session) const noexcept;
    Result<world::Commit> undo();
    Result<world::Commit> redo();
    Result<world::Commit> close();
    Result<world::Commit> start();
    Result<void> update(float);
    Result<world::Commit> stop();
    Result<void> collect_step();
    Result<std::string> serialize() const;
    static Result<std::unique_ptr<Session>> deserialize(std::string_view,Project&,Prepare = {});
    bool dirty() const noexcept { return !saved_cursor_ || *saved_cursor_!=cursor_ || active_ || transform_ || (material_edit_ && material_edit_->component!=ObjectId{}); }
    void mark_saved() noexcept { if(saved_cursor_!=cursor_) {saved_cursor_=cursor_;++revision_;} }
    void mark_modified() noexcept { if(saved_cursor_) {saved_cursor_.reset();++revision_;} }
    // Monotonic authoring observation token, independent of undo cursor reuse.
    std::uint64_t revision() const noexcept { return revision_; }
    bool editing() const noexcept { return active_.has_value() || transform_.has_value() || material_edit_.has_value(); }
    std::size_t undo_count() const noexcept { return cursor_; }
    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }
    static constexpr std::size_t maximum_actors = 8192;
    static constexpr std::size_t maximum_components = 1024;
private:
    // Removals are ordered as detached; restore runs in reverse.
    struct ActorChange { bool added{}; std::vector<world::ActorRemoval> removals; };
    struct ComponentChange { bool added{}; world::ComponentRemoval removal; };
    struct FieldChange {
        ObjectRoot root;
        ObjectId actor;
        world::FieldPath path;
        std::uint64_t control_session{};
        // TRS is one invariant group; ordinary business properties keep exact scalar types.
        std::variant<world::Trs,Scalar> before, after;
    };
    struct TransformChange {
        struct Entry { ObjectRoot root; ObjectId actor; world::Trs before, after; };
        std::vector<Entry> entries;
        std::uint64_t control_session{};
        Selection inspection;
        bool amend_creation{}; // Initial placement belongs to the immediately preceding creation entry.
    };
    struct NameChange { ObjectRoot root; std::wstring before, after; };
    struct BodyChange { ObjectRoot root; ObjectId actor; physics::BodySettings before, after; };
    struct MaterialBindingChange {ObjectRoot root;ObjectId actor;std::vector<world::MaterialBinding> before,after;};
    struct MaterialValueChange {ObjectRoot root;ObjectId actor,component;material::State before,after;};
    using Change = std::variant<ActorChange, ComponentChange, FieldChange, NameChange, BodyChange, TransformChange, MaterialBindingChange, MaterialValueChange>;
    static constexpr std::size_t history_limit = 128;
    Runtime runtime_;
    world::Types types_;
    physics::Types physics_types_;
    std::shared_ptr<void> prepared_;
    world::World world_;
    std::unique_ptr<physics::PhysicsScene> physics_;
    std::array<Parameter, 3> parameters_;
    std::vector<Change> history_;
    std::size_t cursor_{};
    std::uint64_t revision_{1};
    std::optional<std::size_t> saved_cursor_{0};
    Selection selection_;
    std::vector<ObjectId> selected_actors_; // Non-owning; validated on write, pruned with membership.
    std::optional<FieldChange> active_;
    std::optional<TransformChange> transform_;
    std::unordered_map<ObjectId,std::wstring> names_; // Non-owning IDs; prune after collection.
    std::uint64_t next_name_{1};
    struct MaterialDocument {ObjectRoot root;material::State saved;std::vector<material::State> history;std::size_t cursor{};};
    std::unordered_map<std::string,MaterialDocument> materials_;
    std::unordered_map<std::string,ObjectRoot> textures_;
    struct MaterialEdit {ObjectRoot root;material::State before;std::uint64_t control{};world::FieldPath path;ObjectId actor,component;};
    std::optional<MaterialEdit> material_edit_;
    Result<ObjectRoot> texture_resource(Project&,std::string_view);
    Result<void> replace_material_bindings(ObjectId,std::vector<world::MaterialBinding>);
    Result<void> admit_actor_edit(ObjectId) const;
    Result<world::ComponentStatus> admit_component_edit(ObjectId) const;
    Result<ObjectId> admit_property_edit(ObjectId) const;
    Result<void> apply_field(const FieldChange&, bool forward);
    Result<world::Commit> attach_created(ObjectRoot,std::wstring_view);
    void set_selection(Selection); // Single-object selection; resets the Actor set.
    Result<void> write_transforms(const TransformChange&, std::span<const world::Trs>);
    void forget_actor(ObjectId) noexcept;
    void remember(Change);
    static Result<void> validate_name(std::wstring_view);
    Result<world::Commit> replay(Change&, bool forward);
};
}

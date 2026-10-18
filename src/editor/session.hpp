#pragma once

#include "world/world.hpp"
#include "world/diagnostic.hpp"
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
struct Property { std::string name; world::FieldPath path; TypeId type; Scalar value; std::string label,category,description; };
struct Selection {
    ObjectId entity, object; // Entity context and inspected Entity/Component, in this document only.
    bool operator==(const Selection&) const = default;
};
// Tolerant loading keeps data it cannot apply and reports it; strict stops at the first warning.
enum class LoadPolicy { tolerant, strict };
// Saved scene data the loader could not apply, addressed by its scene record path
// (for example properties.speed, space or name). It is written back on save.
struct RetainedEntry { std::vector<std::string> path; std::string value; world::DiagnosticKind kind{}; };
struct RetainedView {
    std::string type; // Original type of a placeholder object; empty when the type resolved.
    std::vector<RetainedEntry> entries;
};
// A project resource a tolerant load could not provide. A stand-in keeps its key,
// so saving writes the reference unchanged. input names a material texture input.
struct MissingResource { std::string key, reason; std::optional<std::size_t> input; };

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
    // Entity-level multi-selection; the active Entity (selection().entity) is the
    // last entry when it is one of them. Selection is view state, not history.
    std::span<const ObjectId> selected_entities() const noexcept { return selected_entities_; }
    bool is_selected(ObjectId entity) const noexcept { return std::ranges::find(selected_entities_,entity)!=selected_entities_.end(); }
    std::wstring_view name(ObjectId) const noexcept;
    Result<void> set_world_lighting(const rendering::SceneLightingSettings&);
    Result<void> rename(ObjectId, std::wstring); // Document label; shares scene history and object identity.
    Result<void> select(ObjectId);
    Result<void> select_entities(std::span<const ObjectId> entities, ObjectId active);
    Result<world::Commit> add_entity(TypeId actual = {});
    Result<world::Commit> create_static_mesh(const MeshAsset&, math::Vector3 position = {});
    Result<world::Commit> create_light(world::LightKind, math::Vector3 position = {});
    Result<world::Commit> erase_selected(); // All selected Entities form one history entry.
    Result<world::Commit> erase_entities(); // Viewport actions target Entities even while inspecting a Component.
    Result<world::Commit> duplicate_selected(); // Fresh Entity/Component identities; immutable resources stay shared.
    Result<world::Commit> reparent(ObjectId child,ObjectId parent,
        world::ReparentRule = world::ReparentRule::keep_world,std::size_t index = world::append);
    Result<void> set_transform(ObjectId,world::Trs);
    Result<void> remove_transform(ObjectId);
    Result<world::Commit> set_space_reference(ObjectId,std::optional<ObjectId>,
        world::ReparentRule = world::ReparentRule::keep_world);
    Result<world::Commit> add_component(TypeId);
    Result<world::Commit> remove_component(ObjectId);
    Result<std::vector<ComponentType>> component_types() const;
    Result<std::vector<Property>> properties(ObjectId) const;
    Result<Scalar> read_property(ObjectId, std::span<const FieldId>) const;
    Result<void> configure_body(ObjectId component, physics::BodySettings);
    Result<void> begin_edit(ObjectId component, std::span<const FieldId>, std::uint64_t control_session);
    // Whole-TRS edit of several Entities (viewport handles, modal G/R/S,
    // Euler fields). Preview writes complete transforms in the same order;
    // commit_edit records one history entry, cancel_edit restores all.
    Result<void> begin_transform(std::span<const ObjectId> entities, std::uint64_t control_session, bool amend_creation = false);
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
    // Several scalar lanes of one object (a colour's R/G/B) as one value: preview writes
    // without history; commit records one entry against the values seen at begin.
    Result<void> begin_group_edit(ObjectId, std::span<const world::FieldPath>, std::uint64_t control_session);
    Result<void> preview_group_edit(std::uint64_t control_session, std::span<const Scalar>);
    // With a missing list, materials and textures absent from the project catalog become
    // stand-ins and are reported there; listed but unreadable assets and calls without a
    // list fail. Stand-ins are never edited or saved as assets.
    Result<ObjectRoot> material_resource(Project&,std::string_view key,std::vector<MissingResource>* missing = nullptr);
    Result<ObjectRoot> material_resource(Project&,const MaterialAsset&,std::vector<MissingResource>* missing = nullptr);
    bool resource_missing(ObjectId material_or_texture) const;
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
    Result<std::string> serialize_subtree(ObjectId) const;
    // Imports a complete detached graph, remaps internal references and records
    // one creation history item. Document identities are always fresh.
    Result<world::Commit> instantiate_subtree(std::string_view,Project&,ObjectId parent = {});
    static Result<std::unique_ptr<Session>> deserialize(std::string_view,Project&,Prepare = {},LoadPolicy = LoadPolicy::tolerant);
    // Diagnostics recorded by the load that created this document.
    std::span<const world::Diagnostic> load_issues() const noexcept { return load_issues_; }
    // Current unresolved data of the document; entries disappear when edited or discarded.
    std::vector<world::Diagnostic> issues() const;
    // ObjectId{} addresses document-level data (top-level keys and World settings).
    bool has_retained(ObjectId) const noexcept;
    Result<RetainedView> retained(ObjectId) const;
    // Drops the retained data of one object (or the document) as one undoable entry.
    // A placeholder keeps its original type.
    Result<void> discard_retained(ObjectId);
    std::size_t placeholder_count() const noexcept;
    std::optional<std::uint64_t> document_identity(ObjectId) const;
    // Legacy control bundles refer to pre-migration Actor/Component indices.
    // Resolve them through the scene importer, then select live document IDs.
    static Result<std::vector<std::pair<std::uint64_t,std::uint64_t>>> legacy_selection(std::string_view,
        std::span<const std::pair<std::size_t,std::optional<std::size_t>>>);
    Result<ObjectId> local_object(std::uint64_t) const;
    bool dirty() const noexcept { return !saved_cursor_ || *saved_cursor_!=cursor_ || active_ || group_ || transform_ || (material_edit_ && material_edit_->component!=ObjectId{}); }
    void mark_saved() noexcept { if(saved_cursor_!=cursor_) {saved_cursor_=cursor_;++revision_;} }
    void mark_modified() noexcept { if(saved_cursor_) {saved_cursor_.reset();++revision_;} }
    // Monotonic authoring observation token, independent of undo cursor reuse.
    std::uint64_t revision() const noexcept { return revision_; }
    bool editing() const noexcept { return active_.has_value() || transform_.has_value() || material_edit_.has_value() || group_.has_value(); }
    std::size_t undo_count() const noexcept { return cursor_; }
    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }
    static constexpr std::size_t maximum_entities = 8192;
    static constexpr std::size_t maximum_components = 1024;
private:
    // Removal records restore together as one forest transaction.
    struct EntityChange { bool added{}; std::vector<world::EntityRemoval> removals; };
    struct ComponentChange { bool added{}; world::ComponentRemoval removal; bool created_transform{}; };
    struct ReparentChange {
        struct Entry {ObjectRoot root;world::Trs before,after;};
        ObjectRoot root;
        ObjectId before_parent,after_parent;
        std::size_t before_index{},after_index{};
        std::vector<Entry> transforms;
    };
    struct TransformPresenceChange {
        ObjectRoot root;
        std::optional<world::Trs> before,after;
        std::optional<ObjectId> before_origin,after_origin;
    };
    struct SpaceReferenceChange {ObjectRoot root;std::optional<ObjectId> before_origin,after_origin;world::Trs before,after;bool spatial{};};
    struct FieldChange {
        ObjectRoot root;
        ObjectId entity;
        world::FieldPath path;
        std::uint64_t control_session{};
        // TRS is one invariant group; ordinary business properties keep exact scalar types.
        std::variant<world::Trs,Scalar> before, after;
    };
    struct FieldGroupChange {
        ObjectRoot root;
        ObjectId entity;
        std::vector<world::FieldPath> paths;
        std::uint64_t control_session{};
        std::vector<Scalar> before, after;
    };
    struct TransformChange {
        struct Entry { ObjectRoot root; ObjectId entity; world::Trs before, after; };
        std::vector<Entry> entries;
        std::uint64_t control_session{};
        Selection inspection;
        bool amend_creation{}; // Initial placement belongs to the immediately preceding creation entry.
    };
    struct LightingChange { rendering::SceneLightingSettings before,after; };
    struct NameChange { ObjectRoot root; std::wstring before, after; };
    struct BodyChange { ObjectRoot root; ObjectId entity; physics::BodySettings before, after; };
    struct MaterialBindingChange {ObjectRoot root;ObjectId entity;std::vector<world::MaterialBinding> before,after;};
    struct MaterialValueChange {ObjectRoot root;ObjectId entity,component;material::State before,after;};
    struct RetainedChange {}; // Explicit discard; the history entry's swaps carry the data.
    using Change = std::variant<LightingChange, EntityChange, ComponentChange, FieldChange, NameChange, BodyChange, TransformChange, MaterialBindingChange, MaterialValueChange,ReparentChange,TransformPresenceChange,SpaceReferenceChange,FieldGroupChange,RetainedChange>;
    // Sparse scene-record JSON text the loader could not apply. On the document entry,
    // materials are unreferenced definitions kept for write-back; on a component, they
    // are the scene materials its kept binding table names.
    struct Retained {
        std::string type, record, slots;
        std::vector<ObjectRoot> materials;
        bool empty() const noexcept { return type.empty() && record.empty() && slots.empty() && materials.empty(); }
    };
    struct RetainedSwap { ObjectId object; Retained before, after; };
    struct Entry { Change change; std::vector<RetainedSwap> retained; };
    static constexpr std::size_t history_limit = 128;
    Runtime runtime_;
    world::Types types_;
    physics::Types physics_types_;
    std::shared_ptr<void> prepared_;
    world::World world_;
    std::unique_ptr<physics::PhysicsScene> physics_;
    std::array<Parameter, 3> parameters_;
    std::vector<Entry> history_;
    std::size_t cursor_{};
    std::uint64_t revision_{1};
    std::optional<std::size_t> saved_cursor_{0};
    Selection selection_;
    std::vector<ObjectId> selected_entities_; // Non-owning; validated on write, pruned with membership.
    std::optional<FieldChange> active_;
    std::optional<FieldGroupChange> group_;
    std::optional<TransformChange> transform_;
    std::unordered_map<ObjectId,std::wstring> names_; // Non-owning IDs; prune after collection.
    // Non-owning IDs like names_; entries travel with their object through history.
    std::unordered_map<ObjectId,Retained> retained_;
    Retained retained_document_;
    std::vector<world::Diagnostic> load_issues_;
    // Document-local identities never confer Runtime validity or World membership.
    // Detached history objects retain their ID; IDs are never reused.
    mutable std::unordered_map<ObjectId,std::uint64_t> local_ids_;
    mutable std::uint64_t next_local_id_{1};
    struct GraphCodec;
    std::uint64_t next_name_{1};
    struct MaterialDocument {ObjectRoot root;material::State saved;std::vector<material::State> history;std::size_t cursor{};bool missing{};};
    std::unordered_map<std::string,MaterialDocument> materials_;
    std::unordered_map<std::string,ObjectRoot> textures_;
    struct MaterialEdit {ObjectRoot root;material::State before;std::uint64_t control{};std::vector<world::FieldPath> paths;ObjectId entity,component;};
    std::optional<MaterialEdit> material_edit_;
    Result<ObjectRoot> texture_resource(Project&,std::string_view,std::vector<MissingResource>* missing = nullptr,std::size_t input = 0);
    Result<void> replace_material_bindings(ObjectId,std::vector<world::MaterialBinding>);
    Result<void> admit_entity_edit(ObjectId) const;
    Result<world::ComponentStatus> admit_component_edit(ObjectId) const;
    Result<ObjectId> admit_property_edit(ObjectId) const;
    Result<void> apply_field(const FieldChange&, bool forward);
    Result<void> apply_group(const FieldGroupChange&, bool forward);
    Result<void> write_group(ObjectId,std::span<const world::FieldPath>,std::span<const Scalar>);
    Result<world::Commit> attach_created(ObjectRoot,std::wstring_view);
    void set_selection(Selection); // Single-object selection; resets the Entity set.
    Result<void> write_transforms(const TransformChange&, std::span<const world::Trs>);
    void forget_entity(ObjectId) noexcept;
    void remember(Change);
    // Releases retained data at the paths a committed change wrote.
    std::vector<RetainedSwap> release(const Change&);
    void restore(std::span<const RetainedSwap>, bool forward);
    static Result<void> validate_name(std::wstring_view);
    Result<world::Commit> replay(Change&, bool forward);
};
}

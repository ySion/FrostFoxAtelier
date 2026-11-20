#pragma once

#include "world/world.hpp"
#include "world/diagnostic.hpp"
#include "assets/reader.hpp"
#include "editor/graph_view.hpp"
#include "physics/physics.hpp"
#include "presentation/presentation.hpp"
#include <map>
#include <unordered_map>

namespace atelier::editor {
class Project;

class ProjectCode;

struct Parameter {
    const char* name;

    const char* unit;

    std::array<FieldId, 2> path;

    float minimum{-10000.f};

    float maximum{10000.f};

    float drag_step{.01f};

    float default_value{};
};

struct ComponentType {
    TypeId type;

    std::string name;
};

struct Property {
    std::string name;

    world::FieldPath path;

    TypeId type;

    Scalar value;

    std::string label;

    std::string category;

    std::string description;

    FieldRange range{};

    bool transient{};

    bool read_only{};

    bool is_string{};

    std::string string_value{};

    std::string default_string_value{};
};

struct PropertyPatch {
    world::FieldPath path;

    Scalar value;
};

struct Selection {
    ObjectId entity;

    ObjectId object; // Entity context and inspected Entity/Component, in this document only.

    bool operator==(const Selection&) const = default;
};

// Tolerant loading keeps data it cannot apply and reports it; strict stops at the first warning.
enum class LoadPolicy { tolerant, strict };

// The editor's own document also publishes the editor.* operator parameter types;
// Play and other standalone Sessions publish only engine and project types.
enum class SessionRole { runtime, editor };

// Saved scene data the loader could not apply, addressed by its scene record path
// (for example properties.speed, space or name). It is written back on save.
struct RetainedEntry {
    std::vector<std::string> path;

    std::string value;

    world::DiagnosticKind kind{};
};

struct RetainedView {
    std::string type; // Original type of a placeholder object; empty when the type resolved.

    std::vector<RetainedEntry> entries;
};

// A project resource a tolerant load could not provide: absent from the catalog
// (missing_resource) or listed but unreadable (unreadable_resource), with the read
// error preserved. A stand-in keeps its key, so saving writes the reference unchanged.
// input names a material texture input.
struct MissingResource {
    std::string key;

    Error error;

    std::optional<std::size_t> input;

    world::DiagnosticKind kind{world::DiagnosticKind::missing_resource};
};

// A key names one resource of each kind: a scene that names a material as a mesh
// leaves only the mesh unavailable.
struct ResourceLess {
    using is_transparent = void;

    bool operator()(std::pair<assets::AssetKind, std::string_view> a,
                    std::pair<assets::AssetKind, std::string_view> b) const noexcept {
        return a < b;
    }
};

using UnavailableResources = std::map<std::pair<assets::AssetKind, std::string>, MissingResource, ResourceLess>;

// Resource publications own their exact content until sent; ordinary changes
// carry only JSON text. Resource keys are part of that text.
struct LiveChange {
    std::string text;

    material::ImageRef texture;

    std::shared_ptr<const assets::MeshAsset> mesh{};
};

// Owning inspection data; paths are declarations, never Runtime fields or slots.
struct MaterialParameterView {
    std::string path;

    std::uint32_t components{}; // 0 is a texture binding.

    material::LocalValue value;

    material::LocalValue initial;
};

struct MaterialView {
    std::string asset_key;

    std::string parent_key;

    bool source{};

    bool scene{};

    bool default_material{};

    std::optional<source::SourceFile> source_file;

    std::vector<MaterialParameterView> declarations;

    std::vector<material::Override> entries;

    std::vector<material::AuthorDiagnostic> diagnostics;
};

// One document writer. History retains ordinary roots and removal records;
// evicting history never explicitly destroys a game object.
class Session final {
public:
    // Runs after engine registration, before any object exists. The returned
    // code owner is released after World End, always before its Runtime.
    using Prepare = std::function<Result<std::shared_ptr<void>>(world::World&)>;

    Session() = default;

    // Call once on an empty shell. Failure permits destruction only, not retry:
    // registration may have partially changed the Runtime. Other methods require success.
    Result<void> initialize(Prepare = {}, SessionRole = SessionRole::runtime);

    ~Session();

    const world::World& world() const noexcept { return world_; }

    Result<void> set_failure_policy(world::FailurePolicy policy) { return world_.set_failure_policy(policy); }

    std::vector<world::Diagnostic> take_diagnostics() { return world_.take_diagnostics(); }

    const world::Types& types() const noexcept { return types_; }

    const presentation::Types& presentation_types() const noexcept { return presentation_types_; }

    const physics::Types& physics_types() const noexcept { return physics_types_; }

    const presentation::Scene& presentation() const noexcept { return *presentation_; }

    // Borrowed running scene; invalidated by stop, close, or a failed update.
    physics::PhysicsScene* physics_scene() noexcept { return physics_.get(); }

    // Poses to draw the running simulation at this frame; empty without one. See PhysicsScene::presented_poses.
    std::span<const world::PresentedPose> presented_poses() const noexcept {
        return physics_ ? physics_->presented_poses() : std::span<const world::PresentedPose>{};
    }

    std::span<const Parameter> parameters() const noexcept { return parameters_; }

    Selection selection() const noexcept { return selection_; }

    // Entity-level multi-selection; the active Entity (selection().entity) is the
    // last entry when it is one of them. Selection is view state, not history.
    std::span<const ObjectId> selected_entities() const noexcept { return selected_entities_; }

    bool is_selected(ObjectId entity) const noexcept {
        return std::ranges::find(selected_entities_, entity) != selected_entities_.end();
    }

    std::wstring_view name(ObjectId) const noexcept;

    // The document's EnvironmentComponent, disabled or not; null without one.
    ObjectId environment() const;

    // World Settings: the World options of `settings` that differ from the current
    // environment become one property edit of the Environment, written as one group.
    // Without an Environment the first edit creates it with these values, in the same
    // history item; undoing that edit removes it again. A disabled Environment (the component or an ancestor)
    // rejects any change.
    Result<void> set_environment(const rendering::SceneLightingSettings&);

    // Binds the Environment's sky map to a resolved asset, as one history item; an empty
    // asset clears it. Creates the Environment like set_environment; a disabled one rejects.
    Result<void> set_sky_map(const assets::SkyAsset&);

    // Authored weak selection, not camera availability; null without an Environment.
    Result<ObjectId> main_camera() const;

    // Same-document Entity with exactly one CameraComponent (disabled is allowed).
    // Null clears. The first Set creates a default Environment in this history item.
    Result<void> set_main_camera(ObjectId);

    Result<void> rename(ObjectId, std::wstring); // Document label; shares scene history and object identity.

    Result<void> select(ObjectId);

    Result<void> select_entities(std::span<const ObjectId> entities, ObjectId active);

    Result<world::Commit> add_entity(TypeId actual = {});

    Result<world::Commit> create_static_mesh(const assets::MeshAsset&, math::Vector3 position = {}, Project* = nullptr);

    Result<world::Commit> create_procedural_generator(std::string_view graph_key = {}, math::Vector3 position = {},
                                                      Project* = nullptr);

    Result<void> set_procedural_graph(Project* project, ObjectId component, std::string_view graph_key);

    Result<void> sync_procedural_generators(Project* project);

    Result<world::Commit> bake_procedural_generator(Project* project, ObjectId target);

    Result<world::Commit> create_light(presentation::LightKind, math::Vector3 position = {});

    Result<world::Commit> create_camera(math::Vector3 position = {});

    // Deletes explicit subtrees as one history entry; unrelated document selection is preserved.
    Result<world::Commit> erase_entities(std::span<const ObjectId>);

    // Selects fresh Entity/Component identities; immutable resources stay shared.
    Result<world::Commit> duplicate_entities(std::span<const ObjectId>);

    Result<world::Commit> reparent(ObjectId child, ObjectId parent,
                                   world::ReparentRule = world::ReparentRule::keep_world,
                                   std::size_t index = world::append);

    // Publishes in input order and records the committed prefix as one history entry,
    // stopping at the first rejection or notification error. This is not atomic publication.
    // A non-append index is the first destination slot; subsequent children follow it.
    Result<world::Commit> reparent(std::span<const ObjectId> children, ObjectId parent,
                                   world::ReparentRule = world::ReparentRule::keep_world,
                                   std::size_t index = world::append);

    Result<void> set_transform(ObjectId, world::Trs);

    Result<void> remove_transform(ObjectId);

    Result<world::Commit> set_space_reference(ObjectId, std::optional<ObjectId>,
                                              world::ReparentRule = world::ReparentRule::keep_world);

    Result<world::Commit> add_component(ObjectId entity, TypeId);

    Result<world::Commit> remove_component(ObjectId);

    Result<std::vector<ComponentType>> component_types() const;

    Result<std::vector<Property>> properties(ObjectId) const;

    Result<Scalar> read_property(ObjectId, std::span<const FieldId>) const;

    Result<void> configure_body(ObjectId component, physics::BodySettings);

    Result<void> begin_edit(ObjectId component, std::span<const FieldId>, std::uint64_t control_session);

    // Whole-TRS edit of several Entities (viewport handles, modal G/R/S,
    // Euler fields). Preview writes complete transforms in the same order;
    // commit_edit records one history entry, cancel_edit restores all.
    Result<void> begin_transform(std::span<const ObjectId> entities, std::uint64_t control_session,
                                 bool amend_creation = false);

    // Bounds |scale|: negative axes mirror and zero is stored, as World accepts them.
    static constexpr float maximum_scale = 10000.f;

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

    // A complete scalar patch, validated together and recorded as one history item.
    // Transform paths reject before cancelling any existing edit.
    Result<void> set_properties(ObjectId, std::span<const PropertyPatch>);

    // Explicit import publication, never a parameter-history entry. Existing
    // resource roots retain identity and future captures resolve current pixels.
    Result<void> reimport_texture(Project&, std::string_view key, const std::filesystem::path& source);

    Result<void> publish_texture(std::string_view key, material::ImageRef);

    Result<void> reimport_mesh(Project&, std::string_view key, const std::filesystem::path& source);

    Result<void> publish_mesh(assets::Reader&, const assets::MeshAsset&);

    // With a missing list, materials and textures the reader cannot provide become
    // stand-ins and are reported there; without one they fail. Stand-ins are never
    // edited or saved as assets.
    Result<ObjectRoot> material_resource(assets::Reader&, std::string_view key,
                                         std::vector<MissingResource>* missing = nullptr);

    Result<ObjectRoot> material_resource(assets::Reader&, const assets::MaterialAsset&,
                                         std::vector<MissingResource>* missing = nullptr);

    // Publish author content on the existing asset identity, without history. A
    // supplied MS snapshot is authoritative: never reread newer text from disk.
    Result<ObjectRoot> publish_material(assets::Reader&, const assets::MaterialAsset&,
                                        std::optional<source::SourceFile> exact_source = {},
                                        std::vector<MissingResource>* missing = nullptr);

    // The mesh asset's default material per slot, as this document's shared material assets.
    // Unavailable ones always stand in and are reported in `missing` when given.
    Result<std::vector<presentation::MaterialBinding>> mesh_defaults(assets::Reader&, const assets::MeshAsset&,
                                                                     std::vector<MissingResource>* missing = nullptr);

    // What each slot shows where no component binds it: the default material, else the imported surface.
    // Generated instances read the same defaults as mesh components; a material edit publishes a new surface.
    Result<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>>
    mesh_surfaces(assets::Reader&, const assets::MeshAsset&, std::vector<MissingResource>* missing = nullptr);

    // Opens a material asset for inspection: the asset itself must read, while its
    // unavailable textures stand in so the material can be repaired.
    Result<ObjectRoot> open_material(Project&, std::string_view key);

    // The unavailable Mesh, Material, Texture or sky map a stand-in in this document replaces.
    const MissingResource* unavailable(assets::AssetKind, std::string_view key) const;

    Result<assets::MaterialAsset> material_asset(ObjectId) const;

    Result<std::vector<assets::MaterialAsset>> material_assets() const;

    std::vector<source::SourceFile> material_sources() const;

    Result<MaterialView> inspect_material(ObjectId) const;

    std::vector<ObjectId> material_objects() const;

    Result<void> begin_material_edit(ObjectId, std::uint64_t control_session);

    Result<void> preview_material_entry(std::uint64_t, material::Override, Project* = nullptr);

    Result<void> commit_material_edit(std::uint64_t);

    Result<void> cancel_material_edit();

    Result<void> set_material_texture(Project&, ObjectId, std::string_view path, std::string_view key);

    Result<void> assign_material(Project&, ObjectId component, std::string_view slot, std::string_view key);

    Result<void> clear_material(ObjectId component, std::string_view slot);

    Result<ObjectRoot> create_scene_material(ObjectId component, std::string_view slot);

    // Project resolves a newly enabled texture that has not been loaded before.
    Result<void> set_material_override(ObjectId, std::string_view path, bool, Project* = nullptr);

    Result<void> discard_material_override(ObjectId, std::string_view path);

    Result<void> save_material(Project&, std::string_view key);

    // The current Project resolves newly active texture keys; history owns no images.
    Result<void> material_undo(std::string_view key, bool redo = false, Project* = nullptr);

    bool material_dirty(std::string_view key) const;

    std::vector<std::string> dirty_materials() const;

    bool material_editing(std::uint64_t control_session) const noexcept;

    ObjectId loaded_material(std::string_view key) const;

    bool is_material(ObjectId) const;

    bool material_can_undo(std::string_view key, bool redo = false) const;

    void bind_project_code(const ProjectCode* code) noexcept;

    Result<std::vector<GraphNodeCatalogEntry>> procedural_node_types();

    Result<ObjectId> open_graph(Project&, std::string_view key);

    Result<ObjectId> create_scratch_graph(std::string_view key = "scratch/procedural");

    bool has_graph(std::string_view key) const noexcept;

    bool is_scratch_graph(std::string_view key) const noexcept;

    std::optional<std::string> procedural_graph_snapshot(std::string_view key) const;

    Result<ObjectId> resolve_procedural_generator(ObjectId target = {}) const;

    ObjectId loaded_graph(std::string_view key) const noexcept;

    bool is_graph_object(ObjectId) const noexcept;

    std::vector<ObjectId> graph_objects() const;

    std::vector<std::string> open_graphs() const;

    std::optional<std::string> graph_of_object(ObjectId) const;

    std::optional<std::string> graph_node_name(ObjectId) const;

    const GraphNode* graph_node(std::string_view key, std::string_view name) const noexcept;

    ObjectId graph_node_id(std::string_view key, std::string_view name) const noexcept;

    const GraphDocument* graph_document(std::string_view key) const noexcept;

    bool graph_dirty(std::string_view key) const;

    std::vector<std::string> dirty_graphs() const;

    bool graph_can_undo(std::string_view key, bool redo = false) const;

    Result<void> graph_undo(std::string_view key, bool redo = false);

    Result<void> save_graph(Project&, std::string_view key);

    Result<std::string> graph_add_node(std::string_view key, std::string_view type_name,
                                       std::string_view desired_name = {}, std::array<float, 2> position = {});

    Result<std::string> graph_place_node(std::string_view key, std::string_view type_name,
                                         std::array<float, 2> position, std::string_view from_node = {},
                                         std::string_view from_port = {}, std::string_view to_node = {},
                                         std::string_view to_port = {}, bool set_display = false);

    Result<void> graph_remove_nodes(std::string_view key, std::span<const std::string> names);

    Result<void> graph_rename_node(std::string_view key, std::string_view old_name, std::string_view new_name);

    Result<void> graph_move_nodes(std::string_view key,
                                  std::span<const std::pair<std::string, std::array<float, 2>>> moves);

    Result<void> graph_move_canvas_items(std::string_view key,
                                         std::span<const std::pair<std::string, std::array<float, 2>>> node_moves,
                                         std::span<const std::pair<std::string, std::array<float, 2>>> note_moves,
                                         std::span<const std::pair<std::string, std::array<float, 2>>> box_moves);

    Result<void> graph_auto_layout(std::string_view key);

    Result<void> graph_connect(std::string_view key, std::string_view from_node, std::string_view from_port,
                               std::string_view to_node, std::string_view to_port);

    Result<void> graph_disconnect(std::string_view key, std::string_view to_node, std::string_view to_port,
                                  std::string_view from_node = {}, std::string_view from_port = {});

    Result<void> graph_disconnect_edges(std::string_view key, std::span<const GraphConnectionView> edges);

    Result<void> graph_rewire(std::string_view key, const GraphConnectionView& remove_edge,
                              std::string_view new_to_node, std::string_view new_to_port);

    Result<void> graph_splice_node(std::string_view key, std::string_view node_name, std::array<float, 2> position,
                                   const GraphConnectionView& wire);

    Result<void> graph_set_display(std::string_view key, std::string_view node_name);

    Result<void> graph_set_template(std::string_view key, std::string_view node_name);

    Result<void> graph_set_bypass(std::string_view key, std::string_view node_name, bool bypassed);

    Result<void> graph_set_node_appearance(std::string_view key, std::span<const std::string> names,
                                           std::optional<GraphNodeShape> shape = {},
                                           std::optional<std::optional<std::uint32_t>> color = {},
                                           std::optional<std::string> comment = {});

    Result<std::string> graph_add_note(std::string_view key, std::array<float, 2> position,
                                       std::array<float, 2> size = {210.f, 116.f}, std::string_view title = "Note",
                                       std::string_view text = {}, std::uint32_t color = 0xd4a72c);

    Result<void> graph_update_note(std::string_view key, std::string_view name,
                                   std::optional<std::array<float, 2>> position = {},
                                   std::optional<std::array<float, 2>> size = {}, std::optional<std::string> title = {},
                                   std::optional<std::string> text = {}, std::optional<std::uint32_t> color = {});

    Result<void> graph_remove_notes(std::string_view key, std::span<const std::string> names);

    Result<std::string> graph_add_box(std::string_view key, std::array<float, 2> position,
                                      std::array<float, 2> size = {320.f, 220.f},
                                      std::string_view title = "Network Box", std::uint32_t color = 0x3b5b78);

    Result<void> graph_update_box(std::string_view key, std::string_view name,
                                  std::optional<std::array<float, 2>> position = {},
                                  std::optional<std::array<float, 2>> size = {}, std::optional<std::string> title = {},
                                  std::optional<std::uint32_t> color = {});

    Result<void> graph_remove_boxes(std::string_view key, std::span<const std::string> names);

    Result<void> graph_set_properties(std::string_view key, std::string_view node_name, json::Value properties);

    Result<void> set_string_property(ObjectId, std::span<const FieldId>, std::string_view value);

    Result<GraphEvaluation> evaluate_graph(std::string_view key, Project* = nullptr, ObjectId inspected_node = {},
                                           bool record_timings = false);

    // Cached authoring topology and last cooked node status. Reading it never cooks or scans geometry.
    Result<std::shared_ptr<const GraphInspectView>> graph_topology(std::string_view key);

    Result<GraphInspectView> inspect_graph(std::string_view key, Project* = nullptr, ObjectId inspected_node = {},
                                           bool record_timings = false, std::size_t max_samples = 4);

    bool owns_edit(std::uint64_t control_session) const noexcept;

    Result<world::Commit> undo(Project* = nullptr);

    Result<world::Commit> redo(Project* = nullptr);

    Result<world::Commit> close();

    Result<world::Commit> start();

    Result<void> update(float);

    Result<world::Commit> stop();

    Result<void> collect_step();

    Result<std::string> serialize() const;

    Result<std::string> serialize_subtree(ObjectId) const;

    // Makes this loaded document a Play copy: objects keep the editor's document
    // identities, and this Session never assigns new ones.
    void enter_play();

    // Applies one editor change batch {"ops":[...]} to a Play copy at a host
    // boundary, in order, with absolute values and without history. Returns one
    // command diagnostic per item not fully applied: warning reference_dropped when
    // the game already destroyed the target, warning command_rejected when Play
    // cannot follow the edit, error command_rejected when Play diverged. See
    // docs/modules/editor/projects.md for the batch format.
    Result<std::vector<world::Diagnostic>> apply_live(std::string_view batch, Project&);

    // Editor-side graphs for add and attach changes: identities are kept and
    // references leaving the graph are written as document identities.
    Result<std::string> serialize_live_subtree(ObjectId root) const;

    Result<std::string> serialize_live_components(ObjectId entity, std::span<const ObjectId> components) const;

    // While forwarding, each applied change, undo and redo of this document is also
    // recorded as Play change ops with the values it left. Previews and cancels are
    // recorded too; a later preview of the same target replaces the earlier op.
    void set_live_forwarding(bool);

    // The ops recorded since the last call, in order, with owning texture content. A change
    // that cannot be described stops recording, and every later call fails until
    // forwarding is set again: Play no longer follows the document.
    Result<std::vector<LiveChange>> take_live_ops();

    // Imports a complete detached graph, remaps internal references and records
    // one creation history item. Document identities are always fresh.
    Result<world::Commit> instantiate_subtree(std::string_view, Project&, ObjectId parent = {});

    static Result<std::unique_ptr<Session>> deserialize(std::string_view, Project&, Prepare = {},
                                                        LoadPolicy = LoadPolicy::tolerant,
                                                        SessionRole = SessionRole::runtime);

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

    Result<ObjectId> local_object(std::uint64_t) const;

    bool dirty() const noexcept {
        return !saved_cursor_ || *saved_cursor_ != cursor_ || (active_ && !is_graph_object(active_->root.id())) ||
               (group_ && !is_graph_object(group_->root.id())) || transform_ ||
               (material_edit_ && material_edit_->scene);
    }

    void mark_saved() noexcept {
        if (saved_cursor_ != cursor_) {
            saved_cursor_ = cursor_;
            ++revision_;
        }
    }

    void mark_modified() noexcept {
        if (saved_cursor_) {
            saved_cursor_.reset();
            ++revision_;
        }
    }

    // Monotonic authoring observation token, independent of undo cursor reuse.
    std::uint64_t revision() const noexcept { return revision_; }

    bool editing() const noexcept {
        return active_.has_value() || transform_.has_value() || material_edit_.has_value() || group_.has_value();
    }

    std::size_t undo_count() const noexcept { return cursor_; }

    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }

    // History entries ever recorded; eviction at the history limit does not lower it.
    std::uint64_t recorded() const noexcept { return recorded_; }

    static constexpr std::size_t maximum_entities = 8192;

    static constexpr std::size_t maximum_components = 1024;

private:
    struct ReferenceFieldChange {
        ObjectRoot root;

        ObjectId entity;

        world::FieldPath path;

        ObjectReference before;

        ObjectReference after; // Weak targets are not kept alive by history.
    };

    // Removal records restore together as one forest transaction.
    struct EntityChange {
        bool added{};

        std::vector<world::EntityRemoval> removals;

        std::optional<ReferenceFieldChange> cleared_camera;
    };

    struct ComponentChange {
        bool added{};

        world::ComponentRemoval removal;

        bool created_transform{};
    };

    struct ReparentChange {
        struct Entry {
            ObjectRoot root;

            world::Trs before;

            world::Trs after;
        };

        struct Move {
            ObjectRoot root;

            ObjectId before_parent;

            ObjectId after_parent;

            std::size_t before_index{};

            std::size_t after_index{};

            std::vector<Entry> transforms;
        };

        std::vector<Move> moves;
    };

    struct TransformPresenceChange {
        ObjectRoot root;

        std::optional<world::Trs> before;

        std::optional<world::Trs> after;

        std::optional<ObjectId> before_origin;

        std::optional<ObjectId> after_origin;
    };

    struct SpaceReferenceChange {
        ObjectRoot root;

        std::optional<ObjectId> before_origin;

        std::optional<ObjectId> after_origin;

        world::Trs before;

        world::Trs after;

        bool spatial{};
    };

    struct FieldChange {
        ObjectRoot root;

        ObjectId entity;

        world::FieldPath path;

        std::uint64_t control_session{};

        // TRS is one invariant group; ordinary business properties keep exact scalar types.
        std::variant<world::Trs, Scalar> before;

        std::variant<world::Trs, Scalar> after;
    };

    struct FieldGroupChange {
        ObjectRoot root;

        ObjectId entity;

        std::vector<world::FieldPath> paths;

        std::uint64_t control_session{};

        std::vector<Scalar> before;

        std::vector<Scalar> after;
    };

    struct TransformChange {
        struct Entry {
            ObjectRoot root;

            ObjectId entity;

            world::Trs before;

            world::Trs after;
        };

        std::vector<Entry> entries;

        std::uint64_t control_session{};

        Selection inspection;

        bool amend_creation{}; // Initial placement belongs to the immediately preceding creation entry.
    };

    struct NameChange {
        ObjectRoot root;

        std::wstring before;

        std::wstring after;
    };

    struct BodyChange {
        ObjectRoot root;

        ObjectId entity;

        physics::BodySettings before;

        physics::BodySettings after;
    };

    struct MaterialBindingChange {
        ObjectRoot root;

        ObjectId entity;

        std::vector<presentation::MaterialBinding> before;

        std::vector<presentation::MaterialBinding> after;
    };

    struct MaterialEntryChange {
        std::string path;

        std::optional<material::Override> before;

        std::optional<material::Override> after;
    };

    struct MaterialValueChange {
        ObjectRoot root;

        std::vector<MaterialEntryChange> entries;
    };

    // Undo and redo restore the stored map, so the scene sees the same pointer again.
    struct SkyMapChange {
        ObjectRoot root;

        ObjectId entity;

        presentation::SkyMapBinding before;

        presentation::SkyMapBinding after;
    };

    struct ProceduralGraphChange {
        ObjectRoot root;

        ObjectId entity;

        std::string before;

        std::string after;
    };

    struct RetainedChange {}; // Explicit discard; the history entry's swaps carry the data.

    using Change = std::variant<EntityChange, ComponentChange, FieldChange, NameChange, BodyChange, TransformChange,
                                MaterialBindingChange, MaterialValueChange, ReparentChange, TransformPresenceChange,
                                SpaceReferenceChange, FieldGroupChange, SkyMapChange, ProceduralGraphChange,
                                RetainedChange, ReferenceFieldChange>;

    // Sparse scene-record JSON text the loader could not apply. On the document entry,
    // materials are unreferenced definitions kept for write-back; on a component, they
    // are the scene materials its kept binding table names.
    struct Retained {
        std::string type;

        std::string record;

        std::string slots;

        std::vector<ObjectRoot> materials;

        bool empty() const noexcept { return type.empty() && record.empty() && slots.empty() && materials.empty(); }
    };

    struct RetainedSwap {
        ObjectId object;

        Retained before;

        Retained after;
    };

    struct Entry {
        Change change;

        std::vector<RetainedSwap> retained;
    };

    static constexpr std::size_t history_limit = 128;

    Runtime runtime_;

    world::Types types_{};

    presentation::Types presentation_types_{};

    physics::Types physics_types_{};

    std::shared_ptr<void> prepared_;

    std::shared_ptr<source::Program> engine_program_;

    const ProjectCode* project_code_{};

    world::World world_;

    std::optional<presentation::Scene> presentation_;

    std::unique_ptr<physics::PhysicsScene> physics_;

    std::array<Parameter, 3> parameters_{};

    std::vector<Entry> history_;

    std::size_t cursor_{};

    std::uint64_t recorded_{};

    std::uint64_t revision_{1};

    std::optional<std::size_t> saved_cursor_{0};

    Selection selection_;

    std::vector<ObjectId> selected_entities_; // Non-owning; validated on write, pruned with membership.

    std::optional<FieldChange> active_;

    std::optional<FieldGroupChange> group_;

    std::optional<TransformChange> transform_;

    std::unordered_map<ObjectId, std::wstring> names_; // Non-owning IDs; prune after collection.

    // Non-owning IDs like names_; entries travel with their object through history.
    std::unordered_map<ObjectId, Retained> retained_;

    Retained retained_document_;

    std::vector<world::Diagnostic> load_issues_;

    // Document-local identities never confer Runtime validity or World membership.
    // Detached history objects retain their ID; IDs are never reused.
    mutable std::unordered_map<ObjectId, std::uint64_t> local_ids_;

    mutable std::uint64_t next_local_id_{1};

    bool play_{};

    // Play copy: document identity to object, including objects the game destroyed.
    std::unordered_map<std::uint64_t, ObjectId> live_objects_;

    std::vector<ObjectRoot> live_materials_; // Scene materials a Play change defined.

    // Document side of Play: pending ops; a keyed op is a preview its next preview replaces.
    struct LiveOp {
        std::string key;

        LiveChange change;
    };

    bool forwarding_{};

    std::vector<LiveOp> live_ops_;

    std::optional<Error> live_failure_;

    // TRS leaves (1 position, 2 rotation, 4 scale) the active edit's previews sent, per Entity.
    std::unordered_map<ObjectId, unsigned> live_leaves_;

    struct GraphCodec;

    std::uint64_t next_name_{1};

    struct MaterialDocument {
        ObjectRoot root;

        assets::MaterialAsset author;

        std::optional<source::SourceFile> source_file;

        std::optional<Error> error;

        std::vector<material::AuthorDiagnostic> diagnostics;

        std::vector<material::Override> saved;

        std::vector<MaterialValueChange> history;

        std::size_t cursor{};
    };

    std::unordered_map<std::string, MaterialDocument> materials_;

    struct OpenGraph;

    struct OpenGraphDeleter {
        void operator()(OpenGraph*) const noexcept;
    };

    std::unordered_map<std::string, std::unique_ptr<OpenGraph, OpenGraphDeleter>> graphs_;

    struct GeneratorRuntimeCache;

    struct GeneratorCacheDeleter {
        void operator()(GeneratorRuntimeCache*) const noexcept;
    };

    std::unique_ptr<GeneratorRuntimeCache, GeneratorCacheDeleter> generator_cache_;

    std::unordered_map<ObjectId, std::string> scene_material_parents_; // Non-owning; survives parent loss.

    std::vector<std::string> loading_materials_; // Synchronous recursion guard, not a resource cache.

    std::unordered_map<std::string, ObjectRoot> textures_;

    UnavailableResources unavailable_;

    const MissingResource& remember_unavailable(const assets::Reader&, assets::AssetKind, std::string_view key,
                                                const Error&);

    struct MaterialEdit {
        ObjectRoot root;

        std::uint64_t control{};

        bool scene{};

        std::vector<MaterialEntryChange> entries;
    };

    std::optional<MaterialEdit> material_edit_;

    Result<void> apply_material_entries(ObjectId, std::span<const MaterialEntryChange>, bool forward,
                                        Project* = nullptr);

    Result<void> preview_material_entry(std::uint64_t, std::string_view, std::optional<material::Override>);

    Result<void> admit_material_edit(ObjectId) const;

    Result<ObjectRoot> texture_resource(assets::Reader&, std::string_view,
                                        std::vector<MissingResource>* missing = nullptr, std::size_t input = 0);

    Result<void> replace_material_bindings(ObjectId, std::vector<presentation::MaterialBinding>);

    Result<void> admit_entity_edit(ObjectId) const;

    Result<world::ComponentStatus> admit_component_edit(ObjectId) const;

    Result<ObjectId> admit_property_edit(ObjectId) const;

    Result<void> apply_field(const FieldChange&, bool forward);

    Result<void> apply_reference(const ReferenceFieldChange&, bool forward);

    Result<std::optional<ReferenceFieldChange>> main_camera_removal(std::span<const ObjectId>);

    Result<void> apply_group(const FieldGroupChange&, bool forward);

    Result<void> write_group(ObjectId, std::span<const world::FieldPath>, std::span<const Scalar>);

    Result<world::Commit> attach_created(ObjectRoot, std::wstring_view);

    struct EnvironmentEdit {
        std::vector<world::FieldPath> paths;

        std::vector<Scalar> values;
    };

    Result<EnvironmentEdit> environment_edit(const rendering::SceneLightingSettings&) const;

    // The owning Entity of an Environment that World settings may change.
    Result<ObjectId> admit_environment_edit(ObjectId) const;

    // A Detached Entity holding a new Environment with these values and sky map.
    Result<ObjectRoot> create_environment(std::span<const world::FieldPath>, std::span<const Scalar>,
                                          const presentation::SkyMapBinding* = nullptr);

    Result<void> publish_environment(ObjectRoot);

    void set_selection(Selection); // Single-object selection; resets the Entity set.

    Result<void> write_transforms(const TransformChange&, std::span<const world::Trs>);

    void prune_selection() noexcept; // After a committed subtree detach, retain only World members.

    // Record the values a change left in the World (forward: after it is applied or redone).
    void forward_change(const Change&, bool forward);

    // Record the current values of the active edit's targets as replaceable ops.
    void forward_edit();

    // A material asset as a resource op, or a scene material as its definition.
    void forward_material(ObjectId);

    // A texture publication retains this exact immutable image, not its latest file.
    void forward_texture(std::string_view key, material::ImageRef);

    void forward_mesh(const assets::MeshAsset&);

    void remember(Change);

    // Releases retained data at the paths a committed change wrote.
    std::vector<RetainedSwap> release(const Change&);

    void restore(std::span<const RetainedSwap>, bool forward);

    static Result<void> validate_name(std::wstring_view);

    Result<world::Commit> replay(Change&, bool forward, Project*);

    void close_graphs() noexcept;

    Result<void> publish_procedural_mesh(assets::Reader&, const assets::MeshAsset&);

    Result<const source::Program*> ensure_procedural_program();

    Result<OpenGraph*> editable_graph(std::string_view key, bool need_program = false);

    Result<std::string> add_graph_node_impl(std::string_view key, std::string_view type_name,
                                            std::string_view desired_name, std::array<float, 2> position,
                                            std::string_view from_node, std::string_view from_port,
                                            std::string_view to_node, std::string_view to_port, bool set_display);

    Result<std::pair<std::string, std::wstring>> resolve_procedural_graph_asset(std::string_view graph_key,
                                                                                Project* project) const;

    Result<void> remember_graph_scalar_edit(ObjectRoot target, std::vector<world::FieldPath> paths,
                                            std::vector<Scalar> before, std::vector<Scalar> after);
};
}

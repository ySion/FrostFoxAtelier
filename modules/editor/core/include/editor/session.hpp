#pragma once

#include "world/world.hpp"
#include "world/diagnostic.hpp"
#include "assets/reader.hpp"
#include "editor/graph_view.hpp"
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "physics/physics.hpp"
#include "presentation/presentation.hpp"
#include <string>
#include <string_view>

namespace atelier::procedural {
class Bvh;
}

namespace atelier::source {
class Program;
}

namespace atelier::editor {
class Project;

class ProjectCode;
struct ProjectDocument;

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

    String name;
};

struct Property {
    String name;

    world::FieldPath path;

    TypeId type;

    Scalar value;

    String label;

    String category;

    String description;

    FieldRange range{};

    bool transient{};

    bool read_only{};

    bool is_string{};

    String string_value{};

    String default_string_value{};

    MetadataList metadata{};
};

struct GraphPromotedParameterView {
    String name;

    String label;

    String category;

    String description;

    Array<String> targets;

    bool valid{};

    String invalid_reason;

    TypeId value_type{};

    bool is_vector{};

    bool overridden{};

    bool undeclared{};

    Array<Property> properties;

    Array<Scalar> default_scalars;

    String default_string;
};

struct PromotedParameterTarget {
    StringView graph_key{};

    StringView node_name{}; // Empty for Graph Root parameter; non-empty for AssetNode override.

    ObjectId generator_component{}; // Non-null for Scene ProceduralGeneratorComponent override.
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
    Array<String> path;

    String value;

    world::DiagnosticKind kind{};
};

struct RetainedView {
    String type; // Original type of a placeholder object; empty when the type resolved.

    Array<RetainedEntry> entries;
};

// A project resource a tolerant load could not provide: absent from the catalog
// (missing_resource) or listed but unreadable (unreadable_resource), with the read
// error preserved. A stand-in keeps its key, so saving writes the reference unchanged.
// input names a material texture input.
struct MissingResource {
    String key;

    Error error;

    Optional<std::size_t> input;

    world::DiagnosticKind kind{world::DiagnosticKind::missing_resource};
};

// A key names one resource of each kind: a scene that names a material as a mesh
// leaves only the mesh unavailable.
using UnavailableResources = Map<std::pair<assets::AssetKind, String>, MissingResource>;

// Resource publications own their exact content until sent; ordinary changes
// carry only JSON text. Resource keys are part of that text.
struct LiveChange {
    String text;

    material::ImageRef texture;

    std::shared_ptr<const assets::MeshAsset> mesh{};
};

// Owning inspection data; paths are declarations, never Runtime fields or slots.
struct MaterialParameterView {
    String path;

    std::uint32_t components{}; // 0 is a texture binding.

    material::LocalValue value;

    material::LocalValue initial;
};

struct MaterialView {
    String asset_key;

    String parent_key;

    bool source{};

    bool scene{};

    bool default_material{};

    Optional<source::SourceFile> source_file;

    Array<MaterialParameterView> declarations;

    Array<material::Override> entries;

    Array<material::AuthorDiagnostic> diagnostics;
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

    Array<world::Diagnostic> take_diagnostics() { return world_.take_diagnostics(); }

    const world::Types& types() const noexcept { return types_; }

    const presentation::Types& presentation_types() const noexcept { return presentation_types_; }

    const physics::Types& physics_types() const noexcept { return physics_types_; }

    const presentation::Scene& presentation() const noexcept { return *presentation_; }

    // Borrowed running scene; invalidated by stop, close, or a failed update.
    physics::PhysicsScene* physics_scene() noexcept { return physics_.get(); }

    // Poses to draw the running simulation at this frame; empty without one. See PhysicsScene::presented_poses.
    Span<const world::PresentedPose> presented_poses() const noexcept {
        return physics_ ? physics_->presented_poses() : Span<const world::PresentedPose>{};
    }

    Span<const Parameter> parameters() const noexcept { return parameters_; }

    Selection selection() const noexcept { return selection_; }

    // Entity-level multi-selection; the active Entity (selection().entity) is the
    // last entry when it is one of them. Selection is view state, not history.
    Span<const ObjectId> selected_entities() const noexcept { return selected_entities_; }

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

    Result<void> select_entities(Span<const ObjectId> entities, ObjectId active);

    Result<world::Commit> add_entity(TypeId actual = {});

    Result<world::Commit> create_static_mesh(const assets::MeshAsset&, math::Vector3 position = {}, Project* = nullptr);

    Result<world::Commit> create_procedural_generator(StringView graph_key = {}, math::Vector3 position = {},
                                                      Project* = nullptr);

    Result<void> set_procedural_graph(Project* project, ObjectId component, StringView graph_key);

    [[nodiscard]] json::Document generator_overrides(ObjectId component) const;

    Result<Array<GraphPromotedParameterView>> generator_promoted_parameters(ObjectId component,
                                                                            Project* project = nullptr) const;

    Result<void> set_generator_override_json(Project* project, ObjectId component, StringView param_name,
                                             Optional<json::Value> value);

    Result<void> sync_procedural_generators(Project* project);

    // Latest generator failure, valid until the next sync or document close.
    [[nodiscard]] const Error* procedural_generator_error(ObjectId component) const noexcept;

    Result<world::Commit> bake_procedural_generator(Project* project, ObjectId target);

    Result<world::Commit> create_light(presentation::LightKind, math::Vector3 position = {});

    Result<world::Commit> create_camera(math::Vector3 position = {});

    // Deletes explicit subtrees as one history entry; unrelated document selection is preserved.
    Result<world::Commit> erase_entities(Span<const ObjectId>);

    // Selects fresh Entity/Component identities; immutable resources stay shared.
    Result<world::Commit> duplicate_entities(Span<const ObjectId>);

    Result<world::Commit> reparent(ObjectId child, ObjectId parent,
                                   world::ReparentRule = world::ReparentRule::keep_world,
                                   std::size_t index = world::append);

    // Publishes in input order and records the committed prefix as one history entry,
    // stopping at the first rejection or notification error. This is not atomic publication.
    // A non-append index is the first destination slot; subsequent children follow it.
    Result<world::Commit> reparent(Span<const ObjectId> children, ObjectId parent,
                                   world::ReparentRule = world::ReparentRule::keep_world,
                                   std::size_t index = world::append);

    Result<void> set_transform(ObjectId, world::Trs);

    Result<void> remove_transform(ObjectId);

    Result<world::Commit> set_space_reference(ObjectId, Optional<ObjectId>,
                                              world::ReparentRule = world::ReparentRule::keep_world);

    Result<world::Commit> add_component(ObjectId entity, TypeId);

    Result<world::Commit> remove_component(ObjectId);

    Result<Array<ComponentType>> component_types() const;

    Result<Array<Property>> properties(ObjectId) const;

    Result<Scalar> read_property(ObjectId, Span<const FieldId>) const;

    Result<void> configure_body(ObjectId component, physics::BodySettings);

    Result<void> begin_edit(ObjectId component, Span<const FieldId>, std::uint64_t control_session);

    // Whole-TRS edit of several Entities (viewport handles, modal G/R/S,
    // Euler fields). Preview writes complete transforms in the same order;
    // commit_edit records one history entry, cancel_edit restores all.
    Result<void> begin_transform(Span<const ObjectId> entities, std::uint64_t control_session,
                                 bool amend_creation = false);

    // Bounds |scale|: negative axes mirror and zero is stored, as World accepts them.
    static constexpr float maximum_scale = 10000.f;

    // Stable gesture origin; previews never feed their converted values back into the next preview.
    Result<world::Trs> original_transform(std::uint64_t control_session, ObjectId component) const;

    Result<void> preview_transform(std::uint64_t control_session, Span<const world::Trs>);

    Result<void> set_property(ObjectId, Span<const FieldId>, Scalar, std::uint64_t control_session);

    Result<void> preview_edit(std::uint64_t control_session, float);

    Result<void> preview_edit(std::uint64_t control_session, Scalar);

    Result<void> commit_edit(std::uint64_t control_session);

    Result<void> cancel_edit();

    Result<void> reset_position(ObjectId component); // One group change and one history entry.

    // Several scalar lanes of one object (a colour's R/G/B) as one value: preview writes
    // without history; commit records one entry against the values seen at begin.
    Result<void> begin_group_edit(ObjectId, Span<const world::FieldPath>, std::uint64_t control_session);

    Result<void> preview_group_edit(std::uint64_t control_session, Span<const Scalar>);

    // A complete scalar patch, validated together and recorded as one history item.
    // Transform paths reject before cancelling any existing edit.
    Result<void> set_properties(ObjectId, Span<const PropertyPatch>);

    // Explicit import publication, never a parameter-history entry. Existing
    // resource roots retain identity and future captures resolve current pixels.
    Result<void> reimport_texture(Project&, StringView key, const std::filesystem::path& source);

    Result<void> publish_texture(StringView key, material::ImageRef);

    Result<void> reimport_mesh(Project&, StringView key, const std::filesystem::path& source);

    Result<void> publish_mesh(assets::Reader&, const assets::MeshAsset&);

    // With a missing list, materials and textures the reader cannot provide become
    // stand-ins and are reported there; without one they fail. Stand-ins are never
    // edited or saved as assets.
    Result<ObjectRoot> material_resource(assets::Reader&, StringView key,
                                         Array<MissingResource>* missing = nullptr);

    Result<ObjectRoot> material_resource(assets::Reader&, const assets::MaterialAsset&,
                                         Array<MissingResource>* missing = nullptr);

    // Publish author content on the existing asset identity, without history. A
    // supplied MS snapshot is authoritative: never reread newer text from disk.
    Result<ObjectRoot> publish_material(assets::Reader&, const assets::MaterialAsset&,
                                        Optional<source::SourceFile> exact_source = {},
                                        Array<MissingResource>* missing = nullptr);

    // The mesh asset's default material per slot, as this document's shared material assets.
    // Unavailable ones always stand in and are reported in `missing` when given.
    Result<Array<presentation::MaterialBinding>> mesh_defaults(assets::Reader&, const assets::MeshAsset&,
                                                               Array<MissingResource>* missing = nullptr);

    // What each slot shows where no component binds it: the default material, else the imported surface.
    // Generated instances read the same defaults as mesh components; a material edit publishes a new surface.
    Result<std::shared_ptr<const Array<material::ProgramSurfaceRef>>>
    mesh_surfaces(assets::Reader&, const assets::MeshAsset&, Array<MissingResource>* missing = nullptr);

    // Opens a material asset for inspection: the asset itself must read, while its
    // unavailable textures stand in so the material can be repaired.
    Result<ObjectRoot> open_material(Project&, StringView key);

    // The unavailable Mesh, Material, Texture or sky map a stand-in in this document replaces.
    const MissingResource* unavailable(assets::AssetKind, StringView key) const;

    Result<assets::MaterialAsset> material_asset(ObjectId) const;

    Result<Array<assets::MaterialAsset>> material_assets() const;

    Array<source::SourceFile> material_sources() const;

    Result<MaterialView> inspect_material(ObjectId) const;

    Array<ObjectId> material_objects() const;

    Result<void> begin_material_edit(ObjectId, std::uint64_t control_session);

    Result<void> preview_material_entry(std::uint64_t, material::Override, Project* = nullptr);

    Result<void> commit_material_edit(std::uint64_t);

    Result<void> cancel_material_edit();

    Result<void> set_material_entry(ObjectId, material::Override, Project* = nullptr);

    Result<void> set_material_texture(Project&, ObjectId, StringView path, StringView key);

    Result<void> assign_material(Project&, ObjectId component, StringView slot, StringView key);

    Result<void> clear_material(ObjectId component, StringView slot);

    Result<ObjectRoot> create_scene_material(ObjectId component, StringView slot);

    // Project resolves a newly enabled texture that has not been loaded before.
    Result<void> set_material_override(ObjectId, StringView path, bool, Project* = nullptr);

    Result<void> discard_material_override(ObjectId, StringView path);

    Result<void> save_material(Project&, StringView key);

    // The current Project resolves newly active texture keys; history owns no images.
    Result<void> material_undo(StringView key, bool redo = false, Project* = nullptr);

    bool material_dirty(StringView key) const;

    Array<String> dirty_materials() const;

    bool material_editing(std::uint64_t control_session) const noexcept;

    ObjectId loaded_material(StringView key) const;

    bool is_material(ObjectId) const;

    Result<bool> is_material_type(TypeId) const;

    bool material_can_undo(StringView key, bool redo = false) const;

    void bind_project_code(const ProjectCode* code) noexcept;

    Result<Optional<GraphNodeCatalogEntry>> procedural_node_type(StringView name, Project* project);

    Result<Array<GraphNodeCatalogEntry>> procedural_node_types(Project* project = nullptr,
                                                               StringView exclude_graph_key = {});

    Result<const source::Program*> procedural_source_program();

    Result<ObjectId> open_graph(Project&, StringView key);

    Result<ObjectId> create_scratch_graph(StringView key = "scratch/procedural");

    bool has_graph(StringView key) const noexcept;

    bool is_scratch_graph(StringView key) const noexcept;

    bool has_graph_placeholders() const noexcept;

    Result<Array<OpenGraphSnapshot>> snapshot_open_graphs() const;

    Result<void> restore_open_graphs(Project* project, Span<const OpenGraphSnapshot> snapshots);

    // Captures the open author graph for repro, checking effective session freezes against a cold replay.
    Result<Optional<String>> procedural_graph_snapshot(StringView key) const;

    // Optional lookup returns a null identity for a selection without a generator. Read failures propagate.
    Result<ObjectId> resolve_procedural_generator(ObjectId target = {}, bool required = true) const;

    ObjectId loaded_graph(StringView key) const noexcept;

    bool is_graph_object(ObjectId) const noexcept;

    Array<ObjectId> graph_objects() const;

    Array<String> open_graphs() const;

    Optional<String> graph_of_object(ObjectId) const;

    Optional<String> graph_node_name(ObjectId) const;

    const GraphNode* graph_node(StringView key, StringView name) const noexcept;

    ObjectId graph_node_id(StringView key, StringView name) const noexcept;

    const GraphDocument* graph_document(StringView key) const noexcept;

    bool graph_dirty(StringView key) const;

    Array<String> dirty_graphs() const;

    bool graph_can_undo(StringView key, bool redo = false) const;

    Result<void> graph_undo(StringView key, bool redo = false);

    Result<void> save_graph(Project&, StringView key);

    Result<String> graph_add_node(StringView key, StringView type_name,
                                  StringView desired_name = {}, std::array<float, 2> position = {},
                                  Project* project = nullptr);

    Result<String> graph_place_node(StringView key, StringView type_name,
                                    std::array<float, 2> position, StringView from_node = {},
                                    StringView from_port = {}, StringView to_node = {},
                                    StringView to_port = {}, bool set_display = false,
                                    Project* project = nullptr);

    Result<void> graph_remove_nodes(StringView key, Span<const String> names);

    Result<Array<String>> graph_duplicate_nodes(StringView key, Span<const String> names,
                                                std::array<float, 2> offset = {36.f, 36.f});

    Result<void> graph_rename_node(StringView key, StringView old_name, StringView new_name);

    Result<void> graph_move_nodes(StringView key,
                                  Span<const std::pair<String, std::array<float, 2>>> moves,
                                  bool amend_creation = false);

    Result<void> graph_move_canvas_items(StringView key,
                                         Span<const std::pair<String, std::array<float, 2>>> node_moves,
                                         Span<const std::pair<String, std::array<float, 2>>> note_moves,
                                         Span<const std::pair<String, std::array<float, 2>>> box_moves);

    Result<void> graph_auto_layout(StringView key);

    Result<void> graph_connect(StringView key, StringView from_node, StringView from_port,
                               StringView to_node, StringView to_port);

    Result<void> graph_disconnect(StringView key, StringView to_node, StringView to_port,
                                  StringView from_node = {}, StringView from_port = {});

    Result<void> graph_disconnect_edges(StringView key, Span<const GraphConnectionView> edges);

    Result<void> graph_rewire(StringView key, const GraphConnectionView& remove_edge,
                              StringView new_to_node, StringView new_to_port);

    Result<void> graph_splice_node(StringView key, StringView node_name, std::array<float, 2> position,
                                   const GraphConnectionView& wire);

    Result<void> graph_set_display(StringView key, StringView node_name);

    Result<void> graph_set_template(StringView key, StringView node_name);

    Result<void> graph_set_bypass(StringView key, StringView node_name, bool bypassed);

    Result<void> graph_set_frozen(StringView key, StringView node_name, bool frozen, Project* = nullptr);

    Result<void> graph_set_node_appearance(StringView key, Span<const String> names,
                                           Optional<GraphNodeShape> shape = {},
                                           Optional<Optional<std::uint32_t>> color = {},
                                           Optional<String> comment = {});

    Result<String> graph_add_note(StringView key, std::array<float, 2> position,
                                  std::array<float, 2> size = {210.f, 116.f}, StringView title = "Note",
                                  StringView text = {}, std::uint32_t color = 0xd4a72c);

    Result<void> graph_update_note(StringView key, StringView name,
                                   Optional<std::array<float, 2>> position = {},
                                   Optional<std::array<float, 2>> size = {}, Optional<String> title = {},
                                   Optional<String> text = {}, Optional<std::uint32_t> color = {});

    Result<void> graph_remove_notes(StringView key, Span<const String> names);

    Result<String> graph_add_box(StringView key, std::array<float, 2> position,
                                 std::array<float, 2> size = {320.f, 220.f},
                                 StringView title = "Network Box", std::uint32_t color = 0x3b5b78);

    Result<void> graph_update_box(StringView key, StringView name,
                                  Optional<std::array<float, 2>> position = {},
                                  Optional<std::array<float, 2>> size = {}, Optional<String> title = {},
                                  Optional<std::uint32_t> color = {});

    Result<void> graph_remove_boxes(StringView key, Span<const String> names);

    // Apply one text edit to the open graph. Returns whether an undo step was published.
    Result<bool> graph_apply(StringView key, json::Value patch, Project* project = nullptr);

    Result<std::shared_ptr<const procedural::Bvh>> graph_node_surface(StringView key, StringView name,
                                                                      StringView output, Project* project);

    Result<procedural::NodeBindings> graph_node_bindings(StringView key, StringView name, Project* project);

    Result<json::Document> graph_node_properties(StringView key, StringView name);

    Result<void> graph_set_properties(StringView key, StringView node_name, json::Value properties);

    Result<Array<GraphPromotedParameterView>> graph_promoted_parameters(StringView key,
                                                                        Project* project = nullptr) const;

    Result<Array<GraphPromotedParameterView>>
    graph_asset_node_parameters(StringView key, StringView node_name, Project* project = nullptr) const;

    Optional<String> graph_controlling_parameter(StringView key, StringView node_name,
                                                 StringView field_path) const noexcept;

    Optional<String> graph_overlapping_parameter(StringView key, StringView node_name,
                                                 StringView field_path) const noexcept;

    Result<String> graph_expose_parameter(StringView key, StringView node_name,
                                          StringView field_path, StringView desired_name = {},
                                          StringView label = {}, StringView category = {},
                                          StringView description = {});

    Result<void> graph_bind_parameter_target(StringView key, StringView param_name,
                                             StringView node_name, StringView field_path);

    Result<void> graph_unbind_parameter_target(StringView key, StringView node_name,
                                               StringView field_path);

    Result<void> graph_update_parameter(StringView key, StringView param_name,
                                        Optional<String> new_name = {}, Optional<String> label = {},
                                        Optional<String> category = {},
                                        Optional<String> description = {},
                                        Optional<std::size_t> new_index = {});

    Result<void> graph_remove_parameter(StringView key, StringView param_name);

    Result<void> graph_set_parameter_json(StringView key, StringView param_name, json::Value value);

    Result<void> graph_set_asset_override_json(StringView key, StringView node_name,
                                               StringView param_name, Optional<json::Value> value,
                                               Project* project = nullptr);

    Result<void> graph_set_node_asset(StringView key, StringView node_name,
                                      StringView target_asset_key, Project* project = nullptr);

    Result<std::pair<String, String>> graph_extract_asset(Project& project, StringView key,
                                                          Span<const String> selected_nodes,
                                                          const std::filesystem::path& asset_relative_path,
                                                          StringView desired_node_name = {});

    Result<void> begin_graph_parameter_edit(StringView key, StringView node_name,
                                            StringView param_name, Optional<std::size_t> lane,
                                            std::uint64_t control_session, Project* project = nullptr,
                                            ObjectId generator_component = {});

    Result<void> preview_graph_parameter_edit(std::uint64_t control_session, Span<const Scalar> values);

    Result<void> commit_graph_parameter_edit(std::uint64_t control_session);

    Result<void> cancel_graph_parameter_edit();

    bool graph_parameter_editing(std::uint64_t control_session) const noexcept;

    Result<void> set_promoted_parameter_scalars(const PromotedParameterTarget& target, StringView param_name,
                                                Span<const Scalar> values, Project* project = nullptr);

    Result<void> set_promoted_parameter_string(const PromotedParameterTarget& target, StringView param_name,
                                               StringView value, Project* project = nullptr);

    Result<void> clear_promoted_parameter_override(const PromotedParameterTarget& target, StringView param_name,
                                                   Project* project = nullptr);

    Result<void> set_string_property(ObjectId, Span<const FieldId>, StringView value);

    Result<GraphEvaluation> evaluate_graph(StringView key, Project* = nullptr, ObjectId inspected_node = {},
                                           bool record_timings = false);

    // Cached authoring topology and last cooked node status. Reading it never cooks or scans geometry.
    Result<std::shared_ptr<const GraphInspectView>> graph_topology(StringView key);

    Result<GraphInspectView> inspect_graph(StringView key, Project* = nullptr, ObjectId inspected_node = {},
                                           bool record_timings = false, std::size_t max_samples = 4,
                                           bool include_spatial = true, ObjectId reference_node = {},
                                           StringView reference_name = {});

    bool owns_edit(std::uint64_t control_session) const noexcept;

    Result<world::Commit> undo(Project* = nullptr);

    Result<world::Commit> redo(Project* = nullptr);

    Result<world::Commit> close();

    Result<world::Commit> start();

    Result<void> update(float);

    Result<world::Commit> stop();

    Result<void> collect_step();

    Result<String> serialize() const;

    Result<String> serialize_subtree(ObjectId) const;

    // Makes this loaded document a Play copy: objects keep the editor's document
    // identities, and this Session never assigns new ones.
    void enter_play();

    // Applies one editor change batch {"ops":[...]} to a Play copy at a host
    // boundary, in order, with absolute values and without history. Returns one
    // command diagnostic per item not fully applied: warning reference_dropped when
    // the game already destroyed the target, warning command_rejected when Play
    // cannot follow the edit, error command_rejected when Play diverged. See
    // docs/modules/editor/projects.md for the batch format.
    Result<Array<world::Diagnostic>> apply_live(StringView batch, Project&);

    // Editor-side graphs for add and attach changes: identities are kept and
    // references leaving the graph are written as document identities.
    Result<String> serialize_live_subtree(ObjectId root) const;

    Result<String> serialize_live_components(ObjectId entity, Span<const ObjectId> components) const;

    // While forwarding, each applied change, undo and redo of this document is also
    // recorded as Play change ops with the values it left. Previews and cancels are
    // recorded too; a later preview of the same target replaces the earlier op.
    void set_live_forwarding(bool);

    // The ops recorded since the last call, in order, with owning texture content. A change
    // that cannot be described stops recording, and every later call fails until
    // forwarding is set again: Play no longer follows the document.
    Result<Array<LiveChange>> take_live_ops();

    // Imports a complete detached graph, remaps internal references and records
    // one creation history item. Document identities are always fresh.
    Result<world::Commit> instantiate_subtree(StringView, Project&, ObjectId parent = {});

    static Result<std::unique_ptr<Session>> deserialize(StringView, Project&, Prepare = {},
                                                        LoadPolicy = LoadPolicy::tolerant,
                                                        SessionRole = SessionRole::runtime);

    // Diagnostics recorded by the load that created this document.
    Span<const world::Diagnostic> load_issues() const noexcept { return load_issues_; }

    // Current unresolved data of the document; entries disappear when edited or discarded.
    Array<world::Diagnostic> issues() const;

    // ObjectId{} addresses document-level data (top-level keys and World settings).
    bool has_retained(ObjectId) const noexcept;

    StringView placeholder_type(ObjectId) const noexcept;

    Result<RetainedView> retained(ObjectId) const;

    // Drops the retained data of one object (or the document) as one undoable entry.
    // A placeholder keeps its original type.
    Result<void> discard_retained(ObjectId);

    std::size_t placeholder_count() const noexcept;

    Optional<std::uint64_t> document_identity(ObjectId) const;

    Result<ObjectId> local_object(std::uint64_t) const;

    bool dirty() const noexcept {
        return !saved_cursor_ || *saved_cursor_ != cursor_ || (active_ && !is_graph_object(active_->root.id())) ||
               (group_ && !is_graph_object(group_->root.id())) || transform_ ||
               (material_edit_ && material_edit_->document_key.empty()) ||
               (graph_param_edit_ && graph_param_edit_->generator_root.id() != ObjectId{});
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
        return active_.has_value() || transform_.has_value() || material_edit_.has_value() ||
               graph_param_edit_.has_value() || group_.has_value();
    }

    std::size_t undo_count() const noexcept { return cursor_; }

    std::size_t redo_count() const noexcept { return history_.size() - cursor_; }

    // History entries ever recorded; eviction at the history limit does not lower it.
    std::uint64_t recorded() const noexcept { return recorded_; }

    static constexpr std::size_t maximum_entities = 8192;

    static constexpr std::size_t maximum_components = 1024;

private:
    friend ProjectDocument load_project_scene(Project&, StringView, LoadPolicy, bool, SessionRole);
    static Result<std::unique_ptr<Session>> restore_scene(StringView, Project&, Prepare, LoadPolicy, SessionRole);
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

        Array<world::EntityRemoval> removals;

        Optional<ReferenceFieldChange> cleared_camera;
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

            Array<Entry> transforms;
        };

        Array<Move> moves;
    };

    struct TransformPresenceChange {
        ObjectRoot root;

        Optional<world::Trs> before;

        Optional<world::Trs> after;

        Optional<ObjectId> before_origin;

        Optional<ObjectId> after_origin;
    };

    struct SpaceReferenceChange {
        ObjectRoot root;

        Optional<ObjectId> before_origin;

        Optional<ObjectId> after_origin;

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

        Array<world::FieldPath> paths;

        std::uint64_t control_session{};

        Array<Scalar> before;

        Array<Scalar> after;
    };

    struct TransformChange {
        struct Entry {
            ObjectRoot root;

            ObjectId entity;

            world::Trs before;

            world::Trs after;
        };

        Array<Entry> entries;

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

        Array<presentation::MaterialBinding> before;

        Array<presentation::MaterialBinding> after;
    };

    struct MaterialEntryChange {
        String path;

        Optional<material::Override> before;

        Optional<material::Override> after;
    };

    struct MaterialValueChange {
        ObjectRoot root;

        Array<MaterialEntryChange> entries;
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

        String before;

        String after;

        json::Document before_overrides;

        json::Document after_overrides;
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
        String type;

        String record;

        String slots;

        Array<ObjectRoot> materials;

        // Material instances named solely by $ref in this owner's retained record or slot data.
        Array<std::pair<std::uint64_t, ObjectRoot>> reference_roots;

        void prune_reference_roots();

        bool empty() const noexcept { return type.empty() && record.empty() && slots.empty() && materials.empty(); }
    };

    struct RetainedSwap {
        ObjectId object;

        Retained before;

        Retained after;
    };

    struct Entry {
        Change change;

        Array<RetainedSwap> retained;
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

    Optional<presentation::Scene> presentation_;

    std::unique_ptr<physics::PhysicsScene> physics_;

    std::array<Parameter, 3> parameters_{};

    Array<Entry> history_;

    std::size_t cursor_{};

    std::uint64_t recorded_{};

    std::uint64_t revision_{1};

    Optional<std::size_t> saved_cursor_{0};

    Selection selection_;

    Array<ObjectId> selected_entities_; // Non-owning; validated on write, pruned with membership.

    Optional<FieldChange> active_;

    Optional<FieldGroupChange> group_;

    Optional<TransformChange> transform_;

    Map<ObjectId, std::wstring> names_; // Non-owning IDs; prune after collection.

    // Non-owning IDs like names_; entries travel with their object through history.
    Map<ObjectId, Retained> retained_;

    Retained retained_document_;

    Array<world::Diagnostic> load_issues_;

    // Document-local identities never confer Runtime validity or World membership.
    // Detached history objects retain their ID; IDs are never reused.
    mutable Map<ObjectId, std::uint64_t> local_ids_;

    mutable std::uint64_t next_local_id_{1};

    bool play_{};

    // Play copy: document identity to object, including objects the game destroyed.
    Map<std::uint64_t, ObjectId> live_objects_;

    Array<ObjectRoot> live_materials_; // Scene materials a Play change defined.

    // Document side of Play: pending ops; a keyed op is a preview its next preview replaces.
    struct LiveOp {
        String key;

        LiveChange change;
    };

    bool forwarding_{};

    Array<LiveOp> live_ops_;

    Optional<Error> live_failure_;

    // TRS leaves (1 position, 2 rotation, 4 scale) the active edit's previews sent, per Entity.
    Map<ObjectId, unsigned> live_leaves_;

    struct GraphCodec;

    std::uint64_t next_name_{1};

    struct MaterialDocument {
        ObjectRoot root;

        assets::MaterialAsset author;

        Optional<source::SourceFile> source_file;

        Optional<Error> error;

        Array<material::AuthorDiagnostic> diagnostics;

        Array<material::Override> saved;

        Array<MaterialValueChange> history;

        std::size_t cursor{};
    };

    Map<String, MaterialDocument> materials_;

    struct OpenGraph;

    struct OpenGraphDeleter {
        void operator()(OpenGraph*) const noexcept;
    };

    Map<String, std::unique_ptr<OpenGraph, OpenGraphDeleter>> graphs_;

    struct GeneratorRuntimeCache;

    struct GeneratorCacheDeleter {
        void operator()(GeneratorRuntimeCache*) const noexcept;
    };

    std::unique_ptr<GeneratorRuntimeCache, GeneratorCacheDeleter> generator_cache_;

    struct AssetSchemaCache;

    struct AssetSchemaCacheDeleter {
        void operator()(AssetSchemaCache*) const noexcept;
    };

    mutable std::unique_ptr<AssetSchemaCache, AssetSchemaCacheDeleter> asset_schema_cache_;

    std::shared_ptr<procedural::CachePool> procedural_cache_pool_;

    // Sparse promoted-parameter override records per ProceduralGeneratorComponent; travels through history like names_.
    Map<ObjectId, json::Document> generator_overrides_;

    struct GraphParameterEdit {
        std::uint64_t control{};

        String graph_key;

        String node_name; // Empty when editing Graph Root promoted parameter; non-empty for AssetNode override.

        ObjectRoot generator_root; // Non-empty when editing a Scene ProceduralGeneratorComponent override.

        ObjectId generator_owner{};

        Project* edit_project{};

        String param_name;

        Optional<std::size_t> lane; // Specific vector lane, or nullopt for scalar/enum or whole vector.

        TypeId value_type{};

        bool is_vector{};

        FieldRange range{};

        Array<FieldRange> lane_ranges;

        Array<detail::ScalarField> lane_fields;

        Array<Scalar> current_scalars;

        struct NodeBackup {
            ObjectRoot target;

            json::Document before_props;

            json::Document before_kept;
        };

        Array<NodeBackup> node_backups;

        json::Document before_overrides;
    };

    Optional<GraphParameterEdit> graph_param_edit_;

    Result<void> reject_if_graph_controlled_field(ObjectId object, Span<const FieldId> path) const;

    Result<const GraphDocument*> resolve_target_graph_document(StringView asset_key, Project* project) const;

    Map<ObjectId, String> scene_material_parents_; // Non-owning; survives parent loss.

    Array<String> loading_materials_; // Synchronous recursion guard, not a resource cache.

    Map<String, ObjectRoot> textures_;

    UnavailableResources unavailable_;

    const MissingResource& remember_unavailable(const assets::Reader&, assets::AssetKind, StringView key,
                                                const Error&);

    struct MaterialEdit {
        ObjectRoot root;

        std::uint64_t control{};

        // Empty for a scene material; asset key in `materials_` for a shared material asset.
        String document_key;

        Array<MaterialEntryChange> entries;
    };

    Optional<MaterialEdit> material_edit_;

    Result<void> apply_material_entries(ObjectId, Span<const MaterialEntryChange>, bool forward,
                                        Project* = nullptr);

    Result<String> admit_material_edit(ObjectId);

    Result<MaterialEdit> prepare_material_edit(ObjectId, std::uint64_t control = 0);

    Result<void> preview_material_entry(MaterialEdit&, MaterialEntryChange, Project*);

    Result<void> change_material_entry(MaterialEdit&, MaterialEntryChange, Project*);

    void record_material_edit(MaterialEdit&);

    // Reading a named dependency fills its cache. Explicit publication also forwards the change to Play.
    Result<ObjectRoot> store_material(assets::Reader&, const assets::MaterialAsset&, Optional<source::SourceFile>,
                                      Array<MissingResource>*);

    Result<ObjectRoot> texture_resource(assets::Reader&, StringView,
                                        Array<MissingResource>* missing = nullptr, std::size_t input = 0);

    Result<void> replace_material_bindings(ObjectId, Array<presentation::MaterialBinding>);

    Result<void> admit_entity_edit(ObjectId) const;

    Result<world::ComponentStatus> admit_component_edit(ObjectId) const;

    Result<ObjectId> admit_property_edit(ObjectId) const;

    Result<void> apply_field(const FieldChange&, bool forward);

    Result<void> apply_reference(const ReferenceFieldChange&, bool forward);

    Result<Optional<ReferenceFieldChange>> main_camera_removal(Span<const ObjectId>);

    Result<void> apply_group(const FieldGroupChange&, bool forward);

    Result<void> write_group(ObjectId, Span<const world::FieldPath>, Span<const Scalar>);

    Result<world::Commit> attach_created(ObjectRoot, std::wstring_view);

    struct EnvironmentEdit {
        Array<world::FieldPath> paths;

        Array<Scalar> values;
    };

    Result<EnvironmentEdit> environment_edit(const rendering::SceneLightingSettings&) const;

    // The owning Entity of an Environment that World settings may change.
    Result<ObjectId> admit_environment_edit(ObjectId) const;

    // A Detached Entity holding a new Environment with these values and sky map.
    Result<ObjectRoot> create_environment(Span<const world::FieldPath>, Span<const Scalar>,
                                          const presentation::SkyMapBinding* = nullptr);

    Result<void> publish_environment(ObjectRoot);

    void set_selection(Selection); // Single-object selection; resets the Entity set.

    Result<void> write_transforms(const TransformChange&, Span<const world::Trs>);

    void prune_selection() noexcept; // After a committed subtree detach, retain only World members.

    // Record the values a change left in the World (forward: after it is applied or redone).
    void forward_change(const Change&, bool forward);

    // Record the current values of the active edit's targets as replaceable ops.
    void forward_edit();

    // A material asset as a resource op, or a scene material as its definition.
    void forward_material(ObjectId);

    // A texture publication retains this exact immutable image, not its latest file.
    void forward_texture(StringView key, material::ImageRef);

    void forward_mesh(const assets::MeshAsset&);

    void remember(Change);

    // Releases retained data at the paths a committed change wrote.
    Array<RetainedSwap> release(const Change&);

    void restore(Span<const RetainedSwap>, bool forward);

    static Result<void> validate_name(std::wstring_view);

    Result<world::Commit> replay(Change&, bool forward, Project*);

    void close_graphs() noexcept;

    Result<void> publish_procedural_mesh(assets::Reader&, const assets::MeshAsset&);

    Result<OpenGraph*> editable_graph(StringView key, bool need_program = false);

    Result<void> apply_graph_patch(StringView key, json::Value patch, Project* project);

    Result<String> add_graph_node_impl(StringView key, StringView type_name,
                                       StringView desired_name, std::array<float, 2> position,
                                       StringView from_node, StringView from_port,
                                       StringView to_node, StringView to_port, bool set_display,
                                       Project* project = nullptr);

    std::shared_ptr<procedural::CachePool> ensure_procedural_cache_pool();

    AssetPortResolver make_asset_port_resolver(Project* project) const;

    Result<procedural::Evaluator*> prepare_graph_evaluator(OpenGraph& open, Project* project);

    Result<void> sync_open_graph_assets(OpenGraph& open, Project* project, bool materialize_subgraphs);

    Result<std::pair<String, std::wstring>> resolve_procedural_graph_asset(StringView graph_key,
                                                                           Project* project) const;

    Result<void> remember_graph_scalar_edit(ObjectRoot target, Array<world::FieldPath> paths,
                                            Array<Scalar> before, Array<Scalar> after);

    void bump_graph_object_epoch(ObjectId object) noexcept;
};
}

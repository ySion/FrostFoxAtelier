#pragma once

#include "editor/session.hpp"
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/optional.hpp"
#include "foundation/set.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "editor/menu_bindings.hpp"
#include "editor/operators.hpp"
#include "editor/keymap.hpp"
#include "editor/control_workbench.hpp"
#include "editor/world_panel.hpp"
#include "editor/property_row.hpp"
#include "editor/procedural_panes.hpp"
#include "editor/asset_browser.hpp"
#include "editor/source_editor.hpp"
#include "editor/output.hpp"
#include "editor/project_code.hpp"
#include "editor/viewport_tool.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"
#include "renderer/view_settings.hpp"
#include "ui_language/live_document.hpp"

#include <algorithm>

namespace atelier::editor {
// Colour editor conversions (sRGB-agnostic: the edited field values are used as-is).
std::array<double, 3> rgb_to_hsv(std::array<double, 3>) noexcept;

std::array<double, 3> hsv_to_rgb(std::array<double, 3>) noexcept;

std::wstring color_hex(std::array<double, 3>);

Optional<std::array<double, 3>> parse_color_hex(std::wstring_view);

namespace control {
inline constexpr ui::ControlKey workbench = 18;

inline constexpr ui::ControlKey position_fold = 57;

inline constexpr ui::ControlKey viewport = 40;

inline constexpr ui::ControlKey viewport_pivot = 845;

inline constexpr ui::ControlKey viewport_modes = 846;

inline constexpr ui::ControlKey viewport_snap = 847;

inline constexpr ui::ControlKey viewport_camera = 848;

inline constexpr ui::ControlKey viewport_tools_menu = 849;

inline constexpr ui::ControlKey viewport_hud = 850;

inline constexpr ui::ControlKey viewport_readout = 851;

inline constexpr ui::ControlKey viewport_rail_snap = 852;

inline constexpr ui::ControlKey viewport_add_menu = 853;

inline constexpr ui::ControlKey viewport_delete_menu = 855;

inline constexpr ui::ControlKey viewport_shading_pie = 856;

inline constexpr ui::ControlKey shading_preset_first = 890;

inline constexpr ui::ControlKey viewport_axis_pie = 857;

inline constexpr ui::ControlKey viewport_cavity = 858;

inline constexpr ui::ControlKey viewport_pie = 859;

inline constexpr ui::ControlKey cavity_enabled = 910;

inline constexpr ui::ControlKey cavity_group_first = 911;

inline constexpr ui::ControlKey cavity_value_first = 920;

inline constexpr ui::ControlKey viewport_fps = 904;

inline constexpr ui::ControlKey viewport_fly_reticle = 860;

inline constexpr ui::ControlKey viewport_hint = 861;

// Blender header menus and grouped header widgets.
inline constexpr ui::ControlKey viewport_view_menu = 974;

inline constexpr ui::ControlKey viewport_select_menu = 975;

inline constexpr ui::ControlKey viewport_object_menu = 976;

inline constexpr ui::ControlKey viewport_header_add = 977;

inline constexpr ui::ControlKey viewport_shading_modes = 948;

inline constexpr ui::ControlKey viewport_select_modes = 949;

inline constexpr ui::ControlKey viewport_snapping = 978;

inline constexpr ui::ControlKey object_clear_menu = 979;

inline constexpr ui::ControlKey object_snap_menu = 980;

inline constexpr ui::ControlKey view_viewpoint_menu = 995;

inline constexpr ui::ControlKey select_mode_menu = 996;

inline constexpr ui::ControlKey viewport_orientation = 998;

// Houdini's right-hand display bar: view toggles that never touch scene history.
inline constexpr ui::ControlKey display_bar = 1006;

inline constexpr ui::ControlKey display_grid = 1007;

inline constexpr ui::ControlKey display_axes = 1008;

inline constexpr ui::ControlKey display_wire = 1009;

inline constexpr ui::ControlKey display_triangles = 1010;

inline constexpr ui::ControlKey display_cavity = 1011;

inline constexpr ui::ControlKey display_fps = 1012;

inline constexpr ui::ControlKey display_wire_only = 1013;

inline constexpr ui::ControlKey display_lit = 1014;

inline constexpr ui::ControlKey display_sop_points = 1160;

inline constexpr ui::ControlKey display_sop_normals = 1161;

inline constexpr ui::ControlKey display_sop_orient = 1162;

inline constexpr ui::ControlKey display_toggle_first = 1200; // Bar buttons: 1200..1215, below dynamic bindings.

inline constexpr ui::ControlKey display_sop_toggle_first = 1220; // SOP bar buttons: 1220..1235.

inline constexpr ui::ControlKey display_backfaces = 1240;

inline constexpr ui::ControlKey display_sop_face_normals = 1241;

inline constexpr ui::ControlKey display_sop_bounds = 1242;

inline constexpr ui::ControlKey display_sop_info = 1243;

inline constexpr ui::ControlKey viewport_geometry_hud = 1244;

inline constexpr ui::ControlKey viewport_scene_overlay = 1245;

inline constexpr ui::ControlKey display_sop_group = 1246;

inline constexpr ui::ControlKey inline_rename = 1023;

// Colour editor popup and its fields (fixed keys; one editor at a time).
inline constexpr ui::ControlKey color_editor = 940;

inline constexpr ui::ControlKey color_rgb = 941;

inline constexpr ui::ControlKey color_hsv = 944;

inline constexpr ui::ControlKey color_hex = 953;

inline constexpr ui::ControlKey color_revert = 954;

inline constexpr ui::ControlKey color_done = 955;

inline constexpr ui::ControlKey color_group = 956; // rgb 941..943, hsv 944..946

inline constexpr ui::ControlKey scene_tab = 600;

inline constexpr ui::ControlKey entities_tab = 601;

inline constexpr ui::ControlKey parameters_tab = 602;

inline constexpr ui::ControlKey assets_tab = 604;

inline constexpr ui::ControlKey assets_content = 650;

inline constexpr ui::ControlKey output_tab = 607;

inline constexpr ui::ControlKey output_content = 690;

inline constexpr ui::ControlKey output_table = 691;

inline constexpr ui::ControlKey structure_tab = 608;

inline constexpr ui::ControlKey structure_content = 694;

inline constexpr ui::ControlKey structure_tree = 695;

inline constexpr ui::ControlKey structure_filter = 696;

inline constexpr ui::ControlKey shelf = 730;

inline constexpr ui::ControlKey shelf_tabs = 731;

inline constexpr ui::ControlKey shelf_create = 732;

inline constexpr ui::ControlKey shelf_view = 733;

inline constexpr ui::ControlKey shelf_body = 734;

inline constexpr ui::ControlKey shelf_handle = 735;

inline constexpr ui::ControlKey shelf_contents = 736;

inline constexpr ui::ControlKey scene_content = 640;

inline constexpr ui::ControlKey scene_shading = 646;

inline constexpr ui::ControlKey shading_first = 760;

inline constexpr ui::ControlKey edges_first = 763;

inline constexpr ui::ControlKey topology_first = 767;

inline constexpr ui::ControlKey scene_guides = 645;

inline constexpr ui::ControlKey guide_background = 769;

inline constexpr ui::ControlKey guide_grid = 770;

inline constexpr ui::ControlKey guide_axes = 771;

inline constexpr ui::ControlKey shading_scene_lit = 772;

inline constexpr ui::ControlKey scene_path = 642;

inline constexpr ui::ControlKey scene_back = 643;

inline constexpr ui::ControlKey scene_forward = 644;

inline constexpr ui::ControlKey entity_path = 22;

inline constexpr ui::ControlKey entity_back = 25;

inline constexpr ui::ControlKey entity_forward = 26;

inline constexpr ui::ControlKey entity_tree = 23;

inline constexpr ui::ControlKey tree_world = 27;

inline constexpr ui::ControlKey tree_filter = 28;

inline constexpr ui::ControlKey parameter_path = 76;

inline constexpr ui::ControlKey parameter_back = 78;

inline constexpr ui::ControlKey parameter_forward = 79;

inline constexpr ui::ControlKey world_context = 720;

constexpr bool is_path(ui::ControlKey key) {
    return key == scene_path || key == entity_path || key == parameter_path;
}

constexpr int navigation_direction(ui::ControlKey key) {
    return key == scene_back || key == entity_back || key == parameter_back || key == network_control::nav_back ? -1
           : key == scene_forward || key == entity_forward || key == parameter_forward ||
                   key == network_control::nav_forward
               ? 1
               : 0;
}

inline constexpr ui::ControlKey parameter_search = 54;

inline constexpr ui::ControlKey parameter_search_toggle = 86;

inline constexpr ui::ControlKey add_component = 87;

inline constexpr ui::ControlKey component_choice_first = 1ULL << 40;

inline constexpr ui::ControlKey parameter_list = 84;

inline constexpr ui::ControlKey group_components = 1ULL << 42;

inline constexpr ui::ControlKey group_categories = group_components + 1;

inline constexpr ui::ControlKey parameter_menu = 500;

inline constexpr ui::ControlKey menu_group_reset = 501;

inline constexpr ui::ControlKey menu_reset = 502;

inline constexpr ui::ControlKey menu_copy = 503;

inline constexpr ui::ControlKey menu_paste = 504;

inline constexpr auto file_menu = menu_key(MenuGroup::file);

inline constexpr auto edit_menu = menu_key(MenuGroup::edit);

inline constexpr auto create_menu = menu_key(MenuGroup::create);

inline constexpr auto view_menu = menu_key(MenuGroup::view);

inline constexpr auto window_menu = menu_key(MenuGroup::window);

inline constexpr auto help_menu = menu_key(MenuGroup::help);

inline constexpr ui::ControlKey menu_bar_handle = 715;

inline constexpr ui::ControlKey menu_bar_body = 716;

constexpr bool is_menu(ui::ControlKey key) {
    return menu_group(key) != nullptr;
}
}

struct Workspace {
    struct Binding {
        enum class Kind {
            object,
            structure,
            property_group,
            field,
            slider,
            rotation,
            property_row,
            position_default,
            rotation_default,
            scale_default,
            name,
            navigation,
            material_slot,
            procedural_graph_slot,
            material_texture,
            material_override,
            material_field,
            color_pick,
            saved_data,
            operator_call,
            text_field,
            graph_param_context,
            graph_param_field,
            graph_param_slider,
            graph_param_text,
            graph_param_override_toggle,
            graph_param_submenu,
            graph_node_asset_slot,
            group_guide
        };

        ui::ControlKey key{};

        ObjectId object;

        Array<FieldId> path; // Empty for object rows; fields address the actual inspected object.

        Kind kind{};

        ObjectId component{};

        String slot{};

        std::size_t index{};

        double color_maximum{}; // color_pick reuses the owning numeric property's range.

        String
            material_path{}; // Stable MS declaration path or Graph parameter / field path; index is a vector lane.

        String node_name{}; // Optional target node name inside a .procgraph.

        std::unique_ptr<OperatorBinding> call{}; // Retired with this binding, before the document closes.
    };

    Workspace();

    // Initialize once at this stable address before using the workspace. On failure, destroy it.
    [[nodiscard]] Result<void> initialize();

    Workspace(const Workspace&) = delete;

    Workspace& operator=(const Workspace&) = delete;

    Workspace(Workspace&&) = delete; // Live scopes borrow this workspace's key authority.

    Workspace& operator=(Workspace&&) = delete;

    OperatorRegistry operators; // Built-in operators; menus, Control and later the keymap call them.

    ui::DockSpace dock;

    ToolSettings viewport_settings;

    bool camera_flying{}; // Projection only; Application owns the navigation lifecycle.

    TransformTool viewport_mode{TransformTool::transform}; // Current operation, including a modal gesture.

    std::wstring viewport_readout; // Modal G/R/S header text; empty hides the band.

    CameraView camera_view{CameraView::perspective};

    bool viewport_options_collapsed{};

    bool show_fps{};

    std::wstring viewport_fps; // Smoothed main-window presentation rate; empty while sampling starts.

    bool inspecting_asset{}; // Inspect the browser's persistent resource identity, not an Entity instance.

    bool inspecting_network{}; // Inspect the selected SOP node / graph root in Parameters while staying in World.

    String material_inspection;

    String active_graph; // Active Isolated SOP .procgraph key; empty in Scene World context.

    bool network_pinned{}; // When true in World mode, Network pane stays locked to pinned_graph.

    String pinned_graph;

    String selected_graph_node; // Decoupled from Display Flag; empty selects the Graph object itself.

    Array<String> selected_graph_nodes; // Multi-selection set on the 2D Network canvas.

    String network_context_node; // Target node for RMB context menu on Network canvas.

    std::array<float, 2> network_context_pos{}; // World position for RMB canvas context menu.

    Optional<bool> sop_show_points{}; // nullopt: auto-enable on pure point clouds, off on surfaces/instances.

    bool sop_show_normals{};

    bool sop_show_face_normals{};

    bool sop_show_orient{};

    bool sop_show_bounds{};

    bool sop_show_info{true};

    GeometryGroupGuide sop_group;

    bool effective_show_points(const procedural::Geometry* geometry) const noexcept;

    Result<ObjectId> active_procedural_generator(const Session& session) const;

    GeometrySpreadsheet spreadsheet;

    NetworkCanvas network;

    void set_single_graph_selection(StringView node_name) {
        selected_graph_node = String::from_utf8_validated(node_name);
        selected_graph_nodes.clear();
        if (!selected_graph_node.empty())
            selected_graph_nodes.add(selected_graph_node);
    }

    struct GraphBreadcrumb {
        String graph_key;

        String asset_node_name;
    };

    Array<GraphBreadcrumb> graph_breadcrumbs;

    String dived_network_graph; // Active child asset graph when diving in while 3D Viewport stays in World.

    Map<String, bool> network_lock_overrides;

    bool is_network_graph_locked(const Session& session, StringView net_graph) const;

    ObjectId inspected_graph_object(const Session&, StringView graph_key = {}) const noexcept;

    String default_selected_graph_node(const Session&, StringView graph_key) const;

    String selected_generator_graph(const Session&) const;

    String current_network_graph(const Session&) const;

    String effective_network_graph(Session&, Project*);

    std::wstring graph_path_title(const Project*) const;

    std::wstring network_path_title(const Session&, const Project*, StringView net_graph) const;

    void bump_view_revision() noexcept { ++view_revision_; }

    Result<void> reset_layout();

    // Scene Tree drag: the row under the pointer, or World, receives the dragged Entities.
    struct TreeDrop {
        ObjectId parent;

        ui::ControlKey row{};
    };

    Optional<TreeDrop> tree_drop(const Session&, const ui::UiFrame&, ObjectId dragged, ui::DragPoint) const;

    ui::ControlKey tree_drop_row{}; // Highlighted drop target while a Scene Tree drag is live.

    // Open colour editor: the edited object/fields, the value at open (for revert and the
    // original chip) and where the popup sits. Edits run as one Session group transaction.
    struct ColorEdit {
        ObjectId object;

        Array<world::FieldPath> paths;

        std::array<double, 3> original{};

        std::array<double, 3> value{};

        double maximum{1};

        float x{};

        float y{};

        std::wstring hex;

        bool composition_failed{};

        Optional<material::Override> material_entry; // Keeps vector4 alpha; no fixed material schema.
    };

    Optional<ColorEdit> color_edit;

    // Inline rename over a tree row: the renamed Entity, drawn once its row is presented.
    // An Entity row (object set) or an Asset Browser cell (asset set); asset paths are project-relative.
    struct RowRename {
        ObjectId object;

        std::wstring text;

        std::filesystem::path asset{};

        ui::TableKey cell{};
    };

    Optional<RowRename> row_rename;

    bool show_workbench() const noexcept { return dock.visible(control::workbench); }

    bool sample_history() const noexcept { return dock.focused_tab() == control::workbench; }

    bool can_navigate(int direction) const noexcept;

    Result<void> navigate(Session&, OperatorHost&, int direction);

    Result<void> select_object(Session&, OperatorHost&, ObjectId);

    Result<void> select_tree(Session&, OperatorHost&, ui::ControlKey, ui::SelectionModifiers = {});

    // Tree row expansion is view state: not undoable, never changes the document
    // or the selection. Scene Tree addresses its World row as ObjectId{}. If any
    // identity is not a row of the tree, nothing changes.
    enum class EntityTree { scene_tree, structure };

    Result<void> set_tree_expanded(const Session&, EntityTree, Span<const ObjectId>, bool expanded);

    // Expands the Entity's Scene Tree ancestors; false when the tree filter hides its row.
    Result<bool> reveal_in_tree(const Session&, ObjectId entity);

    // Finds a currently projected call; closed menus cannot retain an executable binding.
    ui::ControlKey operator_control(const OperatorCall&) const;

    // The accepted focused control (or a menu's return focus) identifies an inline material document.
    Context operator_context(Session&, Project*, bool interactive, const ui::ControlBox* focused = nullptr) const;

    Result<bool> apply_view(Session&, OperatorHost&, const ui::Action&);

    // Copies the World's environment into the view's author settings; true when they changed.
    bool sync_world(const Session&);

    Result<void> set_view_settings(Session&, const renderer::ViewSettings&, math::Vector3 focus = {});

    // Apply preferences after the candidate view and its authored environment have been validated.
    void apply_view_preferences(const renderer::ViewSettings&);

    std::uint64_t view_revision() const noexcept { return view_revision_; }

    Result<bool> apply_ui(const ui::Action&);

    StringView ui_error() const noexcept {
        return ui_document_ ? ui_document_->diagnostic().view() : StringView{};
    }

    Optional<ui_language::Statistics> ui_statistics(StringView component) const noexcept {
        return ui_document_ ? ui_document_->statistics(component) : Optional<ui_language::Statistics>{};
    }

    [[nodiscard]] Result<void> commit_ui_controls();

    bool reject_ui_controls(String message);

    // Choose an authoring file before this document's first projection.
    [[nodiscard]] Result<void> set_ui_source(std::filesystem::path);

    Result<std::filesystem::path> ui_source() const;

    std::uint64_t ui_revision() const noexcept { return ui_document_ ? ui_document_->revision() : 0; }

    const ui_language::LiveDocument::ReloadTiming* profile_ui_reloads(ui_language::LiveDocument::Profile profile) {
        return ui_document_ ? ui_document_->profile_reloads(profile) : nullptr;
    }

    std::shared_ptr<ui_language::Component> ui_component(StringView name) const {
        return ui_document_ ? ui_document_->component(name) : nullptr;
    }

    renderer::ViewSettings view{.shading = renderer::Shading::matcap,
                                .background = renderer::Background::gradient,
                                .reference_grid = true,
                                .origin_axes = true};

    ui::ControlKey shelf_page{control::shelf_create};

    AssetBrowser assets;

    SourceEditor source_editor;

    std::shared_ptr<const ProjectBuild> project_build;

    Output output;

    bool poll_output(); // Service work, including while Output is hidden/minimized.

    Result<void> locate_output(); // Uses the record's original root/version, never the active project.

    bool source_history() const noexcept { return dock.focused_tab() == source_control::tab; }

    bool project_open{};

    bool scene_available{true};

    bool project_running{};

    bool project_stopping{};

    bool project_ui_configured{};

    // While a Play run follows the document: change batches it has not acknowledged yet.
    Optional<std::size_t> play_pending;

    bool play_diverged{}; // Play stopped following the document until it restarts.

    bool menu_bar_collapsed{};

    bool shelf_collapsed{};

    bool parameter_search_visible{};

    ControlWorkbench workbench;

    WorldPanel lighting;

    std::wstring status{L"Ready"};

    std::wstring query;

    std::wstring render_notice; // Shader preparation progress or failure for the current view.

    std::wstring tree_query;

    std::wstring structure_query;

    Optional<ui::ContextMenuRequest> menu;

    Array<ui::MenuItem> menu_items; // Same model for menubar, combo, pane and parameter menus.

    bool pie_menu() const noexcept {
        return menu && menu->source == control::viewport &&
               (menu->group == control::viewport_shading_pie || menu->group == control::viewport_axis_pie);
    }

    // ui supplies tree scroll and focus; trees describe only rows near their viewport.
    // Failure rejects the UI candidate; bindings may have changed, so the host must stop this frame.
    Result<Array<ui::Control>> describe(Session&, ui_layout::Size, const ui::UiFrame*, Project* = nullptr,
                                        const ui::Session* ui = nullptr,
                                        const ui::ControlBox* command_focus = nullptr);

    Result<void> describe(ui::ControlList&, Session&, ui_layout::Size, const ui::UiFrame*, Project*, const ui::Session*,
                          const ui::ControlBox* command_focus);

    void reset_document(bool preserve_view = false);

    void reconcile_rebuild(Session& document, Project* project);

    const Binding* binding(ui::ControlKey) const noexcept;

    ui::ControlKey object_control(ObjectId) const noexcept;

    ui::ControlKey field_control(ObjectId, Span<const FieldId>) const noexcept;

    ui::ControlKey reset_control(ObjectId) const noexcept;

    ui::ControlKey name_control(ObjectId) const noexcept;

    Optional<TypeId> component_choice(ui::ControlKey) const noexcept;

    struct MaterialChoice {
        enum class Kind { assign, clear, edit, texture, create_scene, discard };

        Kind kind{};

        ObjectId object;

        String slot{};

        String key{};

        String path{};
    };

    Optional<MaterialChoice> material_choice(ui::ControlKey) const;

    struct NetworkPathChoice {
        enum class Kind { follow_entity, select_graph, unpin_world };

        Kind kind{};

        ObjectId entity;

        String graph_key;
    };

    Optional<NetworkPathChoice> network_path_choice(ui::ControlKey) const;

    Selection menu_selection() const noexcept { return menu_selection_; }

private:
    Map<String, ui::ControlKey> category_keys_;

    PropertyHelpCache property_help_;

    bool parameter_by_category_{};

    std::uint64_t view_revision_{};

    Runtime ui_runtime_; // Editor authoring scopes outlive individual scene documents.

    std::unique_ptr<ui_language::LiveDocument> ui_document_;

    std::filesystem::path ui_source_;

    Optional<String> ui_checkpoint_; // Source only; never keeps another Runtime alive.

    Array<Binding> bindings_;

    std::size_t binding_cursor_{};

    ui::ControlKey next_binding_{2048};

    std::array<ui::ControlKey, std::size(menu_bindings)> shelf_items_{};

    std::array<ui::ControlKey, 5> resource_properties_{};

    Array<ObjectId> navigation_{ObjectId{}}; // Selection history; never retains scene objects.

    std::size_t navigation_cursor_{};

    struct TreeBuild {
        Array<ui::TreeNode> nodes;

        Array<ObjectId> objects;

        Array<ui::TreeRow> rows;

        Optional<std::pair<ObjectId, ObjectId>> primary;

        Map<ObjectId, bool> edits;

        std::uint32_t node(ObjectId object) const noexcept {
            const auto at = std::ranges::find(objects, object);
            return at == objects.end() ? ui::no_tree_node : static_cast<std::uint32_t>(at - objects.begin());
        }
    };

    struct EntityTreeView {
        Map<ObjectId, bool> expansion;

        Array<ObjectId> objects;

        Array<ui::TreeRow> rows;

        Optional<std::pair<ObjectId, ObjectId>> primary;

        Map<ObjectId, bool> requests;

        TreeBuild candidate;

        bool pending{};

        void begin(bool visible) {
            pending = false;
            candidate.edits.clear();
            if (visible)
                candidate.edits.swap(requests);
        }

        bool expanded(ObjectId object, bool initial) const {
            if (const bool* edit = candidate.edits.find(object))
                return *edit;
            const bool* at = expansion.find(object);
            return at ? *at : initial;
        }

        Result<Array<std::uint32_t>> window(ObjectId selected, std::wstring_view filter, ui::TreeViewport,
                                            Span<const ObjectId> pinned);

        void commit(const Set<ObjectId>& live);
    };

    Optional<Set<ObjectId>> pending_live_;

    EntityTreeView scene_tree_;

    EntityTreeView structure_tree_;

    Result<void> build_structure_tree(const Session&, ObjectId entity, TreeBuild&) const;

    Array<ComponentType> component_choices_;

    Array<MaterialChoice> material_choices_;

    Array<NetworkPathChoice> network_path_choices_;

    String last_network_graph_;

    String failed_auto_open_graph_;

    std::uint64_t empty_network_canvas_revision_{ui::ControlList::new_region_revision()};

    Selection menu_selection_;

    Result<void> build_controls(ui::ControlList&, Session&, ui_layout::Size, const ui::UiFrame*, Project*,
                                const ui::Session*, const ui::ControlBox*, bool& retry);

    Result<ui::ControlKey> allocate_binding_key();

    // Active rows form a prefix. Recompute positions after insertion, which may relocate storage.
    template <class Match, class Make>
    Result<ui::ControlKey> bind_row(std::size_t first, Match match, Make make) {
        const auto at = std::find_if(bindings_.begin() + static_cast<std::ptrdiff_t>(first), bindings_.end(), match);
        const auto index = static_cast<std::size_t>(at - bindings_.begin());
        if (index == bindings_.size()) {
            ATELIER_TRY_BIND(key, allocate_binding_key());
            bindings_.add(make(*key));
        } else if (index < binding_cursor_) {
            return bindings_[index].key;
        }
        if (index != binding_cursor_)
            std::swap(bindings_[index], bindings_[binding_cursor_]);
        return bindings_[binding_cursor_++].key;
    }

    Result<ui::ControlKey> bind_graph_param(StringView graph_key, StringView node_name,
                                            StringView param_path, Binding::Kind kind, std::size_t lane = 0,
                                            ObjectId object = {}, ObjectId component = {},
                                            Span<const FieldId> path = {});

    Result<ui::ControlKey> bind(ObjectId, Span<const FieldId> = {}, Binding::Kind = Binding::Kind::object);

    Result<ui::ControlKey> bind_operator(OperatorCall);

    Result<ui::ControlKey> bind_material(ObjectId, StringView, Binding::Kind, ObjectId component = {},
                                         StringView slot = {}, std::size_t index = 0);

    Result<Array<ui::MenuItem>> material_menu(const Binding&, const Session&, const Project*,
                                              Array<MaterialChoice>&) const;
};

RegionKind region_of(ui::ControlKey tab) noexcept;

ui::ControlKey region_tab(RegionKind) noexcept; // 0 when the region has no Dock page.
}

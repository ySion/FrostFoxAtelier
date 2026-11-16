#pragma once

#include "editor/session.hpp"
#include "editor/menu_bindings.hpp"
#include "editor/operators.hpp"
#include "editor/keymap.hpp"
#include "editor/control_workbench.hpp"
#include "editor/world_panel.hpp"
#include "editor/property_row.hpp"
#include "editor/asset_browser.hpp"
#include "editor/source_editor.hpp"
#include "editor/output.hpp"
#include "editor/project_code.hpp"
#include "editor/viewport_tool.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"
#include "renderer/view_settings.hpp"
#include "ui_language/live_document.hpp"

namespace atelier::editor {
// Colour editor conversions (sRGB-agnostic: the edited field values are used as-is).
std::array<double, 3> rgb_to_hsv(std::array<double, 3>) noexcept;

std::array<double, 3> hsv_to_rgb(std::array<double, 3>) noexcept;

std::wstring color_hex(std::array<double, 3>);

std::optional<std::array<double, 3>> parse_color_hex(std::wstring_view);

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

inline constexpr ui::ControlKey display_toggle_first = 1015; // Eight bar buttons: 1015..1022, below dynamic bindings.

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
    return key == scene_back || key == entity_back || key == parameter_back            ? -1
           : key == scene_forward || key == entity_forward || key == parameter_forward ? 1
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
            material_texture,
            material_override,
            material_field,
            color_pick,
            saved_data,
            operator_call
        };

        ui::ControlKey key{};

        ObjectId object;

        std::vector<FieldId> path; // Empty for object rows; fields address the actual inspected object.

        Kind kind{};

        ObjectId component{};

        std::string slot{};

        std::size_t index{};

        double color_maximum{}; // color_pick reuses the owning numeric property's range.

        std::string material_path{}; // Stable MS declaration path; index is a vector lane.

        std::unique_ptr<OperatorBinding> call{}; // Retired with this binding, before the document closes.
    };

    Workspace();

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

    std::string material_inspection;

    void reset_layout();

    // Scene Tree drag: the row under the pointer, or World, receives the dragged Entities.
    struct TreeDrop {
        ObjectId parent;

        ui::ControlKey row{};
    };

    std::optional<TreeDrop> tree_drop(const Session&, const ui::UiFrame&, ObjectId dragged, ui::DragPoint) const;

    ui::ControlKey tree_drop_row{}; // Highlighted drop target while a Scene Tree drag is live.

    // Open colour editor: the edited object/fields, the value at open (for revert and the
    // original chip) and where the popup sits. Edits run as one Session group transaction.
    struct ColorEdit {
        ObjectId object;

        std::vector<world::FieldPath> paths;

        std::array<double, 3> original{};

        std::array<double, 3> value{};

        double maximum{1};

        float x{};

        float y{};

        std::wstring hex;

        bool composition_failed{};

        std::optional<material::Override> material_entry; // Keeps vector4 alpha; no fixed material schema.
    };

    std::optional<ColorEdit> color_edit;

    // Inline rename over a tree row: the renamed Entity, drawn once its row is presented.
    // An Entity row (object set) or an Asset Browser cell (asset set); asset paths are project-relative.
    struct RowRename {
        ObjectId object;

        std::wstring text;

        std::filesystem::path asset{};

        ui::TableKey cell{};
    };

    std::optional<RowRename> row_rename;

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

    Result<void> set_tree_expanded(const Session&, EntityTree, std::span<const ObjectId>, bool expanded);

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

    std::uint64_t view_revision() const noexcept { return view_revision_; }

    Result<bool> apply_ui(const ui::Action&);

    std::string_view ui_error() const noexcept {
        return ui_document_ ? ui_document_->diagnostic() : std::string_view{};
    }

    std::optional<ui_language::Statistics> ui_statistics(std::string_view component) const noexcept {
        return ui_document_ ? ui_document_->statistics(component) : std::nullopt;
    }

    void commit_ui_controls();

    bool reject_ui_controls(std::string message);

    // Choose an authoring file before this document's first projection.
    void set_ui_source(std::filesystem::path);

    std::filesystem::path ui_source() const;

    std::uint64_t ui_revision() const noexcept { return ui_document_ ? ui_document_->revision() : 0; }

    std::shared_ptr<ui_language::Component> ui_component(std::string_view name) const {
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
    std::optional<std::size_t> play_pending;

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

    std::optional<ui::ContextMenuRequest> menu;

    std::vector<ui::MenuItem> menu_items; // Same model for menubar, combo, pane and parameter menus.

    bool pie_menu() const noexcept {
        return menu && menu->source == control::viewport &&
               (menu->group == control::viewport_shading_pie || menu->group == control::viewport_axis_pie);
    }

    // ui supplies tree scroll and focus; trees describe only rows near their viewport.
    // Failure rejects the UI candidate; bindings may have changed, so the host must stop this frame.
    Result<std::vector<ui::Control>> describe(Session&, ui_layout::Size, const ui::UiFrame*, Project* = nullptr,
                                              const ui::Session* ui = nullptr,
                                              const ui::ControlBox* command_focus = nullptr);

    void reset_document();

    const Binding* binding(ui::ControlKey) const noexcept;

    ui::ControlKey object_control(ObjectId) const noexcept;

    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;

    ui::ControlKey reset_control(ObjectId) const noexcept;

    ui::ControlKey name_control(ObjectId) const noexcept;

    std::optional<TypeId> component_choice(ui::ControlKey) const noexcept;

    struct MaterialChoice {
        enum class Kind { assign, clear, edit, texture, create_scene, discard };

        Kind kind{};

        ObjectId object;

        std::string slot{};

        std::string key{};

        std::string path{};
    };

    std::optional<MaterialChoice> material_choice(ui::ControlKey) const;

    Selection menu_selection() const noexcept { return menu_selection_; }

private:
    std::unordered_map<std::string, ui::ControlKey> category_keys_;

    PropertyHelpCache property_help_;

    bool parameter_by_category_{};

    std::uint64_t view_revision_{};

    Runtime ui_runtime_; // Editor authoring scopes outlive individual scene documents.

    std::unique_ptr<ui_language::LiveDocument> ui_document_;

    std::filesystem::path ui_source_;

    std::optional<std::string> ui_checkpoint_; // Source only; never keeps another Runtime alive.

    std::vector<Binding> bindings_;

    std::size_t binding_cursor_{};

    ui::ControlKey next_binding_{1024};

    std::array<ui::ControlKey, std::size(menu_bindings)> shelf_items_{};

    std::array<ui::ControlKey, 5> resource_properties_{};

    std::vector<ObjectId> navigation_{ObjectId{}}; // Selection history; never retains scene objects.

    std::size_t navigation_cursor_{};

    // A host-flattened entity tree. Expansion survives docking and hiding, not object retirement.
    struct EntityTreeView {
        std::unordered_map<ObjectId, bool> expansion; // Overrides of the tree's default.

        std::vector<ui::TreeNode> nodes;

        std::vector<ObjectId> objects; // Parallel to nodes.

        std::vector<std::wstring> texts; // Filter text, parallel to nodes while filtering.

        std::vector<ui::TreeRow> rows; // Last flattened rows.

        std::optional<std::pair<ObjectId, ObjectId>> primary; // Last primary row and its parent.

        bool expanded(ObjectId object, bool initial) const {
            const auto at = expansion.find(object);
            return at == expansion.end() ? initial : at->second;
        }

        std::uint32_t node(ObjectId object) const noexcept {
            const auto at = std::ranges::find(objects, object);
            return at == objects.end() ? ui::no_tree_node : static_cast<std::uint32_t>(at - objects.begin());
        }
    };

    EntityTreeView scene_tree_;

    EntityTreeView structure_tree_;

    void build_structure_tree(const Session&, ObjectId entity);

    std::vector<std::uint32_t> window_tree(EntityTreeView&, ObjectId primary, std::wstring_view filter,
                                           ui::TreeViewport, std::span<const ObjectId> pinned);

    std::vector<ComponentType> component_choices_;

    std::vector<MaterialChoice> material_choices_;

    Selection menu_selection_;

    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, Binding::Kind = Binding::Kind::object);

    ui::ControlKey bind_operator(OperatorCall);

    ui::ControlKey bind_material(ObjectId, std::string_view, Binding::Kind, ObjectId component = {},
                                 std::string_view slot = {}, std::size_t index = 0);

    std::vector<ui::MenuItem> material_menu(const Binding&, const Session&, const Project*);
};

RegionKind region_of(ui::ControlKey tab) noexcept;

ui::ControlKey region_tab(RegionKind) noexcept; // 0 when the region has no Dock page.
}

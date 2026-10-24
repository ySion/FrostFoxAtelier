#pragma once

#include "editor/session.hpp"
#include "editor/commands.hpp"
#include "editor/operators.hpp"
#include "editor/control_workbench.hpp"
#include "editor/world_panel.hpp"
#include "editor/property_row.hpp"
#include "editor/asset_browser.hpp"
#include "editor/source_editor.hpp"
#include "editor/project_code.hpp"
#include "editor/viewport_tool.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"
#include "renderer/view_settings.hpp"
#include "ui_language/live_document.hpp"

namespace atelier::editor {
// Colour editor conversions (sRGB-agnostic: the edited field values are used as-is).
std::array<double,3> rgb_to_hsv(std::array<double,3>) noexcept;
std::array<double,3> hsv_to_rgb(std::array<double,3>) noexcept;
std::wstring color_hex(std::array<double,3>);
std::optional<std::array<double,3>> parse_color_hex(std::wstring_view);
namespace control {
constexpr ui::ControlKey command_key(Command command) { return static_cast<ui::ControlKey>(command); }
inline constexpr auto add_entity=command_key(Command::create_entity), remove_selected=command_key(Command::erase_selected),
    undo=command_key(Command::undo), redo=command_key(Command::redo), focus=command_key(Command::focus), reset=command_key(Command::reset_camera),
    add_static_mesh=command_key(Command::create_static_mesh);
inline constexpr ui::ControlKey workbench = 18, position_fold = 57;
inline constexpr ui::ControlKey viewport = 40;
inline constexpr ui::ControlKey examples_menu = 840;
inline constexpr ui::ControlKey viewport_pivot = 845, viewport_modes=846;
inline constexpr ui::ControlKey viewport_snap=847, viewport_camera=848, viewport_tools_menu=849, viewport_hud=850, viewport_readout=851, viewport_rail_snap=852, viewport_add_menu=853;
inline constexpr ui::ControlKey viewport_delete_menu=855, viewport_shading_pie=856, shading_preset_first=890;
inline constexpr ui::ControlKey viewport_axis_pie=857, viewport_cavity=858, viewport_pie=859;
inline constexpr ui::ControlKey cavity_enabled=910, cavity_group_first=911, cavity_value_first=920;
inline constexpr ui::ControlKey viewport_fps=904, viewport_fly_reticle=860, viewport_hint=861;
// Blender header menus and grouped header widgets.
inline constexpr ui::ControlKey viewport_view_menu=974, viewport_select_menu=975, viewport_object_menu=976, viewport_header_add=977;
inline constexpr ui::ControlKey viewport_shading_modes=948, viewport_select_modes=949, viewport_snapping=978;
inline constexpr ui::ControlKey object_clear_menu=979, object_snap_menu=980, view_viewpoint_menu=995, select_mode_menu=996, viewport_orientation=998;
// Houdini's right-hand display bar: view toggles that never touch scene history.
inline constexpr ui::ControlKey display_bar=1006, display_grid=1007, display_axes=1008, display_wire=1009,
    display_triangles=1010, display_cavity=1011, display_fps=1012, display_wire_only=1013, display_lit=1014;
inline constexpr ui::ControlKey display_toggle_first=1015; // Eight bar buttons: 1015..1022, below dynamic bindings.
inline constexpr ui::ControlKey inline_rename=1023;
// Colour editor popup and its fields (fixed keys; one editor at a time).
inline constexpr ui::ControlKey color_editor=940, color_rgb=941, color_hsv=944, color_hex=953, color_revert=954, color_done=955, color_group=956; // rgb 941..943, hsv 944..946
inline constexpr ui::ControlKey scene_tab=600, entities_tab=601, parameters_tab=602, reset_layout=command_key(Command::reset_layout);
inline constexpr ui::ControlKey assets_tab=604, assets_content=650;
inline constexpr ui::ControlKey output_tab=607,output_content=690,output_table=691;
inline constexpr ui::ControlKey structure_tab=608,structure_content=694,structure_tree=695,structure_filter=696;
inline constexpr ui::ControlKey shelf=730, shelf_tabs=731, shelf_create=732, shelf_view=733, shelf_body=734, shelf_handle=735, shelf_contents=736;
inline constexpr ui::ControlKey scene_content=640;
inline constexpr ui::ControlKey scene_shading=646;
inline constexpr ui::ControlKey shading_first=760, edges_first=763, topology_first=767;
inline constexpr ui::ControlKey scene_guides=645, guide_background=769, guide_grid=770, guide_axes=771;
inline constexpr ui::ControlKey shading_scene_lit=772;
inline constexpr ui::ControlKey scene_path=642, scene_back=643, scene_forward=644;
inline constexpr ui::ControlKey entity_path=22, entity_back=25, entity_forward=26;
inline constexpr ui::ControlKey entity_tree=23, tree_world=27, tree_filter=28;
inline constexpr ui::ControlKey parameter_path=76, parameter_back=78, parameter_forward=79, world_context=720;
constexpr bool is_path(ui::ControlKey key) { return key==scene_path || key==entity_path || key==parameter_path; }
constexpr int navigation_direction(ui::ControlKey key) {
    return key==scene_back || key==entity_back || key==parameter_back?-1:
        key==scene_forward || key==entity_forward || key==parameter_forward?1:0;
}
inline constexpr ui::ControlKey parameter_search = 54, parameter_search_toggle=86;
inline constexpr ui::ControlKey add_component=87,component_choice_first=1ULL<<40,property_choice_first=1ULL<<39;
inline constexpr ui::ControlKey parameter_list=84, group_components=1ULL<<42, group_categories=group_components+1;
inline constexpr ui::ControlKey parameter_menu=500, menu_group_reset=501, menu_reset=502, menu_copy=503, menu_paste=504;
inline constexpr auto file_menu=menu_key(CommandGroup::file), edit_menu=menu_key(CommandGroup::edit),
    create_menu=menu_key(CommandGroup::create), view_menu=menu_key(CommandGroup::view),
    window_menu=menu_key(CommandGroup::window), help_menu=menu_key(CommandGroup::help);
inline constexpr auto exit=command_key(Command::exit), show_scene=command_key(Command::show_scene),
    show_parameters=command_key(Command::show_parameters), show_controls=command_key(Command::show_controls),
    show_entities=command_key(Command::show_entities), maximize_pane=command_key(Command::maximize_pane);
inline constexpr ui::ControlKey menu_bar_handle=715, menu_bar_body=716;
constexpr bool is_menu(ui::ControlKey key) { return command_group(key)!=nullptr; }
constexpr ui::ControlKey command_menu(ui::ControlKey key) {
    const auto* definition=command_definition(key);
    return definition?menu_key(definition->group):0;
}
}
struct Workspace {
    struct Binding {
        enum class Kind { object, structure, property_group, field, slider, rotation, property_row, position_default, rotation_default, scale_default, name, navigation,
            material_slot, material_texture, material_override, color_pick, saved_data };
        ui::ControlKey key{};
        ObjectId object;
        std::vector<FieldId> path; // Empty for object rows; fields address the actual inspected object.
        Kind kind{};
        ObjectId component{};
        std::string slot{};
        std::size_t index{};
        double color_maximum{}; // color_pick reuses the owning numeric property's range.
    };
    Workspace();
    Workspace(const Workspace&)=delete;
    Workspace& operator=(const Workspace&)=delete;
    Workspace(Workspace&&)=delete; // Live scopes borrow this workspace's key authority.
    Workspace& operator=(Workspace&&)=delete;
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
    struct TreeDrop { ObjectId parent; ui::ControlKey row{}; };
    std::optional<TreeDrop> tree_drop(const Session&,const ui::UiFrame&,ObjectId dragged,ui::DragPoint) const;
    ui::ControlKey tree_drop_row{}; // Highlighted drop target while a Scene Tree drag is live.
    // Open colour editor: the edited object/fields, the value at open (for revert and the
    // original chip) and where the popup sits. Edits run as one Session group transaction.
    struct ColorEdit {
        ObjectId object;
        std::vector<world::FieldPath> paths;
        std::array<double,3> original{}, value{};
        double maximum{1};
        float x{}, y{};
        std::wstring hex;
        bool composition_failed{};
    };
    std::optional<ColorEdit> color_edit;
    // Inline rename over a tree row: the renamed Entity, drawn once its row is presented.
    // An Entity row (object set) or an Asset Browser cell (asset set); asset paths are project-relative.
    struct RowRename { ObjectId object; std::wstring text; std::filesystem::path asset{}; ui::TableKey cell{}; };
    std::optional<RowRename> row_rename;
    bool show_workbench() const noexcept { return dock.visible(control::workbench); }
    bool sample_history() const noexcept { return dock.focused_tab()==control::workbench; }
    bool can_navigate(int direction) const noexcept;
    Result<void> navigate(Session&,int direction);
    Result<void> select_tree(Session&,ui::ControlKey,ui::SelectionModifiers = {});
    // Tree row expansion is view state: not undoable, never changes the document
    // or the selection. Scene Tree addresses its World row as ObjectId{}. If any
    // identity is not a row of the tree, nothing changes.
    enum class EntityTree { scene_tree, structure };
    Result<void> set_tree_expanded(const Session&,EntityTree,std::span<const ObjectId>,bool expanded);
    // Expands the Entity's Scene Tree ancestors; false when the tree filter hides its row.
    Result<bool> reveal_in_tree(const Session&,ObjectId entity);
    // Commands served by an operator answer with its poll.
    bool command_enabled(Command,Session&,Project* = nullptr) const;
    // Built from the focused Dock page until the keymap step; UI calls are interactive.
    Context operator_context(Session&,Project*,bool interactive) const;
    Result<bool> apply_view(Session&,const ui::Action&,std::array<float,3> focus = {});
    // Copies the World's environment into the view's author settings; true when they changed.
    bool sync_world(const Session&);
    Result<void> set_view_settings(Session&,const renderer::ViewSettings&,std::array<float,3> focus = {});
    std::uint64_t view_revision() const noexcept {return view_revision_;}
    Result<bool> apply_ui(const ui::Action&);
    std::string_view ui_error() const noexcept {return ui_document_?ui_document_->diagnostic():std::string_view{};}
    std::optional<ui_language::Statistics> ui_statistics(std::string_view component) const noexcept {
        return ui_document_?ui_document_->statistics(component):std::nullopt;
    }
    void commit_ui_controls();
    bool reject_ui_controls(std::string message);
    // Choose an authoring file before this document's first projection.
    void set_ui_source(std::filesystem::path);
    std::filesystem::path ui_source() const;
    std::uint64_t ui_revision() const noexcept {return ui_document_?ui_document_->revision():0;}
    std::shared_ptr<ui_language::Component> ui_component(std::string_view name) const {
        return ui_document_?ui_document_->component(name):nullptr;
    }
    renderer::ViewSettings view{.shading=renderer::Shading::matcap,
        .background=renderer::Background::gradient,.reference_grid=true,.origin_axes=true};
    ui::ControlKey shelf_page{control::shelf_create};
    AssetBrowser assets;
    SourceEditor source_editor;
    std::shared_ptr<const ProjectBuild> project_build;
    void log(std::wstring);
    // Repeats of the latest line update it in place with a count, so a failure that
    // resolves and recurs (a gesture through zero scale) does not flood the output.
    void log_repeated(std::wstring);
    std::shared_ptr<const ui::TableData> output;
    bool source_history() const noexcept {return dock.focused_tab()==source_control::tab;}
    bool project_open{};
    bool scene_available{true},project_running{},project_stopping{},project_ui_configured{};
    // While a Play run follows the document: change batches it has not acknowledged yet.
    std::optional<std::size_t> play_pending;
    bool play_diverged{}; // Play stopped following the document until it restarts.
    bool menu_bar_collapsed{}, shelf_collapsed{};
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
    bool pie_menu() const noexcept { return menu && menu->source==control::viewport &&
        (menu->group==control::viewport_shading_pie || menu->group==control::viewport_axis_pie); }
    // ui supplies tree scroll and focus; trees describe only rows near their viewport.
    std::vector<ui::Control> describe(Session&, ui_layout::Size, const ui::UiFrame*,Project* = nullptr,const ui::Session* ui = nullptr);
    void reset_document();
    const Binding* binding(ui::ControlKey) const noexcept;
    ui::ControlKey object_control(ObjectId) const noexcept;
    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;
    ui::ControlKey reset_control(ObjectId) const noexcept;
    ui::ControlKey name_control(ObjectId) const noexcept;
    std::optional<TypeId> component_choice(ui::ControlKey) const noexcept;
    struct PropertyChoice { ObjectId object; std::vector<FieldId> path; EnumValue value; ui::ControlKey field; };
    std::optional<PropertyChoice> property_choice(ui::ControlKey) const;
    struct MaterialChoice {enum class Kind {assign,clear,edit,texture};Kind kind{};ObjectId object;std::string slot,key;std::size_t index{};};
    std::optional<MaterialChoice> material_choice(ui::ControlKey) const;
    Selection menu_selection() const noexcept {return menu_selection_;}
private:
    std::unordered_map<std::string,ui::ControlKey> category_keys_;
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
    std::array<ui::ControlKey,command_definitions.size()> shelf_items_{}; // Visual item identities, separate from command IDs.
    std::array<ui::ControlKey,5> resource_properties_{};
    std::vector<ObjectId> navigation_{ObjectId{}}; // Selection history; never retains scene objects.
    std::size_t navigation_cursor_{};
    // A host-flattened entity tree. Expansion survives docking and hiding, not object retirement.
    struct EntityTreeView {
        std::unordered_map<ObjectId,bool> expansion; // Overrides of the tree's default.
        std::vector<ui::TreeNode> nodes;
        std::vector<ObjectId> objects; // Parallel to nodes.
        std::vector<std::wstring> texts; // Filter text, parallel to nodes while filtering.
        std::vector<ui::TreeRow> rows; // Last flattened rows.
        std::optional<std::pair<ObjectId,ObjectId>> primary; // Last primary row and its parent.
        bool expanded(ObjectId object,bool initial) const {
            const auto at=expansion.find(object);return at==expansion.end()?initial:at->second;
        }
        std::uint32_t node(ObjectId object) const noexcept {
            const auto at=std::ranges::find(objects,object);
            return at==objects.end()?ui::no_tree_node:static_cast<std::uint32_t>(at-objects.begin());
        }
    };
    EntityTreeView scene_tree_, structure_tree_;
    void build_structure_tree(const Session&,ObjectId entity);
    std::vector<std::uint32_t> window_tree(EntityTreeView&,ObjectId primary,std::wstring_view filter,
        ui::TreeViewport,std::span<const ObjectId> pinned);
    ui::ControlKey next_output_{1};
    std::wstring repeated_line_;
    ui::ControlKey repeated_row_{};
    std::uint64_t repeated_count_{};
    std::vector<ComponentType> component_choices_;
    std::vector<PropertyChoice> property_choices_;
    std::vector<MaterialChoice> material_choices_;
    Selection menu_selection_;
    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, Binding::Kind = Binding::Kind::object);
    ui::ControlKey bind_material(ObjectId,std::span<const FieldId>,Binding::Kind,ObjectId component={},std::string_view slot={},std::size_t index=0);
    std::vector<ui::MenuItem> material_menu(const Binding&,const Session&,const Project*);
};
// The operator call serving a command, or none; menus and keys call operators directly in later O1 steps.
std::optional<OperatorCall> operator_for(Command);
RegionKind region_of(ui::ControlKey tab) noexcept;
ui::ControlKey region_tab(RegionKind) noexcept; // 0 when the region has no Dock page.
}

#pragma once

#include "editor/session.hpp"
#include "editor/commands.hpp"
#include "editor/control_workbench.hpp"
#include "editor/asset_browser.hpp"
#include "editor/source_editor.hpp"
#include "editor/project_code.hpp"
#include "editor/viewport_tool.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"
#include "renderer/view_settings.hpp"
#include "ui_language/live_document.hpp"

namespace atelier::editor {
namespace control {
constexpr ui::ControlKey command_key(Command command) { return static_cast<ui::ControlKey>(command); }
inline constexpr auto add_actor=command_key(Command::create_actor), remove_selected=command_key(Command::erase_selected),
    undo=command_key(Command::undo), redo=command_key(Command::redo), focus=command_key(Command::focus), reset=command_key(Command::reset_camera),
    add_static_mesh=command_key(Command::create_static_mesh);
inline constexpr ui::ControlKey workbench = 18, position_fold = 57;
inline constexpr ui::ControlKey viewport = 40;
inline constexpr ui::ControlKey examples_menu = 840;
inline constexpr ui::ControlKey viewport_pivot = 845, viewport_modes=846;
inline constexpr ui::ControlKey viewport_snap=847, viewport_camera=848, viewport_tools_menu=849, viewport_hud=850, viewport_readout=851, viewport_rail_snap=852, viewport_add_menu=853, viewport_snap_menu=854;
inline constexpr ui::ControlKey scene_tab=600, actors_tab=601, parameters_tab=602, reset_layout=command_key(Command::reset_layout);
inline constexpr ui::ControlKey assets_tab=604, assets_content=650;
inline constexpr ui::ControlKey output_tab=607,output_content=690,output_table=691;
inline constexpr ui::ControlKey structure_tab=608,structure_content=694,structure_tree=695,structure_filter=696;
inline constexpr ui::ControlKey shelf=730, shelf_tabs=731, shelf_create=732, shelf_view=733, shelf_body=734, shelf_handle=735, shelf_contents=736;
inline constexpr ui::ControlKey scene_content=640;
inline constexpr ui::ControlKey scene_shading=646;
inline constexpr ui::ControlKey shading_first=760, edges_first=763, topology_first=767;
inline constexpr ui::ControlKey scene_guides=645, guide_background=769, guide_grid=770, guide_axes=771;
inline constexpr ui::ControlKey shading_scene_lit=772;
inline constexpr ui::ControlKey lit_menu=860,lit_effects_first=861,lit_exposure_first=867,lit_exposure_menu=872;
inline constexpr ui::ControlKey lit_shadow_budget_menu=873,lit_shadow_budget_first=874;
inline constexpr ui::ControlKey lit_aa_menu=877,lit_aa_first=878;
inline constexpr ui::ControlKey scene_path=642, scene_back=643, scene_forward=644;
inline constexpr ui::ControlKey actor_path=22, actor_back=25, actor_forward=26;
inline constexpr ui::ControlKey actor_tree=23, tree_world=27, tree_filter=28;
inline constexpr ui::ControlKey parameter_path=76, parameter_back=78, parameter_forward=79, world_context=720;
constexpr bool is_path(ui::ControlKey key) { return key==scene_path || key==actor_path || key==parameter_path; }
constexpr int navigation_direction(ui::ControlKey key) {
    return key==scene_back || key==actor_back || key==parameter_back?-1:
        key==scene_forward || key==actor_forward || key==parameter_forward?1:0;
}
inline constexpr ui::ControlKey parameter_search = 54, parameter_search_toggle=86;
inline constexpr ui::ControlKey add_component=87,component_choice_first=1ULL<<40,property_choice_first=1ULL<<39;
inline constexpr ui::ControlKey parameter_list=84;
inline constexpr ui::ControlKey parameter_menu=500, menu_group_reset=501, menu_reset=502, menu_copy=503, menu_paste=504;
inline constexpr auto file_menu=menu_key(CommandGroup::file), edit_menu=menu_key(CommandGroup::edit),
    create_menu=menu_key(CommandGroup::create), view_menu=menu_key(CommandGroup::view),
    window_menu=menu_key(CommandGroup::window), help_menu=menu_key(CommandGroup::help);
inline constexpr auto exit=command_key(Command::exit), show_scene=command_key(Command::show_scene),
    show_parameters=command_key(Command::show_parameters), show_controls=command_key(Command::show_controls),
    show_actors=command_key(Command::show_actors), maximize_pane=command_key(Command::maximize_pane);
inline constexpr ui::ControlKey menu_bar_handle=715, menu_bar_body=716;
constexpr bool is_menu(ui::ControlKey key) { return command_group(key)!=nullptr; }
constexpr ui::ControlKey command_menu(ui::ControlKey key) {
    const auto* definition=command_definition(key);
    return definition?menu_key(definition->group):0;
}
}
struct Workspace {
    struct Binding {
        enum class Kind { object, structure, property_group, field, rotation, property_row, position_default, rotation_default, scale_default, name, navigation };
        ui::ControlKey key{};
        ObjectId object;
        std::vector<FieldId> path; // Empty for object rows; fields address the actual inspected object.
        Kind kind{};
    };
    Workspace();
    Workspace(const Workspace&)=delete;
    Workspace& operator=(const Workspace&)=delete;
    Workspace(Workspace&&)=delete; // Live scopes borrow this workspace's key authority.
    Workspace& operator=(Workspace&&)=delete;
    ui::DockSpace dock;
    ViewportTool::Settings viewport_settings;
    TransformTool viewport_mode{TransformTool::transform}; // Current operation, including a modal gesture.
    std::wstring viewport_readout; // Modal G/R/S header text; empty hides the band.
    CameraView camera_view{CameraView::perspective};
    bool viewport_options_collapsed{};
    bool inspecting_asset{}; // Inspect the browser's persistent resource identity, not an Actor instance.
    void reset_layout();
    bool show_workbench() const noexcept { return dock.visible(control::workbench); }
    bool sample_history() const noexcept { return dock.focused_tab()==control::workbench; }
    bool can_navigate(int direction) const noexcept;
    Result<void> navigate(Session&,int direction);
    Result<void> select_tree(Session&,ui::ControlKey,ui::SelectionModifiers = {},const ui::UiFrame* = nullptr);
    bool command_enabled(Command,const Session&) const;
    bool apply_view(const ui::Action&);
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
    double ui_compile_milliseconds() const noexcept {return ui_document_?ui_document_->compile_milliseconds():0;}
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
    std::shared_ptr<const ui::TableData> output;
    bool source_history() const noexcept {return dock.focused_tab()==source_control::tab;}
    bool project_open{};
    bool scene_available{true},project_running{},project_stopping{},project_ui_configured{};
    bool menu_bar_collapsed{}, shelf_collapsed{};
    bool parameter_search_visible{};
    ControlWorkbench workbench;
    std::wstring status{L"Ready"};
    std::wstring query;
    std::wstring render_notice; // Derived from the last accepted renderer frame.
    std::wstring tree_query;
    std::wstring structure_query;
    std::optional<ui::ContextMenuRequest> menu;
    std::vector<ui::MenuItem> menu_items; // Same model for menubar, combo, pane and parameter menus.
    std::vector<ui::Control> describe(const Session&, ui_layout::Size, const ui::UiFrame*,const Project* = nullptr);
    void reset_document();
    const Binding* binding(ui::ControlKey) const noexcept;
    ui::ControlKey object_control(ObjectId) const noexcept;
    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;
    ui::ControlKey reset_control(ObjectId) const noexcept;
    ui::ControlKey name_control(ObjectId) const noexcept;
    std::optional<TypeId> component_choice(ui::ControlKey) const noexcept;
    struct PropertyChoice { ObjectId object; std::vector<FieldId> path; EnumValue value; ui::ControlKey field; };
    std::optional<PropertyChoice> property_choice(ui::ControlKey) const;
    Selection menu_selection() const noexcept {return menu_selection_;}
private:
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
    std::unordered_map<ui::ControlKey,bool> tree_expansion_; // Accepted view state survives docking/hiding, not object retirement.
    std::uint64_t tree_frame_{}; // Consume each accepted snapshot once, including across hidden frames.
    ui::ControlKey next_output_{1};
    std::vector<ComponentType> component_choices_;
    std::vector<PropertyChoice> property_choices_;
    Selection menu_selection_;
    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, Binding::Kind = Binding::Kind::object);
};
}

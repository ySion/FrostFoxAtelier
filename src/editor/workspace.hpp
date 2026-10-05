#pragma once

#include "editor/session.hpp"
#include "editor/commands.hpp"
#include "editor/control_workbench.hpp"
#include "editor/asset_browser.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"
#include "renderer/view_settings.hpp"
#include "ui_language/live_document.hpp"

namespace atelier::editor {
namespace control {
constexpr ui::ControlKey command_key(Command command) { return static_cast<ui::ControlKey>(command); }
inline constexpr auto add_actor=command_key(Command::create_actor), remove_actor=command_key(Command::erase_actor),
    undo=command_key(Command::undo), redo=command_key(Command::redo), focus=command_key(Command::focus), reset=command_key(Command::reset_camera),
    add_static_mesh=command_key(Command::create_static_mesh);
inline constexpr ui::ControlKey workbench = 18, position_fold = 57;
inline constexpr ui::ControlKey viewport = 40;
inline constexpr ui::ControlKey scene_tab=600, actors_tab=601, parameters_tab=602, reset_layout=command_key(Command::reset_layout);
inline constexpr ui::ControlKey assets_tab=604, assets_content=650;
inline constexpr ui::ControlKey shelf=730, shelf_tabs=731, shelf_create=732, shelf_view=733, shelf_body=734, shelf_handle=735, shelf_contents=736;
inline constexpr ui::ControlKey scene_content=640;
inline constexpr ui::ControlKey scene_shading=646, scene_edges=647, scene_topology=648, scene_display_bar=649;
inline constexpr ui::ControlKey shading_first=760, edges_first=763, topology_first=767;
inline constexpr ui::ControlKey scene_guides=645, guide_background=769, guide_grid=770, guide_axes=771;
inline constexpr ui::ControlKey scene_path=642, scene_back=643, scene_forward=644;
inline constexpr ui::ControlKey actor_path=22, actor_back=25, actor_forward=26;
inline constexpr ui::ControlKey actor_tree=23, tree_world=27, tree_filter=28;
inline constexpr ui::ControlKey parameter_path=76, parameter_back=78, parameter_forward=79, world_context=720;
constexpr bool is_path(ui::ControlKey key) { return key==scene_path || key==actor_path || key==parameter_path; }
constexpr int navigation_direction(ui::ControlKey key) {
    return key==scene_back || key==actor_back || key==parameter_back?-1:
        key==scene_forward || key==actor_forward || key==parameter_forward?1:0;
}
inline constexpr ui::ControlKey parameter_search = 54;
inline constexpr ui::ControlKey parameter_transform=81, parameter_components=82, parameter_pages=83, parameter_list=84;
inline constexpr ui::ControlKey empty_field_base = 200;
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
        enum class Kind { object, field, position_default, name, navigation };
        ui::ControlKey key{};
        ObjectId object;
        std::vector<FieldId> path; // Empty for an Actor row; otherwise an actual component field.
        Kind kind{};
    };
    Workspace();
    Workspace(const Workspace&)=delete;
    Workspace& operator=(const Workspace&)=delete;
    Workspace(Workspace&&)=delete; // Live scopes borrow this workspace's key authority.
    Workspace& operator=(Workspace&&)=delete;
    ui::DockSpace dock;
    void reset_layout();
    bool show_workbench() const noexcept { return dock.visible(control::workbench); }
    bool sample_history() const noexcept { return dock.focused_tab()==control::workbench; }
    bool can_navigate(int direction) const noexcept;
    Result<void> navigate(Session&,int direction);
    Result<void> select_tree(Session&,ui::ControlKey);
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
    renderer::ViewSettings view{.shading=renderer::Shading::matcap,
        .background=renderer::Background::gradient,.reference_grid=true,.origin_axes=true};
    ui::ControlKey shelf_page{control::shelf_create};
    AssetBrowser assets;
    bool project_open{};
    ui::ControlKey parameter_page{control::parameter_transform};
    bool menu_bar_collapsed{}, shelf_collapsed{};
    ControlWorkbench workbench;
    std::wstring status{L"Ready"};
    std::wstring query;
    std::wstring tree_query;
    std::optional<ui::ContextMenuRequest> menu;
    std::vector<ui::MenuItem> menu_items; // Same model for menubar, combo, pane and parameter menus.
    std::vector<ui::Control> describe(const Session&, ui_layout::Size, const ui::UiFrame*,const Project* = nullptr);
    void reset_document();
    const Binding* binding(ui::ControlKey) const noexcept;
    ui::ControlKey object_control(ObjectId) const noexcept;
    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;
    ui::ControlKey reset_control(ObjectId) const noexcept;
    ui::ControlKey name_control(ObjectId) const noexcept;
private:
    std::unique_ptr<ui_language::LiveDocument> ui_document_;
    std::filesystem::path ui_source_;
    std::vector<Binding> bindings_;
    std::size_t binding_cursor_{};
    ui::ControlKey next_binding_{1024};
    std::vector<ObjectId> navigation_{ObjectId{}}; // Selection history; never retains scene objects.
    std::size_t navigation_cursor_{};
    std::unordered_map<ui::ControlKey,bool> tree_expansion_; // Accepted view state survives docking/hiding, not object retirement.
    std::uint64_t tree_frame_{}; // Consume each accepted snapshot once, including across hidden frames.
    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, Binding::Kind = Binding::Kind::object);
};
}

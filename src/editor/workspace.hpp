#pragma once

#include "editor/session.hpp"
#include "editor/control_workbench.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"

namespace atelier::editor {
namespace control {
inline constexpr ui::ControlKey add_actor = 10, remove_actor = 11, undo = 12, redo = 13, focus = 14, reset = 15;
inline constexpr ui::ControlKey add_scene_component = 16, add_static_mesh = 17;
inline constexpr ui::ControlKey workbench = 18, position_fold = 57;
inline constexpr ui::ControlKey viewport = 40;
inline constexpr ui::ControlKey scene_tab=600, actors_tab=601, parameters_tab=602, reset_layout=603;
inline constexpr ui::ControlKey scene_content=640;
inline constexpr ui::ControlKey scene_path=642, scene_back=643, scene_forward=644;
inline constexpr ui::ControlKey actor_path=22, actor_back=25, actor_forward=26;
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
inline constexpr ui::ControlKey file_menu=700, edit_menu=701, create_menu=702, view_menu=703, window_menu=704;
inline constexpr ui::ControlKey exit=705, show_scene=710, show_parameters=711, show_controls=712, show_actors=713, maximize_pane=714;
inline constexpr ui::ControlKey menu_bar_handle=715, menu_bar_body=716;
constexpr bool is_menu(ui::ControlKey key) { return key>=file_menu && key<=window_menu; }
constexpr ui::ControlKey command_menu(ui::ControlKey key) {
    if (key==exit) return file_menu;
    if (key==undo || key==redo || key==remove_actor) return edit_menu;
    if (key==add_actor || key==add_scene_component || key==add_static_mesh) return create_menu;
    if (key==focus || key==reset) return view_menu;
    if (key==reset_layout || (key>=show_scene && key<=maximize_pane)) return window_menu;
    return 0;
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
    ui::DockSpace dock;
    void reset_layout();
    bool show_workbench() const noexcept { return dock.visible(control::workbench); }
    bool sample_history() const noexcept { return dock.focused_tab()==control::workbench; }
    bool can_navigate(int direction) const noexcept;
    Result<void> navigate(Session&,int direction);
    bool position_expanded{true};
    ui::ControlKey parameter_page{control::parameter_transform};
    bool menu_bar_collapsed{};
    ControlWorkbench workbench;
    std::wstring status{L"Ready"};
    std::wstring query;
    std::optional<ui::ContextMenuRequest> menu;
    std::vector<ui::MenuItem> menu_items; // Same model for menubar, combo, pane and parameter menus.
    std::vector<ui::Control> describe(const Session&, ui_layout::Size, const ui::UiFrame*);
    const Binding* binding(ui::ControlKey) const noexcept;
    ui::ControlKey object_control(ObjectId) const noexcept;
    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;
    ui::ControlKey reset_control(ObjectId) const noexcept;
    ui::ControlKey name_control(ObjectId) const noexcept;
private:
    std::vector<Binding> bindings_;
    std::size_t binding_cursor_{};
    ui::ControlKey next_binding_{1024};
    std::vector<ObjectId> navigation_{ObjectId{}}; // Selection history; never retains scene objects.
    std::size_t navigation_cursor_{};
    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, Binding::Kind = Binding::Kind::object);
};
}

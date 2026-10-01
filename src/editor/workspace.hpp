#pragma once

#include "editor/session.hpp"
#include "editor/control_workbench.hpp"
#include "ui/session.hpp"
#include "ui/docking.hpp"

namespace atelier::editor {
namespace control {
inline constexpr ui::ControlKey add_actor = 10, remove_actor = 11, undo = 12, redo = 13, focus = 14, reset = 15;
inline constexpr ui::ControlKey add_transform = 16, add_cube = 17;
inline constexpr ui::ControlKey workbench = 18, position_fold = 57;
inline constexpr ui::ControlKey viewport = 40;
inline constexpr ui::ControlKey scene_tab=600, actors_tab=601, parameters_tab=602, reset_layout=603;
inline constexpr ui::ControlKey scene_content=640;
inline constexpr ui::ControlKey parameter_search = 54;
inline constexpr ui::ControlKey empty_field_base = 200;
inline constexpr ui::ControlKey parameter_menu=500, menu_group_reset=501, menu_reset=502, menu_copy=503, menu_paste=504;
}
struct Workspace {
    struct Binding {
        ui::ControlKey key{};
        ObjectId object;
        std::vector<FieldId> path; // Empty for an Actor row; otherwise an actual component field.
        bool reset_position{};
    };
    Workspace();
    ui::DockSpace dock;
    void reset_layout();
    bool show_workbench() const noexcept { return dock.visible(control::workbench); }
    bool sample_history() const noexcept { return dock.focused_tab()==control::workbench; }
    bool position_expanded{true};
    ControlWorkbench workbench;
    std::wstring status{L"Ready"};
    std::wstring query;
    std::optional<ui::ContextMenuRequest> menu;
    std::vector<ui::Control> describe(const Session&, ui_layout::Size, const ui::UiFrame*);
    const Binding* binding(ui::ControlKey) const noexcept;
    ui::ControlKey object_control(ObjectId) const noexcept;
    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;
    ui::ControlKey reset_control(ObjectId) const noexcept;
private:
    std::vector<Binding> bindings_;
    std::size_t binding_cursor_{};
    ui::ControlKey next_binding_{1024};
    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, bool reset_position = false);
};
}

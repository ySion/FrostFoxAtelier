#pragma once

#include "editor/session.hpp"
#include "editor/control_workbench.hpp"
#include "ui/session.hpp"

namespace atelier::editor {
namespace control {
inline constexpr ui::ControlKey add_actor = 10, remove_actor = 11, undo = 12, redo = 13, focus = 14, reset = 15;
inline constexpr ui::ControlKey add_transform = 16, add_cube = 17;
inline constexpr ui::ControlKey workbench = 18, position_fold = 57;
inline constexpr ui::ControlKey viewport = 40, splitter = 41;
inline constexpr ui::ControlKey parameter_search = 54;
inline constexpr ui::ControlKey empty_field_base = 200;
}
struct Workspace {
    struct Binding {
        ui::ControlKey key{};
        ObjectId object;
        std::vector<FieldId> path; // Empty for an Actor row; otherwise an actual component field.
        bool reset_position{};
    };
    float parameter_width{360};
    bool show_workbench{}, position_expanded{true};
    ControlWorkbench workbench;
    std::wstring status{L"Ready"};
    std::wstring query;
    std::vector<ui::Control> describe(const Session&);
    const Binding* binding(ui::ControlKey) const noexcept;
    ui::ControlKey object_control(ObjectId) const noexcept;
    ui::ControlKey field_control(ObjectId, std::span<const FieldId>) const noexcept;
    ui::ControlKey reset_control(ObjectId) const noexcept;
private:
    std::vector<Binding> bindings_;
    std::size_t binding_count_{};
    ui::ControlKey next_binding_{1024};
    ui::ControlKey bind(ObjectId, std::span<const FieldId> = {}, bool reset_position = false);
};
}

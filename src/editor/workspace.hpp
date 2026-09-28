#pragma once

#include "editor/session.hpp"
#include "ui/session.hpp"

namespace atelier::editor {
namespace control {
inline constexpr ui::ControlKey add = 10, remove = 11, undo = 12, redo = 13, focus = 14, reset = 15;
inline constexpr ui::ControlKey viewport = 40, splitter = 41;
inline constexpr ui::ControlKey first_object = 1000;
inline constexpr ui::ControlKey position_base = 1ull << 48;
constexpr ui::ControlKey position(scene::ObjectKey key, std::size_t axis) { return position_base + key * 4 + axis; }
}
struct Workspace {
    float parameter_width{280};
    std::wstring status{L"Ready"};
    std::vector<ui::Control> describe(const Session&) const;
};
}

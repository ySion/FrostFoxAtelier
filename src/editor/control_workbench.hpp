#pragma once
#include "ui/controls.hpp"
#include <array>

namespace atelier::editor {
namespace workbench_control {
inline constexpr ui::ControlKey disable=310, long_labels=311, list=319, fold=320, body=321;
inline constexpr ui::ControlKey position_reset=334, position_x=340, position_y=341, position_z=342;
inline constexpr ui::ControlKey scalar_reset=354, scalar=355, name=361, enabled=362, reset=370;
inline constexpr ui::ControlKey vector4_reset=384, vector4_x=400, vector4_y=401, vector4_z=402, vector4_w=403;
}

// A live consumer of the production controls, inside the ordinary editor host.
// Its sample values/history never write World or participate in scene history.
class ControlWorkbench {
public:
    struct Values {
        std::array<double,3> position{};
        std::array<double,4> vector4{0,0,0,1};
        double exposure{1};
        std::wstring name{L"Control sample"};
        bool enabled{true};
        bool operator==(const Values&) const = default;
    };
    const Values& values() const noexcept { return values_; }
    std::size_t undo_count() const noexcept { return cursor_; }
    std::size_t redo_count() const noexcept { return history_.size()-cursor_-1; }
    void append(std::vector<ui::Control>&, ui::ControlKey parent) const;
    bool apply(const ui::Action&);
    void undo();
    void redo();
private:
    Values values_;
    std::vector<Values> history_{values_};
    std::size_t cursor_{};
    std::optional<std::pair<ui::ControlKey,Values>> active_;
    bool disabled_{}, long_labels_{}, expanded_{true};
    void remember();
};
} // namespace atelier::editor

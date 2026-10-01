#pragma once
#include "ui/controls.hpp"
#include <array>

namespace atelier::editor {
namespace workbench_control {
inline constexpr ui::ControlKey disable=310, long_labels=311, list=319, fold=320, body=321;
inline constexpr ui::ControlKey position_reset=334, position_x=340, position_y=341, position_z=342;
inline constexpr ui::ControlKey scalar_reset=354, scalar=355, name=361, enabled=362, reset=370;
inline constexpr ui::ControlKey vector4_reset=384, vector4_x=400, vector4_y=401, vector4_z=402, vector4_w=403;
inline constexpr ui::ControlKey mode=420, mode_first=421;
inline constexpr ui::ControlKey scalar_slider=430;
inline constexpr ui::ControlKey pages=440, general_page=441, appearance_page=442, appearance_body=443;
inline constexpr ui::ControlKey color_pages=450, rgb_page=451, channels_page=452, color_body=453, swatch=454;
inline constexpr ui::ControlKey red=460, green=461, blue=462, color_reset=466, color_fold=470;
}

// A live consumer of the production controls, inside the ordinary editor host.
// Its sample values/history never write World or participate in scene history.
class ControlWorkbench {
public:
    struct Values {
        std::array<double,3> position{};
        std::array<double,4> vector4{0,0,0,1};
        std::array<double,3> color{.25,.55,.8};
        double exposure{1};
        std::wstring name{L"场景 Scene 01"};
        bool enabled{true};
        std::size_t mode{};
        bool operator==(const Values&) const = default;
    };
    static constexpr std::array mode_names{L"Perspective",L"Orthographic",L"UV / Texture"};
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
    bool color_expanded_{true};
    ui::ControlKey page_{workbench_control::general_page}, color_page_{workbench_control::rgb_page};
    void remember();
};
} // namespace atelier::editor

#pragma once
#include "ui_language/live_document.hpp"
#include <array>

namespace atelier::editor {
namespace workbench_control {
inline constexpr ui::ControlKey disable = 310;

inline constexpr ui::ControlKey long_labels = 311;

inline constexpr ui::ControlKey list = 319;

inline constexpr ui::ControlKey fold = 320;

inline constexpr ui::ControlKey body = 321;

inline constexpr ui::ControlKey position_reset = 334;

inline constexpr ui::ControlKey position_x = 340;

inline constexpr ui::ControlKey position_y = 341;

inline constexpr ui::ControlKey position_z = 342;

inline constexpr ui::ControlKey scalar_reset = 354;

inline constexpr ui::ControlKey scalar = 355;

inline constexpr ui::ControlKey name = 361;

inline constexpr ui::ControlKey enabled = 362;

inline constexpr ui::ControlKey reset = 370;

inline constexpr ui::ControlKey vector4_reset = 384;

inline constexpr ui::ControlKey vector4_x = 400;

inline constexpr ui::ControlKey vector4_y = 401;

inline constexpr ui::ControlKey vector4_z = 402;

inline constexpr ui::ControlKey vector4_w = 403;

inline constexpr ui::ControlKey mode = 420;

inline constexpr ui::ControlKey mode_first = 421;

inline constexpr ui::ControlKey scalar_slider = 430;

inline constexpr ui::ControlKey pages = 440;

inline constexpr ui::ControlKey general_page = 441;

inline constexpr ui::ControlKey appearance_page = 442;

inline constexpr ui::ControlKey appearance_body = 443;

inline constexpr ui::ControlKey color_pages = 450;

inline constexpr ui::ControlKey rgb_page = 451;

inline constexpr ui::ControlKey channels_page = 452;

inline constexpr ui::ControlKey color_body = 453;

inline constexpr ui::ControlKey swatch = 454;

inline constexpr ui::ControlKey red = 460;

inline constexpr ui::ControlKey green = 461;

inline constexpr ui::ControlKey blue = 462;

inline constexpr ui::ControlKey color_reset = 466;

inline constexpr ui::ControlKey color_fold = 470;
}

// A live consumer of the production controls, inside the ordinary editor host.
// Its sample values/history never write World or participate in scene history.
class ControlWorkbench {
public:
    struct Values {
        std::array<double, 3> position{};

        std::array<double, 4> vector4{0, 0, 0, 1};

        std::array<double, 3> color{.25, .55, .8};

        double exposure{1};

        std::wstring name{L"场景 Scene 01"};

        bool enabled{true};

        std::size_t mode{};

        bool operator==(const Values&) const = default;
    };

    static constexpr std::array mode_names{L"Perspective", L"Orthographic", L"UV / Texture"};

    const Values& values() const noexcept { return values_; }

    std::size_t undo_count() const noexcept { return cursor_; }

    std::size_t redo_count() const noexcept { return history_.size() - cursor_ - 1; }

    void append(ui_language::ControlOutput, ui::ControlKey parent, ui_language::LiveDocument&) const;

    bool apply(const ui::Action&);

    void undo();

    void redo();

private:
    Values values_;

    std::vector<Values> history_{values_};

    std::size_t cursor_{};

    std::optional<std::pair<ui::ControlKey, Values>> active_;

    bool disabled_{};

    bool long_labels_{};

    bool expanded_{true};

    bool color_expanded_{true};

    ui::ControlKey page_{workbench_control::general_page};

    ui::ControlKey color_page_{workbench_control::rgb_page};

    void remember();
};
} // namespace atelier::editor

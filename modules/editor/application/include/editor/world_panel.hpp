#pragma once
#include "editor/view_options.hpp"
#include "editor/performance.hpp"
#include "editor/property_row.hpp"
#include "renderer/renderer.hpp"
#include "ui_language/live_document.hpp"

namespace atelier::editor {
namespace world_control {
inline constexpr ui::ControlKey root = 1ULL << 43;

inline constexpr ui::ControlKey tab = root + 1;

inline constexpr ui::ControlKey content = root + 2;

inline constexpr ui::ControlKey slot = root + 3;

inline constexpr ui::ControlKey setup = root + 4;

inline constexpr ui::ControlKey lighting_tab = root + 8;

inline constexpr ui::ControlKey ddgi_tab = root + 9;

inline constexpr ui::ControlKey effects_tab = root + 10;

inline constexpr ui::ControlKey diagnostics_tab = root + 11;

inline constexpr ui::ControlKey performance_tab = root + 12;

inline constexpr ui::ControlKey performance_table = root + 13;

inline constexpr ui::ControlKey performance_status = root + 14;

inline constexpr ui::ControlKey performance_resources = root + 15;

inline constexpr ui::ControlKey slot_first = root + 16;

inline constexpr ui::ControlKey field_first = root + 256;

inline constexpr ui::ControlKey choice_first = root + 4096;

constexpr ui::ControlKey field(ViewOption option) {
    return field_first + static_cast<unsigned>(option);
}

constexpr ui::ControlKey choice(ViewOption option, unsigned value) {
    return choice_first + static_cast<unsigned>(option) * 32 + value;
}
}

// Owns the panel's selection and uncommitted number edit, never another copy of
// the authoritative display settings. A commit publishes through Workspace.
class WorldPanel {
public:
    Result<bool> apply(const ui::Action&, renderer::ViewSettings& candidate);

    std::vector<ui::MenuItem> menu(ui::ControlKey, const renderer::ViewSettings&) const;

    void append(std::vector<ui::Control>&, ui::ControlKey, ui_language::LiveDocument&, const renderer::ViewSettings&,
                bool editable, std::wstring_view main_camera, ui_language::ActionBinding clear_camera) const;

    void observe(const renderer::FrameStats&);

    FramePerformance& performance() noexcept { return performance_; }

    const FramePerformance& performance() const noexcept { return performance_; }

    void cancel() noexcept { edit_.reset(); }

    bool editing() const noexcept { return edit_.has_value(); }

    unsigned selected_volume() const noexcept { return slot_; }

private:
    struct Edit {
        ui::ControlKey key;

        double value;
    };

    unsigned slot_{};

    std::optional<Edit> edit_;

    FramePerformance performance_;

    mutable PropertyHelpCache help_;

    std::array<renderer::ProbeVolumeStats, renderer::probe_volume_limit> observed_{};

    std::array<std::array<std::wstring, 6>, renderer::probe_volume_limit> readouts_{};

    double displayed(const renderer::ViewSettings&, ViewOption) const;
};
}

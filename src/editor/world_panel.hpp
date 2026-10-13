#pragma once
#include "editor/view_options.hpp"
#include "renderer/renderer.hpp"
#include "ui_language/live_document.hpp"

namespace atelier::editor {
namespace world_control {
inline constexpr ui::ControlKey root=1ULL<<43,tab=root+1,content=root+2,slot=root+3,setup=root+4,
    lighting_tab=root+8,ddgi_tab=root+9,effects_tab=root+10,diagnostics_tab=root+11,slot_first=root+16,field_first=root+256,choice_first=root+4096;
constexpr ui::ControlKey field(ViewOption option) {return field_first+static_cast<unsigned>(option);}
constexpr ui::ControlKey choice(ViewOption option,unsigned value) {return choice_first+static_cast<unsigned>(option)*32+value;}
}
// Owns the panel's selection and uncommitted number edit, never another copy of
// the authoritative display settings. A commit publishes through Workspace.
class WorldPanel {
public:
    Result<bool> apply(const ui::Action&,renderer::ViewSettings& candidate);
    std::vector<ui::MenuItem> menu(ui::ControlKey,const renderer::ViewSettings&) const;
    void append(std::vector<ui::Control>&,ui::ControlKey,ui_language::LiveDocument&,const renderer::ViewSettings&,bool editable) const;
    void observe(const renderer::FrameStats&);
    void cancel() noexcept {edit_.reset();}
    bool editing() const noexcept {return edit_.has_value();}
    unsigned selected_volume() const noexcept {return slot_;}
private:
    struct Edit {ui::ControlKey key;double value;};
    unsigned slot_{};
    std::optional<Edit> edit_;
    std::array<renderer::ProbeVolumeStats,renderer::probe_volume_limit> observed_{};
    std::array<std::array<std::wstring,6>,renderer::probe_volume_limit> readouts_{};
    double displayed(const renderer::ViewSettings&,ViewOption) const;
};
}

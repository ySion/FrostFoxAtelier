#pragma once
#include "application/window.hpp"
#include "renderer/renderer.hpp"
#include "ui/controls.hpp"

namespace atelier::application {
// One reusable owned HWND. The owner retains keyboard focus and mouse capture;
// this surface uses the ordinary Session/SS/renderer and its own accepted frame.
class PopupHost final {
public:
    PopupHost(HWND owner,std::span<const std::wstring> fonts,bool validation);
    ~PopupHost() = default; // Renderer member retires before the Window member.
    void update(ui::ControlKey source,RECT anchor,bool below,std::span<const ui::MenuItem>,
                const ui::Style&,std::shared_ptr<const ui::StyleSheet>);
    void render(renderer::Capture* = nullptr);
    void hide();
    void close(); // Retire GPU work before HWND destruction.
    bool visible() const noexcept { return visible_; }
    HWND window() const noexcept { return window_.handle(); }
    const std::shared_ptr<const ui::UiFrame>& presented() const noexcept { return ui_.presented(); }
    ui::Reply input(ui::Event);
    ui::ControlKey source() const noexcept { return source_; }
private:
    Window window_;
    ui::Session ui_;
    renderer::Renderer renderer_;
    std::vector<ui::MenuItem> items_;
    std::vector<ui::Control> controls_;
    ui::PixelExtent extent_;
    std::shared_ptr<const ui::StyleSheet> sheet_;
    ui::ControlKey source_{};
    std::uint64_t revision_{};
    bool visible_{}, dirty_{}, closed_{};
};
}

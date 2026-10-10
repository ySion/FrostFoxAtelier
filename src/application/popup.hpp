#pragma once
#include "application/window.hpp"
#include "renderer/renderer.hpp"
#include "ui_language/component.hpp"
#include <map>

namespace atelier::application {
// One reusable owned HWND. The owner retains keyboard focus and mouse capture;
// this surface uses the ordinary Session/SS/renderer and its own accepted frame.
class PopupHost final {
public:
    PopupHost(HWND owner,std::span<const std::wstring> fonts,bool validation);
    ~PopupHost() = default; // Renderer member retires before the Window member.
    void update(ui::ControlKey source,RECT anchor,bool below,std::span<const ui::MenuItem>,
                const ui::Style&,std::shared_ptr<const ui::StyleSheet>,std::shared_ptr<ui_language::Component>,bool center_first_item = false);
    void render(renderer::Capture* = nullptr);
    void hide();
    void reset_document(); // Retire JIT scopes before the owner Runtime is closed.
    std::string_view diagnostic() const noexcept { return diagnostic_; }
    void close(); // Retire GPU work before HWND destruction.
    double open_cpu_us() const noexcept { return open_cpu_us_; }
    unsigned open_present_count() const noexcept { return open_present_count_; }
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
    ui::ControlKey next_key_{1ULL<<62};
    std::map<ui::ControlKey,ui::ControlKey> row_keys_;
    std::shared_ptr<ui_language::Component> component_;
    std::unique_ptr<ui_language::Instance> instance_;
    std::string diagnostic_;
    ui::PixelExtent extent_;
    std::shared_ptr<const ui::StyleSheet> sheet_;
    ui::ControlKey source_{};
    std::uint64_t revision_{};
    bool visible_{}, dirty_{}, closed_{};
    double open_cpu_us_{};
    unsigned open_present_count_{};
    bool opening_{};
    float measured_dpi_{};
    ui::PixelExtent measured_area_;
    void suspend();
    void retarget(ui::PixelExtent);
};
}

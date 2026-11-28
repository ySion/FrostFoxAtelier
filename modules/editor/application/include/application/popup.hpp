#pragma once
#include "application/window.hpp"
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "renderer/renderer.hpp"
#include "ui_language/component.hpp"

namespace atelier::application {
// One reusable owned HWND. The owner retains keyboard focus and mouse capture;
// this surface uses the ordinary Session/SS/renderer and its own accepted frame.
class PopupHost final {
public:
    PopupHost() = default;

    // Initialize once at a stable address sharing the process-wide Device.
    Result<void> initialize(std::shared_ptr<rhi::Device> device, HWND owner, Span<const std::wstring> fonts,
                            bool validation);

    ~PopupHost() = default; // Renderer member retires before the Window member.

    Result<void> update(ui::ControlKey source, RECT anchor, bool below, Span<const ui::MenuItem>, const ui::Style&,
                        std::shared_ptr<const ui::StyleSheet>, std::shared_ptr<ui_language::Component>,
                        bool center_first_item = false);

    Result<void> render(renderer::Capture* = nullptr);

    Result<void> hide();

    Result<void> reset_document(); // Retire JIT scopes before the owner Runtime is closed, even on failure.

    const String& diagnostic() const noexcept { return diagnostic_; }

    Result<void> close(); // Retire GPU work before HWND destruction.

    bool visible() const noexcept { return visible_; }

    HWND window() const noexcept { return window_.handle(); }

    const std::shared_ptr<const ui::UiFrame>& presented() const noexcept { return ui_.presented(); }

    ui::Reply input(ui::Event);

    ui::ControlKey source() const noexcept { return source_; }

private:
    Window window_;

    ui::Session ui_;

    ui::Session measurement_;

    renderer::Renderer renderer_;

    Array<ui::MenuItem> items_;

    Array<ui::Control> controls_;

    ui::ControlKey next_key_{1ULL << 62};

    Map<ui::ControlKey, ui::ControlKey> row_keys_;

    std::shared_ptr<ui_language::Component> component_;

    std::unique_ptr<ui_language::Instance> instance_;

    String diagnostic_;

    ui::PixelExtent extent_;

    std::shared_ptr<const ui::StyleSheet> sheet_;

    ui::ControlKey source_{};

    ui::ControlKey restore_focus_{};

    std::uint64_t revision_{};

    std::uint64_t measurement_revision_{};

    bool visible_{};

    bool dirty_{};

    bool closed_{};

    unsigned open_present_count_{};

    bool opening_{};

    float measured_dpi_{};

    float surface_dpi_{};

    ui::PixelExtent measured_area_;

    Result<void> suspend();

    Result<void> retarget(ui::PixelExtent);

    Error graphics_failure(const renderer::Error&); // Terminal failure retires this host before conversion.
};
}

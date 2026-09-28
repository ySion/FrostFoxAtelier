#pragma once

#include "application/window.hpp"
#include "editor/camera.hpp"
#include "editor/workspace.hpp"
#include "renderer/renderer.hpp"

namespace atelier::application {
struct FrameTiming {
    std::uint64_t frame_id{}; // Zero when the last step was not sampled/accepted.
    double prepare_us{}, render_us{}; // CPU wall time; render includes Present/waits.
};
class Application final {
public:
    explicit Application(bool gpu_validation = false);
    ~Application();
    bool step(renderer::Capture* = nullptr);
    void close();
    HWND window() const noexcept { return window_.handle(); }
    const editor::Session& document() const noexcept { return document_; }
    const std::shared_ptr<const ui::UiFrame>& presented() const noexcept { return ui_.presented(); }
    float parameter_width() const noexcept { return workspace_.parameter_width; }
    bool wants_keyboard() const noexcept { return ui_.wants_keyboard(); }
    std::uint64_t accepted_frames() const noexcept { return accepted_frames_; }
    // Explicit diagnostics only; capture frames are excluded because readback stalls.
    void sample_frames(bool enabled) noexcept { sampling_ = enabled; frame_timing_ = {}; }
    const FrameTiming& frame_timing() const noexcept { return frame_timing_; }
    ui::Result<void> set_style(const ui::Style& style) { return ui_.set_style(style); }
    const ui::Style& style() const noexcept { return ui_.style(); }
    ui::Statistics ui_statistics() const noexcept { return ui_.statistics(); }
    // Most recent step's render result; zero when that step did not render.
    const renderer::FrameStats& frame_statistics() const noexcept { return frame_statistics_; }
private:
    Window window_;
    editor::Session document_;
    editor::Camera camera_;
    editor::Workspace workspace_;
    ui::Session ui_;
    renderer::Renderer renderer_;
    std::vector<NativeEvent> events_;
    std::vector<renderer::Cube> cubes_;
    ui::Surface surface_;
    std::uint64_t surface_revision_{1}, accepted_frames_{};
    bool closed_{}, orbiting_{}, sampling_{};
    FrameTiming frame_timing_;
    renderer::FrameStats frame_statistics_;
    float last_x_{}, last_y_{};
    void native(const NativeEvent&);
    void effects(const ui::Reply&);
    void action(const ui::Action&);
    void status(Result<void>, const wchar_t* success);
    void invalidate_surface();
    bool pointer_ready() const noexcept;
    bool over_viewport(float, float) const noexcept;
};
}

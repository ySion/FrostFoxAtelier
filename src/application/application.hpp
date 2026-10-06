#pragma once

#include "application/window.hpp"
#include "application/popup.hpp"
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
    Result<void> open_project(const std::filesystem::path&,bool create=false);
    Result<void> open_scene(const std::filesystem::path&);
    Result<void> save_scene(const std::filesystem::path& relative = {});
    void close();
    HWND window() const noexcept { return window_.handle(); }
    const editor::Session& document() const noexcept { return *document_; }
    const std::shared_ptr<const ui::UiFrame>& presented() const noexcept { return ui_.presented(); }
    HWND menu_window() const noexcept { const auto* host=menu_host(); return host?host->window():nullptr; }
    const ui::UiFrame* menu_frame() const noexcept { const auto* host=menu_host(); return host?host->presented().get():nullptr; }
    double menu_open_cpu_us() const noexcept { const auto* host=menu_host(); return host?host->open_cpu_us():0; }
    void capture_menu(renderer::Capture& capture) { if (auto* host=menu_host()) host->render(&capture); }
    ui::ControlKey object_control(ObjectId actor) const noexcept { return workspace_.object_control(actor); }
    ui::ControlKey field_control(ObjectId component, std::span<const FieldId> path) const noexcept {
        return workspace_.field_control(component, path);
    }
    float parameter_width() const noexcept {
        const auto& frame=ui_.presented();
        const auto* pane=frame?frame->find(workspace_.dock.pane(editor::control::parameters_tab)):nullptr;
        return pane?pane->bounds.width/frame->drawing->dpi_scale:0;
    }
    ui::ControlKey parameter_splitter() const noexcept { return workspace_.dock.divider(editor::control::parameters_tab,ui::DragAxis::horizontal); }
    const ui::DockSpace& docking() const noexcept { return workspace_.dock; }
    ui::ControlKey reset_control(ObjectId component) const noexcept { return workspace_.reset_control(component); }
    bool workbench_visible() const noexcept { return workspace_.show_workbench(); }
    const editor::ControlWorkbench& control_workbench() const noexcept { return workspace_.workbench; }
    bool wants_keyboard() const noexcept { return ui_.wants_keyboard(); }
    const std::wstring& query() const noexcept { return workspace_.query; }
    const std::wstring& status_text() const noexcept { return workspace_.status; }
    std::optional<ui::TextInputState> text_input() const noexcept { return ui_.text_input(); }
    const NativeInputStatistics& input_statistics() const noexcept { return window_.input_statistics(); }
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
    friend struct EditorAcceptanceAccess; // Seeds real backend failures in the window E2E.
    Window window_;
    std::unique_ptr<editor::Session> document_{std::make_unique<editor::Session>()};
    std::optional<editor::Project> project_;
    std::filesystem::path scene_path_;
    std::wstring window_title_;
    editor::Camera camera_;
    editor::Workspace workspace_;
    ui::Session ui_;
    std::shared_ptr<const ui::StyleSheet> style_sheet_;
    std::unique_ptr<PopupHost> popup_;
    std::vector<std::unique_ptr<PopupHost>> submenus_;
    std::vector<ui::ControlKey> menu_path_;
    bool gpu_validation_{};
    std::uint64_t menu_incarnation_{};
    ui::ControlKey menu_return_focus_{};
    ui::ControlKey menu_context_target_{},menu_context_group_{};
    ui::TableLayout menu_table_layout_;
    ui::ControlKey pending_menu_{}; // Keyboard reveal waits for the bar's accepted geometry.
    renderer::Renderer renderer_;
    std::vector<renderer::MeshInstance> meshes_;
    ui::Surface surface_;
    std::uint64_t surface_revision_{1}, accepted_frames_{};
    bool closed_{}, orbiting_{}, sampling_{};
    FrameTiming frame_timing_;
    renderer::FrameStats frame_statistics_;
    float last_x_{}, last_y_{};
    PopupHost* menu_host() const noexcept {
        if (!menu_path_.empty() && submenus_.size()>=menu_path_.size() && submenus_[menu_path_.size()-1]->visible())
            return submenus_[menu_path_.size()-1].get();
        return popup_ && popup_->visible()?popup_.get():nullptr;
    }
    void native(NativeEvent);
    void request_menu(const ui::ContextMenuRequest&);
    void close_menu(bool restore_focus=true);
    void update_menu();
    bool menu_input(const ui::Event&,WPARAM);
    bool effects(const ui::Reply&);
    Result<void> execute(editor::Command,std::string_view asset={});
    Result<void> action(const ui::Action&);
    void sync_text_input();
    void status(Result<void>, const wchar_t* success);
    void status(Result<world::Commit>, const wchar_t* success);
    Result<void> focus_selected();
    bool confirm_discard();
    Result<void> project_command(editor::Command,std::string_view);
    void publish_document(std::unique_ptr<editor::Session>);
    void cancel_interaction(const ui::Reply&);
    void invalidate_surface();
    bool pointer_ready() const noexcept;
    bool over_viewport(float, float) const noexcept;
};
}

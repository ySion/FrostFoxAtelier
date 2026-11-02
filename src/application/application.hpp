#pragma once
#include "render_clock.hpp"
#include "lit_preparation.hpp"

#include "application/window.hpp"
#include "application/popup.hpp"
#include "application/play_process.hpp"
#include "application/scene_snapshot.hpp"
#include "application/timing.hpp"
#include "application/recovery.hpp"
#include "editor/camera.hpp"
#include "editor/operators.hpp"
#include "editor/workspace.hpp"
#include "renderer/renderer.hpp"
#include <chrono>
#include <functional>
#include <source_location>

namespace atelier::application {
class EditorControl;

enum class ProjectOpenPolicy { allow_repair, require_scene };

class Application final : private editor::OperatorHost {
public:
    explicit Application(bool gpu_validation = false, bool activate = true,
                         std::optional<ui::PixelExtent> initial_client = {}, bool vsync = true);

    ~Application();

    bool step(renderer::Capture* = nullptr);

    void enable_control(const std::filesystem::path& directory);

    // A require_scene open also checks the loaded scene with dmit before retiring the current document.
    Result<void> open_project(const std::filesystem::path&, bool create = false,
                              ProjectOpenPolicy = ProjectOpenPolicy::allow_repair,
                              const std::function<Result<void>(const editor::Session&)>& admit = {},
                              bool interactive = true);

    Result<void> open_scene(const std::filesystem::path&, bool interactive = true);

    // View preferences only; authored World settings remain unchanged.
    Result<void> configure_view(math::Vector3 eye, math::Vector3 target, renderer::Shading);

    Result<renderer::ViewSettings> fitted_probe_view(bool follow_focus) const;

    Result<void> open_source(const std::filesystem::path&) override;

    Result<void> save_scene(const std::filesystem::path& relative = {}) override;

    Result<void> build_project(bool rebuild = false);

    Result<void> run_project();

    Result<void> preview_project_ui();

    Result<void> create_source(const std::filesystem::path& relative, bool project_ui = false) override;

    void stop_project() noexcept {
        restart_play_ = false;
        play_.stop();
    }

    void close();

    HWND window() const noexcept { return window_.handle(); }

    const editor::Session& document() const noexcept { return *document_; }

    const std::shared_ptr<const ui::UiFrame>& presented() const noexcept { return ui_.presented(); }

    HWND menu_window() const noexcept {
        const auto* host = menu_host();
        return host ? host->window() : nullptr;
    }

    const ui::UiFrame* menu_frame() const noexcept {
        const auto* host = menu_host();
        return host ? host->presented().get() : nullptr;
    }

    void capture_menu(renderer::Capture& capture) {
        if (auto* host = menu_host())
            host->render(&capture);
    }

    ui::ControlKey object_control(ObjectId entity) const noexcept { return workspace_.object_control(entity); }

    ui::ControlKey field_control(ObjectId component, std::span<const FieldId> path) const noexcept {
        return workspace_.field_control(component, path);
    }

    float parameter_width() const noexcept {
        const auto& frame = ui_.presented();
        const auto* pane = frame ? frame->find(workspace_.dock.pane(editor::control::parameters_tab)) : nullptr;
        return pane ? pane->bounds.width / frame->drawing->dpi_scale : 0;
    }

    ui::ControlKey parameter_splitter() const noexcept {
        return workspace_.dock.divider(editor::control::parameters_tab, ui::DragAxis::horizontal);
    }

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

    // The frame timing record of the most recent step, sampled or not.
    const timing::FrameRecord& frame_record() const noexcept { return recorder_.last(); }

    ui::Result<void> set_style(const ui::Style& style) { return ui_.set_style(style); }

    const ui::Style& style() const noexcept { return ui_.style(); }

    ui::Statistics ui_statistics() const noexcept { return ui_.statistics(); }

    // Most recent step's render result; zero when that step did not render.
    const renderer::FrameStats& frame_statistics() const noexcept { return frame_statistics_; }

private:
    friend struct EditorAcceptanceAccess; // Seeds real backend failures in the window E2E.

    friend class EditorControl;

    std::unique_ptr<EditorControl> control_;

    Window window_;

    std::unique_ptr<editor::Session> document_{
        std::make_unique<editor::Session>(editor::Session::Prepare{}, editor::SessionRole::editor)};

    SceneRecovery recovery_;

    void preserve_failure(std::string_view) noexcept;

    std::optional<editor::Project> project_;

    editor::ProjectCode* project_code_{}; // Borrowed from document_'s preparation owner.

    PlayProcess play_;

    PlayLink live_; // The running Play's change link; the document forwards while it is active.

    bool restart_play_{};

    void forward_live() noexcept;

    std::string pending_scene_;

    std::filesystem::path play_snapshot_;

    std::filesystem::path document_root_;

    std::uint64_t dropped_diagnostics_{};

    bool play_log_ended_{};

    std::array<PlayProcess::StreamStatus, 2> play_output_status_{};

    void attach_play_output() noexcept;

    void consume_play_output() noexcept;

    void notice(std::string_view, std::wstring_view, logging::Severity = logging::Severity::info) noexcept;

    void report_error(const Error&, std::string_view event = "editor.operation_failed",
                      logging::Severity = logging::Severity::error) noexcept;

    void diagnostics() noexcept;

    std::chrono::steady_clock::time_point next_project_poll_{};

    bool source_build_pending_{};

    std::filesystem::path scene_path_;

    std::wstring window_title_;

    editor::Camera camera_;

    editor::ViewportTool viewport_tool_;

    std::optional<editor::ViewportView>
        accepted_view_; // Owns the accepted camera and CPU draw group, never a GPU batch.

    math::Vector2 pointer_position_{};

    editor::Workspace workspace_;

    ui::Session ui_;

    std::shared_ptr<const ui::StyleSheet> style_sheet_;

    std::unique_ptr<PopupHost> popup_;

    std::vector<std::unique_ptr<PopupHost>> submenus_;

    std::vector<ui::ControlKey> menu_path_;

    bool gpu_validation_{};

    bool vsync_{true};

    std::uint64_t menu_incarnation_{};

    ui::ControlKey menu_return_focus_{};

    ui::ControlKey menu_context_target_{};

    ui::ControlKey menu_context_group_{};

    ui::TableLayout menu_table_layout_;

    ui::ControlKey pending_menu_{}; // Keyboard reveal waits for the bar's accepted geometry.

    std::chrono::steady_clock::time_point menu_requested_;

    bool pie_release_pending_{};

    bool pie_release_select_{};

    bool pie_pressed_{};

    ui::ControlKey pie_hot_{};

    bool pending_source_focus_{};

    bool pending_parameter_search_focus_{}; // Search opens only after its field is presented.

    bool pending_name_focus_{}; // F2 focuses the Entity name once Parameters is presented.

    bool pending_rename_focus_{}; // Inline tree rename focuses its field once presented.

    renderer::Renderer renderer_;

    SceneSnapshot scene_; // Pending/adopted batches retire before renderer_ (also on exceptional shutdown).

    bool camera_cut_{true}; // Retained until the renderer accepts the new view.

    std::vector<renderer::OverlayShape> overlay_;

    ui::Surface surface_;

    std::uint64_t surface_revision_{1};

    std::uint64_t accepted_frames_{};

    bool closed_{};

    struct CameraDrag {
        ui::PointerButton button;

        bool pan;

        math::Vector2 previous;
    };

    std::optional<CameraDrag> camera_drag_;

    struct CameraFly {
        editor::Camera entry;

        std::array<bool, 6> movement{}; // W, S, A, D, Q, E.

        bool fast{};

        float speed{5}; // Metres per second, before Shift acceleration.

        std::chrono::steady_clock::time_point previous;
    };

    std::optional<CameraFly> camera_fly_;

    std::optional<ui::PointerButton> camera_fly_release_;

    void finish_camera_fly(bool accept);

    void update_camera_fly();

    void look_camera_fly(float dx, float dy);

    timing::Recorder recorder_;

    std::optional<std::uint64_t> sampled_frame_; // The UI frame of a step accepted without capture.

    double fps_interval_{}; // The frame interval the FPS text was formatted from.

    bool run_step(renderer::Capture*);

    std::chrono::steady_clock::time_point render_retry_at_{};

    std::chrono::milliseconds render_retry_delay_{};

    RenderClock lighting_clock_;

    LitFallback lit_fallback_;

    bool fallback_presented_{}; // The last accepted frame drew a fallback for an unprepared Lit view.

    renderer::FrameStats frame_statistics_;

    PopupHost* menu_host() const noexcept {
        if (!menu_path_.empty() && submenus_.size() >= menu_path_.size() && submenus_[menu_path_.size() - 1]->visible())
            return submenus_[menu_path_.size() - 1].get();
        return popup_ && popup_->visible() ? popup_.get() : nullptr;
    }

    void native(NativeEvent);

    void input(const ui::Event&, WPARAM native_key);

    bool key_input(const ui::Event&, std::uint32_t, editor::RegionKind, editor::KeyPhase);

    void request_menu(const ui::ContextMenuRequest&);

    void close_menu(bool restore_focus = true, std::source_location caller = std::source_location::current());

    void update_menu();

    bool menu_input(const ui::Event&, WPARAM);

    bool pie_input(const ui::Event&, WPARAM);

    bool invoke_menu(const ui::Action&, std::span<const ui::MenuItem>);

    bool effects(const ui::Reply&);

    const ui::ControlBox* command_focus() const noexcept;

    editor::Context operator_context(bool interactive);

    Result<editor::OperatorReport> call_operator(const editor::OperatorCall&, bool interactive,
                                                 const editor::ResolveReference& = {});

    Result<void> apply_document_request(const editor::DocumentRequest&);

    // UI entry: an unavailable operator is a silent no-op unless the caller reports its reason.
    // Completed text edits pass interactive=false: an empty committed name must fail, not reopen a prompt.
    Result<void> run_operator(const editor::OperatorCall&, bool report_unavailable = false, bool interactive = true);

    Result<std::optional<std::filesystem::path>> choose_path(editor::PathPurpose, const std::filesystem::path& initial,
                                                             std::wstring_view filename) override;

    void asset_created(std::string_view key, bool inspect_material) override;

    Result<void> end_interaction() override;

    const editor::Camera& camera() const noexcept override { return camera_; }

    Result<void> set_camera(const editor::Camera& camera) override { return set_camera(camera, true); }

    editor::ViewportTool& viewport_tool() noexcept override { return viewport_tool_; }

    Result<void> place_duplicates() override;

    Result<void> begin_entity_rename(ObjectId, editor::RegionKind) override;

    void reset_layout() override { workspace_.reset_layout(); }

    Result<void> close_tab(editor::RegionKind, bool interactive) override;

    void maximize_tab(editor::RegionKind) override;

    void show_tab(editor::RegionKind) override;

    void cycle_tab(editor::RegionKind, int direction) override;

    Result<void> source_history(bool redo) override;

    Result<void> workbench_history(bool redo) override;

    void scene_changed() override;

    void set_tool_settings(const editor::ToolSettings& settings) override { workspace_.viewport_settings = settings; }

    Result<void> set_view_settings(const renderer::ViewSettings&) override;

    void quit() override { PostMessageW(window(), WM_CLOSE, 0, 0); }

    void about() override;

    void select_box() override;

    void show_menu(editor::MenuKind) override;

    void toggle_view(editor::ViewToggleOption) override;

    Result<void> fit_lighting(bool follow_focus) override;

    Result<void> rename_asset(std::string_view key, std::string_view name, bool interactive) override;

    Result<void> save_documents(const editor::Context&, bool all) override;

    Result<void> close_source(bool interactive) override;

    Result<void> reload_source(bool interactive) override;

    Result<void> start_play(bool preview_ui, bool interactive) override;

    void stop_play(bool restart) override;

    Result<std::string> import_asset(editor::ImportKind, const std::filesystem::path&) override;

    Result<void> reimport_asset(std::string_view key, const std::filesystem::path&) override;

    Result<void> action(const ui::Action&);

    void sync_text_input();

    Result<void> completion(Result<void>);

    Result<void> completion(Result<world::Commit>);

    void status(Result<void>, const wchar_t* success);

    void status(Result<world::Commit>, const wchar_t* success);

    Result<void> open_color_edit(ObjectId, std::span<const FieldId> prefix, ui::ControlKey swatch);

    Result<bool> color_edit_action(const ui::Action&);

    Result<void> apply_color(std::array<double, 3> rgb);

    Result<void> finish_color_edit(bool keep);

    Result<void> begin_asset_rename();

    TypeId take_type(ObjectId, std::span<const FieldId>) const;

    Result<void> set_camera(const editor::Camera&, bool cut);

    void sync_world_view();

    std::array<float, 3> lighting_focus() const;

    Result<bool> confirm_discard(bool sources = true, bool interactive = true);

    Result<bool> confirm_source(editor::SourceEditor::File&, bool interactive = true);

    Result<void> save_source(editor::SourceEditor::File&, bool interactive = true);

    void poll_project();

    void publish_build(std::shared_ptr<const editor::ProjectBuild>);

    void publish_document(std::unique_ptr<editor::Session>, editor::ProjectCode* = nullptr);

    Result<void> load_project(const std::filesystem::path&, bool create = false,
                              const std::filesystem::path& scene = {},
                              ProjectOpenPolicy = ProjectOpenPolicy::allow_repair,
                              const std::function<Result<void>(const editor::Session&)>& admit = {});

    void cancel_interaction(const ui::Reply&);

    void invalidate_surface();

    bool pointer_ready() const noexcept;

    bool viewport_input(const ui::Event&, std::uint32_t native_key);
};
}

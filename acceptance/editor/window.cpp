#include "window.hpp"
#include "assets/png.hpp"
#include "platform/error.hpp"
#include "application/application.hpp"
#include "application/diagnostics.hpp"
#include "application/editor_control.hpp"
#include "assets/file.hpp"
#include "atelier/build_info.hpp"
#include "renderer/raster_viewport.hpp"
#include "renderer/timing.hpp"
#include "ui/text.hpp"
#include "platform/utf.hpp"
#include "foundation/sha256.hpp"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>

namespace {
using atelier::application::Application;

using atelier::Error;

using atelier::ErrorCode;

using atelier::Result;

std::unexpected<Error> command_error(atelier::Utf8Text message, ErrorCode code = ErrorCode::invalid_definition) {
    return std::unexpected(Error{code, std::move(message).into_string()});
}

Result<void> advance(Application& app, atelier::Utf8Text closed, atelier::renderer::Capture* image = nullptr) {
    ATELIER_TRY_BIND(running, app.step(image));
    if (!*running)
        return command_error(std::move(closed));
    return {};
}

Result<void> prepare_output(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error)
        return command_error(atelier::platform::error_message(error), ErrorCode::invalid_storage);
    return {};
}

struct ImageApartment {
    const HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ~ImageApartment() {
        if (SUCCEEDED(status))
            CoUninitialize();
    }

    Result<void> ready() const {
        if (FAILED(status))
            return command_error(atelier::String::concat("Cannot initialize image encoder: ", status),
                                 ErrorCode::invalid_storage);
        return {};
    }
};

Result<void> png(const atelier::renderer::Capture& image, const std::filesystem::path& path) {
    return atelier::assets::write_png(image.extent.width, image.extent.height, image.rgba, path);
}
}

namespace atelier::application {
struct EditorAcceptanceAccess {
    static editor::Workspace& workspace(Application& app) { return app.workspace_; }

    static void mark_saved(Application& app) {
        app.effects(app.ui_.cancel_interaction());
        app.document_->mark_saved();
    }

    static Result<void> invoke(Application& app, Utf8Text id, json::Document args = {}) {
        return app.run_operator({std::move(id).into_string(), std::move(args)}, true);
    }

    static Result<bool> view_preparing(Application& app) {
        const auto prepared = app.renderer_.preparation(app.control_ ? app.control_->view() : app.workspace_.view);
        if (!prepared)
            return std::unexpected(renderer_error(prepared.error()));
        if (!prepared->ready || app.fallback_presented_)
            return true;
        if (app.presented() && app.presented()->find(editor::control::viewport)) {
            const auto* draw = app.scene_.draw();
            return !draw || draw->picking != app.scene_.picking() || !app.accepted_view_ ||
                   app.accepted_view_->scene != draw->picking;
        }
        return false;
    }

    static bool fallback_presented(const Application& app) { return app.fallback_presented_; }

    static auto camera_state(const Application& app) { return app.camera_.state(); }

    static Result<void> move_camera(Application& app, const editor::CameraState& state) {
        auto camera = app.camera_;
        if (!camera.set_state(state))
            return command_error("The orbit camera state was rejected.");
        return app.set_camera(camera, false);
    }

    static const auto& accepted_view(const Application& app) { return app.accepted_view_; }

    static bool gpu_validation(const Application& app) { return app.gpu_validation_; }

    static bool vsync(const Application& app) { return app.vsync_; }

    static std::filesystem::path project_root(const Application& app) {
        return app.project_ ? app.project_->root() : std::filesystem::path{};
    }

    static std::filesystem::path scene_path(const Application& app) { return app.scene_path_; }

    static ui::DockSpace::Presentation dock_presentation(const Application& app) {
        const auto size = app.surface_.extent;
        return app.workspace_.dock.presentation(
            0, {float(size.width) / app.surface_.dpi_scale, float(size.height) / app.surface_.dpi_scale});
    }

    static Result<void> set_scene_maximized(Application& app, bool maximized) {
        auto& dock = app.workspace_.dock;
        if (!dock.select(editor::control::scene_tab))
            return command_error("Scene View cannot be selected.");
        for (const auto& pane : dock_presentation(app).panes)
            if (pane.active && pane.active->key == editor::control::scene_tab) {
                if (pane.maximized != maximized)
                    dock.maximize(editor::control::scene_tab);
                return {};
            }
        return command_error("Scene View has no presented Dock pane.");
    }

    static auto scene_lit_view(const Application& app) { return app.workspace_.view; }

    static Result<void> performance_view(Application& app, const renderer::ViewSettings& view) {
        return app.workspace_.set_view_settings(*app.document_, view, app.lighting_focus());
    }

    static Result<void> performance_panel(Application& app) {
        ATELIER_TRY(
            app.workspace_.dock.move(editor::world_control::tab, editor::control::parameters_tab, ui::DockSide::tabs)
                .transform_error(ui_error));
        app.workspace_.dock.select(editor::world_control::tab);
        return {};
    }

    static const auto& scene_snapshot(const Application& app) { return app.scene_; }

    static const auto& render_notice(const Application& app) { return app.workspace_.render_notice; }

    static void set_tool(Application& app, editor::TransformTool tool) { app.workspace_.viewport_settings.tool = tool; }

    static void set_cursor(Application& app, math::Vector3 at) { app.viewport_tool_.cursor = at; }

    static Result<void> ui_action(Application& app, ui::ControlKey key, ui::ActionValue value = {}) {
        return app.action({ui::ActionKind::activate, key, std::move(value)});
    }

    static void select_tab(Application& app, ui::ControlKey tab) { app.workspace_.dock.select(tab); }

    static Result<void> open_menu(Application& app, ui::ControlKey source) {
        app.request_menu({source, 0, 0, 0});
        return {};
    }

    static void close_menu(Application& app) { app.close_menu(false); }

    static Result<void> verify_keep_open_menu(Application& app) {
        app.request_menu({editor::control::scene_guides, 0, 0, 0});
        for (int i = 0; i < 6 && !app.menu_window(); ++i)
            ATELIER_TRY(advance(app, "Editor closed before Guides menu opened."));
        if (!app.menu_window() || !app.popup_ || !app.popup_->presented())
            return command_error("Guides popup menu was not presented.");

        const auto find_fps = [&]() -> const ui::MenuItem* {
            const auto at = std::ranges::find_if(app.workspace_.menu_items,
                                                 [](const ui::MenuItem& item) { return item.text == L"Show FPS"; });
            return at == app.workspace_.menu_items.end() ? nullptr : &*at;
        };
        const auto* initial = find_fps();
        if (!initial || initial->selected || app.workspace_.show_fps)
            return command_error("Unexpected initial Show FPS menu state.");
        const auto fps_key = initial->key;
        const auto* box = app.popup_->presented()->find(fps_key);
        if (!box)
            return command_error("Show FPS item missing from presented popup.");

        const float scale = app.popup_->presented()->drawing ? app.popup_->presented()->drawing->dpi_scale : 1.f;
        POINT pt{static_cast<LONG>(std::lround(box->bounds.x + 6.f * scale)),
                 static_cast<LONG>(std::lround(box->bounds.y + box->bounds.height * .5f))};
        ClientToScreen(app.popup_->window(), &pt);
        ScreenToClient(app.window(), &pt);

        const auto click_checkbox = [&]() -> Result<void> {
            ui::Event down;
            down.kind = ui::EventKind::pointer_down;
            down.button = ui::PointerButton::primary;
            down.x = static_cast<float>(pt.x);
            down.y = static_cast<float>(pt.y);
            ATELIER_TRY(app.input(down, 0));
            ui::Event up = down;
            up.kind = ui::EventKind::pointer_up;
            return app.input(up, 0);
        };

        // 1. Keep-open checkbox click must refresh menu_items and popup presentation on the very next step,
        // even when VSync=1 throttles host presentation (!host_ready).
        ATELIER_TRY(click_checkbox());
        ATELIER_TRY(advance(app, "Editor closed during keep-open checkbox toggle."));
        const auto* checked_item = find_fps();
        const auto* checked_box = app.popup_->presented() ? app.popup_->presented()->find(fps_key) : nullptr;
        if (!app.menu_window() || !app.workspace_.show_fps || !checked_item || !checked_item->selected ||
            !checked_box || !checked_box->selected)
            return command_error("Keep-open checkbox click left stale popup menu state.");

        // 2. Direct view-revision mutation while the native menu stays open must also invalidate the fast path.
        app.toggle_view(editor::ViewToggleOption::fps);
        ATELIER_TRY(advance(app, "Editor closed after view_revision invalidation."));
        const auto* unchecked_item = find_fps();
        const auto* unchecked_box = app.popup_->presented() ? app.popup_->presented()->find(fps_key) : nullptr;
        if (!app.menu_window() || app.workspace_.show_fps || !unchecked_item || unchecked_item->selected ||
            !unchecked_box || unchecked_box->selected)
            return command_error("View revision change while menu was open left stale popup menu state.");

        app.close_menu(false);
        return {};
    }

    static void cancel_rename(Application& app) { app.workspace_.row_rename.reset(); }

    static ui::ControlKey color_pick(const Application& app) {
        if (!app.presented())
            return 0;
        for (const auto& box : app.presented()->controls)
            if (box.kind == ui::ControlKind::button && box.action_target) {
                const auto* binding = app.workspace_.binding(box.action_target);
                if (binding && binding->kind == editor::Workspace::Binding::Kind::color_pick)
                    return box.action_target;
            }
        return 0;
    }

    // Drive the real tool entry points without OS input (snapshot only).
    static Result<void> tool_key(Application& app, std::uint32_t vk, math::Vector2 pointer) {
        ui::Event event;
        event.kind = ui::EventKind::key_down;
        ATELIER_TRY(app.viewport_tool_.key(*app.document_, *app.accepted_view_, event, vk,
                                           app.workspace_.viewport_settings, pointer));
        return {};
    }

    static Result<void> tool_move(Application& app, math::Vector2 pointer) {
        ui::Event event;
        event.kind = ui::EventKind::pointer_move;
        event.x = pointer.x;
        event.y = pointer.y;
        ATELIER_TRY(
            app.viewport_tool_.pointer(*app.document_, *app.accepted_view_, event, app.workspace_.viewport_settings));
        return {};
    }

    static Result<void> tool_cancel(Application& app) { return app.viewport_tool_.cancel(*app.document_); }

    static Result<void> place_cursor(Application& app, math::Vector2 pointer) {
        ui::Event event;
        event.kind = ui::EventKind::pointer_down;
        event.button = ui::PointerButton::primary;
        event.x = pointer.x;
        event.y = pointer.y;
        auto settings = app.workspace_.viewport_settings;
        settings.tool = editor::TransformTool::select;
        ATELIER_TRY(app.viewport_tool_.pointer(*app.document_, *app.accepted_view_, event, settings));
        event.kind = ui::EventKind::pointer_up;
        ATELIER_TRY(app.viewport_tool_.pointer(*app.document_, *app.accepted_view_, event, settings));
        return {};
    }

    static Result<math::Vector2> viewport_point(const Application& app, math::Vector3 point) {
        if (!app.accepted_view_)
            return command_error("Viewport has no accepted camera.");
        const auto projected = app.accepted_view_->project(point);
        if (!projected)
            return command_error("Viewport point is outside the camera.");
        return math::Vector2{std::round(projected->x), std::round(projected->y)};
    }
};
}

namespace {
using atelier::application::Application;

using Access = atelier::application::EditorAcceptanceAccess;

using atelier::ui::ControlKey;

namespace control = atelier::editor::control;

LRESULT CALLBACK native_dialog_guard(int code, WPARAM window, LPARAM parameter) {
    if (code == HCBT_CREATEWND) {
        const auto& created = *reinterpret_cast<const CBT_CREATEWNDW*>(parameter)->lpcs;
        const bool dialog = IS_INTRESOURCE(created.lpszClass) ? reinterpret_cast<ULONG_PTR>(created.lpszClass) == 0x8002
                                                              : std::wstring_view(created.lpszClass) == L"#32770";
        if (dialog && !(created.style & WS_CHILD)) {
            const std::wstring_view title =
                created.lpszName && !IS_INTRESOURCE(created.lpszName) ? created.lpszName : L"";
            // The hook runs inside a Win32 callback: an unconvertible title must not throw past it.
            const auto text = atelier::platform::utf16_to_utf8(title);
            if (text) {
                std::fprintf(stderr, "Editor acceptance: FAIL: unexpected native dialog \"%s\"\n", text->c_str());
            } else {
                std::fputs("Editor acceptance: FAIL: unexpected native dialog (title not printable)\n", stderr);
            }
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 1);
        }
    }
    return CallNextHookEx(nullptr, code, window, parameter);
}

Result<atelier::renderer::Capture> capture(Application& app) {
    atelier::renderer::Capture image;
    const auto deadline = GetTickCount64() + 10000;
    do {
        // A serviced input wakeup need not have permission to submit a host frame.
        ATELIER_TRY(advance(app, "Editor closed before capture.", &image));
        ATELIER_TRY_BIND(preparing, Access::view_preparing(app));
        if (*preparing)
            return command_error("Capture met a previous scene or unprepared view; tick first.");
        if (!(image.frame_id || GetTickCount64() < deadline))
            return command_error("Renderer did not capture an accepted frame.");
    } while (!image.frame_id);
    if (!(image.rgba.size() == static_cast<std::size_t>(image.extent.width) * image.extent.height * 4))
        return command_error("Capture has an invalid pixel payload.");
    return image;
}

Result<void> capture(Application& app, const std::filesystem::path& path) {
    ATELIER_TRY_BIND(image, capture(app));
    const auto scene_pixels =
        std::count_if(image->depth.begin(), image->depth.end(), [](float value) { return value > 0 && value <= 1; });
    if (app.presented()->find(control::viewport)) {
        if (scene_pixels <= 32)
            return command_error("Real scene geometry is absent from the depth capture.");
    } else {
        if (scene_pixels != 0)
            return command_error("Hidden Scene tab continued rendering geometry.");
    }
    ATELIER_TRY(png(*image, path));
    if (app.menu_window()) {
        atelier::renderer::Capture popup;
        ATELIER_TRY(app.capture_menu(popup));
        ATELIER_TRY(png(popup, path.parent_path() / (path.stem().wstring() + L"-popup.png")));
    }
    return {};
}
}

Result<void> guard_acceptance_native_dialogs() {
    if (!SetWindowsHookExW(WH_CBT, native_dialog_guard, nullptr, GetCurrentThreadId()))
        return command_error(atelier::platform::error_message(std::error_code(GetLastError(), std::system_category())),
                             ErrorCode::invalid_storage);
    return {};
}

Result<void> capture_editor_snapshot(Application& app, const std::filesystem::path& output) {
    using Access = atelier::application::EditorAcceptanceAccess;
    ImageApartment apartment;
    ATELIER_TRY(apartment.ready());
    ATELIER_TRY(prepare_output(output));
    const auto settle = [&]() -> Result<void> {
        for (int i = 0; i < 6; ++i)
            ATELIER_TRY(advance(app, "Snapshot editor closed."));
        return {};
    };
    const auto shot = [&](const char* name) -> Result<void> {
        ATELIER_TRY(settle());
        atelier::renderer::Capture image;
        for (int attempt = 0; attempt < 30 && !image.frame_id; ++attempt)
            ATELIER_TRY(advance(app, "Snapshot editor closed.", &image));
        if (image.frame_id == 0)
            return command_error("Snapshot frame was not accepted (window occluded?).");
        ATELIER_TRY(png(image, output / name));
        return {};
    };
    ATELIER_TRY(settle());
    ATELIER_TRY(Access::invoke(app, "scene.add", atelier::json::Document::literal<R"({"kind":"mesh"})">()));
    Access::set_cursor(app, {1.4f, 0, -1.1f});
    Access::set_tool(app, atelier::editor::TransformTool::rotate);
    ATELIER_TRY(shot("snapshot-rotate.png"));
    Access::set_tool(app, atelier::editor::TransformTool::move);
    ATELIER_TRY(shot("snapshot-move.png"));
    Access::set_tool(app, atelier::editor::TransformTool::scale);
    ATELIER_TRY(shot("snapshot-scale.png"));
    Access::set_tool(app, atelier::editor::TransformTool::transform);
    ATELIER_TRY(shot("snapshot-transform.png"));
    // Surface-aligned cursor on the cube's +X face, then modal feedback.
    ATELIER_TRY_BIND(surface, Access::viewport_point(app, {.5f, .15f, -.15f}));
    ATELIER_TRY(Access::place_cursor(app, *surface));
    ATELIER_TRY(shot("snapshot-cursor-surface.png"));
    ATELIER_TRY_BIND(origin, Access::viewport_point(app, {0, 0, 0}));
    auto pointer = *origin;
    pointer.x += 60;
    pointer.y -= 40;
    ATELIER_TRY(Access::tool_key(app, 'G', pointer));
    ATELIER_TRY(Access::tool_key(app, 'Z', pointer));
    pointer.y -= 50;
    ATELIER_TRY(Access::tool_move(app, pointer));
    ATELIER_TRY(shot("snapshot-modal-grab.png"));
    ATELIER_TRY(Access::tool_cancel(app));
    ATELIER_TRY(Access::tool_key(app, 'R', pointer));
    ATELIER_TRY(Access::tool_key(app, 'X', pointer));
    pointer.x += 70;
    ATELIER_TRY(Access::tool_move(app, pointer));
    ATELIER_TRY(shot("snapshot-modal-rotate.png"));
    ATELIER_TRY(Access::tool_cancel(app));
    // An enum menu and colour swatch row, through the real light creation command.
    ATELIER_TRY(Access::invoke(app, "scene.add", atelier::json::Document::literal<R"({"kind":"rectangle"})">()));
    ATELIER_TRY(shot("snapshot-light-parameters.png"));
    if (const auto pick = Access::color_pick(app)) {
        ATELIER_TRY(Access::ui_action(app, pick));
        ATELIER_TRY(shot("snapshot-color-editor.png"));
        ATELIER_TRY(Access::ui_action(app, atelier::editor::control::color_done));
    }
    // Layout review: category grouping and Scene Outliner (including inline components).
    ATELIER_TRY(Access::ui_action(app, atelier::editor::control::group_categories, true));
    ATELIER_TRY(shot("snapshot-parameters-category.png"));
    ATELIER_TRY(Access::ui_action(app, atelier::editor::control::group_components, true));
    Access::select_tab(app, atelier::editor::control::entities_tab);
    ATELIER_TRY(Access::invoke(app, "scene.rename"));
    ATELIER_TRY(shot("snapshot-inline-rename.png"));
    Access::cancel_rename(app);
    ATELIER_TRY(Access::ui_action(app, atelier::editor::control::outliner_components_toggle, true));
    ATELIER_TRY(shot("snapshot-structure.png"));
    ATELIER_TRY(Access::ui_action(app, atelier::editor::control::outliner_components_toggle, false));
    Access::select_tab(app, atelier::editor::control::scene_tab);
    ATELIER_TRY(Access::open_menu(app, atelier::editor::control::viewport_snap));
    ATELIER_TRY(settle());
    {
        atelier::renderer::Capture menu;
        ATELIER_TRY(app.capture_menu(menu));
        if (!menu.rgba.empty())
            ATELIER_TRY(png(menu, output / "snapshot-snap-menu.png"));
    }
    Access::close_menu(app);
    ATELIER_TRY(Access::open_menu(app, atelier::editor::control::viewport_orientation));
    ATELIER_TRY(settle());
    {
        atelier::renderer::Capture menu;
        ATELIER_TRY(app.capture_menu(menu));
        if (!menu.rgba.empty())
            ATELIER_TRY(png(menu, output / "snapshot-orientation-menu.png"));
    }
    Access::close_menu(app);
    ATELIER_TRY(Access::verify_keep_open_menu(app));
    ATELIER_TRY(Access::performance_panel(app));
    ATELIER_TRY(shot("snapshot-world.png"));
    Access::select_tab(app, atelier::editor::control::workbench);
    ATELIER_TRY(shot("snapshot-controls.png"));
    Access::mark_saved(app);
    SendMessageW(app.window(), WM_CLOSE, 0, 0);
    for (;;) {
        ATELIER_TRY_BIND(running, app.step());
        if (!*running)
            break;
    }
    std::puts("Editor snapshot: written");
    return {};
}

#include "performance.inl"
#include "ui_reload_profile.inl"

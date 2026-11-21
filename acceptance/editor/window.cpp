#include "../helpers.hpp"
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
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <thread>

using atelier::acceptance::helpers::take;

namespace atelier::application {
struct EditorAcceptanceAccess {
    static editor::Workspace& workspace(Application& app) { return app.workspace_; }

    static void mark_saved(Application& app) {
        app.effects(app.ui_.cancel_interaction());
        app.document_->mark_saved();
    }

    static Result<void> invoke(Application& app, std::string id, json::Document args = {}) {
        return app.run_operator({std::move(id), std::move(args)}, true);
    }

    static bool view_preparing(Application& app) {
        const auto prepared = app.renderer_.preparation(app.control_ ? app.control_->view() : app.workspace_.view);
        if (!prepared)
            throw std::runtime_error("Scene Lit preparation failed: " + prepared.error().cause.message);
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

    static bool move_camera(Application& app, const editor::CameraState& state) {
        auto camera = app.camera_;
        return camera.set_state(state) && app.set_camera(camera, false).has_value();
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

    static void set_scene_maximized(Application& app, bool maximized) {
        auto& dock = app.workspace_.dock;
        if (!dock.select(editor::control::scene_tab))
            throw std::runtime_error("Scene View cannot be selected.");
        for (const auto& pane : dock_presentation(app).panes)
            if (pane.active && pane.active->key == editor::control::scene_tab) {
                if (pane.maximized != maximized)
                    dock.maximize(editor::control::scene_tab);
                return;
            }
        throw std::runtime_error("Scene View has no presented Dock pane.");
    }

    static auto scene_lit_view(const Application& app) { return app.workspace_.view; }

    static Result<void> performance_view(Application& app, const renderer::ViewSettings& view) {
        return app.workspace_.set_view_settings(*app.document_, view, app.lighting_focus());
    }

    static void performance_panel(Application& app) {
        app.workspace_.dock.move(editor::world_control::tab, editor::control::parameters_tab, ui::DockSide::tabs);
        app.workspace_.dock.select(editor::world_control::tab);
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

    static void verify_keep_open_menu(Application& app) {
        app.request_menu({editor::control::scene_guides, 0, 0, 0});
        for (int i = 0; i < 6 && !app.menu_window(); ++i)
            if (!take(app.step()))
                throw std::runtime_error("Editor closed before Guides menu opened.");
        if (!app.menu_window() || !app.popup_ || !app.popup_->presented())
            throw std::runtime_error("Guides popup menu was not presented.");

        const auto find_fps = [&]() -> const ui::MenuItem* {
            const auto at = std::ranges::find_if(app.workspace_.menu_items,
                                                 [](const ui::MenuItem& item) { return item.text == L"Show FPS"; });
            return at == app.workspace_.menu_items.end() ? nullptr : &*at;
        };
        const auto* initial = find_fps();
        if (!initial || initial->selected || app.workspace_.show_fps)
            throw std::runtime_error("Unexpected initial Show FPS menu state.");
        const auto fps_key = initial->key;
        const auto* box = app.popup_->presented()->find(fps_key);
        if (!box)
            throw std::runtime_error("Show FPS item missing from presented popup.");

        const float scale = app.popup_->presented()->drawing ? app.popup_->presented()->drawing->dpi_scale : 1.f;
        POINT pt{static_cast<LONG>(std::lround(box->bounds.x + 6.f * scale)),
                 static_cast<LONG>(std::lround(box->bounds.y + box->bounds.height * .5f))};
        ClientToScreen(app.popup_->window(), &pt);
        ScreenToClient(app.window(), &pt);

        const auto click_checkbox = [&]() {
            ui::Event down;
            down.kind = ui::EventKind::pointer_down;
            down.button = ui::PointerButton::primary;
            down.x = static_cast<float>(pt.x);
            down.y = static_cast<float>(pt.y);
            if (auto r = app.input(down, 0); !r)
                throw std::runtime_error(r.error().message);
            ui::Event up = down;
            up.kind = ui::EventKind::pointer_up;
            if (auto r = app.input(up, 0); !r)
                throw std::runtime_error(r.error().message);
        };

        // 1. Keep-open checkbox click must refresh menu_items and popup presentation on the very next step,
        // even when VSync=1 throttles host presentation (!host_ready).
        click_checkbox();
        if (!take(app.step()))
            throw std::runtime_error("Editor closed during keep-open checkbox toggle.");
        const auto* checked_item = find_fps();
        const auto* checked_box = app.popup_->presented() ? app.popup_->presented()->find(fps_key) : nullptr;
        if (!app.menu_window() || !app.workspace_.show_fps || !checked_item || !checked_item->selected ||
            !checked_box || !checked_box->selected)
            throw std::runtime_error("Keep-open checkbox click left stale popup menu state.");

        // 2. Direct view-revision mutation while the native menu stays open must also invalidate the fast path.
        app.toggle_view(editor::ViewToggleOption::fps);
        if (!take(app.step()))
            throw std::runtime_error("Editor closed after view_revision invalidation.");
        const auto* unchecked_item = find_fps();
        const auto* unchecked_box = app.popup_->presented() ? app.popup_->presented()->find(fps_key) : nullptr;
        if (!app.menu_window() || app.workspace_.show_fps || !unchecked_item || unchecked_item->selected ||
            !unchecked_box || unchecked_box->selected)
            throw std::runtime_error("View revision change while menu was open left stale popup menu state.");

        app.close_menu(false);
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
    static void tool_key(Application& app, std::uint32_t vk, math::Vector2 pointer) {
        ui::Event event;
        event.kind = ui::EventKind::key_down;
        if (!app.viewport_tool_.key(*app.document_, *app.accepted_view_, event, vk, app.workspace_.viewport_settings,
                                    pointer))
            throw std::runtime_error("Snapshot tool key failed.");
    }

    static void tool_move(Application& app, math::Vector2 pointer) {
        ui::Event event;
        event.kind = ui::EventKind::pointer_move;
        event.x = pointer.x;
        event.y = pointer.y;
        if (!app.viewport_tool_.pointer(*app.document_, *app.accepted_view_, event, app.workspace_.viewport_settings))
            throw std::runtime_error("Snapshot tool move failed.");
    }

    static void tool_cancel(Application& app) {
        if (!app.viewport_tool_.cancel(*app.document_))
            throw std::runtime_error("Snapshot cancel failed.");
    }

    static void place_cursor(Application& app, math::Vector2 pointer) {
        ui::Event event;
        event.kind = ui::EventKind::pointer_down;
        event.button = ui::PointerButton::primary;
        event.x = pointer.x;
        event.y = pointer.y;
        auto settings = app.workspace_.viewport_settings;
        settings.tool = editor::TransformTool::select;
        if (!app.viewport_tool_.pointer(*app.document_, *app.accepted_view_, event, settings))
            throw std::runtime_error("Snapshot cursor placement failed.");
        event.kind = ui::EventKind::pointer_up;
        static_cast<void>(app.viewport_tool_.pointer(*app.document_, *app.accepted_view_, event, settings));
    }

    static POINT viewport_point(const Application& app, math::Vector3 point) {
        if (!app.accepted_view_)
            throw std::runtime_error("Viewport has no accepted camera.");
        const auto projected = app.accepted_view_->project(point);
        if (!projected)
            throw std::runtime_error("Viewport point is outside the camera.");
        return {static_cast<LONG>(std::round(projected->x)), static_cast<LONG>(std::round(projected->y))};
    }
};
}

namespace {
using atelier::application::Application;

using Access = atelier::application::EditorAcceptanceAccess;

using atelier::ui::ControlKey;

namespace control = atelier::editor::control;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void hr(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("PNG encoding failed.");
}

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

template <class T>
T model(atelier::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

void png(const atelier::renderer::Capture& image, const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream));
    hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    hr(encoder->CreateNewFrame(&frame, nullptr));
    hr(frame->Initialize(nullptr));
    hr(frame->SetSize(image.extent.width, image.extent.height));
    auto format = GUID_WICPixelFormat32bppBGRA;
    hr(frame->SetPixelFormat(&format));
    require(format == GUID_WICPixelFormat32bppBGRA, "PNG codec changed pixel format.");
    auto bgra = image.rgba;
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4)
        std::swap(bgra[pixel], bgra[pixel + 2]);
    hr(frame->WritePixels(image.extent.height, image.extent.width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    hr(frame->Commit());
    hr(encoder->Commit());
}

atelier::renderer::Capture capture(Application& app) {
    atelier::renderer::Capture image;
    const auto deadline = GetTickCount64() + 10000;
    do {
        // A serviced input wakeup need not have permission to submit a host frame.
        require(take(app.step(&image)), "Editor closed before capture.");
        require(!atelier::application::EditorAcceptanceAccess::view_preparing(app),
                "Capture met a previous scene or unprepared view; tick first.");
        require(image.frame_id || GetTickCount64() < deadline, "Renderer did not capture an accepted frame.");
    } while (!image.frame_id);
    require(image.rgba.size() == static_cast<std::size_t>(image.extent.width) * image.extent.height * 4,
            "Capture has an invalid pixel payload.");
    return image;
}

void capture(Application& app, const std::filesystem::path& path) {
    const auto image = capture(app);
    const auto scene_pixels =
        std::count_if(image.depth.begin(), image.depth.end(), [](float value) { return value > 0 && value <= 1; });
    if (app.presented()->find(control::viewport))
        require(scene_pixels > 32, "Real scene geometry is absent from the depth capture.");
    else
        require(scene_pixels == 0, "Hidden Scene tab continued rendering geometry.");
    png(image, path);
    if (app.menu_window()) {
        atelier::renderer::Capture popup;
        take(app.capture_menu(popup));
        png(popup, path.parent_path() / (path.stem().wstring() + L"-popup.png"));
    }
}
}

void guard_acceptance_native_dialogs() {
    if (!SetWindowsHookExW(WH_CBT, native_dialog_guard, nullptr, GetCurrentThreadId()))
        throw std::runtime_error("Cannot install the acceptance native dialog guard.");
}

int capture_editor_snapshot(Application& app, const char* output_directory) {
    using Access = atelier::application::EditorAcceptanceAccess;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope {
        ~ComScope() { CoUninitialize(); }
    } com_scope;
    const auto output_text = atelier::platform::utf8_to_utf16(output_directory);
    require(bool(output_text), "Invalid UTF-8 evidence directory.");
    const auto output = std::filesystem::absolute(*output_text);
    std::filesystem::create_directories(output);
    const auto settle = [&] {
        for (int i = 0; i < 6; ++i)
            require(take(app.step()), "Snapshot editor closed.");
    };
    const auto shot = [&](const char* name) {
        settle();
        atelier::renderer::Capture image;
        for (int attempt = 0; attempt < 30 && !image.frame_id; ++attempt)
            require(take(app.step(&image)), "Snapshot editor closed.");
        require(image.frame_id != 0, "Snapshot frame was not accepted (window occluded?).");
        png(image, output / name);
    };
    settle();
    model(Access::invoke(app, "scene.add", atelier::json::Document::literal<R"({"kind":"mesh"})">()));
    Access::set_cursor(app, {1.4f, 0, -1.1f});
    Access::set_tool(app, atelier::editor::TransformTool::rotate);
    shot("snapshot-rotate.png");
    Access::set_tool(app, atelier::editor::TransformTool::move);
    shot("snapshot-move.png");
    Access::set_tool(app, atelier::editor::TransformTool::scale);
    shot("snapshot-scale.png");
    Access::set_tool(app, atelier::editor::TransformTool::transform);
    shot("snapshot-transform.png");
    const auto at = [&](atelier::math::Vector3 p) {
        const auto q = Access::viewport_point(app, p);
        return atelier::math::Vector2{float(q.x), float(q.y)};
    };
    // Surface-aligned cursor on the cube's +X face, then modal feedback.
    Access::place_cursor(app, at({.5f, .15f, -.15f}));
    shot("snapshot-cursor-surface.png");
    auto pointer = at({0, 0, 0});
    pointer.x += 60;
    pointer.y -= 40;
    Access::tool_key(app, 'G', pointer);
    Access::tool_key(app, 'Z', pointer);
    pointer.y -= 50;
    Access::tool_move(app, pointer);
    shot("snapshot-modal-grab.png");
    Access::tool_cancel(app);
    Access::tool_key(app, 'R', pointer);
    Access::tool_key(app, 'X', pointer);
    pointer.x += 70;
    Access::tool_move(app, pointer);
    shot("snapshot-modal-rotate.png");
    Access::tool_cancel(app);
    // An enum menu and colour swatch row, through the real light creation command.
    model(Access::invoke(app, "scene.add", atelier::json::Document::literal<R"({"kind":"rectangle"})">()));
    shot("snapshot-light-parameters.png");
    if (const auto pick = Access::color_pick(app)) {
        model(Access::ui_action(app, pick));
        shot("snapshot-color-editor.png");
        model(Access::ui_action(app, atelier::editor::control::color_done));
    }
    // Layout review: category grouping and the Entity Structure page.
    model(Access::ui_action(app, atelier::editor::control::group_categories, true));
    shot("snapshot-parameters-category.png");
    model(Access::ui_action(app, atelier::editor::control::group_components, true));
    Access::select_tab(app, atelier::editor::control::entities_tab);
    model(Access::invoke(app, "scene.rename"));
    shot("snapshot-inline-rename.png");
    Access::cancel_rename(app);
    Access::select_tab(app, atelier::editor::control::structure_tab);
    shot("snapshot-structure.png");
    Access::select_tab(app, atelier::editor::control::scene_tab);
    model(Access::open_menu(app, atelier::editor::control::viewport_snap));
    settle();
    {
        atelier::renderer::Capture menu;
        take(app.capture_menu(menu));
        if (!menu.rgba.empty())
            png(menu, output / "snapshot-snap-menu.png");
    }
    Access::close_menu(app);
    model(Access::open_menu(app, atelier::editor::control::viewport_orientation));
    settle();
    {
        atelier::renderer::Capture menu;
        take(app.capture_menu(menu));
        if (!menu.rgba.empty())
            png(menu, output / "snapshot-orientation-menu.png");
    }
    Access::close_menu(app);
    Access::verify_keep_open_menu(app);
    Access::performance_panel(app);
    shot("snapshot-world.png");
    Access::select_tab(app, atelier::editor::control::workbench);
    shot("snapshot-controls.png");
    Access::mark_saved(app);
    SendMessageW(app.window(), WM_CLOSE, 0, 0);
    while (take(app.step())) {
    }
    std::puts("Editor snapshot: written");
    return 0;
}

#include "performance.inl"
#include "ui_reload_profile.inl"

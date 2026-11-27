#include "../helpers.hpp"
#include "renderer/renderer.hpp"
#include "assets/builtin_materials.hpp"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>

namespace {
namespace render {
using namespace atelier::renderer;
}

namespace ui = atelier::ui;

constexpr auto& check = atelier::acceptance::helpers::require;

template <class T, class E>
T take(std::expected<T, E> result) {
    if constexpr (std::is_same_v<E, render::Error>) {
        if (!result) {
            char message[1024]{};
            std::snprintf(message, sizeof(message), "Renderer error: %.980s (terminal=%u)",
                          result.error().cause.message.c_str(), unsigned(result.error().terminal));
            atelier::fatal(message);
        }
        if constexpr (!std::is_void_v<T>)
            return std::move(*result);
    } else {
        return atelier::acceptance::helpers::take(std::move(result));
    }
}

std::filesystem::path shader_cache_directory() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(length && length < executable.size(), "locate Renderer acceptance shader cache");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"renderer-shader-cache";
}

void pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

render::Result<void> wait_preparation(render::Renderer& renderer, const render::ViewSettings& view) {
    // GPU-based validation keeps the first preparation busy for minutes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
    for (;;) {
        const auto preparation = renderer.preparation(view);
        if (!preparation)
            return std::unexpected(preparation.error());
        if (preparation->ready)
            return {};
        check(std::chrono::steady_clock::now() < deadline, "Scene Lit preparation finished within 20 minutes");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

render::Result<render::FrameResult> prepared_render(render::Renderer& renderer, const atelier::renderer::Frame& frame,
                                                    render::Capture* capture = nullptr) {
    if (const auto ready = wait_preparation(renderer, frame.view); !ready)
        return std::unexpected(ready.error());
    return renderer.render(frame, capture);
}
#include "standard_fixture.inl"

render::Result<render::FrameResult> prepared_render(render::Renderer& renderer, const render::Frame& frame,
                                                    render::Capture* capture = nullptr) {
    return prepared_render(renderer, frame.prepare(renderer), capture);
}

template <class FrameValue>
render::FrameResult accepted_render(render::Renderer& renderer, const FrameValue& frame,
                                    render::Capture* capture = nullptr) {
    auto result = take(prepared_render(renderer, frame, capture));
    check(result.status == render::FrameStatus::accepted, "renderer default frame accepted");
    return result;
}

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

struct WindowClass {
    WNDCLASSW description{};

    WindowClass() {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        description.lpfnWndProc = procedure;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = L"AtelierRendererAcceptance";
        check(RegisterClassW(&description) != 0, "register fixture window");
    }

    ~WindowClass() { UnregisterClassW(description.lpszClassName, description.hInstance); }
};

struct Window {
    HWND hwnd{};

    Window(int width = 641, int height = 385) {
        static const WindowClass window_class;
        const auto& wc = window_class.description;
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"Atelier renderer acceptance", WS_OVERLAPPEDWINDOW, 40, 40, 680,
                               440, nullptr, nullptr, wc.hInstance, nullptr);
        check(hwnd != nullptr, "create fixture window");
        resize(width, height);
        ShowWindow(hwnd, SW_SHOW);
        // CMake may apply STARTF_USESHOWWINDOW/SW_HIDE to the first ShowWindow.
        if (!IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_SHOW);
        pump();
    }

    void resize(int width, int height) {
        RECT r{0, 0, width, height};
        check(AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, 0,
                                       GetDpiForWindow(hwnd)) != 0,
              "adjust fixture extent");
        check(SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                           SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED) != 0,
              "resize fixture");
        pump();
    }

    ~Window() { DestroyWindow(hwnd); }
};

const std::uint8_t* pixel(const render::Capture& c, unsigned x, unsigned y) {
    return c.rgba.data() + (std::size_t(y) * c.extent.width + x) * 4;
}

float depth(const render::Capture& c, unsigned x, unsigned y) {
    return c.depth[std::size_t(y) * c.extent.width + x];
}

atelier::math::Matrix4 translation(float x, float y, float z) {
    return atelier::math::translation_matrix(atelier::math::Vector3{x, y, z});
}

void snapshot(const render::Capture& c, const char* path) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << c.extent.width << ' ' << c.extent.height << "\n255\n";
    for (std::size_t i = 0; i < c.rgba.size(); i += 4)
        out.write(reinterpret_cast<const char*>(c.rgba.data() + i), 3);
    check(out.good(), "write capture evidence");
}

#include "ui_shapes.inl"
#include "selection.inl"
#include "viewport_modes.inl"
#include "scene_lit.inl"
} // namespace
#include "material_batches.inl"

int main(int argc, char** argv) {
    atelier::install_fatal_handler();
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1) {
        const std::string_view mode{argv[1]};
        check(
            (argc == 2 || (argc == 4 && std::string_view{argv[2]} == "--output")) &&
                (mode == "--scene-lit" || mode == "--scene-lit-benchmark" || mode == "--material-programs"),
            "usage: atelier_renderer_acceptance [--scene-lit|--scene-lit-benchmark|--material-programs [--output <absolute-directory>]]");
        const std::filesystem::path output =
            argc == 4 ? std::filesystem::path{argv[3]}
                      : std::filesystem::path{mode == "--material-programs" ? "out/material-programs"
                                                                            : "out/scene-lit-20261008"};
        check(argc != 4 || output.is_absolute(), "Renderer --output must be an absolute evidence directory");
        if (mode == "--material-programs") {
            Window window;
            material_batch_e2e::run(window.hwnd, output);
            return 0;
        }
        return scene_lit_acceptance(mode == "--scene-lit-benchmark", output);
    }
    Window window(703, 427);
    auto renderer = take(render::Renderer::open(
        window.hwnd,
        {.validation = atelier::rhi::Validation::gpu, .vsync = false, .shader_cache = shader_cache_directory()}));
    ui::Frame ui_frame{.id = 1, .extent = {703, 427}, .draws = {}};
    std::array<render::MeshInstance, 1> cubes{
        {{translation(0, 0, 2), {1, 0, 0, 1}, false, atelier::geometry::cube_mesh()}}};
    render::Camera camera;
    camera.projection[10] = -.1f;
    render::Frame frame{ui_frame, camera, {120, 40, 480, 320}, cubes};
    render::Capture capture;
    // Non-diagonal view/projection values verify the public row-vector LH ABI.
    frame.camera = {};
    frame.camera.view[14] = 4;
    frame.camera.projection = {{2.0f / 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, -.1f / 9.9f, 1, 0, 0, 1.0f / 9.9f, 0}};
    cubes[0].world = translation(0, 0, 0);
    ++ui_frame.id;
    accepted_render(renderer, frame, &capture);
    check(std::abs(depth(capture, 360, 200) - (1.0f / (9.9f * 3.5f) - .1f / 9.9f)) < .0001f,
          "left-handed row-major view and perspective projection");
    frame.release_materials();
    ui_shape_cases(renderer, ui_frame.extent);
    selection_clipping_cases(renderer, ui_frame.extent);
    viewport_cases(renderer, ui_frame.extent);
    take(renderer.close());
    std::puts("Renderer acceptance: PASS (camera, UI shapes, selection and viewport images)");
}

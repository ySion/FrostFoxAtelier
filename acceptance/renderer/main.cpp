#include "memory/fatal.hpp"
#include "renderer/renderer.hpp"
#include "assets/builtin_materials.hpp"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
namespace render {
using namespace atelier::renderer;
}

namespace ui = atelier::ui;

void check(bool condition, const char* text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}

std::filesystem::path shader_cache_directory() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(length && length < executable.size(), "locate Renderer acceptance shader cache");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"renderer-shader-cache";
}

template <class T>
T take(render::Result<T> result) {
    if (!result) {
        throw std::runtime_error("Renderer error: " + result.error().cause.message +
                                 " (terminal=" + std::to_string(result.error().terminal) + ")");
    }
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
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

std::array<float, 16> translation(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

void snapshot(const render::Capture& c, const char* path) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << c.extent.width << ' ' << c.extent.height << "\n255\n";
    for (std::size_t i = 0; i < c.rgba.size(); i += 4)
        out.write(reinterpret_cast<const char*>(c.rgba.data() + i), 3);
    check(out.good(), "write capture evidence");
}

void lit_preparation_case(render::Renderer& renderer) {
    const auto extent = take(renderer.target()).extent;
    ui::Frame drawing{.id = 900, .extent = extent, .draws = {}};
    drawing.draws.push_back({{4, 4, 16, 16}, {4, 4, 20, 20}, {0, 1, 0, 1}, {0, 0, 1, 1}, {}});
    std::array<render::MeshInstance, 1> cube{
        {{translation(0, 0, 3), {1, 1, 1, 1}, false, atelier::geometry::cube_mesh()}}};
    render::Camera camera;
    camera.projection[10] = -.1f;
    render::Frame frame{drawing, camera, {40, 40, float(extent.width) - 80, float(extent.height) - 80}, cube};
    frame.view.shading = render::Shading::scene_lit;
    frame.scene_identity = 900;
    render::Capture capture;
    capture.frame_id = 7;
    const auto first = take(renderer.render(frame.prepare(renderer), &capture));
    check(first.status == render::FrameStatus::not_ready && first.accepted_id == 0 &&
              first.accepted_extent == ui::PixelExtent{} && first.stats.ui_draw_calls == 0 &&
              first.stats.scene_draw_calls == 0 && first.stats.viewport_draw_calls == 0 && capture.frame_id == 7,
          "an unprepared Lit frame returns not_ready without encoding, presenting or capturing");
    frame.view.shading = render::Shading::smooth;
    ++drawing.id;
    const auto studio = take(renderer.render(frame.prepare(renderer), &capture));
    check(studio.status == render::FrameStatus::accepted && capture.frame_id == drawing.id,
          "a studio view renders while Lit pipelines prepare");
    frame.view.shading = render::Shading::scene_lit;
    take(wait_preparation(renderer, frame.view));
    ++drawing.id;
    const auto lit = take(renderer.render(frame.prepare(renderer), &capture));
    check(lit.status == render::FrameStatus::accepted && capture.frame_id == drawing.id,
          "the prepared Lit view renders normally");
    std::puts("Renderer Lit preparation: PASS (cold not_ready, studio while preparing, ready Lit)");
}

#include "ui_shapes.inl"
#include "selection.inl"
#include "viewport_modes.inl"
#include "scene_lit.inl"
} // namespace
#include "materials.inl"
#include "material_batches.inl"

namespace material_program_e2e {
void run(HWND, const std::filesystem::path&);
}

int main(int argc, char** argv) try {
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
            material_program_e2e::run(window.hwnd, output);
            material_batch_e2e::run(window.hwnd, output);
            return 0;
        }
        return scene_lit_acceptance(mode == "--scene-lit-benchmark", output);
    }
    Window window;
    const auto shader_cache = shader_cache_directory();
    {
        // Keep first-render not_ready coverage cold even on a warm run.
        const auto cold_cache = shader_cache.parent_path() / L"renderer-preparation-shader-cache";
        std::filesystem::remove_all(cold_cache);
        auto renderer = take(render::Renderer::open(
            window.hwnd, {.validation = atelier::rhi::Validation::gpu, .vsync = false, .shader_cache = cold_cache}));
        lit_preparation_case(renderer);
        take(renderer.close());
    }
    auto renderer = take(render::Renderer::open(
        window.hwnd, {.validation = atelier::rhi::Validation::gpu, .vsync = false, .shader_cache = shader_cache}));
    material_cases(renderer);
    ui::Frame ui_frame{.id = 1, .extent = {641, 385}, .draws = {}};
    std::array<render::MeshInstance, 1> cubes{
        {{translation(0, 0, 2), {1, 0, 0, 1}, false, atelier::geometry::cube_mesh()}}};
    render::Camera camera;
    camera.projection[10] = -.1f;
    render::Frame frame{ui_frame, camera, {120, 40, 480, 320}, cubes};
    render::Capture capture;
    take(prepared_render(renderer, frame));
    window.resize(703, 427);
    take(prepared_render(renderer, frame));
    ui_frame.extent = {703, 427};
    ++ui_frame.id;
    const auto resized = take(prepared_render(renderer, frame, &capture));
    check(resized.status == render::FrameStatus::accepted && capture.extent == ui::PixelExtent{703, 427},
          "rebuild depth for resized target");
    // Non-diagonal view/projection values verify the public row-vector LH ABI.
    frame.camera = {};
    frame.camera.view[14] = 4;
    frame.camera.projection = {2.0f / 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, -.1f / 9.9f, 1, 0, 0, 1.0f / 9.9f, 0};
    cubes[0].world = translation(0, 0, 0);
    ++ui_frame.id;
    take(prepared_render(renderer, frame, &capture));
    check(std::abs(depth(capture, 360, 200) - (1.0f / (9.9f * 3.5f) - .1f / 9.9f)) < .0001f,
          "left-handed row-major view and perspective projection");
    frame.release_materials();
    ui_shape_cases(renderer, ui_frame.extent);
    selection_clipping_cases(renderer, ui_frame.extent);
    viewport_cases(renderer, ui_frame.extent);
    // K6: a shutdown request is not a retryable or accepted frame. The expected
    // capture is authored here, not copied from a previous renderer result.
    const std::function<bool()> stop_requested = [] { return true; };
    atelier::renderer::Frame cancelled_frame{ui_frame, {}, {}, {}};
    cancelled_frame.stop_requested = &stop_requested;
    render::Capture cancelled_capture{
        .frame_id = 991, .extent = {1, 1}, .rgba = {17, 43, 91, 255}, .depth = {}, .motion = {}};
    const auto cancelled = take(renderer.render(cancelled_frame, &cancelled_capture));
    check(cancelled.status == render::FrameStatus::cancelled && cancelled.accepted_id == 0 &&
              cancelled.accepted_extent == ui::PixelExtent{} && cancelled.stats.ui_draw_calls == 0 &&
              cancelled.stats.scene_draw_calls == 0 && cancelled.stats.viewport_draw_calls == 0 &&
              cancelled_capture.frame_id == 991 && cancelled_capture.extent == ui::PixelExtent{1, 1} &&
              cancelled_capture.rgba == std::vector<std::uint8_t>{17, 43, 91, 255},
          "shutdown cancellation did not encode, present or publish a capture");
    auto closed = renderer.close();
    check(bool(closed), "drain and close a cancelled Renderer before HWND destruction");
    std::puts(
        "Renderer acceptance: PASS (cold preparation, resize, camera ABI, UI, selection, ownership, cancellation)");
} catch (const std::exception& error) {
    std::fprintf(stderr, "Renderer acceptance FAILED: %s\n", error.what());
    return 1;
}

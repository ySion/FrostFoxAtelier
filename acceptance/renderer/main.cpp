#include "renderer/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <fstream>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
namespace render = atelier::renderer;
namespace ui = atelier::ui;
void check(bool condition, const char *text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}
template <class T> T take(render::Result<T> result) {
    if (!result) {
        throw std::runtime_error("Renderer error: " + result.error().cause.message +
            " (terminal=" + std::to_string(result.error().terminal) + ")");
    }
    if constexpr (!std::is_void_v<T>) return std::move(*result);
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
        if (!preparation) return std::unexpected(preparation.error());
        if (preparation->ready) return {};
        check(std::chrono::steady_clock::now() < deadline, "Scene Lit preparation finished within 20 minutes");
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
// Cases check rendering, not preparation: Lit views wait for their pipelines first.
render::Result<render::FrameResult> prepared_render(render::Renderer& renderer, const render::Frame& frame,
                                                    render::Capture* capture = nullptr) {
    if (const auto ready = wait_preparation(renderer, frame.view); !ready) return std::unexpected(ready.error());
    return renderer.render(frame, capture);
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
    ~WindowClass() {
        UnregisterClassW(description.lpszClassName, description.hInstance);
    }
};
struct Window {
    HWND hwnd{};
    Window(int width = 641, int height = 385) {
        // A process may host several independent Renderer consumers at once.
        // The shared class outlives every fixture HWND, including failed cases.
        static const WindowClass window_class;
        const auto& wc = window_class.description;
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"Atelier renderer acceptance", WS_OVERLAPPEDWINDOW, 40,
                               40, 680, 440, nullptr, nullptr, wc.hInstance, nullptr);
        check(hwnd != nullptr, "create fixture window");
        resize(width, height);
        ShowWindow(hwnd, SW_SHOW);
        // CMake may start the fixture with STARTF_USESHOWWINDOW/SW_HIDE; Windows
        // applies that override to the first ShowWindow regardless of its argument.
        if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
        pump();
    }
    void resize(int width, int height) {
        RECT r{0, 0, width, height};
        check(AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE)),
                                     FALSE, 0, GetDpiForWindow(hwnd)) != 0,
              "adjust fixture extent");
        check(SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                           SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED) != 0,
              "resize fixture");
        pump();
    }
    ~Window() {
        DestroyWindow(hwnd);
    }
};
const std::uint8_t *pixel(const render::Capture &c, unsigned x, unsigned y) {
    return c.rgba.data() + (std::size_t(y) * c.extent.width + x) * 4;
}
float depth(const render::Capture &c, unsigned x, unsigned y) {
    return c.depth[std::size_t(y) * c.extent.width + x];
}
std::array<float, 16> translation(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}
void snapshot(const render::Capture &c, const char* path = "renderer-acceptance.ppm") {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << c.extent.width << ' ' << c.extent.height << "\n255\n";
    for (std::size_t i = 0; i < c.rgba.size(); i += 4)
        out.write(reinterpret_cast<const char *>(c.rgba.data() + i), 3);
    check(out.good(), "write capture evidence");
}
void lit_preparation_case(render::Renderer &renderer) {
    const auto extent = take(renderer.target()).extent;
    ui::Frame drawing{.id = 900, .extent = extent, .draws = {}};
    drawing.draws.push_back({{4, 4, 16, 16}, {4, 4, 20, 20}, {0, 1, 0, 1}, {0, 0, 1, 1}, {}});
    std::array<render::MeshInstance, 1> cube{{{translation(0, 0, 3), {1, 1, 1, 1}, false, atelier::geometry::cube_mesh()}}};
    render::Camera camera;
    camera.projection[10] = -.1f;
    render::Frame frame{drawing, camera, {40, 40, float(extent.width) - 80, float(extent.height) - 80}, cube};
    frame.view.shading = render::Shading::scene_lit;
    frame.scene_identity = 900;
    render::Capture capture;
    capture.frame_id = 7;
    const auto started = std::chrono::steady_clock::now();
    const auto first = take(renderer.render(frame, &capture));
    const auto first_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    check(first.status == render::FrameStatus::not_ready && first.accepted_id == 0 && first.accepted_extent == ui::PixelExtent{} &&
              first.stats.ui_draw_calls == 0 && first.stats.scene_draw_calls == 0 && first.stats.viewport_draw_calls == 0 &&
              capture.frame_id == 7,
          "an unprepared Lit frame returns not_ready without encoding, presenting or capturing");
    check(first_ms < 2000, "starting Lit preparation does not wait for compilation");
    const auto progress = take(renderer.preparation(frame.view));
    check(!progress.ready && progress.total == 38 && progress.done < progress.total,
          "the default Lit view reports its own pipeline groups while they prepare");
    auto reference = frame.view;
    reference.lit.direct_reference = true;
    check(take(renderer.preparation(reference)).total == 39, "the direct reference adds only its ray-traced shading pipeline");
    frame.view.shading = render::Shading::smooth;
    ++drawing.id;
    const auto studio = take(renderer.render(frame, &capture));
    check(studio.status == render::FrameStatus::accepted && capture.frame_id == drawing.id,
          "a studio view renders while Lit pipelines prepare");
    frame.view.shading = render::Shading::scene_lit;
    take(wait_preparation(renderer, frame.view));
    const auto ready_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    ++drawing.id;
    const auto lit = take(renderer.render(frame, &capture));
    check(lit.status == render::FrameStatus::accepted && capture.frame_id == drawing.id && lit.stats.scene_draw_calls == 1,
          "the prepared Lit view renders normally");
    frame.view.lit.probe_debug = true;
    const auto grown = take(renderer.preparation(frame.view));
    ++drawing.id;
    const auto pending = take(renderer.render(frame, &capture));
    check(!grown.ready && grown.total == 39 && grown.done == 38 && pending.status == render::FrameStatus::not_ready &&
              capture.frame_id == drawing.id - 1,
          "a setting that needs a new group raises total and returns the ready view to not_ready");
    take(wait_preparation(renderer, frame.view));
    ++drawing.id;
    check(take(renderer.render(frame, &capture)).status == render::FrameStatus::accepted && capture.frame_id == drawing.id,
          "the view renders once its new group is ready");
    std::printf("Renderer Lit preparation: first-render=not_ready %.1f ms progress=%u/%u studio-while-preparing=accepted ready=%.1f s "
                "new-group=%u/%u not_ready PASS\n",
                first_ms, progress.done, progress.total, ready_s, grown.done, grown.total);
}
void ui_only_cases(render::Renderer &renderer, Window &window) {
    const auto initial_extent = take(renderer.target()).extent;
    ui::Frame ui_frame{.id = 3000, .extent = initial_extent, .draws = {}};
    ui_frame.draws.push_back({{0, 0, 24, 24}, {0, 0, 24, 24}, {1, 0, 0, 1}, {0, 0, 1, 1}, {}});
    render::Frame frame{ui_frame, {}, {}, {}};
    auto result = take(prepared_render(renderer,frame));
    check(result.status == render::FrameStatus::accepted && result.stats.scene_draw_calls == 0 &&
              result.stats.ui_draw_calls == 1,
          "cold renderer accepts an ordinary UI-only frame");
    render::Capture capture;
    auto capture_ui = [&] {
        ++ui_frame.id;
        const auto captured = take(prepared_render(renderer,frame, &capture));
        check(captured.status == render::FrameStatus::accepted && captured.accepted_id == ui_frame.id &&
                  capture.frame_id == ui_frame.id && capture.extent == ui_frame.extent &&
                  captured.stats.scene_draw_calls == 0 && captured.stats.ui_draw_calls == 1,
              "UI-only capture retains accepted metadata and draw counts");
        check(capture.depth.size() == std::size_t(capture.extent.width) * capture.extent.height &&
                  std::ranges::all_of(capture.depth, [](float value) { return value == 0; }) &&
                  pixel(capture, 12, 12)[0] == 255 && pixel(capture, 30, 30)[0] == 0,
              "UI-only capture has rendered color and uniform background depth");
    };
    capture_ui();
    std::array<render::MeshInstance, 1> meshes{};
    meshes[0].geometry = atelier::geometry::cube_mesh();
    frame.meshes = meshes;
    capture_ui(); // A zero viewport still has no scene, even with valid mesh input.
    frame.meshes = {};
    for (const auto extent : {ui::PixelExtent{503, 317}, initial_extent}) {
        window.resize(static_cast<int>(extent.width), static_cast<int>(extent.height));
        ++ui_frame.id;
        result = take(prepared_render(renderer,frame, &capture));
        check(result.status == render::FrameStatus::accepted && result.accepted_extent == ui_frame.extent &&
                  capture.extent == ui_frame.extent && result.next_extent == extent,
              "UI-only resize captures the rendered target before advancing its extent");
        const auto captured_id = capture.frame_id;
        result = take(prepared_render(renderer,frame, &capture));
        check(result.status == render::FrameStatus::skipped && capture.frame_id == captured_id,
              "UI-only extent mismatch retains the previous capture");
        ui_frame.extent = extent;
        capture_ui();
    }
    std::puts("Renderer UI-only: PASS (cold render/capture, zero viewport, resize, skipped capture)");
}
void transform_cases(render::Renderer &renderer, ui::PixelExtent extent) {
    ui::Frame ui_frame{.id = 1000, .extent = extent, .draws = {}};
    std::array<render::MeshInstance, 2> cubes{};
    for (auto& mesh : cubes) mesh.geometry = atelier::geometry::cube_mesh();
    cubes[0].world = translation(0, 0, 2);
    cubes[0].color = {1, 1, 1, 1};
    render::Camera camera;
    camera.projection[10] = -.1f;
    render::Frame frame{ui_frame, camera, {120, 40, 480, 320}, {cubes.data(), 1}};
    render::Capture capture;
    auto render_capture = [&] {
        ++ui_frame.id;
        const auto result = take(prepared_render(renderer,frame, &capture));
        check(result.status == render::FrameStatus::accepted && capture.frame_id == ui_frame.id &&
                  result.stats.scene_draw_calls == 1 && result.stats.ui_draw_calls == 0,
              "transformed scene accepted through the same instanced draw path");
    };
    render_capture();
    const auto front_color = pixel(capture, 360, 200)[0];
    // Scale (2,.5,1.5), then rotate 90 degrees about Y, then translate +2 Z.
    // The visible front face now has local +X normal, not the original -Z.
    cubes[0].world = {0, 0, -2, 0, 0, .5f, 0, 0, 1.5f, 0, 0, 0, 0, 0, 2, 1};
    render_capture();
    check(std::abs(depth(capture, 360, 200) - .9f) < .0001f &&
              std::abs(depth(capture, 510, 200) - .9f) < .0001f && depth(capture, 550, 200) == 0 &&
              depth(capture, 360, 250) == 0,
          "rotated nonuniform cube has transformed silhouette and depth");
    check(std::abs(int(pixel(capture, 360, 200)[0]) - int(front_color)) <= 1,
          "rotation transforms local +X into the outward front normal");

    // An affine hierarchy can shear. Its front plane is z=.5*x+1.5, whose
    // outward normal is (.5,0,-1), unlike the transformed local normal vector.
    cubes[0].world = {1, 0, .5f, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 2, 1};
    render_capture();
    const float sample_x = (420.5f - 120) / 240 - 1;
    const double linear = .3 + .7 * .8 / std::sqrt(1.25 * 1.16);
    const int expected_color =
        static_cast<int>(std::lround((1.055 * std::pow(linear, 1 / 2.4) - .055) * 255));
    check(std::abs(depth(capture, 420, 200) - (.85f - .05f * sample_x)) < .00001f &&
              std::abs(int(pixel(capture, 420, 200)[0]) - expected_color) <= 2,
          "affine hierarchy shear uses inverse-transpose normal direction");
    const auto unreflected = capture;
    for (int component = 0; component < 3; ++component)
        cubes[0].world[component] *= -1;
    render_capture();
    for (unsigned x : {300u, 360u, 420u})
        for (unsigned y : {160u, 200u, 240u}) {
            check(std::abs(depth(capture, x, y) - depth(unreflected, x, y)) < .00001f &&
                      std::abs(int(pixel(capture, x, y)[0]) - int(pixel(unreflected, x, y)[0])) <= 1,
                  "negative determinant preserves outward lighting and mirrored winding visibility");
        }

    // Finite small/large positive scales are not rejected by an arbitrary
    // determinant epsilon. Projection compensates scale to keep the cube visible.
    for (float scale : {1e-20f, 1e20f}) {
        cubes[0].world = {0, 0, -scale, 0, 0, scale, 0, 0, scale, 0, 0, 0, 0, 0, 2 * scale, 1};
        frame.camera.projection = {1 / scale, 0, 0, 0, 0, 1 / scale, 0, 0, 0, 0, -.1f / scale, 0, 0, 0, 1, 1};
        render_capture();
        check(std::abs(depth(capture, 360, 200) - .85f) < .0001f &&
                  std::abs(int(pixel(capture, 360, 200)[0]) - int(front_color)) <= 1,
              "finite extreme scales retain visible geometry and finite unit normals");
    }
    frame.camera = camera;
    cubes[0].world = translation(0, 0, 2);
    render_capture();
    const auto previous_id = capture.frame_id;
    frame.meshes = cubes;
    std::array<std::array<float, 16>, 6> invalid{};
    invalid[0] = {}; // Not affine (all zero).
    invalid[1] = translation(0, 0, 3);
    invalid[1][0] = 0; // Zero scale, singular but otherwise affine.
    invalid[2] = translation(0, 0, 3);
    invalid[2][0] = std::numeric_limits<float>::infinity();
    invalid[3] = translation(0, 0, 3);
    invalid[3][3] = .1f; // Perspective belongs to Camera, not world transforms.
    invalid[4] = translation(0, 0, 3);
    invalid[4][4] = 1;
    invalid[4][5] = 0; // Linearly dependent basis rows despite nonzero entries.
    invalid[5] = translation(0, 0, 3);
    invalid[5][0] = 1e-30f; invalid[5][5] = 1e30f; // Double invertible; FP32 normal matrix loses rank.
    for (const auto &matrix : invalid) {
        cubes[1].world = matrix;
        ++ui_frame.id;
        const auto rejected = prepared_render(renderer,frame, &capture);
        check(!rejected && !rejected.error().terminal &&
                  rejected.error().cause.code == atelier::rhi::ErrorCode::invalid_argument &&
                  capture.frame_id == previous_id,
              "bad second world matrix rejects the whole frame before encoding and retains capture");
    }
    frame.meshes = {cubes.data(), 1};
    render_capture();
    check(std::abs(depth(capture, 360, 200) - .85f) < .0001f,
          "valid world frame remains usable after rejected candidates");
    std::puts("Renderer world transforms: PASS (rotation, nonuniform scale, shear normals, reflection, "
              "extreme scale, invalid-frame recovery)");
}
void mesh_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    auto geometry = std::make_shared<const atelier::geometry::Mesh>(std::vector<atelier::geometry::Vertex>{
        {{-.05f,-.5f,0},{1,0,-1}}, {{.05f,-.5f,0},{1,0,-1}}, {{0,.5f,0},{1,0,-1}}});
    std::weak_ptr<const atelier::geometry::Mesh> observed = geometry;
    std::array<render::MeshInstance,3> meshes{};
    for (auto& mesh : meshes) { mesh.world=translation(30,0,2); mesh.color={1,1,1,1}; }
    meshes[0].world=translation(0,0,2); meshes[0].world[0]=8;
    meshes[0].geometry=geometry; meshes[1].geometry=geometry; meshes[2].geometry=atelier::geometry::cube_mesh();
    ui::Frame ui_frame{.id=2000,.extent=extent,.draws={}};
    render::Camera camera; camera.projection[10]=-.1f;
    render::Frame frame{ui_frame,camera,{120,40,480,320},meshes};
    render::Capture capture;
    const auto result=take(prepared_render(renderer,frame,&capture));
    check(result.status==render::FrameStatus::accepted && result.stats.scene_draw_calls==1 &&
        result.stats.scene_instances==3 && result.stats.scene_culled_instances==2,
        "only the visible mesh is submitted while off-camera geometry retains scene membership");
    const double linear=.3+.7*.65/std::sqrt(1.015625*1.16);
    const int expected=static_cast<int>(std::lround((1.055*std::pow(linear,1/2.4)-.055)*255));
    check(std::abs(depth(capture,360,200)-.8f)<.00001f && std::abs(int(pixel(capture,360,200)[0])-expected)<=2,
        "arbitrary triangle geometry and non-axis normal use the full inverse-transpose under nonuniform scale");
    check(depth(capture,500,240)==0, "triangle does not cover the replacement rectangle's outer area");
    auto replacement=std::make_shared<const atelier::geometry::Mesh>(std::vector<atelier::geometry::Vertex>{
        {{-.1f,-.5f,0},{0,0,-1}},{{.1f,-.5f,0},{0,0,-1}},{{.1f,.5f,0},{0,0,-1}},
        {{-.1f,-.5f,0},{0,0,-1}},{{.1f,.5f,0},{0,0,-1}},{{-.1f,.5f,0},{0,0,-1}}});
    std::weak_ptr<const atelier::geometry::Mesh> replacement_observer=replacement;
    meshes[0].geometry=replacement; meshes[1].geometry=replacement;
    meshes[0].world[14]=2.5f; geometry.reset(); ++ui_frame.id;
    const auto replaced=take(prepared_render(renderer,frame,&capture));
    check(replaced.status==render::FrameStatus::accepted && replaced.stats.scene_draw_calls==1 &&
        std::abs(depth(capture,500,240)-.75f)<.00001f && observed.expired(),
        "new immutable mesh version changes silhouette/depth and retires its old CPU owner");
    const auto accepted_id=capture.frame_id;
    meshes[0].geometry.reset(); ++ui_frame.id;
    auto invalid=prepared_render(renderer,frame,&capture);
    check(!invalid && !invalid.error().terminal && capture.frame_id==accepted_id,
        "missing mesh resource rejects the entire candidate before encoding");
    frame.meshes={}; meshes[1].geometry.reset(); replacement.reset(); ++ui_frame.id;
    const auto empty=take(prepared_render(renderer,frame));
    check(empty.stats.scene_draw_calls==0 && replacement_observer.expired(), "unused geometry CPU/GPU cache ownership retires");
    std::puts("Renderer meshes: PASS (shared geometry batches, arbitrary normals, immutable version replacement, missing-resource rejection, retirement)");
}
void visibility_cases(render::Renderer& renderer,ui::PixelExtent extent) {
    ui::Frame drawing{.id=2500,.extent=extent,.draws={}};
    render::Camera camera;camera.projection[10]=-.1f;
    std::array<render::MeshInstance,3> meshes{};
    for(auto& mesh:meshes)mesh.geometry=atelier::geometry::cube_mesh();
    meshes[0].world=translation(-.5f,0,2);meshes[0].color={1,0,0,1};
    meshes[1].world=translation(8,0,2);
    meshes[2].world=translation(.5f,0,3);meshes[2].color={0,0,1,1};
    render::Frame frame{drawing,camera,{12.25f,20.75f,480.5f,320.25f},meshes};
    frame.view.cavity.enabled=false;
    render::Capture image;
    const auto capture=[&] {++drawing.id;return take(prepared_render(renderer,frame,&image)).stats;};
    auto stats=capture();const auto reference=image;
    check(stats.scene_instances==3 && stats.scene_culled_instances==1 && stats.scene_draw_calls==1,
        "a visibility gap keeps one geometry draw that reads original instance indices");
    std::array<render::MeshInstance,2> visible{meshes[0],meshes[2]};frame.meshes=visible;
    stats=capture();
    check(stats.scene_draw_calls==1 && image.rgba==reference.rgba && image.depth==reference.depth,
        "frustum filtering preserves material identity, pixel-center coverage, color and depth");
    // Another geometry between two instances in the Frame does not split their draw.
    const auto wedge=std::make_shared<const atelier::geometry::Mesh>(std::vector<atelier::geometry::Vertex>{
        {{-.3f,-.3f,0},{0,0,-1}},{{.3f,-.3f,0},{0,0,-1}},{{0,.3f,0},{0,0,-1}}});
    std::array<render::MeshInstance,3> separated{meshes[0],meshes[0],meshes[2]};
    separated[1].geometry=wedge;separated[1].world=translation(0,.4f,2.5f);separated[1].color={0,1,0,1};
    frame.meshes=separated;stats=capture();const auto grouped=image;
    std::array<render::MeshInstance,3> adjacent{separated[0],separated[2],separated[1]};
    frame.meshes=adjacent;
    const auto adjacent_stats=capture();
    check(stats.scene_draw_calls==2 && adjacent_stats.scene_draw_calls==2 && image.rgba==grouped.rgba && image.depth==grouped.depth,
        "instances of one geometry share a draw across other geometries and keep their own transforms and materials");
    frame.meshes=meshes;
    meshes[1].world=translation(0,0,1);stats=capture();
    check(stats.scene_culled_instances==0 && stats.scene_draw_calls==1 && image.depth!=reference.depth,
        "an object reentering the view is drawn immediately in its original snapshot position");
    meshes[1].world=translation(8,0,2);meshes[1].world[0]=0;
    ++drawing.id;const auto invalid=prepared_render(renderer,frame);
    check(!invalid && !invalid.error().terminal,
        "off-camera singular transforms still reject the complete frame before encoding");
    std::array<render::MeshInstance,6> outside{};
    const std::array<std::array<float,3>,6> positions{{{-3,0,2},{3,0,2},{0,-3,2},{0,3,2},{0,0,-2},{0,0,12}}};
    for(std::size_t i=0;i<outside.size();++i) {
        outside[i].geometry=atelier::geometry::cube_mesh();
        outside[i].world=translation(positions[i][0],positions[i][1],positions[i][2]);
    }
    frame.meshes=outside;stats=capture();
    check(stats.scene_culled_instances==6 && stats.scene_draw_calls==0 &&
        std::ranges::all_of(image.depth,[](float d){return d==0;}),
        "all six homogeneous clip planes reject fully outside objects and clear stale depth");
    frame.meshes=std::span(outside).first(1);
    outside[0].world=translation(.99f,0,2);outside[0].world[0]=-.1f;outside[0].world[5]=2;
    stats=capture();
    check(stats.scene_culled_instances==0 && stats.scene_draw_calls==1 &&
        std::ranges::any_of(image.depth,[](float d){return d>0;}),
        "a reflected nonuniform box crossing a side plane stays visible");
    frame.camera.projection={1,0,0,0,0,1,0,0,0,0,-.01f,1,0,0,.1f,0};
    outside[0].world=translation(0,0,.3f);stats=capture();
    check(stats.scene_culled_instances==0 && stats.scene_draw_calls==1,
        "a perspective box crossing the eye and near plane is conservatively retained");
    frame.camera=camera;frame.meshes=meshes;
    meshes[1].world=translation(8,0,2);meshes[1].material.emissive={4,2,1};
    frame.view.shading=render::Shading::scene_lit;frame.scene_identity=drawing.id;
    auto& lit=frame.view.lit;
    lit.ddgi=lit.gtao=lit.sssr=lit.ssil=lit.bloom=false;
    lit.antialiasing=render::Antialiasing::none;
    stats=capture();
    check(stats.scene_instances==3 && stats.scene_culled_instances==1 && stats.scene_draw_calls==1 &&
        stats.scene_lit_light_sources>0 && std::ranges::any_of(image.depth,[](float d){return d>0;}),
        "Lit filters primary GBuffer draws while retaining off-camera emitters in the complete light domain");
    frame.meshes={};stats=capture();
    check(stats.scene_instances==0 && stats.scene_culled_instances==0 && stats.scene_draw_calls==0,
        "empty scenes reset visibility activity without carrying prior-frame counters");
    std::puts("Renderer visibility: PASS (geometry groups, identical pixels, reentry, invalid input, six planes, reflected scale, eye crossing, off-camera light domain, empty scene)");
}
#include "ui_shapes.inl"
#include "selection.inl"
#include "viewport_modes.inl"
#include "reference_guides.inl"
#include "scene_lit.inl"
} // namespace
#include "materials.inl"
#include "cavity.inl"

int main(int argc, char** argv) try {
    // Automated GPU failures must terminate with diagnostics, never wait for
    // someone to dismiss a CRT dialog while the failed device is still owned.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_CALL_REPORTFAULT);
    SetErrorMode(GetErrorMode()|SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    (void)_CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1) {
        const std::string_view mode{argv[1]};
        check((argc == 2 || (argc == 4 && std::string_view{argv[2]} == "--output")) &&
                  (mode == "--scene-lit" || mode == "--scene-lit-benchmark" || mode == "--scene-lit-temporal" || mode == "--scene-lit-quality" || mode == "--scene-lit-restir-suite" || mode == "--scene-lit-clustered" || mode == "--scene-lit-shadows" || mode == "--scene-lit-stability" || mode == "--scene-lit-cost"),
              "usage: atelier_renderer_acceptance [--scene-lit|--scene-lit-benchmark|--scene-lit-temporal|--scene-lit-quality|--scene-lit-restir-suite|--scene-lit-clustered|--scene-lit-shadows|--scene-lit-stability|--scene-lit-cost [--output <absolute-directory>]]");
        const std::filesystem::path output = argc == 4 ? std::filesystem::path{argv[3]}
                                                     : std::filesystem::path{"out/scene-lit-20261008"};
        check(argc != 4 || output.is_absolute(), "SceneLit --output must be an absolute evidence directory");
        if(mode=="--scene-lit-cost") {
            scene_lit_e2e::Fixture fixture(false,output);
            scene_lit_e2e::cost_cases(fixture);fixture.close();return 0;
        }
        if(mode=="--scene-lit-clustered") {
            scene_lit_e2e::Fixture fixture(false,output);
            scene_lit_e2e::clustered_cases(fixture);fixture.close();return 0;
        }
        if(mode=="--scene-lit-shadows") {
            scene_lit_e2e::Fixture fixture(false,output);
            scene_lit_e2e::shadow_cases(fixture);fixture.close();return 0;
        }
        if(mode=="--scene-lit-stability") {
            scene_lit_e2e::Fixture fixture(false,output,atelier::rhi::Validation::debug_layer);
            const auto initial_view=fixture.frame.view;
            const auto initial_camera=fixture.frame.camera;
            const auto initial_environment=fixture.frame.environment;
            scene_lit_e2e::stability_cases(fixture);
            scene_lit_e2e::quality_ddgi(fixture);
            fixture.room=scene_lit_e2e::Room{};fixture.lights=scene_lit_e2e::room_lights();
            fixture.frame.view=initial_view;fixture.frame.camera=initial_camera;
            fixture.frame.environment=initial_environment;fixture.frame.lighting_focus={};
            ++fixture.frame.scene_identity;fixture.frame.camera_cut=true;fixture.edited();
            scene_lit_e2e::probe_volume_cases(fixture);
            fixture.close();
            return 0;
        }
        if(mode=="--scene-lit-restir-suite") {
            scene_lit_e2e::Fixture fixture(false,output);
            const auto initial_view=fixture.frame.view;
            const auto initial_environment=fixture.frame.environment;
            // Material fixtures use frame IDs 9000 and 12000 before this
            // fixture's 20000 sequence. Keep that sequence advancing thereafter.
            material_cases(fixture.renderer,output);
            material_transport_cases(fixture.renderer,output);
            fixture.frame.scene_identity=fixture.drawing.id;
            const auto restore=[&] {
                fixture.drawing.extent=take(fixture.renderer.sync_window_target()).extent;
                fixture.frame.scene_viewport={24,32,float(fixture.drawing.extent.width)-48,
                    float(fixture.drawing.extent.height)-56};
                fixture.frame.camera=scene_lit_e2e::camera({
                    static_cast<unsigned>(fixture.frame.scene_viewport.width),
                    static_cast<unsigned>(fixture.frame.scene_viewport.height)});
                fixture.room=scene_lit_e2e::Room{};fixture.lights=scene_lit_e2e::room_lights();
                fixture.frame.view=initial_view;fixture.frame.environment=initial_environment;
                fixture.frame.lighting_focus={};fixture.frame.camera_cut=true;
                ++fixture.frame.scene_identity;fixture.edited();
            };
            restore();scene_lit_e2e::restir_cases(fixture);
            restore();scene_lit_e2e::probe_volume_cases(fixture);
            restore();scene_lit_e2e::quality_ddgi(fixture);
            restore();scene_lit_e2e::probe_debug_cases(fixture);
            restore();scene_lit_e2e::quality_audit(fixture);
            // Contracts exercise terminal/repeated close and must run last.
            restore();scene_lit_e2e::quality_contracts(fixture);
            fixture.close();
            std::puts("Renderer ReSTIR suite: PASS (materials, transport, ReSTIR, volumes, DDGI quality/debug, audit, contracts; one Renderer)");
            return 0;
        }
        return scene_lit_acceptance(mode == "--scene-lit-benchmark", output, mode == "--scene-lit-temporal", mode == "--scene-lit-quality");
    }
    Window window;
    auto renderer = take(
        render::Renderer::open(window.hwnd, {.validation = atelier::rhi::Validation::gpu, .vsync = false}));
    lit_preparation_case(renderer);
    cavity_cases(renderer,window);
    material_cases(renderer);
    material_transport_cases(renderer);
    ui_only_cases(renderer, window);
    auto target = take(renderer.target());
    check(target.drawable && target.extent == ui::PixelExtent{641, 385}, "actual initial target");
    ui::Frame ui_frame{.id = 1, .extent = target.extent, .draws = {}};
    const ui::Clip full{0, 0, 641, 385};
    ui_frame.draws.push_back({{10, 20, 80, 60}, {20, 30, 50, 60}, {0, 1, 0, 1}, {0, 0, 1, 1}, {}});
    ui_frame.draws.push_back({{10, 20, 80, 60}, {20, 30, 50, 60}, {1, 0, 0, .5f}, {0, 0, 1, 1}, {}});
    auto coverage = std::make_shared<ui::Coverage>();
    coverage->identity = 1;
    coverage->revision = 1;
    coverage->width = 3;
    coverage->height = 2;
    coverage->row_pitch = 5;
    coverage->pixels = {255, 128, 0, 77, 77, 255, 128, 0, 77, 77};
    std::weak_ptr<const ui::Coverage> old_coverage = coverage;
    ui_frame.draws.push_back({{10, 100, 60, 40}, full, {1, 1, 1, 1}, {0, 0, 1, 1}, coverage});
    coverage.reset();
    std::array<render::MeshInstance, 2> cubes{{{translation(0, 0, 2), {1, 0, 0, 1}, false, atelier::geometry::cube_mesh()},
                                     {translation(0, 0, 5), {0, 0, 1, 1}, false, atelier::geometry::cube_mesh()}}};
    render::Camera camera;
    camera.projection[10] = -.1f;
    render::Frame frame{ui_frame, camera, {120, 40, 480, 320}, cubes};
    render::Capture capture;
    auto result = take(prepared_render(renderer,frame, &capture));
    check(result.status == render::FrameStatus::accepted && result.accepted_id == 1 &&
              result.accepted_extent == target.extent,
          "accepted metadata");
    check(result.stats.ui_draw_calls == 1 && result.stats.ui_quads == 3 && result.stats.scene_draw_calls == 1 &&
              result.stats.coverage_upload_count == 1 && result.stats.coverage_upload_bytes == 6 &&
              result.stats.cached_coverage_count == 1 && result.stats.cached_coverage_texel_bytes == 6 &&
              result.stats.cached_coverage_cpu_bytes == 10,
          "first frame reports actual draws, tight R8 upload and padded CPU cache bytes");
    const auto *blend = pixel(capture, 30, 40);
    check(std::abs(int(blend[0]) - 188) < 3 && std::abs(int(blend[1]) - 188) < 3 && blend[2] == 0,
          "linear premultiplied blend into sRGB target");
    check(pixel(capture, 15, 40)[0] == 0 && pixel(capture, 50, 40)[0] == 0, "half-open clipped rectangle");
    check(pixel(capture, 20, 120)[0] > 245 && pixel(capture, 60, 120)[0] < 20,
          "padded R8 text coverage and ordered UI");
    check(pixel(capture, 360, 200)[0] > 180 && pixel(capture, 360, 200)[2] == 0,
          "near cube occludes farther cube drawn later");
    check(std::abs(depth(capture, 360, 200) - .85f) < .0001f && depth(capture, 100, 200) == 0,
          "depth capture at unaligned window width");
    snapshot(capture);
    const auto initial = capture;
    cubes[0].selected = true;
    ui_frame.id++;
    result = take(prepared_render(renderer,frame, &capture));
    check(pixel(capture, 240, 200)[1] > pixel(initial, 240, 200)[1] + 60 &&
          pixel(capture, 360, 200)[1] == pixel(initial, 360, 200)[1],
          "selection highlights polygon edges while preserving the surface");
    // Cavity composites before selected edges; UI coverage activity remains unchanged.
    check(result.stats.ui_draw_calls == 1 && result.stats.ui_quads == 3 && result.stats.scene_draw_calls == 2 &&
              result.stats.coverage_upload_count == 0 && result.stats.coverage_upload_bytes == 0 &&
              result.stats.cached_coverage_cpu_bytes == 10,
          "unchanged coverage reuses cache without accumulating per-frame activity");
    ui_frame.draws.push_back({{350, 190, 20, 20}, full, {1, 1, 1, .5f}, {0, 0, 1, 1}, {}});
    ui_frame.id++;
    result = take(prepared_render(renderer,frame, &capture));
    check(pixel(capture, 360, 200)[2] > 100 && std::abs(depth(capture, 360, 200) - .85f) < .0001f,
          "UI overlays scene without changing depth");
    frame.camera.view[0] = std::numeric_limits<float>::quiet_NaN();
    auto invalid = prepared_render(renderer,frame);
    check(!invalid && !invalid.error().terminal, "invalid CPU frame is recoverable before encoding");
    frame.camera = camera;
    bool wrong_thread = false;
    std::thread worker([&] {
        const auto foreign = renderer.target();
        wrong_thread = !foreign && foreign.error().cause.code == atelier::rhi::ErrorCode::wrong_thread;
    });
    worker.join();
    check(wrong_thread, "owner thread enforcement");
    // Same identity with a new immutable revision replaces GPU contents. Old CPU
    // content has no owner after that frame, while GPU retirement stays fence-safe.
    auto replacement = std::make_shared<ui::Coverage>();
    replacement->identity = 1;
    replacement->revision = 2;
    replacement->width = 3;
    replacement->height = 2;
    replacement->row_pitch = 3;
    replacement->pixels = {0, 0, 255, 0, 0, 255};
    ui_frame.draws[2].coverage = replacement;
    replacement.reset();
    for (int i = 0; i < 12; ++i) {
        ++ui_frame.id;
        result = take(prepared_render(renderer,frame));
        check(result.stats.ui_draw_calls == 1 && result.stats.ui_quads == 4 && result.stats.scene_draw_calls == 2 &&
                  result.stats.coverage_upload_count == (i == 0 ? 1u : 0u) &&
                  result.stats.coverage_upload_bytes == (i == 0 ? 6u : 0u) &&
                  result.stats.cached_coverage_count == 1 && result.stats.cached_coverage_texel_bytes == 6 &&
                  result.stats.cached_coverage_cpu_bytes == 6,
              "revision upload counted once, retired old version removed from cache totals");
    }
    check(old_coverage.expired(), "unused coverage owner released");
    ++ui_frame.id;
    take(prepared_render(renderer,frame, &capture));
    check(pixel(capture, 20, 120)[0] < 20 && pixel(capture, 60, 120)[0] > 245,
          "revision upload replaces cached pixels");
    auto hidden = std::make_shared<ui::Coverage>(*ui_frame.draws[2].coverage);
    hidden->identity = 2;
    ui_frame.draws.push_back({{0, 0, 30, 20}, {}, {1, 1, 1, 1}, {0, 0, 1, 1}, hidden});
    ui_frame.draws.push_back({{0, 0, 30, 20}, {30, 0, 60, 20}, {1, 1, 1, 1}, {0, 0, 1, 1}, hidden});
    ++ui_frame.id;
    result = take(prepared_render(renderer,frame));
    check(result.stats.ui_draw_calls == 1 && result.stats.ui_quads == 4 && result.stats.scene_draw_calls == 2 &&
              result.stats.coverage_upload_count == 0 && result.stats.coverage_upload_bytes == 0 &&
              result.stats.cached_coverage_count == 1,
          "empty or disjoint clips are not counted as a quad, draw or upload");
    window.resize(703, 427);
    result = take(prepared_render(renderer,frame, &capture));
    check(result.accepted_extent == ui::PixelExtent{641, 385} && capture.extent == result.accepted_extent &&
              result.next_extent == ui::PixelExtent{703, 427},
          "resize accepts old target and reports next target separately");
    const auto accepted = capture.frame_id;
    result = take(prepared_render(renderer,frame, &capture));
    check(result.status == render::FrameStatus::skipped && capture.frame_id == accepted,
          "stale extent skips without publishing capture");
    check(result.stats.ui_draw_calls == 0 && result.stats.scene_draw_calls == 0 &&
              result.stats.coverage_upload_count == 0 && result.stats.coverage_upload_bytes == 0 &&
              result.stats.cached_coverage_count == 1 && result.stats.cached_coverage_texel_bytes == 6 &&
              result.stats.cached_coverage_cpu_bytes == 6,
          "pre-encoding skip reports zero activity and unchanged current cache ownership");
    ui_frame.extent = result.next_extent;
    ++ui_frame.id;
    result = take(prepared_render(renderer,frame, &capture));
    check(result.status == render::FrameStatus::accepted && capture.extent == ui_frame.extent,
          "rebuild depth for resized target");
    ShowWindow(window.hwnd, SW_MINIMIZE);
    pump();
    check(!take(renderer.target()).drawable, "minimized target not drawable");
    result = take(prepared_render(renderer,frame, &capture));
    check(result.status == render::FrameStatus::skipped, "minimized frame not accepted");
    ShowWindow(window.hwnd, SW_RESTORE);
    pump();
    ++ui_frame.id;
    check(take(prepared_render(renderer,frame)).status == render::FrameStatus::accepted, "restore rendering");
    ShowWindow(window.hwnd, SW_HIDE);
    pump();
    check(take(prepared_render(renderer,frame)).status == render::FrameStatus::skipped, "hidden target skips");
    ShowWindow(window.hwnd, SW_SHOW);
    pump();
    // Non-diagonal view/projection values verify the public row-vector LH ABI.
    frame.camera = {};
    frame.camera.view[14] = 4;
    frame.camera.projection = {2.0f / 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, -.1f / 9.9f, 1,
                               0, 0, 1.0f / 9.9f, 0};
    cubes[0].world = translation(0, 0, 0);
    ++ui_frame.id;
    take(prepared_render(renderer,frame, &capture));
    check(std::abs(depth(capture, 360, 200) - (1.0f / (9.9f * 3.5f) - .1f / 9.9f)) < .0001f,
          "left-handed row-major view and perspective projection");
    ui_frame.draws.erase(ui_frame.draws.begin() + 2);
    frame.meshes = {};
    ++ui_frame.id;
    result = take(prepared_render(renderer,frame, &capture));
    check(result.stats.ui_draw_calls == 1 && result.stats.ui_quads == 3 && result.stats.scene_draw_calls == 0 &&
              result.stats.coverage_upload_count == 0 && result.stats.coverage_upload_bytes == 0 &&
              result.stats.cached_coverage_count == 0 && result.stats.cached_coverage_texel_bytes == 0 &&
              result.stats.cached_coverage_cpu_bytes == 0,
          "unused coverage leaves cache totals and empty scene emits no draw");
    check(std::ranges::all_of(capture.depth, [](float value) { return value == 0; }),
          "UI-only capture after a scene does not expose the retained scene depth");
    transform_cases(renderer, ui_frame.extent);
    mesh_cases(renderer, ui_frame.extent);
    visibility_cases(renderer, ui_frame.extent);
    ui_shape_cases(renderer, ui_frame.extent);
    selection_depth_cases(renderer,ui_frame.extent);
    selection_clipping_cases(renderer, ui_frame.extent);
    viewport_cases(renderer, ui_frame.extent);
    reference_guide_cases(renderer, ui_frame.extent);
    auto closed = renderer.close();
    check(bool(closed), "drain and close before HWND destruction");
    check(bool(renderer.close()) && !renderer.target(), "idempotent close and closed target");
    std::puts("Renderer acceptance: PASS (GPU validation, scene/depth, UI coverage/blend/clip, revisions, "
              "resize, minimize/restore, lifetime)");
} catch (const std::exception& error) {
    std::fprintf(stderr, "Renderer acceptance FAILED: %s\n", error.what());
    return 1;
}

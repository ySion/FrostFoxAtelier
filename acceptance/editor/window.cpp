#include "application/application.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using atelier::application::Application;
using atelier::ui::ControlKey;
namespace control = atelier::editor::control;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("PNG encoding failed."); }
void tick(Application& app, unsigned count = 2) {
    const auto before = app.accepted_frames();
    for (unsigned attempt = 0; app.accepted_frames() < before + count && attempt < 180; ++attempt)
        require(app.step(), "Editor closed during input acceptance.");
    require(app.accepted_frames() >= before + count, "Editor did not accept a frame within 180 iterations.");
}
POINT centre(Application& app, ControlKey key) {
    require(app.presented() != nullptr, "Missing presented UI frame.");
    const auto* box = app.presented()->find(key);
    require(box != nullptr, "Missing control in presented UI frame.");
    const auto left = std::max(box->bounds.x, static_cast<float>(box->clip.left));
    const auto right = std::min(box->bounds.x + box->bounds.width, static_cast<float>(box->clip.right));
    const auto top = std::max(box->bounds.y, static_cast<float>(box->clip.top));
    const auto bottom = std::min(box->bounds.y + box->bounds.height, static_cast<float>(box->clip.bottom));
    require(right > left && bottom > top, "Control has no visible hit area.");
    return {static_cast<LONG>((left + right) * .5f), static_cast<LONG>((top + bottom) * .5f)};
}
void mouse(Application& app, UINT message, POINT point) {
    SendMessageW(app.window(), message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(point.x, point.y));
    tick(app, 1);
}
void click(Application& app, ControlKey key) {
    const auto point = centre(app, key);
    mouse(app, WM_MOUSEMOVE, point); mouse(app, WM_LBUTTONDOWN, point); mouse(app, WM_LBUTTONUP, point);
}
void key(Application& app, WPARAM code, bool ctrl = false) {
    BYTE original[256]{}; GetKeyboardState(original);
    BYTE state[256]{}; std::copy(std::begin(original), std::end(original), state);
    if (ctrl) state[VK_CONTROL] |= 0x80;
    SetKeyboardState(state);
    SendMessageW(app.window(), WM_KEYDOWN, code, 0);
    SetKeyboardState(original);
    tick(app, 1);
    SendMessageW(app.window(), WM_KEYUP, code, 0);
    tick(app, 1);
}
void type(Application& app, const wchar_t* value) {
    key(app, 'A', true);
    while (*value) SendMessageW(app.window(), WM_CHAR, static_cast<WPARAM>(*value++), 0);
    tick(app, 1);
}
float x(const Application& app, atelier::scene::ObjectKey key) {
    const auto point = app.document().scene().position(key);
    require(point.has_value(), "Missing position in current scene."); return point->x;
}
void png(const atelier::renderer::Capture& image, const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; hr(factory->CreateStream(&stream));
    hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; hr(encoder->CreateNewFrame(&frame, nullptr)); hr(frame->Initialize(nullptr));
    hr(frame->SetSize(image.extent.width, image.extent.height));
    auto format = GUID_WICPixelFormat32bppBGRA; hr(frame->SetPixelFormat(&format));
    require(format == GUID_WICPixelFormat32bppBGRA, "PNG codec changed pixel format.");
    auto bgra = image.rgba;
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4) std::swap(bgra[pixel], bgra[pixel + 2]);
    hr(frame->WritePixels(image.extent.height, image.extent.width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    hr(frame->Commit()); hr(encoder->Commit());
}
void capture(Application& app, const std::filesystem::path& path) {
    atelier::renderer::Capture image;
    require(app.step(&image) && image.frame_id != 0, "Renderer did not capture an accepted frame.");
    require(image.rgba.size() == static_cast<std::size_t>(image.extent.width) * image.extent.height * 4,
        "Capture has an invalid pixel payload.");
    const auto scene_pixels = std::count_if(image.depth.begin(), image.depth.end(), [](float value) { return value > 0 && value < .999999f; });
    require(scene_pixels > 32, "Real scene geometry is absent from the depth capture.");
    png(image, path);
}
}

int verify_editor_window(Application& app, const char* output_directory) {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const std::filesystem::path output(output_directory);
    std::filesystem::create_directories(output);
    std::ofstream(output / "acceptance.txt") << "Editor window acceptance: RUNNING\n";
    SetForegroundWindow(app.window()); SetFocus(app.window());
    tick(app);
    capture(app, output / "m0.png");
    click(app, control::reset);
    const auto original_width = app.parameter_width();
    auto divider = centre(app, control::splitter);
    mouse(app, WM_LBUTTONDOWN, divider); divider.x -= 50;
    mouse(app, WM_MOUSEMOVE, divider); mouse(app, WM_LBUTTONUP, divider);
    require(app.parameter_width() > original_width + 20, "Pane divider did not change the workspace.");

    const auto initial_count = app.document().scene().members().size();
    click(app, control::add); click(app, control::add);
    require(app.document().scene().members().size() == initial_count + 2, "Add cube did not enter the scene.");
    const auto added_key = app.document().selected();
    const auto added_id = app.document().scene().find(added_key)->id();
    click(app, control::undo);
    require(!app.document().scene().find(added_key), "Undo add did not remove membership.");
    click(app, control::redo);
    require(app.document().scene().find(added_key)->id() == added_id, "Redo add replaced object identity.");
    const auto object = app.document().scene().members().front().key;
    click(app, control::first_object + object);
    require(app.document().selected() == object, "List selection did not select the clicked object.");
    const auto field = control::position(object, 0);
    const auto before = x(app, object);
    const auto history = app.document().undo_count();
    auto point = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, point);
    for (unsigned move = 0; move < 4; ++move) { point.x += 12; mouse(app, WM_MOUSEMOVE, point); }
    require(x(app, object) > before && app.document().undo_count() == history, "Drag failed to preview the real object without history.");
    mouse(app, WM_LBUTTONUP, point);
    const auto after = x(app, object);
    require(app.document().undo_count() == history + 1, "Continuous drag did not commit exactly one undo.");
    click(app, control::undo); require(x(app, object) == before, "Single undo did not restore the drag origin.");
    click(app, control::redo); require(x(app, object) == after, "Redo did not restore the committed position.");
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 32; mouse(app, WM_MOUSEMOVE, point);
    key(app, VK_ESCAPE); mouse(app, WM_LBUTTONUP, point);
    require(x(app, object) == after && app.document().undo_count() == history + 1, "Esc did not cancel a continuous edit.");

    click(app, field);
    require(app.wants_keyboard(), "Numeric control did not enter text input.");
    const auto count = app.document().scene().members().size();
    key(app, 'A', true); key(app, VK_DELETE);
    require(app.document().scene().members().size() == count, "Delete in text focus deleted a scene object.");
    key(app, VK_ESCAPE);
    click(app, field); type(app, L"-2.5"); key(app, VK_RETURN);
    require(std::abs(x(app, object) + 2.5f) < .0001f, "Numeric text commit did not update the Runtime field.");
    click(app, field); type(app, L"10001"); key(app, VK_RETURN);
    require(std::abs(x(app, object) + 2.5f) < .0001f, "Out-of-range text changed the scene.");
    key(app, VK_ESCAPE);
    const auto identity = app.document().scene().find(object)->id();
    click(app, control::remove);
    require(!app.document().scene().find(object), "Deleted object remained a member.");
    tick(app, 64); // host GC crosses complete cycles while only history holds this object
    click(app, control::undo);
    require(app.document().scene().find(object)->id() == identity && std::abs(x(app, object) + 2.5f) < .0001f,
        "Window delete/GC/undo failed to restore identity and position.");
    click(app, control::focus); click(app, control::reset);

    atelier::renderer::Capture camera_before, camera_orbit, camera_zoom;
    require(app.step(&camera_before), "Camera capture failed.");
    auto view = centre(app, control::viewport);
    SendMessageW(app.window(), WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    view.x += 42; view.y += 18;
    SendMessageW(app.window(), WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    SendMessageW(app.window(), WM_RBUTTONUP, 0, MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(app.step(&camera_orbit) && camera_orbit.depth != camera_before.depth, "Right-drag orbit did not change the rendered scene.");
    ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(app.step(&camera_zoom) && camera_zoom.depth != camera_orbit.depth, "Wheel zoom did not change the rendered scene.");
    click(app, control::reset);
    view = centre(app, control::viewport);
    SendMessageW(app.window(), WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(GetCapture() == app.window(), "Orbit did not acquire window capture.");
    SetFocus(nullptr); tick(app, 2);
    require(GetCapture() != app.window(), "Focus loss retained camera-owned capture.");
    SetFocus(app.window()); tick(app, 1);

    const auto before_loss = x(app, object);
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    ReleaseCapture(); tick(app, 2);
    require(!app.document().editing() && x(app, object) == before_loss, "Unexpected capture loss retained a partial edit.");
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    SetFocus(nullptr); tick(app, 2); SetFocus(app.window()); tick(app, 1);
    require(!app.document().editing() && x(app, object) == before_loss, "Focus loss retained a partial edit.");

    const auto before_resize = x(app, object);
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    SetWindowPos(app.window(), nullptr, 0, 0, 1120, 740, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    tick(app, 4);
    require(!app.document().editing() && x(app, object) == before_resize, "Resize retained a partial edit.");
    ShowWindow(app.window(), SW_MINIMIZE);
    const auto accepted = app.accepted_frames(); app.step(); app.step();
    require(app.accepted_frames() == accepted, "Minimized window accepted an interactive frame.");
    ShowWindow(app.window(), SW_RESTORE); tick(app, 4);
    const auto revision = app.presented()->drawing->surface_revision;
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    RECT bounds{}; GetWindowRect(app.window(), &bounds);
    const auto dpi = GetDpiForWindow(app.window());
    SendMessageW(app.window(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
    tick(app, 3);
    require(app.presented()->drawing->surface_revision > revision && !app.document().editing() && x(app, object) == before_resize,
        "DPI transition policy did not cancel and publish matching hit data.");
    view = centre(app, control::viewport); ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), MAKELPARAM(view.x, view.y)); tick(app, 1);
    capture(app, output / "editor.png");
    SendMessageW(app.window(), WM_CLOSE, 0, 0);
    require(!app.step(), "WM_CLOSE did not close the application.");
    std::ofstream report(output / "acceptance.txt");
    report << "Editor window acceptance: PASS\n"
           << "Real HWND messages -> platform adapter -> UI accepted-frame input -> Session -> Runtime -> renderer\n"
           << "splitter; add/undo/redo identity; list selection; drag preview/one undo; Esc; text/Delete; numeric range; delete/GC/undo; camera buttons/orbit/zoom; capture/focus loss; resize; minimize; DPI transition; shutdown\n"
           << "DPI coverage: WM_DPICHANGED policy at the current monitor DPI; physical multi-monitor DPI migration not exercised.\n"
           << "Accepted frames: " << app.accepted_frames() << "\n";
    std::puts("Editor window acceptance: PASS");
    std::fflush(stdout);
    return 0;
}

#include "application/application.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
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
bool intersects(const atelier::ui::Rect& a, const atelier::ui::Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
std::shared_ptr<const atelier::ui::Coverage> text_coverage(const atelier::ui::UiFrame& frame, ControlKey key) {
    const auto* box = frame.find(key);
    require(box != nullptr, "Text control disappeared from the frame.");
    for (const auto& draw : frame.drawing->draws) {
        if (draw.coverage && intersects(draw.bounds, box->bounds) &&
            draw.clip.left < box->bounds.x + box->bounds.width && draw.clip.right > box->bounds.x &&
            draw.clip.top < box->bounds.y + box->bounds.height && draw.clip.bottom > box->bounds.y)
            return draw.coverage;
    }
    return {};
}
std::vector<atelier::ui::Color> solid_colors(const atelier::ui::UiFrame& frame, ControlKey key) {
    const auto* box = frame.find(key);
    require(box != nullptr, "Paint control disappeared from the frame.");
    std::vector<atelier::ui::Color> colors;
    for (const auto& draw : frame.drawing->draws)
        if (!draw.coverage && intersects(draw.bounds, box->bounds)) colors.push_back(draw.color);
    return colors;
}
void wheel(Application& app, ControlKey key, short notches) {
    auto point = centre(app, key);
    ClientToScreen(app.window(), &point);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(notches * WHEEL_DELTA)), MAKELPARAM(point.x, point.y));
    tick(app, 1);
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
void verify_control_states(Application& app, const std::filesystem::path& output, atelier::scene::ObjectKey object) {
    mouse(app, WM_MOUSEMOVE, centre(app, control::viewport));
    const auto normal = solid_colors(*app.presented(), control::reset);
    const auto button = centre(app, control::reset);
    mouse(app, WM_MOUSEMOVE, button);
    const auto hover = solid_colors(*app.presented(), control::reset);
    require(hover != normal, "Hover did not use the shared control state style.");
    mouse(app, WM_LBUTTONDOWN, button);
    require(solid_colors(*app.presented(), control::reset) != hover, "Pressed state did not change the shared paint.");
    capture(app, output / "s1-pressed.png");
    mouse(app, WM_LBUTTONUP, button);

    const auto field = control::position(object, 0);
    click(app, field);
    const auto focused = solid_colors(*app.presented(), field);
    capture(app, output / "s1-focused.png");
    type(app, L"10001");
    require(solid_colors(*app.presented(), field) != focused, "Invalid draft did not change the shared field state.");
    capture(app, output / "s1-invalid.png");
    key(app, VK_ESCAPE);

    const auto before = x(app, object);
    click(app, control::remove);
    const auto* disabled = app.presented()->find(control::position(0, 0));
    require(disabled && !disabled->enabled && text_coverage(*app.presented(), disabled->key),
        "Unselected parameter state did not retain disabled text and geometry.");
    capture(app, output / "s1-disabled.png");
    click(app, control::undo);
    require(app.document().selected() == object && x(app, object) == before, "State verification did not restore the edited object.");
    const auto* selected = app.presented()->find(control::first_object + object);
    require(selected && selected->enabled, "Selected object row lost its enabled control geometry.");
}
void verify_style_changes(Application& app, const std::filesystem::path& output, atelier::scene::ObjectKey object) {
    const auto initial_style = app.style();
    const auto field = control::position(object, 0);
    click(app, field);
    key(app, VK_END);
    const auto original = app.presented();
    const auto original_coverage = text_coverage(*original, field);
    require(original_coverage != nullptr, "Focused numeric field has no real text coverage.");
    const auto original_pixels = original_coverage->pixels;
    const auto original_bounds = original->find(field)->bounds;
    const auto original_provider = original->measure_provider_revision;
    const auto caret = [&](const atelier::ui::UiFrame& frame) {
        const auto* box = frame.find(field);
        for (const auto& draw : frame.drawing->draws)
            if (!draw.coverage && draw.color == app.style().palette.text &&
                draw.bounds.width <= app.style().caret_width * frame.drawing->dpi_scale + .01f &&
                draw.bounds.height > 5 && intersects(draw.bounds, box->bounds)) return draw.bounds;
        throw std::runtime_error("Focused field has no caret using the shared text style.");
    };
    const auto original_caret = caret(*original);
    auto colors = initial_style;
    colors.palette.text = {.64f, .88f, .72f, 1};
    colors.palette.panel = {.04f, .052f, .048f, 1};
    const auto before_color = app.ui_statistics();
    require(app.set_style(colors).has_value() && app.presented() == original,
        "Setting a style changed the already accepted frame.");
    tick(app, 1);
    require(text_coverage(*app.presented(), field) == original_coverage &&
        app.ui_statistics().rasterizations == before_color.rasterizations &&
        app.frame_statistics().coverage_upload_count == 0 &&
        app.presented()->measure_provider_revision == original_provider,
        "Color-only style change invalidated text coverage or metrics.");
    capture(app, output / "s1-color.png");

    const auto colored_frame = app.presented();
    auto invalid = colors;
    invalid.font.size = -1;
    invalid.palette.panel = {.4f, .1f, .1f, 1};
    const auto rejected = app.set_style(invalid);
    require(!rejected && rejected.error().code == atelier::ui::ErrorCode::invalid_style &&
        app.style() == colors && app.presented() == colored_frame, "Invalid style partially modified the theme or accepted frame.");

    auto font = colors;
    font.font.family = L"Segoe UI";
    font.font.size = 16;
    font.font.weight = 500;
    font.title_weight = 700;
    require(app.set_style(font).has_value(), "Unified font change was rejected.");
    tick(app, 1);
    const auto current_coverage = text_coverage(*app.presented(), field);
    const auto current_caret = caret(*app.presented());
    require(current_coverage && current_coverage != original_coverage &&
        app.presented()->measure_provider_revision > original_provider &&
        current_caret.x != original_caret.x && current_caret.height != original_caret.height,
        "Font identity did not invalidate text layout, coverage and caret together.");
    require(current_caret.x >= original_bounds.x && current_caret.x <= original_bounds.x + original_bounds.width &&
        original->find(field)->bounds == original_bounds && original_coverage->pixels == original_pixels,
        "Font update changed old-frame geometry/content or misplaced the new caret.");
    for (const auto command : {control::add, control::remove, control::undo, control::redo, control::focus, control::reset}) {
        const auto* box = app.presented()->find(command);
        const auto coverage = text_coverage(*app.presented(), command);
        require(box && coverage && box->bounds.x + box->bounds.width <= app.presented()->drawing->extent.width,
            "Unified font change pushed a toolbar command outside the window.");
        for (const auto& draw : app.presented()->drawing->draws)
            if (draw.coverage == coverage) require(draw.bounds.x >= box->bounds.x &&
                draw.bounds.x + draw.bounds.width <= box->bounds.x + box->bounds.width,
                "Measured toolbar width clipped the changed font's glyphs.");
    }
    capture(app, output / "s1-font.png");
    key(app, VK_ESCAPE);
    require(app.set_style(initial_style).has_value(), "Could not restore the editor style.");
    tick(app, 1);
}
struct TimingSeries {
    std::vector<double> prepare, render;
    atelier::renderer::FrameStats total;
    std::uint64_t peak_live_coverages{}, peak_live_bytes{};
    atelier::ui::Statistics start, finish;
    explicit TimingSeries(const Application& app) : start(app.ui_statistics()) {}
    void record(const Application& app) {
        const auto& sample = app.frame_timing();
        require(sample.frame_id == app.presented()->drawing->id, "Timing did not describe the accepted frame.");
        prepare.push_back(sample.prepare_us);
        render.push_back(sample.render_us);
        const auto& stats = app.frame_statistics();
        total.ui_draw_calls += stats.ui_draw_calls;
        total.scene_draw_calls += stats.scene_draw_calls;
        total.coverage_upload_count += stats.coverage_upload_count;
        total.coverage_upload_bytes += stats.coverage_upload_bytes;
        total.cached_coverage_count = std::max(total.cached_coverage_count, stats.cached_coverage_count);
        total.cached_coverage_texel_bytes = std::max(total.cached_coverage_texel_bytes, stats.cached_coverage_texel_bytes);
        total.cached_coverage_cpu_bytes = std::max(total.cached_coverage_cpu_bytes, stats.cached_coverage_cpu_bytes);
        finish = app.ui_statistics();
        peak_live_coverages = std::max(peak_live_coverages, finish.live_coverages);
        peak_live_bytes = std::max(peak_live_bytes, finish.live_coverage_bytes);
    }
    void write(std::ostream& report, const char* name) const {
        const auto distribution = [&](const char* stage, std::vector<double> values) {
            require(!values.empty(), "No timing samples were collected.");
            std::sort(values.begin(), values.end());
            const auto p95 = (values.size() * 95 + 99) / 100 - 1;
            report << name << ',' << stage << ',' << values.size() << ','
                   << std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()) << ','
                   << values[values.size() / 2] << ',' << values[p95] << ',' << values.back() << '\n';
        };
        distribution("prepare_cpu_us", prepare);
        distribution("render_cpu_us", render);
    }
    void resources(std::ostream& report, const char* name) const {
        report << name << ',' << prepare.size() << ',' << finish.prepares - start.prepares << ','
               << finish.rasterizations - start.rasterizations << ',' << finish.raster_bytes - start.raster_bytes << ','
               << finish.culled_text_controls - start.culled_text_controls << ','
               << total.ui_draw_calls << ',' << total.scene_draw_calls << ',' << total.coverage_upload_count << ','
               << total.coverage_upload_bytes << ',' << peak_live_coverages << ',' << peak_live_bytes << ','
               << total.cached_coverage_count << ',' << total.cached_coverage_texel_bytes << ',' << total.cached_coverage_cpu_bytes << '\n';
    }
};
void verify_list_workload(Application& app, const std::filesystem::path& output) {
    // Real scene additions use the existing toolbar and Window/UI/Session route.
    const auto add = centre(app, control::add);
    while (app.document().scene().members().size() < atelier::scene::Scene::maximum_members) {
        const auto before_count = app.document().scene().members().size();
        SendMessageW(app.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(add.x, add.y));
        SendMessageW(app.window(), WM_LBUTTONUP, 0, MAKELPARAM(add.x, add.y));
        tick(app, 1);
        require(app.document().scene().members().size() == before_count + 1, "Toolbar failed to populate the real list workload.");
    }
    const auto first = app.document().scene().members().front().key;
    const auto last = app.document().scene().members().back().key;
    require(app.presented()->find(control::first_object + last) &&
        !text_coverage(*app.presented(), control::first_object + last),
        "Offscreen list row lost geometry or emitted text before scrolling.");
    wheel(app, 23, -240);
    require(text_coverage(*app.presented(), control::first_object + last) &&
        !text_coverage(*app.presented(), control::first_object + first), "Scrolling did not exchange visible list text.");
    capture(app, output / "s1-list-256.png");
    wheel(app, 23, 240);
    require(text_coverage(*app.presented(), control::first_object + first) != nullptr,
        "Scrolling back did not regenerate visible list text.");

    TimingSeries scroll(app);
    app.sample_frames(true);
    for (unsigned frame = 0; frame < 48; ++frame) {
        wheel(app, 23, frame < 24 ? -8 : 8);
        scroll.record(app);
    }
    const auto field = control::position(last, 0);
    const auto before = x(app, last);
    const auto history = app.document().undo_count();
    auto point = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, point);
    TimingSeries editing(app);
    for (unsigned frame = 0; frame < 64; ++frame) {
        point.x += frame < 32 ? 2 : -2;
        mouse(app, WM_MOUSEMOVE, point);
        editing.record(app);
    }
    point.x += 12;
    mouse(app, WM_MOUSEMOVE, point);
    mouse(app, WM_LBUTTONUP, point);
    require(x(app, last) != before && app.document().undo_count() == std::min(history + 1, std::size_t{128}),
        "Dense-scene drag did not keep the existing position/history behavior.");
    app.sample_frames(false);
    const auto after = x(app, last);
    // Number focus owns Ctrl+Z for drafts; document shortcuts follow focus exit.
    const auto committed_history = app.document().undo_count();
    key(app, 'Z', true);
    require(x(app, last) == after && app.document().undo_count() == committed_history && app.wants_keyboard(),
        "Ctrl+Z in numeric focus escaped to scene history.");
    key(app, VK_ESCAPE);
    require(!app.wants_keyboard(), "Esc did not release numeric keyboard ownership.");
    key(app, 'Z', true);
    require(x(app, last) == before, "One undo did not restore the dense-scene drag.");
    key(app, 'Y', true);
    require(x(app, last) == after, "Redo did not restore the dense-scene drag.");
    std::ofstream report(output / "s1-performance.csv");
    report << "workload,stage,samples,mean,p50,p95,max\n";
    scroll.write(report, "256_objects_scroll");
    editing.write(report, "256_objects_position_drag");
    require(report.good(), "Cannot save window sampling evidence.");
    std::ofstream resources(output / "s1-resources.csv");
    resources << "workload,accepted_samples,prepare_calls,rasterizations,raster_bytes,culled_text_controls,ui_draw_calls,scene_draw_calls,coverage_upload_count,coverage_payload_bytes,peak_live_cpu_coverages,peak_live_cpu_bytes,peak_renderer_cache_count,peak_renderer_cache_texel_bytes,peak_renderer_cache_cpu_bytes\n";
    scroll.resources(resources, "256_objects_scroll");
    editing.resources(resources, "256_objects_position_drag");
    require(resources.good(), "Cannot save resource sampling evidence.");
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
    verify_control_states(app, output, object);
    verify_style_changes(app, output, object);
    view = centre(app, control::viewport); ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), MAKELPARAM(view.x, view.y)); tick(app, 1);
    capture(app, output / "editor.png");
    verify_list_workload(app, output);
    SendMessageW(app.window(), WM_CLOSE, 0, 0);
    require(!app.step(), "WM_CLOSE did not close the application.");
    std::ofstream report(output / "acceptance.txt");
    report << "Editor window acceptance: PASS\n"
           << "Real HWND messages -> platform adapter -> UI accepted-frame input -> Session -> Runtime -> renderer\n"
           << "splitter; add/undo/redo identity; list selection; drag preview/one undo; Esc; text/Delete; numeric range; delete/GC/undo; camera buttons/orbit/zoom; capture/focus loss; resize; minimize; DPI transition; shutdown\n"
           << "S1: shared normal/hover/pressed/focus/invalid/disabled states; color-only coverage reuse/zero upload; atomic invalid-style rejection; font family/weight/size metrics/caret/raster; retained old-frame geometry/pixels; 256 real objects/offscreen geometry/culling/scroll-back; numeric-focus Ctrl+Z stays local, Esc then native Ctrl+Z/Y drives dense-scene history\n"
           << "Sampling: s1-performance.csv contains CPU call microseconds, including Present/waits in render; capture frames excluded. No GPU duration or before/after speedup claimed.\n"
           << "Sampling environment: this acceptance application enables hardware D3D12 debug/GBV and renderer vsync, including in release; results do not represent validation-disabled production frame time.\n"
           << "s1-resources.csv: UI cumulative counters use interval differences; renderer activity sums sampled render results. Resource peaks are sampled ownership counts, not total process memory or physical VRAM; R8 upload bytes exclude transfer padding.\n"
           << "DPI coverage: WM_DPICHANGED policy at the current monitor DPI; physical multi-monitor DPI migration not exercised.\n"
           << "Accepted frames: " << app.accepted_frames() << "\n";
    std::puts("Editor window acceptance: PASS");
    std::fflush(stdout);
    return 0;
}

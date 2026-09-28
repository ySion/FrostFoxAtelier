#include "application/application.hpp"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace atelier::application {
namespace {
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
template<class T> T graphics(renderer::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().cause.message);
    return std::move(*result);
}
std::wstring widen(const std::string& value) { return {value.begin(), value.end()}; }
ui::Key key(WPARAM value) {
    switch (value) {
    case VK_BACK: return ui::Key::backspace; case VK_DELETE: return ui::Key::delete_key;
    case VK_RETURN: return ui::Key::enter; case VK_ESCAPE: return ui::Key::escape;
    case VK_TAB: return ui::Key::tab; case VK_LEFT: return ui::Key::left; case VK_RIGHT: return ui::Key::right;
    case VK_HOME: return ui::Key::home; case VK_END: return ui::Key::end; case VK_SPACE: return ui::Key::space;
    case 'A': return ui::Key::a; case 'Z': return ui::Key::z; case 'Y': return ui::Key::y;
    default: return ui::Key::none;
    }
}
ui::PixelExtent client_extent(HWND window) {
    if (IsIconic(window)) return {};
    RECT bounds{}; GetClientRect(window, &bounds);
    return {static_cast<std::uint32_t>(bounds.right), static_cast<std::uint32_t>(bounds.bottom)};
}
}
Application::Application(bool validation) {
    take(document_.add());
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    renderer_ = graphics(renderer::Renderer::open(window_.handle(), options));
    window_.show();
}
Application::~Application() {
    try { close(); }
    catch (const std::exception& error) { std::fprintf(stderr, "Editor shutdown: %s\n", error.what()); }
}
void Application::status(Result<void> result, const wchar_t* success) {
    workspace_.status = result ? success : widen(result.error().message);
}
void Application::action(const ui::Action& event) {
    using namespace editor::control;
    if (event.key == splitter) {
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit || event.kind == ui::ActionKind::edit_cancel)
            workspace_.parameter_width = static_cast<float>(-event.value);
        return;
    }
    if (event.key >= position_base) {
        const auto bound = (event.key - position_base) / 4;
        const auto axis = static_cast<std::size_t>((event.key - position_base) % 4);
        if (bound != document_.selected() || axis >= 3) return;
        switch (event.kind) {
        case ui::ActionKind::edit_begin: status(document_.begin_position(), L"Editing position / Esc cancels"); break;
        case ui::ActionKind::edit_preview:
            status(document_.preview_position(axis, static_cast<float>(event.value)), L"Position preview / Esc cancels"); break;
        case ui::ActionKind::edit_commit: {
            const auto preview = document_.preview_position(axis, static_cast<float>(event.value));
            if (!preview) { status(preview, L""); status(document_.cancel_position(), L"Invalid edit cancelled"); }
            else status(document_.commit_position(), L"Position committed");
            break;
        }
        case ui::ActionKind::edit_cancel: status(document_.cancel_position(), L"Edit cancelled"); break;
        default: break;
        }
        return;
    }
    if (event.kind == ui::ActionKind::select && event.key >= first_object) {
        status(document_.select(event.key - first_object), L"Object selected"); return;
    }
    if (event.kind != ui::ActionKind::activate) return;
    switch (event.key) {
    case add: {
        const auto count = document_.scene().members().size();
        const auto created = document_.add({static_cast<float>(count % 5) * 1.6f, 0, static_cast<float>(count / 5) * 1.6f});
        workspace_.status = created ? L"Cube added" : widen(created.error().message);
        break;
    }
    case remove: status(document_.erase_selected(), L"Cube deleted / Undo restores it"); break;
    case undo: status(document_.undo(), L"Undo"); break;
    case redo: status(document_.redo(), L"Redo"); break;
    case focus: {
        auto point = document_.scene().position(document_.selected());
        if (point) { camera_.focus(*point); workspace_.status = L"Focused selection"; }
        break;
    }
    case reset: camera_.reset(); workspace_.status = L"Camera reset"; break;
    default: break;
    }
}
void Application::effects(const ui::Reply& reply) {
    for (const auto& event : reply.actions) action(event);
    if (reply.capture == ui::Capture::acquire) window_.capture(true);
    else if (reply.capture == ui::Capture::release) window_.capture(false);
    const wchar_t* cursor = IDC_ARROW;
    if (reply.cursor == ui::Cursor::text) cursor = IDC_IBEAM;
    else if (reply.cursor == ui::Cursor::drag_horizontal || reply.cursor == ui::Cursor::resize_horizontal) cursor = IDC_SIZEWE;
    else if (reply.cursor == ui::Cursor::resize_vertical) cursor = IDC_SIZENS;
    SetCursor(LoadCursorW(nullptr, cursor));
}
void Application::invalidate_surface() {
    ++surface_revision_;
    orbiting_ = false;
    effects(ui_.cancel_interaction());
    status(document_.cancel_position(), L"Ready");
    window_.capture(false);
}
bool Application::pointer_ready() const noexcept {
    const auto& frame = ui_.presented();
    return frame && frame->drawing->extent == client_extent(window_.handle()) &&
        frame->drawing->surface_revision == surface_revision_ && frame->drawing->dpi_scale == window_.dpi_scale();
}
bool Application::over_viewport(float x, float y) const noexcept {
    if (!pointer_ready()) return false;
    const auto* viewport = ui_.presented()->find(editor::control::viewport);
    return viewport && x >= viewport->bounds.x && y >= viewport->bounds.y &&
        x < viewport->bounds.x + viewport->bounds.width && y < viewport->bounds.y + viewport->bounds.height;
}
void Application::native(const NativeEvent& message) {
    if (message.message == WM_SIZE || message.message == WM_DPICHANGED) { invalidate_surface(); return; }
    if (message.message == WM_SETFOCUS) return;
    ui::Event event;
    event.shift = message.shift; event.control = message.control; event.alt = message.alt;
    event.x = static_cast<float>(GET_X_LPARAM(message.lparam)); event.y = static_cast<float>(GET_Y_LPARAM(message.lparam));
    bool pointer = false;
    switch (message.message) {
    case WM_MOUSEMOVE: event.kind = ui::EventKind::pointer_move; pointer = true; break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: event.kind = ui::EventKind::pointer_down; pointer = true; break;
    case WM_LBUTTONUP: event.kind = ui::EventKind::pointer_up; pointer = true; break;
    case WM_RBUTTONDOWN: event.kind = ui::EventKind::pointer_down; event.button = ui::PointerButton::secondary; pointer = true; break;
    case WM_RBUTTONUP: event.kind = ui::EventKind::pointer_up; event.button = ui::PointerButton::secondary; pointer = true; break;
    case WM_MBUTTONDOWN: event.kind = ui::EventKind::pointer_down; event.button = ui::PointerButton::middle; pointer = true; break;
    case WM_MBUTTONUP: event.kind = ui::EventKind::pointer_up; event.button = ui::PointerButton::middle; pointer = true; break;
    case WM_MOUSEWHEEL: {
        POINT position{GET_X_LPARAM(message.lparam), GET_Y_LPARAM(message.lparam)};
        ScreenToClient(window_.handle(), &position);
        event.x = static_cast<float>(position.x); event.y = static_cast<float>(position.y);
        event.kind = ui::EventKind::wheel; event.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(message.wparam)) / WHEEL_DELTA;
        pointer = true; break;
    }
    case WM_KEYDOWN: event.kind = ui::EventKind::key_down; event.key = key(message.wparam); break;
    case WM_KEYUP: event.kind = ui::EventKind::key_up; event.key = key(message.wparam); break;
    case WM_CHAR: event.kind = ui::EventKind::text; event.character = static_cast<char32_t>(message.wparam); break;
    case WM_KILLFOCUS: event.kind = ui::EventKind::focus_lost; orbiting_ = false; break;
    case WM_CAPTURECHANGED: event.kind = ui::EventKind::capture_lost; orbiting_ = false; break;
    default: return;
    }
    const bool release = event.kind == ui::EventKind::pointer_up;
    if (pointer && !release && !pointer_ready()) return;
    if (pointer && over_viewport(event.x, event.y)) {
        if (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::secondary) {
            effects(ui_.cancel_interaction());
            orbiting_ = true; last_x_ = event.x; last_y_ = event.y; window_.capture(true); return;
        }
        if (event.kind == ui::EventKind::wheel) { camera_.zoom(event.wheel); return; }
    }
    if (orbiting_ && event.kind == ui::EventKind::pointer_move) {
        const auto dpi = window_.dpi_scale(); camera_.orbit((event.x - last_x_) / dpi, (event.y - last_y_) / dpi);
        last_x_ = event.x; last_y_ = event.y; return;
    }
    if (orbiting_ && release && event.button == ui::PointerButton::secondary) { orbiting_ = false; window_.capture(false); return; }
    auto reply = ui_.input(event);
    effects(reply);
    if (event.kind == ui::EventKind::focus_lost || event.kind == ui::EventKind::capture_lost) {
        window_.capture(false); // Includes camera capture, which is owned outside UI.
        status(document_.cancel_position(), L"Ready"); return;
    }
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        if (event.key == ui::Key::delete_key) status(document_.erase_selected(), L"Cube deleted");
        else if (event.control && event.key == ui::Key::z) status(document_.undo(), L"Undo");
        else if (event.control && event.key == ui::Key::y) status(document_.redo(), L"Redo");
        else if (event.key == ui::Key::escape) status(document_.cancel_position(), L"Edit cancelled");
        else if (message.wparam == 'F' && !ui_.wants_keyboard()) {
            const auto point = document_.scene().position(document_.selected()); if (point) camera_.focus(*point);
        }
    }
}
bool Application::step(renderer::Capture* capture) {
    if (closed_) return false;
    window_.poll(events_);
    for (const auto& event : events_) native(event);
    if (window_.closing()) { close(); return false; }
    auto target = graphics(renderer_.target());
    const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
    surface_ = {extent, window_.dpi_scale(), surface_revision_};
    auto updated = ui_.set_surface(surface_);
    if (!updated) throw std::runtime_error("UI rejected the window surface.");
    effects(*updated);
    auto controls = workspace_.describe(document_);
    auto described = ui_.set_controls(controls);
    if (!described) throw std::runtime_error("UI rejected editor controls (key " + std::to_string(described.error().key) + ").");
    effects(*described);
    auto gc = document_.collect_step();
    if (!gc) throw std::runtime_error(gc.error().message);
    if (!target.drawable) { MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE); return true; }
    auto prepared = ui_.prepare();
    if (!prepared) {
        if (!ui_.presented()) throw std::runtime_error("Initial editor frame preparation failed (UI code " + std::to_string(static_cast<int>(prepared.error().code)) + ").");
        workspace_.status = L"Frame preparation failed; previous frame retained";
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        return true;
    }
    const auto& frame = **prepared;
    const auto* viewport = frame.find(editor::control::viewport);
    if (!viewport) throw std::runtime_error("Editor viewport is missing.");
    cubes_.clear();
    for (const auto& member : document_.scene().members()) {
        const auto position = take(document_.scene().position(member.key));
        cubes_.push_back({{position.x, position.y, position.z}, {.19f, .42f, .57f, 1}, member.key == document_.selected()});
    }
    const auto camera = camera_.matrices(viewport->bounds.width / std::max(viewport->bounds.height, 1.f));
    renderer::Frame render_frame{*frame.drawing, {camera.view, camera.projection}, viewport->bounds, cubes_};
    auto rendered = renderer_.render(render_frame, capture);
    if (!rendered) {
        if (rendered.error().terminal) throw std::runtime_error("Renderer stopped: " + rendered.error().cause.message);
        workspace_.status = widen(rendered.error().cause.message); return true;
    }
    if (rendered->status == renderer::FrameStatus::accepted) {
        if (rendered->accepted_id != frame.drawing->id || rendered->accepted_extent != frame.drawing->extent)
            throw std::runtime_error("Renderer accepted a mismatched frame.");
        if (!ui_.accept(*prepared)) throw std::runtime_error("UI rejected an accepted renderer frame.");
        ++accepted_frames_;
    } else MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    return true;
}
void Application::close() {
    if (closed_) return;
    closed_ = true;
    effects(ui_.cancel_interaction());
    const auto cancelled = document_.cancel_position();
    if (!cancelled) throw std::runtime_error(cancelled.error().message);
    window_.capture(false);
    const auto stopped = renderer_.close();
    if (!stopped) throw std::runtime_error(stopped.error().cause.message);
}
}

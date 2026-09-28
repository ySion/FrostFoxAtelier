#include "application/application.hpp"
#include <windowsx.h>
#include <algorithm>
#include <chrono>
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
    case 'C': return ui::Key::c; case 'X': return ui::Key::x; case 'V': return ui::Key::v;
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
Result<void> Application::action(const ui::Action& event) {
    using namespace editor::control;
    const auto invalid = [] { return std::unexpected(Error{ErrorCode::invalid_definition, "UI action does not match its live binding or value type."}); };
    const auto done = [&](Result<void> result, const wchar_t* message) -> Result<void> {
        if (result) workspace_.status = message;
        return result;
    };
    if (event.key == parameter_search) {
        const auto* value = std::get_if<std::wstring>(&event.value);
        if (!value) return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit || event.kind == ui::ActionKind::edit_cancel)
            workspace_.query = *value;
        return {};
    }
    if (event.key == splitter) {
        const auto* value = std::get_if<double>(&event.value);
        if (!value) return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit || event.kind == ui::ActionKind::edit_cancel)
            workspace_.parameter_width = static_cast<float>(-*value);
        return {};
    }
    if (event.key >= position_base) {
        if (event.kind == ui::ActionKind::edit_cancel) return done(document_.cancel_position(), L"Edit cancelled");
        const auto bound = (event.key - position_base) / 4;
        const auto axis = static_cast<std::size_t>((event.key - position_base) % 4);
        const auto* value = std::get_if<double>(&event.value);
        if (bound != document_.selected() || axis >= 3 || !value) return invalid();
        switch (event.kind) {
        case ui::ActionKind::edit_begin: return done(document_.begin_position(), L"Editing position / Esc cancels");
        case ui::ActionKind::edit_preview:
            return done(document_.preview_position(axis, static_cast<float>(*value)), L"Position preview / Esc cancels");
        case ui::ActionKind::edit_commit: {
            auto preview = document_.preview_position(axis, static_cast<float>(*value));
            if (!preview) return preview;
            return done(document_.commit_position(), L"Position committed");
        }
        default: return {};
        }
    }
    if (event.kind == ui::ActionKind::select && event.key >= first_object)
        return done(document_.select(event.key - first_object), L"Object selected");
    if (event.kind != ui::ActionKind::activate) return {};
    switch (event.key) {
    case add: {
        const auto count = document_.scene().members().size();
        const auto created = document_.add({static_cast<float>(count % 5) * 1.6f, 0, static_cast<float>(count / 5) * 1.6f});
        if (!created) return std::unexpected(created.error());
        workspace_.status = L"Cube added"; return {};
    }
    case remove: return done(document_.erase_selected(), L"Cube deleted / Undo restores it");
    case undo: return done(document_.undo(), L"Undo");
    case redo: return done(document_.redo(), L"Redo");
    case focus: {
        auto point = document_.scene().position(document_.selected());
        if (!point) return std::unexpected(point.error());
        camera_.focus(*point); workspace_.status = L"Focused selection"; return {};
    }
    case reset: camera_.reset(); workspace_.status = L"Camera reset"; return {};
    default: return {};
    }
}
void Application::effects(const ui::Reply& reply) {
    std::wstring first_error;
    bool business_failed = false;
    const auto remember = [&](std::wstring message) { if (first_error.empty()) first_error = std::move(message); };
    const auto apply = [&](auto&& self, const ui::Reply& current, bool cleanup) -> void {
        if (current.error) {
            const auto& error = *current.error;
            if (error.code == ui::ErrorCode::platform_failed)
                remember(L"Input operation failed (Windows " + std::to_wstring(error.native_code) + L").");
            else if (error.code == ui::ErrorCode::capacity_exceeded) remember(L"Input is too long.");
            else remember(L"Input must be valid single-line text.");
        }
        for (const auto& effect : current.effects) {
            if (business_failed && !cleanup) break;
            if (const auto* command = std::get_if<ui::Action>(&effect)) {
                if (cleanup && command->kind != ui::ActionKind::edit_cancel) continue;
                const auto result = action(*command);
                if (!result) {
                    if (cleanup) first_error += L"; rollback failed: " + widen(result.error().message);
                    else { remember(widen(result.error().message)); business_failed = true; }
                }
            } else {
                const auto& request = std::get<ui::PlatformRequest>(effect);
                ui::Event response;
                if (const auto* clipboard = std::get_if<ui::ClipboardRequest>(&request)) {
                    if (cleanup) continue;
                    response.kind = ui::EventKind::clipboard_result;
                    const auto target = ui_.text_input();
                    response.data = target && target->token == clipboard->token ? window_.clipboard(*clipboard) :
                        ui::ClipboardResult{clipboard->kind, clipboard->operation_id, clipboard->token, false, ERROR_INVALID_STATE, {}};
                } else {
                    const auto& ime = std::get<ui::ImeRequest>(request);
                    if (cleanup && ime.kind != ui::ImeKind::cancel) continue;
                    response.kind = ui::EventKind::ime_result; response.data = window_.ime(ime);
                }
                self(self, ui_.input(response), cleanup);
            }
        }
        if (current.capture == ui::Capture::release) window_.capture(false);
        else if (current.capture == ui::Capture::acquire && !business_failed) window_.capture(true);
        const wchar_t* cursor = IDC_ARROW;
        if (current.cursor == ui::Cursor::text) cursor = IDC_IBEAM;
        else if (current.cursor == ui::Cursor::drag_horizontal || current.cursor == ui::Cursor::resize_horizontal) cursor = IDC_SIZEWE;
        else if (current.cursor == ui::Cursor::resize_vertical) cursor = IDC_SIZENS;
        SetCursor(LoadCursorW(nullptr, cursor));
    };
    apply(apply, reply, false);
    if (business_failed) {
        apply(apply, ui_.cancel_interaction(), true);
        const auto cancelled = document_.cancel_position();
        if (!cancelled) first_error += L"; rollback failed: " + widen(cancelled.error().message);
        window_.capture(false);
    }
    const auto native_error = window_.text_input(ui_.text_input());
    if (native_error) remember(L"Text input unavailable (Windows " + std::to_wstring(native_error) + L").");
    if (!first_error.empty()) workspace_.status = std::move(first_error);
}
void Application::sync_text_input() {
    const auto error = window_.text_input(ui_.text_input());
    if (error) workspace_.status = L"Text input unavailable (Windows " + std::to_wstring(error) + L").";
}
void Application::invalidate_surface() {
    ++surface_revision_;
    orbiting_ = false;
    effects(ui_.cancel_interaction());
    const auto cancelled = document_.cancel_position();
    if (!cancelled) workspace_.status += L"; rollback failed: " + widen(cancelled.error().message);
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
void Application::native(NativeEvent message) {
    if (message.message == WM_SIZE || message.message == WM_DPICHANGED) { invalidate_surface(); return; }
    if (message.message == WM_SETFOCUS) return;
    if (message.ime_owned) return;
    if (message.composition) {
        ui::Event composition;
        composition.kind = ui::EventKind::composition; composition.data = std::move(*message.composition);
        auto reply = ui_.input(composition);
        if (message.error) reply.error = ui::Error{ui::ErrorCode::platform_failed, 0, message.error};
        effects(reply);
        return;
    }
    if (message.error) { workspace_.status = L"Invalid native text input."; return; }
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
    case WM_CHAR: event.kind = ui::EventKind::text; event.data = std::move(message.text); break;
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
        const auto cancelled = document_.cancel_position();
        if (!cancelled) workspace_.status += L"; rollback failed: " + widen(cancelled.error().message);
        return;
    }
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        if (event.key == ui::Key::delete_key) status(document_.erase_selected(), L"Cube deleted");
        else if (event.control && event.key == ui::Key::z) status(event.shift ? document_.redo() : document_.undo(), event.shift ? L"Redo" : L"Undo");
        else if (event.control && event.key == ui::Key::y) status(document_.redo(), L"Redo");
        else if (event.key == ui::Key::escape) status(document_.cancel_position(), L"Edit cancelled");
        else if (message.wparam == 'F' && !ui_.wants_keyboard()) {
            const auto point = document_.scene().position(document_.selected()); if (point) camera_.focus(*point);
        }
    }
}
bool Application::step(renderer::Capture* capture) {
    frame_timing_ = {};
    frame_statistics_ = {};
    if (closed_) return false;
    window_.poll([&](NativeEvent&& event) { native(std::move(event)); });
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
    using Clock = std::chrono::steady_clock;
    const bool sample = sampling_ && !capture;
    const auto prepare_start = sample ? Clock::now() : Clock::time_point{};
    auto prepared = ui_.prepare();
    const auto prepare_us = sample ? std::chrono::duration<double, std::micro>(Clock::now() - prepare_start).count() : 0;
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
    const auto render_start = sample ? Clock::now() : Clock::time_point{};
    auto rendered = renderer_.render(render_frame, capture);
    const auto render_us = sample ? std::chrono::duration<double, std::micro>(Clock::now() - render_start).count() : 0;
    if (!rendered) {
        if (rendered.error().terminal) throw std::runtime_error("Renderer stopped: " + rendered.error().cause.message);
        workspace_.status = widen(rendered.error().cause.message); return true;
    }
    frame_statistics_ = rendered->stats;
    if (rendered->status == renderer::FrameStatus::accepted) {
        if (rendered->accepted_id != frame.drawing->id || rendered->accepted_extent != frame.drawing->extent)
            throw std::runtime_error("Renderer accepted a mismatched frame.");
        if (!ui_.accept(*prepared)) throw std::runtime_error("UI rejected an accepted renderer frame.");
        ++accepted_frames_;
        sync_text_input();
        if (sample) frame_timing_ = {rendered->accepted_id, prepare_us, render_us};
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

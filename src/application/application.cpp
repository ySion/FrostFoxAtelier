#include "application/application.hpp"
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace atelier::application {
namespace {
Result<void> completion(Result<void> result) { return result; }
Result<void> completion(Result<world::Commit> result) {
    if (!result) return std::unexpected(result.error());
    if (result->notification_error) {
        auto error = std::move(*result->notification_error);
        error.message = "Operation committed; lifecycle notification failed: " + error.message;
        return std::unexpected(std::move(error));
    }
    return {};
}
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
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
    case VK_UP: return ui::Key::up; case VK_DOWN: return ui::Key::down;
    case VK_APPS: return ui::Key::menu; case VK_F10: return ui::Key::f10;
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
std::filesystem::path editor_directory() {
    std::wstring executable(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if (!length || length>=executable.size()) throw std::runtime_error("Cannot locate editor style assets");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}
std::vector<std::wstring> editor_fonts() {
    const auto directory=editor_directory()/"fonts";
    std::vector<std::wstring> paths;
    for (const auto* file:{L"SarasaUiSC-Regular.ttf",L"SarasaUiSC-SemiBold.ttf",
                          L"SourceCodePro-Regular.otf",L"SourceCodePro-Bold.otf"}) paths.push_back((directory/file).wstring());
    return paths;
}
std::shared_ptr<const ui::StyleSheet> editor_styles() {
    const auto path=editor_directory()/"editor.ss";
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open editor.ss beside the executable");
    const auto size=file.tellg();
    if (size<0 || size>1024*1024) throw std::runtime_error("Invalid or oversized editor.ss");
    std::string source(static_cast<std::size_t>(size),'\0');
    file.seekg(0);
    if (!file.read(source.data(),static_cast<std::streamsize>(source.size()))) throw std::runtime_error("Cannot read editor.ss");
    auto parsed=ui::StyleSheet::parse(source);
    if (!parsed) throw std::runtime_error("editor.ss:"+std::to_string(parsed.error().line)+":"+
        std::to_string(parsed.error().column)+": "+parsed.error().message);
    return std::move(*parsed);
}
}
Application::Application(bool validation) : ui_(editor_fonts()),gpu_validation_(validation) {
    auto style=ui_.style(); style.font.family=L"Sarasa UI SC"; style.font.size=13; style.caption_scale=1; style.title_scale=7.f/6;
    if (!ui_.set_style(style)) throw std::runtime_error("Cannot publish editor typography");
    style_sheet_=editor_styles();
    if (!ui_.set_style_sheet(style_sheet_)) throw std::runtime_error("Cannot publish editor style sheet");
    take(completion(document_.add_actor()));
    take(completion(document_.add_component(document_.types().transform)));
    take(completion(document_.add_component(document_.types().cube)));
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
void Application::status(Result<world::Commit> result, const wchar_t* success) {
    status(completion(std::move(result)), success);
}

Result<void> Application::focus_selected() {
    auto transform = world::root_transform(document_.world().runtime(), document_.selected());
    if (!transform) return std::unexpected(transform.error());
    if (!*transform) return std::unexpected(Error{ErrorCode::invalid_definition, "The Actor has no Transform to focus."});
    auto matrix = world::world_matrix(document_.world().runtime(), **transform);
    if (!matrix) return std::unexpected(matrix.error());
    camera_.focus({(*matrix)[12], (*matrix)[13], (*matrix)[14]});
    return {};
}
Result<void> Application::action(const ui::Action& event) {
    using namespace editor::control;
    const auto invalid = [] { return std::unexpected(Error{ErrorCode::invalid_definition, "UI action does not match its live binding or value type."}); };
    const auto done = [&](auto result, const wchar_t* message) -> Result<void> {
        auto finished = completion(std::move(result));
        if (finished) workspace_.status = message;
        return finished;
    };
    if (event.kind==ui::ActionKind::context_menu) {
        const auto* request=std::get_if<ui::ContextMenuRequest>(&event.value);
        if (!request) return invalid();
        request_menu(*request); return {};
    }
    if (event.kind==ui::ActionKind::dismiss) { close_menu(); return {}; }
    if (event.kind==ui::ActionKind::activate && event.key==menu_bar_handle) {
        close_menu(); workspace_.menu_bar_collapsed=!workspace_.menu_bar_collapsed; return {};
    }
    if (event.kind==ui::ActionKind::activate && is_menu(event.key)) {
        request_menu({event.key,0,0,0}); return {};
    }
    if (event.kind==ui::ActionKind::activate && event.key==menu_group_reset) {
        if (!workspace_.menu) return {};
        return action({ui::ActionKind::reset,workspace_.menu->group?workspace_.menu->group:workspace_.menu->source,{}});
    }
    if (event.key==reset_layout && event.kind==ui::ActionKind::activate) {
        workspace_.reset_layout(); workspace_.status=L"Default workspace restored"; return {};
    }
    if (ui_.presented() && workspace_.dock.apply(event,*ui_.presented())) return {};
    if (workspace_.workbench.apply(event)) return {};
    if (workspace_.sample_history() && event.kind==ui::ActionKind::activate && (event.key==undo || event.key==redo)) {
        if (event.key==undo) workspace_.workbench.undo(); else workspace_.workbench.redo();
        workspace_.status=L"Sample history updated"; return {};
    }
    if (event.key==position_fold && event.kind==ui::ActionKind::activate) {
        const auto* expanded=std::get_if<bool>(&event.value);
        if (!expanded) return invalid();
        workspace_.position_expanded=*expanded; return {};
    }
    if ((event.key==parameter_transform || event.key==parameter_components) && event.kind==ui::ActionKind::activate) {
        workspace_.parameter_page=event.key; return {};
    }
    if (event.kind==ui::ActionKind::activate) {
        if (const auto direction=navigation_direction(event.key)) return done(workspace_.navigate(document_,direction),L"Selection history");
        if (event.key==world_context) return done(document_.select({}),L"World context");
    }
    if (event.key == parameter_search) {
        const auto* value = std::get_if<std::wstring>(&event.value);
        if (!value) return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit || event.kind == ui::ActionKind::edit_cancel) {
            workspace_.query = *value;
            workspace_.status = event.kind == ui::ActionKind::edit_cancel ? L"Filter cancelled" : L"Parameters filtered";
        }
        return {};
    }
    // A retired control's cleanup must not cancel a different live edit.
    if (event.kind == ui::ActionKind::edit_cancel)
        return document_.owns_edit(event.key) ? done(document_.cancel_edit(), L"Edit cancelled") : Result<void>{};
    if (const auto* binding = workspace_.binding(event.key)) {
        if (binding->kind==editor::Workspace::Binding::Kind::navigation) {
            if (event.kind==ui::ActionKind::activate) return done(document_.select(binding->object),L"Actor selected");
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::name) {
            if (event.kind==ui::ActionKind::edit_commit)
                return done(document_.rename(binding->object,std::get<std::wstring>(event.value)),L"Actor renamed / Undo restores its name");
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::position_default) {
            if (event.kind!=ui::ActionKind::reset) return invalid();
            return done(document_.reset_position(binding->object),L"Position restored to default / Undo restores the vector");
        }
        if (binding->path.empty()) {
            if (event.kind == ui::ActionKind::select) return done(document_.select(binding->object), L"Actor selected");
            if (event.kind == ui::ActionKind::activate) return done(document_.remove_component(binding->object), L"Component removed / Undo restores it");
            return invalid();
        }
        const auto* value = std::get_if<double>(&event.value);
        if (!value) return invalid();
        switch (event.kind) {
        case ui::ActionKind::edit_begin:
            return done(document_.begin_edit(binding->object, binding->path, event.key), L"Editing field / Esc cancels");
        case ui::ActionKind::edit_preview:
            return done(document_.preview_edit(event.key, static_cast<float>(*value)), L"Field preview / Esc cancels");
        case ui::ActionKind::edit_commit: {
            auto preview = document_.preview_edit(event.key, static_cast<float>(*value));
            if (!preview) return preview;
            return done(document_.commit_edit(event.key), L"Field committed");
        }
        default: return {};
        }
    }
    if (event.kind != ui::ActionKind::activate) return {}; // Includes retired field/Actor bindings.
    switch (event.key) {
    case exit: PostMessageW(window(),WM_CLOSE,0,0); return {};
    case show_scene: workspace_.dock.select(scene_tab); return {};
    case show_parameters: workspace_.dock.select(parameters_tab); return {};
    case show_controls: workspace_.dock.select(workbench); return {};
    case show_actors: workspace_.dock.select(actors_tab); return {};
    case maximize_pane: workspace_.dock.maximize(workspace_.dock.focused_tab()); return {};
    case add_actor: return done(document_.add_actor(), L"Empty Actor added");
    case remove_actor: return done(document_.erase_selected(), L"Actor removed / Undo restores it");
    case add_transform: return done(document_.add_component(document_.types().transform), L"Transform added");
    case add_cube: return done(document_.add_component(document_.types().cube), L"Cube component added");
    case undo: return done(document_.undo(), L"Undo");
    case redo: return done(document_.redo(), L"Redo");
    case focus: return done(focus_selected(), L"Focused selection");
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
                if (cleanup && command->kind != ui::ActionKind::edit_cancel && command->kind!=ui::ActionKind::dismiss && command->kind!=ui::ActionKind::drag_cancel) continue;
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
                    auto result = window_.ime(ime);
                    if (!result.success) {
                        const auto message = L"IME operation failed (Windows " + std::to_wstring(result.native_error) + L").";
                        if ((cleanup || ime.kind == ui::ImeKind::cancel) && !first_error.empty()) first_error += L"; " + message;
                        else remember(message);
                    }
                    response.kind = ui::EventKind::ime_result; response.data = std::move(result);
                }
                self(self, ui_.input(response), cleanup);
            }
        }
        if (current.capture == ui::Capture::release) window_.capture(false);
        else if (current.capture == ui::Capture::acquire && !business_failed) window_.capture(true);
        if (current.cursor) window_.cursor(*current.cursor);
    };
    apply(apply, reply, false);
    if (business_failed) {
        apply(apply, ui_.cancel_interaction(), true);
        const auto cancelled = document_.cancel_edit();
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
    close_menu(false);
    ++surface_revision_;
    orbiting_ = false;
    effects(ui_.cancel_interaction());
    const auto cancelled = document_.cancel_edit();
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
void Application::request_menu(const ui::ContextMenuRequest& request) {
    const auto* source=ui_.presented()?ui_.presented()->find(request.source):nullptr;
    if (!source || !source->enabled) return;
    if (!editor::control::is_menu(request.source)) menu_return_focus_=ui_.focused_control();
    // The native menu owns subsequent mouse-up events. Retire the previous
    // gesture through its UI/business transaction before transferring capture.
    effects(ui_.cancel_interaction());
    workspace_.menu=request; menu_incarnation_=source->incarnation;
    orbiting_=false; window_.capture(false);
}
void Application::close_menu(bool restore_focus) {
    pending_menu_=0;
    const bool was_open=workspace_.menu.has_value() || menu_window();
    if (!was_open) return;
    workspace_.menu.reset(); workspace_.menu_items.clear();
    if (popup_) popup_->hide();
    if (was_open) {
        window_.capture(false);
        if (restore_focus) effects(ui_.focus(menu_return_focus_));
    }
    menu_return_focus_=0; menu_incarnation_=0;
}
void Application::update_menu() {
    if (!workspace_.menu || workspace_.menu_items.empty()) { close_menu(); return; }
    const auto request=*workspace_.menu;
    const auto* source=ui_.presented()->find(request.source);
    if (!source || source->incarnation!=menu_incarnation_ || !source->enabled ||
        source->bounds.y>=source->clip.bottom || source->bounds.y+source->bounds.height<=source->clip.top) { close_menu(); return; }
    const bool below=editor::control::is_menu(source->key) || source->kind==ui::ControlKind::combo;
    POINT points[2];
    if (below) {
        points[0]={static_cast<LONG>(source->bounds.x),static_cast<LONG>(source->bounds.y)};
        points[1]={static_cast<LONG>(source->bounds.x+source->bounds.width),static_cast<LONG>(source->bounds.y+source->bounds.height)};
    } else points[0]=points[1]={static_cast<LONG>(request.x),static_cast<LONG>(request.y)};
    MapWindowPoints(window(),nullptr,points,2);
    if (!popup_) popup_=std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_);
    popup_->update(source->key,{points[0].x,points[0].y,points[1].x,points[1].y},below,workspace_.menu_items,ui_.style(),style_sheet_);
    window_.capture(true); popup_->render();
}
bool Application::menu_input(const ui::Event& input,WPARAM native_key) {
    if (!workspace_.menu) return false;
    const auto source=workspace_.menu->source;
    const bool top=editor::control::is_menu(source);
    auto menu_at=ui::ControlKey{};
    if (ui_.presented()) for (const auto& box:ui_.presented()->controls)
        if (editor::control::is_menu(box.key) && input.x>=box.bounds.x && input.x<box.bounds.x+box.bounds.width &&
            input.y>=box.bounds.y && input.y<box.bounds.y+box.bounds.height) { menu_at=box.key; break; }
    const auto change=[&](ui::ControlKey key) {
        effects(ui_.focus(key)); request_menu({key,0,0,0});
    };
    if (top && input.kind==ui::EventKind::pointer_move && menu_at && menu_at!=source) { change(menu_at); return true; }
    if (input.kind==ui::EventKind::pointer_down && menu_at) {
        if (menu_at==source) close_menu(); else change(menu_at);
        return true;
    }
    if (input.kind==ui::EventKind::key_down && (input.key==ui::Key::f10 || native_key==VK_MENU)) { close_menu(); return true; }
    if (top && input.kind==ui::EventKind::key_down && (input.key==ui::Key::left || input.key==ui::Key::right)) {
        change(editor::control::file_menu+(source-editor::control::file_menu+5+(input.key==ui::Key::left?-1:1))%5); return true;
    }
    if (!menu_window()) return true; // Opening transition is modal before the first accepted popup frame.
    auto event=input;
    POINT point{static_cast<LONG>(event.x),static_cast<LONG>(event.y)};
    MapWindowPoints(window(),popup_->window(),&point,1);
    event.x=static_cast<float>(point.x); event.y=static_cast<float>(point.y);
    const auto reply=popup_->input(event);
    for (const auto& effect:reply.effects) if (const auto* command=std::get_if<ui::Action>(&effect)) {
        if (command->kind==ui::ActionKind::activate) {
            const auto at=std::find_if(workspace_.menu_items.begin(),workspace_.menu_items.end(),[&](const auto& item){return item.key==command->key;});
            if (at==workspace_.menu_items.end() || !at->enabled) continue;
            const auto edit=at->edit_command;
            if (edit) {
                const auto incarnation=menu_incarnation_;
                close_menu(); effects(ui_.execute(*edit,incarnation));
            } else {
                const auto result=action(*command);
                if (!result) status(result,L"");
                close_menu();
            }
        } else if (command->kind==ui::ActionKind::dismiss) close_menu();
    }
    return true;
}
void Application::native(NativeEvent message) {
    if (message.message == WM_SIZE || message.message == WM_DPICHANGED) { invalidate_surface(); return; }
    if (message.message==WM_MOVE) { close_menu(false); return; }
    if (message.message==WM_CANCELMODE || (message.message==WM_ACTIVATEAPP && !message.wparam)) {
        close_menu(false); effects(ui_.cancel_interaction()); window_.capture(false); return;
    }
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
    case WM_CAPTURECHANGED: event.kind = ui::EventKind::capture_lost; orbiting_ = false; window_.cursor(ui::Cursor::arrow); break;
    default: return;
    }
    if (event.kind==ui::EventKind::focus_lost || event.kind==ui::EventKind::capture_lost) close_menu(false);
    if (menu_input(event,message.wparam)) return;
    if (event.kind==ui::EventKind::key_down && ((event.key==ui::Key::f10 && !event.shift) ||
        (event.alt && (message.wparam=='F' || message.wparam=='E' || message.wparam=='C' || message.wparam=='V' || message.wparam=='W')))) {
        const auto menu=message.wparam=='E'?editor::control::edit_menu:message.wparam=='C'?editor::control::create_menu:
            message.wparam=='V'?editor::control::view_menu:message.wparam=='W'?editor::control::window_menu:editor::control::file_menu;
        menu_return_focus_=ui_.focused_control();
        if (workspace_.menu_bar_collapsed) { workspace_.menu_bar_collapsed=false; pending_menu_=menu; }
        else { effects(ui_.focus(menu)); request_menu({menu,0,0,0}); }
        return;
    }
    if (event.kind==ui::EventKind::pointer_down && ui_.presented())
        for (const auto& box:ui_.presented()->controls) if (editor::control::is_menu(box.key) &&
            event.x>=box.bounds.x && event.x<box.bounds.x+box.bounds.width && event.y>=box.bounds.y && event.y<box.bounds.y+box.bounds.height)
            menu_return_focus_=ui_.focused_control();
    const bool release = event.kind == ui::EventKind::pointer_up;
    if (pointer && !release && !pointer_ready()) return;
    if (event.kind==ui::EventKind::key_down && event.control && !workspace_.menu &&
        (message.wparam==VK_TAB || message.wparam=='B' || message.wparam=='W')) {
        effects(ui_.cancel_interaction());
        if (message.wparam==VK_TAB) workspace_.dock.cycle(event.shift?-1:1);
        else if (message.wparam=='B') workspace_.dock.maximize(workspace_.dock.focused_tab());
        else workspace_.dock.close(workspace_.dock.focused_tab());
        return;
    }
    if (pointer && !workspace_.menu && !workspace_.dock.dragging() && over_viewport(event.x, event.y)) {
        if (event.kind==ui::EventKind::pointer_down) workspace_.dock.select(editor::control::scene_tab);
        if (event.kind == ui::EventKind::pointer_down && event.button == ui::PointerButton::secondary) {
            effects(ui_.cancel_interaction());
            orbiting_ = true; last_x_ = event.x; last_y_ = event.y; window_.capture(true); window_.cursor(ui::Cursor::pan); return;
        }
        if (event.kind == ui::EventKind::wheel) { camera_.zoom(event.wheel); return; }
    }
    if (orbiting_ && event.kind == ui::EventKind::pointer_move) {
        const auto dpi = window_.dpi_scale(); camera_.orbit((event.x - last_x_) / dpi, (event.y - last_y_) / dpi);
        last_x_ = event.x; last_y_ = event.y; return;
    }
    if (orbiting_ && release && event.button == ui::PointerButton::secondary) { orbiting_ = false; window_.capture(false); window_.cursor(ui::Cursor::arrow); return; }
    auto reply = ui_.input(event);
    if (ui_.presented()) workspace_.dock.focus_control(ui_.focused_control(),*ui_.presented());
    effects(reply);
    if (event.kind == ui::EventKind::focus_lost || event.kind == ui::EventKind::capture_lost) {
        window_.capture(false); // Includes camera capture, which is owned outside UI.
        const auto cancelled = document_.cancel_edit();
        if (!cancelled) workspace_.status += L"; rollback failed: " + widen(cancelled.error().message);
        return;
    }
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        if (workspace_.sample_history()) {
            if (event.control && (event.key==ui::Key::z || event.key==ui::Key::y)) {
                const bool redo=event.key==ui::Key::y || event.shift;
                status(action({ui::ActionKind::activate,redo?editor::control::redo:editor::control::undo,{}}),L"Sample history updated");
            }
            return;
        }
        if (event.key == ui::Key::delete_key) status(document_.erase_selected(), L"Actor removed");
        else if (event.control && event.key == ui::Key::z) status(event.shift ? document_.redo() : document_.undo(), event.shift ? L"Redo" : L"Undo");
        else if (event.control && event.key == ui::Key::y) status(document_.redo(), L"Redo");
        else if (event.key == ui::Key::escape) status(document_.cancel_edit(), L"Edit cancelled");
        else if (message.wparam == 'F' && !ui_.wants_keyboard()) {
            status(focus_selected(), L"Focused selection");
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
    auto controls = workspace_.describe(document_,{static_cast<float>(extent.width)/surface_.dpi_scale,static_cast<float>(extent.height)/surface_.dpi_scale},ui_.presented().get());
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
    const auto scene_bounds=viewport?viewport->bounds:ui::Rect{};
    cubes_.clear();
    const auto items = viewport?take(document_.world().render_items()):std::vector<world::RenderItem>{};
    cubes_.reserve(items.size());
    for (const auto& item : items)
        cubes_.push_back({item.world, {.19f, .42f, .57f, 1}, item.actor == document_.selected()});
    const auto camera = camera_.matrices(std::max(scene_bounds.width,1.f) / std::max(scene_bounds.height,1.f));
    renderer::Frame render_frame{*frame.drawing, {camera.view, camera.projection}, scene_bounds, cubes_};
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
        if (pending_menu_) {
            const auto menu=pending_menu_; pending_menu_=0;
            effects(ui_.focus(menu)); request_menu({menu,0,0,0});
        } else update_menu();
        if (sample) frame_timing_ = {rendered->accepted_id, prepare_us, render_us};
    } else MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    return true;
}
void Application::close() {
    if (closed_) return;
    closed_ = true;
    close_menu(false);
    if (popup_) popup_->close();
    effects(ui_.cancel_interaction());
    const auto cancelled = document_.cancel_edit();
    if (!cancelled) throw std::runtime_error(cancelled.error().message);
    window_.capture(false);
    take(completion(document_.close()));
    const auto stopped = renderer_.close();
    if (!stopped) throw std::runtime_error(stopped.error().cause.message);
}
}

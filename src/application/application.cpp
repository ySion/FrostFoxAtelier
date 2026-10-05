#include "application/application.hpp"
#include "atelier/build_info.hpp"
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
std::vector<std::wstring> editor_fonts() {
    const auto directory=editor::asset_directory()/"fonts";
    std::vector<std::wstring> paths;
    for (const auto* file:{L"SarasaUiSC-Regular.ttf",L"SarasaUiSC-SemiBold.ttf",
                          L"SourceCodePro-Regular.otf",L"SourceCodePro-Bold.otf"}) paths.push_back((directory/file).wstring());
    return paths;
}
std::shared_ptr<const ui::StyleSheet> editor_styles() {
    const auto path=editor::asset_directory()/"editor.ss";
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
    auto style=ui_.style(); style.font.family=L"Sarasa UI SC"; style.font.size=12.5f; style.caption_scale=1; style.title_scale=7.f/6;
    if (!ui_.set_style(style)) throw std::runtime_error("Cannot publish editor typography");
    style_sheet_=editor_styles();
    if (!ui_.set_style_sheet(style_sheet_)) throw std::runtime_error("Cannot publish editor style sheet");
    renderer::Options options;
    options.validation = validation ? rhi::Validation::gpu : rhi::Validation::disabled;
    renderer_ = graphics(renderer::Renderer::open(window_.handle(), options));
    popup_=std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_);
    submenus_.push_back(std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_));
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
    auto transform = world::root_component(document_->world().runtime(), document_->selected());
    if (!transform) return std::unexpected(transform.error());
    if (!*transform) return std::unexpected(Error{ErrorCode::invalid_definition, "The Actor has no root SceneComponent to focus."});
    auto matrix = world::world_matrix(document_->world().runtime(), **transform);
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
    if (event.kind==ui::ActionKind::activate && (event.key==menu_bar_handle || event.key==shelf_handle)) {
        close_menu();
        auto& collapsed=event.key==shelf_handle?workspace_.shelf_collapsed:workspace_.menu_bar_collapsed;
        collapsed=!collapsed; return {};
    }
    if (event.kind==ui::ActionKind::activate && is_menu(event.key)) {
        request_menu({event.key,0,0,0}); return {};
    }
    if (event.kind==ui::ActionKind::activate && event.key==menu_group_reset) {
        if (!workspace_.menu) return {};
        return action({ui::ActionKind::reset,workspace_.menu->group?workspace_.menu->group:workspace_.menu->source,{}});
    }
    if (ui_.presented() && workspace_.dock.apply(event,*ui_.presented())) return {};
    if (workspace_.workbench.apply(event)) return {};
    if (workspace_.assets.apply(event)) return {};
    if (workspace_.apply_view(event)) return {};
    if (event.kind==ui::ActionKind::activate && event.key==physics_root_add)
        return done(document_->add_component(document_->types().scene_component),L"Scene root added");
    if (event.kind==ui::ActionKind::activate && event.key==physics_add)
        return done(document_->add_component(document_->physics_types().body_component),L"Physics body added");
    if (event.kind==ui::ActionKind::activate && event.key==physics_remove)
        return done(document_->remove_component(workspace_.physics_component()),L"Physics body removed");
    const bool physics_choice=event.kind==ui::ActionKind::activate &&
        ((event.key>=physics_shape_first && event.key<physics_shape_first+4) ||
         (event.key>=physics_motion_first && event.key<physics_motion_first+3) || event.key==physics_sensor);
    const bool physics_number=event.key>=physics_friction && event.key<=physics_extent_z;
    if (physics_choice || physics_number) {
        if (physics_number && event.kind!=ui::ActionKind::edit_commit) return {};
        const auto body=workspace_.physics_component();
        if (body==ObjectId{}) return invalid();
        auto settings=physics::read_settings(document_->world().runtime(),body);
        if (!settings) return std::unexpected(settings.error());
        if (event.key>=physics_shape_first && event.key<physics_shape_first+4) {
            settings->shape=static_cast<physics::ShapeKind>(event.key-physics_shape_first);
            if (settings->shape==physics::ShapeKind::triangle_mesh) settings->motion=physics::MotionKind::stationary;
            else settings->cooked_shape.reset();
        } else if (event.key>=physics_motion_first && event.key<physics_motion_first+3)
            settings->motion=static_cast<physics::MotionKind>(event.key-physics_motion_first);
        else if (event.key==physics_sensor) {
            const auto* selected=std::get_if<bool>(&event.value);
            if (!selected) return invalid();
            settings->sensor=*selected;
        } else {
            const auto* value=std::get_if<double>(&event.value);
            if (!value || !std::isfinite(*value)) return invalid();
            const auto number=static_cast<float>(*value);
            switch (event.key) {
            case physics_friction: settings->material.friction=number; break;
            case physics_restitution: settings->material.restitution=number; break;
            case physics_radius: settings->radius=number; break;
            case physics_capsule_height: settings->capsule_half_height=number; break;
            case physics_extent_x: settings->half_extents.x=number; break;
            case physics_extent_y: settings->half_extents.y=number; break;
            case physics_extent_z: settings->half_extents.z=number; break;
            default: return invalid();
            }
        }
        if (settings->shape==physics::ShapeKind::triangle_mesh && !settings->cooked_shape) {
            auto root=world::root_component(document_->world().runtime(),document_->selected());
            if (!root) return std::unexpected(root.error());
            if (!*root) return invalid();
            auto mesh=world::read_static_mesh(document_->world().runtime(),**root);
            if (!mesh) return std::unexpected(mesh.error());
            if (!mesh->mesh.resolved) return invalid();
            if (mesh->mesh.source.starts_with("project/")) {
                if (!project_) return invalid();
                auto transform=world::read_transform(document_->world().runtime(),**root);
                if (!transform) return std::unexpected(transform.error());
                auto cooked=project_->cook_collision(mesh->mesh.source,transform->scale);
                if (!cooked) return std::unexpected(cooked.error());
                auto bytes=project_->collision(mesh->mesh.source,transform->scale);
                if (!bytes) return std::unexpected(bytes.error());
                settings->cooked_shape=std::move(*bytes);
            }
        }
        return done(document_->configure_body(body,std::move(*settings)),L"Physics body configured");
    }
    const auto parameter_event=workspace_.apply_ui(event);
    if (!parameter_event) return std::unexpected(parameter_event.error());
    if (*parameter_event) return {};
    if (event.kind==ui::ActionKind::activate && (event.key==editor::browser_control::open || event.key==editor::browser_control::add)) {
        const auto selected=workspace_.assets.selected();
        if (!selected) return {};
        if (event.key==editor::browser_control::add && selected->kind==editor::AssetKind::mesh)
            return execute(editor::Command::create_static_mesh,selected->key);
        if (selected->kind==editor::AssetKind::folder) workspace_.assets.navigate(selected->path);
        else if (selected->kind==editor::AssetKind::scene) return open_scene(selected->path);
        return {};
    }
    if ((event.key==parameter_transform || event.key==parameter_components) && event.kind==ui::ActionKind::activate) {
        workspace_.parameter_page=event.key; return {};
    }
    if (event.kind==ui::ActionKind::activate) {
        if (const auto direction=navigation_direction(event.key)) return done(workspace_.navigate(*document_,direction),L"Selection history");
        if (event.key==world_context) return done(document_->select({}),L"World context");
    }
    if (event.kind==ui::ActionKind::select && event.key==tree_world)
        return done(workspace_.select_tree(*document_,event.key),L"World context");
    if (event.kind==ui::ActionKind::activate && (event.key==shelf_create || event.key==shelf_view)) {
        workspace_.shelf_page=event.key; return {};
    }
    if (event.key == parameter_search || event.key==tree_filter) {
        const auto* value = std::get_if<std::wstring>(&event.value);
        if (!value) return invalid();
        if (event.kind == ui::ActionKind::edit_preview || event.kind == ui::ActionKind::edit_commit || event.kind == ui::ActionKind::edit_cancel) {
            (event.key==tree_filter?workspace_.tree_query:workspace_.query) = *value;
            workspace_.status = event.kind == ui::ActionKind::edit_cancel ? L"Filter cancelled" :
                event.key==tree_filter?L"Tree filtered":L"Parameters filtered";
        }
        return {};
    }
    // A retired control's cleanup must not cancel a different live edit.
    if (event.kind == ui::ActionKind::edit_cancel)
        return document_->owns_edit(event.key) ? done(document_->cancel_edit(), L"Edit cancelled") : Result<void>{};
    if (const auto* binding = workspace_.binding(event.key)) {
        if (event.kind==ui::ActionKind::select && binding->kind==editor::Workspace::Binding::Kind::object && binding->path.empty())
            return done(workspace_.select_tree(*document_,event.key),L"Tree selection updated");
        if (binding->kind==editor::Workspace::Binding::Kind::navigation) {
            if (event.kind==ui::ActionKind::activate) return done(document_->select(binding->object),L"Actor selected");
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::name) {
            if (event.kind==ui::ActionKind::edit_commit)
                return done(document_->rename(binding->object,std::get<std::wstring>(event.value)),L"Actor renamed / Undo restores its name");
            return {};
        }
        if (binding->kind==editor::Workspace::Binding::Kind::position_default) {
            if (event.kind!=ui::ActionKind::reset) return invalid();
            return done(document_->reset_position(binding->object),L"Position restored to default / Undo restores the vector");
        }
        if (binding->path.empty()) {
            return invalid();
        }
        const auto* value = std::get_if<double>(&event.value);
        if (!value) return invalid();
        switch (event.kind) {
        case ui::ActionKind::edit_begin:
            return done(document_->begin_edit(binding->object, binding->path, event.key), L"Editing field / Esc cancels");
        case ui::ActionKind::edit_preview:
            return done(document_->preview_edit(event.key, static_cast<float>(*value)), L"Field preview / Esc cancels");
        case ui::ActionKind::edit_commit: {
            auto preview = document_->preview_edit(event.key, static_cast<float>(*value));
            if (!preview) return preview;
            return done(document_->commit_edit(event.key), L"Field committed");
        }
        default: return {};
        }
    }
    if (event.kind != ui::ActionKind::activate) return {}; // Includes retired field/Actor bindings.
    if (const auto* command=editor::command_definition(event.key)) return execute(command->id);
    return {};
}
Result<void> Application::execute(editor::Command command,std::string_view asset_key) {
    using enum editor::Command;
    if (!workspace_.command_enabled(command,*document_)) return {};
    Result<void> result;
    switch (command) {
    case new_project: case open_project: case save_scene: case save_scene_as: case open_scene: case import_mesh: case refresh_assets:
        workspace_.status=editor::command_definition(static_cast<std::uint64_t>(command))->title;
        return project_command(command,asset_key);
    case exit: PostMessageW(window(),WM_CLOSE,0,0); break;
    case create_actor: result=completion(document_->add_actor()); break;
    case create_static_mesh: {
        const auto catalog=editor::mesh_assets();
        if (asset_key.empty()) asset_key=catalog.front().key;
        if (project_) {
            auto asset=project_->mesh(asset_key);if (!asset) return std::unexpected(asset.error());
            result=completion(document_->create_static_mesh(*asset));
        } else {
            const auto* asset=editor::mesh_asset(asset_key);
            if (!asset) return std::unexpected(Error{ErrorCode::invalid_definition,"Unknown mesh asset."});
            result=completion(document_->create_static_mesh(*asset));
        }
        break;
    }
    case erase_actor: result=completion(document_->erase_selected()); break;
    case undo: case redo:
        if (workspace_.sample_history()) { if (command==undo) workspace_.workbench.undo(); else workspace_.workbench.redo(); }
        else result=completion(command==undo?document_->undo():document_->redo());
        break;
    case focus: result=focus_selected(); break;
    case reset_camera: camera_.reset(); break;
    case show_scene: workspace_.dock.select(editor::control::scene_tab); break;
    case show_parameters: workspace_.dock.select(editor::control::parameters_tab); break;
    case show_controls: workspace_.dock.select(editor::control::workbench); break;
    case show_actors: workspace_.dock.select(editor::control::actors_tab); break;
    case show_assets: workspace_.dock.select(editor::control::assets_tab); break;
    case maximize_pane: workspace_.dock.maximize(workspace_.dock.focused_tab()); break;
    case previous_tab: case next_tab: workspace_.dock.cycle(command==previous_tab?-1:1); break;
    case close_tab: workspace_.dock.close(workspace_.dock.focused_tab()); break;
    case reset_layout: workspace_.reset_layout(); break;
    case about: {
        close_menu(false);
        const auto details=L"FrostFox Atelier\n\nBuild: "+editor::wide(build_info::revision)+
            L"\nConfiguration: "+editor::wide(build_info::configuration)+L"\n\nProject:\n"+
            (project_?project_->root().wstring():L"No project open")+L"\n\nScene:\n"+
            (scene_path_.empty()?L"Untitled":scene_path_.generic_wstring());
        MessageBoxW(window(),details.c_str(),L"About FrostFox Atelier",MB_OK|MB_ICONINFORMATION);
        break;
    }
    }
    if (result) workspace_.status=editor::command_definition(static_cast<std::uint64_t>(command))->title;
    return result;
}
bool Application::effects(const ui::Reply& reply) {
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
        const auto cancelled = document_->cancel_edit();
        if (!cancelled) first_error += L"; rollback failed: " + widen(cancelled.error().message);
        window_.capture(false);
    }
    const auto native_error = window_.text_input(ui_.text_input());
    if (native_error) remember(L"Text input unavailable (Windows " + std::to_wstring(native_error) + L").");
    const bool success=first_error.empty();
    if (!success) workspace_.status = std::move(first_error);
    return success;
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
    const auto cancelled = document_->cancel_edit();
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
    for (auto& submenu:submenus_) submenu->hide();
    menu_path_.clear();
    workspace_.menu=request; menu_incarnation_=source->incarnation;
    orbiting_=false; window_.capture(false);
}
void Application::close_menu(bool restore_focus) {
    pending_menu_=0;
    const bool was_open=workspace_.menu.has_value() || menu_window();
    if (!was_open) return;
    workspace_.menu.reset(); workspace_.menu_items.clear();
    for (auto& submenu:submenus_) submenu->hide();
    menu_path_.clear();
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
    auto items=std::span<const ui::MenuItem>(workspace_.menu_items);
    auto* parent=popup_.get();
    for (std::size_t depth=0;depth<menu_path_.size();++depth) {
        const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.key==menu_path_[depth];});
        const auto* box=parent->presented()?parent->presented()->find(menu_path_[depth]):nullptr;
        if (at==items.end() || at->children.empty() || !at->enabled || !box) { menu_path_.resize(depth); break; }
        if (depth==submenus_.size()) submenus_.push_back(std::make_unique<PopupHost>(window(),editor_fonts(),gpu_validation_));
        POINT corners[2]{{static_cast<LONG>(box->bounds.x),static_cast<LONG>(box->bounds.y)},
                         {static_cast<LONG>(box->bounds.x+box->bounds.width),static_cast<LONG>(box->bounds.y+box->bounds.height)}};
        MapWindowPoints(parent->window(),nullptr,corners,2);
        auto& child=*submenus_[depth];
        child.update(at->key,{corners[0].x,corners[0].y,corners[1].x,corners[1].y},false,at->children,ui_.style(),style_sheet_);
        child.render(); parent=&child; items=at->children;
    }
    for (std::size_t depth=menu_path_.size();depth<submenus_.size();++depth) submenus_[depth]->hide();
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
    if (top && menu_path_.empty() && input.kind==ui::EventKind::key_down && (input.key==ui::Key::left || input.key==ui::Key::right)) {
        change(editor::cycle_menu(source,input.key==ui::Key::left)); return true;
    }
    if (!menu_window()) return true; // Opening transition is modal before the first accepted popup frame.
    const bool pointer=input.kind==ui::EventKind::pointer_down || input.kind==ui::EventKind::pointer_up ||
        input.kind==ui::EventKind::pointer_move || input.kind==ui::EventKind::wheel;
    std::size_t level=menu_path_.size();
    POINT screen{static_cast<LONG>(input.x),static_cast<LONG>(input.y)}; ClientToScreen(window(),&screen);
    if (pointer) for (std::size_t i=menu_path_.size()+1;i>0;--i) {
        const auto index=i-1; auto* host=index?submenus_[index-1].get():popup_.get();
        RECT bounds{}; GetWindowRect(host->window(),&bounds);
        if (PtInRect(&bounds,screen)) { level=index; break; }
    }
    if (input.kind==ui::EventKind::key_down && !menu_path_.empty() &&
        (input.key==ui::Key::left || input.key==ui::Key::escape)) {
        submenus_[menu_path_.size()-1]->hide(); menu_path_.pop_back(); return true;
    }
    auto* host=level?submenus_[level-1].get():popup_.get();
    auto items=std::span<const ui::MenuItem>(workspace_.menu_items);
    for (std::size_t depth=0;depth<level;++depth) {
        const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.key==menu_path_[depth];});
        if (at==items.end()) { close_menu(); return true; }
        items=at->children;
    }
    auto event=input; ScreenToClient(host->window(),&screen);
    event.x=static_cast<float>(screen.x); event.y=static_cast<float>(screen.y);
    const auto reply=host->input(event);
    for (const auto& effect:reply.effects) if (const auto* command=std::get_if<ui::Action>(&effect)) {
        if (command->kind==ui::ActionKind::activate) {
            const auto at=std::find_if(items.begin(),items.end(),[&](const auto& item){return item.key==command->key;});
            if (at==items.end() || !at->enabled) continue;
            if (!at->children.empty()) {
                menu_path_.resize(level); menu_path_.push_back(at->key); update_menu(); return true;
            }
            const auto edit=at->edit_command;
            if (edit) {
                const auto incarnation=menu_incarnation_;
                close_menu(); effects(ui_.execute(*edit,incarnation));
            } else {
                const auto result=action(*command);
                if (!result) status(result,L"");
                close_menu();
            }
            return true;
        } else if (command->kind==ui::ActionKind::dismiss) { close_menu(); return true; }
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
    ui::ControlKey requested_menu{};
    if (event.kind==ui::EventKind::key_down) {
        if (event.key==ui::Key::f10 && !event.shift) requested_menu=editor::control::file_menu;
        else if (event.alt) for (const auto& group:editor::command_groups)
            if (message.wparam==static_cast<WPARAM>(group.access_key)) { requested_menu=group.key; break; }
    }
    if (requested_menu) {
        const auto menu=requested_menu;
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
    if (event.kind==ui::EventKind::key_down && !workspace_.menu &&
        ((event.control && (message.wparam=='S' || message.wparam=='O')) || message.wparam==VK_F5)) {
        const auto command=message.wparam=='O'?editor::Command::open_project:message.wparam==VK_F5?editor::Command::refresh_assets:
            event.shift?editor::Command::save_scene_as:editor::Command::save_scene;
        status(execute(command),L"Project command completed");return;
    }
    if (event.kind==ui::EventKind::key_down && event.control && !workspace_.menu &&
        (message.wparam==VK_TAB || message.wparam=='B' || message.wparam=='W')) {
        effects(ui_.cancel_interaction());
        const auto command=message.wparam==VK_TAB?(event.shift?editor::Command::previous_tab:editor::Command::next_tab):
            message.wparam=='B'?editor::Command::maximize_pane:editor::Command::close_tab;
        if (auto result=execute(command);!result) status(result,L"");
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
        const auto cancelled = document_->cancel_edit();
        if (!cancelled) workspace_.status += L"; rollback failed: " + widen(cancelled.error().message);
        return;
    }
    if (!reply.consumed && event.kind == ui::EventKind::key_down) {
        std::optional<editor::Command> command;
        if (event.control && (event.key==ui::Key::z || event.key==ui::Key::y))
            command=(event.key==ui::Key::y || event.shift)?editor::Command::redo:editor::Command::undo;
        else if (!workspace_.sample_history()) {
            if (event.key==ui::Key::delete_key) command=editor::Command::erase_actor;
            else if (message.wparam=='F' && !ui_.wants_keyboard()) command=editor::Command::focus;
            else if (event.key==ui::Key::escape) status(document_->cancel_edit(),L"Edit cancelled");
        }
        if (command) if (auto result=execute(*command);!result) status(result,L"");
    }
}
bool Application::step(renderer::Capture* capture) {
    frame_timing_ = {};
    frame_statistics_ = {};
    if (closed_) return false;
    window_.poll([&](NativeEvent&& event) { native(std::move(event)); });
    if (window_.closing()) {
        if (confirm_discard()) {close();return false;}
        window_.cancel_close();
    }
    const auto title=(project_?project_->name():L"No project")+L" — "+(scene_path_.empty()?L"Untitled":scene_path_.filename().wstring())+
        (document_->dirty()?L" *":L"")+L" — FrostFox Atelier";
    if (title!=window_title_) { SetWindowTextW(window(),title.c_str());window_title_=title; }
    auto target = graphics(renderer_.target());
    const auto extent = target.drawable ? target.extent : ui::PixelExtent{};
    surface_ = {extent, window_.dpi_scale(), surface_revision_};
    auto updated = ui_.set_surface(surface_);
    if (!updated) throw std::runtime_error("UI rejected the window surface.");
    effects(*updated);
    auto controls = workspace_.describe(*document_,{static_cast<float>(extent.width)/surface_.dpi_scale,static_cast<float>(extent.height)/surface_.dpi_scale},ui_.presented().get(),project_?&*project_:nullptr);
    auto described = ui_.set_controls(controls);
    if (!described && workspace_.reject_ui_controls("UI candidate rejected at control "+std::to_string(described.error().key))) {
        controls=workspace_.describe(*document_,{static_cast<float>(extent.width)/surface_.dpi_scale,static_cast<float>(extent.height)/surface_.dpi_scale},ui_.presented().get(),project_?&*project_:nullptr);
        described=ui_.set_controls(controls);
    }
    if (!described) throw std::runtime_error("UI rejected editor controls (key " + std::to_string(described.error().key) + ").");
    effects(*described);
    workspace_.commit_ui_controls();
    auto gc = document_->collect_step();
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
    meshes_.clear();
    const auto items = viewport?take(document_->world().render_items()):std::vector<world::RenderItem>{};
    meshes_.reserve(items.size());
    for (const auto& item : items)
        meshes_.push_back({item.world.elements, {item.material.base_color.x,item.material.base_color.y,item.material.base_color.z,1}, item.actor == document_->selected(),item.geometry});
    const auto camera = camera_.matrices(std::max(scene_bounds.width,1.f) / std::max(scene_bounds.height,1.f));
    renderer::Frame render_frame{*frame.drawing, {camera.view.elements, camera.projection.elements}, scene_bounds, meshes_, workspace_.view};
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
    for (auto& submenu:submenus_) submenu->close();
    if (popup_) popup_->close();
    effects(ui_.cancel_interaction());
    const auto cancelled = document_->cancel_edit();
    if (!cancelled) throw std::runtime_error(cancelled.error().message);
    window_.capture(false);
    take(completion(document_->close()));
    const auto stopped = renderer_.close();
    if (!stopped) throw std::runtime_error(stopped.error().cause.message);
}
}

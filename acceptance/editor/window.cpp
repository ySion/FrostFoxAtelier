#include "application/application.hpp"
#include "dungeons_roundtrip.hpp"
#include "atelier/build_info.hpp"
#include "control/control.hpp"
#include "renderer/raster_viewport.hpp"
#include "renderer/timing.hpp"
#include "ui/text.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <msctf.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace atelier::application {
struct EditorAcceptanceAccess {
    static void input(Application& app,ui::Event event,WPARAM key=0) {app.input(event,key);}
    static bool menu_open(const Application& app) {return app.workspace_.menu.has_value();}
    static const std::vector<ui::MenuItem>& menu_items(const Application& app) {return app.workspace_.menu_items;}
    static std::string menu_input_diagnostic(const Application& app) {
        const auto pointer=app.pointer_position_;
        return "pointer=("+std::to_string(pointer.x)+","+std::to_string(pointer.y)+"), ready="+std::to_string(app.pointer_ready())+
            ", hit="+std::to_string(app.ui_.pointer_target(pointer.x,pointer.y))+", focus="+std::to_string(app.ui_.focused_control())+
            ", wants_keyboard="+std::to_string(app.ui_.wants_keyboard())+", popup="+(app.popup_?std::string(app.popup_->diagnostic()):"missing");
    }
    static unsigned menu_presents(Application& app) {return app.menu_host()?app.menu_host()->open_present_count():0;}
    static auto focused(const Application& app) {return app.ui_.focused_control();}
    static void finish_menu_hold(Application& app) {app.menu_requested_-=std::chrono::seconds(1);}
    static bool tool_active(const Application& app) {return app.viewport_tool_.active();}
    static bool camera_flying(const Application& app) {return app.camera_fly_.has_value();}
    static editor::Session& physics_document(Application& app) {return *app.document_;}
    static void ui_source(Application& app,const std::filesystem::path& path) {
        auto source=editor::read_file(app.workspace_.ui_source());
        if (!source) throw std::runtime_error(source.error().message);
        auto saved=editor::write_file(path,*source);if (!saved) throw std::runtime_error(saved.error().message);
        app.workspace_.set_ui_source(path);
    }
    static auto ui_revision(const Application& app) {return app.workspace_.ui_revision();}
    static auto ui_error(const Application& app) {return app.workspace_.ui_error();}
    static auto ui_compile_milliseconds(const Application& app) {return app.workspace_.ui_compile_milliseconds();}
    static void mark_saved(Application& app) { app.effects(app.ui_.cancel_interaction()); app.document_->mark_saved(); }
    static Result<void> command(Application& app,editor::Command command,std::string_view argument={}) { return app.execute(command,argument); }
    static auto asset_key(const Application& app,std::string_view resource) {
        return app.workspace_.assets.row_key(resource);
    }
    static bool inspecting_resource(const Application& app,std::string_view resource) {
        const auto& selected=app.workspace_.assets.selected();
        return app.workspace_.inspecting_asset && selected && selected->key==resource;
    }
    static auto directory_key(const Application& app,const std::filesystem::path& path) {
        return app.workspace_.assets.directory_control(path);
    }
    static ui::ControlKey menu_item(const Application& app,std::wstring_view text) {
        for(const auto& item:app.workspace_.menu_items) if(item.text==text) return item.key;
        return 0;
    }
    static const auto& project_build(const Application& app) {return app.workspace_.project_build;}
    static bool project_running(const Application& app) {return app.play_.running();}
    static bool output_contains(const Application& app,std::wstring_view text) {
        for(const auto& row:app.workspace_.output->rows) if(row.cells.front().text.find(text)!=std::wstring::npos) return true;
        return false;
    }
    static const ui::TableData& output(const Application& app) {return *app.workspace_.output;}

    static auto name_control(const Application& app,ObjectId entity) { return app.workspace_.name_control(entity); }
    static ui::ControlKey structure_control(const Application& app,ObjectId object) {
        if(app.presented()) for(const auto& row:app.presented()->tree_rows) {
            const auto* binding=app.workspace_.binding(row.key);
            if(binding && binding->kind==editor::Workspace::Binding::Kind::structure && binding->object==object) return row.key;
        }
        return 0;
    }
    static bool property_group(const Application& app,ObjectId object) {
        if(app.presented()) for(const auto& control:app.presented()->controls) {
            const auto* binding=app.workspace_.binding(control.key);
            if(binding && binding->kind==editor::Workspace::Binding::Kind::property_group && binding->object==object) return true;
        }
        return false;
    }
    static void frame_failure(Application& app) {
        const auto frame=app.ui_.prepare();
        std::fprintf(stderr,"Status: %ls\n",app.status_text().c_str());
        if (frame) if (const auto* view=(*frame)->find(editor::control::viewport))
            std::fprintf(stderr,"Surface %u x %u; viewport %.3f %.3f %.3f %.3f\n",(*frame)->drawing->extent.width,(*frame)->drawing->extent.height,
                view->bounds.x,view->bounds.y,view->bounds.width,view->bounds.height);
    }
    static auto camera_view(const Application& app) { return app.camera_.matrices(1).view; }
    static auto camera_mode(const Application& app) {return app.camera_.view();}
    static auto camera_eye(const Application& app) {return app.camera_.eye();}
    static auto camera_state(const Application& app) {return app.camera_.state();}
    static bool move_camera(Application& app,const editor::CameraState& state) {
        auto camera=app.camera_;return camera.set_state(state) && app.set_camera(camera,false).has_value();
    }
    static const auto& accepted_view(const Application& app) {return app.accepted_view_;}
    static bool gpu_validation(const Application& app) {return app.gpu_validation_;}
    static bool vsync(const Application& app) {return app.vsync_;}
    static bool frame_sampling(const Application& app) {return app.sampling_;}
    static auto render_retry_delay(const Application& app) {return app.render_retry_delay_;}
    static auto render_retry_deadline(const Application& app) {return app.render_retry_at_;}
    static void render_retry_deadline(Application& app,std::chrono::steady_clock::time_point value) {app.render_retry_at_=value;}
    static void unchecked_cavity_radius(Application& app,float value) {app.workspace_.view.cavity.radius=value;}
    static std::filesystem::path project_root(const Application& app) {return app.project_?app.project_->root():std::filesystem::path{};}
    static std::filesystem::path scene_path(const Application& app) {return app.scene_path_;}
    static auto dock_presentation(const Application& app) {
        const auto size=app.surface_.extent;
        return app.workspace_.dock.presentation(0,{float(size.width)/app.surface_.dpi_scale,float(size.height)/app.surface_.dpi_scale});
    }
    static void set_scene_maximized(Application& app,bool maximized) {
        auto& dock=app.workspace_.dock;
        if(!dock.select(editor::control::scene_tab)) throw std::runtime_error("Scene View cannot be selected.");
        for(const auto& pane:dock_presentation(app).panes) if(pane.active && pane.active->key==editor::control::scene_tab) {
            if(pane.maximized!=maximized) dock.maximize(editor::control::scene_tab);
            return;
        }
        throw std::runtime_error("Scene View has no presented Dock pane.");
    }
    static Result<void> select_entity(Application& app,ObjectId entity) {return app.document_->select(entity);}
    static auto cursor_position(const Application& app) {return app.viewport_tool_.cursor;}
    static auto viewport_settings(const Application& app) {return app.workspace_.viewport_settings;}
    static auto scene_lit_view(const Application& app) {return app.workspace_.view;}
    static const auto& performance(const Application& app) {return app.workspace_.lighting.performance();}
    static Result<void> performance_view(Application& app,const renderer::ViewSettings& view) {
        return app.workspace_.set_view_settings(*app.document_,view,app.lighting_focus());
    }
    static void performance_panel(Application& app) {
        app.workspace_.dock.move(editor::world_control::tab,editor::control::parameters_tab,ui::DockSide::tabs);
        app.workspace_.dock.select(editor::world_control::tab);
    }
    static const auto& fps_text(const Application& app) {return app.workspace_.viewport_fps;}
    static Result<void> fps_setting(Application& app,bool enabled) {
        return app.action({ui::ActionKind::activate,editor::control::command_key(editor::Command::show_fps),enabled});
    }
    static Result<void> scene_lit_settings(Application& app,const renderer::SceneLitSettings& settings) {
        auto view=app.workspace_.view;view.lit=settings;
        return app.workspace_.set_view_settings(*app.document_,view,app.lighting_focus());
    }
    static const auto& scene_snapshot(const Application& app) {return app.scene_;}
    static const auto& render_notice(const Application& app) {return app.workspace_.render_notice;}
    static void set_tool(Application& app,editor::TransformTool tool) {app.workspace_.viewport_settings.tool=tool;}
    static void set_cursor(Application& app,math::Vector3 at) {app.viewport_tool_.cursor=at;}
    static Result<void> ui_action(Application& app,ui::ControlKey key,ui::ActionValue value={}) {return app.action({ui::ActionKind::activate,key,std::move(value)});}
    static void select_tab(Application& app,ui::ControlKey tab) {app.workspace_.dock.select(tab);}
    static Result<void> open_menu(Application& app,ui::ControlKey source) {app.request_menu({source,0,0,0});return {};}
    static void close_menu(Application& app) {app.close_menu(false);}
    static void cancel_rename(Application& app) {app.workspace_.row_rename.reset();}
    static ui::ControlKey color_pick(const Application& app) {
        if(!app.presented()) return 0;
        for(const auto& box:app.presented()->controls) if(box.kind==ui::ControlKind::button && box.action_target) {
            const auto* binding=app.workspace_.binding(box.action_target);
            if(binding && binding->kind==editor::Workspace::Binding::Kind::color_pick) return box.action_target;
        }
        return 0;
    }
    static ui::ControlKey viewport_readout_key(const Application&) {return editor::control::viewport_readout;}
    // Drive the real tool entry points without OS input (snapshot only).
    static void tool_key(Application& app,std::uint32_t vk,math::Vector2 pointer) {
        ui::Event event;event.kind=ui::EventKind::key_down;
        if(!app.viewport_tool_.key(*app.document_,*app.accepted_view_,event,vk,app.workspace_.viewport_settings,pointer))
            throw std::runtime_error("Snapshot tool key failed.");
    }
    static void tool_move(Application& app,math::Vector2 pointer) {
        ui::Event event;event.kind=ui::EventKind::pointer_move;event.x=pointer.x;event.y=pointer.y;
        if(!app.viewport_tool_.pointer(*app.document_,*app.accepted_view_,event,app.workspace_.viewport_settings))
            throw std::runtime_error("Snapshot tool move failed.");
    }
    static void tool_cancel(Application& app) {if(!app.viewport_tool_.cancel(*app.document_)) throw std::runtime_error("Snapshot cancel failed.");}
    static void place_cursor(Application& app,math::Vector2 pointer) {
        ui::Event event;event.kind=ui::EventKind::pointer_down;event.button=ui::PointerButton::primary;event.x=pointer.x;event.y=pointer.y;
        auto settings=app.workspace_.viewport_settings;settings.tool=editor::TransformTool::select;
        if(!app.viewport_tool_.pointer(*app.document_,*app.accepted_view_,event,settings)) throw std::runtime_error("Snapshot cursor placement failed.");
        event.kind=ui::EventKind::pointer_up;static_cast<void>(app.viewport_tool_.pointer(*app.document_,*app.accepted_view_,event,settings));
    }
    static POINT viewport_point(const Application& app,math::Vector3 point) {
        if(!app.accepted_view_) throw std::runtime_error("Viewport has no accepted camera.");
        const auto projected=app.accepted_view_->project(point);if(!projected) throw std::runtime_error("Viewport point is outside the camera.");
        return {static_cast<LONG>(std::round(projected->x)),static_cast<LONG>(std::round(projected->y))};
    }
    static void require_camera(const Application& app,bool passed,UINT reason,const char* message) {
        if (!passed) throw std::runtime_error(std::string(message)+"; reason="+std::to_string(reason)+
            "; camera drag="+std::to_string(bool(app.camera_drag_))+"; pointer=("+std::to_string(app.pointer_position_.x)+","+std::to_string(app.pointer_position_.y)+")");
    }
    static void move_pointer(Application& app,POINT target,UINT reason=0) {
        const auto verify=[&](bool passed,const char* message) {
            if (!passed) throw std::runtime_error(std::string(message)+"; reason="+std::to_string(reason)+
                "; target=("+std::to_string(target.x)+","+std::to_string(target.y)+")");
        };
        POINT before{}; verify(GetCursorPos(&before)!=FALSE,"Cannot read the acceptance pointer");
        ScreenToClient(app.window(),&before);
        const bool changed=before.x!=target.x || before.y!=target.y;
        auto screen=target; ClientToScreen(app.window(),&screen);
        verify(SetCursorPos(screen.x,screen.y)!=FALSE,"Cannot move the acceptance pointer");
        // SetCursorPos updates the physical position before its queued movement is
        // delivered. Use that single move source and wait for the actual target event.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        bool delivered=false;
        do {
            app.window_.poll([&](NativeEvent&& event) {
                if (event.message==WM_MOUSEMOVE) delivered=
                    static_cast<short>(LOWORD(event.lparam))==target.x && static_cast<short>(HIWORD(event.lparam))==target.y;
                app.native(std::move(event));
            });
            if (delivered || !changed) break;
            MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        } while (std::chrono::steady_clock::now()<deadline);
        verify(delivered || !changed,"Pointer move was not delivered before the deadline");
        POINT actual{}; verify(GetCursorPos(&actual)!=FALSE,"Cannot verify the acceptance pointer");
        ScreenToClient(app.window(),&actual);
        verify(actual.x==target.x && actual.y==target.y,"Pointer moved outside the requested target");
    }
    static void menu_items(Application& app,std::span<const ui::MenuItem> items) {
        if (!app.popup_ || !app.workspace_.menu) throw std::runtime_error("Menu fixture needs an open native host.");
        RECT anchor{};
        if (!GetWindowRect(app.menu_window(),&anchor)) throw std::runtime_error("Cannot locate native menu fixture.");
        anchor.bottom=anchor.top;
        app.popup_->update(app.workspace_.menu->source,anchor,false,items,app.ui_.style(),app.style_sheet_,app.workspace_.ui_component("PopupMenu"));
    }
    static void invalidate_menu_measurement(Application& app) {
        if (!app.popup_) throw std::runtime_error("Menu fixture needs a retained native host.");
        // Model a changed monitor/DPI measurement domain without changing the desktop.
        app.popup_->measured_dpi_=0;
    }
    static void menu_end(Application& app) {
        static_cast<void>(app.popup_->input({.kind=ui::EventKind::key_down,.key=ui::Key::end}));
        static_cast<void>(app.popup_->input({.kind=ui::EventKind::key_up,.key=ui::Key::end}));
    }
    static Result<void> begin_edit(Application& app, ui::ControlKey control) {
        const auto* binding = app.workspace_.binding(control);
        if (!binding || binding->path.empty()) return std::unexpected(Error{ErrorCode::invalid_definition, "Missing field binding."});
        return app.document_->begin_edit(binding->object, binding->path, control);
    }
    static Result<void> edit_field(Application& app, ObjectId component, std::span<const FieldId> path, float value) {
        constexpr auto session = std::numeric_limits<ui::ControlKey>::max();
        auto began = app.document_->begin_edit(component, path, session);
        if (!began) return began;
        auto preview = app.document_->preview_edit(session, value);
        if (!preview) return preview;
        return app.document_->commit_edit(session);
    }
};
}


namespace {
using atelier::application::Application;
using atelier::ui::ControlKey;
namespace control = atelier::editor::control;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("PNG encoding failed."); }
struct KeyStateScope {
    BYTE original[256]{};
    explicit KeyStateScope(bool control = false, bool shift = false, bool alt = false) {
        require(GetKeyboardState(original) != FALSE, "Cannot preserve acceptance thread key state.");
        BYTE state[256]{}; std::copy(std::begin(original), std::end(original), state);
        for (const auto code : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU})
            state[code] = 0;
        state[VK_CONTROL] = state[VK_LCONTROL] = control ? 0x80 : 0;
        state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
        state[VK_MENU] = state[VK_LMENU] = alt ? 0x80 : 0;
        require(SetKeyboardState(state) != FALSE, "Cannot set acceptance thread modifiers.");
    }
    ~KeyStateScope() { SetKeyboardState(original); }
};
// Unattended runs cannot wait for a person: any top-level dialog outside an expected scope ends the run.
int expected_native_dialogs{};
struct ExpectedNativeDialogs {
    ExpectedNativeDialogs() { ++expected_native_dialogs; }
    ~ExpectedNativeDialogs() { --expected_native_dialogs; }
};
LRESULT CALLBACK native_dialog_guard(int code, WPARAM window, LPARAM parameter) {
    if (code == HCBT_CREATEWND && !expected_native_dialogs) {
        const auto& created = *reinterpret_cast<const CBT_CREATEWNDW*>(parameter)->lpcs;
        const bool dialog = IS_INTRESOURCE(created.lpszClass) ? reinterpret_cast<ULONG_PTR>(created.lpszClass) == 0x8002
                                                              : std::wstring_view(created.lpszClass) == L"#32770";
        if (dialog && !(created.style & WS_CHILD)) {
            const std::wstring_view title = created.lpszName && !IS_INTRESOURCE(created.lpszName) ? created.lpszName : L"";
            std::fprintf(stderr, "Editor acceptance: FAIL: unexpected native dialog \"%s\"\n", atelier::editor::utf8(title).c_str());
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 1);
        }
    }
    return CallNextHookEx(nullptr, code, window, parameter);
}
void require_input_focus(Application& app) {
    require(GetFocus() == app.window() && GetForegroundWindow() == app.window(),
        "Acceptance interrupted by external focus loss; no further synthetic input was sent.");
}
void tick(Application& app, unsigned count = 2) {
    const auto before = app.accepted_frames();
    for (unsigned attempt = 0; app.accepted_frames() < before + count && attempt < 180; ++attempt)
        require(app.step(), "Editor closed during input acceptance.");
    if (app.accepted_frames()<before+count) atelier::application::EditorAcceptanceAccess::frame_failure(app);
    require(app.accepted_frames() >= before + count, "Editor did not accept a frame within 180 iterations.");
}
const atelier::ui::ControlBox* command_tool(Application& app,ControlKey target) {
    if (!app.presented()) return nullptr;
    for (const auto& control:app.presented()->controls)
        if (control.kind==atelier::ui::ControlKind::button && control.action_target==target) return &control;
    return nullptr;
}
const atelier::ui::TableGeometry* row_table(Application& app,ControlKey row) {
    if (!app.presented()) return nullptr;
    for (const auto& table:app.presented()->tables)
        if (table.data && std::ranges::find(table.data->rows,row,&atelier::ui::TableRow::key)!=table.data->rows.end()) return &table;
    return nullptr;
}
POINT centre(Application& app, ControlKey key) {
    require(app.presented() != nullptr, "Missing presented UI frame.");
    // An accepted active popup owns input before host controls. The ToolShelf
    // may expose the same command target; its host bounds are not menu bounds.
    const auto* popup_box=app.menu_frame()?app.menu_frame()->find(key):nullptr;
    const bool popup=popup_box!=nullptr;
    auto* box = popup_box?popup_box:app.presented()->find(key);
    if (!box) box=command_tool(app,key);
    const atelier::ui::TableCellGeometry* cell{};
    if (!box) if (const auto* table=row_table(app,key)) {
        const auto found=std::ranges::find(table->cells,key,&atelier::ui::TableCellGeometry::row);
        if (found!=table->cells.end()) cell=&*found;
    }
    if (!box && !cell) {
        std::string menu;
        if (app.menu_frame()) for (const auto& item:app.menu_frame()->controls) menu+=" "+std::to_string(item.key);
        POINT pointer{}; GetCursorPos(&pointer); ScreenToClient(app.window(),&pointer);
        throw std::runtime_error("Missing control "+std::to_string(key)+" in presented main/menu frames; popup keys:"+menu+
            "; OS pointer="+std::to_string(pointer.x)+","+std::to_string(pointer.y));
    }
    const auto bounds=box?box->bounds:cell->bounds;
    const auto clip=box?box->clip:cell->clip;
    const auto left = std::max(bounds.x, static_cast<float>(clip.left));
    const auto right = std::min(bounds.x + bounds.width, static_cast<float>(clip.right));
    const auto top = std::max(bounds.y, static_cast<float>(clip.top));
    const auto bottom = std::min(bounds.y + bounds.height, static_cast<float>(clip.bottom));
    if (!(right>left && bottom>top)) throw std::runtime_error("Control "+std::to_string(key)+
        " has no visible hit area (y="+std::to_string(bounds.y)+", height="+std::to_string(bounds.height)+
        ", clip="+std::to_string(clip.top)+".."+std::to_string(clip.bottom)+").");
    POINT result{static_cast<LONG>((left + right) * .5f), static_cast<LONG>((top + bottom) * .5f)};
    if (popup) MapWindowPoints(app.menu_window(),app.window(),&result,1);
    return result;
}
POINT table_header_centre(Application& app,ControlKey owner,atelier::ui::TableKey column) {
    const auto* table=app.presented()?app.presented()->find_table(owner):nullptr;
    require(table,"Missing accepted Table for header input.");
    const auto header=std::ranges::find(table->columns,column,&atelier::ui::TableColumnGeometry::column);
    require(header!=table->columns.end(),"Table header has no accepted visible geometry.");
    const auto left=std::max(header->bounds.x,static_cast<float>(header->clip.left));
    const auto right=std::min(header->bounds.x+header->bounds.width,static_cast<float>(header->clip.right));
    const auto top=std::max(header->bounds.y,static_cast<float>(header->clip.top));
    const auto bottom=std::min(header->bounds.y+header->bounds.height,static_cast<float>(header->clip.bottom));
    require(right>left && bottom>top,"Table header has no visible hit area.");
    return {static_cast<LONG>((left+right)*.5f),static_cast<LONG>((top+bottom)*.5f)};
}
void mouse(Application& app, UINT message, POINT point) {
    require_input_focus(app);
    KeyStateScope modifiers;
    // Held gestures use one native movement source, including the initial point.
    // Mixing SendMessage movement with capture-generated movement can reverse a drag.
    if (message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN) {
        atelier::application::EditorAcceptanceAccess::move_pointer(app,point);
    } else if (message==WM_MOUSEMOVE && GetCapture()==app.window() && !app.menu_window()) {
        atelier::application::EditorAcceptanceAccess::move_pointer(app,point);
        tick(app,1); return;
    }
    const bool released=message==WM_LBUTTONUP || message==WM_RBUTTONUP || message==WM_MBUTTONUP;
    SendMessageW(app.window(),message,released?0:message==WM_RBUTTONDOWN?MK_RBUTTON:message==WM_MBUTTONDOWN?MK_MBUTTON:MK_LBUTTON,MAKELPARAM(point.x,point.y));
    tick(app, 1);
    if (released) {
        RECT host{}; GetWindowRect(app.window(),&host);
        require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park the pointer after the held gesture.");
        tick(app,1);
    }
}
void click_at(Application& app, POINT point) {
    require_input_focus(app);
    KeyStateScope modifiers;
    // Deliver a complete synthetic click before polling queued native movement.
    // Deliberate drags and pressed-state captures still use mouse() with ticks.
    SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
    tick(app,2);
}
void click(Application& app, ControlKey key) {
    if (!app.presented()->find(key) && !command_tool(app,key) && !row_table(app,key) && (!app.menu_frame() || !app.menu_frame()->find(key))) {
        namespace browser=atelier::editor::browser_control;
        if (key==browser::open || key==browser::add) click(app,browser::actions);
        else if (key==browser::list || key==browser::details || key==browser::tiles) click(app,browser::view_menu);
        else if (const auto menu=control::command_menu(key)) {
            click(app,menu);
            // Some commands sit one submenu below their menubar group, e.g. View > Camera View.
            if (app.menu_frame() && !app.menu_frame()->find(key))
                for (const auto& item:atelier::application::EditorAcceptanceAccess::menu_items(app))
                    if (std::ranges::find(item.children,key,&atelier::ui::MenuItem::key)!=item.children.end()) {click(app,item.key);break;}
        }
    }
    // Docked views can be shorter than their content. Reach the target through
    // actual wheel messages, using the same accepted clip as a user would see.
    for (unsigned attempt=0;attempt<40;++attempt) {
        const auto* box=app.presented()->find(key);
        if (!box) {
            const auto* table=row_table(app,key); if (!table) break;
            const auto cell=std::ranges::find(table->cells,key,&atelier::ui::TableCellGeometry::row);
            if (cell!=table->cells.end() && cell->bounds.y>=cell->clip.top &&
                cell->bounds.y+cell->bounds.height<=cell->clip.bottom) break;
            require(!table->cells.empty(),"Table row has no visible viewport to scroll.");
            const auto row=std::ranges::find(table->data->rows,key,&atelier::ui::TableRow::key);
            const auto first=std::ranges::find(table->data->rows,table->cells.front().row,&atelier::ui::TableRow::key);
            const bool above=cell!=table->cells.end()?cell->bounds.y<cell->clip.top:row<first;
            const auto left=std::max(table->body.x,static_cast<float>(table->clip.left));
            const auto right=std::min(table->body.x+table->body.width,static_cast<float>(table->clip.right));
            const auto top=std::max(table->body.y,static_cast<float>(table->clip.top));
            const auto bottom=std::min(table->body.y+table->body.height,static_cast<float>(table->clip.bottom));
            require(right>left && bottom>top,"Table row has no visible body to scroll.");
            POINT point{static_cast<LONG>((left+right)*.5f),static_cast<LONG>((top+bottom)*.5f)};
            ClientToScreen(app.window(),&point);
            SendMessageW(app.window(),WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(above?WHEEL_DELTA:-WHEEL_DELTA)),MAKELPARAM(point.x,point.y));
            tick(app,1);continue;
        }
        if (box->bounds.y>=box->clip.top && box->bounds.y+box->bounds.height<=box->clip.bottom) break;
        auto parent=box->parent;
        while (parent) {
            const auto* ancestor=app.presented()->find(parent); if (!ancestor) { parent=0; break; }
            if (ancestor->kind==atelier::ui::ControlKind::list || ancestor->kind==atelier::ui::ControlKind::tree) break;
            parent=ancestor->parent;
        }
        if (!parent) break;
        POINT point=centre(app,parent); ClientToScreen(app.window(),&point);
        const auto delta=box->bounds.y<box->clip.top?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(app.window(),WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),MAKELPARAM(point.x,point.y));
        tick(app,1);
    }
    const auto point = centre(app, key);
    mouse(app, WM_MOUSEMOVE, point); click_at(app, point);
}
void double_click(Application& app,ControlKey key) {
    require((GetClassLongPtrW(app.window(),GCL_STYLE)&CS_DBLCLKS)!=0,"Editor window does not request native double-click messages.");
    click(app,key);
    require_input_focus(app);
    KeyStateScope modifiers;
    const auto point=centre(app,key);
    SendMessageW(app.window(),WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
    tick(app,2);
}
void key(Application& app, WPARAM code, bool ctrl = false, bool shift = false) {
    require_input_focus(app);
    KeyStateScope modifiers(ctrl, shift);
    SendMessageW(app.window(), WM_KEYDOWN, code, 0);
    tick(app, 1);
    SendMessageW(app.window(), WM_KEYUP, code, 0);
    tick(app, 1);
}
void type(Application& app, const wchar_t* value) {
    KeyStateScope modifiers;
    key(app, 'A', true);
    if (!*value) { key(app, VK_DELETE); return; }
    require_input_focus(app);
    while (*value) SendMessageW(app.window(), WM_CHAR, static_cast<WPARAM>(*value++), 0);
    tick(app, 1);
}
template<class T> T model(atelier::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}
std::span<const atelier::world::Member> entities(const Application& app) { return model(app.document().world().members()); }
bool contains(const Application& app, atelier::ObjectId entity) {
    const auto members = entities(app);
    return std::any_of(members.begin(), members.end(), [entity](const auto& member) { return member.id() == entity; });
}
atelier::ObjectId component(const Application& app, atelier::ObjectId entity, atelier::TypeId type) {
    auto& runtime = app.document().world().runtime();
    for (const auto& root : model(atelier::world::components(runtime, entity)))
        if (model(runtime.is_a(model(runtime.type_of(root.id())), type))) return root.id();
    throw std::runtime_error("Required component is absent.");
}
ControlKey property_control(const Application& app, atelier::ObjectId entity, std::size_t axis) {
    if (entity == atelier::ObjectId{}) return 0;
    if (!model(atelier::world::has_transform(app.document().world().runtime(),entity))) return 0;
    const auto key = app.field_control(entity, app.document().parameters()[axis].path);
    require(key != 0, "Missing live component-field control binding.");
    return key;
}
float x(const Application& app, atelier::ObjectId entity) {
    require(model(atelier::world::has_transform(app.document().world().runtime(),entity)), "Missing Entity Transform in current World.");
    return std::get<float>(model(app.document().world().read(entity, app.document().parameters()[0].path)));
}
atelier::ObjectId component_object(const atelier::editor::Session& document,atelier::ObjectId entity,atelier::TypeId type) {
    for(const auto& component:model(atelier::world::components(document.world().runtime(),entity)))
        if(model(document.world().runtime().type_of(component.id()))==type) return component.id();
    throw std::runtime_error("Expected capability component is absent.");
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
        if (!draw.coverage && intersects(draw.bounds, box->bounds)) {
            colors.push_back(draw.color);
            if (draw.border_width>0) colors.push_back(draw.border_color);
        }
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
    const auto scene_pixels = std::count_if(image.depth.begin(), image.depth.end(), [](float value) { return value > 0 && value <= 1; });
    if (app.presented()->find(control::viewport)) require(scene_pixels>32,"Real scene geometry is absent from the depth capture.");
    else require(scene_pixels==0,"Hidden Scene tab continued rendering geometry.");
    png(image, path);
    if (app.menu_window()) {
        atelier::renderer::Capture popup; app.capture_menu(popup);
        png(popup,path.parent_path()/(path.stem().wstring()+L"-popup.png"));
    }
}
void require_gray_pixel(const atelier::renderer::Capture& pixels,int x,int y,int gray) {
    require(x>=0 && y>=0 && x<static_cast<int>(pixels.extent.width) && y<static_cast<int>(pixels.extent.height),"Chrome probe left the surface.");
    const auto offset=(static_cast<std::size_t>(y)*pixels.extent.width+static_cast<std::size_t>(x))*4;
    for (std::size_t channel=0;channel<3;++channel)
        if (std::abs(static_cast<int>(pixels.rgba[offset+channel])-gray)>1)
            throw std::runtime_error("Chrome pixel ("+std::to_string(x)+", "+std::to_string(y)+") is "+
                std::to_string(pixels.rgba[offset])+"/"+std::to_string(pixels.rgba[offset+1])+"/"+std::to_string(pixels.rgba[offset+2])+
                ", expected sRGB gray "+std::to_string(gray)+".");
}
void verify_chrome_pixels(Application& app,const std::filesystem::path& output) {
    atelier::renderer::Capture pixels;
    require(app.step(&pixels) && pixels.frame_id,"Chrome pixel capture failed.");
    const auto pixel=[&](int x,int y,int gray) {
        require_gray_pixel(pixels,x,y,gray);
    };
    const auto field=app.presented()->find(control::parameter_search)->bounds;
    for (const auto& control:app.presented()->controls)
        if (control.kind==atelier::ui::ControlKind::number || control.kind==atelier::ui::ControlKind::text_edit)
            require(std::abs(control.bounds.height-field.height)<.1f,"Single-line editors have inconsistent heights.");
    const auto x=static_cast<int>(std::round(field.x)),y=static_cast<int>(std::round(field.y));
    // Current display: a sub-DIP authored edge rounds to one physical pixel.
    const auto stroke=std::max(1,static_cast<int>(std::round(.667f*app.presented()->drawing->dpi_scale)));
    pixel(x,y+4*stroke,36); pixel(x+stroke,y+4*stroke,46); pixel(x+3*stroke,y+4*stroke,19);
    pixel(x+8*stroke,y,36); pixel(x+8*stroke,y+stroke,46);
    const auto selected=app.presented()->find(control::shelf_create)->bounds;
    const auto tx=static_cast<int>(std::round(selected.x)),ty=static_cast<int>(std::round(selected.y));
    pixel(tx+5*stroke,ty,38); pixel(tx+5*stroke,ty+2*stroke,79);
    pixel(tx+5*stroke,static_cast<int>(std::round(selected.y+selected.height))-1,79);
    const auto tab_bar=app.presented()->find(control::shelf_tabs)->bounds;
    const auto unselected=app.presented()->find(control::shelf_view)->bounds;
    const auto list=app.presented()->find(control::parameter_list)->bounds;
    const auto fold=app.presented()->find(control::position_fold)->bounds;
    const auto header=app.presented()->find(51)->bounds;
    require(field.y>=header.y+header.height-.1f && field.y+field.height<=list.y+.1f,
        "Parameter Filter must remain between the object header and flat properties, outside scrolling content.");
    require(std::abs(selected.x-tab_bar.x)<.1f && std::abs(fold.x-list.x)<.1f,
        "Shelf tabs or parameter fold header added an independent left inset.");
    // Unselected section tabs draw a vertical #555555 to #303030 gradient across their bounds.
    const auto face_y=static_cast<int>(std::round(unselected.y))+2*stroke;
    const auto face_t=(static_cast<float>(face_y)+.5f-unselected.y)/unselected.height;
    pixel(static_cast<int>(std::round(unselected.x))+5*stroke,face_y,static_cast<int>(std::round(0x55+(0x30-0x55)*face_t)));
    require(tab_bar.x+tab_bar.width>unselected.x+unselected.width+6*stroke,"Tab-bar color probe has no uncovered strip.");
    pixel(static_cast<int>(std::round(tab_bar.x+tab_bar.width))-3*stroke,static_cast<int>(std::round(tab_bar.y))+2*stroke,0x3a);
    const auto footer=app.presented()->find(100)->bounds;
    pixel(static_cast<int>(std::round(footer.x+footer.width))-4*stroke,
        static_cast<int>(std::round(footer.y+footer.height))-3*stroke,25);
    pixel(static_cast<int>(std::round(fold.x))+stroke,static_cast<int>(std::round(fold.y))+stroke,51);
    for (const auto key:{control::scene_tab,control::parameters_tab}) {
        const auto* tab=app.presented()->find(key);
        const auto group=app.presented()->find(tab->parent)->bounds;
        // The first tab continues the selected face after the rail's trailing edge.
        pixel(static_cast<int>(std::round(group.x)),static_cast<int>(std::round(group.y))+3*stroke,73);
    }
    for (const auto axis:{atelier::ui::DragAxis::horizontal,atelier::ui::DragAxis::vertical}) {
        const bool vertical=axis==atelier::ui::DragAxis::horizontal;
        const auto divider=app.docking().divider(vertical?control::scene_tab:control::parameters_tab,axis);
        const auto rail=app.presented()->find(divider)->bounds;
        const auto rx=static_cast<int>(std::round(rail.x)),ry=static_cast<int>(std::round(rail.y));
        if (vertical) {
            pixel(rx,ry+8*stroke,0); pixel(rx+2*stroke,ry+8*stroke,73);
            pixel(static_cast<int>(std::round(rail.x+rail.width))-1,ry+8*stroke,51);
        } else {
            pixel(rx+8*stroke,ry,0); pixel(rx+8*stroke,ry+2*stroke,73);
            pixel(rx+8*stroke,static_cast<int>(std::round(rail.y+rail.height))-1,51);
        }
    }
    png(pixels,output/"chrome-pixels.png");
}
void verify_control_states(Application& app, const std::filesystem::path& output, atelier::ObjectId object) {
    const auto paint=[&] {
        const auto bounds=app.presented()->find(control::file_menu)->bounds;
        std::shared_ptr<const atelier::ui::ColorImage> image;
        for (const auto& draw:app.presented()->drawing->draws) if (draw.bounds==bounds && draw.image) image=draw.image;
        return std::pair{solid_colors(*app.presented(),control::file_menu),image};
    };
    mouse(app, WM_MOUSEMOVE, centre(app, control::viewport));
    const auto normal = paint();
    const auto button = centre(app, control::file_menu);
    mouse(app, WM_MOUSEMOVE, button);
    const auto hover = paint();
    require(hover==normal && !app.menu_window(),"Closed menubar reacted to pointer hover.");
    mouse(app, WM_LBUTTONDOWN, button);
    require(paint() != hover, "Pressed state did not change the shared paint.");
    capture(app, output / "s1-pressed.png");
    mouse(app, WM_LBUTTONUP, button);
    key(app,VK_ESCAPE);
    require(app.presented()->find(control::file_menu)->style->border_width==0,
        "Menu focus added an outer frame after pointer activation.");

    auto field = property_control(app, object, 0);
    click(app, field);
    const auto focused = solid_colors(*app.presented(), field);
    const auto edge=app.presented()->find(field)->style->border;
    require(edge.r==edge.g && edge.g==edge.b,"Editing focus did not use a neutral field edge.");
    capture(app, output / "s1-focused.png");
    type(app, L"1000e");
    require(solid_colors(*app.presented(), field) != focused, "Invalid draft did not change the shared field state.");
    capture(app, output / "s1-invalid.png");
    key(app, VK_ESCAPE);

    const auto before = x(app, object);
    click(app,app.object_control(object));
    click(app, control::remove_selected);
    const auto disabled=std::ranges::find_if(app.presented()->controls,[](const auto& control) {
        return control.parent==51 && control.kind==atelier::ui::ControlKind::text_edit;
    });
    require(disabled!=app.presented()->controls.end() && !disabled->enabled && text_coverage(*app.presented(),disabled->key) &&
        !app.presented()->find(field) && !app.presented()->find(control::parameter_search) &&
        !app.presented()->find(control::parameter_list) && !app.presented()->find(control::position_fold),
        "No selection lost its disabled name field or retained Entity-specific property controls.");
    capture(app, output / "s1-disabled.png");
    click(app, control::undo);
    require(app.document().selection().entity == object && x(app, object) == before, "State verification did not restore the edited object.");
    const auto* selected = app.presented()->find(app.object_control(object));
    require(selected && selected->enabled, "Selected object row lost its enabled control geometry.");
}
void verify_style_changes(Application& app, const std::filesystem::path& output, atelier::ObjectId object) {
    const auto initial_style = app.style();
    auto field = property_control(app, object, 0);
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
    for (const auto& group : atelier::editor::command_groups) {
        const auto command=group.key;
        const auto* box = app.presented()->find(command);
        const auto coverage = text_coverage(*app.presented(), command);
        require(box && coverage && box->bounds.x + box->bounds.width <= app.presented()->drawing->extent.width,
            "Unified font change pushed a menubar title outside the window.");
        for (const auto& draw : app.presented()->drawing->draws)
            if (draw.coverage == coverage) require(draw.bounds.x >= box->bounds.x &&
                draw.bounds.x + draw.bounds.width <= box->bounds.x + box->bounds.width,
                "Measured menubar width clipped the changed font's glyphs.");
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
        total.viewport_draw_calls += stats.viewport_draw_calls;
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
               << total.ui_draw_calls << ',' << total.scene_draw_calls << ',' << total.viewport_draw_calls << ',' << total.coverage_upload_count << ','
               << total.coverage_upload_bytes << ',' << peak_live_coverages << ',' << peak_live_bytes << ','
               << total.cached_coverage_count << ',' << total.cached_coverage_texel_bytes << ',' << total.cached_coverage_cpu_bytes << '\n';
    }
};
void verify_shared_text(Application& app, const std::filesystem::path& output, atelier::ObjectId object) {
    auto field = property_control(app, object, 0);
    const auto original_position = x(app, object);
    const auto original_history = app.document().undo_count();
    click(app, control::parameter_search);
    type(app, L"位置");
    require(app.query() == L"位置" && app.presented()->find(field), "Owned Unicode search did not match real parameter aliases.");
    capture(app, output / "s2-search-unicode.png"); // WM_CHAR coverage, not a claim of actual IME input.
    key(app, VK_RETURN);
    require(app.query() == L"位置" && app.wants_keyboard(), "Search Enter did not commit while retaining focus.");
    type(app, L"x");
    require(app.presented()->find(field) && !app.presented()->find(property_control(app, object, 1)), "Search did not filter actual parameter rows.");
    key(app, 'Z', true);
    require(app.query() == L"位置", "Local undo did not restore the committed search.");
    key(app, 'Y', true);
    require(app.query() == L"x", "Local redo did not restore the search draft.");
    key(app, VK_ESCAPE);
    require(app.query() == L"位置", "Search cancellation did not restore the transaction origin.");
    type(app, L"z"); click(app, control::reset);
    require(app.query() == L"z", "Ordinary blur did not commit search preview.");
    click(app, control::parameter_search); type(app, L"x");
    SetFocus(nullptr); tick(app, 1); SetFocus(app.window()); tick(app, 1);
    require(app.query() == L"z", "OS blur did not roll search preview back.");
    click(app, control::parameter_search); type(app, L""); key(app, VK_RETURN);
    require(app.query().empty() && app.presented()->find(property_control(app, object, 2)), "Empty search did not restore all parameter rows.");

    constexpr auto graphemes = L"a\U0001F600e\u0301\U0001F469\u200d\U0001F4BB";
    type(app, graphemes); key(app, VK_END);
    key(app, VK_BACK);
    require(app.query() == L"a\U0001F600e\u0301", "Backspace split a ZWJ grapheme.");
    key(app, VK_BACK);
    require(app.query() == L"a\U0001F600", "Backspace split a combining sequence.");
    key(app, VK_BACK);
    require(app.query() == L"a", "Backspace split a surrogate pair.");
    key(app, 'Z', true);
    require(app.query() == graphemes, "Contiguous backspace was not one local history group.");
    key(app, 'A', true); key(app, 'C', true);
    {
        const auto holder = CreateWindowExW(0, L"STATIC", L"Atelier clipboard acceptance", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(holder != nullptr, "Cannot create a clipboard contention holder.");
        struct Holder { HWND window; ~Holder() { CloseClipboard(); DestroyWindow(window); } } held{holder};
        require(OpenClipboard(holder) != FALSE, "Cannot establish clipboard contention.");
        key(app, 'V', true);
        require(app.query() == graphemes && app.status_text().find(L"Input operation failed") != std::wstring::npos,
            "Failed native clipboard read changed the draft or lost its error.");
    }
    key(app, 'X', true);
    require(app.query().empty(), "Successful native clipboard cut did not remove its selection.");
    key(app, 'V', true);
    require(app.query() == graphemes, "Native Unicode clipboard paste did not restore the exact text.");
    { KeyStateScope unrelated_shift(false, true); key(app, 'Z', true); }
    require(app.query().empty(), "Paste undo or explicit modifier normalization failed.");
    key(app, 'Z', true); require(app.query() == graphemes, "Cut was not one local undo unit.");
    key(app, VK_ESCAPE);
    require(app.query().empty() && app.document().undo_count() == original_history, "Search history escaped into scene history.");
    type(app, L"ab אבג xy"); key(app, 'A', true);
    const auto colors = solid_colors(*app.presented(), control::parameter_search);
    require(std::count(colors.begin(), colors.end(), app.presented()->find(control::parameter_search)->style->selection) > 1,
        "Bidirectional selection did not produce multiple visual ranges.");
    capture(app, output / "s2-bidi-selection.png");
    key(app, VK_ESCAPE);

    const auto field_point=centre(app,field);
    mouse(app,WM_MOUSEMOVE,field_point);
    require(PostMessageW(app.window(),WM_MOUSEMOVE,0,MAKELPARAM(field_point.x+8,field_point.y))!=FALSE,
        "Could not enqueue native mouse movement for click acceptance.");
    click_at(app,field_point);
    require(x(app,object)==original_position && app.document().undo_count()==original_history && !app.document().editing(),
        "Queued native movement turned a synthetic click into a committed drag.");
    type(app, L"42");
    require(x(app, object) == 42 && app.document().undo_count() == original_history, "Numeric draft did not preview without business history.");
    key(app, 'Z', true);
    require(x(app, object) == original_position, "Numeric local undo did not restore the original preview.");
    key(app, 'Z', true);
    require(x(app, object) == original_position && app.document().undo_count() == original_history, "Exhausted local undo fell through to business history.");
    key(app, 'Y', true);
    require(x(app, object) == 42, "Local redo did not remain owned after undo returned to the origin.");
    key(app, VK_RETURN);
    require(app.wants_keyboard() && x(app, object) == 42, "Commit did not retain passive field focus.");
    key(app, 'Z', true); require(x(app, object) == original_position, "Passive-focus undo did not reach scene history.");
    key(app, 'Z', true, true); require(x(app, object) == 42, "Ctrl+Shift+Z did not redo scene history.");
    key(app, 'Z', true); require(x(app, object) == original_position, "Text acceptance did not restore the scene value.");
    click(app, field); key(app, VK_END);
    auto start = centre(app, field); start.x = static_cast<LONG>(app.presented()->find(field)->bounds.x + 9 * app.presented()->drawing->dpi_scale);
    mouse(app, WM_LBUTTONDOWN, start); start.x += 24; mouse(app, WM_MOUSEMOVE, start); mouse(app, WM_LBUTTONUP, start);
    require(x(app, object) == original_position && !app.document().editing(), "Text-mode mouse selection started numeric dragging.");
    capture(app, output / "s2-number-selection.png");
    key(app, VK_ESCAPE);
    const auto failure_history = app.document().undo_count();
    require(atelier::application::EditorAcceptanceAccess::begin_edit(app, field).has_value(), "Could not seed an active backend transaction.");
    auto drag = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, drag); drag.x += 30; mouse(app, WM_MOUSEMOVE, drag);
    require(!app.document().editing() && x(app, object) == original_position && GetCapture() != app.window() &&
        app.document().undo_count() == failure_history && app.status_text().find(L"already active") != std::wstring::npos,
        "Failed edit_begin did not stop preview, roll back, release capture and preserve the first error.");
    capture(app, output / "s2-failed-edit.png");
    mouse(app, WM_LBUTTONUP, drag);
    click(app, control::reset);
}
void verify_transform_projection(Application& app, const std::filesystem::path& output, atelier::ObjectId entity) {
    const auto transform = entity;
    const auto before = model(atelier::world::read_transform(app.document().world().runtime(), transform));
    atelier::renderer::Capture original, rotated, scaled, restored;
    require(app.step(&original), "Could not capture the original World transform.");
    require(atelier::application::EditorAcceptanceAccess::edit_field(app, transform,
        app.document().types().rotation_fields[2], .7f).has_value(), "The real Session rotation transaction failed.");
    tick(app);
    require(app.step(&rotated) && rotated.depth != original.depth, "World rotation did not reach actual GPU geometry.");
    capture(app, output / "world-trs-rotated.png");
    require(atelier::application::EditorAcceptanceAccess::edit_field(app, transform,
        app.document().types().scale_fields[0], 1.75f).has_value(), "The real Session scale transaction failed.");
    tick(app);
    require(app.step(&scaled) && scaled.depth != rotated.depth, "World nonuniform scale did not reach actual GPU geometry.");
    capture(app, output / "world-trs-scaled.png");
    click(app, control::undo); click(app, control::undo);
    require(model(atelier::world::read_transform(app.document().world().runtime(), transform)) == before &&
        app.step(&restored) && restored.depth == original.depth, "TRS history did not restore the original value and GPU projection.");
}
void verify_list_workload(Application& app, const std::filesystem::path& output) {
    // Resize/DPI cases can leave the restored fixture partly off screen. Native
    // scrollbar and field gestures need their whole accepted area to be reachable.
    MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
    require(GetMonitorInfoW(MonitorFromWindow(app.window(),MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,
        "Cannot query the list workload monitor.");
    const auto area=monitor.rcWork;
    require(SetWindowPos(app.window(),nullptr,area.left,area.top,area.right-area.left,area.bottom-area.top,
        SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Cannot fit the list workload window on its monitor.");
    tick(app,4);
    // Populate through the real Create menu and its native popup surface.
    while (entities(app).size() + 1 < 256) {
        const auto before_count = entities(app).size();
        click(app,control::add_entity);
        require(entities(app).size() == before_count + 1, "Create menu failed to populate the real list workload.");
    }
    click(app, control::add_static_mesh);
    const auto first = entities(app).front().id();
    const auto last = entities(app).back().id();
    require(text_coverage(*app.presented(),app.object_control(last))!=nullptr,
        "Creating an Entity did not reveal its selected Scene Tree row.");
    wheel(app,control::entity_tree,240);
    require(app.presented()->find(app.object_control(last)) &&
        !text_coverage(*app.presented(), app.object_control(last)),
        "Offscreen list row lost geometry or emitted text before scrolling.");
    wheel(app, 23, -240);
    require(text_coverage(*app.presented(), app.object_control(last)) &&
        !text_coverage(*app.presented(), app.object_control(first)), "Scrolling did not exchange visible list text.");
    capture(app, output / "s1-list-256.png");
    wheel(app, 23, 240);
    require(text_coverage(*app.presented(), app.object_control(first)) != nullptr,
        "Scrolling back did not regenerate visible list text.");

    const auto bar=[&]() {
        const auto& bars=app.presented()->scrollbars;
        const auto found=std::find_if(bars.begin(),bars.end(),[](const auto& value){return value.key==23;});
        require(found!=bars.end(),"Overflowing Entity list has no accepted scrollbar geometry.");
        return *found;
    };
    const auto middle=[](atelier::ui::Rect box) { return POINT{static_cast<LONG>(box.x+box.width*.5f),static_cast<LONG>(box.y+box.height*.5f)}; };
    click_at(app,middle(bar().increment));
    require(bar().value>0,"Scrollbar step button did not scroll.");
    click_at(app,middle(bar().decrement));
    require(bar().value==0,"Scrollbar reverse step did not restore the start.");
    auto page_point=middle(bar().track);
    click_at(app,page_point);
    require(bar().value>=bar().page-1,"Scrollbar track click did not advance a page.");
    const auto history_before_scroll=app.document().undo_count();
    const auto from=middle(bar().thumb);
    auto to=from; to.y=static_cast<LONG>(bar().track.y+bar().track.height+80);
    {
        require_input_focus(app); KeyStateScope modifiers;
        SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(from.x,from.y));
        SendMessageW(app.window(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
        SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y)); tick(app,2);
    }
    require(bar().value==bar().maximum && text_coverage(*app.presented(),app.object_control(last)),"Scrollbar thumb did not reach the last Entity.");
    mouse(app,WM_LBUTTONDOWN,middle(bar().thumb));
    key(app,VK_ESCAPE);
    require(GetCapture()!=app.window() && app.document().undo_count()==history_before_scroll,"Scrollbar cancellation changed document history or retained capture.");
    require(app.presented()->find(app.object_control(last))->clip.right<=bar().bounds.x,"Entity rows overlapped the scrollbar gutter.");
    atelier::renderer::Capture track_pixels;
    require(app.step(&track_pixels) && track_pixels.frame_id,"Scrollbar color capture failed.");
    const auto accepted_bar=bar();
    const auto stroke=std::max(1.f,std::round(app.presented()->drawing->dpi_scale*2.f/3));
    const auto probe_y=accepted_bar.track.y+3*stroke;
    require(probe_y<accepted_bar.thumb.y,"Scrollbar track probe overlaps the thumb.");
    require_gray_pixel(track_pixels,static_cast<int>(std::round(accepted_bar.track.x+accepted_bar.track.width*.5f)),
        static_cast<int>(std::round(probe_y)),46);
    png(track_pixels,output/"scrollbar-thumb-end.png");
    wheel(app,23,240);

    TimingSeries scroll(app);
    app.sample_frames(true);
    for (unsigned frame = 0; frame < 48; ++frame) {
        wheel(app, 23, frame < 24 ? -8 : 8);
        scroll.record(app);
    }
    const auto field = property_control(app, last, 0);
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
    // A committed drag leaves passive focus, so document history owns Z/Y.
    key(app, 'Z', true);
    require(x(app, last) == before, "One undo did not restore the dense-scene drag.");
    key(app, 'Y', true);
    require(x(app, last) == after, "Redo did not restore the dense-scene drag.");
    std::ofstream report(output / "s1-performance.csv");
    report << "workload,stage,samples,mean,p50,p95,max\n";
    scroll.write(report, "256_entities_scroll");
    editing.write(report, "256_entities_field_drag");
    require(report.good(), "Cannot save window sampling evidence.");
    std::ofstream resources(output / "s1-resources.csv");
    resources << "workload,accepted_samples,prepare_calls,rasterizations,raster_bytes,culled_text_controls,ui_draw_calls,scene_draw_calls,viewport_draw_calls,coverage_upload_count,coverage_payload_bytes,peak_live_cpu_coverages,peak_live_cpu_bytes,peak_renderer_cache_count,peak_renderer_cache_texel_bytes,peak_renderer_cache_cpu_bytes\n";
    scroll.resources(resources, "256_entities_scroll");
    editing.resources(resources, "256_entities_field_drag");
    require(resources.good(), "Cannot save resource sampling evidence.");
}
#include "controls.inl"
#include "docking.inl"
#include "shelf.inl"
RECT await_acceptance_activation(Application& app,const std::filesystem::path& output,const char* kind) {
    const auto label=std::string("Editor ")+kind+" acceptance";
    std::ofstream(output/"acceptance.txt")<<label<<": WAITING FOR ACTIVATION (no synthetic input)\n";
    std::printf("%s: waiting up to 120 seconds; click the editor title bar to begin. No synthetic input has started.\n",label.c_str());
    std::fflush(stdout);
    SetForegroundWindow(app.window());SetFocus(app.window());tick(app);
    const auto deadline=GetTickCount64()+120000;
    while ((GetForegroundWindow()!=app.window() || GetFocus()!=app.window()) && GetTickCount64()<deadline)
        require(app.step(),"Editor closed while waiting for initial foreground.");
    if (GetFocus()!=app.window() || GetForegroundWindow()!=app.window()) {
        const auto foreground=GetForegroundWindow();
        DWORD process{};GetWindowThreadProcessId(foreground,&process);
        std::ofstream(output/"acceptance.txt")<<label<<": BLOCKED before input; initial activation timed out\n"
            <<"editor="<<app.window()<<"; foreground="<<foreground<<"; focus="<<GetFocus()<<"; foreground PID="<<process<<'\n';
        throw std::runtime_error("Initial foreground activation was not granted within 120 seconds; no synthetic input was sent.");
    }
    std::ofstream(output/"acceptance.txt")<<label<<": RUNNING (activation acquired)\n";
    std::printf("%s: activation acquired; starting input checks.\n",label.c_str());std::fflush(stdout);
    // Keep native pointer movement away from menu headings during message-driven gestures.
    RECT host{};GetWindowRect(app.window(),&host);
    require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park the acceptance pointer.");
    return host;
}

#include "menus.inl"
#include "tree.inl"
#include "project_window.inl"
#include "ui_authoring_window.inl"
#include "project_authoring_window.inl"
#include "viewport.inl"
#include "viewport_edit_window.inl"
#include "scene_lit_window.inl"
}

void guard_acceptance_native_dialogs() {
    if (!SetWindowsHookExW(WH_CBT, native_dialog_guard, nullptr, GetCurrentThreadId()))
        throw std::runtime_error("Cannot install the acceptance native dialog guard.");
}

int verify_editor_scene_lit(Application& app,const char* output_directory,const wchar_t* source_project) {
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const auto output=std::filesystem::absolute(atelier::editor::wide(output_directory));
    std::filesystem::create_directories(output);
    await_acceptance_activation(app,output,"Scene Lit");
    const auto legacy=output/"legacy-viewport";
    std::filesystem::create_directories(legacy);
    verify_viewport_display(app,legacy);
    verify_scene_lit_window(app,output,source_project?std::filesystem::path{source_project}:std::filesystem::path{});
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(),WM_CLOSE,0,0);require(!app.step(),"Scene Lit acceptance did not close normally.");
    std::ofstream report(output/"acceptance.txt");
    report<<"Editor Scene Lit window acceptance: PASS\nBuild: "<<atelier::build_info::revision
        <<"\nConfiguration: "<<atelier::build_info::configuration
        <<"\nFormal Project copy/save/reopen, native Scene Lit/MatCap display menu, three direct-light creation menus plus unique Sky Light, typed Parameters, native middle-orbit direction, actual World/snapshot/GPU frames, Shadows 4/5 status.\n"
            <<"Runtime World snapshots use separate document/Renderer E2E; no Simulate UI exists. Native display choices leave the editing lifecycle, authored history and data unchanged.\n"
        <<"GPU validation/vsync enabled; this window run is not a performance benchmark.\n";
    require(report.good(),"Cannot save Scene Lit window acceptance evidence.");
    std::puts("Editor Scene Lit window acceptance: PASS");return 0;
}

// Visual-only capture for design review: direct document/API calls, GPU
// readback PNGs, no activation and no synthetic OS input. Not an acceptance.
int capture_editor_snapshot(Application& app,const char* output_directory) {
    using Access=atelier::application::EditorAcceptanceAccess;
    using atelier::editor::Command;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    const auto output=std::filesystem::absolute(atelier::editor::wide(output_directory));std::filesystem::create_directories(output);
    const auto settle=[&]{for(int i=0;i<6;++i) require(app.step(),"Snapshot editor closed.");};
    const auto shot=[&](const char* name) {
        settle();atelier::renderer::Capture image;
        for(int attempt=0;attempt<30 && !image.frame_id;++attempt) require(app.step(&image),"Snapshot editor closed.");
        require(image.frame_id!=0,"Snapshot frame was not accepted (window occluded?).");png(image,output/name);
    };
    settle();
    model(Access::command(app,Command::create_static_mesh));
    Access::set_cursor(app,{1.4f,0,-1.1f});
    Access::set_tool(app,atelier::editor::TransformTool::rotate);shot("snapshot-rotate.png");
    Access::set_tool(app,atelier::editor::TransformTool::move);shot("snapshot-move.png");
    Access::set_tool(app,atelier::editor::TransformTool::scale);shot("snapshot-scale.png");
    Access::set_tool(app,atelier::editor::TransformTool::transform);shot("snapshot-transform.png");
    const auto at=[&](atelier::math::Vector3 p){const auto q=Access::viewport_point(app,p);return atelier::math::Vector2{float(q.x),float(q.y)};};
    // Surface-aligned cursor on the cube's +X face, then modal feedback.
    Access::place_cursor(app,at({.5f,.15f,-.15f}));shot("snapshot-cursor-surface.png");
    auto pointer=at({0,0,0});pointer.x+=60;pointer.y-=40;
    Access::tool_key(app,'G',pointer);Access::tool_key(app,'Z',pointer);pointer.y-=50;Access::tool_move(app,pointer);shot("snapshot-modal-grab.png");
    Access::tool_cancel(app);
    Access::tool_key(app,'R',pointer);Access::tool_key(app,'X',pointer);pointer.x+=70;Access::tool_move(app,pointer);shot("snapshot-modal-rotate.png");
    Access::tool_cancel(app);
    // An enum menu and colour swatch row, through the real light creation command.
    model(Access::command(app,Command::create_rectangle_light));shot("snapshot-light-parameters.png");
    if(const auto pick=Access::color_pick(app)) {model(Access::ui_action(app,pick));shot("snapshot-color-editor.png");
        model(Access::ui_action(app,atelier::editor::control::color_done));}
    // Layout review: category grouping and the Entity Structure page.
    model(Access::ui_action(app,atelier::editor::control::group_categories,true));shot("snapshot-parameters-category.png");
    model(Access::ui_action(app,atelier::editor::control::group_components,true));
    Access::select_tab(app,atelier::editor::control::entities_tab);
    model(Access::command(app,Command::rename_selected));shot("snapshot-inline-rename.png");
    Access::cancel_rename(app);
    Access::select_tab(app,atelier::editor::control::structure_tab);shot("snapshot-structure.png");
    Access::select_tab(app,atelier::editor::control::scene_tab);
    model(Access::open_menu(app,atelier::editor::control::viewport_snap));settle();
    {atelier::renderer::Capture menu;app.capture_menu(menu);if(!menu.rgba.empty()) png(menu,output/"snapshot-snap-menu.png");}
    Access::close_menu(app);
    model(Access::open_menu(app,atelier::editor::control::viewport_orientation));settle();
    {atelier::renderer::Capture menu;app.capture_menu(menu);if(!menu.rgba.empty()) png(menu,output/"snapshot-orientation-menu.png");}
    Access::close_menu(app);
    Access::performance_panel(app);shot("snapshot-world.png");
    Access::select_tab(app,atelier::editor::control::workbench);shot("snapshot-controls.png");
    Access::mark_saved(app);SendMessageW(app.window(),WM_CLOSE,0,0);
    while(app.step()) {}
    std::puts("Editor snapshot: written");return 0;
}
#include "performance.inl"
#include "surfaces.inl"
#include "tooltips_window.inl"
#include "native_dialogs.inl"
#include "dock_resize.inl"
#include "viewport_navigation.inl"
#include "shortcuts.inl"
int verify_editor_shortcuts(Application& app,const char* output_directory) {
    using namespace atelier;
    namespace control=atelier::editor::control;
    using Access=application::EditorAcceptanceAccess;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    const auto output=std::filesystem::absolute(editor::wide(output_directory));std::filesystem::create_directories(output);
    tick(app);
    verify_viewport_splitter_drag(app,output);
    require(bool(Access::command(app,editor::Command::create_static_mesh)),"Shortcut scene creation failed.");tick(app);
    verify_viewport_navigation(app,output);
    verify_menu_input_transitions(app);
    const auto first=app.document().selection().entity;
    const auto viewport=app.presented()->find(control::viewport)->bounds;
    const float x=viewport.x+viewport.width*.5f,y=viewport.y+viewport.height*.5f;
    const auto move=[&](float px,float py) {Access::input(app,{.kind=ui::EventKind::pointer_move,.x=px,.y=py});};
    const auto key=[&](WPARAM code,ui::Key mapped=ui::Key::none,bool shift=false,bool ctrl=false,bool up=false,bool repeat=false) {
        ui::Event e;e.kind=up?ui::EventKind::key_up:ui::EventKind::key_down;e.key=mapped;e.shift=shift;e.control=ctrl;e.repeat=repeat;
        Access::input(app,e,code);
    };
    move(x,y);
    key('X');key(VK_ESCAPE,ui::Key::escape);tick(app);
    require(!Access::menu_open(app),"An immediate Escape failed to cancel a pending popup.");
    const auto history=app.document().undo_count();key('D',ui::Key::none,true);tick(app);
    require(app.document().selected_entities().size()==1 && app.document().selection().entity!=first && Access::tool_active(app),
        "Shift+D did not duplicate and enter modal move.");
    key('D',ui::Key::none,true,false,false,true);
    require(app.document().undo_count()==history+1,"Repeated Shift+D created extra copies.");
    key('X');Access::input(app,{.kind=ui::EventKind::text,.data=std::wstring{L"2"}});key(VK_RETURN,ui::Key::enter);tick(app);
    require(app.document().undo_count()==history+1 && !Access::tool_active(app),"Duplicate movement created an extra undo step.");
    key('Z',ui::Key::z,false,true);tick(app);
    require(app.document().world().members()->size()==1,"Ctrl+Z did not undo the entire duplicate gesture.");
    key('Z',ui::Key::z,true,true);tick(app);
    require(app.document().world().members()->size()==2,"Redo did not restore the duplicate.");
    {
        // Zero scale is legal but degenerate: Project Output shows one line, counted in place when it recurs.
        auto& document=Access::physics_document(app);const auto copy=document.selection().entity;
        const auto original=*world::read_transform(document.world().runtime(),copy);auto flat=original;flat.scale={0,1,1};
        const auto degenerate=[&] {
            std::vector<std::wstring> lines;
            for(const auto& row:Access::output(app).rows)
                if(row.cells.front().text.find(L"degenerate_value")!=std::wstring::npos) lines.push_back(row.cells.front().text);
            return lines;
        };
        const auto flatten=[&](const world::Trs& value) {require(bool(document.set_transform(copy,value)),"A zero or restored scale was rejected.");tick(app);};
        flatten(flat);
        auto lines=degenerate();
        require(lines.size()==1 && lines[0].starts_with(L"Diagnostic [warning] degenerate_value in presentation") &&
            lines[0].find(L" scale: ")!=std::wstring::npos && lines[0].find(L"(×")==std::wstring::npos,"Zero scale did not report one degenerate warning.");
        flatten(original);flatten(flat);lines=degenerate();
        require(lines.size()==1 && lines[0].ends_with(L" (×2)"),"A recurring zero scale was not counted on its existing Project Output line.");
        flatten(original);
    }
    move(x,y);key('X');tick(app);
    require(app.menu_frame() && app.menu_frame()->find(static_cast<ui::ControlKey>(editor::Command::erase_entities)),"X did not open its confirmation menu.");
    RECT popup{};GetWindowRect(app.menu_window(),&popup);POINT client{static_cast<LONG>(x),static_cast<LONG>(y)};ClientToScreen(app.window(),&client);
    const auto item=app.menu_frame()->find(static_cast<ui::ControlKey>(editor::Command::erase_entities))->bounds;
    require(client.x-popup.left>=item.x && client.x-popup.left<item.x+item.width && client.y-popup.top>=item.y && client.y-popup.top<item.y+item.height,
        "Delete confirmation did not place its action under the invocation point.");
    const auto popup_hwnd=app.menu_window();const auto first_open=app.menu_open_cpu_us();
    key(VK_ESCAPE,ui::Key::escape);key(VK_ESCAPE,ui::Key::escape,false,false,true);tick(app);
    require(app.document().world().members()->size()==2 && !Access::menu_open(app),"Cancelling Delete changed the document.");
    const auto before_reopen=Access::menu_input_diagnostic(app);
    key('X');const bool reopen_requested=Access::menu_open(app);tick(app);
    if(app.menu_window()!=popup_hwnd) throw std::runtime_error("Delete menu did not reuse its native window: old="+
        std::to_string(reinterpret_cast<std::uintptr_t>(popup_hwnd))+", new="+std::to_string(reinterpret_cast<std::uintptr_t>(app.menu_window()))+
        ", requested="+std::to_string(reopen_requested)+", menu="+std::to_string(Access::menu_open(app))+", status="+editor::utf8(app.status_text())+
        "; before=["+before_reopen+"]; after=["+Access::menu_input_diagnostic(app)+"].");
    const auto warm_open=app.menu_open_cpu_us();
    Access::input(app,{.kind=ui::EventKind::pointer_down,.x=x,.y=y});Access::input(app,{.kind=ui::EventKind::pointer_up,.x=x,.y=y});tick(app);
    require(app.document().world().members()->size()==1 && !Access::menu_open(app),"Delete confirmation click missed its command.");
    key('Z',ui::Key::z);key('Z',ui::Key::z,false,false,true);tick(app);
    require(Access::menu_open(app) && app.presented()->popup==control::viewport_pie,"Tapping Z did not retain its pie menu.");
    move(x-90,y);tick(app);capture(app,output/"shading-pie.png");
    Access::input(app,{.kind=ui::EventKind::pointer_down,.x=x-90,.y=y});Access::input(app,{.kind=ui::EventKind::pointer_up,.x=x-90,.y=y});tick(app);
    require(!Access::menu_open(app) && Access::scene_lit_view(app).wireframe==renderer::Wireframe::only,"Pie click did not select Wireframe.");
    move(x,y);key('Z',ui::Key::z);move(x,y+90);key('Z',ui::Key::z,false,false,true);tick(app);
    require(!Access::menu_open(app) && Access::scene_lit_view(app).shading==renderer::Shading::matcap &&
        Access::scene_lit_view(app).wireframe==renderer::Wireframe::off,"Fast hold/swipe/release was lost before the first pie frame.");
    move(x,y);key('Z',ui::Key::z);tick(app);Access::finish_menu_hold(app);key('Z',ui::Key::z,false,false,true);tick(app);
    require(!Access::menu_open(app),"Held pie release in the dead zone did not cancel.");
    key('Z',ui::Key::z);tick(app);key(VK_ESCAPE,ui::Key::escape);tick(app);
    require(!Access::menu_open(app) && !app.presented()->popup,"Pie cancellation leaked modal state.");
    move(viewport.x+viewport.width-6,viewport.y+6);key('Z',ui::Key::z);key('Z',ui::Key::z,false,false,true);tick(app);
    const auto edge_item=app.presented()->find(control::shading_preset_first+1)->bounds;
    Access::input(app,{.kind=ui::EventKind::pointer_down,.x=edge_item.x+edge_item.width*.5f,.y=edge_item.y+edge_item.height*.5f});
    Access::input(app,{.kind=ui::EventKind::pointer_up,.x=edge_item.x+edge_item.width*.5f,.y=edge_item.y+edge_item.height*.5f});tick(app);
    require(!Access::menu_open(app) && Access::scene_lit_view(app).shading==renderer::Shading::smooth,
        "A screen-edge pie slot click was interpreted relative to the old gesture origin.");
    const auto camera=app.presented()->find(control::viewport_camera)->bounds;
    const auto click=[&](float px,float py) {
        Access::input(app,{.kind=ui::EventKind::pointer_down,.x=px,.y=py});
        Access::input(app,{.kind=ui::EventKind::pointer_up,.x=px,.y=py});tick(app);
    };
    Access::input(app,{.kind=ui::EventKind::pointer_down,.x=camera.x+camera.width*.5f,.y=camera.y+camera.height*.5f});
    require(Access::menu_open(app),"Camera menu waited for button release.");tick(app);
    Access::input(app,{.kind=ui::EventKind::pointer_up,.x=camera.x+camera.width*.5f,.y=camera.y+camera.height*.5f});tick(app);
    const auto* pill=app.presented()->find(control::viewport_camera);
    require(pill->style->border_style==ui::BorderStyle::solid && pill->style->radius.top_left>0,
        "The open camera button changed from a pill to a square.");
    capture(app,output/"persp-menu.png");key(VK_ESCAPE,ui::Key::escape);tick(app);
    require(bool(Access::select_entity(app,{})),"Empty selection failed.");tick(app);capture(app,output/"parameters-empty.png");
    // Shortcut dispatch must yield to actual text focus.
    require(bool(Access::select_entity(app,first)),"Text focus setup failed.");tick(app);
    const auto name=app.presented()->find(Access::name_control(app,first))->bounds;
    click(name.x+name.width*.5f,name.y+name.height*.5f);move(x,y);const auto text_history=app.document().undo_count();
    key('D',ui::Key::none,true);key('X');key('Z',ui::Key::z);tick(app);
    require(!Access::menu_open(app) && !Access::tool_active(app) && app.document().undo_count()==text_history,
        "Viewport shortcuts escaped text focus.");
    verify_shortcut_input_boundaries(app);
    Access::mark_saved(app);app.close();
    std::ofstream(output/"acceptance.txt")<<"Viewport shortcut acceptance: PASS\nConfiguration: "<<build_info::configuration
        <<"\nDelete menu CPU first/warm us: "<<first_open<<" / "<<warm_open
        <<"\nMenu transitions: queued replacement, top-level branch, command capture transfer."
        <<"\nReal Application input routing, World history, Source/JIT controls, native popup, GPU captures. No desktop input injection.\n";
    std::puts("Viewport shortcut acceptance: PASS");return 0;
}
int verify_editor_edit_mode(Application& app,const char* output_directory) {
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope {~ComScope(){CoUninitialize();}} com_scope;
    const auto output=std::filesystem::absolute(atelier::editor::wide(output_directory));std::filesystem::create_directories(output);
    await_acceptance_activation(app,output,"edit mode");tick(app);
    verify_viewport_edit_window(app,output);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(),WM_CLOSE,0,0);require(!app.step(),"Edit-mode acceptance did not close normally.");
    std::ofstream(output/"acceptance.txt")<<"Editor edit-mode window acceptance: PASS\nBuild: "<<atelier::build_info::revision
        <<"\nConfiguration: "<<atelier::build_info::configuration<<"\nNative selection, grouped transforms, parameter tuples, undo/redo, cancellation and resource inspection.\n";
    std::puts("Editor edit-mode window acceptance: PASS");return 0;
}

int verify_editor_source(Application& app,const char* output_directory) {
    using Access=atelier::application::EditorAcceptanceAccess;
    using atelier::editor::Command;
    namespace source=atelier::editor::source_control;
    const auto output=std::filesystem::absolute(atelier::editor::wide(output_directory));
    std::filesystem::create_directories(output);
    const auto path=output/"editor.ui.source";
    Access::ui_source(app,path);
    await_acceptance_activation(app,output,"source");
    click(app,control::add_static_mesh);const auto scene_history=app.document().undo_count();
    click(app,static_cast<ControlKey>(Command::edit_ui_source));tick(app,3);
    const auto* code=app.presented()->find(source::buffer);
    require(code && code->enabled && code->code,"The real menu did not open the active UI module in Code.");
    const auto document=code->code;
    const auto original=document->text();
    const auto marker=original.find(L"Scene root content");require(marker!=original.npos,"Source fixture marker is missing.");
    const auto row=static_cast<unsigned>(std::count(original.begin(),original.begin()+static_cast<std::ptrdiff_t>(marker),L'\n'));
    key(app,VK_HOME,true);
    for (unsigned i=0;i<row;++i) key(app,VK_DOWN);
    key(app,VK_HOME);key(app,VK_END,false,true);
    {
        KeyStateScope modifiers;
        for (const auto c:std::wstring_view(L"        text: \"Edited inside Atelier\";")) SendMessageW(app.window(),WM_CHAR,c,0);
        tick(app,2);
    }
    require(document->text().find(L"Edited inside Atelier")!=std::wstring::npos && app.document().undo_count()==scene_history,"Native source input missed its document or edited scene history.");
    const auto revision=Access::ui_revision(app);
    key(app,'S',true);
    const auto wait=[&](auto predicate,const char* message) {
        const auto deadline=GetTickCount64()+10000;
        do {tick(app,1);if (predicate()) return;} while (GetTickCount64()<deadline);
        throw std::runtime_error(std::string(message)+": "+std::string(Access::ui_error(app)));
    };
    wait([&]{return Access::ui_revision(app)>revision;},"Native Ctrl+S did not publish the edited UI");
    require(model(atelier::editor::read_file(path)).find("Edited inside Atelier")!=std::string::npos,"Native save did not write the actual file.");
    capture(app,output/"source-editor.png");
    key(app,VK_END,true);key(app,VK_RETURN);
    {KeyStateScope modifiers;for (const auto c:std::wstring_view(L"ui Broken( {")) SendMessageW(app.window(),WM_CHAR,c,0);tick(app,2);}
    const auto valid_revision=Access::ui_revision(app);key(app,'S',true);
    wait([&]{return !Access::ui_error(app).empty();},"Native invalid source produced no diagnostic");
    require(Access::ui_revision(app)==valid_revision && app.presented()->find(source::problem)->enabled,"Invalid source replaced the live UI or disabled error navigation.");
    click(app,source::problem);tick(app,2);
    require(app.text_input() && app.text_input()->token.key==source::buffer && document->edit().line_at(document->edit().current().caret.offset)>500,
        "Clicking the compiler diagnostic did not focus the source location.");
    capture(app,output/"source-diagnostic.png");
    key(app,'Z',true);key(app,'S',true);
    wait([&]{return Access::ui_revision(app)>valid_revision && Access::ui_error(app).empty();},"Native source correction did not recover");
    const auto saved=document->text();
    key(app,'A',true);key(app,'C',true);key(app,VK_END,true);key(app,'V',true);
    require(document->text().size()==2*saved.size(),"Native source clipboard retained the single-line 4096-character limit.");
    key(app,'Z',true);require(document->text()==saved,"Native document undo failed after a large paste.");
    key(app,'S',true);key(app,'W',true);tick(app,2);
    require(app.presented()->find(source::buffer) && !app.presented()->find(source::buffer)->enabled,"Closing a source document left a live edit target.");
    verify_project_authoring_window(app,output);
    Access::mark_saved(app);SendMessageW(app.window(),WM_CLOSE,0,0);require(!app.step(),"Source acceptance did not close cleanly.");
    std::ofstream(output/"acceptance.txt")<<"Editor source window acceptance: PASS\nConfiguration: "<<atelier::build_info::configuration
        <<"\nAccepted frames: "<<app.accepted_frames()<<"\nNative menu/open, caret/selection, source edit, Ctrl+S/JIT publication, invalid-save retention, diagnostic click, correction, large clipboard, undo, project component/typed properties/save/reopen, multi-file diagnostic, isolated play and UI preview, close.\n";
    std::puts("Editor source window acceptance: PASS");return 0;
}

int verify_editor_project(Application& app,const char* output_directory,const wchar_t* source_project) {
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const auto base=std::filesystem::weakly_canonical(atelier::editor::wide(output_directory));
    const auto output=base/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if (source_project) {
        const auto source=std::filesystem::canonical(source_project);
        const auto relative=base.lexically_relative(source);
        require(relative.empty() || *relative.begin()==L"..","Project acceptance output must be outside the source project.");
        const auto folder=output/L"项目";
        std::filesystem::create_directories(folder);
        for (const auto name:{L"Config",L"Content",L"Source"}) if (std::filesystem::exists(source/name))
            std::filesystem::copy(source/name,folder/name,std::filesystem::copy_options::recursive|std::filesystem::copy_options::skip_symlinks);
    } else std::filesystem::create_directories(output);
    const auto ui_source=output/"editor.ui.source";
    atelier::application::EditorAcceptanceAccess::ui_source(app,ui_source);
    await_acceptance_activation(app,output,"project");
    require(entities(app).empty(),"The production editor did not start with an empty scene.");
    verify_viewport_display(app,output);
    verify_project_window(app,output,source_project!=nullptr);
    verify_ui_authoring_window(app,output,ui_source);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(),WM_CLOSE,0,0);require(!app.step(),"Project acceptance did not close the editor.");
    std::ofstream report(output/"acceptance.txt");
    report<<"Editor project acceptance: PASS\nBuild: "<<atelier::build_info::revision<<"\nConfiguration: "<<atelier::build_info::configuration
        <<"\nSource project: "<<(source_project?atelier::editor::utf8(source_project):"new fixture")
        <<"\nSource project is read only; Config/Content/Source copied without links or Cache.\n"
        <<"Shared viewport display/TRS checks followed by project commands, About identity/current root, Entity creation, active-field save, asset views/search/filter, OBJ import/create, Scene Tree selection, position edit/undo/redo, actual MatCap/wireframe pixels, save/reopen, failed open, refresh failure after save, movable project and disposable Cache.\n"
        <<"Native UI authoring: real file save/recompile/present, compile-error retention/recovery, repeated tree rows, foldout state and active text token retained.\n";
    std::printf("Editor project acceptance: PASS; evidence: %s\n",atelier::editor::utf8(output.wstring()).c_str());
    return 0;
}

int verify_editor_window(Application& app, const char* output_directory) {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const std::filesystem::path output(output_directory);
    std::filesystem::create_directories(output);
    const auto host=await_acceptance_activation(app,output,"window");
    tick(app);
    require(entities(app).empty(),"The production editor did not start with an empty scene.");
    verify_viewport_fly(app,output);
    click(app,control::file_menu);
    const auto first_open_us=app.menu_open_cpu_us(); const auto retained_popup=app.menu_window(); key(app,VK_ESCAPE);
    click(app,control::file_menu);
    const auto warm_open_us=app.menu_open_cpu_us();
    require(app.menu_window()==retained_popup,"Warm open replaced the native popup window."); key(app,VK_ESCAPE);
    std::ofstream(output/"popup-open.csv") << "path,cpu_us\nfirst," << first_open_us << "\nwarm," << warm_open_us << "\n";
    click(app,control::add_static_mesh);
    require(entities(app).size()==1 && app.document().undo_count()==1,"Static mesh creation was not a single document command.");
    require(model(app.document().world().runtime().type_of(app.document().selection().entity))==app.document().types().static_mesh_entity,
        "Create shelf did not construct the real StaticMeshEntity type.");
    verify_parameter_search(app,output);
    verify_tool_shelf(app,output);
    capture(app, output / "m0.png");
    verify_chrome_pixels(app,output);
    const auto named_entity=app.document().selection().entity;
    const auto name_key=atelier::application::EditorAcceptanceAccess::name_control(app,named_entity);
    const std::wstring original_name(app.document().name(named_entity));
    click(app,name_key); type(app,L"主角 Hero"); key(app,VK_RETURN);
    require(app.document().name(named_entity)==L"主角 Hero","Object header did not commit the document name.");
    key(app,'Z',true);
    require(app.document().name(named_entity)==original_name,"Object name bypassed scene Undo.");
    key(app,'Y',true);
    require(app.document().name(named_entity)==L"主角 Hero","Object name Redo failed.");
    key(app,'Z',true); key(app,VK_ESCAPE);
    mouse(app,WM_MOUSEMOVE,centre(app,control::viewport));
    const auto tab_style=*app.presented()->find(control::parameters_tab)->style;
    mouse(app,WM_MOUSEMOVE,centre(app,control::parameters_tab));
    require(*app.presented()->find(control::parameters_tab)->style==tab_style,"Workspace tab hover changed its appearance.");
    auto cursor_point=centre(app,property_control(app,named_entity,0));
    ClientToScreen(app.window(),&cursor_point);
    require(SetCursorPos(cursor_point.x,cursor_point.y)!=FALSE,"Cannot position the cursor over its accepted field.");
    tick(app,3);
    const auto adjust_cursor=GetCursor();
    require(adjust_cursor && adjust_cursor!=LoadCursorW(nullptr,IDC_ARROW) && adjust_cursor!=LoadCursorW(nullptr,IDC_SIZEWE),"Number adjustment did not use its own cursor.");
    SendMessageW(app.window(),WM_SETCURSOR,reinterpret_cast<WPARAM>(app.window()),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
    require(GetCursor()==adjust_cursor,"WM_SETCURSOR overwrote the adjustment cursor.");
    require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot restore the acceptance pointer.");
    tick(app,3);
    const auto navigation_history=app.document().undo_count();
    click(app,control::parameter_back);
    require(app.document().selection().entity==atelier::ObjectId{} && app.presented()->find(control::scene_forward)->enabled,"Path back did not reach World context.");
    require(!app.presented()->find(control::parameter_list) && !app.presented()->find(control::position_fold) &&
        !app.presented()->find(control::parameter_search),"World context retained object properties or Filter.");
    click(app,control::scene_forward);
    require(app.document().selection().entity==named_entity,"Path forward did not restore the selected Entity.");
    click(app,control::entity_path); capture(app,output/"path-context-menu.png");
    click(app,control::world_context);
    click(app,control::parameter_path); key(app,VK_DOWN); key(app,VK_RETURN);
    require(app.document().selection().entity==named_entity && app.document().undo_count()==navigation_history,"Path menu selection changed document history or chose the wrong Entity.");
    click(app, control::reset);
    const auto original_width = app.parameter_width();
    auto divider = centre(app, app.parameter_splitter());
    {
        require_input_focus(app); KeyStateScope modifiers;
        SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(divider.x,divider.y));
        divider.x-=50;
        SendMessageW(app.window(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(divider.x,divider.y));
        SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(divider.x,divider.y)); tick(app,2);
    }
    require(app.parameter_width() > original_width + 20, "Pane divider did not change the workspace.");

    const auto initial_count = entities(app).size();
    click(app, control::add_entity); click(app, control::add_entity);
    require(entities(app).size() == initial_count + 2, "Create command did not enter the scene.");
    const auto added_key = app.document().selection().entity;
    const auto added_id = added_key;
    click(app, control::undo);
    require(!contains(app, added_key), "Undo add did not remove membership.");
    click(app, control::redo);
    require(contains(app, added_id), "Redo add replaced object identity.");
    require(model(atelier::world::components(app.document().world().runtime(), added_key)).empty(),
        "Add Entity did not produce an empty Entity.");
    const auto render_count = model(app.document().world().render_items()).size();
    const auto creation_history=app.document().undo_count();
    click(app,atelier::application::EditorAcceptanceAccess::asset_key(app,"builtin/sphere"));
    require(app.document().undo_count()==creation_history,"Browsing a resource created a scene object.");
    click(app,atelier::editor::browser_control::add);
    const auto mesh_entity=app.document().selection().entity;
    const auto mesh_component=component(app,mesh_entity,app.document().types().static_mesh_component);
    require(app.document().undo_count()==creation_history+1 &&
        model(app.document().world().render_items()).size()==render_count+1 &&
        model(app.document().world().runtime().type_of(mesh_entity))==app.document().types().static_mesh_entity,
        "Asset shelf did not create one complete typed Entity through scene history.");
    capture(app,output/"world-properties.png");
    click(app,control::undo);
    require(!contains(app,mesh_entity) && model(app.document().world().render_items()).size()==render_count,
        "Undo did not remove the whole Entity created by the shelf.");
    click(app,control::redo);
    require(contains(app,mesh_entity) && component(app,mesh_entity,app.document().types().static_mesh_component)==mesh_component,
        "Redo changed the created Entity/component identity.");
    click(app,static_cast<ControlKey>(atelier::editor::Command::show_structure));
    click(app,control::add_component);
    const auto body_choice=atelier::application::EditorAcceptanceAccess::menu_item(app,L"world.CollisionComponent");
    require(body_choice,"Entity Structure omitted the physical Body type.");
    click(app,body_choice);
    const auto physics_body=component(app,mesh_entity,app.document().types().collision_component);
    require(app.document().selection()==atelier::editor::Selection{mesh_entity,physics_body},
        "Adding a physical Body did not inspect the new Component.");
    capture(app,output/"physics-components.png");
    click(app,atelier::application::EditorAcceptanceAccess::structure_control(app,physics_body));
    key(app,VK_DELETE);
    require(model(atelier::world::components(app.document().world().runtime(),mesh_entity)).size()==1,
        "Physics body removal left a component attached.");
    key(app,'Z',true);
    require(component(app,mesh_entity,app.document().types().collision_component)==physics_body,
        "Undo physical Body removal changed identity.");
    key(app,'Y',true);
    click(app,control::show_entities);
    const auto object = entities(app).front().id();
    click(app, app.object_control(object));
    require(app.document().selection().entity == object, "List selection did not select the clicked object.");
    auto field = property_control(app, object, 0);
    const auto before = x(app, object);
    const auto history = app.document().undo_count();
    auto point = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, point);
    for (unsigned move = 0; move < 4; ++move) { point.x += 12; mouse(app, WM_MOUSEMOVE, point); }
    if (!(x(app,object)>before && app.document().undo_count()==history)) {
        capture(app,output/"failed-field-drag.png");
        throw std::runtime_error("Drag failed to preview without history: before="+std::to_string(before)+
            ", value="+std::to_string(x(app,object))+", undo="+std::to_string(app.document().undo_count())+
            ", expected undo="+std::to_string(history)+", editing="+std::to_string(app.document().editing()));
    }
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
    const auto count = entities(app).size();
    key(app, 'A', true); key(app, VK_DELETE);
    require(entities(app).size() == count, "Delete in text focus deleted a scene object.");
    key(app, VK_ESCAPE);
    click(app, field); type(app, L"-2.5"); key(app, VK_RETURN);
    require(std::abs(x(app, object) + 2.5f) < .0001f, "Numeric text commit did not update the Runtime field.");
    click(app, field); type(app, L"1000e");
    const auto last_valid_preview = x(app, object);
    const auto invalid_history = app.document().undo_count();
    key(app, VK_RETURN);
    require(x(app, object) == last_valid_preview && app.document().undo_count() == invalid_history && app.wants_keyboard(),
        "Invalid Enter wrote the scene, recorded history or exited text focus.");
    key(app, VK_ESCAPE);
    require(std::abs(x(app, object) + 2.5f) < .0001f, "Esc did not roll back the invalid draft's earlier preview.");
    const auto identity = object;
    const auto old_entity_control = app.object_control(object);
    const auto old_field_control = field;
    click(app,app.object_control(object));
    click(app, control::remove_selected);
    require(!contains(app, object), "Deleted object remained a member.");
    tick(app, 64); // host GC crosses complete cycles while only history holds this object
    click(app, control::undo);
    require(contains(app, identity) && std::abs(x(app, object) + 2.5f) < .0001f,
        "Window delete/GC/undo failed to restore identity and position.");
    field = property_control(app, object, 0);
    require(field != old_field_control && app.object_control(object) != old_entity_control,
        "Entity restore reused retired control identities.");
    click(app, control::focus); click(app, control::reset);

    verify_transform_projection(app, output, object);

    atelier::renderer::Capture camera_before, camera_orbit, camera_zoom;
    using CameraInput=atelier::application::EditorAcceptanceAccess;
    require(app.step(&camera_before), "Camera capture failed.");
    auto view = centre(app, control::viewport);
    CameraInput::move_pointer(app,view); tick(app,1);
    SendMessageW(app.window(), WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    view.x += 42; view.y += 18;
    CameraInput::move_pointer(app,view); tick(app,1);
    SendMessageW(app.window(), WM_MBUTTONUP, 0, MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(app.step(&camera_orbit) && camera_orbit.depth != camera_before.depth, "Middle-drag orbit did not change the rendered scene.");
    ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(app.step(&camera_zoom) && camera_zoom.depth != camera_orbit.depth, "Wheel zoom did not change the rendered scene.");
    click(app, control::reset);
    for (const auto reason : {WM_CANCELMODE, WM_ACTIVATEAPP, WM_CAPTURECHANGED, WM_KILLFOCUS}) {
        require_input_focus(app);
        view = centre(app, control::viewport);
        CameraInput::move_pointer(app,view,reason); tick(app,1);
        SendMessageW(app.window(), WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
        CameraInput::require_camera(app,GetCapture()==app.window(),reason,"Orbit did not acquire window capture");
        const auto before = CameraInput::camera_view(app);
        view.x += 18; view.y += 9;
        CameraInput::move_pointer(app,view,reason); tick(app,1);
        CameraInput::require_camera(app,CameraInput::camera_view(app)!=before,reason,"Camera cancellation did not start a real orbit");
        if (reason == WM_CAPTURECHANGED) ReleaseCapture();
        else if (reason == WM_KILLFOCUS) SetFocus(nullptr);
        else SendMessageW(app.window(), reason, 0, 0);
        tick(app, 2);
        CameraInput::require_camera(app,GetCapture()!=app.window(),reason,"Native cancellation retained camera-owned capture");
        const auto stopped = CameraInput::camera_view(app);
        view.x += 24; view.y += 12;
        CameraInput::move_pointer(app,view,reason); tick(app,1);
        CameraInput::require_camera(app,CameraInput::camera_view(app)==stopped,reason,
            "Native cancellation left camera orbit active after capture release");
        SendMessageW(app.window(), WM_MBUTTONUP, 0, MAKELPARAM(view.x, view.y)); tick(app, 1);
        CameraInput::require_camera(app,CameraInput::camera_view(app)==stopped,reason,"Late button release changed a cancelled orbit");
        if (reason == WM_KILLFOCUS) SetFocus(app.window());
        else if (reason == WM_ACTIVATEAPP) SendMessageW(app.window(), WM_ACTIVATEAPP, TRUE, 0);
        tick(app, 1);
    }

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
    verify_stow_alignment(app);
    verify_control_states(app, output, object);
    verify_style_changes(app, output, object);
    verify_shared_text(app, output, object);
    verify_docking(app, output, object);
    verify_native_menus(app, output, object);
    verify_basic_controls(app, output, object);
    verify_tree_view(app,output,object);
    view = centre(app, control::viewport); ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), MAKELPARAM(view.x, view.y)); tick(app, 1);
    capture(app, output / "editor.png");
    verify_list_workload(app, output);
    verify_project_window(app,output);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(), WM_CLOSE, 0, 0);
    require(!app.step(), "WM_CLOSE did not close the application.");
    {
        Application quit_app(true);
        SetForegroundWindow(quit_app.window()); SetFocus(quit_app.window());
        RECT quit_host{}; GetWindowRect(quit_app.window(),&quit_host);
        require(SetCursorPos(quit_host.left+32,quit_host.top+12)!=FALSE,"Cannot park the quit acceptance pointer.");
        tick(quit_app);
        key(quit_app,VK_F10);
        require(quit_app.menu_window() && quit_app.menu_frame(),"Quit acceptance did not open an accepted native menu.");
        PostQuitMessage(0);
        atelier::renderer::Capture quit_capture;
        quit_app.capture_menu(quit_capture);
        require(quit_capture.frame_id!=0,"Quit acceptance did not render the native menu capture.");
        require(!quit_app.step(),"Native menu capture consumed the thread quit before the main window.");
        require(!quit_app.menu_window() && GetCapture()!=quit_app.window(),"Thread quit retained an interactive native menu.");
    }
    std::ofstream report(output / "acceptance.txt");
    report << "Editor window acceptance: PASS\n"
           << "Real HWND messages -> platform adapter -> UI accepted-frame input -> Session -> World -> Runtime -> matrix-value renderer\n"
           << "World: empty Entity; typed StaticMeshEntity creation from shelf; one-step creation undo/redo preserves component identity; Entity delete/GC/undo same composition/new bindings; AttachedInactive presentation/editing\n"
           << "TRS: actual Session field transactions set rotation/nonuniform scale, World value matrices alter captured GPU depth, two UI undo commands restore the original value and depth; XYZ controls remain position-only\n"
           << "splitter; add/undo/redo identity; list selection; drag preview/one undo; Esc; text/Delete; numeric range; delete/GC/undo; camera buttons/orbit/zoom; cancel mode/deactivation/capture/focus loss with stopped orbit and late release; resize; minimize; DPI transition; WM_CLOSE shutdown; thread quit preserved through native menu capture\n"
           << "S1 retained: shared states; color-only coverage reuse/zero upload; invalid-style rejection; font/metrics/caret/raster; old-frame lifetime; 256 Entities/culling/scroll-back\n"
           << "Scene Tree: actual World membership and Entity types; Entity-only rows; World expansion; type/name filtering; close/reopen retains expansion; no scene history changes.\n"
           << "Entity Structure: separate dock tab; Entity context plus inspected Component identity; keyboard expansion/navigation and type filtering; native add/delete/undo/redo restore the same component; Scene Tree remains Entity-only.\n"
           << "S2: Unicode parameter search; preview/commit/Esc/OS blur; ZWJ/combining/surrogate deletion; grouped local undo/redo; native clipboard cut/paste; bidi selection; numeric text selection; invalid Enter; retained focus; passive-focus business Z/Y and active local history ownership\n"
           << "Dock workspace: accepted-frame tab drag/preview/cancel, edge split, strip merge/reorder, both divider axes, maximize/restore, Ctrl+Tab/Ctrl+W, reopen and final-pane reset, retained field identity and separate histories; collapse either side, thin restore strip, exact ratio restoration, Show reveals ancestors, central swap and Alt rotation; tab-first chrome, adjacent add button, native add menu moving the existing view.\n"
           << "Parameter controls: shared aligned text/choice/vector rows, modified/default states, atomic tuple reset, native context menus and clipboard, invalid paste cancellation, modal outside click/wheel/Delete, Apps/Shift+F10/Esc, combo keyboard selection/history, middle-drag ladder/fine steps/defaults/cancellation/cache reuse, checkbox/foldout, disabled inheritance, narrow Vector3/Vector4, isolated sample history\n"
           << "Parameter slider: separate navigation/legal ranges, native track click and drag preview, one commit/undo, Esc and capture-loss rollback, original value outside navigation range, Home/End and Shift keyboard steps, disabled input; shared axis-accent SS and GPU capture.\n"
           << "Chrome: GPU pixels match ordinary field outer/inner/fill, selected shelf tab open-bottom colors, #2E2E2E unselected tabs/tab strip/scroll track and #191919 status bar; Filter remains above one flat property list; shared tab edges, horizontal scrollbar gradient/dots/step availability, combo arrows, leading path icon, context menu selection and back/forward without document history.\n"
           << "Shelf: 56 DIP expanded; shared 12 DIP rail and full-width 12 DIP restore strip; leading arrow, neutral idle/hover/focus and blue pressed gradient; middle/far-end click and keyboard expansion retain category, control identity and scene state.\n"
           << "Native menus: actual owned WS_POPUP HWND; shared UI/SS/GPU renderer; beyond-editor placement and screen-work-area containment; F10/Alt mnemonic, arrows/hover switching, reused host, restored owner focus, cancel/focus-loss cleanup; menu commands, context clipboard and combo choices use the original consumers; narrow menubar stow rail, restored geometry/history and F10 reveal.\n"
           << "Failure evidence: real clipboard lock contention preserved the draft; a seeded real Session transaction made native drag edit_begin fail, then preview stopped, rollback/capture release ran, and the first error remained visible.\n"
           << "IME evidence: Unicode WM_CHAR and clipboard checks do not establish system IME acceptance. Real profile/physical-key/composition/candidate verification remains a separate completion gate.\n"
           << "Sampling: s1-performance.csv contains CPU call microseconds, including Present/waits in render; capture frames excluded. No GPU duration or before/after speedup claimed.\n"
           << "Sampling environment: this acceptance application enables hardware D3D12 debug/GBV and renderer vsync, including in release; results do not represent validation-disabled production frame time.\n"
           << "s1-resources.csv: UI cumulative counters use interval differences; renderer activity sums sampled render results. Resource peaks are sampled ownership counts, not total process memory or physical VRAM; R8 upload bytes exclude transfer padding.\n"
           << "DPI coverage: WM_DPICHANGED policy at the current monitor DPI; physical multi-monitor DPI migration not exercised.\n"
           << "Accepted frames: " << app.accepted_frames() << "\n";
    std::puts("Editor window acceptance: PASS");
    std::fflush(stdout);
    return 0;
}

int verify_editor_ime(Application& app, const char* output_directory) try {
    using Microsoft::WRL::ComPtr;
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "Cannot initialize IME acceptance COM.");
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const std::filesystem::path output(output_directory);
    std::filesystem::create_directories(output);
    std::ofstream(output / "ime-acceptance.txt") << "Sogou Pinyin acceptance: RUNNING\n";
    std::ofstream setup(output / "ime-platform.txt");
    const auto record_profile = [&](const char* label, HRESULT result, const TF_INPUTPROCESSORPROFILE& value) {
        wchar_t clsid[40]{}, profile_id[40]{};
        StringFromGUID2(value.clsid, clsid, 40); StringFromGUID2(value.guidProfile, profile_id, 40);
        setup << label << ": HRESULT=" << std::hex << result << "; type=" << value.dwProfileType
              << "; language=" << value.langid << "; CLSID=" << std::string(clsid, clsid + wcslen(clsid))
              << "; profile=" << std::string(profile_id, profile_id + wcslen(profile_id)) << "; HKL=" << value.hkl << '\n';
        setup.flush();
    };
    CLSID sogou{}; GUID profile{};
    require(SUCCEEDED(CLSIDFromString(L"{E7EA138E-69F8-11D7-A6EA-00065B844310}", &sogou)) &&
        SUCCEEDED(CLSIDFromString(L"{E7EA138F-69F8-11D7-A6EA-00065B844311}", &profile)), "Invalid Pinyin profile identity.");
    ComPtr<ITfInputProcessorProfiles> languages;
    require(SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&languages))), "Cannot inspect input profiles.");
    BOOL enabled{};
    require(SUCCEEDED(languages->IsEnabledLanguageProfile(sogou, 0x0804, profile, &enabled)), "Cannot query Sogou Pinyin enablement.");
    if (!enabled) {
        std::ofstream(output / "ime-acceptance.txt") << "Sogou Pinyin acceptance: BLOCKED (profile is not enabled; no settings changed)\n";
        throw std::runtime_error("Sogou Pinyin is not enabled; real IME acceptance is blocked.");
    }
    const auto require_foreground = [&] {
        const auto foreground = GetForegroundWindow();
        if (foreground == app.window()) return;
        DWORD owner{}; GetWindowThreadProcessId(foreground, &owner);
        wchar_t class_name[256]{}; GetClassNameW(foreground, class_name, 256);
        setup << "Foreground interruption: HWND=" << foreground << "; editor=" << app.window()
              << "; focus=" << GetFocus() << "; PID=" << std::dec << owner
              << "; class=" << std::string(class_name, class_name + wcslen(class_name));
        if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner)) {
            wchar_t name[32768]{}; DWORD size = 32768;
            if (QueryFullProcessImageNameW(process, 0, name, &size)) {
                const auto filename = std::filesystem::path(name).filename().wstring();
                setup << "; executable=" << std::string(filename.begin(), filename.end());
            }
            CloseHandle(process);
        }
        setup << '\n'; setup.flush();
        throw std::runtime_error("IME acceptance lost foreground ownership; no further key was sent (see ime-platform.txt).");
    };
    SetForegroundWindow(app.window()); SetFocus(app.window()); tick(app);
    const auto foreground_deadline = GetTickCount64() + 20000;
    while (GetForegroundWindow() != app.window() && GetTickCount64() < foreground_deadline)
        require(app.step(), "Editor closed while waiting for initial foreground.");
    require_foreground(); // Wait for the user once; never repeatedly seize foreground.
    click(app, control::parameter_search_toggle);
    click(app, control::parameter_search);
    ComPtr<ITfInputProcessorProfileMgr> manager;
    require(SUCCEEDED(languages.As(&manager)), "Cannot inspect active input profile.");
    TF_INPUTPROCESSORPROFILE original_profile{};
    const auto original_result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &original_profile);
    record_profile("Original thread profile", original_result, original_profile);
    require(original_result == S_OK, "Cannot preserve original input profile (S_FALSE means no active profile).");
    struct ProfileScope {
        ITfInputProcessorProfiles* languages;
        ITfInputProcessorProfileMgr* manager;
        TF_INPUTPROCESSORPROFILE original;
        std::ofstream& evidence;
        bool restored{};
        bool restore() {
            if (restored) return true;
            TF_INPUTPROCESSORPROFILE current{};
            const auto matches = [&] {
                return manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &current) == S_OK &&
                    current.dwProfileType == original.dwProfileType && current.langid == original.langid &&
                    IsEqualGUID(current.clsid, original.clsid) && IsEqualGUID(current.guidProfile, original.guidProfile) && current.hkl == original.hkl;
            };
            if (!matches()) {
                if (languages->ChangeCurrentLanguage(original.langid) != S_OK) return false;
                if (!matches() && manager->ActivateProfile(original.dwProfileType, original.langid, original.clsid, original.guidProfile,
                    original.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR ? nullptr : original.hkl, 0) != S_OK) return false;
            }
            restored = matches();
            evidence << "Original thread profile restore verified=" << restored << "; type=" << current.dwProfileType
                     << "; language=" << std::hex << current.langid << "; HKL=" << current.hkl << '\n'; evidence.flush();
            return restored;
        }
        ~ProfileScope() { if (!restore()) std::fputs("IME acceptance: restoring the original thread profile failed.\n", stderr); }
    } restore{languages.Get(), manager.Get(), original_profile, setup};
    if (original_profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR || original_profile.langid != 0x0804 ||
        !IsEqualGUID(original_profile.clsid, sogou) || !IsEqualGUID(original_profile.guidProfile, profile)) {
        const auto language_result = languages->ChangeCurrentLanguage(0x0804);
        setup << "ChangeCurrentLanguage(0804): HRESULT=" << std::hex << language_result << '\n'; setup.flush();
        require(language_result == S_OK, "Cannot select Chinese on the acceptance thread.");
        TF_INPUTPROCESSORPROFILE after_language{};
        const auto after_result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &after_language);
        record_profile("After language selection", after_result, after_language);
        if (after_result != S_OK || after_language.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR ||
            after_language.langid != 0x0804 || !IsEqualGUID(after_language.clsid, sogou) || !IsEqualGUID(after_language.guidProfile, profile)) {
            const auto activated = manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, sogou, profile, nullptr, 0);
            setup << "ActivateProfile(Sogou,flags=0): HRESULT=" << std::hex << activated << '\n'; setup.flush();
            require(activated == S_OK, "Cannot select the enabled Sogou profile on the acceptance thread.");
        }
        tick(app, 2);
    }
    const auto check_active_profile = [&] {
        TF_INPUTPROCESSORPROFILE current{};
        const auto result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &current);
        record_profile("Active thread profile", result, current);
        require(result == S_OK && current.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && current.langid == 0x0804 &&
            IsEqualGUID(current.clsid, sogou) && IsEqualGUID(current.guidProfile, profile), "Active input profile is not Sogou Pinyin.");
    };
    check_active_profile(); // Only this thread's enabled profile may change; no enablement or default setters.
    const auto record_input = [&](const char* phase) {
        const auto context = ImmGetContext(app.window());
        DWORD conversion{}, sentence{};
        const auto conversion_found = context && ImmGetConversionStatus(context, &conversion, &sentence);
        const auto stats = app.input_statistics();
        setup << phase << ": IMC=" << context << "; open=" << (context && ImmGetOpenStatus(context))
              << "; conversion-valid=" << conversion_found << "; conversion=" << std::hex << conversion
              << "; native-comp-bytes=" << std::dec << (context ? ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) : -1)
              << "; starts=" << stats.compositions << "; updates=" << stats.updates << "; results=" << stats.results
              << "; input=" << app.text_input().has_value() << "; composing=" << (app.text_input() && app.text_input()->composing)
              << "; query-units=" << app.query().size() << '\n';
        setup.flush();
        if (context) ImmReleaseContext(app.window(), context);
    };
    record_input("Before physical input");
    const auto verify_native_origin = [&] {
        const auto state = app.text_input();
        require(state && state->accepted_anchor, "No accepted insertion/composition origin for native query.");
        const auto& geometry = *state->accepted_anchor;
        IMECHARPOSITION position{}; position.dwSize = sizeof(position);
        require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&position)) != 0,
            "Native composition-origin query was rejected.");
        POINT expected{static_cast<LONG>(std::floor(geometry.bounds.x)), static_cast<LONG>(std::floor(geometry.bounds.y))};
        POINT document[2]{{geometry.clip.left, geometry.clip.top}, {geometry.clip.right, geometry.clip.bottom}};
        require(ClientToScreen(app.window(), &expected) && position.pt.x == expected.x && position.pt.y == expected.y &&
            position.cLineHeight == static_cast<UINT>(std::ceil(geometry.bounds.height)), "Native origin is not the accepted physical position in screen coordinates.");
        require(ClientToScreen(app.window(), &document[0]) && ClientToScreen(app.window(), &document[1]) &&
            position.rcDocument.left == document[0].x && position.rcDocument.top == document[0].y &&
            position.rcDocument.right == document[1].x && position.rcDocument.bottom == document[1].y,
            "Native text document is not the accepted field clip in screen coordinates.");
        position.dwCharPos = 1;
        require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&position)) == 0,
            "Unsupported composition offset reused the insertion origin.");
        position.dwCharPos = 0; position.dwSize = 0;
        require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&position)) == 0 &&
            SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, 0) == 0, "Invalid native geometry request was accepted.");
    };
    verify_native_origin();
    const auto capture_native_candidate = [&](const char* filename) {
        // Desktop pixels inside the foreground editor include the OS candidate popup.
        RECT rect{}; require(GetWindowRect(app.window(), &rect) != FALSE, "Cannot locate IME screenshot region.");
        const auto width = rect.right - rect.left, height = rect.bottom - rect.top;
        HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        struct CaptureScope {
            HDC screen, memory; HBITMAP bitmap;
            ~CaptureScope() { DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen); }
        } resources{screen, memory, bitmap};
        require(screen && memory && bitmap, "Cannot allocate native candidate capture.");
        const auto old = SelectObject(memory, bitmap);
        const auto copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
        SelectObject(memory, old);
        require(copied != FALSE, "Cannot capture native candidate pixels.");
        atelier::renderer::Capture image;
        image.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        require(GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), image.rgba.data(), &info, DIB_RGB_COLORS) == height,
            "Cannot read native candidate pixels.");
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) { std::swap(image.rgba[i], image.rgba[i + 2]); image.rgba[i + 3] = 255; }
        png(image, output / filename);
    };
    const auto physical_key = [&](WORD code, bool control = false) {
        require_foreground();
        for (const auto modifier : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
            require((GetAsyncKeyState(modifier) & 0x8000) == 0, "A physical modifier is held; IME acceptance stopped.");
        INPUT input[4]{};
        const UINT count = control ? 4 : 2;
        for (UINT index = 0; index < count; ++index) input[index].type = INPUT_KEYBOARD;
        const UINT first = control ? 1 : 0;
        input[first].ki.wScan = input[first + 1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(code, MAPVK_VK_TO_VSC));
        input[first].ki.dwFlags = KEYEVENTF_SCANCODE;
        input[first + 1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (control) {
            input[0].ki.wScan = input[3].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC));
            input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            input[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        }
        require(SendInput(count, input, sizeof(INPUT)) == count, "SendInput did not insert the full physical key sequence.");
        tick(app, 2);
    };
    const auto wait_for = [&](const auto& predicate, const char* message) {
        const auto deadline = GetTickCount64() + 5000;
        while (!predicate() && GetTickCount64() < deadline) {
            require_foreground();
            require(app.step(), "Editor closed during real IME acceptance.");
        }
        if (!predicate()) { record_input(message); capture_native_candidate("s2-ime-failure-native.png"); }
        require(predicate(), message);
    };
    const auto candidate_count = [&] {
        const auto context = ImmGetContext(app.window());
        if (!context) return DWORD{0};
        struct ContextScope { HWND window; HIMC context; ~ContextScope() { ImmReleaseContext(window, context); } } context_scope{app.window(), context};
        const auto bytes = ImmGetCandidateListW(context, 0, nullptr, 0);
        if (bytes < sizeof(CANDIDATELIST)) return DWORD{0};
        require(bytes <= 65536, "Native candidate state exceeded the inspection bound.");
        std::vector<DWORD> storage((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto* candidates = reinterpret_cast<CANDIDATELIST*>(storage.data());
        require(ImmGetCandidateListW(context, 0, candidates, bytes) == bytes, "Cannot inspect native candidate state.");
        return candidates->dwCount;
    };
    const auto compose_position = [&] {
        for (const auto code : {'W','E','I','Z','H','I'}) physical_key(static_cast<WORD>(code));
        wait_for([&] { return candidate_count() != 0; }, "System IME did not produce candidates.");
    };
    const auto native_before = app.input_statistics();
    compose_position();
    capture(app, output / "s2-ime-preedit.png");
    capture_native_candidate("s2-ime-native-candidate.png");
    physical_key(VK_SPACE); // The captured local Sogou candidate page shows position as item 1.
    wait_for([&] { return app.query() == L"位置" && app.text_input() && !app.text_input()->composing; }, "Pinyin candidate did not confirm exactly one position query.");
    require(entities(app).size() == 1, "IME confirmation triggered a scene command.");
    capture(app, output / "s2-ime-confirmed.png");
    const auto confirmed_results = app.input_statistics().results;
    physical_key('X');
    wait_for([&] { return candidate_count() != 0; }, "Sogou cancel scenario did not show candidates.");
    const auto reading_history = app.document().undo_count();
    physical_key('Z', true);
    require(app.query() == L"位置" && app.document().undo_count() == reading_history,
        "Ctrl+Z during native candidates changed the field or scene history.");
    if (!candidate_count()) { physical_key('X'); wait_for([&] { return candidate_count() != 0; }, "Sogou redo scenario did not show candidates."); }
    physical_key('Y', true);
    require(app.query() == L"位置" && app.document().undo_count() == reading_history,
        "Ctrl+Y during native candidates changed the field or scene history.");
    if (!candidate_count()) { physical_key('X'); wait_for([&] { return candidate_count() != 0; }, "Sogou Escape scenario did not show candidates."); }
    physical_key(VK_ESCAPE);
    wait_for([&] { return candidate_count() == 0 && app.text_input() && !app.text_input()->composing; }, "IME Escape did not close candidates.");
    require(app.query() == L"位置", "Cancelling IME also cancelled confirmed field text.");
    physical_key('A'); physical_key('B'); physical_key('C');
    wait_for([&] { return candidate_count() != 0; }, "Sogou Enter scenario did not show candidates.");
    physical_key(VK_RETURN);
    wait_for([&] { return app.input_statistics().results > confirmed_results && app.text_input() && !app.text_input()->composing; }, "IME Enter did not produce a confirmed result.");
    key(app, 'Z', true);
    require(app.query() == L"位置" && entities(app).size() == 1, "IME Enter also committed the field or reached scene history.");
    physical_key(VK_RETURN); // A new key sequence commits the search field.
    physical_key(VK_SPACE);
    const auto clean_space_query = app.query();
    require(clean_space_query.size() > 2 && clean_space_query.starts_with(L"位置"), "Clean-state Space did not insert text at the search caret.");
    physical_key(VK_ESCAPE);
    require(app.query() == L"位置", "Clean-state Space cancellation did not restore the committed query.");
    if (entities(app).empty()) click(app,control::add_static_mesh);
    const auto object = app.document().selection().entity;
    click(app, property_control(app, object, 0));
    compose_position(); physical_key(VK_SPACE); // The captured local Sogou candidate page shows position as item 1.
    wait_for([&] { return app.text_input() && !app.text_input()->composing; }, "Numeric Pinyin result did not end composition.");
    const auto history = app.document().undo_count();
    physical_key(VK_RETURN);
    require(x(app, object) == 0 && app.document().undo_count() == history && app.wants_keyboard(), "Invalid IME numeric draft wrote the scene or exited focus.");
    physical_key(VK_ESCAPE);
    require(x(app, object) == 0, "Numeric IME cancellation did not restore its value.");

    click(app, control::parameter_search); compose_position();
    SetFocus(nullptr);
    IMECHARPOSITION stale_origin{}; stale_origin.dwSize = sizeof(stale_origin);
    require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&stale_origin)) == 0,
        "Native focus loss retained an input-origin snapshot before UI delivery.");
    tick(app, 2);
    require(!app.text_input() && app.query() == L"位置", "OS blur did not cancel the real composition and retain the committed query.");
    require_foreground(); SetFocus(app.window()); tick(app, 1);
    click(app, control::parameter_search);
    physical_key(VK_SPACE);
    require(app.query() == clean_space_query, "Space after OS blur differed from the same IME's clean-state input.");
    physical_key(VK_ESCAPE);
    compose_position();
    RECT original_bounds{}; require(GetWindowRect(app.window(), &original_bounds) != FALSE, "Cannot preserve editor bounds.");
    SetWindowPos(app.window(), nullptr, 0, 0, original_bounds.right - original_bounds.left + 24,
        original_bounds.bottom - original_bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&stale_origin)) == 0,
        "Native surface change retained an input-origin snapshot before UI delivery.");
    tick(app, 3);
    require((!app.text_input() || !app.text_input()->composing) && app.query() == L"位置", "Surface change retained a real composition or changed the query.");
    SetWindowPos(app.window(), nullptr, 0, 0, original_bounds.right - original_bounds.left,
        original_bounds.bottom - original_bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    tick(app, 3);
    click(app, control::parameter_search);
    capture_native_candidate("s2-ime-surface-cleared.png");
    physical_key(VK_SPACE);
    require(app.query() == clean_space_query && x(app, object) == 0,
        "Space after a surface change differed from clean-state input or changed the object.");
    physical_key(VK_ESCAPE);
    click(app, property_control(app, object, 0));
    compose_position();
    require(!app.document().editing(), "Native reading began a numeric business edit before confirmation.");
    click(app, control::parameter_search);
    require(app.text_input() && app.text_input()->token.key == control::parameter_search,
        "Field-switch input target is not the search field.");
    physical_key(VK_SPACE);
    setup << "Field-switch expected UTF16:";
    for (const auto unit : clean_space_query) setup << ' ' << std::hex << static_cast<unsigned>(unit);
    setup << "\nField-switch actual UTF16:";
    for (const auto unit : app.query()) setup << ' ' << std::hex << static_cast<unsigned>(unit);
    setup << "\nOld object X=" << x(app, object) << "; business editing=" << app.document().editing() << '\n'; setup.flush();
    if (app.query() != clean_space_query || x(app, object) != 0 || app.document().editing())
        capture(app, output / "s2-ime-field-switch-failure.png");
    require(app.query() == clean_space_query, "Space after switching editable fields differed from clean-state input.");
    require(x(app, object) == 0, "Space after switching editable fields changed the old object's value.");
    require(!app.document().editing(), "Space in the search field retained a numeric business edit.");
    physical_key(VK_ESCAPE);
    click(app, control::add_static_mesh);
    const auto other = app.document().selection().entity;
    const auto other_x = x(app, other);
    click(app, app.object_control(object));
    click(app, property_control(app, object, 0)); compose_position();
    click(app, app.object_control(other));
    require(app.document().selection().entity == other && !app.document().editing() && x(app, object) == 0 &&
        x(app, other) == other_x && (!app.text_input() || !app.text_input()->composing),
        "Changing the parameter target retained composition or edited either object.");

    click(app, control::parameter_search);
    physical_key(VK_SPACE);
    require(app.query() == clean_space_query, "Space after changing the parameter target differed from clean-state input.");
    physical_key(VK_ESCAPE);
    const std::wstring long_query(100, L'x');
    type(app, long_query.c_str()); key(app, VK_RETURN); // Owned WM_CHAR only seeds the horizontal-scroll fixture.
    key(app, VK_END); compose_position();
    const auto text_state = app.text_input();
    require(text_state && candidate_count() != 0 && text_state->accepted_caret, "Long-field native candidates have no accepted caret.");
    const auto& caret = *text_state->accepted_caret;
    verify_native_origin();
    require(caret.bounds.x >= static_cast<float>(caret.clip.left) &&
        caret.bounds.x + caret.bounds.width <= static_cast<float>(caret.clip.right), "Scrolled composition caret escaped the field clip.");
    const auto input_context = ImmGetContext(app.window());
    require(input_context != nullptr, "Long-field composition lost its input context.");
    CANDIDATEFORM candidate{};
    const auto candidate_found = ImmGetCandidateWindow(input_context, 0, &candidate);
    ImmReleaseContext(app.window(), input_context);
    require(candidate_found && candidate.dwStyle == CFS_EXCLUDE &&
        candidate.rcArea.left == static_cast<LONG>(std::floor(caret.bounds.x)) &&
        candidate.rcArea.top == static_cast<LONG>(std::floor(caret.bounds.y)) &&
        candidate.rcArea.bottom == static_cast<LONG>(std::ceil(caret.bounds.y + caret.bounds.height)),
        "Native candidate anchor does not match the accepted scrolled caret.");
    capture(app, output / "s2-ime-horizontal.png"); capture_native_candidate("s2-ime-horizontal-native.png");
    physical_key(VK_ESCAPE);
    require(app.query() == long_query, "Cancelling scrolled composition changed confirmed text.");
    type(app, L""); key(app, VK_RETURN);
    const auto scene_history=app.document().undo_count();
    namespace sample=atelier::editor::workbench_control;
    click(app,control::workbench);
    click(app,sample::name); type(app,L""); key(app,VK_RETURN);
    compose_position(); verify_native_origin();
    physical_key(VK_SPACE);
    wait_for([&] { return app.control_workbench().values().name==L"位置" && app.text_input() && !app.text_input()->composing; },
        "Workbench IME Space did not confirm the selected candidate.");
    physical_key(VK_RETURN);
    compose_position();
    click(app,sample::disable);
    require(!app.text_input() && !app.presented()->find(sample::name)->enabled && app.control_workbench().values().name==L"位置",
        "Disabling the workbench retained native composition or changed confirmed text.");
    key(app,VK_SPACE);
    click(app,sample::name);
    compose_position();
    click(app,control::parameters_tab);
    require(!app.text_input() && app.control_workbench().values().name==L"位置" && app.document().undo_count()==scene_history,
        "Switching from workbench to scene retained composition or changed scene history.");
    click(app,control::workbench);
    click(app,sample::name);
    capture(app,output/"controls-ime-confirmed.png");
    click(app,control::parameters_tab);
    const auto native_after = app.input_statistics();
    capture(app, output / "s2-ime-final.png");
    check_active_profile();
    require(restore.restore(), "Cannot restore the original thread input profile.");
    TF_INPUTPROCESSORPROFILE restored_profile{};
    record_profile("Restored thread profile", manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &restored_profile), restored_profile);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(), WM_CLOSE, 0, 0); require(!app.step(), "IME acceptance did not close normally.");
    std::ofstream report(output / "ime-acceptance.txt");
    report << "Sogou Pinyin acceptance: PASS\n"
           << "Enabled profile 0804:{E7EA138E-69F8-11D7-A6EA-00065B844310}{E7EA138F-69F8-11D7-A6EA-00065B844311}\n"
           << "Physical SendInput scan codes -> system IME -> owned IMM32 snapshots -> shared TextEdit -> search/number\n"
           << "Composition starts: " << native_after.compositions - native_before.compositions
           << "; result snapshots: " << native_after.results - native_before.results
           << "; candidate opens: " << native_after.candidate_opens - native_before.candidate_opens << '\n'
           << "Candidate changes: " << native_after.candidate_changes - native_before.candidate_changes << '\n'
           << "Candidate/Space confirm/Escape cancel; physical Ctrl+Z/Y during candidates preserve field and scene history; IME Enter ownership; invalid numeric result; OS blur/surface/editable-field/target cancellation; horizontal composition/accepted candidate anchor; original thread profile restored; no enable/default/list setters used.\n"
           << "The long-field fixture uses owned WM_CHAR for its confirmed prefix; all IME compositions use physical SendInput and native IMM32 events.\n";
    report << "Control workbench: real candidate Space confirmation, accepted native anchor, foldout and scene-tab composition cancellation, sample/scene history isolation.\n";
    require(report.good(), "Cannot save real IME acceptance evidence.");
    std::puts("Sogou Pinyin acceptance: PASS"); return 0;
} catch (const std::exception& error) {
    const std::string_view reason(error.what());
    const auto outcome = reason.starts_with("IME acceptance lost foreground") ? "INTERRUPTED" :
        reason.find("not enabled") != std::string_view::npos ? "BLOCKED" : "FAIL";
    std::ofstream(std::filesystem::path(output_directory) / "ime-acceptance.txt")
        << "Sogou Pinyin acceptance: " << outcome << '\n' << error.what() << '\n';
    throw;
}

#include "sponza_window.inl"
#include "examples_window.inl"
#include "physics_examples_window.inl"
#include "dungeons_window.inl"

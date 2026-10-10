#include "application/application.hpp"

namespace atelier::application {
bool Application::viewport_input(const ui::Event& event,std::uint32_t native_key) {
    if(workspace_.menu || workspace_.dock.dragging() || !accepted_view_ || !pointer_ready()) return false;
    const auto& view=*accepted_view_;
    const bool pointer=event.kind==ui::EventKind::pointer_down || event.kind==ui::EventKind::pointer_up ||
        event.kind==ui::EventKind::pointer_move || event.kind==ui::EventKind::wheel;
    if(orbiting_) {
        if(event.kind==ui::EventKind::pointer_move) {
            if(panning_) camera_.pan(event.x-last_x_,event.y-last_y_,view.bounds.height);
            else camera_.orbit((event.x-last_x_)/window_.dpi_scale(),(event.y-last_y_)/window_.dpi_scale());
            last_x_=event.x;last_y_=event.y;return true;
        }
        if(event.kind==ui::EventKind::pointer_up && event.button==camera_button_) {
            orbiting_=false;window_.capture(false);window_.cursor(ui::Cursor::arrow);return true;
        }
        return pointer;
    }
    const bool inside=view.contains(pointer_position_.x,pointer_position_.y);
    if(!viewport_tool_.active() && !inside) return false;
    if(!viewport_tool_.active() && ui_.pointer_target(pointer_position_.x,pointer_position_.y)!=editor::control::viewport) return false;
    if(pointer && event.kind==ui::EventKind::pointer_down && !viewport_tool_.active()) {
        effects(ui_.cancel_interaction());effects(ui_.focus(0));workspace_.dock.select(editor::control::scene_tab);
        if(event.button==ui::PointerButton::middle || (event.button==ui::PointerButton::primary && event.alt)) {
            orbiting_=true;panning_=event.button==ui::PointerButton::middle && event.shift;camera_button_=event.button;
            last_x_=event.x;last_y_=event.y;window_.capture(true);window_.cursor(ui::Cursor::pan);return true;
        }
    }
    if(!pointer && !viewport_tool_.active() && ui_.wants_keyboard()) return false;
    if(!viewport_tool_.active() && event.kind==ui::EventKind::key_down && !event.control && !event.alt) {
        if (event.repeat) return true; // A held shortcut must not restart after its modal owner dismisses.
        if (event.shift && native_key=='D') {
            effects(ui_.cancel_interaction());effects(ui_.focus(0));workspace_.dock.select(editor::control::scene_tab);
            const auto duplicated=execute(editor::Command::duplicate_selected);
            if (!duplicated) {status(duplicated,L"");return true;}
            if (!document_->selected_actors().empty())
                status(viewport_tool_.place_duplicates(*document_,view,workspace_.viewport_settings,pointer_position_),L"Duplicate · Move · Right click / Esc keeps copies in place");
            window_.capture(viewport_tool_.capture());return true;
        }
        if (!event.shift && native_key=='X') {
            if (!document_->selected_actors().empty())
                request_menu({editor::control::viewport,editor::control::viewport_delete_menu,pointer_position_.x,pointer_position_.y});
            return true;
        }
        if(native_key=='W') {
            effects(ui_.cancel_interaction());effects(ui_.focus(0));workspace_.dock.select(editor::control::scene_tab);
            request_menu({editor::control::viewport,0,pointer_position_.x,pointer_position_.y});return true;
        }
        if(event.key==ui::Key::tab && event.shift) {status(execute(editor::Command::snap_toggle),L"Grid snapping toggled");return true;}
        // Blender navigation keys while the pointer is over the scene.
        using enum editor::Command;
        const auto view=[&](editor::Command command,const wchar_t* text){status(execute(command),text);return true;};
        if(native_key==VK_NUMPAD1 && !event.shift) return view(view_front,L"Front view");
        if(native_key==VK_NUMPAD3 && !event.shift) return view(view_right,L"Right view");
        if(native_key==VK_NUMPAD7 && !event.shift) return view(view_top,L"Top view");
        if((native_key==VK_NUMPAD5 || native_key==VK_NUMPAD0) && !event.shift) return view(view_perspective,L"Perspective view");
        if(native_key==VK_DECIMAL && !event.shift) {status(frame_actors(true),L"View Selected");return true;}
        if(native_key==VK_HOME && !event.shift) {status(frame_actors(false),L"View All");return true;}
        // Shift+S follows the editor's requested Shading pie binding. Snapping
        // remains in the magnet menu and Shift+Tab.
        if(event.shift && (native_key=='A' || native_key=='S')) {
            effects(ui_.cancel_interaction());effects(ui_.focus(0));workspace_.dock.select(editor::control::scene_tab);
            // The scene is the anchor; group selects which pointer menu to build.
            request_menu({editor::control::viewport,native_key=='A'?editor::control::viewport_add_menu:editor::control::viewport_shading_pie,pointer_position_.x,pointer_position_.y});
            return true;
        }
    }
    if(viewport_tool_.active() && event.kind==ui::EventKind::key_down && event.control && (native_key=='Z' || native_key=='Y')) {
        auto cancelled=viewport_tool_.cancel(*document_);
        window_.capture(false);
        if(!cancelled) status(cancelled,L"");
        else status(execute(native_key=='Y' || event.shift?editor::Command::redo:editor::Command::undo),L"History updated");
        return true;
    }
    if(!pointer && !viewport_tool_.active() && event.kind==ui::EventKind::key_down && native_key=='Q' && !event.control && !event.alt) {
        status(execute(editor::Command::tool_select),L"Select · Right click selects · Left click places the cursor");return true;
    }
    if(pointer && event.kind==ui::EventKind::wheel && !viewport_tool_.active()) {camera_.zoom(event.wheel);return true;}
    const auto result=pointer?viewport_tool_.pointer(*document_,view,event,workspace_.viewport_settings):
        viewport_tool_.key(*document_,view,event,native_key,workspace_.viewport_settings,pointer_position_);
    if(!result) {status(Result<void>{std::unexpected(result.error())},L"");return true;}
    if(*result) {
        workspace_.inspecting_asset=false;
        window_.capture(viewport_tool_.capture());
        workspace_.status=viewport_tool_.hint().empty()?L"Viewport selection / transform updated":std::wstring(viewport_tool_.hint());
    }
    if(pointer) window_.cursor(viewport_tool_.pointer_cursor());
    // A viewport hover owns its pointer so generic UI hover cannot erase handle feedback.
    return *result || (pointer && inside && event.kind==ui::EventKind::pointer_move);
}
}

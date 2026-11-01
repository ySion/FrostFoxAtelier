#include "application/application.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::application {
Result<void> Application::place_duplicates() {
    if (!accepted_view_)
        return std::unexpected(
            Error{ErrorCode::unsupported_operation, "Wait for an accepted viewport before placing copies."});
    cancel_interaction(ui_.cancel_interaction());
    effects(ui_.focus(0));
    workspace_.dock.select(editor::control::scene_tab);
    auto placed =
        viewport_tool_.place_duplicates(*document_, *accepted_view_, workspace_.viewport_settings, pointer_position_);
    window_.capture(viewport_tool_.capture());
    window_.cursor(viewport_tool_.pointer_cursor());
    if (placed)
        workspace_.status = L"Duplicate · Move · Right click / Esc keeps copies in place";
    return placed;
}
void Application::show_menu(editor::MenuKind menu) {
    using namespace editor::control;
    if (menu == editor::MenuKind::delete_selection && document_->selected_entities().empty())
        return;
    constexpr std::array groups{ui::ControlKey{}, viewport_delete_menu, viewport_shading_pie, viewport_add_menu,
                                viewport_axis_pie};
    effects(ui_.focus(0));
    workspace_.dock.select(scene_tab);
    request_menu({viewport, groups[static_cast<std::size_t>(menu)], pointer_position_.x, pointer_position_.y});
}
void Application::finish_camera_fly(bool accept) {
    if (!camera_fly_)
        return;
    const auto entry = camera_fly_->entry;
    camera_fly_.reset(); // Retire modal ownership before ReleaseCapture can call back.
    if (!accept) {
        camera_ = entry;
        camera_cut_ = true;
    }
    window_.relative_pointer(false);
    window_.capture(false);
    window_.cursor(ui::Cursor::arrow);
    workspace_.status = accept ? L"Fly navigation confirmed" : L"Fly navigation cancelled";
}
void Application::look_camera_fly(float dx, float dy) {
    if (!camera_fly_)
        return;
    auto camera = camera_;
    camera.look(dx / window_.dpi_scale(), dy / window_.dpi_scale());
    if (auto result = set_camera(camera, false); !result)
        status(result, L"");
}
void Application::update_camera_fly() {
    if (!camera_fly_)
        return;
    if (workspace_.menu || !workspace_.dock.visible(editor::control::scene_tab) || GetCapture() != window() ||
        GetForegroundWindow() != window() || GetFocus() != window()) {
        finish_camera_fly(false);
        return;
    }
    auto& fly = *camera_fly_;
    const auto now = std::chrono::steady_clock::now();
    // A stalled render/modal OS loop must never teleport the camera on resume.
    const auto seconds = std::clamp(std::chrono::duration<float>(now - fly.previous).count(), 0.f, .1f);
    fly.previous = now;
    const auto& held = fly.movement;
    const math::Vector3 direction{float(held[3]) - float(held[2]), float(held[5]) - float(held[4]),
                                  float(held[0]) - float(held[1])};
    const auto normalized = math::try_normalize(direction);
    if (!normalized)
        return;
    auto camera = camera_;
    camera.fly(*normalized * (seconds * fly.speed * (fly.fast ? 4.f : 1.f)));
    if (auto result = set_camera(camera, false); !result)
        status(result, L"");
}

bool Application::viewport_input(const ui::Event& event, std::uint32_t native_key) {
    if (camera_fly_) {
        if (event.kind == ui::EventKind::key_down && (event.key == ui::Key::escape || event.key == ui::Key::enter)) {
            finish_camera_fly(event.key == ui::Key::enter);
            return true;
        }
        if (event.kind == ui::EventKind::pointer_down &&
            (event.button == ui::PointerButton::primary || event.button == ui::PointerButton::secondary)) {
            finish_camera_fly(event.button == ui::PointerButton::primary);
            camera_fly_release_ = event.button;
            return true;
        }
        if (event.kind == ui::EventKind::wheel)
            camera_fly_->speed = std::clamp(camera_fly_->speed * std::exp(event.wheel * .2f), .05f, 1000.f);
        if (event.kind == ui::EventKind::key_down || event.kind == ui::EventKind::key_up) {
            constexpr std::array<std::uint32_t, 6> keys{'W', 'S', 'A', 'D', 'Q', 'E'};
            const auto found = std::ranges::find(keys, native_key);
            if (found != keys.end())
                camera_fly_->movement[found - keys.begin()] = event.kind == ui::EventKind::key_down;
            camera_fly_->fast = event.shift;
        }
        return true; // This modal owner consumes text, shortcuts and button releases too.
    }
    // A UI press owns its whole gesture, including movement across the viewport.
    if (workspace_.menu || workspace_.dock.dragging() || ui_.interaction_state().pointer || !accepted_view_ ||
        !pointer_ready())
        return false;
    const auto& view = *accepted_view_;
    const bool pointer = event.kind == ui::EventKind::pointer_down || event.kind == ui::EventKind::pointer_up ||
                         event.kind == ui::EventKind::pointer_move || event.kind == ui::EventKind::wheel;
    if (camera_drag_) {
        auto& drag = *camera_drag_;
        if (event.kind == ui::EventKind::pointer_move) {
            auto camera = camera_;
            if (drag.pan)
                camera.pan(event.x - drag.previous.x, event.y - drag.previous.y, view.bounds.height);
            else
                camera.orbit((event.x - drag.previous.x) / window_.dpi_scale(),
                             (event.y - drag.previous.y) / window_.dpi_scale());
            if (auto changed = set_camera(camera, false); !changed)
                status(changed, L"");
            drag.previous = {event.x, event.y};
            return true;
        }
        if (event.kind == ui::EventKind::pointer_up && event.button == drag.button) {
            camera_drag_.reset();
            window_.capture(false);
            window_.cursor(ui::Cursor::arrow);
            return true;
        }
        return pointer;
    }
    const bool inside = view.contains(pointer_position_.x, pointer_position_.y);
    if (!viewport_tool_.active() && !inside)
        return false;
    if (!viewport_tool_.active() &&
        ui_.pointer_target(pointer_position_.x, pointer_position_.y) != editor::control::viewport)
        return false;
    if (pointer && event.kind == ui::EventKind::pointer_down && !viewport_tool_.active()) {
        cancel_interaction(ui_.cancel_interaction());
        effects(ui_.focus(0));
        workspace_.dock.select(editor::control::scene_tab);
        if (event.button == ui::PointerButton::middle || (event.button == ui::PointerButton::primary && event.alt)) {
            camera_drag_ =
                CameraDrag{event.button, event.button == ui::PointerButton::middle && event.shift, {event.x, event.y}};
            window_.capture(true);
            window_.cursor(camera_drag_->pan ? ui::Cursor::pan : ui::Cursor::orbit);
            return true;
        }
    }
    if (!pointer && !viewport_tool_.active() && ui_.wants_keyboard())
        return false;
    if (!viewport_tool_.active() &&
        key_input(event, native_key, editor::RegionKind::viewport, editor::KeyPhase::before_controls))
        return true;
    if (!viewport_tool_.active() && event.kind == ui::EventKind::key_down && !event.control && !event.alt) {
        if (event.repeat)
            return true; // A held shortcut must not restart after its modal owner dismisses.
        if (native_key == VK_OEM_3 && event.shift) {
            cancel_interaction(ui_.cancel_interaction());
            effects(ui_.focus(0));
            workspace_.dock.select(editor::control::scene_tab);
            const auto entry = camera_;
            auto camera = camera_;
            camera.look(0, 0);
            const auto changed = set_camera(camera, false);
            if (!changed) {
                status(changed, L"");
                return true;
            }
            if (!window_.relative_pointer(true)) {
                camera_ = entry;
                workspace_.status = L"Cannot capture the mouse for fly navigation";
                return true;
            }
            camera_fly_ = CameraFly{.entry = entry, .fast = event.shift, .previous = std::chrono::steady_clock::now()};
            workspace_.status =
                L"Fly · WASD move · Q/E down/up · Wheel speed · Shift fast · Enter/LMB confirm · Esc/RMB cancel";
            return true;
        }
    }
    if (viewport_tool_.active() && event.kind == ui::EventKind::key_down && event.control &&
        (native_key == 'Z' || native_key == 'Y')) {
        auto cancelled = viewport_tool_.cancel(*document_);
        window_.capture(false);
        window_.cursor(ui::Cursor::arrow);
        if (!cancelled)
            status(cancelled, L"");
        else
            key_input(event, native_key, editor::RegionKind::none, editor::KeyPhase::after_controls);
        return true;
    }
    if (pointer && event.kind == ui::EventKind::wheel && !viewport_tool_.active()) {
        camera_.zoom(event.wheel);
        return true;
    }
    const auto result = pointer ? viewport_tool_.pointer(*document_, view, event, workspace_.viewport_settings)
                                : viewport_tool_.key(*document_, view, event, native_key, workspace_.viewport_settings,
                                                     pointer_position_);
    if (!result) {
        status(Result<void>{std::unexpected(result.error())}, L"");
        return true;
    }
    if (*result) {
        workspace_.inspecting_asset = false;
        window_.capture(viewport_tool_.capture());
        workspace_.status = viewport_tool_.hint().empty() ? L"Viewport selection / transform updated"
                                                          : std::wstring(viewport_tool_.hint());
    }
    if (pointer || *result)
        window_.cursor(viewport_tool_.pointer_cursor());
    // A viewport hover owns its pointer so generic UI hover cannot erase handle feedback.
    return *result || (pointer && inside && event.kind == ui::EventKind::pointer_move);
}
}

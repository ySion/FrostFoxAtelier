#pragma once

#include "editor/camera.hpp"
#include "editor/operator_parameters.hpp"
#include "editor/session.hpp"
#include "renderer/overlay.hpp"
#include "ui/session.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace atelier::editor {
enum class SelectAll : std::uint8_t { select, deselect, toggle, invert };
enum class TransformChannel : std::uint8_t { location, rotation, scale };

// XYZ Euler angles in degrees, applied X then Y then Z (Houdini "Rx Ry Rz").
math::Vector3 euler_degrees(math::Quaternion) noexcept;
math::Quaternion from_euler_degrees(math::Vector3) noexcept;

// Camera snapshot for one accepted scene viewport, in physical pixels.
struct ViewportView {
    ui::Rect bounds;
    float dpi_scale{1};
    math::Matrix4 view, projection;
    math::Matrix4d inverse; // view * projection, inverted.
    math::Vector3 eye, right, up, forward;
    math::Vector3 construction_normal{0,1,0};
    bool orthographic{};
    static ViewportView make(const Camera&, ui::Rect bounds, float dpi_scale);
    bool contains(float x, float y) const noexcept;
    // Screen position; empty outside the camera's near/far depth interval.
    std::optional<math::Vector2> project(math::Vector3) const noexcept;
    struct Ray { math::Vector3 origin, direction; };
    Ray ray(float x, float y) const noexcept;
    float pixel_size(math::Vector3 at) const noexcept; // Metres per physical pixel at that depth.
};

// Viewport editing: click/box selection, gizmo handle drags, Blender-style
// modal G/R/S with axis constraints and typed values, and the 3D cursor.
// Every change goes through Session transactions; one gesture is one undo.
class ViewportTool final {
public:
    math::Vector3 cursor{}; // Blender-style 3D cursor: pivot option and creation point.
    // The cursor is a full 3D frame like Blender's: surface placement aligns its
    // local Z with the hit normal; the drawn ticks are its X/Y axes in 3D.
    math::Quaternion cursor_rotation{};

    bool active() const noexcept { return gesture_.has_value() || marquee_.has_value(); }
    bool modal() const noexcept { return gesture_ && gesture_->modal; }
    std::wstring_view hint() const noexcept { return hint_; }
    // Live modal readout (Blender's header line); empty outside a modal gesture.
    std::wstring_view readout() const noexcept { return gesture_ && gesture_->modal?std::wstring_view(hint_):std::wstring_view{}; }
    TransformTool displayed_tool(ToolSettings) const noexcept;
    Result<void> snap_selection(Session&,ToolSettings,bool to_cursor);
    Result<void> cursor_to_selection(const Session&);
    Result<void> place_duplicates(Session&, const ViewportView&, ToolSettings, math::Vector2 pointer);
    void cursor_to_grid(ToolSettings) noexcept;
    Result<void> select_all(Session&,SelectAll);
    Result<void> clear(Session& session,TransformChannel channel) {
        return clear_channel(session,channel==TransformChannel::location?Operation::move:channel==TransformChannel::rotation?Operation::rotate:Operation::scale);
    }
    void arm_box_select(math::Vector2 pointer,bool extend);
    ui::Cursor pointer_cursor() const noexcept;
    // Pointer events inside the viewport, or any pointer event while active().
    // Returns true when consumed; capture() reports whether the host should hold mouse capture.
    Result<bool> pointer(Session&, const ViewportView&, const ui::Event&, ToolSettings);
    // Viewport hotkeys (host checks pointer hover and text focus). vk is the native virtual key.
    Result<bool> key(Session&, const ViewportView&, const ui::Event&, std::uint32_t vk, ToolSettings,
                     math::Vector2 pointer);
    bool capture() const noexcept { return active(); }
    // Ends any gesture: restores previews through Session::cancel_edit.
    Result<void> cancel(Session&);
    // Drop tool state after Session already ended the transaction (undo, selection change...).
    void forget(const Session&) noexcept;
    // Geometry membership comes from the same frame snapshot as scene rendering.
    void overlay(const Session&, const ViewportView&, ToolSettings, std::span<const ObjectId> mesh_entities,
        std::vector<renderer::OverlayShape>&) const;
    // Topmost Entity under a physical-pixel point.
    std::optional<ObjectId> pick(const Session&, const ViewportView&, float x, float y) const;
    std::vector<ObjectId> pick_box(const Session&, const ViewportView&, ui::Rect) const;

    enum class Handle : std::uint8_t {
        none, move_x, move_y, move_z, plane_yz, plane_xz, plane_xy, move_view,
        rotate_x, rotate_y, rotate_z, rotate_view, scale_x, scale_y, scale_z, scale_uniform
    };
    Handle hovered() const noexcept { return hover_; }
private:
    enum class Operation : std::uint8_t { move, rotate, scale };
    struct Targets {
        std::vector<ObjectId> entities;
        std::vector<world::Trs> transforms;
        world::Trs active_frame;
        math::Vector3 pivot;
    };
    struct Gesture {
        Operation operation{};
        Handle handle{};
        bool modal{}, valid{true};
        ToolSettings settings;
        int axis{-1};       // Constraint axis, or -1.
        bool plane{};       // Constraint is the plane normal to axis.
        std::uint64_t session{};
        std::vector<ObjectId> entities;
        std::vector<world::Trs> before;
        world::Trs active_frame;
        math::Vector3 pivot;
        math::Vector2 start, last;
        float angle{};      // Unwrapped screen angle for rotation.
        std::wstring typed; // Numeric input.
        Orientation orientation() const noexcept { return operation==Operation::scale?Orientation::local:settings.orientation; }
        math::Quaternion basis() const noexcept;
    };
    struct Marquee {
        math::Vector2 start, current; bool dragging{}; bool shift{}, control{}, await_press{};
        ui::PointerButton button{ui::PointerButton::primary};
    };
    std::optional<Gesture> gesture_;
    std::optional<Marquee> marquee_;
    Handle hover_{Handle::none};
    std::wstring hint_;
    std::uint64_t next_session_{(1ULL<<62)+1};

    static Result<void> preview_world(Session&,std::uint64_t,std::span<const ObjectId>,std::span<const world::Trs>,bool local_scale=false);
    static std::optional<Targets> targets(const Session&, PivotMode, math::Vector3 cursor);
    struct Hit { ObjectId entity; math::Vector3 position, normal{0,1,0}; };
    static std::optional<Hit> trace(const Session&,const ViewportView&,float x,float y,bool locators);
    Result<void> place_cursor(const Session&,const ViewportView&,math::Vector2,ToolSettings,bool control);
    Handle hit(const ViewportView&, const Targets&, TransformTool, math::Vector2, Orientation) const;
    Result<void> begin(Session&, const ViewportView&, ToolSettings, Operation, Handle, bool modal, math::Vector2, bool amend_creation = false);
    Result<void> update(Session&, const ViewportView&, math::Vector2, bool snap, bool fine=false);
    Result<void> finish(Session&);
    Result<void> clear_channel(Session&, Operation);
    Result<void> select_click(Session&, const ViewportView&, math::Vector2, ToolSettings, bool shift, bool control);
    Result<void> select_box(Session&, const ViewportView&, ToolSettings, const Marquee&);
    static Result<void> combine(Session&, std::span<const ObjectId> hits, ObjectId active, SelectMode);
};
}

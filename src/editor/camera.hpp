#pragma once

#include "editor/operator_parameters.hpp"
#include "math/math.hpp"
#include <string_view>

namespace atelier::presentation {
struct CameraSnapshot;
}
namespace atelier::editor {
constexpr std::wstring_view camera_view_name(CameraView view) noexcept {
    switch (view) {
    case CameraView::perspective:
        return L"Persp";
    case CameraView::top:
        return L"Top";
    case CameraView::front:
        return L"Front";
    case CameraView::right:
        return L"Right";
    case CameraView::bottom:
        return L"Bottom";
    case CameraView::back:
        return L"Back";
    case CameraView::left:
        return L"Left";
    }
    return {};
}
struct CameraMatrices {
    math::Matrix4 view;
    math::Matrix4 projection;
    float far_depth{}; // Derived finite clipping boundary, separate from background depth zero.
};
// Left-handed, metres, +Y up; row-major matrices and row vectors.
class Camera final {
public:
    Camera() { reset(); }
    void reset() noexcept;
    void set_view(CameraView) noexcept;
    CameraView view() const noexcept { return view_; }
    void focus(math::Vector3) noexcept;
    bool look_at(math::Vector3 eye, math::Vector3 target) noexcept;
    // A one-shot view, including roll and projection, not a binding to the scene.
    bool view_from(const presentation::CameraSnapshot&) noexcept;
    // Frame a world-space sphere so it fills the view (Blender View Selected / View All).
    void frame(math::Vector3 center, float radius) noexcept;
    void orbit(float logical_dx, float logical_dy) noexcept;
    void zoom(float wheel_steps) noexcept;
    // First-person navigation retains eye position while looking; movement uses metres.
    void look(float logical_dx, float logical_dy) noexcept;
    void fly(math::Vector3 local_metres) noexcept;
    // Moves the target in the view plane so the grabbed point follows the pointer.
    void pan(float pixel_dx, float pixel_dy, float viewport_pixel_height) noexcept;
    math::Vector3 target() const noexcept { return target_; }
    math::Vector3 eye() const noexcept;
    math::Vector3 up_hint() const noexcept;
    math::Vector3 construction_normal() const noexcept;
    CameraMatrices matrices(float aspect) const noexcept;
    CameraState state() const noexcept {
        return {view_, target_, yaw_, pitch_, distance_, roll_, vertical_fov_, near_clip_, far_clip_};
    }
    bool set_state(const CameraState&) noexcept;

private:
    CameraView view_{CameraView::perspective};
    math::Vector3 target_;
    float yaw_{}, pitch_{}, distance_{};
    float roll_{}, vertical_fov_{45}, near_clip_{.05f}, far_clip_{30000};
};
}

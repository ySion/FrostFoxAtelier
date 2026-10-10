#pragma once

#include "math/math.hpp"

namespace atelier::editor {
enum class CameraView : std::uint8_t { perspective, top, front, right };
struct CameraMatrices {
    math::Matrix4 view;
    math::Matrix4 projection;
};
struct CameraState {
    CameraView view{CameraView::perspective};
    math::Vector3 target{};
    float yaw{}, pitch{}, distance{};
    bool operator==(const CameraState&) const = default;
};
// Left-handed, metres, +Y up; row-major matrices and row vectors.
class Camera final {
public:
    static constexpr float field_of_view = 0.785398163f; // Vertical, radians.
    Camera() { reset(); }
    void reset() noexcept;
    void set_view(CameraView) noexcept;
    CameraView view() const noexcept { return view_; }
    void focus(math::Vector3) noexcept;
    bool look_at(math::Vector3 eye, math::Vector3 target) noexcept;
    // Frame a world-space sphere so it fills the view (Blender View Selected / View All).
    void frame(math::Vector3 center, float radius) noexcept;
    void orbit(float logical_dx, float logical_dy) noexcept;
    void zoom(float wheel_steps) noexcept;
    // Moves the target in the view plane so the grabbed point follows the pointer.
    void pan(float pixel_dx, float pixel_dy, float viewport_pixel_height) noexcept;
    math::Vector3 target() const noexcept { return target_; }
    math::Vector3 eye() const noexcept;
    CameraMatrices matrices(float aspect) const noexcept;
    CameraState state() const noexcept { return {view_, target_, yaw_, pitch_, distance_}; }
    bool set_state(const CameraState&) noexcept;
private:
    CameraView view_{CameraView::perspective};
    math::Vector3 target_;
    float yaw_{}, pitch_{}, distance_{};
};
}

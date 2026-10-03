#pragma once

#include "math/math.hpp"

namespace atelier::editor {
struct CameraMatrices {
    math::Matrix4 view;
    math::Matrix4 projection;
};
// Left-handed, metres, +Y up; row-major matrices and row vectors.
class Camera final {
public:
    Camera() { reset(); }
    void reset() noexcept;
    void focus(math::Vector3) noexcept;
    void orbit(float logical_dx, float logical_dy) noexcept;
    void zoom(float wheel_steps) noexcept;
    CameraMatrices matrices(float aspect) const noexcept;
private:
    math::Vector3 target_;
    float yaw_{}, pitch_{}, distance_{};
};
}

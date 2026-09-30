#include "editor/camera.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace atelier::editor {
void Camera::reset() noexcept {
    target_ = {};
    distance_ = std::sqrt(125.f);
    yaw_ = std::atan2(6.f, -8.f);
    pitch_ = std::asin(5.f / distance_);
}
void Camera::focus(world::Vec3 target) noexcept { target_ = target; distance_ = 5.f; }
void Camera::orbit(float dx, float dy) noexcept {
    yaw_ -= dx * .006f;
    pitch_ = std::clamp(pitch_ + dy * .006f, -1.45f, 1.45f);
}
void Camera::zoom(float steps) noexcept { distance_ = std::clamp(distance_ * std::exp(-steps * .12f), .5f, 2000.f); }
CameraMatrices Camera::matrices(float aspect) const noexcept {
    using namespace DirectX;
    const auto eye = XMVectorSet(target_.x + distance_ * std::cos(pitch_) * std::sin(yaw_),
        target_.y + distance_ * std::sin(pitch_), target_.z + distance_ * std::cos(pitch_) * std::cos(yaw_), 1.f);
    const auto target = XMVectorSet(target_.x, target_.y, target_.z, 1.f);
    XMFLOAT4X4 view, projection;
    XMStoreFloat4x4(&view, XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat4x4(&projection, XMMatrixPerspectiveFovLH(XM_PIDIV4, std::max(aspect, .01f), .05f, 30000.f));
    CameraMatrices result;
    static_assert(sizeof(view) == sizeof(result.view));
    std::memcpy(result.view.data(), &view, sizeof(view));
    std::memcpy(result.projection.data(), &projection, sizeof(projection));
    return result;
}
}

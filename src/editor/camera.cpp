#include "editor/camera.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace atelier::editor {
void Camera::reset() noexcept {
    view_=CameraView::perspective;
    target_ = {};
    distance_ = std::sqrt(125.f);
    yaw_ = std::atan2(6.f, -8.f);
    pitch_ = std::asin(5.f / distance_);
}
void Camera::set_view(CameraView view) noexcept {
    view_=view;
    if(view==CameraView::perspective) {yaw_=std::atan2(6.f,-8.f);pitch_=std::asin(5.f/std::sqrt(125.f));}
    else {yaw_=view==CameraView::right?DirectX::XM_PIDIV2:DirectX::XM_PI;pitch_=view==CameraView::top?DirectX::XM_PIDIV2:0;}
}
void Camera::focus(math::Vector3 target) noexcept { target_ = target; distance_ = 5.f; }
bool Camera::look_at(math::Vector3 eye, math::Vector3 target) noexcept {
    const double dx=double(eye.x)-target.x,dy=double(eye.y)-target.y,dz=double(eye.z)-target.z;
    const auto distance=std::hypot(dx,dy,dz);
    if(!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) ||
        !std::isfinite(distance) || distance<.5 || distance>2000 || std::hypot(dx,dz)<1e-6) return false;
    view_=CameraView::perspective;target_=target;distance_=static_cast<float>(distance);yaw_=static_cast<float>(std::atan2(dx,dz));
    pitch_=static_cast<float>(std::asin(dy/distance));return true;
}
void Camera::frame(math::Vector3 center, float radius) noexcept {
    target_ = center;
    // Fit the sphere to the vertical field with a small margin; orthographic views share the distance.
    distance_ = std::clamp(std::max(radius, .05f) * 1.15f / std::sin(field_of_view * .5f), .5f, 2000.f);
}
void Camera::orbit(float dx, float dy) noexcept {
    view_=CameraView::perspective;
    yaw_ -= dx * .006f;
    pitch_ = std::clamp(pitch_ + dy * .006f, -1.45f, 1.45f);
}
void Camera::zoom(float steps) noexcept { distance_ = std::clamp(distance_ * std::exp(-steps * .12f), .5f, 2000.f); }
math::Vector3 Camera::eye() const noexcept {
    return {target_.x + distance_ * std::cos(pitch_) * std::sin(yaw_), target_.y + distance_ * std::sin(pitch_),
        target_.z + distance_ * std::cos(pitch_) * std::cos(yaw_)};
}
void Camera::pan(float dx, float dy, float height) noexcept {
    if (!(height > 0)) return;
    const auto forward = math::try_normalize(target_ - eye());
    if (!forward) return;
    const auto right = math::try_normalize(math::cross(view_==CameraView::top?math::Vector3{0,0,1}:math::Vector3{0,1,0}, *forward));
    if (!right) return;
    const auto up = math::cross(*forward, *right);
    // One pixel spans this many metres on the target plane.
    const float metres = 2 * distance_ * std::tan(field_of_view * .5f) / height;
    target_ -= *right * (dx * metres);
    target_ += up * (dy * metres);
}
CameraMatrices Camera::matrices(float aspect) const noexcept {
    using namespace DirectX;
    const auto position = eye();
    const auto eye_vector = XMVectorSet(position.x, position.y, position.z, 1.f);
    const auto target = XMVectorSet(target_.x, target_.y, target_.z, 1.f);
    XMFLOAT4X4 view, projection;
    XMStoreFloat4x4(&view, XMMatrixLookAtLH(eye_vector, target, view_==CameraView::top?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0)));
    const float height=2*distance_*std::tan(field_of_view*.5f);
    XMStoreFloat4x4(&projection, view_==CameraView::perspective?
        XMMatrixPerspectiveFovLH(field_of_view,std::max(aspect,.01f),.05f,30000.f):
        XMMatrixOrthographicLH(height*std::max(aspect,.01f),height,.05f,30000.f));
    CameraMatrices result;
    static_assert(sizeof(view) == sizeof(result.view));
    std::memcpy(result.view.data(), &view, sizeof(view));
    std::memcpy(result.projection.data(), &projection, sizeof(projection));
    return result;
}
}

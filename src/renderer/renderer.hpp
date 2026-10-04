#pragma once

#include "rhi/rhi.hpp"
#include "geometry/mesh.hpp"
#include "ui/frame.hpp"
#include "renderer/view_settings.hpp"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace atelier::renderer {

struct Error {
    rhi::Error cause;
    // Once encoding/uploads start, failure permits only close/destruction.
    bool terminal{};
};
template <class T> using Result = std::expected<T, Error>;

struct Options {
    rhi::Validation validation{rhi::Validation::disabled};
    std::uint32_t wait_timeout_ms{10000};
    bool vsync{true};
};
struct Target {
    ui::PixelExtent extent;
    bool drawable{};
};
// Left-handed, +Y up, meters, D3D depth [0,1]. Row-major storage and row
// vectors: p * world * view * projection (LookAtLH / PerspectiveFovLH).
struct Camera {
    std::array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 16> projection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
struct MeshInstance {
    // Producer-evaluated local-to-world affine matrix. Same row-major storage
    // and row-vector convention as Camera; hierarchy evaluation stays outside
    // the renderer. Geometry is an immutable, shareable triangle resource.
    // Non-finite, non-affine, singular or GPU-unrepresentable normal transforms
    // reject the entire frame before encoding. Invertible reflected transforms are supported by this boundary.
    std::array<float, 16> world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ui::Color color{0.2f, 0.45f, 0.75f, 1};
    bool selected{};
    geometry::MeshRef geometry;
};
struct Frame {
    const ui::Frame &ui_frame; // Sole authoritative id/extent; borrowed to return.
    Camera camera;
    ui::Rect scene_viewport;     // Physical pixels, top-left origin.
    std::span<const MeshInstance> meshes; // Borrowed draw snapshots; no business object ownership.
    ViewSettings view{};
};
enum class FrameStatus { accepted, skipped };
// Activity from this successful render return (accepted or skipped). Counts
// encoded commands, including an occluded Present; pre-encoding skips have zero
// activity. These are not GPU timings. Errors provide no complete statistics.
struct FrameStats {
    std::uint64_t ui_draw_calls{};
    std::uint64_t ui_quads{}; // Visible instances; adjacent compatible quads share a draw.
    std::uint64_t ui_instance_upload_bytes{};
    std::uint64_t scene_draw_calls{};
    std::uint64_t viewport_draw_calls{}; // Background and reference guides; excludes scene geometry.
    std::uint64_t coverage_upload_count{};
    std::uint64_t coverage_upload_bytes{}; // Tight R8 payload; excludes copy padding/scratch.
    // Current renderer cache only, excluding gfx's in-flight retirement queue.
    std::uint64_t cached_coverage_count{};
    std::uint64_t cached_coverage_texel_bytes{}; // Logical R8 texels, not physical VRAM.
    std::uint64_t cached_coverage_cpu_bytes{};   // Owned pixels.size(), including CPU row padding.
    std::uint64_t image_upload_count{};
    std::uint64_t image_upload_bytes{}; // Tight RGBA8 payload.
    std::uint64_t cached_image_count{};
    std::uint64_t cached_image_texel_bytes{};
    std::uint64_t cached_image_cpu_bytes{};
};
struct FrameResult {
    FrameStatus status{FrameStatus::skipped};
    std::uint64_t accepted_id{};
    ui::PixelExtent accepted_extent; // Actual target BEFORE recording/Present.
    ui::PixelExtent next_extent;     // Resize during Present applies to NEXT frame.
    FrameStats stats{};
};
// Explicit diagnostic capture stalls the GPU once; normal rendering does not.
// Color is tightly packed sRGB RGBA8, depth tightly packed float32, top-left.
// Output is replaced only for a fully accepted frame, before any end-of-frame
// resize. UI does not modify scene depth. Acceptance is not a scanout guarantee.
struct Capture {
    std::uint64_t frame_id{};
    ui::PixelExtent extent;
    std::vector<std::uint8_t> rgba;
    std::vector<float> depth;
};

// Single owner thread. Application owns HWND and keeps it alive through close.
// Renderer owns GPU objects, rendering, Present, resize and GPU retirement;
// application owns Runtime identities, camera state, input and accepted hit data.
class Renderer {
  public:
    Renderer();
    ~Renderer();
    Renderer(Renderer &&) noexcept;
    Renderer &operator=(Renderer &&) noexcept;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    [[nodiscard]] static Result<Renderer> open(HWND window, const Options &options = {});
    [[nodiscard]] Result<Target> target() const;
    // Minimized/zero client size, target mismatch or occluded Present is skipped.
    // CPU validation failure retains usability. Upload/encoding/execution failure
    // is terminal. Switch the application's hit frame ONLY on accepted.
    [[nodiscard]] Result<FrameResult> render(const Frame &frame, Capture *capture = nullptr);
    [[nodiscard]] Result<void> close();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace atelier::renderer

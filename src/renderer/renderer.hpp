#pragma once

#include "rhi/rhi.hpp"
#include "geometry/mesh.hpp"
#include "ui/frame.hpp"
#include "renderer/view_settings.hpp"
#include "renderer/overlay.hpp"

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
// vectors: p * world * view * projection. Finite Reversed-Z: near=1, far=0.
struct Camera {
    std::array<float, 16> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 16> projection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1};
};
struct Material {
    float roughness{0.65f};
    float metallic{};
    std::array<float, 3> emissive{}; // Linear HDR radiance, before display exposure.
};
enum class LightKind : std::uint32_t { point, rectangle, directional };
struct Light {
    std::uint64_t stable_key{}; // Authority-provided identity scoped by Frame::scene_identity.
    LightKind kind{LightKind::point};
    std::array<float, 3> position{};
    std::array<float, 3> direction{0, 0, 1}; // Unit world propagation direction.
    std::array<float, 3> color{1, 1, 1}; // Linear RGB multiplier.
    // Point: candela; rectangle: cd/m² radiance; directional: lux.
    float intensity{100.f};
    float range{10.f}; // Local smooth cutoff (m), separate from emitting size.
    float source_radius{}; // Point sphere radius (m); directional angular radius (radians).
    std::array<float, 3> right{1, 0, 0};
    std::array<float, 3> up{0, 1, 0}; // Rectangle unit world tangent axes.
    float width{1.f}, height{1.f}; // Rectangle emitting dimensions (m).
    bool two_sided{};
    bool casts_shadow{true}; // Direct-light visibility at every receiving point, including DDGI hits.
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
    Material material{};
    std::uint64_t stable_key{}; // Zero allows legacy draws but disables temporal object reuse.
};
struct Frame {
    const ui::Frame &ui_frame; // Sole authoritative id/extent; borrowed to return.
    Camera camera;
    ui::Rect scene_viewport;     // Physical pixels, top-left origin.
    std::span<const MeshInstance> meshes; // Borrowed draw snapshots; no business object ownership.
    ViewSettings view{};
    std::span<const OverlayShape> overlay{};
    std::span<const Light> lights{};
    std::uint64_t scene_identity{}; // Change for replacement world and Edit/Play execution domain.
    std::uint64_t scene_revision{}; // Content revision; signal owners distinguish changes from scene replacement.
    bool camera_cut{};
    // Uniform radiance from the scene's single Sky Light.
    // No Sky Light means zero; this is not a viewport ambient-light setting.
    std::array<float, 3> environment{};
};
enum class FrameStatus { accepted, skipped };
// Activity from this successful render return (accepted or skipped). Counts
// encoded commands, including an occluded Present; pre-encoding skips have zero
// activity. Legacy counts are not GPU timings. Errors provide no complete statistics.
struct FrameStats {
    std::uint64_t ui_draw_calls{};
    std::uint64_t ui_quads{}; // Visible instances; adjacent compatible quads share a draw.
    std::uint64_t ui_instance_upload_bytes{};
    std::uint64_t scene_draw_calls{};
    std::uint64_t material_upload_count{}; // New immutable scene image versions, including neutral defaults.
    std::uint64_t cached_material_images{}; // Excludes UI textures and gfx's retirement queue.
    std::uint64_t viewport_draw_calls{}; // Background and reference guides; excludes scene geometry.
    std::uint64_t overlay_shapes{}, overlay_draw_calls{};
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
    // Completed, raw GPU sample. All eight passes and the counters refer to
    // gpu_sample_frame_id; may lag this return and repeat until a new completion.
    std::array<float, 8> scene_lit_gpu_ms{}; // GBuffer, AS/uploads, direct/denoise, DDGI, GTAO/Local GI, HDR/SSSR, HDR resolve, bloom/tone/SMAA/display.
    std::uint64_t scene_lit_resource_bytes{}; // Logical resources; excludes in-flight retirement.
    std::uint64_t scene_lit_shadow_rays{}, scene_lit_probe_rays{}; // Actual completed visibility/primary probe queries.
    std::uint32_t scene_lit_shadow_lights{}, scene_lit_unshadowed_lights{}; // This frame's traced slots / requested fallback shadows.
    std::uint64_t scene_lit_history_resets{}; // Reset events in this encoded render; ordinary next frame is zero.
    bool scene_lit_gpu_timing_valid{};
    std::uint64_t scene_lit_gpu_sample_sequence{}, scene_lit_gpu_sample_frame_id{};
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

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
    std::filesystem::path shader_cache{}; // rhi::ContextOptions::shader_cache.
};
// Pipelines a view needs, counted over the groups it enables. Lit groups are
// prepared on Context workers from their first request; a view that enables a
// new group raises total and is not ready again until that group completes.
struct Preparation {
    std::uint32_t done{}, total{};
    bool ready{};
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
    // Complete resolved surfaces in geometry material-slot order. Captures own
    // immutable content; no Runtime objects enter the renderer. Empty uses the
    // mesh defaults with the instance's existing constant material values.
    std::shared_ptr<const std::vector<material::SurfaceRef>> surfaces{};
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
    bool camera_cut{};
    // Uniform sky radiance from the scene's environment (rendering::sky_radiance).
    // Zero when the sky is off; this is not a viewport ambient-light setting.
    std::array<float, 3> environment{};
    std::array<float, 3> lighting_focus{}; // Receiver/play focus for following DDGI volumes; not camera eye.
    // Active rendering time since the last accepted frame, independent of Play.
    // Zero restarts a paused/discontinuous clock; fixed-step producers use 1/60.
    // Only presentation filtering consumes this [0, .1] interval, not sampling confidence.
    float render_delta_seconds{1.f/60.f};
};
// not_ready: the view's pipelines are still preparing; nothing was encoded or
// presented and the frame may be retried, or drawn with another view.
enum class FrameStatus { accepted, skipped, not_ready };
// Activity from this successful render return (accepted or skipped). Counts
// encoded commands, including an occluded Present; pre-encoding skips have zero
// activity. Legacy counts are not GPU timings. Errors provide no complete statistics.
struct ProbeVolumeStats {
    bool enabled{},rolling{};
    std::array<float,3> minimum{},maximum{},spacing{};
    std::array<std::uint32_t,3> counts{};
    std::uint32_t resident{},updated{},reset_probes{}; // CPU schedule, not GPU-valid probe counts.
};
// One receiver's actual gather inputs. Linear irradiance, not display RGB.
// Identity: volume, physical probe, support stage (0 unused, 1 pending,
// 2 inside, 3 unobserved, 4 zero weight, 5 occluded, 6 contributor), update age.
struct ProbeContribution {
    std::array<std::uint32_t,4> identity{};
    std::array<float,4> irradiance{}; // RGB and directional observation count.
    std::array<float,4> weights{}; // Spatial, facing squared, moment visibility, unnormalized visible weight within volume.
    std::array<float,4> position{}; // Probe world XYZ and normalized receiver distance.
    std::array<float,4> transport{}; // Authoritative RGB / observations before presentation smoothing.
    std::array<std::uint32_t,4> response{}; // Requested epoch, min/max effective observations over tile, active visit cursor.
};
struct ProbeInspection {
    std::uint64_t sequence{},frame_id{},scene_identity{};
    std::array<std::uint32_t,2> pixel{};
    ProbeHistory history{ProbeHistory::live};
    std::int32_t requested_probe{-1}; // -1 automatic; explicit requests never fall back to another probe.
    std::uint32_t rays{},updates{};
    std::array<ProbeVolumeStats,probe_volume_limit> volumes{};
    // Receiver identity: gather status, selected volume, contributors, reserved.
    // Irradiance RGB / coverage; weights: sum, dominance; position: world XYZ.
    ProbeContribution receiver{};
    std::array<ProbeContribution,probe_volume_limit*8> probes{};
    struct Ray {
        std::array<std::uint32_t,4> identity{}; // Source, kind, ray ordinal, valid record.
        std::array<float,4> direct{}; // Local diffuse/specular, directional diffuse/specular luminance.
        std::array<float,4> surface{}; // World hit XYZ, roughness.
        std::array<float,4> sampling{}; // Reservoir weight, full receiver proposal PMF, feedback luminance, primary hit distance.
        std::array<float,4> endpoint_distance{}; // Selected local endpoint XYZ, hit-to-endpoint distance; w=-1 if none.
        std::array<float,4> source_parameters{}; // Radius, range, area, attenuation minimum distance squared (zero for mesh sources).
    };
    std::uint32_t traced_probe{~0u};
    std::array<Ray,256> ray_samples{};
};
struct FrameStats {
    std::array<ProbeVolumeStats,probe_volume_limit> scene_lit_probe_volumes{};
    // Optional completed inspection; owns its exact sampled layout and frame.
    std::shared_ptr<const ProbeInspection> scene_lit_probe_inspection;
    std::uint64_t ui_draw_calls{};
    std::uint64_t ui_quads{}; // Visible instances that overlap their clip; all share one draw.
    std::uint64_t ui_instance_upload_bytes{};
    std::uint64_t scene_draw_calls{};
    std::uint64_t scene_instances{}, scene_culled_instances{}; // Current primary raster frustum; full lighting scene is retained.
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
    // Completed, raw GPU sample. All passes, subpasses and counters refer to
    // gpu_sample_frame_id; may lag this return and repeat until a new completion.
    std::array<float, 8> scene_lit_gpu_ms{}; // GBuffer, AS/uploads, direct lighting and HDR composition, DDGI, GTAO, SSSR, HDR resolve, bloom/tone/SMAA/display.
    std::array<float, 2> scene_lit_ddgi_gpu_ms{}; // Update/transfer, receiver gather; subsets of DDGI time.
    std::array<float, 2> scene_lit_ddgi_update_gpu_ms{}; // Primary trace, hit lighting/convolution; subsets of update/transfer (copy excluded).
    float scene_lit_direct_cluster_gpu_ms{}; // Light list construction; subset of direct lighting.
    float scene_lit_direct_shading_gpu_ms{}; // Light evaluation fused with HDR composition; subset of direct lighting.
    // Completed conservative receiver clusters, not per-pixel light counts.
    std::uint32_t scene_lit_direct_cluster_cells{},scene_lit_direct_cluster_overflows{},scene_lit_direct_cluster_max_sources{};
    // Optional completed counts at actual GBuffer receivers: 0..64, then >64.
    // Conservative local analytic support only; no directional sources or visibility test.
    std::array<std::uint32_t,66> scene_lit_direct_receiver_histogram{};
    std::uint32_t scene_lit_direct_receiver_max_sources{};
    float scene_lit_shadow_cache_gpu_ms{}; // Atlas updates only; subset of direct lighting.
    std::uint32_t scene_lit_shadow_cache_faces{},scene_lit_shadow_cache_reused_faces{};
    std::uint32_t scene_lit_shadow_cache_static_updates{},scene_lit_shadow_cache_dynamic_updates{},scene_lit_shadow_cache_draws{};
    // Dropped lights cast shadows but did not fit the atlas; they contribute no direct light.
    std::uint32_t scene_lit_shadow_cache_resolution_downgrades{},scene_lit_shadow_cache_dropped_lights{},scene_lit_shadow_cache_near_plane_faces{};
    std::uint32_t scene_lit_shadow_cache_missing_taps{},scene_lit_shadow_cache_plane_fallbacks{};
    // Faces deferred by the per-frame redraw budget, their longest consecutive deferral
    // in frames, the 128² atlas cells held by requested faces, and lights withheld
    // from direct light until all of their faces are drawn.
    std::uint32_t scene_lit_shadow_cache_deferred_faces{},scene_lit_shadow_cache_deferral_frames{},scene_lit_shadow_cache_atlas_cells{};
    std::uint32_t scene_lit_shadow_cache_pending_lights{};
    // Enabled scopes in that completed sample: bits 0..7 main, 8..9 DDGI,
    // 10..11 DDGI update, 12 light lists, 13 shadow atlas updates, 14 direct shading and composition.
    // A skipped scope still has timestamp overhead.
    std::uint32_t scene_lit_gpu_pass_mask{};
    std::array<std::uint64_t, 2> scene_lit_ddgi_visibility_rays{}; // Probe-hit shading, receiver gather; subsets of visibility queries.
    std::uint64_t scene_lit_resource_bytes{}; // Logical resources; excludes in-flight retirement.
    std::uint64_t scene_lit_shadow_rays{}, scene_lit_probe_rays{}; // Actual completed visibility/primary probe queries.
    std::uint32_t scene_lit_light_sources{},scene_lit_shadow_sources{}; // Analytic lights; those casting shadows.
    std::uint32_t scene_lit_spatial_sources{},scene_lit_spatial_cells{}; // Retained sources and nonempty proposal cells; not a culling count.
    std::uint64_t scene_lit_spatial_bytes{}; // Three current logical GPU buffers, including empty sentinels.
    bool scene_lit_spatial_changed{}; // Proposal buffers uploaded in this encoded render.
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
// Scene Lit captures add surface motion: per pixel, previous minus current
// window UV as two float32; empty for other shading.
// Output is replaced only for a fully accepted frame, before any end-of-frame
// resize. UI does not modify scene depth. Acceptance is not a scanout guarantee.
struct Capture {
    std::uint64_t frame_id{};
    ui::PixelExtent extent;
    std::vector<std::uint8_t> rgba;
    std::vector<float> depth;
    std::vector<float> motion;
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
    // Match the native client before preparing its UI frame; no Present is needed.
    [[nodiscard]] Result<Target> sync_window_target();
    // Never waits for compilation. Starts this view's missing pipeline groups and
    // collects finished ones; views that draw no Lit pass are always ready. A
    // preparation failure is non-terminal and returned again for later Lit views.
    [[nodiscard]] Result<Preparation> preparation(const ViewSettings &view);
    // Minimized/zero client size, target mismatch or occluded Present is skipped.
    // A drawn Lit view whose preparation is incomplete returns not_ready.
    // CPU validation failure retains usability. Upload/encoding/execution failure
    // is terminal. Switch the application's hit frame ONLY on accepted.
    [[nodiscard]] Result<FrameResult> render(const Frame &frame, Capture *capture = nullptr);
    [[nodiscard]] Result<void> close();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace atelier::renderer

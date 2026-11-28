#pragma once

#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "math/math.hpp"
#include "rdg/rdg.hpp"
#include "rhi/rhi.hpp"
#include "geometry/mesh.hpp"
#include "material/program_resources.hpp"
#include "ui/frame.hpp"
#include "renderer/view_settings.hpp"
#include "renderer/overlay.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace atelier::renderer {

namespace detail {
struct MaterialBatchState;
}

struct Error {
    rhi::Error cause;

    // Once encoding/uploads start, failure permits only close/destruction.
    bool terminal{};
};

template <class T>
using Result = std::expected<T, Error>;

struct Options {
    rhi::Validation validation{rhi::Validation::disabled};

    std::uint32_t wait_timeout_ms{10000};

    bool vsync{true};

    bool frame_latency_waitable{}; // Explicit host pacing; independent of VSync and resource slots.

    bool ui_only{}; // Lightweight UI/popup host: skips 3D PSOs, LUTs, GeometryPool, VSM and GI caches.

    std::filesystem::path shader_cache{}; // rhi::ContextOptions::shader_cache.
};

// Pipelines a view needs, counted over the groups it enables. Lit groups are
// prepared on Context workers from their first request; a view that enables a
// new group raises total and is not ready again until that group completes.
struct Preparation {
    std::uint32_t done{};

    std::uint32_t total{};

    bool ready{};
};

// One immutable candidate's material programs, parameters and GPU preparation.
// Owner-thread only; release before closing its Renderer. Moving the owner does
// not change its slot order. No Runtime roots or live material objects survive.
class MaterialBatch {
public:
    MaterialBatch();

    ~MaterialBatch();

    MaterialBatch(MaterialBatch&&) noexcept;

    MaterialBatch& operator=(MaterialBatch&&) noexcept;

    MaterialBatch(const MaterialBatch&) = delete;

    MaterialBatch& operator=(const MaterialBatch&) = delete;

    Span<const material::ProgramSurfaceRef> surfaces() const noexcept;

private:
    friend class Renderer;

    std::unique_ptr<detail::MaterialBatchState> state_;
};

struct Target {
    ui::PixelExtent extent;

    bool drawable{};
};

// Left-handed, +Y up, meters, D3D depth [0,1]. Row-major storage and row
// vectors: p * world * view * projection. Reversed-Z: near=1, background=0.
// Infinite perspective maps depth=near/view Z; finite orthographic maps far=0.
struct Camera {
    math::Matrix4 view = math::Matrix4::identity();

    math::Matrix4 projection{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1}};

    // Lower clipping depth: near/far for infinite perspective with a finite far
    // plane, zero for a projection carrying its own far plane (or no far limit).
    // A change is a projection change: the producer must set Frame::camera_cut.
    float far_depth{};
};

enum class LightKind : std::uint32_t { point, rectangle, directional, spot };

struct Light {
    std::uint64_t stable_key{}; // Authority-provided identity scoped by Frame::scene_identity.

    LightKind kind{LightKind::point};

    math::Vector3 position{};

    math::Vector3 direction{0, 0, 1}; // Unit world propagation direction.

    math::Vector3 color{1, 1, 1}; // Linear RGB multiplier.

    // Point/Spot: candela; rectangle: cd/m² radiance; directional: lux.
    float intensity{100.f};

    float range{10.f}; // Local smooth cutoff (m), separate from emitting size.

    float source_radius{}; // Point/Spot sphere radius (m); directional angular radius (radians).

    math::Vector3 right{1, 0, 0};

    math::Vector3 up{0, 1, 0}; // Rectangle unit world tangent axes.

    float width{1.f};

    float height{1.f}; // Rectangle emitting dimensions (m).

    float spot_inner_radians{.43633232f}; // Spot inner cone half-angle (radians).

    float spot_outer_radians{.6981317f}; // Spot outer cone half-angle (radians).

    bool two_sided{};

    bool casts_shadow{true}; // Direct-light visibility at every receiving point, including GI hits.
};

struct MeshInstance {
    // Producer-evaluated local-to-world affine matrix. Same row-major storage
    // and row-vector convention as Camera; hierarchy evaluation stays outside
    // the renderer. Geometry is an immutable, shareable triangle resource.
    // Non-finite, non-affine, singular or GPU-unrepresentable normal transforms
    // reject the entire frame before encoding. Invertible reflected transforms are supported by this boundary.
    math::Matrix4 world = math::Matrix4::identity();

    bool selected{};

    geometry::MeshRef geometry;

    std::uint64_t stable_key{}; // Zero allows legacy draws but disables temporal object reuse.

    // Required complete geometry-slot -> Frame::materials index mapping.
    // No business material defaults or instance overrides exist at this boundary.
    std::shared_ptr<const Array<std::uint32_t>> material_slots{};
};

struct PickPixelSample {
    std::uint32_t instance_index{};

    std::uint32_t global_triangle_id{};

    float depth{};
};

struct Frame {
    const ui::Frame& ui_frame; // Sole authoritative id/extent; borrowed to return.

    Camera camera;

    ui::Rect scene_viewport; // Physical pixels, top-left origin.

    Span<const MeshInstance> meshes; // Borrowed draw snapshots; no business object ownership.

    ViewSettings view{};

    Span<const OverlayShape> overlay{};

    Span<const Light> lights{};

    std::uint64_t scene_identity{}; // Change for replacement world, isolated SOP graph or Play domain.

    bool camera_cut{};

    // sky_color * sky_intensity from the scene's environment (rendering::sky_radiance);
    // view.lit.sky_map and sky_rotation shape it by direction. Zero when the sky is
    // off; this is not a viewport ambient-light setting.
    math::Vector3 environment{};

    math::Vector3 lighting_focus{}; // Editor observation target; Play main camera world position.

    // Active rendering time since the last accepted frame, independent of Play.
    // Zero restarts a paused/discontinuous clock; fixed-step producers use 1/60.
    // Only presentation filtering consumes this [0, .1] interval, not sampling confidence.
    float render_delta_seconds{1.f / 60.f};

    // Explicit candidate, borrowed through render(). Its slot order and every
    // mesh mapping must belong together. Required and ready for any mesh frame;
    // UI/no-mesh frames may omit it.
    const MaterialBatch* materials{};

    // Optional shutdown request, borrowed only through render(). Called on the
    // owner thread between preparation batches; must not throw or reenter Renderer.
    // A true result cancels this frame and permits only close/destruction afterwards.
    const std::function<bool()>* stop_requested{};

    // Optional, preallocated one-attempt RDG observation. Borrowed
    // only through render(), never reused for fallback/retry or GPU readback.
    rdg::TraceSnapshot* command_trace{};

    std::string_view trace_scope{"full"};
};

enum class FrameStatus { accepted, skipped, not_ready, cancelled };

struct FrameStats {
    std::uint64_t ui_draw_calls{};

    std::uint64_t scene_draw_calls{};

    std::uint64_t scene_instances{};

    std::uint64_t scene_culled_instances{};

    std::uint64_t material_upload_count{};

    std::uint64_t cached_material_images{};

    std::uint64_t viewport_draw_calls{};

    std::uint64_t overlay_shapes{};

    std::uint64_t overlay_draw_calls{};

    // Completed, raw GPU sample. All passes, subpasses and counters refer to
    // gpu_sample_frame_id; may lag this return and repeat until a new completion.
    std::array<float, 8>
        scene_lit_gpu_ms{}; // VisBuffer/GBuffer, AS/uploads, direct lighting, 3-Layer GI, GT-VBAO, 3-Tier reflections, HDR resolve, bloom/tone/AA/display.

    float scene_lit_gpu_union_ms{}; // True union GPU active interval across graphics + async compute + copy queues.

    std::array<float, 2> scene_lit_gi_gpu_ms{}; // Update/SHaRC, spatial-temporal gather; subsets of 3-Layer GI.

    std::array<float, 2>
        scene_lit_gi_update_gpu_ms{}; // Screen-probe primary trace, secondary hit shading/SHaRC; subsets of update.

    float scene_lit_direct_cluster_gpu_ms{}; // 16x16x32 light list construction; subset of direct lighting.

    float scene_lit_direct_shading_gpu_ms{}; // Direct BRDF/LTC evaluation; subset of direct lighting.

    // Completed conservative receiver clusters, not per-pixel light counts.
    std::uint32_t scene_lit_direct_cluster_cells{};

    std::uint32_t scene_lit_direct_cluster_overflows{};

    std::uint32_t scene_lit_direct_cluster_max_sources{};

    // Optional completed counts at actual GBuffer receivers: 0..64, then >64.
    std::array<std::uint32_t, 66> scene_lit_direct_receiver_histogram{};

    std::uint32_t scene_lit_direct_receiver_max_sources{};

    float scene_lit_vsm_gpu_ms{}; // VSM physical page clear/CoW/cull/draw; subset of direct lighting.

    std::uint32_t scene_lit_vsm_allocated_pages{};

    std::uint32_t scene_lit_vsm_reused_pages{};

    std::uint32_t scene_lit_vsm_static_renders{};

    std::uint32_t scene_lit_vsm_dynamic_renders{};

    std::uint32_t scene_lit_vsm_cow_copies{};

    std::uint32_t scene_lit_vsm_draws{};

    std::uint32_t scene_lit_vsm_evictions{};

    // Enabled scopes in that completed sample: bits 0..7 main, 8..9 GI,
    // 10..11 GI update, 12 light lists, 13 VSM page updates, 14 direct shading.
    std::uint32_t scene_lit_gpu_pass_mask{};

    std::array<std::uint64_t, 2>
        scene_lit_gi_visibility_rays{}; // Secondary hit NEE shadow rays, gather visibility rays.

    std::uint64_t scene_lit_resource_bytes{}; // Logical resources; excludes in-flight retirement.

    std::uint64_t scene_lit_shadow_rays{};

    std::uint64_t scene_lit_gi_primary_rays{}; // Actual completed screen-probe primary rays.

    std::uint32_t
        scene_lit_reflection_work_items{}; // Pixels classified for reflection ray tracing in that completed sample.

    std::uint32_t scene_lit_light_sources{};

    std::uint32_t scene_lit_shadow_sources{}; // Analytic lights; those casting shadows.

    std::uint64_t scene_lit_history_resets{}; // Reset events in this encoded render; ordinary next frame is zero.

    bool scene_lit_gpu_timing_valid{};

    std::uint64_t scene_lit_gpu_sample_sequence{};

    std::uint64_t scene_lit_gpu_sample_frame_id{};
};

struct FrameResult {
    FrameStatus status{FrameStatus::skipped};

    std::uint64_t accepted_id{};

    ui::PixelExtent accepted_extent; // Actual target BEFORE recording/Present.

    ui::PixelExtent next_extent; // Resize during Present applies to NEXT frame.

    FrameStats stats{};
};

struct Capture {
    std::uint64_t frame_id{};

    ui::PixelExtent extent;

    Array<std::uint8_t> rgba;

    Array<float> depth;

    Array<float> motion;
};

enum class Dormancy : std::uint8_t {
    transients = 0, // L1: release RDG transient heaps & reclaim completed GPU resources
    viewport_hidden = 1, // L2: transients + retire screen-space LitContext history targets
    deep = 2, // L3: transients + retire LitContext targets + retire 128 MiB VSM physical page pools
};

class Renderer {
public:
    Renderer();

    ~Renderer();

    Renderer(Renderer&&) noexcept;

    Renderer& operator=(Renderer&&) noexcept;

    Renderer(const Renderer&) = delete;

    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] static Result<Renderer> open(std::shared_ptr<rhi::Device> device, rhi::NativeWindow window,
                                               const Options& options = {});

    [[nodiscard]] static Result<Renderer> open(rhi::NativeWindow window, const Options& options = {});

    [[nodiscard]] const std::shared_ptr<rhi::Device>& device() const noexcept;

    [[nodiscard]] Result<Target> target() const;

    [[nodiscard]] Result<Target> sync_window_target();

    [[nodiscard]] Result<bool> wait_frame(std::uint32_t timeout_ms);

    [[nodiscard]] Result<Preparation> preparation(const ViewSettings& view);

    // Prepares the entire material-dependent raster/ray pipeline set, not a view.
    // Submission never adopts the candidate. Live batches with the same exact
    // program set share compilation/PSOs, independently of policy, parameters,
    // input images and slot order. Images use the existing linear/sRGB mip cache.
    // The last rendered code set (not its tables/images) stays reusable until
    // another code set is rendered or Renderer closes.
    // Unrelated product/UI preparation remains separate.
    [[nodiscard]] Result<MaterialBatch> prepare_materials(Span<const material::ProgramSurfaceRef>);

    [[nodiscard]] Result<Preparation> preparation(MaterialBatch&);

    [[nodiscard]] Result<FrameResult> render(const Frame& frame, Capture* capture = nullptr);

    // GPU VisBuffer picking on the latest accepted 3D viewport image:
    [[nodiscard]] Result<std::optional<PickPixelSample>> pick_pixel(std::uint32_t x, std::uint32_t y);

    [[nodiscard]] Result<std::vector<std::uint32_t>> pick_box(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                                                              std::uint32_t y1);

    void release_transients(Dormancy level = Dormancy::transients) noexcept;

    [[nodiscard]] Result<void> close();

private:
    struct State;

    std::unique_ptr<State> state_;
};

} // namespace atelier::renderer

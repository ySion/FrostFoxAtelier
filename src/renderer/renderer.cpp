#include "renderer.hpp"
#include "shader_source.hpp"
#include "math/math.hpp"
#include "scene_lit.hpp"

#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace atelier::renderer {
namespace {
Error error(rhi::ErrorCode code, const char *text, bool terminal = false,
            GfxResult native = kGfxResult_InvalidParameter) {
    return {{code, native, text}, terminal};
}
void require(GfxResult result, const char *operation) {
    if (result != kGfxResult_NoError) throw error(rhi::ErrorCode::execution_failed, operation, true, result);
}
void allocation(bool valid) {
    if (!valid)
        throw error(rhi::ErrorCode::creation_failed, "GPU allocation failed", true, kGfxResult_OutOfMemory);
}
bool finite(float value) {
    return std::isfinite(value);
}
bool valid(const ui::Color &c) {
    return finite(c.r) && finite(c.g) && finite(c.b) && finite(c.a) && c.r >= 0 && c.g >= 0 && c.b >= 0 &&
           c.a >= 0 && c.r <= 1 && c.g <= 1 && c.b <= 1 && c.a <= 1;
}
bool valid(const ui::Rect &r) {
    return finite(r.x) && finite(r.y) && finite(r.width) && finite(r.height) && r.width >= 0 &&
           r.height >= 0 && finite(r.x + r.width) && finite(r.y + r.height);
}
template<class Pixels> bool valid_pixels(const Pixels &image, unsigned channels) {
    return image.identity != 0 && image.width > 0 && image.height > 0 &&
        image.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        image.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        std::uint64_t(image.row_pitch) >= std::uint64_t(image.width) * channels &&
        std::uint64_t(image.row_pitch) * image.height <= image.pixels.size();
}
void validate(const Frame &frame) {
    const auto &ui = frame.ui_frame;
    bool good =
        ui.id != 0 && finite(ui.dpi_scale) && ui.dpi_scale > 0 && valid(frame.scene_viewport) &&
        frame.scene_viewport.x >= 0 && frame.scene_viewport.y >= 0 &&
        frame.scene_viewport.x + frame.scene_viewport.width <= static_cast<float>(ui.extent.width) &&
        frame.scene_viewport.y + frame.scene_viewport.height <= static_cast<float>(ui.extent.height) &&
        frame.meshes.size() <= std::numeric_limits<std::uint32_t>::max() &&
        ui.draws.size() <= std::numeric_limits<std::uint32_t>::max();
    good &= frame.view.shading <= Shading::scene_lit && frame.view.wireframe <= Wireframe::only &&
        frame.view.background <= Background::gradient && frame.view.grid_plane <= GridPlane::yz &&
        finite(frame.view.grid_spacing) && frame.view.grid_spacing>=.001f && frame.view.grid_spacing<=10000.f &&
        finite(frame.view.wire_width) && frame.view.wire_width > 0 && frame.view.wire_width <= 8 &&
        finite(frame.view.wire_opacity) && frame.view.wire_opacity >= 0 && frame.view.wire_opacity <= 1;
    for (float v : frame.camera.view)
        good &= finite(v);
    for (float v : frame.camera.projection)
        good &= finite(v);
    for (const auto &mesh : frame.meshes) {
        good &= bool(mesh.geometry) && valid(mesh.color);
        for (float v : mesh.world)
            good &= finite(v);
        good &= mesh.world[3] == 0 && mesh.world[7] == 0 && mesh.world[11] == 0 && mesh.world[15] == 1;
    }
    for (const auto &draw : ui.draws) {
        good &= valid(draw.bounds) && valid(draw.uv) && valid(draw.color) && valid(draw.border_color) && valid(draw.image_background) &&
            finite(draw.border_width) && draw.border_width >= 0 && !(draw.coverage && draw.image);
        for (float radius : {draw.radius.top_left, draw.radius.top_right, draw.radius.bottom_right, draw.radius.bottom_left})
            good &= finite(radius) && radius >= 0;
        if (draw.coverage) good &= valid_pixels(*draw.coverage, 1);
        if (draw.image) good &= valid_pixels(*draw.image, 4);
    }
    if (!good)
        throw error(rhi::ErrorCode::invalid_argument, "Invalid renderer frame, camera, shape, color or image");
    for (const auto &shape : frame.overlay) {
        bool valid_shape = shape.kind <= OverlayShape::Kind::rectangle && finite(shape.width) && shape.width >= 0 &&
            shape.width <= 64 && valid(shape.color) && valid(shape.border);
        for (float v : shape.points) valid_shape &= finite(v) && std::abs(v) < 1e7f;
        if (shape.kind == OverlayShape::Kind::rectangle) valid_shape &= shape.points[2] >= 0 && shape.points[3] >= 0;
        if (!valid_shape) throw error(rhi::ErrorCode::invalid_argument, "Invalid viewport overlay shape");
    }
}
struct Instance {
    std::array<float, 16> world;
    std::array<float, 9> normal_matrix;
    float selected;
    ui::Color color;
};
static_assert(sizeof(Instance) == 120 && sizeof(geometry::Vertex) == 24);
struct SceneTriangle {
    std::array<geometry::Vertex, 3> vertices;
    std::uint32_t edge_mask;
    std::array<float, 3> face_normal;
};
struct GridCamera {
    std::array<float, 16> view_projection, inverse;
};
std::optional<GridCamera> prepare_grid_camera(const Frame& frame) {
    if ((!frame.view.reference_grid && !frame.view.origin_axes) ||
        frame.scene_viewport.width == 0 || frame.scene_viewport.height == 0) return {};
    math::Matrix4d view, projection;
    for (std::size_t i=0; i<16; ++i) {
        view.elements[i]=frame.camera.view[i];
        projection.elements[i]=frame.camera.projection[i];
    }
    const auto combined=view*projection;
    const auto inverse=math::try_inverse(combined);
    // An optional reference guide must not reject an otherwise valid camera.
    if (!inverse) return {};
    GridCamera result;
    for (std::size_t i=0; i<16; ++i) {
        result.view_projection[i]=static_cast<float>(combined.elements[i]);
        result.inverse[i]=static_cast<float>(inverse->elements[i]);
        if (!finite(result.view_projection[i]) || !finite(result.inverse[i])) return {};
    }
    return result;
}
enum class PipelineStyle { ui, surface, depth, visible_lines, all_lines, background, grid, grid_depth };
static_assert(sizeof(SceneTriangle) == 88);
SceneTriangle prepare_triangle(const geometry::Mesh& mesh, std::size_t index) {
    const auto vertices = mesh.vertices().subspan(index * 3, 3);
    SceneTriangle result{{vertices[0], vertices[1], vertices[2]}, mesh.edge_mask(index), {}};
    std::array<double, 3> a{}, b{}, normal{};
    for (std::size_t i = 0; i < 3; ++i) {
        a[i] = double(vertices[1].position[i]) - vertices[0].position[i];
        b[i] = double(vertices[2].position[i]) - vertices[0].position[i];
    }
    normal = {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    const double length = std::hypot(normal[0], normal[1], normal[2]);
    double orientation = 0;
    for (std::size_t i = 0; i < 3; ++i)
        orientation += normal[i] * (double(vertices[0].normal[i]) + vertices[1].normal[i] + vertices[2].normal[i]);
    for (std::size_t i = 0; i < 3; ++i)
        result.face_normal[i] = length == 0 ? vertices[0].normal[i] :
            static_cast<float>(normal[i] / length * (orientation < 0 ? -1 : 1));
    return result;
}
Instance prepare_instance(const MeshInstance &mesh) {
    const auto &m = mesh.world;
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    // Cofactor rows are inverse-transpose rows times determinant. Double
    // intermediates avoid overflow/underflow for finite float matrix entries.
    const std::array<double, 9> cofactors{e * i - f * h, f * g - d * i, d * h - e * g,
                                          c * h - b * i, a * i - c * g, b * g - a * h,
                                          b * f - c * e, c * d - a * f, a * e - b * d};
    const double determinant = a * cofactors[0] + b * cofactors[1] + c * cofactors[2];
    if (determinant == 0) throw error(rhi::ErrorCode::invalid_argument, "Mesh world matrix is singular");
    Instance result{mesh.world, {}, mesh.selected ? 1.0f : 0.0f, mesh.color};
    // One common factor preserves inverse-transpose row weights for arbitrary
    // vertex normals. Double cofactors also support very small/large scales.
    double maximum = 0;
    for (double value : cofactors) maximum = std::max(maximum, std::abs(value));
    const double sign = determinant < 0 ? -1. : 1.;
    for (std::size_t n = 0; n < cofactors.size(); ++n)
        result.normal_matrix[n] = static_cast<float>(sign * cofactors[n] / maximum);
    // Test the actual FP32 representation, including GPU flush-to-zero. A
    // nonzero double determinant alone does not guarantee a usable GPU matrix.
    for (float& value : result.normal_matrix)
        if (std::abs(value) < std::numeric_limits<float>::min()) value = 0;
    const auto& normal = result.normal_matrix;
    const double gpu_determinant = double(normal[0])*(double(normal[4])*normal[8]-double(normal[5])*normal[7])
        - double(normal[1])*(double(normal[3])*normal[8]-double(normal[5])*normal[6])
        + double(normal[2])*(double(normal[3])*normal[7]-double(normal[4])*normal[6]);
    if (gpu_determinant == 0)
        throw error(rhi::ErrorCode::invalid_argument, "Mesh normal transform cannot be represented by GPU floats");
    return result;
}
struct TextureKey {
    std::uint64_t identity, revision;
    bool color{};
    bool operator==(const TextureKey &) const = default;
};
struct TextureHash {
    std::size_t operator()(const TextureKey &k) const noexcept {
        return std::hash<std::uint64_t>{}(k.identity) ^ (std::hash<std::uint64_t>{}(k.revision) << 1) ^
            (std::hash<bool>{}(k.color) << 2);
    }
};
struct CachedImage {
    std::shared_ptr<const void> owner;
    GfxTexture texture;
    bool used{};
    std::uint64_t texel_bytes{}, cpu_bytes{};
};
struct UiInstance {
    ui::Rect bounds, uv;
    ui::Color tint, border_color;
    ui::CornerRadii radius;
    ui::Color image_background;
    float border_width{}, reserved[3]{};
};
static_assert(sizeof(UiInstance) == 112);
struct UiBatch {
    std::uint32_t first{}, count{};
    ui::Clip clip;
    TextureKey texture;
    const ui::DrawQuad *source{};
};
ui::CornerRadii fit_radius(ui::CornerRadii r, ui::Rect bounds) {
    double factor = 1;
    const auto fit = [&](double sum, double extent) { if (sum > 0) factor = std::min(factor, extent / sum); };
    fit(double(r.top_left) + r.top_right, bounds.width);
    fit(double(r.bottom_left) + r.bottom_right, bounds.width);
    fit(double(r.top_left) + r.bottom_left, bounds.height);
    fit(double(r.top_right) + r.bottom_right, bounds.height);
    return {float(r.top_left * factor), float(r.top_right * factor),
            float(r.bottom_right * factor), float(r.bottom_left * factor)};
}
// gfx destruction queues retirement in the same execution domain; it is safe
// after the last encoded use, without waiting for each individual resource.
struct BufferOwner {
    GfxContext context;
    GfxBuffer buffer;
    ~BufferOwner() {
        if (buffer) {
            if (gfxDestroyBuffer(context, buffer) != kGfxResult_NoError) std::abort();
        }
    }
};
} // namespace

struct Renderer::State {
    const std::thread::id owner = std::this_thread::get_id();
    rhi::Context context;
    GfxContext gfx;
    Options options;
    rhi::Pipeline scene, scene_depth, visible_lines, all_lines, solid, coverage, color_image;
    rhi::Pipeline background, grid, grid_depth;
    std::optional<GridCamera> grid_camera;
    GfxTexture matcap;
    GfxSamplerState sampler;
    struct CachedGeometry { geometry::MeshRef source; GfxBuffer vertices; bool used{}; };
    struct MeshBatch { std::uint32_t first, count; geometry::MeshRef geometry; };
    std::unordered_map<const geometry::Mesh*, CachedGeometry> geometries;
    std::vector<MeshBatch> mesh_batches;
    std::unique_ptr<detail::SceneLit> scene_lit;
    std::vector<detail::SceneLitDraw> lit_draws;
    GfxTexture depth;
    ui::PixelExtent depth_extent;
    std::unordered_map<TextureKey, CachedImage, TextureHash> images;
    FrameStats stats;
    std::vector<Instance> instances;
    std::vector<UiInstance> ui_instances;
    std::vector<UiBatch> ui_batches;
    std::size_t overlay_instances{}, overlay_batches{}; // Leading viewport annotations in ui_instances/ui_batches.
    std::vector<std::uint8_t> upload_pixels;
    std::optional<Error> failure;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> debug;

    ~State() {
        const auto result = close();
        if (!result) {
            std::fprintf(stderr, "Renderer cleanup failed: %s\n", result.error().cause.message.c_str());
            std::abort();
        }
    }
    Result<void> check() const {
        if (owner != std::this_thread::get_id())
            return std::unexpected(error(rhi::ErrorCode::wrong_thread, "Renderer requires owner thread"));
        if (!gfx) return std::unexpected(error(rhi::ErrorCode::closed, "Renderer is closed"));
        if (failure) return std::unexpected(*failure);
        return {};
    }
    Result<void> close() {
        if (!gfx) return {};
        if (owner != std::this_thread::get_id())
            return std::unexpected(
                error(rhi::ErrorCode::wrong_thread, "Renderer close requires owner thread"));
        if(scene_lit) {
            const auto result=scene_lit->close();
            if(!result)return result;
            scene_lit.reset();
        }
        for (auto &[key, image] : images) {
            const auto r = gfxDestroyTexture(gfx, image.texture);
            if (r != kGfxResult_NoError)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "UI texture retirement failed", true, r));
            image.texture = {};
        }
        images.clear();
        if (matcap) {
            const auto r = gfxDestroyTexture(gfx, matcap);
            if (r) return std::unexpected(error(rhi::ErrorCode::execution_failed, "MatCap retirement failed", true, r));
            matcap = {};
        }
        if (depth) {
            const auto r = gfxDestroyTexture(gfx, depth);
            if (r)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "Depth retirement failed", true, r));
            depth = {};
        }
        for (auto& [key, geometry] : geometries) {
            (void)key;
            if (!geometry.vertices) continue;
            const auto r = gfxDestroyBuffer(gfx, geometry.vertices);
            if (r) return std::unexpected(error(rhi::ErrorCode::execution_failed, "Geometry retirement failed", true, r));
            geometry.vertices = {};
        }
        geometries.clear(); mesh_batches.clear();
        if (sampler) {
            const auto r = gfxDestroySamplerState(gfx, sampler);
            if (r)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "Sampler retirement failed", true, r));
            sampler = {};
        }
        scene = {};
        scene_depth = {};
        visible_lines = {};
        all_lines = {};
        solid = {};
        coverage = {};
        color_image = {};
        background = {};
        grid = {};
        grid_depth = {};
        auto result = context.close();
        if (!result) return std::unexpected(Error{result.error(), true});
        gfx = {};
        try {
            check_debug();
        } catch (const Error &e) {
            debug.Reset();
            return std::unexpected(e);
        }
        debug.Reset();
        return {};
    }
    void prepare_pipeline(rhi::Pipeline &output, const char *name, const char *vs, const char *ps,
                          PipelineStyle style) {
        rhi::PipelineDesc desc;
        desc.kind = rhi::PipelineKind::vertex;
        desc.name = name;
        desc.source.vs = vs;
        desc.source.ps = ps;
        require(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
                "Set window color target");
        require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set culling");
        const bool with_depth = style == PipelineStyle::surface || style == PipelineStyle::depth ||
            style == PipelineStyle::visible_lines || style == PipelineStyle::all_lines || style == PipelineStyle::grid_depth;
        const bool write_depth = style == PipelineStyle::surface || style == PipelineStyle::depth;
        if (with_depth) {
            require(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT),
                    "Set depth target");
            require(gfxDrawStateSetDepthFunction(desc.draw_state,
                    style == PipelineStyle::all_lines ? D3D12_COMPARISON_FUNC_ALWAYS :
                    style == PipelineStyle::visible_lines ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_GREATER),
                    "Set depth comparison");
        }
        require(gfxDrawStateSetDepthWriteMask(desc.draw_state,
                write_depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO), "Set depth writes");
        if (style != PipelineStyle::surface && style != PipelineStyle::background)
            require(gfxDrawStateSetBlendMode(desc.draw_state,
                style == PipelineStyle::depth ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
                style == PipelineStyle::depth ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
                style == PipelineStyle::depth ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
                style == PipelineStyle::depth ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD),
                "Set pass blending");
        auto prepared = context.prepare(desc);
        if (!prepared) throw Error{prepared.error(), false};
        output = std::move(*prepared);
    }
    void prepare_matcap() {
        if (matcap) return;
        // Bake one neutral clay sphere in linear light, then encode its pixels as
        // sRGB. This is a fixed material capture, shared by every scene instance.
        constexpr unsigned side = 256;
        std::vector<std::uint8_t> pixels(side * side * 4);
        for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
            float nx = 2 * (float(x) + .5f) / side - 1, ny = 1 - 2 * (float(y) + .5f) / side;
            const float radius = std::hypot(nx, ny);
            if (radius > 1) { nx /= radius; ny /= radius; }
            const float nz = -std::sqrt(std::max(0.f, 1 - nx*nx - ny*ny));
            const float key = std::max(0.f, -.45f*nx + .65f*ny - .6124f*nz);
            const float fill = std::max(0.f, .65f*nx - .2f*ny - .7331f*nz);
            const float highlight = std::pow(std::max(0.f, -.32f*nx + .46f*ny - .8283f*nz), 40.f);
            const float light = .14f + .57f*key + .15f*fill + .09f*std::pow(1 + nz, 3.f);
            const std::array tint{1.f, .98f, .95f};
            const auto offset = (std::size_t(y) * side + x) * 4;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const float linear = std::clamp(light * tint[channel] + .1f * highlight, 0.f, 1.f);
                const float srgb = linear <= .0031308f ? linear * 12.92f :
                    1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
                pixels[offset + channel] = static_cast<std::uint8_t>(std::lround(srgb * 255));
            }
            pixels[offset + 3] = 255;
        }
        matcap = gfxCreateTexture2D(gfx, side, side, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        allocation(bool(matcap));
        BufferOwner upload{gfx, gfxCreateBuffer(gfx, pixels.size(), pixels.data(), kGfxCpuAccess_Write)};
        allocation(bool(upload.buffer));
        require(gfxCommandCopyBufferToTexture(gfx, matcap, upload.buffer), "Upload clay MatCap");
    }
    void check_debug() {
        if (!debug) return;
        const auto count = debug->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(debug->GetMessage(i, nullptr, &size))) continue;
            std::vector<std::byte> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(debug->GetMessage(i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw Error{{rhi::ErrorCode::execution_failed, kGfxResult_DeviceError, message->pDescription},
                            true};
        }
        debug->ClearStoredMessages();
    }
    template<class Pixels> GfxTexture texture(const std::shared_ptr<const Pixels> &owner) {
        constexpr bool color = std::is_same_v<Pixels, ui::ColorImage>;
        constexpr unsigned channels = color ? 4 : 1;
        const TextureKey key{owner->identity, owner->revision, color};
        auto [it, inserted] = images.try_emplace(key);
        auto &image = it->second;
        image.used = true;
        if (!inserted) return image.texture;
        image.owner = owner;
        image.texture = gfxCreateTexture2D(gfx, owner->width, owner->height,
            color ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8_UNORM);
        allocation(bool(image.texture));
        image.texel_bytes = std::uint64_t(owner->width) * owner->height * channels;
        image.cpu_bytes = owner->pixels.size();
        ++(color ? stats.cached_image_count : stats.cached_coverage_count);
        (color ? stats.cached_image_texel_bytes : stats.cached_coverage_texel_bytes) += image.texel_bytes;
        (color ? stats.cached_image_cpu_bytes : stats.cached_coverage_cpu_bytes) += image.cpu_bytes;
        // The protocol permits padded CPU rows; gfx's upload contract is tight.
        const auto pitch = std::size_t(owner->width) * channels;
        upload_pixels.resize(pitch * owner->height);
        for (std::uint32_t y = 0; y < owner->height; ++y)
            std::memcpy(upload_pixels.data() + std::size_t(y) * pitch,
                        owner->pixels.data() + std::size_t(y) * owner->row_pitch, pitch);
        BufferOwner upload{
            gfx, gfxCreateBuffer(gfx, upload_pixels.size(), upload_pixels.data(), kGfxCpuAccess_Write)};
        allocation(bool(upload.buffer));
        require(gfxCommandCopyBufferToTexture(gfx, image.texture, upload.buffer), "Upload UI texture");
        ++(color ? stats.image_upload_count : stats.coverage_upload_count);
        (color ? stats.image_upload_bytes : stats.coverage_upload_bytes) += upload_pixels.size();
        return image.texture;
    }
    void prepare_ui(const Frame &source) {
        const auto &frame = source.ui_frame;
        ui_instances.clear(); ui_batches.clear(); overlay_instances = 0;
        ui_instances.reserve(frame.draws.size() + source.overlay.size());
        const auto &viewport = source.scene_viewport;
        if (viewport.width > 0 && viewport.height > 0 && !source.overlay.empty()) {
            const ui::Clip clip{std::clamp(static_cast<int>(std::floor(viewport.x)), 0, int(frame.extent.width)),
                std::clamp(static_cast<int>(std::floor(viewport.y)), 0, int(frame.extent.height)),
                std::clamp(static_cast<int>(std::ceil(viewport.x + viewport.width)), 0, int(frame.extent.width)),
                std::clamp(static_cast<int>(std::ceil(viewport.y + viewport.height)), 0, int(frame.extent.height))};
            for (const auto &shape : source.overlay) {
                UiInstance instance{};
                instance.tint = shape.color;
                if (shape.kind == OverlayShape::Kind::segment) {
                    if (shape.color.a == 0 || shape.width == 0) continue;
                    const auto [ax, ay, bx, by] = shape.points;
                    const float pad = shape.width * .5f + 1;
                    instance.bounds = {std::min(ax, bx) - pad, std::min(ay, by) - pad,
                        std::abs(bx - ax) + 2 * pad, std::abs(by - ay) + 2 * pad};
                    instance.uv = {ax, ay, bx, by};
                    instance.radius = {shape.width * .5f, 0, 0, 0};
                    instance.reserved[0] = 1; // Capsule coverage in the solid pipeline.
                } else {
                    if ((shape.color.a == 0 && (shape.width == 0 || shape.border.a == 0)) ||
                        shape.points[2] == 0 || shape.points[3] == 0) continue;
                    instance.bounds = {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
                    instance.uv = {0, 0, 1, 1};
                    instance.border_color = shape.border;
                    instance.border_width = std::min({shape.width, shape.points[2] * .5f, shape.points[3] * .5f});
                }
                const auto first = static_cast<std::uint32_t>(ui_instances.size());
                ui_instances.push_back(instance);
                ++overlay_instances;
                if (!ui_batches.empty()) ++ui_batches.back().count;
                else ui_batches.push_back({first, 1, clip, {}, nullptr});
            }
        }
        overlay_batches = ui_batches.size();
        for (const auto &item : frame.draws) {
            const ui::Clip clip{std::clamp(item.clip.left, 0, int(frame.extent.width)),
                std::clamp(item.clip.top, 0, int(frame.extent.height)),
                std::clamp(item.clip.right, 0, int(frame.extent.width)),
                std::clamp(item.clip.bottom, 0, int(frame.extent.height))};
            if (clip.left >= clip.right || clip.top >= clip.bottom || item.bounds.width == 0 || item.bounds.height == 0 ||
                (item.color.a == 0 && (!item.image || item.image_background.a == 0) &&
                    (item.border_width == 0 || item.border_color.a == 0))) continue;
            TextureKey key{};
            if (item.coverage) key = {item.coverage->identity, item.coverage->revision, false};
            if (item.image) key = {item.image->identity, item.image->revision, true};
            const auto first = static_cast<std::uint32_t>(ui_instances.size());
            ui_instances.push_back({item.bounds, item.uv, item.color, item.border_color,
                fit_radius(item.radius, item.bounds), item.image_background,
                std::min({item.border_width, item.bounds.width * .5f, item.bounds.height * .5f})});
            if (ui_batches.size() > overlay_batches && ui_batches.back().clip == clip && ui_batches.back().texture == key)
                ++ui_batches.back().count;
            else ui_batches.push_back({first, 1, clip, key, &item});
        }
    }
    void draw_grid(const Frame& frame, bool with_depth) {
        if (!grid_camera) return;
        auto& pipeline=with_depth?grid_depth:grid;
        if (!pipeline)
            prepare_pipeline(pipeline, "viewport reference grid", shaders::viewport_vs, with_depth?shaders::grid_ps:shaders::grid_empty_ps,
                             with_depth?PipelineStyle::grid_depth:PipelineStyle::grid);
        require(gfxCommandBindDepthStencilTarget(gfx, with_depth?depth:GfxTexture{}), "Bind guide occlusion depth");
        const auto program=pipeline.program();
        const std::array<std::uint32_t,2> guides{frame.view.reference_grid?1u:0u,frame.view.origin_axes?1u:0u};
        require(gfxProgramSetParameter(gfx, program, "inverse_view_projection", grid_camera->inverse), "Set grid inverse camera");
        require(gfxProgramSetParameter(gfx, program, "view_projection", grid_camera->view_projection), "Set grid camera");
        require(gfxProgramSetParameter(gfx, program, "guides", guides), "Set reference guides");
        require(gfxProgramSetParameter(gfx, program, "grid_spacing", frame.view.grid_spacing), "Set grid spacing");
        require(gfxProgramSetParameter(gfx, program, "grid_plane", static_cast<std::uint32_t>(frame.view.grid_plane)), "Set grid plane");
        require(gfxCommandBindKernel(gfx, pipeline.kernel()), "Bind reference grid");
        require(gfxCommandDraw(gfx, 3), "Draw reference grid");
        ++stats.viewport_draw_calls;
    }
    void draw(const Frame &frame, const ui::PixelExtent extent, Capture *capture) {
        const auto &viewport = frame.scene_viewport;
        const bool has_viewport = viewport.width > 0 && viewport.height > 0;
        const bool draw_scene = !instances.empty() && has_viewport;
        require(gfxCommandClearBackBuffer(gfx), "Clear window");
        require(gfxCommandBindDepthStencilTarget(gfx, {}), "Unbind depth for viewport background");
        if (has_viewport) {
            require(gfxCommandSetViewport(gfx, viewport.x, viewport.y, viewport.width, viewport.height), "Set scene viewport");
            require(gfxCommandSetScissorRect(gfx, static_cast<int>(std::floor(viewport.x)),
                static_cast<int>(std::floor(viewport.y)),
                static_cast<int>(std::ceil(viewport.x+viewport.width))-static_cast<int>(std::floor(viewport.x)),
                static_cast<int>(std::ceil(viewport.y+viewport.height))-static_cast<int>(std::floor(viewport.y))), "Set scene scissor");
            if (frame.view.background == Background::gradient) {
                if (!background)
                    prepare_pipeline(background, "viewport background", shaders::viewport_vs, shaders::background_ps, PipelineStyle::background);
                require(gfxCommandBindKernel(gfx, background.kernel()), "Bind viewport background");
                require(gfxCommandDraw(gfx, 3), "Draw viewport background");
                ++stats.viewport_draw_calls;
            }
        }
        for (auto& [key, geometry] : geometries) { (void)key; geometry.used = false; }
        if (draw_scene) {
            const bool lines_only = frame.view.wireframe == Wireframe::only;
            const bool lighting=frame.view.shading==Shading::scene_lit&&!lines_only;
            const bool jittered=lighting&&(frame.view.lit.antialiasing==Antialiasing::taa||frame.view.lit.antialiasing==Antialiasing::smaa_t2x);
            const bool editing_depth=jittered&&(capture||frame.view.reference_grid||frame.view.origin_axes||
                frame.view.wireframe==Wireframe::overlay||std::ranges::any_of(frame.meshes,[](const MeshInstance& mesh){return mesh.selected;}));
            if ((lines_only||editing_depth)&&!scene_depth)
                prepare_pipeline(scene_depth, "editor scene depth", shaders::scene_vs, shaders::scene_ps, PipelineStyle::depth);
            if (!lighting && !lines_only && !scene)
                prepare_pipeline(scene, "editor surface", shaders::scene_vs, shaders::scene_ps, PipelineStyle::surface);
            if (lines_only) {
                auto& lines = frame.view.hidden_lines ? visible_lines : all_lines;
                if (!lines)
                    prepare_pipeline(lines, "editor scene lines", shaders::scene_vs, shaders::scene_ps,
                                     frame.view.hidden_lines?PipelineStyle::visible_lines:PipelineStyle::all_lines);
            }
            if(lighting&&!visible_lines)
                prepare_pipeline(visible_lines,"Scene Lit editing lines",shaders::scene_vs,shaders::scene_ps,PipelineStyle::visible_lines);
            if(!lighting)prepare_matcap();
            if (depth_extent != extent) {
                if (depth) require(gfxDestroyTexture(gfx, std::exchange(depth, {})), "Retire old depth");
                const float clear[4] = {0, 0, 0, 0};
                depth = gfxCreateTexture2D(gfx, extent.width, extent.height, DXGI_FORMAT_D32_FLOAT, 1, clear);
                allocation(bool(depth));
                depth_extent = extent;
            }
            require(gfxCommandClearTexture(gfx, depth), "Clear depth");
            BufferOwner data{gfx, gfxCreateBuffer<Instance>(gfx, static_cast<std::uint32_t>(instances.size()),
                                                            instances.data())};
            allocation(bool(data.buffer));
            require(gfxCommandBindDepthStencilTarget(gfx, depth), "Bind scene depth");
            for (const auto& batch : mesh_batches) {
                auto [found, inserted] = geometries.try_emplace(batch.geometry.get(), CachedGeometry{batch.geometry, {}, false});
                auto& geometry = found->second;
                const auto vertices = geometry.source->vertices();
                if (inserted) {
                    std::vector<SceneTriangle> triangles;
                    triangles.reserve(vertices.size() / 3);
                    for (std::size_t i = 0; i < vertices.size() / 3; ++i)
                        triangles.push_back(prepare_triangle(*geometry.source, i));
                    geometry.vertices = gfxCreateBuffer<SceneTriangle>(gfx,
                        static_cast<std::uint32_t>(triangles.size()), triangles.data());
                    allocation(bool(geometry.vertices));
                }
                geometry.used = true;
            }
            const auto draw_batches = [&](const rhi::Pipeline& pipeline, std::uint32_t pass) {
                const auto program = pipeline.program();
                const std::array<std::uint32_t, 4> display{
                    static_cast<std::uint32_t>(frame.view.shading), static_cast<std::uint32_t>(frame.view.wireframe),
                    frame.view.triangle_edges ? 1u : 0u, pass};
                const std::array<float, 2> wire_style{frame.view.wire_width, frame.view.wire_opacity};
                require(gfxProgramSetParameter(gfx, program, "instances", data.buffer), "Bind mesh instances");
                require(gfxProgramSetParameter(gfx, program, "view", frame.camera.view), "Set view matrix");
                require(gfxProgramSetParameter(gfx, program, "projection", frame.camera.projection), "Set projection matrix");
                require(gfxProgramSetParameter(gfx, program, "display", display), "Set viewport display policy");
                require(gfxProgramSetParameter(gfx, program, "wire_style", wire_style), "Set wire appearance");
                require(gfxProgramSetParameter(gfx, program, "matcap", matcap), "Bind MatCap");
                require(gfxProgramSetParameter(gfx, program, "matcap_sampler", sampler), "Bind MatCap sampler");
                require(gfxCommandBindKernel(gfx, pipeline.kernel()), "Bind scene pipeline");
                for (const auto& batch : mesh_batches) {
                    const auto& geometry = geometries.at(batch.geometry.get());
                    require(gfxProgramSetParameter(gfx, program, "triangles", geometry.vertices), "Bind mesh triangles");
                    require(gfxProgramSetParameter(gfx, program, "first_instance", batch.first), "Set mesh instance offset");
                    require(gfxCommandDraw(gfx, static_cast<std::uint32_t>(geometry.source->vertices().size()),
                                           batch.count), "Draw mesh batch");
                    ++stats.scene_draw_calls;
                }
            };
            if(lighting) {
                lit_draws.clear();lit_draws.reserve(mesh_batches.size());
                for(const auto& batch:mesh_batches){const auto& geometry=geometries.at(batch.geometry.get());
                    lit_draws.push_back({geometry.vertices,batch.first,batch.count,static_cast<std::uint32_t>(geometry.source->vertices().size())});}
                scene_lit->draw(frame,extent,depth,lit_draws,stats);
                require(gfxCommandBindDepthStencilTarget(gfx,depth),"Bind Scene Lit editing depth");
                if(editing_depth) {
                    // AA has consumed and retained its sampled depth. Editing
                    // guides and capture share the public, unjittered camera.
                    require(gfxCommandClearTexture(gfx,depth),"Clear sampled scene depth for editing");
                    draw_batches(scene_depth,2);
                }
                draw_grid(frame,true);
                draw_batches(visible_lines,1);
            } else if (lines_only) {
                draw_batches(scene_depth, 2);
                draw_grid(frame, true);
                draw_batches(frame.view.hidden_lines ? visible_lines : all_lines, 1);
            } else {
                draw_batches(scene, 0);
                draw_grid(frame, true);
            }
        } else if (has_viewport) draw_grid(frame, false);
        for (auto it = geometries.begin(); it != geometries.end();) {
            if (it->second.used) { ++it; continue; }
            require(gfxDestroyBuffer(gfx, it->second.vertices), "Retire unused mesh geometry");
            it = geometries.erase(it);
        }
        require(gfxCommandBindDepthStencilTarget(gfx, {}), "Unbind scene depth");
        require(gfxCommandSetViewport(gfx, 0, 0, static_cast<float>(extent.width),
                                      static_cast<float>(extent.height)),
                "Set UI viewport");
        for (auto &[key, image] : images)
            image.used = false;
        BufferOwner ui_data{gfx, {}};
        if (!ui_instances.empty()) {
            ui_data.buffer = gfxCreateBuffer<UiInstance>(gfx, static_cast<std::uint32_t>(ui_instances.size()), ui_instances.data());
            allocation(bool(ui_data.buffer));
            stats.ui_instance_upload_bytes = (ui_instances.size() - overlay_instances) * sizeof(UiInstance);
            stats.ui_quads = ui_instances.size() - overlay_instances;
            stats.overlay_shapes = overlay_instances;
        }
        for (std::size_t index = 0; index < ui_batches.size(); ++index) {
            const auto &batch = ui_batches[index];
            const auto *source = batch.source;
            const auto &pipeline = source && source->coverage ? coverage : source && source->image ? color_image : solid;
            const auto program = pipeline.program();
            if (source && source->coverage) {
                require(gfxProgramSetParameter(gfx, program, "coverage", texture(source->coverage)),
                        "Bind UI coverage");
                require(gfxProgramSetParameter(gfx, program, "coverage_sampler", sampler),
                        "Bind coverage sampler");
            }
            if (source && source->image) {
                require(gfxProgramSetParameter(gfx, program, "color_image", texture(source->image)), "Bind UI image");
                require(gfxProgramSetParameter(gfx, program, "image_sampler", sampler), "Bind image sampler");
            }
            const std::array<float, 2> size{static_cast<float>(extent.width),
                                            static_cast<float>(extent.height)};
            require(gfxProgramSetParameter(gfx, program, "ui_instances", ui_data.buffer), "Bind UI instances");
            require(gfxProgramSetParameter(gfx, program, "first_instance", batch.first), "Set UI instance offset");
            require(gfxProgramSetParameter(gfx, program, "target_size", size), "Set UI target");
            const auto &clip = batch.clip;
            require(gfxCommandSetScissorRect(gfx, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top), "Set UI scissor");
            require(gfxCommandBindKernel(gfx, pipeline.kernel()), "Bind UI pipeline");
            require(gfxCommandDraw(gfx, 6, batch.count), "Draw UI batch");
            ++(index < overlay_batches ? stats.overlay_draw_calls : stats.ui_draw_calls);
        }
        for (auto it = images.begin(); it != images.end();) {
            if (it->second.used) {
                ++it;
                continue;
            }
            require(gfxDestroyTexture(gfx, it->second.texture), "Retire unused UI texture");
            const bool color = it->first.color;
            --(color ? stats.cached_image_count : stats.cached_coverage_count);
            (color ? stats.cached_image_texel_bytes : stats.cached_coverage_texel_bytes) -= it->second.texel_bytes;
            (color ? stats.cached_image_cpu_bytes : stats.cached_coverage_cpu_bytes) -= it->second.cpu_bytes;
            it = images.erase(it);
        }
        if (capture) {
            const std::uint32_t pitch = (extent.width * 4 + 255) & ~255u;
            BufferOwner color{
                gfx, gfxCreateBuffer(gfx, std::uint64_t(pitch) * extent.height, nullptr, kGfxCpuAccess_Read)};
            allocation(bool(color.buffer));
            require(gfxCommandCopyBackBufferToBuffer(gfx, color.buffer, pitch), "Capture window color");
            BufferOwner z{gfx, {}};
            if (draw_scene) {
                z.buffer = gfxCreateBuffer(gfx, std::uint64_t(pitch) * extent.height, nullptr, kGfxCpuAccess_Read);
                allocation(bool(z.buffer));
                require(gfxCommandCopyTextureToBufferPitched(gfx, z.buffer, depth, pitch), "Capture scene depth");
            } else {
                // A UI-only capture describes this frame, not a retained scene depth texture.
                std::fill(capture->depth.begin(), capture->depth.end(), 0.0f);
            }
            const auto finished = context.finish();
            if (!finished) throw Error{finished.error(), true};
            const auto *rgba = static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, color.buffer));
            const auto *depth_bytes = draw_scene
                ? static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, z.buffer)) : nullptr;
            allocation(rgba && (!draw_scene || depth_bytes));
            for (std::uint32_t y = 0; y < extent.height; ++y) {
                std::memcpy(capture->rgba.data() + std::size_t(y) * extent.width * 4,
                            rgba + std::size_t(y) * pitch, extent.width * 4);
                if (depth_bytes)
                    std::memcpy(capture->depth.data() + std::size_t(y) * extent.width,
                                depth_bytes + std::size_t(y) * pitch, extent.width * 4);
            }
        }
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer &&) noexcept = default;
Renderer &Renderer::operator=(Renderer &&) noexcept = default;

Result<Renderer> Renderer::open(HWND window, const Options &options) {
    if (!window)
        return std::unexpected(
            error(rhi::ErrorCode::invalid_argument, "Renderer requires an application window"));
    try {
        Renderer renderer;
        renderer.state_ = std::make_unique<State>();
        auto &s = *renderer.state_;
        auto opened = rhi::Context::open({.window = window,
                                          .validation = options.validation,
                                          .wait_timeout_ms = options.wait_timeout_ms,
                                          .rgba8_window = true});
        if (!opened) return std::unexpected(Error{opened.error(), false});
        s.context = std::move(*opened);
        s.gfx = s.context.borrow();
        s.options = options;
        if (gfxGetBackBufferFormat(s.gfx) != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            return std::unexpected(
                error(rhi::ErrorCode::unsupported, "Editor renderer requires an sRGB RGBA8 window target"));
        if (options.validation != rhi::Validation::disabled)
            gfxGetDevice(s.gfx)->QueryInterface(IID_PPV_ARGS(&s.debug));
        s.prepare_pipeline(s.solid, "editor rectangles", shaders::ui_vs, shaders::solid_ps, PipelineStyle::ui);
        s.prepare_pipeline(s.coverage, "editor text coverage", shaders::ui_vs, shaders::coverage_ps, PipelineStyle::ui);
        s.prepare_pipeline(s.color_image, "editor color images", shaders::ui_vs, shaders::image_ps, PipelineStyle::ui);
        s.sampler = gfxCreateSamplerState(s.gfx, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
        allocation(bool(s.sampler));
        s.check_debug();
        return renderer;
    } catch (const Error &e) {
        auto result = e;
        result.terminal = false;
        return std::unexpected(std::move(result));
    } catch (const std::bad_alloc &) {
        return std::unexpected(error(rhi::ErrorCode::creation_failed, "Renderer allocation failed", false,
                                     kGfxResult_OutOfMemory));
    }
}
Result<Target> Renderer::target() const {
    if (!state_) return std::unexpected(error(rhi::ErrorCode::closed, "Renderer is closed"));
    if (const auto checked = state_->check(); !checked) return std::unexpected(checked.error());
    auto target = state_->context.target();
    if (!target) return std::unexpected(Error{target.error(), true});
    return Target{{target->width, target->height}, target->drawable};
}
Result<FrameResult> Renderer::render(const Frame &frame, Capture *capture) {
    const auto destination = target();
    if (!destination) return std::unexpected(destination.error());
    auto &s = *state_;
    s.stats.ui_draw_calls = 0;
    s.stats.ui_quads = 0;
    s.stats.ui_instance_upload_bytes = 0;
    s.stats.scene_draw_calls = 0;
    s.stats.viewport_draw_calls = 0;
    s.stats.overlay_shapes = 0;
    s.stats.overlay_draw_calls = 0;
    s.stats.coverage_upload_count = 0;
    s.stats.coverage_upload_bytes = 0;
    s.stats.image_upload_count = 0;
    s.stats.image_upload_bytes = 0;
    s.stats.scene_lit_gpu_ms={};s.stats.scene_lit_gpu_timing_valid=false;
    s.stats.scene_lit_resource_bytes=0;s.stats.scene_lit_shadow_rays=0;s.stats.scene_lit_probe_rays=0;
    s.stats.scene_lit_shadow_lights=0;s.stats.scene_lit_unshadowed_lights=0;s.stats.scene_lit_history_resets=0;
    s.stats.scene_lit_gpu_sample_sequence=0;s.stats.scene_lit_gpu_sample_frame_id=0;
    const auto extent = destination->extent;
    if (!destination->drawable || frame.ui_frame.extent != extent)
        return FrameResult{.accepted_extent = {}, .next_extent = extent, .stats = s.stats};
    bool encoding = false;
    try {
        validate(frame);
        if(frame.view.shading==Shading::scene_lit&&frame.view.wireframe!=Wireframe::only&&!frame.meshes.empty()&&frame.scene_viewport.width>0&&frame.scene_viewport.height>0) {
            if(!s.scene_lit)s.scene_lit=std::make_unique<detail::SceneLit>(s.context);
            s.scene_lit->validate(frame);
        } else if(s.scene_lit)s.scene_lit->invalidate();
        s.grid_camera=prepare_grid_camera(frame);
        s.instances.clear();
        s.instances.reserve(frame.meshes.size());
        s.mesh_batches.clear();
        for (const auto &mesh : frame.meshes) {
            const auto first = static_cast<std::uint32_t>(s.instances.size());
            s.instances.push_back(prepare_instance(mesh));
            if (!s.mesh_batches.empty() && s.mesh_batches.back().geometry == mesh.geometry)
                ++s.mesh_batches.back().count;
            else s.mesh_batches.push_back({first, 1, mesh.geometry});
        }
        s.prepare_ui(frame);
        Capture pending;
        if (capture) {
            pending.frame_id = frame.ui_frame.id;
            pending.extent = extent;
            pending.rgba.resize(std::size_t(extent.width) * extent.height * 4);
            pending.depth.resize(std::size_t(extent.width) * extent.height);
        }
        encoding = true;
        s.draw(frame, extent, capture ? &pending : nullptr);
        rhi::FrameInfo details;
        const auto presented = s.context.frame(s.options.vsync, &details);
        if (!presented) throw Error{presented.error(), true};
        s.check_debug();
        FrameResult result{.accepted_extent = {}, .next_extent = {details.next.width, details.next.height},
                           .stats = s.stats};
        if (!details.occluded) {
            result.status = FrameStatus::accepted;
            result.accepted_id = frame.ui_frame.id;
            result.accepted_extent = extent;
            if (capture) *capture = std::move(pending);
        }
        return result;
    } catch (Error e) {
        e.terminal = encoding;
        if (encoding) state_->failure = e;
        return std::unexpected(std::move(e));
    } catch (const std::bad_alloc &) {
        auto e = error(rhi::ErrorCode::creation_failed, "Renderer allocation failed", encoding,
                       kGfxResult_OutOfMemory);
        if (encoding) state_->failure = e;
        return std::unexpected(std::move(e));
    }
}
Result<void> Renderer::close() {
    if (!state_) return {};
    auto result = state_->close();
    if (result)
        state_.reset();
    else if (result.error().terminal)
        state_->failure = result.error();
    return result;
}
} // namespace atelier::renderer

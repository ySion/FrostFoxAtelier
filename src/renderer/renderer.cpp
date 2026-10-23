#include "renderer.hpp"
#include "shader_source.hpp"
#include "math/math.hpp"
#include "scene_lit.hpp"
#include "scene_geometry.hpp"
#include "frame_slots.hpp"
#include "raster_viewport.hpp"
#include "scene_visibility.hpp"
#include "timing.hpp"

#include <ankerl/unordered_dense.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace atelier::renderer::detail {
WeldedMesh weld(const geometry::Mesh& mesh) {
    // Merges only bitwise-identical corners: every vertex keeps its exact authored data.
    struct Hash {
        using is_transparent=void;using is_avalanching=void;
        const std::vector<geometry::Vertex>* unique;
        std::uint64_t operator()(const geometry::Vertex& v) const noexcept {
            return ankerl::unordered_dense::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(&v),sizeof v));
        }
        std::uint64_t operator()(std::uint32_t i) const noexcept {return (*this)((*unique)[i]);}
    };
    struct Equal {
        using is_transparent=void;
        const std::vector<geometry::Vertex>* unique;
        bool operator()(std::uint32_t a,std::uint32_t b) const noexcept {return a==b;}
        bool operator()(const geometry::Vertex& v,std::uint32_t i) const noexcept {return std::memcmp(&v,&(*unique)[i],sizeof v)==0;}
        bool operator()(std::uint32_t i,const geometry::Vertex& v) const noexcept {return (*this)(v,i);}
    };
    const auto source=mesh.vertices();
    WeldedMesh result;result.indices.resize(source.size());result.faces.resize(source.size()/3);
    ankerl::unordered_dense::set<std::uint32_t,Hash,Equal> seen(0,Hash{&result.vertices},Equal{&result.vertices});
    for(std::size_t i=0;i<source.size();++i) {
        if(const auto found=seen.find(source[i]);found!=seen.end()) {result.indices[i]=*found;continue;}
        const auto index=static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.push_back(source[i]);seen.insert(index);result.indices[i]=index;
    }
    for(const auto& part:mesh.parts())for(std::uint32_t t=part.first_vertex/3;t<(part.first_vertex+part.vertex_count)/3;++t) {
        const auto* v=source.data()+std::size_t(t)*3;
        std::array<double,3> a{},b{};
        for(std::size_t i=0;i<3;++i) {a[i]=double(v[1].position[i])-v[0].position[i];b[i]=double(v[2].position[i])-v[0].position[i];}
        const std::array<double,3> normal{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        const double length=std::hypot(normal[0],normal[1],normal[2]);
        double orientation=0;
        for(std::size_t i=0;i<3;++i)orientation+=normal[i]*(double(v[0].normal[i])+v[1].normal[i]+v[2].normal[i]);
        auto& face=result.faces[t];
        for(std::size_t i=0;i<3;++i)
            face.normal[i]=length==0?v[0].normal[i]:static_cast<float>(normal[i]/length*(orientation<0?-1:1));
        face.slot_edges=part.material|std::uint32_t(mesh.edge_mask(t))<<face_slot_bits;
    }
    return result;
}
}

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
float half_float(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
    if (exponent) return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
    return (sign ? -1.0f : 1.0f) * std::ldexp(float(mantissa), -24);
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
struct Instance {
    std::array<float, 16> world;
    std::array<float, 9> normal_matrix;
    float selected;
    float tangent_sign{1};
};
static_assert(sizeof(Instance) == 108 && sizeof(geometry::Vertex) == 48);
bool same_bits(float a, float b) noexcept { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
template<std::size_t N> bool same_bits(const std::array<float, N> &a, const std::array<float, N> &b) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        if (!same_bits(a[i], b[i])) return false;
    return true;
}
bool same_bits(const ui::Color &a, const ui::Color &b) noexcept {
    return same_bits(a.r, b.r) && same_bits(a.g, b.g) && same_bits(a.b, b.b) && same_bits(a.a, b.a);
}
bool same_bits(const Material &a, const Material &b) noexcept {
    return same_bits(a.roughness, b.roughness) && same_bits(a.metallic, b.metallic) && same_bits(a.emissive, b.emissive);
}
// Everything an instance check reads belongs to the instance itself.
bool valid_instance(const MeshInstance &mesh) {
    bool good = bool(mesh.geometry) && valid(mesh.color);
    if (mesh.surfaces) {
        good &= mesh.geometry && mesh.surfaces->size() == mesh.geometry->materials().size();
        for (const auto &surface : *mesh.surfaces) good &= bool(surface);
    }
    good &= finite(mesh.material.roughness) && mesh.material.roughness >= 0 && mesh.material.roughness <= 1 &&
        finite(mesh.material.metallic) && mesh.material.metallic >= 0 && mesh.material.metallic <= 1;
    for (float e : mesh.material.emissive) good &= finite(e) && e >= 0 && e <= material::emission_limit;
    for (float v : mesh.world) good &= finite(v);
    return good && mesh.world[3] == 0 && mesh.world[7] == 0 && mesh.world[11] == 0 && mesh.world[15] == 1;
}
// The last content at each Frame::meshes position that passed valid_instance, and
// the stable keys it holds in every shading mode. An unchanged position skips its
// check and reuses its prepared Instance. Geometry, surfaces and their images are
// immutable once published, so while an entry owns them pointer equality is content equality.
class MeshChecks {
public:
    struct Entry {
        std::array<float, 16> world{};
        ui::Color color{};
        Material material{};
        std::uint64_t stable_key{};
        geometry::MeshRef geometry;
        std::shared_ptr<const std::vector<material::SurfaceRef>> surfaces;
        std::uint32_t material_slots{};
        std::optional<Instance> prepared; // Set by the first frame that prepares this content.
    };
    // Adds the frame's material slot count; false means an invalid instance. Changes nothing.
    bool check(std::span<const MeshInstance> meshes, std::uint64_t &material_slots) {
        changed_.clear();
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const auto &mesh = meshes[i];
            if (i < entries_.size() && same(entries_[i], mesh)) {
                material_slots += entries_[i].material_slots;
                continue;
            }
            if (!valid_instance(mesh)) return false;
            material_slots += mesh.geometry->materials().size();
            changed_.push_back(static_cast<std::uint32_t>(i));
        }
        return true;
    }
    // Adopts the meshes of a frame whose renderer-level checks all passed.
    void commit(std::span<const MeshInstance> meshes) {
        for (std::size_t i = meshes.size(); i < entries_.size(); ++i) release(entries_[i].stable_key);
        if (entries_.size() != meshes.size()) materials_changed = true;
        entries_.resize(meshes.size());
        for (const auto i : changed_) {
            const auto &mesh = meshes[i];
            auto &entry = entries_[i];
            materials_changed |= entry.geometry != mesh.geometry || entry.surfaces != mesh.surfaces ||
                !same_bits(entry.color, mesh.color) || !same_bits(entry.material, mesh.material);
            // Only acquire allocates; once it succeeds the rest of this position's update cannot throw.
            if (entry.stable_key != mesh.stable_key) {
                acquire(mesh.stable_key);
                release(entry.stable_key);
            }
            entry = {mesh.world, mesh.color, mesh.material, mesh.stable_key, mesh.geometry, mesh.surfaces,
                     static_cast<std::uint32_t>(mesh.geometry->materials().size()), std::nullopt};
        }
        changed_.clear();
    }
    [[nodiscard]] Entry &operator[](std::size_t i) noexcept { return entries_[i]; }
    // Nonzero stable keys held by more than one position of the committed frame.
    [[nodiscard]] bool duplicate_keys() const noexcept { return duplicates_ != 0; }
    // The committed geometry, surfaces or constant materials differ from the built material tables.
    bool materials_changed{true};

private:
    // Every MeshInstance field but selected; a new field must join same() and valid_instance.
    static_assert(sizeof(MeshInstance) == 152);
    // Checked entries have geometry; a default entry left by a failed commit never matches.
    static bool same(const Entry &entry, const MeshInstance &mesh) noexcept {
        return entry.geometry && entry.geometry == mesh.geometry && entry.surfaces == mesh.surfaces && entry.stable_key == mesh.stable_key &&
            same_bits(entry.world, mesh.world) && same_bits(entry.color, mesh.color) && same_bits(entry.material, mesh.material);
    }
    void acquire(std::uint64_t key) {
        if (key && ++uses_[key] == 2) ++duplicates_;
    }
    void release(std::uint64_t key) {
        if (!key) return;
        const auto at = uses_.find(key);
        if (--at->second == 1) --duplicates_;
        else if (at->second == 0) uses_.erase(at);
    }
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> changed_;
    std::unordered_map<std::uint64_t, std::uint32_t> uses_;
    std::uint32_t duplicates_{};
};
void validate(const Frame &frame, MeshChecks &meshes) {
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
    const auto& cavity=frame.view.cavity;
    good &= finite(cavity.ridge)&&cavity.ridge>=0&&cavity.ridge<=2&&
        finite(cavity.valley)&&cavity.valley>=0&&cavity.valley<=2&&
        finite(cavity.radius)&&cavity.radius>=.5f&&cavity.radius<=4;
    for (float v : frame.camera.view)
        good &= finite(v);
    for (float v : frame.camera.projection)
        good &= finite(v);
    std::uint64_t material_slots=0;
    good &= meshes.check(frame.meshes, material_slots);
    good &= material_slots<=std::numeric_limits<std::uint32_t>::max()/sizeof(detail::GpuSurface);
    static_assert(std::numeric_limits<std::uint32_t>::max()/sizeof(detail::GpuSurface)<(1u<<detail::face_slot_bits),
        "An accepted Frame's material slots fit GpuFace");
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
    meshes.commit(frame.meshes);
}
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
std::optional<std::array<float,16>> prepare_cavity_projection(const Frame& frame) {
    const auto& c=frame.view.cavity;
    if(!c.enabled||(c.ridge==0&&c.valley==0)||frame.view.shading==Shading::scene_lit||
       frame.view.wireframe==Wireframe::only||frame.meshes.empty()||frame.scene_viewport.width==0||frame.scene_viewport.height==0)return {};
    math::Matrix4d projection;
    for(std::size_t i=0;i<16;++i)projection.elements[i]=frame.camera.projection[i];
    const auto inverse=math::try_inverse(projection);if(!inverse)return {};
    std::array<float,16> result;
    for(std::size_t i=0;i<16;++i) {
        result[i]=static_cast<float>(inverse->elements[i]);if(!finite(result[i]))return {};
    }
    return result;
}
enum class PipelineStyle { ui, surface, cavity_surface, depth, visible_lines, all_lines, background, grid, grid_depth };
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
    Instance result{mesh.world, {}, mesh.selected ? 1.0f : 0.0f, determinant<0?-1.f:1.f};
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
    std::uint32_t slot{}; // Index in this frame's ui_textures while used.
    std::uint64_t texel_bytes{}, cpu_bytes{};
};
enum class UiShading : std::uint32_t { solid, coverage, image, capsule };
struct UiInstance {
    ui::Rect bounds, uv;
    ui::Color tint, border_color;
    ui::CornerRadii radius;
    ui::Color image_background;
    float border_width{};
    std::uint32_t shading{}; // UiShading; textured kinds add their ui_textures slot << 2.
    std::array<std::uint32_t, 2> clip{}; // Half-open pixels: left | top << 16, right | bottom << 16.
};
static_assert(sizeof(UiInstance) == 112);
std::array<std::uint32_t, 2> pack_clip(const ui::Clip &c) {
    return {std::uint32_t(c.left) | std::uint32_t(c.top) << 16, std::uint32_t(c.right) | std::uint32_t(c.bottom) << 16};
}
// Conservative: a quad that misses the clip cannot cover any pixel center inside it.
bool overlaps(const ui::Rect &bounds, const ui::Clip &c) {
    return bounds.x < float(c.right) && bounds.x + bounds.width > float(c.left) &&
        bounds.y < float(c.bottom) && bounds.y + bounds.height > float(c.top);
}
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
    rhi::Pipeline scene, scene_depth, visible_lines, all_lines, ui_pipeline;
    rhi::Pipeline background, grid, grid_depth;
    rhi::Pipeline cavity_surface,cavity_composite;
    GfxTexture cavity_normals,cavity_color;
    ui::PixelExtent cavity_extent{};
    std::optional<std::array<float,16>> cavity_projection;
    std::optional<GridCamera> grid_camera;
    std::array<float,4> raster_transform{1,1,0,0};
    GfxTexture matcap;
    GfxSamplerState sampler;
    struct MaterialImage { material::ImageRef owner; GfxTexture texture; bool used{}; };
    std::map<std::pair<const material::Image*,bool>,MaterialImage> material_images;
    material::ImageRef white_image;
    // group is this frame's MeshGroup only while that group points back at the entry.
    struct CachedGeometry { geometry::MeshRef source; detail::SceneGeometry gpu; bool used{}; std::uint32_t group{}; };
    // One per geometry in first-appearance order over the complete snapshot, so
    // visibility and selection never reorder draws. Ranges index draw_instances.
    struct MeshGroup { CachedGeometry* geometry; std::uint32_t visible_first{}, visible_count{}, selected_first{}, selected_count{}; };
    std::unordered_map<const geometry::Mesh*, CachedGeometry> geometries;
    std::vector<MeshGroup> mesh_groups;
    // Snapshot index -> group; original snapshot indices of visible, then visible selected instances.
    std::vector<std::uint32_t> instance_groups, draw_instances;
    detail::FrameSlots<std::uint32_t> draw_order_slots; // draw_instances, read in place.
    detail::FrameSlots<Instance> instance_slots;
    std::unique_ptr<detail::SceneLit> scene_lit;
    std::vector<detail::SceneLitDraw> lit_draws;
    detail::MaterialFrame materials;
    struct MaterialCaptureKey {
        geometry::MeshRef geometry;
        std::shared_ptr<const std::vector<material::SurfaceRef>> surfaces;
        ui::Color color;
        float roughness{},metallic{};
        std::array<float,3> emissive{};
        bool operator==(const MaterialCaptureKey&) const=default;
    };
    std::vector<MaterialCaptureKey> material_keys,next_material_keys;
    MeshChecks mesh_checks;
    std::vector<std::pair<const material::Image*,bool>> used_material_images;
    std::vector<detail::GpuSurface> next_surfaces;
    std::vector<std::uint32_t> next_material_indices,next_material_offsets;
    GfxTexture depth;
    ui::PixelExtent depth_extent;
    std::unordered_map<TextureKey, CachedImage, TextureHash> images;
    FrameStats stats;
    std::vector<Instance> instances;
    std::vector<std::uint8_t> raster_visibility;
    std::vector<UiInstance> ui_instances;
    std::size_t overlay_instances{}; // Leading viewport annotations in ui_instances.
    // Textured UI instances; their slots are resolved while encoding, when uploads can be recorded.
    std::vector<std::pair<std::uint32_t, const ui::DrawQuad *>> ui_textured;
    std::vector<GfxTexture> ui_textures; // This frame's distinct UI images in first-use order.
    detail::FrameSlots<UiInstance> ui_slots; // Overlay then UI instances, read in place.
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
        for(auto& [key,image]:material_images) {
            (void)key;
            const auto r=gfxDestroyTexture(gfx,image.texture);
            if(r) return std::unexpected(error(rhi::ErrorCode::execution_failed,"Material texture retirement failed",true,r));
            image.texture={};
        }
        material_images.clear();white_image.reset();
        for(auto* buffer:{&materials.surface_buffer,&materials.index_buffer,&materials.offset_buffer})if(*buffer) {
            const auto result=gfxDestroyBuffer(gfx,*buffer);
            if(result)return std::unexpected(error(rhi::ErrorCode::execution_failed,"Material table retirement failed",true,result));
            *buffer={};
        }
        for(auto& material_sampler:materials.samplers)if(material_sampler) {
            const auto result=gfxDestroySamplerState(gfx,material_sampler);
            if(result)return std::unexpected(error(rhi::ErrorCode::execution_failed,"Material sampler retirement failed",true,result));
            material_sampler={};
        }
        material_keys.clear();next_material_keys.clear();used_material_images.clear();
        materials.descriptions.clear();materials.textures.clear();materials.surfaces.clear();materials.indices.clear();materials.offsets.clear();++materials.revision;
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
        if(cavity_normals) {
            const auto r=gfxDestroyTexture(gfx,cavity_normals);
            if(r)return std::unexpected(error(rhi::ErrorCode::execution_failed,"Cavity normal retirement failed",true,r));
            cavity_normals={};
        }
        if(cavity_color) {
            const auto r=gfxDestroyTexture(gfx,cavity_color);
            if(r)return std::unexpected(error(rhi::ErrorCode::execution_failed,"Cavity color retirement failed",true,r));
            cavity_color={};
        }
        for (auto& [key, geometry] : geometries) {
            (void)key;
            for (auto* buffer : {&geometry.gpu.vertices, &geometry.gpu.indices, &geometry.gpu.faces}) {
                if (!*buffer) continue;
                const auto r = gfxDestroyBuffer(gfx, *buffer);
                if (r) return std::unexpected(error(rhi::ErrorCode::execution_failed, "Geometry retirement failed", true, r));
                *buffer = {};
            }
        }
        geometries.clear(); mesh_groups.clear();
        for(const auto r:{draw_order_slots.retire(gfx),instance_slots.retire(gfx)})if(r)
            return std::unexpected(error(rhi::ErrorCode::execution_failed,"Scene frame slot retirement failed",true,r));
        if(const auto r=ui_slots.retire(gfx))
            return std::unexpected(error(rhi::ErrorCode::execution_failed,"UI frame slot retirement failed",true,r));
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
        ui_pipeline = {};
        background = {};
        grid = {};
        grid_depth = {};
        cavity_surface={};cavity_composite={};
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
        if(style==PipelineStyle::cavity_surface)
            require(gfxDrawStateSetColorTarget(desc.draw_state,1,DXGI_FORMAT_R16G16B16A16_FLOAT),"Set cavity normal target");
        require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set culling");
        const bool surface=style==PipelineStyle::surface||style==PipelineStyle::cavity_surface;
        const bool with_depth = surface || style == PipelineStyle::depth ||
            style == PipelineStyle::visible_lines || style == PipelineStyle::all_lines || style == PipelineStyle::grid_depth;
        const bool write_depth = surface || style == PipelineStyle::depth;
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
        if (!surface && style != PipelineStyle::background)
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
    GfxTexture material_texture(material::ImageRef source,bool srgb) {
        if(!source) {
            if(!white_image) white_image=std::make_shared<const material::Image>(1,1,std::vector<std::uint8_t>{255,255,255,255});
            source=white_image;
        }
        auto [found,inserted]=material_images.try_emplace({source.get(),srgb},MaterialImage{source,{},true});
        auto& image=found->second;image.used=true;
        if(!inserted) return image.texture;
        image.texture=gfxCreateTexture2D(gfx,source->width(),source->height(),
            srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM,
            gfxCalculateMipCount(source->width(),source->height()));
        allocation(bool(image.texture));
        BufferOwner upload{gfx,gfxCreateBuffer(gfx,source->pixels().size(),source->pixels().data(),kGfxCpuAccess_Write)};
        allocation(bool(upload.buffer));
        require(gfxCommandCopyBufferToTexture(gfx,image.texture,upload.buffer),"Upload material texture");
        require(gfxCommandGenerateMips(gfx,image.texture),"Generate material mipmaps");
        ++stats.material_upload_count;return image.texture;
    }
    void prepare_materials(const Frame& frame) {
        if(materials.surface_buffer&&!mesh_checks.materials_changed) {
            for(const auto& key:used_material_images)material_images.at(key).used=true;
            return;
        }
        next_material_keys.clear();next_material_keys.reserve(frame.meshes.size());
        for(const auto& mesh:frame.meshes) {
            MaterialCaptureKey key{mesh.geometry,mesh.surfaces,{1,1,1,1},0,0,{}};
            if(!mesh.surfaces) {key.color=mesh.color;key.roughness=mesh.material.roughness;key.metallic=mesh.material.metallic;key.emissive=mesh.material.emissive;}
            next_material_keys.push_back(std::move(key));
        }
        if(materials.surface_buffer&&material_keys==next_material_keys) {
            for(const auto& key:used_material_images)material_images.at(key).used=true;
            next_material_keys.clear();mesh_checks.materials_changed=false;return;
        }
        material_keys.swap(next_material_keys);next_material_keys.clear();used_material_images.clear();
        materials.descriptions.clear();materials.textures.clear();++materials.revision;
        next_surfaces.clear();next_material_indices.clear();next_material_offsets.clear();
        std::map<std::pair<const material::Image*,bool>,std::uint32_t> texture_indices;
        std::map<detail::GpuSurface,std::uint32_t> surface_indices;
        materials.textures.push_back(material_texture({},true));
        used_material_images.emplace_back(white_image.get(),true);
        for(const auto& mesh:frame.meshes) {
            next_material_offsets.push_back(static_cast<std::uint32_t>(materials.descriptions.size()));
            for(std::size_t slot=0;slot<mesh.geometry->materials().size();++slot) {
                const auto value=detail::resolved_surface(mesh,slot);materials.descriptions.push_back(value);
                const auto& p=value.parameters;detail::GpuSurface gpu{};
                gpu.base_roughness={p.base_color[0],p.base_color[1],p.base_color[2],p.roughness};
                gpu.emissive_metallic={p.emissive[0],p.emissive[1],p.emissive[2],p.metallic};
                gpu.coverage_normal={p.base_color[3],p.alpha_cutoff,p.normal_scale,p.normal_y_sign};
                gpu.flags={static_cast<std::uint32_t>(p.coverage),p.double_sided?1u:0u,p.emission_two_sided?1u:0u,0};
                for(std::size_t input=0;input<material::input_count;++input) {
                    const auto& t=value.textures[input];auto& binding=gpu.inputs[input];
                    binding.scale_offset={t.scale[0],t.scale[1],t.offset[0],t.offset[1]};
                    binding.rotation_size={std::cos(t.rotation),std::sin(t.rotation),1,1};
                    binding.meta={0,t.channel,static_cast<std::uint32_t>(t.address),t.image?1u:0u};
                    if(t.image) {
                        const bool srgb=input==0||input==4;
                        auto [found,added]=texture_indices.try_emplace({t.image.get(),srgb},static_cast<std::uint32_t>(materials.textures.size()));
                        if(added) {materials.textures.push_back(material_texture(t.image,srgb));used_material_images.emplace_back(t.image.get(),srgb);}
                        binding.meta[0]=found->second;
                        binding.rotation_size[2]=static_cast<float>(t.image->width());binding.rotation_size[3]=static_cast<float>(t.image->height());
                    }
                }
                auto [found,added]=surface_indices.try_emplace(gpu,static_cast<std::uint32_t>(next_surfaces.size()));
                if(added)next_surfaces.push_back(gpu);
                next_material_indices.push_back(found->second);
            }
        }
        const auto upload=[&]<class T>(GfxBuffer& buffer,std::vector<T>& current,std::vector<T>& next) {
            if(buffer&&current==next)return;
            if(buffer)require(gfxDestroyBuffer(gfx,std::exchange(buffer,{})),"Retire material table");
            buffer=gfxCreateBuffer<T>(gfx,static_cast<std::uint32_t>(next.size()),next.data());allocation(bool(buffer));current.swap(next);
        };
        upload(materials.surface_buffer,materials.surfaces,next_surfaces);
        upload(materials.index_buffer,materials.indices,next_material_indices);
        upload(materials.offset_buffer,materials.offsets,next_material_offsets);
        constexpr D3D12_TEXTURE_ADDRESS_MODE addresses[]{D3D12_TEXTURE_ADDRESS_MODE_WRAP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
        for(std::size_t i=0;i<materials.samplers.size();++i)if(!materials.samplers[i]) {
            materials.samplers[i]=gfxCreateSamplerState(gfx,D3D12_FILTER_ANISOTROPIC,addresses[i],addresses[i]);allocation(bool(materials.samplers[i]));
        }
        mesh_checks.materials_changed=false;
    }
    // Returns the image's slot in this frame's ui_textures; a new revision uploads once.
    template<class Pixels> std::uint32_t texture(const std::shared_ptr<const Pixels> &owner) {
        constexpr bool color = std::is_same_v<Pixels, ui::ColorImage>;
        constexpr unsigned channels = color ? 4 : 1;
        const TextureKey key{owner->identity, owner->revision, color};
        auto [it, inserted] = images.try_emplace(key);
        auto &image = it->second;
        if (inserted) {
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
        }
        if (!image.used) {
            image.used = true;
            image.slot = static_cast<std::uint32_t>(ui_textures.size());
            ui_textures.push_back(image.texture);
        }
        return image.slot;
    }
    void prepare_ui(const Frame &source) {
        const auto &frame = source.ui_frame;
        ui_instances.clear(); ui_textured.clear(); overlay_instances = 0;
        ui_instances.reserve(frame.draws.size() + source.overlay.size());
        const auto &viewport = source.scene_viewport;
        if (viewport.width > 0 && viewport.height > 0 && !source.overlay.empty()) {
            const auto clip=detail::raster_viewport(viewport).scissor;
            for (const auto &shape : source.overlay) {
                UiInstance instance{};
                instance.tint = shape.color;
                instance.clip = pack_clip(clip);
                if (shape.kind == OverlayShape::Kind::segment) {
                    if (shape.color.a == 0 || shape.width == 0) continue;
                    const auto [ax, ay, bx, by] = shape.points;
                    const float pad = shape.width * .5f + 1;
                    instance.bounds = {std::min(ax, bx) - pad, std::min(ay, by) - pad,
                        std::abs(bx - ax) + 2 * pad, std::abs(by - ay) + 2 * pad};
                    instance.uv = {ax, ay, bx, by};
                    instance.radius = {shape.width * .5f, 0, 0, 0};
                    instance.shading = std::to_underlying(UiShading::capsule);
                } else {
                    if ((shape.color.a == 0 && (shape.width == 0 || shape.border.a == 0)) ||
                        shape.points[2] == 0 || shape.points[3] == 0) continue;
                    instance.bounds = {shape.points[0], shape.points[1], shape.points[2], shape.points[3]};
                    instance.uv = {0, 0, 1, 1};
                    instance.border_color = shape.border;
                    instance.border_width = std::min({shape.width, shape.points[2] * .5f, shape.points[3] * .5f});
                }
                if (overlaps(instance.bounds, clip)) ui_instances.push_back(instance);
            }
            overlay_instances = ui_instances.size();
        }
        for (const auto &item : frame.draws) {
            const ui::Clip clip{std::clamp(item.clip.left, 0, int(frame.extent.width)),
                std::clamp(item.clip.top, 0, int(frame.extent.height)),
                std::clamp(item.clip.right, 0, int(frame.extent.width)),
                std::clamp(item.clip.bottom, 0, int(frame.extent.height))};
            if (clip.left >= clip.right || clip.top >= clip.bottom || item.bounds.width == 0 || item.bounds.height == 0 ||
                !overlaps(item.bounds, clip) ||
                (item.color.a == 0 && (!item.image || item.image_background.a == 0) &&
                    (item.border_width == 0 || item.border_color.a == 0))) continue;
            const auto shading = item.coverage ? UiShading::coverage : item.image ? UiShading::image : UiShading::solid;
            if (shading != UiShading::solid)
                ui_textured.emplace_back(static_cast<std::uint32_t>(ui_instances.size()), &item);
            ui_instances.push_back({item.bounds, item.uv, item.color, item.border_color,
                fit_radius(item.radius, item.bounds), item.image_background,
                std::min({item.border_width, item.bounds.width * .5f, item.bounds.height * .5f}),
                std::to_underlying(shading), pack_clip(clip)});
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
        require(gfxProgramSetParameter(gfx,program,"raster_viewport_transform",raster_transform),"Set grid raster viewport");
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
    void draw_cavity(const Frame& frame) {
        if(!cavity_composite)
            prepare_pipeline(cavity_composite,"studio screen cavity",shaders::viewport_vs,shaders::cavity_ps,PipelineStyle::ui);
        require(gfxCommandBindDepthStencilTarget(gfx,{}),"Unbind depth for cavity sampling");
        const auto program=cavity_composite.program();
        require(gfxProgramSetParameter(gfx,program,"raster_viewport_transform",raster_transform),"Set cavity raster viewport");
        const auto& v=frame.scene_viewport;const auto& c=frame.view.cavity;
        const std::array<float,4> viewport{v.x,v.y,v.width,v.height},controls{c.ridge,c.valley,c.radius,0};
        require(gfxProgramSetParameter(gfx,program,"cavity_normals",cavity_normals),"Bind cavity normals");
        require(gfxProgramSetParameter(gfx,program,"cavity_color",cavity_color),"Bind studio color");
        require(gfxProgramSetParameter(gfx,program,"cavity_depth",depth),"Bind cavity depth");
        require(gfxProgramSetParameter(gfx,program,"cavity_inverse_projection",*cavity_projection),"Set cavity projection");
        require(gfxProgramSetParameter(gfx,program,"cavity_viewport",viewport),"Set cavity viewport");
        require(gfxProgramSetParameter(gfx,program,"cavity_controls",controls),"Set cavity controls");
        require(gfxCommandBindKernel(gfx,cavity_composite.kernel()),"Bind cavity pass");
        require(gfxCommandDraw(gfx,3),"Draw studio cavity");++stats.viewport_draw_calls;
    }
    void draw(const Frame &frame, const ui::PixelExtent extent, Capture *capture) {
        const auto &viewport = frame.scene_viewport;
        const bool has_viewport = viewport.width > 0 && viewport.height > 0;
        const bool draw_scene = !instances.empty() && has_viewport;
        if(!cavity_projection||cavity_extent!=extent) {
            if(cavity_normals)require(gfxDestroyTexture(gfx,std::exchange(cavity_normals,{})),"Retire unused cavity normals");
            if(cavity_color)require(gfxDestroyTexture(gfx,std::exchange(cavity_color,{})),"Retire unused studio color");
            cavity_extent={};
        }
        require(gfxCommandClearBackBuffer(gfx), "Clear window");
        require(gfxCommandBindDepthStencilTarget(gfx, {}), "Unbind depth for viewport background");
        if (has_viewport) {
            const auto raster=detail::raster_viewport(viewport);
            raster_transform=raster.clip_transform;
            require(gfxCommandSetViewport(gfx,raster.hardware.x,raster.hardware.y,raster.hardware.width,raster.hardware.height),"Set scene raster viewport");
            require(gfxCommandSetScissorRect(gfx,raster.scissor.left,raster.scissor.top,
                raster.scissor.right-raster.scissor.left,raster.scissor.bottom-raster.scissor.top),"Set scene pixel-center scissor");
            if (frame.view.shading != Shading::scene_lit && frame.view.background == Background::gradient) {
                if (!background)
                    prepare_pipeline(background, "viewport background", shaders::viewport_vs, shaders::background_ps, PipelineStyle::background);
                require(gfxProgramSetParameter(gfx,background.program(),"raster_viewport_transform",raster_transform),"Set background raster viewport");
                require(gfxCommandBindKernel(gfx, background.kernel()), "Bind viewport background");
                require(gfxCommandDraw(gfx, 3), "Draw viewport background");
                ++stats.viewport_draw_calls;
            }
        }
        for (auto& [key, geometry] : geometries) { (void)key; geometry.used = false; }
        for(auto& [key,image]:material_images) { (void)key;image.used=false; }
        if (draw_scene) {
            timing::Scope scene_stage{phases::encode_scene};
            {
                timing::Scope materials_stage{phases::encode_materials};
                prepare_materials(frame);
            }
            const bool lines_only = frame.view.wireframe == Wireframe::only;
            const bool lighting=frame.view.shading==Shading::scene_lit&&!lines_only;
            const bool selected=std::ranges::any_of(frame.meshes,[](const MeshInstance& mesh){return mesh.selected;});
            const bool editing_lines=frame.view.wireframe==Wireframe::overlay||selected;
            const bool jittered=lighting&&(frame.view.lit.antialiasing==Antialiasing::taa||frame.view.lit.antialiasing==Antialiasing::smaa_t2x);
            const bool editing_depth=jittered&&(capture||frame.view.reference_grid||frame.view.origin_axes||
                frame.view.lit.probe_overlay()||editing_lines);
            if ((lines_only||editing_depth)&&!scene_depth)
                prepare_pipeline(scene_depth, "editor scene depth", shaders::scene_vs, shaders::scene_ps, PipelineStyle::depth);
            if (!lighting && !lines_only && !cavity_projection && !scene)
                prepare_pipeline(scene, "editor surface", shaders::scene_vs, shaders::scene_ps, PipelineStyle::surface);
            if(cavity_projection) {
                if(!cavity_surface)prepare_pipeline(cavity_surface,"studio surface normals",shaders::scene_vs,shaders::cavity_surface_ps,PipelineStyle::cavity_surface);
                if(!cavity_normals) {
                    constexpr float empty[4]{};
                    cavity_normals=gfxCreateTexture2D(gfx,extent.width,extent.height,DXGI_FORMAT_R16G16B16A16_FLOAT,1,empty);
                    allocation(bool(cavity_normals));
                    cavity_color=gfxCreateTexture2D(gfx,extent.width,extent.height,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
                    allocation(bool(cavity_color));cavity_extent=extent;
                }
                require(gfxCommandClearTexture(gfx,cavity_normals),"Clear cavity coverage");
            }
            if (lines_only) {
                auto& lines = frame.view.hidden_lines ? visible_lines : all_lines;
                if (!lines)
                    prepare_pipeline(lines, "editor scene lines", shaders::scene_vs, shaders::scene_ps,
                                     frame.view.hidden_lines?PipelineStyle::visible_lines:PipelineStyle::all_lines);
            }
            if((lighting||cavity_projection)&&editing_lines&&!visible_lines)
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
            GfxBuffer instance_data{}, order{};
            require(gfxCommandBindDepthStencilTarget(gfx, depth), "Bind scene depth");
            {
                timing::Scope upload_stage{phases::encode_scene_upload};
                for (const auto& group : mesh_groups) {
                    auto& geometry = *group.geometry;
                    if (!geometry.gpu.vertices) {
                        const auto welded = detail::weld(*geometry.source);
                        auto& gpu = geometry.gpu;
                        gpu.vertices = gfxCreateBuffer<geometry::Vertex>(gfx, static_cast<std::uint32_t>(welded.vertices.size()), welded.vertices.data());
                        allocation(bool(gpu.vertices));
                        gpu.indices = gfxCreateBuffer<std::uint32_t>(gfx, static_cast<std::uint32_t>(welded.indices.size()), welded.indices.data());
                        allocation(bool(gpu.indices));
                        gpu.faces = gfxCreateBuffer<detail::GpuFace>(gfx, static_cast<std::uint32_t>(welded.faces.size()), welded.faces.data());
                        allocation(bool(gpu.faces));
                        gpu.triangle_count = static_cast<std::uint32_t>(welded.faces.size());
                    }
                    geometry.used = true;
                }
                if (!draw_instances.empty())
                    order = draw_order_slots.write(gfx, context.info().frames_in_flight, draw_instances, true, "Draw order");
            }
            std::array<float,16> view_projection{};
            const auto draw_batches = [&](const rhi::Pipeline& pipeline, std::uint32_t pass) {
                const bool selected_only=(pass==3||(pass==1&&lighting))&&frame.view.wireframe!=Wireframe::overlay;
                if(selected_only&&!selected)return;
                if(!order)return; // Nothing visible.
                if(!instance_data) {
                    timing::Scope instance_upload{phases::encode_scene_upload};
                    // Instance transforms were validated before encoding. Upload
                    // editing data only when an actual raster pass consumes it.
                    instance_data=instance_slots.write(gfx,context.info().frames_in_flight,instances,true,"Scene instance");
                    // Match SceneLit's CPU-composed view/projection before the
                    // precise clip path; chained GPU matrices differ by ULPs.
                    math::Matrix4d camera_view,camera_projection;
                    for(std::size_t i=0;i<16;++i) {camera_view.elements[i]=frame.camera.view[i];camera_projection.elements[i]=frame.camera.projection[i];}
                    const auto combined=camera_view*camera_projection;
                    for(std::size_t i=0;i<16;++i)view_projection[i]=static_cast<float>(combined.elements[i]);
                }
                const auto program = pipeline.program();
                require(gfxProgramSetParameter(gfx,program,"raster_viewport_transform",raster_transform),"Set scene raster viewport");
                detail::bind_materials(gfx,program,materials);
                const std::array<std::uint32_t, 4> display{
                    static_cast<std::uint32_t>(frame.view.shading), static_cast<std::uint32_t>(frame.view.wireframe),
                    frame.view.triangle_edges ? 1u : 0u, pass};
                const std::array<float, 2> wire_style{frame.view.wire_width, frame.view.wire_opacity};
                require(gfxProgramSetParameter(gfx, program, "instances", instance_data), "Bind mesh instances");
                require(gfxProgramSetParameter(gfx, program, "draw_instances", order), "Bind mesh draw order");
                require(gfxProgramSetParameter(gfx, program, "view", frame.camera.view), "Set view matrix");
                require(gfxProgramSetParameter(gfx, program, "projection", frame.camera.projection), "Set projection matrix");
                require(gfxProgramSetParameter(gfx, program, "scene_view_projection", view_projection), "Set raster view projection");
                require(gfxProgramSetParameter(gfx, program, "display", display), "Set viewport display policy");
                require(gfxProgramSetParameter(gfx, program, "wire_style", wire_style), "Set wire appearance");
                require(gfxProgramSetParameter(gfx, program, "matcap", matcap), "Bind MatCap");
                require(gfxProgramSetParameter(gfx, program, "matcap_sampler", sampler), "Bind MatCap sampler");
                require(gfxCommandBindKernel(gfx, pipeline.kernel()), "Bind scene pipeline");
                timing::Scope draws_stage{phases::encode_draws};
                for (const auto& group : mesh_groups) {
                    const auto first=selected_only?group.selected_first:group.visible_first;
                    const auto count=selected_only?group.selected_count:group.visible_count;
                    if(!count)continue;
                    const auto& geometry = *group.geometry;
                    require(gfxProgramSetParameter(gfx, program, "vertices", geometry.gpu.vertices), "Bind mesh vertices");
                    require(gfxProgramSetParameter(gfx, program, "faces", geometry.gpu.faces), "Bind mesh triangle records");
                    require(gfxProgramSetParameter(gfx, program, "first_triangle", 0u), "Set mesh triangle offset");
                    require(gfxProgramSetParameter(gfx,program,"first_instance",first),"Set mesh instance offset");
                    require(gfxCommandBindIndexBuffer(gfx, geometry.gpu.indices), "Bind mesh indices");
                    require(gfxCommandDrawIndexed(gfx,geometry.gpu.triangle_count*3,count),"Draw mesh batch");
                    ++stats.scene_draw_calls;
                }
            };
            if(lighting) {
                lit_draws.clear();lit_draws.reserve(mesh_groups.size());
                for(const auto& group:mesh_groups)
                    lit_draws.push_back({group.geometry->gpu,group.visible_first,group.visible_count});
                {
                    timing::Scope lit_stage{phases::encode_lit};
                    scene_lit->draw(frame,extent,depth,{lit_draws,instance_groups,order},materials,stats);
                }
                require(gfxCommandBindDepthStencilTarget(gfx,depth),"Bind Scene Lit editing depth");
                if(editing_depth) {
                    // AA has consumed and retained its sampled depth. Editing
                    // guides and capture share the public, unjittered camera.
                    require(gfxCommandClearTexture(gfx,depth),"Clear sampled scene depth for editing");
                    draw_batches(scene_depth,2);
                }
                {
                    timing::Scope lit_stage{phases::encode_lit};
                    scene_lit->draw_debug(frame,extent,depth,stats);
                }
                draw_grid(frame,true);
                if(editing_lines)draw_batches(visible_lines,1);
            } else if (lines_only) {
                draw_batches(scene_depth, 2);
                draw_grid(frame, true);
                draw_batches(frame.view.hidden_lines ? visible_lines : all_lines, 1);
            } else {
                if(cavity_projection) {
                    require(gfxCommandBindColorTarget(gfx,0,cavity_color),"Bind studio surface color");
                    require(gfxCommandBindColorTarget(gfx,1,cavity_normals),"Bind cavity surface normals");
                    draw_batches(cavity_surface,0);
                    require(gfxCommandBindColorTarget(gfx,1,{}),"Unbind cavity surface normals");
                    require(gfxCommandBindColorTarget(gfx,0,{}),"Restore window color target");
                    draw_cavity(frame);
                    require(gfxCommandBindDepthStencilTarget(gfx,depth),"Restore studio editing depth");
                } else draw_batches(scene,0);
                draw_grid(frame, true);
                if(cavity_projection&&editing_lines)draw_batches(visible_lines,3);
            }
        } else {
            materials.descriptions.clear();materials.textures.clear();material_keys.clear();next_material_keys.clear();used_material_images.clear();++materials.revision;
            mesh_checks.materials_changed=true;
            if(has_viewport) {
                if(scene_lit)scene_lit->draw_debug(frame,extent,depth,stats);
                draw_grid(frame,false);
            }
        }
        for(auto it=material_images.begin();it!=material_images.end();) {
            if(it->second.used) {++it;continue;}
            require(gfxDestroyTexture(gfx,it->second.texture),"Retire unused material texture");it=material_images.erase(it);
        }
        stats.cached_material_images=material_images.size();
        for (auto it = geometries.begin(); it != geometries.end();) {
            if (it->second.used) { ++it; continue; }
            for (const auto &buffer : {it->second.gpu.vertices, it->second.gpu.indices, it->second.gpu.faces})
                if (buffer) require(gfxDestroyBuffer(gfx, buffer), "Retire unused mesh geometry");
            it = geometries.erase(it);
        }
        require(gfxCommandBindDepthStencilTarget(gfx, {}), "Unbind scene depth");
        require(gfxCommandSetViewport(gfx, 0, 0, static_cast<float>(extent.width),
                                      static_cast<float>(extent.height)),
                "Set UI viewport");
        for (auto &[key, image] : images)
            image.used = false;
        std::optional<timing::Scope> ui_stage{std::in_place, phases::encode_ui};
        if (!ui_instances.empty()) {
            ui_textures.clear();
            for (const auto &[index, item] : ui_textured)
                ui_instances[index].shading |= (item->coverage ? texture(item->coverage) : texture(item->image)) << 2;
            GfxBuffer ui_data;
            {
                timing::Scope ui_upload{phases::encode_ui_upload};
                ui_data = ui_slots.write(gfx, context.info().frames_in_flight, ui_instances, true, "UI instance");
            }
            stats.ui_instance_upload_bytes = (ui_instances.size() - overlay_instances) * sizeof(UiInstance);
            stats.ui_quads = ui_instances.size() - overlay_instances;
            stats.overlay_shapes = overlay_instances;
            const auto program = ui_pipeline.program();
            const std::array<float, 2> size{static_cast<float>(extent.width), static_cast<float>(extent.height)};
            require(gfxProgramSetParameter(gfx, program, "ui_instances", ui_data), "Bind UI instances");
            require(gfxProgramSetParameter(gfx, program, "target_size", size), "Set UI target");
            require(gfxProgramSetParameter(gfx, program, "ui_textures", ui_textures.data(),
                                           static_cast<std::uint32_t>(ui_textures.size())), "Bind UI textures");
            require(gfxProgramSetParameter(gfx, program, "ui_sampler", sampler), "Bind UI sampler");
            require(gfxCommandSetScissorRect(gfx, 0, 0, static_cast<std::int32_t>(extent.width),
                                             static_cast<std::int32_t>(extent.height)), "Set UI scissor");
            require(gfxCommandBindKernel(gfx, ui_pipeline.kernel()), "Bind UI pipeline");
            // Clips are per instance and D3D blends a draw's primitives in submission
            // order, so each layer keeps painter order in one draw.
            const auto draw_layer = [&](std::size_t first, std::size_t count, std::uint64_t &calls) {
                if (count == 0) return;
                require(gfxProgramSetParameter(gfx, program, "first_instance", static_cast<std::uint32_t>(first)),
                        "Set UI instance offset");
                require(gfxCommandDraw(gfx, 6, static_cast<std::uint32_t>(count)), "Draw UI instances");
                ++calls;
            };
            draw_layer(0, overlay_instances, stats.overlay_draw_calls);
            draw_layer(overlay_instances, ui_instances.size() - overlay_instances, stats.ui_draw_calls);
        }
        ui_stage.reset();
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
            BufferOwner z{gfx, {}}, motion{gfx, {}};
            const std::uint32_t motion_pitch = (extent.width * 8 + 255) & ~255u;
            if (draw_scene) {
                z.buffer = gfxCreateBuffer(gfx, std::uint64_t(pitch) * extent.height, nullptr, kGfxCpuAccess_Read);
                allocation(bool(z.buffer));
                require(gfxCommandCopyTextureToBufferPitched(gfx, z.buffer, depth, pitch), "Capture scene depth");
                if (frame.view.shading == Shading::scene_lit && frame.view.wireframe != Wireframe::only) {
                    motion.buffer = gfxCreateBuffer(gfx, std::uint64_t(motion_pitch) * extent.height, nullptr, kGfxCpuAccess_Read);
                    allocation(bool(motion.buffer));
                    require(gfxCommandCopyTextureToBufferPitched(gfx, motion.buffer, scene_lit->motion(), motion_pitch),
                            "Capture Scene Lit surface motion");
                }
            } else {
                // A UI-only capture describes this frame, not a retained scene depth texture.
                std::fill(capture->depth.begin(), capture->depth.end(), 0.0f);
            }
            const auto finished = context.finish();
            if (!finished) throw Error{finished.error(), true};
            const auto *rgba = static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, color.buffer));
            const auto *depth_bytes = draw_scene
                ? static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, z.buffer)) : nullptr;
            const auto *motion_bytes = motion.buffer
                ? static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, motion.buffer)) : nullptr;
            allocation(rgba && (!draw_scene || depth_bytes) && (!motion.buffer || motion_bytes));
            if (motion_bytes) capture->motion.resize(std::size_t(extent.width) * extent.height * 2);
            for (std::uint32_t y = 0; y < extent.height; ++y) {
                std::memcpy(capture->rgba.data() + std::size_t(y) * extent.width * 4,
                            rgba + std::size_t(y) * pitch, extent.width * 4);
                if (depth_bytes)
                    std::memcpy(capture->depth.data() + std::size_t(y) * extent.width,
                                depth_bytes + std::size_t(y) * pitch, extent.width * 4);
                if (motion_bytes)
                    for (std::uint32_t x = 0; x < extent.width; ++x)
                        for (std::uint32_t c = 0; c < 2; ++c) {
                            std::uint16_t half;
                            std::memcpy(&half, motion_bytes + std::size_t(y) * motion_pitch + x * 8 + c * 2, 2);
                            capture->motion[(std::size_t(y) * extent.width + x) * 2 + c] = half_float(half);
                        }
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
                                          .rgba8_window = true,
                                          .shader_cache = options.shader_cache});
        if (!opened) return std::unexpected(Error{opened.error(), false});
        s.context = std::move(*opened);
        s.gfx = s.context.borrow();
        s.options = options;
        if (gfxGetBackBufferFormat(s.gfx) != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            return std::unexpected(
                error(rhi::ErrorCode::unsupported, "Editor renderer requires an sRGB RGBA8 window target"));
        if (options.validation != rhi::Validation::disabled)
            gfxGetDevice(s.gfx)->QueryInterface(IID_PPV_ARGS(&s.debug));
        s.prepare_pipeline(s.ui_pipeline, "editor UI", shaders::ui_vs, shaders::ui_ps, PipelineStyle::ui);
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
Result<Target> Renderer::sync_window_target() {
    if (!state_) return std::unexpected(error(rhi::ErrorCode::closed, "Renderer is closed"));
    if (const auto checked=state_->check();!checked) return std::unexpected(checked.error());
    const auto target=state_->context.sync_window_target();
    if(!target) return std::unexpected(Error{target.error(),true});
    return Target{{target->width,target->height},target->drawable};
}
Result<Preparation> Renderer::preparation(const ViewSettings &view) {
    if (!state_) return std::unexpected(error(rhi::ErrorCode::closed, "Renderer is closed"));
    if (const auto checked = state_->check(); !checked) return std::unexpected(checked.error());
    if (view.shading != Shading::scene_lit || view.wireframe == Wireframe::only) return Preparation{.ready = true};
    auto &s = *state_;
    try {
        if (!s.scene_lit) s.scene_lit = std::make_unique<detail::SceneLit>(s.context);
        return s.scene_lit->preparation(view);
    } catch (const std::bad_alloc &) {
        return std::unexpected(error(rhi::ErrorCode::creation_failed, "Renderer allocation failed", false,
                                     kGfxResult_OutOfMemory));
    }
}
Result<FrameResult> Renderer::render(const Frame &frame, Capture *capture) {
    timing::Scope rendering{phases::render};
    const auto destination = target();
    if (!destination) return std::unexpected(destination.error());
    auto &s = *state_;
    s.stats.ui_draw_calls = 0;
    s.stats.ui_quads = 0;
    s.stats.ui_instance_upload_bytes = 0;
    s.stats.scene_draw_calls = 0;
    s.stats.scene_instances = s.stats.scene_culled_instances = 0;
    s.stats.material_upload_count = 0;
    s.stats.viewport_draw_calls = 0;
    s.stats.overlay_shapes = 0;
    s.stats.overlay_draw_calls = 0;
    s.stats.coverage_upload_count = 0;
    s.stats.coverage_upload_bytes = 0;
    s.stats.image_upload_count = 0;
    s.stats.image_upload_bytes = 0;
    s.stats.scene_lit_gpu_ms={};s.stats.scene_lit_ddgi_gpu_ms={};s.stats.scene_lit_ddgi_visibility_rays={};s.stats.scene_lit_gpu_timing_valid=false;
    s.stats.scene_lit_direct_shading_gpu_ms=0;s.stats.scene_lit_ddgi_update_gpu_ms={};s.stats.scene_lit_gpu_pass_mask=0;
    s.stats.scene_lit_resource_bytes=0;s.stats.scene_lit_shadow_rays=0;s.stats.scene_lit_probe_rays=0;
    s.stats.scene_lit_light_sources=0;s.stats.scene_lit_shadow_sources=0;s.stats.scene_lit_history_resets=0;s.stats.scene_lit_probe_volumes={};
    s.stats.scene_lit_spatial_sources=0;s.stats.scene_lit_spatial_cells=0;s.stats.scene_lit_spatial_bytes=0;s.stats.scene_lit_spatial_changed=false;
    s.stats.scene_lit_gpu_sample_sequence=0;s.stats.scene_lit_gpu_sample_frame_id=0;
    s.stats.scene_lit_probe_inspection.reset();
    const auto extent = destination->extent;
    if (!destination->drawable || frame.ui_frame.extent != extent)
        return FrameResult{.accepted_extent = {}, .next_extent = extent, .stats = s.stats};
    bool encoding = false;
    try {
        std::optional<timing::Scope> stage{std::in_place, phases::prepare};
        validate(frame, s.mesh_checks);
        if(!std::isfinite(frame.render_delta_seconds) || frame.render_delta_seconds<0 || frame.render_delta_seconds>.1f)
            throw error(rhi::ErrorCode::invalid_argument,"Render delta must be finite and in [0, .1] seconds");
        if(frame.view.shading==Shading::scene_lit&&frame.view.wireframe!=Wireframe::only&&(!frame.meshes.empty()||frame.view.lit.probe_overlay())&&frame.scene_viewport.width>0&&frame.scene_viewport.height>0) {
            if(!s.scene_lit)s.scene_lit=std::make_unique<detail::SceneLit>(s.context);
            s.scene_lit->validate(frame, s.mesh_checks.duplicate_keys());
            const auto prepared=s.scene_lit->preparation(frame.view);
            if(!prepared) return std::unexpected(prepared.error());
            if(!prepared->ready)
                return FrameResult{.status = FrameStatus::not_ready, .accepted_extent = {}, .next_extent = extent, .stats = s.stats};
        }
        s.grid_camera=prepare_grid_camera(frame);
        s.cavity_projection=prepare_cavity_projection(frame);
        s.instances.clear();
        s.instances.reserve(frame.meshes.size());
        s.raster_visibility.clear();s.raster_visibility.reserve(frame.meshes.size());
        const detail::SceneFrustum frustum(frame.camera.view,frame.camera.projection,frame.scene_viewport.width,frame.scene_viewport.height);
        s.mesh_groups.clear();s.instance_groups.clear();s.instance_groups.reserve(frame.meshes.size());
        for (std::size_t i = 0; i < frame.meshes.size(); ++i) {
            const auto &mesh = frame.meshes[i];
            auto &checked = s.mesh_checks[i];
            if (!checked.prepared) checked.prepared = prepare_instance(mesh);
            s.instances.push_back(*checked.prepared);
            s.instances.back().selected = mesh.selected ? 1.0f : 0.0f;
            const bool visible=frustum.visible(mesh.geometry->bounds(),mesh.world);
            s.raster_visibility.push_back(visible?1u:0u);
            ++s.stats.scene_instances;s.stats.scene_culled_instances+=visible?0u:1u;
            const auto [found,inserted]=s.geometries.try_emplace(mesh.geometry.get());
            auto& cached=found->second;
            if(inserted)cached.source=mesh.geometry;
            if(cached.group>=s.mesh_groups.size()||s.mesh_groups[cached.group].geometry!=&cached) {
                cached.group=static_cast<std::uint32_t>(s.mesh_groups.size());
                s.mesh_groups.push_back({&cached});
            }
            auto& group=s.mesh_groups[cached.group];
            group.visible_count+=visible?1u:0u;group.selected_count+=visible&&mesh.selected?1u:0u;
            s.instance_groups.push_back(cached.group);
        }
        std::uint32_t order_size=0;
        for(auto& group:s.mesh_groups){group.visible_first=order_size;order_size+=group.visible_count;}
        for(auto& group:s.mesh_groups){group.selected_first=order_size;order_size+=group.selected_count;}
        s.draw_instances.resize(order_size);
        // Firsts advance as fill cursors, then return to the range starts.
        for(std::uint32_t i=0;i<s.instance_groups.size();++i) {
            if(!s.raster_visibility[i])continue;
            auto& group=s.mesh_groups[s.instance_groups[i]];
            s.draw_instances[group.visible_first++]=i;
            if(frame.meshes[i].selected)s.draw_instances[group.selected_first++]=i;
        }
        for(auto& group:s.mesh_groups){group.visible_first-=group.visible_count;group.selected_first-=group.selected_count;}
        s.prepare_ui(frame);
        stage.reset();
        Capture pending;
        if (capture) {
            pending.frame_id = frame.ui_frame.id;
            pending.extent = extent;
            pending.rgba.resize(std::size_t(extent.width) * extent.height * 4);
            pending.depth.resize(std::size_t(extent.width) * extent.height);
        }
        stage.emplace(phases::encode);
        encoding = true;
        if(s.scene_lit && (frame.view.shading!=Shading::scene_lit || frame.view.wireframe==Wireframe::only ||
                          frame.meshes.empty() || frame.scene_viewport.width==0 || frame.scene_viewport.height==0))
            s.scene_lit->invalidate(frame.meshes.empty(),s.stats);
        s.draw(frame, extent, capture ? &pending : nullptr);
        stage.reset();
        rhi::FrameInfo details;
        {
            timing::Scope submitting{phases::frame};
            const auto presented = s.context.frame(s.options.vsync, &details);
            if (!presented) throw Error{presented.error(), true};
            // gfx partitions its frame call by CPU wall time; record it as children of the call.
            const auto stage_time = [](double us) { return std::chrono::nanoseconds{std::llround(us * 1000)}; };
            const auto &cpu = details.cpu;
            timing::record(phases::submit, stage_time(cpu.prepare_us + cpu.submit_us));
            timing::record(phases::present, stage_time(cpu.present_us));
            timing::record(phases::wait, stage_time(cpu.wait_us));
            timing::record(phases::recycle, stage_time(cpu.resize_us + cpu.recycle_us + cpu.reclaim_us));
        }
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

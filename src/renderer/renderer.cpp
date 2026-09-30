#include "renderer.hpp"
#include "shader_source.hpp"

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
void validate(const Frame &frame) {
    const auto &ui = frame.ui_frame;
    bool good =
        ui.id != 0 && finite(ui.dpi_scale) && ui.dpi_scale > 0 && valid(frame.scene_viewport) &&
        frame.scene_viewport.x >= 0 && frame.scene_viewport.y >= 0 &&
        frame.scene_viewport.x + frame.scene_viewport.width <= static_cast<float>(ui.extent.width) &&
        frame.scene_viewport.y + frame.scene_viewport.height <= static_cast<float>(ui.extent.height) &&
        frame.cubes.size() <= std::numeric_limits<std::uint32_t>::max();
    for (float v : frame.camera.view)
        good &= finite(v);
    for (float v : frame.camera.projection)
        good &= finite(v);
    for (const auto &cube : frame.cubes) {
        good &= valid(cube.color);
        for (float v : cube.world)
            good &= finite(v);
        good &= cube.world[3] == 0 && cube.world[7] == 0 && cube.world[11] == 0 && cube.world[15] == 1;
    }
    for (const auto &draw : ui.draws) {
        good &= valid(draw.bounds) && valid(draw.uv) && valid(draw.color);
        if (draw.coverage) {
            const auto &image = *draw.coverage;
            good &= image.identity != 0 && image.width > 0 && image.height > 0 &&
                    image.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
                    image.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && image.row_pitch >= image.width &&
                    std::uint64_t(image.row_pitch) * image.height <= image.pixels.size();
        }
    }
    if (!good)
        throw error(rhi::ErrorCode::invalid_argument, "Invalid renderer frame, camera, color or coverage");
}
struct Vertex {
    float position[3];
    float normal[3];
};
struct Instance {
    std::array<float, 16> world;
    std::array<float, 9> face_normals;
    float selected;
    ui::Color color;
};
static_assert(sizeof(Instance) == 120 && sizeof(Vertex) == 24);
Instance prepare_instance(const Cube &cube) {
    const auto &m = cube.world;
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[4], e = m[5], f = m[6];
    const double g = m[8], h = m[9], i = m[10];
    // Cofactor rows are inverse-transpose rows times determinant. Double
    // intermediates avoid overflow/underflow for finite float matrix entries.
    const std::array<double, 9> cofactors{e * i - f * h, f * g - d * i, d * h - e * g,
                                          c * h - b * i, a * i - c * g, b * g - a * h,
                                          b * f - c * e, c * d - a * f, a * e - b * d};
    const double determinant = a * cofactors[0] + b * cofactors[1] + c * cofactors[2];
    if (determinant == 0) throw error(rhi::ErrorCode::invalid_argument, "Cube world matrix is singular");
    Instance result{cube.world, {}, cube.selected ? 1.0f : 0.0f, cube.color};
    // Cube normals are signed basis axes. Normalize their transformed directions
    // individually on the CPU so extreme nonuniform scales cannot underflow a
    // normal in the shader. det's sign preserves outward normals under reflection.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::size_t offset = axis * 3;
        const double length = std::hypot(cofactors[offset], cofactors[offset + 1], cofactors[offset + 2]);
        if (length == 0)
            throw error(rhi::ErrorCode::invalid_argument, "Cube world matrix has a degenerate face");
        const double sign = determinant < 0 ? -1.0 : 1.0;
        for (std::size_t component = 0; component < 3; ++component)
            result.face_normals[offset + component] =
                static_cast<float>(sign * cofactors[offset + component] / length);
    }
    return result;
}
std::array<Vertex, 36> cube_vertices() {
    constexpr float p[8][3] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                               {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {.5f, .5f, .5f},  {-.5f, .5f, .5f}};
    constexpr int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                                 {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    constexpr float normals[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    constexpr int indices[6] = {0, 1, 2, 0, 2, 3};
    std::array<Vertex, 36> vertices{};
    for (int face = 0; face < 6; ++face)
        for (int i = 0; i < 6; ++i) {
            auto &v = vertices[face * 6 + i];
            std::copy_n(p[faces[face][indices[i]]], 3, v.position);
            std::copy_n(normals[face], 3, v.normal);
        }
    return vertices;
}
struct CoverageKey {
    std::uint64_t identity, revision;
    bool operator==(const CoverageKey &) const = default;
};
struct CoverageHash {
    std::size_t operator()(const CoverageKey &k) const noexcept {
        return std::hash<std::uint64_t>{}(k.identity) ^ (std::hash<std::uint64_t>{}(k.revision) << 1);
    }
};
struct CachedCoverage {
    std::shared_ptr<const ui::Coverage> owner;
    GfxTexture texture;
    bool used{};
};
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
    rhi::Pipeline scene, solid, coverage;
    GfxSamplerState sampler;
    GfxBuffer vertices;
    GfxTexture depth;
    ui::PixelExtent depth_extent;
    std::unordered_map<CoverageKey, CachedCoverage, CoverageHash> images;
    FrameStats stats;
    std::vector<Instance> instances;
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
        for (auto &[key, image] : images) {
            const auto r = gfxDestroyTexture(gfx, image.texture);
            if (r != kGfxResult_NoError)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "Coverage retirement failed", true, r));
            image.texture = {};
        }
        images.clear();
        if (depth) {
            const auto r = gfxDestroyTexture(gfx, depth);
            if (r)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "Depth retirement failed", true, r));
            depth = {};
        }
        if (vertices) {
            const auto r = gfxDestroyBuffer(gfx, vertices);
            if (r)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "Geometry retirement failed", true, r));
            vertices = {};
        }
        if (sampler) {
            const auto r = gfxDestroySamplerState(gfx, sampler);
            if (r)
                return std::unexpected(
                    error(rhi::ErrorCode::execution_failed, "Sampler retirement failed", true, r));
            sampler = {};
        }
        scene = {};
        solid = {};
        coverage = {};
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
                          bool scene_depth) {
        rhi::PipelineDesc desc;
        desc.kind = rhi::PipelineKind::vertex;
        desc.name = name;
        desc.source.vs = vs;
        desc.source.ps = ps;
        require(gfxDrawStateSetColorTarget(desc.draw_state, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
                "Set window color target");
        require(gfxDrawStateSetCullMode(desc.draw_state, D3D12_CULL_MODE_NONE), "Set culling");
        if (scene_depth) {
            require(gfxDrawStateSetDepthStencilTarget(desc.draw_state, DXGI_FORMAT_D32_FLOAT),
                    "Set depth target");
            require(gfxDrawStateSetDepthFunction(desc.draw_state, D3D12_COMPARISON_FUNC_LESS),
                    "Set depth comparison");
            require(gfxDrawStateSetDepthWriteMask(desc.draw_state, D3D12_DEPTH_WRITE_MASK_ALL),
                    "Set depth writes");
        } else {
            require(gfxDrawStateSetDepthWriteMask(desc.draw_state, D3D12_DEPTH_WRITE_MASK_ZERO),
                    "Disable depth writes");
            require(gfxDrawStateSetBlendMode(desc.draw_state, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA,
                                             D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA,
                                             D3D12_BLEND_OP_ADD),
                    "Set premultiplied alpha blending");
        }
        auto prepared = context.prepare(desc);
        if (!prepared) throw Error{prepared.error(), false};
        output = std::move(*prepared);
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
    GfxTexture texture(const std::shared_ptr<const ui::Coverage> &owner) {
        const CoverageKey key{owner->identity, owner->revision};
        auto [it, inserted] = images.try_emplace(key);
        auto &image = it->second;
        image.used = true;
        if (!inserted) return image.texture;
        image.owner = owner;
        image.texture = gfxCreateTexture2D(gfx, owner->width, owner->height, DXGI_FORMAT_R8_UNORM);
        allocation(bool(image.texture));
        ++stats.cached_coverage_count;
        stats.cached_coverage_texel_bytes += std::uint64_t(owner->width) * owner->height;
        stats.cached_coverage_cpu_bytes += owner->pixels.size();
        // The protocol permits padded CPU rows; gfx's upload contract is tight.
        upload_pixels.resize(std::size_t(owner->width) * owner->height);
        for (std::uint32_t y = 0; y < owner->height; ++y)
            std::memcpy(upload_pixels.data() + std::size_t(y) * owner->width,
                        owner->pixels.data() + std::size_t(y) * owner->row_pitch, owner->width);
        BufferOwner upload{
            gfx, gfxCreateBuffer(gfx, upload_pixels.size(), upload_pixels.data(), kGfxCpuAccess_Write)};
        allocation(bool(upload.buffer));
        require(gfxCommandCopyBufferToTexture(gfx, image.texture, upload.buffer), "Upload UI coverage");
        ++stats.coverage_upload_count;
        stats.coverage_upload_bytes += upload_pixels.size();
        return image.texture;
    }
    void draw(const Frame &frame, const ui::PixelExtent extent, Capture *capture) {
        if (depth_extent != extent) {
            if (depth) require(gfxDestroyTexture(gfx, std::exchange(depth, {})), "Retire old depth");
            const float clear[4] = {1, 0, 0, 0};
            depth = gfxCreateTexture2D(gfx, extent.width, extent.height, DXGI_FORMAT_D32_FLOAT, 1, clear);
            allocation(bool(depth));
            depth_extent = extent;
        }
        require(gfxCommandClearBackBuffer(gfx), "Clear window");
        require(gfxCommandClearTexture(gfx, depth), "Clear depth");
        const auto &viewport = frame.scene_viewport;
        if (!instances.empty() && viewport.width > 0 && viewport.height > 0) {
            BufferOwner data{gfx, gfxCreateBuffer<Instance>(gfx, static_cast<std::uint32_t>(instances.size()),
                                                            instances.data())};
            allocation(bool(data.buffer));
            require(gfxCommandBindDepthStencilTarget(gfx, depth), "Bind scene depth");
            require(gfxCommandSetViewport(gfx, viewport.x, viewport.y, viewport.width, viewport.height),
                    "Set scene viewport");
            require(gfxCommandSetScissorRect(gfx, static_cast<int>(std::floor(viewport.x)),
                                             static_cast<int>(std::floor(viewport.y)),
                                             static_cast<int>(std::ceil(viewport.x + viewport.width)) -
                                                 static_cast<int>(std::floor(viewport.x)),
                                             static_cast<int>(std::ceil(viewport.y + viewport.height)) -
                                                 static_cast<int>(std::floor(viewport.y))),
                    "Set scene scissor");
            const auto program = scene.program();
            require(gfxProgramSetParameter(gfx, program, "vertices", vertices), "Bind cube vertices");
            require(gfxProgramSetParameter(gfx, program, "instances", data.buffer), "Bind cube instances");
            require(gfxProgramSetParameter(gfx, program, "view", frame.camera.view), "Set view matrix");
            require(gfxProgramSetParameter(gfx, program, "projection", frame.camera.projection),
                    "Set projection matrix");
            require(gfxCommandBindKernel(gfx, scene.kernel()), "Bind scene pipeline");
            require(gfxCommandDraw(gfx, 36, static_cast<std::uint32_t>(instances.size())),
                    "Draw scene cubes");
            ++stats.scene_draw_calls;
        }
        require(gfxCommandBindDepthStencilTarget(gfx, {}), "Unbind scene depth");
        require(gfxCommandSetViewport(gfx, 0, 0, static_cast<float>(extent.width),
                                      static_cast<float>(extent.height)),
                "Set UI viewport");
        for (auto &[key, image] : images)
            image.used = false;
        for (const auto &item : frame.ui_frame.draws) {
            const int left = std::clamp(item.clip.left, 0, static_cast<int>(extent.width));
            const int top = std::clamp(item.clip.top, 0, static_cast<int>(extent.height));
            const int right = std::clamp(item.clip.right, 0, static_cast<int>(extent.width));
            const int bottom = std::clamp(item.clip.bottom, 0, static_cast<int>(extent.height));
            if (left >= right || top >= bottom || item.bounds.width == 0 || item.bounds.height == 0 ||
                item.color.a == 0)
                continue;
            const auto &pipeline = item.coverage ? coverage : solid;
            const auto program = pipeline.program();
            if (item.coverage) {
                require(gfxProgramSetParameter(gfx, program, "coverage", texture(item.coverage)),
                        "Bind UI coverage");
                require(gfxProgramSetParameter(gfx, program, "coverage_sampler", sampler),
                        "Bind coverage sampler");
            }
            const std::array<float, 2> size{static_cast<float>(extent.width),
                                            static_cast<float>(extent.height)};
            require(gfxProgramSetParameter(gfx, program, "bounds", item.bounds), "Set UI bounds");
            require(gfxProgramSetParameter(gfx, program, "uv_bounds", item.uv), "Set UI UV");
            require(gfxProgramSetParameter(gfx, program, "target_size", size), "Set UI target");
            require(gfxProgramSetParameter(gfx, program, "tint", item.color), "Set UI color");
            require(gfxCommandSetScissorRect(gfx, left, top, right - left, bottom - top), "Set UI scissor");
            require(gfxCommandBindKernel(gfx, pipeline.kernel()), "Bind UI pipeline");
            require(gfxCommandDraw(gfx, 6), "Draw UI quad");
            ++stats.ui_draw_calls;
        }
        for (auto it = images.begin(); it != images.end();) {
            if (it->second.used) {
                ++it;
                continue;
            }
            require(gfxDestroyTexture(gfx, it->second.texture), "Retire unused coverage");
            const auto &owner = *it->second.owner;
            --stats.cached_coverage_count;
            stats.cached_coverage_texel_bytes -= std::uint64_t(owner.width) * owner.height;
            stats.cached_coverage_cpu_bytes -= owner.pixels.size();
            it = images.erase(it);
        }
        if (capture) {
            const std::uint32_t pitch = (extent.width * 4 + 255) & ~255u;
            BufferOwner color{
                gfx, gfxCreateBuffer(gfx, std::uint64_t(pitch) * extent.height, nullptr, kGfxCpuAccess_Read)};
            BufferOwner z{
                gfx, gfxCreateBuffer(gfx, std::uint64_t(pitch) * extent.height, nullptr, kGfxCpuAccess_Read)};
            allocation(bool(color.buffer) && bool(z.buffer));
            require(gfxCommandCopyBackBufferToBuffer(gfx, color.buffer, pitch), "Capture window color");
            require(gfxCommandCopyTextureToBufferPitched(gfx, z.buffer, depth, pitch), "Capture scene depth");
            const auto finished = context.finish();
            if (!finished) throw Error{finished.error(), true};
            const auto *rgba = static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, color.buffer));
            const auto *depth_bytes = static_cast<const std::uint8_t *>(gfxBufferGetData(gfx, z.buffer));
            allocation(rgba && depth_bytes);
            for (std::uint32_t y = 0; y < extent.height; ++y) {
                std::memcpy(capture->rgba.data() + std::size_t(y) * extent.width * 4,
                            rgba + std::size_t(y) * pitch, extent.width * 4);
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
        s.prepare_pipeline(s.scene, "editor scene", shaders::scene_vs, shaders::scene_ps, true);
        s.prepare_pipeline(s.solid, "editor rectangles", shaders::ui_vs, shaders::solid_ps, false);
        s.prepare_pipeline(s.coverage, "editor text coverage", shaders::ui_vs, shaders::coverage_ps, false);
        s.sampler = gfxCreateSamplerState(s.gfx, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
        allocation(bool(s.sampler));
        const auto geometry = cube_vertices();
        s.vertices =
            gfxCreateBuffer<Vertex>(s.gfx, static_cast<std::uint32_t>(geometry.size()), geometry.data());
        allocation(bool(s.vertices));
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
    s.stats.scene_draw_calls = 0;
    s.stats.coverage_upload_count = 0;
    s.stats.coverage_upload_bytes = 0;
    const auto extent = destination->extent;
    if (!destination->drawable || frame.ui_frame.extent != extent)
        return FrameResult{.accepted_extent = {}, .next_extent = extent, .stats = s.stats};
    bool encoding = false;
    try {
        validate(frame);
        s.instances.clear();
        s.instances.reserve(frame.cubes.size());
        for (const auto &cube : frame.cubes)
            s.instances.push_back(prepare_instance(cube));
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

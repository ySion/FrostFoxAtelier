#include "material/program.hpp"
#include "material/program_resources.hpp"
#include "source/material_program.hpp"
#include "rhi/rhi.hpp"
#include "../rhi/support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace material_program_e2e {
namespace {
using namespace atelier;

using rhi_acceptance::check;

using rhi_acceptance::require;

using Program = std::shared_ptr<const source::MaterialProgram>;

using rhi_acceptance::take;

void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(file.good(), "material program evidence write failed");
}

// Boundary loads/stores follow registered native declarations, not a second ABI.
std::string adapter(Runtime& runtime, TypeId type, std::string_view ns, bool input) {
    const auto* info = take(runtime.describe(type));
    const std::string qualified = std::string(ns) + (input ? "::ms_Context" : "::ms_Output");
    std::string code = input ? qualified + " load_" + std::string(ns) + "(uint base) {\n" + qualified + " value;\n"
                             : "void store_" + std::string(ns) + "(uint base," + qualified + " value) {\n";
    for (const auto& field : info->fields) {
        const auto* leaf = take(runtime.describe(field.value.type));
        const auto components = leaf->math ? leaf->math.components() : 1;
        require(field.offset && components <= 4 && leaf->size == components * sizeof(float),
                "material boundary must contain native float/vector leaves");
        const std::string width = components == 1 ? "" : std::to_string(components);
        const std::string address = "base+" + std::to_string(*field.offset);
        const std::string member = "value.ms_" + field.name;
        code += input ? member + "=asfloat(contexts.Load" + width + "(" + address + "));\n"
                      : "results.Store" + width + "(" + address + ",asuint(" + member + "));\n";
    }
    return code + (input ? "return value;\n}\n" : "}\n");
}

void parameter(const source::MaterialProgram& program, std::vector<std::byte>& bytes, std::string_view key,
               std::span<const float> values) {
    const auto at = std::ranges::find(program.parameters(), key, &source::MaterialParameter::key);
    require(at != program.parameters().end() && at->components == values.size() &&
                at->offset + values.size_bytes() <= bytes.size(),
            "parameter descriptor does not match declared leaf");
    std::memcpy(bytes.data() + at->offset, values.data(), values.size_bytes());
}

constexpr std::string_view sampling_shader = R"hlsl(
ByteAddressBuffer contexts;
ByteAddressBuffer parameters;
RWByteAddressBuffer results;
StructuredBuffer<uint4> bindings;
Texture2D<float4> images[] : register(t0,space1);
SamplerState repeat_sampler;
float4 atelier_material_sample(uint environment,uint local_slot,float2 uv) {
    uint4 binding=bindings[0];
    if(binding.z!=local_slot)binding=bindings[1];
    return images[NonUniformResourceIndex(binding.x)].SampleLevel(repeat_sampler,uv,0);
}
)hlsl";

struct Compiled {
    Program program;

    std::string shader;
};

Compiled compile(const char* name, std::string_view text, const std::filesystem::path& evidence, bool coverage) {
    Runtime runtime;
    take(material::register_program_types(runtime));
    const auto context = take(runtime.native_type<material::Context>());
    const auto output = take(runtime.native_type<material::Output>());
    const auto texture = take(runtime.native_type<material::TextureInput>());
    auto program = take(source::compile_material(runtime, context, output, texture, name, text));
    std::string shader = std::string(sampling_shader) + take(program->hlsl(name)) +
                         adapter(runtime, context, name, true) + adapter(runtime, output, name, false);
    const std::string ns = name;
    if (coverage)
        shader += take(program->hlsl_coverage("Coverage")) + adapter(runtime, context, "Coverage", true);
    shader += "[numthreads(1,1,1)] void main() { store_" + ns + "(0," + ns + "::ms_shade(0,load_" + ns + "(0)," + ns +
              "::ms_load_parameters(parameters,0)));\n";
    if (coverage)
        shader += "results.Store(" + std::to_string(sizeof(material::Output)) +
                  ",asuint(Coverage::ms_coverage(0,load_Coverage(0),Coverage::ms_load_parameters(parameters,0))));\n";
    shader += "}\n";
    write(evidence / (ns + ".source"), text);
    write(evidence / (ns + ".hlsl"), shader);
    return {std::move(program), std::move(shader)};
}

struct Device {
    rhi::Context owner;

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> debug;

    rhi::Pipeline pipeline;

    std::vector<GfxBuffer> buffers;

    std::vector<GfxTexture> images;

    GfxSamplerState sampler{};

    Device(HWND window, const std::filesystem::path& shader_cache)
        : owner(take(rhi::Context::open(
              {.window = window, .validation = rhi::Validation::gpu, .shader_cache = shader_cache}))),
          debug(rhi_acceptance::debug_queue(owner.borrow())) {
        require(owner.info().debug_layer && owner.info().gpu_validation == rhi_acceptance::expected_gpu_validation(),
                "B0 validation differs from the requested mode");
        std::printf("material-programs: debug-layer=%u gpu-validation=%u\n", unsigned(owner.info().debug_layer),
                    unsigned(owner.info().gpu_validation));
        rhi_acceptance::print_adapter(owner.borrow());
    }

    ~Device() {
        pipeline = {};
        for (auto buffer : buffers)
            (void)gfxDestroyBuffer(owner.borrow(), buffer);
        for (auto image : images)
            (void)gfxDestroyTexture(owner.borrow(), image);
        if (sampler)
            (void)gfxDestroySamplerState(owner.borrow(), sampler);
    }

    void close() {
        pipeline = {};
        while (!buffers.empty()) {
            check(gfxDestroyBuffer(owner.borrow(), buffers.back()));
            buffers.pop_back();
        }
        while (!images.empty()) {
            check(gfxDestroyTexture(owner.borrow(), images.back()));
            images.pop_back();
        }
        check(gfxDestroySamplerState(owner.borrow(), sampler));
        sampler = {};
        take(owner.finish());
        take(owner.close());
        rhi_acceptance::check_debug(debug.Get());
    }

    GfxBuffer buffer(std::uint64_t size, const void* data = nullptr, GfxCpuAccess access = kGfxCpuAccess_None) {
        auto result = gfxCreateBuffer(owner.borrow(), size, data, access);
        require(bool(result), "B0 GPU buffer creation failed");
        buffers.push_back(result);
        return result;
    }

    void prepare(const std::string& text) {
        rhi::PipelineDesc desc;
        desc.name = "B0 material programs";
        desc.source.cs = text.c_str();
        auto job = take(owner.submit(desc));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
        for (;;) {
            if (auto result = owner.take(job)) {
                pipeline = take(std::move(*result));
                return;
            }
            require(std::chrono::steady_clock::now() < deadline, "B0 material pipeline preparation timed out");
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    material::Output dispatch(const source::MaterialProgram& material_program, const material::Context& context,
                              std::span<const std::byte> parameters, float* coverage = nullptr) {
        const auto gfx = owner.borrow();
        std::array<std::array<std::uint32_t, 4>, 2> bindings{};
        constexpr std::array keys{"color", "data"};
        for (std::uint32_t i = 0; i < keys.size(); ++i) {
            const auto at = std::ranges::find(material_program.textures(), keys[i], &source::MaterialTextureInput::key);
            require(at != material_program.textures().end(), "fixture texture input is missing");
            bindings[i] = {i, 0, at->slot, 0};
        }
        const auto binding_buffer = gfxCreateBuffer<std::array<std::uint32_t, 4>>(gfx, 2, bindings.data());
        require(bool(binding_buffer), "named binding buffer unavailable");
        buffers.push_back(binding_buffer);
        const auto context_buffer = buffer(sizeof(context), &context);
        const auto parameter_buffer = buffer(parameters.size(), parameters.data());
        const auto result = buffer(sizeof(material::Output) + (coverage ? sizeof(float) : 0));
        const auto readback = buffer(result.getSize(), nullptr, kGfxCpuAccess_Read);
        const auto program = pipeline.program();
        check(gfxProgramSetParameter(gfx, program, "contexts", context_buffer));
        check(gfxProgramSetParameter(gfx, program, "parameters", parameter_buffer));
        check(gfxProgramSetParameter(gfx, program, "results", result));
        check(gfxProgramSetParameter(gfx, program, "bindings", binding_buffer));
        check(gfxProgramSetParameter(gfx, program, "images", images.data(), static_cast<std::uint32_t>(images.size())));
        check(gfxProgramSetParameter(gfx, program, "repeat_sampler", sampler));
        check(gfxCommandBindKernel(gfx, pipeline.kernel()));
        check(gfxCommandDispatch(gfx, 1, 1, 1));
        check(gfxCommandCopyBuffer(gfx, readback, result));
        take(owner.finish());
        const auto* mapped = gfxBufferGetData<std::byte>(gfx, readback);
        require(mapped != nullptr, "B0 output readback unavailable");
        material::Output output;
        std::memcpy(&output, mapped, sizeof(output));
        if (coverage)
            std::memcpy(coverage, mapped + sizeof(output), sizeof(float));
        rhi_acceptance::check_debug(debug.Get());
        return output;
    }
};

void compare(const material::Output& got, const material::Output& wanted, float tolerance) {
    const auto scalar = [](float a, float b, float epsilon) {
        require(std::isfinite(a) && std::abs(a - b) <= epsilon,
                "material output disagrees with independent expected value");
    };
    for (std::size_t i = 0; i < 3; ++i) {
        scalar(got.base_color[i], wanted.base_color[i], std::min(tolerance, 2e-6f));
        scalar(got.emissive[i], wanted.emissive[i], tolerance);
        scalar(got.normal_world[i], wanted.normal_world[i], std::min(tolerance, 2e-6f));
    }
    scalar(got.roughness, wanted.roughness, tolerance);
    scalar(got.metallic, wanted.metallic, tolerance);
    scalar(got.coverage, wanted.coverage, tolerance);
}

void math_case(Device& device, const std::filesystem::path& evidence) {
    constexpr std::string_view text = R"source(
struct Textures { material.TextureInput data; material.TextureInput color; };
struct Params {
    float x; float low; float high;
    float a; float b; float t; float scalar_fraction;
    vector3 values; vector3 start; vector3 finish;
    vector2 fraction;
};
Params defaults() { Params p; return p; }
material.Output shade(material.Context c, Params p, Textures textures) {
    material.Output o;
    o.coverage = math.clamp(p.x, p.low, p.high) - 0.5;
    o.base_color = math.clamp(p.values, p.low, p.high);
    o.roughness = math.lerp(p.a, p.b, p.t);
    o.emissive = math.lerp(p.start, p.finish, p.t);
    vector2 fraction = math.fract(p.fraction);
    o.normal_world = vector3(math.fract(p.scalar_fraction), fraction.x, fraction.y);
    o.metallic = math.lerp(sample(textures.color, c.uv).w, sample(textures.data, c.uv).w, p.t);
    return o;
}
)source";
    const auto compiled = compile("Math", text, evidence, false);
    const auto& program = *compiled.program;
    std::vector<std::byte> parameters(program.default_parameters().begin(), program.default_parameters().end());
    constexpr std::array keys{"x", "low", "high", "a", "b", "t", "scalar_fraction"};
    constexpr std::array values{-2.f, 0.f, 1.f, 4.f, 12.f, .25f, 2.75f};
    for (std::size_t i = 0; i < keys.size(); ++i)
        parameter(program, parameters, keys[i], std::span{&values[i], 1});
    parameter(program, parameters, "values", std::array{.25f, -2.f, 3.f});
    parameter(program, parameters, "start", std::array{4.f, 8.f, 16.f});
    parameter(program, parameters, "finish", std::array{12.f, 24.f, 32.f});
    parameter(program, parameters, "fraction", std::array{-2.5f, 3.f});
    material::Context context;
    context.uv = {.25f, .75f}; // Authored texel 2 has alpha 255 in both views.
    device.prepare(compiled.shader);
    const auto got = device.dispatch(program, context, parameters);
    const material::Output wanted{.base_color = {.25f, 0, 1},
                                  .emissive = {6, 12, 20},
                                  .roughness = 6,
                                  .metallic = 1,
                                  .coverage = -.5f,
                                  .normal_world = {.75f, .5f, 0}};
    compare(got, wanted, 0);
}
} // namespace

void run(HWND window, const std::filesystem::path& evidence) {
    std::filesystem::create_directories(evidence);
    constexpr std::string_view text = R"source(
struct Textures { material.TextureInput data; material.TextureInput color; };
struct Params { float cutoff; vector3 tint; vector2 shift; float gain; vector4 weights; };
Params defaults() {
    Params p; p.cutoff=0.5; p.tint=vector3(0.2,0.4,0.8); p.shift=vector2(0.125,-0.125);
    p.gain=2.0; p.weights=vector4(0.1,0.2,0.3,0.4); return p;
}
struct Layer { material.Output output; };
Layer coverage_layer(material.Context c, Params p, vector4 color, vector4 data) {
    Layer layer;
    layer.output.coverage = data.w - p.cutoff;
    if (c.uv.x < 0.0) { layer.output.coverage = p.cutoff - data.w; return layer; }
    if (p.cutoff < 0.0) { layer.output.coverage = color.x + p.cutoff; }
    return layer;
}
material.Output shade(material.Context c, Params p, Textures textures) {
    vector2 uv = c.uv + p.shift;
    vector4 color = sample(textures.color, uv);
    vector4 data = sample(textures.data, uv);
    material.Output o;
    o.base_color = p.tint * (0.125 + 0.75 * c.uv.y);
    o.emissive = vector3(color.x, color.y, color.z) * p.gain;
    o.roughness = p.weights.x + data.x * p.weights.y;
    o.metallic = p.weights.z + data.y * p.weights.w;
    Layer layer = coverage_layer(c, p, color, data);
    o.coverage = layer.output.coverage;
    o.normal_world = c.normal_world;
    return o;
}
)source";
    const auto compiled = compile("ProgramB", text, evidence, true);
    const auto& program = *compiled.program;
    Device device(window, evidence / "shader-cache");
    device.sampler = gfxCreateSamplerState(device.owner.borrow(), D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                           D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    require(bool(device.sampler), "B0 sampler creation failed");
    constexpr std::array<std::uint8_t, 16> texels{16, 64,  128, 32,  224, 32,  96, 192,
                                                  64, 160, 240, 255, 128, 224, 16, 224};
    for (bool srgb : {true, false}) {
        auto texture = gfxCreateTexture2D(device.owner.borrow(), 2, 2,
                                          srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        require(bool(texture), "B0 image creation failed");
        device.images.push_back(texture);
        auto upload = device.buffer(texels.size(), texels.data(), kGfxCpuAccess_Write);
        check(gfxCommandCopyBufferToTexture(device.owner.borrow(), texture, upload));
    }
    material::Context context;
    context.uv = {-.5f, .375f};
    context.position_world = {3, -2, 5};
    context.normal_world = {0, .6f, .8f};
    context.geometric_normal_world = {0, 0, 1};
    context.tangent_world = {1, 0, 0, -1};
    // Shifted repeat UV (.625,.25) weights authored texels 0/1 by .25/.75.
    std::array<float, 4> color{};
    std::array<float, 4> data{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t i = 0; i < 2; ++i) {
            const double value = double(texels[i * 4 + c]) / 255;
            const double weight = i == 0 ? .25 : .75;
            data[c] += float(weight * value);
            color[c] += float(weight * (c == 3            ? value
                                        : value <= .04045 ? value / 12.92
                                                          : std::pow((value + .055) / 1.055, 2.4)));
        }
    const material::Output wanted{.base_color = {.08125f, .1625f, .325f},
                                  .emissive = {color[0] * 2, color[1] * 2, color[2] * 2},
                                  .roughness = .1f + data[0] * .2f,
                                  .metallic = .3f + data[1] * .4f,
                                  .coverage = .5f - data[3],
                                  .normal_world = {0, .6f, .8f}};
    device.prepare(compiled.shader);
    float coverage = 0;
    const auto got = device.dispatch(program, context, program.default_parameters(), &coverage);
    compare(got, wanted, 3e-3f);
    require(std::isfinite(coverage) && std::abs(coverage - wanted.coverage) <= 3e-3f &&
                (coverage >= 0) == (wanted.coverage >= 0),
            "coverage-only GPU entry disagrees with authored sampling truth");
    math_case(device, evidence);
    device.close();
    std::puts("Material programs: PASS (one GPU sampling ABI record and one authored math tuple)");
}
} // namespace material_program_e2e

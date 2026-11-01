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
#include <iomanip>
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

template <class T, class E>
T need(std::expected<T, E> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(file.good(), "material program evidence write failed");
}

constexpr std::string_view declarations = R"source(

struct Params {
    float cutoff;
    vector3 tint;
    vector2 shift;
    float gain;
    vector4 weights;
};
Params defaults() {
    Params p;
    p.cutoff = 0.5;
    p.tint = vector3(0.2, 0.4, 0.8);
    p.shift = vector2(0.125, -0.125);
    p.gain = 2.0;
    p.weights = vector4(0.1, 0.2, 0.3, 0.4);
    return p;
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
)source";
constexpr std::string_view outputs = R"source(
    o.emissive = vector3(color.x, color.y, color.z) * p.gain;
    o.roughness = p.weights.x + data.x * p.weights.y;
    o.metallic = p.weights.z + data.y * p.weights.w;
    Layer layer = coverage_layer(c, p, color, data);
    o.coverage = layer.output.coverage;
    o.normal_world = c.normal_world;
    return o;
}
)source";

// Generate boundary loads/stores from the sole registered native declarations,
// not a second handwritten HLSL Context/Output or assumed cbuffer layout.
std::string adapter(Runtime& runtime, TypeId type, std::string_view ns, bool input) {
    const auto* info = need(runtime.describe(type));
    const std::string qualified = std::string(ns) + (input ? "::ms_Context" : "::ms_Output");
    std::string code = input ? qualified + " load_" + std::string(ns) + "(uint base) {\n" + qualified + " value;\n"
                             : "void store_" + std::string(ns) + "(uint base," + qualified + " value) {\n";
    for (const auto& field : info->fields) {
        const auto* leaf = need(runtime.describe(field.value.type));
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

struct Compiled {
    std::array<Program, 2> programs;

    std::string adapters;
};

std::array<std::uint32_t, 2> texture_slots(const source::MaterialProgram& program) {
    constexpr std::array keys{"color", "data"};
    require(program.textures().size() == keys.size(), "fixture texture declarations changed");
    std::array<std::uint32_t, 2> slots{};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto at = std::ranges::find(program.textures(), keys[i], &source::MaterialTextureInput::key);
        require(at != program.textures().end(), "named material input is missing");
        slots[i] = at->slot;
    }
    require(slots[0] != slots[1], "distinct named inputs must have distinct local slots");
    return slots;
}

Compiled compile(const std::filesystem::path& evidence) {
    Runtime runtime;
    need(material::register_program_types(runtime));
    const auto context = need(runtime.native_type<material::Context>());
    const auto output = need(runtime.native_type<material::Output>());
    const auto texture_input = need(runtime.native_type<material::TextureInput>());
    Compiled result;
    constexpr const char* expressions[]{"o.base_color = p.tint * (0.25 + 0.5 * c.uv.x);\n",
                                        "o.base_color = p.tint * (0.125 + 0.75 * c.uv.y);\n"};
    constexpr const char* resources[]{
        "struct Textures { material.TextureInput color; material.TextureInput data; };\n",
        "struct Textures { material.TextureInput data; material.TextureInput color; };\n"};
    for (std::size_t i = 0; i < result.programs.size(); ++i) {
        const std::string name = i == 0 ? "ProgramA" : "ProgramB";
        const std::string text = resources[i] + std::string(declarations) + expressions[i] + std::string(outputs);
        write(evidence / (name + ".source"), text);
        auto program = need(source::compile_material(runtime, context, output, texture_input, name + ".source", text));
        require(program->context_layout().size == sizeof(material::Context) &&
                    program->output_layout().size == sizeof(material::Output),
                "native material boundary sizes differ");
        (void)texture_slots(*program);
        result.programs[i] = std::move(program);
        result.adapters += adapter(runtime, context, name, true) + adapter(runtime, output, name, false);
        result.adapters += adapter(runtime, context, "Coverage" + name, true);
    }
    return result; // Both Runtime and temporary source strings die before any evaluation/HLSL emission.
}

void parameter(const source::MaterialProgram& program, std::vector<std::byte>& bytes, std::string_view key,
               std::span<const float> values) {
    const auto at = std::ranges::find(program.parameters(), key, &source::MaterialParameter::key);
    require(at != program.parameters().end() && at->components == values.size() &&
                at->offset + values.size_bytes() <= bytes.size(),
            "parameter descriptor does not match declared leaf");
    std::memcpy(bytes.data() + at->offset, values.data(), values.size_bytes());
}

struct Texture {
    material::ImageRef image;

    bool srgb;

    material::Address address;
};

double decode(std::uint8_t byte, bool srgb) {
    const double value = double(byte) / 255;
    return !srgb ? value : value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}

Result<math::Vector4> sample(const void* data, math::Vector2 uv) {
    const auto& texture = *static_cast<const Texture*>(data);
    const auto& image = *texture.image;
    const std::array extent{int(image.width()), int(image.height())};
    std::array<int, 2> lower{};
    std::array<double, 2> weight{};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        double coordinate = uv[axis];
        if (texture.address == material::Address::repeat)
            coordinate -= std::floor(coordinate);
        else if (texture.address == material::Address::mirror) {
            coordinate -= std::floor(coordinate * .5) * 2;
            if (coordinate > 1)
                coordinate = 2 - coordinate;
        } else
            coordinate = std::clamp(coordinate, 0., 1.);
        const double pixel = coordinate * extent[axis] - .5;
        lower[axis] = static_cast<int>(std::floor(pixel));
        weight[axis] = pixel - lower[axis];
    }
    const auto at = [&](int x, int y, std::size_t channel) {
        const auto index = [&](int coordinate, int size) {
            return texture.address == material::Address::repeat ? (coordinate + size) % size
                                                                : std::clamp(coordinate, 0, size - 1);
        };
        const auto offset = std::size_t(index(y, extent[1]) * extent[0] + index(x, extent[0])) * 4 + channel;
        return decode(image.pixels()[offset], texture.srgb && channel < 3);
    };
    math::Vector4 result;
    for (std::size_t channel = 0; channel < 4; ++channel)
        result[channel] = static_cast<float>(std::lerp(
            std::lerp(at(lower[0], lower[1], channel), at(lower[0] + 1, lower[1], channel), weight[0]),
            std::lerp(at(lower[0], lower[1] + 1, channel), at(lower[0] + 1, lower[1] + 1, channel), weight[0]),
            weight[1]));
    return result;
}

using Environment = std::array<Texture, 2>;

struct Record {
    std::uint32_t program{};

    std::uint32_t environment{};

    material::Context context;

    std::vector<std::byte> parameters;

    // Independently selected texel weights, not computed by the CPU sampler.
    std::array<float, 4> texels;

    math::Vector3 tint{.2f, .4f, .8f};

    float cutoff{.5f};
};

material::Output expected(const Record& record, const Environment& environment) {
    std::array<math::Vector4, 2> samples{};
    for (std::size_t input = 0; input < 2; ++input)
        for (std::size_t channel = 0; channel < 4; ++channel)
            for (std::size_t pixel = 0; pixel < 4; ++pixel)
                samples[input][channel] +=
                    record.texels[pixel] *
                    static_cast<float>(decode(environment[input].image->pixels()[pixel * 4 + channel],
                                              environment[input].srgb && channel < 3));
    material::Output result;
    const float factor = record.program == 0 ? .25f + .5f * record.context.uv.x : .125f + .75f * record.context.uv.y;
    result.base_color = record.tint * factor;
    result.emissive = math::Vector3{samples[0].x, samples[0].y, samples[0].z} * 2.f;
    result.roughness = .1f + samples[1].x * .2f;
    result.metallic = .3f + samples[1].y * .4f;
    result.coverage = samples[1].w - record.cutoff;
    if (record.context.uv.x < 0)
        result.coverage = record.cutoff - samples[1].w;
    else if (record.cutoff < 0)
        result.coverage = samples[0].x + record.cutoff;
    result.normal_world = record.context.normal_world;
    return result;
}

void compare(const material::Output& got, const material::Output& wanted, float sample_tolerance) {
    const auto scalar = [](const char* field, float a, float b, float tolerance) {
        if (!std::isfinite(a) || std::abs(a - b) > tolerance) {
            std::fprintf(stderr, "%s: got=%.9g expected=%.9g tolerance=%.9g\n", field, a, b, tolerance);
            throw std::runtime_error("material output disagrees with independent expected value");
        }
    };
    for (std::size_t i = 0; i < 3; ++i) {
        scalar("base_color", got.base_color[i], wanted.base_color[i], std::min(sample_tolerance, 2e-6f));
        scalar("emissive", got.emissive[i], wanted.emissive[i], sample_tolerance);
        scalar("normal_world", got.normal_world[i], wanted.normal_world[i], std::min(sample_tolerance, 2e-6f));
    }
    scalar("roughness", got.roughness, wanted.roughness, sample_tolerance);
    scalar("metallic", got.metallic, wanted.metallic, sample_tolerance);
    scalar("coverage", got.coverage, wanted.coverage, sample_tolerance);
    require((got.coverage >= 0) == (wanted.coverage >= 0), "material coverage threshold changed");
}

constexpr std::string_view sampling_shader = R"hlsl(
ByteAddressBuffer contexts;
ByteAddressBuffer parameters;
RWByteAddressBuffer results;
StructuredBuffer<uint4> records;
StructuredBuffer<uint4> bindings;
Texture2D<float4> images[] : register(t0,space1);
SamplerState repeat_sampler;
SamplerState clamp_sampler;
SamplerState mirror_sampler;
uint record_count;
float4 atelier_material_sample(uint environment,uint local_slot,float2 uv) {
    uint4 binding=bindings[environment*2];
    if(binding.z!=local_slot)binding=bindings[environment*2+1];
    uint image=NonUniformResourceIndex(binding.x);
    if(binding.y==0)return images[image].SampleLevel(repeat_sampler,uv,0);
    if(binding.y==1)return images[image].SampleLevel(clamp_sampler,uv,0);
    return images[image].SampleLevel(mirror_sampler,uv,0);
}
)hlsl";

struct Device {
    rhi::Context owner;

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> debug;

    rhi::Pipeline pipeline;

    std::vector<GfxBuffer> buffers;

    std::vector<GfxTexture> images;

    std::array<GfxSamplerState, 3> samplers{};

    std::uint32_t submissions{};

    Device(HWND window, const std::filesystem::path& shader_cache)
        : owner(need(rhi::Context::open(
              {.window = window, .validation = rhi::Validation::gpu, .shader_cache = shader_cache}))),
          debug(rhi_acceptance::debug_queue(owner.borrow())) {
        require(owner.info().gpu_validation, "B0 GPU acceptance requires actual GPU validation");
        rhi_acceptance::print_adapter(owner.borrow());
    }

    ~Device() {
        pipeline = {};
        for (auto buffer : buffers)
            (void)gfxDestroyBuffer(owner.borrow(), buffer);
        for (auto image : images)
            (void)gfxDestroyTexture(owner.borrow(), image);
        for (auto sampler : samplers)
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
        for (auto& sampler : samplers)
            if (sampler) {
                check(gfxDestroySamplerState(owner.borrow(), sampler));
                sampler = {};
            }
        need(owner.finish());
        need(owner.close());
        rhi_acceptance::check_debug(debug.Get());
    }

    GfxBuffer buffer(std::uint64_t size, const void* data = nullptr, GfxCpuAccess access = kGfxCpuAccess_None) {
        auto result = gfxCreateBuffer(owner.borrow(), size, data, access);
        require(bool(result), "B0 GPU buffer creation failed");
        buffers.push_back(result);
        return result;
    }

    rhi::Result<rhi::Pipeline> prepare(const std::string& text) {
        rhi::PipelineDesc desc;
        desc.name = "B0 material programs";
        desc.source.cs = text.c_str();
        auto job = owner.submit(desc);
        if (!job)
            return std::unexpected(job.error());
        ++submissions;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
        for (;;) {
            if (auto result = owner.take(*job))
                return std::move(*result);
            require(std::chrono::steady_clock::now() < deadline, "B0 material pipeline preparation timed out");
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    GfxBuffer bindings(std::span<const Program> programs) {
        // Image/address environments stay unchanged; only each version's key
        // mapping supplies slots. Program index changes select the matching table.
        constexpr std::array<std::array<std::uint32_t, 4>, 6> resources{
            {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 1, 0, 0}, {3, 1, 0, 0}, {1, 2, 0, 0}, {3, 2, 0, 0}}};
        std::vector<std::array<std::uint32_t, 4>> values;
        for (std::size_t environment = 0; environment < 3; ++environment)
            for (const auto& program : programs) {
                const auto slots = texture_slots(*program);
                for (std::size_t input = 0; input < slots.size(); ++input) {
                    auto binding = resources[environment * 2 + input];
                    binding[2] = slots[input];
                    values.push_back(binding);
                }
            }
        auto result = gfxCreateBuffer<std::array<std::uint32_t, 4>>(
            owner.borrow(), static_cast<std::uint32_t>(values.size()), values.data());
        require(bool(result), "named binding buffer unavailable");
        buffers.push_back(result);
        return result;
    }

    std::vector<material::Output> dispatch(std::span<const Record> input, GfxBuffer bindings,
                                           std::uint32_t program_count, std::vector<float>* coverage = nullptr) {
        const auto gfx = owner.borrow();
        std::vector<material::Context> contexts;
        std::vector<std::byte> parameters;
        std::vector<std::array<std::uint32_t, 4>> records;
        for (const auto& record : input) {
            records.push_back({record.program, record.environment * program_count + record.program,
                               static_cast<std::uint32_t>(parameters.size()),
                               static_cast<std::uint32_t>(contexts.size() * sizeof(material::Context))});
            contexts.push_back(record.context);
            parameters.insert(parameters.end(), record.parameters.begin(), record.parameters.end());
        }
        auto context_buffer = buffer(contexts.size() * sizeof(material::Context), contexts.data());
        auto parameter_buffer = buffer(parameters.size(), parameters.data());
        auto record_buffer = gfxCreateBuffer<std::array<std::uint32_t, 4>>(
            gfx, static_cast<std::uint32_t>(records.size()), records.data());
        require(bool(record_buffer), "B0 record buffer unavailable");
        buffers.push_back(record_buffer);
        auto result = buffer(input.size() * (sizeof(material::Output) + (coverage ? sizeof(float) : 0)));
        auto readback = buffer(result.getSize(), nullptr, kGfxCpuAccess_Read);
        const auto program = pipeline.program();
        check(gfxProgramSetParameter(gfx, program, "contexts", context_buffer));
        check(gfxProgramSetParameter(gfx, program, "parameters", parameter_buffer));
        check(gfxProgramSetParameter(gfx, program, "results", result));
        check(gfxProgramSetParameter(gfx, program, "records", record_buffer));
        check(gfxProgramSetParameter(gfx, program, "bindings", bindings));
        check(gfxProgramSetParameter(gfx, program, "images", images.data(), static_cast<std::uint32_t>(images.size())));
        constexpr const char* names[]{"repeat_sampler", "clamp_sampler", "mirror_sampler"};
        for (std::size_t i = 0; i < samplers.size(); ++i)
            check(gfxProgramSetParameter(gfx, program, names[i], samplers[i]));
        const auto count = static_cast<std::uint32_t>(input.size());
        check(gfxProgramSetParameter(gfx, program, "record_count", count));
        check(gfxCommandBindKernel(gfx, pipeline.kernel()));
        check(gfxCommandDispatch(gfx, (count + 31) / 32, 1, 1));
        check(gfxCommandCopyBuffer(gfx, readback, result));
        need(owner.finish());
        const auto* mapped = gfxBufferGetData<std::byte>(gfx, readback);
        require(mapped != nullptr, "B0 output readback unavailable");
        std::vector<material::Output> output(input.size());
        std::memcpy(output.data(), mapped, output.size() * sizeof(material::Output));
        if (coverage) {
            coverage->resize(input.size());
            std::memcpy(coverage->data(), mapped + output.size() * sizeof(material::Output),
                        coverage->size() * sizeof(float));
        }
        rhi_acceptance::check_debug(debug.Get());
        return output;
    }
};

bool math_cases(Device& device, std::span<const Environment> environments, const std::filesystem::path& evidence) {
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
    Program program;
    std::string adapters;
    {
        Runtime runtime;
        need(material::register_program_types(runtime));
        const auto context = need(runtime.native_type<material::Context>());
        const auto output = need(runtime.native_type<material::Output>());
        const auto texture_input = need(runtime.native_type<material::TextureInput>());
        program = need(source::compile_material(runtime, context, output, texture_input, "Math.source", text));
        adapters = adapter(runtime, context, "Math", true) + adapter(runtime, output, "Math", false);
    }
    const auto slots = texture_slots(*program);
    std::string shader = std::string(sampling_shader) + need(program->hlsl("Math")) + adapters;
    shader += "[numthreads(32,1,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
              "if(id.x>=record_count)return; uint4 r=records[id.x]; uint base=id.x*" +
              std::to_string(sizeof(material::Output)) +
              ";\n"
              "store_Math(base,Math::ms_shade(r.y,load_Math(r.w),Math::ms_load_parameters(parameters,r.z)));\n}\n";
    write(evidence / "Math.source", text);
    write(evidence / "math-composed.hlsl", shader);

    struct Case {
        const char* name;

        float x;

        float low;

        float high;

        float a;

        float b;

        float t;

        float scalar_fraction;

        std::array<float, 3> start;

        std::array<float, 3> finish;

        std::array<float, 2> fraction;

        material::Output wanted;
    };
    // Independent exact expectations from docs/math.md, not HLSL intrinsics or
    // the CPU evaluator. next_one is the immediately preceding float below 1.
    constexpr float next_one = 0x1.fffffep-1f;
    const std::array<Case, 4> cases{{{"reversed-end1",
                                      .5f,
                                      1,
                                      0,
                                      1e8f,
                                      1,
                                      1,
                                      -1e-8f,
                                      {1e8f, -1e8f, 8},
                                      {1, -1, 24},
                                      {-1e-8f, -2},
                                      {.base_color = {1, 1, 1},
                                       .emissive = {1, -1, 24},
                                       .roughness = 1,
                                       .coverage = .5f,
                                       .normal_world = {next_one, next_one, 0}}},
                                     {"ordered-end0",
                                      .5f,
                                      0,
                                      1,
                                      1e8f,
                                      1,
                                      0,
                                      0,
                                      {1e8f, -1e8f, 8},
                                      {1, -1, 24},
                                      {1.25f, -1.25f},
                                      {.base_color = {.25f, 0, 1},
                                       .emissive = {1e8f, -1e8f, 8},
                                       .roughness = 1e8f,
                                       .coverage = 0,
                                       .normal_world = {0, .25f, .75f}}},
                                     {"ordered-below-mid",
                                      -2,
                                      0,
                                      1,
                                      4,
                                      12,
                                      .25f,
                                      2.75f,
                                      {4, 8, 16},
                                      {12, 24, 32},
                                      {-2.5f, 3},
                                      {.base_color = {.25f, 0, 1},
                                       .emissive = {6, 12, 20},
                                       .roughness = 6,
                                       .coverage = -.5f,
                                       .normal_world = {.75f, .5f, 0}}},
                                     {"ordered-above-mid",
                                      3,
                                      0,
                                      1,
                                      4,
                                      12,
                                      .75f,
                                      -3,
                                      {4, 8, 16},
                                      {12, 24, 32},
                                      {1, -1e-8f},
                                      {.base_color = {.25f, 0, 1},
                                       .emissive = {10, 20, 28},
                                       .roughness = 10,
                                       .coverage = .5f,
                                       .normal_world = {0, 0, next_one}}}}};
    std::vector<Record> records;
    for (const auto& test : cases)
        for (std::uint32_t environment = 0; environment < environments.size(); ++environment) {
            Record record;
            record.environment = environment;
            record.context.uv = {.25f, .75f}; // Exact center of texel 2, in each real resource environment.
            record.parameters.assign(program->default_parameters().begin(), program->default_parameters().end());
            constexpr std::array keys{"x", "low", "high", "a", "b", "t", "scalar_fraction"};
            const std::array values{test.x, test.low, test.high, test.a, test.b, test.t, test.scalar_fraction};
            for (std::size_t i = 0; i < keys.size(); ++i)
                parameter(*program, record.parameters, keys[i], std::span{&values[i], 1});
            parameter(*program, record.parameters, "values", std::array{.25f, -2.f, 3.f});
            parameter(*program, record.parameters, "start", test.start);
            parameter(*program, record.parameters, "finish", test.finish);
            parameter(*program, record.parameters, "fraction", test.fraction);
            records.push_back(std::move(record));
        }
    device.pipeline = need(device.prepare(shader));
    const auto gpu = device.dispatch(records, device.bindings(std::span{&program, 1}), 1);
    std::ofstream log(evidence / "math-outputs.csv");
    log << "case,environment,field,cpu,gpu,expected,cpu_pass,gpu_pass,samples_color,samples_data\n"
        << std::setprecision(9);
    const auto fields = [](const material::Output& value) {
        return std::array{value.coverage,       value.roughness,      value.base_color.x,   value.base_color.y,
                          value.base_color.z,   value.emissive.x,     value.emissive.y,     value.emissive.z,
                          value.normal_world.x, value.normal_world.y, value.normal_world.z, value.metallic};
    };
    constexpr std::array names{"clamp.coverage", "lerp.scalar",    "clamp.vector.x", "clamp.vector.y",
                               "clamp.vector.z", "lerp.vector.x",  "lerp.vector.y",  "lerp.vector.z",
                               "fract.scalar",   "fract.vector.x", "fract.vector.y", "lerp.samples"};
    bool correct = true;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        const auto& test = cases[i / environments.size()];
        const auto& environment = environments[record.environment];
        struct Counted {
            const Texture* texture;

            unsigned* calls;
        };
        std::array<unsigned, 2> calls{};
        const std::array<Counted, 2> counted{{{&environment[0], &calls[0]}, {&environment[1], &calls[1]}}};
        const auto counted_sample = +[](const void* data, math::Vector2 uv) -> Result<math::Vector4> {
            const auto& value = *static_cast<const Counted*>(data);
            ++*value.calls;
            return sample(value.texture, uv);
        };
        const std::array<source::MaterialTexture, 2> textures{
            {{slots[0], &counted[0], counted_sample}, {slots[1], &counted[1], counted_sample}}};
        material::Output cpu;
        need(program->evaluate(std::as_bytes(std::span{&record.context, 1}), record.parameters,
                               std::as_writable_bytes(std::span{&cpu, 1}), textures));
        auto wanted = test.wanted;
        const double alpha_a = double(environment[0].image->pixels()[11]) / 255;
        const double alpha_b = double(environment[1].image->pixels()[11]) / 255;
        wanted.metallic = static_cast<float>(alpha_a * (1 - double(test.t)) + alpha_b * test.t);
        const auto cpu_fields = fields(cpu);
        const auto gpu_fields = fields(gpu[i]);
        const auto expected_fields = fields(wanted);
        correct &= calls[0] == 1 && calls[1] == 1;
        for (std::size_t field = 0; field < names.size(); ++field) {
            const bool sampling = field == names.size() - 1;
            const bool fraction = field >= 8 && field <= 10;
            const auto matches = [&](float actual, float tolerance) {
                return std::isfinite(actual) && std::abs(actual - expected_fields[field]) <= tolerance &&
                       (!fraction || (actual >= 0 && actual < 1)) &&
                       (field != 0 || ((actual >= 0) == (wanted.coverage >= 0)));
            };
            // Sampling precision is separate. No tolerance can conceal a math
            // endpoint, coverage sign, or fract returning 1 instead of < 1.
            const bool cpu_ok = matches(cpu_fields[field], sampling ? 2e-6f : 0);
            const bool gpu_ok = matches(gpu_fields[field], sampling ? 3e-3f : 0);
            log << test.name << ',' << record.environment << ',' << names[field] << ',' << cpu_fields[field] << ','
                << gpu_fields[field] << ',' << expected_fields[field] << ',' << cpu_ok << ',' << gpu_ok << ','
                << calls[0] << ',' << calls[1] << '\n';
            if (!cpu_ok || !gpu_ok)
                std::fprintf(stderr, "math %s env=%u %s: cpu=%.9g gpu=%.9g expected=%.9g\n", test.name,
                             record.environment, names[field], cpu_fields[field], gpu_fields[field],
                             expected_fields[field]);
            correct &= cpu_ok && gpu_ok;
        }
    }
    log.flush();
    require(log.good(), "B0 math evidence write failed");
    return correct; // Keep every case's raw results, including later failures in the red run.
}
} // namespace

void run(HWND window, const std::filesystem::path& evidence) {
    std::filesystem::create_directories(evidence);
    auto compiled = compile(evidence);
    const auto& programs = compiled.programs;
    require(programs[0]->parameter_layout().size == programs[1]->parameter_layout().size &&
                std::ranges::is_permutation(programs[0]->parameters(), programs[1]->parameters(),
                                            [](const auto& a, const auto& b) {
                                                return a.key == b.key && a.components == b.components &&
                                                       a.offset == b.offset;
                                            }),
            "two source programs must share the same parameter layout for index-only control");
    for (const auto& program : programs) {
        auto defaults =
            std::vector<std::byte>(program->default_parameters().begin(), program->default_parameters().end());
        auto expected_defaults = defaults;
        parameter(*program, expected_defaults, "cutoff", std::array{.5f});
        parameter(*program, expected_defaults, "tint", std::array{.2f, .4f, .8f});
        parameter(*program, expected_defaults, "shift", std::array{.125f, -.125f});
        parameter(*program, expected_defaults, "gain", std::array{2.f});
        parameter(*program, expected_defaults, "weights", std::array{.1f, .2f, .3f, .4f});
        require(program->parameters().size() == 5 && defaults == expected_defaults,
                "mixed parameter defaults differ from source declarations");
    }
    std::string shader = std::string(sampling_shader) + need(programs[0]->hlsl("ProgramA")) +
                         need(programs[1]->hlsl("ProgramB")) + need(programs[0]->hlsl_coverage("CoverageProgramA")) +
                         need(programs[1]->hlsl_coverage("CoverageProgramB")) + compiled.adapters;
    shader +=
        "[numthreads(32,1,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
        "if(id.x>=record_count)return; uint4 r=records[id.x]; uint base=id.x*" +
        std::to_string(sizeof(material::Output)) +
        ";\n"
        "uint coverage_base=record_count*" +
        std::to_string(sizeof(material::Output)) +
        "+id.x*4;\n"
        "if(r.x==0)store_ProgramA(base,ProgramA::ms_shade(r.y,load_ProgramA(r.w),ProgramA::ms_load_parameters(parameters,r.z)));\n"
        "else store_ProgramB(base,ProgramB::ms_shade(r.y,load_ProgramB(r.w),ProgramB::ms_load_parameters(parameters,r.z)));\n"
        "float coverage;\n"
        "if(r.x==0)coverage=CoverageProgramA::ms_coverage(r.y,load_CoverageProgramA(r.w),CoverageProgramA::ms_load_parameters(parameters,r.z));\n"
        "else coverage=CoverageProgramB::ms_coverage(r.y,load_CoverageProgramB(r.w),CoverageProgramB::ms_load_parameters(parameters,r.z));\n"
        "results.Store(coverage_base,asuint(coverage));\n}\n";
    write(evidence / "composed.hlsl", shader);

    auto first = std::make_shared<const material::Image>(
        2, 2, std::vector<std::uint8_t>{16, 64, 128, 32, 224, 32, 96, 192, 64, 160, 240, 255, 128, 224, 16, 224});
    auto second = std::make_shared<const material::Image>(
        2, 2, std::vector<std::uint8_t>{192, 48, 16, 224, 32, 192, 128, 64, 240, 96, 64, 192, 80, 16, 224, 32});
    const std::array<Environment, 3> environments{
        {{{{first, true, material::Address::repeat}, {first, false, material::Address::repeat}}},
         {{{second, true, material::Address::clamp}, {second, false, material::Address::clamp}}},
         {{{first, false, material::Address::mirror}, {second, false, material::Address::mirror}}}}};
    std::vector<Record> records;
    constexpr std::array<math::Vector2, 3> uvs{{{.125f, .875f}, {.375f, .625f}, {-.5f, .375f}}};
    for (std::uint32_t environment = 0; environment < environments.size(); ++environment)
        for (std::size_t point = 0; point < uvs.size(); ++point)
            for (std::uint32_t program = 0; program < programs.size(); ++program) {
                Record record;
                record.program = program;
                record.environment = environment;
                record.context.uv = uvs[point];
                record.context.position_world = {3, -2, 5};
                record.context.normal_world = {0, .6f, .8f};
                record.context.geometric_normal_world = {0, 0, 1};
                record.context.tangent_world = {1, 0, 0, -1};
                record.parameters.assign(programs[program]->default_parameters().begin(),
                                         programs[program]->default_parameters().end());
                if (point == 0)
                    record.texels = {0, 0, 1, 0};
                else if (point == 1)
                    record.texels = {.25f, .25f, .25f, .25f};
                else if (environment == 0)
                    record.texels = {.25f, .75f, 0, 0};
                else if (environment == 1)
                    record.texels = {1, 0, 0, 0};
                else
                    record.texels = {.75f, .25f, 0, 0};
                records.push_back(std::move(record));
            }
    // A texel-center alpha of 255 gives exact 1-1=0 on both backends, without
    // imposing bit identity on a filtered value near a rounding boundary.
    records[0].cutoff = 1;
    parameter(*programs[0], records[0].parameters, "cutoff", std::array{records[0].cutoff});

    Device device(window, evidence / "shader-cache");
    constexpr D3D12_TEXTURE_ADDRESS_MODE addresses[]{D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                                     D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
    for (std::size_t i = 0; i < device.samplers.size(); ++i) {
        device.samplers[i] =
            gfxCreateSamplerState(device.owner.borrow(), D3D12_FILTER_MIN_MAG_MIP_LINEAR, addresses[i], addresses[i]);
        require(bool(device.samplers[i]), "B0 sampler creation failed");
    }
    for (const auto& image : {first, second})
        for (bool srgb : {true, false}) {
            auto texture = gfxCreateTexture2D(device.owner.borrow(), 2, 2,
                                              srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, 1);
            require(bool(texture), "B0 image creation failed");
            device.images.push_back(texture);
            auto upload = device.buffer(image->pixels().size(), image->pixels().data(), kGfxCpuAccess_Write);
            check(gfxCommandCopyBufferToTexture(device.owner.borrow(), texture, upload));
        }
    const auto binding_buffer = device.bindings(programs);
    device.pipeline = need(device.prepare(shader));
    std::ofstream log(evidence / "outputs.csv");
    log << "stage,record,program,environment,cpu_base_r,gpu_base_r,expected_base_r,cpu_coverage,gpu_coverage,expected_coverage,gpu_coverage_only\n"
        << std::setprecision(9);
    const auto verify = [&](const char* stage) {
        std::vector<float> coverage;
        const auto gpu =
            device.dispatch(records, binding_buffer, static_cast<std::uint32_t>(programs.size()), &coverage);
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto& record = records[i];
            const auto& environment = environments[record.environment];
            const auto slots = texture_slots(*programs[record.program]);
            const std::array<source::MaterialTexture, 2> textures{
                {{slots[0], &environment[0], sample}, {slots[1], &environment[1], sample}}};
            material::Output cpu;
            need(programs[record.program]->evaluate(std::as_bytes(std::span{&record.context, 1}), record.parameters,
                                                    std::as_writable_bytes(std::span{&cpu, 1}), textures));
            const auto wanted = expected(record, environment);
            log << stage << ',' << i << ',' << record.program << ',' << record.environment << ',' << cpu.base_color.x
                << ',' << gpu[i].base_color.x << ',' << wanted.base_color.x << ',' << cpu.coverage << ','
                << gpu[i].coverage << ',' << wanted.coverage << ',' << coverage[i] << '\n';
            log.flush();
            require(log.good(), "B0 numeric evidence write failed");
            compare(cpu, wanted, 2e-6f);
            compare(gpu[i], wanted, 3e-3f); // D3D texture decode/filter precision is not CPU bit identity.
            require(std::isfinite(coverage[i]) && std::abs(coverage[i] - gpu[i].coverage) <= 2e-6f &&
                        (coverage[i] >= 0) == (gpu[i].coverage >= 0),
                    "coverage-only GPU entry must match full point coverage, including branches and cutoff equality");
        }
    };
    verify("original");
    for (auto& record : records)
        record.program ^= 1;
    verify("program-index-swap");
    records[0].tint = {.7f, .1f, .3f};
    parameter(*programs[records[0].program], records[0].parameters, "tint", std::array{.7f, .1f, .3f});
    verify("parameter-only");
    // Change the coverage branch and its texture dependency without compiling
    // either GPU entry again. Every resource environment exercises this path.
    for (auto& record : records)
        if (record.context.uv.x > 0.3f) {
            record.cutoff = -.25f;
            parameter(*programs[record.program], record.parameters, "cutoff", std::array{record.cutoff});
        }
    verify("coverage-parameter-branch");
    require(device.submissions == 1, "index/parameter edits must use the original prepared pipeline");

    {
        Runtime runtime;
        need(material::register_program_types(runtime));
        const auto rejected = source::compile_material(
            runtime, need(runtime.native_type<material::Context>()), need(runtime.native_type<material::Output>()),
            need(runtime.native_type<material::TextureInput>()), "invalid.source",
            "material.Output shade(material.Context c, Missing p) { return c; }");
        require(!rejected, "invalid replacement source was accepted");
    }
    const auto failed = device.prepare(shader + "\n#error B0 deliberate candidate failure\n");
    require(!failed && failed.error().code == rhi::ErrorCode::shader_failed,
            "invalid GPU candidate did not fail preparation");
    verify("failed-candidate-retains-old");
    const auto& record = records[0];
    const auto& environment = environments[record.environment];
    const std::array<source::MaterialTexture, 1> missing{
        {{texture_slots(*programs[record.program])[0], &environment[0], sample}}};
    material::Output preserved;
    preserved.coverage = 17;
    const auto original = preserved;
    const auto rejected =
        programs[record.program]->evaluate(std::as_bytes(std::span{&record.context, 1}), record.parameters,
                                           std::as_writable_bytes(std::span{&preserved, 1}), missing);
    require(!rejected, "missing slot must reject without replacing CPU output");
    compare(preserved, original, 0);
    const bool math_correct = math_cases(device, environments, evidence);
    device.close();
    require(math_correct, "Source math semantics or single operand evaluation disagreed");
    std::puts(
        "Material programs: PASS (CPU/compute LOD0, coverage-only/full point parity, branches, two real programs, index swap, parameters, independent bindings, mixed layout, Runtime lifetime, failed candidates, GBV)");
}
} // namespace material_program_e2e

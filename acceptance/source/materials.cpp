#include "../helpers.hpp"
#include "source/material_program.hpp"
#include "material/program.hpp"
#include "runtime/native.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

bool near(float a, float b) {
    return std::abs(a - b) < 0.00001f;
}

struct UnusedDefault {
    float value;
};

const std::string declarations = R"(
    struct Mask { float cutoff; vector2 scale; };
    struct Params { float gain; vector3 tint; Mask mask; };
    struct ColorInputs { material.TextureInput color; };
    struct Textures { material.TextureInput alpha; ColorInputs surface; };
    Params defaults() {
        Params p;
        p.gain = 2.0; p.tint = vector3(0.25,0.5,0.75);
        p.mask.cutoff = 0.5; p.mask.scale = vector2(1.0,1.0);
        return p;
    }
)";

struct Texture {
    math::Vector4 value;

    bool uv_alpha{};
};

Result<math::Vector4> sample(const void* data, math::Vector2 uv) noexcept {
    auto value = static_cast<const Texture*>(data)->value;
    if (static_cast<const Texture*>(data)->uv_alpha)
        value.w = uv.x;
    return value;
}

std::uint32_t slot(const source::MaterialProgram& program, std::string_view key) {
    const auto found = std::ranges::find(program.textures(), key, &source::MaterialTextureInput::key);
    require(found != program.textures().end(), "Material lost a declared texture key.");
    return found->slot;
}

material::Output evaluate(const source::MaterialProgram& program, material::Context context,
                          std::span<const source::MaterialTexture> textures,
                          std::span<const std::byte> parameters = {}) {
    material::Output output;
    take(program.evaluate(std::as_bytes(std::span(&context, 1)),
                          parameters.empty() ? program.default_parameters() : parameters,
                          std::as_writable_bytes(std::span(&output, 1)), textures));
    return output;
}
}

void verify_source_materials() {
    std::shared_ptr<const source::MaterialProgram> a;
    std::shared_ptr<const source::MaterialProgram> b;
    std::shared_ptr<const source::MaterialProgram> oriented;
    {
        Runtime runtime;
        take(material::register_program_types(runtime));
        auto unused =
            NativeRegistration<UnusedDefault>{runtime, "unused.Value", TypeKind::record}
                .field<&UnusedDefault::value>("value", {TypeReference::native<float>()}, offsetof(UnusedDefault, value))
                .build();
        unused.native.construct = [](void*) noexcept -> Result<void> {
            return std::unexpected(Error{ErrorCode::construction_failed, "Injected native default failure."});
        };
        take(runtime.register_type(std::move(unused)));
        const auto context = take(runtime.native_type<material::Context>());
        const auto output = take(runtime.native_type<material::Output>());
        const auto texture_input = take(runtime.native_type<material::TextureInput>());
        const auto types_before = runtime.type_count();
        const auto failed_defaults = source::compile_material(runtime, context, output, texture_input, "failed.ms", R"(
            struct Params { unused.Value input; };
            Params defaults() { Params p; return p; }
            material.Output shade(material.Context c, Params p) {
                material.Output o; o.coverage=p.input.value; return o;
            }
        )");
        require(!failed_defaults && runtime.type_count() == types_before,
                "Failed native default capture published partial Source types.");
        a = take(source::compile_material(runtime, context, output, texture_input, "mask-a.ms", declarations + R"(
            material.Output shade(material.Context c, Params p, Textures textures) {
                material.Output o;
                vector4 alpha = sample(textures.alpha,c.uv);
                vector4 color = sample(textures.surface.color,c.uv);
                o.base_color = p.tint * color.x;
                o.coverage = alpha.w - p.mask.cutoff;
                o.normal_world = c.normal_world;
                return o;
            }
        )"));
        b = take(source::compile_material(runtime, context, output, texture_input, "mask-b.ms", declarations + R"(
            float margin(vector2 uv, Params p, material.TextureInput input) {
                vector4 a = sample(input,vector2(uv.x * p.mask.scale.x, uv.y * p.mask.scale.y));
                if (uv.y > 0.5) { return p.mask.cutoff - a.w; }
                return a.w * p.gain - p.mask.cutoff;
            }
            material.Output shade(material.Context c, Params p, Textures textures) {
                material.Output o;
                o.emissive = vector3(sample(textures.surface.color,c.uv).x,0.0,0.0);
                o.coverage = margin(c.uv,p,textures.alpha);
                return o;
            }
        )"));
        oriented = take(source::compile_material(runtime, context, output, texture_input, "orientation.ms", R"(
            struct Params { float enabled; };
            struct Textures { material.TextureInput normal; };
            Params defaults() { Params p; p.enabled=1.0; return p; }
            vector2 orientation(material.TextureInput input,vector2 uv) { return sample_orientation(input,uv); }
            material.Output shade(material.Context c,Params p,Textures textures) {
                material.Output o; o.coverage=99.0;
                if (p.enabled>0.0) {
                    vector2 direction=orientation(textures.normal,c.uv);
                    vector4 color=sample(textures.normal,direction*0.25+vector2(0.5,0.5));
                    o.base_color=vector3(direction.x,direction.y,color.x);
                }
                return o;
            }
        )"));
    }
    // Detached programs must remain executable after their compiling Runtime is gone.
    struct OrientationProbe {
        mutable unsigned queries{};

        mutable unsigned samples{};

        mutable math::Vector2 sampled_uv{};
    } probe;
    const auto query_orientation = +[](const void* data, math::Vector2 uv) noexcept -> Result<math::Vector2> {
        ++static_cast<const OrientationProbe*>(data)->queries;
        math::Vector2 direction{1, 1};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            double u = uv[axis];
            u -= std::floor(u * .5) * 2;
            direction[axis] = u > 1 ? -1.0f : 1.0f;
        }
        return direction;
    };
    const auto orientation_sample = +[](const void* data, math::Vector2 uv) noexcept -> Result<math::Vector4> {
        const auto& value = *static_cast<const OrientationProbe*>(data);
        ++value.samples;
        value.sampled_uv = uv;
        return math::Vector4{0.4f, 0.6f, 0.8f, 1};
    };
    std::array<source::MaterialTexture, 1> orientation_bindings{
        {{slot(*oriented, "normal"), &probe, orientation_sample, query_orientation}}};
    material::Context orientation_context;
    orientation_context.uv = {-0.25f, 1.25f};
    const auto orientation = evaluate(*oriented, orientation_context, orientation_bindings);
    require(orientation.base_color == math::Vector3{-1, -1, 0.4f} && orientation.coverage == 99 && probe.queries == 1 &&
                probe.samples == 1 && probe.sampled_uv == math::Vector2{0.25f, 0.25f},
            "Orientation callback lost its typed token, operands or result.");
    probe.queries = probe.samples = 0;
    const std::array skipped{0.0f};
    require(
        evaluate(*oriented, orientation_context, orientation_bindings, std::as_bytes(std::span(skipped))).coverage ==
                99 &&
            probe.queries == 0 && probe.samples == 0,
        "An untaken orientation branch executed its callback.");
    material::Output untouched;
    untouched.coverage = 321;
    const auto orientation_failure = [&](ErrorCode code) {
        std::array<std::byte, sizeof(untouched)> before;
        std::memcpy(before.data(), &untouched, before.size());
        const auto result =
            oriented->evaluate(std::as_bytes(std::span(&orientation_context, 1)), oriented->default_parameters(),
                               std::as_writable_bytes(std::span(&untouched, 1)), orientation_bindings);
        require(!result && result.error().code == code && std::memcmp(before.data(), &untouched, before.size()) == 0,
                "Orientation callback failure partially published Output.");
    };
    orientation_bindings[0].orientation = +[](const void*, math::Vector2) noexcept -> Result<math::Vector2> {
        return std::unexpected(Error{ErrorCode::execution_aborted, "Injected orientation failure."});
    };
    orientation_failure(ErrorCode::execution_aborted);
    orientation_bindings[0].orientation = +[](const void*, math::Vector2) noexcept -> Result<math::Vector2> {
        return math::Vector2{1, std::numeric_limits<float>::infinity()};
    };
    orientation_failure(ErrorCode::invocation_failed);
    orientation_bindings[0].orientation = query_orientation;
    require(evaluate(*oriented, orientation_context, orientation_bindings).base_color == math::Vector3{-1, -1, 0.4f},
            "Orientation failures damaged a later call.");

    Texture alpha{{0.1f, 0.2f, 0.3f, 0.9f}, true};
    Texture color{{0.2f, 0.4f, 0.6f, 0.8f}};
    std::array<source::MaterialTexture, 2> textures{
        {{slot(*a, "surface.color"), &color, sample}, {slot(*a, "alpha"), &alpha, sample}}};
    require(a->parameter_layout().size == 28 && a->parameters().size() == 4 && a->parameters()[1].key == "tint" &&
                a->parameters()[1].offset == 4 && a->parameters()[1].components == 3 &&
                a->parameters()[2].key == "mask.cutoff" && a->parameters()[2].offset == 16 &&
                a->parameters()[3].key == "mask.scale" && a->parameters()[3].offset == 20,
            "Material parameter storage differs from the declared layout.");
    material::Context context;
    context.uv = {0.25f, 0.75f};
    context.normal_world = {0, 1, 0};
    const auto result = evaluate(*a, context, textures);
    require(near(result.coverage, -0.25f) && near(result.base_color.x, 0.05f) && near(result.base_color.z, 0.15f) &&
                near(result.roughness, 0.65f) && result.normal_world == math::Vector3{0, 1, 0},
            "Detached evaluator lost parameters, sampling or native defaults.");
    untouched.coverage = 123;
    const auto sample_failure = [&](ErrorCode code) {
        std::array<std::byte, sizeof(untouched)> before;
        std::memcpy(before.data(), &untouched, before.size());
        const auto result = a->evaluate(std::as_bytes(std::span(&context, 1)), a->default_parameters(),
                                        std::as_writable_bytes(std::span(&untouched, 1)), textures);
        require(!result && result.error().code == code && std::memcmp(before.data(), &untouched, before.size()) == 0,
                "Sampler failure partially published Output.");
    };
    textures[1].sample = +[](const void*, math::Vector2) noexcept -> Result<math::Vector4> {
        return std::unexpected(Error{ErrorCode::execution_aborted, "Injected sampler failure."});
    };
    sample_failure(ErrorCode::execution_aborted);
    textures[1].sample = +[](const void*, math::Vector2) noexcept -> Result<math::Vector4> {
        return math::Vector4{0.1f, 0.2f, std::numeric_limits<float>::quiet_NaN(), 1};
    };
    sample_failure(ErrorCode::invocation_failed);
    textures[1].sample = sample;
    require(near(evaluate(*a, context, textures).coverage, -0.25f), "Sampler failures damaged a later call.");
    textures[0].slot = slot(*b, "surface.color");
    textures[1].slot = slot(*b, "alpha");
    textures[0].data = &b;
    textures[0].sample = +[](const void* owner, math::Vector2) noexcept -> Result<math::Vector4> {
        auto* program = static_cast<const std::shared_ptr<const source::MaterialProgram>*>(owner);
        const_cast<std::shared_ptr<const source::MaterialProgram>*>(program)->reset();
        return math::Vector4{0.2f, 0.4f, 0.6f, 0.8f};
    };
    context.uv = {0.5f, 0.25f};
    const auto* active = b.get();
    take(active->evaluate(std::as_bytes(std::span(&context, 1)), b->default_parameters(),
                          std::as_writable_bytes(std::span(&untouched, 1)), textures));
    require(!b && near(untouched.coverage, 0.5f), "Dropping the last content owner freed an active call's code.");
    std::puts("Source material consumer: PASS (detached CPU ABI, native failures and active owner release)");
}

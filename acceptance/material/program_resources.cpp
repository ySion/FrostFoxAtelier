#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "material/program_resources.hpp"
#include "material/resources.hpp"
#include "runtime/native.hpp"
#include <cmath>
#include <cstdio>

int main() {
    atelier::install_fatal_handler();
    using namespace atelier;
    using namespace atelier::material;
    using namespace atelier::acceptance::helpers;
    ProgramSurfaceRef surviving;
    std::weak_ptr<const Image> released;
    {
        Runtime runtime;
        take(register_resource_types(runtime));
        take(register_program_types(runtime));
        const auto program = take(
            source::compile_material(runtime, take(runtime.native_type<Context>()), take(runtime.native_type<Output>()),
                                     take(runtime.native_type<TextureInput>()), "resources.source", R"(
            struct Textures { material.TextureInput color; };
            struct Params { float gain; };
            Params defaults() { Params p; p.gain=0.75; return p; }
            material.Output shade(material.Context c, Params p, Textures textures) {
                vector4 color = sample(textures.color, c.uv);
                vector3 tint = c.uv.x <= 1.0 ? vector3(color.x, color.y, color.z) : vector3(0.0, 0.0, 0.0);
                tint *= p.gain;
                float cov = 0.25;
                cov += (p.gain > 0.0 ? 0.25 : 0.0);
                material.Output o; o.base_color = tint;
                o.emissive = vector3(0.0, 0.0, color.w); o.coverage = cov;
                o.normal_world = c.normal_world; return o;
            })"));
        const std::array<TextureDefault, 1> defaults{
            {{"color", {"image/color", Address::clamp}, TextureColor::srgb, {1, 1, 1, 1}}}};
        const auto def = take(make_definition(program, {Coverage::mask, false, false}, defaults));
        const auto image = take(Image::create(1, 1, {128, 64, 32, 128}));
        released = image;
        auto texture = take(create_texture(runtime, "image/color", image));
        auto source = take(create_source(runtime, "material/source", {def, {texture}}));
        InstanceState state{source, {{"gain", .5f, true}}, {}};
        state.textures.resize(state.entries.size());
        auto instance = take(create_instance(runtime, "material/instance", std::move(state)));
        surviving = take(capture_program(runtime, instance.id()));

        require(take(program->hlsl_coverage("test_ns")).contains("float ms_coverage("),
                "A material with a valid coverage projection must expose its shader.");
        // Reuse the same material after its coverage projection exceeds the node budget.
        std::string diamond =
            "struct Textures { material.TextureInput color; };\n"
            "struct Params { float gain; };\n"
            "Params defaults() { Params p; p.gain = 1.0; return p; }\n"
            "float d0(material.TextureInput t, vector2 uv) { vector4 s = sample(t, uv); return s.x; }\n";
        for (int i = 1; i <= 16; ++i) {
            const auto prev = "d" + std::to_string(i - 1);
            diamond += "float d" + std::to_string(i) + "(material.TextureInput t, vector2 uv) { return " + prev +
                       "(t, uv) + " + prev + "(t, uv * 0.5); }\n";
        }
        diamond += "material.Output shade(material.Context c, Params p, Textures t) {\n"
                   "    material.Output o; o.coverage = d16(t.color, c.uv) * p.gain;\n"
                   "    o.normal_world = c.normal_world; return o;\n}";
        const auto over_budget = take(
            source::compile_material(runtime, take(runtime.native_type<Context>()), take(runtime.native_type<Output>()),
                                     take(runtime.native_type<TextureInput>()), "diamond.source", diamond));
        const auto rejected = over_budget->hlsl_coverage("test_ns");
        require(!rejected && rejected.error().code == ErrorCode::capacity_exceeded &&
                    rejected.error().message.contains("node budget"),
                "The coverage caller must receive the projection budget error.");
        const auto point = take(over_budget->hlsl("test_ns"));
        const auto footprint = take(over_budget->hlsl_footprint("test_ns"));
        require(point.contains("ms_Output ms_shade(") && !point.contains("#error") &&
                    footprint.contains("ms_Output ms_shade_footprint(") && !footprint.contains("#error"),
                "Coverage budget failure disabled an independent shading variant.");
        const std::array<ImageRef, 1> white{take(Image::create(1, 1, {255, 255, 255, 255}))};
        const auto surface = take(resolve_source(take(make_definition(over_budget, {}, defaults)), white));
        require(take(surface->evaluate({})).coverage == 65536.f,
                "Coverage projection failure changed CPU shading on the same material.");

        struct ColorOnly {
            float value;
        };
        const auto color_type = take(runtime.register_type(
            NativeRegistration<ColorOnly>(runtime, "test.ColorOnly", TypeKind::record)
                .field<&ColorOnly::value>("value", {TypeReference::native<float>()}, offsetof(ColorOnly, value))
                .build()));
        const auto color_only =
            take(source::compile_material(runtime, take(runtime.native_type<Context>()), color_type,
                                          take(runtime.native_type<TextureInput>()), "color.source", R"(
            struct Params { float value; };
            Params defaults() { Params p; return p; }
            test.ColorOnly shade(material.Context c, Params p) { test.ColorOnly o; o.value = p.value; return o; }
        )"));
        const auto missing = color_only->hlsl_coverage("test_ns");
        require(!missing && missing.error().code == ErrorCode::invalid_type && color_only->hlsl("test_ns") &&
                    color_only->hlsl_footprint("test_ns"),
                "An output without a float coverage field must reject only the coverage variant.");
    }
    const auto output = take(surviving->evaluate({}));
    // IEC sRGB decode and the authored gain/alpha, not a previous capture's output.
    const float expected = float(std::pow((128. / 255 + .055) / 1.055, 2.4)) * .5f;
    require(std::abs(output.base_color.x - expected) < 2e-6f && std::abs(output.emissive.z - 128.f / 255) < 2e-6f &&
                output.coverage == .5f && !released.expired(),
            "Captured material lost its code or image after Runtime destruction.");
    surviving.reset();
    require(released.expired(), "Last material capture retained its image.");
    std::puts("Material acceptance: PASS (shader variants, coverage errors, captured code and image lifetime)");
}

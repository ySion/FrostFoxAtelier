#include "source/material_program.hpp"
#include "material/program.hpp"
#include "runtime/native.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using namespace atelier;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void take(Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
bool near(float a, float b) { return std::abs(a - b) < 0.00001f; }
struct UnusedDefault {
    float value;
    UnusedDefault() { throw std::runtime_error("Unrelated native constructor must not run during material compilation."); }
};
const std::string declarations = R"(
    struct Mask { float cutoff; vector2 scale; };
    struct Params { float gain; vector3 tint; Mask mask; };
    Params defaults() {
        Params p;
        p.gain = 2.0; p.tint = vector3(0.25,0.5,0.75);
        p.mask.cutoff = 0.5; p.mask.scale = vector2(1.0,1.0);
        return p;
    }
)";
const std::string resources = R"(
    struct ColorInputs { material.TextureInput color; };
    struct Textures { material.TextureInput alpha; ColorInputs surface; material.TextureInput unused; };
)";
const std::string first_body = R"(
    material.Output shade(material.Context c, Params p, Textures textures) {
        material.Output o;
        material.TextureInput input = textures.surface.color;
        input = textures.alpha;
        vector4 alpha = sample(input,c.uv);
        vector4 color = sample(textures.surface.color,c.uv);
        o.base_color = p.tint * color.x;
        o.coverage = alpha.w - p.mask.cutoff;
        o.normal_world = c.normal_world;
        return o;
    }
)";
const std::string first = declarations + resources + first_body;
const std::string second = declarations + resources + R"(
    float margin(vector2 uv, Params p, material.TextureInput input) {
        material.TextureInput copied = input;
        vector4 a = sample(copied,vector2(uv.x * p.mask.scale.x, uv.y * p.mask.scale.y));
        if (uv.y > 0.5) { return p.mask.cutoff - a.w; }
        return a.w * p.gain - p.mask.cutoff;
    }
    material.Output shade(material.Context c, Params p, Textures textures) {
        Textures copied = textures;
        material.Output o;
        o.base_color = p.tint + vector3(0.1,0.2,0.3);
        o.emissive = vector3(sample(copied.surface.color,c.uv).x,0.0,0.0);
        o.coverage = margin(c.uv,p,copied.alpha);
        o.normal_world = c.normal_world;
        return o;
    }
)";
// One real Source module exercises every footprint operation through the public
// compiler. The point evaluator still uses the existing synchronous sampler;
// the GPU artifact below must be dispatched/read back to validate changes.
const std::string footprint_source = R"(
    struct Params { float mode; float gain; };
    struct Textures { material.TextureInput warp; material.TextureInput color; };
    struct Coordinates { vector2 uv; };
    Params defaults() { Params p; p.gain=1.0; return p; }
    Coordinates coordinates(material.Context c,Params p) {
        Coordinates q; q.uv=c.uv;
        if (p.mode==0.0) { q.uv=vector2(c.uv.x*c.uv.x,c.uv.x*c.uv.y)*p.gain; }
        if (p.mode==1.0) {
            if (c.uv.x<0.5) { q.uv=vector2(c.uv.x*2.0,c.uv.y*3.0); }
            else { q.uv=c.uv*64.0; }
        }
        if (p.mode==3.0) { q.uv=vector2(0.25,0.5); }
        if (p.mode==4.0) {
            if (c.uv.x>=0.5) { q.uv=vector2(c.normal_world.x,c.normal_world.y); }
        }
        if (p.mode==5.0) { q.uv.x=c.normal_world.x*0.0; }
        if (p.mode==6.0) { q.uv=vector2(math.sqrt(c.uv.x),math.length(c.uv)); }
        if (p.mode==7.0) { q.uv=vector2(math.lerp(2.0,7.0,c.uv.x),math.clamp(c.uv.y,0.75,0.75)); }
        if (p.mode==8.0) { q.uv=math.sin(c.uv); q.uv.y=math.cos(c.uv).y; }
        if (p.mode==9.0) { q.uv=math.fract(c.uv); }
        if (p.mode==12.0) {
            vector3 t=vector3(c.tangent_world.x,c.tangent_world.y,c.tangent_world.z);
            q.uv=vector2(math.dot(c.normal_world,t),math.cross(c.normal_world,t).z)/2.0;
        }
        if (p.mode==13.0) {
            q.uv.x=math.abs(c.position_world.x)+math.min(c.uv.x,c.position_world.y)+math.max(c.uv.y,c.position_world.z);
            q.uv.y=math.floor(c.uv.x)+math.ceil(c.uv.y)+math.trunc(c.position_world.x)+math.step(c.uv.x,c.uv.y);
        }
        return q;
    }
    material.Output shade(material.Context c,Params p,Textures textures) {
        Coordinates q=coordinates(c,p);
        if (p.mode==2.0 || p.mode==10.0 || p.mode==11.0) {
            vector4 warped=sample(textures.warp,q.uv);
            q.uv=vector2(warped.x,warped.y*warped.y);
            if (p.mode==10.0) { q.uv.x=warped.x*0.0; }
        }
        // A skipped RHS must not sample, or import its invalid normal changes.
        if (p.mode<0.0 && sample(textures.warp,vector2(c.normal_world.x,c.normal_world.y)).x>0.0) {
            q.uv=-q.uv;
        }
        vector4 color=sample(textures.color,q.uv);
        material.Output o;
        o.base_color=vector3(color.x,color.y,color.z);
        o.emissive=vector3(q.uv.x,q.uv.y,0.0);
        o.coverage=color.w;
        o.normal_world=c.normal_world;
        return o;
    }
)";
const std::string orientation_source = R"(
    struct Params { float enabled; float denominator; };
    struct Textures { material.TextureInput normal; material.TextureInput unused; };
    Params defaults() { Params p; p.enabled=1.0; p.denominator=1.0; return p; }
    vector2 orientation(material.TextureInput input,vector2 uv) {
        material.TextureInput copied=input;
        return sample_orientation(copied,uv);
    }
    material.Output shade(material.Context c,Params p,Textures textures) {
        material.Output o; o.coverage=99.0;
        if (p.enabled>0.0) {
            vector2 direction=orientation(textures.normal,c.uv/p.denominator);
            // Feed the piecewise-constant result into a sampled UV so the GPU
            // trace can observe its zero changes, including at fold points.
            vector4 color=sample(textures.normal,direction*0.25+vector2(0.5,0.5));
            o.base_color=vector3(direction.x,direction.y,color.x);
        }
        return o;
    }
)";
struct Texture { math::Vector4 value; bool uv_alpha{}; };
Result<math::Vector4> sample(const void* data, math::Vector2 uv) {
    auto value = static_cast<const Texture*>(data)->value;
    if (static_cast<const Texture*>(data)->uv_alpha) value.w = uv.x;
    return value;
}
std::uint32_t slot(const source::MaterialProgram& program, std::string_view key) {
    const auto found = std::ranges::find(program.textures(), key, &source::MaterialTextureInput::key);
    require(found != program.textures().end(), "Material lost a declared texture key.");
    return found->slot;
}
std::array<source::MaterialTexture,3> bindings(const source::MaterialProgram& program, const Texture& color, const Texture& alpha) {
    return {{{slot(program,"surface.color"),&color,sample}, {slot(program,"alpha"),&alpha,sample},
        {slot(program,"unused"),&color,sample}}};
}
material::Output evaluate(const source::MaterialProgram& program, material::Context context,
    std::span<const source::MaterialTexture> textures, std::span<const std::byte> parameters = {}) {
    material::Output output;
    take(program.evaluate(std::as_bytes(std::span(&context, 1)), parameters.empty() ? program.default_parameters() : parameters,
        std::as_writable_bytes(std::span(&output, 1)), textures));
    return output;
}
}

void verify_source_materials() {
    std::shared_ptr<const source::MaterialProgram> a, b, revised, reordered, numeric, footprint, oriented;
    {
        Runtime runtime;
        take(material::register_program_types(runtime));
        take(NativeRegistration<UnusedDefault>{runtime,"unused.Value",TypeKind::record}
            .field<&UnusedDefault::value>("value",{TypeReference::native<float>()},offsetof(UnusedDefault,value)).publish());
        const auto context = take(runtime.native_type<material::Context>());
        const auto output = take(runtime.native_type<material::Output>());
        const auto texture_input = take(runtime.native_type<material::TextureInput>());
        const auto types_before = runtime.type_count();
        const std::string unused_loop = "float unused_loop(float x){ while(x>0.0){x=x-1.0;} return x; }";
        a = take(source::compile_material(runtime, context, output, texture_input, "mask-a.ms", first + unused_loop));
        b = take(source::compile_material(runtime, context, output, texture_input, "mask-b.ms", second));
        reordered = take(source::compile_material(runtime, context, output, texture_input, "reordered.ms", declarations + R"(
            struct ColorInputs { material.TextureInput color; };
            struct Textures { ColorInputs surface; material.TextureInput unused; material.TextureInput alpha; };
        )" + first_body));
        revised = take(source::compile_material(runtime, context, output, texture_input, "revised.ms", R"(
            struct Params { vector2 shifted; float threshold; };
            Params defaults() { Params p; p.threshold=0.75; return p; }
            material.Output shade(material.Context c,Params p) { material.Output o; o.coverage=c.uv.x-p.threshold; return o; }
        )"));
        numeric = take(source::compile_material(runtime, context, output, texture_input, "numeric.ms", R"(
            struct Params { float a; float b; float t; float x; float low; float high; };
            Params defaults() {
                Params p; p.a=100000000.0; p.b=1.0; p.t=1.0; p.x=-1e-30; p.low=1.0; p.high=0.0;
                return p;
            }
            material.Output shade(material.Context c,Params p) {
                material.Output o;
                o.coverage=math.clamp(c.uv.x,p.low,p.high)-0.5;
                o.base_color=math.lerp(vector3(p.a,p.b,p.a),vector3(p.b,p.a,p.b),p.t);
                o.emissive=math.fract(vector3(p.x,-2.25,0.75));
                o.roughness=math.fract(p.x);
                o.metallic=math.lerp(p.a,p.b,p.t);
                o.normal_world=math.clamp(vector3(-2.0,0.5,2.0),vector3(p.low,0.0,p.low),vector3(p.high,1.0,p.high));
                return o;
            }
        )"));
        footprint = take(source::compile_material(runtime, context, output, texture_input, "footprint.ms", footprint_source));
        oriented = take(source::compile_material(runtime, context, output, texture_input, "orientation.ms", orientation_source));
        require(runtime.type_count() == types_before, "Material compilation published/redefined Params into live Runtime.");
        const auto rejected = [&](std::string text, std::string_view reason) {
            auto result = source::compile_material(runtime, context, output, texture_input, "rejected.ms", text);
            require(!result, "Material accepted an operation outside its pure subset.");
            require(result.error().message.contains(reason), result.error().message.c_str());
            require(runtime.type_count() == types_before, "Rejected material polluted the Runtime type directory.");
        };
        rejected(declarations + unused_loop + "material.Output shade(material.Context c,Params p){ material.Output o; o.coverage=unused_loop(c.uv.x); return o; }", "loops");
        rejected(unused_loop + R"(struct Params { float cutoff; };
            Params defaults(){ Params p; p.cutoff=unused_loop(1.0); return p; }
            material.Output shade(material.Context c,Params p){ material.Output o; return o; })", "loops");
        rejected(first + "float unused_bad(){ return missing_name(); }", "Unknown source function");
        rejected(first + "float unused_recursive(float x){ return unused_recursive(x); }", "Recurs");
        rejected(declarations + "material.Output shade(material.Context c,Params p){ while(c.uv.x>0.0) {} material.Output o; return o; }", "loops");
        rejected(declarations + "material.Output shade(material.Context c,Params p){ uint x=1; material.Output o; return o; }", "float");
        rejected(declarations + "material.Output shade(material.Context c,Params p){ material.Output o; o.coverage=host_call(); return o; }", "Unknown source function");
        rejected(declarations + "material.Output shade(material.Context c,Params p){ return shade(c,p); }", "Recurs");
        rejected(R"(struct Params { float cutoff; };
            Params defaults(){ material.TextureInput input; Params p; return p; }
            material.Output shade(material.Context c,Params p){ material.Output o; return o; })", "defaults cannot contain texture");
        rejected(R"(struct Params { bool enabled; };
            Params defaults(){ Params p; return p; }
            material.Output shade(material.Context c,Params p){ material.Output o; return o; })", "parameters require float");
        rejected(R"(struct Params { float cutoff; };
            float impure(){ material.TextureInput input; return sample(input,vector2()).w; }
            Params defaults(){ Params p; p.cutoff=impure(); return p; }
            material.Output shade(material.Context c,Params p){ material.Output o; return o; })", "defaults cannot contain texture");
        rejected(declarations + "async material.Output shade(material.Context c,Params p){ yield; material.Output o; return o; }", "async");
        const auto rejected_texture = [&](std::string_view body, std::string_view reason) {
            rejected(declarations + resources + "material.Output shade(material.Context c,Params p,Textures textures){" +
                std::string(body) + " material.Output o; return o; }", reason);
        };
        rejected_texture("vector4 x=sample(7,c.uv);", "not a numeric slot");
        rejected_texture("vector2 x=sample_orientation(7,c.uv);", "not a numeric slot");
        rejected_texture("vector2 x=sample_orientation(textures.alpha,1.0);", "coordinates require vector2");
        rejected_texture("material.TextureInput forged;", "cannot be default constructed");
        rejected_texture("Textures forged;", "cannot be default constructed");
        rejected_texture("textures.alpha.slot=0;", "members are compiler-owned");
        rejected_texture("float x=float(textures.alpha.slot);", "members are compiler-owned");
        rejected_texture("uint x=uint(textures.alpha);", "Numeric conversion");
        rejected(declarations + R"(
            struct FakeInput { uint slot; };
            material.Output shade(material.Context c,Params p){
                FakeInput fake; vector4 x=sample(fake,c.uv); material.Output o; return o;
            })", "registered TextureInput");
        rejected(R"(struct Params { material.TextureInput texture; };
            Params defaults(){ Params p; return p; }
            material.Output shade(material.Context c,Params p){ material.Output o; return o; })", "not texture inputs");
        rejected(declarations + R"(struct Textures { float input; };
            material.Output shade(material.Context c,Params p,Textures textures){ material.Output o; return o; })", "require TextureInput leaves");
        rejected(declarations + R"(struct Textures { material.TextureInput tint; };
            material.Output shade(material.Context c,Params p,Textures textures){ material.Output o; return o; })", "key conflict: tint");
        const auto token_member_error = source::compile_material(runtime, context, output, texture_input, "token-member.ms",
            "struct Params { float cutoff; };\nstruct Textures { material.TextureInput input; };\n"
            "Params defaults(){ Params p; return p; }\nmaterial.Output shade(material.Context c,Params p,Textures textures){\n"
            "material.Output o; o.coverage=float(textures.input.slot); return o; }");
        require(!token_member_error && token_member_error.error().message.contains("token-member.ms:5:") &&
            token_member_error.error().message.contains("members are compiler-owned"),
            "Opaque token member access lost its actual source location/reason.");
        rejected(R"(struct Params { float cutoff; };
            float impure(){ material.TextureInput input; return sample_orientation(input,vector2()).x; }
            Params defaults(){ Params p; p.cutoff=impure(); return p; }
            material.Output shade(material.Context c,Params p){ material.Output o; return o; })", "defaults cannot contain texture");

        const auto declaration_error = source::compile_material(runtime, context, output, texture_input, "declaration.ms",
            "\nstruct Params {\nfloat cutoff\n};");
        require(!declaration_error && declaration_error.error().message.contains("declaration.ms:4:1: Expected ';'"),
            "Material declaration diagnostic lost its actual source/line/column/cause.");
        const auto long_name = std::string(4096, 'x') + "/diagnostic-tail.ms";
        const auto typed_error = source::compile_material(runtime, context, output, texture_input, long_name,
            "struct Params { float cutoff; };\nParams defaults(){ Params p; return p; }\n"
            "material.Output shade(material.Context c,Params p){\nmaterial.Output o;\n"
            "o.coverage = missing_name();\nreturn o;\n}");
        require(!typed_error && typed_error.error().message.contains("diagnostic-tail.ms:5:14: Unknown source function 'missing_name'"),
            "Long material source name displaced the typed diagnostic's location/cause.");
        const auto publication_error = source::compile_material(runtime, context, output, texture_input, "recursive-layout.ms",
            "struct Params { Params child; };");
        require(!publication_error && publication_error.error().message.starts_with("recursive-layout.ms: ") &&
            !publication_error.error().message.contains(":1:1:"),
            "Material layout publication lost source identity or fabricated an exact position.");
    } // All content below is used after its compilation Runtime is destroyed.

    material::Context numeric_context;
    numeric_context.uv = {0.5f,0.25f};
    const auto numeric_result = evaluate(*numeric,numeric_context,{});
    require(numeric_result.coverage == 0.5f && numeric_result.normal_world == math::Vector3{1,0.5f,1},
        "Material clamp changed Source's inverted-bound contract.");
    require(numeric_result.metallic == 1.0f && numeric_result.base_color == math::Vector3{1,1e8f,1},
        "Material lerp lost the exact t=1 endpoint.");
    const auto below_one = std::nextafter(1.0f,0.0f);
    require(numeric_result.roughness == below_one && numeric_result.emissive == math::Vector3{below_one,0.75f,0.75f},
        "Material fract lost Source's strictly-below-one cap.");
    const std::array opposite{1.0f,1e8f,0.0f,-1e-30f,0.0f,1.0f};
    const auto opposite_result = evaluate(*numeric,numeric_context,{},std::as_bytes(std::span(opposite)));
    require(opposite_result.metallic == 1.0f && opposite_result.base_color == math::Vector3{1,1e8f,1} &&
        opposite_result.coverage == 0.0f && opposite_result.normal_world == math::Vector3{0,0.5f,1},
        "Runtime material parameters lost the opposite lerp endpoint or ordered clamp bounds.");

    struct PointProbe { mutable std::uint32_t calls{}; bool warp{}; } warp_probe{0,true}, color_probe;
    const auto point_probe = +[](const void* data,math::Vector2 uv) -> Result<math::Vector4> {
        const auto& probe = *static_cast<const PointProbe*>(data);
        ++probe.calls;
        return math::Vector4{uv.x,uv.y*(probe.warp?2.0f:1.0f),0,1};
    };
    const std::array<source::MaterialTexture,2> probe_bindings{{
        {slot(*footprint,"warp"),&warp_probe,point_probe}, {slot(*footprint,"color"),&color_probe,point_probe}}};
    material::Context probe_context;
    probe_context.uv={0.25f,0.75f};
    for (const float mode : {0.0f,1.0f,2.0f,8.0f}) {
        warp_probe.calls=color_probe.calls=0;
        const std::array parameters{mode,1.0f};
        const auto value=evaluate(*footprint,probe_context,probe_bindings,std::as_bytes(std::span(parameters)));
        const auto expected = mode==0 ? math::Vector2{0.0625f,0.1875f} : mode==1 ? math::Vector2{0.5f,2.25f} :
            mode==2 ? math::Vector2{0.25f,2.25f} : math::Vector2{std::sin(0.25f),std::cos(0.75f)};
        require(near(value.emissive.x,expected.x) && near(value.emissive.y,expected.y) &&
            near(value.base_color.x,expected.x) && near(value.base_color.y,expected.y),
            "Footprint-capable Source changed point arithmetic, struct/helper flow or sin/cos CPU evaluation.");
        require(color_probe.calls==1 && warp_probe.calls==(mode==2?1u:0u),
            "Point evaluation gained neighbor samples or lost its short-circuit.");
    }
    require(!footprint->hlsl_footprint("bad::namespace") && !footprint->hlsl("9invalid"),
        "A material HLSL variant lost namespace validation.");

    // Public compiler/JIT/callback integration. Actual SurfaceInput binding
    // integration is separately owned by the material resource acceptance.
    struct OrientationProbe {
        unsigned address{}; // repeat, clamp, mirror
        mutable std::uint32_t queries{}, samples{};
        mutable math::Vector2 sampled_uv{};
    } orientation_probe;
    const auto query_orientation = +[](const void* data,math::Vector2 uv) -> Result<math::Vector2> {
        const auto& probe=*static_cast<const OrientationProbe*>(data);
        ++probe.queries;
        math::Vector2 direction{1,1};
        if (probe.address==2) for (std::size_t axis=0;axis<2;++axis) {
            double u=uv[axis]; u-=std::floor(u*.5)*2;
            direction[axis]=u>1?-1.0f:1.0f;
        }
        return direction;
    };
    const auto orientation_sample = +[](const void* data,math::Vector2 uv) -> Result<math::Vector4> {
        const auto& probe=*static_cast<const OrientationProbe*>(data);
        ++probe.samples; probe.sampled_uv=uv;
        return math::Vector4{0.4f,0.6f,0.8f,1};
    };
    std::array<source::MaterialTexture,2> orientation_bindings{{
        {slot(*oriented,"normal"),&orientation_probe,orientation_sample,query_orientation},
        {slot(*oriented,"unused"),&orientation_probe,orientation_sample}}};
    constexpr std::array<math::Vector2,6> orientation_uvs{{{-0.25f,1.25f},{0.25f,0.75f},{-1,-2},{1,2},{2.25f,3.25f},{-2.25f,-3.25f}}};
    constexpr std::array<math::Vector2,6> mirror_directions{{{-1,-1},{1,1},{1,1},{1,1},{1,-1},{-1,1}}};
    material::Context orientation_context;
    for (unsigned address=0;address<3;++address) for (std::size_t i=0;i<orientation_uvs.size();++i) {
        orientation_probe.address=address; orientation_probe.queries=orientation_probe.samples=0;
        orientation_context.uv=orientation_uvs[i];
        const auto value=evaluate(*oriented,orientation_context,orientation_bindings);
        const auto direction=address==2?mirror_directions[i]:math::Vector2{1,1};
        require(value.base_color==math::Vector3{direction.x,direction.y,0.4f} && value.coverage==99 &&
            orientation_probe.queries==1 && orientation_probe.samples==1 &&
            orientation_probe.sampled_uv==direction*0.25f+math::Vector2{0.5f,0.5f},
            "sample_orientation lost its typed token, binding, fold convention or value result.");
    }
    orientation_bindings[0].orientation=nullptr;
    const std::array skipped_orientation{0.0f,1.0f};
    orientation_probe.queries=orientation_probe.samples=0;
    require(evaluate(*oriented,orientation_context,orientation_bindings,std::as_bytes(std::span(skipped_orientation))).coverage==99 &&
        orientation_probe.queries==0 && orientation_probe.samples==0,
        "An unexecuted orientation query incorrectly required a callback.");
    material::Output orientation_untouched; orientation_untouched.coverage=321;
    const auto orientation_failure = [&](std::string_view reason,std::span<const std::byte> parameters) {
        std::array<std::byte,sizeof(material::Output)> before;
        std::memcpy(before.data(),&orientation_untouched,before.size());
        auto failed=oriented->evaluate(std::as_bytes(std::span(&orientation_context,1)),parameters,
            std::as_writable_bytes(std::span(&orientation_untouched,1)),orientation_bindings);
        require(!failed && failed.error().message.contains(reason) &&
            std::memcmp(before.data(),&orientation_untouched,before.size())==0,
            "Orientation failure lost its diagnostic or partially published Output.");
    };
    orientation_failure("Missing material sample_orientation callback",oriented->default_parameters());
    orientation_bindings[0].orientation=+[](const void*,math::Vector2) -> Result<math::Vector2> {
        return std::unexpected(Error{ErrorCode::invocation_failed,"orientation unavailable"});
    };
    orientation_failure("orientation unavailable",oriented->default_parameters());
    orientation_bindings[0].orientation=+[](const void*,math::Vector2) -> Result<math::Vector2> {
        return math::Vector2{1,std::numeric_limits<float>::quiet_NaN()};
    };
    orientation_failure("sample_orientation result is not finite",oriented->default_parameters());
    orientation_bindings[0].orientation=+[](const void*,math::Vector2) -> Result<math::Vector2> {
        throw std::runtime_error("orientation exception");
    };
    orientation_failure("orientation exception",oriented->default_parameters());
    orientation_bindings[0].orientation=query_orientation;
    orientation_probe.queries=orientation_probe.samples=0;
    const std::array invalid_orientation_uv{1.0f,0.0f};
    orientation_failure("sample_orientation UV is not finite",std::as_bytes(std::span(invalid_orientation_uv)));
    require(orientation_probe.queries==0 && orientation_probe.samples==0,
        "Nonfinite orientation coordinates reached the host sampler.");

    Texture alpha{{0.1f,0.2f,0.3f,0.9f}, true}, color{{0.2f,0.4f,0.6f,0.8f}};
    const auto textures = bindings(*a,color,alpha);
    require(a->textures().size() == 3 && a->textures()[0].key == "alpha" && a->textures()[1].key == "surface.color" &&
        a->textures()[2].key == "unused" && revised->textures().empty(),
        "Material lost its declared resource schema or confused numeric-only and textured entries.");
    require(a->parameter_layout().size == 28 && a->parameters().size() == 4 &&
        a->parameters()[1].key == "tint" && a->parameters()[1].offset == 4 && a->parameters()[1].components == 3 &&
        a->parameters()[2].key == "mask.cutoff" && a->parameters()[2].offset == 16 &&
        a->parameters()[3].key == "mask.scale" && a->parameters()[3].offset == 20,
        "Material parameter paths, types or packed CPU offsets differ from Source declarations.");
    material::Context context;
    context.uv = {0.25f,0.75f}; context.normal_world = {0.0f,1.0f,0.0f};
    const auto out_a = evaluate(*a, context, textures), out_b = evaluate(*b, context, textures);
    require(near(out_a.coverage,-0.25f) && near(out_a.base_color.x,0.05f) && near(out_a.base_color.z,0.15f) &&
        near(out_a.roughness,0.65f) && out_a.normal_world == context.normal_world,
        "Detached material A lost sampling, parameters or registered native defaults.");
    require(near(out_b.coverage,0.25f) && near(out_b.base_color.x,0.35f) && near(out_b.emissive.x,0.2f),
        "Same-layout material B did not run its distinct branch/helper logic.");
    const auto reordered_bindings = bindings(*reordered,color,alpha);
    const auto reordered_result = evaluate(*reordered,context,reordered_bindings);
    require(slot(*a,"alpha") != slot(*reordered,"alpha") && reordered_result.coverage == out_a.coverage &&
        reordered_result.base_color == out_a.base_color,
        "A source field reorder changed stable keys or reused an old program's slots.");
    auto shared_image = textures;
    shared_image[1].data = shared_image[0].data;
    shared_image[1].sample = [](const void* data,math::Vector2 uv) -> Result<math::Vector4> {
        auto value = static_cast<const Texture*>(data)->value;
        value.w -= uv.x;
        return value;
    };
    const auto shared_result = evaluate(*a,context,shared_image);
    require(near(shared_result.coverage,0.05f) && shared_result.base_color == out_a.base_color,
        "Two inputs using the same image lost their independent sampling environments.");
    context.uv = {0.75f,0.25f};
    require(near(evaluate(*b,context,textures).coverage,1.0f), "Material B branch used the wrong operands.");
    context.uv.x = 0.5f;
    require(evaluate(*a,context,textures).coverage == 0.0f, "Masked equality boundary was not preserved.");
    require(near(evaluate(*revised,context,{}).coverage,-0.25f), "Same-named revised Params lost its independent layout/code.");
    std::vector<std::byte> changed(a->default_parameters().begin(),a->default_parameters().end());
    const float cutoff = 0.625f;
    std::memcpy(changed.data()+16,&cutoff,sizeof(cutoff));
    require(near(evaluate(*a,context,textures,changed).coverage,-0.125f) && evaluate(*a,context,textures).coverage == 0.0f,
        "Parameter override changed compiled/default content or required recompilation.");
    material::Output untouched; untouched.coverage = 123;
    auto missing = a->evaluate(std::as_bytes(std::span(&context,1)),a->default_parameters(),
        std::as_writable_bytes(std::span(&untouched,1)),std::span(textures).first(1));
    require(!missing && missing.error().message.contains("alpha") && untouched.coverage == 123,
        "Missing texture binding did not fail atomically.");
    auto unsampled = a->evaluate(std::as_bytes(std::span(&context,1)),a->default_parameters(),
        std::as_writable_bytes(std::span(&untouched,1)),std::span(textures).first(2));
    require(!unsampled && unsampled.error().message.contains("unused") && untouched.coverage == 123,
        "Declared but unsampled texture input silently escaped the binding contract.");
    const std::array duplicate{textures[0],textures[1],textures[2],textures[1]};
    auto duplicated = a->evaluate(std::as_bytes(std::span(&context,1)),a->default_parameters(),
        std::as_writable_bytes(std::span(&untouched,1)),duplicate);
    require(!duplicated && duplicated.error().message.contains("Duplicate material texture input: alpha") && untouched.coverage == 123,
        "Duplicate named texture binding did not fail atomically.");
    auto bad = textures;
    bad[1].sample = [](const void*, math::Vector2) -> Result<math::Vector4> {
        return std::unexpected(Error{ErrorCode::invocation_failed,"texture unavailable"});
    };
    auto failed = a->evaluate(std::as_bytes(std::span(&context,1)),a->default_parameters(),
        std::as_writable_bytes(std::span(&untouched,1)),bad);
    require(!failed && failed.error().message == "texture unavailable" && untouched.coverage == 123,
        "Sampler failure changed the caller output or lost its diagnostic.");
    auto changed_bindings = textures;
    Texture brighter{{0.8f,0.1f,0.4f,0.7f}};
    changed_bindings[0].data = &brighter;
    require(near(evaluate(*a,context,changed_bindings).base_color.z,0.6f) &&
        near(evaluate(*a,context,textures).base_color.z,0.15f), "A previous invocation leaked its texture environment.");
    auto nan_bindings = textures;
    nan_bindings[0].sample = [](const void*,math::Vector2) -> Result<math::Vector4> {
        return math::Vector4{std::numeric_limits<float>::quiet_NaN(),0,0,1};
    };
    require(!a->evaluate(std::as_bytes(std::span(&context,1)),a->default_parameters(),
        std::as_writable_bytes(std::span(&untouched,1)),nan_bindings) && untouched.coverage == 123,
        "Nonfinite samples were silently published.");

    // Actual generated modules are composed, not hand-written stand-ins. The
    // artifact can also be compiled by pinned DXC, without GPU execution.
    const auto path = std::filesystem::temp_directory_path()/"atelier-source-materials.hlsl";
    std::ofstream hlsl(path,std::ios::binary);
    hlsl << take(a->hlsl("MaterialA")) << take(b->hlsl("MaterialB")) << take(numeric->hlsl("MaterialMath")) << R"(
        ByteAddressBuffer Parameters : register(t0);
        Texture2D<float4> Textures[16] : register(t1);
        SamplerState Filter : register(s0);
        RWStructuredBuffer<float4> Result : register(u0);
        float4 atelier_material_sample(uint environment,uint slot,float2 uv) {
            return Textures[environment+slot].SampleLevel(Filter,uv,0.0);
        }
        [numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {
            MaterialA::ms_Context a=(MaterialA::ms_Context)0;
            MaterialB::ms_Context b=(MaterialB::ms_Context)0;
            a.ms_uv=float2(0.25,0.75); b.ms_uv=float2(0.75,0.25);
            MaterialA::ms_Output x=MaterialA::ms_shade(id.x,a,MaterialA::ms_load_parameters(Parameters,0));
            MaterialB::ms_Output y=MaterialB::ms_shade(id.x+8,b,MaterialB::ms_load_parameters(Parameters,28));
            Result[id.x]=float4(x.ms_coverage,y.ms_coverage,x.ms_base_color.x,y.ms_base_color.z);
            MaterialMath::ms_Context c=(MaterialMath::ms_Context)0;
            c.ms_uv=float2(0.5,0.25);
            MaterialMath::ms_Output z=MaterialMath::ms_shade(id.x,c,MaterialMath::ms_load_parameters(Parameters,56));
            Result[id.x+16]=float4(z.ms_coverage,z.ms_metallic,z.ms_roughness,z.ms_normal_world.y);
            Result[id.x+32]=float4(z.ms_base_color,z.ms_emissive.x);
        }
    )";
    hlsl.close();
    require(hlsl.good(), "Could not write generated HLSL validation artifact.");

    // This is an executable GPU validation input, NOT a GPU pass. Dispatch 16
    // threads with two real mipmapped textures and a Mirror sampler, clear
    // Counts[32], bind Result[34], Trace[192], then read back gradients/output.
    // Keep optimized DXIL as evidence that IEEE-bit guards survive fast-math.
    const auto footprint_path=std::filesystem::temp_directory_path()/"atelier-source-material-footprints.hlsl";
    std::ofstream footprint_hlsl(footprint_path,std::ios::binary);
    footprint_hlsl << "// Source below is generated by MaterialProgram::hlsl_footprint.\n"
        << "// Program-local warp slot=" << slot(*footprint,"warp") << ", color slot=" << slot(*footprint,"color") << "\n"
        << take(footprint->hlsl_footprint("Footprint")) << take(numeric->hlsl_footprint("FootprintMath"))
        << take(oriented->hlsl("OrientationPoint")) << take(oriented->hlsl_footprint("OrientationFootprint")) << R"(
        Texture2D<float4> Images[2] : register(t0);
        SamplerState Filter : register(s0);
        RWStructuredBuffer<float4> Result : register(u0);
        RWStructuredBuffer<float4> Trace : register(u1);
        RWStructuredBuffer<uint> Counts : register(u2);
        bool finite2(float2 v) { return all((asuint(v)&0x7f800000u)!=0x7f800000u); }
        float4 atelier_material_sample(uint environment,uint slot,float2 uv) {
            return Images[slot].SampleLevel(Filter,uv,0);
        }
        float2 atelier_material_sample_orientation(uint environment,uint slot,float2 uv) {
            // This fixture binds Mirror for every image. Production queries
            // its actual input table, not a generated author parameter/slot flag.
            if (!finite2(uv)) return (float2)asfloat(0x7fc00000u);
            precise float2 folded=uv-floor(uv*0.5)*2.0;
            return float2(folded.x>1.0?-1.0:1.0,folded.y>1.0?-1.0:1.0);
        }
        float4 atelier_material_sample_grad(uint environment,uint slot,float2 uv,float2 d0,float2 d1) {
            uint index=environment*2+slot;
            uint call=Counts[index]; Counts[index]=call+1;
            if (call<3) {
                Trace[index*6+call*2]=float4(uv,d0);
                Trace[index*6+call*2+1]=float4(d1,0,0);
            }
            uint width,height,levels; Images[slot].GetDimensions(0,width,height,levels);
            precise float2 x=d0*float2(width,height), y=d1*float2(width,height);
            if (!finite2(d0) || !finite2(d1) || !finite2(x) || !finite2(y))
                return Images[slot].SampleLevel(Filter,uv,float(levels-1));
            return Images[slot].SampleGrad(Filter,uv,d0,d1);
        }
        [numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {
            if (id.x>=16) return;
            if (id.x>=14) {
                float2 uv=id.x==14?float2(-0.25,1.25):float2(-1,2);
                OrientationPoint::ms_Context p=(OrientationPoint::ms_Context)0; p.ms_uv=uv;
                OrientationPoint::ms_Output a=OrientationPoint::ms_shade(id.x,p,OrientationPoint::ms_defaults());
                OrientationFootprint::ms_Context c=(OrientationFootprint::ms_Context)0; c.ms_uv=uv;
                OrientationFootprint::ms_Context d0=(OrientationFootprint::ms_Context)0, d1=(OrientationFootprint::ms_Context)0;
                d0.ms_uv=float2(4,asfloat(0x7fc00000u)); d1.ms_uv=float2(-5,asfloat(0x7f800000u));
                OrientationFootprint::ms_Output b=OrientationFootprint::ms_shade_footprint(id.x,c,d0,d1,OrientationFootprint::ms_defaults());
                // Point/footprint xy: (-1,-1) for 14; (+1,+1) at folds for 15.
                // The following texture UV has ZERO gradients despite these
                // unusable/cross-fold input changes. Inspect Trace, not strings.
                Result[30+(id.x-14)*2]=float4(a.ms_base_color,a.ms_coverage);
                Result[31+(id.x-14)*2]=float4(b.ms_base_color,b.ms_coverage);
                return;
            }
            Footprint::ms_Context c=(Footprint::ms_Context)0;
            Footprint::ms_Context d0=(Footprint::ms_Context)0, d1=(Footprint::ms_Context)0;
            c.ms_uv=float2(0.25,0.75); c.ms_normal_world=float3(0,0,1);
            c.ms_geometric_normal_world=float3(0,0,1); c.ms_tangent_world=float4(1,0,0,1);
            d0.ms_uv=float2(0.125,0.03125); d1.ms_uv=float2(-0.0625,0.25);
            // 0: nonlinear UV, expected d0=(.0625,.1015625), d1=(-.03125,.015625).
            // 1: offset crosses branch, but local d0=(2,.09375), d1=(-.125,.75).
            if (id.x==1) d0.ms_uv.x=1.0;
            // 2: texture-driven UV; compare second-slot gradients to SAME-gradient
            // warp offset samples, then the local square rule 2*warp.y*dwarp.y.
            if (id.x==2) c.ms_uv.y=0.5;
            // 3/4: bad unused normal (and bad unused UV for constant case) must
            // not poison constant/selected UV. Constant gradients are both zero.
            if (id.x>=3 && id.x<=5) {
                d0.ms_normal_world=(float3)asfloat(0x7fc00000u);
                d1.ms_normal_world=(float3)asfloat(0x7f800000u);
            }
            if (id.x==3) d0.ms_uv.y=asfloat(0x7fc00000u);
            // 5: 0*unavailable must remain unavailable, not an optimized zero.
            // 6: zero sqrt/length: d0 valid zero, d1 unavailable (no epsilon).
            if (id.x==6) { c.ms_uv=0; d0.ms_uv=0; d1.ms_uv=float2(0.25,0); }
            // 7: t=1 still has dt contribution: x changes .625/-.3125; clamp
            // equal-bound selects constant lo, y changes zero.
            if (id.x==7) c.ms_uv.x=1;
            // 8: sin/cos chain; not a finite difference of neighboring sin/cos.
            // 9: fract's actually capped x has zero changes; integer y keeps d.
            if (id.x==9) c.ms_uv=float2(-1e-30,2);
            // 10: one bad input axis forces one warp sample and BOTH warp result
            // changes unavailable; multiplication by zero cannot hide either.
            if (id.x==10) d0.ms_uv.x=asfloat(0x7fc00000u);
            // 11: finite gradients, offset0 overflows; only offset1 is sampled.
            // Texel-scale overflow falls back inside the bridge, not generator.
            if (id.x==11) { c.ms_uv.x=3e38; d0.ms_uv.x=3e38; d1.ms_uv.x=0; }
            // 12: dot/cross/reduction and division. 13: signed zero abs, min/max
            // ties select their first operand; discrete operations have zero d.
            d0.ms_tangent_world=float4(0.25,0.5,0.125,0);
            d1.ms_tangent_world=float4(-0.5,0.125,0.75,0);
            if (id.x==13) {
                c.ms_position_world=float3(asfloat(0x80000000u),0.25,0.75);
                d0.ms_position_world=float3(0.25,2,3); d1.ms_position_world=float3(-0.5,4,5);
            }
            Footprint::ms_Parameters p=Footprint::ms_defaults(); p.ms_mode=float(id.x);
            Footprint::ms_Output o=Footprint::ms_shade_footprint(id.x,c,d0,d1,p);
            Result[id.x*2]=float4(o.ms_base_color,o.ms_coverage);
            Result[id.x*2+1]=float4(o.ms_emissive,o.ms_roughness);
            if (id.x==0) {
                FootprintMath::ms_Context m=(FootprintMath::ms_Context)0;
                m.ms_uv=float2(0.5,0.25);
                FootprintMath::ms_Context z=(FootprintMath::ms_Context)0;
                FootprintMath::ms_Output n=FootprintMath::ms_shade_footprint(0,m,z,z,FootprintMath::ms_defaults());
                // Existing weighted lerp endpoint, inverted clamp and capped
                // fract must survive the new lowering too, not merely point.
                Result[28]=float4(n.ms_coverage,n.ms_metallic,n.ms_roughness,n.ms_normal_world.y);
                Result[29]=float4(n.ms_base_color,n.ms_emissive.x);
            }
        }
    )";
    footprint_hlsl.close();
    require(footprint_hlsl.good(),"Could not write generated footprint HLSL validation artifact.");
    auto release_bindings = textures;
    release_bindings[0].data = &b;
    release_bindings[0].sample = [](const void* owner,math::Vector2) -> Result<math::Vector4> {
        auto* program = static_cast<const std::shared_ptr<const source::MaterialProgram>*>(owner);
        const_cast<std::shared_ptr<const source::MaterialProgram>*>(program)->reset();
        return math::Vector4{0.2f,0.4f,0.6f,0.8f};
    };
    const auto* active = b.get();
    take(active->evaluate(std::as_bytes(std::span(&context,1)),b->default_parameters(),
        std::as_writable_bytes(std::span(&untouched,1)),release_bindings));
    require(!b && near(untouched.coverage,0.5f), "Dropping the last content owner freed an active call's code.");
    std::printf("Material HLSL artifact: %s\n",path.string().c_str());
    std::printf("Material footprint HLSL artifact (GPU execution still required): %s\n",footprint_path.string().c_str());
    std::puts("Source material consumer: PASS (CPU; GPU execution not covered)");
}

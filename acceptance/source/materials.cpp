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
    std::shared_ptr<const source::MaterialProgram> a, b, revised, reordered, numeric;
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
    std::puts("Source material consumer: PASS (CPU; GPU execution not covered)");
}

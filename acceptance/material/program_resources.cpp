#include "material/program_resources.hpp"
#include "material/resources.hpp"

#include <cstdio>

namespace {
using namespace atelier;
using namespace atelier::material;
using Program = std::shared_ptr<const source::MaterialProgram>;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(*value);
}
void near(float actual, float expected) { require(std::isfinite(actual) && std::abs(actual-expected) < 2e-6f, "Material output differs from independent expected value."); }
void collect(Runtime& runtime) {
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle) (void)take(runtime.collect_step({32}));
}
Program compile(std::string_view gain = "0.5", bool renamed = false, bool reversed = false) {
    Runtime compiler;
    take(register_program_types(compiler));
    const std::string field = renamed ? "strength" : "gain";
    const std::string text = std::string(reversed ?
        "struct Textures { material.TextureInput data; material.TextureInput color; };\n" :
        "struct Textures { material.TextureInput color; material.TextureInput data; };\n") +
        "struct Params { float " + field + "; vector3 tint; };\n"
        "Params defaults() { Params p; p." + field + "=" + std::string(gain) + "; p.tint=vector3(0.2,0.3,0.4); return p; }\n"
        "material.Output shade(material.Context c, Params p, Textures textures) {\n"
        "vector4 color=sample(textures.color,c.uv); vector4 data=sample(textures.data,c.uv);\n"
        "material.Output o; o.base_color=vector3(color.x,color.y,color.z)*p." + field + ";\n"
        "o.emissive=vector3(p.tint.x,p.tint.y,color.w); o.coverage=data.w-0.5; o.roughness=data.x; o.metallic=data.y; o.normal_world=c.normal_world; return o; }";
    return take(source::compile_material(compiler, take(compiler.native_type<Context>()),
        take(compiler.native_type<Output>()), take(compiler.native_type<TextureInput>()), "resources.source", text));
} // Each compile Runtime dies before its program is consumed.
DefinitionRef definition(Program program) {
    // Author order intentionally differs from the first program's declarations.
    std::array<TextureDefault,2> defaults{{
        {"data", {"image/data", Address::repeat}, TextureColor::linear, {.7f,.8f,.9f,.75f}},
        {"color", {"image/color", Address::clamp}, TextureColor::srgb, {.1f,.2f,.3f,.25f}}}};
    auto result = take(make_definition(std::move(program), {Coverage::mask, false, false}, defaults));
    defaults[1].empty_value = {9,9,9,9}; // Factory must not retain writable author aliases.
    return result;
}
ImageRef pixel(std::array<std::uint8_t,4> rgba) {
    return std::make_shared<const Image>(1,1,std::vector<std::uint8_t>(rgba.begin(),rgba.end()));
}
SourceState source_state(DefinitionRef def, const ObjectRoot& color, const ObjectRoot& data) {
    SourceState result{def,{}};
    for (const auto& input : def->textures()) result.default_textures.push_back(input.path == "color" ? color : data);
    return result;
}
InstanceState instance_state(ObjectRoot parent, std::vector<Override> entries = {}) {
    InstanceState state{std::move(parent), std::move(entries), {}};
    state.textures.resize(state.entries.size());
    return state;
}
float color_red(const ProgramSurfaceRef& surface) { return take(surface->evaluate({})).base_color.x; }

void resources() {
    const auto program = compile();
    const auto def = definition(program);
    const auto second_def = definition(compile("0.75", false, true));
    const auto changed_def = definition(compile("0.75", true));
    ProgramSurfaceRef surviving;
    std::weak_ptr<const Image> weak_color;
    {
        Runtime runtime;
        take(register_resource_types(runtime));
        const auto color_image = pixel({128,64,32,128}), data_image = pixel({48,96,192,255});
        weak_color = color_image;
        auto color = take(create_texture(runtime, "image/color", color_image));
        auto data = take(create_texture(runtime, "image/data", data_image));
        auto source = take(create_source(runtime, "material/source", source_state(def, color, data)));
        auto a = take(create_instance(runtime, "material/a", instance_state(source, {{"gain", .5f, true}})));
        auto b = take(create_instance(runtime, "", instance_state(a, {
            {"gain", .9f, false}, {"removed", TextureBinding{"image/missing", Address::mirror}, false}})));
        require(take(material_key(runtime,a.id())) == "material/a" && take(material_key(runtime,b.id())).empty(), "Asset identity was inferred from parent role.");
        const auto initial = take(capture_program(runtime,b.id()));
        const float red = static_cast<float>(std::pow((128./255+.055)/1.055,2.4));
        near(color_red(initial),red*.5f);
        auto output = take(initial->evaluate({}));
        near(output.roughness,48.f/255); near(output.metallic,96.f/255); near(output.coverage,.5f);
        near(output.emissive.z,128.f/255); // Observe color alpha separately: sRGB decode must affect only RGB.
        require(initial->policy().coverage == Coverage::mask && !initial->policy().double_sided, "Source policy was lost.");
        require(take(read_diagnostics(runtime,b.id())).size() == 1, "Dormant unknown input did not retain a diagnostic.");

        // Steady capture/read are const, including nested borrowing of another
        // native object, as presentation's mesh traversal will do.
        auto unrelated = take(runtime.create(take(runtime.native_type<TextureResource>())));
        const auto cursor = take(runtime.change_cursor());
        auto steady = take(runtime.with_native<const TextureResource>(unrelated.id(), [&](const auto&) {
            return capture_program(runtime,b.id());
        }));
        (void)take(read_instance(runtime,b.id()));
        require(steady == initial && !runtime.changed_since(source.id(),cursor) && !runtime.changed_since(a.id(),cursor) &&
            !runtime.changed_since(b.id(),cursor) && !runtime.changed_since(color.id(),cursor), "Steady capture/read rewrote material or texture objects.");

        auto author_a = take(read_instance(runtime,a.id()));
        take(update_source(runtime,source.id(),source_state(second_def,color,data)));
        auto refreshed = take(runtime.with_native<const TextureResource>(unrelated.id(), [&](const auto&) {
            return capture_program(runtime,b.id());
        }));
        near(color_red(refreshed),red*.5f); // Equal override remains authored.
        author_a.entries[0].enabled = false; take(update_instance(runtime,a.id(),author_a));
        near(color_red(take(capture_program(runtime,b.id()))),red*.75f);
        require(std::get<float>(take(read_instance(runtime,a.id())).entries[0].value) == .5f, "Inheritance overwrote dormant author value.");
        author_a.entries[0].enabled = true; take(update_instance(runtime,a.id(),author_a));
        near(color_red(take(capture_program(runtime,b.id()))),red*.5f);

        auto author_b = take(read_instance(runtime,b.id()));
        author_b.entries[0].enabled = true; take(update_instance(runtime,b.id(),author_b));
        auto enabled = take(capture_program(runtime,b.id()));
        near(color_red(enabled),red*.9f);
        require(&enabled->program() == &second_def->program(), "An MI value edit replaced program content.");
        // Explicit old parent content remains meaningful after a live Source edit.
        const std::array<Override,1> inherited{{{"gain", .9f, false}}};
        const std::array<ImageRef,1> no_images{};
        near(color_red(take(resolve_instance(initial,inherited,no_images)).surface),red*.5f);

        // Clear samples the MS's linear empty value, not parent/default image or
        // another sRGB decode. Disabling restores inheritance without losing it.
        author_b.entries.push_back({"color",TextureBinding{"",Address::mirror},true});
        author_b.textures.resize(author_b.entries.size());
        take(update_instance(runtime,b.id(),author_b));
        const auto cleared = take(take(capture_program(runtime,b.id()))->evaluate({}));
        near(cleared.base_color.x,.1f*.9f);
        near(cleared.emissive.z,.25f); // Explicit empty input is already linear, including alpha.
        author_b.entries.back().enabled=false; take(update_instance(runtime,b.id(),author_b));
        near(color_red(take(capture_program(runtime,b.id()))),red*.9f);
        author_b.entries.back().enabled=true; take(update_instance(runtime,b.id(),author_b));
        near(color_red(take(capture_program(runtime,b.id()))),.1f*.9f);

        auto replacement = take(create_texture(runtime,"image/color",pixel({255,16,32,255})));
        author_b.entries.back().value=TextureBinding{"image/color",Address::repeat}; author_b.textures.back()=replacement;
        take(update_instance(runtime,b.id(),author_b));
        near(color_red(take(capture_program(runtime,b.id()))),.9f); // Same persistent key, exact different content.
        author_b.entries.back().enabled=false; take(update_instance(runtime,b.id(),author_b));
        near(color_red(take(capture_program(runtime,b.id()))),red*.9f);
        require(take(read_instance(runtime,b.id())).textures.back().id()==replacement.id(), "Dormant captured texture root was lost.");
        author_b.entries.back().enabled=true; take(update_instance(runtime,b.id(),author_b));
        near(color_red(take(capture_program(runtime,b.id()))),.9f);
        author_b.entries.back().value=TextureBinding{"",Address::mirror}; author_b.textures.back().reset();
        take(update_instance(runtime,b.id(),author_b));

        const auto before = take(capture_program(runtime,b.id()));
        auto bad = author_b;
        bad.entries.back().value=TextureBinding{"image/data",Address::clamp}; bad.textures.back()=color;
        require(!update_instance(runtime,b.id(),bad), "Wrong-key texture root was accepted.");
        bad=author_b; bad.entries[0].value=math::Vector2{1,2};
        require(!update_instance(runtime,b.id(),bad), "Enabled numeric type mismatch was accepted.");
        bad=author_b; bad.entries[1].enabled=true;
        require(!update_instance(runtime,b.id(),bad), "Unknown enabled input was accepted.");
        require(take(capture_program(runtime,b.id()))==before &&
            std::get<TextureBinding>(take(read_instance(runtime,b.id())).entries.back().value).asset_key.empty(), "Failed candidate replaced author data or Surface.");

        bad=author_a; bad.parent=b;
        require(!update_instance(runtime,a.id(),bad), "Indirect material cycle was accepted.");
        auto bare=take(runtime.create(take(runtime.native_type<Material>())));
        auto uninitialized=take(runtime.create(take(runtime.native_type<MaterialSource>())));
        for(const auto& invalid:{bare,uninitialized}) {
            bad=author_b; bad.parent=invalid;
            require(!update_instance(runtime,b.id(),bad) && !capture_program(runtime,invalid.id()), "Bare or uninitialized material was consumable.");
        }
        {
            Runtime other; take(register_resource_types(other));
            auto foreign=take(create_texture(other,"image/data",data_image));
            bad=author_b; bad.entries.back().value=TextureBinding{"image/data",Address::repeat}; bad.textures.back()=foreign;
            require(!update_instance(runtime,b.id(),bad), "A foreign Runtime texture root was accepted.");
        }
        require(take(read_instance(runtime,a.id())).parent.id()==source.id() &&
            take(read_instance(runtime,b.id())).parent.id()==a.id(), "Rejected parent update changed formal edges.");

        // A descendant cannot hide an ancestor's enabled mismatch. Earlier
        // owning content remains usable, but capture must report actual failure.
        take(update_source(runtime,source.id(),source_state(changed_def,color,data)));
        require(!capture_program(runtime,a.id()) && !capture_program(runtime,b.id()), "Old child cache disguised a failed new definition as success.");
        near(color_red(before),.1f*.9f);
        author_a.entries[0].enabled=false; take(update_instance(runtime,a.id(),author_a));
        author_b.entries[0].enabled=false; take(update_instance(runtime,b.id(),author_b));
        near(color_red(take(capture_program(runtime,b.id()))),.1f*.75f);
        require(!take(read_diagnostics(runtime,a.id())).empty(), "Dormant removed declaration lost its diagnostic.");
        take(update_source(runtime,source.id(),source_state(def,color,data)));
        author_b.entries[0].enabled=true; take(update_instance(runtime,b.id(),author_b));
        surviving=take(capture_program(runtime,b.id()));
        near(color_red(surviving),.1f*.9f);
        require(std::get<float>(take(read_instance(runtime,b.id())).entries[0].value)==.9f, "Declaration restoration lost local value.");

        // Required MS defaults remain dependencies even when this descendant
        // clears that input. Source replacement also fails atomically.
        const auto old_source=take(capture_program(runtime,source.id()));
        auto missing=source_state(def,color,data); missing.default_textures[0].reset();
        require(!update_source(runtime,source.id(),missing) && take(capture_program(runtime,source.id()))==old_source, "Missing MS default replaced valid content.");
        take(runtime.destroy(color.id()));
        require(!capture_program(runtime,b.id()), "Descendant clear concealed a destroyed required MS default.");
        near(color_red(initial),red*.5f); // Previously captured exact image survives.
        surviving=initial;
    }
    require(!weak_color.expired(), "Surface failed to own image content beyond Runtime closure.");
    near(take(surviving->evaluate({})).coverage,.5f);
    surviving.reset();
    require(weak_color.expired(), "Image content leaked after its final Surface owner.");
}

void ownership_and_filtering() {
    const auto def=definition(compile());
    Runtime runtime; take(register_resource_types(runtime));
    ObjectRoot leaf;
    ObjectId source_id, parent_id, texture_id, dormant_id;
    {
        auto color=take(create_texture(runtime,"image/color",pixel({255,128,0,255})));
        auto data=take(create_texture(runtime,"image/data",pixel({0,64,128,255})));
        auto dormant=take(create_texture(runtime,"image/dormant",pixel({16,32,64,128})));
        auto source=take(create_source(runtime,"source",source_state(def,color,data)));
        auto author=instance_state(source,{{"color",TextureBinding{"image/dormant",Address::repeat},false}});
        author.textures[0]=dormant;
        auto parent=take(create_instance(runtime,"parent",author));
        leaf=take(create_instance(runtime,"",instance_state(parent)));
        source_id=source.id(); parent_id=parent.id(); texture_id=color.id(); dormant_id=dormant.id();
    }
    collect(runtime);
    near(color_red(take(capture_program(runtime,leaf.id()))),.5f);
    require(bool(runtime.type_of(source_id)) && bool(runtime.type_of(parent_id)) && bool(runtime.type_of(texture_id)) &&
        bool(runtime.type_of(dormant_id)), "Formal parent/texture edges failed to retain their targets, including dormant captured inputs.");
    auto saved=take(capture_program(runtime,leaf.id()));
    leaf.reset(); collect(runtime);
    require(!runtime.type_of(source_id) && !runtime.type_of(parent_id) && !runtime.type_of(texture_id) &&
        !runtime.type_of(dormant_id), "Surface accidentally rooted Runtime objects.");
    near(color_red(saved),.5f);

    const auto colors=std::make_shared<const Image>(2,1,std::vector<std::uint8_t>{255,0,0,255, 0,128,0,64});
    const auto data=pixel({0,0,0,255});
    std::vector<ImageRef> images;
    for(const auto& input:def->textures()) images.push_back(input.path=="color"?colors:data);
    auto base=take(resolve_source(def,images));
    Context context; context.uv={1.125f,.5f};
    const std::array<ImageRef,1> local_images{colors};
    const auto red=[&](Address address) {
        const std::array<Override,1> entries{{{"color",TextureBinding{"image/color",address},true}}};
        return take(take(resolve_instance(base,entries,local_images)).surface->evaluate(context)).base_color.x;
    };
    near(red(Address::repeat),.375f); near(red(Address::clamp),0); near(red(Address::mirror),0);
    context.uv={-.375f,.5f};
    near(red(Address::repeat),.125f); near(red(Address::clamp),.5f); near(red(Address::mirror),.375f);
}
}

int main() {
    try {
        resources(); ownership_and_filtering();
        std::puts("Material resources: PASS (MS/MI/MI, sparse intent, exact bindings, explicit parent candidates, atomic target updates, const capture, owning CPU surface; GPU group adoption not covered)");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Material resources: FAIL: %s\n",error.what()); return 1;
    }
}

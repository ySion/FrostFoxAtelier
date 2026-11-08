#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "material/program_resources.hpp"
#include "material/resources.hpp"

#include <cstdio>
#include <limits>

namespace {
using namespace atelier;

using namespace atelier::material;

using Program = std::shared_ptr<const source::MaterialProgram>;

using namespace atelier::acceptance::helpers;

void near(float actual, float expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 2e-6f,
            "Material output differs from independent expected value.");
}

void collect(Runtime& runtime) {
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        (void)take(runtime.collect_step({32}));
}

Program compile(std::string_view gain = "0.5", bool renamed = false, bool reversed = false, bool vector_gain = false) {
    Runtime compiler;
    take(register_program_types(compiler));
    const std::string field = renamed ? "strength" : "gain";
    const std::string text =
        std::string(reversed ? "struct Textures { material.TextureInput data; material.TextureInput color; };\n"
                             : "struct Textures { material.TextureInput color; material.TextureInput data; };\n") +
        "struct Params { " + (vector_gain ? "vector2 " : "float ") + field +
        "; vector3 tint; };\n"
        "Params defaults() { Params p; p." +
        field + "=" + (vector_gain ? "vector2(" + std::string(gain) + ",0.0)" : std::string(gain)) +
        "; p.tint=vector3(0.2,0.3,0.4); return p; }\n"
        "material.Output shade(material.Context c, Params p, Textures textures) {\n"
        "vector4 color=sample(textures.color,c.uv); vector4 data=sample(textures.data,c.uv);\n"
        "material.Output o; o.base_color=vector3(color.x,color.y,color.z)*p." +
        field + (vector_gain ? ".x" : "") +
        ";\n"
        "o.emissive=vector3(p.tint.x,p.tint.y,color.w); o.coverage=data.w-0.5; o.roughness=data.x; o.metallic=data.y; o.normal_world=c.normal_world; return o; }";
    return take(source::compile_material(compiler, take(compiler.native_type<Context>()),
                                         take(compiler.native_type<Output>()),
                                         take(compiler.native_type<TextureInput>()), "resources.source", text));
} // Each compile Runtime dies before its program is consumed.

DefinitionRef definition(Program program) {
    // Author order intentionally differs from the first program's declarations.
    std::array<TextureDefault, 2> defaults{
        {{"data", {"image/data", Address::repeat}, TextureColor::linear, {.7f, .8f, .9f, .75f}},
         {"color", {"image/color", Address::clamp}, TextureColor::srgb, {.1f, .2f, .3f, .25f}}}};
    auto result = take(make_definition(std::move(program), {Coverage::mask, false, false}, defaults));
    defaults[1].empty_value = {9, 9, 9, 9}; // Factory must not retain writable author aliases.
    return result;
}

ImageRef pixel(std::array<std::uint8_t, 4> rgba) {
    return take(Image::create(1, 1, std::vector<std::uint8_t>(rgba.begin(), rgba.end())));
}

SourceState source_state(DefinitionRef def, const ObjectRoot& color, const ObjectRoot& data) {
    SourceState result{def, {}};
    for (const auto& input : def->textures())
        result.default_textures.push_back(input.path == "color" ? color : data);
    return result;
}

InstanceState instance_state(ObjectRoot parent, std::vector<Override> entries = {}) {
    InstanceState state{std::move(parent), std::move(entries), {}};
    state.textures.resize(state.entries.size());
    return state;
}

float color_red(const ProgramSurfaceRef& surface) {
    return take(surface->evaluate({})).base_color.x;
}

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
        const auto color_image = pixel({128, 64, 32, 128});
        const auto data_image = pixel({48, 96, 192, 255});
        weak_color = color_image;
        auto color = take(create_texture(runtime, "image/color", color_image));
        auto data = take(create_texture(runtime, "image/data", data_image));
        auto source = take(create_source(runtime, "material/source", source_state(def, color, data)));
        auto a = take(create_instance(runtime, "material/a", instance_state(source, {{"gain", .5f, true}})));
        auto b = take(
            create_instance(runtime, "",
                            instance_state(a, {{"gain", .9f, false},
                                               {"removed", TextureBinding{"image/missing", Address::mirror}, false}})));
        require(take(material_key(runtime, a.id())) == "material/a" && take(material_key(runtime, b.id())).empty(),
                "Asset identity was inferred from parent role.");
        const auto initial = take(capture_program(runtime, b.id()));
        const float red = static_cast<float>(std::pow((128. / 255 + .055) / 1.055, 2.4));
        near(color_red(initial), red * .5f);
        auto output = take(initial->evaluate({}));
        near(output.roughness, 48.f / 255);
        near(output.metallic, 96.f / 255);
        near(output.coverage, .5f);
        near(output.emissive.z, 128.f / 255); // Observe color alpha separately: sRGB decode must affect only RGB.
        require(initial->policy().coverage == Coverage::mask && !initial->policy().double_sided,
                "Source policy was lost.");
        require(initial->diagnostics().size() == 1, "Dormant unknown input did not retain a diagnostic.");

        // Steady capture/read are const, including nested borrowing of another
        // native object, as presentation's mesh traversal will do.
        auto unrelated = take(runtime.create(take(runtime.native_type<TextureResource>())));
        const auto cursor = take(runtime.change_cursor());
        auto steady = take(runtime.with_native<const TextureResource>(
            unrelated.id(), [&](const auto&) { return capture_program(runtime, b.id()); }));
        (void)take(read_instance(runtime, b.id()));
        require(steady == initial && !runtime.changed_since(source.id(), cursor) &&
                    !runtime.changed_since(a.id(), cursor) && !runtime.changed_since(b.id(), cursor) &&
                    !runtime.changed_since(color.id(), cursor),
                "Steady capture/read rewrote material or texture objects.");

        auto author_a = take(read_instance(runtime, a.id()));
        take(update_source(runtime, source.id(), source_state(second_def, color, data)));
        auto refreshed = take(runtime.with_native<const TextureResource>(
            unrelated.id(), [&](const auto&) { return capture_program(runtime, b.id()); }));
        near(color_red(refreshed), red * .5f); // Equal override remains authored.
        author_a.entries[0].enabled = false;
        take(update_instance(runtime, a.id(), author_a));
        near(color_red(take(capture_program(runtime, b.id()))), red * .75f);
        require(std::get<float>(take(read_instance(runtime, a.id())).entries[0].value) == .5f,
                "Inheritance overwrote dormant author value.");
        author_a.entries[0].enabled = true;
        take(update_instance(runtime, a.id(), author_a));
        near(color_red(take(capture_program(runtime, b.id()))), red * .5f);

        auto author_b = take(read_instance(runtime, b.id()));
        author_b.entries[0].enabled = true;
        take(update_instance(runtime, b.id(), author_b));
        auto enabled = take(capture_program(runtime, b.id()));
        near(color_red(enabled), red * .9f);
        require(&enabled->program() == &second_def->program(), "An MI value edit replaced program content.");
        // Explicit old parent content remains meaningful after a live Source edit.
        const std::array<Override, 1> inherited{{{"gain", .9f, false}}};
        const std::array<ImageRef, 1> no_images{};
        near(color_red(take(resolve_instance(initial, inherited, no_images))), red * .5f);

        // Clear samples the MS's linear empty value, not parent/default image or
        // another sRGB decode. Disabling restores inheritance without losing it.
        author_b.entries.push_back({"color", TextureBinding{"", Address::mirror}, true});
        author_b.textures.resize(author_b.entries.size());
        take(update_instance(runtime, b.id(), author_b));
        const auto cleared = take(take(capture_program(runtime, b.id()))->evaluate({}));
        near(cleared.base_color.x, .1f * .9f);
        near(cleared.emissive.z, .25f); // Explicit empty input is already linear, including alpha.
        author_b.entries.back().enabled = false;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), red * .9f);
        author_b.entries.back().enabled = true;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), .1f * .9f);

        auto replacement = take(create_texture(runtime, "image/color", pixel({255, 16, 32, 255})));
        author_b.entries.back().value = TextureBinding{"image/color", Address::repeat};
        author_b.textures.back() = replacement;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), .9f); // Same persistent key, exact different content.
        author_b.entries.back().enabled = false;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), red * .9f);
        require(take(read_instance(runtime, b.id())).textures.back().id() == replacement.id(),
                "Dormant captured texture root was lost.");
        author_b.entries.back().enabled = true;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), .9f);
        const auto exact_replacement = take(capture_program(runtime, b.id()));
        take(runtime.destroy(replacement.id()));
        const auto replaced_fallback = take(capture_program(runtime, b.id()));
        near(color_red(replaced_fallback), red * .9f);
        near(color_red(exact_replacement), .9f);
        require(replaced_fallback->diagnostics().size() == 2 && replaced_fallback->inputs()[1].image == color_image,
                "Destroyed MI texture did not inherit the current exact parent image.");
        auto after_loss = take(read_instance(runtime, b.id()));
        after_loss.entries[0].value = .85f;
        require(bool(update_instance(runtime, b.id(), after_loss)),
                "Editing an MI after its known texture was destroyed must keep using inherited content.");
        near(color_red(take(capture_program(runtime, b.id()))), red * .85f);
        after_loss.entries.back().value = TextureBinding{"image/wrong", Address::repeat};
        require(!update_instance(runtime, b.id(), after_loss),
                "A destroyed known root was accepted for a different authored key.");
        auto unavailable = take(create_texture(runtime, "image/unavailable", {}));
        author_b.entries.back().value = TextureBinding{"image/unavailable", Address::repeat};
        author_b.textures.back() = unavailable;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), red * .9f);
        require(std::get<TextureBinding>(take(read_instance(runtime, b.id())).entries.back().value).asset_key ==
                    "image/unavailable",
                "Unavailable image content changed the authored resource key.");
        author_b.textures.back().reset();
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), red * .9f);
        auto repaired = take(create_texture(runtime, "image/unavailable", pixel({255, 16, 32, 255})));
        author_b.textures.back() = repaired;
        take(update_instance(runtime, b.id(), author_b));
        const auto repaired_surface = take(capture_program(runtime, b.id()));
        near(color_red(repaired_surface), .9f);
        require(repaired_surface->diagnostics().size() == 1, "Repair left a stale local texture diagnostic.");
        author_b.entries.back().value = TextureBinding{"", Address::mirror};
        author_b.textures.back().reset();
        take(update_instance(runtime, b.id(), author_b));

        const auto before = take(capture_program(runtime, b.id()));
        auto bad = author_b;
        bad.entries.back().value = TextureBinding{"image/data", Address::clamp};
        bad.textures.back() = color;
        require(!update_instance(runtime, b.id(), bad), "Wrong-key texture root was accepted.");
        require(
            take(capture_program(runtime, b.id())) == before &&
                std::get<TextureBinding>(take(read_instance(runtime, b.id())).entries.back().value).asset_key.empty(),
            "Failed candidate replaced author data or Surface.");
        bad = author_b;
        bad.entries[0].value = math::Vector2{1, 2};
        require(bool(update_instance(runtime, b.id(), bad)),
                "Enabled numeric type mismatch must publish the current inherited value with a diagnostic.");
        const auto mismatched_surface = take(capture_program(runtime, b.id()));
        near(color_red(mismatched_surface), .1f * .5f);
        const auto mismatched = take(read_instance(runtime, b.id()));
        require(mismatched.entries[0].enabled && std::get<math::Vector2>(mismatched.entries[0].value).y == 2 &&
                    mismatched_surface->diagnostics().size() == 2,
                "Fallback rewrote the enabled author value or lost its diagnostic.");
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            bad = author_b;
            bad.entries[0].value = invalid;
            bad.entries.push_back({"tint", math::Vector3{.8f, .7f, .6f}, true});
            bad.textures.emplace_back();
            take(update_instance(runtime, b.id(), bad));
            const auto resolved = take(capture_program(runtime, b.id()));
            near(color_red(resolved), .1f * .5f);
            near(take(resolved->evaluate({})).emissive.x, .8f);
            require(!std::isfinite(std::get<float>(take(read_instance(runtime, b.id())).entries[0].value)) &&
                        resolved->diagnostics().size() == 2,
                    "Nonfinite fallback changed author data or discarded a valid sibling override.");
        }
        bad = author_b;
        bad.entries.push_back({"tint", math::Vector3{.8f, std::numeric_limits<float>::quiet_NaN(), .6f}, true});
        bad.textures.emplace_back();
        take(update_instance(runtime, b.id(), bad));
        near(take(take(capture_program(runtime, b.id()))->evaluate({})).emissive.x, .2f);
        take(update_instance(runtime, b.id(), author_b));
        bad = author_b;
        bad.entries[1].enabled = true;
        take(update_instance(runtime, b.id(), bad));
        const auto removed_input = take(capture_program(runtime, b.id()));
        near(color_red(removed_input), .1f * .9f);
        require(take(read_instance(runtime, b.id())).entries[1].enabled && removed_input->diagnostics().size() == 1,
                "Removed enabled input lost authored intent or diagnostic.");
        take(update_instance(runtime, b.id(), author_b));

        bad = author_a;
        bad.parent = b;
        require(!update_instance(runtime, a.id(), bad), "Indirect material cycle was accepted.");
        auto bare = take(runtime.create(take(runtime.native_type<Material>())));
        auto uninitialized = take(runtime.create(take(runtime.native_type<MaterialSource>())));
        for (const auto& invalid : {bare, uninitialized}) {
            bad = author_b;
            bad.parent = invalid;
            require(!update_instance(runtime, b.id(), bad) && !capture_program(runtime, invalid.id()),
                    "Bare or uninitialized material was consumable.");
        }
        {
            Runtime other;
            take(register_resource_types(other));
            auto foreign = take(create_texture(other, "image/data", data_image));
            bad = author_b;
            bad.entries.back().value = TextureBinding{"image/data", Address::repeat};
            bad.textures.back() = foreign;
            require(!update_instance(runtime, b.id(), bad), "A foreign Runtime texture root was accepted.");
        }
        require(take(read_instance(runtime, a.id())).parent.id() == source.id() &&
                    take(read_instance(runtime, b.id())).parent.id() == a.id(),
                "Rejected parent update changed formal edges.");

        take(update_source(runtime, source.id(),
                           source_state(definition(compile("0.8", false, false, true)), color, data)));
        const auto retyped = take(capture_program(runtime, b.id()));
        near(color_red(retyped), .1f * .8f);
        require(retyped->diagnostics().size() == 3 &&
                    std::get<float>(take(read_instance(runtime, b.id())).entries[0].value) == .9f,
                "Source parameter type change lost overrides or failed to report inherited mismatches.");

        // Declaration removal uses current defaults, not the old layout/value.
        // Earlier owners remain usable without becoming the new publication.
        take(update_source(runtime, source.id(), source_state(changed_def, color, data)));
        const auto removed_declaration = take(capture_program(runtime, a.id()));
        near(color_red(removed_declaration), red * .75f);
        near(color_red(take(capture_program(runtime, b.id()))), .1f * .75f);
        require(take(read_instance(runtime, a.id())).entries[0].enabled && !removed_declaration->diagnostics().empty(),
                "Enabled removed declaration was rewritten or lost its diagnostic.");
        near(color_red(before), .1f * .9f);
        author_a.entries[0].enabled = false;
        take(update_instance(runtime, a.id(), author_a));
        author_b.entries[0].enabled = false;
        take(update_instance(runtime, b.id(), author_b));
        near(color_red(take(capture_program(runtime, b.id()))), .1f * .75f);
        const auto dormant_parent = take(capture_program(runtime, a.id()));
        require(!dormant_parent->diagnostics().empty(), "Dormant removed declaration lost its diagnostic.");
        take(update_source(runtime, source.id(), source_state(def, color, data)));
        author_b.entries[0].enabled = true;
        take(update_instance(runtime, b.id(), author_b));
        surviving = take(capture_program(runtime, b.id()));
        near(color_red(surviving), .1f * .9f);
        require(std::get<float>(take(read_instance(runtime, b.id())).entries[0].value) == .9f,
                "Declaration restoration lost local value.");

        // Missing MS content resolves to its typed empty value, never old pixels.
        const auto old_source = take(capture_program(runtime, source.id()));
        auto missing = source_state(def, color, data);
        missing.default_textures[0].reset();
        require(bool(update_source(runtime, source.id(), missing)),
                "Missing MS texture must publish typed default sampling with a diagnostic.");
        const auto missing_surface = take(capture_program(runtime, source.id()));
        near(color_red(missing_surface), .1f * .5f);
        require(take(capture_program(runtime, source.id())) != old_source && missing_surface->diagnostics().size() == 1,
                "Missing source texture retained stale content or lost its diagnostic.");
        take(update_source(runtime, source.id(), source_state(def, color, data)));
        take(runtime.destroy(color.id()));
        auto lost = capture_program(runtime, b.id());
        require(bool(lost), "Destroyed required texture must refresh descendant caches to current defaults.");
        near(color_red(*lost), .1f * .9f);
        near(color_red(take(capture_program(runtime, source.id()))), .1f * .5f);
        require((*lost)->diagnostics().size() == 2 &&
                    (*lost)->diagnostics()[0].message.find("image/color") != std::string::npos,
                "Ancestor texture loss is invisible in the consumed result.");
        const auto lost_cursor = take(runtime.change_cursor());
        require(take(capture_program(runtime, b.id())) == *lost && !runtime.changed_since(source.id(), lost_cursor) &&
                    !runtime.changed_since(a.id(), lost_cursor) && !runtime.changed_since(b.id(), lost_cursor),
                "Steady fallback capture rewrote caches.");
        take(update_source(runtime, source.id(), source_state(second_def, color, data)));
        near(color_red(take(capture_program(runtime, source.id()))), .1f * .75f);
        require(take(read_source(runtime, source.id())).default_textures[1].id() == color.id(),
                "Source edit after texture loss erased the known author root or rebound by old slot order.");
        near(color_red(initial), red * .5f); // Previously captured exact image survives.
        surviving = initial;
    }
    require(!weak_color.expired(), "Surface failed to own image content beyond Runtime closure.");
    near(take(surviving->evaluate({})).coverage, .5f);
    surviving.reset();
    require(weak_color.expired(), "Image content leaked after its final Surface owner.");
}

void ownership_and_filtering() {
    const auto def = definition(compile());
    Runtime runtime;
    take(register_resource_types(runtime));
    ObjectRoot leaf;
    ObjectId source_id;
    ObjectId parent_id;
    ObjectId texture_id;
    ObjectId dormant_id;
    {
        auto color = take(create_texture(runtime, "image/color", pixel({255, 128, 0, 255})));
        auto data = take(create_texture(runtime, "image/data", pixel({0, 64, 128, 255})));
        auto dormant = take(create_texture(runtime, "image/dormant", pixel({16, 32, 64, 128})));
        auto source = take(create_source(runtime, "source", source_state(def, color, data)));
        auto author = instance_state(source, {{"color", TextureBinding{"image/dormant", Address::repeat}, false}});
        author.textures[0] = dormant;
        auto parent = take(create_instance(runtime, "parent", author));
        leaf = take(create_instance(runtime, "", instance_state(parent)));
        source_id = source.id();
        parent_id = parent.id();
        texture_id = color.id();
        dormant_id = dormant.id();
    }
    collect(runtime);
    near(color_red(take(capture_program(runtime, leaf.id()))), .5f);
    require(bool(runtime.type_of(source_id)) && bool(runtime.type_of(parent_id)) && bool(runtime.type_of(texture_id)) &&
                bool(runtime.type_of(dormant_id)),
            "Formal parent/texture edges failed to retain their targets, including dormant captured inputs.");
    auto saved = take(capture_program(runtime, leaf.id()));
    take(update_texture(runtime, texture_id, pixel({0, 128, 255, 128})));
    const auto refreshed = take(capture_program(runtime, leaf.id()));
    near(color_red(refreshed), 0);
    near(take(refreshed->evaluate({})).emissive.z, 128.f / 255);
    near(color_red(saved), .5f);
    require(
        refreshed != saved && take(capture_program(runtime, leaf.id())) == refreshed &&
            take(read_source(runtime, source_id)).default_textures[0].id() == texture_id,
        "In-place texture publication lost the identity, mutated an old capture or failed to stabilize descendants.");
    leaf.reset();
    collect(runtime);
    require(!runtime.type_of(source_id) && !runtime.type_of(parent_id) && !runtime.type_of(texture_id) &&
                !runtime.type_of(dormant_id),
            "Surface accidentally rooted Runtime objects.");
    near(color_red(saved), .5f);

    const auto colors = take(Image::create(2, 1, std::vector<std::uint8_t>{255, 0, 0, 255, 0, 128, 0, 64}));
    const auto data = pixel({0, 0, 0, 255});
    std::vector<ImageRef> images;
    for (const auto& input : def->textures())
        images.push_back(input.path == "color" ? colors : data);
    auto base = take(resolve_source(def, images));
    Context context;
    context.uv = {1.125f, .5f};
    const std::array<ImageRef, 1> local_images{colors};
    const auto red = [&](Address address) {
        const std::array<Override, 1> entries{{{"color", TextureBinding{"image/color", address}, true}}};
        return take(take(resolve_instance(base, entries, local_images))->evaluate(context)).base_color.x;
    };
    near(red(Address::repeat), .375f);
    near(red(Address::clamp), 0);
    near(red(Address::mirror), 0);
    context.uv = {-.375f, .5f};
    near(red(Address::repeat), .125f);
    near(red(Address::clamp), .5f);
    near(red(Address::mirror), .375f);
}

void whole_material_fallback() {
    Runtime compiler;
    take(register_program_types(compiler));
    auto rejected =
        source::compile_material(compiler, take(compiler.native_type<Context>()), take(compiler.native_type<Output>()),
                                 take(compiler.native_type<TextureInput>()), "broken.source", "not a material");
    require(!rejected, "Invalid material source was silently accepted by the compiler.");
    auto resolved = resolve_source({}, {});
    require(bool(resolved), "An explicit missing definition must resolve to the engine default material.");
    auto fallback = *resolved;
    const auto output = take(fallback->evaluate({}));
    near(output.base_color.x, 1);
    near(output.base_color.y, 0);
    near(output.base_color.z, 1);
    require(fallback->policy().coverage == Coverage::opaque && fallback->inputs().empty() &&
                !fallback->diagnostics().empty() && !take(fallback->program().hlsl("engine_default")).empty(),
            "Whole-material fallback lacks policy, diagnostic, or actual HLSL content.");
    require(fallback == take(default_program_surface()) && fallback == take(resolve_source({}, {})),
            "Engine default code/content was not reused.");

    Runtime runtime;
    take(register_resource_types(runtime));
    const auto def = definition(compile());
    auto color = take(create_texture(runtime, "image/color", pixel({255, 128, 0, 255})));
    auto data = take(create_texture(runtime, "image/data", pixel({0, 64, 128, 255})));
    auto source = take(create_source(runtime, "source", source_state(def, color, data)));
    auto parent = take(create_instance(runtime, "parent", instance_state(source, {{"gain", .8f, true}})));
    auto author = instance_state(parent, {{"gain", .6f, true}, {"marker", 7.f, true}});
    auto child = take(create_instance(runtime, "child", author));
    const auto original = take(capture_program(runtime, child.id()));
    near(color_red(original), .6f);
    take(update_source(runtime, source.id(), {}));
    const auto missing = take(capture_program(runtime, child.id()));
    require(missing->uses_default_material() && missing->diagnostics().size() == 4,
            "Missing definition did not use the default material or hid ancestor diagnostics.");
    near(color_red(missing), 1);
    require(!take(read_source(runtime, source.id())).definition &&
                take(read_instance(runtime, child.id())).entries.size() == 2,
            "Default publication was written back as authored definition or overrides.");
    take(update_source(runtime, source.id(), source_state(def, color, data)));
    near(color_red(take(capture_program(runtime, child.id()))), .6f);
    take(runtime.destroy(source.id()));
    auto lost = capture_program(runtime, child.id());
    require(
        bool(lost),
        "An initialized MI with a destroyed ancestor must consume the engine default, not fail or retain old content.");
    require((*lost)->uses_default_material() && (*lost)->diagnostics()[0].path == "$parent",
            "Destroyed parent fallback is not visible to descendants.");
    near(color_red(*lost), 1);
    near(color_red(original), .6f);
    const auto cursor = take(runtime.change_cursor());
    require(take(capture_program(runtime, child.id())) == *lost && !runtime.changed_since(parent.id(), cursor) &&
                !runtime.changed_since(child.id(), cursor),
            "Steady missing-parent capture rewrote material caches.");
    auto parent_author = take(read_instance(runtime, parent.id()));
    require(parent_author.parent.id() == source.id(), "Destroyed parent identity was erased from author state.");
    parent_author.entries[0].value = .7f;
    require(bool(update_instance(runtime, parent.id(), parent_author)),
            "Editing an initialized MI after parent destruction must retain its fallback and author edge.");
    auto recovered = take(create_source(runtime, "source", source_state(def, color, data)));
    parent_author.parent = recovered;
    take(update_instance(runtime, parent.id(), parent_author));
    near(color_red(take(capture_program(runtime, child.id()))), .6f);
    parent_author.parent.reset();
    require(bool(update_instance(runtime, parent.id(), parent_author)),
            "An explicitly missing parent on an initialized MI must resolve to the engine default.");
    require(take(capture_program(runtime, child.id()))->uses_default_material(),
            "Explicit parent absence left descendant old content active.");
    require(!create_instance(runtime, "invalid", instance_state({})),
            "An uninitialized instance without a parent was accepted.");
}

void fallback_boundaries() {
    Runtime runtime;
    take(register_resource_types(runtime));
    const auto def = definition(compile());
    auto color = take(create_texture(runtime, "image/color", pixel({255, 0, 0, 255})));
    auto data = take(create_texture(runtime, "image/data", pixel({0, 64, 128, 255})));
    auto source = take(create_source(runtime, "source", source_state(def, color, data)));
    auto author = instance_state(
        source, {{"color", TextureBinding{"image/missing", Address::mirror}, true}, {"gain", .8f, true}});
    auto child = take(create_instance(runtime, "child", author));
    near(color_red(take(capture_program(runtime, child.id()))), .8f);
    take(update_source(runtime, source.id(), source_state(def, {}, {})));
    auto defaults = take(capture_program(runtime, child.id()));
    auto output = take(defaults->evaluate({}));
    near(output.base_color.x, .08f);
    near(output.emissive.z, .25f);
    near(output.roughness, .7f);
    near(output.metallic, .8f);
    near(output.coverage, .25f);
    require(defaults->diagnostics().size() == 3 && !defaults->inputs()[0].image && !defaults->inputs()[1].image &&
                take(read_source(runtime, source.id())).definition->textures()[0].binding.asset_key == "image/color",
            "Typed source defaults concealed missing content or rewrote source author keys.");
    auto absent = take(create_instance(runtime, "absent", instance_state(source)));
    require(take(read_instance(runtime, absent.id())).entries.empty(),
            "Never-authored overrides were materialized by fallback.");
    take(update_source(runtime, source.id(), source_state(def, color, data)));
    defaults = take(capture_program(runtime, child.id()));
    near(color_red(defaults), .8f);
    require(defaults->diagnostics().size() == 1, "Source repair did not clear inherited failure diagnostics.");

    auto malformed = author;
    malformed.textures.pop_back();
    require(!update_instance(runtime, child.id(), malformed),
            "Malformed root table was defaulted instead of rejected.");
    malformed = author;
    malformed.entries.push_back(malformed.entries[0]);
    malformed.textures.emplace_back();
    require(!update_instance(runtime, child.id(), malformed),
            "Duplicate author paths were defaulted instead of rejected.");
    malformed = author;
    malformed.textures[0] = data;
    require(!update_instance(runtime, child.id(), malformed),
            "Wrong-key supplied texture was defaulted instead of rejected.");
    malformed = author;
    malformed.textures[1] = color;
    require(!update_instance(runtime, child.id(), malformed), "Numeric input with a texture root was accepted.");
    auto bad_source = source_state(def, color, data);
    bad_source.default_textures.pop_back();
    require(!update_source(runtime, source.id(), bad_source),
            "Malformed source roots were defaulted instead of rejected.");
    const std::array<ImageRef, 1> no_image{};
    require(!resolve_source(def, no_image) && !resolve_source({}, no_image) &&
                !resolve_instance(defaults, author.entries, no_image),
            "Pure resolution accepted malformed image tables.");
    const std::array<Override, 1> cleared{{{"removed", TextureBinding{}, false}}};
    const std::array<ImageRef, 1> content{pixel({255, 255, 255, 255})};
    require(!resolve_instance(defaults, cleared, content),
            "Dormant deleted clear with supplied image bypassed structural validation.");
    require(take(capture_program(runtime, child.id())) == defaults,
            "Structural failure replaced the previous publication.");

    Runtime foreign;
    take(register_resource_types(foreign));
    auto foreign_source = take(create_source(foreign, "foreign", {}));
    malformed = author;
    malformed.parent = foreign_source;
    require(!update_instance(runtime, child.id(), malformed) && !capture_program(foreign, source.id()),
            "Foreign Runtime material identity was defaulted.");
    auto dead = take(create_source(runtime, "dead", {}));
    take(runtime.destroy(dead.id()));
    malformed = author;
    malformed.parent = dead;
    require(!update_instance(runtime, child.id(), malformed), "An unrelated destroyed parent identity was defaulted.");
    auto dead_texture = take(create_texture(runtime, "image/missing", pixel({0, 0, 0, 255})));
    take(runtime.destroy(dead_texture.id()));
    malformed = author;
    malformed.textures[0] = dead_texture;
    require(!update_instance(runtime, child.id(), malformed),
            "An unvalidated destroyed texture identity was defaulted.");
    require(!capture_program(runtime, ObjectId{}), "Invalid material identity was defaulted.");
    Context invalid;
    invalid.uv.x = std::numeric_limits<float>::quiet_NaN();
    require(!defaults->evaluate(invalid), "Runtime evaluation failure was silently replaced by a material default.");
    require(
        take(runtime.with_native<const MaterialSource>(
            source.id(), [&](const auto&) -> Result<bool> { return !update_instance(runtime, child.id(), author); })),
        "Runtime publication-busy failure was masked by fallback.");
}

void capture_dependencies() {
    Runtime runtime;
    take(register_resource_types(runtime));
    auto empty = take(create_texture(runtime, "image/color", {}));
    auto dormant = take(create_texture(runtime, "image/dormant", pixel({255, 0, 0, 255})));
    auto source = take(create_source(runtime, "source", source_state(definition(compile()), empty, {})));
    auto a = take(create_instance(runtime, "a", instance_state(source)));
    auto b = take(create_instance(runtime, "b", instance_state(source)));
    auto state = instance_state(a, {{"removed", TextureBinding{"image/dormant"}, false}});
    state.textures[0] = dormant;
    auto child = take(create_instance(runtime, "child", state));
    const auto first = take(capture_program_with_dependencies(runtime, child.id()));
    require(first.surface == take(capture_program(runtime, child.id())) && first.dependencies.size() == 5 &&
                first.dependencies.contains(child.id()) && first.dependencies.contains(a.id()) &&
                first.dependencies.contains(source.id()) && first.dependencies.contains(empty.id()) &&
                first.dependencies.contains(dormant.id()),
            "Capture lost its live empty image, dormant texture, or actual parent chain.");
    state.parent = b;
    take(update_instance(runtime, child.id(), state));
    const auto rebound = take(capture_program_with_dependencies(runtime, child.id()));
    require(rebound.surface == first.surface && rebound.dependencies.size() == 5 &&
                !rebound.dependencies.contains(a.id()) && rebound.dependencies.contains(b.id()),
            "An unchanged Surface concealed a changed parent dependency.");
    take(update_texture(runtime, empty.id(), pixel({255, 0, 0, 255})));
    const auto restored = take(capture_program_with_dependencies(runtime, child.id()));
    near(color_red(restored.surface), .5f);
    near(color_red(first.surface), .05f);
    take(runtime.destroy(empty.id()));
    const auto lost = take(capture_program_with_dependencies(runtime, child.id()));
    require(!lost.dependencies.contains(empty.id()) && lost.dependencies.contains(dormant.id()),
            "Successful texture fallback retained a dead dependency or discarded a live dormant edge.");
    near(color_red(lost.surface), .05f);
    take(runtime.destroy(b.id()));
    const auto orphan = take(capture_program_with_dependencies(runtime, child.id()));
    require(orphan.surface->uses_default_material() && orphan.dependencies.size() == 2 &&
                orphan.dependencies.contains(child.id()) && orphan.dependencies.contains(dormant.id()),
            "Successful parent fallback kept the dead parent or its unreachable ancestors.");
    const auto nested = take(runtime.with_native<const MaterialInstance>(
        child.id(), [&](const auto&) { return capture_program_with_dependencies(runtime, child.id()); }));
    require(nested.surface == orphan.surface && nested.dependencies == orphan.dependencies,
            "Dependency capture changed the legal nested const-borrow contract.");
    const auto bad = capture_program_with_dependencies(runtime, ObjectId{});
    const auto ordinary = capture_program(runtime, ObjectId{});
    require(!bad && !ordinary && bad.error().code == ordinary.error().code &&
                bad.error().message == ordinary.error().message,
            "Dependency capture changed the original capture error.");
    auto temporary = take(create_instance(runtime, "temporary", instance_state(source)));
    const auto id = temporary.id();
    const auto lease = take(capture_program_with_dependencies(runtime, id));
    temporary = {};
    collect(runtime);
    require(!runtime.type_of(id) && lease.dependencies.contains(id), "Dependency observations rooted a material.");
    near(color_red(lease.surface), .05f);
}
}

int main() {
    atelier::install_fatal_handler();
    resources();
    ownership_and_filtering();
    whole_material_fallback();
    fallback_boundaries();
    capture_dependencies();
    std::puts(
        "Material resources: PASS (MS/MI/MI, sparse intent, typed/default fallback, ancestor diagnostics, destruction/recovery, structural rejection, const capture, owning CPU/HLSL surface; GPU group adoption not covered)");
    return 0;
}

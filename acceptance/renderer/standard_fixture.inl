// Acceptance author values only. None of these types enter the Renderer.
// Existing image/lighting scenes author real built-in StandardPBR MS/MI content,
// then submit the same public ProgramSurface/MaterialBatch API as native hosts.
namespace standard_fixture {
using atelier::material::Image;

using atelier::material::ImageRef;

using atelier::material::Address;

using atelier::material::Coverage;

enum class Input { base_color, roughness, metallic, normal, emissive, opacity };

struct Texture {
    ImageRef image;

    std::array<float, 2> scale{1, 1};

    std::array<float, 2> offset{};

    float rotation{};

    Address address{Address::repeat};

    std::uint32_t channel{};

    bool operator==(const Texture&) const = default;
};

struct Parameters {
    std::array<float, 4> base_color{1, 1, 1, 1};

    float roughness{.65f};

    float metallic{};

    float normal_scale{1};

    float normal_y_sign{1};

    std::array<float, 3> emissive{};

    Coverage coverage{Coverage::opaque};

    float alpha_cutoff{.5f};

    bool double_sided{true};

    bool emission_two_sided{true};

    bool operator==(const Parameters&) const = default;
};

struct Description {
    Parameters parameters;

    std::array<Texture, 6> textures;

    bool operator==(const Description&) const = default;

    Texture& texture(Input input) { return textures[static_cast<std::size_t>(input)]; }

    const Texture& texture(Input input) const { return textures[static_cast<std::size_t>(input)]; }
};

atelier::material::ProgramSurfaceRef capture(const Description& value) {
    namespace m = atelier::material;
    namespace math = atelier::math;
    static const auto program = [] {
        atelier::Runtime compiler;
        take(m::register_program_types(compiler));
        const auto catalog = atelier::assets::material_assets();
        const auto& author = std::get<atelier::assets::MaterialSourceAsset>(catalog.front().author);
        const auto file = atelier::assets::builtin_material_source(author.source_path);
        check(bool(file), "StandardPBR source belongs to the real builtin catalog");
        return take(atelier::source::compile_material(
            compiler, take(compiler.native_type<m::Context>()), take(compiler.native_type<m::Output>()),
            take(compiler.native_type<m::TextureInput>()), file->path, file->source));
    }();
    const auto& p = value.parameters;
    const auto catalog = atelier::assets::material_assets();
    const auto& author = std::get<atelier::assets::MaterialSourceAsset>(
        catalog[(p.coverage == Coverage::mask ? 2 : 0) + (p.double_sided ? 0 : 1)].author);
    const auto definition =
        take(m::make_definition(program, {p.coverage, p.double_sided, p.emission_two_sided}, author.inputs));
    const atelier::Array<ImageRef> default_images(definition->textures().size());
    const auto parent = take(m::resolve_source(definition, default_images));
    atelier::Array<m::Override> entries;
    atelier::Array<ImageRef> images;
    const auto add = [&](atelier::Utf8Text path, m::LocalValue v, ImageRef image = {}) {
        entries.add({std::move(path), std::move(v), true});
        images.add(std::move(image));
    };
    add("base_color", math::Vector4{p.base_color[0], p.base_color[1], p.base_color[2], p.base_color[3]});
    add("roughness", p.roughness);
    add("metallic", p.metallic);
    add("emissive", math::Vector3{p.emissive[0], p.emissive[1], p.emissive[2]});
    add("alpha_cutoff", p.alpha_cutoff);
    add("normal_scale", p.normal_scale);
    add("normal_y_sign", p.normal_y_sign);
    constexpr atelier::StringView names[]{"base_color", "roughness", "metallic", "normal", "emissive", "opacity"};
    for (std::size_t i = 0; i < value.textures.size(); ++i) {
        using namespace atelier::literals;
        const auto& t = value.textures[i];
        const auto name = atelier::String::validated(names[i]);
        add(atelier::String::concat(name, "_texture"),
            m::TextureBinding{t.image ? "acceptance/texture"_s : ""_s, t.address}, t.image);
        add(atelier::String::concat(name, "_uv.scale"), math::Vector2{t.scale[0], t.scale[1]});
        add(atelier::String::concat(name, "_uv.offset"), math::Vector2{t.offset[0], t.offset[1]});
        add(atelier::String::concat(name, "_uv.rotation"), t.rotation);
        if (i == 1 || i == 2 || i == 5)
            add(atelier::String::concat(name, "_channel"), float(t.channel));
    }
    auto result = take(m::resolve_instance(parent, entries, images));
    check(result->diagnostics().empty(), "fixture StandardPBR MI resolves every typed author entry");
    return result;
}

class Surface {
public:
    explicit Surface(Description value = {}) : value_(std::move(value)) {}

    bool describes(const Description& value) const { return value_ == value; }

    atelier::material::ProgramSurfaceRef program() const {
        if (!program_)
            program_ = capture(value_);
        return program_;
    }

private:
    Description value_;

    mutable atelier::material::ProgramSurfaceRef program_;
};

using SurfaceRef = std::shared_ptr<const Surface>;
}

namespace render {
// Compact author fixture notation, NOT aliases of the removed product material
// API. prepare() below is the only conversion into native draw snapshots.
struct Material {
    float roughness{.65f};

    float metallic{};

    std::array<float, 3> emissive{};
};

struct MeshInstance {
    atelier::math::Matrix4 world = atelier::math::Matrix4::identity();

    ui::Color color{.2f, .45f, .75f, 1};

    bool selected{};

    atelier::geometry::MeshRef geometry;

    Material material{};

    std::uint64_t stable_key{};

    std::shared_ptr<const atelier::Array<standard_fixture::SurfaceRef>> surfaces{};
};

struct Frame : atelier::renderer::Frame {
    atelier::Span<const MeshInstance> meshes;

    struct Candidate {
        MaterialBatch batch;

        atelier::Array<standard_fixture::SurfaceRef> authors;

        atelier::Array<atelier::renderer::MeshInstance> meshes;
    };

    mutable std::unique_ptr<Candidate> candidate;

    Frame(const ui::Frame& drawing, Camera view_camera, ui::Rect viewport, atelier::Span<const MeshInstance> instances)
        : atelier::renderer::Frame{drawing, view_camera, viewport, {}}, meshes(instances) {}

    Frame(const Frame& other) : atelier::renderer::Frame(other), meshes(other.meshes) {}

    void release_materials() const { candidate.reset(); }

    atelier::renderer::Frame prepare(Renderer& renderer) const {
        auto native = static_cast<const atelier::renderer::Frame&>(*this);
        if (meshes.empty()) {
            candidate.reset();
            native.meshes = {};
            native.materials = nullptr;
            return native;
        }
        if (!candidate)
            candidate = std::make_unique<Candidate>();
        auto& old = *candidate;
        atelier::Array<standard_fixture::SurfaceRef> authors;
        atelier::Array<atelier::material::ProgramSurfaceRef> surfaces;
        atelier::Array<atelier::renderer::MeshInstance> draws;
        for (const auto& mesh : meshes) {
            const auto count =
                mesh.surfaces ? mesh.surfaces->size() : (mesh.geometry ? mesh.geometry->materials().size() : 1);
            auto slots = std::make_shared<atelier::Array<std::uint32_t>>();
            for (std::size_t i = 0; i < count; ++i) {
                standard_fixture::SurfaceRef author;
                if (mesh.surfaces)
                    author = (*mesh.surfaces)[i];
                else {
                    standard_fixture::Description d;
                    d.parameters.base_color = {mesh.color.r, mesh.color.g, mesh.color.b, mesh.color.a};
                    d.parameters.roughness = mesh.material.roughness;
                    d.parameters.metallic = mesh.material.metallic;
                    d.parameters.emissive = mesh.material.emissive;
                    // Preserve capture identity through object reordering and
                    // deduplicate equal fixture values on the first submission.
                    const auto same = [&](const auto& item) { return item->describes(d); };
                    if (const auto found = std::ranges::find_if(old.authors, same); found != old.authors.end())
                        author = *found;
                    else if (const auto current = std::ranges::find_if(authors, same); current != authors.end())
                        author = *current;
                    else
                        author = std::make_shared<const standard_fixture::Surface>(std::move(d));
                }
                check(bool(author), "fixture has an explicit StandardPBR author for every slot");
                slots->add(static_cast<std::uint32_t>(surfaces.size()));
                surfaces.add(author->program());
                authors.add(std::move(author));
            }
            std::shared_ptr<const atelier::Array<std::uint32_t>> mapping = slots;
            if (draws.size() < old.meshes.size() && old.meshes[draws.size()].material_slots &&
                *old.meshes[draws.size()].material_slots == *slots)
                mapping = old.meshes[draws.size()].material_slots;
            draws.add({mesh.world, mesh.selected, mesh.geometry, mesh.stable_key, std::move(mapping)});
        }
        if (!std::ranges::equal(old.batch.surfaces(), surfaces)) {
            // Submit before retiring the old batch: the exact live code-set owner
            // is reused for MI/texture edits. Wait only at this acceptance boundary.
            auto next = take(renderer.prepare_materials(surfaces));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
            while (!take(renderer.preparation(next)).ready) {
                check(std::chrono::steady_clock::now() < deadline, "fixture material preparation deadline");
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            old.batch = std::move(next);
        }
        old.authors = std::move(authors);
        old.meshes = std::move(draws);
        native.meshes = old.meshes;
        native.materials = &old.batch;
        return native;
    }
};
}

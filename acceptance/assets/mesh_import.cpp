#include "memory/fatal.hpp"
#include "assets/mesh_import.hpp"
#include "assets/reader.hpp"
#include "json/json.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>

namespace {
using namespace atelier;

namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T, class E>
T take(std::expected<T, E> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

void write(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(bool(stream), "Could not write import acceptance fixture.");
}

template <class F>
void rejects(F&& action, std::string_view reason) {
    try {
        action();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(reason) != std::string_view::npos)
            return;
        throw;
    }
    throw std::runtime_error("Import accepted invalid input: " + std::string(reason));
}

template <class T>
const T& value(const assets::MaterialInstanceAsset& instance, std::string_view path) {
    const auto at = std::ranges::find(instance.entries, path, &material::Override::path);
    require(at != instance.entries.end() && at->enabled, "An imported author value is absent or disabled.");
    return std::get<T>(at->value);
}

void aligned(const assets::MeshImport& imported) {
    require(imported.mesh && imported.materials.size() == imported.mesh->materials().size() &&
                imported.textures.size() == imported.materials.size(),
            "Import author arrays do not align with geometry slots.");
    for (std::size_t slot = 0; slot < imported.materials.size(); ++slot) {
        if (!imported.materials[slot]) {
            require(std::ranges::all_of(imported.textures[slot], [](const auto& path) { return path.empty(); }),
                    "An unbound slot retained a texture source.");
            continue;
        }
        const auto& instance = *imported.materials[slot];
        std::set<std::string> paths;
        for (const auto& entry : instance.entries)
            require(paths.insert(entry.path).second, "Import produced duplicate parameter paths.");
        require(!paths.contains("double_sided") && !paths.contains("coverage") && !paths.contains("mask"),
                "MS policy leaked into MI overrides.");
        for (std::size_t input = 0; input < assets::imported_material_inputs.size(); ++input) {
            const auto& path = imported.textures[slot][input];
            if (path.empty())
                continue;
            require(std::ranges::find(imported.files, path, &assets::ImportedFile::relative) != imported.files.end(),
                    "Texture source is absent from the immutable import bundle.");
            const auto name = std::string(assets::imported_material_inputs[input]);
            require(value<material::TextureBinding>(instance, name + "_texture").asset_key.empty(),
                    "Import invented a persistent texture key before the Project transaction.");
            require(value<math::Vector2>(instance, name + "_uv.scale") == math::Vector2{1, 1} &&
                        value<math::Vector2>(instance, name + "_uv.offset") == math::Vector2{} &&
                        value<float>(instance, name + "_uv.rotation") == 0,
                    "Neutral sampling values lost their typed StandardPBR paths.");
        }
    }
}

std::string bmp(unsigned char red, unsigned char green, unsigned char blue) {
    std::string bytes(70, '\0');
    bytes[0] = 'B';
    bytes[1] = 'M';
    bytes[2] = 70;
    bytes[10] = 54;
    bytes[14] = 40;
    bytes[18] = 2;
    bytes[22] = 2;
    bytes[26] = 1;
    bytes[28] = 32;
    for (std::size_t i = 54; i < bytes.size(); i += 4) {
        bytes[i] = char(blue);
        bytes[i + 1] = char(green);
        bytes[i + 2] = char(red);
        bytes[i + 3] = char(255);
    }
    return bytes;
}

void obj(const fs::path& root) {
    write(root / "color.bmp", bmp(255, 128, 64));
    write(root / "opacity.bmp", bmp(128, 128, 128));
    write(root / "scene.mtl",
          "newmtl White\nKd 1 1 1\nd 1\nnewmtl Leaf\nKd .2 .3 .4\nTr .25\nmap_Kd color.bmp\nmap_d opacity.bmp\n");
    const std::string geometry_text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n";
    write(root / "scene.obj",
          "mtllib scene.mtl\n" + geometry_text + "f 1 2 3\nusemtl White\nf 1 2 3\nusemtl Leaf\nf 1/1 2/2 3/3\n");
    const auto imported = take(assets::load_obj(root / "scene.obj"));
    aligned(imported);
    require(imported.mesh->vertices().size() == 9 && imported.mesh->parts().size() == 3 &&
                imported.materials.size() == 3,
            "OBJ geometry or MTL slots changed during author-data separation.");
    require(!imported.materials[0] && geometry::material_slot_name(*imported.mesh, 0) == "slot/0" &&
                imported.mesh->material_slot(0) == 0 && imported.mesh->material_slot(1) == 1 &&
                imported.mesh->material_slot(2) == 2,
            "OBJ missing material or triangle-to-slot mapping was lost.");
    const auto& white = *imported.materials[1];
    const auto& leaf = *imported.materials[2];
    require(white.parent_key == "builtin/material/standard-opaque-double" &&
                value<math::Vector4>(white, "base_color") == math::Vector4{1, 1, 1, 1} &&
                value<float>(white, "roughness") == .65f && value<float>(white, "metallic") == 0,
            "Authored white MTL was treated as no material or lost explicit defaults.");
    require(leaf.parent_key == "builtin/material/standard-mask-double" &&
                value<math::Vector4>(leaf, "base_color") == math::Vector4{.2f, .3f, .4f, .75f} &&
                value<float>(leaf, "opacity_channel") == 0 && imported.textures[2][0] == "color.bmp" &&
                imported.textures[2][5] == "opacity.bmp",
            "MTL color, opacity, mask policy or texture paths were lost.");
    require(imported.mesh->vertices()[6].uv[1] == 1 && imported.mesh->edge_mask(2) == 7,
            "OBJ UV convention or polygon edge masks changed.");
    const auto original = take(assets::mesh_content_sha256({"mesh/one", L"first", imported.mesh, {}}));
    write(root / "scene.mtl",
          "newmtl White\nKd .8 .7 .6\nnewmtl Leaf\nKd .2 .3 .4\nmap_Kd color.bmp\nmap_d opacity.bmp\n");
    const auto edited = take(assets::load_obj(root / "scene.obj"));
    require(value<math::Vector4>(*edited.materials[1], "base_color").x == .8f &&
                take(assets::mesh_content_sha256({"mesh/two", L"second", edited.mesh, {}})) == original,
            "MI author edits still changed geometry content or identity leaked into its digest.");
    write(root / "bad.obj", "mtllib ../escape.mtl\n" + geometry_text + "f 1 2 3\n");
    rejects([&] { static_cast<void>(assets::load_obj(root / "bad.obj")); }, "inside the source directory");
    write(root / "bad.obj", "mtllib scene.mtl\n" + geometry_text + "usemtl Leaf\nf 1 2 3\n");
    rejects([&] { static_cast<void>(assets::load_obj(root / "bad.obj")); }, "requires UV");
    write(root / "scene.mtl", "newmtl White\nKd 2 1 1\nnewmtl Leaf\nKd 1 1 1\n");
    rejects([&] { static_cast<void>(assets::load_obj(root / "scene.obj")); }, "Invalid MTL base color");

    write(root / "scene.obj", "mtllib scene.mtl\n" + geometry_text + "usemtl White\nf 1 2 3\n");
    write(root / "scene.mtl", "newmtl White\nKd 1 1 1\nnewmtl 中🦊\nKd .2 .3 .4\n");
    const auto unused = take(assets::load_obj(root / "scene.obj"));
    require(unused.mesh->materials().size() == 3 && unused.mesh->material_slot(0) == 1 &&
                geometry::material_slot_name(*unused.mesh, 2) == "中🦊",
            "OBJ lost an unused Unicode material slot.");
    write(root / "scene.mtl", "newmtl White\nKd 1 1 1\nnewmtl \xFF\nKd .2 .3 .4\n");
    const auto invalid_name = assets::load_obj(root / "scene.obj");
    require(!invalid_name && invalid_name.error().code == ErrorCode::invalid_storage,
            "OBJ accepted invalid UTF-8 in an unused material slot.");
}

void gltf(const fs::path& root) {
    write(root / "color.bmp", bmp(255, 128, 64));
    write(root / "packed.bmp", bmp(0, 90, 200));
    write(root / "normal.bmp", bmp(128, 128, 255));
    std::string bin;
    const float positions[3][3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const float uv[3][2]{{0, 0}, {1, 0}, {0, 1}};
    for (std::size_t i = 0; i < 3; ++i)
        for (float v :
             {positions[i][0], positions[i][1], positions[i][2], 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, uv[i][0], uv[i][1]})
            bin.append(reinterpret_cast<const char*>(&v), sizeof(v));
    write(root / "scene.bin", bin);
    const auto& fixture = json::Document::literal<R"({
        "asset":{"version":"2.0"},"scenes":[{"nodes":[0,1]}],
        "nodes":[{"mesh":0},{"mesh":0,"scale":[-1,1,1]}],
        "meshes":[{"primitives":[
            {"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3},"material":0},
            {"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3},"material":1},
            {"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":3}},
            {"attributes":{"POSITION":0},"material":2}]}],
        "materials":[
            {"name":"white","doubleSided":true,"emissiveFactor":[0.1,0.2,0.3],
             "pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"roughnessFactor":1,"metallicFactor":0,
                "baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":1}},
             "normalTexture":{"index":2,"scale":0.25},"emissiveTexture":{"index":0}},
            {"name":"white","alphaMode":"MASK","alphaCutoff":0.37,
             "pbrMetallicRoughness":{"baseColorFactor":[0.3,0.4,0.5,0.8],"roughnessFactor":0.65,"metallicFactor":0.25}},
            {"name":"glass","alphaMode":"BLEND","pbrMetallicRoughness":{"baseColorTexture":{"index":3}}}],
        "textures":[{"source":0,"sampler":0},{"source":1,"sampler":1},{"source":2},{"source":3}],
        "samplers":[{"wrapS":33071,"wrapT":33071},{"wrapS":33648,"wrapT":33648}],
        "images":[{"uri":"color.bmp"},{"uri":"packed.bmp"},{"uri":"normal.bmp"},{"uri":"unread.bmp"}],
        "buffers":[{"uri":"scene.bin","byteLength":144}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":144,"byteStride":48}],
        "accessors":[
            {"bufferView":0,"byteOffset":0,"componentType":5126,"count":3,"type":"VEC3"},
            {"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},
            {"bufferView":0,"byteOffset":24,"componentType":5126,"count":3,"type":"VEC4"},
            {"bufferView":0,"byteOffset":40,"componentType":5126,"count":3,"type":"VEC2"}]})">();
    const auto fixture_bytes = json::write(fixture.root());
    write(root / "scene.gltf", {fixture_bytes.data(), fixture_bytes.size()});
    const auto imported = take(assets::load_gltf(root / "scene.gltf"));
    aligned(imported);
    require(imported.mesh->vertices().size() == 18 && imported.mesh->parts().size() == 6 &&
                imported.materials.size() == 3 && imported.mesh->materials()[1].name == "white (2)" &&
                imported.mesh->materials()[2].name == "default" && !imported.materials[2],
            "glTF lost named/implicit slots, node instances or the missing-author distinction.");
    require(imported.notes.size() == 1 && imported.notes[0].find("glass") != std::string::npos &&
                imported.notes[0].find("2 triangles") != std::string::npos && imported.files.size() == 5,
            "glTF BLEND omission diagnostics or unused-image filtering changed.");
    const auto& opaque = *imported.materials[0];
    const auto& mask = *imported.materials[1];
    require(opaque.parent_key == "builtin/material/standard-opaque-double" &&
                mask.parent_key == "builtin/material/standard-mask-single" &&
                value<math::Vector4>(opaque, "base_color") == math::Vector4{1, 1, 1, 1} &&
                value<float>(opaque, "metallic") == 0 && value<float>(opaque, "roughness") == 1 &&
                value<float>(mask, "alpha_cutoff") == .37f && value<float>(mask, "roughness") == .65f,
            "glTF policy or explicit default-valued factors were discarded.");
    require(value<float>(opaque, "roughness_channel") == 1 && value<float>(opaque, "metallic_channel") == 2 &&
                imported.textures[0][1] == "packed.bmp" && imported.textures[0][2] == "packed.bmp" &&
                imported.textures[0][0] == imported.textures[0][4],
            "glTF packed G/B channels or per-input image paths changed.");
    require(value<material::TextureBinding>(opaque, "base_color_texture").address == material::Address::clamp &&
                value<material::TextureBinding>(opaque, "roughness_texture").address == material::Address::mirror &&
                value<material::TextureBinding>(opaque, "normal_texture").address == material::Address::repeat &&
                value<float>(opaque, "normal_scale") == .25f && value<float>(opaque, "normal_y_sign") == 1 &&
                value<math::Vector3>(opaque, "emissive") == math::Vector3{.1f, .2f, .3f},
            "glTF typed samplers, normals or emission changed.");
    const auto file = std::ranges::find(imported.files, fs::path("packed.bmp"), &assets::ImportedFile::relative);
    const auto decoded = assets::decode_image(file->bytes);
    require(decoded->pixels()[1] == 90 && decoded->pixels()[2] == 200 && file->bytes == bmp(0, 90, 200),
            "Import baked a transfer function into packed data.");
    const auto vertices = imported.mesh->vertices();
    require(vertices[1].position[0] == -1 && vertices[10].position[0] == 1 && vertices[0].normal[2] == 1 &&
                vertices[0].tangent[0] == -1 && vertices[0].tangent[3] == -1 && vertices[9].tangent[0] == 1 &&
                vertices[9].tangent[3] == 1 && vertices[6].tangent[3] == 1 && vertices[15].tangent[3] == -1,
            "glTF mirror or authored/generated tangent handedness changed.");
    json::Builder variant{fixture};
    auto materials = take(variant.root().at("materials"));
    take(variant.set(take(materials.at(0)), "doubleSided", false));
    take(variant.set(take(materials.at(1)), "doubleSided", true));
    const auto variant_bytes = json::write(variant.root());
    write(root / "variant.gltf", {variant_bytes.data(), variant_bytes.size()});
    const auto sided = take(assets::load_gltf(root / "variant.gltf"));
    require(sided.materials[0]->parent_key == "builtin/material/standard-opaque-single" &&
                sided.materials[1]->parent_key == "builtin/material/standard-mask-double",
            "glTF did not select the other StandardPBR policy sources.");
    // Decoded duplicate keys keep the last value, including nested objects.
    std::string duplicate{fixture_bytes.data(), fixture_bytes.size()};
    duplicate.insert(1, R"("nodes":null,"scene":999,"sc\u0065ne":0.0,)");
    const auto asset = duplicate.find(R"("asset":{)") + std::string_view{R"("asset":{)"}.size();
    duplicate.insert(asset, R"("version":"unsupported",)");
    write(root / "variant.gltf", duplicate);
    const auto repeated = take(assets::load_gltf(root / "variant.gltf"));
    require(repeated.mesh->vertices().size() == vertices.size() &&
                repeated.materials.size() == imported.materials.size(),
            "glTF duplicate members did not keep their last decoded value.");
    variant = json::Builder{fixture};
    const auto scene_nodes = take(take(take(variant.root().at("scenes")).at(0)).at("nodes"));
    take(variant.set(take(scene_nodes.at(0)), 0.0));
    const auto real_index_bytes = json::write(variant.root());
    write(root / "variant.gltf", {real_index_bytes.data(), real_index_bytes.size()});
    require(take(assets::load_gltf(root / "variant.gltf")).mesh->vertices().size() == vertices.size(),
            "glTF rejected an integral real node index.");
    const auto reject = [&](const json::Builder& variant, std::string_view reason) {
        const auto bytes = json::write(variant.root());
        write(root / "variant.gltf", {bytes.data(), bytes.size()});
        const auto loaded = assets::load_gltf(root / "variant.gltf");
        require(!loaded && loaded.error().code == ErrorCode::invalid_storage &&
                    loaded.error().message.find(reason) != std::string::npos,
                "glTF rejection did not return the expected error.");
    };
    variant = json::Builder{fixture};
    take(variant.set(take(take(variant.root().at("buffers")).at(0)), "uri", "%2e%2e/outside.bin"));
    reject(variant, "inside the source directory");
    variant = json::Builder{fixture};
    take(variant.set(take(take(variant.root().at("accessors")).at(0)), "count", 100u));
    reject(variant, "exceeds its buffer");
    variant = json::Builder{fixture};
    take(variant.set(take(take(take(variant.root().at("materials")).at(0)).at("pbrMetallicRoughness")),
                     "roughnessFactor", 2u));
    reject(variant, "invalid material");
    variant = json::Builder{fixture};
    const auto primitive = take(take(take(take(variant.root().at("meshes")).at(0)).at("primitives")).at(0));
    variant.erase(take(primitive.at("attributes")), "TEXCOORD_0");
    reject(variant, "no TEXCOORD_0");
    write(root / "broken.bmp", "not an image");
    variant = json::Builder{fixture};
    take(variant.set(take(take(variant.root().at("images")).at(0)), "uri", "broken.bmp"));
    const auto broken_bytes = json::write(variant.root());
    write(root / "variant.gltf", {broken_bytes.data(), broken_bytes.size()});
    rejects([&] { static_cast<void>(assets::load_gltf(root / "variant.gltf")); }, "Cannot decode texture");
}

void digest() {
    require(assets::mesh_digest_version == 2, "Mesh-only digest schema must be version 2.");
    const auto cube = geometry::cube_mesh();
    std::vector<geometry::Vertex> vertices(cube->vertices().begin(), cube->vertices().end());
    const auto mesh = std::make_shared<const geometry::Mesh>(vertices, std::vector<std::uint8_t>{},
                                                             std::vector<geometry::MaterialSlot>{{"paint"}, {"trim"}},
                                                             std::vector<geometry::MeshPart>{{0, 18, 0}, {18, 18, 1}});
    assets::MeshAsset asset{"mesh/a", L"A", mesh, {{"paint", "material/a"}, {"trim", "material/b"}}};
    const auto initial = take(assets::mesh_content_sha256(asset));
    auto changed = asset;
    changed.key = "mesh/b";
    changed.name = L"B";
    std::ranges::reverse(changed.materials);
    require(take(assets::mesh_content_sha256(changed)) == initial,
            "Digest includes asset identity or binding storage order.");
    changed.materials[0].key = "material/c";
    require(take(assets::mesh_content_sha256(changed)) != initial, "Digest omitted named default bindings.");
    changed = asset;
    changed.materials.push_back(changed.materials.front());
    require(!assets::mesh_content_sha256(changed), "Digest accepted duplicate bindings.");
    changed = asset;
    changed.mesh = std::make_shared<const geometry::Mesh>(vertices, std::vector<std::uint8_t>(12, 3),
                                                          std::vector<geometry::MaterialSlot>{{"paint"}, {"trim"}},
                                                          std::vector<geometry::MeshPart>{{0, 18, 0}, {18, 18, 1}});
    require(take(assets::mesh_content_sha256(changed)) != initial, "Digest omitted edge masks.");
    changed = asset;
    changed.mesh = std::make_shared<const geometry::Mesh>(vertices, std::vector<std::uint8_t>{},
                                                          std::vector<geometry::MaterialSlot>{{"paint"}, {"trim"}},
                                                          std::vector<geometry::MeshPart>{{0, 18, 1}, {18, 18, 0}});
    require(take(assets::mesh_content_sha256(changed)) != initial, "Digest omitted triangle-to-slot relationships.");
    rejects([&] { geometry::Mesh invalid(vertices, {}, {{"only"}}, {{0, 36, 1}}); }, "valid materials");
}
}

int main() {
    atelier::install_fatal_handler();
    const auto root = fs::absolute("mesh-import-evidence/" +
                                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root / "obj");
        fs::create_directories(root / "gltf");
        obj(root / "obj");
        gltf(root / "gltf");
        digest();
        fs::remove_all(root);
        std::puts(
            "Mesh import acceptance: PASS (geometry-only slots, typed OBJ/glTF MI, paths, policy, tangents, digest v2)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mesh import acceptance: FAIL: %s\nEvidence: %s\n", error.what(), root.string().c_str());
        return 1;
    }
}

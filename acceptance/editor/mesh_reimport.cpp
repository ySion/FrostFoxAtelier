#include "memory/fatal.hpp"
#include "editor/project.hpp"
#include "assets/material_format.hpp"
#include "platform/utf.hpp"
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <map>

namespace {
using namespace atelier;

namespace fs = std::filesystem;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*value);
}

void write(const fs::path& path, std::string_view bytes) {
    take(editor::write_file(path, bytes));
}

std::string read(const fs::path& path) {
    return take(assets::read_file(path));
}

fs::path sidecar(fs::path path) {
    path += L".asset.json";
    return path;
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

void fixture(const fs::path& source, std::string_view filename, int width,
             std::initializer_list<std::string_view> slots, unsigned char red, unsigned char blue,
             bool extra_texture = false) {
    std::string mtl;
    std::string geometry =
        "mtllib model.mtl\nv 0 0 0\nv " + std::to_string(width) + " 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n";
    for (const auto slot : slots) {
        mtl += "newmtl " + std::string(slot) + (slot == "New.ms" ? "\nKd 1 1 1\n" : "\nKd .8 .7 .6\n") +
               "map_Kd color.bmp\n";
        if (extra_texture && slot == "Paint")
            mtl += "map_d unrelated.bmp\n";
        geometry += "usemtl " + std::string(slot) + "\nf 1/1 2/2 3/3\n";
    }
    write(source / platform::utf8_to_utf16_validated(filename), geometry);
    write(source / "model.mtl", mtl);
    write(source / "color.bmp", bmp(red, 64, blue));
    if (extra_texture)
        write(source / "unrelated.bmp", bmp(10, 20, 30));
}

std::string binding(const assets::MeshAsset& mesh, std::string_view slot) {
    const auto at = std::ranges::find(mesh.materials, slot, &assets::SlotMaterial::slot);
    require(at != mesh.materials.end(), "Named mesh default is missing.");
    return at->key;
}

material::Override& parameter(assets::MaterialAsset& asset, std::string_view path) {
    auto& entries = std::get<assets::MaterialInstanceAsset>(asset.author).entries;
    const auto at = std::ranges::find(entries, path, &material::Override::path);
    require(at != entries.end(), "Expected typed MI author parameter is missing.");
    return *at;
}

auto snapshot(const fs::path& root) {
    std::map<fs::path, std::pair<std::string, fs::file_time_type>> files;
    for (const auto& file : fs::recursive_directory_iterator(root))
        if (file.is_regular_file())
            files.emplace(file.path().lexically_relative(root), std::pair{read(file.path()), file.last_write_time()});
    return files;
}

void run(const fs::path& root) {
    const auto source = root / "source";
    fixture(source, "model.obj", 1, {"Paint", "Trim"}, 210, 30);
    write(source / "copy.bmp", read(source / "color.bmp"));
    auto initial_mtl = read(source / "model.mtl");
    initial_mtl.replace(initial_mtl.rfind("color.bmp"), 9, "copy.bmp");
    write(source / "model.mtl", initial_mtl);
    auto project = take(editor::Project::create(root / "project"));
    const auto key = take(project.import_mesh(source / "model.obj"));
    const auto first = take(project.assets().mesh(key));
    require(first.materials.size() == 2 && first.mesh->materials().size() == 3,
            "Import treated the unbound OBJ slot as an authored MI.");
    const auto paint_key = binding(first, "Paint");
    const auto trim_key = binding(first, "Trim");
    const auto mesh_relative = project.entry(key)->path;
    const auto mesh_file = take(project.resolve(mesh_relative));
    const auto mesh_identity = assets::json_format::parse(read(sidecar(mesh_file))).at("id");
    auto paint = take(project.assets().material(paint_key));
    parameter(paint, "roughness").value = .23f;
    parameter(paint, "base_color").value = math::Vector4{.12f, .34f, .56f, .78f};
    parameter(paint, "metallic").value = .87f;
    parameter(paint, "metallic").enabled = false;
    std::get<assets::MaterialInstanceAsset>(paint.author).parent_key = "builtin/material/standard-mask-single";
    const auto texture_key = std::get<material::TextureBinding>(parameter(paint, "base_color_texture").value).asset_key;
    take(project.save_material(paint));
    const auto paint_relative = project.entry(paint_key)->path;
    const auto paint_file = take(project.resolve(paint_relative));
    const auto painted_bytes = read(paint_file);
    const auto paint_identity = read(sidecar(paint_file));
    const auto texture_file = take(project.resolve(project.entry(texture_key)->path));
    const auto texture_identity = read(sidecar(texture_file));
    const auto old_image = take(project.assets().texture(texture_key)).image;
    auto trim_author = take(project.assets().material(trim_key));
    const auto copy_key =
        std::get<material::TextureBinding>(parameter(trim_author, "base_color_texture").value).asset_key;
    require(copy_key != texture_key && take(project.assets().texture(copy_key)).image == old_image,
            "Initial import lost shared pixels for distinct file identities with identical bytes.");
    {
        auto reopened = take(editor::Project::open(project.root()));
        const auto first_image = take(reopened.assets().texture(texture_key)).image;
        require(take(reopened.assets().texture(copy_key)).image == first_image,
                "Reopened identical image bytes stopped sharing pixels.");
    }
    const auto original_dependencies = take(project.assets().mesh_dependency_files(key));
    require(original_dependencies.size() == 3 &&
                std::ranges::find(original_dependencies, mesh_relative.parent_path() / "model.mtl") !=
                    original_dependencies.end() &&
                std::ranges::none_of(original_dependencies,
                                     [](const auto& path) {
                                         return path.extension() == L".material" ||
                                                path.filename().wstring().ends_with(L".asset.json");
                                     }),
            "Raw mesh companions omitted MTL/images or recursively included generated author assets.");
    const auto old_revision = project.revision();

    // The external source filename and source slot order both change. Neither is an asset identity.
    fixture(source, "updated.obj", 2, {"Trim", "Paint"}, 20, 220);
    auto update = take(project.reimport_mesh(key, source / "updated.obj"));
    require(update.mesh.key == key && project.entry(key)->path == mesh_relative && project.revision() > old_revision,
            "Reimport changed the mesh GUID/location or did not publish a new request.");
    require(update.mesh.mesh->bounds().maximum[0] == 2 && first.mesh->bounds().maximum[0] == 1 &&
                update.mesh.mesh->materials()[1].name == "Trim" && first.mesh->materials()[1].name == "Paint",
            "Reimport did not replace geometry and reorder slots without mutating the old snapshot.");
    require(binding(update.mesh, "Paint") == paint_key && binding(update.mesh, "Trim") == trim_key,
            "Reimport matched defaults by source ordinal instead of slot name.");
    require(read(paint_file) == painted_bytes && read(sidecar(paint_file)) == paint_identity &&
                read(sidecar(texture_file)) == texture_identity &&
                assets::json_format::parse(read(sidecar(mesh_file))).at("id") == mesh_identity,
            "Reimport overwrote manual MI parameters, policy, disabled values or persistent identities.");
    require(update.materials.size() == 2 && update.textures.size() == 1 && update.textures[0].key == texture_key &&
                update.textures[0].image->pixels()[2] == 220 &&
                take(project.assets().texture(texture_key)).image == update.textures[0].image &&
                old_image->pixels()[0] == 210 && old_image->pixels()[2] == 30 && !update.notes.empty(),
            "Reimport did not publish exact input pixels and readable preserved author assets.");
    auto returned = std::ranges::find(update.materials, paint_key, &assets::MaterialAsset::key);
    require(returned != update.materials.end() && std::get<float>(parameter(*returned, "roughness").value) == .23f &&
                !parameter(*returned, "metallic").enabled,
            "Returned MI data came from the new source instead of the saved author.");

    fixture(source, "updated.obj", 3, {"Trim", "Paint", "New.ms"}, 30, 200);
    auto added = take(project.reimport_mesh(key, source / "updated.obj"));
    const auto new_key = binding(added.mesh, "New.ms");
    auto new_material = take(project.assets().material(new_key));
    require(new_key != paint_key && new_key != trim_key &&
                !assets::is_material_source_path(project.entry(new_key)->path) &&
                std::get<math::Vector4>(parameter(new_material, "base_color").value) == math::Vector4{1, 1, 1, 1},
            "New authored white slot did not get an independent, correctly named MI.");
    const auto trim_file = take(project.resolve(project.entry(trim_key)->path));
    const auto trim_bytes = read(trim_file);
    const auto trim_identity = read(sidecar(trim_file));
    const auto renamed = take(project.rename(paint_relative, L"User Paint.material"));
    fixture(source, "updated.obj", 4, {"Paint", "New.ms"}, 40, 180);
    auto removed = take(project.reimport_mesh(key, source / "updated.obj"));
    require(removed.mesh.materials.size() == 2 && binding(removed.mesh, "Paint") == paint_key &&
                binding(removed.mesh, "New.ms") == new_key && project.entry(paint_key)->path == renamed &&
                read(take(project.resolve(renamed))) == painted_bytes && read(trim_file) == trim_bytes &&
                read(sidecar(trim_file)) == trim_identity,
            "Slot removal deleted old assets or a renamed material lost its persistent binding.");
    auto reopened = take(editor::Project::open(project.root()));
    const auto reloaded = take(reopened.assets().mesh(key));
    require(reloaded.mesh->bounds().maximum[0] == 4 && binding(reloaded, "Paint") == paint_key &&
                binding(reloaded, "New.ms") == new_key &&
                take(reopened.assets().texture(texture_key)).image->pixels()[0] == 40,
            "Reimport publication did not survive reopening the project.");

    // Real on-disk content is replaced with equal byte counts and restored timestamps.
    // Explicit reimport must still replace a Reader's already cached mesh/pixels.
    const auto mesh_stamp = fs::last_write_time(mesh_file);
    const auto texture_stamp = fs::last_write_time(texture_file);
    const auto cached = take(project.assets().mesh(key)).mesh;
    const auto cached_image = take(project.assets().texture(texture_key)).image;
    fixture(source, "updated.obj", 5, {"Paint", "New.ms"}, 50, 160);
    write(mesh_file, read(source / "updated.obj"));
    write(texture_file, read(source / "color.bmp"));
    fs::last_write_time(mesh_file, mesh_stamp);
    fs::last_write_time(texture_file, texture_stamp);
    auto same_stamp = take(project.reimport_mesh(key, source / "updated.obj"));
    require(same_stamp.mesh.mesh != cached && same_stamp.mesh.mesh->bounds().maximum[0] == 5 &&
                take(project.assets().texture(texture_key)).image != cached_image &&
                take(project.assets().texture(texture_key)).image->pixels()[0] == 50,
            "Explicit reimport was suppressed by identical file stamps.");

    const auto stable = snapshot(project.root());
    const auto revision = project.revision();
    write(source / "color.bmp", "corrupt image");
    (void)project.reimport_mesh(key, source / "updated.obj");
    require(snapshot(project.root()) == stable && project.revision() == revision,
            "A corrupt source texture changed committed project files or publication.");
    // Without an import ownership manifest, new dependencies cannot overwrite unrelated assets.
    const auto unrelated = mesh_file.parent_path() / "unrelated.bmp";
    write(unrelated, bmp(240, 230, 220));
    take(project.refresh());
    fixture(source, "updated.obj", 6, {"Paint", "New.ms"}, 60, 140, true);
    const auto before_collision = snapshot(project.root());
    const auto collision_revision = project.revision();
    (void)project.reimport_mesh(key, source / "updated.obj");
    require(snapshot(project.root()) == before_collision && project.revision() == collision_revision,
            "A new dependency silently overwrote an unrelated project asset.");

    // A real Windows sharing violation on the final metadata replacement forces
    // rollback after geometry, dependencies and a new MI have already been written.
    fixture(source, "updated.obj", 7, {"Paint", "New.ms", "Rollback"}, 70, 120);
    const auto before_rollback = snapshot(project.root());
    const auto rollback_revision = project.revision();
    const auto before_mesh = take(project.assets().mesh(key)).mesh;
    const auto metadata = sidecar(mesh_file);
    const auto locked = CreateFileW(metadata.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "Could not acquire the real metadata replacement lock.");
    (void)project.reimport_mesh(key, source / "updated.obj");
    CloseHandle(locked);
    require(
        snapshot(project.root()) == before_rollback && project.revision() == rollback_revision &&
            take(project.assets().mesh(key)).mesh == before_mesh,
        "A late write failure did not restore original bytes/timestamps, remove new outputs and retain Reader publication.");
    auto recovered = take(project.reimport_mesh(key, source / "updated.obj"));
    require(recovered.mesh.mesh->bounds().maximum[0] == 7 && !binding(recovered.mesh, "Rollback").empty(),
            "A new explicit request did not recover after the failed transaction.");

    // Corrupt current geometry can be repaired when dependencies are unchanged;
    // no guessed dependency ownership or previous-version restoration is needed.
    write(mesh_file, "corrupt OBJ");
    auto repaired = take(project.reimport_mesh(key, source / "updated.obj"));
    require(repaired.mesh.mesh->bounds().maximum[0] == 7 && repaired.mesh.key == key && !repaired.notes.empty(),
            "A fresh request could not repair invalid current geometry with verified unchanged dependencies.");

    // The new source stops sampling color, but preserved/user MIs still use it.
    // A touched existing texture must be returned even without a new active input.
    fixture(source, "updated.obj", 8, {"Paint", "New.ms", "Rollback"}, 80, 100);
    auto mtl = read(source / "model.mtl");
    for (auto at = mtl.find("map_Kd"); at != std::string::npos; at = mtl.find("map_Kd"))
        mtl.replace(at, 6, "bump");
    write(source / "model.mtl", mtl);
    const auto dormant = take(project.reimport_mesh(key, source / "updated.obj"));
    require(dormant.textures.size() == 1 && dormant.textures[0].key == texture_key &&
                dormant.textures[0].image->pixels()[0] == 80 && read(take(project.resolve(renamed))) == painted_bytes,
            "Reimport omitted a touched existing texture still referenced by a preserved or unsaved user MI.");
    const auto dependencies = take(project.assets().mesh_dependency_files(key));
    take(project.assets().publish_mesh_content(dormant.mesh));
    require(take(project.assets().mesh_dependency_files(key)) == dependencies,
            "Publishing the prepared snapshot discarded its verified raw companions.");
    fixture(source, "updated.obj", 9, {"Paint"}, 90, 90);
    const auto after_exact = take(project.reimport_mesh(key, source / "updated.obj"));
    require(after_exact.mesh.mesh->bounds().maximum[0] == 9 && after_exact.mesh.materials.size() == 1,
            "Explicit reimport could not supersede the current exact content/default snapshot.");

    // A Play-side Reader receives an earlier exact snapshot while disk already
    // contains newer geometry. Later catalogue discovery must not reread it.
    auto live = take(editor::Project::open(project.root()));
    const auto live_image = take(live.assets().texture(texture_key)).image;
    const auto before_receive = snapshot(project.root());
    const auto receive_revision = live.revision();
    take(live.assets().publish_mesh_content(first));
    require(snapshot(project.root()) == before_receive && live.revision() == receive_revision + 1 &&
                take(live.assets().mesh(key)).mesh == first.mesh &&
                take(live.assets().texture(texture_key)).image == live_image && live.entry(key)->path == mesh_relative,
            "Exact mesh publication read/wrote source content, changed identity, or changed texture pixels.");
    const auto accepted_revision = live.revision();
    auto invalid = first;
    invalid.materials.push_back(invalid.materials.front());
    (void)live.assets().publish_mesh_content(invalid);
    require(live.revision() == accepted_revision && take(live.assets().mesh(key)).mesh == first.mesh,
            "Invalid exact mesh publication changed the accepted snapshot.");
    write(mesh_file, "newer invalid disk geometry");
    auto newer_metadata = assets::json_format::parse(read(sidecar(mesh_file)));
    newer_metadata["materials"] = {{"missing slot", paint_key}};
    write(sidecar(mesh_file), newer_metadata.dump());
    write(project.root() / "Content/Other.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    take(live.refresh());
    const auto exact_again = take(live.assets().mesh(key));
    require(
        exact_again.mesh == first.mesh && binding(exact_again, "Trim") == trim_key &&
            take(live.assets().mesh_dependency_files(key)).empty(),
        "Catalogue discovery replaced received geometry/defaults or inferred raw dependencies from newer disk content.");
}

void gltf(const fs::path& root) {
    const auto source = root / "gltf";
    auto document = assets::json_format::parse(R"({
        "asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
        "buffers":[{"uri":"geometry.bin","byteLength":36}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],
        "materials":[{"name":"Body"},{"name":"Trim","alphaMode":"MASK","doubleSided":false}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0},{"attributes":{"POSITION":0},"material":1}]}]
    })");
    const auto geometry = [&](float width) {
        const float positions[]{0, 0, 0, width, 0, 0, 0, 1, 0};
        write(source / "geometry.bin", std::string_view(reinterpret_cast<const char*>(positions), sizeof(positions)));
    };
    geometry(1);
    write(source / "model.gltf", document.dump());
    auto project = take(editor::Project::create(root / "gltf-project"));
    const auto key = take(project.import_mesh(source / "model.gltf"));
    const auto first = take(project.assets().mesh(key));
    const auto companions = take(project.assets().mesh_dependency_files(key));
    require(companions.size() == 1 && companions[0].filename() == L"geometry.bin",
            "Raw glTF companion query omitted the external buffer.");
    const auto body = binding(first, "Body");
    const auto trim = binding(first, "Trim");
    auto author = take(project.assets().material(trim));
    require(std::get<assets::MaterialInstanceAsset>(author.author).parent_key ==
                "builtin/material/standard-mask-single",
            "glTF import lost the authored single-sided Mask policy.");
    parameter(author, "roughness").value = .19f;
    take(project.save_material(author));
    const auto saved = read(take(project.resolve(project.entry(trim)->path)));
    std::swap(document["materials"][0], document["materials"][1]);
    geometry(3);
    write(source / "model.gltf", document.dump());
    const auto updated = take(project.reimport_mesh(key, source / "model.gltf"));
    // glTF +X becomes engine -X; both bounds distinguish replaced from stale geometry.
    require(updated.mesh.mesh->bounds().minimum[0] == -3 && updated.mesh.mesh->bounds().maximum[0] == 0 &&
                updated.mesh.mesh->materials()[0].name == "Trim" && binding(updated.mesh, "Body") == body &&
                binding(updated.mesh, "Trim") == trim &&
                read(take(project.resolve(project.entry(trim)->path))) == saved,
            "glTF external buffer replacement or material identity preservation failed.");
    const auto stable = snapshot(project.root());
    write(source / "geometry.bin", "truncated");
    static_cast<void>(project.reimport_mesh(key, source / "model.gltf"));
    require(snapshot(project.root()) == stable, "A truncated glTF buffer changed committed files.");
    auto reopened = take(editor::Project::open(project.root()));
    const auto bounds = take(reopened.assets().mesh(key)).mesh->bounds();
    require(bounds.minimum[0] == -3 && bounds.maximum[0] == 0, "Updated glTF buffer was not persisted.");
}
}

int main() {
    atelier::install_fatal_handler();
    const auto root = fs::absolute("mesh-reimport-evidence/" +
                                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        run(root);
        gltf(root);
        fs::remove_all(root);
        std::puts(
            "Mesh reimport acceptance: PASS (stable keys, named slots, preserved MI authors, exact pixels, reopen and rollback)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mesh reimport acceptance: FAIL: %s\nEvidence: %s\n", error.what(), root.string().c_str());
        return 1;
    }
}

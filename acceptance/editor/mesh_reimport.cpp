#include "../helpers.hpp"
#include "editor/project.hpp"
#include "assets/material_format.hpp"
#include <chrono>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void fixture(const std::filesystem::path& source, int width, bool reversed, unsigned char red, unsigned char blue) {
    const auto geometry =
        "mtllib model.mtl\nv 0 0 0\nv " + std::to_string(width) + " 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n";
    const auto faces = reversed ? "usemtl Trim\nf 1/1 2/2 3/3\nusemtl Paint\nf 1/1 2/2 3/3\n"
                                : "usemtl Paint\nf 1/1 2/2 3/3\nusemtl Trim\nf 1/1 2/2 3/3\n";
    take(editor::write_file(source / "model.obj", geometry + faces));
    take(editor::write_file(
        source / "model.mtl",
        "newmtl Paint\nKd .8 .7 .6\nmap_Kd color.bmp\nnewmtl Trim\nKd .8 .7 .6\nmap_Kd color.bmp\n"));
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
        bytes[i + 1] = 64;
        bytes[i + 2] = char(red);
        bytes[i + 3] = char(255);
    }
    take(editor::write_file(source / "color.bmp", bytes));
}

std::string binding(const assets::MeshAsset& mesh, std::string_view slot) {
    const auto at = std::ranges::find(mesh.materials, slot, &assets::SlotMaterial::slot);
    require(at != mesh.materials.end(), "Named mesh default is missing.");
    return at->key;
}
}

void verify_mesh_reimport() {
    const auto root = std::filesystem::current_path() / "mesh-reimport-evidence" /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto source = root / "source";
    fixture(source, 1, false, 210, 30);
    auto project = take(editor::Project::create(root / "project"));
    const auto key = take(project.import_mesh(source / "model.obj"));
    const auto first = take(project.assets().mesh(key));
    const auto paint_key = binding(first, "Paint");
    const auto trim_key = binding(first, "Trim");
    auto paint = take(project.assets().material(paint_key));
    auto& entries = std::get<assets::MaterialInstanceAsset>(paint.author).entries;
    const auto roughness = std::ranges::find(entries, "roughness", &material::Override::path);
    require(roughness != entries.end(), "Material roughness is absent.");
    roughness->value = .23f;
    take(project.save_material(paint));
    fixture(source, 2, true, 20, 220);
    const auto update = take(project.reimport_mesh(key, source / "model.obj"));
    require(update.mesh.key == key && update.mesh.mesh->bounds().maximum[0] == 2 &&
                first.mesh->bounds().maximum[0] == 1 && binding(update.mesh, "Paint") == paint_key &&
                binding(update.mesh, "Trim") == trim_key,
            "Reimport lost identity, slot bindings or old geometry.");
    require(update.textures.size() == 1 && update.textures.front().image->pixels()[0] == 20 &&
                update.textures.front().image->pixels()[2] == 220,
            "Reimport did not publish the new input pixels.");
    const auto restored = take(project.assets().material(paint_key));
    const auto& restored_entries = std::get<assets::MaterialInstanceAsset>(restored.author).entries;
    const auto value = std::ranges::find(restored_entries, "roughness", &material::Override::path);
    require(value != restored_entries.end() && std::get<float>(value->value) == .23f,
            "Reimport overwrote authored roughness.");
}

#include "../helpers.hpp"
#include "files.hpp"
#include "editor/project.hpp"
#include "assets/material_format.hpp"
#include "assets/file.hpp"
#include "foundation/map.hpp"
#include <Windows.h>
#include <chrono>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void fixture(const std::filesystem::path& source, int width, bool reversed, unsigned char red, unsigned char blue) {
    const auto geometry =
        String::concat("mtllib model.mtl\nv 0 0 0\nv ", width, " 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n");
    const StringView faces = reversed ? "usemtl Trim\nf 1/1 2/2 3/3\nusemtl Paint\nf 1/1 2/2 3/3\n"
                                      : "usemtl Paint\nf 1/1 2/2 3/3\nusemtl Trim\nf 1/1 2/2 3/3\n";
    take(editor::write_file(source / "model.obj", String::concat(geometry, String::validated(faces))));
    take(editor::write_file(
        source / "model.mtl",
        "newmtl Paint\nKd .8 .7 .6\nmap_Kd color.bmp\nnewmtl Trim\nKd .8 .7 .6\nmap_Kd color.bmp\n"));
    Array<std::byte> bytes(70);
    bytes[0] = std::byte{'B'};
    bytes[1] = std::byte{'M'};
    bytes[2] = std::byte{70};
    bytes[10] = std::byte{54};
    bytes[14] = std::byte{40};
    bytes[18] = std::byte{2};
    bytes[22] = std::byte{2};
    bytes[26] = std::byte{1};
    bytes[28] = std::byte{32};
    for (std::size_t i = 54; i < bytes.size(); i += 4) {
        bytes[i] = static_cast<std::byte>(blue);
        bytes[i + 1] = std::byte{64};
        bytes[i + 2] = static_cast<std::byte>(red);
        bytes[i + 3] = std::byte{255};
    }
    take(editor::write_file(source / "color.bmp", bytes));
}

String binding(const assets::MeshAsset& mesh, StringView slot) {
    const auto at = std::ranges::find(mesh.materials, slot, &assets::SlotMaterial::slot);
    require(at != mesh.materials.end(), "Named mesh default is missing.");
    return at->key;
}

// Include empty directories as well as every file's exact bytes and modification time.
auto content_snapshot(const std::filesystem::path& root) {
    using File = std::pair<Array<std::byte>, std::filesystem::file_time_type>;
    Map<std::filesystem::path, Optional<File>> result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(root / "Content", error);
    std::filesystem::recursive_directory_iterator end;
    while (!error && entry != end) {
        auto& file = *result.try_emplace(entry->path().lexically_relative(root)).value;
        const auto status = entry->status(error);
        if (error)
            break;
        if (!std::filesystem::is_directory(status)) {
            const auto modified = entry->last_write_time(error);
            if (error)
                break;
            file.emplace(take(assets::read_file(entry->path())), modified);
        }
        entry.increment(error);
    }
    require(!error, "Cannot snapshot the import destination for comparison.");
    return result;
}

void verify_failed_imports(editor::Project& project, const std::filesystem::path& source,
                           const assets::MeshAsset& previous) {
    const auto disk = content_snapshot(project.root());
    const Array<assets::ProjectEntry> directory{project.entries()};
    const auto revision = project.revision();
    const auto path = project.entry(previous.key)->path;
    const auto texture_path = path.parent_path() / "color.bmp";
    const auto listed = std::ranges::find(directory, texture_path, &assets::ProjectEntry::path);
    require(listed != directory.end(), "Imported bundle did not list its texture.");
    const auto texture = take(project.assets().texture(listed->key));
    const auto preserved = [&] {
        require(content_snapshot(project.root()) == disk, "Rejected import changed files, timestamps or directories.");
        require(project.revision() == revision && std::ranges::equal(project.entries(), directory) &&
                    project.entry(previous.key) && project.entry(previous.key)->path == path &&
                    take(project.assets().mesh(previous.key)).mesh == previous.mesh &&
                    take(project.assets().texture(texture.key)).image == texture.image,
                "Rejected import changed asset identities, the published catalogue or cached content.");
    };

    // An initial import over an existing bundle must fail during preparation.
    const auto collision = project.import_mesh(source / "model.obj");
    require(!collision && collision.error().message.contains("would overwrite"),
            "Import did not reject an output collision.");
    preserved();

    // Permit reads for the backup, then deny the atomic replacement of the texture after the OBJ was replaced.
    const auto locked =
        CreateFileW((project.root() / texture_path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "Could not lock the imported texture for the write rejection.");
    const auto denied = project.reimport_mesh(previous.key, source / "model.obj");
    CloseHandle(locked);
    require(!denied && denied.error().code == ErrorCode::invalid_storage &&
                denied.error().message.starts_with("Cannot save ") &&
                denied.error().message.contains("color.bmp (Windows error "),
            "The failed reimport did not return the original texture replacement error.");
    preserved();

    // WIC decodes these BMP bytes, while the project's catalogue classifies the unregistered suffix as a plain file.
    // The final Reader therefore rejects the prepared texture after the files have been written.
    const auto other = source.parent_path() / "unlisted-texture";
    const auto model_obj_bytes = take(assets::read_file(source / "model.obj"));
    take(editor::write_file(other / "other.obj", model_obj_bytes));
    auto mtl = take(assets::read_utf8_file(source / "model.mtl"));
    static_cast<void>(mtl.replace_all("color.bmp", "nested/color.payload"));
    take(editor::write_file(other / "model.mtl", mtl));
    const auto color_bmp_bytes = take(assets::read_file(source / "color.bmp"));
    take(editor::write_file(other / "nested/color.payload", color_bmp_bytes));
    const auto check_publication = [&](const auto& rejected) {
        require(!rejected && rejected.error().code == ErrorCode::invalid_storage &&
                    rejected.error().message.starts_with("Missing resource or incorrect resource type: project/"),
                "The rejected bundle lost its Reader publication error.");
        preserved();
    };
    check_publication(project.import_mesh(other / "other.obj"));
    check_publication(project.reimport_mesh(previous.key, other / "other.obj"));
}

Result<void> verify_catalog_refresh(editor::Project& project, const assets::MeshAsset& published) {
    const auto revision = project.revision();
    const Array<assets::ProjectEntry> before{project.entries()};
    const auto original = project.entry(published.key)->path;
    const std::filesystem::path relative = L"Content/CatalogCandidate.obj";
    const auto candidate = project.root() / relative;
    auto metadata = candidate;
    metadata += L".asset.json";
    take(editor::write_file(candidate, "v 0 0 0\nv 3 0 0\nv 0 1 0\nf 1 2 3\n"));
    const auto preserved = [&](StringView diagnostic) {
        const auto refreshed = project.refresh();
        const auto opened = editor::Project::open(project.root());
        require(!refreshed && refreshed.error().message.contains(diagnostic) && !opened &&
                    opened.error().code == refreshed.error().code &&
                    opened.error().message == refreshed.error().message,
                "Open and Refresh did not report the same rejected catalogue.");
        require(project.revision() == revision && std::ranges::equal(project.entries(), before) &&
                    project.entry(published.key) && project.entry(published.key)->path == original &&
                    take(project.assets().mesh(published.key)).mesh == published.mesh,
                "A failed refresh changed the published entries, key index, revision or cached geometry.");
    };
    const std::filesystem::path source = L"Source/Discovered.source";
    take(editor::write_file(project.root() / source, "float discovered() { return 1.0f; }"));
    const auto sources = take(project.source_paths());
    require(std::ranges::find(sources, source) != sources.end() && !project.entry("Source/Discovered.source"),
            "Source discovery must see external files before the next catalogue publication.");
    take(editor::write_file(metadata, "{"));
    preserved("CatalogCandidate.obj.asset.json");
    auto original_metadata = project.root() / original;
    original_metadata += L".asset.json";
    const auto orig_meta_bytes = take(assets::read_file(original_metadata));
    take(editor::write_file(metadata, orig_meta_bytes));
    preserved(published.key);

    ATELIER_TRY(acceptance::remove_file(metadata));
    take(project.refresh());
    const auto found = std::ranges::find(project.entries(), relative, &assets::ProjectEntry::path);
    require(found != project.entries().end() && found->key != published.key && project.revision() == revision + 1,
            "Repairing the identity did not publish a single new catalogue.");
    const auto new_key = found->key;
    const auto loaded = take(project.assets().mesh(new_key));
    require(loaded.mesh->bounds().maximum[0] == 3 && take(project.assets().mesh(published.key)).mesh == published.mesh,
            "Recovered catalogue did not resolve the new mesh or retain the unchanged one.");
    take(project.refresh());
    auto reopened = take(editor::Project::open(project.root()));
    require(project.revision() == revision + 1 && reopened.entry(new_key) && reopened.entry(new_key)->path == relative,
            "An unchanged refresh advanced the revision or reopening lost the generated identity.");
    return {};
}
}

atelier::Result<void> verify_mesh_reimport() {
    ATELIER_TRY_BIND(directory,
                     acceptance::file_operation([](auto& error) { return std::filesystem::current_path(error); }));
    const auto root = *directory / "mesh-reimport-evidence" /
                      std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
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
    verify_failed_imports(project, source, first);
    const auto update = take(project.reimport_mesh(key, source / "model.obj"));
    require(update.mesh.key == key && update.mesh.mesh->bounds().maximum[0] == 2 &&
                first.mesh->bounds().maximum[0] == 1 && binding(update.mesh, "Paint") == paint_key &&
                binding(update.mesh, "Trim") == trim_key,
            "Reimport lost identity, slot bindings or old geometry.");
    const auto reimported_px =
        update.textures.empty() ? nullptr : update.textures.front().image->acquire_pixels();
    require(update.textures.size() == 1 && reimported_px && (*reimported_px)[0] == 20 && (*reimported_px)[2] == 220,
            "Reimport did not publish the new input pixels.");
    const auto restored = take(project.assets().material(paint_key));
    const auto& restored_entries = std::get<assets::MaterialInstanceAsset>(restored.author).entries;
    const auto value = std::ranges::find(restored_entries, "roughness", &material::Override::path);
    require(value != restored_entries.end() && std::get<float>(value->value) == .23f,
            "Reimport overwrote authored roughness.");
    return verify_catalog_refresh(project, update.mesh);
}

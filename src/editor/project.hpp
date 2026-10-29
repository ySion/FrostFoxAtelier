#pragma once
#include "runtime/runtime.hpp"
#include "math/math.hpp"
#include "assets/reader.hpp"
#include <filesystem>

namespace atelier::editor {
struct MeshImportPublication {
    assets::MeshAsset mesh;
    std::vector<assets::MaterialAsset> materials; // New MIs and readable preserved slot defaults.
    std::vector<assets::TextureAsset> textures; // Exact pixels for inputs and touched existing texture assets.
    std::vector<std::string> notes;
};
// The folder is the project. Config/Project.json identifies it; all persisted
// locations are relative to this root. Cache never owns authoritative content.
class Project final {
public:
    static Result<Project> create(const std::filesystem::path&);
    static Result<Project> open(const std::filesystem::path&);
    static bool is_scene_path(const std::filesystem::path& path) {
        return !path.empty() && path.extension()==L".scene" &&
            *path.begin()==L"Content";
    }
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::filesystem::path& startup_scene() const noexcept { return startup_; }
    const std::filesystem::path& ui_source() const noexcept { return ui_source_; }
    const std::wstring& name() const noexcept { return name_; }
    assets::Reader& assets() noexcept { return assets_; }
    const assets::Reader& assets() const noexcept { return assets_; }
    std::span<const assets::ProjectEntry> entries() const noexcept { return assets_.entries(); }
    const assets::ProjectEntry* entry(std::string_view key) const { return assets_.entry(key); }
    // The top-level folders the catalogue scans: Content, Source, Config.
    std::span<const std::filesystem::path> roots() const noexcept { return roots_; }
    std::uint64_t revision() const noexcept { return assets_.revision(); }
    Result<void> refresh();
    Result<std::filesystem::path> resolve(const std::filesystem::path&) const;
    // Authored OBJ/glTF slots publish named MI assets; unbound slots use the engine grid.
    Result<std::string> import_mesh(const std::filesystem::path&);
    // Explicit same-format replacement at the mesh's current project location.
    // Same named slots retain their defaults and existing material files unchanged.
    // New authored slots create MIs; dependencies at existing bundle paths keep
    // their GUIDs. Removed slot defaults disappear, but their files remain.
    // No import history, source-value merge or rename inference. Unverified changed
    // dependencies/collisions fail; unchanged files may be reused without ownership.
    Result<MeshImportPublication> reimport_mesh(std::string_view key,const std::filesystem::path& source);
    Result<std::string> import_texture(const std::filesystem::path&);
    // Explicit author operation: decode before replacing the same asset file.
    // The identity sidecar is unchanged; returned pixels are this exact import.
    Result<assets::TextureAsset> reimport_texture(std::string_view key,const std::filesystem::path& source);
    Result<std::string> create_material(const std::filesystem::path&,const assets::MaterialAsset& = {});
    // Creates the MS and its sibling .source file together; empty text uses a
    // small editable shader. Material Source files stay outside gameplay Source.
    Result<std::string> create_material_source(const std::filesystem::path&,std::string_view text = {});
    Result<void> save_material(const assets::MaterialAsset&);
    // Fresh, deterministic compiler inputs. UI declarations have their own
    // configured entry and are not parsed as gameplay Source functions.
    Result<std::vector<std::filesystem::path>> source_paths() const;
    Result<void> create_source(const std::filesystem::path& relative,std::string_view);
    Result<void> set_ui_source(const std::filesystem::path& relative);
    // Rename a project file or folder in place. Mesh/material/texture keys come from
    // their sidecar identity, which moves with them, so scene references survive.
    // The startup scene, UI source and top-level folders are not renamed here.
    Result<std::filesystem::path> rename(const std::filesystem::path& relative, std::wstring_view name);
    Result<std::filesystem::path> cook_collision(std::string_view mesh_key, math::Vector3 scale);
    // Cooked collision for the mesh at this scale; a missing or stale cache entry is rebuilt and written back.
    Result<std::shared_ptr<const std::string>> collision(std::string_view mesh_key, math::Vector3 scale);
private:
    Result<std::vector<assets::ProjectEntry>> scan_entries(bool create_identities) const;
    Result<MeshImportPublication> import_mesh_files(const std::filesystem::path&,std::string_view existing_key);
    std::filesystem::path root_, startup_, ui_source_;
    std::wstring name_;
    std::vector<std::filesystem::path> roots_;
    assets::Reader assets_;
};
// Same-directory temporary + flush + replacement. Failed writes leave the old
// file intact. Strings and on-disk paths use validated UTF-8.
enum class WriteMode { replace, create };
Result<void> write_file(const std::filesystem::path&, std::string_view, WriteMode = WriteMode::replace);
}

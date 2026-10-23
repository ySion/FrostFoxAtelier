#pragma once
#include "runtime/runtime.hpp"
#include "math/math.hpp"
#include "editor/assets.hpp"
#include "editor/mesh_import.hpp"
#include <filesystem>
#include <unordered_map>

namespace atelier::editor {
struct TextureAsset {std::string key;std::wstring name;material::ImageRef image;};
struct MaterialAsset {
    std::string key;
    std::wstring name;
    material::Description description;
    std::array<std::string,material::input_count> textures;
};
struct ProjectEntry {
    std::filesystem::path path; // Relative to the project root; never a Runtime ID.
    std::string key;
    AssetKind kind{};
    std::uintmax_t bytes{};
    std::filesystem::file_time_type modified{};
    std::vector<SlotMaterial> materials{}; // A mesh sidecar's default material per slot, by slot name.
    bool operator==(const ProjectEntry&) const = default;
};
// The folder is the project. Config/Project.json identifies it; all persisted
// locations are relative to this root. Cache never owns authoritative content.
class Project final {
public:
    static Result<Project> create(const std::filesystem::path&);
    static Result<Project> open(const std::filesystem::path&);
    static bool is_texture_path(const std::filesystem::path&);
    static bool is_sky_path(const std::filesystem::path&);
    static bool is_scene_path(const std::filesystem::path& path) {
        return !path.empty() && path.extension()==L".scene" &&
            (*path.begin()==L"Content" || *path.begin()==L"Examples");
    }
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::filesystem::path& startup_scene() const noexcept { return startup_; }
    const std::filesystem::path& ui_source() const noexcept { return ui_source_; }
    const std::wstring& name() const noexcept { return name_; }
    std::span<const ProjectEntry> entries() const noexcept { return entries_; }
    const ProjectEntry* entry(std::string_view key) const;
    std::uint64_t revision() const noexcept { return revision_; }
    Result<void> refresh();
    Result<std::filesystem::path> resolve(const std::filesystem::path&) const;
    Result<MeshAsset> mesh(std::string_view);
    // glTF imports also publish one material asset per slot, which the mesh names as its slot defaults.
    Result<std::string> import_mesh(const std::filesystem::path&);
    // Source content the loaded mesh left out (see MeshImport::notes); empty until mesh() loads it.
    std::span<const std::string> mesh_notes(std::string_view key) const;
    Result<TextureAsset> texture(std::string_view);
    // A Radiance .hdr sky asset; the same key returns the same map until refresh sees the file change.
    Result<SkyAsset> sky_map(std::string_view);
    Result<MaterialAsset> material(std::string_view);
    Result<std::string> import_texture(const std::filesystem::path&);
    Result<std::string> create_material(const std::filesystem::path&,const MaterialAsset& = {});
    Result<void> save_material(const MaterialAsset&);
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
    std::filesystem::path root_, startup_, ui_source_;
    std::wstring name_;
    std::vector<ProjectEntry> entries_;
    std::uint64_t revision_{};
    struct KeyHash {
        using is_transparent=void;
        std::size_t operator()(std::string_view key) const noexcept {return std::hash<std::string_view>{}(key);}
    };
    std::unordered_map<std::string,std::size_t,KeyHash,std::equal_to<>> entry_index_; // Key to entries_ position.
    std::unordered_map<std::string,material::ImageRef> textures_;
    std::unordered_map<std::string,rendering::SkyMapRef> sky_maps_;
    struct CachedMesh { geometry::MeshRef mesh; std::vector<std::filesystem::path> dependencies; std::vector<std::string> notes; };
    std::unordered_map<std::string,CachedMesh> meshes_;
    ImageCache images_;
};
// Same-directory temporary + flush + replacement. Failed writes leave the old
// file intact. Strings and on-disk paths use validated UTF-8.
Result<std::string> read_file(const std::filesystem::path&, std::size_t maximum=16*1024*1024);
enum class WriteMode { replace, create };
Result<void> write_file(const std::filesystem::path&, std::string_view, WriteMode = WriteMode::replace);
std::string utf8(std::wstring_view);
std::wstring wide(std::string_view);
}

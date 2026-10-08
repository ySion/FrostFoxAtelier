#pragma once
#include "runtime/runtime.hpp"
#include "math/math.hpp"
#include "editor/assets.hpp"
#include <filesystem>
#include <unordered_map>

namespace atelier::editor {
enum class AssetKind { folder, mesh, scene, source, file };
struct ProjectEntry {
    std::filesystem::path path; // Relative to the project root; never a Runtime ID.
    std::string key;
    AssetKind kind{};
    std::uintmax_t bytes{};
    std::filesystem::file_time_type modified{};
    bool operator==(const ProjectEntry&) const = default;
};
// The folder is the project. Config/Project.json identifies it; all persisted
// locations are relative to this root. Cache never owns authoritative content.
class Project final {
public:
    static Result<Project> create(const std::filesystem::path&);
    static Result<Project> open(const std::filesystem::path&);
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::filesystem::path& startup_scene() const noexcept { return startup_; }
    const std::filesystem::path& ui_source() const noexcept { return ui_source_; }
    const std::wstring& name() const noexcept { return name_; }
    std::span<const ProjectEntry> entries() const noexcept { return entries_; }
    std::uint64_t revision() const noexcept { return revision_; }
    Result<void> refresh();
    Result<std::filesystem::path> resolve(const std::filesystem::path&) const;
    Result<MeshAsset> mesh(std::string_view);
    Result<std::string> import_mesh(const std::filesystem::path&);
    // Fresh, deterministic compiler inputs. UI declarations have their own
    // configured entry and are not parsed as gameplay Source functions.
    Result<std::vector<std::filesystem::path>> source_paths() const;
    Result<void> create_source(const std::filesystem::path& relative,std::string_view);
    Result<void> set_ui_source(const std::filesystem::path& relative);
    Result<std::filesystem::path> cook_collision(std::string_view mesh_key, math::Vector3 scale);
    Result<std::shared_ptr<const std::string>> collision(std::string_view mesh_key, math::Vector3 scale);
private:
    std::filesystem::path root_, startup_, ui_source_;
    std::wstring name_;
    std::vector<ProjectEntry> entries_;
    std::uint64_t revision_{};
    std::unordered_map<std::string,std::size_t> mesh_entries_;
    struct CachedMesh { geometry::MeshRef mesh; std::vector<std::filesystem::path> dependencies; };
    std::unordered_map<std::string,CachedMesh> meshes_;
};
// Same-directory temporary + flush + replacement. Failed writes leave the old
// file intact. Strings and on-disk paths use validated UTF-8.
Result<std::string> read_file(const std::filesystem::path&, std::size_t maximum=16*1024*1024);
enum class WriteMode { replace, create };
Result<void> write_file(const std::filesystem::path&, std::string_view, WriteMode = WriteMode::replace);
std::string utf8(std::wstring_view);
std::wstring wide(std::string_view);
}

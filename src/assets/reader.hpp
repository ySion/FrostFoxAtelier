#pragma once
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include "assets/mesh_import.hpp"

namespace atelier::assets {
// Synchronous asset reads, independent of author documents, Runtime objects and devices.
// The default reader has only the engine catalogue. A project owner supplies a root
// and complete scanned entries; the reader never creates sidecars or writes files.
class Reader final {
public:
    Reader() = default;
    // The publishing owner supplies the canonical project root (not a current-directory fallback).
    explicit Reader(std::filesystem::path root) : root_(std::move(root)) {}
    std::span<const ProjectEntry> entries() const noexcept {return entries_;}
    const ProjectEntry* entry(std::string_view key) const;
    std::uint64_t revision() const noexcept {return revision_;}
    // Publishes the complete candidate, invalidating caches by the existing file stamps.
    // Entries have already passed the owner's path, kind and sidecar validation.
    // Borrowed entries/notes expire on publication; returned content keeps its own owners.
    Result<void> publish(std::vector<ProjectEntry>);
    Result<MeshAsset> mesh(std::string_view);
    Result<TextureAsset> texture(std::string_view);
    Result<MaterialAsset> material(std::string_view);
    Result<SkyAsset> sky_map(std::string_view);
    std::span<const std::string> mesh_notes(std::string_view key) const;
    // Project-relative dependency files, including identities. Preserves the existing
    // tolerant copying contract: missing/wrong-kind referenced assets have no files,
    // unreadable assets keep their own files, and builtin prefixes have no files.
    Result<std::vector<std::filesystem::path>> asset_files(std::string_view key);
    // Author import preparation uses the same weak image cache as normal reads.
    // The author owns file writes/rollback, retaining the prepared mesh until refresh
    // publishes its directory entry, or forgetting it if the import fails.
    MeshImport prepare_mesh(const std::filesystem::path& source);
    void retain_import(std::string key,const std::filesystem::path& relative,MeshImport&);
    void forget_import(std::string_view key);
private:
    Result<std::filesystem::path> resolve(const std::filesystem::path& relative) const;
    std::filesystem::path root_;
    std::vector<ProjectEntry> entries_;
    std::uint64_t revision_{};
    struct KeyHash {
        using is_transparent=void;
        std::size_t operator()(std::string_view key) const noexcept {return std::hash<std::string_view>{}(key);}
    };
    std::unordered_map<std::string,std::size_t,KeyHash,std::equal_to<>> entry_index_;
    std::unordered_map<std::string,material::ImageRef> textures_;
    std::unordered_map<std::string,rendering::SkyMapRef> sky_maps_;
    struct CachedMesh {geometry::MeshRef mesh;std::vector<std::filesystem::path> dependencies;std::vector<std::string> notes;};
    std::unordered_map<std::string,CachedMesh> meshes_;
    ImageCache images_;
};
}

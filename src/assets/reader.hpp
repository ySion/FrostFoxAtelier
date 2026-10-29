#pragma once
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include "assets/mesh_import.hpp"
#include "source/snapshot.hpp"

namespace atelier::assets {
// Cold-path, bounded-memory canonical encoding of actual content and slot defaults.
// Invalid content returns an error; SHA provider failures throw std::runtime_error.
Result<std::string> mesh_content_sha256(const MeshAsset&);
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
    // Explicit import/reimport publication, after the owner has written the bundle.
    // Validates on a private candidate, then publishes catalogue, prepared geometry
    // and exact input pixels together. Failure leaves this Reader unchanged.
    // Disk-derived dependency caches invalidate even when file stamps match.
    Result<MeshAsset> publish_mesh(std::vector<ProjectEntry>,std::string_view key,
        const MeshImport&,std::span<const TextureAsset>);
    // Exact received content for an existing mesh key. No disk reads/writes,
    // import, normalization or texture changes. Catalogue refresh retains this
    // current geometry/default snapshot while the key remains a mesh; an explicit
    // mesh publication replaces it. Failure leaves this Reader unchanged.
    Result<void> publish_mesh_content(const MeshAsset&);
    Result<MeshAsset> mesh(std::string_view);
    // Exact installed identity and kind, never a project fallback. SHA failures throw.
    Result<EngineDependency> engine_dependency(std::string_view,AssetKind expected) const;
    Result<TextureAsset> texture(std::string_view);
    // Author has replaced this existing file after decoding. Publish its exact
    // pixels and entry, even when a file stamp happens to equal the previous one.
    Result<void> publish_texture(ProjectEntry,material::ImageRef);
    // Reads v2 author data only. The file suffix must agree with its MS/MI role.
    Result<MaterialAsset> material(std::string_view);
    // Exact contained project path, including Content/, or reserved builtin/ Source.
    // No game-Source scan or compilation; callers retain this saved-text snapshot.
    Result<source::SourceFile> source(const std::filesystem::path& project_relative) const;
    Result<SkyAsset> sky_map(std::string_view);
    std::span<const std::string> mesh_notes(std::string_view key) const;
    // Owning project-relative raw companions (MTL, bin, source images), excluding
    // the mesh file/sidecars and never following MS/MI author references. A cold
    // disk-derived mesh is prepared; received exact content never rereads disk.
    Result<std::vector<std::filesystem::path>> mesh_dependency_files(std::string_view key);
    // Candidate preparation shares the Reader's weak image cache. It neither
    // publishes geometry nor writes files; the owner commits through publish_mesh.
    MeshImport prepare_mesh(const std::filesystem::path& source);
    std::vector<material::ImageRef> prepare_images(std::span<const std::string_view>,std::uint64_t pixel_budget);
private:
    void retain_import(std::string key,const std::filesystem::path& relative,MeshImport&);
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
    struct CachedMesh {
        geometry::MeshRef mesh;
        std::vector<std::filesystem::path> dependencies;
        std::vector<std::string> notes;
        bool exact_content{}; // Received publication, not a disk-derived cache entry.
    };
    std::unordered_map<std::string,CachedMesh> meshes_;
    ImageCache images_;
};
}

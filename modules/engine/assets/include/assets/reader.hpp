#pragma once
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include "assets/mesh_import.hpp"
#include "foundation/map.hpp"
#include "source/snapshot.hpp"

namespace atelier::assets {
// Cold-path, bounded-memory canonical encoding of actual content and slot defaults.
// Invalid content returns an error; memory SHA primitive failures are fatal.
Result<String> mesh_content_sha256(const MeshAsset&);

// Synchronous asset reads, independent of author documents, Runtime objects and devices.
// The default reader has only the engine catalogue. A project owner supplies a root
// and complete scanned entries; the reader never creates sidecars or writes files.
class Reader final {
public:
    Reader() = default;

    // The publishing owner supplies the canonical project root (not a current-directory fallback).
    explicit Reader(std::filesystem::path root) : root_(std::move(root)) {}

    Span<const ProjectEntry> entries() const noexcept { return entries_; }

    const ProjectEntry* entry(StringView key) const;

    const ProjectEntry* entry(const String& key) const { return entry(key.view()); }

    std::uint64_t revision() const noexcept { return revision_; }

    // Publishes the complete candidate, invalidating caches by the existing file stamps.
    // Entries have already passed the owner's path, kind and sidecar validation.
    // Borrowed entries/notes expire on publication; returned content keeps its own owners.
    Result<void> publish(Array<ProjectEntry>);

    // Explicit import/reimport publication, after the owner has written the bundle.
    // Consumes a private copy of this Reader used for preparation, then publishes catalogue, geometry
    // and exact input pixels together. Failure leaves this Reader unchanged.
    // Disk-derived dependency caches invalidate even when file stamps match.
    Result<MeshAsset> publish_mesh(Reader candidate, Array<ProjectEntry>, StringView key, const MeshImport&,
                                   Span<const TextureAsset>);

    // Exact received content for an existing mesh key. No disk reads/writes,
    // import, normalization or texture changes. Catalogue refresh retains this
    // current geometry/default snapshot while the key remains a mesh; an explicit
    // mesh publication replaces it. Failure leaves this Reader unchanged.
    Result<void> publish_mesh_content(const MeshAsset&);

    Result<MeshAsset> mesh(StringView);

    Result<MeshAsset> mesh(const String& key) { return mesh(key.view()); }

    // Exact installed identity and kind, never a project fallback. SHA failures are fatal.
    Result<EngineDependency> engine_dependency(StringView, AssetKind expected) const;

    Result<TextureAsset> texture(StringView);

    Result<TextureAsset> texture(const String& key) { return texture(key.view()); }

    // Author has replaced this existing file after decoding. Publish its exact
    // pixels and entry, even when a file stamp happens to equal the previous one.
    Result<void> publish_texture(ProjectEntry, material::ImageRef);

    // Reads v2 author data only. The file suffix must agree with its MS/MI role.
    Result<MaterialAsset> material(StringView);

    Result<MaterialAsset> material(const String& key) { return material(key.view()); }

    // Reads UTF-8 procedural graph JSON text for a listed AssetKind::procedural_graph entry.
    Result<ProceduralGraphAsset> procedural_graph(StringView) const;

    Result<ProceduralGraphAsset> procedural_graph(const String& key) const { return procedural_graph(key.view()); }

    // Exact contained project path, including Content/, or reserved builtin/ Source.
    // No game-Source scan or compilation; callers retain this saved-text snapshot.
    Result<source::SourceFile> source(const std::filesystem::path& project_relative) const;

    Result<SkyAsset> sky_map(StringView);

    Result<SkyAsset> sky_map(const String& key) { return sky_map(key.view()); }

    Span<const String> mesh_notes(StringView key) const;

    // Owning project-relative raw companions (MTL, bin, source images), excluding
    // the mesh file/sidecars and never following MS/MI author references. A cold
    // disk-derived mesh is prepared; received exact content never rereads disk.
    Result<Array<std::filesystem::path>> mesh_dependency_files(StringView key);

    // Candidate preparation shares the Reader's weak image cache. It neither
    // publishes geometry nor writes files; the owner commits through publish_mesh.
    Result<MeshImport> prepare_mesh(const std::filesystem::path& source);

    Result<Array<material::ImageRef>> prepare_images(Span<const Span<const std::byte>>, std::uint64_t pixel_budget);

private:
    struct CachedMesh;

    CachedMesh& retain_import(String key, const std::filesystem::path& relative, MeshImport&);

    Result<std::filesystem::path> resolve(const std::filesystem::path& relative) const;

    std::filesystem::path root_;

    Array<ProjectEntry> entries_;

    std::uint64_t revision_{};

    Map<String, std::size_t> entry_index_;

    Map<String, material::ImageRef> textures_;

    Map<String, rendering::SkyMapRef> sky_maps_;

    struct CachedMesh {
        geometry::MeshRef mesh;

        Array<std::filesystem::path> dependencies;

        Array<String> notes;

        bool exact_content{}; // Received publication, not a disk-derived cache entry.
    };

    Map<String, CachedMesh> meshes_;

    ImageCache images_;
};
}

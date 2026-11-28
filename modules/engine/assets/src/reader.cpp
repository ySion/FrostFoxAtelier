#include "assets/reader.hpp"
#include "assets/builtin_materials.hpp"
#include "assets/material_format.hpp"
#include "foundation/set.hpp"
#include "platform/utf.hpp"
#include <Windows.h>
#include <cwctype>

namespace atelier::assets {
namespace {
auto failure(Utf8Text text) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(text)});
}

String safe_text(StringView text) {
    return String::from_utf8(text).value_or("<invalid utf-8>"_s);
}

Result<const ProjectEntry*> listed(const Reader& reader, StringView key, AssetKind kind) {
    const auto* at = reader.entry(key);
    if (!at || at->kind != kind)
        return failure(String::concat("Missing resource or incorrect resource type: ", safe_text(key)));
    return at;
}

bool is_source_path(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    for (auto& c : extension)
        c = static_cast<wchar_t>(std::towlower(c));
    return extension == L".source";
}
}

bool is_texture_path(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    for (auto& c : extension)
        c = static_cast<wchar_t>(std::towlower(c));
    return extension == L".png" || extension == L".jpg" || extension == L".jpeg" || extension == L".bmp" ||
           extension == L".tif" || extension == L".tiff";
}

bool is_sky_path(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    for (auto& c : extension)
        c = static_cast<wchar_t>(std::towlower(c));
    return extension == L".hdr";
}

bool is_material_source_path(const std::filesystem::path& path) {
    auto filename = path.filename().wstring();
    for (auto& c : filename)
        c = static_cast<wchar_t>(std::towlower(c));
    return filename.ends_with(L".ms.material");
}

bool is_procedural_graph_path(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    for (auto& c : extension)
        c = static_cast<wchar_t>(std::towlower(c));
    return extension == L".procgraph";
}

Result<std::filesystem::path> Reader::resolve(const std::filesystem::path& relative) const {
    return resolve_project_path(root_, relative);
}

Result<void> Reader::publish(Array<ProjectEntry> entries) {
    // A received current snapshot must not turn into a read of newer disk
    // content when Live refreshes the catalogue merely to discover another key.
    for (auto& item : entries)
        if (item.kind == AssetKind::mesh) {
            if (const auto* cached = meshes_.find(item.key); cached && cached->exact_content)
                if (const auto* previous = entry(item.key))
                    item.materials = previous->materials;
        }
    if (entries == entries_)
        return {};
    if (root_.empty())
        return failure("Publishing project assets requires a project root.");
    decltype(entry_index_) index;
    index.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& key = entries[i].key;
        if (key.starts_with("builtin/"))
            return failure(String::concat("Reserved project resource identity: ", key));
        const auto [previous, inserted] = index.try_emplace(key, i);
        if (!inserted) {
            const auto first = platform::utf16_to_utf8(entries[*previous].path.generic_wstring());
            const auto second = platform::utf16_to_utf8(entries[i].path.generic_wstring());
            if (!first || !second)
                return failure(String::concat("Duplicate project resource identity: ", key));
            return failure(
                String::concat("Duplicate project resource identity ", key, " in ", *first, " and ", *second, "."));
        }
    }
    const auto next_entry = [&](StringView key) -> const ProjectEntry* {
        const auto* at = index.find(key);
        return at ? &entries[*at] : nullptr;
    };
    Map<std::filesystem::path, const ProjectEntry*> previous_files;
    Map<std::filesystem::path, const ProjectEntry*> next_files;
    const auto has_mesh_dependencies = [&] {
        for (const auto mesh : meshes_)
            if (!mesh.value.exact_content && !mesh.value.dependencies.empty())
                return true;
        return false;
    };
    if (has_mesh_dependencies()) {
        previous_files.reserve(entries_.size());
        for (const auto& file : entries_)
            previous_files.try_emplace(file.path, &file);
        next_files.reserve(entries.size());
        for (const auto& file : entries)
            next_files.try_emplace(file.path, &file);
    }
    const auto file_changed = [&](StringView key) {
        const auto* before = entry(key);
        const auto* after = next_entry(key);
        return !before || !after || before->kind != after->kind || before->bytes != after->bytes ||
               before->modified != after->modified;
    };
    textures_.remove_if([&](StringView key, const auto&) { return file_changed(key); });
    sky_maps_.remove_if([&](StringView key, const auto&) { return file_changed(key); });
    meshes_.remove_if([&](StringView key, const CachedMesh& cached) {
        const auto* next = next_entry(key);
        if (!next || next->kind != AssetKind::mesh)
            return true;
        if (cached.exact_content)
            return false;
        const auto* old = entry(key);
        if (old->bytes != next->bytes || old->modified != next->modified)
            return true;
        for (const auto& path : cached.dependencies) {
            const auto* a = previous_files.find(path);
            const auto* b = next_files.find(path);
            if (!a || !b || (*a)->bytes != (*b)->bytes || (*a)->modified != (*b)->modified)
                return true;
        }
        return false;
    });
    images_.entries.remove_if([](const String&, const auto& entry) { return entry.expired(); });
    entries_ = std::move(entries);
    entry_index_ = std::move(index);
    ++revision_;
    return {};
}

Result<MeshAsset> Reader::publish_mesh(Reader candidate, Array<ProjectEntry> entries, StringView key,
                                       const MeshImport& loaded, Span<const TextureAsset> textures) {
    if (!loaded.mesh)
        return failure("Mesh publication requires prepared geometry.");
    candidate.meshes_.remove(key); // Explicit reimport supersedes received content/defaults.
    ATELIER_TRY(candidate.publish(std::move(entries)));
    ATELIER_TRY_BIND(found, listed(candidate, key, AssetKind::mesh));
    const auto& item = **found;
    Array<std::filesystem::path> changed;
    CachedMesh cached{loaded.mesh, {}, loaded.notes};
    for (const auto& file : loaded.files) {
        const auto path = item.path.parent_path() / file.relative;
        ATELIER_TRY(candidate.resolve(path));
        changed.add(path);
        if (file.relative != item.path.filename())
            cached.dependencies.add(path);
    }
    const auto touched = [&](const std::filesystem::path& path) {
        const auto text = path.lexically_normal().generic_wstring();
        return std::ranges::any_of(changed, [&](const auto& other) {
            const auto named = other.lexically_normal().generic_wstring();
            return CompareStringOrdinal(text.c_str(), -1, named.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    };
    if (!touched(item.path))
        return failure("Prepared mesh bundle does not contain its published source file.");
    // Explicit author replacement must work even if size and timestamp match.
    for (const auto [cached_key, image] : candidate.textures_) {
        (void)image;
        ATELIER_TRY(listed(candidate, cached_key, AssetKind::texture));
    }
    candidate.textures_.remove_if(
        [&](StringView cached_key, const auto&) { return touched(candidate.entry(cached_key)->path); });
    for (const auto [cached_key, map] : candidate.sky_maps_) {
        (void)map;
        ATELIER_TRY(listed(candidate, cached_key, AssetKind::sky));
    }
    candidate.sky_maps_.remove_if(
        [&](StringView cached_key, const auto&) { return touched(candidate.entry(cached_key)->path); });
    for (const auto [cached_key, mesh] : candidate.meshes_)
        if (!mesh.exact_content)
            ATELIER_TRY(listed(candidate, cached_key, AssetKind::mesh));
    candidate.meshes_.remove_if([&](StringView cached_key, const CachedMesh& mesh) {
        if (mesh.exact_content)
            return false;
        return touched(candidate.entry(cached_key)->path) || std::ranges::any_of(mesh.dependencies, touched);
    });
    for (const auto& texture : textures) {
        ATELIER_TRY_BIND(entry, listed(candidate, texture.key, AssetKind::texture));
        if (!texture.image || !touched((*entry)->path))
            return failure("Published texture must be decoded content of this mesh bundle.");
        candidate.textures_.insert_or_assign(texture.key, texture.image);
    }
    candidate.meshes_.insert_or_assign(item.key, std::move(cached));
    ATELIER_TRY_BIND(result, candidate.mesh(key));
    candidate.revision_ = revision_ + 1;
    static_assert(std::is_nothrow_move_assignable_v<Reader>);
    *this = std::move(candidate);
    return result;
}

Result<void> Reader::publish_mesh_content(const MeshAsset& asset) {
    ATELIER_TRY(listed(*this, asset.key, AssetKind::mesh));
    if (!asset.mesh)
        return failure("Exact mesh publication requires resolved geometry.");
    Reader candidate = *this;
    auto& item = candidate.entries_[candidate.entry_index_.at(asset.key)];
    item.materials = asset.materials;
    CachedMesh cached{asset.mesh, {}, {}, true};
    if (const auto* old = candidate.meshes_.find(asset.key); old && old->mesh == asset.mesh) {
        cached.dependencies = old->dependencies;
        cached.notes = old->notes;
    }
    candidate.meshes_.insert_or_assign(asset.key, std::move(cached));
    // Same named-slot validation as normal reads and file-transaction publication.
    ATELIER_TRY(candidate.mesh(asset.key));
    ++candidate.revision_;
    static_assert(std::is_nothrow_move_assignable_v<Reader>);
    *this = std::move(candidate);
    return {};
}

const ProjectEntry* Reader::entry(StringView key) const {
    const auto* at = entry_index_.find(key);
    return at ? &entries_[*at] : nullptr;
}

Result<MeshImport> Reader::prepare_mesh(const std::filesystem::path& source) {
    return load_mesh(source, &images_);
}

Result<Array<material::ImageRef>> Reader::prepare_images(Span<const Span<const std::byte>> sources,
                                                         std::uint64_t pixel_budget) {
    return decode_images(sources, &images_, pixel_budget);
}

Reader::CachedMesh& Reader::retain_import(String key, const std::filesystem::path& relative, MeshImport& loaded) {
    CachedMesh cached{loaded.mesh, {}, std::move(loaded.notes)};
    const auto base_dir = relative.parent_path();
    for (const auto& file : loaded.files)
        if (file.relative != relative.filename())
            cached.dependencies.add(base_dir / file.relative);
    if (!loaded.decoded_textures.empty()) {
        Array<std::pair<std::wstring, StringView>> uncached_textures;
        for (const auto& entry : entries_) {
            if (entry.kind == AssetKind::texture && !textures_.contains(entry.key))
                uncached_textures.add({entry.path.lexically_normal().generic_wstring(), entry.key.view()});
        }
        if (!uncached_textures.empty()) {
            for (const auto& dt : loaded.decoded_textures) {
                if (!dt.image)
                    continue;
                const auto dep_norm = (base_dir / dt.relative).lexically_normal().generic_wstring();
                for (auto& [ent_norm, ent_key] : uncached_textures) {
                    if (ent_key.empty())
                        continue;
                    if (CompareStringOrdinal(dep_norm.c_str(), -1, ent_norm.c_str(), -1, TRUE) == CSTR_EQUAL) {
                        textures_.try_emplace(String::from_utf8_validated(ent_key), dt.image);
                        ent_key = {};
                        break;
                    }
                }
            }
        }
    }
    return *meshes_.try_emplace(std::move(key), std::move(cached)).value;
}

Result<MeshAsset> Reader::mesh(StringView key) {
    for (const auto& builtin : mesh_assets())
        if (builtin.key == key)
            return builtin;
    const auto* item = entry(key);
    if (!item || item->kind != AssetKind::mesh)
        return failure(String::concat("Missing mesh resource: ", safe_text(key)));
    const auto& entry = *item;
    auto* found_mesh = meshes_.find(entry.key);
    if (!found_mesh) {
        ATELIER_TRY_BIND(path, resolve(entry.path));
        ATELIER_TRY_BIND(loaded, prepare_mesh(*path));
        found_mesh = &retain_import(entry.key, entry.path, *loaded);
    }
    // In slot order, so every consumer lists the defaults as the mesh lists its slots.
    const auto& mesh = *found_mesh->mesh;
    Array<std::pair<std::size_t, SlotMaterial>> ordered;
    Set<StringView> bound;
    bound.reserve(entry.materials.size());
    for (const auto& binding : entry.materials) {
        std::size_t matches{};
        std::size_t index{};
        for (std::size_t i = 0; i < mesh.materials().size(); ++i)
            if (geometry::material_slot_name(mesh, i) == binding.slot) {
                ++matches;
                index = i;
            }
        if (matches != 1)
            return failure(
                String::concat("Mesh metadata names a default material for a missing or ambiguous slot: ",
                               binding.slot));
        if (!bound.add(binding.slot))
            return failure(String::concat("Mesh metadata repeats a default material slot: ", binding.slot));
        ordered.emplace(index, binding);
    }
    std::ranges::sort(ordered, {}, &std::pair<std::size_t, SlotMaterial>::first);
    MeshAsset result{entry.key, entry.path.stem().wstring(), found_mesh->mesh};
    for (auto& [index, binding] : ordered)
        result.materials.add(std::move(binding));
    return result;
}

Span<const String> Reader::mesh_notes(StringView key) const {
    const auto* found = meshes_.find(key);
    return found ? Span<const String>(found->notes) : Span<const String>{};
}

Result<Array<std::filesystem::path>> Reader::mesh_dependency_files(StringView key) {
    for (const auto& builtin : mesh_assets())
        if (builtin.key == key)
            return Array<std::filesystem::path>{};
    ATELIER_TRY_BIND(found, listed(*this, key, AssetKind::mesh));
    const auto& item = **found;
    auto* cached = meshes_.find(item.key);
    if (!cached) {
        ATELIER_TRY_BIND(path, resolve(item.path));
        ATELIER_TRY_BIND(loaded, prepare_mesh(*path));
        cached = &retain_import(item.key, item.path, *loaded);
    }
    return cached->dependencies;
}

Result<SkyAsset> Reader::sky_map(StringView key) {
    const auto* at = entry(key);
    if (!at)
        return failure(String::concat("Sky map key is not a Radiance .hdr asset of this project: ", safe_text(key)));
    const auto relative = platform::utf16_to_utf8(at->path.generic_wstring());
    if (!relative)
        return failure("Invalid UTF-16 text.");
    if (at->kind != AssetKind::sky)
        return failure(String::concat("Sky map key names ", *relative, ", which is not a Radiance .hdr asset."));
    SkyAsset asset{at->key, at->path.stem().wstring(), {}};
    if (const auto* cached = sky_maps_.find(at->key)) {
        asset.map = *cached;
        return asset;
    }
    auto path = resolve(at->path);
    auto bytes =
        path ? read_file(*path, std::size_t(256) << 20) : Result<Array<std::byte>>{std::unexpected(path.error())};
    if (!bytes)
        return failure(String::concat("Sky map ", *relative, " could not be read: ", bytes.error().message));
    auto decoded = rendering::decode_radiance_hdr(*bytes);
    if (!decoded)
        return failure(
            String::concat("Sky map ", *relative, " is not a valid Radiance HDR file: ", decoded.error().message));
    asset.map = std::move(*decoded);
    sky_maps_.try_emplace(at->key, asset.map);
    return asset;
}

Result<TextureAsset> Reader::texture(StringView key) {
    ATELIER_TRY_BIND(found, listed(*this, key, AssetKind::texture));
    const auto& item = **found;
    const auto* at = textures_.find(item.key);
    if (!at) {
        // Through the image cache: a mesh import that decoded the same bytes shares its pixels.
        ATELIER_TRY_BIND(path, resolve(item.path));
        ATELIER_TRY_BIND(bytes, read_file(*path, 128 * 1024 * 1024));
        const std::array<Span<const std::byte>, 1> source{*bytes};
        ATELIER_TRY_BIND(decoded, decode_images(source, &images_));
        at = textures_.try_emplace(item.key, decoded->front()).value;
    }
    return TextureAsset{item.key, item.path.stem().wstring(), *at};
}

Result<void> Reader::publish_texture(ProjectEntry item, material::ImageRef image) {
    const auto* at = entry_index_.find(item.key);
    if (!at || !image || item.kind != AssetKind::texture || entries_[*at].kind != AssetKind::texture ||
        entries_[*at].path != item.path)
        return failure("Texture publication must replace an existing texture at its current location.");
    const auto slot = *at;
    textures_.insert_or_assign(item.key, std::move(image));
    // A mesh may have decoded the same file into its imported default Surface.
    meshes_.remove_if([&](const String&, const CachedMesh& cached) {
        return !cached.exact_content &&
               std::ranges::find(cached.dependencies, item.path) != cached.dependencies.end();
    });
    entries_[slot] = std::move(item);
    ++revision_;
    return {};
}

Result<MaterialAsset> Reader::material(StringView key) {
    for (const auto& builtin : material_assets())
        if (builtin.key == key)
            return builtin;
    ATELIER_TRY_BIND(found, listed(*this, key, AssetKind::material));
    const auto& item = **found;
    auto extension = item.path.extension().wstring();
    for (auto& c : extension)
        c = static_cast<wchar_t>(std::towlower(c));
    if (extension != L".material")
        return failure("Material assets require a .material file.");
    ATELIER_TRY_BIND(path, resolve(item.path));
    ATELIER_TRY_BIND(bytes, read_file(*path));
    const auto document = json_format::parse(*bytes);
    if (!document)
        return failure(document.error().describe());
    auto decoded = material_format::decode(document->root());
    if (!decoded)
        return failure(decoded.error().describe());
    auto asset = std::move(*decoded);
    const bool is_source = is_material_source_path(item.path);
    if (is_source != std::holds_alternative<MaterialSourceAsset>(asset.author))
        return failure(
            "Material role does not match its path: MS requires .ms.material; MI requires .material without .ms.");
    asset.key = item.key;
    asset.name = (is_source ? item.path.stem().stem() : item.path.stem()).wstring();
    return asset;
}

Result<ProceduralGraphAsset> Reader::procedural_graph(StringView key) const {
    ATELIER_TRY_BIND(found, listed(*this, key, AssetKind::procedural_graph));
    const auto& item = **found;
    if (!is_procedural_graph_path(item.path))
        return failure("Procedural graph assets require a .procgraph file.");
    ATELIER_TRY_BIND(path, resolve(item.path));
    ATELIER_TRY_BIND(bytes, read_file(*path, 16 * 1024 * 1024));
    auto text = String::from_utf8(as_chars(*bytes));
    if (!text)
        return failure(String::concat("Invalid UTF-8 text in procedural graph: ", safe_text(key)));
    return ProceduralGraphAsset{item.key, item.path.stem().wstring(), std::move(*text)};
}

Result<source::SourceFile> Reader::source(const std::filesystem::path& project_relative) const {
    const auto named = platform::utf16_to_utf8(project_relative.generic_wstring());
    if (!named)
        return failure("Invalid UTF-16 text.");
    if (named->starts_with("builtin/")) {
        auto builtin = builtin_material_source(*named);
        if (!builtin)
            return failure(String::concat("Unknown installed material Source: ", *named));
        return std::move(*builtin);
    }
    if (root_.empty())
        return failure("Reading project Source requires a project root.");
    if (!is_source_path(project_relative))
        return failure("Source requires an explicit project-relative .source path.");
    ATELIER_TRY_BIND(resolved, resolve(project_relative));
    auto normalized = platform::utf16_to_utf8(project_relative.lexically_normal().generic_wstring());
    if (!normalized)
        return failure("Invalid UTF-16 text.");
    return source::read_source_file(*resolved, std::move(*normalized));
}
}

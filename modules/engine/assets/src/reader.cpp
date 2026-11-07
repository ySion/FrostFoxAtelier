#include "assets/reader.hpp"
#include "assets/builtin_materials.hpp"
#include "assets/material_format.hpp"
#include "platform/utf.hpp"
#include <Windows.h>
#include <cwctype>
#include <map>
#include <set>

namespace atelier::assets {
namespace {
auto failure(std::string text) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(text)});
}

template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*value);
}

const ProjectEntry& listed(const Reader& reader, std::string_view key, AssetKind kind) {
    const auto* at = reader.entry(key);
    if (!at || at->kind != kind)
        throw std::runtime_error("Missing resource or incorrect resource type: " + std::string(key));
    return *at;
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

Result<std::filesystem::path> Reader::resolve(const std::filesystem::path& relative) const {
    return resolve_project_path(root_, relative);
}

Result<void> Reader::publish(std::vector<ProjectEntry> entries) {
    try {
        // A received current snapshot must not turn into a read of newer disk
        // content when Live refreshes the catalogue merely to discover another key.
        for (auto& item : entries)
            if (item.kind == AssetKind::mesh) {
                const auto cached = meshes_.find(item.key);
                if (cached != meshes_.end() && cached->second.exact_content)
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
            if (entries[i].key.starts_with("builtin/") || !index.emplace(entries[i].key, i).second)
                return failure("Duplicate or reserved resource identity in project catalogue.");
        }
        const auto find = [](const std::vector<ProjectEntry>& list, const decltype(entry_index_)& positions,
                             std::string_view key) -> const ProjectEntry* {
            const auto at = positions.find(key);
            return at == positions.end() ? nullptr : &list[at->second];
        };
        std::map<std::filesystem::path, const ProjectEntry*> previous_files;
        std::map<std::filesystem::path, const ProjectEntry*> next_files;
        for (const auto& entry : entries_)
            previous_files.emplace(entry.path, &entry);
        for (const auto& entry : entries)
            next_files.emplace(entry.path, &entry);
        std::erase_if(textures_, [&](const auto& cached) {
            const auto* before = find(entries_, entry_index_, cached.first);
            const auto* after = find(entries, index, cached.first);
            return !before || !after || before->bytes != after->bytes || before->modified != after->modified;
        });
        std::erase_if(sky_maps_, [&](const auto& cached) {
            const auto* before = find(entries_, entry_index_, cached.first);
            const auto* after = find(entries, index, cached.first);
            return !before || !after || before->bytes != after->bytes || before->modified != after->modified;
        });
        std::erase_if(meshes_, [&](const auto& cached) {
            const auto* next = find(entries, index, cached.first);
            if (!next || next->kind != AssetKind::mesh)
                return true;
            const auto* old = find(entries_, entry_index_, cached.first);
            if (cached.second.exact_content)
                return false;
            if (old->bytes != next->bytes || old->modified != next->modified)
                return true;
            for (const auto& path : cached.second.dependencies) {
                const auto a = previous_files.find(path);
                const auto b = next_files.find(path);
                if (a == previous_files.end() || b == next_files.end() || a->second->bytes != b->second->bytes ||
                    a->second->modified != b->second->modified)
                    return true;
            }
            return false;
        });
        std::erase_if(images_.entries, [](const auto& entry) { return entry.second.expired(); });
        entries_ = std::move(entries);
        entry_index_ = std::move(index);
        ++revision_;
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<MeshAsset> Reader::publish_mesh(std::vector<ProjectEntry> entries, std::string_view key,
                                       const MeshImport& loaded, std::span<const TextureAsset> textures) {
    try {
        if (!loaded.mesh)
            return failure("Mesh publication requires prepared geometry.");
        Reader candidate = *this;
        candidate.meshes_.erase(std::string(key)); // Explicit reimport supersedes received content/defaults.
        take(candidate.publish(std::move(entries)));
        const auto& item = listed(candidate, key, AssetKind::mesh);
        std::vector<std::filesystem::path> changed;
        CachedMesh cached{loaded.mesh, {}, loaded.notes};
        for (const auto& file : loaded.files) {
            const auto path = item.path.parent_path() / file.relative;
            ATELIER_TRY(candidate.resolve(path));
            changed.push_back(path);
            if (file.relative != item.path.filename())
                cached.dependencies.push_back(path);
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
        std::erase_if(candidate.textures_, [&](const auto& value) {
            return touched(listed(candidate, value.first, AssetKind::texture).path);
        });
        std::erase_if(candidate.sky_maps_,
                      [&](const auto& value) { return touched(listed(candidate, value.first, AssetKind::sky).path); });
        std::erase_if(candidate.meshes_, [&](const auto& value) {
            return !value.second.exact_content && (touched(listed(candidate, value.first, AssetKind::mesh).path) ||
                                                   std::ranges::any_of(value.second.dependencies, touched));
        });
        for (const auto& texture : textures) {
            const auto& entry = listed(candidate, texture.key, AssetKind::texture);
            if (!texture.image || !touched(entry.path))
                return failure("Published texture must be decoded content of this mesh bundle.");
            candidate.textures_.insert_or_assign(texture.key, texture.image);
        }
        candidate.meshes_.insert_or_assign(item.key, std::move(cached));
        auto result = take(candidate.mesh(key));
        if (candidate.revision_ == revision_)
            ++candidate.revision_;
        static_assert(std::is_nothrow_move_assignable_v<Reader>);
        *this = std::move(candidate);
        return result;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<void> Reader::publish_mesh_content(const MeshAsset& asset) {
    try {
        static_cast<void>(listed(*this, asset.key, AssetKind::mesh));
        if (!asset.mesh)
            return failure("Exact mesh publication requires resolved geometry.");
        Reader candidate = *this;
        auto& item = candidate.entries_[candidate.entry_index_.at(asset.key)];
        item.materials = asset.materials;
        CachedMesh cached{asset.mesh, {}, {}, true};
        if (const auto old = candidate.meshes_.find(asset.key);
            old != candidate.meshes_.end() && old->second.mesh == asset.mesh) {
            cached.dependencies = old->second.dependencies;
            cached.notes = old->second.notes;
        }
        candidate.meshes_.insert_or_assign(asset.key, std::move(cached));
        // Same named-slot validation as normal reads and file-transaction publication.
        take(candidate.mesh(asset.key));
        ++candidate.revision_;
        static_assert(std::is_nothrow_move_assignable_v<Reader>);
        *this = std::move(candidate);
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

const ProjectEntry* Reader::entry(std::string_view key) const {
    const auto at = entry_index_.find(key);
    return at == entry_index_.end() ? nullptr : &entries_[at->second];
}

Result<MeshImport> Reader::prepare_mesh(const std::filesystem::path& source) {
    return load_mesh(source, &images_);
}

std::vector<material::ImageRef> Reader::prepare_images(std::span<const std::string_view> sources,
                                                       std::uint64_t pixel_budget) {
    return decode_images(sources, &images_, pixel_budget);
}

void Reader::retain_import(std::string key, const std::filesystem::path& relative, MeshImport& loaded) {
    CachedMesh cached{loaded.mesh, {}, std::move(loaded.notes)};
    for (const auto& file : loaded.files)
        if (file.relative != relative.filename())
            cached.dependencies.push_back(relative.parent_path() / file.relative);
    meshes_.emplace(std::move(key), std::move(cached));
}

Result<MeshAsset> Reader::mesh(std::string_view key) {
    for (const auto& builtin : mesh_assets())
        if (builtin.key == key)
            return builtin;
    try {
        const auto* item = entry(key);
        if (!item || item->kind != AssetKind::mesh)
            return failure("Missing mesh resource: " + std::string(key));
        const auto& entry = *item;
        auto found_mesh = meshes_.find(entry.key);
        if (found_mesh == meshes_.end()) {
            ATELIER_TRY_BIND(path, resolve(entry.path));
            ATELIER_TRY_BIND(loaded, prepare_mesh(*path));
            retain_import(entry.key, entry.path, *loaded);
            found_mesh = meshes_.find(entry.key);
        }
        // In slot order, so every consumer lists the defaults as the mesh lists its slots.
        const auto& mesh = *found_mesh->second.mesh;
        std::vector<std::pair<std::size_t, SlotMaterial>> ordered;
        std::set<std::string> bound;
        for (const auto& binding : entry.materials) {
            std::size_t matches{};
            std::size_t index{};
            for (std::size_t i = 0; i < mesh.materials().size(); ++i)
                if (geometry::material_slot_name(mesh, i) == binding.slot) {
                    ++matches;
                    index = i;
                }
            if (matches != 1)
                return failure("Mesh metadata names a default material for a missing or ambiguous slot: " +
                               binding.slot);
            if (!bound.insert(binding.slot).second)
                return failure("Mesh metadata repeats a default material slot: " + binding.slot);
            ordered.emplace_back(index, binding);
        }
        std::ranges::sort(ordered, {}, &std::pair<std::size_t, SlotMaterial>::first);
        MeshAsset result{entry.key, entry.path.stem().wstring(), found_mesh->second.mesh};
        for (auto& [index, binding] : ordered)
            result.materials.push_back(std::move(binding));
        return result;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

std::span<const std::string> Reader::mesh_notes(std::string_view key) const {
    const auto found = meshes_.find(std::string(key));
    return found == meshes_.end() ? std::span<const std::string>{} : std::span<const std::string>(found->second.notes);
}

Result<std::vector<std::filesystem::path>> Reader::mesh_dependency_files(std::string_view key) {
    try {
        for (const auto& builtin : mesh_assets())
            if (builtin.key == key)
                return std::vector<std::filesystem::path>{};
        const auto& item = listed(*this, key, AssetKind::mesh);
        auto cached = meshes_.find(item.key);
        if (cached == meshes_.end()) {
            ATELIER_TRY_BIND(path, resolve(item.path));
            ATELIER_TRY_BIND(loaded, prepare_mesh(*path));
            retain_import(item.key, item.path, *loaded);
            cached = meshes_.find(item.key);
        }
        return cached->second.dependencies;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<SkyAsset> Reader::sky_map(std::string_view key) {
    const auto* at = entry(key);
    if (!at)
        return failure("Sky map key is not a Radiance .hdr asset of this project: " + std::string(key));
    const auto relative = platform::utf16_to_utf8(at->path.generic_wstring());
    if (!relative)
        return failure("Invalid UTF-16 text.");
    if (at->kind != AssetKind::sky)
        return failure("Sky map key names " + *relative + ", which is not a Radiance .hdr asset.");
    SkyAsset asset{at->key, at->path.stem().wstring(), {}};
    if (const auto cached = sky_maps_.find(at->key); cached != sky_maps_.end()) {
        asset.map = cached->second;
        return asset;
    }
    auto path = resolve(at->path);
    auto bytes = path ? read_file(*path, std::size_t(256) << 20) : Result<std::string>{std::unexpected(path.error())};
    if (!bytes)
        return failure("Sky map " + *relative + " could not be read: " + bytes.error().message);
    auto decoded = rendering::decode_radiance_hdr(*bytes);
    if (!decoded)
        return failure("Sky map " + *relative + " is not a valid Radiance HDR file: " + decoded.error().message);
    asset.map = std::move(*decoded);
    sky_maps_.emplace(at->key, asset.map);
    return asset;
}

Result<TextureAsset> Reader::texture(std::string_view key) {
    try {
        const auto& item = listed(*this, key, AssetKind::texture);
        auto at = textures_.find(item.key);
        if (at == textures_.end()) {
            // Through the image cache: a mesh import that decoded the same bytes shares its pixels.
            ATELIER_TRY_BIND(path, resolve(item.path));
            ATELIER_TRY_BIND(bytes, read_file(*path, 128 * 1024 * 1024));
            const std::array<std::string_view, 1> source{*bytes};
            at = textures_.emplace(item.key, decode_images(source, &images_).front()).first;
        }
        return TextureAsset{item.key, item.path.stem().wstring(), at->second};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<void> Reader::publish_texture(ProjectEntry item, material::ImageRef image) {
    const auto at = entry_index_.find(item.key);
    if (at == entry_index_.end() || !image || item.kind != AssetKind::texture ||
        entries_[at->second].kind != AssetKind::texture || entries_[at->second].path != item.path)
        return failure("Texture publication must replace an existing texture at its current location.");
    textures_.insert_or_assign(item.key, std::move(image));
    // A mesh may have decoded the same file into its imported default Surface.
    std::erase_if(meshes_, [&](const auto& cached) {
        return !cached.second.exact_content &&
               std::ranges::find(cached.second.dependencies, item.path) != cached.second.dependencies.end();
    });
    entries_[at->second] = std::move(item);
    ++revision_;
    return {};
}

Result<MaterialAsset> Reader::material(std::string_view key) {
    try {
        for (const auto& builtin : material_assets())
            if (builtin.key == key)
                return builtin;
        const auto& item = listed(*this, key, AssetKind::material);
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
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<source::SourceFile> Reader::source(const std::filesystem::path& project_relative) const {
    const auto named = platform::utf16_to_utf8(project_relative.generic_wstring());
    if (!named)
        return failure("Invalid UTF-16 text.");
    if (named->starts_with("builtin/")) {
        auto builtin = builtin_material_source(*named);
        if (!builtin)
            return failure("Unknown installed material Source: " + *named);
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

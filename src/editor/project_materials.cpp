#include "editor/project.hpp"
#include "assets/file.hpp"
#include "assets/material_format.hpp"
#include "assets/image_decode.hpp"

namespace atelier::editor {
namespace {
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

auto failure(const std::exception& error) {
    return std::unexpected(Error{ErrorCode::invalid_storage, error.what()});
}

const assets::ProjectEntry& listed(const Project& project, std::string_view key, assets::AssetKind kind) {
    const auto* at = project.entry(key);
    if (!at || at->kind != kind)
        throw std::runtime_error("Missing resource or incorrect resource type: " + std::string(key));
    return *at;
}

auto failed_import(const std::exception& original, const std::vector<std::filesystem::path>& paths) {
    std::string message = original.what();
    for (auto at = paths.rbegin(); at != paths.rend(); ++at) {
        std::error_code error;
        std::filesystem::remove(*at, error);
        if (error)
            message += "; cleanup failed: " + error.message();
    }
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message)});
}
}

Result<std::string> Project::import_texture(const std::filesystem::path& source) {
    std::vector<std::filesystem::path> created;
    try {
        if (!assets::is_texture_path(source))
            throw std::runtime_error("Import a PNG, JPEG, BMP or TIFF texture.");
        auto bytes = take(assets::read_file(source, 128 * 1024 * 1024));
        static_cast<void>(assets::decode_image(bytes));
        auto relative = std::filesystem::path("Content/Textures") / source.filename();
        auto target = take(resolve(relative));
        auto metadata = target;
        metadata += L".asset.json";
        if (std::filesystem::exists(metadata))
            throw std::runtime_error("Texture metadata already exists.");
        take(write_file(target, bytes, WriteMode::create));
        created.push_back(target);
        // refresh is the sole GUID producer for all resource kinds.
        auto refreshed = refresh();
        if (std::filesystem::exists(metadata))
            created.push_back(metadata);
        take(std::move(refreshed));
        const auto at = std::ranges::find(entries(), relative, &assets::ProjectEntry::path);
        if (at == entries().end() || at->kind != assets::AssetKind::texture)
            throw std::runtime_error("Unsupported texture extension.");
        return at->key;
    } catch (const std::exception& error) {
        return failed_import(error, created);
    }
}

Result<assets::TextureAsset> Project::reimport_texture(std::string_view key, const std::filesystem::path& source) {
    try {
        auto item = listed(*this, key, assets::AssetKind::texture);
        if (!assets::is_texture_path(source))
            throw std::runtime_error("Reimport a PNG, JPEG, BMP or TIFF texture.");
        const auto bytes = take(assets::read_file(source, 128 * 1024 * 1024));
        auto image = assets::decode_image(bytes);
        const auto target = take(resolve(item.path));
        // No new sidecar, asset key or import-history entry. The old committed
        // file remains intact until both decode and the atomic write succeed.
        take(write_file(target, bytes));
        item.bytes = bytes.size();
        item.modified = std::filesystem::last_write_time(target);
        auto result = assets::TextureAsset{item.key, item.path.stem().wstring(), image};
        take(assets_.publish_texture(std::move(item), std::move(image)));
        return result;
    } catch (const std::exception& error) {
        return failure(error);
    }
}

Result<std::string> Project::create_material(const std::filesystem::path& relative,
                                             const assets::MaterialAsset& asset) {
    std::vector<std::filesystem::path> created;
    try {
        const auto normalized = relative.lexically_normal();
        if (normalized.empty() || normalized.has_root_path() || normalized.extension() != L".material" ||
            *normalized.begin() != L"Content")
            throw std::runtime_error("Create materials inside Content with a .material extension.");
        const bool source = std::holds_alternative<assets::MaterialSourceAsset>(asset.author);
        if (source != assets::is_material_source_path(normalized))
            throw std::runtime_error("Material Sources use .ms.material; Material Instances use .material.");
        auto target = take(resolve(normalized));
        const auto catalog_path = target.lexically_relative(root_);
        if (catalog_path.empty() || *catalog_path.begin() != L"Content")
            throw std::runtime_error("Material destination must resolve inside Content.");
        auto metadata = target;
        metadata += L".asset.json";
        if (std::filesystem::exists(metadata))
            throw std::runtime_error("Material metadata already exists.");
        // Validate author structure, not the currently resolved fallback values.
        // Unavailable dependency keys remain author intent and can be repaired.
        if (source)
            static_cast<void>(take(assets_.source(std::get<assets::MaterialSourceAsset>(asset.author).source_path)));
        else
            static_cast<void>(take(assets_.material(std::get<assets::MaterialInstanceAsset>(asset.author).parent_key)));
        take(write_file(target, assets::material_format::encode(asset).dump(2) + "\n", WriteMode::create));
        created.push_back(target);
        auto refreshed = refresh();
        if (std::filesystem::exists(metadata))
            created.push_back(metadata);
        take(std::move(refreshed));
        const auto at = std::ranges::find(entries(), catalog_path, &assets::ProjectEntry::path);
        if (at == entries().end())
            throw std::runtime_error("Published material is missing from the resource catalog.");
        return at->key;
    } catch (const std::exception& error) {
        return failed_import(error, created);
    }
}

Result<std::string> Project::create_material_source(const std::filesystem::path& relative, std::string_view text) {
    std::vector<std::filesystem::path> created;
    try {
        const auto normalized = relative.lexically_normal();
        if (normalized.empty() || normalized.has_root_path() || *normalized.begin() != L"Content" ||
            !assets::is_material_source_path(normalized))
            throw std::runtime_error("Create Material Sources inside Content with a .ms.material extension.");
        auto source_path = normalized;
        source_path.replace_extension(L".source");
        auto source_file = take(resolve(source_path));
        if (text.empty())
            text = R"(struct Params { vector3 tint; float roughness; };
Params defaults() { Params p; p.tint=vector3(0.6,0.6,0.6); p.roughness=0.65; return p; }
material.Output shade(material.Context context,Params p) {
    material.Output result; result.base_color=p.tint; result.roughness=p.roughness;
    result.normal_world=context.normal_world; return result;
}
)";
        take(write_file(source_file, text, WriteMode::create));
        created.push_back(source_file);
        assets::MaterialAsset asset;
        asset.author = assets::MaterialSourceAsset{assets::utf8(source_path.generic_wstring()), {}, {}};
        return take(create_material(normalized, asset));
    } catch (const std::exception& error) {
        return failed_import(error, created);
    }
}

Result<void> Project::save_material(const assets::MaterialAsset& asset) {
    try {
        const auto& item = listed(*this, asset.key, assets::AssetKind::material);
        if (std::holds_alternative<assets::MaterialSourceAsset>(asset.author) !=
            assets::is_material_source_path(item.path))
            throw std::runtime_error("Saving cannot change an asset between Material Source and Material Instance.");
        take(write_file(take(resolve(item.path)), assets::material_format::encode(asset).dump(2) + "\n"));
        return {}; // The caller records the successful save before refreshing the catalog.
    } catch (const std::exception& error) {
        return failure(error);
    }
}
}

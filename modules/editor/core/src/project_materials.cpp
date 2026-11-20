#include "editor/project.hpp"
#include "editor/graph_format.hpp"
#include "assets/file.hpp"
#include "assets/material_format.hpp"
#include "assets/image_decode.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "procedural/graph.hpp"

namespace atelier::editor {
namespace {
Result<const assets::ProjectEntry*> listed(const Project& project, std::string_view key, assets::AssetKind kind) {
    const auto* at = project.entry(key);
    if (!at || at->kind != kind)
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Missing resource or incorrect resource type: " + std::string(key)});
    return at;
}

auto failed_import(std::string message, const std::vector<std::filesystem::path>& paths) {
    for (auto at = paths.rbegin(); at != paths.rend(); ++at) {
        std::error_code error;
        std::filesystem::remove(*at, error);
        if (error)
            message += "; cleanup failed: " + platform::error_message(error);
    }
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message)});
}
}

Result<std::string> Project::import_texture(const std::filesystem::path& source) {
    std::vector<std::filesystem::path> created;
    if (!assets::is_texture_path(source))
        return failed_import("Import a PNG, JPEG, BMP or TIFF texture.", created);
    ATELIER_TRY_BIND(bytes, assets::read_file(source, 128 * 1024 * 1024));
    if (const auto decoded = assets::decode_image(*bytes); !decoded)
        return failed_import(decoded.error().message, created);
    auto relative = std::filesystem::path(L"Content/Textures") / source.filename();
    ATELIER_TRY_BIND(target, resolve(relative));
    auto metadata = *target;
    metadata += L".asset.json";
    std::error_code error;
    const auto exists = std::filesystem::exists(metadata, error);
    if (error)
        return failed_import(platform::error_message(error), created);
    if (exists)
        return failed_import("Texture metadata already exists.", created);
    ATELIER_TRY(write_file(*target, *bytes, WriteMode::create));
    created.push_back(*target);
    // refresh is the sole GUID producer for all resource kinds.
    auto refreshed = refresh();
    if (std::filesystem::exists(metadata, error))
        created.push_back(metadata);
    if (error)
        return failed_import(platform::error_message(error), created);
    if (!refreshed)
        return failed_import(std::move(refreshed.error().message), created);
    const auto at = std::ranges::find(entries(), relative, &assets::ProjectEntry::path);
    if (at == entries().end() || at->kind != assets::AssetKind::texture)
        return failed_import("Unsupported texture extension.", created);
    return at->key;
}

Result<assets::TextureAsset> Project::reimport_texture(std::string_view key, const std::filesystem::path& source) {
    ATELIER_TRY_BIND(listed_item, listed(*this, key, assets::AssetKind::texture));
    auto item = **listed_item;
    if (!assets::is_texture_path(source))
        return std::unexpected(Error{ErrorCode::invalid_storage, "Reimport a PNG, JPEG, BMP or TIFF texture."});
    ATELIER_TRY_BIND(bytes, assets::read_file(source, 128 * 1024 * 1024));
    auto image = assets::decode_image(*bytes);
    if (!image)
        return std::unexpected(Error{ErrorCode::invalid_storage, image.error().message});
    ATELIER_TRY_BIND(target, resolve(item.path));
    // No new sidecar, asset key or import-history entry. The old committed
    // file remains intact until both decode and the atomic write succeed.
    ATELIER_TRY(write_file(*target, *bytes));
    item.bytes = bytes->size();
    std::error_code error;
    item.modified = std::filesystem::last_write_time(*target, error);
    if (error)
        return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
    auto result = assets::TextureAsset{item.key, item.path.stem().wstring(), *image};
    ATELIER_TRY(assets_.publish_texture(std::move(item), std::move(*image)));
    return result;
}

Result<std::string> Project::create_material(const std::filesystem::path& relative,
                                             const assets::MaterialAsset& asset) {
    std::vector<std::filesystem::path> created;
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.has_root_path() || normalized.extension() != L".material" ||
        *normalized.begin() != L"Content")
        return failed_import("Create materials inside Content with a .material extension.", created);
    const bool source = std::holds_alternative<assets::MaterialSourceAsset>(asset.author);
    if (source != assets::is_material_source_path(normalized))
        return failed_import("Material Sources use .ms.material; Material Instances use .material.", created);
    ATELIER_TRY_BIND(target, resolve(normalized));
    const auto catalog_path = target->lexically_relative(root_);
    if (catalog_path.empty() || *catalog_path.begin() != L"Content")
        return failed_import("Material destination must resolve inside Content.", created);
    auto metadata = *target;
    metadata += L".asset.json";
    std::error_code error;
    const auto exists = std::filesystem::exists(metadata, error);
    if (error)
        return failed_import(platform::error_message(error), created);
    if (exists)
        return failed_import("Material metadata already exists.", created);
    // Validate author structure, not the currently resolved fallback values.
    // Unavailable dependency keys remain author intent and can be repaired.
    if (source) {
        const auto path = platform::utf8_to_utf16(std::get<assets::MaterialSourceAsset>(asset.author).source_path);
        if (!path)
            return failed_import("Invalid UTF-8 text.", created);
        ATELIER_TRY(assets_.source(*path));
    } else {
        const auto parent = assets_.material(std::get<assets::MaterialInstanceAsset>(asset.author).parent_key);
        if (!parent)
            return failed_import(parent.error().message, created);
    }
    const auto document = assets::material_format::encode(asset);
    if (!document)
        return failed_import(document.error().describe(), created);
    const auto bytes = json::write(document->root(), {true, true});
    ATELIER_TRY(write_file(*target, {bytes.data(), bytes.size()}, WriteMode::create));
    created.push_back(*target);
    auto refreshed = refresh();
    if (std::filesystem::exists(metadata, error))
        created.push_back(metadata);
    if (error)
        return failed_import(platform::error_message(error), created);
    if (!refreshed)
        return failed_import(std::move(refreshed.error().message), created);
    const auto at = std::ranges::find(entries(), catalog_path, &assets::ProjectEntry::path);
    if (at == entries().end())
        return failed_import("Published material is missing from the resource catalog.", created);
    return at->key;
}

Result<std::string> Project::create_material_source(const std::filesystem::path& relative, std::string_view text) {
    std::vector<std::filesystem::path> created;
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.has_root_path() || *normalized.begin() != L"Content" ||
        !assets::is_material_source_path(normalized))
        return failed_import("Create Material Sources inside Content with a .ms.material extension.", created);
    auto source_path = normalized;
    source_path.replace_extension(L".source");
    ATELIER_TRY_BIND(source_file, resolve(source_path));
    if (text.empty())
        text = R"(struct Params { vector3 tint; float roughness; };
Params defaults() { Params p; p.tint=vector3(0.6,0.6,0.6); p.roughness=0.65; return p; }
material.Output shade(material.Context context,Params p) {
    material.Output result; result.base_color=p.tint; result.roughness=p.roughness;
    result.normal_world=context.normal_world; return result;
}
)";
    ATELIER_TRY(write_file(*source_file, text, WriteMode::create));
    created.push_back(*source_file);
    assets::MaterialAsset asset;
    auto name = platform::utf16_to_utf8(source_path.generic_wstring());
    if (!name)
        return failed_import("Invalid UTF-16 text.", created);
    asset.author = assets::MaterialSourceAsset{std::move(*name), {}, {}};
    auto material = create_material(normalized, asset);
    if (!material)
        return failed_import(std::move(material.error().message), created);
    return material;
}

Result<void> Project::save_material(const assets::MaterialAsset& asset) {
    ATELIER_TRY_BIND(item, listed(*this, asset.key, assets::AssetKind::material));
    if (std::holds_alternative<assets::MaterialSourceAsset>(asset.author) !=
        assets::is_material_source_path((*item)->path))
        return std::unexpected(Error{ErrorCode::invalid_storage,
                                     "Saving cannot change an asset between Material Source and Material Instance."});
    ATELIER_TRY_BIND(target, resolve((*item)->path));
    const auto document = assets::material_format::encode(asset);
    if (!document)
        return std::unexpected(Error{ErrorCode::invalid_storage, document.error().describe()});
    const auto bytes = json::write(document->root(), {true, true});
    ATELIER_TRY(write_file(*target, {bytes.data(), bytes.size()}));
    return {}; // The caller records the successful save before refreshing the catalog.
}

Result<std::string> Project::create_procedural_graph(const std::filesystem::path& relative, std::string_view text) {
    std::vector<std::filesystem::path> created;
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.has_root_path() || !assets::is_procedural_graph_path(normalized) ||
        *normalized.begin() != L"Content")
        return failed_import("Create procedural graphs inside Content with a .procgraph extension.", created);
    ATELIER_TRY_BIND(target, resolve(normalized));
    const auto catalog_path = target->lexically_relative(root_);
    if (catalog_path.empty() || *catalog_path.begin() != L"Content")
        return failed_import("Procedural graph destination must resolve inside Content.", created);
    auto metadata = *target;
    metadata += L".asset.json";
    std::error_code error;
    const auto exists = std::filesystem::exists(metadata, error);
    if (error)
        return failed_import(platform::error_message(error), created);
    if (exists)
        return failed_import("Procedural graph metadata already exists.", created);
    std::string content;
    if (text.empty()) {
        Runtime validation;
        ATELIER_TRY(procedural::register_types(validation));
        ATELIER_TRY_BIND(graph_type, validation.native_type<procedural::Graph>());
        ATELIER_TRY_BIND(graph_root, validation.create(*graph_type));
        GraphDocument empty_doc;
        empty_doc.graph = std::move(*graph_root);
        ATELIER_TRY_BIND(canonical, save_graph(validation, empty_doc));
        content = std::move(*canonical);
    } else {
        if (!StringView(text).is_valid_utf8())
            return failed_import("Invalid UTF-8 text.", created);
        Runtime validation;
        ATELIER_TRY(procedural::register_types(validation));
        const auto parsed = load_graph(validation, text);
        if (!parsed)
            return failed_import(parsed.error().message, created);
        content.assign(text);
    }
    ATELIER_TRY(write_file(*target, content, WriteMode::create));
    created.push_back(*target);
    auto refreshed = refresh();
    if (std::filesystem::exists(metadata, error))
        created.push_back(metadata);
    if (error)
        return failed_import(platform::error_message(error), created);
    if (!refreshed)
        return failed_import(std::move(refreshed.error().message), created);
    const auto at = std::ranges::find(entries(), catalog_path, &assets::ProjectEntry::path);
    if (at == entries().end() || at->kind != assets::AssetKind::procedural_graph)
        return failed_import("Published procedural graph is missing from the resource catalog.", created);
    return at->key;
}

Result<void> Project::save_procedural_graph(std::string_view key, std::string_view text) {
    ATELIER_TRY_BIND(item, listed(*this, key, assets::AssetKind::procedural_graph));
    if (!StringView(text).is_valid_utf8())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-8 text."});
    Runtime validation;
    ATELIER_TRY(procedural::register_types(validation));
    const auto parsed = load_graph(validation, text);
    if (!parsed)
        return std::unexpected(Error{ErrorCode::invalid_storage, parsed.error().message});
    ATELIER_TRY_BIND(target, resolve((*item)->path));
    ATELIER_TRY(write_file(*target, text));
    return {};
}
}

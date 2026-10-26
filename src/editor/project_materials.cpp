#include "editor/project.hpp"
#include "assets/file.hpp"
#include "assets/material_format.hpp"
#include "assets/image_decode.hpp"

namespace atelier::editor {
namespace {
template<class T> T take(Result<T> result) {if(!result) throw std::runtime_error(result.error().message);if constexpr(!std::is_void_v<T>) return std::move(*result);}
auto failure(const std::exception& error) {return std::unexpected(Error{ErrorCode::invalid_storage,error.what()});}
const assets::ProjectEntry& listed(const Project& project,std::string_view key,assets::AssetKind kind) {
    const auto* at=project.entry(key);
    if(!at || at->kind!=kind) throw std::runtime_error("Missing resource or incorrect resource type: "+std::string(key));
    return *at;
}
auto failed_import(const std::exception& original,const std::vector<std::filesystem::path>& paths) {
    std::string message=original.what();
    for(auto at=paths.rbegin();at!=paths.rend();++at) {std::error_code error;std::filesystem::remove(*at,error);if(error) message+="; cleanup failed: "+error.message();}
    return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)});
}
}
Result<std::string> Project::import_texture(const std::filesystem::path& source) {
    std::vector<std::filesystem::path> created;
    try {
        if(!assets::is_texture_path(source)) throw std::runtime_error("Import a PNG, JPEG, BMP or TIFF texture.");
        auto bytes=take(assets::read_file(source,128*1024*1024));static_cast<void>(assets::decode_image(bytes));
        auto relative=std::filesystem::path("Content/Textures")/source.filename();
        auto target=take(resolve(relative));auto metadata=target;metadata+=L".asset.json";
        if(std::filesystem::exists(metadata)) throw std::runtime_error("Texture metadata already exists.");
        take(write_file(target,bytes,WriteMode::create));created.push_back(target);
        // refresh is the sole GUID producer for all resource kinds.
        auto refreshed=refresh();if(std::filesystem::exists(metadata)) created.push_back(metadata);take(std::move(refreshed));
        const auto at=std::ranges::find(entries(),relative,&assets::ProjectEntry::path);
        if(at==entries().end() || at->kind!=assets::AssetKind::texture) throw std::runtime_error("Unsupported texture extension.");
        return at->key;
    } catch(const std::exception& error) {return failed_import(error,created);}
}
Result<std::string> Project::create_material(const std::filesystem::path& relative,const assets::MaterialAsset& asset) {
    std::vector<std::filesystem::path> created;
    try {
        const auto normalized=relative.lexically_normal();
        if(normalized.empty() || normalized.has_root_path() || normalized.extension()!=L".material" || *normalized.begin()!=L"Content")
            throw std::runtime_error("Create materials inside Content with a .material extension.");
        auto target=take(resolve(normalized));const auto catalog_path=target.lexically_relative(root_);
        if(catalog_path.empty() || *catalog_path.begin()!=L"Content") throw std::runtime_error("Material destination must resolve inside Content.");
        auto metadata=target;metadata+=L".asset.json";
        if(std::filesystem::exists(metadata)) throw std::runtime_error("Material metadata already exists.");
        static_cast<void>(material::Surface(asset.description));
        for(const auto& key:asset.textures) if(!key.empty()) static_cast<void>(take(assets_.texture(key)));
        take(write_file(target,assets::material_format::encode(asset).dump(2)+"\n",WriteMode::create));created.push_back(target);
        auto refreshed=refresh();if(std::filesystem::exists(metadata)) created.push_back(metadata);take(std::move(refreshed));
        const auto at=std::ranges::find(entries(),catalog_path,&assets::ProjectEntry::path);
        if(at==entries().end()) throw std::runtime_error("Published material is missing from the resource catalog.");
        return at->key;
    } catch(const std::exception& error) {return failed_import(error,created);}
}
Result<void> Project::save_material(const assets::MaterialAsset& asset) {
    try {
        const auto& item=listed(*this,asset.key,assets::AssetKind::material);
        static_cast<void>(material::Surface(asset.description));
        // A key the catalog no longer lists is a missing texture; saving keeps the reference.
        // A listed key must name a texture; whether its file still decodes does not block the save.
        for(const auto& key:asset.textures)
            if(!key.empty() && Project::entry(key)) static_cast<void>(listed(*this,key,assets::AssetKind::texture));
        take(write_file(take(resolve(item.path)),assets::material_format::encode(asset).dump(2)+"\n"));
        return {}; // The caller records the successful save before refreshing the catalog.
    } catch(const std::exception& error) {return failure(error);}
}
}

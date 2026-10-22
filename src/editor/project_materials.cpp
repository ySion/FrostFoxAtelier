#include "editor/material_format.hpp"
#include "editor/image_decode.hpp"
#include <Windows.h>
#include <objbase.h>
#include <cwctype>

namespace atelier::editor {
namespace {
template<class T> T take(Result<T> result) {if(!result) throw std::runtime_error(result.error().message);if constexpr(!std::is_void_v<T>) return std::move(*result);}
auto failure(const std::exception& error) {return std::unexpected(Error{ErrorCode::invalid_storage,error.what()});}
const ProjectEntry& entry(const Project& project,std::string_view key,AssetKind kind) {
    const auto at=std::ranges::find(project.entries(),key,&ProjectEntry::key);
    if(at==project.entries().end() || at->kind!=kind) throw std::runtime_error("Missing resource or incorrect resource type: "+std::string(key));
    return *at;
}
auto failed_import(const std::exception& original,const std::vector<std::filesystem::path>& paths) {
    std::string message=original.what();
    for(auto at=paths.rbegin();at!=paths.rend();++at) {std::error_code error;std::filesystem::remove(*at,error);if(error) message+="; cleanup failed: "+error.message();}
    return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)});
}
}
bool Project::is_texture_path(const std::filesystem::path& path) {
    auto extension=path.extension().wstring();for(auto& c:extension) c=static_cast<wchar_t>(std::towlower(c));
    return extension==L".png" || extension==L".jpg" || extension==L".jpeg" || extension==L".bmp" || extension==L".tif" || extension==L".tiff";
}
bool Project::is_sky_path(const std::filesystem::path& path) {
    auto extension=path.extension().wstring();for(auto& c:extension) c=static_cast<wchar_t>(std::towlower(c));
    return extension==L".hdr";
}
Result<SkyAsset> Project::sky_map(std::string_view key) {
    const auto at=std::ranges::find(entries_,key,&ProjectEntry::key);
    if(at==entries_.end() || at->kind!=AssetKind::sky)
        return failure(std::runtime_error(at==entries_.end()?"Sky map key is not a Radiance .hdr asset of this project: "+std::string(key)
            :"Sky map key names "+utf8(at->path.generic_wstring())+", which is not a Radiance .hdr asset."));
    const auto relative=utf8(at->path.generic_wstring());
    SkyAsset asset{at->key,at->path.stem().wstring(),{}};
    if(const auto cached=sky_maps_.find(at->key);cached!=sky_maps_.end()) {asset.map=cached->second;return asset;}
    auto path=resolve(at->path);
    auto bytes=path?read_file(*path,std::size_t(256)<<20):Result<std::string>{std::unexpected(path.error())};
    if(!bytes) return failure(std::runtime_error("Sky map "+relative+" could not be read: "+bytes.error().message));
    try {asset.map=rendering::decode_radiance_hdr(*bytes);}
    catch(const std::exception& error) {return failure(std::runtime_error("Sky map "+relative+" is not a valid Radiance HDR file: "+error.what()));}
    sky_maps_.emplace(at->key,asset.map);
    return asset;
}
Result<TextureAsset> Project::texture(std::string_view key) {
    try {
        const auto& item=entry(*this,key,AssetKind::texture);
        auto at=textures_.find(item.key);
        if(at==textures_.end()) at=textures_.emplace(item.key,decode_image(take(read_file(take(resolve(item.path)),128*1024*1024)))).first;
        return TextureAsset{item.key,item.path.stem().wstring(),at->second};
    } catch(const std::exception& error) {return failure(error);}
}
Result<MaterialAsset> Project::material(std::string_view key) {
    try {
        const auto& item=entry(*this,key,AssetKind::material);
        auto asset=material_format::decode(json_format::parse(take(read_file(take(resolve(item.path))))));
        asset.key=item.key;asset.name=item.path.stem().wstring();return asset;
    } catch(const std::exception& error) {return failure(error);}
}
Result<std::string> Project::import_texture(const std::filesystem::path& source) {
    std::vector<std::filesystem::path> created;
    try {
        if(!is_texture_path(source)) throw std::runtime_error("Import a PNG, JPEG, BMP or TIFF texture.");
        auto bytes=take(read_file(source,128*1024*1024));static_cast<void>(decode_image(bytes));
        auto relative=std::filesystem::path("Content/Textures")/source.filename();
        auto target=take(resolve(relative));auto metadata=target;metadata+=L".asset.json";
        if(std::filesystem::exists(metadata)) throw std::runtime_error("Texture metadata already exists.");
        take(write_file(target,bytes,WriteMode::create));created.push_back(target);
        // refresh is the sole GUID producer for all resource kinds.
        auto refreshed=refresh();if(std::filesystem::exists(metadata)) created.push_back(metadata);take(std::move(refreshed));
        const auto at=std::ranges::find(entries_,relative,&ProjectEntry::path);
        if(at==entries_.end() || at->kind!=AssetKind::texture) throw std::runtime_error("Unsupported texture extension.");
        return at->key;
    } catch(const std::exception& error) {return failed_import(error,created);}
}
Result<std::string> Project::create_material(const std::filesystem::path& relative,const MaterialAsset& asset) {
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
        for(const auto& key:asset.textures) if(!key.empty()) static_cast<void>(take(texture(key)));
        take(write_file(target,material_format::encode(asset).dump(2)+"\n",WriteMode::create));created.push_back(target);
        auto refreshed=refresh();if(std::filesystem::exists(metadata)) created.push_back(metadata);take(std::move(refreshed));
        const auto at=std::ranges::find(entries_,catalog_path,&ProjectEntry::path);
        if(at==entries_.end()) throw std::runtime_error("Published material is missing from the resource catalog.");
        return at->key;
    } catch(const std::exception& error) {return failed_import(error,created);}
}
Result<void> Project::save_material(const MaterialAsset& asset) {
    try {
        const auto& item=entry(*this,asset.key,AssetKind::material);
        static_cast<void>(material::Surface(asset.description));
        // A key the catalog no longer lists is a missing texture; saving keeps the reference.
        // A listed key must name a texture; whether its file still decodes does not block the save.
        for(const auto& key:asset.textures)
            if(!key.empty() && std::ranges::find(entries_,key,&ProjectEntry::key)!=entries_.end()) static_cast<void>(entry(*this,key,AssetKind::texture));
        take(write_file(take(resolve(item.path)),material_format::encode(asset).dump(2)+"\n"));
        return {}; // The caller records the successful save before refreshing the catalog.
    } catch(const std::exception& error) {return failure(error);}
}
}

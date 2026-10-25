#include "assets/reader.hpp"
#include "assets/material_format.hpp"
#include "platform/windows_path.hpp"
#include <Windows.h>
#include <cwctype>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace atelier::assets {
namespace {
auto failure(std::string text) {return std::unexpected(Error{ErrorCode::invalid_storage,std::move(text)});}
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(*value);
}
const ProjectEntry& listed(const Reader& reader,std::string_view key,AssetKind kind) {
    const auto* at=reader.entry(key);
    if(!at || at->kind!=kind) throw std::runtime_error("Missing resource or incorrect resource type: "+std::string(key));
    return *at;
}
}
bool is_texture_path(const std::filesystem::path& path) {
    auto extension=path.extension().wstring();for(auto& c:extension) c=static_cast<wchar_t>(std::towlower(c));
    return extension==L".png" || extension==L".jpg" || extension==L".jpeg" || extension==L".bmp" || extension==L".tif" || extension==L".tiff";
}
bool is_sky_path(const std::filesystem::path& path) {
    auto extension=path.extension().wstring();for(auto& c:extension) c=static_cast<wchar_t>(std::towlower(c));
    return extension==L".hdr";
}
Result<std::filesystem::path> Reader::resolve(const std::filesystem::path& relative) const {
    return resolve_project_path(root_,relative);
}
Result<void> Reader::publish(std::vector<ProjectEntry> entries) {
    try {
        if (entries==entries_) return {};
        if(root_.empty()) return failure("Publishing project assets requires a project root.");
        decltype(entry_index_) index;index.reserve(entries.size());
        for (std::size_t i=0;i<entries.size();++i) {
            if(entries[i].key.starts_with("builtin/") || !index.emplace(entries[i].key,i).second)
                return failure("Duplicate or reserved resource identity in project catalogue.");
        }
        const auto find=[](const std::vector<ProjectEntry>& list,const decltype(entry_index_)& positions,std::string_view key)->const ProjectEntry* {
            const auto at=positions.find(key);return at==positions.end()?nullptr:&list[at->second];
        };
        std::map<std::filesystem::path,const ProjectEntry*> previous_files,next_files;
        for(const auto& entry:entries_) previous_files.emplace(entry.path,&entry);
        for(const auto& entry:entries) next_files.emplace(entry.path,&entry);
        std::erase_if(textures_,[&](const auto& cached) {
            const auto* before=find(entries_,entry_index_,cached.first);const auto* after=find(entries,index,cached.first);
            return !before || !after || before->bytes!=after->bytes || before->modified!=after->modified;
        });
        std::erase_if(sky_maps_,[&](const auto& cached) {
            const auto* before=find(entries_,entry_index_,cached.first);const auto* after=find(entries,index,cached.first);
            return !before || !after || before->bytes!=after->bytes || before->modified!=after->modified;
        });
        std::erase_if(meshes_,[&](const auto& cached) {
            const auto* next=find(entries,index,cached.first);
            if (!next || next->kind!=AssetKind::mesh) return true;
            const auto* old=find(entries_,entry_index_,cached.first);
            if (!old) return false; // Prepared, newly published import.
            if(old->bytes!=next->bytes || old->modified!=next->modified) return true;
            for(const auto& path:cached.second.dependencies) {
                const auto a=previous_files.find(path),b=next_files.find(path);
                if(a==previous_files.end() || b==next_files.end() || a->second->bytes!=b->second->bytes ||
                    a->second->modified!=b->second->modified) return true;
            }
            return false;
        });
        std::erase_if(images_.entries,[](const auto& entry){return entry.second.expired();});
        entries_=std::move(entries);entry_index_=std::move(index);++revision_;return {};
    } catch (const std::exception& error) {return failure(error.what());}
}
const ProjectEntry* Reader::entry(std::string_view key) const {
    const auto at=entry_index_.find(key);return at==entry_index_.end()?nullptr:&entries_[at->second];
}
MeshImport Reader::prepare_mesh(const std::filesystem::path& source) {
    return load_mesh(source,&images_);
}
void Reader::retain_import(std::string key,const std::filesystem::path& relative,MeshImport& loaded) {
    CachedMesh cached{loaded.mesh,{},std::move(loaded.notes)};
    for(const auto& file:loaded.files) if(file.relative!=relative.filename()) cached.dependencies.push_back(relative.parent_path()/file.relative);
    meshes_.emplace(std::move(key),std::move(cached));
}
void Reader::forget_import(std::string_view key) {meshes_.erase(std::string(key));}
Result<MeshAsset> Reader::mesh(std::string_view key) {
    for(const auto& builtin:mesh_assets()) if(builtin.key==key) return builtin;
    try {
        const auto* item=entry(key);
        if (!item || item->kind!=AssetKind::mesh) return failure("Missing mesh resource: "+std::string(key));
        const auto& entry=*item;
        auto found_mesh=meshes_.find(entry.key);
        if(found_mesh==meshes_.end()) {
            auto loaded=prepare_mesh(take(resolve(entry.path)));
            retain_import(entry.key,entry.path,loaded);found_mesh=meshes_.find(entry.key);
        }
        // In slot order, so every consumer lists the defaults as the mesh lists its slots.
        const auto& mesh=*found_mesh->second.mesh;std::vector<std::pair<std::size_t,SlotMaterial>> ordered;
        for(const auto& binding:entry.materials) {
            std::size_t matches{},index{};
            for(std::size_t i=0;i<mesh.materials().size();++i) if(geometry::material_slot_name(mesh,i)==binding.slot) {++matches;index=i;}
            if(matches!=1) return failure("Mesh metadata names a default material for a missing or ambiguous slot: "+binding.slot);
            ordered.emplace_back(index,binding);
        }
        std::ranges::sort(ordered,{},&std::pair<std::size_t,SlotMaterial>::first);
        MeshAsset result{entry.key,entry.path.stem().wstring(),found_mesh->second.mesh};
        for(auto& [index,binding]:ordered) result.materials.push_back(std::move(binding));
        return result;
    } catch (const std::exception& error) {return failure(error.what());}
}
std::span<const std::string> Reader::mesh_notes(std::string_view key) const {
    const auto found=meshes_.find(std::string(key));
    return found==meshes_.end()?std::span<const std::string>{}:std::span<const std::string>(found->second.notes);
}
Result<SkyAsset> Reader::sky_map(std::string_view key) {
    const auto* at=entry(key);
    if(!at || at->kind!=AssetKind::sky)
        return failure(!at?"Sky map key is not a Radiance .hdr asset of this project: "+std::string(key)
            :"Sky map key names "+utf8(at->path.generic_wstring())+", which is not a Radiance .hdr asset.");
    const auto relative=utf8(at->path.generic_wstring());
    SkyAsset asset{at->key,at->path.stem().wstring(),{}};
    if(const auto cached=sky_maps_.find(at->key);cached!=sky_maps_.end()) {asset.map=cached->second;return asset;}
    auto path=resolve(at->path);
    auto bytes=path?read_file(*path,std::size_t(256)<<20):Result<std::string>{std::unexpected(path.error())};
    if(!bytes) return failure("Sky map "+relative+" could not be read: "+bytes.error().message);
    try {asset.map=rendering::decode_radiance_hdr(*bytes);}
    catch(const std::exception& error) {return failure("Sky map "+relative+" is not a valid Radiance HDR file: "+error.what());}
    sky_maps_.emplace(at->key,asset.map);
    return asset;
}
Result<TextureAsset> Reader::texture(std::string_view key) {
    try {
        const auto& item=listed(*this,key,AssetKind::texture);
        auto at=textures_.find(item.key);
        if(at==textures_.end()) {
            // Through the image cache: a mesh import that decoded the same bytes shares its pixels.
            const auto bytes=take(read_file(take(resolve(item.path)),128*1024*1024));const std::array<std::string_view,1> source{bytes};
            at=textures_.emplace(item.key,decode_images(source,&images_).front()).first;
        }
        return TextureAsset{item.key,item.path.stem().wstring(),at->second};
    } catch(const std::exception& error) {return failure(error.what());}
}
Result<MaterialAsset> Reader::material(std::string_view key) {
    try {
        const auto& item=listed(*this,key,AssetKind::material);
        auto asset=material_format::decode(json_format::parse(take(read_file(take(resolve(item.path))))));
        asset.key=item.key;asset.name=item.path.stem().wstring();return asset;
    } catch(const std::exception& error) {return failure(error.what());}
}
Result<std::vector<std::filesystem::path>> Reader::asset_files(std::string_view key) {
    try {
        std::vector<std::filesystem::path> files;std::set<std::filesystem::path> listed;std::set<std::string,std::less<>> visited;
        const auto add=[&](const std::filesystem::path& named) {
            // Keep the catalogue's spelling, so case aliases are one dependency file.
            const auto text=named.generic_wstring();
            const auto at=std::ranges::find_if(entries_,[&](const ProjectEntry& entry) {
                const auto other=entry.path.generic_wstring();
                return CompareStringOrdinal(text.data(),int(text.size()),other.data(),int(other.size()),TRUE)==CSTR_EQUAL;
            });
            if(at==entries_.end()) throw std::runtime_error("Asset file is not in the project catalogue: "+utf8(text));
            if(!listed.insert(at->path).second) return;
            files.push_back(at->path);
            // Without its identity file a copy would get a new key from refresh.
            if(at->kind==AssetKind::mesh || at->kind==AssetKind::material || at->kind==AssetKind::texture || at->kind==AssetKind::sky) {
                auto identity=at->path;identity+=L".asset.json";
                if(!std::filesystem::exists(platform::windows_file_path(take(resolve(identity))))) throw std::runtime_error("Asset identity file is missing: "+utf8(identity.generic_wstring()));
                listed.insert(identity);files.push_back(std::move(identity));
            }
        };
        // Missing/wrong-kind references have no files. The load reports them; an in-memory
        // draft may no longer reference them. An unreadable asset keeps only its own files.
        const std::function<void(std::string_view,std::optional<AssetKind>)> visit=[&](std::string_view asset,std::optional<AssetKind> kind) {
            if(asset.empty() || asset.starts_with("builtin/")) return;
            const auto* at=entry(asset);
            if(!at) {if(!kind) throw std::runtime_error("Missing project asset: "+std::string(asset));return;}
            if((kind && at->kind!=*kind) || !visited.emplace(asset).second) return;
            add(at->path);
            if(at->kind==AssetKind::mesh) {
                if(const auto loaded=mesh(asset)) {
                    for(const auto& dependency:meshes_.at(std::string(asset)).dependencies) add(dependency);
                    for(const auto& slot:loaded->materials) visit(slot.key,AssetKind::material);
                }
            } else if(at->kind==AssetKind::material) {
                if(const auto loaded=material(asset)) for(const auto& texture:loaded->textures) visit(texture,AssetKind::texture);
            }
        };
        visit(key,std::nullopt);return files;
    } catch (const std::exception& error) {return failure(error.what());}
}
}

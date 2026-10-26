#include "application/control_artifacts.hpp"
#include "editor/project.hpp"
#include "editor/session.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "platform/windows_path.hpp"
#include "source/snapshot.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace atelier::application::control_artifacts {
namespace {
using control::Json;using control::Failure;
template<class T> T take(Result<T> value) {if(!value) throw Failure("storage_error",value.error().message);return std::move(*value);}
void take(Result<void> value) {if(!value) throw Failure("storage_error",value.error().message);}
void checkpoint(const std::atomic_bool& cancelled) {if(cancelled.load()) throw Failure("cancelled","Artifact export cancelled");}
void write(const std::filesystem::path& path,std::string_view bytes) {
    const auto native=platform::windows_file_path(path);
    std::filesystem::create_directories(native.parent_path());take(editor::write_file(native,bytes,editor::WriteMode::create));
}
std::string path_text(const std::filesystem::path& path) {return control::utf8(path.generic_wstring());}
bool generated_cache(const std::filesystem::path& relative) {
    auto part=relative.begin();if(part==relative.end() || *part++!=L"Project") return false;
    return part!=relative.end() && *part==L"Cache";
}
Json engine_source_hashes(std::span<const language::SourceFile> files) {
    auto hashes=Json::object();
    for(const auto& file:files) if(file.path.starts_with("engine/"))
        hashes[file.path]=control::sha256(std::as_bytes(std::span(file.source)));
    return hashes;
}
std::string mesh_hash(const assets::MeshAsset& asset) try {
    auto digest=assets::mesh_content_sha256(asset);
    if(!digest) throw Failure("dependency_mismatch",digest.error().message);
    return std::move(*digest);
} catch(const Failure&) {throw;}
catch(const std::exception& error) {throw Failure("storage_error",error.what());}
assets::EngineDependency installed_asset(std::string_view key,assets::AssetKind kind) try {
    auto result=assets::Reader{}.engine_dependency(key,kind);
    if(!result) throw Failure("dependency_mismatch",result.error().message);
    return std::move(*result);
} catch(const Failure&) {throw;}
catch(const std::exception& error) {throw Failure("storage_error",error.what());}
Json engine_assets_json(std::span<const assets::EngineDependency> assets) {
    auto records=Json::array();
    for(const auto& asset:assets) records.push_back({{"key",asset.key},{"kind","mesh"},{"semantic_version",asset.semantic_version},
        {"digest_version",asset.digest_version},{"content_sha256",asset.content_sha256}});
    return {{"version",1},{"records",std::move(records)}};
}
std::vector<assets::EngineDependency> installed_assets(const Json& manifest) {
    if(!manifest.contains("engine_assets")) throw Failure("invalid_request","Repro lacks engine assets; export it again");
    const auto version=[](const Json& value)->std::uint32_t {
        if(!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>()<=0) ||
            value.get<std::uint64_t>()==0 || value.get<std::uint64_t>()>UINT32_MAX)
            throw Failure("invalid_request","Engine asset versions require positive uint32 integers");
        return value.get<std::uint32_t>();
    };
    const auto& object=manifest.at("engine_assets");control::fields(object,{"version","records"});
    if(version(object.at("version"))!=1) throw Failure("invalid_request","Unknown engine assets schema; export it again");
    const auto& records=object.at("records");if(!records.is_array()) throw Failure("invalid_request","Engine assets require a record array");
    std::set<std::string> keys;std::vector<assets::EngineDependency> result;
    for(const auto& record:records) {
        control::fields(record,{"key","kind","semantic_version","digest_version","content_sha256"});
        const auto key=record.at("key").get<std::string>();
        if(!keys.insert(key).second) throw Failure("invalid_request","Duplicate engine asset: "+key);
        const auto semantic=version(record.at("semantic_version")),encoding=version(record.at("digest_version"));
        if(encoding!=assets::mesh_digest_version) throw Failure("invalid_request","Unknown engine asset digest encoding; export it again");
        const auto digest=record.at("content_sha256").get<std::string>();
        if(digest.size()!=64 || !std::ranges::all_of(digest,[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}))
            throw Failure("invalid_request","Invalid engine asset SHA-256: "+key);
        const auto kind=record.at("kind").get<std::string>();
        if(kind!="mesh") throw Failure("dependency_mismatch","Installed engine asset kind differs: "+key);
        auto current=installed_asset(key,assets::AssetKind::mesh);
        if(current.semantic_version!=semantic) throw Failure("dependency_mismatch","Installed engine asset semantic version differs: "+key);
        if(current.content_sha256!=digest) throw Failure("dependency_mismatch","Installed engine asset content differs: "+key);
        result.push_back(std::move(current));
    }
    std::ranges::sort(result,{},&assets::EngineDependency::key);return result;
}
std::string string_field(const Json& object,const char* name) {
    const auto at=object.find(name);return at!=object.end() && at->is_string()?at->get<std::string>():std::string{};
}
// Only schema resource slots are references. Do not inspect arbitrary strings,
// cached drafts, copied files or unknown extension properties.
template<class Reference> void scene_references(const Json& scene,const Reference& reference) {
    if(const auto materials=scene.find("materials");materials!=scene.end() && materials->is_array())
        for(const auto& definition:*materials) if(definition.is_object()) {
            reference(assets::AssetKind::material,string_field(definition,"source"));
            reference(assets::AssetKind::material,string_field(definition,"parent"));
            if(const auto value=definition.find("inline");value!=definition.end() && value->is_object())
                if(const auto textures=value->find("textures");textures!=value->end() && textures->is_object())
                    for(const auto* name:assets::material_format::names)
                        if(const auto texture=textures->find(name);texture!=textures->end() && texture->is_object())
                            reference(assets::AssetKind::texture,string_field(*texture,"source"));
        }
    if(const auto entities=scene.find("entities");entities!=scene.end() && entities->is_array())
        for(const auto& entity:*entities)
            if(const auto components=entity.find("components");components!=entity.end() && components->is_array())
                for(const auto& component:*components) if(component.is_object()) {
                    reference(assets::AssetKind::mesh,string_field(component,"mesh"));
                    reference(assets::AssetKind::sky,string_field(component,"sky_map"));
                }
}
// Only inspect published Scene resource slots and their mesh defaults/material
// inputs, never all copied files. The strict loader still owns project failures;
// this preflight identifies invalid engine keys before it can reject the load.
void preflight_resources(editor::Project& project,const Json& scene) {
    std::set<std::pair<assets::AssetKind,std::string>> references;
    const auto reference=[&](assets::AssetKind kind,const std::string& key) {
        if(!key.empty()) references.emplace(kind,key);
    };
    scene_references(scene,reference);
    // Collect before visiting materials; their ordinal need not follow meshes.
    std::vector<std::string> meshes;
    for(const auto& [kind,key]:references) if(kind==assets::AssetKind::mesh) meshes.push_back(key);
    for(const auto& key:meshes) if(auto mesh=project.assets().mesh(key))
        for(const auto& binding:mesh->materials) reference(assets::AssetKind::material,binding.key);
    std::vector<std::string> materials;
    for(const auto& [kind,key]:references) if(kind==assets::AssetKind::material) materials.push_back(key);
    for(const auto& key:materials) if(auto material=project.assets().material(key))
        for(const auto& texture:material->textures) reference(assets::AssetKind::texture,texture);
    for(const auto& [kind,key]:references) if(key.starts_with("builtin/")) static_cast<void>(installed_asset(key,kind));
}
std::string sky_hash(const rendering::SkyMap& map) {
    const auto bytes=Json{{"width",map.width()},{"height",map.height()},{"scale_exponent",map.scale_exponent()},{"mean",map.mean()},
        {"pixels",control::sha256(std::as_bytes(map.pixels()))},{"cube",control::sha256(std::as_bytes(map.cube()))}}.dump();
    return control::sha256(std::as_bytes(std::span(bytes)));
}
struct Com {
    Com() {if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))) throw Failure("storage_error","Cannot initialize image encoder");}
    ~Com() {CoUninitialize();}
};
void png(const renderer::Capture& image,const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    const auto hr=[](HRESULT result) {if(FAILED(result)) throw Failure("storage_error","PNG encoding failed: "+std::to_string(result));};
    ComPtr<IWICImagingFactory> factory;hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;hr(factory->CreateStream(&stream));hr(stream->InitializeFromFilename(platform::windows_file_path(path).c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;hr(encoder->CreateNewFrame(&frame,nullptr));hr(frame->Initialize(nullptr));
    hr(frame->SetSize(image.extent.width,image.extent.height));auto format=GUID_WICPixelFormat32bppBGRA;hr(frame->SetPixelFormat(&format));
    if(format!=GUID_WICPixelFormat32bppBGRA) throw Failure("storage_error","PNG codec changed BGRA format");
    auto bgra=image.rgba;for(std::size_t pixel=0;pixel<bgra.size();pixel+=4) std::swap(bgra[pixel],bgra[pixel+2]);
    hr(frame->WritePixels(image.extent.height,image.extent.width*4,static_cast<UINT>(bgra.size()),bgra.data()));
    hr(frame->Commit());hr(encoder->Commit());
}
void finish(const std::filesystem::path& directory,Json& manifest,const std::atomic_bool& cancel) {
    checkpoint(cancel);auto files=Json::array();
    for(const auto& item:std::filesystem::recursive_directory_iterator(directory)) if(item.is_regular_file() && !generated_cache(item.path().lexically_relative(directory))) {
        checkpoint(cancel);files.push_back({{"path",path_text(item.path().lexically_relative(directory))},{"bytes",item.file_size()},{"sha256",control::sha256_file(item.path())}});
    }
    checkpoint(cancel);manifest["files"]=std::move(files);manifest["complete"]=true;
    const auto encoded=manifest.dump(2)+"\n";
    if(encoded.size()>assets::json_format::maximum_bytes) throw Failure("resource_limit","Artifact manifest exceeds the 16 MiB file limit");
    write(directory/"manifest.json",encoded);
}
}
Repro snapshot_resources(const editor::Session& document) {
    Repro data;data.scene=take(document.serialize());
    std::map<std::string,assets::EngineDependency,std::less<>> engine;
    std::set<std::string> material_keys;
    const auto reference=[&](assets::AssetKind kind,const std::string& key) {
        if(key.empty()) return;
        if(key.starts_with("builtin/")) {
            const auto at=engine.find(key);
            if(at==engine.end()) engine.emplace(key,installed_asset(key,kind));
            else if(at->second.kind!=kind) throw Failure("dependency_mismatch","Engine asset used with conflicting kinds: "+key);
        } else if(kind==assets::AssetKind::material) material_keys.insert(key);
    };
    // The serialized graph includes retained resource definitions, but not every
    // material a browser happened to load. Ordinary properties are not references.
    scene_references(assets::json_format::parse(data.scene),reference);
    auto& runtime=document.world().runtime();
    for(const auto& entity:take(document.world().members())) for(const auto& component:take(world::components(runtime,entity.id()))) {
        if(auto mesh=presentation::read_static_mesh(runtime,component.id())) {
            reference(assets::AssetKind::mesh,mesh->mesh.source);
            assets::MeshAsset snapshot{mesh->mesh.source,{},mesh->mesh.resolved};
            for(const auto& binding:mesh->mesh.defaults) {
                const auto key=take(material::read_material(runtime,binding.resource.id())).source;
                reference(assets::AssetKind::material,key);snapshot.materials.push_back({binding.slot,key});
            }
            if(snapshot.mesh && std::ranges::none_of(data.meshes,[&](const auto& entry) {
                return entry.key==snapshot.key && entry.mesh==snapshot.mesh && entry.materials==snapshot.materials;
            })) data.meshes.push_back(std::move(snapshot));
        } else if(auto sky=presentation::read_sky_map(runtime,component.id())) {
            reference(assets::AssetKind::sky,sky->source);
            if(sky->resolved && std::ranges::none_of(data.skies,[&](const auto& entry) {
                return entry.first==sky->source && entry.second==sky->resolved;
            })) data.skies.emplace_back(sky->source,sky->resolved);
        }
    }
    for(const auto& key:material_keys) {
        if(document.unavailable(assets::AssetKind::material,key)) continue;
        const auto object=document.loaded_material(key);if(object==ObjectId{}) continue;
        auto asset=take(document.material_asset(object));
        for(const auto& texture:asset.textures) reference(assets::AssetKind::texture,texture);
        data.materials.push_back(std::move(asset));
    }
    // Compare every distinct loaded version before deduplicating the declaration.
    for(const auto& mesh:data.meshes) if(const auto at=engine.find(mesh.key);at!=engine.end())
        if(mesh_hash(mesh)!=at->second.content_sha256)
            throw Failure("dependency_mismatch","Loaded engine mesh differs from its installed producer: "+mesh.key);
    for(auto& [key,dependency]:engine) {static_cast<void>(key);data.engine_assets.push_back(std::move(dependency));}
    return data;
}
void validate_candidate_assets(const editor::Session& candidate,const Json& manifest) {
    const auto expected=installed_assets(manifest),actual=snapshot_resources(candidate).engine_assets;
    for(const auto& item:actual) if(std::ranges::find(expected,item)==expected.end())
        throw Failure("dependency_mismatch","Repro omits or changes an actual engine dependency: "+item.key);
    for(const auto& item:expected) if(std::ranges::find(actual,item)==actual.end())
        throw Failure("dependency_mismatch","Repro declares an unused engine dependency: "+item.key);
}
Json export_repro(const std::filesystem::path& output,Repro data,const std::atomic_bool& cancel) {
    checkpoint(cancel);
    // Keep all worker filesystem operations in the Win32 extended namespace,
    // including metadata reads and directory entries. Public paths keep their
    // original spelling; relative manifest identities never contain the prefix.
    const auto directory=platform::windows_file_path(output);
    if(!data.project.empty()) data.project=platform::windows_file_path(data.project);
    // Resolve existing ancestors by identity, including case aliases/junctions,
    // before creating any output that a source traversal could encounter.
    if(!data.project.empty()) {
        for(const auto& section:data.roots) {
            const auto input=platform::windows_file_path(std::filesystem::canonical(data.project/section));
            for(auto parent=std::filesystem::canonical(directory.parent_path());;) {
                if(std::filesystem::equivalent(platform::windows_file_path(parent),input)) throw Failure("invalid_destination","Repro output cannot be inside a project folder ("+path_text(section)+")");
                const auto next=parent.parent_path();if(next==parent || next.empty()) break;parent=next;
            }
        }
    }
    if(!std::filesystem::create_directory(directory)) throw Failure("storage_error","Artifact directory already exists");
    const auto project=directory/"Project";
    if(data.project.empty()) take(editor::Project::create(output/"Project"));
    else {
        std::uintmax_t bytes{};std::size_t files{};Json input_files=Json::array();
        const auto link=[](const std::filesystem::path& path) {return (GetFileAttributesW(platform::windows_file_path(path).c_str())&FILE_ATTRIBUTE_REPARSE_POINT)!=0;};
        const auto copy=[&](const std::filesystem::path& item) {
            checkpoint(cancel);
            if(link(item)) throw Failure("dependency_mismatch","Repro export does not follow links");
            const auto relative=item.lexically_relative(data.project);
            const auto target=platform::windows_file_path(project/relative);
            if(std::filesystem::is_directory(item)) {std::filesystem::create_directories(target);return;}
            if(!std::filesystem::is_regular_file(item)) throw Failure("dependency_mismatch","Unsupported project file");
            const auto length=std::filesystem::file_size(item);bytes+=length;
            if(++files>10000 || length>256*1024*1024 || bytes>1024ull*1024*1024) throw Failure("resource_limit","Repro exceeds 10000 files / 1 GiB / 256 MiB per file");
            const auto stamp=std::filesystem::last_write_time(item);const auto hash=control::sha256_file(item);
            std::filesystem::create_directories(target.parent_path());
            if(!std::filesystem::copy_file(item,target)) throw Failure("storage_error","Cannot copy project dependency");
            if(stamp!=std::filesystem::last_write_time(item) || length!=std::filesystem::file_size(item) ||
                hash!=control::sha256_file(item) || hash!=control::sha256_file(project/relative))
                throw Failure("dependency_mismatch","Project dependency changed during export");
            input_files.push_back({{"path",path_text(relative)},{"sha256",hash}});
        };
        // Config and Source are copied whole: the configuration is rewritten and the
        // published code replaces Source below. Other folders hold assets; only the
        // files of the scene's assets and their references are copied.
        for(const auto* section:{"Config","Source"}) {
            const auto root=data.project/section;
            if(link(root)) throw Failure("dependency_mismatch","Project sections cannot be links");
            std::filesystem::create_directories(project/section);
            for(const auto& item:std::filesystem::recursive_directory_iterator(root)) copy(item.path());
        }
        const auto sections=files;
        for(const auto& relative:data.files) {
            if(const auto top=*relative.begin();top==L"Config" || top==L"Source") continue;
            for(auto parent=relative.parent_path();!parent.empty();parent=parent.parent_path())
                if(link(data.project/parent)) throw Failure("dependency_mismatch","Repro export does not follow links");
            copy(data.project/relative);
        }
        std::filesystem::create_directories(project/"Content");
        // Recheck the copied set after copying, including additions/removals in Config and Source.
        std::size_t current_files{};
        for(const auto* section:{"Config","Source"}) for(const auto& item:std::filesystem::recursive_directory_iterator(data.project/section))
            if(item.is_regular_file()) ++current_files;
        if(current_files!=sections) throw Failure("dependency_mismatch","Project file set changed during export");
        for(const auto& file:input_files) {checkpoint(cancel);if(control::sha256_file(data.project/control::wide(file.at("path").get<std::string>()))!=file.at("sha256").get<std::string>()) throw Failure("dependency_mismatch","Project changed during export");}
        data.manifest["input_files"]=std::move(input_files);
    }
    checkpoint(cancel);
    // Reproduce the code actually published into this Runtime. Drafts are separate.
    // Saved edits/additions may not have published. Keep them outside the
    // runnable Source tree, then materialize exactly the live inputs.
    std::unordered_map<std::string,const language::SourceFile*> published;
    if(data.sources) for(const auto& file:data.sources->files) published.emplace(file.path,&file);
    if(data.ui_source) published.emplace(data.ui_source->path,&*data.ui_source);
    std::vector<std::filesystem::path> copied_code;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(project/"Source"))
        if(entry.is_regular_file() && entry.path().extension()==L".source") copied_code.push_back(entry.path());
    for(const auto& path:copied_code) {
        checkpoint(cancel);const auto relative=path.lexically_relative(project);
        const auto found=published.find(path_text(relative));
        if(found!=published.end() && std::filesystem::file_size(path)==found->second->source.size() &&
            take(assets::read_file(path))==found->second->source) {std::filesystem::remove(path);continue;}
        const auto archive=platform::windows_file_path(directory/"unpublished"/relative);
        std::filesystem::create_directories(archive.parent_path());std::filesystem::rename(path,archive);
    }
    const auto publish_source=[&](const language::SourceFile& file) {
        const std::filesystem::path relative=control::wide(file.path);
        if(relative.is_absolute() || relative.empty() || *relative.begin()!=L"Source" || relative.lexically_normal()!=relative)
            throw Failure("dependency_mismatch","Published source is not project relative");
        write(project/relative,file.source);
    };
    // Engine definitions stay installation-owned. Pin the published inputs rather
    // than copying them into project scope or silently using a newer installation.
    data.manifest["engine_sources"]=data.sources?engine_source_hashes(data.sources->files):
        engine_source_hashes(take(source::engine_sources(source::default_engine_root())));
    if(data.sources) for(const auto& file:data.sources->files)
        if(!file.path.starts_with("engine/")) publish_source(file);
    if(data.ui_source) publish_source(*data.ui_source);
    const auto scene_path=std::filesystem::path("Content/Scenes")/("Control-"+control::unique_id()+".scene");
    write(project/scene_path,data.scene);
    auto config=assets::json_format::parse(take(assets::read_file(project/"Config/Project.json")));
    if(data.ui_source) config["ui_source"]=data.ui_source->path;else config.erase("ui_source");
    config["startup_scene"]=path_text(scene_path);take(editor::write_file(project/"Config/Project.json",config.dump(2)+"\n"));
    auto clone=take(editor::Project::open(output/"Project"));auto resources=Json::array();
    for(const auto& material:data.materials) {
        checkpoint(cancel);
        for(std::size_t i=0;i<material.textures.size();++i) if(!material.textures[i].empty()) {
            const auto copied=take(clone.assets().texture(material.textures[i])).image;const auto& expected=material.description.textures[i].image;
            if(!expected || copied->width()!=expected->width() || copied->height()!=expected->height() ||
                control::sha256(std::as_bytes(copied->pixels()))!=control::sha256(std::as_bytes(expected->pixels())))
                throw Failure("dependency_mismatch","Copied texture differs from the loaded material: "+material.textures[i]);
        }
        // Publish the live draft only in this independent bundle, never in the source project.
        take(clone.save_material(material));
    }
    std::set<std::string> recorded;
    for(const auto& mesh:data.meshes) {
        checkpoint(cancel);auto loaded=take(clone.assets().mesh(mesh.key));
        const auto expected=mesh_hash(mesh);
        if(!loaded.mesh || mesh_hash(loaded)!=expected) throw Failure("dependency_mismatch","Copied dependency differs from the loaded mesh content or slot defaults: "+mesh.key);
        if(mesh.key.starts_with("builtin/")) {
            const auto at=std::ranges::find(data.engine_assets,mesh.key,&assets::EngineDependency::key);
            if(at==data.engine_assets.end() || *at!=installed_asset(mesh.key,assets::AssetKind::mesh) || at->content_sha256!=expected)
                throw Failure("dependency_mismatch","Frozen engine declaration differs from the loaded mesh: "+mesh.key);
        } else if(recorded.insert(mesh.key).second)
            resources.push_back({{"key",mesh.key},{"digest_version",assets::mesh_digest_version},{"content_sha256",expected}});
    }
    for(const auto& [key,map]:data.skies) {
        checkpoint(cancel);const auto loaded=take(clone.assets().sky_map(key)).map;
        const auto expected=sky_hash(*map);
        if(!loaded || sky_hash(*loaded)!=expected) throw Failure("dependency_mismatch","Copied dependency differs from the loaded sky map: "+key);
        if(recorded.insert(key).second) resources.push_back({{"key",key},{"content_sha256",expected}});
    }
    data.manifest["engine_assets"]=engine_assets_json(data.engine_assets);
    data.manifest["resources"]=std::move(resources);data.manifest["project"]="Project";data.manifest["scene"]=path_text(scene_path);
    if(!data.drafts.empty()) write(directory/"drafts.json",data.drafts.dump(2)+"\n");
    finish(directory,data.manifest,cancel);
    return {{"directory",path_text(output)},{"manifest",path_text(output/"manifest.json")},{"project",path_text(output/"Project")}};
}
Json validate_repro(const std::filesystem::path& input) {
    const auto directory=platform::windows_file_path(input);
    const auto text=take(assets::read_file(directory/"manifest.json"));
    Json manifest;
    // Format-limit and duplicate-field failures are input errors, not I/O failures.
    try {manifest=assets::json_format::parse(text);}
    catch(const std::runtime_error& error) {throw Failure("invalid_request",error.what());}
    if(manifest.value("kind","")!="repro" || !manifest.value("complete",false) ||
        manifest.at("protocol")!=control::protocol_version || manifest.at("project")!="Project")
        throw Failure("invalid_request","Not a completed compatible repro bundle");
    static_cast<void>(installed_assets(manifest));
    if(!manifest.contains("engine_sources") || !manifest.at("engine_sources").is_object())
        throw Failure("invalid_request","Repro lacks engine Source dependencies; export it again");
    const auto engine=source::engine_sources(source::default_engine_root());
    if(!engine || engine_source_hashes(*engine)!=manifest.at("engine_sources"))
        throw Failure("dependency_mismatch","Installed engine Source differs from the published repro inputs");
    const auto& files=manifest.at("files");
    if(!files.is_array() || files.size()>11000) throw Failure("invalid_request","Invalid repro file list");
    std::unordered_map<std::string,const Json*> expected;
    for(const auto& file:files) {
        const auto name=file.at("path").get<std::string>();const std::filesystem::path path=control::wide(name);
        if(path.empty() || path.has_root_path() || path.lexically_normal()!=path || *path.begin()==L".." ||
            name.find(':')!=std::string::npos || path==L"manifest.json" || generated_cache(path) || !expected.emplace(path_text(path),&file).second)
            throw Failure("invalid_request","Invalid or duplicate bundle path");
    }
    // Reject additions as well as missing/modified inputs. Generated Cache is
    // outside the publication and may be created when opening the project.
    for(auto it=std::filesystem::recursive_directory_iterator(directory);it!=std::filesystem::recursive_directory_iterator();++it) {
        const auto relative=it->path().lexically_relative(directory);
        if(relative==L"manifest.json") continue;
        if(GetFileAttributesW(platform::windows_file_path(it->path()).c_str())&FILE_ATTRIBUTE_REPARSE_POINT)
            throw Failure("dependency_mismatch","Repro bundle cannot contain links");
        if(generated_cache(relative)) {if(it->is_directory()) it.disable_recursion_pending();continue;}
        if(it->is_directory()) continue;
        const auto found=expected.find(path_text(relative));
        if(!it->is_regular_file() || found==expected.end() || it->file_size()!=found->second->at("bytes").get<std::uintmax_t>() ||
            control::sha256_file(it->path())!=found->second->at("sha256").get<std::string>())
            throw Failure("dependency_mismatch","Repro bundle file set or content changed");
        expected.erase(found);
    }
    if(!expected.empty()) throw Failure("dependency_mismatch","Repro bundle file is missing");
    auto project=take(editor::Project::open(input/"Project"));
    const auto scene=take(project.resolve(project.startup_scene()));
    preflight_resources(project,assets::json_format::parse(take(assets::read_file(scene))));
    return manifest;
}
Json write_samples(const std::filesystem::path& output,std::vector<Sample> samples,Json manifest,const std::atomic_bool& cancel) {
    const auto directory=platform::windows_file_path(output);
    checkpoint(cancel);Com com;
    if(!std::filesystem::create_directory(directory)) throw Failure("storage_error","Artifact directory already exists");
    manifest["samples"]=Json::array();std::size_t index{};
    for(auto& sample:samples) {
        checkpoint(cancel);const auto name="sample-"+std::to_string(++index);
        const auto& pixels=sample.pixels;
        const auto count=std::uint64_t(pixels.extent.width)*pixels.extent.height;
        if(!count || count*4!=pixels.rgba.size() || count!=pixels.depth.size()) throw Failure("render_failure","Capture size does not match accepted extent");
        png(pixels,directory/(name+".png"));checkpoint(cancel);
        write(directory/(name+".depth.f32"),{reinterpret_cast<const char*>(pixels.depth.data()),pixels.depth.size()*sizeof(float)});
        sample.metadata["color"]=name+".png";sample.metadata["depth"]=name+".depth.f32";
        sample.metadata["extent"]={pixels.extent.width,pixels.extent.height};
        sample.metadata["depth_format"]="float32-le, top-left, tightly packed, reversed-Z near=1 background=0; camera.far_depth is the geometry clipping boundary; unjittered";
        sample.metadata["color_format"]="sRGB RGBA8, top-left; complete editor client area";
        write(directory/(name+".json"),sample.metadata.dump(2)+"\n");manifest["samples"].push_back(std::move(sample.metadata));
    }
    finish(directory,manifest,cancel);
    return {{"directory",path_text(output)},{"manifest",path_text(output/"manifest.json")},{"samples",manifest["samples"]}};
}
}

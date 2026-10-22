#include "editor/project.hpp"
#include "editor/session.hpp"
#include "platform/windows_path.hpp"
#include "editor/mesh_import.hpp"
#include "editor/json_format.hpp"
#include "physics/physics.hpp"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <bit>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_set>

namespace atelier::editor {
namespace {
using json_format::Json;
auto failure(std::string text) { return std::unexpected(Error{ErrorCode::invalid_storage,std::move(text)}); }
std::string identity() {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create asset identity.");
    wchar_t text[40]{}; StringFromGUID2(id,text,40); return utf8(text);
}
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(*value);
}
Json read_json(const std::filesystem::path& path) { return json_format::parse(take(read_file(path))); }
bool within(const std::filesystem::path& root,const std::filesystem::path& path) {
    const auto relative=path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin()!=L"..";
}

}
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if (!count) throw std::runtime_error("Invalid UTF-16 text.");
    std::string result(count,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr); return result;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if (!count) throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring result(count,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count); return result;
}
Result<std::string> read_file(const std::filesystem::path& path,std::size_t maximum) {
    std::ifstream file(platform::windows_file_path(path),std::ios::binary|std::ios::ate);
    if (!file) return failure("Cannot read "+utf8(path.wstring()));
    const auto size=file.tellg(); if (size<0 || static_cast<std::uint64_t>(size)>maximum) return failure("File exceeds its size limit.");
    std::string bytes(static_cast<std::size_t>(size),'\0'); file.seekg(0);
    if (!file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))) return failure("Could not read the complete file.");
    return bytes;
}
Result<void> write_file(const std::filesystem::path& path,std::string_view text,WriteMode mode) {
    std::filesystem::path temporary;
    try {
        const auto native=platform::windows_file_path(path);
        std::filesystem::create_directories(native.parent_path());
        temporary=native; temporary+=L"."+wide(identity())+L".tmp";
        const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) return failure("Cannot create temporary file for "+utf8(path.wstring())+" (Windows error "+std::to_string(GetLastError())+").");
        DWORD written{}; const bool ok=text.size()<=MAXDWORD && WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
        const auto write_error=GetLastError(); CloseHandle(file);
        const DWORD flags=MOVEFILE_WRITE_THROUGH|(mode==WriteMode::replace?MOVEFILE_REPLACE_EXISTING:0);
        if (!ok || !MoveFileExW(temporary.c_str(),native.c_str(),flags)) {
            const auto error=ok?GetLastError():write_error;
            DeleteFileW(temporary.c_str()); return failure("Cannot save "+utf8(path.wstring())+" (Windows error "+std::to_string(error)+").");
        }
        return {};
    } catch (const std::exception& error) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        return failure(error.what());
    }
}
Result<std::filesystem::path> Project::resolve(const std::filesystem::path& relative) const {
    try {
        if (relative.empty() || relative.has_root_path()) return failure("Expected a project-relative path.");
        for (const auto& part:relative) if (part==L".." || part.wstring().find(L':')!=std::wstring::npos ||
            std::ranges::any_of(part.wstring(),[](wchar_t c){return c<32;})) return failure("Invalid project-relative path.");
        auto path=std::filesystem::weakly_canonical(platform::windows_file_path(root_/relative));
        if (!within(root_,path)) return failure("Project path escapes its root through a link.");
        return path;
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<Project> Project::create(const std::filesystem::path& folder) {
    try {
        if (folder.empty()) return failure("Choose an empty project folder.");
        const auto native=platform::windows_file_path(folder);
        if (std::filesystem::exists(native) && (!std::filesystem::is_directory(native) || !std::filesystem::is_empty(native)))
            return failure("Create Project requires an empty folder; existing files were not changed.");
        for (const auto* directory:{L"Config",L"Content/Scenes",L"Source",L"Cache"}) std::filesystem::create_directories(platform::windows_file_path(folder/directory));
        const Json config={{"version",1},{"name",utf8(std::filesystem::canonical(native).filename().wstring())},{"startup_scene","Content/Scenes/Main.scene"}};
        Session empty_scene;take(write_file(folder/"Content/Scenes/Main.scene",take(empty_scene.serialize()),WriteMode::create));
        take(write_file(folder/".gitignore","/Cache/\n",WriteMode::create));
        take(write_file(folder/"Config/Project.json",config.dump(2)+"\n",WriteMode::create)); // Marker is published last.
        return open(folder);
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<Project> Project::open(const std::filesystem::path& folder) {
    try {
        Project project; project.root_=std::filesystem::canonical(platform::windows_file_path(folder));
        const auto config=read_json(take(project.resolve("Config/Project.json")));
        json_format::fields(config,{"version","name","startup_scene","ui_source"});
        if (json_format::identity(config.at("version"))!=1) return failure("Unsupported project format version.");
        project.name_=wide(config.at("name").get<std::string>());
        project.startup_=wide(config.at("startup_scene").get<std::string>());
        if (config.contains("ui_source")) {
            project.ui_source_=wide(config.at("ui_source").get<std::string>());
            if (!project.ui_source_.empty() && (*project.ui_source_.begin()!=L"Source" ||
                !project.ui_source_.filename().wstring().ends_with(L".ui.source"))) return failure("Project UI must be a .ui.source file inside Source.");
            if (!project.ui_source_.empty()) take(project.resolve(project.ui_source_));
        }
        if (project.name_.empty() || project.name_.size()>256 || std::ranges::any_of(project.name_,[](wchar_t c){return c<32 || c==127;}) ||
            !is_scene_path(project.startup_)) return failure("Invalid project name or startup scene.");
        take(project.resolve(project.startup_));
        for (const auto* directory:{L"Config",L"Content",L"Source"}) if (!std::filesystem::is_directory(platform::windows_file_path(take(project.resolve(directory))))) return failure("Project is missing Config, Content or Source.");
        take(project.refresh()); return project;
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<std::vector<std::filesystem::path>> Project::source_paths() const {
    try {
        std::vector<std::filesystem::path> paths;
        const auto native_root=platform::windows_file_path(root_);
        const auto directory=platform::windows_file_path(take(resolve("Source")));
        for (const auto& entry:std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_symlink() || !entry.is_regular_file()) continue;
            if (entry.path().extension()!=L".source" || entry.path().filename().wstring().ends_with(L".ui.source")) continue;
            const auto relative=entry.path().lexically_relative(native_root);
            take(resolve(relative));
            if (paths.size()>=256) return failure("Project exceeds the 256 Source file limit.");
            paths.push_back(relative);
        }
        std::ranges::sort(paths);
        return paths;
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<void> Project::create_source(const std::filesystem::path& relative,std::string_view text) {
    if (relative.empty() || *relative.begin()!=L"Source" || relative.extension()!=L".source")
        return failure("Create source files inside the project's Source directory.");
    const auto path=resolve(relative);if (!path) return std::unexpected(path.error());
    if (text.size()>1024*1024) return failure("Source file exceeds the 1 MiB limit.");
    try { static_cast<void>(wide(text)); }
    catch (const std::exception& error) { return failure(error.what()); }
    return write_file(*path,text,WriteMode::create);
}
Result<void> Project::set_ui_source(const std::filesystem::path& relative) {
    if (!relative.empty() && (*relative.begin()!=L"Source" || !relative.filename().wstring().ends_with(L".ui.source")))
        return failure("Project UI must be a .ui.source file inside Source.");
    if (!relative.empty()) {auto path=resolve(relative);if (!path) return std::unexpected(path.error());}
    try {
        const auto path=take(resolve("Config/Project.json"));auto config=read_json(path);
        json_format::fields(config,{"version","name","startup_scene","ui_source"});
        if (json_format::identity(config.at("version"))!=1 || config.at("name")!=utf8(name_) ||
            config.at("startup_scene")!=utf8(startup_.generic_wstring()) ||
            config.value("ui_source",std::string{})!=utf8(ui_source_.generic_wstring()))
            return failure("Project configuration changed on disk. Reopen the project before changing its UI entry.");
        if (relative.empty()) config.erase("ui_source");else config["ui_source"]=utf8(relative.generic_wstring());
        auto saved=write_file(path,config.dump(2)+"\n");if (!saved) return saved;
        ui_source_=relative;return {};
    } catch (const std::exception& error) {return failure(error.what());}
}
Result<std::filesystem::path> Project::rename(const std::filesystem::path& relative,std::wstring_view name) {
    if(relative.empty() || std::distance(relative.begin(),relative.end())<2) return failure("Top-level project folders cannot be renamed.");
    if(name.empty() || name.size()>128 || name.find_first_of(L"\\/:*?\"<>|")!=std::wstring_view::npos || name.back()==L'.' || name.back()==L' ' ||
       name==L"." || name==L"..")
        return failure("Enter a file name without \\ / : * ? \" < > | and not ending in a dot or space.");
    if(relative==startup_ || relative==ui_source_) return failure("The startup scene and UI source are configured by path; change Config first.");
    const auto source=resolve(relative);if(!source) return std::unexpected(source.error());
    const auto target_relative=relative.parent_path()/std::filesystem::path(name);
    if(target_relative==relative) return relative;
    const auto target=resolve(target_relative);if(!target) return std::unexpected(target.error());
    const auto native_source=platform::windows_file_path(*source),native_target=platform::windows_file_path(*target);
    std::error_code error;
    if(!std::filesystem::exists(native_source,error)) return failure("The asset no longer exists on disk.");
    // Case-only renames are the same file on Windows; anything else must not overwrite.
    if(std::filesystem::exists(native_target,error) && !std::filesystem::equivalent(native_source,native_target,error))
        return failure("Another file or folder already has that name.");
    const auto sidecar=[&](const std::filesystem::path& path){auto result=path;result+=L".asset.json";return result;};
    const bool has_sidecar=!std::filesystem::is_directory(native_source,error) && std::filesystem::exists(sidecar(native_source),error);
    std::filesystem::rename(native_source,native_target,error);
    if(error) return failure("The file could not be renamed; it may be open in another program.");
    if(has_sidecar) {
        std::filesystem::rename(sidecar(native_source),sidecar(native_target),error);
        if(error) {
            std::error_code undo;std::filesystem::rename(native_target,native_source,undo);
            return failure("The asset identity file could not be moved; the rename was undone.");
        }
    }
    if(auto refreshed=refresh();!refreshed) return std::unexpected(refreshed.error());
    return target_relative;
}
Result<void> Project::refresh() {
    try {
        std::vector<ProjectEntry> entries;
        std::unordered_set<std::string> keys;
        const auto native_root=platform::windows_file_path(root_);
        for (const auto* directory:{L"Content",L"Source",L"Config",L"Examples"}) {
            const auto root=platform::windows_file_path(take(resolve(directory)));
            if(std::wstring_view(directory)==L"Examples" && !std::filesystem::exists(root)) continue;
            if (entries.size()>=10000) return failure("Project exceeds the current 10000-entry catalog limit.");
            entries.push_back({directory,utf8(directory),AssetKind::folder});
            for (const auto& entry:std::filesystem::recursive_directory_iterator(root)) {
                if (entry.is_symlink()) continue;
                const auto relative=entry.path().lexically_relative(native_root);
                take(resolve(relative));
                const auto filename=entry.path().filename().wstring();
                if (filename.ends_with(L".asset.json") || filename.ends_with(L".meta") || filename.ends_with(L".tmp")) continue;
                if (entries.size()>=10000) return failure("Project exceeds the current 10000-entry catalog limit.");
                auto kind=entry.is_directory()?AssetKind::folder:is_mesh_path(entry.path())?AssetKind::mesh:
                    entry.path().extension()==L".material"?AssetKind::material:is_texture_path(entry.path())?AssetKind::texture:
                    entry.path().extension()==L".scene"?AssetKind::scene:std::wstring_view(directory)==L"Source"?AssetKind::source:AssetKind::file;
                std::string key=utf8(relative.generic_wstring());
                if (kind==AssetKind::mesh || kind==AssetKind::material || kind==AssetKind::texture) {
                    auto meta_relative=relative; meta_relative+=L".asset.json";
                    auto metadata=take(resolve(meta_relative));
                    if (!std::filesystem::exists(metadata)) {
                        const auto created=write_file(metadata,Json{{"version",1},{"id",identity()}}.dump(2)+"\n",WriteMode::create);
                        if (!created && !std::filesystem::exists(metadata)) return created;
                    }
                    const auto info=read_json(metadata);json_format::fields(info,{"version","id"});
                    if (json_format::identity(info.at("version"))!=1) return failure("Unsupported resource metadata version.");
                    const auto raw=wide(info.at("id").get<std::string>());GUID id{};
                    if (raw.size()!=38 || raw.front()!=L'{' || raw.back()!=L'}' || FAILED(CLSIDFromString(raw.c_str(),&id)))
                        return failure("Invalid resource identity metadata.");
                    wchar_t canonical[40]{};StringFromGUID2(id,canonical,40);key="project/"+utf8(canonical);
                    if (!keys.insert(key).second) return failure("Duplicate resource identity; import or create an independent asset.");
                }
                entries.push_back({relative,std::move(key),kind,kind==AssetKind::folder?0:entry.file_size(),
                    kind!=AssetKind::folder?entry.last_write_time():std::filesystem::file_time_type{}});
            }
        }
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return a.path<b.path; });
        if (entries==entries_) return {};
        std::unordered_map<std::string,std::size_t> mesh_entries;
        for (std::size_t i=0;i<entries.size();++i) if (entries[i].kind==AssetKind::mesh) mesh_entries.emplace(entries[i].key,i);
        std::map<std::filesystem::path,const ProjectEntry*> previous_files,next_files;
        for(const auto& entry:entries_) previous_files.emplace(entry.path,&entry);
        for(const auto& entry:entries) next_files.emplace(entry.path,&entry);
        std::erase_if(textures_,[&](const auto& cached) {
            const auto before=std::ranges::find(entries_,cached.first,&ProjectEntry::key);
            const auto after=std::ranges::find(entries,cached.first,&ProjectEntry::key);
            return before==entries_.end() || after==entries.end() || before->bytes!=after->bytes || before->modified!=after->modified;
        });
        std::erase_if(meshes_,[&](const auto& cached) {
            const auto before=mesh_entries_.find(cached.first),after=mesh_entries.find(cached.first);
            if (after==mesh_entries.end()) return true;
            if (before==mesh_entries_.end()) return false; // Prepared, newly published import.
            const auto& old=entries_[before->second];const auto& next=entries[after->second];
            if(old.bytes!=next.bytes || old.modified!=next.modified) return true;
            for(const auto& path:cached.second.dependencies) {
                const auto a=previous_files.find(path),b=next_files.find(path);
                if(a==previous_files.end() || b==next_files.end() || a->second->bytes!=b->second->bytes ||
                    a->second->modified!=b->second->modified) return true;
            }
            return false;
        });
        std::erase_if(images_.entries,[](const auto& entry){return entry.second.expired();});
        entries_=std::move(entries);mesh_entries_=std::move(mesh_entries);++revision_;return {};
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<MeshAsset> Project::mesh(std::string_view key) {
    if (const auto* builtin=mesh_asset(key)) return *builtin;
    try {
        const auto found=mesh_entries_.find(std::string(key));
        if (found==mesh_entries_.end()) return failure("Missing mesh resource: "+std::string(key));
        const auto& entry=entries_[found->second];
        auto found_mesh=meshes_.find(entry.key);
        if(found_mesh==meshes_.end()) {
            auto loaded=load_mesh(take(resolve(entry.path)),&images_);CachedMesh cached{loaded.mesh,{},std::move(loaded.notes)};
            for(const auto& file:loaded.files) if(file.relative!=entry.path.filename()) cached.dependencies.push_back(entry.path.parent_path()/file.relative);
            found_mesh=meshes_.emplace(entry.key,std::move(cached)).first;
        }
        return MeshAsset{entry.key,entry.path.stem().wstring(),found_mesh->second.mesh};
    } catch (const std::exception& error) { return failure(error.what()); }
}
std::span<const std::string> Project::mesh_notes(std::string_view key) const {
    const auto found=meshes_.find(std::string(key));
    return found==meshes_.end()?std::span<const std::string>{}:std::span<const std::string>(found->second.notes);
}
Result<std::string> Project::import_mesh(const std::filesystem::path& source) {
    std::vector<std::filesystem::path> created;
    std::string resource;
    try {
        if (!is_mesh_path(source)) return failure("Import Mesh accepts Wavefront OBJ or glTF 2.0 (.gltf) geometry and its dependencies.");
        auto loaded=load_mesh(std::filesystem::absolute(source),&images_);
        take(refresh());
        // A dependency bundle gets its own namespace. Existing single-file OBJ
        // imports retain their established location and exclusive-create contract.
        auto relative=std::filesystem::path("Content/Meshes");
        if(loaded.files.size()>1) relative/=source.stem();
        const auto target_relative=relative/source.filename();
        auto meta_relative=target_relative;meta_relative+=L".asset.json";
        auto metadata=take(resolve(meta_relative));
        if(std::filesystem::exists(metadata)) return failure("Mesh metadata already exists at the import destination.");
        for(const auto& file:loaded.files) if(std::filesystem::exists(take(resolve(relative/file.relative))))
            return failure("A file already exists at the import destination; no files were changed.");
        resource="project/"+identity();CachedMesh cached{loaded.mesh,{},std::move(loaded.notes)};
        for(const auto& file:loaded.files) if(file.relative!=source.filename()) cached.dependencies.push_back(relative/file.relative);
        meshes_.emplace(resource,std::move(cached));
        created.reserve(loaded.files.size()+1);
        for(const auto& file:loaded.files) {
            auto path=take(resolve(relative/file.relative));take(write_file(path,file.bytes,WriteMode::create));created.push_back(std::move(path));
        }
        take(write_file(metadata,Json{{"version",1},{"id",resource.substr(8)}}.dump(2)+"\n",WriteMode::create));created.push_back(std::move(metadata));
        take(refresh());return resource;
    } catch (const std::exception& error) {
        if(!resource.empty()) meshes_.erase(resource);
        std::string message=error.what();
        for(auto it=created.rbegin();it!=created.rend();++it) {
            std::error_code cleanup;std::filesystem::remove(*it,cleanup);
            if(cleanup) message+="; import cleanup failed: "+cleanup.message();
        }
        return failure(std::move(message));
    }
}

namespace {
std::filesystem::path collision_path(std::string_view key, math::Vector3 scale) {
    // The key is a validated project GUID; float bits keep distinct import scales apart.
    std::ostringstream name;
    name << "Cache/Physics/" << key.substr(8) << '-' << std::hex
        << std::bit_cast<std::uint32_t>(scale.x) << '-'
        << std::bit_cast<std::uint32_t>(scale.y) << '-'
        << std::bit_cast<std::uint32_t>(scale.z) << ".jolt-shape";
    return name.str();
}
}
Result<std::filesystem::path> Project::cook_collision(std::string_view key, math::Vector3 scale) {
    if (!key.starts_with("project/")) return failure("Collision Cook requires a project mesh identity.");
    auto asset = mesh(key);
    if (!asset) return std::unexpected(asset.error());
    auto cooked = physics::cook_triangle_mesh(asset->mesh, scale, key);
    if (!cooked) return std::unexpected(cooked.error());
    auto path = resolve(collision_path(key, scale));
    if (!path) return std::unexpected(path.error());
    if (auto written = write_file(*path, *cooked); !written) return std::unexpected(written.error());
    return *path;
}
Result<std::shared_ptr<const std::string>> Project::collision(std::string_view key, math::Vector3 scale) {
    if (!key.starts_with("project/")) return failure("Collision asset requires a project mesh identity.");
    auto asset = mesh(key);
    if (!asset) return std::unexpected(asset.error());
    auto path = resolve(collision_path(key, scale));
    if (!path) return std::unexpected(path.error());
    if (auto bytes = read_file(*path, 128ull * 1024 * 1024 + 8192);
        bytes && physics::validate_cooked_triangle_mesh(*bytes, asset->mesh, scale, key))
        return std::make_shared<const std::string>(std::move(*bytes));
    // The cache is derived data: a missing or stale entry is rebuilt from the source mesh.
    auto cooked = physics::cook_triangle_mesh(asset->mesh, scale, key);
    if (!cooked) return std::unexpected(cooked.error());
    static_cast<void>(write_file(*path, *cooked)); // A failed write only costs the next load another Cook.
    return std::make_shared<const std::string>(std::move(*cooked));
}
}

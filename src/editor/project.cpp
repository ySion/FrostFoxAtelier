#include "editor/project.hpp"
#include "editor/json_format.hpp"
#include "physics/physics.hpp"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <bit>
#include <fstream>
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
geometry::MeshRef parse_obj(std::string text) {
    std::istringstream input(std::move(text)); input.imbue(std::locale::classic());
    std::vector<std::array<float,3>> positions,normals;
    std::vector<geometry::Vertex> vertices;
    std::string line,kind;
    const auto index=[](std::string_view text,std::size_t size) {
        int value{}; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
        if (error!=std::errc{} || end!=text.data()+text.size() || !value) throw std::runtime_error("Invalid OBJ index.");
        const auto resolved=value>0?value-1:static_cast<int>(size)+value;
        if (resolved<0 || static_cast<std::size_t>(resolved)>=size) throw std::runtime_error("OBJ index is out of range.");
        return static_cast<std::size_t>(resolved);
    };
    while (std::getline(input,line)) {
        if (const auto comment=line.find('#');comment!=line.npos) line.resize(comment);
        std::istringstream row(line); row.imbue(std::locale::classic()); if (!(row>>kind)) continue;
        if (kind=="v" || kind=="vn") {
            std::array<float,3> v{}; if (!(row>>v[0]>>v[1]>>v[2])) throw std::runtime_error("Invalid OBJ vertex/normal.");
            auto& target=kind=="v"?positions:normals;
            if (target.size()>=1000000) throw std::runtime_error("OBJ exceeds the import vertex limit.");
            target.push_back(v);
        } else if (kind=="f") {
            std::array<geometry::Vertex,3> face{}; std::array<bool,3> has_normal{};
            std::string token;
            for (std::size_t i=0;i<3;++i) {
                if (!(row>>token)) throw std::runtime_error("OBJ faces must be triangles. Triangulate before importing.");
                const auto slash=token.find('/'); std::ranges::copy(positions.at(index(std::string_view(token).substr(0,slash),positions.size())),face[i].position);
                const auto normal=slash==token.npos?token.npos:token.find('/',slash+1);
                if (normal!=token.npos && normal+1<token.size()) {
                    std::ranges::copy(normals.at(index(std::string_view(token).substr(normal+1),normals.size())),face[i].normal); has_normal[i]=true;
                }
            }
            if (row>>token) throw std::runtime_error("OBJ faces must be triangles. Triangulate before importing.");
            std::array<float,3> a{},b{},normal{};
            for (std::size_t i=0;i<3;++i) { a[i]=face[1].position[i]-face[0].position[i]; b[i]=face[2].position[i]-face[0].position[i]; }
            normal={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
            for (std::size_t i=0;i<3;++i) if (!has_normal[i]) std::ranges::copy(normal,face[i].normal);
            if (vertices.size()>3000000-3) throw std::runtime_error("OBJ exceeds the triangle limit.");
            vertices.insert(vertices.end(),face.begin(),face.end());
        } else if (kind!="vt" && kind!="o" && kind!="g" && kind!="s" && kind!="usemtl" && kind!="mtllib")
            throw std::runtime_error("Unsupported OBJ record: "+kind);
    }
    return std::make_shared<const geometry::Mesh>(std::move(vertices));
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
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) return failure("Cannot read "+utf8(path.wstring()));
    const auto size=file.tellg(); if (size<0 || static_cast<std::uint64_t>(size)>maximum) return failure("File exceeds its size limit.");
    std::string bytes(static_cast<std::size_t>(size),'\0'); file.seekg(0);
    if (!file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))) return failure("Could not read the complete file.");
    return bytes;
}
Result<void> write_file(const std::filesystem::path& path,std::string_view text,WriteMode mode) {
    std::filesystem::path temporary;
    try {
        std::filesystem::create_directories(path.parent_path());
        temporary=path; temporary+=L"."+wide(identity())+L".tmp";
        const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) return failure("Cannot create temporary file for "+utf8(path.wstring()));
        DWORD written{}; const bool ok=text.size()<=MAXDWORD && WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
        const auto write_error=GetLastError(); CloseHandle(file);
        const DWORD flags=MOVEFILE_WRITE_THROUGH|(mode==WriteMode::replace?MOVEFILE_REPLACE_EXISTING:0);
        if (!ok || !MoveFileExW(temporary.c_str(),path.c_str(),flags)) {
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
        auto path=std::filesystem::weakly_canonical(root_/relative);
        if (!within(root_,path)) return failure("Project path escapes its root through a link.");
        return path;
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<Project> Project::create(const std::filesystem::path& folder) {
    try {
        if (folder.empty()) return failure("Choose an empty project folder.");
        if (std::filesystem::exists(folder) && (!std::filesystem::is_directory(folder) || !std::filesystem::is_empty(folder)))
            return failure("Create Project requires an empty folder; existing files were not changed.");
        for (const auto* directory:{L"Config",L"Content/Scenes",L"Source",L"Cache"}) std::filesystem::create_directories(folder/directory);
        const Json config={{"version",1},{"name",utf8(std::filesystem::canonical(folder).filename().wstring())},{"startup_scene","Content/Scenes/Main.scene"}};
        take(write_file(folder/"Content/Scenes/Main.scene",Json{{"version",1},{"actors",Json::array()}}.dump(2)+"\n",WriteMode::create));
        take(write_file(folder/".gitignore","/Cache/\n",WriteMode::create));
        take(write_file(folder/"Config/Project.json",config.dump(2)+"\n",WriteMode::create)); // Marker is published last.
        return open(folder);
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<Project> Project::open(const std::filesystem::path& folder) {
    try {
        Project project; project.root_=std::filesystem::canonical(folder);
        const auto config=read_json(take(project.resolve("Config/Project.json")));
        json_format::fields(config,{"version","name","startup_scene"});
        if (json_format::identity(config.at("version"))!=1) return failure("Unsupported project format version.");
        project.name_=wide(config.at("name").get<std::string>());
        project.startup_=wide(config.at("startup_scene").get<std::string>());
        if (project.name_.empty() || project.name_.size()>256 || std::ranges::any_of(project.name_,[](wchar_t c){return c<32 || c==127;}) ||
            project.startup_.empty() || project.startup_.extension()!=L".scene" || *project.startup_.begin()!=L"Content") return failure("Invalid project name or startup scene.");
        take(project.resolve(project.startup_));
        for (const auto* directory:{L"Config",L"Content",L"Source"}) if (!std::filesystem::is_directory(take(project.resolve(directory)))) return failure("Project is missing Config, Content or Source.");
        take(project.refresh()); return project;
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<void> Project::refresh() {
    try {
        std::vector<ProjectEntry> entries;
        std::unordered_set<std::string> keys;
        for (const auto* directory:{L"Content",L"Source",L"Config"}) {
            if (entries.size()>=10000) return failure("Project exceeds the current 10000-entry catalog limit.");
            entries.push_back({directory,utf8(directory),AssetKind::folder});
            for (const auto& entry:std::filesystem::recursive_directory_iterator(take(resolve(directory)))) {
                if (entry.is_symlink()) continue;
                const auto relative=entry.path().lexically_relative(root_);
                take(resolve(relative));
                const auto filename=entry.path().filename().wstring();
                if (filename.ends_with(L".asset.json") || filename.ends_with(L".tmp")) continue;
                if (entries.size()>=10000) return failure("Project exceeds the current 10000-entry catalog limit.");
                auto kind=entry.is_directory()?AssetKind::folder:entry.path().extension()==L".obj"?AssetKind::mesh:
                    entry.path().extension()==L".scene"?AssetKind::scene:std::wstring_view(directory)==L"Source"?AssetKind::source:AssetKind::file;
                std::string key=utf8(relative.generic_wstring());
                if (kind==AssetKind::mesh) {
                    auto meta_relative=relative; meta_relative+=L".asset.json";
                    auto metadata=take(resolve(meta_relative));
                    if (!std::filesystem::exists(metadata)) {
                        const auto created=write_file(metadata,Json{{"version",1},{"id",identity()}}.dump(2)+"\n",WriteMode::create);
                        if (!created && !std::filesystem::exists(metadata)) return created;
                    }
                    const auto info=read_json(metadata);json_format::fields(info,{"version","id"});
                    if (json_format::identity(info.at("version"))!=1) return failure("Unsupported mesh metadata version.");
                    const auto raw=wide(info.at("id").get<std::string>());GUID id{};
                    if (raw.size()!=38 || raw.front()!=L'{' || raw.back()!=L'}' || FAILED(CLSIDFromString(raw.c_str(),&id)))
                        return failure("Invalid resource identity metadata.");
                    wchar_t canonical[40]{};StringFromGUID2(id,canonical,40);key="project/"+utf8(canonical);
                    if (!keys.insert(key).second) return failure("Duplicate resource identity; copy assets through Import Mesh.");
                }
                entries.push_back({relative,std::move(key),kind,kind==AssetKind::folder?0:entry.file_size(),
                    kind==AssetKind::mesh?entry.last_write_time():std::filesystem::file_time_type{}});
            }
        }
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return a.path<b.path; });
        if (entries==entries_) return {};
        std::unordered_map<std::string,std::size_t> mesh_entries;
        for (std::size_t i=0;i<entries.size();++i) if (entries[i].kind==AssetKind::mesh) mesh_entries.emplace(entries[i].key,i);
        std::erase_if(meshes_,[&](const auto& cached) {
            const auto before=mesh_entries_.find(cached.first),after=mesh_entries.find(cached.first);
            if (after==mesh_entries.end()) return true;
            if (before==mesh_entries_.end()) return false; // Prepared, newly published import.
            const auto& old=entries_[before->second];const auto& next=entries[after->second];
            return old.bytes!=next.bytes || old.modified!=next.modified;
        });
        entries_=std::move(entries);mesh_entries_=std::move(mesh_entries);++revision_;return {};
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<MeshAsset> Project::mesh(std::string_view key) {
    if (const auto* builtin=mesh_asset(key)) return *builtin;
    try {
        const auto found=mesh_entries_.find(std::string(key));
        if (found==mesh_entries_.end()) return failure("Missing mesh resource: "+std::string(key));
        const auto& entry=entries_[found->second];
        auto& cached=meshes_[entry.key];if (!cached) cached=parse_obj(take(read_file(take(resolve(entry.path)))));
        return MeshAsset{entry.key,entry.path.stem().wstring(),cached};
    } catch (const std::exception& error) { return failure(error.what()); }
}
Result<std::string> Project::import_mesh(const std::filesystem::path& source) {
    std::filesystem::path target,metadata;
    std::string resource;
    bool created_target{},created_metadata{};
    try {
        if (source.extension()!=L".obj") return failure("Import Mesh accepts triangulated Wavefront OBJ geometry.");
        const auto text=take(read_file(source));const auto loaded=parse_obj(text);
        take(refresh()); // A broken existing catalog cannot leave a half-imported file.
        target=take(resolve(std::filesystem::path("Content/Meshes")/source.filename()));
        auto meta_relative=target.lexically_relative(root_);meta_relative+=L".asset.json";metadata=take(resolve(meta_relative));
        if (std::filesystem::exists(target) || std::filesystem::exists(metadata)) return failure("A mesh or its metadata already exists with this name in Content/Meshes.");
        resource="project/"+identity();
        meshes_.emplace(resource,loaded); // Allocate before publishing either file.
        take(write_file(target,text,WriteMode::create));created_target=true;
        take(write_file(metadata,Json{{"version",1},{"id",resource.substr(8)}}.dump(2)+"\n",WriteMode::create));created_metadata=true;
        take(refresh());
        return resource;
    } catch (const std::exception& error) {
        if (!resource.empty()) meshes_.erase(resource);
        std::string message=error.what();std::error_code cleanup;
        if (created_metadata) {std::filesystem::remove(metadata,cleanup);if (cleanup) message+="; metadata cleanup failed: "+cleanup.message();}
        cleanup.clear();
        if (created_target) {std::filesystem::remove(target,cleanup);if (cleanup) message+="; mesh cleanup failed: "+cleanup.message();}
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
    auto bytes = read_file(*path, 128ull * 1024 * 1024 + 8192);
    if (!bytes) return std::unexpected(bytes.error());
    auto valid = physics::validate_cooked_triangle_mesh(*bytes, asset->mesh, scale, key);
    if (!valid) return std::unexpected(valid.error());
    return std::make_shared<const std::string>(std::move(*bytes));
}
}

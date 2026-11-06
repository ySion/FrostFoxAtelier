#include "editor/project.hpp"
#include "editor/session.hpp"
#include "platform/windows_path.hpp"
#include "assets/file.hpp"
#include "assets/mesh_import.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "physics/physics.hpp"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <bit>
#include <cwctype>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

namespace atelier::editor {
namespace {
using assets::json_format::Json;

auto failure(std::string text) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(text)});
}

std::string identity() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        throw std::runtime_error("Cannot create asset identity.");
    wchar_t text[40]{};
    StringFromGUID2(id, text, 40);
    return assets::utf8(text);
}

template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    if constexpr (!std::is_void_v<T>)
        return std::move(*value);
}

Json read_json(const std::filesystem::path& path) {
    return assets::json_format::parse(take(assets::read_file(path)));
}

// A generated material's file name: valid on Windows and unique among `taken`, compared as the file system does (ordinal, ignoring case).
std::wstring material_file(std::string_view slot, std::vector<std::wstring>& taken) {
    auto name = assets::wide(slot);
    for (auto& c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos)
            c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        name.pop_back();
    if (name.empty())
        name = L"material";
    // Device names stay reserved with any extension, and with spaces before it, so the prefix goes first.
    auto device = name.substr(0, name.find(L'.'));
    while (!device.empty() && device.back() == L' ')
        device.pop_back();
    for (auto& c : device)
        c = static_cast<wchar_t>(std::towupper(c));
    const auto numbered = device.size() == 4 && (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
                          ((device[3] >= L'1' && device[3] <= L'9') || device[3] == L'\u00B9' ||
                           device[3] == L'\u00B2' || device[3] == L'\u00B3');
    if (numbered || device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL" ||
        device == L"CONIN$" || device == L"CONOUT$")
        name.insert(0, 1, L'_');
    auto candidate = name;
    for (std::size_t n = 2;; ++n) {
        const auto file = candidate + L".material";
        if (std::ranges::none_of(taken, [&](const std::wstring& other) {
                return CompareStringOrdinal(other.c_str(), -1, file.c_str(), -1, TRUE) == CSTR_EQUAL;
            })) {
            taken.push_back(file);
            return file;
        }
        candidate = name + L" (" + std::to_wstring(n) + L")";
    }
}

}

Result<void> write_file(const std::filesystem::path& path, std::string_view text, WriteMode mode) {
    std::filesystem::path temporary;
    try {
        const auto native = platform::windows_file_path(path);
        std::filesystem::create_directories(native.parent_path());
        temporary = native;
        temporary += L"." + assets::wide(identity()) + L".tmp";
        const auto file =
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return failure("Cannot create temporary file for " + assets::utf8(path.wstring()) + " (Windows error " +
                           std::to_string(GetLastError()) + ").");
        DWORD written{};
        const bool ok = text.size() <= MAXDWORD &&
                        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                        written == text.size() && FlushFileBuffers(file);
        const auto write_error = GetLastError();
        CloseHandle(file);
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == WriteMode::replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!ok || !MoveFileExW(temporary.c_str(), native.c_str(), flags)) {
            const auto error = ok ? GetLastError() : write_error;
            DeleteFileW(temporary.c_str());
            return failure("Cannot save " + assets::utf8(path.wstring()) + " (Windows error " + std::to_string(error) +
                           ").");
        }
        return {};
    } catch (const std::exception& error) {
        if (!temporary.empty())
            DeleteFileW(temporary.c_str());
        return failure(error.what());
    }
}

Result<std::filesystem::path> Project::resolve(const std::filesystem::path& relative) const {
    return assets::resolve_project_path(root_, relative);
}

Result<Project> Project::create(const std::filesystem::path& folder) {
    try {
        if (folder.empty())
            return failure("Choose an empty project folder.");
        const auto native = platform::windows_file_path(folder);
        if (std::filesystem::exists(native) &&
            (!std::filesystem::is_directory(native) || !std::filesystem::is_empty(native)))
            return failure("Create Project requires an empty folder; existing files were not changed.");
        for (const auto* directory : {L"Config", L"Content/Scenes", L"Source", L"Cache"})
            std::filesystem::create_directories(platform::windows_file_path(folder / directory));
        const Json config = {{"version", 1},
                             {"name", assets::utf8(std::filesystem::canonical(native).filename().wstring())},
                             {"startup_scene", "Content/Scenes/Main.scene"}};
        Session empty_scene;
        take(write_file(folder / "Content/Scenes/Main.scene", take(empty_scene.serialize()), WriteMode::create));
        take(write_file(folder / ".gitignore", "/Cache/\n", WriteMode::create));
        take(write_file(folder / "Config/Project.json", config.dump(2) + "\n",
                        WriteMode::create)); // Marker is published last.
        return open(folder);
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<Project> Project::open(const std::filesystem::path& folder) {
    try {
        Project project;
        project.root_ = std::filesystem::canonical(platform::windows_file_path(folder));
        project.assets_ = assets::Reader(project.root_);
        const auto config = read_json(take(project.resolve("Config/Project.json")));
        assets::json_format::fields(config, {"version", "name", "startup_scene", "ui_source"});
        if (assets::json_format::identity(config.at("version")) != 1)
            return failure("Unsupported project format version.");
        project.name_ = assets::wide(config.at("name").get<std::string>());
        project.startup_ = assets::wide(config.at("startup_scene").get<std::string>());
        if (config.contains("ui_source")) {
            project.ui_source_ = assets::wide(config.at("ui_source").get<std::string>());
            if (!project.ui_source_.empty() && (*project.ui_source_.begin() != L"Source" ||
                                                !project.ui_source_.filename().wstring().ends_with(L".ui.source")))
                return failure("Project UI must be a .ui.source file inside Source.");
            if (!project.ui_source_.empty())
                take(project.resolve(project.ui_source_));
        }
        if (project.name_.empty() || project.name_.size() > 256 ||
            std::ranges::any_of(project.name_, [](wchar_t c) { return c < 32 || c == 127; }) ||
            !is_scene_path(project.startup_))
            return failure("Invalid project name or startup scene.");
        take(project.resolve(project.startup_));
        for (const auto* directory : {L"Config", L"Content", L"Source"})
            if (!std::filesystem::is_directory(platform::windows_file_path(take(project.resolve(directory)))))
                return failure("Project is missing Config, Content or Source.");
        take(project.refresh());
        return project;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<std::vector<std::filesystem::path>> Project::source_paths() const {
    try {
        std::vector<std::filesystem::path> paths;
        const auto native_root = platform::windows_file_path(root_);
        const auto directory = platform::windows_file_path(take(resolve("Source")));
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_symlink() || !entry.is_regular_file())
                continue;
            if (entry.path().extension() != L".source" || entry.path().filename().wstring().ends_with(L".ui.source"))
                continue;
            const auto relative = entry.path().lexically_relative(native_root);
            take(resolve(relative));
            if (paths.size() >= 256)
                return failure("Project exceeds the 256 Source file limit.");
            paths.push_back(relative);
        }
        std::ranges::sort(paths);
        return paths;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<void> Project::create_source(const std::filesystem::path& relative, std::string_view text) {
    if (relative.empty() || *relative.begin() != L"Source" || relative.extension() != L".source")
        return failure("Create source files inside the project's Source directory.");
    const auto path = resolve(relative);
    if (!path)
        return std::unexpected(path.error());
    if (text.size() > 1024 * 1024)
        return failure("Source file exceeds the 1 MiB limit.");
    try {
        static_cast<void>(assets::wide(text));
    } catch (const std::exception& error) {
        return failure(error.what());
    }
    return write_file(*path, text, WriteMode::create);
}

Result<void> Project::set_ui_source(const std::filesystem::path& relative) {
    if (!relative.empty() &&
        (*relative.begin() != L"Source" || !relative.filename().wstring().ends_with(L".ui.source")))
        return failure("Project UI must be a .ui.source file inside Source.");
    if (!relative.empty()) {
        ATELIER_TRY_BIND(path, resolve(relative));
    }
    try {
        const auto path = take(resolve("Config/Project.json"));
        auto config = read_json(path);
        assets::json_format::fields(config, {"version", "name", "startup_scene", "ui_source"});
        if (assets::json_format::identity(config.at("version")) != 1 || config.at("name") != assets::utf8(name_) ||
            config.at("startup_scene") != assets::utf8(startup_.generic_wstring()) ||
            config.value("ui_source", std::string{}) != assets::utf8(ui_source_.generic_wstring()))
            return failure("Project configuration changed on disk. Reopen the project before changing its UI entry.");
        if (relative.empty())
            config.erase("ui_source");
        else
            config["ui_source"] = assets::utf8(relative.generic_wstring());
        ATELIER_TRY_BIND(saved, write_file(path, config.dump(2) + "\n"));
        ui_source_ = relative;
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<std::filesystem::path> Project::rename(const std::filesystem::path& relative, std::wstring_view name) {
    if (relative.empty() || std::distance(relative.begin(), relative.end()) < 2)
        return failure("Top-level project folders cannot be renamed.");
    if (name.empty() || name.size() > 128 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring_view::npos ||
        name.back() == L'.' || name.back() == L' ' || name == L"." || name == L"..")
        return failure("Enter a file name without \\ / : * ? \" < > | and not ending in a dot or space.");
    if (relative == startup_ || relative == ui_source_)
        return failure("The startup scene and UI source are configured by path; change Config first.");
    const auto source = resolve(relative);
    if (!source)
        return std::unexpected(source.error());
    const auto target_relative = relative.parent_path() / std::filesystem::path(name);
    if (target_relative == relative)
        return relative;
    const auto target = resolve(target_relative);
    if (!target)
        return std::unexpected(target.error());
    const auto native_source = platform::windows_file_path(*source);
    const auto native_target = platform::windows_file_path(*target);
    std::error_code error;
    if (!std::filesystem::exists(native_source, error))
        return failure("The asset no longer exists on disk.");
    // Case-only renames are the same file on Windows; anything else must not overwrite.
    if (std::filesystem::exists(native_target, error) &&
        !std::filesystem::equivalent(native_source, native_target, error))
        return failure("Another file or folder already has that name.");
    const auto sidecar = [&](const std::filesystem::path& path) {
        auto result = path;
        result += L".asset.json";
        return result;
    };
    const bool has_sidecar =
        !std::filesystem::is_directory(native_source, error) && std::filesystem::exists(sidecar(native_source), error);
    std::filesystem::rename(native_source, native_target, error);
    if (error)
        return failure("The file could not be renamed; it may be open in another program.");
    if (has_sidecar) {
        std::filesystem::rename(sidecar(native_source), sidecar(native_target), error);
        if (error) {
            std::error_code undo;
            std::filesystem::rename(native_target, native_source, undo);
            return failure("The asset identity file could not be moved; the rename was undone.");
        }
    }
    ATELIER_TRY(refresh());
    return target_relative;
}

Result<std::vector<assets::ProjectEntry>> Project::scan_entries(bool create_identities) const {
    try {
        std::vector<assets::ProjectEntry> entries;
        std::unordered_set<std::string> keys;
        const auto native_root = platform::windows_file_path(root_);
        std::vector<std::filesystem::path> roots{"Content", "Source", "Config"};
        for (const auto& directory : roots) {
            const auto root = platform::windows_file_path(take(resolve(directory)));
            entries.push_back({directory, assets::utf8(directory.generic_wstring()), assets::AssetKind::folder});
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                if (entry.is_symlink())
                    continue;
                const auto relative = entry.path().lexically_relative(native_root);
                take(resolve(relative));
                const auto filename = entry.path().filename().wstring();
                if (filename.ends_with(L".asset.json") || filename.ends_with(L".meta") || filename.ends_with(L".tmp"))
                    continue;
                auto kind = entry.is_directory()                       ? assets::AssetKind::folder
                            : assets::is_mesh_path(entry.path())       ? assets::AssetKind::mesh
                            : entry.path().extension() == L".material" ? assets::AssetKind::material
                            : assets::is_texture_path(entry.path())    ? assets::AssetKind::texture
                            : assets::is_sky_path(entry.path())        ? assets::AssetKind::sky
                            : entry.path().extension() == L".scene"    ? assets::AssetKind::scene
                            : directory == L"Source"                   ? assets::AssetKind::source
                                                                       : assets::AssetKind::file;
                std::string key = assets::utf8(relative.generic_wstring());
                std::vector<assets::SlotMaterial> materials;
                if (kind == assets::AssetKind::mesh || kind == assets::AssetKind::material ||
                    kind == assets::AssetKind::texture || kind == assets::AssetKind::sky) {
                    auto meta_relative = relative;
                    meta_relative += L".asset.json";
                    auto metadata = take(resolve(meta_relative));
                    if (!std::filesystem::exists(metadata)) {
                        if (!create_identities)
                            return failure("Asset identity is missing; refresh the project before importing: " +
                                           assets::utf8(meta_relative.generic_wstring()));
                        const auto created = write_file(
                            metadata, Json{{"version", 1}, {"id", identity()}}.dump(2) + "\n", WriteMode::create);
                        if (!created && !std::filesystem::exists(metadata))
                            return std::unexpected(created.error());
                    }
                    const auto info = read_json(metadata);
                    assets::json_format::fields(info, {"version", "id", "materials"});
                    if (assets::json_format::identity(info.at("version")) != 1)
                        return failure("Unsupported resource metadata version.");
                    const auto raw = assets::wide(info.at("id").get<std::string>());
                    GUID id{};
                    if (raw.size() != 38 || raw.front() != L'{' || raw.back() != L'}' ||
                        FAILED(CLSIDFromString(raw.c_str(), &id)))
                        return failure("Invalid resource identity metadata.");
                    wchar_t canonical[40]{};
                    StringFromGUID2(id, canonical, 40);
                    key = "project/" + assets::utf8(canonical);
                    if (!keys.insert(key).second)
                        return failure("Duplicate resource identity; import or create an independent asset.");
                    if (const auto mapping = info.find("materials"); mapping != info.end()) {
                        if (kind != assets::AssetKind::mesh || !mapping->is_object())
                            return failure(
                                "Only mesh metadata lists default materials, as slot names to material keys: " +
                                assets::utf8(meta_relative.generic_wstring()));
                        for (const auto& [slot, material] : mapping->items()) {
                            if (slot.empty() || !material.is_string())
                                return failure("Invalid default material in " +
                                               assets::utf8(meta_relative.generic_wstring()));
                            materials.push_back({slot, material.get<std::string>()});
                        }
                    }
                }
                entries.push_back(
                    {relative, std::move(key), kind, kind == assets::AssetKind::folder ? 0 : entry.file_size(),
                     kind != assets::AssetKind::folder ? entry.last_write_time() : std::filesystem::file_time_type{},
                     std::move(materials)});
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        return entries;
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<void> Project::refresh() {
    try {
        auto roots = std::vector<std::filesystem::path>{"Content", "Source", "Config"};
        take(assets_.publish(take(scan_entries(true))));
        roots_ = std::move(roots);
        return {};
    } catch (const std::exception& error) {
        return failure(error.what());
    }
}

Result<std::string> Project::import_mesh(const std::filesystem::path& source) {
    ATELIER_TRY_BIND(published, import_mesh_files(source, {}));
    return std::move(published->mesh.key);
}

Result<MeshImportPublication> Project::reimport_mesh(std::string_view key, const std::filesystem::path& source) {
    if (!key.starts_with("project/"))
        return failure("Reimport requires an existing project mesh identity.");
    return import_mesh_files(source, key);
}

Result<MeshImportPublication> Project::import_mesh_files(const std::filesystem::path& source,
                                                         std::string_view existing_key) {
    struct Write {
        std::filesystem::path path;

        std::string bytes;

        std::optional<std::string> before;

        std::filesystem::file_time_type modified{};

        bool written{};
    };
    std::vector<Write> writes;
    std::vector<std::filesystem::path> directories;
    try {
        if (!assets::is_mesh_path(source))
            return failure("Import Mesh accepts Wavefront OBJ or glTF 2.0 (.gltf) geometry and its dependencies.");
        const std::string requested(existing_key); // The caller may borrow its key from this Reader.
        auto loaded = assets_.prepare_mesh(std::filesystem::absolute(source));
        // A read-only scan cannot create unrelated sidecars during a failed import.
        const auto catalog = take(scan_entries(false));
        const auto same_path = [](const std::filesystem::path& a, const std::filesystem::path& b) {
            const auto left = a.lexically_normal().generic_wstring();
            const auto right = b.lexically_normal().generic_wstring();
            return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
        };
        const auto file_entry = [&](const std::filesystem::path& path) -> const assets::ProjectEntry* {
            const auto at =
                std::ranges::find_if(catalog, [&](const auto& entry) { return same_path(entry.path, path); });
            return at == catalog.end() ? nullptr : &*at;
        };
        const auto sidecar = [](std::filesystem::path path) {
            path += L".asset.json";
            return path;
        };
        const auto identity_file = [](const std::string& key, Json extra = Json::object()) {
            extra["version"] = 1;
            extra["id"] = key.substr(8);
            return extra.dump(2) + "\n";
        };
        auto relative = std::filesystem::path("Content/Meshes");
        if (loaded.files.size() > 1)
            relative /= source.stem();
        auto target_relative = relative / source.filename();
        std::string resource = requested.empty() ? "project/" + identity() : requested;
        Json metadata = Json::object();
        Json defaults = Json::object();
        std::vector<std::filesystem::path> owned;
        std::string dependency_error;
        if (!requested.empty()) {
            const auto at = std::ranges::find(catalog, requested, &assets::ProjectEntry::key);
            if (at == catalog.end() || at->kind != assets::AssetKind::mesh)
                return failure("Reimport requires an existing project mesh identity.");
            target_relative = at->path;
            relative = target_relative.parent_path();
            if (!same_path(source.extension(), target_relative.extension()))
                return failure("Reimport must retain the mesh format (.obj or .gltf).");
            metadata = read_json(take(resolve(sidecar(target_relative))));
            owned.push_back(target_relative);
            // Current raw dependencies, not material closure, prove which existing
            // files this operation may replace. There is no import ownership history.
            try {
                const auto previous = assets_.prepare_mesh(take(resolve(target_relative)));
                for (const auto& file : previous.files)
                    owned.push_back(relative / file.relative);
            } catch (const std::exception& error) {
                dependency_error = "Existing mesh dependencies could not be verified: " + std::string(error.what());
                loaded.notes.push_back(dependency_error + "; only unchanged existing dependencies may be reused.");
            }
            auto main = std::ranges::find(loaded.files, source.filename(), &assets::ImportedFile::relative);
            if (main == loaded.files.end())
                return failure("Prepared mesh has no source file.");
            main->relative = target_relative.filename();
        }
        const auto queue = [&](const std::filesystem::path& relative_path, std::string bytes, bool replace) {
            auto path = take(resolve(relative_path));
            if (std::ranges::any_of(writes, [&](const auto& write) { return same_path(write.path, path); }))
                throw std::runtime_error("Import outputs collide: " + assets::utf8(relative_path.generic_wstring()));
            Write write{std::move(path), std::move(bytes), {}};
            if (std::filesystem::exists(write.path)) {
                write.modified = std::filesystem::last_write_time(write.path);
                write.before = take(assets::read_file(write.path, 512ull << 20));
                if (!replace && (requested.empty() || *write.before != write.bytes))
                    throw std::runtime_error("Import would overwrite an unrelated or unverified file: " +
                                             assets::utf8(relative_path.generic_wstring()) +
                                             (dependency_error.empty() ? std::string{} : "; " + dependency_error));
            }
            writes.push_back(std::move(write));
        };
        // Prepare exact input pixels and persistent texture keys before moving the
        // immutable file bytes into the common create/replace transaction.
        std::map<std::filesystem::path, std::string> texture_keys;
        std::vector<std::string_view> image_bytes;
        MeshImportPublication result;
        const auto prepare_texture = [&](const std::filesystem::path& file) {
            if (file.empty() || texture_keys.contains(file))
                return;
            const auto* existing = file_entry(relative / file);
            if (existing && existing->kind != assets::AssetKind::texture)
                throw std::runtime_error("Imported texture path names another resource kind.");
            const auto key = existing ? existing->key : "project/" + identity();
            texture_keys.emplace(file, key);
            const auto bytes = std::ranges::find(loaded.files, file, &assets::ImportedFile::relative);
            if (bytes == loaded.files.end())
                throw std::runtime_error("Imported texture is absent from the prepared bundle.");
            image_bytes.push_back(bytes->bytes);
            result.textures.push_back({key, file.stem().wstring(), {}});
        };
        for (const auto& inputs : loaded.textures)
            for (const auto& file : inputs)
                prepare_texture(file);
        // Saved or unsaved user MIs may still reference a bundle texture that the
        // new source material stopped sampling. Publish every existing texture
        // asset touched by this reimport, not only the new source's active inputs.
        if (!requested.empty())
            for (const auto& file : loaded.files)
                if (const auto* existing = file_entry(relative / file.relative);
                    existing && existing->kind == assets::AssetKind::texture)
                    prepare_texture(file.relative);
        const auto images = assets_.prepare_images(image_bytes, 2ull << 30);
        for (std::size_t i = 0; i < images.size(); ++i)
            result.textures[i].image = images[i];
        for (auto& file : loaded.files) {
            const auto path = relative / file.relative;
            auto filename = path.filename().wstring();
            for (auto& c : filename)
                c = static_cast<wchar_t>(std::towlower(c));
            if (filename.ends_with(L".asset.json") || filename.ends_with(L".meta") || filename.ends_with(L".tmp"))
                return failure("Import dependencies cannot replace identity or temporary files.");
            const bool replace = std::ranges::any_of(owned, [&](const auto& old) { return same_path(old, path); });
            // A source dependency is not permission to overwrite a hand-edited MI/MS.
            const auto* existing = file_entry(path);
            if (existing && existing->kind == assets::AssetKind::material)
                return failure("Import cannot overwrite an existing material author file.");
            queue(path, std::move(file.bytes), replace);
            if (same_path(path, target_relative))
                continue;
            if (assets::is_texture_path(path) || assets::is_mesh_path(path) || assets::is_sky_path(path) ||
                path.extension() == L".material") {
                if (!file_entry(path)) {
                    const auto texture = texture_keys.find(file.relative);
                    const auto key = texture != texture_keys.end() ? texture->second : "project/" + identity();
                    queue(sidecar(path), identity_file(key), false);
                }
            }
        }
        assets::Reader current = assets_;
        take(current.publish(catalog));
        std::set<std::string> returned_materials;
        std::set<std::string> slots;
        std::vector<std::wstring> names;
        for (const auto& entry : catalog)
            if (same_path(entry.path.parent_path(), relative / "Materials"))
                names.push_back(entry.path.filename().wstring());
        for (std::size_t i = 0; i < loaded.mesh->materials().size(); ++i) {
            const auto slot = geometry::material_slot_name(*loaded.mesh, i);
            if (!slots.insert(slot).second)
                return failure("Reimport requires unique material slot names.");
            if (metadata.contains("materials") && metadata["materials"].contains(slot)) {
                const auto key = metadata["materials"][slot].get<std::string>();
                defaults[slot] = key;
                loaded.notes.push_back("Preserved material binding and author values for slot '" + slot +
                                       "'; source material changes were not merged.");
                if (returned_materials.insert(key).second) {
                    if (auto material = current.material(key))
                        result.materials.push_back(std::move(*material));
                    else
                        loaded.notes.push_back("Preserved unavailable material " + key + ": " +
                                               material.error().message);
                }
                continue;
            }
            if (loaded.materials.empty() || !loaded.materials[i])
                continue;
            auto author = *loaded.materials[i];
            for (std::size_t input = 0; input < assets::imported_material_inputs.size(); ++input)
                if (const auto& file = loaded.textures[i][input]; !file.empty()) {
                    const auto path = std::string(assets::imported_material_inputs[input]) + "_texture";
                    const auto entry = std::ranges::find(author.entries, path, &material::Override::path);
                    if (entry == author.entries.end() ||
                        !std::holds_alternative<material::TextureBinding>(entry->value))
                        return failure("Imported texture has no typed MI override.");
                    std::get<material::TextureBinding>(entry->value).asset_key = texture_keys.at(file);
                }
            auto filename = material_file(slot, names);
            if (assets::is_material_source_path(filename))
                filename = material_file(slot + " (MI)", names);
            const auto path = relative / "Materials" / filename;
            const auto key = "project/" + identity();
            assets::MaterialAsset asset{key, path.stem().wstring(), std::move(author)};
            queue(path, assets::material_format::encode(asset).dump(2) + "\n", false);
            queue(sidecar(path), identity_file(key), false);
            defaults[slot] = key;
            result.materials.push_back(std::move(asset));
        }
        if (!defaults.empty())
            metadata["materials"] = std::move(defaults);
        else
            metadata.erase("materials");
        queue(sidecar(target_relative), identity_file(resource, std::move(metadata)), !requested.empty());
        result.notes = loaded.notes;
        for (const auto& write : writes)
            for (auto parent = write.path.parent_path(); !std::filesystem::exists(parent);
                 parent = parent.parent_path())
                if (std::ranges::find(directories, parent) == directories.end())
                    directories.push_back(parent);
        std::ranges::sort(directories,
                          [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
        // Every decode, author decision, identity and backup is ready. Both initial
        // import and explicit reimport use this one transaction and publication.
        for (auto& write : writes) {
            if (write.before && *write.before == write.bytes)
                continue;
            take(write_file(write.path, write.bytes, write.before ? WriteMode::replace : WriteMode::create));
            write.written = true;
        }
        result.mesh = take(assets_.publish_mesh(take(scan_entries(false)), resource, loaded, result.textures));
        return result;
    } catch (const std::exception& error) {
        std::string message = error.what();
        for (auto it = writes.rbegin(); it != writes.rend(); ++it)
            if (it->written) {
                std::error_code cleanup;
                if (it->before) {
                    auto restored = write_file(it->path, *it->before);
                    if (!restored) {
                        message += "; import rollback failed: " + restored.error().message;
                        continue;
                    }
                    std::filesystem::last_write_time(it->path, it->modified, cleanup);
                } else
                    std::filesystem::remove(it->path, cleanup);
                if (cleanup)
                    message += "; import rollback failed: " + cleanup.message();
            }
        for (const auto& directory : directories) {
            std::error_code cleanup;
            if (std::filesystem::exists(directory, cleanup) && std::filesystem::is_directory(directory, cleanup))
                std::filesystem::remove(directory, cleanup); // Only empty directories planned above.
            if (cleanup)
                message += "; import directory cleanup failed: " + cleanup.message();
        }
        return failure(std::move(message));
    }
}

namespace {
std::filesystem::path collision_path(std::string_view key, math::Vector3 scale) {
    // The key is a validated project GUID; float bits keep distinct import scales apart.
    std::ostringstream name;
    name << "Cache/Physics/" << key.substr(8) << '-' << std::hex << std::bit_cast<std::uint32_t>(scale.x) << '-'
         << std::bit_cast<std::uint32_t>(scale.y) << '-' << std::bit_cast<std::uint32_t>(scale.z) << ".jolt-shape";
    return name.str();
}
}

Result<std::filesystem::path> Project::cook_collision(std::string_view key, math::Vector3 scale) {
    if (!key.starts_with("project/"))
        return failure("Collision Cook requires a project mesh identity.");
    ATELIER_TRY_BIND(asset, assets_.mesh(key));
    ATELIER_TRY_BIND(cooked, physics::cook_triangle_mesh(asset->mesh, scale, key));
    ATELIER_TRY_BIND(path, resolve(collision_path(key, scale)));
    ATELIER_TRY(write_file(*path, *cooked));
    return *path;
}

Result<std::shared_ptr<const std::string>> Project::collision(std::string_view key, math::Vector3 scale) {
    if (!key.starts_with("project/"))
        return failure("Collision asset requires a project mesh identity.");
    ATELIER_TRY_BIND(asset, assets_.mesh(key));
    ATELIER_TRY_BIND(path, resolve(collision_path(key, scale)));
    if (auto bytes = assets::read_file(*path, 128ull * 1024 * 1024 + 8192);
        bytes && physics::validate_cooked_triangle_mesh(*bytes, asset->mesh, scale, key))
        return std::make_shared<const std::string>(std::move(*bytes));
    // The cache is derived data: a missing or stale entry is rebuilt from the source mesh.
    ATELIER_TRY_BIND(cooked, physics::cook_triangle_mesh(asset->mesh, scale, key));
    static_cast<void>(write_file(*path, *cooked)); // A failed write only costs the next load another Cook.
    return std::make_shared<const std::string>(std::move(*cooked));
}
}

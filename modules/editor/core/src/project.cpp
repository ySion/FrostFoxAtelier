#include "editor/project.hpp"
#include "editor/session.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include "assets/file.hpp"
#include "assets/mesh_import.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "physics/physics.hpp"
#include "foundation/set.hpp"
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <bit>
#include <cwctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <array>

namespace atelier::editor {
namespace {
auto failure(std::string text) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(text)});
}

Result<std::string> identity() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return failure("Cannot create asset identity.");
    wchar_t text[40]{};
    StringFromGUID2(id, text, 40);
    auto value = platform::utf16_to_utf8(text);
    if (!value)
        return failure("Invalid UTF-16 text.");
    return std::move(*value);
}

Error json_error(const json::Error& error) {
    return {ErrorCode::invalid_storage, error.describe()};
}

Error file_error(std::error_code error) {
    return {ErrorCode::invalid_storage, platform::error_message(error)};
}

Result<json::Document> read_json(const std::filesystem::path& path) {
    ATELIER_TRY_BIND(bytes, assets::read_file(path));
    return assets::json_format::parse(*bytes).transform_error(json_error);
}

struct ProjectSettings {
    std::wstring name;
    std::filesystem::path startup;
    std::filesystem::path ui_source;
};

Result<ProjectSettings> read_settings(json::Value root) {
    ProjectSettings settings;
    ATELIER_TRY(root.allowed_members({"version", "name", "startup_scene", "ui_source"}).transform_error(json_error));
    ATELIER_TRY_BIND(version, root.u64({"version"}).transform_error(json_error));
    if (*version != 1)
        return failure("Unsupported project format version.");
    ATELIER_TRY_BIND(name, root.string({"name"}).transform_error(json_error));
    ATELIER_TRY_BIND(startup, root.string({"startup_scene"}).transform_error(json_error));
    settings.name = platform::utf8_to_utf16_validated(*name);
    settings.startup = platform::utf8_to_utf16_validated(*startup);
    if (const auto ui = *root.find("ui_source")) {
        ATELIER_TRY_BIND(text, ui->string().transform_error(json_error));
        settings.ui_source = platform::utf8_to_utf16_validated(*text);
        if (!settings.ui_source.empty() && (*settings.ui_source.begin() != L"Source" ||
                                            !settings.ui_source.filename().wstring().ends_with(L".ui.source")))
            return failure("Project UI must be a .ui.source file inside Source.");
    }
    if (settings.name.empty() || settings.name.size() > 256 ||
        std::ranges::any_of(settings.name, [](wchar_t c) { return c < 32 || c == 127; }) ||
        !Project::is_scene_path(settings.startup))
        return failure("Invalid project name or startup scene.");
    return settings;
}

std::filesystem::path sidecar(std::filesystem::path path) {
    path += L".asset.json";
    return path;
}

std::string identity_file(std::string_view key, json::Builder record = json::Builder{}) {
    static_cast<void>(record.set(record.root(), "version", std::uint32_t{1}));
    record.set_validated(record.root(), "id", key.substr(std::string_view{"project/"}.size()));
    const auto bytes = json::write(record.root(), {true, true});
    return {bytes.data(), bytes.size()};
}

// These kinds use sidecar identities. Other catalogue entries are addressed by relative path.
Optional<assets::AssetKind> resource_kind(const std::filesystem::path& path,
                                          const std::filesystem::path& directory) {
    using enum assets::AssetKind;
    if (assets::is_mesh_path(path))
        return mesh;
    if (path.extension() == L".material")
        return material;
    if (directory == L"Content" && assets::is_procedural_graph_path(path))
        return procedural_graph;
    if (assets::is_texture_path(path))
        return texture;
    if (assets::is_sky_path(path))
        return sky;
    return {};
}

Result<void> read_asset_identity(const Project& project, assets::ProjectEntry& entry, bool create_if_missing) {
    ATELIER_TRY_BIND(path, project.resolve(sidecar(entry.path)));
    ATELIER_TRY_BIND(native, platform::windows_file_path(*path).transform_error(file_error));
    std::error_code error;
    if (!std::filesystem::exists(*native, error)) {
        if (error)
            return std::unexpected(file_error(error));
        if (!create_if_missing)
            return failure("Asset identity is missing; refresh the project before importing.");
        ATELIER_TRY_BIND(id, identity());
        auto key = "project/" + *id;
        auto created = write_file(*native, identity_file(key), WriteMode::create);
        if (created) {
            entry.key = std::move(key);
            return {};
        }
        // Another scanner can win the exclusive create. Its identity is authoritative.
        if (!std::filesystem::exists(*native, error))
            return std::unexpected(std::move(created.error()));
    }
    ATELIER_TRY_BIND(info, read_json(*native));
    const auto record = info->root();
    ATELIER_TRY(record.allowed_members({"version", "id", "materials"}).transform_error(json_error));
    ATELIER_TRY_BIND(version, record.u64({"version"}).transform_error(json_error));
    if (*version != 1)
        return failure("Unsupported resource metadata version.");
    ATELIER_TRY_BIND(text, record.string({"id"}).transform_error(json_error));
    const auto raw = platform::utf8_to_utf16_validated(*text);
    GUID id{};
    if (raw.size() != 38 || raw.front() != L'{' || raw.back() != L'}' || FAILED(CLSIDFromString(raw.c_str(), &id)))
        return failure("Invalid resource identity metadata.");
    wchar_t canonical[40]{};
    StringFromGUID2(id, canonical, 40);
    auto name = platform::utf16_to_utf8(canonical);
    if (!name)
        return failure("Invalid UTF-16 text.");
    if (const auto mapping = *record.find("materials")) {
        if (entry.kind != assets::AssetKind::mesh || mapping->kind() != json::Kind::object)
            return failure("Only mesh metadata lists default materials, as slot names to material keys.");
        const auto items = *mapping->object_items();
        for (const auto& [slot, value] : items) {
            ATELIER_TRY_BIND(material, value.string().transform_error(json_error));
            if (slot.empty())
                return failure("Default material slot names cannot be empty.");
            entry.materials.add({std::string(slot), std::string(*material)});
        }
    }
    entry.key = "project/" + *name;
    return {};
}

// The compiler also walks Source between catalogue publications to discover external edits.
template <class Visit>
Result<void> visit_project_directory(const Project& project, const std::filesystem::path& directory, Visit&& visit) {
    ATELIER_TRY_BIND(resolved, project.resolve(directory));
    ATELIER_TRY_BIND(root, platform::windows_file_path(*resolved).transform_error(file_error));
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(*root, error), end;
    while (!error && it != end) {
        const auto status = it->symlink_status(error);
        if (error)
            break;
        if (!std::filesystem::is_symlink(status))
            ATELIER_TRY(visit(*it, status, *root));
        it.increment(error);
    }
    if (error) {
        const auto name = platform::utf16_to_utf8(directory.native());
        return failure("Cannot scan " + name.value_or("project directory") + ": " + platform::error_message(error));
    }
    return {};
}

// A generated material's file name: valid on Windows and unique among `taken`, compared as the file system does (ordinal, ignoring case).
Result<std::wstring> material_file(std::string_view slot, Array<std::wstring>& taken) {
    auto converted = platform::utf8_to_utf16(slot);
    if (!converted)
        return failure("Invalid UTF-8 text.");
    auto name = std::move(*converted);
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
            taken.add(file);
            return file;
        }
        candidate = name + L" (" + std::to_wstring(n) + L")";
    }
}

Result<void> write_in_directory(const std::filesystem::path& native, std::string_view text, WriteMode mode) {
    if (text.size() > MAXDWORD)
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "File exceeds the write size limit."});
    ATELIER_TRY_BIND(id, identity());
    auto temporary = native;
    temporary += L"." + platform::utf8_to_utf16_validated(*id) + L".tmp";
    const auto file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        const auto name = platform::utf16_to_utf8(native.native()).value_or("<invalid UTF-16 path>");
        return failure("Cannot create temporary file for " + name + " (Windows error " + std::to_string(error) + ").");
    }
    DWORD written{};
    DWORD first_error{};
    if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
        first_error = GetLastError();
    else if (written != text.size())
        first_error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file))
        first_error = GetLastError();
    if (!CloseHandle(file) && !first_error)
        first_error = GetLastError();
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == WriteMode::replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!first_error && !MoveFileExW(temporary.c_str(), native.c_str(), flags))
        first_error = GetLastError();
    if (first_error) {
        const auto name = platform::utf16_to_utf8(native.native()).value_or("<invalid UTF-16 path>");
        auto message = "Cannot save " + name + " (Windows error " + std::to_string(first_error) + ").";
        if (!DeleteFileW(temporary.c_str()))
            message += " Temporary file cleanup failed (Windows error " + std::to_string(GetLastError()) + ").";
        return failure(std::move(message));
    }
    return {};
}

bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto left = a.lexically_normal().generic_wstring();
    const auto right = b.lexically_normal().generic_wstring();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// All backups are captured before the first write. Only successful replacements
// and directories created by this batch belong to its rollback.
class MeshImportFiles {
public:
    MeshImportFiles(const Project& project, bool reimport, Optional<Error> unverified)
        : project_(project), reimport_(reimport), unverified_(std::move(unverified)) {}

    Result<void> stage(const std::filesystem::path& relative, std::string bytes, bool replace) {
        ATELIER_TRY_BIND(resolved, project_.resolve(relative));
        ATELIER_TRY_BIND(path, platform::windows_file_path(*resolved).transform_error(file_error));
        if (std::ranges::any_of(files_, [&](const auto& file) { return same_path(file.path, *path); }))
            return failure("Import outputs collide: " +
                           platform::utf16_to_utf8(relative.generic_wstring()).value_or("<invalid UTF-16 path>"));
        File file{std::move(*path), std::move(bytes)};
        std::error_code error;
        const bool exists = std::filesystem::exists(file.path, error);
        if (error)
            return std::unexpected(file_error(error));
        if (exists) {
            file.modified = std::filesystem::last_write_time(file.path, error);
            if (error)
                return std::unexpected(file_error(error));
            ATELIER_TRY_BIND(before, assets::read_file(file.path, 512ull << 20));
            if (!replace && (!reimport_ || file.bytes != *before)) {
                const auto name = platform::utf16_to_utf8(relative.generic_wstring()).value_or("<invalid UTF-16 path>");
                if (unverified_) {
                    auto cause = *unverified_;
                    cause.message += "; cannot overwrite " + name;
                    return std::unexpected(std::move(cause));
                }
                return failure("Import would overwrite an unrelated or unverified file: " + name);
            }
            file.before = std::move(*before);
        }
        files_.add(std::move(file));
        return {};
    }

    Result<void> write() {
        Array<std::filesystem::path> missing;
        for (auto& file : files_) {
            if (file.before && *file.before == file.bytes)
                continue;
            std::error_code error;
            missing.clear();
            for (auto parent = file.path.parent_path(); !std::filesystem::exists(parent, error);
                 parent = parent.parent_path()) {
                if (error)
                    return std::unexpected(file_error(error));
                if (parent == parent.root_path())
                    return failure("The import destination volume is unavailable.");
                missing.add(parent);
            }
            for (auto at = missing.rbegin(); at != missing.rend(); ++at) {
                if (std::filesystem::create_directory(*at, error))
                    directories_.add(*at);
                else if (error)
                    return std::unexpected(file_error(error));
            }
            ATELIER_TRY(
                write_in_directory(file.path, file.bytes, file.before ? WriteMode::replace : WriteMode::create));
            file.written = true;
        }
        return {};
    }

    Error rollback(Error first) const {
        const auto cleanup = [&](const std::filesystem::path& path, const std::string& message) {
            first.message += "; import rollback failed for " +
                             platform::utf16_to_utf8(path.native()).value_or("<invalid UTF-16 path>") + ": " + message;
        };
        for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
            if (!it->written)
                continue;
            std::error_code error;
            if (it->before) {
                if (auto restored = write_in_directory(it->path, *it->before, WriteMode::replace); !restored) {
                    cleanup(it->path, restored.error().message);
                    continue;
                }
                std::filesystem::last_write_time(it->path, it->modified, error);
            } else
                std::filesystem::remove(it->path, error);
            if (error)
                cleanup(it->path, platform::error_message(error));
        }
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it) {
            std::error_code error;
            std::filesystem::remove(*it, error);
            if (error)
                cleanup(*it, platform::error_message(error));
        }
        return first;
    }

private:
    struct File {
        std::filesystem::path path;

        std::string bytes;

        Optional<std::string> before{};

        std::filesystem::file_time_type modified{};

        bool written{};
    };

    const Project& project_;

    bool reimport_;

    Optional<Error> unverified_;

    Array<File> files_;

    Array<std::filesystem::path> directories_;
};
}

Result<void> write_file(const std::filesystem::path& path, std::string_view text, WriteMode mode) {
    ATELIER_TRY_BIND(native, platform::windows_file_path(path).transform_error(file_error));
    std::error_code error;
    std::filesystem::create_directories(native->parent_path(), error);
    if (error)
        return std::unexpected(file_error(error));
    return write_in_directory(*native, text, mode);
}

Result<std::filesystem::path> Project::resolve(const std::filesystem::path& relative) const {
    return assets::resolve_project_path(root_, relative);
}

Result<std::string> Project::read_scene(const std::filesystem::path& relative) const {
    ATELIER_TRY_BIND(path, resolve(relative));
    return assets::read_file(*path);
}

Result<Project> Project::create(const std::filesystem::path& folder) {
    if (folder.empty())
        return failure("Choose an empty project folder.");
    const auto native = platform::windows_file_path(folder);
    if (!native)
        return failure(platform::error_message(native.error()));
    std::error_code error;
    const auto exists = std::filesystem::exists(*native, error);
    if (error)
        return failure(platform::error_message(error));
    if (exists) {
        const auto status = std::filesystem::status(*native, error);
        if (error && !std::filesystem::status_known(status))
            return failure(platform::error_message(error));
        const auto directory = std::filesystem::is_directory(status);
        error.clear();
        const auto empty = directory && std::filesystem::is_empty(*native, error);
        if (error)
            return failure(platform::error_message(error));
        if (!empty)
            return failure("Create Project requires an empty folder; existing files were not changed.");
    }
    for (const auto* directory : {L"Config", L"Content/Scenes", L"Source", L"Cache"}) {
        const auto path = platform::windows_file_path(folder / directory);
        if (!path)
            return failure(platform::error_message(path.error()));
        std::filesystem::create_directories(*path, error);
        if (error)
            return failure(platform::error_message(error));
    }
    const auto canonical = std::filesystem::canonical(*native, error);
    if (error)
        return failure(platform::error_message(error));
    auto name = platform::utf16_to_utf8(canonical.filename().native());
    if (!name)
        return failure("Invalid UTF-16 text.");
    json::Builder config;
    static_cast<void>(config.set(config.root(), "version", std::uint32_t{1}));
    config.set_validated(config.root(), "name", *name);
    config.set_validated(config.root(), "startup_scene", "Content/Scenes/Main.scene");
    Session empty_scene;
    ATELIER_TRY(empty_scene.initialize());
    ATELIER_TRY_BIND(scene, empty_scene.serialize());
    ATELIER_TRY(write_file(folder / "Content/Scenes/Main.scene", *scene, WriteMode::create));
    ATELIER_TRY(write_file(folder / ".gitignore", "/Cache/\n", WriteMode::create));
    // Marker is published last.
    const auto bytes = json::write(config.root(), {true, true});
    ATELIER_TRY(write_file(folder / "Config/Project.json", {bytes.data(), bytes.size()}, WriteMode::create));
    return open(folder);
}

Result<Project> Project::open(const std::filesystem::path& folder) {
    Project project;
    ATELIER_TRY_BIND(native, platform::windows_file_path(folder).transform_error(file_error));
    std::error_code error;
    project.root_ = std::filesystem::canonical(*native, error);
    if (error)
        return failure(platform::error_message(error));
    project.assets_ = assets::Reader(project.root_);
    ATELIER_TRY_BIND(config_path, project.resolve("Config/Project.json"));
    ATELIER_TRY_BIND(config, read_json(*config_path));
    ATELIER_TRY_BIND(settings, read_settings(config->root()));
    project.name_ = std::move(settings->name);
    project.startup_ = std::move(settings->startup);
    project.ui_source_ = std::move(settings->ui_source);
    if (!project.ui_source_.empty())
        ATELIER_TRY(project.resolve(project.ui_source_));
    ATELIER_TRY(project.resolve(project.startup_));
    ATELIER_TRY(project.refresh());
    return project;
}

Result<Array<std::filesystem::path>> Project::source_paths() const {
    Array<std::filesystem::path> paths;
    const std::filesystem::path source{"Source"};
    auto scanned =
        visit_project_directory(*this, source, [&](const auto& file, auto status, const auto& root) -> Result<void> {
            if (std::filesystem::is_regular_file(status) && file.path().extension() == L".source" &&
                !file.path().filename().native().ends_with(L".ui.source")) {
                auto relative = source / file.path().lexically_relative(root);
                ATELIER_TRY(resolve(relative));
                if (paths.size() >= 256)
                    return failure("Project exceeds the 256 Source file limit.");
                paths.add(std::move(relative));
            }
            return {};
        });
    ATELIER_TRY(scanned);
    std::ranges::sort(paths);
    return paths;
}

Result<void> Project::create_source(const std::filesystem::path& relative, std::string_view text) {
    if (relative.empty() || *relative.begin() != L"Source" || relative.extension() != L".source")
        return failure("Create source files inside the project's Source directory.");
    const auto path = resolve(relative);
    if (!path)
        return std::unexpected(path.error());
    if (text.size() > 1024 * 1024)
        return failure("Source file exceeds the 1 MiB limit.");
    if (!StringView(text).is_valid_utf8())
        return failure("Invalid UTF-8 text.");
    return write_file(*path, text, WriteMode::create);
}

Result<void> Project::set_ui_source(const std::filesystem::path& relative) {
    if (!relative.empty() &&
        (*relative.begin() != L"Source" || !relative.filename().wstring().ends_with(L".ui.source")))
        return failure("Project UI must be a .ui.source file inside Source.");
    if (!relative.empty())
        ATELIER_TRY(resolve(relative));
    ATELIER_TRY_BIND(path, resolve("Config/Project.json"));
    ATELIER_TRY_BIND(config, read_json(*path));
    ATELIER_TRY_BIND(settings, read_settings(config->root()));
    if (settings->name != name_ || settings->startup != startup_ || settings->ui_source != ui_source_)
        return failure("Project configuration changed on disk. Reopen the project before changing its UI entry.");
    json::Builder next_config(*config);
    if (relative.empty())
        next_config.erase(next_config.root(), "ui_source");
    else {
        auto next = platform::utf16_to_utf8(relative.generic_wstring());
        if (!next)
            return failure("Invalid UTF-16 text.");
        next_config.set_validated(next_config.root(), "ui_source", *next);
    }
    const auto bytes = json::write(next_config.root(), {true, true});
    ATELIER_TRY(write_file(*path, {bytes.data(), bytes.size()}));
    ui_source_ = relative;
    return {};
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
    if (!native_source)
        return failure(platform::error_message(native_source.error()));
    const auto native_target = platform::windows_file_path(*target);
    if (!native_target)
        return failure(platform::error_message(native_target.error()));
    std::error_code error;
    if (!std::filesystem::exists(*native_source, error))
        return failure("The asset no longer exists on disk.");
    // Case-only renames are the same file on Windows; anything else must not overwrite.
    if (std::filesystem::exists(*native_target, error) &&
        !std::filesystem::equivalent(*native_source, *native_target, error))
        return failure("Another file or folder already has that name.");
    const bool has_sidecar = !std::filesystem::is_directory(*native_source, error) &&
                             std::filesystem::exists(sidecar(*native_source), error);
    std::filesystem::rename(*native_source, *native_target, error);
    if (error)
        return failure("The file could not be renamed; it may be open in another program.");
    if (has_sidecar) {
        std::filesystem::rename(sidecar(*native_source), sidecar(*native_target), error);
        if (error) {
            std::error_code undo;
            std::filesystem::rename(*native_target, *native_source, undo);
            return failure("The asset identity file could not be moved; the rename was undone.");
        }
    }
    ATELIER_TRY(refresh());
    return target_relative;
}

Span<const std::filesystem::path> Project::roots() noexcept {
    static const std::array<std::filesystem::path, 3> directories{L"Content", L"Source", L"Config"};
    return directories;
}

Result<Array<assets::ProjectEntry>> Project::scan_entries(bool create_identities) const {
    using enum assets::AssetKind;
    Array<assets::ProjectEntry> entries;
    for (const auto& directory : roots()) {
        entries.add({directory, directory.string(), folder});
        const auto collect = [&](const std::filesystem::directory_entry& file, std::filesystem::file_status status,
                                 const std::filesystem::path& root) -> Result<void> {
            const auto& path = file.path();
            const auto filename = path.filename().native();
            if (filename.ends_with(L".asset.json") || filename.ends_with(L".tmp"))
                return {};
            auto relative = directory / path.lexically_relative(root);
            ATELIER_TRY(resolve(relative));
            auto name = platform::utf16_to_utf8(relative.generic_wstring());
            if (!name)
                return failure("Invalid UTF-16 text.");
            auto& entry = entries.emplace(std::move(relative), std::move(*name), folder);
            if (std::filesystem::is_directory(status))
                return {};
            if (const auto kind = resource_kind(path, directory)) {
                entry.kind = *kind;
                if (auto identity = read_asset_identity(*this, entry, create_identities); !identity) {
                    identity.error().message = entry.key + ".asset.json: " + identity.error().message;
                    return identity;
                }
            } else
                entry.kind = path.extension() == L".scene" ? scene
                             : directory == L"Source"      ? source
                                                           : assets::AssetKind::file;
            std::error_code error;
            entry.bytes = file.file_size(error);
            if (error)
                return std::unexpected(file_error(error));
            entry.modified = file.last_write_time(error);
            if (error)
                return std::unexpected(file_error(error));
            return {};
        };
        ATELIER_TRY(visit_project_directory(*this, directory, collect));
    }
    std::ranges::sort(entries, {}, &assets::ProjectEntry::path);
    return entries;
}

Result<void> Project::refresh() {
    ATELIER_TRY_BIND(entries, scan_entries(true));
    return assets_.publish(std::move(*entries));
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
    if (!assets::is_mesh_path(source))
        return failure("Import Mesh accepts Wavefront OBJ or glTF 2.0 (.gltf) geometry and its dependencies.");
    const std::string requested(existing_key); // The caller may borrow its key from this Reader.
    std::error_code error;
    const auto absolute = std::filesystem::absolute(source, error);
    if (error)
        return failure(platform::error_message(error));
    assets::Reader current = assets_;
    ATELIER_TRY_BIND(prepared, current.prepare_mesh(absolute));
    auto loaded = std::move(*prepared);
    // A read-only scan cannot create unrelated sidecars during a failed import.
    ATELIER_TRY_BIND(scanned, scan_entries(false));
    ATELIER_TRY(current.publish(std::move(*scanned)));
    const auto catalog = current.entries();
    const auto file_entry = [&](const std::filesystem::path& path) -> const assets::ProjectEntry* {
        const auto at = std::ranges::find_if(catalog, [&](const auto& entry) { return same_path(entry.path, path); });
        return at == catalog.end() ? nullptr : &*at;
    };
    auto relative = std::filesystem::path("Content/Meshes");
    if (loaded.files.size() > 1)
        relative /= source.stem();
    auto target_relative = relative / source.filename();
    std::string resource = requested;
    if (requested.empty()) {
        ATELIER_TRY_BIND(id, identity());
        resource = "project/" + *id;
    }
    json::Builder metadata;
    json::Builder defaults;
    Array<std::filesystem::path> owned;
    Optional<Error> dependency_error;
    if (!requested.empty()) {
        const auto* at = current.entry(requested);
        if (!at || at->kind != assets::AssetKind::mesh)
            return failure("Reimport requires an existing project mesh identity.");
        target_relative = at->path;
        relative = target_relative.parent_path();
        if (!same_path(source.extension(), target_relative.extension()))
            return failure("Reimport must retain the mesh format (.obj or .gltf).");
        ATELIER_TRY_BIND(metadata_path, resolve(sidecar(target_relative)));
        ATELIER_TRY_BIND(previous_metadata, read_json(*metadata_path));
        metadata = json::Builder(*previous_metadata);
        owned.add(target_relative);
        // Current raw dependencies, not material closure, prove which existing
        // files this operation may replace. There is no import ownership history.
        auto previous = resolve(target_relative).and_then([&](const auto& path) { return current.prepare_mesh(path); });
        if (previous) {
            for (const auto& file : previous->files)
                owned.add(relative / file.relative);
        } else {
            dependency_error = std::move(previous.error());
            dependency_error->message =
                "Existing mesh dependencies could not be verified: " + dependency_error->message;
            loaded.notes.add(dependency_error->message + "; only unchanged existing dependencies may be reused.");
        }
        auto main = std::ranges::find(loaded.files, source.filename(), &assets::ImportedFile::relative);
        if (main == loaded.files.end())
            return failure("Prepared mesh has no source file.");
        main->relative = target_relative.filename();
    }
    MeshImportFiles files{*this, !requested.empty(), std::move(dependency_error)};
    // Prepare exact input pixels and persistent texture keys before moving the
    // immutable file bytes into the common create/replace transaction.
    Map<std::filesystem::path, std::string> texture_keys;
    Array<std::string_view> image_bytes;
    MeshImportPublication result;
    const auto prepare_texture = [&](const std::filesystem::path& file) -> Result<void> {
        if (file.empty() || texture_keys.contains(file))
            return {};
        const auto* existing = file_entry(relative / file);
        if (existing && existing->kind != assets::AssetKind::texture)
            return failure("Imported texture path names another resource kind.");
        std::string key;
        if (existing)
            key = existing->key;
        else {
            ATELIER_TRY_BIND(id, identity());
            key = "project/" + *id;
        }
        texture_keys.try_emplace(file, key);
        const auto bytes = std::ranges::find(loaded.files, file, &assets::ImportedFile::relative);
        if (bytes == loaded.files.end())
            return failure("Imported texture is absent from the prepared bundle.");
        image_bytes.add(bytes->bytes);
        result.textures.add({key, file.stem().wstring(), {}});
        return {};
    };
    for (const auto& inputs : loaded.textures)
        for (const auto& file : inputs)
            ATELIER_TRY(prepare_texture(file));
    // Saved or unsaved user MIs may still reference a bundle texture that the
    // new source material stopped sampling. Publish every existing texture
    // asset touched by this reimport, not only the new source's active inputs.
    if (!requested.empty())
        for (const auto& file : loaded.files)
            if (const auto* existing = file_entry(relative / file.relative);
                existing && existing->kind == assets::AssetKind::texture)
                ATELIER_TRY(prepare_texture(file.relative));
    ATELIER_TRY_BIND(images, current.prepare_images(image_bytes, 2ull << 30));
    for (std::size_t i = 0; i < images->size(); ++i)
        result.textures[i].image = (*images)[i];
    for (auto& file : loaded.files) {
        const auto path = relative / file.relative;
        auto filename = path.filename().wstring();
        for (auto& c : filename)
            c = static_cast<wchar_t>(std::towlower(c));
        if (filename.ends_with(L".asset.json") || filename.ends_with(L".tmp"))
            return failure("Import dependencies cannot replace identity or temporary files.");
        const bool replace = std::ranges::any_of(owned, [&](const auto& old) { return same_path(old, path); });
        // A source dependency is not permission to overwrite a hand-edited MI/MS.
        const auto* existing = file_entry(path);
        if (existing &&
            (existing->kind == assets::AssetKind::material || existing->kind == assets::AssetKind::procedural_graph))
            return failure("Import cannot overwrite an existing author file.");
        ATELIER_TRY(files.stage(path, std::move(file.bytes), replace));
        if (same_path(path, target_relative))
            continue;
        if (assets::is_texture_path(path) || assets::is_mesh_path(path) || assets::is_sky_path(path) ||
            path.extension() == L".material") {
            if (!file_entry(path)) {
                std::string key;
                if (const auto* texture = texture_keys.find(file.relative))
                    key = *texture;
                else {
                    ATELIER_TRY_BIND(id, identity());
                    key = "project/" + *id;
                }
                ATELIER_TRY(files.stage(sidecar(path), identity_file(key), false));
            }
        }
    }
    Set<std::string> returned_materials;
    Set<std::string> slots;
    Array<std::wstring> names;
    for (const auto& entry : catalog)
        if (same_path(entry.path.parent_path(), relative / "Materials"))
            names.add(entry.path.filename().wstring());
    const auto mapping = metadata.root().find("materials");
    if (!mapping)
        return failure(mapping.error().describe());
    for (std::size_t i = 0; i < loaded.mesh->materials().size(); ++i) {
        const auto slot = geometry::material_slot_name(*loaded.mesh, i);
        if (!slots.add(slot))
            return failure("Reimport requires unique material slot names.");
        Optional<json::Value> saved;
        if (*mapping) {
            const auto at = (*mapping)->find(slot);
            if (!at)
                return failure(at.error().describe());
            saved = *at;
        }
        if (saved) {
            const auto text = saved->string();
            if (!text)
                return failure(text.error().describe());
            const std::string key(*text);
            if (auto written = defaults.set(defaults.root(), slot, *saved); !written)
                return failure(written.error().describe());
            loaded.notes.add("Preserved material binding and author values for slot '" + slot +
                             "'; source material changes were not merged.");
            if (returned_materials.add(key)) {
                if (auto material = current.material(key))
                    result.materials.add(std::move(*material));
                else
                    loaded.notes.add("Preserved unavailable material " + key + ": " + material.error().message);
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
                auto* binding =
                    entry == author.entries.end() ? nullptr : std::get_if<material::TextureBinding>(&entry->value);
                if (!binding)
                    return failure("Imported texture has no typed MI override.");
                const auto* texture = texture_keys.find(file);
                if (!texture)
                    return failure("Imported material references a texture outside the prepared bundle.");
                binding->asset_key = *texture;
            }
        ATELIER_TRY_BIND(filename, material_file(slot, names));
        if (assets::is_material_source_path(*filename)) {
            filename = material_file(slot + " (MI)", names);
            if (!filename)
                return std::unexpected(std::move(filename.error()));
        }
        const auto path = relative / "Materials" / *filename;
        ATELIER_TRY_BIND(id, identity());
        const auto key = "project/" + *id;
        assets::MaterialAsset asset{key, path.stem().wstring(), std::move(author)};
        const auto encoded = assets::material_format::encode(asset);
        if (!encoded)
            return failure(encoded.error().describe());
        const auto bytes = json::write(encoded->root(), {true, true});
        ATELIER_TRY(files.stage(path, {bytes.data(), bytes.size()}, false));
        ATELIER_TRY(files.stage(sidecar(path), identity_file(key), false));
        if (auto written = defaults.set(defaults.root(), slot, key); !written)
            return failure(written.error().describe());
        result.materials.add(std::move(asset));
    }
    if (defaults.root().size())
        metadata.set_validated(metadata.root(), "materials", defaults.root());
    else
        metadata.erase(metadata.root(), "materials");
    ATELIER_TRY(
        files.stage(sidecar(target_relative), identity_file(resource, std::move(metadata)), !requested.empty()));
    const auto commit = [&]() -> Result<assets::MeshAsset> {
        ATELIER_TRY(files.write());
        ATELIER_TRY_BIND(updated, scan_entries(false));
        return assets_.publish_mesh(std::move(current), std::move(*updated), resource, loaded, result.textures);
    };
    auto published = commit();
    if (!published)
        return std::unexpected(files.rollback(std::move(published.error())));
    result.notes = std::move(loaded.notes);
    result.mesh = std::move(*published);
    return result;
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

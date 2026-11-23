#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "application/diagnostics.hpp"
#include "foundation/result.hpp"
#include "logging/native.hpp"
#include "assets/file.hpp"
#include "platform/utf.hpp"
#include <cstdio>
#include <shobjidl.h>
#include <wrl/client.h>

namespace atelier::application {
namespace {
auto failed(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_storage, std::move(message)});
}

constexpr const wchar_t* placeholder_notice =
    L"Project types are unavailable until the Source build succeeds. Project objects load as placeholders and keep their saved values; fix the build, then Rebuild Types.";

using Pick = editor::PathPurpose;

Result<std::optional<std::filesystem::path>> pick(HWND owner, Pick kind, const std::filesystem::path& initial = {},
                                                  const wchar_t* filename = L"Move.source") {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return failed("Cannot initialize the native file dialog.");
    struct Apartment {
        bool owns;

        ~Apartment() {
            if (owns)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(initialized)};
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    const bool material = kind == Pick::material_save || kind == Pick::material_source_save;
    const bool procgraph = kind == Pick::procedural_graph_save;
    const bool save = kind == Pick::scene_save || kind == Pick::source_save || material || procgraph;
    auto hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        return failed("Cannot create the native file dialog.");
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (kind == Pick::folder) {
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Choose a project folder");
    } else {
        const bool source = kind == Pick::source || kind == Pick::source_save;
        const COMDLG_FILTERSPEC type{source                  ? L"Source and text files"
                                     : kind == Pick::mesh    ? L"OBJ or glTF models"
                                     : kind == Pick::texture ? L"Textures"
                                     : material              ? L"Material"
                                     : procgraph             ? L"Procedural Graph"
                                                             : L"Atelier scene",
                                     source                  ? L"*.source;*.ss;*.txt;*.json"
                                     : kind == Pick::mesh    ? L"*.obj;*.gltf"
                                     : kind == Pick::texture ? L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff"
                                     : material              ? L"*.material"
                                     : procgraph             ? L"*.procgraph"
                                                             : L"*.scene"};
        dialog->SetFileTypes(1, &type);
        dialog->SetDefaultExtension(source                  ? L"source"
                                    : kind == Pick::mesh    ? L"obj"
                                    : kind == Pick::texture ? L"png"
                                    : material              ? L"material"
                                    : procgraph             ? L"procgraph"
                                                            : L"scene");
        if (!save)
            options |= FOS_FILEMUSTEXIST;
        if (kind == Pick::source_save)
            dialog->SetFileName(filename);
        if (kind == Pick::material_save)
            dialog->SetFileName(L"Material.material");
        if (kind == Pick::material_source_save)
            dialog->SetFileName(L"Material.ms.material");
        if (procgraph)
            dialog->SetFileName(L"Terrain.procgraph");
    }
    dialog->SetOptions(options);
    if (!initial.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dialog->SetFolder(folder.Get());
    }
    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return std::optional<std::filesystem::path>{};
    if (FAILED(hr))
        return failed("File dialog failed (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ").");
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return failed("Cannot read the selected location.");
    PWSTR path{};
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return failed("The selected location is not a file-system path.");
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return std::optional{result};
}
}

Result<std::optional<std::filesystem::path>> Application::choose_path(editor::PathPurpose purpose,
                                                                      const std::filesystem::path& initial,
                                                                      std::wstring_view filename) {
    close_menu(false);
    return pick(window(), purpose, initial, filename.empty() ? L"Move.source" : std::wstring(filename).c_str());
}

void Application::asset_created(std::string_view key, bool inspect_material) {
    if (const auto* entry = project_->entry(key))
        workspace_.assets.navigate(entry->path.parent_path());
    if (inspect_material) {
        workspace_.material_inspection = key;
        workspace_.inspecting_asset = true;
    }
    workspace_.status = inspect_material ? L"Material created" : L"Resource imported";
}

Result<bool> Application::confirm_discard(bool sources, bool interactive) {
    if (!interactive) {
        if (document_->dirty() || !document_->dirty_materials().empty() || !document_->dirty_graphs().empty() ||
            (sources &&
             std::ranges::any_of(workspace_.source_editor.files(), [](const auto& file) { return file->dirty(); })))
            return failed(
                "Save the current scene, material assets, procedural graphs and source drafts before replacing the document.");
        return true;
    }
    if (const auto edit = ui_.commit_edit(); edit) {
        if (!effects(*edit))
            return failed("The active field could not be committed.");
    } else
        return failed("Finish the active field before replacing the document. " + ui_error(edit.error()).message);
    ATELIER_TRY(finish_color_edit(true));
    if (sources)
        for (const auto& file : workspace_.source_editor.files()) {
            const auto confirmed = confirm_source(*file);
            if (!confirmed)
                return std::unexpected(confirmed.error());
            if (!*confirmed)
                return false;
        }
    for (const auto& key : document_->dirty_materials()) {
        const auto response = MessageBoxW(
            window(), (L"Save changes to material " + platform::utf8_to_utf16_validated(key) + L"?").c_str(),
            L"Unsaved material", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (response == IDCANCEL)
            return false;
        if (response == IDYES) {
            if (!project_)
                return failed("Open a project before saving material assets.");
            const auto saved = document_->save_material(*project_, key);
            if (!saved)
                return std::unexpected(saved.error());
        }
    }
    for (const auto& key : document_->dirty_graphs()) {
        const auto response = MessageBoxW(
            window(), (L"Save changes to procedural graph " + platform::utf8_to_utf16_validated(key) + L"?").c_str(),
            L"Unsaved procedural graph", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (response == IDCANCEL)
            return false;
        if (response == IDYES) {
            if (!project_)
                return failed("Open a project before saving procedural graph assets.");
            const auto saved = document_->save_graph(*project_, key);
            if (!saved)
                return std::unexpected(saved.error());
        }
    }
    if (!document_->dirty())
        return true;
    const auto response = MessageBoxW(window(), L"Save changes to the current scene?", L"Unsaved scene",
                                      MB_YESNOCANCEL | MB_ICONQUESTION);
    if (response == IDCANCEL)
        return false;
    if (response == IDNO)
        return true;
    const auto saved = save_scene();
    if (!saved) {
        workspace_.status = diagnostic_text(saved.error().message);
        MessageBoxW(window(), workspace_.status.c_str(), L"Scene was not saved", MB_OK | MB_ICONERROR);
        return std::unexpected(saved.error());
    }
    return true;
}

Result<void> Application::publish_document(std::unique_ptr<editor::Session> next, editor::ProjectCode* code,
                                           const std::function<void()>& commit_location, bool preserve_view) {
    const auto previous_document = document_->presentation().identity();
    const editor::Camera saved_camera = camera_;
    std::vector<std::uint64_t> saved_selected_entities;
    std::optional<std::uint64_t> saved_active_entity;
    std::optional<std::uint64_t> saved_inspected_object;
    if (preserve_view) {
        for (const auto entity : document_->selected_entities()) {
            if (const auto lid = document_->document_identity(entity))
                saved_selected_entities.push_back(*lid);
        }
        saved_active_entity = document_->document_identity(document_->selection().entity);
        saved_inspected_object = document_->document_identity(document_->selection().object);
    }

    restart_play_ = false;
    finish_camera_fly(false);
    camera_fly_release_.reset();
    render_retry_at_ = {};
    render_retry_delay_ = {};
    const auto cancelled = viewport_tool_.cancel(*document_);
    if (!cancelled)
        return std::unexpected(cancelled.error());
    viewport_tool_.cursor = {};
    accepted_view_.reset();
    cancel_interaction(ui_.cancel_interaction());
    auto cleared = ui_.set_controls({});
    if (!cleared) {
        auto error = ui_error(cleared.error());
        error.message = "Cannot retire the previous document UI. " + error.message;
        return std::unexpected(std::move(error));
    }
    effects(*cleared);
    if (cleared->error)
        return std::unexpected(ui_error(*cleared->error));
    sync_text_input();
    if (popup_)
        ATELIER_TRY(popup_->reset_document());
    for (auto& popup : submenus_)
        ATELIER_TRY(popup->reset_document());
    diagnostics();
    const auto closed = document_->close();
    diagnostics();
    if (!closed)
        return std::unexpected(closed.error());
    if (commit_location)
        commit_location();
    if (control_)
        control_->document_replaced();
    workspace_.reset_document(preserve_view);
    document_ = std::move(next);
    project_code_ = code;
    document_->bind_project_code(code);
    scene_.reset();
    procedural_preview_.reset();
    workspace_.render_notice.clear();
    if (preserve_view) {
        camera_ = saved_camera;
        workspace_.camera_view = camera_.view();
        std::vector<ObjectId> restored_entities;
        restored_entities.reserve(saved_selected_entities.size());
        for (const auto lid : saved_selected_entities) {
            if (const auto resolved = document_->local_object(lid))
                restored_entities.push_back(*resolved);
        }
        ObjectId restored_active{};
        if (saved_active_entity) {
            if (const auto resolved = document_->local_object(*saved_active_entity))
                restored_active = *resolved;
        }
        if (restored_entities.size() > 1) {
            static_cast<void>(document_->select_entities(restored_entities, restored_active));
        } else if (saved_inspected_object) {
            if (const auto resolved = document_->local_object(*saved_inspected_object))
                static_cast<void>(document_->select(*resolved));
            else if (!restored_entities.empty())
                static_cast<void>(document_->select_entities(restored_entities, restored_active));
        } else if (!restored_entities.empty()) {
            static_cast<void>(document_->select_entities(restored_entities, restored_active));
        }
        workspace_.reconcile_rebuild(*document_, project_ ? &*project_ : nullptr);
    } else {
        camera_.reset();
    }
    sync_world_view();
    camera_cut_ = true;
    invalidate_surface();
    dropped_diagnostics_ = 0;
    document_root_ = project_ ? project_->root() : std::filesystem::path{};
    auto root = platform::utf16_to_utf8(document_root_.generic_wstring());
    auto scene = platform::utf16_to_utf8(scene_path_.generic_wstring());
    if (!root || !scene) {
        std::fputs("Cannot log document adoption: invalid UTF-16 path; the adopted document is unchanged.\n", stderr);
        return {};
    }
    const std::array fields{logging::Field{"previous_document", previous_document},
                            logging::Field{"document", document_->presentation().identity()},
                            logging::Field{"project_root", std::move(*root)},
                            logging::Field{"scene", std::move(*scene)}};
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "application", "document.adopted",
                                       logging::native::Context{fields}, "Editor document adopted"));
    return {};
}

Result<void> Application::open_project(const std::filesystem::path& folder, bool create, ProjectOpenPolicy policy,
                                       bool interactive) {
    // A save decision can change the file we are about to open. Read only after
    // that decision; cancellation also precedes Project::create's disk changes.
    const auto confirmed = confirm_discard(true, interactive);
    if (!confirmed)
        return std::unexpected(confirmed.error());
    if (!*confirmed) {
        cancel_operator();
        return {};
    }
    ATELIER_TRY_BIND(project, create ? editor::Project::create(folder) : editor::Project::open(folder));
    ATELIER_TRY_BIND(content, project->read_scene(project->startup_scene()));
    ATELIER_TRY_BIND(candidate, prepare_project_scene(std::move(*project), std::move(*content), policy));
    return publish_project(std::move(*candidate));
}

Result<Application::ProjectCandidate> Application::prepare_project_scene(editor::Project project, std::string content,
                                                                         ProjectOpenPolicy policy) {
    auto owner = std::make_unique<editor::Project>(std::move(project));
    const bool strict = policy == ProjectOpenPolicy::require_scene;
    auto candidate =
        editor::load_project_scene(*owner, content, strict ? editor::LoadPolicy::strict : editor::LoadPolicy::tolerant,
                                   !strict, editor::SessionRole::editor);
    if (strict) {
        if (candidate.error)
            return std::unexpected(std::move(*candidate.error));
        if (!candidate.session)
            return failed("Project did not produce a complete scene.");
    }
    return ProjectCandidate{std::move(owner), std::move(content), std::move(candidate)};
}

Result<void> Application::publish_project(ProjectCandidate prepared) {
    auto& candidate = prepared.document;
    const bool scene_available = candidate.session != nullptr;
    if (!scene_available) {
        candidate.session = std::make_unique<editor::Session>();
        ATELIER_TRY(candidate.session->initialize({}, editor::SessionRole::editor));
    }
    play_.close();
    consume_play_output();
    ATELIER_TRY(publish_document(std::move(candidate.session), candidate.code, [&] {
        scene_path_ = prepared.project->startup_scene();
        project_ = std::move(prepared.project);
        pending_scene_ = std::move(prepared.scene_text);
        workspace_.project_open = true;
        workspace_.scene_available = scene_available;
    }));
    workspace_.source_editor.clear();
    if (candidate.error) {
        auto report = std::make_shared<editor::ProjectBuild>();
        if (candidate.build)
            *report = *candidate.build;
        report->error = candidate.error;
        report->state = editor::BuildState::failed;
        candidate.build = std::move(report);
    }
    publish_build(candidate.build);
    workspace_.project_ui_configured = !project_->ui_source().empty();
    if (workspace_.scene_available && candidate.error)
        notice("document.placeholders", placeholder_notice, logging::Severity::warning);
    if (workspace_.scene_available)
        report_load(*document_, document_root_);
    else if (candidate.error)
        report_error(*candidate.error, "document.rejected");
    if (candidate.error) {
        workspace_.dock.select(editor::source_control::tab);
        if (candidate.build && candidate.build->sources && !candidate.build->sources->files.empty()) {
            auto name = platform::utf8_to_utf16(candidate.build->sources->files.front().path);
            auto source_path = name ? project_->resolve(*name) : failed("Invalid UTF-8 text.");
            if (source_path)
                return open_source(*source_path);
        }
    }
    return {};
}

Result<void> Application::open_scene(const std::filesystem::path& relative, bool interactive) {
    if (!project_)
        return failed("Open a project folder first.");
    const auto confirmed = confirm_discard(false, interactive);
    if (!confirmed)
        return std::unexpected(confirmed.error());
    if (!*confirmed) {
        cancel_operator();
        return {};
    }
    ATELIER_TRY_BIND(text, project_->read_scene(relative));
    auto next =
        editor::load_project_scene(*project_, *text, editor::LoadPolicy::tolerant, true, editor::SessionRole::editor);
    if (!next.session) {
        publish_build(next.build);
        return std::unexpected(*next.error);
    }
    play_.close();
    consume_play_output();
    ATELIER_TRY(publish_document(std::move(next.session), next.code, [&] {
        scene_path_ = relative;
        pending_scene_ = std::move(*text);
        workspace_.scene_available = true;
    }));
    publish_build(next.build);
    if (next.error)
        notice("document.placeholders", placeholder_notice, logging::Severity::warning);
    report_load(*document_, document_root_);
    return {};
}

Result<void> Application::save_scene(const std::filesystem::path& relative) {
    if (!project_)
        return failed("Create or open a project folder before saving a scene.");
    if (!workspace_.scene_available)
        return failed(
            "Repair the project build and reload its scene before saving. The original scene has been preserved.");
    const auto edit = ui_.commit_edit();
    if (!edit)
        return failed("Correct the active field before saving. " + ui_error(edit.error()).message);
    if (!effects(*edit))
        return failed("The active field could not be committed; the scene was not saved.");
    ATELIER_TRY(finish_color_edit(true));
    const auto destination = relative.empty() ? scene_path_ : relative;
    if (!editor::Project::is_scene_path(destination))
        return failed("Save scenes inside the project's Content folder with a .scene extension.");
    const auto path = project_->resolve(destination);
    if (!path)
        return std::unexpected(path.error());
    const auto text = document_->serialize();
    if (!text)
        return std::unexpected(text.error());
    ATELIER_TRY(editor::write_file(*path, *text));
    scene_path_ = destination;
    document_->mark_saved();
    const auto refreshed = project_->refresh();
    if (!refreshed)
        report_error(refreshed.error(), "editor.asset_refresh_failed");
    workspace_.status = refreshed ? L"Scene saved"
                                  : L"Scene saved; asset refresh failed: " + diagnostic_text(refreshed.error().message);
    auto pending = document_->issues();
    const auto kept = pending.size();
    std::erase_if(pending, [](const auto& item) { return item.kind != world::DiagnosticKind::reference_dropped; });
    report_world(*document_, pending, document_root_, "save");
    if (kept)
        workspace_.status += L" (" + std::to_wstring(kept) + L" unresolved saved item(s) kept)";
    return {};
}

Result<void> Application::reimport_asset(std::string_view key, const std::filesystem::path& path) {
    close_menu(false);
    const auto* entry = project_ ? project_->entry(key) : nullptr;
    if (!entry || (entry->kind != assets::AssetKind::texture && entry->kind != assets::AssetKind::mesh))
        return failed("Select a matching project resource to reimport.");
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return failed("Finish the active edit before reimporting.");
    ATELIER_TRY(finish_color_edit(true));
    ATELIER_TRY(entry->kind == assets::AssetKind::texture ? document_->reimport_texture(*project_, key, path)
                                                          : document_->reimport_mesh(*project_, key, path));
    workspace_.status = L"Resource reimported; current content published";
    return {};
}

Result<std::string> Application::import_asset(editor::ImportKind kind, const std::filesystem::path& path) {
    close_menu(false);
    if (kind == editor::ImportKind::texture) {
        ATELIER_TRY_BIND(created, project_->import_texture(path));
        if (const auto* at = project_->entry(*created))
            workspace_.assets.navigate(at->path.parent_path());
        workspace_.status = L"Texture imported";
        return created;
    }
    ATELIER_TRY_BIND(imported, project_->import_mesh(path));
    for (const auto& note : project_->assets().mesh_notes(*imported))
        notice("asset.mesh_import_note", L"Import Mesh: " + diagnostic_text(note));
    workspace_.assets.reset(true);
    workspace_.assets.navigate(L"Content/Meshes");
    return imported;
}

Result<void> Application::open_source(const std::filesystem::path& path) {
    close_menu(false);
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return failed("Finish the current edit before opening source.");
    ATELIER_TRY(workspace_.source_editor.open(path));
    workspace_.dock.select(editor::source_control::tab);
    pending_source_focus_ = true;
    return {};
}

Result<void> Application::save_source(editor::SourceEditor::File& file, bool interactive) {
    const auto edit = ui_.commit_edit();
    if (!edit || !effects(*edit))
        return failed("Finish the current edit before saving source.");
    auto saved = workspace_.source_editor.save(file);
    if (!saved && file.conflict && interactive) {
        const auto message = L"The file changed outside the editor:\n" + file.path.wstring() +
                             L"\n\nOverwrite the external changes with this document?";
        if (MessageBoxW(window(), message.c_str(), L"Source file conflict",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES)
            saved = workspace_.source_editor.save(file, true);
    }
    if (saved) {
        workspace_.status = L"Source saved";
        source_build_pending_ = true;
        if (project_) {
            ATELIER_TRY_BIND(materials, document_->material_assets());
            for (const auto& asset : *materials) {
                const auto* author = std::get_if<assets::MaterialSourceAsset>(&asset.author);
                if (!author || author->source_path.starts_with("builtin/"))
                    continue;
                const auto path = project_->resolve(platform::utf8_to_utf16_validated(author->source_path));
                if (!path || _wcsicmp(path->lexically_normal().c_str(), file.path.lexically_normal().c_str()) != 0)
                    continue;
                // The saved editor buffer is authoritative; another disk write
                // must not change the publication or the subsequent Live payload.
                auto text = platform::utf16_to_utf8(file.document->text());
                if (!text)
                    return failed("Invalid UTF-16 text.");
                auto disk_path = platform::utf16_to_utf8(file.path.generic_wstring());
                if (!disk_path)
                    return failed("Invalid UTF-16 text.");
                source::SourceFile exact{author->source_path, source::content_hash(*text), std::move(*text),
                                         std::move(*disk_path)};
                std::vector<editor::MissingResource> missing;
                ATELIER_TRY_BIND(published,
                                 document_->publish_material(project_->assets(), asset, std::move(exact), &missing));
                for (const auto& issue : missing)
                    report_error(issue.error, "material.resource_missing", logging::Severity::warning);
                if (!missing.empty())
                    workspace_.status = L"Source saved; material diagnostics need attention";
            }
        }
    }
    return saved;
}

Result<bool> Application::confirm_source(editor::SourceEditor::File& file, bool interactive) {
    if (!file.dirty())
        return true;
    if (!interactive)
        return failed("Save the source draft before closing it.");
    const auto message = L"Save changes to " + file.path.filename().wstring() + L"?";
    const auto response =
        MessageBoxW(window(), message.c_str(), L"Unsaved source document", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (response == IDCANCEL)
        return false;
    if (response == IDNO)
        return true;
    const auto saved = save_source(file);
    if (!saved) {
        workspace_.status = diagnostic_text(saved.error().message);
        MessageBoxW(window(), workspace_.status.c_str(), L"Source was not saved", MB_OK | MB_ICONERROR);
        return std::unexpected(saved.error());
    }
    return true;
}

Result<void> Application::save_documents(const editor::Context& context, bool all) {
    const auto& material = context.asset;
    close_menu(false);
    auto* file = workspace_.source_editor.active();
    if (!all && context.region == editor::RegionKind::code && file)
        return save_source(*file, context.interactive);
    const auto target_graph = all ? std::string{} : context.focused_graph();
    if (!all && target_graph.empty() && !material.empty()) {
        auto edit = ui_.commit_edit();
        if (!edit || !effects(*edit))
            return failed("Finish the material field before saving.");
        ATELIER_TRY(finish_color_edit(true));
        if (!project_)
            return failed("No project is open.");
        ATELIER_TRY(document_->save_material(*project_, material));
        const auto refreshed = project_->refresh();
        workspace_.status = refreshed ? L"Material saved" : L"Material saved; catalog refresh failed";
        return {};
    }
    if (!all) {
        if (!target_graph.empty()) {
            auto edit = ui_.commit_edit();
            if (!edit || !effects(*edit))
                return failed("Finish the active field before saving.");
            ATELIER_TRY(finish_color_edit(true));
            if (!project_ || !project_->entry(target_graph))
                return failed("Create a project .procgraph asset before saving this procedural graph.");
            ATELIER_TRY(document_->save_graph(*project_, target_graph));
            const auto refreshed = project_->refresh();
            workspace_.status =
                refreshed ? L"Procedural graph saved" : L"Procedural graph saved; catalog refresh failed";
            return {};
        }
        return this->save_scene();
    }
    if (all) {
        auto edit = ui_.commit_edit();
        if (!edit || !effects(*edit))
            return failed("Finish the current field before saving.");
        ATELIER_TRY(finish_color_edit(true));
        if (project_) {
            for (const auto& key : document_->dirty_materials()) {
                ATELIER_TRY(document_->save_material(*project_, key));
            }
            for (const auto& key : document_->dirty_graphs()) {
                ATELIER_TRY(document_->save_graph(*project_, key));
            }
        }
        for (const auto& item : workspace_.source_editor.files())
            if (item->dirty())
                ATELIER_TRY(save_source(*item, context.interactive));
        return project_ && workspace_.scene_available ? this->save_scene() : Result<void>{};
    }
    return {};
}

Result<void> Application::close_source(bool interactive) {
    close_menu(false);
    auto* file = workspace_.source_editor.active();
    if (!file)
        return {};
    const auto confirmed = confirm_source(*file, interactive);
    if (!confirmed)
        return std::unexpected(confirmed.error());
    if (*confirmed) {
        effects(ui_.focus(0));
        workspace_.source_editor.close(file->key);
        pending_source_focus_ = true;
    } else
        cancel_operator();
    return {};
}

Result<void> Application::reload_source(bool interactive) {
    close_menu(false);
    auto* file = workspace_.source_editor.active();
    if (!file)
        return {};
    if (file->dirty()) {
        if (!interactive)
            return failed("Save the source draft before reloading it.");
        if (MessageBoxW(window(), L"Discard this source draft and reload the file from disk?", L"Reload source",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) {
            cancel_operator();
            return {};
        }
    }
    effects(ui_.focus(0));
    ATELIER_TRY(workspace_.source_editor.reload(*file));
    pending_source_focus_ = true;
    return {};
}
}

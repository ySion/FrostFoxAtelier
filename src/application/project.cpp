#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "application/diagnostics.hpp"
#include "editor/error.hpp"
#include "foundation/result.hpp"
#include "logging/native.hpp"
#include "assets/file.hpp"
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
    const bool save = kind == Pick::scene_save || kind == Pick::source_save || material;
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
                                                             : L"Atelier scene",
                                     source                  ? L"*.source;*.ss;*.txt;*.json"
                                     : kind == Pick::mesh    ? L"*.obj;*.gltf"
                                     : kind == Pick::texture ? L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff"
                                     : material              ? L"*.material"
                                                             : L"*.scene"};
        dialog->SetFileTypes(1, &type);
        dialog->SetDefaultExtension(source                  ? L"source"
                                    : kind == Pick::mesh    ? L"obj"
                                    : kind == Pick::texture ? L"png"
                                    : material              ? L"material"
                                                            : L"scene");
        if (!save)
            options |= FOS_FILEMUSTEXIST;
        if (kind == Pick::source_save)
            dialog->SetFileName(filename);
        if (kind == Pick::material_save)
            dialog->SetFileName(L"Material.material");
        if (kind == Pick::material_source_save)
            dialog->SetFileName(L"Material.ms.material");
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
        if (document_->dirty() || !document_->dirty_materials().empty() ||
            (sources &&
             std::ranges::any_of(workspace_.source_editor.files(), [](const auto& file) { return file->dirty(); })))
            return failed("Save the current scene, material assets and source drafts before replacing the document.");
        return true;
    }
    if (const auto edit = ui_.commit_edit(); edit) {
        if (!effects(*edit))
            return failed("The active field could not be committed.");
    } else
        return failed("Finish the active field before replacing the document.");
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
        const auto response = MessageBoxW(window(), (L"Save changes to material " + assets::wide(key) + L"?").c_str(),
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
        workspace_.status = assets::wide(saved.error().message);
        MessageBoxW(window(), workspace_.status.c_str(), L"Scene was not saved", MB_OK | MB_ICONERROR);
        return std::unexpected(saved.error());
    }
    return true;
}

void Application::publish_document(std::unique_ptr<editor::Session> next, editor::ProjectCode* code) {
    const auto previous_document = document_->presentation().identity();
    restart_play_ = false;
    finish_camera_fly(false);
    camera_fly_release_.reset();
    render_retry_at_ = {};
    render_retry_delay_ = {};
    if (control_)
        control_->document_replaced();
    const auto cancelled = viewport_tool_.cancel(*document_);
    if (!cancelled)
        throw editor::detail::Failure(cancelled.error());
    viewport_tool_.cursor = {};
    accepted_view_.reset();
    cancel_interaction(ui_.cancel_interaction());
    auto cleared = ui_.set_controls({});
    if (!cleared)
        throw std::runtime_error("Cannot retire the previous document UI.");
    effects(*cleared);
    sync_text_input();
    if (popup_)
        popup_->reset_document();
    for (auto& popup : submenus_)
        popup->reset_document();
    workspace_.reset_document();
    project_code_ = nullptr;
    diagnostics();
    const auto closed = document_->close();
    diagnostics();
    if (!closed)
        throw editor::detail::Failure(closed.error());
    document_ = std::move(next);
    project_code_ = code;
    scene_.reset();
    workspace_.render_notice.clear();
    camera_.reset();
    sync_world_view();
    camera_cut_ = true;
    invalidate_surface();
    dropped_diagnostics_ = 0;
    document_root_ = project_ ? project_->root() : std::filesystem::path{};
    const std::array fields{logging::Field{"previous_document", previous_document},
                            logging::Field{"document", document_->presentation().identity()},
                            logging::Field{"project_root", assets::utf8(document_root_.generic_wstring())},
                            logging::Field{"scene", assets::utf8(scene_path_.generic_wstring())}};
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "application", "document.adopted",
                                       logging::native::Context{fields}, "Editor document adopted"));
}

Result<void> Application::open_project(const std::filesystem::path& folder, bool create, ProjectOpenPolicy policy,
                                       const std::function<Result<void>(const editor::Session&)>& admit,
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
    return load_project(folder, create, {}, policy, admit);
}

Result<void> Application::load_project(const std::filesystem::path& folder, bool create,
                                       const std::filesystem::path& scene, ProjectOpenPolicy policy,
                                       const std::function<Result<void>(const editor::Session&)>& admit) {
    ATELIER_TRY_BIND(project, create ? editor::Project::create(folder) : editor::Project::open(folder));
    const auto initial_scene = scene.empty() ? project->startup_scene() : scene;
    const auto path = project->resolve(initial_scene);
    if (!path)
        return std::unexpected(path.error());
    const auto content = assets::read_file(*path);
    if (!content)
        return std::unexpected(content.error());
    // Reproducible workflows admit only a complete, exactly applied candidate
    // before retiring the current document. Interactive opening keeps unresolved
    // saved data and loads placeholders while the Source build is broken.
    const bool reproducible = policy == ProjectOpenPolicy::require_scene;
    auto candidate = editor::load_project_scene(
        *project, *content, reproducible ? editor::LoadPolicy::strict : editor::LoadPolicy::tolerant, !reproducible,
        editor::SessionRole::editor);
    if (reproducible && (!candidate.session || candidate.error))
        return candidate.error ? std::unexpected(*candidate.error)
                               : failed("Project did not produce a complete scene.");
    if (reproducible && admit)
        ATELIER_TRY(admit(*candidate.session));
    play_.close();
    consume_play_output();
    workspace_.source_editor.clear();
    project_ = std::move(*project);
    scene_path_ = initial_scene;
    workspace_.project_open = true;
    pending_scene_ = *content;
    workspace_.scene_available = candidate.session != nullptr;
    if (candidate.session)
        publish_document(std::move(candidate.session), candidate.code);
    else
        publish_document(std::make_unique<editor::Session>(editor::Session::Prepare{}, editor::SessionRole::editor));
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
            auto source_path = project_->resolve(assets::wide(candidate.build->sources->files.front().path));
            if (source_path)
                static_cast<void>(open_source(*source_path));
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
    ATELIER_TRY_BIND(path, project_->resolve(relative));
    ATELIER_TRY_BIND(text, assets::read_file(*path));
    auto next =
        editor::load_project_scene(*project_, *text, editor::LoadPolicy::tolerant, true, editor::SessionRole::editor);
    if (!next.session) {
        publish_build(next.build);
        return std::unexpected(*next.error);
    }
    play_.close();
    consume_play_output();
    scene_path_ = relative;
    pending_scene_ = *text;
    workspace_.scene_available = true;
    publish_document(std::move(next.session), next.code);
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
        return failed("Correct the active field before saving.");
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
    workspace_.status =
        refreshed ? L"Scene saved" : L"Scene saved; asset refresh failed: " + assets::wide(refreshed.error().message);
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
        notice("asset.mesh_import_note", L"Import Mesh: " + assets::wide(note));
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
                const auto path = project_->resolve(assets::wide(author->source_path));
                if (!path || _wcsicmp(path->lexically_normal().c_str(), file.path.lexically_normal().c_str()) != 0)
                    continue;
                // The saved editor buffer is authoritative; another disk write
                // must not change the publication or the subsequent Live payload.
                auto text = assets::utf8(file.document->text());
                source::SourceFile exact{author->source_path, source::content_hash(text), std::move(text),
                                         assets::utf8(file.path.generic_wstring())};
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
        workspace_.status = assets::wide(saved.error().message);
        MessageBoxW(window(), workspace_.status.c_str(), L"Source was not saved", MB_OK | MB_ICONERROR);
        return std::unexpected(saved.error());
    }
    return true;
}

Result<void> Application::save_documents(const editor::Context& context, bool all) {
    const auto& material = context.asset;
    close_menu(false);
    auto* file = workspace_.source_editor.active();
    if (!all && !material.empty()) {
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
    if (!all)
        return context.region == editor::RegionKind::code && file ? save_source(*file, context.interactive)
                                                                  : this->save_scene();
    if (all) {
        auto edit = ui_.commit_edit();
        if (!edit || !effects(*edit))
            return failed("Finish the current field before saving.");
        ATELIER_TRY(finish_color_edit(true));
        if (project_)
            for (const auto& key : document_->dirty_materials()) {
                ATELIER_TRY(document_->save_material(*project_, key));
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

#include "application/application.hpp"
#include "application/editor_control.hpp"
#include "assets/file.hpp"
#include <cstdio>
#include <shobjidl.h>
#include <wrl/client.h>

namespace atelier::application {
namespace {
auto failed(std::string message) { return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)}); }
constexpr const wchar_t* placeholder_notice=L"Project types are unavailable until the Source build succeeds. Project objects load as placeholders and keep their saved values; fix the build, then Rebuild Types.";
enum class Pick { folder, scene_open, scene_save, mesh, source, source_save, texture, material_save, material_source_save };
Result<std::optional<std::filesystem::path>> pick(HWND owner,Pick kind,const std::filesystem::path& initial={},const wchar_t* filename=L"Move.source") {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE) return failed("Cannot initialize the native file dialog.");
    struct Apartment { bool owns; ~Apartment(){if (owns) CoUninitialize();} } apartment{SUCCEEDED(initialized)};
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    const bool material=kind==Pick::material_save || kind==Pick::material_source_save;
    const bool save=kind==Pick::scene_save || kind==Pick::source_save || material;
    auto hr=CoCreateInstance(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return failed("Cannot create the native file dialog.");
    FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);
    options|=FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR;
    if (kind==Pick::folder) {options|=FOS_PICKFOLDERS;dialog->SetTitle(L"Choose a project folder");}
    else {
        const bool source=kind==Pick::source || kind==Pick::source_save;
        const COMDLG_FILTERSPEC type{source?L"Source and text files":kind==Pick::mesh?L"OBJ or glTF models":kind==Pick::texture?L"Textures":material?L"Material":L"Atelier scene",
            source?L"*.source;*.ss;*.txt;*.json":kind==Pick::mesh?L"*.obj;*.gltf":kind==Pick::texture?L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff":material?L"*.material":L"*.scene"};
        dialog->SetFileTypes(1,&type);dialog->SetDefaultExtension(source?L"source":kind==Pick::mesh?L"obj":kind==Pick::texture?L"png":material?L"material":L"scene");
        if (!save) options|=FOS_FILEMUSTEXIST;
        if(kind==Pick::source_save) dialog->SetFileName(filename);
        if(kind==Pick::material_save) dialog->SetFileName(L"Material.material");
        if(kind==Pick::material_source_save) dialog->SetFileName(L"Material.ms.material");
    }
    dialog->SetOptions(options);
    if (!initial.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(),nullptr,IID_PPV_ARGS(&folder)))) dialog->SetFolder(folder.Get());
    }
    hr=dialog->Show(owner);
    if (hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::optional<std::filesystem::path>{};
    if (FAILED(hr)) return failed("File dialog failed (HRESULT "+std::to_string(static_cast<unsigned long>(hr))+").");
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return failed("Cannot read the selected location.");
    PWSTR path{};if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) return failed("The selected location is not a file-system path.");
    const std::filesystem::path result(path);CoTaskMemFree(path);return std::optional{result};
}
}
bool Application::confirm_discard(bool sources) {
    if (const auto edit=ui_.commit_edit();edit) {if (!effects(*edit)) return false;}
    else return false;
    if(auto committed=finish_color_edit(true);!committed) {status(committed,L"");return false;}
    if (sources) for (const auto& file:workspace_.source_editor.files()) if (!confirm_source(*file)) return false;
    for(const auto& key:document_->dirty_materials()) {
        const auto response=MessageBoxW(window(),(L"Save changes to material "+assets::wide(key)+L"?").c_str(),L"Unsaved material",MB_YESNOCANCEL|MB_ICONQUESTION);
        if(response==IDCANCEL) return false;
        if(response==IDYES) {
            if(!project_) return false;
            const auto saved=document_->save_material(*project_,key);
            if(!saved) {workspace_.status=assets::wide(saved.error().message);return false;}
        }
    }
    if (!document_->dirty()) return true;
    const auto response=MessageBoxW(window(),L"Save changes to the current scene?",L"Unsaved scene",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (response==IDCANCEL) return false;
    if (response==IDNO) return true;
    const auto saved=save_scene();
    if (!saved) {workspace_.status=assets::wide(saved.error().message);MessageBoxW(window(),workspace_.status.c_str(),L"Scene was not saved",MB_OK|MB_ICONERROR);}
    return saved.has_value();
}
void Application::publish_document(std::unique_ptr<editor::Session> next,editor::ProjectCode* code) {
    restart_play_=false;
    finish_camera_fly(false);camera_fly_release_.reset();
    render_retry_at_={};render_retry_delay_={};
    if(control_) control_->document_replaced();
    const auto cancelled=viewport_tool_.cancel(*document_);if(!cancelled) throw std::runtime_error(cancelled.error().message);
    viewport_tool_.cursor={};accepted_view_.reset();
    cancel_interaction(ui_.cancel_interaction());
    auto cleared=ui_.set_controls({});if (!cleared) throw std::runtime_error("Cannot retire the previous document UI.");effects(*cleared);
    sync_text_input();
    if (popup_) popup_->reset_document();
    for (auto& popup:submenus_) popup->reset_document();
    workspace_.reset_document();
    project_code_=nullptr;
    const auto closed=document_->close();if (!closed) throw std::runtime_error(closed.error().message);
    document_=std::move(next);project_code_=code;scene_.reset();workspace_.render_notice.clear();camera_.reset();sync_world_view();camera_cut_=true;invalidate_surface();
}
Result<void> Application::open_project(const std::filesystem::path& folder,bool create,ProjectOpenPolicy policy,const std::function<Result<void>(const editor::Session&)>& admit) {
    // A save decision can change the file we are about to open. Read only after
    // that decision; cancellation also precedes Project::create's disk changes.
    if (!confirm_discard()) return {};
    return load_project(folder,create,{},policy,admit);
}
Result<void> Application::load_project(const std::filesystem::path& folder,bool create,const std::filesystem::path& scene,ProjectOpenPolicy policy,const std::function<Result<void>(const editor::Session&)>& admit) {
    auto project=create?editor::Project::create(folder):editor::Project::open(folder);
    if (!project) return std::unexpected(project.error());
    const auto initial_scene=scene.empty()?project->startup_scene():scene;
    const auto path=project->resolve(initial_scene);if (!path) return std::unexpected(path.error());
    const auto content=assets::read_file(*path);if (!content) return std::unexpected(content.error());
    // Reproducible workflows admit only a complete, exactly applied candidate
    // before retiring the current document. Interactive opening keeps unresolved
    // saved data and loads placeholders while the Source build is broken.
    const bool reproducible=policy==ProjectOpenPolicy::require_scene;
    auto candidate=editor::load_project_scene(*project,*content,reproducible?editor::LoadPolicy::strict:editor::LoadPolicy::tolerant,!reproducible,
        editor::SessionRole::editor);
    if(reproducible && (!candidate.session || candidate.error))
        return candidate.error?std::unexpected(*candidate.error):failed("Project did not produce a complete scene.");
    if(reproducible && admit) if(auto admitted=admit(*candidate.session);!admitted) return admitted;
    play_.close();
    workspace_.source_editor.clear();
    project_=std::move(*project);scene_path_=initial_scene;workspace_.project_open=true;
    pending_scene_=*content;workspace_.scene_available=candidate.session!=nullptr;
    if(candidate.session) publish_document(std::move(candidate.session),candidate.code);
    else publish_document(std::make_unique<editor::Session>(editor::Session::Prepare{},editor::SessionRole::editor));
    if(candidate.error) {
        auto report=std::make_shared<editor::ProjectBuild>();if(candidate.build) *report=*candidate.build;
        report->error=candidate.error;report->state=editor::BuildState::failed;candidate.build=std::move(report);
    }
    publish_build(candidate.build);workspace_.project_ui_configured=!project_->ui_source().empty();
    if(candidate.session && candidate.error) workspace_.log(placeholder_notice);
    for(auto& line:editor::describe_issues(*document_,document_->load_issues(),L"Scene "+initial_scene.generic_wstring())) workspace_.log(std::move(line));
    if(candidate.error) {
        workspace_.dock.select(editor::source_control::tab);
        if(candidate.build && candidate.build->sources && !candidate.build->sources->files.empty()) {
            auto source_path=project_->resolve(assets::wide(candidate.build->sources->files.front().path));
            if(source_path) static_cast<void>(open_source(*source_path));
        }
    }
    std::printf("Project: %s\n",assets::utf8(project_->root().wstring()).c_str());
    return {};
}
Result<void> Application::open_scene(const std::filesystem::path& relative) {
    if (!project_) return failed("Open a project folder first.");
    if (!confirm_discard(false)) return {};
    auto path=project_->resolve(relative);if (!path) return std::unexpected(path.error());
    auto text=assets::read_file(*path);if (!text) return std::unexpected(text.error());
    auto next=editor::load_project_scene(*project_,*text,editor::LoadPolicy::tolerant,true,editor::SessionRole::editor);
    if(!next.session) {publish_build(next.build);return std::unexpected(*next.error);}
    play_.close();scene_path_=relative;pending_scene_=*text;workspace_.scene_available=true;
    publish_document(std::move(next.session),next.code);publish_build(next.build);
    if(next.error) workspace_.log(placeholder_notice);
    for(auto& line:editor::describe_issues(*document_,document_->load_issues(),L"Scene "+relative.generic_wstring())) workspace_.log(std::move(line));
    return {};
}
Result<void> Application::save_scene(const std::filesystem::path& relative) {
    if (!project_) return failed("Create or open a project folder before saving a scene.");
    if (!workspace_.scene_available) return failed("Repair the project build and reload its scene before saving. The original scene has been preserved.");
    const auto edit=ui_.commit_edit();if (!edit) return failed("Correct the active field before saving.");
    if (!effects(*edit)) return failed("The active field could not be committed; the scene was not saved.");
    if(auto committed=finish_color_edit(true);!committed) return committed;
    const auto destination=relative.empty()?scene_path_:relative;
    if (!editor::Project::is_scene_path(destination)) return failed("Save scenes inside the project's Content folder with a .scene extension.");
    const auto path=project_->resolve(destination);if (!path) return std::unexpected(path.error());
    const auto text=document_->serialize();if (!text) return std::unexpected(text.error());
    auto saved=editor::write_file(*path,*text);if (!saved) return saved;
    scene_path_=destination;document_->mark_saved();
    const auto refreshed=project_->refresh();
    workspace_.status=refreshed?L"Scene saved":L"Scene saved; asset refresh failed: "+assets::wide(refreshed.error().message);
    auto pending=document_->issues();
    const auto kept=pending.size();
    std::erase_if(pending,[](const auto& item){return item.kind!=world::DiagnosticKind::reference_dropped;});
    for(auto& line:editor::describe_issues(*document_,pending,L"Saved "+destination.generic_wstring())) workspace_.log(std::move(line));
    if(kept) workspace_.status+=L" ("+std::to_wstring(kept)+L" unresolved saved item(s) kept)";
    return {};
}
Result<void> Application::project_command(editor::Command command,std::string_view argument) {
    using enum editor::Command;
    close_menu(false);
    if (command==save_scene) return this->save_scene();
    if(command==reimport_texture || command==reimport_mesh) {
        const bool texture=command==reimport_texture;
        const auto selected=workspace_.assets.selected();
        if(!project_ || !selected || workspace_.assets.engine() || selected->kind!=(texture?assets::AssetKind::texture:assets::AssetKind::mesh))
            return failed("Select a matching project resource to reimport.");
        const auto edit=ui_.commit_edit();if(!edit || !effects(*edit)) return failed("Finish the active edit before reimporting.");
        if(auto finished=finish_color_edit(true);!finished) return finished;
        std::optional<std::filesystem::path> path;
        if(!argument.empty()) path=assets::wide(argument);
        else {auto chosen=pick(window(),texture?Pick::texture:Pick::mesh,project_->root()/selected->path.parent_path());if(!chosen) return std::unexpected(chosen.error());path=*chosen;}
        if(!path) return {};
        auto imported=texture?document_->reimport_texture(*project_,selected->key,*path):document_->reimport_mesh(*project_,selected->key,*path);
        if(!imported) return imported;
        workspace_.status=L"Resource reimported; current content published";return {};
    }
    if(command==create_material || command==create_material_source || command==import_texture) {
        if(!project_) return failed("Open a project before creating resources.");
        const bool material=command!=import_texture;
        std::filesystem::path path;
        if(!argument.empty()) path=assets::wide(argument);
        else {
            auto picked=pick(window(),command==create_material_source?Pick::material_source_save:material?Pick::material_save:Pick::texture,project_->root()/"Content");
            if(!picked) return std::unexpected(picked.error());if(!*picked) return {};path=**picked;
        }
        assets::MaterialAsset asset;
        if(command==create_material) {
            const auto selected=workspace_.assets.selected();
            if(selected && selected->kind==assets::AssetKind::material)
                std::get<assets::MaterialInstanceAsset>(asset.author).parent_key=selected->key;
        }
        const auto relative=path.is_absolute()?path.lexically_relative(project_->root()):path;
        Result<std::string> created=command==create_material_source?project_->create_material_source(relative):
            material?project_->create_material(relative,asset):project_->import_texture(path);
        if(!created) return std::unexpected(created.error());
        if(const auto* at=project_->entry(*created)) workspace_.assets.navigate(at->path.parent_path());
        if(material) {
            auto root=document_->material_resource(project_->assets(),*created);if(!root) return std::unexpected(root.error());
            workspace_.material_inspection=*created;workspace_.inspecting_asset=true;
        }
        workspace_.status=material?L"Material created":L"Texture imported";return {};
    }
    if (command==refresh_assets) return project_?project_->refresh():Result<void>(failed("No project is open."));
    const auto kind=command==new_project || command==open_project?Pick::folder:command==open_scene?Pick::scene_open:command==save_scene_as?Pick::scene_save:Pick::mesh;
    std::optional<std::filesystem::path> path;
    if (!argument.empty()) path=assets::wide(argument);
    else {
        const auto chosen=pick(window(),kind,project_?project_->root()/"Content":std::filesystem::path{});
        if (!chosen) return std::unexpected(chosen.error());path=*chosen;
    }
    if (!path) return {};
    if (command==new_project || command==open_project) return this->open_project(*path,command==new_project);
    if (!project_) return failed("Open a project folder first.");
    if (command==import_mesh) {
        auto imported=project_->import_mesh(*path);if (!imported) return std::unexpected(imported.error());
        for(const auto& note:project_->assets().mesh_notes(*imported)) workspace_.log(L"Import Mesh: "+assets::wide(note));
        workspace_.assets.reset(true);workspace_.assets.navigate(L"Content/Meshes");return {};
    }
    auto relative=path->is_absolute()?path->lexically_relative(project_->root()):*path;
    return command==open_scene?this->open_scene(relative):this->save_scene(relative);
}
Result<void> Application::open_source(const std::filesystem::path& path) {
    close_menu(false);
    const auto edit=ui_.commit_edit();if (!edit || !effects(*edit)) return failed("Finish the current edit before opening source.");
    auto opened=workspace_.source_editor.open(path);if (!opened) return opened;
    workspace_.dock.select(editor::source_control::tab);pending_source_focus_=true;return {};
}
Result<void> Application::save_source(editor::SourceEditor::File& file) {
    const auto edit=ui_.commit_edit();if (!edit || !effects(*edit)) return failed("Finish the current edit before saving source.");
    auto saved=workspace_.source_editor.save(file);
    if (!saved && file.conflict) {
        const auto message=L"The file changed outside the editor:\n"+file.path.wstring()+L"\n\nOverwrite the external changes with this document?";
        if (MessageBoxW(window(),message.c_str(),L"Source file conflict",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)==IDYES)
            saved=workspace_.source_editor.save(file,true);
    }
    if (saved) {
        workspace_.status=L"Source saved";source_build_pending_=true;
        if(project_) {
            auto materials=document_->material_assets();if(!materials) return std::unexpected(materials.error());
            for(const auto& asset:*materials) {
                const auto* author=std::get_if<assets::MaterialSourceAsset>(&asset.author);
                if(!author || author->source_path.starts_with("builtin/")) continue;
                const auto path=project_->resolve(assets::wide(author->source_path));
                if(!path || _wcsicmp(path->lexically_normal().c_str(),file.path.lexically_normal().c_str())!=0) continue;
                // The saved editor buffer is authoritative; another disk write
                // must not change the publication or the subsequent Live payload.
                auto text=assets::utf8(file.document->text());
                source::SourceFile exact{author->source_path,source::content_hash(text),std::move(text)};
                std::vector<editor::MissingResource> missing;
                auto published=document_->publish_material(project_->assets(),asset,std::move(exact),&missing);
                if(!published) return std::unexpected(published.error());
                for(const auto& issue:missing) workspace_.log(L"Material Source: "+assets::wide(issue.reason));
                if(!missing.empty()) workspace_.status=L"Source saved; material diagnostics need attention";
            }
        }
    }
    return saved;
}
bool Application::confirm_source(editor::SourceEditor::File& file) {
    if (!file.dirty()) return true;
    const auto message=L"Save changes to "+file.path.filename().wstring()+L"?";
    const auto response=MessageBoxW(window(),message.c_str(),L"Unsaved source document",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (response==IDCANCEL) return false;
    if (response==IDNO) return true;
    const auto saved=save_source(file);
    if (!saved) {workspace_.status=assets::wide(saved.error().message);MessageBoxW(window(),workspace_.status.c_str(),L"Source was not saved",MB_OK|MB_ICONERROR);}
    return saved.has_value();
}
Result<void> Application::source_command(editor::Command command,std::string_view argument) {
    using enum editor::Command;
    const auto material=command==save_document?operator_context(true).asset:std::string{};
    close_menu(false);
    auto* file=workspace_.source_editor.active();
    if(command==new_source || command==new_project_ui) {
        if(!project_) return failed("Open a project first.");
        auto path=pick(window(),Pick::source_save,project_->root()/"Source",command==new_project_ui?L"Main.ui.source":L"Move.source");if(!path) return std::unexpected(path.error());
        if(!*path) return {};
        auto relative=(**path).lexically_relative(project_->root());
        if(command==new_project_ui && !relative.filename().wstring().ends_with(L".ui.source")) relative.replace_extension(L".ui.source");
        return create_source(relative,command==new_project_ui);
    }
    if(command==edit_project_ui) {
        if(!project_ || project_->ui_source().empty()) return failed("Create a project UI first.");
        auto path=project_->resolve(project_->ui_source());return path?this->open_source(*path):Result<void>{std::unexpected(path.error())};
    }
    if (command==edit_ui_source) return this->open_source(workspace_.ui_source());
    if (command==open_source) {
        if (!argument.empty()) return this->open_source(assets::wide(argument));
        const auto path=pick(window(),Pick::source,project_?project_->root()/"Source":workspace_.ui_source().parent_path());
        if (!path) return std::unexpected(path.error());
        return *path?this->open_source(**path):Result<void>{};
    }
    if(command==save_document && !material.empty()) {
        auto edit=ui_.commit_edit();if(!edit || !effects(*edit)) return failed("Finish the material field before saving.");
        if(auto committed=finish_color_edit(true);!committed) return committed;
        if(!project_) return failed("No project is open.");
        auto saved=document_->save_material(*project_,material);if(!saved) return saved;
        const auto refreshed=project_->refresh();workspace_.status=refreshed?L"Material saved":L"Material saved; catalog refresh failed";return {};
    }
    if (command==save_document) return workspace_.source_history() && file?save_source(*file):this->save_scene();
    if (command==save_all) {
        auto edit=ui_.commit_edit();if(!edit || !effects(*edit)) return failed("Finish the current field before saving.");
        if(auto committed=finish_color_edit(true);!committed) return committed;
        if(project_) for(const auto& key:document_->dirty_materials()) {auto saved=document_->save_material(*project_,key);if(!saved) return saved;}
        for (const auto& item:workspace_.source_editor.files()) if (item->dirty()) if (auto result=save_source(*item);!result) return result;
        return project_ && workspace_.scene_available?this->save_scene():Result<void>{};
    }
    if (!file) return {};
    if (command==close_source) {
        if (confirm_source(*file)) {
            effects(ui_.focus(0));workspace_.source_editor.close(file->key);pending_source_focus_=true;
        }
    } else if (command==reload_source) {
        if (file->dirty() && MessageBoxW(window(),L"Discard this source draft and reload the file from disk?",L"Reload source",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES) return {};
        effects(ui_.focus(0));auto result=workspace_.source_editor.reload(*file);if (!result) return result;pending_source_focus_=true;
    }
    return {};
}

}

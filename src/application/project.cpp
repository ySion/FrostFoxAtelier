#include "application/application.hpp"
#include "editor/examples.hpp"
#include <cstdio>
#include <shobjidl.h>
#include <wrl/client.h>

namespace atelier::application {
namespace {
auto failed(std::string message) { return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)}); }
enum class Pick { folder, scene_open, scene_save, mesh, source, source_save };
Result<std::optional<std::filesystem::path>> pick(HWND owner,Pick kind,const std::filesystem::path& initial={},const wchar_t* filename=L"Move.source") {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE) return failed("Cannot initialize the native file dialog.");
    struct Apartment { bool owns; ~Apartment(){if (owns) CoUninitialize();} } apartment{SUCCEEDED(initialized)};
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    const bool save=kind==Pick::scene_save || kind==Pick::source_save;
    auto hr=CoCreateInstance(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return failed("Cannot create the native file dialog.");
    FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);
    options|=FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR;
    if (kind==Pick::folder) {options|=FOS_PICKFOLDERS;dialog->SetTitle(L"Choose a project folder");}
    else {
        const bool source=kind==Pick::source || kind==Pick::source_save;
        const COMDLG_FILTERSPEC type{source?L"Source and text files":kind==Pick::mesh?L"OBJ models and materials":L"Atelier scene",source?L"*.source;*.ss;*.txt;*.json":kind==Pick::mesh?L"*.obj":L"*.scene"};
        dialog->SetFileTypes(1,&type);dialog->SetDefaultExtension(source?L"source":kind==Pick::mesh?L"obj":L"scene");
        if (!save) options|=FOS_FILEMUSTEXIST;
        if(kind==Pick::source_save) dialog->SetFileName(filename);
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
    if (sources) for (const auto& file:workspace_.source_editor.files()) if (!confirm_source(*file)) return false;
    if (!document_->dirty()) return true;
    const auto response=MessageBoxW(window(),L"Save changes to the current scene?",L"Unsaved scene",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (response==IDCANCEL) return false;
    if (response==IDNO) return true;
    const auto saved=save_scene();
    if (!saved) {workspace_.status=editor::wide(saved.error().message);MessageBoxW(window(),workspace_.status.c_str(),L"Scene was not saved",MB_OK|MB_ICONERROR);}
    return saved.has_value();
}
void Application::publish_document(std::unique_ptr<editor::Session> next,editor::ProjectCode* code) {
    const auto cancelled=viewport_tool_.cancel(*document_);if(!cancelled) throw std::runtime_error(cancelled.error().message);
    viewport_tool_.cursor={};accepted_view_.reset();
    close_menu(false);effects(ui_.cancel_interaction());
    auto cleared=ui_.set_controls({});if (!cleared) throw std::runtime_error("Cannot retire the previous document UI.");effects(*cleared);
    sync_text_input();
    if (popup_) popup_->reset_document();
    for (auto& popup:submenus_) popup->reset_document();
    workspace_.reset_document();
    project_code_=nullptr;
    const auto closed=document_->close();if (!closed) throw std::runtime_error(closed.error().message);
    document_=std::move(next);project_code_=code;scene_.reset();workspace_.render_notice.clear();camera_.reset();camera_cut_=true;invalidate_surface();
}
Result<void> Application::open_project(const std::filesystem::path& folder,bool create) {
    // A save decision can change the file we are about to open. Read only after
    // that decision; cancellation also precedes Project::create's disk changes.
    if (!confirm_discard()) return {};
    return load_project(folder,create);
}
Result<void> Application::open_example(editor::Command command) {
    const auto* example=editor::example_definition(command);
    if(!example) return failed("Unknown official example.");
    if(!confirm_discard()) return {};
    auto folder=editor::prepare_examples(examples_workspace_);
    if(!folder) return std::unexpected(folder.error());
    auto loaded=load_project(*folder,false,example->scene);if(!loaded) return loaded;
    workspace_.assets.navigate(std::filesystem::path(example->scene).parent_path().parent_path());
    if(!workspace_.scene_available) return {}; // Keep normal project build diagnostics visible.
    const auto items=document_->world().render_items();if(!items) return std::unexpected(items.error());
    workspace_.view.lit={};
    return configure_view(example->eye,example->target,renderer::Shading::scene_lit,!items->empty());
}
Result<void> Application::load_project(const std::filesystem::path& folder,bool create,const std::filesystem::path& scene) {
    auto project=create?editor::Project::create(folder):editor::Project::open(folder);
    if (!project) return std::unexpected(project.error());
    const auto initial_scene=scene.empty()?project->startup_scene():scene;
    const auto path=project->resolve(initial_scene);if (!path) return std::unexpected(path.error());
    const auto content=editor::read_file(*path);if (!content) return std::unexpected(content.error());
    auto candidate=editor::load_project_scene(*project,*content);
    play_.close();
    workspace_.source_editor.clear();
    project_=std::move(*project);scene_path_=initial_scene;workspace_.project_open=true;
    pending_scene_=*content;workspace_.scene_available=candidate.session!=nullptr;
    if(candidate.session) publish_document(std::move(candidate.session),candidate.code);
    else publish_document(std::make_unique<editor::Session>());
    if(candidate.error) {
        auto report=std::make_shared<editor::ProjectBuild>();if(candidate.build) *report=*candidate.build;
        report->error=candidate.error;report->state=editor::BuildState::failed;candidate.build=std::move(report);
    }
    publish_build(candidate.build);workspace_.project_ui_configured=!project_->ui_source().empty();
    if(candidate.error) {
        workspace_.dock.select(editor::source_control::tab);
        if(candidate.build && candidate.build->sources && !candidate.build->sources->files.empty()) {
            auto source_path=project_->resolve(editor::wide(candidate.build->sources->files.front().path));
            if(source_path) static_cast<void>(open_source(*source_path));
        }
    }
    std::printf("Project: %s\n",editor::utf8(project_->root().wstring()).c_str());
    return {};
}
Result<void> Application::open_scene(const std::filesystem::path& relative) {
    if (!project_) return failed("Open a project folder first.");
    if (!confirm_discard(false)) return {};
    auto path=project_->resolve(relative);if (!path) return std::unexpected(path.error());
    auto text=editor::read_file(*path);if (!text) return std::unexpected(text.error());
    auto next=editor::load_project_scene(*project_,*text);
    if(!next.session) {publish_build(next.build);return std::unexpected(*next.error);}
    play_.close();scene_path_=relative;pending_scene_=*text;workspace_.scene_available=true;
    publish_document(std::move(next.session),next.code);publish_build(next.build);return {};
}
Result<void> Application::save_scene(const std::filesystem::path& relative) {
    if (!project_) return failed("Create or open a project folder before saving a scene.");
    if (!workspace_.scene_available) return failed("Repair the project build and reload its scene before saving. The original scene has been preserved.");
    const auto edit=ui_.commit_edit();if (!edit) return failed("Correct the active field before saving.");
    if (!effects(*edit)) return failed("The active field could not be committed; the scene was not saved.");
    const auto destination=relative.empty()?scene_path_:relative;
    if (!editor::Project::is_scene_path(destination)) return failed("Save scenes inside the project's Content or Examples folder with a .scene extension.");
    const auto path=project_->resolve(destination);if (!path) return std::unexpected(path.error());
    const auto text=document_->serialize();if (!text) return std::unexpected(text.error());
    auto saved=editor::write_file(*path,*text);if (!saved) return saved;
    scene_path_=destination;document_->mark_saved();
    const auto refreshed=project_->refresh();
    workspace_.status=refreshed?L"Scene saved":L"Scene saved; asset refresh failed: "+editor::wide(refreshed.error().message);
    return {};
}
Result<void> Application::project_command(editor::Command command,std::string_view argument) {
    using enum editor::Command;
    close_menu(false);
    if (command==save_scene) return this->save_scene();
    if (command==refresh_assets) return project_?project_->refresh():Result<void>(failed("No project is open."));
    const auto kind=command==new_project || command==open_project?Pick::folder:command==open_scene?Pick::scene_open:command==save_scene_as?Pick::scene_save:Pick::mesh;
    std::optional<std::filesystem::path> path;
    if (!argument.empty()) path=editor::wide(argument);
    else {
        const auto chosen=pick(window(),kind,project_?project_->root()/"Content":std::filesystem::path{});
        if (!chosen) return std::unexpected(chosen.error());path=*chosen;
    }
    if (!path) return {};
    if (command==new_project || command==open_project) return this->open_project(*path,command==new_project);
    if (!project_) return failed("Open a project folder first.");
    if (command==import_mesh) {
        auto imported=project_->import_mesh(*path);if (!imported) return std::unexpected(imported.error());
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
    if (saved) {workspace_.status=L"Source saved";source_build_pending_=true;}
    return saved;
}
bool Application::confirm_source(editor::SourceEditor::File& file) {
    if (!file.dirty()) return true;
    const auto message=L"Save changes to "+file.path.filename().wstring()+L"?";
    const auto response=MessageBoxW(window(),message.c_str(),L"Unsaved source document",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (response==IDCANCEL) return false;
    if (response==IDNO) return true;
    const auto saved=save_source(file);
    if (!saved) {workspace_.status=editor::wide(saved.error().message);MessageBoxW(window(),workspace_.status.c_str(),L"Source was not saved",MB_OK|MB_ICONERROR);}
    return saved.has_value();
}
Result<void> Application::source_command(editor::Command command,std::string_view argument) {
    using enum editor::Command;
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
        if (!argument.empty()) return this->open_source(editor::wide(argument));
        const auto path=pick(window(),Pick::source,project_?project_->root()/"Source":workspace_.ui_source().parent_path());
        if (!path) return std::unexpected(path.error());
        return *path?this->open_source(**path):Result<void>{};
    }
    if (command==save_document) return workspace_.source_history() && file?save_source(*file):this->save_scene();
    if (command==save_all) {
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

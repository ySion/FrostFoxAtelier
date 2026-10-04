#include "application/application.hpp"
#include <cstdio>
#include <shobjidl.h>
#include <wrl/client.h>

namespace atelier::application {
namespace {
auto failed(std::string message) { return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)}); }
enum class Pick { folder, scene_open, scene_save, mesh };
Result<std::optional<std::filesystem::path>> pick(HWND owner,Pick kind,const std::filesystem::path& initial={}) {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE) return failed("Cannot initialize the native file dialog.");
    struct Apartment { bool owns; ~Apartment(){if (owns) CoUninitialize();} } apartment{SUCCEEDED(initialized)};
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    auto hr=CoCreateInstance(kind==Pick::scene_save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return failed("Cannot create the native file dialog.");
    FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);
    options|=FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR;
    if (kind==Pick::folder) {options|=FOS_PICKFOLDERS;dialog->SetTitle(L"Choose a project folder");}
    else {
        const COMDLG_FILTERSPEC type{kind==Pick::mesh?L"Triangulated OBJ geometry":L"Atelier scene",kind==Pick::mesh?L"*.obj":L"*.scene"};
        dialog->SetFileTypes(1,&type);dialog->SetDefaultExtension(kind==Pick::mesh?L"obj":L"scene");
        if (kind!=Pick::scene_save) options|=FOS_FILEMUSTEXIST;
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
bool Application::confirm_discard() {
    if (const auto edit=ui_.commit_edit();edit) {if (!effects(*edit)) return false;}
    else return false;
    if (!document_->dirty()) return true;
    const auto response=MessageBoxW(window(),L"Save changes to the current scene?",L"Unsaved scene",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (response==IDCANCEL) return false;
    if (response==IDNO) return true;
    const auto saved=save_scene();
    if (!saved) {workspace_.status=editor::wide(saved.error().message);MessageBoxW(window(),workspace_.status.c_str(),L"Scene was not saved",MB_OK|MB_ICONERROR);}
    return saved.has_value();
}
void Application::publish_document(std::unique_ptr<editor::Session> next) {
    close_menu(false);effects(ui_.cancel_interaction());
    auto cleared=ui_.set_controls({});if (!cleared) throw std::runtime_error("Cannot retire the previous document UI.");effects(*cleared);
    sync_text_input();
    const auto closed=document_->close();if (!closed) throw std::runtime_error(closed.error().message);
    document_=std::move(next);workspace_.reset_document();camera_.reset();invalidate_surface();
}
Result<void> Application::open_project(const std::filesystem::path& folder,bool create) {
    // A save decision can change the file we are about to open. Read only after
    // that decision; cancellation also precedes Project::create's disk changes.
    if (!confirm_discard()) return {};
    auto project=create?editor::Project::create(folder):editor::Project::open(folder);
    if (!project) return std::unexpected(project.error());
    const auto path=project->resolve(project->startup_scene());if (!path) return std::unexpected(path.error());
    const auto content=editor::read_file(*path);if (!content) return std::unexpected(content.error());
    auto candidate=editor::Session::deserialize(*content,*project);if (!candidate) return std::unexpected(candidate.error());
    project_=std::move(*project);scene_path_=project_->startup_scene();workspace_.project_open=true;
    publish_document(std::move(*candidate));
    std::printf("Project: %s\n",editor::utf8(project_->root().wstring()).c_str());
    return {};
}
Result<void> Application::open_scene(const std::filesystem::path& relative) {
    if (!project_) return failed("Open a project folder first.");
    if (!confirm_discard()) return {};
    auto path=project_->resolve(relative);if (!path) return std::unexpected(path.error());
    auto text=editor::read_file(*path);if (!text) return std::unexpected(text.error());
    auto next=editor::Session::deserialize(*text,*project_);if (!next) return std::unexpected(next.error());
    scene_path_=relative;publish_document(std::move(*next));return {};
}
Result<void> Application::save_scene(const std::filesystem::path& relative) {
    if (!project_) return failed("Create or open a project folder before saving a scene.");
    const auto edit=ui_.commit_edit();if (!edit) return failed("Correct the active field before saving.");
    if (!effects(*edit)) return failed("The active field could not be committed; the scene was not saved.");
    const auto destination=relative.empty()?scene_path_:relative;
    if (destination.empty() || *destination.begin()!=L"Content" || destination.extension()!=L".scene") return failed("Save scenes inside the project's Content folder with a .scene extension.");
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
}

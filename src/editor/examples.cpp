#include "editor/examples.hpp"
#include <shlobj.h>

namespace atelier::editor {
Result<std::filesystem::path> prepare_examples(const std::filesystem::path& workspace_root) {
    namespace fs=std::filesystem;
    const auto fail=[](std::string message) {return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)});};
    fs::path staging;
    try {
        auto root=workspace_root;
        if(root.empty()) {
            PWSTR local{};
            const auto result=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&local);
            if(FAILED(result)) return fail("Cannot locate the examples workspace.");
            root=fs::path(local)/"FrostFoxAtelier/OfficialExamples";CoTaskMemFree(local);
        }
        root=fs::absolute(root);
        const auto destination=root;
        if(fs::exists(destination)) {
            auto project=Project::open(destination);
            if(!project) return std::unexpected(project.error());
            // Publish newly shipped example folders only; existing user scenes
            // and edits are never merged or replaced by template updates.
            for(const auto& example:official_examples) {
                const auto folder=fs::path(example.scene).parent_path().parent_path();
                if(fs::exists(destination/folder)) continue;
                const auto source=asset_directory()/"examples"/folder;
                if(!fs::is_directory(source)) return fail("Missing bundled example: "+utf8(folder.wstring()));
                GUID id{};if(FAILED(CoCreateGuid(&id))) return fail("Cannot stage a new example.");
                wchar_t suffix[40]{};StringFromGUID2(id,suffix,40);
                const auto temporary=destination/(std::wstring(L".example-")+suffix);
                if(!fs::create_directory(temporary)) return fail("Cannot stage a new example.");
                staging=temporary;
                fs::copy(source,staging,fs::copy_options::recursive|fs::copy_options::skip_symlinks);
                fs::create_directories((destination/folder).parent_path());
                std::error_code error;fs::rename(staging,destination/folder,error);
                if(error) {
                    if(!fs::is_directory(destination/folder)) throw std::runtime_error(error.message());
                    fs::remove_all(staging);
                }
                staging.clear();
            }
            return destination;
        }
        const auto source=asset_directory()/"examples";
        if(!fs::is_regular_file(source/"Config/Project.json"))
            return fail("The bundled example is missing: "+utf8(source.wstring())+". Rebuild or restore the editor examples.");
        fs::create_directories(root.parent_path());
        GUID id{};if(FAILED(CoCreateGuid(&id))) return fail("Cannot allocate an examples workspace.");
        wchar_t suffix[40]{};StringFromGUID2(id,suffix,40);
        const auto temporary=root.parent_path()/(std::wstring(L".initializing-")+suffix);
        if(!fs::create_directory(temporary)) return fail("Cannot create the example staging directory.");
        staging=temporary; // Only this exclusively created directory may be cleaned up.
        for(const auto* directory:{"Config","Content","Source","Examples"}) {
            if(fs::exists(source/directory)) fs::copy(source/directory,staging/directory,
                fs::copy_options::recursive|fs::copy_options::skip_symlinks);
            else fs::create_directories(staging/directory);
        }
        auto project=Project::open(staging);
        if(!project) throw std::runtime_error(project.error().message);
        // Publish the complete project atomically; never merge into a user's copy.
        std::error_code error;fs::rename(staging,destination,error);
        if(error) {
            auto concurrent=Project::open(destination);
            if(!concurrent) throw std::runtime_error("Cannot publish the example workspace: "+error.message());
            fs::remove_all(staging);
        }
        staging.clear();return destination;
    } catch(const std::exception& error) {
        if(!staging.empty()) {std::error_code ignored;fs::remove_all(staging,ignored);}
        return fail(error.what());
    }
}
}

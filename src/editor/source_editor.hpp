#pragma once
#include "editor/project.hpp"
#include "ui_language/live_document.hpp"
#include <chrono>

namespace atelier::editor {
struct ProjectBuild;
namespace source_control {
inline constexpr ui::ControlKey tab=605,content=660,buffer=661,files=662,problem=663;
inline constexpr ui::ControlKey first_document=1ULL<<45;
}
// Files belong to the authoring workspace, independent of a scene Runtime.
// The only persistent artifact is UTF-8 source on disk; UI scopes retain buffers.
class SourceEditor {
public:
    struct File {
        ui::ControlKey key{};
        std::filesystem::path path;
        std::shared_ptr<ui::CodeDocument> document;
        bool dirty() const;
        bool conflict{};
        std::string storage_error;
    private:
        friend class SourceEditor;
        std::string disk;
        std::wstring saved_text;
        bool bom{},crlf{};
        std::optional<std::filesystem::file_time_type> stamp;
        std::uintmax_t disk_size{};
        mutable std::uint64_t checked_revision{};
        mutable bool modified{};
    };
    Result<void> open(const std::filesystem::path&);
    Result<void> save(File&,bool overwrite=false);
    Result<void> reload(File&);
    void close(ui::ControlKey);
    void clear();
    bool select(ui::ControlKey);
    File* active() const noexcept;
    const std::vector<std::unique_ptr<File>>& files() const noexcept {return files_;}
    void poll(bool composing=false);
    void append(std::vector<ui::Control>&,ui::ControlKey,ui_language::LiveDocument&,
        const Project* = nullptr,const ProjectBuild* = nullptr,bool running=false);
    std::vector<ui::MenuItem> menu_items() const;
    bool locate_problem();
    // Last compiler diagnostic is actionable only for the exact saved revision.
    bool problem_current() const noexcept {return problem_line_!=0;}
private:
    using Clock=std::chrono::steady_clock;
    std::vector<std::unique_ptr<File>> files_;
    ui::ControlKey selected_{},next_key_{source_control::first_document};
    Clock::time_point next_poll_{};
    std::shared_ptr<ui::CodeDocument> empty_{std::make_shared<ui::CodeDocument>(L"")};
    std::size_t problem_line_{},problem_column_{};
    std::uint64_t problem_revision_{};
    std::weak_ptr<ui::CodeDocument> problem_document_;
    std::filesystem::path problem_path_;
    std::string problem_source_;
    static Result<void> load(File&,std::string);
};
}

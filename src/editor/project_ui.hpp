#pragma once
#include "editor/project.hpp"
#include "ui_language/live_document.hpp"
#include <functional>

namespace atelier::editor {
// The same project UI module runs in isolated preview and game windows.
// Its sampled inputs are values; no scene Runtime pointers enter UI scopes.
class ProjectUI final {
public:
    ProjectUI();
    void sync(const Project*);
    // Owns the complete source/controls/layout transaction. A zero surface
    // suspends publication and returns no frame. The host routes cancellation
    // effects synchronously, then renders/accepts the returned valid frame.
    Result<std::shared_ptr<const ui::UiFrame>> prepare(ui::Session&, ui::Surface, double elapsed, std::size_t entities,
                                                       const std::function<void(const ui::Reply&)>& effects);
    Result<bool> event(const ui::Action&);
    std::string_view diagnostic() const noexcept;
    std::uint64_t revision() const noexcept { return document_ ? document_->revision() : 0; }
    bool configured() const noexcept { return !entry_.empty(); }

private:
    Runtime runtime_;
    ui::ControlKey next_key_{1ULL << 41};
    std::unique_ptr<ui_language::LiveDocument> document_;
    std::filesystem::path root_;
    std::filesystem::path entry_;
    std::wstring title_;
    std::string error_;
    std::vector<ui::Control> accepted_controls_;
    bool accepted_{};
    Result<void> collect();
};
}

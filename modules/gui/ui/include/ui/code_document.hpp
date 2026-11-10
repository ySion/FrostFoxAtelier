#pragma once
#include "ui/detail/text_edit.hpp"
#include "ui/error.hpp"
#include "foundation/result.hpp"
#include <algorithm>

namespace atelier::ui {
// The host owns document lifetime. A CodeEditor borrows it through a shared
// descriptor; hiding, docking or replacing UI scopes never discards its edits.
class CodeDocument {
public:
    CodeDocument() { buffer_.select({}, false); }

    // Admission and ICU segmentation complete before publishing the document.
    static Result<CodeDocument> create(std::wstring text) {
        if (text.size() > 1024 * 1024 || !valid_text(text))
            return std::unexpected(Error{ErrorCode::invalid_text, 0, 0,
                                         "CodeDocument requires bounded Unicode text with LF line endings."});
        ATELIER_TRY_BIND(buffer, detail::TextEdit::create(std::move(text), 1024 * 1024, false, false));
        return CodeDocument(std::move(*buffer));
    }

    // The same character contract governs creation and edits. Hosts may use it
    // without imposing the document-size budget on non-creating consumers.
    static bool valid_text(std::wstring_view text) noexcept { return detail::valid_document_text(text); }

    const std::wstring& text() const noexcept { return buffer_.current().text; }

    std::uint64_t revision() const noexcept { return buffer_.revision(); }

    detail::TextEdit& edit() noexcept { return buffer_; }

    const detail::TextEdit& edit() const noexcept { return buffer_; }

    void locate(std::size_t line, std::size_t column) {
        const auto row = std::min(line ? line - 1 : 0, buffer_.line_count() - 1);
        const auto offset = buffer_.line_start(row) +
                            static_cast<std::uint32_t>(std::min(column ? column - 1 : 0, buffer_.line(row).size()));
        buffer_.select({offset, false}, false);
    }

private:
    explicit CodeDocument(detail::TextEdit buffer) : buffer_(std::move(buffer)) { buffer_.select({}, false); }

    detail::TextEdit buffer_{1024 * 1024, false};
};
}

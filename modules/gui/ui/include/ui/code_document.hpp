#pragma once
#include "ui/detail/text_edit.hpp"
#include "ui/error.hpp"
#include <algorithm>

namespace atelier::ui {
// The host owns document lifetime. A CodeEditor borrows it through a shared
// descriptor; hiding, docking or replacing UI scopes never discards its edits.
class CodeDocument {
public:
    CodeDocument() : CodeDocument(std::wstring{}) {}

    // Text admission only; ICU failures still belong to the text backend.
    static Result<CodeDocument> create(std::wstring text) {
        if (text.size() > 1024 * 1024 || !valid_text(text))
            return std::unexpected(Error{ErrorCode::invalid_text, 0, 0,
                                         "CodeDocument requires bounded Unicode text with LF line endings."});
        return CodeDocument(std::move(text));
    }

    // Character contract, without the document-size budget. Editing does not
    // reapply this predicate; hosts also use it for non-creating text consumers.
    static bool valid_text(std::wstring_view text) noexcept {
        return detail::valid_text(text, false) &&
               std::ranges::none_of(text, [](wchar_t c) { return (c < 32 && c != L'\n' && c != L'\t') || c == 127; });
    }

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
    explicit CodeDocument(std::wstring text) : buffer_(std::move(text), 1024 * 1024, false, false) {
        buffer_.select({}, false);
    }

    detail::TextEdit buffer_;
};
}

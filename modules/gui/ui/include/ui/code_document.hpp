#pragma once
#include "ui/detail/text_edit.hpp"
#include <algorithm>
#include <stdexcept>

namespace atelier::ui {
// The host owns document lifetime. A CodeEditor borrows it through a shared
// descriptor; hiding, docking or replacing UI scopes never discards its edits.
class CodeDocument {
public:
    explicit CodeDocument(std::wstring text) : buffer_(validated(std::move(text)), 1024 * 1024, false, false) {
        buffer_.select({}, false);
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
    static std::wstring validated(std::wstring text) {
        if (text.size() > 1024 * 1024 || !detail::valid_text(text, false) ||
            std::ranges::any_of(text, [](wchar_t c) { return (c < 32 && c != L'\n' && c != L'\t') || c == 127; }))
            throw std::invalid_argument("CodeDocument requires bounded Unicode text with LF line endings.");
        return text;
    }

    detail::TextEdit buffer_;
};
}

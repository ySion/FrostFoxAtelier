#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::ui::detail {
struct TextPosition {
    std::uint32_t offset{};
    bool trailing{}; // Visual affinity at this logical UTF-16 boundary.
    bool operator==(const TextPosition&) const = default;
};
bool valid_text(std::wstring_view, bool single_line = true) noexcept;
std::vector<std::uint32_t> character_boundaries(std::wstring_view);

// The single bounded editing buffer for both text and numeric fields.
class TextEdit {
public:
    enum class Group { insert, backspace, delete_forward, isolated };
    enum class Change { none, selection, text, rejected };
    struct Snapshot {
        std::wstring text;
        TextPosition caret, anchor;
        bool operator==(const Snapshot&) const = default;
    };
    TextEdit(std::wstring text, std::size_t limit, bool select_all);
    const Snapshot& current() const noexcept { return current_; }
    const std::wstring& original() const noexcept { return original_; }
    const std::vector<std::uint32_t>& boundaries() const noexcept { return boundaries_; }
    bool active() const noexcept { return active_; }
    bool select(TextPosition, bool extend);
    bool select_all();
    TextPosition snap(std::uint32_t, bool trailing = false) const noexcept;
    Change replace(std::wstring_view, Group = Group::isolated);
    Change erase(bool backwards);
    Change history(bool redo);
    void break_group() noexcept { merge_ = false; }
private:
    struct Entry { Snapshot before, after; Group group; };
    Snapshot current_;
    std::wstring original_;
    std::vector<std::uint32_t> boundaries_;
    std::vector<Entry> history_;
    std::size_t limit_{}, applied_{}, bytes_{};
    bool active_{}, merge_{};
    Change replace_range(std::uint32_t, std::uint32_t, std::wstring_view, Group);
};
}

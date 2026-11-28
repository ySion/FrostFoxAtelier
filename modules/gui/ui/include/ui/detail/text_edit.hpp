#pragma once
#include "foundation/array.hpp"
#include "foundation/ring_deque.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "ui/error.hpp"
#include <cstdint>

namespace atelier::ui::detail {
struct TextPosition {
    std::uint32_t offset{};

    bool trailing{}; // Visual affinity at this logical UTF-8 grapheme boundary.

    bool operator==(const TextPosition&) const = default;
};

bool valid_text(StringView, bool single_line = true) noexcept;

bool valid_document_text(StringView) noexcept;

Result<Array<std::uint32_t>> character_boundaries(StringView);

// Shared Unicode editing mechanics. Documents retain this buffer across views;
// fields own a short-lived buffer. History stores UTF-8 splices, never whole documents.
class TextEdit {
public:
    enum class Group { insert, backspace, delete_forward, isolated };

    enum class Change { none, selection, text, rejected };

    struct Snapshot {
        String text;

        TextPosition caret;

        TextPosition anchor;

        bool operator==(const Snapshot&) const = default;
    };

    // Empty buffers need no system service. Nonempty creation segments with ICU.
    explicit TextEdit(std::size_t limit, bool single_line = true)
        : current_{{}, {0, true}, {0, true}}, limit_(limit), single_line_(single_line) {
        boundaries_.add(0);
        lines_.add(0);
    }

    static Result<TextEdit> create(String text, std::size_t limit, bool select_all, bool single_line = true);

    const Snapshot& current() const noexcept { return current_; }

    const String& original() const noexcept { return original_; }

    Span<const std::uint32_t> boundaries() const noexcept { return boundaries_.view(); }

    bool active() const noexcept { return active_; }

    std::uint64_t revision() const noexcept { return revision_; }

    std::uint64_t selection_revision() const noexcept { return selection_revision_; }

    bool can_undo() const noexcept { return applied_ != 0; }

    bool can_redo() const noexcept { return applied_ != history_.size(); }

    std::size_t line_count() const noexcept { return lines_.size(); }

    std::size_t line_at(std::uint32_t) const noexcept;

    std::uint32_t line_start(std::size_t) const noexcept;

    StringView line(std::size_t) const noexcept;

    bool select(TextPosition, bool extend);

    bool select_all();

    TextPosition snap(std::uint32_t, bool trailing = false) const noexcept;

    Result<Change> replace(StringView, Group = Group::isolated);

    Result<Change> erase(bool backwards);

    Result<Change> history(bool redo);

    void break_group() noexcept { merge_ = false; }

private:
    struct Selection {
        TextPosition caret;

        TextPosition anchor;

        bool operator==(const Selection&) const = default;
    };

    struct Entry {
        std::uint32_t first;

        String removed;

        String inserted;

        Selection before;

        Selection after;

        Group group;
    };

    Snapshot current_;

    String original_;

    Array<std::uint32_t> boundaries_;

    Array<std::uint32_t> lines_;

    RingDeque<Entry> history_;

    std::size_t limit_{};

    std::size_t applied_{};

    std::size_t bytes_{};

    bool active_{};

    bool merge_{};

    bool single_line_{true};

    std::uint64_t revision_{1};

    std::uint64_t selection_revision_{1};

    Result<void> splice(std::uint32_t, std::uint32_t, StringView);

    Result<Change> replace_range(std::uint32_t, std::uint32_t, StringView, Group);
};
} // namespace atelier::ui::detail

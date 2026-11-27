#include "ui/detail/text_edit.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/result.hpp"
#include <Windows.h>
#include <icu.h>
#include <algorithm>
#include <memory>

namespace atelier::ui::detail {
bool valid_text(std::wstring_view text, bool single_line) noexcept {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<std::uint16_t>(text[i]);
        if (!c || (single_line && (c < 32 || c == 127 || c == 0x2028 || c == 0x2029)))
            return false;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                return false;
        } else if (c >= 0xdc00 && c <= 0xdfff)
            return false;
    }
    return true;
}

bool valid_document_text(std::wstring_view text) noexcept {
    return valid_text(text, false) &&
           std::ranges::none_of(text, [](wchar_t c) { return (c < 32 && c != L'\n' && c != L'\t') || c == 127; });
}

Result<Array<std::uint32_t>> character_boundaries(std::wstring_view text) {
    // ICU retains the source until close. An explicit conversion avoids aliasing
    // wchar_t storage as UChar when these are distinct C++ types.
    const InlineArray<UChar, 256> input(text.begin(), text.end());
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> iterator(
        ubrk_open(UBRK_CHARACTER, "root", input.data(), static_cast<std::int32_t>(input.size()), &error), ubrk_close);
    if (U_FAILURE(error) || !iterator)
        return std::unexpected(
            Error{ErrorCode::text_failed, 0, static_cast<std::uint32_t>(E_FAIL), "UI text/resource operation failed"});
    Array<std::uint32_t> result;
    result.reserve(input.size() + 1);
    for (auto at = ubrk_first(iterator.get()); at != UBRK_DONE; at = ubrk_next(iterator.get()))
        result.add(static_cast<std::uint32_t>(at));
    return result;
}

Result<TextEdit> TextEdit::create(std::wstring text, std::size_t limit, bool select_all, bool single_line) {
    TextEdit result(limit, single_line);
    result.current_.text = std::move(text);
    result.current_.caret = {static_cast<std::uint32_t>(result.current_.text.size()), true};
    result.current_.anchor = select_all ? TextPosition{} : result.current_.caret;
    if (single_line)
        result.original_ = result.current_.text;
    if (!result.current_.text.empty()) {
        ATELIER_TRY_BIND(boundaries, character_boundaries(result.current_.text));
        result.boundaries_ = std::move(*boundaries);
    }
    for (std::size_t i = 0; i < result.current_.text.size(); ++i)
        if (result.current_.text[i] == L'\n')
            result.lines_.add(static_cast<std::uint32_t>(i + 1));
    return result;
}

std::size_t TextEdit::line_at(std::uint32_t offset) const noexcept {
    return static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), offset) - lines_.begin() - 1);
}

std::uint32_t TextEdit::line_start(std::size_t row) const noexcept {
    return row < lines_.size() ? lines_[row] : static_cast<std::uint32_t>(current_.text.size());
}

std::wstring_view TextEdit::line(std::size_t row) const noexcept {
    if (row >= lines_.size())
        return {};
    const auto start = lines_[row];
    const auto end = row + 1 < lines_.size() ? lines_[row + 1] - 1 : static_cast<std::uint32_t>(current_.text.size());
    return std::wstring_view(current_.text).substr(start, end - start);
}

TextPosition TextEdit::snap(std::uint32_t offset, bool trailing) const noexcept {
    auto at = std::lower_bound(boundaries_.begin(), boundaries_.end(), offset);
    if (at == boundaries_.end())
        return {boundaries_.back(), true};
    if (*at != offset && at != boundaries_.begin() && !trailing)
        --at;
    return {*at, trailing && *at != 0};
}

bool TextEdit::select(TextPosition position, bool extend) {
    break_group();
    position = snap(position.offset, position.trailing);
    const auto anchor = extend ? current_.anchor : position;
    const bool changed = current_.caret != position || current_.anchor != anchor;
    current_.caret = position;
    current_.anchor = anchor;
    if (changed)
        ++selection_revision_;
    return changed;
}

bool TextEdit::select_all() {
    break_group();
    const TextPosition end{static_cast<std::uint32_t>(current_.text.size()), true};
    const bool changed = current_.anchor.offset != 0 || current_.caret != end;
    current_.anchor = {};
    current_.caret = end;
    if (changed)
        ++selection_revision_;
    return changed;
}

Result<void> TextEdit::splice(std::uint32_t first, std::uint32_t last, std::wstring_view inserted) {
    // Re-segment only the touched logical lines. Newlines isolate grapheme
    // clusters; unchanged suffix offsets need a shift, not another ICU scan.
    const auto start = line_start(line_at(first));
    const auto end = line_start(line_at(last) + 1);
    auto segment = current_.text.substr(start, first - start);
    segment.append(inserted);
    segment.append(current_.text, last, end - last);
    ATELIER_TRY_BIND(local, character_boundaries(segment));
    for (auto& offset : *local)
        offset += start;
    const auto delta = static_cast<std::int64_t>(inserted.size()) - (last - first);
    Array<std::uint32_t> new_lines;
    for (std::size_t i = 0; i < segment.size(); ++i)
        if (segment[i] == L'\n')
            new_lines.add(start + static_cast<std::uint32_t>(i + 1));
    current_.text.replace(first, last - first, inserted);
    const auto b_first =
        static_cast<std::size_t>(std::lower_bound(boundaries_.begin(), boundaries_.end(), start) - boundaries_.begin());
    const auto b_last =
        static_cast<std::size_t>(std::upper_bound(boundaries_.begin(), boundaries_.end(), end) - boundaries_.begin());
    boundaries_.remove_at(b_first, b_last - b_first);
    for (std::size_t tail = b_first; tail < boundaries_.size(); ++tail)
        boundaries_[tail] = static_cast<std::uint32_t>(boundaries_[tail] + delta);
    boundaries_.insert_range(b_first, *local);
    const auto l_first =
        static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), start) - lines_.begin());
    const auto l_last = static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), end) - lines_.begin());
    lines_.remove_at(l_first, l_last - l_first);
    for (std::size_t tail = l_first; tail < lines_.size(); ++tail)
        lines_[tail] = static_cast<std::uint32_t>(lines_[tail] + delta);
    lines_.insert_range(l_first, new_lines);
    ++revision_;
    ++selection_revision_;
    return {};
}

Result<TextEdit::Change> TextEdit::replace(std::wstring_view inserted, Group group) {
    return replace_range(std::min(current_.caret.offset, current_.anchor.offset),
                         std::max(current_.caret.offset, current_.anchor.offset), inserted, group);
}

Result<TextEdit::Change> TextEdit::replace_range(std::uint32_t first, std::uint32_t last, std::wstring_view inserted,
                                                 Group group) {
    if (!(single_line_ ? valid_text(inserted) : valid_document_text(inserted)) ||
        current_.text.size() - (last - first) + inserted.size() > limit_)
        return Change::rejected;
    if (std::wstring_view(current_.text).substr(first, last - first) == inserted) {
        const bool changed = select(snap(first + static_cast<std::uint32_t>(inserted.size()), true), false);
        return changed ? Change::selection : Change::none;
    }
    Entry next{
        first, current_.text.substr(first, last - first), std::wstring(inserted), {current_.caret, current_.anchor}, {},
        group};
    bool merge = merge_ && applied_ == history_.size() && applied_ && group != Group::isolated &&
                 history_.back().group == group && history_.back().after == next.before;
    if (merge) {
        const auto& previous = history_.back();
        if (group == Group::insert && next.removed.empty() && first == previous.first + previous.inserted.size()) {
            next.first = previous.first;
            next.removed = previous.removed;
            next.inserted = previous.inserted + next.inserted;
        } else if (group == Group::backspace && next.inserted.empty() && previous.inserted.empty() &&
                   last == previous.first) {
            next.removed += previous.removed;
        } else if (group == Group::delete_forward && next.inserted.empty() && previous.inserted.empty() &&
                   first == previous.first) {
            next.removed = previous.removed + next.removed;
        } else
            merge = false;
        if (merge)
            next.before = previous.before;
    }
    ATELIER_TRY(splice(first, last, inserted));
    current_.caret = current_.anchor = snap(first + static_cast<std::uint32_t>(inserted.size()), true);
    next.after = {current_.caret, current_.anchor};
    const auto cost = [](const Entry& entry) { return 2 * (entry.removed.size() + entry.inserted.size()); };
    while (history_.size() > applied_) {
        bytes_ -= cost(history_.back());
        history_.pop_back();
    }
    if (merge) {
        bytes_ -= cost(history_.back());
        history_.pop_back();
    }
    bytes_ += cost(next);
    history_.emplace_back(std::move(next));
    while (history_.size() > (single_line_ ? 64u : 512u) ||
           (history_.size() > 1 && bytes_ > (single_line_ ? 64u * 1024 : 8u * 1024 * 1024))) {
        bytes_ -= cost(history_.front());
        history_.pop_front();
    }
    applied_ = history_.size();
    active_ = true;
    merge_ = group != Group::isolated;
    return Change::text;
}

Result<TextEdit::Change> TextEdit::erase(bool backwards) {
    auto first = std::min(current_.caret.offset, current_.anchor.offset);
    auto last = std::max(current_.caret.offset, current_.anchor.offset);
    if (first == last) {
        auto at = std::lower_bound(boundaries_.begin(), boundaries_.end(), first);
        if (backwards) {
            if (at == boundaries_.begin())
                return Change::none;
            first = *--at;
        } else {
            if (++at == boundaries_.end())
                return Change::none;
            last = *at;
        }
    }
    return replace_range(first, last, {}, backwards ? Group::backspace : Group::delete_forward);
}

Result<TextEdit::Change> TextEdit::history(bool redo) {
    break_group();
    if ((!redo && !applied_) || (redo && applied_ == history_.size()))
        return Change::none;
    const auto& entry = history_[redo ? applied_ : applied_ - 1];
    ATELIER_TRY(splice(entry.first,
                       entry.first + static_cast<std::uint32_t>(redo ? entry.removed.size() : entry.inserted.size()),
                       redo ? entry.inserted : entry.removed));
    const auto selection = redo ? entry.after : entry.before;
    current_.caret = selection.caret;
    current_.anchor = selection.anchor;
    if (redo)
        ++applied_;
    else
        --applied_;
    active_ = true;
    return Change::text;
}
}

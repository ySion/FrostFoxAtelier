#include "ui/detail/text_edit.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/result.hpp"
#include "foundation/scope_exit.hpp"
#include <Windows.h>
#include <icu.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace atelier::ui::detail {
namespace {
void splice_offsets(Array<std::uint32_t>& offsets, std::size_t prefix_len, std::size_t old_mid_len,
                    Span<const std::uint32_t> inserted, std::int64_t delta) {
    const std::size_t suffix_start = prefix_len + old_mid_len;
    const std::size_t suffix_len = offsets.size() - suffix_start;
    const std::size_t new_mid_len = inserted.size();
    const std::size_t new_total = prefix_len + new_mid_len + suffix_len;
    if (new_total > offsets.size()) {
        offsets.resize(new_total);
        if (suffix_len > 0) {
            std::memmove(offsets.data() + prefix_len + new_mid_len, offsets.data() + suffix_start,
                         suffix_len * sizeof(std::uint32_t));
        }
    } else if (new_total < offsets.size()) {
        if (suffix_len > 0) {
            std::memmove(offsets.data() + prefix_len + new_mid_len, offsets.data() + suffix_start,
                         suffix_len * sizeof(std::uint32_t));
        }
        offsets.resize(new_total);
    }
    if (new_mid_len > 0)
        std::memcpy(offsets.data() + prefix_len, inserted.data(), new_mid_len * sizeof(std::uint32_t));
    if (delta != 0) {
        std::uint32_t* tail = offsets.data() + prefix_len + new_mid_len;
        for (std::size_t i = 0; i < suffix_len; ++i)
            tail[i] = static_cast<std::uint32_t>(static_cast<std::int64_t>(tail[i]) + delta);
    }
}

enum class TextValidationPolicy : std::uint8_t { multiline_any, single_line, document_lf };

bool check_utf8_text(StringView text, TextValidationPolicy policy) noexcept {
    if (!text.is_valid_utf8())
        return false;
    const char* data = text.data();
    const std::size_t len = text.size();
    for (std::size_t i = 0; i < len; ++i) {
        const auto b = static_cast<unsigned char>(data[i]);
        if (b == 0u)
            return false;
        if (policy == TextValidationPolicy::single_line) {
            if (b < 32u || b == 127u)
                return false;
            if (b == 0xe2u && i + 2 < len && static_cast<unsigned char>(data[i + 1]) == 0x80u) {
                const auto b2 = static_cast<unsigned char>(data[i + 2]);
                if (b2 == 0xa8u || b2 == 0xa9u)
                    return false;
            }
        } else if (policy == TextValidationPolicy::document_lf) {
            if ((b < 32u && b != '\n' && b != '\t') || b == 127u)
                return false;
        }
    }
    return true;
}
} // namespace

bool valid_text(StringView text, bool single_line) noexcept {
    return check_utf8_text(text, single_line ? TextValidationPolicy::single_line : TextValidationPolicy::multiline_any);
}

bool valid_document_text(StringView text) noexcept {
    return check_utf8_text(text, TextValidationPolicy::document_lf);
}

Result<Array<std::uint32_t>> character_boundaries(StringView text) {
    Array<std::uint32_t> result;
    if (text.empty()) {
        result.add(0);
        return result;
    }
    UErrorCode error = U_ZERO_ERROR;
    UText ut = UTEXT_INITIALIZER;
    utext_openUTF8(&ut, text.data(), static_cast<std::int64_t>(text.size()), &error);
    ScopeExit close_ut{[&]() noexcept { utext_close(&ut); }};
    std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> iterator(
        ubrk_open(UBRK_CHARACTER, "root", nullptr, 0, &error), ubrk_close);
    if (U_SUCCESS(error) && iterator)
        ubrk_setUText(iterator.get(), &ut, &error);
    if (U_FAILURE(error) || !iterator)
        return std::unexpected(
            Error{ErrorCode::text_failed, 0, static_cast<std::uint32_t>(E_FAIL), "UI text/resource operation failed"});
    result.reserve(text.size() + 1);
    for (auto at = ubrk_first(iterator.get()); at != UBRK_DONE; at = ubrk_next(iterator.get()))
        result.add(static_cast<std::uint32_t>(at));
    return result;
}

Result<TextEdit> TextEdit::create(String text, std::size_t limit, bool select_all, bool single_line) {
    if (text.size() > limit || !(single_line ? valid_text(text.view(), true) : valid_document_text(text.view())))
        return std::unexpected(Error{ErrorCode::invalid_text, 0, 0, "TextEdit requires bounded valid UTF-8 text."});
    TextEdit result(limit, single_line);
    result.current_.text = std::move(text);
    result.current_.caret = {static_cast<std::uint32_t>(result.current_.text.size()), true};
    result.current_.anchor = select_all ? TextPosition{} : result.current_.caret;
    if (single_line)
        result.original_ = result.current_.text;
    if (!result.current_.text.empty()) {
        ATELIER_TRY_BIND(boundaries, character_boundaries(result.current_.text.view()));
        result.boundaries_ = std::move(*boundaries);
    }
    for (std::size_t i = 0; i < result.current_.text.size(); ++i)
        if (result.current_.text[i] == '\n')
            result.lines_.add(static_cast<std::uint32_t>(i + 1));
    return result;
}

std::size_t TextEdit::line_at(std::uint32_t offset) const noexcept {
    return static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), offset) - lines_.begin() - 1);
}

std::uint32_t TextEdit::line_start(std::size_t row) const noexcept {
    return row < lines_.size() ? lines_[row] : static_cast<std::uint32_t>(current_.text.size());
}

StringView TextEdit::line(std::size_t row) const noexcept {
    if (row >= lines_.size())
        return {};
    const auto start = lines_[row];
    const auto end = row + 1 < lines_.size() ? lines_[row + 1] - 1 : static_cast<std::uint32_t>(current_.text.size());
    return current_.text.slice(start, end - start);
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

Result<void> TextEdit::splice(std::uint32_t first, std::uint32_t last, StringView inserted) {
    // Re-segment only the touched logical lines. Newlines isolate grapheme
    // clusters; unchanged suffix offsets need a shift, not another ICU scan.
    const auto start = line_start(line_at(first));
    const auto end = line_start(line_at(last) + 1);
    const StringView prefix = current_.text.slice(start, first - start);
    const StringView suffix = current_.text.slice(last, end - last);
    InlineArray<char, 256> segment;
    segment.resize(prefix.size() + inserted.size() + suffix.size());
    if (!prefix.empty())
        std::memcpy(segment.data(), prefix.data(), prefix.size());
    if (!inserted.empty())
        std::memcpy(segment.data() + prefix.size(), inserted.data(), inserted.size());
    if (!suffix.empty())
        std::memcpy(segment.data() + prefix.size() + inserted.size(), suffix.data(), suffix.size());
    ATELIER_TRY_BIND(local, character_boundaries(StringView{segment.data(), segment.size()}));
    for (auto& offset : *local)
        offset += start;
    const auto delta = static_cast<std::int64_t>(inserted.size()) - static_cast<std::int64_t>(last - first);
    InlineArray<std::uint32_t, 8> new_lines;
    for (std::size_t i = 0; i < segment.size(); ++i)
        if (segment[i] == '\n')
            new_lines.add(start + static_cast<std::uint32_t>(i + 1));
    (void)current_.text.replace(first, last - first, inserted);
    const auto b_low =
        static_cast<std::size_t>(std::lower_bound(boundaries_.begin(), boundaries_.end(), start) - boundaries_.begin());
    const auto b_high =
        static_cast<std::size_t>(std::upper_bound(boundaries_.begin(), boundaries_.end(), end) - boundaries_.begin());
    splice_offsets(boundaries_, b_low, b_high - b_low, local->view(), delta);
    const auto l_low = static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), start) - lines_.begin());
    const auto l_high = static_cast<std::size_t>(std::upper_bound(lines_.begin(), lines_.end(), end) - lines_.begin());
    splice_offsets(lines_, l_low, l_high - l_low, new_lines.view(), delta);
    ++revision_;
    ++selection_revision_;
    return {};
}

Result<TextEdit::Change> TextEdit::replace(StringView inserted, Group group) {
    return replace_range(std::min(current_.caret.offset, current_.anchor.offset),
                         std::max(current_.caret.offset, current_.anchor.offset), inserted, group);
}

Result<TextEdit::Change> TextEdit::replace_range(std::uint32_t first, std::uint32_t last, StringView inserted,
                                                 Group group) {
    if (!(single_line_ ? valid_text(inserted) : valid_document_text(inserted)) ||
        current_.text.size() - (last - first) + inserted.size() > limit_)
        return Change::rejected;
    if (current_.text.slice(first, last - first) == inserted) {
        const bool changed = select(snap(first + static_cast<std::uint32_t>(inserted.size()), true), false);
        return changed ? Change::selection : Change::none;
    }
    Entry next{first,
               String::from_utf8_validated(current_.text.slice(first, last - first)),
               String::from_utf8_validated(inserted),
               {current_.caret, current_.anchor},
               {},
               group};
    bool merge = merge_ && applied_ == history_.size() && applied_ && group != Group::isolated &&
                 history_.back().group == group && history_.back().after == next.before;
    if (merge) {
        const auto& previous = history_.back();
        if (group == Group::insert && next.removed.empty() && first == previous.first + previous.inserted.size()) {
            next.first = previous.first;
            next.removed = previous.removed;
            String merged = previous.inserted;
            (void)merged.append(next.inserted);
            next.inserted = std::move(merged);
        } else if (group == Group::backspace && next.inserted.empty() && previous.inserted.empty() &&
                   last == previous.first) {
            (void)next.removed.append(previous.removed);
        } else if (group == Group::delete_forward && next.inserted.empty() && previous.inserted.empty() &&
                   first == previous.first) {
            String merged = previous.removed;
            (void)merged.append(next.removed);
            next.removed = std::move(merged);
        } else {
            merge = false;
        }
        if (merge)
            next.before = previous.before;
    }
    ATELIER_TRY(splice(first, last, inserted));
    current_.caret = current_.anchor = snap(first + static_cast<std::uint32_t>(inserted.size()), true);
    next.after = {current_.caret, current_.anchor};
    const auto cost = [](const Entry& entry) { return entry.removed.size() + entry.inserted.size(); };
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
                       redo ? entry.inserted.view() : entry.removed.view()));
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
} // namespace atelier::ui::detail

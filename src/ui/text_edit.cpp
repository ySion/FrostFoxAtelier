#include "ui/text_edit.hpp"
#include "ui/text.hpp"
#include <Windows.h>
#include <icu.h>
#include <algorithm>
#include <memory>

namespace atelier::ui::detail {
bool valid_text(std::wstring_view text, bool single_line) noexcept {
    for (std::size_t i=0;i<text.size();++i) {
        const auto c=static_cast<std::uint16_t>(text[i]);
        if (!c || (single_line && (c<32 || c==127 || c==0x2028 || c==0x2029))) return false;
        if (c>=0xd800 && c<=0xdbff) {
            if (++i==text.size() || text[i]<0xdc00 || text[i]>0xdfff) return false;
        } else if (c>=0xdc00 && c<=0xdfff) return false;
    }
    return true;
}
std::vector<std::uint32_t> character_boundaries(std::wstring_view text) {
    // ICU retains the source until close. An explicit conversion avoids aliasing
    // wchar_t storage as UChar when these are distinct C++ types.
    const std::vector<UChar> input(text.begin(),text.end());
    UErrorCode error=U_ZERO_ERROR;
    std::unique_ptr<UBreakIterator,decltype(&ubrk_close)> iterator(
        ubrk_open(UBRK_CHARACTER,"root",input.data(),static_cast<std::int32_t>(input.size()),&error),ubrk_close);
    if (U_FAILURE(error) || !iterator) throw TextFailure{static_cast<std::uint32_t>(E_FAIL)};
    std::vector<std::uint32_t> result;
    for (auto at=ubrk_first(iterator.get());at!=UBRK_DONE;at=ubrk_next(iterator.get())) result.push_back(static_cast<std::uint32_t>(at));
    return result;
}
TextEdit::TextEdit(std::wstring text, std::size_t limit, bool select_all)
    : current_{text,{static_cast<std::uint32_t>(text.size()),true},{}}, original_(std::move(text)),
      boundaries_(character_boundaries(current_.text)),limit_(limit) {
    if (!select_all) current_.anchor=current_.caret;
}
TextPosition TextEdit::snap(std::uint32_t offset, bool trailing) const noexcept {
    auto at=std::lower_bound(boundaries_.begin(),boundaries_.end(),offset);
    if (at==boundaries_.end()) return {boundaries_.back(),true};
    if (*at!=offset && at!=boundaries_.begin() && !trailing) --at;
    return {*at,trailing && *at!=0};
}
bool TextEdit::select(TextPosition position, bool extend) {
    break_group(); position=snap(position.offset,position.trailing);
    const auto anchor=extend?current_.anchor:position;
    const bool changed=current_.caret!=position || current_.anchor!=anchor;
    current_.caret=position; current_.anchor=anchor; return changed;
}
bool TextEdit::select_all() {
    break_group(); const TextPosition end{static_cast<std::uint32_t>(current_.text.size()),true};
    const bool changed=current_.anchor.offset!=0 || current_.caret!=end;
    current_.anchor={}; current_.caret=end; return changed;
}
TextEdit::Change TextEdit::replace(std::wstring_view inserted, Group group) {
    return replace_range(std::min(current_.caret.offset,current_.anchor.offset),
        std::max(current_.caret.offset,current_.anchor.offset),inserted,group);
}
TextEdit::Change TextEdit::replace_range(std::uint32_t first, std::uint32_t last, std::wstring_view inserted, Group group) {
    if (!valid_text(inserted) || current_.text.size()-(last-first)+inserted.size()>limit_) return Change::rejected;
    Snapshot next=current_;
    next.text.replace(first,last-first,inserted);
    auto boundaries=character_boundaries(next.text);
    auto at=std::lower_bound(boundaries.begin(),boundaries.end(),first+inserted.size());
    next.caret=next.anchor={*at,*at!=0};
    if (next.text==current_.text) {
        const bool changed=next!=current_; current_=std::move(next);
        merge_=false; return changed?Change::selection:Change::none;
    }
    while (history_.size()>applied_) {
        bytes_-=2*(history_.back().before.text.size()+history_.back().after.text.size()); history_.pop_back();
    }
    const bool merge=merge_ && group!=Group::isolated && !history_.empty() && history_.back().group==group &&
        history_.back().after==current_;
    if (merge) {
        auto replacement=next; // Allocate before releasing the previous history state.
        bytes_-=2*history_.back().after.text.size(); history_.back().after=std::move(replacement);
        bytes_+=2*history_.back().after.text.size();
    } else {
        history_.push_back({current_,next,group});
        bytes_+=2*(current_.text.size()+next.text.size());
    }
    while (history_.size()>64 || bytes_>64*1024) {
        bytes_-=2*(history_.front().before.text.size()+history_.front().after.text.size()); history_.erase(history_.begin());
    }
    applied_=history_.size(); current_=std::move(next); boundaries_=std::move(boundaries);
    active_=true; merge_=group!=Group::isolated; return Change::text;
}
TextEdit::Change TextEdit::erase(bool backwards) {
    auto first=std::min(current_.caret.offset,current_.anchor.offset), last=std::max(current_.caret.offset,current_.anchor.offset);
    if (first==last) {
        auto at=std::lower_bound(boundaries_.begin(),boundaries_.end(),first);
        if (backwards) { if (at==boundaries_.begin()) return Change::none; first=*--at; }
        else { if (++at==boundaries_.end()) return Change::none; last=*at; }
    }
    return replace_range(first,last,{},backwards?Group::backspace:Group::delete_forward);
}
TextEdit::Change TextEdit::history(bool redo) {
    break_group();
    if ((!redo && !applied_) || (redo && applied_==history_.size())) return Change::none;
    const auto& next=redo?history_[applied_].after:history_[applied_-1].before;
    auto boundaries=character_boundaries(next.text);
    current_=next; boundaries_=std::move(boundaries);
    if (redo) ++applied_; else --applied_;
    return Change::text;
}
}

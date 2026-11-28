#include "foundation/string.hpp"

#include <mimalloc.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace atelier {

char* String::allocate_heap(std::size_t capacity) noexcept {
    if (capacity > max_size)
        fatal("String exceeds addressable storage.");
    auto* buffer = static_cast<char*>(mi_malloc(capacity + 1));
    if (!buffer)
        fatal("String allocation failed.");
    return buffer;
}

char* String::reallocate_heap(char* data, std::size_t capacity) noexcept {
    if (capacity > max_size)
        fatal("String exceeds addressable storage.");
    auto* buffer = static_cast<char*>(mi_realloc(data, capacity + 1));
    if (!buffer)
        fatal("String allocation failed.");
    return buffer;
}

std::expected<String, StringError> String::from_utf8(StringView text) noexcept {
    if (text.size() > max_size)
        fatal("String exceeds addressable storage.");
    if (!text.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    return from_utf8_validated(text);
}

String String::from_utf8_validated(StringView text) noexcept {
    if (text.size() > max_size)
        fatal("String exceeds addressable storage.");
    String result{Uninitialized{}};
    if (text.size() <= inline_capacity) {
        std::memset(result.bytes_, 0, sizeof(result.bytes_));
        if (text.size())
            std::memcpy(result.bytes_, text.data(), text.size());
        result.bytes_[31] = static_cast<char>(text.size());
    } else {
        char* buffer = allocate_heap(text.size());
        std::memcpy(buffer, text.data(), text.size());
        buffer[text.size()] = '\0';
        result.set_long({buffer, text.size(), text.size()}, heap_tag);
    }
    return result;
}

void String::append_hex_value(std::uint64_t value, std::size_t min_digits, bool uppercase) noexcept {
    if (min_digits > max_size - size())
        fatal("String exceeds addressable storage.");
    const char* alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[16];
    std::size_t count = 0;
    do {
        digits[15 - count] = alphabet[value & 0xfu];
        value >>= 4;
        ++count;
    } while (value != 0);
    const std::size_t pad = min_digits > count ? min_digits - count : 0;
    const std::size_t current = size();
    if (count > max_size - current - pad)
        fatal("String exceeds addressable storage.");
    const std::size_t total = current + pad + count;
    if (total > capacity() || tag() == literal_tag) {
        if (tag() == literal_tag && total <= inline_capacity) {
            char local[inline_capacity + 1];
            if (current)
                std::memcpy(local, c_str(), current);
            if (pad)
                std::memset(local + current, '0', pad);
            std::memcpy(local + current + pad, digits + (16 - count), count);
            local[total] = '\0';
            std::memcpy(bytes_, local, total + 1);
            bytes_[31] = static_cast<char>(total);
            return;
        }
        reallocate(grown_capacity(total));
    }
    char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
    if (pad)
        std::memset(dst + current, '0', pad);
    std::memcpy(dst + current + pad, digits + (16 - count), count);
    dst[total] = '\0';
    if (is_inline()) {
        bytes_[31] = static_cast<char>(total);
    } else {
        auto val = long_value();
        val.size = total;
        set_long(val, heap_tag);
    }
}

String String::format_hex_value(std::uint64_t value, std::size_t min_digits, bool uppercase) noexcept {
    String result;
    if (min_digits > inline_capacity)
        result.reserve(min_digits);
    result.append_hex_value(value, min_digits, uppercase);
    return result;
}

void String::append_float_fixed(double value, int precision) noexcept {
    const int clamped = precision < 0 ? 0 : precision;
    if (clamped <= 64) {
        char stack_buf[384];
        const auto res = std::to_chars(stack_buf, stack_buf + sizeof(stack_buf), value, std::chars_format::fixed,
                                       clamped);
        if (res.ec != std::errc{})
            fatal("Float fixed formatting overflow.");
        write_validated({StringView::Unchecked{}, stack_buf, static_cast<std::size_t>(res.ptr - stack_buf)}, size());
        return;
    }
    const auto extra = static_cast<std::size_t>(clamped);
    if (extra > max_size - 336)
        fatal("Float precision exceeds addressable storage.");
    const std::size_t cap = 336 + extra;
    char* temp = allocate_heap(cap);
    const auto res = std::to_chars(temp, temp + cap, value, std::chars_format::fixed, clamped);
    if (res.ec != std::errc{}) {
        mi_free_size(temp, cap + 1);
        fatal("Float fixed formatting overflow.");
    }
    write_validated({StringView::Unchecked{}, temp, static_cast<std::size_t>(res.ptr - temp)}, size());
    mi_free_size(temp, cap + 1);
}

void String::append_float_general(double value, int precision) noexcept {
    const int clamped = precision < 1 ? 1 : precision;
    if (clamped <= 64) {
        char stack_buf[96];
        const auto res =
            std::to_chars(stack_buf, stack_buf + sizeof(stack_buf), value, std::chars_format::general, clamped);
        if (res.ec != std::errc{})
            fatal("Float general formatting overflow.");
        write_validated({StringView::Unchecked{}, stack_buf, static_cast<std::size_t>(res.ptr - stack_buf)}, size());
        return;
    }
    const auto extra = static_cast<std::size_t>(clamped);
    if (extra > max_size - 32)
        fatal("Float precision exceeds addressable storage.");
    const std::size_t cap = 32 + extra;
    char* temp = allocate_heap(cap);
    const auto res = std::to_chars(temp, temp + cap, value, std::chars_format::general, clamped);
    if (res.ec != std::errc{}) {
        mi_free_size(temp, cap + 1);
        fatal("Float general formatting overflow.");
    }
    write_validated({StringView::Unchecked{}, temp, static_cast<std::size_t>(res.ptr - temp)}, size());
    mi_free_size(temp, cap + 1);
}

String String::from_float_fixed(double value, int precision) noexcept {
    String result;
    result.append_float_fixed(value, precision);
    return result;
}

String String::from_float_general(double value, int precision) noexcept {
    String result;
    result.append_float_general(value, precision);
    return result;
}

String String::repeat(const String& unit, std::size_t count) noexcept {
    const auto slice = unit.view();
    if (count == 0 || slice.empty())
        return {};
    if (count == 1)
        return unit;
    if (slice.size() > max_size / count)
        fatal("String exceeds addressable storage.");
    const std::size_t total = slice.size() * count;
    String result{Uninitialized{}};
    char* dst{};
    if (total <= inline_capacity) {
        std::memset(result.bytes_, 0, sizeof(result.bytes_));
        result.bytes_[31] = static_cast<char>(total);
        dst = result.bytes_;
    } else {
        dst = allocate_heap(total);
        dst[total] = '\0';
        result.set_long({dst, total, total}, heap_tag);
    }
    if (slice.size() == 1) {
        std::memset(dst, static_cast<unsigned char>(slice.data()[0]), total);
    } else {
        for (std::size_t i = 0; i != count; ++i) {
            std::memcpy(dst, slice.data(), slice.size());
            dst += slice.size();
        }
    }
    return result;
}

std::expected<String, StringError> String::repeat(StringView unit, std::size_t count) noexcept {
    if (!unit.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    if (count == 0 || unit.empty())
        return String{};
    if (unit.size() > max_size / count)
        fatal("String exceeds addressable storage.");
    const std::size_t total = unit.size() * count;
    String result{Uninitialized{}};
    char* dst{};
    if (total <= inline_capacity) {
        std::memset(result.bytes_, 0, sizeof(result.bytes_));
        result.bytes_[31] = static_cast<char>(total);
        dst = result.bytes_;
    } else {
        dst = allocate_heap(total);
        dst[total] = '\0';
        result.set_long({dst, total, total}, heap_tag);
    }
    if (unit.size() == 1) {
        std::memset(dst, static_cast<unsigned char>(unit.data()[0]), total);
    } else {
        for (std::size_t i = 0; i != count; ++i) {
            std::memcpy(dst, unit.data(), unit.size());
            dst += unit.size();
        }
    }
    return result;
}

String String::concat_views(Span<const StringView> pieces) noexcept {
    std::size_t total = 0;
    for (const StringView v : pieces) {
        if (v.size() > max_size - total)
            fatal("String exceeds addressable storage.");
        total += v.size();
    }
    if (total == 0)
        return {};
    String result{Uninitialized{}};
    char* dst{};
    if (total <= inline_capacity) {
        std::memset(result.bytes_, 0, sizeof(result.bytes_));
        result.bytes_[31] = static_cast<char>(total);
        dst = result.bytes_;
    } else {
        dst = allocate_heap(total);
        dst[total] = '\0';
        result.set_long({dst, total, total}, heap_tag);
    }
    for (const StringView v : pieces) {
        if (v.size()) {
            std::memcpy(dst, v.data(), v.size());
            dst += v.size();
        }
    }
    return result;
}

template <class Part, class ViewOf>
String String::build_joined(Span<const Part> parts, StringView separator, ViewOf view_of) noexcept {
    if (parts.empty())
        return {};
    std::size_t total{};
    for (std::size_t i = 0; i != parts.size(); ++i) {
        if (i) {
            if (separator.size() > max_size - total)
                fatal("String exceeds addressable storage.");
            total += separator.size();
        }
        const StringView piece = view_of(parts[i]);
        if (piece.size() > max_size - total)
            fatal("String exceeds addressable storage.");
        total += piece.size();
    }
    String result{Uninitialized{}};
    char* destination{};
    if (total <= inline_capacity) {
        std::memset(result.bytes_, 0, sizeof(result.bytes_));
        result.bytes_[31] = static_cast<char>(total);
        destination = result.bytes_;
    } else {
        destination = allocate_heap(total);
        destination[total] = '\0';
        result.set_long({destination, total, total}, heap_tag);
    }
    for (std::size_t i = 0; i != parts.size(); ++i) {
        if (i && separator.size()) {
            std::memcpy(destination, separator.data(), separator.size());
            destination += separator.size();
        }
        const StringView piece = view_of(parts[i]);
        if (piece.size()) {
            std::memcpy(destination, piece.data(), piece.size());
            destination += piece.size();
        }
    }
    return result;
}

std::expected<String, StringError> String::join_views(Span<const StringView> parts, StringView separator) noexcept {
    if (!separator.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    for (const StringView part : parts)
        if (!part.is_valid_utf8())
            return std::unexpected(StringError::invalid_utf8);
    return build_joined(parts, separator, [](StringView part) noexcept { return part; });
}

String String::join_strings(Span<const String> parts, const String& separator) noexcept {
    if (parts.size() == 1)
        return parts[0];
    return build_joined(parts, separator.view(), [](const String& part) noexcept { return part.view(); });
}

std::expected<String, StringError> String::join_strings_with_view(Span<const String> parts,
                                                                  StringView separator) noexcept {
    if (!separator.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    if (parts.size() == 1)
        return parts[0];
    return build_joined(parts, separator, [](const String& part) noexcept { return part.view(); });
}

void String::free_heap() noexcept {
    const auto value = long_value();
    mi_free_size(const_cast<char*>(value.data), value.capacity + 1);
}

void String::copy_heap(const String& other) noexcept {
    const auto value = other.long_value();
    if (value.size <= inline_capacity) {
        std::memset(bytes_, 0, sizeof(bytes_));
        std::memcpy(bytes_, value.data, value.size + 1);
        bytes_[31] = static_cast<char>(value.size);
        return;
    }
    char* copy = allocate_heap(value.size);
    std::memcpy(copy, value.data, value.size + 1);
    set_long({copy, value.size, value.size}, heap_tag);
}

void String::assign_from(const String& other) noexcept {
    if (other.tag() == literal_tag) {
        release();
        std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
    } else
        write_validated(other.view(), 0);
}

void String::clear() noexcept {
    if (tag() != heap_tag) {
        bytes_[0] = '\0';
        bytes_[31] = 0;
        return;
    }
    auto value = long_value();
    const_cast<char*>(value.data)[0] = '\0';
    value.size = 0;
    set_long(value, heap_tag);
}

std::size_t String::grown_capacity(std::size_t minimum) const noexcept {
    const auto current = capacity();
    return std::max(minimum, current + std::min(max_size - current, std::max(current / 2, std::size_t{8})));
}

void String::reallocate(std::size_t new_capacity) noexcept {
    if (tag() == heap_tag) {
        const auto value = long_value();
        char* replacement = reallocate_heap(const_cast<char*>(value.data), new_capacity);
        set_long({replacement, value.size, new_capacity}, heap_tag);
        return;
    }
    const auto length = size();
    char* replacement = allocate_heap(new_capacity);
    std::memcpy(replacement, c_str(), length + 1);
    set_long({replacement, length, new_capacity}, heap_tag);
}

void String::reserve(std::size_t bytes) noexcept {
    if (bytes > max_size)
        fatal("String exceeds addressable storage.");
    if (bytes <= capacity())
        return;
    if (tag() == literal_tag && bytes <= inline_capacity) {
        const auto length = size();
        const char* source = c_str();
        std::memset(bytes_, 0, sizeof(bytes_));
        if (length)
            std::memcpy(bytes_, source, length);
        bytes_[31] = static_cast<char>(length);
        return;
    }
    reallocate(bytes);
}

void String::squeeze() noexcept {
    if (tag() != heap_tag)
        return;
    const auto value = long_value();
    if (value.size <= inline_capacity) {
        std::memcpy(bytes_, value.data, value.size + 1);
        bytes_[31] = static_cast<char>(value.size);
        mi_free_size(const_cast<char*>(value.data), value.capacity + 1);
    } else if (value.size < value.capacity)
        reallocate(value.size);
}

std::expected<void, StringError> String::assign(StringView text) noexcept {
    if (text.size() > max_size)
        fatal("String exceeds addressable storage.");
    if (!text.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    write_validated(text, 0);
    return {};
}

std::expected<void, StringError> String::append(StringView text) noexcept {
    if (text.size() > max_size - size())
        fatal("String exceeds addressable storage.");
    if (!text.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    write_validated(text, size());
    return {};
}

void String::append_ascii(char ascii, std::size_t count) noexcept {
    if ((static_cast<unsigned char>(ascii) & 0x80u) != 0)
        fatal("String::append_ascii requires a 7-bit ASCII byte.");
    if (count == 0)
        return;
    const auto current = size();
    if (count > max_size - current)
        fatal("String exceeds addressable storage.");
    const auto total = current + count;
    if (tag() == literal_tag && total <= inline_capacity) {
        char local[inline_capacity + 1];
        if (current)
            std::memcpy(local, c_str(), current);
        std::memset(local + current, static_cast<unsigned char>(ascii), count);
        local[total] = '\0';
        std::memcpy(bytes_, local, total + 1);
        bytes_[31] = static_cast<char>(total);
        return;
    }
    if (total > capacity() || tag() == literal_tag)
        reallocate(grown_capacity(total));
    char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
    std::memset(dst + current, static_cast<unsigned char>(ascii), count);
    dst[total] = '\0';
    if (is_inline()) {
        bytes_[31] = static_cast<char>(total);
    } else {
        auto val = long_value();
        val.size = total;
        set_long(val, heap_tag);
    }
}

void String::trim_start_in_place() noexcept {
    const auto cur = view();
    const auto trimmed = cur.trim_start();
    if (trimmed.size() != cur.size())
        write_validated(trimmed, 0);
}

void String::trim_end_in_place() noexcept {
    const auto cur = view();
    const auto trimmed = cur.trim_end();
    if (trimmed.size() == cur.size())
        return;
    if (tag() == literal_tag) {
        write_validated(trimmed, 0);
    } else if (is_inline()) {
        bytes_[trimmed.size()] = '\0';
        bytes_[31] = static_cast<char>(trimmed.size());
    } else {
        auto val = long_value();
        val.size = trimmed.size();
        const_cast<char*>(val.data)[val.size] = '\0';
        set_long(val, heap_tag);
    }
}

void String::trim_in_place() noexcept {
    const auto cur = view();
    const auto trimmed = cur.trim();
    if (trimmed.size() != cur.size())
        write_validated(trimmed, 0);
}

bool String::strip_prefix(StringView prefix) noexcept {
    const auto cur = view();
    if (!cur.starts_with(prefix) || !is_code_point_boundary(prefix.size()))
        return false;
    if (!prefix.empty())
        write_validated(cur.slice(prefix.size()), 0);
    return true;
}

bool String::strip_prefix(char prefix) noexcept {
    if ((static_cast<unsigned char>(prefix) & 0x80u) != 0)
        return false;
    const auto cur = view();
    if (!cur.starts_with(prefix))
        return false;
    write_validated(cur.slice(1), 0);
    return true;
}

bool String::strip_suffix(StringView suffix) noexcept {
    const auto cur = view();
    if (!cur.ends_with(suffix))
        return false;
    const auto kept = cur.size() - suffix.size();
    if (!is_code_point_boundary(kept))
        return false;
    if (suffix.empty())
        return true;
    if (tag() == literal_tag) {
        write_validated(cur.first(kept), 0);
    } else if (is_inline()) {
        bytes_[kept] = '\0';
        bytes_[31] = static_cast<char>(kept);
    } else {
        auto val = long_value();
        val.size = kept;
        const_cast<char*>(val.data)[kept] = '\0';
        set_long(val, heap_tag);
    }
    return true;
}

bool String::strip_suffix(char suffix) noexcept {
    if ((static_cast<unsigned char>(suffix) & 0x80u) != 0)
        return false;
    const auto ch = suffix;
    return strip_suffix(StringView{&ch, 1});
}

std::expected<void, StringError> String::slice_in_place(std::size_t first, std::size_t count) noexcept {
    const auto cur = view();
    if (first > cur.size() || (count != StringView::npos && count > cur.size() - first))
        fatal("String slice range.");
    const std::size_t actual = count == StringView::npos ? cur.size() - first : count;
    if (!is_code_point_boundary(first) || !is_code_point_boundary(first + actual))
        return std::unexpected(StringError::invalid_utf8);
    if (first != 0 || actual != cur.size())
        write_validated(cur.slice(first, actual), 0);
    return {};
}

std::expected<void, StringError> String::erase(std::size_t first, std::size_t count) noexcept {
    const auto cur = view();
    if (first > cur.size() || (count != StringView::npos && count > cur.size() - first))
        fatal("String erase range.");
    const std::size_t actual = count == StringView::npos ? cur.size() - first : count;
    if (!is_code_point_boundary(first) || !is_code_point_boundary(first + actual))
        return std::unexpected(StringError::invalid_utf8);
    if (actual == 0)
        return {};
    const std::size_t kept = cur.size() - actual;
    if (tag() == literal_tag) {
        String candidate;
        candidate.reserve(kept);
        candidate.write_validated(cur.first(first), 0);
        candidate.write_validated(cur.slice(first + actual), candidate.size());
        swap(candidate);
        return {};
    }
    char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
    const std::size_t tail = cur.size() - (first + actual);
    std::memmove(dst + first, dst + first + actual, tail + 1);
    if (is_inline()) {
        bytes_[31] = static_cast<char>(kept);
    } else {
        auto val = long_value();
        val.size = kept;
        set_long(val, heap_tag);
    }
    return {};
}

void String::insert_validated(std::size_t position, StringView text) noexcept {
    if (text.empty())
        return;
    const auto cur = view();
    if (text.size() > max_size - cur.size())
        fatal("String exceeds addressable storage.");
    const std::size_t total = cur.size() + text.size();
    const auto cur_begin = reinterpret_cast<std::uintptr_t>(cur.data());
    const auto txt_begin = reinterpret_cast<std::uintptr_t>(text.data());
    const bool aliased = txt_begin >= cur_begin && (txt_begin - cur_begin) <= cur.size();
    if (tag() != literal_tag && total <= capacity() && !aliased) {
        char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
        std::memmove(dst + position + text.size(), dst + position, cur.size() - position + 1);
        std::memcpy(dst + position, text.data(), text.size());
        if (is_inline()) {
            bytes_[31] = static_cast<char>(total);
        } else {
            auto val = long_value();
            val.size = total;
            set_long(val, heap_tag);
        }
        return;
    }
    String candidate;
    candidate.reserve(total);
    candidate.write_validated(cur.first(position), 0);
    candidate.write_validated(text, candidate.size());
    candidate.write_validated(cur.slice(position), candidate.size());
    swap(candidate);
}

std::expected<void, StringError> String::insert(std::size_t position, StringView text) noexcept {
    if (position > size())
        fatal("String insert position.");
    if (!is_code_point_boundary(position) || !text.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    insert_validated(position, text);
    return {};
}

std::expected<void, StringError> String::insert(std::size_t position, const String& text) noexcept {
    if (position > size())
        fatal("String insert position.");
    if (!is_code_point_boundary(position))
        return std::unexpected(StringError::invalid_utf8);
    insert_validated(position, text.view());
    return {};
}

std::expected<void, StringError> String::replace(std::size_t first, std::size_t count,
                                                 StringView replacement) noexcept {
    const auto text = view();
    if (first > text.size() || count > text.size() - first)
        fatal("String replacement range.");
    if (!is_code_point_boundary(first) || !is_code_point_boundary(first + count) || !replacement.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    if (count == 0 && replacement.empty())
        return {};
    const auto kept = text.size() - count;
    if (replacement.size() > max_size - kept)
        fatal("String exceeds addressable storage.");
    const std::size_t total = kept + replacement.size();
    const auto cur_begin = reinterpret_cast<std::uintptr_t>(text.data());
    const auto rep_begin = reinterpret_cast<std::uintptr_t>(replacement.data());
    const bool aliased = rep_begin >= cur_begin && (rep_begin - cur_begin) <= text.size();
    if (tag() != literal_tag && total <= capacity() && !aliased) {
        char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
        if (replacement.size() != count)
            std::memmove(dst + first + replacement.size(), dst + first + count, text.size() - (first + count) + 1);
        if (replacement.size())
            std::memcpy(dst + first, replacement.data(), replacement.size());
        dst[total] = '\0';
        if (is_inline()) {
            bytes_[31] = static_cast<char>(total);
        } else {
            auto val = long_value();
            val.size = total;
            set_long(val, heap_tag);
        }
        return {};
    }
    String candidate;
    candidate.reserve(total);
    candidate.write_validated(text.slice(0, first), 0);
    candidate.write_validated(replacement, candidate.size());
    candidate.write_validated(text.slice(first + count), candidate.size());
    swap(candidate);
    return {};
}

std::expected<std::size_t, StringError> String::replace_all(StringView search, StringView replacement) noexcept {
    if (!search.is_valid_utf8() || !replacement.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    if (search.empty())
        return 0;
    const auto text = view();
    auto at = text.find(search);
    if (at == StringView::npos)
        return 0;
    String candidate;
    if (replacement.size() >= search.size()) {
        const auto growth = replacement.size() - search.size();
        if (growth > max_size - text.size())
            fatal("String exceeds addressable storage.");
        candidate.reserve(text.size() + growth);
    }
    std::size_t first{};
    std::size_t replaced{};
    do {
        candidate.write_validated(text.slice(first, at - first), candidate.size());
        candidate.write_validated(replacement, candidate.size());
        first = at + search.size();
        ++replaced;
        at = text.find(search, first);
    } while (at != StringView::npos);
    candidate.write_validated(text.slice(first), candidate.size());
    swap(candidate);
    return replaced;
}

std::expected<void, StringError> String::replace_line_endings(StringView newline) noexcept {
    if (!newline.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    const auto cur = view();
    if (cur.empty())
        return {};
    if (newline == "\n") {
        const auto first_cr = cur.find('\r');
        if (first_cr == StringView::npos)
            return {};
        if (tag() == literal_tag)
            write_validated(cur, 0);
        char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
        const std::size_t length = cur.size();
        std::size_t write_pos = first_cr;
        for (std::size_t read_pos = first_cr; read_pos != length; ++read_pos) {
            if (dst[read_pos] == '\r') {
                dst[write_pos++] = '\n';
                if (read_pos + 1 < length && dst[read_pos + 1] == '\n')
                    ++read_pos;
            } else {
                dst[write_pos++] = dst[read_pos];
            }
        }
        dst[write_pos] = '\0';
        if (is_inline()) {
            bytes_[31] = static_cast<char>(write_pos);
        } else {
            auto val = long_value();
            val.size = write_pos;
            set_long(val, heap_tag);
        }
        return {};
    }
    const auto first_break = cur.find_first_of(line_break_set);
    if (first_break == StringView::npos)
        return {};
    std::size_t breaks = 0;
    std::size_t plain_bytes = first_break;
    for (std::size_t i = first_break; i != cur.size(); ++i) {
        if (cur[i] == '\r') {
            ++breaks;
            if (i + 1 < cur.size() && cur[i + 1] == '\n')
                ++i;
        } else if (cur[i] == '\n') {
            ++breaks;
        } else {
            ++plain_bytes;
        }
    }
    if (newline.size() != 0 && breaks > (max_size - plain_bytes) / newline.size())
        fatal("String exceeds addressable storage.");
    const std::size_t total = plain_bytes + breaks * newline.size();
    String candidate;
    candidate.reserve(total);
    std::size_t pos = 0;
    while (pos < cur.size()) {
        const auto at = cur.find_first_of(line_break_set, pos);
        if (at == StringView::npos) {
            candidate.write_validated(cur.slice(pos), candidate.size());
            break;
        }
        candidate.write_validated(cur.slice(pos, at - pos), candidate.size());
        candidate.write_validated(newline, candidate.size());
        pos = (cur[at] == '\r' && at + 1 < cur.size() && cur[at + 1] == '\n') ? at + 2 : at + 1;
    }
    swap(candidate);
    return {};
}

void String::pad_left(std::size_t total_bytes, char fill_ascii) noexcept {
    if ((static_cast<unsigned char>(fill_ascii) & 0x80u) != 0)
        fatal("String::pad_left requires a 7-bit ASCII byte.");
    const auto current = size();
    if (total_bytes <= current)
        return;
    if (total_bytes > max_size)
        fatal("String exceeds addressable storage.");
    const std::size_t pad = total_bytes - current;
    if (tag() == literal_tag && total_bytes <= inline_capacity) {
        char local[inline_capacity + 1];
        std::memset(local, static_cast<unsigned char>(fill_ascii), pad);
        if (current)
            std::memcpy(local + pad, c_str(), current);
        local[total_bytes] = '\0';
        std::memcpy(bytes_, local, total_bytes + 1);
        bytes_[31] = static_cast<char>(total_bytes);
        return;
    }
    if (total_bytes > capacity() || tag() == literal_tag)
        reallocate(total_bytes);
    char* dst = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
    if (current)
        std::memmove(dst + pad, dst, current);
    std::memset(dst, static_cast<unsigned char>(fill_ascii), pad);
    dst[total_bytes] = '\0';
    if (is_inline()) {
        bytes_[31] = static_cast<char>(total_bytes);
    } else {
        auto val = long_value();
        val.size = total_bytes;
        set_long(val, heap_tag);
    }
}

void String::pad_right(std::size_t total_bytes, char fill_ascii) noexcept {
    if ((static_cast<unsigned char>(fill_ascii) & 0x80u) != 0)
        fatal("String::pad_right requires a 7-bit ASCII byte.");
    const auto current = size();
    if (total_bytes <= current)
        return;
    append_ascii(fill_ascii, total_bytes - current);
}

void String::change_ascii_case(bool upper) noexcept {
    const auto convert = [upper](char byte) noexcept {
        return upper ? detail::ascii_upper(byte) : detail::ascii_lower(byte);
    };
    const auto length = size();
    const char* current = c_str();
    std::size_t first = 0;
    while (length - first >= 16) {
        const auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(current + first));
        const auto transformed = upper ? detail::simd_ascii_upper(chunk) : detail::simd_ascii_lower(chunk);
        const auto eq = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, transformed)));
        if (eq != 0xffffu) {
            first += static_cast<std::size_t>(std::countr_zero(~eq & 0xffffu));
            break;
        }
        first += 16;
    }
    while (first != length && convert(current[first]) == current[first])
        ++first;
    if (first == length)
        return;
    if (tag() == literal_tag)
        write_validated(view(), 0);
    auto* destination = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
    while (length - first >= 32) {
        const auto c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + first));
        const auto c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + first + 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + first),
                         upper ? detail::simd_ascii_upper(c0) : detail::simd_ascii_lower(c0));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + first + 16),
                         upper ? detail::simd_ascii_upper(c1) : detail::simd_ascii_lower(c1));
        first += 32;
    }
    if (length - first >= 16) {
        const auto c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination + first));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + first),
                         upper ? detail::simd_ascii_upper(c) : detail::simd_ascii_lower(c));
        first += 16;
    }
    for (; first != length; ++first)
        destination[first] = convert(destination[first]);
}

void String::to_ascii_lower() noexcept {
    change_ascii_case(false);
}

void String::to_ascii_upper() noexcept {
    change_ascii_case(true);
}

void String::write_validated(StringView text, std::size_t prefix) noexcept {
    if (text.size() > max_size - prefix)
        fatal("String exceeds addressable storage.");
    const auto total = prefix + text.size();
    if (tag() == literal_tag && total <= inline_capacity) {
        // Consume both borrowed ranges before overwriting the long descriptor.
        char replacement[inline_capacity + 1];
        if (prefix)
            std::memcpy(replacement, c_str(), prefix);
        if (text.size())
            std::memcpy(replacement + prefix, text.data(), text.size());
        replacement[total] = '\0';
        std::memcpy(bytes_, replacement, total + 1);
        bytes_[31] = static_cast<char>(total);
    } else if (tag() == heap_tag && total > long_value().capacity) {
        const auto value = long_value();
        const auto new_capacity = grown_capacity(total);
        const auto old_begin = reinterpret_cast<std::uintptr_t>(value.data);
        const auto txt_begin = reinterpret_cast<std::uintptr_t>(text.data());
        const bool aliased = text.size() != 0 && txt_begin >= old_begin && (txt_begin - old_begin) <= value.size;
        const std::size_t alias_offset = aliased ? static_cast<std::size_t>(txt_begin - old_begin) : 0;
        char* replacement = reallocate_heap(const_cast<char*>(value.data), new_capacity);
        if (text.size()) {
            if (aliased)
                std::memmove(replacement + prefix, replacement + alias_offset, text.size());
            else
                std::memcpy(replacement + prefix, text.data(), text.size());
        }
        replacement[total] = '\0';
        set_long({replacement, total, new_capacity}, heap_tag);
    } else if (total > capacity() || tag() == literal_tag) {
        const auto new_capacity = grown_capacity(total);
        char* replacement = allocate_heap(new_capacity);
        if (prefix)
            std::memcpy(replacement, c_str(), prefix);
        if (text.size())
            std::memcpy(replacement + prefix, text.data(), text.size());
        replacement[total] = '\0';
        set_long({replacement, total, new_capacity}, heap_tag);
    } else if (is_inline()) {
        if (text.size())
            std::memmove(bytes_ + prefix, text.data(), text.size());
        bytes_[total] = '\0';
        bytes_[31] = static_cast<char>(total);
    } else {
        auto value = long_value();
        auto* destination = const_cast<char*>(value.data);
        if (text.size())
            std::memmove(destination + prefix, text.data(), text.size());
        destination[total] = '\0';
        value.size = total;
        set_long(value, heap_tag);
    }
}

}

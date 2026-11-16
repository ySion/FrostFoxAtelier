#include "foundation/string.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace atelier {

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
        char* buffer = new char[text.size() + 1];
        std::memcpy(buffer, text.data(), text.size());
        buffer[text.size()] = '\0';
        result.set_long({buffer, text.size(), text.size()}, heap_tag);
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
        destination = new char[total + 1];
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
    ::operator delete[](const_cast<char*>(value.data), value.capacity + 1);
}

void String::copy_heap(const String& other) noexcept {
    const auto value = other.long_value();
    if (value.size <= inline_capacity) {
        std::memset(bytes_, 0, sizeof(bytes_));
        std::memcpy(bytes_, value.data, value.size + 1);
        bytes_[31] = static_cast<char>(value.size);
        return;
    }
    char* copy = new char[value.size + 1];
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

void String::reallocate(std::size_t capacity) noexcept {
    const auto length = size();
    char* replacement = new char[capacity + 1];
    std::memcpy(replacement, c_str(), length + 1);
    if (tag() == heap_tag)
        free_heap();
    set_long({replacement, length, capacity}, heap_tag);
}

void String::reserve(std::size_t bytes) noexcept {
    if (bytes > max_size)
        fatal("String exceeds addressable storage.");
    if (bytes > capacity())
        reallocate(bytes);
}

void String::squeeze() noexcept {
    if (tag() != heap_tag)
        return;
    const auto value = long_value();
    if (value.size <= inline_capacity) {
        std::memcpy(bytes_, value.data, value.size + 1);
        bytes_[31] = static_cast<char>(value.size);
        ::operator delete[](const_cast<char*>(value.data), value.capacity + 1);
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

std::expected<void, StringError> String::replace(std::size_t first, std::size_t count,
                                                 StringView replacement) noexcept {
    const auto text = view();
    if (first > text.size() || count > text.size() - first)
        fatal("String replacement range.");
    const auto boundary = [&](std::size_t offset) {
        return offset == text.size() || (static_cast<unsigned char>(text.data()[offset]) & 0xc0) != 0x80;
    };
    if (!boundary(first) || !boundary(first + count) || !replacement.is_valid_utf8())
        return std::unexpected(StringError::invalid_utf8);
    const auto kept = text.size() - count;
    if (replacement.size() > max_size - kept)
        fatal("String exceeds addressable storage.");
    String candidate;
    candidate.reserve(kept + replacement.size());
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
    if (replacement.size() <= search.size())
        candidate.reserve(text.size());
    else if (const auto delta = replacement.size() - search.size(); delta <= max_size - text.size())
        candidate.reserve(text.size() + delta);
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

void String::change_ascii_case(bool upper) noexcept {
    const auto convert = [upper](char byte) { return upper ? detail::ascii_upper(byte) : detail::ascii_lower(byte); };
    const auto length = size();
    const char* current = c_str();
    std::size_t first{};
    while (first != length && convert(current[first]) == current[first])
        ++first;
    if (first == length)
        return;
    if (tag() == literal_tag)
        write_validated(view(), 0);
    auto* destination = is_inline() ? bytes_ : const_cast<char*>(long_value().data);
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
        std::memcpy(replacement, c_str(), prefix);
        std::memcpy(replacement + prefix, text.data(), text.size());
        replacement[total] = '\0';
        std::memcpy(bytes_, replacement, total + 1);
        bytes_[31] = static_cast<char>(total);
    } else if (total > capacity() || tag() == literal_tag) {
        const auto capacity = grown_capacity(total);
        char* replacement = new char[capacity + 1];
        if (prefix)
            std::memcpy(replacement, c_str(), prefix);
        // Self-views must be consumed before releasing their old buffer.
        std::memcpy(replacement + prefix, text.data(), text.size());
        replacement[total] = '\0';
        if (tag() == heap_tag)
            free_heap();
        set_long({replacement, total, capacity}, heap_tag);
    } else if (is_inline()) {
        std::memmove(bytes_ + prefix, text.data(), text.size());
        bytes_[total] = '\0';
        bytes_[31] = static_cast<char>(total);
    } else {
        auto value = long_value();
        auto* destination = const_cast<char*>(value.data);
        std::memmove(destination + prefix, text.data(), text.size());
        destination[total] = '\0';
        value.size = total;
        set_long(value, heap_tag);
    }
}

}

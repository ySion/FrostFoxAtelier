#include "foundation/string.hpp"
#include "foundation/result.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace atelier {

std::expected<String, StringError> String::from_utf8(StringView text) noexcept {
    String result;
    ATELIER_TRY(result.assign(text));
    return result;
}

String::String(const String& other) noexcept {
    if (other.tag() == literal_tag)
        std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
    else
        write_validated(other.view(), 0);
}

String& String::operator=(const String& other) noexcept {
    if (this != &other) {
        if (other.tag() == literal_tag) {
            release();
            std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
        } else
            write_validated(other.view(), 0);
    }
    return *this;
}

String::String(String&& other) noexcept {
    move_from(other);
}

String& String::operator=(String&& other) noexcept {
    if (this != &other) {
        release();
        move_from(other);
    }
    return *this;
}

void String::move_from(String& other) noexcept {
    std::memcpy(bytes_, other.bytes_, sizeof(bytes_));
    other.bytes_[0] = '\0';
    other.bytes_[31] = 0;
}

void String::release() noexcept {
    if (tag() == heap_tag)
        delete[] long_value().data;
    bytes_[0] = '\0';
    bytes_[31] = 0;
}

void String::clear() noexcept {
    if (tag() != heap_tag) {
        release();
        return;
    }
    auto value = long_value();
    const_cast<char*>(value.data)[0] = '\0';
    value.size = 0;
    set_long(value, heap_tag);
}

void String::swap(String& other) noexcept {
    if (this == &other)
        return;
    String temporary{std::move(other)};
    other = std::move(*this);
    *this = std::move(temporary);
}

std::size_t String::grown_capacity(std::size_t minimum) const noexcept {
    const auto current = capacity();
    return std::max(minimum, current + std::min(max_size - current, std::max(current / 2, std::size_t{8})));
}

void String::reallocate(std::size_t capacity) noexcept {
    const auto length = size();
    char* replacement = new char[capacity + 1];
    std::memcpy(replacement, c_str(), length + 1);
    release();
    set_long({replacement, length, capacity}, heap_tag);
}

void String::reserve(std::size_t bytes) noexcept {
    if (bytes > max_size)
        fatal("String exceeds addressable storage.");
    if (bytes > capacity())
        reallocate(grown_capacity(bytes));
}

void String::squeeze() noexcept {
    if (tag() != heap_tag)
        return;
    const auto value = long_value();
    if (value.size <= inline_capacity) {
        std::memcpy(bytes_, value.data, value.size + 1);
        bytes_[31] = static_cast<char>(value.size);
        delete[] value.data;
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
        release();
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

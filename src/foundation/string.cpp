#include "foundation/string.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace atelier {

bool StringView::is_valid_utf8() const noexcept {
    for (std::size_t i = 0; i < size_;) {
        // Skip complete ASCII words without reading beyond the view. No CPU
        // dispatch or alignment requirement; memcpy lowers to unaligned loads.
        while (size_ - i >= sizeof(std::uint64_t)) {
            std::uint64_t bytes;
            std::memcpy(&bytes, data_ + i, sizeof(bytes));
            if (bytes & UINT64_C(0x8080808080808080))
                break;
            i += sizeof(bytes);
        }
        if (i == size_)
            return true;
        const auto first = static_cast<unsigned char>(data_[i++]);
        if (first < 0x80)
            continue;
        const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                               : first >= 0xe0 && first <= 0xef ? 2
                               : first >= 0xf0 && first <= 0xf4 ? 3
                                                                : 0;
        if (!count || count > size_ - i)
            return false;
        const auto second = static_cast<unsigned char>(data_[i]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) || (first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second >= 0x90))
            return false;
        for (unsigned j = 0; j != count; ++j)
            if ((static_cast<unsigned char>(data_[i++]) & 0xc0) != 0x80)
                return false;
    }
    return true;
}

String::String(const String& other) {
    write_validated(other.view(), 0);
}

String& String::operator=(const String& other) {
    if (this != &other)
        write_validated(other.view(), 0);
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
    size_ = std::exchange(other.size_, 0);
    if (other.heap_) {
        heap_ = std::exchange(other.heap_, nullptr);
        buffer_.capacity = other.buffer_.capacity;
    } else
        std::memcpy(buffer_.inline_bytes, other.buffer_.inline_bytes, size_ + 1);
    // Assignment to an array member starts its union lifetime, including after
    // moving a heap string. Empty/moved-from objects always use inline storage.
    other.buffer_.inline_bytes[0] = '\0';
}

void String::release() noexcept {
    delete[] heap_;
    heap_ = nullptr;
    size_ = 0;
    buffer_.inline_bytes[0] = '\0';
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

void String::reallocate(std::size_t capacity) {
    char* replacement = new char[capacity + 1];
    std::memcpy(replacement, c_str(), size_ + 1);
    delete[] heap_;
    heap_ = replacement;
    buffer_.capacity = capacity;
}

void String::reserve(std::size_t bytes) {
    if (bytes > max_size)
        throw std::length_error("String exceeds addressable storage.");
    if (bytes > capacity())
        reallocate(grown_capacity(bytes));
}

void String::squeeze() {
    if (!heap_)
        return;
    if (size_ <= inline_capacity) {
        char* previous = heap_;
        buffer_.inline_bytes[0] = '\0';
        std::memcpy(buffer_.inline_bytes, previous, size_ + 1);
        heap_ = nullptr;
        delete[] previous;
    } else if (size_ < buffer_.capacity)
        reallocate(size_);
}

void String::assign(StringView text) {
    if (text.size() > max_size)
        throw std::length_error("String exceeds addressable storage.");
    if (!text.is_valid_utf8())
        throw std::invalid_argument("String requires valid UTF-8.");
    write_validated(text, 0);
}

void String::append(StringView text) {
    if (text.size() > max_size - size_)
        throw std::length_error("String exceeds addressable storage.");
    if (!text.is_valid_utf8())
        throw std::invalid_argument("String requires valid UTF-8.");
    write_validated(text, size_);
}

void String::write_validated(StringView text, std::size_t prefix) {
    if (text.size() > max_size - prefix)
        throw std::length_error("String exceeds addressable storage.");
    const auto total = prefix + text.size();
    if (total > capacity()) {
        const auto capacity = grown_capacity(total);
        char* replacement = new char[capacity + 1];
        if (prefix)
            std::memcpy(replacement, c_str(), prefix);
        // Consume the original view before freeing or replacing its storage,
        // including a self-view across the inline/heap boundary.
        std::memcpy(replacement + prefix, text.data(), text.size());
        replacement[total] = '\0';
        delete[] heap_;
        heap_ = replacement;
        buffer_.capacity = capacity;
    } else {
        char* destination = mutable_data();
        if (!text.empty())
            std::memmove(destination + prefix, text.data(), text.size());
        destination[total] = '\0';
    }
    size_ = total;
}

}

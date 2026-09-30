#include "foundation/string.hpp"

#include <cstring>

namespace atelier {

bool StringView::is_valid_utf8() const noexcept {
    for (std::size_t i = 0; i < size_;) {
        const auto first = static_cast<unsigned char>(data_[i++]);
        if (first < 0x80) continue;
        const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!count || count > size_ - i) return false;
        const auto second = static_cast<unsigned char>(data_[i]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        for (unsigned j = 0; j != count; ++j)
            if ((static_cast<unsigned char>(data_[i++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

void String::reserve(std::size_t bytes) {
    if (bytes >= Array<char>::max_size) throw std::length_error("String exceeds addressable storage.");
    storage_.reserve(bytes + 1);
}

void String::append(StringView text) {
    if (!text.is_valid_utf8()) throw std::invalid_argument("String requires valid UTF-8.");
    if (text.empty()) return;
    const auto old_size = size();
    if (text.size() > Array<char>::max_size - old_size - 1)
        throw std::length_error("String exceeds addressable storage.");
    // A view into this string must survive reserve; preserve its byte offset.
    const auto address = reinterpret_cast<std::uintptr_t>(text.data());
    const auto start = reinterpret_cast<std::uintptr_t>(c_str());
    const bool alias = address >= start && address - start <= old_size && text.size() <= old_size + 1 - (address - start);
    const auto offset = alias ? address - start : 0;
    storage_.resize(old_size + text.size() + 1);
    const auto* source = alias ? storage_.data() + offset : text.data();
    std::memmove(storage_.data() + old_size, source, text.size());
    storage_[old_size + text.size()] = '\0';
}

}

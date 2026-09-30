#pragma once

#include "foundation/array.hpp"

#include <string_view>

namespace atelier {

class String;

// A byte range interpreted as UTF-8 by text consumers. A view may cover external,
// unvalidated bytes or a partial code point; it never promises a trailing zero.
class StringView {
public:
    static constexpr std::size_t npos = Span<const char>::npos;
    constexpr StringView() noexcept = default;
    constexpr StringView(const char* data, std::size_t size) : data_(data ? data : ""), size_(size) {
        if ((!data && size) || size > static_cast<std::size_t>(PTRDIFF_MAX))
            throw std::invalid_argument("Invalid StringView storage.");
    }
    constexpr StringView(const char* text) : StringView(text, text ? std::char_traits<char>::length(text) : 0) {}
    explicit constexpr StringView(std::string_view text) : StringView(text.data(), text.size()) {}
    StringView(const String&) noexcept;
    StringView(String&&) = delete;
    StringView(const String&&) = delete;

    [[nodiscard]] constexpr const char* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr const char* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr const char* end() const noexcept { return data_ + size_; }
    [[nodiscard]] constexpr char operator[](std::size_t index) const {
        if (index >= size_) throw std::out_of_range("StringView index.");
        return data_[index];
    }
    [[nodiscard]] constexpr StringView slice(std::size_t first, std::size_t count = npos) const {
        if (first > size_ || (count != npos && count > size_ - first)) throw std::out_of_range("StringView slice.");
        return {data_ + first, count == npos ? size_ - first : count};
    }
    [[nodiscard]] constexpr bool starts_with(StringView value) const noexcept { return std_view().starts_with(value.std_view()); }
    [[nodiscard]] constexpr bool ends_with(StringView value) const noexcept { return std_view().ends_with(value.std_view()); }
    [[nodiscard]] constexpr std::size_t find(StringView value, std::size_t from = 0) const noexcept { return std_view().find(value.std_view(), from); }
    [[nodiscard]] constexpr bool contains(StringView value) const noexcept { return find(value) != npos; }
    [[nodiscard]] constexpr std::string_view std_view() const noexcept { return {data_, size_}; }
    [[nodiscard]] bool is_valid_utf8() const noexcept;
    friend constexpr bool operator==(StringView left, StringView right) noexcept { return left.std_view() == right.std_view(); }
    friend constexpr auto operator<=>(StringView left, StringView right) noexcept { return left.std_view() <=> right.std_view(); }

private:
    const char* data_{""};
    std::size_t size_{};
};

// Owning, validated UTF-8 with a trailing zero. Embedded zeros remain part of
// size(). Length and all view positions are bytes, not glyphs or code points.
class String {
public:
    String() noexcept = default;
    explicit String(StringView text) { append(text); }
    String(const String&) = default;
    String& operator=(const String&) = default;
    String(String&&) noexcept = default;
    String& operator=(String&&) noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept { return storage_.empty() ? 0 : storage_.size() - 1; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.capacity() ? storage_.capacity() - 1 : 0; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const char* c_str() const noexcept { return storage_.empty() ? "" : storage_.data(); }
    [[nodiscard]] StringView view() const & noexcept { return {c_str(), size()}; }
    StringView view() const && = delete;
    [[nodiscard]] char operator[](std::size_t index) const { return StringView{c_str(), size()}[index]; }
    void reserve(std::size_t bytes);
    void squeeze() { storage_.squeeze(); }
    void clear() noexcept { storage_.clear(); }
    void release() noexcept { storage_.release(); }
    void swap(String& other) noexcept { storage_.swap(other.storage_); }
    void assign(StringView text) { String copy{text}; swap(copy); }
    void append(StringView text);
    [[nodiscard]] bool contains(StringView text) const noexcept { return view().contains(text); }
    friend bool operator==(const String& left, const String& right) noexcept { return left.view() == right.view(); }
    friend auto operator<=>(const String& left, const String& right) noexcept { return left.view() <=> right.view(); }

private:
    Array<char> storage_;
};

inline StringView::StringView(const String& text) noexcept : StringView(text.view()) {}

}

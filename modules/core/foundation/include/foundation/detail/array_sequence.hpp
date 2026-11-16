#pragma once

#include "foundation/span.hpp"
#include "foundation/detail/array_elements.hpp"

#include <concepts>
#include <functional>

namespace atelier::detail {

// Storage-independent operations on a contiguous live prefix. This base has no
// fields; storage owners provide capacity changes and retain their ABI.
template <class Owner, class T>
class ArraySequence {
    using Elements = ArrayElements<T>;

public:
    using Value = T;

    static constexpr std::size_t npos = Span<T>::npos;

    [[nodiscard]] bool empty() const noexcept { return owner().size() == 0; }

    [[nodiscard]] T* begin() noexcept [[clang::lifetimebound]] { return owner().data(); }

    [[nodiscard]] const T* begin() const noexcept [[clang::lifetimebound]] { return owner().data(); }

    [[nodiscard]] T* end() noexcept [[clang::lifetimebound]] { return begin() + owner().size(); }

    [[nodiscard]] const T* end() const noexcept [[clang::lifetimebound]] { return begin() + owner().size(); }

    [[nodiscard]] Span<T> view() & noexcept [[clang::lifetimebound]] { return {begin(), owner().size()}; }

    [[nodiscard]] Span<const T> view() const& noexcept [[clang::lifetimebound]] { return {begin(), owner().size()}; }

    Span<T> view() && = delete;

    Span<const T> view() const&& = delete;

    [[nodiscard]] Span<T> slice(std::size_t first, std::size_t count = npos) & noexcept [[clang::lifetimebound]] {
        return view().slice(first, count);
    }

    [[nodiscard]] Span<const T> slice(std::size_t first, std::size_t count = npos) const& noexcept
        [[clang::lifetimebound]] {
        return view().slice(first, count);
    }

    Span<T> slice(std::size_t first, std::size_t count = npos) && = delete;

    Span<const T> slice(std::size_t first, std::size_t count = npos) const&& = delete;

    [[nodiscard]] T& operator[](std::size_t index) noexcept [[clang::lifetimebound]] {
        check_range(index, 1);
        return begin()[index];
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept [[clang::lifetimebound]] {
        check_range(index, 1);
        return begin()[index];
    }

    [[nodiscard]] T* try_at(std::size_t index) noexcept [[clang::lifetimebound]] {
        return index < owner().size() ? begin() + index : nullptr;
    }

    [[nodiscard]] const T* try_at(std::size_t index) const noexcept [[clang::lifetimebound]] {
        return index < owner().size() ? begin() + index : nullptr;
    }

    [[nodiscard]] T& front() noexcept [[clang::lifetimebound]] { return (*this)[0]; }

    [[nodiscard]] const T& front() const noexcept [[clang::lifetimebound]] { return (*this)[0]; }

    [[nodiscard]] T& back() noexcept [[clang::lifetimebound]] { return (*this)[owner().size() - 1]; }

    [[nodiscard]] const T& back() const noexcept [[clang::lifetimebound]] { return (*this)[owner().size() - 1]; }

    void pop() noexcept {
        check_range(owner().size() - 1, 1);
        std::destroy_at(begin() + --owner().size_);
    }

    void clear() noexcept {
        Elements::destroy(begin(), owner().size());
        owner().size_ = 0;
    }

    template <class... Args>
        requires std::constructible_from<T, Args...>
    T& emplace(Args&&... arguments) noexcept [[clang::lifetimebound]] {
        auto& array = owner();
        if (array.size() < array.capacity()) {
            std::construct_at(end(), std::forward<Args>(arguments)...);
            return begin()[array.size_++];
        }
        if constexpr (Elements::byte_reallocate) {
            const auto capacity = array.grown_capacity(1);
            // Constructor arguments may name arbitrary subobjects of old values.
            T candidate(std::forward<Args>(arguments)...);
            array.reserve(capacity);
            std::construct_at(end(), std::move_if_noexcept(candidate));
            return begin()[array.size_++];
        }
        array.extend(
            1, [&](T* destination, std::size_t) { std::construct_at(destination, std::forward<Args>(arguments)...); });
        return begin()[array.size() - 1];
    }

    void append(Span<const T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        auto& array = owner();
        if constexpr (Elements::byte_reallocate && Elements::byte_copy) {
            if (values.empty())
                return;
            const auto capacity = array.grown_capacity(values.size());
            const auto offset = Elements::alias_offset(begin(), array.size(), values.data(), values.size());
            array.reserve(capacity);
            std::memcpy(end(), offset != SIZE_MAX ? begin() + offset : values.data(), values.size() * sizeof(T));
            array.size_ += values.size();
            return;
        }
        array.extend(values.size(), [&](T* destination, std::size_t count) {
            Elements::copy_construct(destination, values.data(), count);
        });
    }

    void resize(std::size_t count) noexcept
        requires std::default_initializable<T>
    {
        auto& array = owner();
        const auto previous = array.size();
        if (count <= previous) {
            if (count < previous) {
                Elements::destroy(begin() + count, previous - count);
                array.size_ = count;
            }
            return;
        }
        const auto added = count - previous;
        if constexpr (Elements::byte_reallocate && std::is_nothrow_default_constructible_v<T>) {
            if (count > array.capacity())
                array.reserve(array.grown_capacity(added));
            auto* first = begin() + previous;
            for (std::size_t index = 0; index != added; ++index)
                std::construct_at(first + index);
            array.size_ = count;
        } else {
            array.extend(added, [](T* destination, std::size_t total) {
                for (std::size_t index = 0; index != total; ++index)
                    std::construct_at(destination + index);
            });
        }
    }

    void resize(std::size_t count, const T& value) noexcept
        requires std::is_copy_constructible_v<T>
    {
        auto& array = owner();
        const auto previous = array.size();
        if (count <= previous) {
            if (count < previous) {
                Elements::destroy(begin() + count, previous - count);
                array.size_ = count;
            }
            return;
        }
        const auto added = count - previous;
        if constexpr (Elements::byte_reallocate && Elements::byte_copy) {
            const T saved(value);
            if (count > array.capacity())
                array.reserve(array.grown_capacity(added));
            auto* first = begin() + previous;
            for (std::size_t index = 0; index != added; ++index)
                std::construct_at(first + index, saved);
            array.size_ = count;
        } else {
            array.extend(added, [&](T* destination, std::size_t total) {
                for (std::size_t index = 0; index != total; ++index)
                    std::construct_at(destination + index, value);
            });
        }
    }

    T& add(const T& value) noexcept [[clang::lifetimebound]]
        requires std::is_copy_constructible_v<T>
    {
        return owner().emplace(value);
    }

    T& add(T&& value) noexcept [[clang::lifetimebound]]
        requires std::is_move_constructible_v<T>
    {
        return owner().emplace(std::move(value));
    }

    template <class... Args>
        requires std::constructible_from<T, Args...> && std::is_move_assignable_v<T>
    T& insert(std::size_t index, Args&&... arguments) noexcept [[clang::lifetimebound]] {
        check_range(index, 0);
        owner().emplace(std::forward<Args>(arguments)...);
        std::rotate(begin() + index, end() - 1, end());
        return begin()[index];
    }

    void move(std::size_t from, std::size_t to) noexcept
        requires std::is_move_assignable_v<T>
    {
        check_range(from, 1);
        check_range(to, 1);
        if (from < to)
            std::rotate(begin() + from, begin() + from + 1, begin() + to + 1);
        else if (to < from)
            std::rotate(begin() + to, begin() + from, begin() + from + 1);
    }

    void remove_at(std::size_t first, std::size_t count = 1) noexcept
        requires std::is_move_assignable_v<T>
    {
        check_range(first, count);
        if (!count)
            return;
        if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_move_assignable_v<T>) {
            const auto remaining = owner().size() - first - count;
            if (remaining)
                std::memmove(begin() + first, begin() + first + count, remaining * sizeof(T));
        } else {
            for (auto index = first; index != owner().size() - count; ++index)
                begin()[index] = std::move(begin()[index + count]);
        }
        Elements::destroy(end() - count, count);
        owner().size_ -= count;
    }

    void remove_at_swap(std::size_t index) noexcept
        requires std::is_move_assignable_v<T>
    {
        check_range(index, 1);
        if (index != owner().size() - 1)
            begin()[index] = std::move(begin()[owner().size() - 1]);
        std::destroy_at(begin() + --owner().size_);
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&> && std::is_move_assignable_v<T>
    std::size_t remove_if(Predicate&& predicate) noexcept {
        std::size_t write{};
        for (std::size_t read = 0; read != owner().size(); ++read) {
            if (std::invoke(predicate, std::as_const(begin()[read])))
                continue;
            if (write != read)
                begin()[write] = std::move(begin()[read]);
            ++write;
        }
        const auto removed = owner().size() - write;
        if (removed)
            Elements::destroy(begin() + write, removed);
        owner().size_ = write;
        return removed;
    }

    [[nodiscard]] std::size_t find(const T& value) const noexcept
        requires std::equality_comparable<T>
    {
        return find_if([&](const T& candidate) { return candidate == value; });
    }

    [[nodiscard]] bool contains(const T& value) const noexcept
        requires std::equality_comparable<T>
    {
        return find(value) != npos;
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] std::size_t find_if(Predicate&& predicate) const noexcept {
        for (std::size_t index = 0; index != owner().size(); ++index)
            if (std::invoke(predicate, std::as_const(begin()[index])))
                return index;
        return npos;
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] std::size_t find_last_if(Predicate&& predicate) const noexcept {
        for (auto index = owner().size(); index;)
            if (std::invoke(predicate, std::as_const(begin()[--index])))
                return index;
        return npos;
    }

    [[nodiscard]] std::size_t find_last(const T& value) const noexcept
        requires std::equality_comparable<T>
    {
        return find_last_if([&](const T& candidate) { return candidate == value; });
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] bool contains_if(Predicate&& predicate) const noexcept {
        return find_if(predicate) != npos;
    }

    // Maps a live element address, not an equal value, back to its index.
    // Null, foreign, misaligned and one-past addresses are absent.
    [[nodiscard]] std::size_t index_of(const T* element) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(element);
        const auto first = reinterpret_cast<std::uintptr_t>(begin());
        if (address < first || address - first >= owner().size() * sizeof(T) || (address - first) % sizeof(T))
            return npos;
        return (address - first) / sizeof(T);
    }

    template <class U>
        requires std::same_as<std::remove_cvref_t<U>, T> && std::constructible_from<T, U> && std::equality_comparable<T>
    std::size_t add_unique(U&& value) noexcept {
        const auto found = find(value);
        if (found != npos)
            return found;
        const auto index = owner().size();
        emplace(std::forward<U>(value));
        return index;
    }

    bool remove(const T& value) noexcept
        requires std::equality_comparable<T> && std::is_move_assignable_v<T>
    {
        return erase_match<false>(find(value));
    }

    bool remove_last(const T& value) noexcept
        requires std::equality_comparable<T> && std::is_move_assignable_v<T>
    {
        return erase_match<false>(find_last(value));
    }

    bool remove_swap(const T& value) noexcept
        requires std::equality_comparable<T> && std::is_move_assignable_v<T>
    {
        return erase_match<true>(find(value));
    }

    bool remove_last_swap(const T& value) noexcept
        requires std::equality_comparable<T> && std::is_move_assignable_v<T>
    {
        return erase_match<true>(find_last(value));
    }

    // Value is a snapshot: a key borrowed from this array must not change while
    // compaction overwrites its original slot. Copy/move cost is explicit.
    std::size_t remove_all(T value) noexcept
        requires std::equality_comparable<T> && std::is_move_assignable_v<T>
    {
        return remove_if([&](const T& candidate) { return candidate == value; });
    }

    std::size_t remove_all_swap(T value) noexcept
        requires std::equality_comparable<T> && std::is_move_assignable_v<T>
    {
        return remove_all_swap_if([&](const T& candidate) { return candidate == value; });
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&> && std::is_move_assignable_v<T>
    bool remove_first_if(Predicate&& predicate) noexcept {
        return erase_match<false>(find_if(predicate));
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&> && std::is_move_assignable_v<T>
    bool remove_last_if(Predicate&& predicate) noexcept {
        return erase_match<false>(find_last_if(predicate));
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&> && std::is_move_assignable_v<T>
    bool remove_swap_if(Predicate&& predicate) noexcept {
        return erase_match<true>(find_if(predicate));
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&> && std::is_move_assignable_v<T>
    std::size_t remove_all_swap_if(Predicate&& predicate) noexcept {
        const auto original = owner().size();
        std::size_t index{};
        while (index != owner().size()) {
            if (std::invoke(predicate, std::as_const(begin()[index])))
                remove_at_swap(index);
            else
                ++index;
        }
        return original - owner().size();
    }

protected:
    ArraySequence() = default;

    ~ArraySequence() = default;

private:
    template <bool Swap>
    bool erase_match(std::size_t index) noexcept {
        if (index == npos)
            return false;
        if constexpr (Swap)
            remove_at_swap(index);
        else
            remove_at(index);
        return true;
    }

    [[nodiscard]] Owner& owner() noexcept { return static_cast<Owner&>(*this); }

    [[nodiscard]] const Owner& owner() const noexcept { return static_cast<const Owner&>(*this); }

    void check_range(std::size_t first, std::size_t count) const noexcept {
        if (first > owner().size() || count > owner().size() - first)
            fatal("Array range.");
    }
};

}

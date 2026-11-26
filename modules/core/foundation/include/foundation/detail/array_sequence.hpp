#pragma once

#include "foundation/span.hpp"
#include "foundation/detail/array_elements.hpp"

#include <compare>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <ranges>

namespace atelier::detail {

struct ArraySequenceBase {};

template <class L, class R>
    requires std::derived_from<L, ArraySequenceBase> && std::derived_from<R, ArraySequenceBase> &&
             SpanEqualityComparable<typename L::Value, typename R::Value>
[[nodiscard]] bool operator==(const L& left, const R& right) noexcept {
    return left.view() == right.view();
}

template <class L, class R>
    requires std::derived_from<L, ArraySequenceBase> && std::derived_from<R, ArraySequenceBase> &&
             std::three_way_comparable_with<typename L::Value, typename R::Value>
[[nodiscard]] auto operator<=>(const L& left, const R& right) noexcept {
    return left.view() <=> right.view();
}

// Storage-independent operations on a contiguous live prefix. This base has no
// fields; storage owners provide capacity changes and retain their ABI.
template <class Owner, class T>
class ArraySequence : public ArraySequenceBase {
    using Elements = ArrayElements<T>;

public:
    using Value = T;

    using ReverseIterator = std::reverse_iterator<T*>;

    using ConstReverseIterator = std::reverse_iterator<const T*>;

    static constexpr std::size_t npos = Span<T>::npos;

    [[nodiscard]] bool empty() const noexcept { return owner().size() == 0; }

    [[nodiscard]] T* begin() noexcept [[clang::lifetimebound]] { return owner().data(); }

    [[nodiscard]] const T* begin() const noexcept [[clang::lifetimebound]] { return owner().data(); }

    [[nodiscard]] T* end() noexcept [[clang::lifetimebound]] { return begin() + owner().size(); }

    [[nodiscard]] const T* end() const noexcept [[clang::lifetimebound]] { return begin() + owner().size(); }

    [[nodiscard]] ReverseIterator rbegin() noexcept [[clang::lifetimebound]] { return ReverseIterator{end()}; }

    [[nodiscard]] ConstReverseIterator rbegin() const noexcept [[clang::lifetimebound]] {
        return ConstReverseIterator{end()};
    }

    [[nodiscard]] ReverseIterator rend() noexcept [[clang::lifetimebound]] { return ReverseIterator{begin()}; }

    [[nodiscard]] ConstReverseIterator rend() const noexcept [[clang::lifetimebound]] {
        return ConstReverseIterator{begin()};
    }

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
        Elements::destroy(begin() + --owner().size_, 1);
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
            const auto index = array.size();
            std::construct_at(raw_begin() + index, std::forward<Args>(arguments)...);
            array.size_ = index + 1;
            return begin()[index];
        }
        if constexpr (Elements::byte_reallocate) {
            const auto capacity = array.grown_capacity(1);
            // Constructor arguments may name arbitrary subobjects of old values.
            T candidate(std::forward<Args>(arguments)...);
            array.reserve(capacity);
            const auto index = array.size();
            std::construct_at(raw_begin() + index, std::move_if_noexcept(candidate));
            array.size_ = index + 1;
            return begin()[index];
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
            std::memcpy(raw_begin() + array.size(), offset != SIZE_MAX ? begin() + offset : values.data(),
                        values.size() * sizeof(T));
            array.size_ += values.size();
            return;
        }
        array.extend(values.size(), [&](T* destination, std::size_t count) {
            Elements::copy_construct(destination, values.data(), count);
        });
    }

    void append(std::initializer_list<T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        append(Span<const T>{values.begin(), values.size()});
    }

    template <std::ranges::input_range R>
        requires(!std::same_as<std::remove_cvref_t<R>, T>) &&
                (!std::is_convertible_v<R&&, Span<const T>> || move_elements_from_range_v<R>) &&
                std::constructible_from<T, RangeElementSource<R>>
    void append(R&& values) noexcept {
        auto& array = owner();
        if constexpr (std::same_as<std::remove_cvref_t<R>, Owner>) {
            if (std::addressof(values) == std::addressof(array)) {
                if constexpr (std::is_copy_constructible_v<T>) {
                    append(array.view());
                    return;
                } else {
                    fatal("Array self-move append.");
                }
            }
        }
        if constexpr (std::ranges::sized_range<R>) {
            const auto extra = static_cast<std::size_t>(std::ranges::size(values));
            array.extend(extra, [&](T* destination, std::size_t) {
                std::size_t index = 0;
                for (auto&& element : values) {
                    if constexpr (move_elements_from_range_v<R>)
                        std::construct_at(destination + index++, std::move(element));
                    else
                        std::construct_at(destination + index++, std::forward<decltype(element)>(element));
                }
            });
            if constexpr (move_elements_from_range_v<R> && requires { values.clear(); })
                values.clear();
        } else {
            Owner staged(std::forward<R>(values));
            array.extend(staged.size(), [&](T* destination, std::size_t count) {
                Elements::relocate(destination, staged.data(), count);
            });
            staged.clear();
        }
    }

    void assign(std::size_t count, const T& value) noexcept
        requires std::is_copy_constructible_v<T>
    {
        auto& array = owner();
        if constexpr (Elements::byte_copy && std::is_trivially_copy_assignable_v<T> && std::is_implicit_lifetime_v<T>) {
            const T saved(value);
            if (count > array.capacity()) {
                array.release();
                array.reserve(count);
            }
            Elements::fill_construct(raw_begin(), count, saved);
            array.size_ = count;
            return;
        }
        if (count == 0) {
            clear();
            return;
        }
        if (!empty()) {
            const T saved(value);
            assign_external_fill(count, saved);
        } else {
            assign_external_fill(count, value);
        }
    }

    void assign(Span<const T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        auto& array = owner();
        if constexpr (Elements::byte_copy && std::is_trivially_copy_assignable_v<T> && std::is_implicit_lifetime_v<T>) {
            if (values.size() <= array.capacity()) {
                if (!values.empty() && raw_begin() != values.data())
                    std::memmove(raw_begin(), values.data(), values.size_bytes());
                array.size_ = values.size();
                return;
            }
            array.release();
            array.reserve(values.size());
            std::memcpy(raw_begin(), values.data(), values.size_bytes());
            array.size_ = values.size();
            return;
        }
        const auto offset = Elements::alias_offset(begin(), array.size(), values.data(), values.size());
        if (offset != SIZE_MAX) {
            if (offset != 0) {
                if constexpr (std::is_move_assignable_v<T>) {
                    for (std::size_t index = 0; index != values.size(); ++index)
                        begin()[index] = std::move(begin()[offset + index]);
                } else {
                    Owner copy(values);
                    array.swap(copy);
                    return;
                }
            }
            Elements::destroy(begin() + values.size(), array.size() - values.size());
            array.size_ = values.size();
            return;
        }
        if (values.size() <= array.capacity()) {
            if constexpr (std::is_copy_assignable_v<T>) {
                const auto common = std::min(array.size(), values.size());
                for (std::size_t index = 0; index != common; ++index)
                    begin()[index] = values.data()[index];
                if (values.size() < array.size())
                    Elements::destroy(begin() + values.size(), array.size() - values.size());
                else if (values.size() > array.size())
                    Elements::copy_construct(raw_begin() + common, values.data() + common, values.size() - common);
                array.size_ = values.size();
            } else {
                clear();
                Elements::copy_construct(raw_begin(), values.data(), values.size());
                array.size_ = values.size();
            }
            return;
        }
        Owner replacement(values);
        array.swap(replacement);
    }

    void assign(std::initializer_list<T> values) noexcept
        requires std::is_copy_constructible_v<T>
    {
        assign(Span<const T>{values.begin(), values.size()});
    }

    template <std::input_iterator It, std::sentinel_for<It> Sentinel>
        requires std::constructible_from<T, std::iter_reference_t<It>>
    void assign(It first, Sentinel last) noexcept {
        if constexpr (std::contiguous_iterator<It> && std::sized_sentinel_for<Sentinel, It> &&
                      std::same_as<std::remove_cvref_t<std::iter_reference_t<It>>, T> &&
                      std::is_copy_constructible_v<T>) {
            assign(Span<const T>{first, last});
        } else if (empty()) {
            if constexpr (std::sized_sentinel_for<Sentinel, It>) {
                const auto distance = last - first;
                if (distance < 0)
                    fatal("Invalid Array range.");
                const auto count = static_cast<std::size_t>(distance);
                owner().reserve(count);
                for (std::size_t index = 0; index != count; ++index, ++first)
                    std::construct_at(raw_begin() + index, *first);
                owner().size_ = count;
            } else {
                for (; first != last; ++first)
                    emplace(*first);
            }
        } else {
            assign_from_staged(Owner(first, last));
        }
    }

    template <std::ranges::input_range R>
        requires(!std::same_as<std::remove_cvref_t<R>, T>) &&
                (!std::is_convertible_v<R&&, Span<const T>> || move_elements_from_range_v<R>) &&
                std::constructible_from<T, RangeElementSource<R>>
    void assign(R&& values) noexcept {
        auto& array = owner();
        if constexpr (std::same_as<std::remove_cvref_t<R>, Owner>) {
            if (std::addressof(values) == std::addressof(array))
                return;
        }
        if constexpr (std::ranges::view<std::remove_cvref_t<R>>) {
            if (!empty()) {
                assign_from_staged(Owner(std::forward<R>(values)));
                return;
            }
        }
        if constexpr (std::ranges::sized_range<R>) {
            const auto count = static_cast<std::size_t>(std::ranges::size(values));
            if (count > array.capacity()) {
                Owner replacement(std::forward<R>(values));
                array.swap(replacement);
                return;
            }
            if constexpr (std::is_assignable_v<T&, RangeElementSource<R>>) {
                const auto common = std::min(array.size(), count);
                auto it = std::ranges::begin(values);
                for (std::size_t index = 0; index != common; ++index, ++it) {
                    if constexpr (move_elements_from_range_v<R>)
                        begin()[index] = std::move(*it);
                    else
                        begin()[index] = *it;
                }
                if (count < array.size()) {
                    Elements::destroy(begin() + count, array.size() - count);
                } else {
                    for (std::size_t index = common; index != count; ++index, ++it) {
                        if constexpr (move_elements_from_range_v<R>)
                            std::construct_at(raw_begin() + index, std::move(*it));
                        else
                            std::construct_at(raw_begin() + index, *it);
                    }
                }
                array.size_ = count;
            } else {
                clear();
                std::size_t index = 0;
                for (auto&& element : values) {
                    if constexpr (move_elements_from_range_v<R>)
                        std::construct_at(raw_begin() + index++, std::move(element));
                    else
                        std::construct_at(raw_begin() + index++, std::forward<decltype(element)>(element));
                }
                array.size_ = count;
            }
        } else {
            clear();
            for (auto&& element : values) {
                if constexpr (move_elements_from_range_v<R>)
                    emplace(std::move(element));
                else
                    emplace(std::forward<decltype(element)>(element));
            }
        }
        if constexpr (move_elements_from_range_v<R> && requires { values.clear(); })
            values.clear();
    }

    void resize_uninitialized(std::size_t count) noexcept
        requires(std::is_implicit_lifetime_v<T> && std::is_trivially_destructible_v<T>)
    {
        auto& array = owner();
        if (count > array.capacity())
            array.reserve(array.grown_capacity(count - array.size()));
        array.size_ = count;
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
            Elements::default_construct(raw_begin() + previous, added);
            array.size_ = count;
        } else {
            array.extend(added, [](T* destination, std::size_t total) {
                Elements::default_construct(destination, total);
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
            Elements::fill_construct(raw_begin() + previous, added, saved);
            array.size_ = count;
        } else {
            array.extend(added, [&](T* destination, std::size_t total) {
                Elements::fill_construct(destination, total, value);
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
        Elements::destroy(begin() + --owner().size_, 1);
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

    template <class U = T>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] std::size_t find(const U& value, std::size_t from = 0) const noexcept {
        return view().find(value, from);
    }

    template <class U = T>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] bool contains(const U& value) const noexcept {
        return view().contains(value);
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] std::size_t find_if(Predicate&& predicate) const noexcept {
        return view().find_if(std::forward<Predicate>(predicate));
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] std::size_t find_last_if(Predicate&& predicate) const noexcept {
        return view().find_last_if(std::forward<Predicate>(predicate));
    }

    template <class U = T>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] std::size_t find_last(const U& value, std::size_t from = npos) const noexcept {
        return view().find_last(value, from);
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] bool contains_if(Predicate&& predicate) const noexcept {
        return view().contains_if(std::forward<Predicate>(predicate));
    }

    // Maps a live element address, not an equal value, back to its index.
    // Null, foreign, misaligned and one-past addresses are absent.
    [[nodiscard]] std::size_t index_of(const T* element) const noexcept { return view().index_of(element); }

    template <class U = T>
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

    template <class R>
        requires(!std::derived_from<std::remove_cvref_t<R>, ArraySequenceBase>) &&
                std::is_nothrow_convertible_v<const R&, Span<const T>> && SpanEqualityComparable<T, T>
    [[nodiscard]] friend bool operator==(const Owner& left, const R& right) noexcept {
        return left.view() == Span<const T>{right};
    }

    template <class R>
        requires(!std::derived_from<std::remove_cvref_t<R>, ArraySequenceBase>) &&
                std::is_nothrow_convertible_v<const R&, Span<const T>> && std::three_way_comparable<T>
    [[nodiscard]] friend auto operator<=>(const Owner& left, const R& right) noexcept {
        return left.view() <=> Span<const T>{right};
    }

protected:
    ArraySequence() = default;

    ~ArraySequence() = default;

private:
    [[nodiscard]] T* raw_begin() noexcept { return owner().raw_data(); }

    void assign_from_staged(Owner&& staged) noexcept {
        auto& array = owner();
        if (staged.size() > array.capacity()) {
            array.swap(staged);
            return;
        }
        const auto count = staged.size();
        if constexpr (Elements::byte_relocate && std::is_trivially_move_assignable_v<T> &&
                      std::is_implicit_lifetime_v<T>) {
            if (count)
                std::memcpy(raw_begin(), staged.data(), count * sizeof(T));
            array.size_ = count;
            staged.clear();
        } else if constexpr (std::is_move_assignable_v<T>) {
            const auto common = std::min(array.size(), count);
            for (std::size_t index = 0; index != common; ++index)
                begin()[index] = std::move(staged.begin()[index]);
            if (count < array.size())
                Elements::destroy(begin() + count, array.size() - count);
            else if (count > array.size())
                Elements::relocate(raw_begin() + common, staged.data() + common, count - common);
            array.size_ = count;
            staged.clear();
        } else {
            clear();
            Elements::relocate(raw_begin(), staged.data(), count);
            array.size_ = count;
            staged.clear();
        }
    }

    void assign_external_fill(std::size_t count, const T& value) noexcept {
        auto& array = owner();
        if (count <= array.capacity()) {
            if constexpr (std::is_copy_assignable_v<T>) {
                const auto common = std::min(array.size(), count);
                for (std::size_t index = 0; index != common; ++index)
                    begin()[index] = value;
                if (count < array.size())
                    Elements::destroy(begin() + count, array.size() - count);
                else if (count > array.size())
                    Elements::fill_construct(raw_begin() + common, count - common, value);
                array.size_ = count;
            } else {
                clear();
                Elements::fill_construct(raw_begin(), count, value);
                array.size_ = count;
            }
            return;
        }
        array.release();
        array.reserve(count);
        Elements::fill_construct(raw_begin(), count, value);
        array.size_ = count;
    }
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

#pragma once

#include "foundation/detail/array_elements.hpp"
#include "foundation/span.hpp"

#include <compare>
#include <concepts>
#include <iterator>

namespace atelier {

// One allocation, with the live sequence split at most once at the end of the buffer.
template <class T>
class RingDeque {
    using Elements = detail::ArrayElements<T>;

    static_assert(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);

    static_assert(std::is_nothrow_destructible_v<T>);

    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_copy_constructible_v<T>);

public:
    template <bool Const>
    class QueueIterator {
        using Pointer = std::conditional_t<Const, const T*, T*>;

        Pointer data_{};

        std::size_t capacity_{};

        std::size_t head_{};

        std::size_t index_{};

        constexpr QueueIterator(Pointer data, std::size_t capacity, std::size_t head, std::size_t index) noexcept
            : data_(data), capacity_(capacity), head_(head), index_(index) {}

        friend class RingDeque;

        template <bool>
        friend class QueueIterator;

    public:
        using value_type = T;

        using reference = std::conditional_t<Const, const T&, T&>;

        using pointer = Pointer;

        using difference_type = std::ptrdiff_t;

        using iterator_concept = std::random_access_iterator_tag;

        using iterator_category = std::random_access_iterator_tag;

        constexpr QueueIterator() noexcept = default;

        template <bool Other>
            requires(Const && !Other)
        constexpr QueueIterator(const QueueIterator<Other>& other) noexcept
            : data_(other.data_), capacity_(other.capacity_), head_(other.head_), index_(other.index_) {}

        [[nodiscard]] constexpr reference operator*() const noexcept {
            const auto until_end = capacity_ - head_;
            const auto slot = index_ < until_end ? head_ + index_ : index_ - until_end;
            return data_[slot];
        }

        [[nodiscard]] constexpr pointer operator->() const noexcept { return std::addressof(**this); }

        [[nodiscard]] constexpr reference operator[](difference_type offset) const noexcept {
            return *(*this + offset);
        }

        constexpr QueueIterator& operator++() noexcept {
            ++index_;
            return *this;
        }

        constexpr QueueIterator operator++(int) noexcept {
            auto previous = *this;
            ++index_;
            return previous;
        }

        constexpr QueueIterator& operator--() noexcept {
            --index_;
            return *this;
        }

        constexpr QueueIterator operator--(int) noexcept {
            auto previous = *this;
            --index_;
            return previous;
        }

        constexpr QueueIterator& operator+=(difference_type offset) noexcept {
            index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + offset);
            return *this;
        }

        constexpr QueueIterator& operator-=(difference_type offset) noexcept {
            index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) - offset);
            return *this;
        }

        [[nodiscard]] friend constexpr QueueIterator operator+(QueueIterator it, difference_type offset) noexcept {
            it += offset;
            return it;
        }

        [[nodiscard]] friend constexpr QueueIterator operator+(difference_type offset, QueueIterator it) noexcept {
            it += offset;
            return it;
        }

        [[nodiscard]] friend constexpr QueueIterator operator-(QueueIterator it, difference_type offset) noexcept {
            it -= offset;
            return it;
        }

        template <bool Other>
        [[nodiscard]] friend constexpr difference_type operator-(const QueueIterator& left,
                                                                 const QueueIterator<Other>& right) noexcept {
            return static_cast<difference_type>(left.index_) - static_cast<difference_type>(right.index_);
        }

        template <bool Other>
        [[nodiscard]] friend constexpr bool operator==(const QueueIterator& left,
                                                       const QueueIterator<Other>& right) noexcept {
            return left.data_ == right.data_ && left.index_ == right.index_;
        }

        template <bool Other>
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const QueueIterator& left,
                                                                        const QueueIterator<Other>& right) noexcept {
            return left.index_ <=> right.index_;
        }
    };

    using Iterator = QueueIterator<false>;

    using ConstIterator = QueueIterator<true>;

    using ReverseIterator = std::reverse_iterator<Iterator>;

    using ConstReverseIterator = std::reverse_iterator<ConstIterator>;

    RingDeque() noexcept = default;

    RingDeque(const RingDeque& other) noexcept
        requires std::is_copy_constructible_v<T>
        : data_(Elements::allocate(other.size_)), size_(other.size_), capacity_(other.size_) {
        const auto [first, second] = other.segments();
        Elements::copy_construct(data_, first.data(), first.size());
        Elements::copy_construct(data_ + first.size(), second.data(), second.size());
    }

    RingDeque(RingDeque&& other) noexcept { swap(other); }

    RingDeque& operator=(const RingDeque& other) noexcept
        requires std::is_copy_constructible_v<T>
    {
        if (this != &other) {
            RingDeque copy{other};
            swap(copy);
        }
        return *this;
    }

    RingDeque& operator=(RingDeque&& other) noexcept {
        if (this != &other) {
            RingDeque incoming{std::move(other)};
            swap(incoming);
        }
        return *this;
    }

    ~RingDeque() { release(); }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] T& operator[](std::size_t index) & noexcept [[clang::lifetimebound]] {
        return data_[checked_slot(index)];
    }

    [[nodiscard]] const T& operator[](std::size_t index) const& noexcept [[clang::lifetimebound]] {
        return data_[checked_slot(index)];
    }

    const T& operator[](std::size_t) const&& = delete;

    [[nodiscard]] T& front() & noexcept [[clang::lifetimebound]] { return (*this)[0]; }

    [[nodiscard]] const T& front() const& noexcept [[clang::lifetimebound]] { return (*this)[0]; }

    const T& front() const&& = delete;

    [[nodiscard]] T& back() & noexcept [[clang::lifetimebound]] { return (*this)[size_ - 1]; }

    [[nodiscard]] const T& back() const& noexcept [[clang::lifetimebound]] { return (*this)[size_ - 1]; }

    const T& back() const&& = delete;

    [[nodiscard]] std::pair<Span<T>, Span<T>> segments() & noexcept [[clang::lifetimebound]] {
        const auto first = first_size();
        return {{data_ + head_, first}, {data_, size_ - first}};
    }

    [[nodiscard]] std::pair<Span<const T>, Span<const T>> segments() const& noexcept [[clang::lifetimebound]] {
        const auto first = first_size();
        return {{data_ + head_, first}, {data_, size_ - first}};
    }

    void segments() const&& = delete;

    [[nodiscard]] Iterator begin() & noexcept [[clang::lifetimebound]] { return {data_, capacity_, head_, 0}; }

    [[nodiscard]] Iterator end() & noexcept [[clang::lifetimebound]] { return {data_, capacity_, head_, size_}; }

    [[nodiscard]] ConstIterator begin() const& noexcept [[clang::lifetimebound]] {
        return {data_, capacity_, head_, 0};
    }

    [[nodiscard]] ConstIterator end() const& noexcept [[clang::lifetimebound]] {
        return {data_, capacity_, head_, size_};
    }

    [[nodiscard]] ConstIterator cbegin() const& noexcept [[clang::lifetimebound]] { return begin(); }

    [[nodiscard]] ConstIterator cend() const& noexcept [[clang::lifetimebound]] { return end(); }

    [[nodiscard]] ReverseIterator rbegin() & noexcept [[clang::lifetimebound]] { return ReverseIterator{end()}; }

    [[nodiscard]] ReverseIterator rend() & noexcept [[clang::lifetimebound]] { return ReverseIterator{begin()}; }

    [[nodiscard]] ConstReverseIterator rbegin() const& noexcept [[clang::lifetimebound]] {
        return ConstReverseIterator{end()};
    }

    [[nodiscard]] ConstReverseIterator rend() const& noexcept [[clang::lifetimebound]] {
        return ConstReverseIterator{begin()};
    }

    [[nodiscard]] ConstReverseIterator crbegin() const& noexcept [[clang::lifetimebound]] { return rbegin(); }

    [[nodiscard]] ConstReverseIterator crend() const& noexcept [[clang::lifetimebound]] { return rend(); }

    ConstIterator begin() const&& = delete;

    ConstIterator end() const&& = delete;

    ConstIterator cbegin() const&& = delete;

    ConstIterator cend() const&& = delete;

    ConstReverseIterator rbegin() const&& = delete;

    ConstReverseIterator rend() const&& = delete;

    ConstReverseIterator crbegin() const&& = delete;

    ConstReverseIterator crend() const&& = delete;

    void reserve(std::size_t capacity) noexcept {
        if (capacity > capacity_)
            relocate_to(Elements::allocate(capacity), capacity, 0);
    }

    template <class... Args>
        requires std::constructible_from<T, Args...>
    T& emplace_back(Args&&... args) & noexcept [[clang::lifetimebound]] {
        return emplace_end<false>(std::forward<Args>(args)...);
    }

    template <class... Args>
        requires std::constructible_from<T, Args...>
    T& emplace_front(Args&&... args) & noexcept [[clang::lifetimebound]] {
        return emplace_end<true>(std::forward<Args>(args)...);
    }

    void pop_front() noexcept {
        std::destroy_at(std::addressof(front()));
        head_ = physical(1);
        --size_;
    }

    void pop_back() noexcept {
        std::destroy_at(std::addressof(back()));
        --size_;
    }

    void clear() noexcept {
        const auto [first, second] = segments();
        Elements::destroy(second.data(), second.size());
        Elements::destroy(first.data(), first.size());
        head_ = 0;
        size_ = 0;
    }

    void release() noexcept {
        clear();
        Elements::deallocate(data_);
        data_ = nullptr;
        capacity_ = 0;
    }

    void swap(RingDeque& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        std::swap(head_, other.head_);
    }

private:
    [[nodiscard]] std::size_t checked_slot(std::size_t index) const noexcept {
        if (index >= size_)
            fatal("RingDeque index out of range.");
        return physical(index);
    }

    [[nodiscard]] std::size_t physical(std::size_t index) const noexcept {
        const auto until_end = capacity_ - head_;
        return index < until_end ? head_ + index : index - until_end;
    }

    [[nodiscard]] std::size_t first_size() const noexcept { return std::min(size_, capacity_ - head_); }

    void relocate_to(T* replacement, std::size_t capacity, std::size_t offset) noexcept {
        const auto [first, second] = segments();
        Elements::relocate(replacement + offset, first.data(), first.size());
        Elements::relocate(replacement + offset + first.size(), second.data(), second.size());
        Elements::destroy(second.data(), second.size());
        Elements::destroy(first.data(), first.size());
        Elements::deallocate(data_);
        data_ = replacement;
        capacity_ = capacity;
        head_ = 0;
    }

    template <bool Front, class... Args>
    T& emplace_end(Args&&... args) noexcept {
        std::size_t slot;
        if (size_ == capacity_) {
            const auto capacity = Elements::grown_capacity(size_, capacity_, 1);
            T* replacement = Elements::allocate(capacity);
            slot = Front ? 0 : size_;
            // The arguments may borrow any part of a live element, even across wraparound.
            std::construct_at(replacement + slot, std::forward<Args>(args)...);
            relocate_to(replacement, capacity, Front ? 1 : 0);
        } else {
            slot = Front ? (head_ ? head_ - 1 : capacity_ - 1) : physical(size_);
            std::construct_at(data_ + slot, std::forward<Args>(args)...);
            if constexpr (Front)
                head_ = slot;
        }
        ++size_;
        return data_[slot];
    }

    T* data_{};

    std::size_t size_{};

    std::size_t capacity_{};

    std::size_t head_{};
};

}

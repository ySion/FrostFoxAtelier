#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>

namespace atelier::detail {

// Uninitialized call-scoped bytes. Layouts are validated by the caller; this
// owner neither constructs values nor depends on Runtime metadata. Preparing
// another layout invalidates previous contents and borrowed addresses.
class ValueStorage {
    alignas(double) std::byte inline_[128];
    struct Delete {
        std::size_t alignment{alignof(double)};
        void operator()(std::byte* storage) const noexcept {
            ::operator delete(storage, std::align_val_t{alignment});
        }
    };
    std::unique_ptr<std::byte, Delete> expanded_{nullptr, Delete{}};
    std::size_t capacity_{};
public:
    ValueStorage() noexcept = default;
    ValueStorage(const ValueStorage&) = delete;
    ValueStorage& operator=(const ValueStorage&) = delete;

    [[nodiscard]] std::byte* prepare(std::size_t size, std::size_t alignment) {
        if (size <= sizeof(inline_) && alignment <= alignof(double)) return inline_;
        if (size > capacity_ || alignment > expanded_.get_deleter().alignment) {
            const auto capacity = std::max(size, capacity_);
            const auto required_alignment = std::max(alignment, expanded_.get_deleter().alignment);
            auto* storage = static_cast<std::byte*>(::operator new(capacity, std::align_val_t{required_alignment}));
            expanded_.reset();
            expanded_.get_deleter().alignment = required_alignment;
            expanded_.reset(storage);
            capacity_ = capacity;
        }
        return expanded_.get();
    }
};

}

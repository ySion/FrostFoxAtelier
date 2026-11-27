#pragma once

#include "runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::material {

class Image;

using ImageRef = std::shared_ptr<const Image>;

// Immutable RGBA8 content. The material input determines its transfer function.
// After GPU/ATX staging, evict_cold_pixels() compresses RGBA8 losslessly in-memory
// and acquire_pixels() inflates a pinned shared_ptr buffer on cold CPU paths.
class Image final {
public:
    // Failure consumes the candidate pixels without publishing an image.
    [[nodiscard]] static Result<ImageRef> create(std::uint32_t width, std::uint32_t height,
                                                 std::vector<std::uint8_t> pixels);

    Image& operator=(const Image&) = delete;

    Image& operator=(Image&&) = delete;

    std::uint32_t width() const noexcept { return width_; }

    std::uint32_t height() const noexcept { return height_; }

    [[nodiscard]] std::size_t byte_size() const noexcept { return std::size_t(width_) * height_ * 4u; }

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> acquire_pixels() const;

    void evict_cold_pixels() const noexcept;

    [[nodiscard]] bool has_resident_pixels() const noexcept;

    [[nodiscard]] std::string_view rgba_sha256() const noexcept { return rgba_sha256_; }

    bool has_alpha() const noexcept { return has_alpha_; }

    bool is_grayscale() const noexcept { return is_grayscale_; }

private:
    Image(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels);

    std::uint32_t width_;

    std::uint32_t height_;

    std::string rgba_sha256_;

    mutable std::mutex mutex_;

    mutable std::shared_ptr<const std::vector<std::uint8_t>> pixels_;

    mutable std::weak_ptr<const std::vector<std::uint8_t>> cold_inflated_;

    mutable std::vector<std::uint8_t> compressed_;

    bool has_alpha_{};

    bool is_grayscale_{};
};

} // namespace atelier::material

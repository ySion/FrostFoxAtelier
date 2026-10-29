#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace atelier::material {

// Immutable RGBA8 content. The material input determines its transfer function.
class Image final {
public:
    Image(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {
        if (!width || !height || width > 8192 || height > 8192 ||
            std::uint64_t(width) * height * 4 != pixels_.size())
            throw std::invalid_argument("Invalid material image dimensions or RGBA storage.");
        for (std::size_t i = 3; i < pixels_.size(); i += 4)
            has_alpha_ |= pixels_[i] != 255;
    }
    Image& operator=(const Image&) = delete;
    Image& operator=(Image&&) = delete;
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::span<const std::uint8_t> pixels() const noexcept { return pixels_; }
    bool has_alpha() const noexcept { return has_alpha_; }
private:
    std::uint32_t width_, height_;
    std::vector<std::uint8_t> pixels_;
    bool has_alpha_{};
};
using ImageRef = std::shared_ptr<const Image>;

} // namespace atelier::material

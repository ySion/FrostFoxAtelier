#pragma once

#include "foundation/array.hpp"
#include "foundation/span.hpp"
#include "runtime/runtime.hpp"
#include <cstdint>
#include <memory>

namespace atelier::material {

class Image;

using ImageRef = std::shared_ptr<const Image>;

// Immutable RGBA8 content. The material input determines its transfer function.
class Image final {
public:
    // Failure consumes the candidate pixels without publishing an image.
    [[nodiscard]] static Result<ImageRef> create(std::uint32_t width, std::uint32_t height,
                                                 Array<std::uint8_t> pixels) {
        if (!width || !height || width > 8192 || height > 8192 || std::uint64_t(width) * height * 4 != pixels.size())
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Invalid material image dimensions or RGBA storage."});
        return ImageRef(new Image(width, height, std::move(pixels)));
    }

    Image& operator=(const Image&) = delete;

    Image& operator=(Image&&) = delete;

    std::uint32_t width() const noexcept { return width_; }

    std::uint32_t height() const noexcept { return height_; }

    Span<const std::uint8_t> pixels() const noexcept { return pixels_; }

    bool has_alpha() const noexcept { return has_alpha_; }

private:
    Image(std::uint32_t width, std::uint32_t height, Array<std::uint8_t> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {
        for (std::size_t i = 3; i < pixels_.size(); i += 4)
            has_alpha_ |= pixels_[i] != 255;
    }

    std::uint32_t width_;

    std::uint32_t height_;

    Array<std::uint8_t> pixels_;

    bool has_alpha_{};
};

} // namespace atelier::material

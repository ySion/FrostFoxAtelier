#pragma once
#include "ui/frame.hpp"
#include "ui_layout/layout.hpp"
#include <string_view>
#include <exception>

namespace atelier::ui::detail {
struct TextFailure final : std::exception {
    std::uint32_t code;
    explicit TextFailure(std::uint32_t value) : code(value) {}
    const char* what() const noexcept override { return "UI text/resource operation failed"; }
};
std::uint64_t new_identity();
struct RasterText { std::shared_ptr<const Coverage> coverage; float left{}, top{}, logical_height{}; };

// Private production text service: the same DWrite layout supplies metrics,
// caret positions and grayscale pixels. Frames retain only immutable pixels.
class TextStore {
public:
    TextStore();
    ~TextStore();
    std::uint64_t set(std::uint64_t key, std::wstring_view, float font_size, bool wrap);
    void erase(std::uint64_t key);
    RasterText raster(std::uint64_t key, float width, float dpi);
    float caret(std::uint64_t key, float width, std::uint32_t position);
    static ui_layout::MeasureResult measure(void*, const ui_layout::MeasureQuery&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atelier::ui::detail

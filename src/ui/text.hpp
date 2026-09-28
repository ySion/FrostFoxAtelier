#pragma once
#include "ui/frame.hpp"
#include "ui/style.hpp"
#include "ui/text_edit.hpp"
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
struct RasterText { std::shared_ptr<const Coverage> coverage; float left{}, top{}; };
struct TextStatistics {
    std::uint64_t measure_calls{}, text_layouts{}, rasterizations{}, raster_bytes{}, coverage_cache_hits{};
    std::uint64_t live_coverages{}, live_coverage_bytes{};
};

// Private production text service: the same DWrite layout supplies metrics,
// caret positions and grayscale pixels. Frames retain only immutable pixels.
class TextStore {
public:
    TextStore();
    ~TextStore();
    std::uint64_t set(std::uint64_t key, std::wstring_view, const Font&, float size, std::uint16_t weight, bool wrap);
    std::uint64_t provider_revision() const noexcept;
    void set_provider_revision(std::uint64_t) noexcept;
    TextStatistics statistics() const noexcept;
    void erase(std::uint64_t key);
    ui_layout::MeasuredSize metrics(std::uint64_t key, float width);
    RasterText raster(std::uint64_t key, float width, float dpi, std::optional<Clip> viewport = {});
    Rect caret(std::uint64_t key, float width, TextPosition position);
    Rect character(std::uint64_t key, float width, std::uint32_t position);
    TextPosition hit(std::uint64_t key, float width, float x, float y);
    TextPosition move(std::uint64_t key, float width, TextPosition position, int direction, bool edge);
    std::vector<Rect> selection(std::uint64_t key, float width, std::uint32_t first, std::uint32_t last);
    static ui_layout::MeasureResult measure(void*, const ui_layout::MeasureQuery&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atelier::ui::detail

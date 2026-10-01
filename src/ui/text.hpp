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

// Immutable paragraph geometry retained by accepted frames. Native types stay private.
class TextSnapshot {
public:
    bool matches(std::wstring_view) const noexcept;
    Rect caret(TextPosition) const;
    Rect character(std::uint32_t) const;
    TextPosition hit(float x, float y) const;
    TextPosition move(TextPosition, int direction, bool edge) const;
    std::vector<Rect> selection(std::uint32_t first, std::uint32_t last) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit TextSnapshot(std::shared_ptr<const Impl>);
    friend class TextStore;
};

// One DWrite layout supplies metrics, caret positions and grayscale pixels.
class TextStore {
public:
    TextStore();
    ~TextStore();
    std::uint64_t set(std::uint64_t key, std::wstring_view, const Font&, bool wrap);
    std::uint64_t provider_revision() const noexcept;
    void set_provider_revision(std::uint64_t) noexcept;
    TextStatistics statistics() const noexcept;
    void erase(std::uint64_t key);
    ui_layout::MeasuredSize metrics(std::uint64_t key, float width);
    RasterText raster(std::uint64_t key, float width, float dpi, std::optional<Clip> viewport = {});
    std::shared_ptr<const TextSnapshot> snapshot(std::uint64_t key, float width);
    // Keyboard edits may arrive before repaint. Shape the current draft using
    // the displayed paragraph's font/width policy, never the pending style.
    std::shared_ptr<const TextSnapshot> edited_snapshot(std::uint64_t key, const TextSnapshot&, std::wstring_view);
    static ui_layout::MeasureResult measure(void*, const ui_layout::MeasureQuery&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atelier::ui::detail

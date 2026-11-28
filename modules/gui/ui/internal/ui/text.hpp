#pragma once
#include "ui/frame.hpp"
#include "ui/style.hpp"
#include "ui/detail/text_edit.hpp"
#include "ui_layout/layout.hpp"
#include <string_view>
#include <compare>

namespace atelier::ui::detail {
std::uint64_t new_identity();

constexpr bool fits_coverage(std::int64_t width, std::int64_t height) noexcept {
    return width >= 0 && height >= 0 && width <= max_texture_dimension_px && height <= max_texture_dimension_px &&
           width * height <= 16 * 1024 * 1024;
}

struct RasterText {
    std::shared_ptr<const Coverage> coverage;

    float left{};

    float top{};
};

struct TextKey {
    std::uint64_t owner{};

    std::uint64_t key{};

    bool header{};

    TextKey(std::uint64_t value) : key(value) {}

    TextKey(std::uint64_t owner_value, std::uint64_t value, bool is_header)
        : owner(owner_value), key(value), header(is_header) {}

    auto operator<=>(const TextKey&) const noexcept = default;
};

struct TextStatistics {
    std::uint64_t measure_calls{};

    std::uint64_t text_layouts{};

    std::uint64_t rasterizations{};

    std::uint64_t raster_bytes{};

    std::uint64_t coverage_cache_hits{};

    std::uint64_t live_coverages{};

    std::uint64_t live_coverage_bytes{};
};

// Immutable paragraph geometry retained by accepted frames. Native types stay private.
class TextSnapshot {
public:
    bool matches(StringView) const noexcept;

    Result<Rect> caret(TextPosition) const;

    Result<Rect> character(std::uint32_t) const;

    Result<TextPosition> hit(float x, float y) const;

    Result<TextPosition> move(TextPosition, int direction, bool edge) const;

    Result<Array<Rect>> selection(std::uint32_t first, std::uint32_t last) const;

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

    // Stable-address storage; failure leaves only a destructible empty store.
    Result<void> initialize(Span<const std::wstring> font_files = {});

    std::uint64_t set(TextKey key, StringView, const Font&, bool wrap, TextAlign = TextAlign::left,
                      bool ellipsis = false);

    enum class Symbol {
        check,
        right,
        down,
        close,
        maximize,
        restore,
        left,
        up,
        plus,
        back,
        forward,
        world,
        disclosure_right,
        disclosure_down,
        object,
        component,
        mesh,
        folder,
        search,
        view_list,
        view_details,
        view_tiles,
        filter,
        more,
        radio,
        radio_checked,
        pointer,
        translate,
        rotate,
        scale,
        cursor3d,
        magnet,
        caret,
        transform,
        fly,
        point_light,
        area_light,
        sun_light,
        sky_light,
        shade_wire,
        shade_solid,
        shade_matcap,
        shade_lit,
        overlays,
        select_new,
        select_extend,
        select_subtract,
        select_invert,
        select_intersect,
        grid,
        axes_origin,
        wire_overlay,
        lights_off,
        fps,
        color_picker,
        rename,
        duplicate,
        trash,
        unparent,
        points,
        point_normals,
        face_normals,
        orient_scale,
        backface_tint,
        bounds_cage,
        info,
        sticky_note,
        network_box,
        palette,
        node_shapes,
        network_layout,
        pin,
        isolate,
        sliders,
        bypass_arrow,
        info_glyph,
        snowflake,
        eye,
        template_flag,
        eye_off,
        scene,
        material,
        texture,
        source_file,
        procgraph,
        physics,
        camera,
        copy,
        save,
        play,
        stop,
        build,
        reload,
        warning,
        lock,
        unlock
    };

    std::shared_ptr<const Coverage> symbol(Symbol, std::uint32_t pixels);

    std::uint64_t provider_revision() const noexcept;

    void set_provider_revision(std::uint64_t) noexcept;

    TextStatistics statistics() const noexcept;

    void erase(TextKey key);

    Result<ui_layout::MeasuredSize> metrics(TextKey key, float width);

    Result<RasterText> raster(TextKey key, float width, float dpi, Optional<Clip> viewport = {});

    Result<std::shared_ptr<const TextSnapshot>> snapshot(TextKey key, float width);

    // Keyboard edits may arrive before repaint. Shape the current draft using
    // the displayed paragraph's font/width policy, never the pending style.
    Result<std::shared_ptr<const TextSnapshot>> edited_snapshot(std::uint64_t key, const TextSnapshot&, StringView);

    static ui_layout::MeasureResult measure(void*, const ui_layout::MeasureQuery&);

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};
} // namespace atelier::ui::detail

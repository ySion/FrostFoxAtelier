#include "ui/text.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/result.hpp"
#include "memory/fatal.hpp"
#include "platform/utf.hpp"
#include "ui/icons.hpp"
#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace atelier::ui::detail {
using Microsoft::WRL::ComPtr;

namespace {
Result<void> check(HRESULT status) {
    if (FAILED(status))
        return std::unexpected(
            Error{ErrorCode::text_failed, 0, static_cast<std::uint32_t>(status), "UI text/resource operation failed"});
    return {};
}

std::atomic<std::uint64_t> next_identity{1};

struct CoverageAccounting {
    std::atomic<std::uint64_t> count{};

    std::atomic<std::uint64_t> bytes{};
};

struct TrackedCoverage final : Coverage {
    std::shared_ptr<CoverageAccounting> accounting;

    TrackedCoverage(std::shared_ptr<CoverageAccounting> owner, std::uint32_t w, std::uint32_t h)
        : accounting(std::move(owner)) {
        identity = new_identity();
        revision = 1;
        width = row_pitch = w;
        height = h;
        pixels.resize(static_cast<std::size_t>(w) * h);
        accounting->count.fetch_add(1, std::memory_order_relaxed);
        accounting->bytes.fetch_add(pixels.size(), std::memory_order_relaxed);
    }

    ~TrackedCoverage() {
        accounting->count.fetch_sub(1, std::memory_order_relaxed);
        accounting->bytes.fetch_sub(pixels.size(), std::memory_order_relaxed);
    }
};

struct FontLess {
    bool operator()(const Font& a, const Font& b) const {
        return std::tie(a.family, a.locale, a.size, a.weight, a.style) <
               std::tie(b.family, b.locale, b.size, b.weight, b.style);
    }
};

Clip visible_ink(Clip ink, const std::optional<Clip>& viewport) noexcept {
    if (!viewport)
        return ink;
    Clip clipped{std::max(ink.left, viewport->left), std::max(ink.top, viewport->top),
                 std::min(ink.right, viewport->right), std::min(ink.bottom, viewport->bottom)};
    return clipped.left < clipped.right && clipped.top < clipped.bottom ? clipped : Clip{};
}

struct Rasterizer final : IDWriteTextRenderer {
    struct Run {
        RECT bounds{};

        std::vector<std::uint8_t> pixels;
    };

    IDWriteFactory2* factory;

    float scale;

    std::optional<Clip> viewport;

    RECT ink_bounds{};

    ULONG refs{1};

    std::vector<Run> runs;

    HRESULT failure{S_OK};

    Rasterizer(IDWriteFactory2* value, float dpi, std::optional<Clip> clip)
        : factory(value), scale(dpi), viewport(clip) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) ||
            iid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs; }

    ULONG STDMETHODCALLTYPE Release() noexcept override { return --refs; } // Stack-owned for synchronous Draw.

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override {
        *disabled = TRUE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override {
        *transform = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) noexcept override {
        *value = scale;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
                                           const DWRITE_GLYPH_RUN* glyphs, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) noexcept override {
        if (FAILED(failure))
            return failure;
        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        const DWRITE_MATRIX transform{scale, 0, 0, scale, 0, 0};
        failure = factory->CreateGlyphRunAnalysis(glyphs, &transform, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, mode,
                                                  DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
                                                  x, y, &analysis);
        if (FAILED(failure))
            return failure;
        Run run;
        failure = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &run.bounds);
        if (FAILED(failure))
            return failure;
        // Keep the unclipped ink extent even for runs completely outside the current viewport.
        UnionRect(&ink_bounds, &ink_bounds, &run.bounds);
        if (viewport) {
            run.bounds.left = std::max<LONG>(run.bounds.left, viewport->left);
            run.bounds.top = std::max<LONG>(run.bounds.top, viewport->top);
            run.bounds.right = std::min<LONG>(run.bounds.right, viewport->right);
            run.bounds.bottom = std::min<LONG>(run.bounds.bottom, viewport->bottom);
        }
        const auto width = static_cast<std::int64_t>(run.bounds.right) - run.bounds.left;
        const auto height = static_cast<std::int64_t>(run.bounds.bottom) - run.bounds.top;
        if (width <= 0 || height <= 0)
            return S_OK;
        if (!fits_coverage(width, height))
            return failure = E_OUTOFMEMORY;
        run.pixels.resize(static_cast<std::size_t>(width * height));
        failure = analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &run.bounds, run.pixels.data(),
                                               static_cast<UINT32>(run.pixels.size()));
        if (FAILED(failure))
            return failure;
        runs.push_back(std::move(run));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*,
                                                IUnknown*) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object,
                                               BOOL sideways, BOOL right_to_left, IUnknown* effect) noexcept override {
        return object->Draw(context, this, x, y, sideways, right_to_left, effect);
    }
};
}

std::uint64_t new_identity() {
    auto value = next_identity.load(std::memory_order_relaxed);
    do {
        if (value == UINT64_MAX)
            fatal("UI text identity exhausted.");
    } while (!next_identity.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}

struct TextStore::Impl {
    struct Paragraph {
        ComPtr<IDWriteTextLayout> layout;

        std::shared_ptr<const TextSnapshot> snapshot;

        std::optional<float> width;

        ui_layout::MeasuredSize metrics;

        RasterText raster;

        float raster_scale{};

        Clip ink_bounds{};

        Clip raster_region{};

        bool valid{};
    };

    struct Entry {
        String text;

        InlineArray<wchar_t, 128> wide_text;

        Font font;

        bool wrap{};

        bool ellipsis{};

        TextAlign alignment{TextAlign::left};

        std::uint64_t revision{};

        std::array<Paragraph, 2> paragraphs;
    };

    ComPtr<IDWriteFactory2> factory;

    ComPtr<IDWriteFontCollection1> font_collection;

    std::map<TextKey, Entry> entries;

    std::map<Font, ComPtr<IDWriteTextFormat>, FontLess> formats;

    std::map<std::pair<TextStore::Symbol, std::uint32_t>, std::shared_ptr<const Coverage>> symbols;

    std::vector<DWRITE_LINE_METRICS> lines;

    std::uint64_t provider_revision{new_identity()};

    TextStatistics statistics;

    std::shared_ptr<CoverageAccounting> accounting{std::make_shared<CoverageAccounting>()};

    Result<void> initialize(std::span<const std::wstring> font_files) {
        ATELIER_TRY(check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
                                              reinterpret_cast<IUnknown**>(factory.GetAddressOf()))));
        if (!font_files.empty()) {
            ComPtr<IDWriteFactory5> loader;
            ATELIER_TRY(check(factory.As(&loader)));
            ComPtr<IDWriteFontSetBuilder1> builder;
            ATELIER_TRY(check(loader->CreateFontSetBuilder(&builder)));
            for (const auto& path : font_files) {
                ComPtr<IDWriteFontFile> file;
                ATELIER_TRY(check(loader->CreateFontFileReference(path.c_str(), nullptr, &file)));
                ATELIER_TRY(check(builder->AddFontFile(file.Get())));
            }
            ComPtr<IDWriteFontSet> system;
            ATELIER_TRY(check(loader->GetSystemFontSet(&system)));
            ATELIER_TRY(check(builder->AddFontSet(system.Get())));
            ComPtr<IDWriteFontSet> fonts;
            ATELIER_TRY(check(builder->CreateFontSet(&fonts)));
            ATELIER_TRY(check(loader->CreateFontCollectionFromFontSet(fonts.Get(), &font_collection)));
        }
        return {};
    }

    Result<Paragraph*> resolve(TextKey key, std::optional<float> width) {
        auto found = entries.find(key);
        if (found == entries.end())
            fatal("UI text key is missing.");
        auto& entry = found->second;
        auto& cached = entry.paragraphs[width ? 1 : 0];
        if (cached.layout && cached.valid && cached.width == width)
            return &cached;
        // Published snapshots can retain this native layout. Width changes create
        // a new layout instead of mutating geometry still used by an older frame.
        Paragraph paragraph;
        auto& format = formats[entry.font];
        if (!format) {
            const auto result = check(factory->CreateTextFormat(
                entry.font.family.c_str(), font_collection.Get(), static_cast<DWRITE_FONT_WEIGHT>(entry.font.weight),
                entry.font.style == FontStyle::italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, entry.font.size, entry.font.locale.c_str(), &format));
            ATELIER_TRY(result);
        }
        const wchar_t* wide_ptr = entry.wide_text.empty() ? L"" : entry.wide_text.data();
        ATELIER_TRY(check(factory->CreateTextLayout(wide_ptr, static_cast<UINT32>(entry.wide_text.size()), format.Get(),
                                                    width.value_or(0), 0, &paragraph.layout)));
        ++statistics.text_layouts;
        ATELIER_TRY(check(paragraph.layout->SetIncrementalTabStop(entry.font.size * 2.4f)));
        ATELIER_TRY(check(paragraph.layout->SetWordWrapping(width && entry.wrap ? DWRITE_WORD_WRAPPING_WRAP
                                                                                : DWRITE_WORD_WRAPPING_NO_WRAP)));
        if (width)
            ATELIER_TRY(check(paragraph.layout->SetTextAlignment(
                entry.alignment == TextAlign::right    ? DWRITE_TEXT_ALIGNMENT_TRAILING
                : entry.alignment == TextAlign::center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                       : DWRITE_TEXT_ALIGNMENT_LEADING)));
        DWRITE_TEXT_METRICS metrics{};
        ATELIER_TRY(check(paragraph.layout->GetMetrics(&metrics)));
        UINT32 count{};
        const auto status = paragraph.layout->GetLineMetrics(nullptr, 0, &count);
        if (status != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
            ATELIER_TRY(check(status));
        lines.resize(count);
        ATELIER_TRY(check(paragraph.layout->GetLineMetrics(lines.data(), count, &count)));
        paragraph.metrics = {metrics.widthIncludingTrailingWhitespace, metrics.height, count ? lines[0].baseline : 0};
        if (width && entry.ellipsis) {
            // Layout measures the complete line. Trimming is paint only; feeding
            // trimmed metrics back into intrinsic sizing progressively shortens labels.
            if (metrics.widthIncludingTrailingWhitespace <= *width + .01f)
                ATELIER_TRY(
                    check(paragraph.layout->SetMaxWidth(std::max(*width, metrics.widthIncludingTrailingWhitespace))));
            else {
                ComPtr<IDWriteInlineObject> sign;
                ATELIER_TRY(check(factory->CreateEllipsisTrimmingSign(format.Get(), &sign)));
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ATELIER_TRY(check(paragraph.layout->SetMaxHeight(32768.f)));
                ATELIER_TRY(check(paragraph.layout->SetTrimming(&trimming, sign.Get())));
            }
        }
        paragraph.width = width;
        paragraph.valid = true;
        cached = std::move(paragraph);
        return &cached;
    }
};

struct TextSnapshot::Impl {
    struct Stop {
        TextPosition position;

        float x;
    };

    ComPtr<IDWriteTextLayout> layout;

    String text;

    Font font;

    TextAlign alignment{TextAlign::left};

    float width{};

    mutable Array<Stop> stops; // Owner-thread query cache; paragraph geometry stays immutable.

    Result<Rect> caret_box(TextPosition position) const {
        FLOAT x{};
        FLOAT y{};
        DWRITE_HIT_TEST_METRICS metrics{};
        const std::size_t clamped_u8 = std::min<std::size_t>(position.offset, text.size());
        const bool trailing = position.trailing && clamped_u8 != 0;
        UINT32 dwrite_pos = 0;
        if (trailing) {
            std::size_t prev_u8 = clamped_u8 - 1;
            while (prev_u8 > 0 && (static_cast<unsigned char>(text[prev_u8]) & 0xc0u) == 0x80u)
                --prev_u8;
            dwrite_pos = static_cast<UINT32>(platform::utf8_offset_to_utf16(text.view(), prev_u8));
        } else {
            dwrite_pos = static_cast<UINT32>(platform::utf8_offset_to_utf16(text.view(), clamped_u8));
        }
        ATELIER_TRY(check(layout->HitTestTextPosition(dwrite_pos, trailing ? TRUE : FALSE, &x, &y, &metrics)));
        return Rect{x, y, 1, metrics.height};
    }

    Result<void> resolve_stops() const {
        if (!stops.empty())
            return {};
        ATELIER_TRY_BIND(boundaries, character_boundaries(text.view()));
        Array<Stop> candidate;
        candidate.reserve(boundaries->size() * 2);
        for (auto offset : *boundaries) {
            const TextPosition leading{offset, false};
            const TextPosition trailing{offset, offset != 0};
            ATELIER_TRY_BIND(first, caret_box(leading));
            ATELIER_TRY_BIND(last, caret_box(trailing));
            candidate.add({leading, first->x});
            if (std::abs(first->x - last->x) > .01f)
                candidate.add({trailing, last->x});
        }
        std::stable_sort(candidate.begin(), candidate.end(), [](auto a, auto b) { return a.x < b.x; });
        stops = std::move(candidate);
        return {};
    }
};

TextSnapshot::TextSnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

bool TextSnapshot::matches(StringView text) const noexcept {
    return impl_->text == text;
}

TextStore::TextStore() = default;

TextStore::~TextStore() = default;

Result<void> TextStore::initialize(std::span<const std::wstring> files) {
    auto candidate = std::make_unique<Impl>();
    ATELIER_TRY(candidate->initialize(files));
    impl_ = std::move(candidate);
    return {};
}

std::uint64_t TextStore::set(TextKey key, StringView text, const Font& font, bool wrap, TextAlign alignment,
                             bool ellipsis) {
    auto& entry = impl_->entries[key];
    const bool text_changed = entry.text != text;
    if (entry.revision == 0 || text_changed || entry.font != font || entry.wrap != wrap ||
        entry.alignment != alignment || entry.ellipsis != ellipsis) {
        if (text_changed) {
            entry.text = String::from_utf8_validated(text);
            const auto widened = platform::utf8_to_utf16_into(entry.text.view(), entry.wide_text);
            if (!widened)
                fatal("UI TextStore received invalid UTF-8 text.");
        }
        entry.font = font;
        entry.wrap = wrap;
        entry.alignment = alignment;
        entry.ellipsis = ellipsis;
        entry.revision = new_identity();
        entry.paragraphs = {};
    }
    return entry.revision;
}

void TextStore::erase(TextKey key) {
    impl_->entries.erase(key);
}

std::shared_ptr<const Coverage> TextStore::symbol(Symbol symbol, std::uint32_t pixels) {
    if (!pixels || pixels > 256)
        fatal("UI symbol pixels must be in 1..256.");
    const auto key = std::pair{symbol, pixels};
    if (const auto found = impl_->symbols.find(key); found != impl_->symbols.end()) {
        ++impl_->statistics.coverage_cache_hits;
        return found->second;
    }
    auto coverage = std::make_shared<TrackedCoverage>(impl_->accounting, pixels, pixels);
    const auto bundled = [symbol]() -> std::optional<Icon> {
        switch (symbol) {
        case Symbol::pointer:
            return Icon::pointer;
        case Symbol::translate:
            return Icon::translate;
        case Symbol::rotate:
            return Icon::rotate;
        case Symbol::scale:
            return Icon::scale;
        case Symbol::transform:
            return Icon::transform;
        case Symbol::cursor3d:
            return Icon::cursor3d;
        case Symbol::magnet:
            return Icon::magnet;
        case Symbol::fly:
            return Icon::fly;
        // Object-type glyphs come from the same Tabler set as the tools.
        case Symbol::point_light:
            return Icon::point_light;
        case Symbol::area_light:
            return Icon::area_light;
        case Symbol::sun_light:
            return Icon::sun_light;
        case Symbol::sky_light:
            return Icon::sky_light;
        case Symbol::shade_wire:
            return Icon::shade_wire;
        case Symbol::shade_solid:
            return Icon::shade_solid;
        case Symbol::shade_matcap:
            return Icon::shade_matcap;
        case Symbol::shade_lit:
            return Icon::shade_lit;
        case Symbol::overlays:
            return Icon::overlays;
        case Symbol::select_new:
            return Icon::select_new;
        case Symbol::select_extend:
            return Icon::select_extend;
        case Symbol::select_subtract:
            return Icon::select_subtract;
        case Symbol::select_invert:
            return Icon::select_invert;
        case Symbol::select_intersect:
            return Icon::select_intersect;
        case Symbol::grid:
            return Icon::grid;
        case Symbol::axes_origin:
            return Icon::axes_origin;
        case Symbol::wire_overlay:
            return Icon::wire_overlay;
        case Symbol::lights_off:
            return Icon::lights_off;
        case Symbol::fps:
            return Icon::fps;
        case Symbol::color_picker:
            return Icon::color_picker;
        case Symbol::rename:
            return Icon::rename;
        case Symbol::duplicate:
            return Icon::duplicate;
        case Symbol::trash:
            return Icon::trash;
        case Symbol::unparent:
            return Icon::unparent;
        case Symbol::mesh:
            return Icon::mesh;
        case Symbol::object:
            return Icon::object;
        case Symbol::component:
            return Icon::component;
        case Symbol::world:
            return Icon::world;
        case Symbol::folder:
            return Icon::folder;
        case Symbol::search:
            return Icon::search;
        default:
            return std::nullopt;
        }
    }();
    if (bundled)
        raster_icon(*bundled, pixels, coverage->pixels);
    else {
        const auto segment = [symbol](float x, float y, float ax, float ay, float bx, float by) {
            const auto dx = bx - ax;
            const auto dy = by - ay;
            const auto t = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
            const auto px = x - ax - t * dx;
            const auto py = y - ay - t * dy;
            return px * px + py * py <= ((symbol == Symbol::world || symbol == Symbol::mesh ||
                                          symbol == Symbol::folder || symbol >= Symbol::search)
                                             ? .0016f
                                             : .0064f);
        };
        for (std::uint32_t y = 0; y < pixels; ++y)
            for (std::uint32_t x = 0; x < pixels; ++x) {
                unsigned inside{};
                for (unsigned sy = 0; sy < 4; ++sy)
                    for (unsigned sx = 0; sx < 4; ++sx) {
                        const auto px =
                            (static_cast<float>(x) + (static_cast<float>(sx) + .5f) / 4) / static_cast<float>(pixels);
                        const auto py =
                            (static_cast<float>(y) + (static_cast<float>(sy) + .5f) / 4) / static_cast<float>(pixels);
                        const auto outline = [&](float left, float top, float right, float bottom) {
                            return px >= left && px <= right && py >= top && py <= bottom &&
                                   (px < left + .1f || px > right - .1f || py < top + .1f || py > bottom - .1f);
                        };
                        const auto ax = symbol == Symbol::forward ? 1 - px : px;
                        const bool hit =
                            (symbol == Symbol::back || symbol == Symbol::forward)
                                ? ((ax >= .08f && ax <= .53f && std::abs(py - .5f) < (ax - .08f) * .85f) ||
                                   (ax >= .5f && ax <= .92f && py >= .37f && py <= .63f))
                            : symbol == Symbol::world
                                ? (std::abs(std::hypot(px - .5f, py - .5f) - .36f) < .035f ||
                                   (std::abs(py - .5f) < .025f && std::abs(px - .5f) < .36f) ||
                                   (std::abs(std::hypot((px - .5f) * 2, py - .5f) - .36f) < .035f))
                            : symbol == Symbol::folder
                                ? (segment(px, py, .12f, .28f, .40f, .28f) || segment(px, py, .40f, .28f, .5f, .38f) ||
                                   segment(px, py, .5f, .38f, .88f, .38f) || segment(px, py, .88f, .38f, .88f, .80f) ||
                                   segment(px, py, .88f, .80f, .12f, .80f) || segment(px, py, .12f, .80f, .12f, .28f))
                            : symbol == Symbol::mesh
                                ? (segment(px, py, .16f, .30f, .50f, .12f) || segment(px, py, .50f, .12f, .84f, .30f) ||
                                   segment(px, py, .16f, .30f, .50f, .49f) || segment(px, py, .50f, .49f, .84f, .30f) ||
                                   segment(px, py, .16f, .30f, .16f, .70f) || segment(px, py, .84f, .30f, .84f, .70f) ||
                                   segment(px, py, .50f, .49f, .50f, .89f) || segment(px, py, .16f, .70f, .50f, .89f) ||
                                   segment(px, py, .50f, .89f, .84f, .70f))
                            : symbol == Symbol::search ? (std::abs(std::hypot(px - .4f, py - .4f) - .25f) < .04f ||
                                                          segment(px, py, .59f, .59f, .86f, .86f))
                            : symbol == Symbol::view_list
                                ? (segment(px, py, .33f, .25f, .86f, .25f) || segment(px, py, .33f, .5f, .86f, .5f) ||
                                   segment(px, py, .33f, .75f, .86f, .75f) ||
                                   (px >= .12f && px <= .22f &&
                                    (std::abs(py - .25f) < .05f || std::abs(py - .5f) < .05f ||
                                     std::abs(py - .75f) < .05f)))
                            : symbol == Symbol::view_details
                                ? (outline(.12f, .16f, .88f, .84f) || segment(px, py, .12f, .39f, .88f, .39f) ||
                                   segment(px, py, .12f, .62f, .88f, .62f) || segment(px, py, .63f, .16f, .63f, .84f))
                            : symbol == Symbol::view_tiles
                                ? (outline(.12f, .12f, .43f, .43f) || outline(.57f, .12f, .88f, .43f) ||
                                   outline(.12f, .57f, .43f, .88f) || outline(.57f, .57f, .88f, .88f))
                            : symbol == Symbol::filter
                                ? (segment(px, py, .13f, .2f, .87f, .2f) || segment(px, py, .13f, .2f, .43f, .55f) ||
                                   segment(px, py, .87f, .2f, .57f, .55f) || segment(px, py, .43f, .55f, .43f, .83f) ||
                                   segment(px, py, .43f, .83f, .57f, .73f) || segment(px, py, .57f, .73f, .57f, .55f))
                            : symbol == Symbol::more
                                ? (std::hypot(px - .2f, py - .5f) < .075f || std::hypot(px - .5f, py - .5f) < .075f ||
                                   std::hypot(px - .8f, py - .5f) < .075f)
                            : symbol == Symbol::caret
                                ? (py >= .42f && py <= .68f && std::abs(px - .5f) <= (.68f - py) * 1.15f)
                            : (symbol == Symbol::radio || symbol == Symbol::radio_checked)
                                ? (std::abs(std::hypot(px - .5f, py - .5f) - .33f) < .04f ||
                                   (symbol == Symbol::radio_checked && std::hypot(px - .5f, py - .5f) < .15f))
                            : symbol == Symbol::object
                                ? ((px >= .18f && px <= .82f && py >= .28f && py <= .75f) ||
                                   (py >= .12f && py < .28f && std::abs(px - .5f) < (py - .08f) * 2))
                            : symbol == Symbol::component ? ((px - .5f) * (px - .5f) + (py - .5f) * (py - .5f) < .16f &&
                                                             (px - .5f) * (px - .5f) + (py - .5f) * (py - .5f) > .06f)
                            : symbol == Symbol::close
                                ? (segment(px, py, .2f, .2f, .8f, .8f) || segment(px, py, .8f, .2f, .2f, .8f))
                            : symbol == Symbol::plus
                                ? (segment(px, py, .2f, .5f, .8f, .5f) || segment(px, py, .5f, .2f, .5f, .8f))
                            : symbol == Symbol::maximize
                                ? (px >= .15f && px <= .85f && py >= .225f && py <= .775f && px + py < 1)
                            : symbol == Symbol::restore ? (outline(.1f, .3f, .7f, .9f) ||
                                                           ((py < .3f || px > .7f) && outline(.3f, .1f, .9f, .7f)))
                            : symbol == Symbol::check
                                ? (segment(px, py, .17f, .53f, .40f, .77f) || segment(px, py, .40f, .77f, .85f, .23f))
                            : symbol == Symbol::disclosure_right
                                ? (px >= .2f && px <= .8f && std::abs(py - .5f) < (.8f - px) * .75f)
                            : symbol == Symbol::disclosure_down
                                ? (py >= .2f && py <= .8f && std::abs(px - .5f) < (.8f - py) * .75f)
                            : symbol == Symbol::left
                                ? (px >= .25f && px <= .7f && std::abs(py - .5f) < (px - .25f) * .7f)
                            : symbol == Symbol::up ? (py >= .25f && py <= .7f && std::abs(px - .5f) < (py - .25f) * .7f)
                            : symbol == Symbol::right
                                ? (px >= .3f && px <= .75f && std::abs(py - .5f) < (.75f - px) * .7f)
                            : symbol == Symbol::points
                                ? (std::hypot(px - .24f, py - .30f) < .095f ||
                                   std::hypot(px - .64f, py - .22f) < .095f ||
                                   std::hypot(px - .76f, py - .56f) < .095f ||
                                   std::hypot(px - .32f, py - .72f) < .095f || std::hypot(px - .56f, py - .80f) < .085f)
                            : symbol == Symbol::point_normals
                                ? (std::hypot(px - .26f, py - .74f) < .13f || segment(px, py, .28f, .72f, .80f, .20f) ||
                                   segment(px, py, .80f, .20f, .60f, .22f) || segment(px, py, .80f, .20f, .78f, .40f))
                            : symbol == Symbol::face_normals
                                ? (segment(px, py, .14f, .70f, .44f, .86f) || segment(px, py, .44f, .86f, .86f, .66f) ||
                                   segment(px, py, .86f, .66f, .56f, .50f) || segment(px, py, .56f, .50f, .14f, .70f) ||
                                   segment(px, py, .50f, .68f, .50f, .14f) || segment(px, py, .50f, .14f, .38f, .28f) ||
                                   segment(px, py, .50f, .14f, .62f, .28f))
                            : symbol == Symbol::orient_scale
                                ? (std::abs(std::hypot(px - .5f, py - .54f) - .35f) < .032f ||
                                   segment(px, py, .5f, .54f, .5f, .15f) || segment(px, py, .5f, .54f, .84f, .66f) ||
                                   segment(px, py, .5f, .54f, .20f, .70f) || std::hypot(px - .5f, py - .54f) < .08f)
                            : symbol == Symbol::backface_tint
                                ? (segment(px, py, .14f, .74f, .50f, .16f) || segment(px, py, .50f, .16f, .86f, .74f) ||
                                   segment(px, py, .14f, .74f, .86f, .74f) || segment(px, py, .50f, .16f, .50f, .74f) ||
                                   (px >= .50f && py <= .74f && py >= .16f + (px - .50f) * (0.58f / 0.36f)))
                            : symbol == Symbol::bounds_cage
                                ? (segment(px, py, .15f, .15f, .36f, .15f) || segment(px, py, .15f, .15f, .15f, .36f) ||
                                   segment(px, py, .85f, .15f, .64f, .15f) || segment(px, py, .85f, .15f, .85f, .36f) ||
                                   segment(px, py, .15f, .85f, .36f, .85f) || segment(px, py, .15f, .85f, .15f, .64f) ||
                                   segment(px, py, .85f, .85f, .64f, .85f) || segment(px, py, .85f, .85f, .85f, .64f) ||
                                   std::hypot(px - .5f, py - .5f) < .07f)
                            : symbol == Symbol::info ? (std::abs(std::hypot(px - .5f, py - .5f) - .37f) < .042f ||
                                                        std::hypot(px - .5f, py - .30f) < .065f ||
                                                        (std::abs(px - .5f) < .052f && py >= .44f && py <= .74f))
                            : symbol == Symbol::sticky_note
                                ? (segment(px, py, .18f, .14f, .82f, .14f) || segment(px, py, .18f, .14f, .18f, .86f) ||
                                   segment(px, py, .82f, .14f, .82f, .62f) || segment(px, py, .18f, .86f, .58f, .86f) ||
                                   segment(px, py, .58f, .86f, .82f, .62f) || segment(px, py, .58f, .62f, .82f, .62f) ||
                                   segment(px, py, .58f, .62f, .58f, .86f) || segment(px, py, .30f, .35f, .70f, .35f) ||
                                   segment(px, py, .30f, .50f, .54f, .50f))
                            : symbol == Symbol::network_box
                                ? (outline(.12f, .16f, .78f, .82f) ||
                                   (px >= .12f && px <= .78f && py >= .16f && py <= .33f) ||
                                   segment(px, py, .64f, .74f, .92f, .74f) || segment(px, py, .78f, .60f, .78f, .88f))
                            : symbol == Symbol::palette
                                ? ((px >= .14f && px <= .44f && py >= .14f && py <= .44f) ||
                                   outline(.56f, .14f, .86f, .44f) || outline(.14f, .56f, .44f, .86f) ||
                                   (px >= .56f && px <= .86f && py >= .56f && py <= .86f))
                            : symbol == Symbol::node_shapes
                                ? ((py >= .20f && py <= .40f && px >= .12f && px <= .46f &&
                                    std::hypot(std::max(0.f, std::abs(px - .29f) - .07f), py - .30f) <= .10f) ||
                                   std::hypot(px - .72f, py - .30f) <= .13f ||
                                   (std::abs(px - .29f) * .8f + std::abs(py - .70f) * 1.15f <= .15f) ||
                                   (py >= .58f && py <= .82f && std::abs(px - .72f) <= .16f - (py - .58f) * .25f))
                            : symbol == Symbol::network_layout
                                ? ((px >= .34f && px <= .66f && py >= .12f && py <= .28f) ||
                                   (px >= .12f && px <= .42f && py >= .68f && py <= .86f) ||
                                   (px >= .58f && px <= .88f && py >= .68f && py <= .86f) ||
                                   segment(px, py, .50f, .28f, .50f, .48f) || segment(px, py, .27f, .48f, .73f, .48f) ||
                                   segment(px, py, .27f, .48f, .27f, .68f) || segment(px, py, .73f, .48f, .73f, .68f))
                            : symbol == Symbol::pin
                                ? ((py >= .15f && py <= .25f && std::abs(px - .5f) <= .24f) ||
                                   (py >= .25f && py <= .52f && std::abs(px - .5f) <= .14f + (py - .25f) * .5f) ||
                                   (py >= .52f && py <= .60f && std::abs(px - .5f) <= .30f) ||
                                   (py >= .60f && py <= .88f &&
                                    std::abs(px - .5f) <= .048f * (1.f - (py - .60f) * 2.f)))
                            : symbol == Symbol::isolate
                                ? (std::abs(std::hypot(px - .5f, py - .5f) - .34f) < .042f ||
                                   std::hypot(px - .5f, py - .5f) < .11f || segment(px, py, .5f, .08f, .5f, .22f) ||
                                   segment(px, py, .5f, .78f, .5f, .92f) || segment(px, py, .08f, .5f, .22f, .5f) ||
                                   segment(px, py, .78f, .5f, .92f, .5f))
                            : symbol == Symbol::sliders
                                ? (segment(px, py, .14f, .26f, .86f, .26f) || segment(px, py, .14f, .50f, .86f, .50f) ||
                                   segment(px, py, .14f, .74f, .86f, .74f) ||
                                   std::hypot(px - .36f, py - .26f) < .095f ||
                                   std::hypot(px - .68f, py - .50f) < .095f || std::hypot(px - .44f, py - .74f) < .095f)
                            : symbol == Symbol::bypass_arrow
                                ? ((std::abs(px - .50f) <= .074f && py >= .17f && py <= .71f) ||
                                   [&] {
                                       const auto bold_seg = [&](float ax, float ay, float bx, float by) {
                                           const float dx = bx - ax;
                                           const float dy = by - ay;
                                           const float t = std::clamp(
                                               ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
                                           return std::hypot(px - ax - t * dx, py - ay - t * dy) <= .068f;
                                       };
                                       return bold_seg(.24f, .51f, .50f, .78f) || bold_seg(.76f, .51f, .50f, .78f);
                                   }())
                            : symbol == Symbol::info_glyph ? (std::hypot(px - .50f, py - .22f) <= .090f ||
                                                              (px >= .36f && px <= .57f && py >= .40f && py <= .48f) ||
                                                              (px >= .43f && px <= .57f && py >= .40f && py <= .80f) ||
                                                              (px >= .34f && px <= .66f && py >= .74f && py <= .83f))
                            : symbol == Symbol::snowflake  ? ([&] {
                                  constexpr std::array<std::array<float, 2>, 6> dirs{{
                                      {0.f, -1.f},
                                      {.8660254f, -.5f},
                                      {.8660254f, .5f},
                                      {0.f, 1.f},
                                      {-.8660254f, .5f},
                                      {-.8660254f, -.5f},
                                  }};
                                  for (int k = 0; k < 3; ++k) {
                                      if (segment(px, py, .50f - dirs[k][0] * .39f, .50f - dirs[k][1] * .39f,
                                                  .50f + dirs[k][0] * .39f, .50f + dirs[k][1] * .39f))
                                          return true;
                                  }
                                  for (int k = 0; k < 6; ++k) {
                                      const float bx = .50f + dirs[k][0] * .21f;
                                      const float by = .50f + dirs[k][1] * .21f;
                                      const auto& d1 = dirs[(k + 1) % 6];
                                      const auto& d2 = dirs[(k + 5) % 6];
                                      if (segment(px, py, bx, by, bx + (dirs[k][0] + d1[0]) * .095f,
                                                  by + (dirs[k][1] + d1[1]) * .095f) ||
                                          segment(px, py, bx, by, bx + (dirs[k][0] + d2[0]) * .095f,
                                                  by + (dirs[k][1] + d2[1]) * .095f))
                                          return true;
                                  }
                                  return std::hypot(px - .50f, py - .50f) <= .07f;
                              }())
                            : symbol == Symbol::eye ? ((px >= .12f && px <= .88f && py <= .52f &&
                                                        std::abs(std::hypot(px - .50f, py - .80f) - .48f) <= .044f) ||
                                                       (px >= .12f && px <= .88f && py >= .48f &&
                                                        std::abs(std::hypot(px - .50f, py - .20f) - .48f) <= .044f) ||
                                                       std::hypot(px - .50f, py - .50f) <= .145f)
                            : symbol == Symbol::eye_off
                                ? (((px >= .12f && px <= .88f && py <= .52f &&
                                     std::abs(std::hypot(px - .50f, py - .80f) - .48f) <= .042f) ||
                                    (px >= .12f && px <= .88f && py >= .48f &&
                                     std::abs(std::hypot(px - .50f, py - .20f) - .48f) <= .042f)) &&
                                   std::abs((px - .5f) + (py - .5f)) > .09f) ||
                                      segment(px, py, .18f, .82f, .82f, .18f)
                            : symbol == Symbol::scene
                                ? (outline(.12f, .18f, .88f, .82f) || segment(px, py, .12f, .56f, .88f, .56f) ||
                                   segment(px, py, .50f, .56f, .24f, .82f) || segment(px, py, .50f, .56f, .76f, .82f) ||
                                   std::hypot(px - .70f, py - .35f) <= .085f)
                            : symbol == Symbol::material
                                ? (std::abs(std::hypot(px - .50f, py - .50f) - .36f) <= .042f ||
                                   (px >= .50f && std::hypot(px - .50f, py - .50f) <= .36f) ||
                                   std::hypot(px - .35f, py - .36f) <= .065f)
                            : symbol == Symbol::texture
                                ? (outline(.14f, .16f, .86f, .84f) || std::hypot(px - .34f, py - .35f) <= .075f ||
                                   segment(px, py, .16f, .76f, .44f, .46f) || segment(px, py, .44f, .46f, .60f, .64f) ||
                                   segment(px, py, .60f, .64f, .73f, .50f) || segment(px, py, .73f, .50f, .85f, .76f))
                            : symbol == Symbol::source_file
                                ? (segment(px, py, .20f, .12f, .62f, .12f) || segment(px, py, .62f, .12f, .80f, .30f) ||
                                   segment(px, py, .80f, .30f, .80f, .88f) || segment(px, py, .80f, .88f, .20f, .88f) ||
                                   segment(px, py, .20f, .88f, .20f, .12f) || segment(px, py, .62f, .12f, .62f, .30f) ||
                                   segment(px, py, .62f, .30f, .80f, .30f) || segment(px, py, .43f, .46f, .31f, .58f) ||
                                   segment(px, py, .31f, .58f, .43f, .70f) || segment(px, py, .57f, .46f, .69f, .58f) ||
                                   segment(px, py, .69f, .58f, .57f, .70f))
                            : symbol == Symbol::procgraph
                                ? (std::hypot(px - .26f, py - .24f) <= .12f ||
                                   std::hypot(px - .74f, py - .24f) <= .12f ||
                                   std::hypot(px - .50f, py - .76f) <= .13f ||
                                   segment(px, py, .26f, .34f, .50f, .65f) || segment(px, py, .74f, .34f, .50f, .65f))
                            : symbol == Symbol::physics
                                ? (std::abs(std::hypot(px - .56f, py - .36f) - .20f) <= .042f ||
                                   segment(px, py, .14f, .80f, .86f, .80f) || segment(px, py, .18f, .78f, .34f, .54f) ||
                                   segment(px, py, .34f, .54f, .42f, .76f))
                            : symbol == Symbol::camera
                                ? (outline(.12f, .28f, .66f, .74f) || segment(px, py, .66f, .43f, .88f, .30f) ||
                                   segment(px, py, .88f, .30f, .88f, .72f) || segment(px, py, .66f, .59f, .88f, .72f))
                            : symbol == Symbol::copy
                                ? (outline(.32f, .30f, .84f, .86f) || segment(px, py, .16f, .14f, .68f, .14f) ||
                                   segment(px, py, .16f, .14f, .16f, .70f) || segment(px, py, .16f, .70f, .32f, .70f) ||
                                   segment(px, py, .68f, .14f, .68f, .30f))
                            : symbol == Symbol::save
                                ? (segment(px, py, .16f, .16f, .70f, .16f) || segment(px, py, .70f, .16f, .84f, .30f) ||
                                   segment(px, py, .84f, .30f, .84f, .84f) || segment(px, py, .84f, .84f, .16f, .84f) ||
                                   segment(px, py, .16f, .84f, .16f, .16f) || outline(.30f, .16f, .64f, .40f) ||
                                   outline(.28f, .54f, .72f, .84f))
                            : symbol == Symbol::play
                                ? (px >= .26f && px <= .80f && std::abs(py - .50f) <= (.80f - px) * .62f)
                            : symbol == Symbol::stop ? (px >= .24f && px <= .76f && py >= .24f && py <= .76f)
                            : symbol == Symbol::build
                                ? (segment(px, py, .20f, .80f, .60f, .40f) || segment(px, py, .24f, .84f, .64f, .44f) ||
                                   segment(px, py, .46f, .20f, .80f, .54f) || segment(px, py, .50f, .16f, .84f, .50f))
                            : symbol == Symbol::reload
                                ? ((std::abs(std::hypot(px - .50f, py - .50f) - .32f) <= .045f &&
                                    !(px > .58f && py < .44f)) ||
                                   segment(px, py, .78f, .18f, .78f, .40f) || segment(px, py, .56f, .40f, .78f, .40f))
                            : symbol == Symbol::warning
                                ? (segment(px, py, .50f, .15f, .87f, .82f) || segment(px, py, .87f, .82f, .13f, .82f) ||
                                   segment(px, py, .13f, .82f, .50f, .15f) ||
                                   (std::abs(px - .50f) <= .042f && py >= .38f && py <= .61f) ||
                                   std::hypot(px - .50f, py - .71f) <= .052f)
                            : symbol == Symbol::lock
                                ? (outline(.23f, .46f, .77f, .85f) ||
                                   (std::abs(px - .50f) <= .045f && py >= .58f && py <= .73f) ||
                                   (std::abs(std::hypot(px - .50f, py - .33f) - .15f) <= .042f && py <= .33f) ||
                                   (std::abs(px - .35f) <= .042f && py >= .33f && py <= .46f) ||
                                   (std::abs(px - .65f) <= .042f && py >= .33f && py <= .46f))
                            : symbol == Symbol::unlock
                                ? (outline(.23f, .48f, .77f, .86f) ||
                                   (std::abs(px - .50f) <= .045f && py >= .60f && py <= .74f) ||
                                   (std::abs(std::hypot(px - .54f, py - .28f) - .15f) <= .042f && py <= .28f) ||
                                   (std::abs(px - .39f) <= .042f && py >= .28f && py <= .48f) ||
                                   (std::abs(px - .69f) <= .042f && py >= .28f && py <= .33f))
                            : symbol == Symbol::template_flag
                                ? ((py >= .21f && py <= .36f &&
                                    std::abs(px - .50f) <= .30f - std::max(0.f, .28f - py) * 1.15f) ||
                                   (std::abs(px - .50f) <= .085f && py >= .26f && py <= .82f) ||
                                   (std::abs(px - .50f) <= .14f && py >= .76f && py <= .83f))
                                : (py >= .3f && py <= .75f && std::abs(px - .5f) < (.75f - py) * .7f);
                        inside += hit ? 1u : 0u;
                    }
                coverage->pixels[static_cast<std::size_t>(y) * pixels + x] =
                    static_cast<std::uint8_t>((inside * 255 + 8) / 16);
            }
    }
    ++impl_->statistics.rasterizations;
    impl_->statistics.raster_bytes += coverage->pixels.size();
    impl_->symbols.emplace(key, coverage);
    return coverage;
}

std::uint64_t TextStore::provider_revision() const noexcept {
    return impl_->provider_revision;
}

void TextStore::set_provider_revision(std::uint64_t revision) noexcept {
    impl_->provider_revision = revision;
    impl_->formats.clear();
    for (auto& [key, entry] : impl_->entries) {
        (void)key;
        entry.paragraphs = {};
    }
}

TextStatistics TextStore::statistics() const noexcept {
    auto result = impl_->statistics;
    result.live_coverages = impl_->accounting->count.load(std::memory_order_relaxed);
    result.live_coverage_bytes = impl_->accounting->bytes.load(std::memory_order_relaxed);
    return result;
}

ui_layout::MeasureResult TextStore::measure(void* context, const ui_layout::MeasureQuery& query) {
    auto& impl = *static_cast<TextStore*>(context)->impl_;
    ++impl.statistics.measure_calls;
    const auto found = impl.entries.find(query.content_key);
    if (found == impl.entries.end() || found->second.revision != query.content_revision ||
        query.provider_revision != impl.provider_revision)
        return std::unexpected(ui_layout::MeasureError{static_cast<std::uint32_t>(E_INVALIDARG)});
    const auto paragraph = impl.resolve(query.content_key, query.width);
    if (!paragraph)
        return std::unexpected(ui_layout::MeasureError{paragraph.error().native_code});
    return (*paragraph)->metrics;
}

Result<ui_layout::MeasuredSize> TextStore::metrics(TextKey key, float width) {
    ATELIER_TRY_BIND(paragraph, impl_->resolve(key, width));
    return (*paragraph)->metrics;
}

Result<RasterText> TextStore::raster(TextKey key, float width, float dpi, std::optional<Clip> viewport) {
    ATELIER_TRY_BIND(resolved, impl_->resolve(key, width));
    auto& paragraph = **resolved;
    if (paragraph.raster_scale == dpi && paragraph.raster_region == visible_ink(paragraph.ink_bounds, viewport)) {
        ++impl_->statistics.coverage_cache_hits;
        return paragraph.raster;
    }
    ++impl_->statistics.rasterizations;
    Rasterizer rasterizer(impl_->factory.Get(), dpi, viewport);
    const auto result = paragraph.layout->Draw(nullptr, &rasterizer, 0, 0);
    ATELIER_TRY(check(rasterizer.failure));
    ATELIER_TRY(check(result));
    RasterText output;
    if (!rasterizer.runs.empty()) {
        auto bounds = rasterizer.runs.front().bounds;
        for (const auto& run : rasterizer.runs) {
            bounds.left = std::min(bounds.left, run.bounds.left);
            bounds.top = std::min(bounds.top, run.bounds.top);
            bounds.right = std::max(bounds.right, run.bounds.right);
            bounds.bottom = std::max(bounds.bottom, run.bounds.bottom);
        }
        const auto width_px = static_cast<std::int64_t>(bounds.right) - bounds.left;
        const auto height_px = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
        if (!fits_coverage(width_px, height_px))
            return std::unexpected(check(E_OUTOFMEMORY).error());
        auto coverage = std::make_shared<TrackedCoverage>(impl_->accounting, static_cast<std::uint32_t>(width_px),
                                                          static_cast<std::uint32_t>(height_px));
        for (const auto& run : rasterizer.runs) {
            const auto run_width = static_cast<std::size_t>(run.bounds.right - run.bounds.left);
            for (LONG y = run.bounds.top; y < run.bounds.bottom; ++y) {
                auto* destination = coverage->pixels.data() +
                                    static_cast<std::size_t>(y - bounds.top) * coverage->row_pitch +
                                    (run.bounds.left - bounds.left);
                const auto* source = run.pixels.data() + static_cast<std::size_t>(y - run.bounds.top) * run_width;
                for (std::size_t x = 0; x < run_width; ++x) {
                    const auto a = static_cast<unsigned>(source[x]);
                    const auto b = static_cast<unsigned>(destination[x]);
                    destination[x] = static_cast<std::uint8_t>(a + b - (a * b + 127) / 255);
                }
            }
        }
        impl_->statistics.raster_bytes += coverage->pixels.size();
        output.coverage = std::move(coverage);
        output.left = static_cast<float>(bounds.left);
        output.top = static_cast<float>(bounds.top);
    }
    paragraph.raster = output;
    paragraph.raster_scale = dpi;
    const auto& ink = rasterizer.ink_bounds;
    paragraph.ink_bounds = {ink.left, ink.top, ink.right, ink.bottom};
    paragraph.raster_region = visible_ink(paragraph.ink_bounds, viewport);
    return output;
}

Result<std::shared_ptr<const TextSnapshot>> TextStore::snapshot(TextKey key, float width) {
    ATELIER_TRY_BIND(resolved, impl_->resolve(key, width));
    auto& paragraph = **resolved;
    if (!paragraph.snapshot) {
        auto data = std::make_shared<TextSnapshot::Impl>();
        data->layout = paragraph.layout;
        data->text = impl_->entries.at(key).text;
        data->font = impl_->entries.at(key).font;
        data->alignment = impl_->entries.at(key).alignment;
        data->width = width;
        paragraph.snapshot = std::shared_ptr<const TextSnapshot>(new TextSnapshot(std::move(data)));
    }
    return paragraph.snapshot;
}

Result<std::shared_ptr<const TextSnapshot>> TextStore::edited_snapshot(std::uint64_t key, const TextSnapshot& displayed,
                                                                       StringView value) {
    const auto& policy = *displayed.impl_;
    set(key, value, policy.font, false, policy.alignment);
    return snapshot(key, policy.width);
}

Result<Rect> TextSnapshot::caret(TextPosition position) const {
    return impl_->caret_box(position);
}

Result<Rect> TextSnapshot::character(std::uint32_t position) const {
    FLOAT x{};
    FLOAT y{};
    DWRITE_HIT_TEST_METRICS metrics{};
    const auto u16_pos = static_cast<UINT32>(platform::utf8_offset_to_utf16(impl_->text.view(), position));
    ATELIER_TRY(check(impl_->layout->HitTestTextPosition(u16_pos, FALSE, &x, &y, &metrics)));
    // The leading caret of an RTL run is its right edge. Native character
    // position requests need the actual enclosing character's upper-left.
    return Rect{metrics.left, metrics.top, metrics.width, metrics.height};
}

Result<TextPosition> TextSnapshot::hit(float x, float y) const {
    ATELIER_TRY(impl_->resolve_stops());
    BOOL trailing{};
    BOOL inside{};
    DWRITE_HIT_TEST_METRICS metrics{};
    ATELIER_TRY(check(impl_->layout->HitTestPoint(x, y, &trailing, &inside, &metrics)));
    (void)inside;
    const std::size_t u16_cand = static_cast<std::size_t>(metrics.textPosition) + (trailing ? metrics.length : 0);
    const auto u8_cand = static_cast<std::uint32_t>(platform::utf16_offset_to_utf8(impl_->text.view(), u16_cand));
    const TextPosition candidate{u8_cand, trailing != FALSE && u8_cand != 0};
    auto selected = impl_->stops.front();
    for (auto stop : impl_->stops) {
        const auto distance = std::abs(stop.x - x);
        const auto previous = std::abs(selected.x - x);
        if (distance < previous || (distance == previous && stop.position == candidate))
            selected = stop;
    }
    return selected.position;
}

Result<TextPosition> TextSnapshot::move(TextPosition position, int direction, bool edge) const {
    ATELIER_TRY(impl_->resolve_stops());
    const auto& stops = impl_->stops;
    if (edge)
        return direction < 0 ? stops.front().position : stops.back().position;
    ATELIER_TRY_BIND(caret, impl_->caret_box(position));
    const float x = caret->x;
    if (direction < 0) {
        for (std::size_t i = stops.size(); i-- > 0;)
            if (stops[i].x < x - .01f)
                return stops[i].position;
    } else
        for (auto stop : stops)
            if (stop.x > x + .01f)
                return stop.position;
    return position;
}

Result<Array<Rect>> TextSnapshot::selection(std::uint32_t first, std::uint32_t last) const {
    if (first >= last)
        return {};
    const auto u16_first = static_cast<UINT32>(platform::utf8_offset_to_utf16(impl_->text.view(), first));
    const auto u16_last = static_cast<UINT32>(platform::utf8_offset_to_utf16(impl_->text.view(), last));
    if (u16_first >= u16_last)
        return {};
    UINT32 count{};
    const auto status = impl_->layout->HitTestTextRange(u16_first, u16_last - u16_first, 0, 0, nullptr, 0, &count);
    if (status != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
        ATELIER_TRY(check(status));
    InlineArray<DWRITE_HIT_TEST_METRICS, 8> metrics;
    metrics.resize(count);
    ATELIER_TRY(
        check(impl_->layout->HitTestTextRange(u16_first, u16_last - u16_first, 0, 0, metrics.data(), count, &count)));
    Array<Rect> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        result.add({metrics[i].left, metrics[i].top, metrics[i].width, metrics[i].height});
    return result;
}
} // namespace atelier::ui::detail

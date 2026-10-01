#include "ui/text.hpp"
#include <Windows.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <map>
#include <string>
#include <tuple>

namespace atelier::ui::detail {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT status) { if (FAILED(status)) throw TextFailure{static_cast<std::uint32_t>(status)}; }
std::atomic<std::uint64_t> next_identity{1};
constexpr std::int64_t max_bitmap_bytes = 16 * 1024 * 1024;
struct CoverageAccounting {
    std::atomic<std::uint64_t> count{}, bytes{};
};
struct TrackedCoverage final : Coverage {
    std::shared_ptr<CoverageAccounting> accounting;
    TrackedCoverage(std::shared_ptr<CoverageAccounting> owner, std::uint32_t w, std::uint32_t h)
        : accounting(std::move(owner)) {
        identity = new_identity(); revision = 1; width = row_pitch = w; height = h;
        pixels.resize(static_cast<std::size_t>(w)*h);
        accounting->count.fetch_add(1,std::memory_order_relaxed);
        accounting->bytes.fetch_add(pixels.size(),std::memory_order_relaxed);
    }
    ~TrackedCoverage() {
        accounting->count.fetch_sub(1,std::memory_order_relaxed);
        accounting->bytes.fetch_sub(pixels.size(),std::memory_order_relaxed);
    }
};
struct FontLess {
    bool operator()(const Font& a, const Font& b) const {
        return std::tie(a.family,a.locale,a.size,a.weight) < std::tie(b.family,b.locale,b.size,b.weight);
    }
};

struct Rasterizer final : IDWriteTextRenderer {
    struct Run { RECT bounds{}; std::vector<std::uint8_t> pixels; };
    IDWriteFactory2* factory;
    float scale;
    std::optional<Clip> viewport;
    ULONG refs{1};
    std::vector<Run> runs;
    std::exception_ptr failure;
    Rasterizer(IDWriteFactory2* value, float dpi, std::optional<Clip> clip) : factory(value), scale(dpi),viewport(clip) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return --refs; } // Stack-owned for synchronous Draw.
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override { *disabled = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override {
        *transform = {1, 0, 0, 1, 0, 0}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) noexcept override { *value = scale; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* glyphs, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) noexcept override {
        try {
            ComPtr<IDWriteGlyphRunAnalysis> analysis;
            const DWRITE_MATRIX transform{scale, 0, 0, scale, 0, 0};
            check(factory->CreateGlyphRunAnalysis(glyphs, &transform, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                mode, DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, x, y, &analysis));
            Run run;
            check(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &run.bounds));
            if (viewport) {
                run.bounds.left=std::max<LONG>(run.bounds.left,viewport->left); run.bounds.top=std::max<LONG>(run.bounds.top,viewport->top);
                run.bounds.right=std::min<LONG>(run.bounds.right,viewport->right); run.bounds.bottom=std::min<LONG>(run.bounds.bottom,viewport->bottom);
            }
            const auto width = static_cast<std::int64_t>(run.bounds.right) - run.bounds.left;
            const auto height = static_cast<std::int64_t>(run.bounds.bottom) - run.bounds.top;
            if (width <= 0 || height <= 0) return S_OK;
            if (width > 32768 || height > 32768 || width * height > max_bitmap_bytes) return E_OUTOFMEMORY;
            run.pixels.resize(static_cast<std::size_t>(width * height));
            check(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &run.bounds, run.pixels.data(),
                static_cast<UINT32>(run.pixels.size())));
            runs.push_back(std::move(run));
            return S_OK;
        } catch (...) { failure = std::current_exception(); return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) noexcept override { return E_NOTIMPL; }
};
}

std::uint64_t new_identity() {
    auto value = next_identity.load(std::memory_order_relaxed);
    do {
        if (value == UINT64_MAX) throw TextFailure{static_cast<std::uint32_t>(E_FAIL)};
    } while (!next_identity.compare_exchange_weak(value,value+1,std::memory_order_relaxed));
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
        std::optional<Clip> raster_viewport;
        bool valid{};
    };
    struct Entry {
        std::wstring text;
        Font font;
        bool wrap{};
        std::uint64_t revision{1};
        std::array<Paragraph, 2> paragraphs;
    };
    ComPtr<IDWriteFactory2> factory;
    std::map<std::uint64_t, Entry> entries;
    std::map<Font, ComPtr<IDWriteTextFormat>, FontLess> formats;
    std::vector<DWRITE_LINE_METRICS> lines;
    std::uint64_t provider_revision{new_identity()};
    TextStatistics statistics;
    std::shared_ptr<CoverageAccounting> accounting{std::make_shared<CoverageAccounting>()};
    Impl() {
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
    }
    Paragraph& resolve(std::uint64_t key, std::optional<float> width) {
        auto found = entries.find(key);
        if (found == entries.end()) throw TextFailure{static_cast<std::uint32_t>(E_INVALIDARG)};
        auto& entry = found->second;
        auto& paragraph = entry.paragraphs[width ? 1 : 0];
        if (paragraph.layout && paragraph.valid && paragraph.width == width) return paragraph;
        // Published snapshots can retain this native layout. Width changes create
        // a new layout instead of mutating geometry still used by an older frame.
        paragraph = {};
        auto& format = formats[entry.font];
        if (!format) {
            check(factory->CreateTextFormat(entry.font.family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(entry.font.weight),
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, entry.font.size, entry.font.locale.c_str(), &format));
        }
        check(factory->CreateTextLayout(entry.text.data(), static_cast<UINT32>(entry.text.size()), format.Get(),
            width.value_or(0), 0, &paragraph.layout));
        ++statistics.text_layouts;
        check(paragraph.layout->SetWordWrapping(width && entry.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP));
        DWRITE_TEXT_METRICS metrics{};
        check(paragraph.layout->GetMetrics(&metrics));
        UINT32 count{};
        const auto status = paragraph.layout->GetLineMetrics(nullptr, 0, &count);
        if (status != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) check(status);
        lines.resize(count);
        check(paragraph.layout->GetLineMetrics(lines.data(), count, &count));
        paragraph.metrics = {metrics.widthIncludingTrailingWhitespace, metrics.height, count ? lines[0].baseline : 0};
        paragraph.width = width;
        paragraph.valid = true;
        return paragraph;
    }
};

struct TextSnapshot::Impl {
    struct Stop { TextPosition position; float x; };
    ComPtr<IDWriteTextLayout> layout;
    std::wstring text;
    Font font;
    float width{};
    mutable std::vector<Stop> stops; // Owner-thread query cache; paragraph geometry stays immutable.
    Rect caret_box(TextPosition position) const {
        FLOAT x{},y{}; DWRITE_HIT_TEST_METRICS metrics{};
        const bool trailing=position.trailing && position.offset!=0;
        check(layout->HitTestTextPosition(position.offset-(trailing?1:0),trailing,&x,&y,&metrics));
        return {x,y,1,metrics.height};
    }
    void resolve_stops() const {
        if (!stops.empty()) return;
        const auto boundaries=character_boundaries(text);
        for (auto offset:boundaries) {
            const TextPosition leading{offset,false},trailing{offset,offset!=0};
            const auto first=caret_box(leading), last=caret_box(trailing);
            stops.push_back({leading,first.x});
            if (std::abs(first.x-last.x)>.01f) stops.push_back({trailing,last.x});
        }
        std::stable_sort(stops.begin(),stops.end(),[](auto a,auto b){ return a.x<b.x; });
    }
};
TextSnapshot::TextSnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
bool TextSnapshot::matches(std::wstring_view text) const noexcept { return impl_->text == text; }

TextStore::TextStore() : impl_(std::make_unique<Impl>()) {}
TextStore::~TextStore() = default;
std::uint64_t TextStore::set(std::uint64_t key, std::wstring_view text, const Font& font, bool wrap) {
    auto& entry = impl_->entries[key];
    if (entry.text != text || entry.font != font || entry.wrap != wrap) {
        entry.text = text; entry.font = font; entry.wrap = wrap;
        entry.revision = new_identity(); entry.paragraphs = {};
    }
    return entry.revision;
}
void TextStore::erase(std::uint64_t key) { impl_->entries.erase(key); }
std::uint64_t TextStore::provider_revision() const noexcept { return impl_->provider_revision; }
void TextStore::set_provider_revision(std::uint64_t revision) noexcept {
    impl_->provider_revision = revision;
    impl_->formats.clear();
    for (auto& [key,entry] : impl_->entries) { (void)key; entry.paragraphs = {}; }
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
    if (found == impl.entries.end() || found->second.revision != query.content_revision || query.provider_revision != impl.provider_revision)
        return std::unexpected(ui_layout::MeasureError{static_cast<std::uint32_t>(E_INVALIDARG)});
    try { return impl.resolve(query.content_key, query.width).metrics; }
    catch (const TextFailure& failure) { return std::unexpected(ui_layout::MeasureError{failure.code}); }
}
ui_layout::MeasuredSize TextStore::metrics(std::uint64_t key, float width) { return impl_->resolve(key,width).metrics; }
RasterText TextStore::raster(std::uint64_t key, float width, float dpi, std::optional<Clip> viewport) {
    auto& paragraph = impl_->resolve(key, width);
    if (paragraph.raster_scale == dpi && paragraph.raster_viewport==viewport) { ++impl_->statistics.coverage_cache_hits; return paragraph.raster; }
    ++impl_->statistics.rasterizations;
    Rasterizer rasterizer(impl_->factory.Get(), dpi,viewport);
    const auto result = paragraph.layout->Draw(nullptr, &rasterizer, 0, 0);
    if (rasterizer.failure) std::rethrow_exception(rasterizer.failure);
    check(result);
    RasterText output;
    if (!rasterizer.runs.empty()) {
        auto bounds = rasterizer.runs.front().bounds;
        for (const auto& run : rasterizer.runs) {
            bounds.left = std::min(bounds.left, run.bounds.left); bounds.top = std::min(bounds.top, run.bounds.top);
            bounds.right = std::max(bounds.right, run.bounds.right); bounds.bottom = std::max(bounds.bottom, run.bounds.bottom);
        }
        const auto width_px = static_cast<std::int64_t>(bounds.right) - bounds.left;
        const auto height_px = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
        if (width_px > 32768 || height_px > 32768 || width_px * height_px > max_bitmap_bytes)
            throw TextFailure{static_cast<std::uint32_t>(E_OUTOFMEMORY)};
        auto coverage = std::make_shared<TrackedCoverage>(impl_->accounting,static_cast<std::uint32_t>(width_px),static_cast<std::uint32_t>(height_px));
        for (const auto& run : rasterizer.runs) {
            const auto run_width = static_cast<std::size_t>(run.bounds.right - run.bounds.left);
            for (LONG y = run.bounds.top; y < run.bounds.bottom; ++y) {
                auto* destination = coverage->pixels.data() + static_cast<std::size_t>(y - bounds.top) * coverage->row_pitch + (run.bounds.left - bounds.left);
                const auto* source = run.pixels.data() + static_cast<std::size_t>(y - run.bounds.top) * run_width;
                for (std::size_t x = 0; x < run_width; ++x) {
                    const auto a = static_cast<unsigned>(source[x]), b = static_cast<unsigned>(destination[x]);
                    destination[x] = static_cast<std::uint8_t>(a + b - (a * b + 127) / 255);
                }
            }
        }
        impl_->statistics.raster_bytes += coverage->pixels.size();
        output.coverage = std::move(coverage); output.left = static_cast<float>(bounds.left); output.top = static_cast<float>(bounds.top);
    }
    paragraph.raster = output; paragraph.raster_scale = dpi; paragraph.raster_viewport=viewport;
    return output;
}
std::shared_ptr<const TextSnapshot> TextStore::snapshot(std::uint64_t key, float width) {
    auto& paragraph=impl_->resolve(key,width);
    if (!paragraph.snapshot) {
        auto data=std::make_shared<TextSnapshot::Impl>();
        data->layout=paragraph.layout; data->text=impl_->entries.at(key).text;
        data->font=impl_->entries.at(key).font; data->width=width;
        paragraph.snapshot=std::shared_ptr<const TextSnapshot>(new TextSnapshot(std::move(data)));
    }
    return paragraph.snapshot;
}
std::shared_ptr<const TextSnapshot> TextStore::edited_snapshot(std::uint64_t key, const TextSnapshot& displayed, std::wstring_view value) {
    const auto& policy=*displayed.impl_;
    set(key,value,policy.font,false);
    return snapshot(key,policy.width);
}
Rect TextSnapshot::caret(TextPosition position) const { return impl_->caret_box(position); }
Rect TextSnapshot::character(std::uint32_t position) const {
    FLOAT x{},y{}; DWRITE_HIT_TEST_METRICS metrics{};
    check(impl_->layout->HitTestTextPosition(position,FALSE,&x,&y,&metrics));
    // The leading caret of an RTL run is its right edge. Native character
    // position requests need the actual enclosing character's upper-left.
    return {metrics.left,metrics.top,metrics.width,metrics.height};
}
TextPosition TextSnapshot::hit(float x, float y) const {
    impl_->resolve_stops();
    BOOL trailing{},inside{}; DWRITE_HIT_TEST_METRICS metrics{};
    check(impl_->layout->HitTestPoint(x,y,&trailing,&inside,&metrics));
    (void)inside;
    const TextPosition candidate{metrics.textPosition+(trailing?metrics.length:0),trailing!=FALSE};
    auto selected=impl_->stops.front();
    for (auto stop:impl_->stops) {
        const auto distance=std::abs(stop.x-x), previous=std::abs(selected.x-x);
        if (distance<previous || (distance==previous && stop.position==candidate)) selected=stop;
    }
    return selected.position;
}
TextPosition TextSnapshot::move(TextPosition position, int direction, bool edge) const {
    impl_->resolve_stops();
    const auto& stops=impl_->stops;
    if (edge) return direction<0?stops.front().position:stops.back().position;
    const float x=impl_->caret_box(position).x;
    if (direction<0) {
        for (auto at=stops.rbegin();at!=stops.rend();++at) if (at->x<x-.01f) return at->position;
    } else for (auto stop:stops) if (stop.x>x+.01f) return stop.position;
    return position;
}
std::vector<Rect> TextSnapshot::selection(std::uint32_t first, std::uint32_t last) const {
    if (first==last) return {};
    UINT32 count{};
    const auto status=impl_->layout->HitTestTextRange(first,last-first,0,0,nullptr,0,&count);
    if (status!=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) check(status);
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    check(impl_->layout->HitTestTextRange(first,last-first,0,0,metrics.data(),count,&count));
    std::vector<Rect> result; result.reserve(count);
    for (const auto& metric:metrics) result.push_back({metric.left,metric.top,metric.width,metric.height});
    return result;
}
} // namespace atelier::ui::detail

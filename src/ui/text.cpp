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

namespace atelier::ui::detail {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT status) { if (FAILED(status)) throw TextFailure{static_cast<std::uint32_t>(status)}; }
std::atomic<std::uint64_t> next_coverage{1};
constexpr std::int64_t max_bitmap_bytes = 16 * 1024 * 1024;

struct Rasterizer final : IDWriteTextRenderer {
    struct Run { RECT bounds{}; std::vector<std::uint8_t> pixels; };
    IDWriteFactory2* factory;
    float scale;
    ULONG refs{1};
    std::vector<Run> runs;
    std::exception_ptr failure;
    Rasterizer(IDWriteFactory2* value, float dpi) : factory(value), scale(dpi) {}
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
    auto value = next_coverage.load(std::memory_order_relaxed);
    do {
        if (value == UINT64_MAX) throw TextFailure{static_cast<std::uint32_t>(E_FAIL)};
    } while (!next_coverage.compare_exchange_weak(value,value+1,std::memory_order_relaxed));
    return value;
}

struct TextStore::Impl {
    struct Paragraph {
        ComPtr<IDWriteTextLayout> layout;
        std::optional<float> width;
        ui_layout::MeasuredSize metrics;
        RasterText raster;
        float raster_scale{};
        bool valid{};
    };
    struct Entry {
        std::wstring text;
        float size{};
        bool wrap{};
        std::uint64_t revision{1};
        std::array<Paragraph, 2> paragraphs;
    };
    ComPtr<IDWriteFactory2> factory;
    std::map<std::uint64_t, Entry> entries;
    std::map<float, ComPtr<IDWriteTextFormat>> formats;
    std::vector<DWRITE_LINE_METRICS> lines;
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
        paragraph.valid = false;
        auto& format = formats[entry.size];
        if (!format) {
            check(factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, entry.size, L"en-US", &format));
        }
        if (!paragraph.layout) {
            check(factory->CreateTextLayout(entry.text.data(), static_cast<UINT32>(entry.text.size()), format.Get(),
                width.value_or(0), 0, &paragraph.layout));
            check(paragraph.layout->SetWordWrapping(width && entry.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP));
        } else check(paragraph.layout->SetMaxWidth(width.value_or(0)));
        DWRITE_TEXT_METRICS metrics{};
        check(paragraph.layout->GetMetrics(&metrics));
        UINT32 count{};
        const auto status = paragraph.layout->GetLineMetrics(nullptr, 0, &count);
        if (status != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) check(status);
        lines.resize(count);
        check(paragraph.layout->GetLineMetrics(lines.data(), count, &count));
        paragraph.metrics = {metrics.widthIncludingTrailingWhitespace, metrics.height, count ? lines[0].baseline : 0};
        paragraph.width = width;
        paragraph.raster = {};
        paragraph.raster_scale = 0;
        paragraph.valid = true;
        return paragraph;
    }
};
TextStore::TextStore() : impl_(std::make_unique<Impl>()) {}
TextStore::~TextStore() = default;
std::uint64_t TextStore::set(std::uint64_t key, std::wstring_view text, float size, bool wrap) {
    auto& entry = impl_->entries[key];
    if (entry.text != text || entry.size != size || entry.wrap != wrap) {
        entry.text = text; entry.size = size; entry.wrap = wrap;
        entry.revision = new_identity(); entry.paragraphs = {};
    }
    return entry.revision;
}
void TextStore::erase(std::uint64_t key) { impl_->entries.erase(key); }
ui_layout::MeasureResult TextStore::measure(void* context, const ui_layout::MeasureQuery& query) {
    auto& impl = *static_cast<TextStore*>(context)->impl_;
    const auto found = impl.entries.find(query.content_key);
    if (found == impl.entries.end() || found->second.revision != query.content_revision || query.provider_revision != 1)
        return std::unexpected(ui_layout::MeasureError{static_cast<std::uint32_t>(E_INVALIDARG)});
    try { return impl.resolve(query.content_key, query.width).metrics; }
    catch (const TextFailure& failure) { return std::unexpected(ui_layout::MeasureError{failure.code}); }
}
RasterText TextStore::raster(std::uint64_t key, float width, float dpi) {
    auto& paragraph = impl_->resolve(key, width);
    if (paragraph.raster_scale == dpi) return paragraph.raster;
    Rasterizer rasterizer(impl_->factory.Get(), dpi);
    const auto result = paragraph.layout->Draw(nullptr, &rasterizer, 0, 0);
    if (rasterizer.failure) std::rethrow_exception(rasterizer.failure);
    check(result);
    RasterText output; output.logical_height = paragraph.metrics.height;
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
        auto coverage = std::make_shared<Coverage>();
        coverage->identity = new_identity();
        coverage->revision = 1;
        coverage->width = coverage->row_pitch = static_cast<std::uint32_t>(width_px);
        coverage->height = static_cast<std::uint32_t>(height_px);
        coverage->pixels.resize(static_cast<std::size_t>(width_px * height_px));
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
        output.coverage = std::move(coverage); output.left = static_cast<float>(bounds.left); output.top = static_cast<float>(bounds.top);
    }
    paragraph.raster = output; paragraph.raster_scale = dpi;
    return output;
}
float TextStore::caret(std::uint64_t key, float width, std::uint32_t position) {
    auto& paragraph = impl_->resolve(key, width);
    FLOAT x{}, y{}; DWRITE_HIT_TEST_METRICS metrics{};
    check(paragraph.layout->HitTestTextPosition(position, FALSE, &x, &y, &metrics));
    return x;
}
} // namespace atelier::ui::detail

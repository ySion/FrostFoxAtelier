#include "ui/text.hpp"
#include <Windows.h>
#include <dwrite_3.h>
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
        return std::tie(a.family,a.locale,a.size,a.weight,a.style) < std::tie(b.family,b.locale,b.size,b.weight,b.style);
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
        TextAlign alignment{TextAlign::left};
        std::uint64_t revision{1};
        std::array<Paragraph, 2> paragraphs;
    };
    ComPtr<IDWriteFactory2> factory;
    ComPtr<IDWriteFontCollection1> font_collection;
    std::map<TextKey, Entry> entries;
    std::map<Font, ComPtr<IDWriteTextFormat>, FontLess> formats;
    std::map<std::pair<TextStore::Symbol,std::uint32_t>,std::shared_ptr<const Coverage>> symbols;
    std::vector<DWRITE_LINE_METRICS> lines;
    std::uint64_t provider_revision{new_identity()};
    TextStatistics statistics;
    std::shared_ptr<CoverageAccounting> accounting{std::make_shared<CoverageAccounting>()};
    explicit Impl(std::span<const std::wstring> font_files) {
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
              reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
        if (!font_files.empty()) {
            ComPtr<IDWriteFactory5> loader; check(factory.As(&loader));
            ComPtr<IDWriteFontSetBuilder1> builder; check(loader->CreateFontSetBuilder(&builder));
            for (const auto& path:font_files) {
                ComPtr<IDWriteFontFile> file; check(loader->CreateFontFileReference(path.c_str(),nullptr,&file));
                check(builder->AddFontFile(file.Get()));
            }
            ComPtr<IDWriteFontSet> system; check(loader->GetSystemFontSet(&system));
            check(builder->AddFontSet(system.Get()));
            ComPtr<IDWriteFontSet> fonts; check(builder->CreateFontSet(&fonts));
            check(loader->CreateFontCollectionFromFontSet(fonts.Get(),&font_collection));
        }
    }
    Paragraph& resolve(TextKey key, std::optional<float> width) {
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
            check(factory->CreateTextFormat(entry.font.family.c_str(), font_collection.Get(), static_cast<DWRITE_FONT_WEIGHT>(entry.font.weight),
                entry.font.style==FontStyle::italic?DWRITE_FONT_STYLE_ITALIC:DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, entry.font.size, entry.font.locale.c_str(), &format));
        }
        check(factory->CreateTextLayout(entry.text.data(), static_cast<UINT32>(entry.text.size()), format.Get(),
            width.value_or(0), 0, &paragraph.layout));
        ++statistics.text_layouts;
        check(paragraph.layout->SetIncrementalTabStop(entry.font.size*2.4f));
        check(paragraph.layout->SetWordWrapping(width && entry.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP));
        if (width) check(paragraph.layout->SetTextAlignment(entry.alignment==TextAlign::right?DWRITE_TEXT_ALIGNMENT_TRAILING:
            entry.alignment==TextAlign::center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING));
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
    TextAlign alignment{TextAlign::left};
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

TextStore::TextStore(std::span<const std::wstring> files) : impl_(std::make_unique<Impl>(files)) {}
TextStore::~TextStore() = default;
std::uint64_t TextStore::set(TextKey key, std::wstring_view text, const Font& font, bool wrap, TextAlign alignment) {
    auto& entry = impl_->entries[key];
    if (entry.text != text || entry.font != font || entry.wrap != wrap || entry.alignment!=alignment) {
        entry.text = text; entry.font = font; entry.wrap = wrap; entry.alignment=alignment;
        entry.revision = new_identity(); entry.paragraphs = {};
    }
    return entry.revision;
}
void TextStore::erase(TextKey key) { impl_->entries.erase(key); }
std::shared_ptr<const Coverage> TextStore::symbol(Symbol symbol, std::uint32_t pixels) {
    if (!pixels || pixels>256) throw TextFailure{static_cast<std::uint32_t>(E_INVALIDARG)};
    const auto key=std::pair{symbol,pixels};
    if (const auto found=impl_->symbols.find(key);found!=impl_->symbols.end()) {
        ++impl_->statistics.coverage_cache_hits; return found->second;
    }
    auto coverage=std::make_shared<TrackedCoverage>(impl_->accounting,pixels,pixels);
    const auto segment=[symbol](float x,float y,float ax,float ay,float bx,float by) {
        const auto dx=bx-ax,dy=by-ay;
        const auto t=std::clamp(((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy),0.f,1.f);
        const auto px=x-ax-t*dx,py=y-ay-t*dy;
        return px*px+py*py<=((symbol==Symbol::world || symbol==Symbol::mesh || symbol==Symbol::folder || symbol>=Symbol::search)?.0016f:.0064f);
    };
    for (std::uint32_t y=0;y<pixels;++y) for (std::uint32_t x=0;x<pixels;++x) {
        unsigned inside{};
        for (unsigned sy=0;sy<4;++sy) for (unsigned sx=0;sx<4;++sx) {
            const auto px=(static_cast<float>(x)+(static_cast<float>(sx)+.5f)/4)/static_cast<float>(pixels);
            const auto py=(static_cast<float>(y)+(static_cast<float>(sy)+.5f)/4)/static_cast<float>(pixels);
            const auto outline=[&](float left,float top,float right,float bottom) {
                return px>=left && px<=right && py>=top && py<=bottom &&
                    (px<left+.1f || px>right-.1f || py<top+.1f || py>bottom-.1f);
            };
            const auto ax=symbol==Symbol::forward?1-px:px;
            const bool hit=(symbol==Symbol::back || symbol==Symbol::forward)?
                ((ax>=.08f && ax<=.53f && std::abs(py-.5f)<(ax-.08f)*.85f) || (ax>=.5f && ax<=.92f && py>=.37f && py<=.63f)):
                symbol==Symbol::world?(std::abs(std::hypot(px-.5f,py-.5f)-.36f)<.035f ||
                    (std::abs(py-.5f)<.025f && std::abs(px-.5f)<.36f) ||
                    (std::abs(std::hypot((px-.5f)*2,py-.5f)-.36f)<.035f)):
                symbol==Symbol::folder?(segment(px,py,.12f,.28f,.40f,.28f) || segment(px,py,.40f,.28f,.5f,.38f) ||
                    segment(px,py,.5f,.38f,.88f,.38f) || segment(px,py,.88f,.38f,.88f,.80f) ||
                    segment(px,py,.88f,.80f,.12f,.80f) || segment(px,py,.12f,.80f,.12f,.28f)):
                symbol==Symbol::mesh?(segment(px,py,.16f,.30f,.50f,.12f) || segment(px,py,.50f,.12f,.84f,.30f) ||
                    segment(px,py,.16f,.30f,.50f,.49f) || segment(px,py,.50f,.49f,.84f,.30f) ||
                    segment(px,py,.16f,.30f,.16f,.70f) || segment(px,py,.84f,.30f,.84f,.70f) ||
                    segment(px,py,.50f,.49f,.50f,.89f) || segment(px,py,.16f,.70f,.50f,.89f) || segment(px,py,.50f,.89f,.84f,.70f)):
                symbol==Symbol::search?(std::abs(std::hypot(px-.4f,py-.4f)-.25f)<.04f || segment(px,py,.59f,.59f,.86f,.86f)):
                symbol==Symbol::view_list?(segment(px,py,.33f,.25f,.86f,.25f) || segment(px,py,.33f,.5f,.86f,.5f) || segment(px,py,.33f,.75f,.86f,.75f) ||
                    (px>=.12f && px<=.22f && (std::abs(py-.25f)<.05f || std::abs(py-.5f)<.05f || std::abs(py-.75f)<.05f))):
                symbol==Symbol::view_details?(outline(.12f,.16f,.88f,.84f) || segment(px,py,.12f,.39f,.88f,.39f) || segment(px,py,.12f,.62f,.88f,.62f) || segment(px,py,.63f,.16f,.63f,.84f)):
                symbol==Symbol::view_tiles?(outline(.12f,.12f,.43f,.43f) || outline(.57f,.12f,.88f,.43f) || outline(.12f,.57f,.43f,.88f) || outline(.57f,.57f,.88f,.88f)):
                symbol==Symbol::filter?(segment(px,py,.13f,.2f,.87f,.2f) || segment(px,py,.13f,.2f,.43f,.55f) || segment(px,py,.87f,.2f,.57f,.55f) ||
                    segment(px,py,.43f,.55f,.43f,.83f) || segment(px,py,.43f,.83f,.57f,.73f) || segment(px,py,.57f,.73f,.57f,.55f)):
                symbol==Symbol::more?(std::hypot(px-.2f,py-.5f)<.075f || std::hypot(px-.5f,py-.5f)<.075f || std::hypot(px-.8f,py-.5f)<.075f):
                symbol==Symbol::pointer?(segment(px,py,.23f,.12f,.23f,.79f) || segment(px,py,.23f,.12f,.78f,.59f) ||
                    segment(px,py,.23f,.79f,.44f,.59f) || segment(px,py,.44f,.59f,.78f,.59f) || segment(px,py,.47f,.64f,.61f,.9f)):
                symbol==Symbol::translate?(segment(px,py,.5f,.1f,.5f,.9f) || segment(px,py,.1f,.5f,.9f,.5f) ||
                    segment(px,py,.5f,.1f,.35f,.27f) || segment(px,py,.5f,.1f,.65f,.27f) ||
                    segment(px,py,.9f,.5f,.73f,.35f) || segment(px,py,.9f,.5f,.73f,.65f) ||
                    segment(px,py,.5f,.9f,.35f,.73f) || segment(px,py,.1f,.5f,.27f,.65f)):
                symbol==Symbol::rotate?((std::abs(std::hypot(px-.5f,py-.5f)-.32f)<.04f && !(px>.5f && py<.35f)) ||
                    segment(px,py,.8f,.33f,.61f,.32f) || segment(px,py,.8f,.33f,.81f,.14f)):
                symbol==Symbol::scale?(outline(.1f,.61f,.38f,.89f) || outline(.66f,.1f,.9f,.34f) ||
                    segment(px,py,.34f,.66f,.72f,.28f)):
                symbol==Symbol::cursor3d?(std::abs(std::hypot(px-.5f,py-.5f)-.25f)<.04f ||
                    segment(px,py,.5f,.07f,.5f,.93f) || segment(px,py,.07f,.5f,.93f,.5f)):
                symbol==Symbol::caret?(py>=.42f && py<=.68f && std::abs(px-.5f)<=(.68f-py)*1.15f):
                symbol==Symbol::magnet?((py>=.5f && std::abs(std::hypot(px-.5f,py-.5f)-.28f)<.09f) ||
                    (py>=.13f && py<=.5f && ((px>=.13f && px<=.31f) || (px>=.69f && px<=.87f)))):
                (symbol==Symbol::radio || symbol==Symbol::radio_checked)?(std::abs(std::hypot(px-.5f,py-.5f)-.33f)<.04f ||
                    (symbol==Symbol::radio_checked && std::hypot(px-.5f,py-.5f)<.15f)):
                (symbol==Symbol::checkbox || symbol==Symbol::checkbox_checked)?(outline(.13f,.13f,.87f,.87f) ||
                    (symbol==Symbol::checkbox_checked && (segment(px,py,.28f,.51f,.44f,.68f) || segment(px,py,.44f,.68f,.74f,.32f)))):
                symbol==Symbol::object?((px>=.18f && px<=.82f && py>=.28f && py<=.75f) || (py>=.12f && py<.28f && std::abs(px-.5f)<(py-.08f)*2)):
                symbol==Symbol::axes?(segment(px,py,.45f,.55f,.85f,.75f) || segment(px,py,.45f,.55f,.15f,.85f) || segment(px,py,.45f,.55f,.45f,.10f)):
                symbol==Symbol::component?((px-.5f)*(px-.5f)+(py-.5f)*(py-.5f)<.16f && (px-.5f)*(px-.5f)+(py-.5f)*(py-.5f)>.06f):
                symbol==Symbol::close?(segment(px,py,.2f,.2f,.8f,.8f) || segment(px,py,.8f,.2f,.2f,.8f)):
                symbol==Symbol::plus?(segment(px,py,.2f,.5f,.8f,.5f) || segment(px,py,.5f,.2f,.5f,.8f)):
                symbol==Symbol::maximize?(px>=.15f && px<=.85f && py>=.225f && py<=.775f && px+py<1):
                symbol==Symbol::restore?(outline(.1f,.3f,.7f,.9f) || ((py<.3f || px>.7f) && outline(.3f,.1f,.9f,.7f))):
                symbol==Symbol::check?
                (segment(px,py,.17f,.53f,.40f,.77f) || segment(px,py,.40f,.77f,.85f,.23f)):
                symbol==Symbol::disclosure_right?(px>=.2f && px<=.8f && std::abs(py-.5f)<(.8f-px)*.75f):
                symbol==Symbol::disclosure_down?(py>=.2f && py<=.8f && std::abs(px-.5f)<(.8f-py)*.75f):
                symbol==Symbol::left?(px>=.25f && px<=.7f && std::abs(py-.5f)<(px-.25f)*.7f):
                symbol==Symbol::up?(py>=.25f && py<=.7f && std::abs(px-.5f)<(py-.25f)*.7f):
                symbol==Symbol::right?(px>=.3f && px<=.75f && std::abs(py-.5f)<(.75f-px)*.7f):
                (py>=.3f && py<=.75f && std::abs(px-.5f)<(.75f-py)*.7f);
            inside+=hit?1u:0u;
        }
        coverage->pixels[static_cast<std::size_t>(y)*pixels+x]=static_cast<std::uint8_t>((inside*255+8)/16);
    }
    ++impl_->statistics.rasterizations; impl_->statistics.raster_bytes+=coverage->pixels.size();
    impl_->symbols.emplace(key,coverage); return coverage;
}
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
ui_layout::MeasuredSize TextStore::metrics(TextKey key, float width) { return impl_->resolve(key,width).metrics; }
RasterText TextStore::raster(TextKey key, float width, float dpi, std::optional<Clip> viewport) {
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
std::shared_ptr<const TextSnapshot> TextStore::snapshot(TextKey key, float width) {
    auto& paragraph=impl_->resolve(key,width);
    if (!paragraph.snapshot) {
        auto data=std::make_shared<TextSnapshot::Impl>();
        data->layout=paragraph.layout; data->text=impl_->entries.at(key).text;
        data->font=impl_->entries.at(key).font; data->alignment=impl_->entries.at(key).alignment; data->width=width;
        paragraph.snapshot=std::shared_ptr<const TextSnapshot>(new TextSnapshot(std::move(data)));
    }
    return paragraph.snapshot;
}
std::shared_ptr<const TextSnapshot> TextStore::edited_snapshot(std::uint64_t key, const TextSnapshot& displayed, std::wstring_view value) {
    const auto& policy=*displayed.impl_;
    set(key,value,policy.font,false,policy.alignment);
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

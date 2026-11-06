#include "memory/fatal.hpp"
#include "ui_layout/layout.hpp"

#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace atelier::ui_layout;

using Microsoft::WRL::ComPtr;

namespace {
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void check_error(const Error& error) {
    throw std::runtime_error(std::string(describe(error.code)) + ": " + std::string(describe(error.field)) +
                             "; provider=" + std::to_string(error.provider_code));
}

void accepted(Result<void> result) {
    if (!result)
        check_error(result.error());
}

template <class T>
T accepted(Result<T> result) {
    if (!result)
        check_error(result.error());
    return std::move(*result);
}

bool close(float a, float b) {
    return std::abs(a - b) <= 0.0002f + 0.000002f * std::max(std::abs(a), std::abs(b));
}

const LayoutBox& box(LayoutView view, LayoutNodeId id) {
    const auto* found = view.find(id);
    require(found != nullptr, "published node missing");
    return *found;
}

NodeInput fixed_box(float width, float height) {
    NodeInput input;
    input.width = Axis::fixed(width);
    input.height = Axis::fixed(height);
    return input;
}

NodeInput text(std::uint64_t key, std::uint64_t revision = 1) {
    NodeInput input;
    input.kind = Kind::measured;
    input.content_key = key;
    input.content_revision = revision;
    return input;
}

NodeInput column() {
    NodeInput input;
    input.kind = Kind::column;
    return input;
}

void hr(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("DirectWrite HRESULT " + std::to_string(static_cast<unsigned long>(result)));
}

struct TextProvider {
    struct Content {
        std::wstring text;

        float font_size{16};

        std::uint64_t revision{1};
    };

    struct Paragraph {
        ComPtr<IDWriteTextLayout> layout;

        std::uint64_t content_revision{};

        std::uint64_t provider_revision{};
    };

    ComPtr<IDWriteFactory> factory;

    std::map<std::uint64_t, Content> content;

    std::map<std::uint64_t, Paragraph> paragraphs;

    std::uint64_t calls{};

    std::uint64_t fail_at{};

    std::uint64_t epoch{1};

    float scale{1};

    TextProvider() {
        hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
        ComPtr<IDWriteFontCollection> fonts;
        hr(factory->GetSystemFontCollection(fonts.GetAddressOf()));
        UINT32 index{};
        BOOL found{};
        hr(fonts->FindFamilyName(L"Segoe UI", &index, &found));
        require(found != FALSE, "Segoe UI unavailable");
    }

    ComPtr<IDWriteTextLayout> create(const Content& value) {
        ComPtr<IDWriteTextFormat> format;
        hr(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, value.font_size * scale, L"en-US",
                                     format.GetAddressOf()));
        hr(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        ComPtr<IDWriteTextLayout> result;
        hr(factory->CreateTextLayout(value.text.data(), static_cast<UINT32>(value.text.size()), format.Get(), 0, 0,
                                     result.GetAddressOf()));
        return result;
    }

    Size metrics(IDWriteTextLayout* layout, std::optional<float> width) {
        const auto wrapping = width ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP;
        if (layout->GetWordWrapping() != wrapping)
            hr(layout->SetWordWrapping(wrapping));
        if (layout->GetMaxWidth() != width.value_or(0))
            hr(layout->SetMaxWidth(width.value_or(0)));
        DWRITE_TEXT_METRICS value{};
        hr(layout->GetMetrics(&value));
        return {value.widthIncludingTrailingWhitespace, value.height};
    }

    Size fresh(std::uint64_t key, std::optional<float> width) {
        const auto layout = create(content.at(key));
        return metrics(layout.Get(), width);
    }

    static MeasureResult measure(void* context, const MeasureQuery& query) {
        auto& self = *static_cast<TextProvider*>(context);
        ++self.calls;
        if (self.fail_at && self.calls == self.fail_at)
            return std::unexpected(MeasureError{77});
        const auto found = self.content.find(query.content_key);
        if (found == self.content.end() || found->second.revision != query.content_revision ||
            query.provider_revision != self.epoch)
            return std::unexpected(MeasureError{91});
        auto& paragraph = self.paragraphs[query.content_key];
        if (!paragraph.layout || paragraph.content_revision != query.content_revision ||
            paragraph.provider_revision != query.provider_revision)
            paragraph = {self.create(found->second), query.content_revision, query.provider_revision};
        const auto size = self.metrics(paragraph.layout.Get(), query.width);
        return MeasuredSize{size.width, size.height, std::nullopt};
    }
};

void bounded_allocation() {
    LayoutDocument d(nullptr, nullptr, {3, 1});
    accepted(d.set_root({Kind::row}));
    auto first = fixed_box(0, 1);
    auto second = first;
    first.width = Axis::expand(1, 80);
    second.width = Axis::expand(1, 0, 10);
    const auto a = accepted(d.create(d.root(), first));
    const auto b = accepted(d.create(d.root(), second));
    const auto result = accepted(d.compute({100, 10}, 1));
    require(box(result, a).width == 90 && box(result, b).width == 10, "minimum incorrectly frozen with upper cap");
}

void slots_padding_and_columns() {
    LayoutDocument d(nullptr, nullptr, {3, 1});
    accepted(d.set_root({Kind::row, {2, 3, 2, 3}, 4}));
    auto first = fixed_box(0, 10);
    auto second = first;
    first.width = Axis::expand(1, 20);
    first.horizontal_alignment = Alignment::fit(.5f);
    first.vertical_alignment = Alignment::stretch(.5f);
    second.width = Axis::expand();
    const auto a = accepted(d.create(d.root(), first));
    const auto b = accepted(d.create(d.root(), second));
    const auto result = accepted(d.compute({108, 40}, 1));
    require(box(result, a).width == 20 && box(result, a).x == 17 && box(result, a).y == 15 && box(result, b).x == 56 &&
                box(result, b).width == 50,
            "padding/slot alignment");
}

void known_width_natural_height() {
    TextProvider p;
    p.content[1] = {L"这是没有空格的中文内容，用于验证已知宽度下的自然高度，而不是视口高度。", 20, 1};
    LayoutDocument d(TextProvider::measure, &p, {4, 2});
    const auto id = accepted(d.create(d.root(), text(1)));
    auto result = accepted(d.compute({100, 40}, p.epoch));
    const float natural = p.fresh(1, 100).height;
    require(close(box(result, id).height, natural) && natural > 40, "natural height truncated to viewport");
    p.scale = 1.25f;
    ++p.epoch;
    result = accepted(d.compute({220, 80}, p.epoch));
    require(close(box(result, id).height, p.fresh(1, 220).height),
            "font/provider version did not invalidate measurement");
}

void transactional_failure_and_reentry() {
    TextProvider p;
    for (std::uint64_t i = 1; i <= 3; ++i)
        p.content[i] = {L"A real paragraph with width-dependent height.", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {5, 2});
    std::array<LayoutNodeId, 3> ids;
    for (unsigned i = 0; i < 3; ++i)
        ids[i] = accepted(d.create(d.root(), text(i + 1)));
    const auto initial = accepted(d.compute({180, 300}, p.epoch));
    const auto owned = d.snapshot();
    for (unsigned i = 0; i < 3; ++i) {
        ++p.content[i + 1].revision;
        p.content[i + 1].text += L" Changed.";
        accepted(d.update(ids[i], text(i + 1, 2)));
    }
    p.fail_at = p.calls + 3;
    const auto failed = d.compute({180, 300}, p.epoch);
    require(!failed, "injected measurement failure succeeded");
    require(d.view().publication == initial.publication && d.view().input_revision == initial.input_revision &&
                d.view().boxes.data() == initial.boxes.data() && owned.find(ids[0]),
            "failed compute changed publication");
    p.fail_at = 0;
    accepted(d.compute({180, 300}, p.epoch));
}

void anchors_and_logical_precision() {
    LayoutDocument d(nullptr, nullptr, {6, 3});
    accepted(d.set_root({Kind::overlay}));
    auto corner = fixed_box(80, 40);
    corner.anchor_x = 1;
    corner.anchor_y = 1;
    corner.pivot_x = 1;
    corner.pivot_y = 1;
    corner.offset_x = -10;
    corner.offset_y = -10;
    const auto a = accepted(d.create(d.root(), corner));
    auto inner = fixed_box(100, 100);
    inner.kind = Kind::overlay;
    inner.offset_x = .4f;
    inner.offset_y = .4f;
    const auto b = accepted(d.create(d.root(), inner));
    auto leaf = fixed_box(10, 10);
    leaf.offset_x = .4f;
    leaf.offset_y = .4f;
    const auto c = accepted(d.create(b, leaf));
    const auto result = accepted(d.compute({800, 450}, 1));
    require(box(result, a).x == 710 && box(result, a).y == 400 && close(box(result, b).x + box(result, c).x, .8f),
            "anchors or unrounded local geometry");
}

void owned_geometry_lifetime() {
    LayoutSnapshot owned;
    {
        LayoutDocument d(nullptr, nullptr, {3, 1});
        accepted(d.set_root({Kind::overlay}));
        auto value = fixed_box(10.25f, 5.5f);
        value.offset_x = 2.25f;
        const auto id = accepted(d.create(d.root(), value));
        const auto first = accepted(d.compute({100, 100}, 1));
        owned = d.snapshot();
        value.offset_x = 9.5f;
        accepted(d.update(id, value));
        require(d.view().boxes.data() == first.boxes.data() && box(d.view(), id).x == 2.25f,
                "editing replaced borrowed publication");
        const auto second = accepted(d.compute({100, 100}, 1));
        require(box(second, id).x == 9.5f && owned.boxes[1].x == 2.25f, "snapshot changed after publication");
    }
    require(owned.boxes.size() == 2 && owned.boxes[1].x == 2.25f && owned.boxes[1].width == 10.25f &&
                owned.boxes[1].height == 5.5f,
            "snapshot did not survive document destruction");
}

#include "practical.inl"
#include "scaling.inl"
#include "flex_grid.inl"
#include "audit.inl"
#include "topology.inl"
}

int main() {
    atelier::install_fatal_handler();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        const std::pair<const char*, void (*)()> cases[]{
            {"bounded_allocation", bounded_allocation},
            {"slots_padding_and_columns", slots_padding_and_columns},
            {"known_width_natural_height", known_width_natural_height},
            {"transactional_failure_and_reentry", transactional_failure_and_reentry},
            {"anchors_and_logical_precision", anchors_and_logical_precision},
            {"owned_geometry_lifetime", owned_geometry_lifetime},
            {"shared_columns_structure_and_intrinsic", shared_columns_structure_and_intrinsic},
            {"wrap_flow_and_repeated_grid", wrap_flow_and_repeated_grid},
            {"virtual_range_and_anchor", virtual_range_and_anchor},
            {"flex_allocation_and_distribution", flex_allocation_and_distribution},
            {"flex_wrapping_and_alignment", flex_wrapping_and_alignment},
            {"grid_tracks_spans_and_placement", grid_tracks_spans_and_placement},
            {"grid_collision_boundaries", grid_collision_boundaries},
            {"final_tree_transactions", final_tree_transactions}};
        unsigned passed = 0;
        unsigned failed = 0;
        for (const auto& [name, run] : cases) {
            try {
                run();
                ++passed;
                std::printf("{\"kind\":\"case\",\"name\":\"%s\",\"status\":\"PASS\"}\n", name);
            } catch (const std::exception& error) {
                ++failed;
                std::printf("{\"kind\":\"case\",\"name\":\"%s\",\"status\":\"FAIL\",\"error\":\"%s\"}\n", name,
                            error.what());
            }
        }
        std::printf("{\"kind\":\"summary\",\"passed\":%u,\"failed\":%u}\nUI layout acceptance: %s\n", passed, failed,
                    failed ? "FAIL" : "PASS");
        return failed ? 2 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UI layout acceptance fatal: %s\n", error.what());
        return 3;
    }
}

#include "ui_layout/layout.hpp"

#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <mimalloc.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <map>
#include <new>
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

template <class T>
void rejected(const Result<T>& result, ErrorCode code) {
    require(!result && result.error().code == code, "wrong rejection or unexpected success");
}

bool close(float a, float b) {
    return std::abs(a - b) <= 0.0002f + 0.000002f * std::max(std::abs(a), std::abs(b));
}

const LayoutBox& box(LayoutView view, LayoutNodeId id) {
    const auto* found = view.find(id);
    require(found != nullptr, "published node missing");
    return *found;
}

void same_geometry(LayoutView a, LayoutView b) {
    require(a.boxes.size() == b.boxes.size(), "fresh result has different size");
    for (std::size_t i = 0; i < a.boxes.size(); ++i) {
        const auto& x = a.boxes[i];
        const auto& y = b.boxes[i];
        require(close(x.x, y.x) && close(x.y, y.y) && close(x.width, y.width) && close(x.height, y.height),
                "warm/fresh geometry differs");
        const auto parent_position = [](LayoutView view, LayoutNodeId parent) {
            return std::find_if(view.boxes.begin(), view.boxes.end(),
                                [parent](const auto& value) { return value.node == parent; }) -
                   view.boxes.begin();
        };
        require(parent_position(a, x.parent) == parent_position(b, y.parent), "warm/fresh topology differs");
    }
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

    struct Resolved {
        std::shared_ptr<const Paragraph> paragraph;

        Size size;
    };

    ComPtr<IDWriteFactory> factory;

    std::map<std::uint64_t, Content> content;

    std::map<std::uint64_t, std::shared_ptr<Paragraph>> paragraphs;

    std::vector<DWRITE_LINE_METRICS> line_metrics;

    std::uint64_t calls{};

    std::uint64_t creates{};

    std::uint64_t fail_at{};

    std::uint64_t epoch{1};

    std::uint64_t measure_nanoseconds{};

    struct Costs {
        std::uint64_t format{};

        std::uint64_t create{};

        std::uint64_t wrapping{};

        std::uint64_t width{};

        std::uint64_t metrics{};
    } costs;

    std::wstring font_family{L"Segoe UI"};

    bool wrap_enabled{true};

    float scale{1};

    bool invalid{};

    bool exception{};

    bool allocation_exception{};

    bool try_reentry{};

    bool reentry_rejected{};

    LayoutDocument* active{};

    LayoutNodeId active_leaf{};

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
        timed(costs.format, [&] {
            return factory->CreateTextFormat(font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             value.font_size * scale, L"en-US", format.GetAddressOf());
        });
        timed(costs.format, [&] { return format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); });
        ComPtr<IDWriteTextLayout> result;
        timed(costs.create, [&] {
            return factory->CreateTextLayout(value.text.data(), static_cast<UINT32>(value.text.size()), format.Get(), 0,
                                             0, result.GetAddressOf());
        });
        ++creates;
        return result;
    }

    template <class Action>
    static void timed(std::uint64_t& total, Action action) {
        const auto start = std::chrono::steady_clock::now();
        hr(action());
        total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }

    Size metrics(IDWriteTextLayout* layout, std::optional<float> width) {
        const auto wrapping = width && wrap_enabled ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP;
        if (layout->GetWordWrapping() != wrapping)
            timed(costs.wrapping, [&] { return layout->SetWordWrapping(wrapping); });
        if (layout->GetMaxWidth() != width.value_or(0))
            timed(costs.width, [&] { return layout->SetMaxWidth(width.value_or(0)); });
        DWRITE_TEXT_METRICS value{};
        timed(costs.metrics, [&] { return layout->GetMetrics(&value); });
        return {value.widthIncludingTrailingWhitespace, value.height};
    }

    Size fresh(std::uint64_t key, std::optional<float> width) {
        const auto layout = create(content.at(key));
        return metrics(layout.Get(), width);
    }

    float baseline(IDWriteTextLayout* layout) {
        UINT32 count{};
        const auto status = layout->GetLineMetrics(nullptr, 0, &count);
        if (status != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
            hr(status);
        line_metrics.resize(count);
        hr(layout->GetLineMetrics(line_metrics.data(), count, &count));
        return count ? line_metrics[0].baseline : 0;
    }

    std::expected<Resolved, MeasureError> resolve(const MeasureQuery& query) {
        const auto found = content.find(query.content_key);
        if (found == content.end() || found->second.revision != query.content_revision ||
            query.provider_revision != epoch)
            return std::unexpected(MeasureError{91});
        auto& paragraph = paragraphs[query.content_key];
        const auto wrapping = query.width && wrap_enabled ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP;
        // A frame owns its paragraph version. AddRef alone would still let a
        // later SetMaxWidth change the old frame's text. Copy only before mutation.
        if (!paragraph || paragraph->content_revision != query.content_revision ||
            paragraph->provider_revision != query.provider_revision ||
            (paragraph.use_count() > 1 && (paragraph->layout->GetMaxWidth() != query.width.value_or(0) ||
                                           paragraph->layout->GetWordWrapping() != wrapping))) {
            paragraph = std::make_shared<Paragraph>(
                Paragraph{create(found->second), query.content_revision, query.provider_revision});
        }
        const auto size = metrics(paragraph->layout.Get(), query.width);
        return Resolved{paragraph, size};
    }

    static MeasureResult measure(void* context, const MeasureQuery& query) {
        auto& self = *static_cast<TextProvider*>(context);
        ++self.calls;
        struct Timer {
            std::uint64_t& total;

            std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};

            ~Timer() {
                total += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                        .count());
            }
        } timer{self.measure_nanoseconds};
        if (self.try_reentry) {
            self.try_reentry = false;
            rejected(self.active->create(self.active->root(), fixed_box(1, 1)), ErrorCode::busy);
            rejected(self.active->update(self.active_leaf, text(query.content_key, query.content_revision)),
                     ErrorCode::busy);
            rejected(self.active->move(self.active_leaf, self.active->root()), ErrorCode::busy);
            rejected(self.active->remove(self.active_leaf), ErrorCode::busy);
            rejected(self.active->set_tree({}), ErrorCode::busy);
            rejected(self.active->set_root({Kind::row}), ErrorCode::busy);
            rejected(self.active->compute({1, 1}, query.provider_revision), ErrorCode::busy);
            require(self.active->view().publication != 0, "callback cannot read old publication");
            self.reentry_rejected = true;
        }
        if (self.allocation_exception)
            throw std::bad_alloc();
        if (self.exception)
            throw std::runtime_error("provider exception");
        if (self.invalid)
            return Size{0, std::numeric_limits<float>::quiet_NaN()};
        if (self.fail_at && self.calls == self.fail_at)
            return std::unexpected(MeasureError{77});
        auto result = self.resolve(query);
        if (!result)
            return std::unexpected(result.error());
        return MeasuredSize{result->size.width, result->size.height,
                            query.first_baseline ? std::optional<float>(self.baseline(result->paragraph->layout.Get()))
                                                 : std::nullopt};
    }
};

// Consumer-owned output: the core snapshot deliberately owns geometry only.
struct TextFrame {
    struct Run {
        LayoutNodeId node;

        MeasureQuery query;

        TextProvider::Resolved text;
    };

    LayoutSnapshot geometry;

    std::vector<Run> runs;

    static std::shared_ptr<const TextFrame> capture(LayoutDocument& document, TextProvider& provider) {
        auto frame = std::make_shared<TextFrame>();
        frame->geometry = document.snapshot();
        for (const auto& bounds : frame->geometry.boxes) {
            if (bounds.node == document.root())
                continue;
            const auto input = accepted(document.input(bounds.node));
            if (input.kind != Kind::measured)
                continue;
            const MeasureQuery query{input.content_key, input.content_revision, frame->geometry.provider_revision,
                                     std::max(0.f, bounds.width - input.padding.left - input.padding.right)};
            // A fixed-size node may never be measured; two nodes may also share
            // content but have different widths. Resolve each final constraint.
            auto resolved = provider.resolve(query);
            require(resolved.has_value(), "frame text resolution failed");
            frame->runs.push_back({bounds.node, query, std::move(*resolved)});
        }
        return frame;
    }

    void check() const {
        for (const auto& run : runs) {
            const auto& paragraph = *run.text.paragraph;
            require(geometry.find(run.node) && paragraph.content_revision == run.query.content_revision &&
                        paragraph.provider_revision == geometry.provider_revision &&
                        paragraph.layout->GetMaxWidth() == *run.query.width,
                    "retained text version or width changed");
            DWRITE_TEXT_METRICS current{};
            hr(paragraph.layout->GetMetrics(&current));
            require(close(current.widthIncludingTrailingWhitespace, run.text.size.width) &&
                        close(current.height, run.text.size.height),
                    "retained frame text metrics changed");
        }
    }
};

struct ComputeCosts {
    std::uint64_t total{};

    std::uint64_t measure{};

    LayoutView run(LayoutDocument& document, TextProvider& provider, Size viewport) {
        const auto before = provider.measure_nanoseconds;
        const auto start = std::chrono::steady_clock::now();
        auto result = document.compute(viewport, provider.epoch);
        total += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        measure += provider.measure_nanoseconds - before;
        return accepted(std::move(result));
    }
};

void identity_and_reuse() {
    LayoutDocument a(nullptr, nullptr, {4, 3});
    LayoutDocument b(nullptr, nullptr, {4, 3});
    const auto parent = accepted(a.create(a.root(), column()));
    const auto child = accepted(a.create(parent, fixed_box(20, 10)));
    require(a.root() != b.root() && a.node_count() == 3, "document identities or count");
    rejected(b.create(parent, fixed_box(1, 1)), ErrorCode::invalid_node);
    rejected(b.update(child, fixed_box(1, 1)), ErrorCode::invalid_node);
    rejected(b.move(b.root(), parent), ErrorCode::invalid_node);
    rejected(a.input({}), ErrorCode::invalid_node);
    rejected(a.remove(a.root()), ErrorCode::root_operation);
    rejected(a.move(a.root(), parent), ErrorCode::root_operation);
    rejected(a.update(a.root(), column()), ErrorCode::root_operation);
    const auto first = accepted(a.compute({100, 50}, 1));
    const auto saved = a.snapshot();
    accepted(a.remove(parent));
    require(a.node_count() == 1, "subtree removal count");
    rejected(a.input(parent), ErrorCode::invalid_node);
    rejected(a.input(child), ErrorCode::invalid_node);
    require(a.view().publication == first.publication && a.view().boxes.data() == first.boxes.data(),
            "remove invalidated old view");
    const auto reused = accepted(a.create(a.root(), fixed_box(7, 9)));
    require(reused != parent && reused != child, "slot reuse resurrected an identity");
    rejected(a.update(child, fixed_box(3, 3)), ErrorCode::invalid_node);
    const auto next = accepted(a.compute({100, 50}, 1));
    require(next.boxes.size() == 2 && box(next, reused).width == 7 && saved.boxes.size() == 3,
            "old/new geometry separation");
    LayoutNodeId expired;
    {
        LayoutDocument temporary(nullptr, nullptr, {2, 1});
        expired = accepted(temporary.create(temporary.root(), fixed_box(1, 1)));
    }
    rejected(b.input(expired), ErrorCode::invalid_node);
}

void atomic_ordered_edits() {
    LayoutDocument d(nullptr, nullptr, {12, 8});
    accepted(d.set_root({Kind::row}));
    const auto a = accepted(d.create(d.root(), fixed_box(10, 10)));
    const auto b = accepted(d.create(d.root(), fixed_box(20, 10)));
    const auto c = accepted(d.create(d.root(), fixed_box(30, 10), 1));
    auto view = accepted(d.compute({100, 20}, 1));
    require(view.boxes[1].node == a && view.boxes[2].node == c && view.boxes[3].node == b, "insertion order");
    auto revision = d.input_revision();
    rejected(d.move(a, d.root(), 3), ErrorCode::invalid_input);
    rejected(d.create(a, fixed_box(1, 1)), ErrorCode::invalid_structure);
    require(d.input_revision() == revision, "invalid edit changed revision");
    accepted(d.move(a, d.root(), 2));
    view = accepted(d.compute({100, 20}, 1));
    require(view.boxes[1].node == c && view.boxes[2].node == b && view.boxes[3].node == a && box(view, a).x == 50,
            "same-parent move semantics");
    revision = d.input_revision();
    const auto publication = view.publication;
    accepted(d.move(a, d.root()));
    accepted(d.update(a, fixed_box(10, 10)));
    accepted(d.set_root({Kind::row}));
    require(d.input_revision() == revision && accepted(d.compute({100, 20}, 1)).publication == publication,
            "no-op edits dirtied input");
    const auto parent = accepted(d.create(d.root(), column()));
    accepted(d.move(a, parent));
    rejected(d.move(parent, a), ErrorCode::invalid_structure);
    rejected(d.update(parent, fixed_box(1, 1)), ErrorCode::invalid_structure);
    view = accepted(d.compute({100, 20}, 1));
    require(box(view, a).parent == parent, "cross-parent move identity");
    const auto nested = accepted(d.create(parent, column()));
    rejected(d.move(parent, nested), ErrorCode::invalid_structure);
    accepted(d.remove(parent));
    rejected(d.input(a), ErrorCode::invalid_node);
    rejected(d.input(nested), ErrorCode::invalid_node);
}

void capacity_and_depth() {
    bool threw = false;
    try {
        LayoutDocument invalid(nullptr, nullptr, {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "zero slot capacity accepted");
    LayoutDocument capacity(nullptr, nullptr, {2, 1});
    const auto old = accepted(capacity.create(capacity.root(), fixed_box(1, 1)));
    const auto revision = capacity.input_revision();
    rejected(capacity.create(capacity.root(), fixed_box(1, 1)), ErrorCode::capacity_exceeded);
    require(capacity.input_revision() == revision, "capacity rejection edited input");
    accepted(capacity.remove(old));
    const auto fresh = accepted(capacity.create(capacity.root(), fixed_box(2, 2)));
    require(fresh != old, "bounded slot pool did not advance generation");
    LayoutDocument depth(nullptr, nullptr, {12, 3});
    const auto branch = accepted(depth.create(depth.root(), column()));
    const auto descendant = accepted(depth.create(branch, column()));
    const auto target = accepted(depth.create(depth.root(), column()));
    const auto deeper = accepted(depth.create(target, column()));
    const auto old_revision = depth.input_revision();
    rejected(depth.move(branch, deeper), ErrorCode::capacity_exceeded);
    require(depth.input_revision() == old_revision, "depth-invalid move was not atomic");
    const auto bottom = accepted(depth.create(descendant, column()));
    rejected(depth.create(bottom, fixed_box(1, 1)), ErrorCode::capacity_exceeded);
    accepted(depth.remove(bottom));
    accepted(depth.move(branch, target));
    rejected(depth.create(descendant, fixed_box(1, 1)), ErrorCode::capacity_exceeded);
}

void bounded_allocation() {
    std::array<unsigned, 3> order{0, 1, 2};
    const std::array<float, 3> expected{120, 150, 30};
    do {
        LayoutDocument d(nullptr, nullptr, {4, 1});
        accepted(d.set_root({Kind::row}));
        std::array<LayoutNodeId, 3> ids;
        for (const auto i : order) {
            auto value = fixed_box(0, 10);
            value.width = Axis::expand(1, 0, i == 1 ? std::nullopt : std::optional<float>(expected[i]));
            ids[i] = accepted(d.create(d.root(), value));
        }
        const auto result = accepted(d.compute({300, 20}, 1));
        for (unsigned i = 0; i < 3; ++i)
            require(close(box(result, ids[i]).width, expected[i]), "saturation order dependence");
    } while (std::next_permutation(order.begin(), order.end()));
    LayoutDocument d(nullptr, nullptr, {3, 1});
    accepted(d.set_root({Kind::row}));
    auto first = fixed_box(0, 1);
    auto second = first;
    first.width = Axis::expand(1, 80);
    second.width = Axis::expand();
    const auto a = accepted(d.create(d.root(), first));
    const auto b = accepted(d.create(d.root(), second));
    auto result = accepted(d.compute({100, 10}, 1));
    require(box(result, a).width == 80 && box(result, b).width == 20, "whole-pool minimum policy");
    result = accepted(d.compute({40, 10}, 1));
    require(box(result, a).width == 80 && box(result, b).width == 0, "hard minimum overflow policy");
    second.width.maximum = 10;
    accepted(d.update(b, second));
    result = accepted(d.compute({100, 10}, 1));
    require(box(result, a).width == 90 && box(result, b).width == 10, "minimum incorrectly frozen with upper cap");
    first.width = Axis::expand(1, 0, 30);
    second.width = Axis::expand(1, 0, 20);
    accepted(d.update(a, first));
    accepted(d.update(b, second));
    result = accepted(d.compute({100, 10}, 1));
    require(box(result, a).width == 30 && box(result, b).width == 20, "saturated surplus forced into child");
    first.width = Axis::expand(1);
    second.width = Axis::expand(2);
    accepted(d.update(a, first));
    accepted(d.update(b, second));
    result = accepted(d.compute({90, 10}, 1));
    require(box(result, a).width == 30 && box(result, b).width == 60, "weighted pool");
}

void weight_dynamic_range() {
    std::array<unsigned, 3> order{0, 1, 2};
    const std::array<float, 3> weights{1e30f, 1, 1e-30f};
    const std::array<float, 3> expected{1, 1, 98};
    do {
        LayoutDocument d(nullptr, nullptr, {4, 1});
        accepted(d.set_root({Kind::row}));
        std::array<LayoutNodeId, 3> ids;
        for (const auto i : order) {
            auto value = fixed_box(0, 1);
            value.width = Axis::expand(weights[i], 0, i < 2 ? std::optional<float>(1) : std::nullopt);
            ids[i] = accepted(d.create(d.root(), value));
        }
        const auto result = accepted(d.compute({100, 10}, 1));
        for (unsigned i = 0; i < 3; ++i)
            require(close(box(result, ids[i]).width, expected[i]), "small active weight lost after caps");
    } while (std::next_permutation(order.begin(), order.end()));
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
    auto result = accepted(d.compute({108, 40}, 1));
    require(box(result, a).width == 20 && box(result, a).x == 17 && box(result, a).y == 15 && box(result, b).x == 56 &&
                box(result, b).width == 50,
            "padding/slot alignment");
    accepted(d.set_root({Kind::column}));
    first.width = Axis::fixed(10);
    first.height = Axis::expand(1, 80);
    second.height = Axis::expand();
    accepted(d.update(a, first));
    accepted(d.update(b, second));
    result = accepted(d.compute({100, 100}, 1));
    require(box(result, a).height == 80 && box(result, b).height == 20 && box(result, b).y == 80,
            "column weighted slots");
}

void known_width_natural_height() {
    TextProvider p;
    p.content[1] = {L"这是没有空格的中文内容，用于验证已知宽度下的自然高度，而不是视口高度。", 20, 1};
    LayoutDocument d(TextProvider::measure, &p, {4, 2});
    auto input = text(1);
    const auto id = accepted(d.create(d.root(), input));
    auto result = accepted(d.compute({100, 40}, p.epoch));
    const float natural = p.fresh(1, 100).height;
    require(close(box(result, id).height, natural) && natural > 40, "natural height truncated to viewport");
    const auto* address = result.boxes.data();
    const auto publication = result.publication;
    const auto stats = d.stats();
    result = accepted(d.compute({100, 40}, p.epoch));
    require(result.boxes.data() == address && result.publication == publication &&
                d.stats().node_visits == stats.node_visits && d.stats().measure_calls == stats.measure_calls,
            "clean compute repeated work");
    result = accepted(d.compute({100, 80}, p.epoch));
    require(close(box(result, id).height, natural) && d.stats().measure_calls == stats.measure_calls,
            "viewport height invalidated H(W)");
    result = accepted(d.compute({220, 80}, p.epoch));
    require(box(result, id).height < natural && close(box(result, id).height, p.fresh(1, 220).height),
            "width did not change natural height");
    p.scale = 1.25f;
    ++p.epoch;
    result = accepted(d.compute({220, 80}, p.epoch));
    require(close(box(result, id).height, p.fresh(1, 220).height),
            "font/provider version did not invalidate measurement");
    LayoutDocument fresh(TextProvider::measure, &p, {4, 2});
    accepted(fresh.create(fresh.root(), input));
    same_geometry(result, accepted(fresh.compute({220, 80}, p.epoch)));
    std::printf("{\"kind\":\"text\",\"width100_natural_height\":%.6f,\"viewport_height\":40}\n", natural);
}

void independent_content_width() {
    TextProvider p;
    p.content[1] = {L"Label with independent intrinsic width", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {4, 3});
    auto row = column();
    row.kind = Kind::row;
    row.width = Axis::content(0, 180);
    const auto parent = accepted(d.create(d.root(), row));
    const auto leaf = accepted(d.create(parent, text(1)));
    auto result = accepted(d.compute({400, 100}, p.epoch));
    require(box(result, parent).width == 180 && box(result, leaf).width == 180 &&
                close(box(result, leaf).height, p.fresh(1, 180).height),
            "intrinsic then constrained text sizing");
    require(d.stats().intrinsic_queries == 1 && d.stats().width_queries == 1,
            "unnecessary or missing measurement queries");
    const auto calls = p.calls;
    accepted(d.compute({400, 120}, p.epoch));
    require(p.calls == calls, "intrinsic cache mixed with constrained cache");
    LayoutDocument fresh(TextProvider::measure, &p, {4, 3});
    const auto root = accepted(fresh.create(fresh.root(), row));
    accepted(fresh.create(root, text(1)));
    same_geometry(d.view(), accepted(fresh.compute({400, 120}, p.epoch)));
}

void demand_driven_measurement() {
    TextProvider p;
    p.content[1] = {L"Natural demand is needed only when the geometry uses it.", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {4, 3});
    auto value = text(1);
    value.height = Axis::expand();
    const auto leaf = accepted(d.create(d.root(), value));
    auto result = accepted(d.compute({100, 50}, p.epoch));
    require(box(result, leaf).height == 50 && p.calls == 0, "filled definite height queried unused natural demand");
    const auto parent = accepted(d.create(d.root(), column()));
    accepted(d.move(leaf, parent));
    result = accepted(d.compute({100, 50}, p.epoch));
    require(p.calls == 1 && close(box(result, parent).height, p.fresh(1, 100).height),
            "Content parent failed to request Expand child natural height");
    value.height = Axis::fixed(12);
    accepted(d.update(leaf, value));
    const auto calls = p.calls;
    accepted(d.compute({140, 50}, p.epoch));
    require(p.calls == calls, "fixed text box performed unused size query");
}

void provider_metrics_reuse() {
    TextProvider p;
    const std::array<std::wstring, 5> values{L"A\nB", L"word   ", L"", L"averylongunbreakablewordwithoutspaces",
                                             L"自动换行的中文文字"};
    std::uint64_t key = 0;
    for (const auto& value : values) {
        ++key;
        p.content[key] = {value, 16, 1};
        for (const auto width :
             {std::optional<float>{}, std::optional<float>{40}, std::optional<float>{220}, std::optional<float>{40}}) {
            const auto actual = TextProvider::measure(&p, {key, 1, 1, width});
            const auto expected = p.fresh(key, width);
            require(actual && close(actual->width, expected.width) && close(actual->height, expected.height),
                    "mutable DirectWrite metrics differ from fresh object");
        }
    }
    p.content[100] = {L"Object reuse does not count internal shaping.", 16, 1};
    const auto before = p.creates;
    for (float width : {80.f, 160.f, 80.f})
        require(TextProvider::measure(&p, {100, 1, 1, width}).has_value(), "reused text query failed");
    require(p.creates - before == 1, "three width queries did not reuse one metrics object");
}

void transactional_failure_and_reentry() {
    TextProvider p;
    for (std::uint64_t i = 1; i <= 3; ++i)
        p.content[i] = {L"A real paragraph with width-dependent height.", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {5, 2});
    p.active = &d;
    std::array<LayoutNodeId, 3> ids;
    for (unsigned i = 0; i < 3; ++i)
        ids[i] = accepted(d.create(d.root(), text(i + 1)));
    p.active_leaf = ids[0];
    const auto initial = accepted(d.compute({180, 300}, p.epoch));
    const auto owned = d.snapshot();
    for (unsigned i = 0; i < 3; ++i) {
        ++p.content[i + 1].revision;
        p.content[i + 1].text += L" Changed.";
        accepted(d.update(ids[i], text(i + 1, 2)));
    }
    const auto before = p.calls;
    p.try_reentry = true;
    p.fail_at = p.calls + 3;
    const auto failed = d.compute({180, 300}, p.epoch);
    rejected(failed, ErrorCode::measurement_failed);
    require(failed.error().provider_code == 77 && p.reentry_rejected, "provider diagnostic or callback guard lost");
    require(d.view().publication == initial.publication && d.view().input_revision == initial.input_revision &&
                d.view().boxes.data() == initial.boxes.data() &&
                std::equal(owned.boxes.begin(), owned.boxes.end(), d.view().boxes.begin()),
            "failed compute changed publication");
    p.fail_at = 0;
    const auto retry = accepted(d.compute({180, 300}, p.epoch));
    require(p.calls - before == 6 && retry.publication == initial.publication + 1,
            "failed cache writes leaked into retry");
    LayoutDocument fresh(TextProvider::measure, &p, {5, 2});
    for (unsigned i = 0; i < 3; ++i)
        accepted(fresh.create(fresh.root(), text(i + 1, 2)));
    same_geometry(retry, accepted(fresh.compute({180, 300}, p.epoch)));
    const auto good = d.view().publication;
    ++p.epoch;
    p.invalid = true;
    rejected(d.compute({180, 300}, p.epoch), ErrorCode::invalid_measurement);
    p.invalid = false;
    p.exception = true;
    rejected(d.compute({180, 300}, p.epoch), ErrorCode::measurement_failed);
    p.exception = false;
    p.allocation_exception = true;
    bool threw = false;
    try {
        static_cast<void>(d.compute({180, 300}, p.epoch));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    require(threw && d.view().publication == good, "provider bad_alloc was swallowed or publication changed");
    p.allocation_exception = false;
    accepted(d.compute({180, 300}, p.epoch));
}

void geometry_failure_cache_rollback() {
    TextProvider p;
    p.content[1] = {L"Initial content", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {3, 1});
    accepted(d.set_root({Kind::overlay}));
    auto value = text(1);
    const auto id = accepted(d.create(d.root(), value));
    const auto first = accepted(d.compute({100, 100}, p.epoch));
    ++value.content_revision;
    ++p.content[1].revision;
    p.content[1].text += L" with more content";
    value.anchor_x = std::numeric_limits<float>::max();
    accepted(d.update(id, value));
    const auto calls = p.calls;
    const auto failed = d.compute({100, 100}, p.epoch);
    rejected(failed, ErrorCode::numeric_overflow);
    require(failed.error().node == id && d.view().publication == first.publication && p.calls == calls + 1,
            "numeric failure did not occur after real measurement");
    value.anchor_x = 0;
    accepted(d.update(id, value));
    accepted(d.compute({100, 100}, p.epoch));
    require(p.calls == calls + 2, "geometry failure committed measurement cache");
}

void local_cache_and_slot_reuse() {
    TextProvider p;
    p.content[1] = {L"First pane", 16, 1};
    p.content[2] = {L"Second pane", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {3, 1});
    accepted(d.set_root({Kind::row}));
    const auto a = accepted(d.create(d.root(), text(1)));
    const auto b = accepted(d.create(d.root(), text(2)));
    accepted(d.compute({400, 100}, p.epoch));
    auto before = p.calls;
    ++p.content[1].revision;
    p.content[1].text += L" changes only this leaf";
    accepted(d.update(a, text(1, 2)));
    accepted(d.compute({400, 100}, p.epoch));
    require(p.calls == before + 1, "stable sibling measurement was discarded");
    before = p.calls;
    accepted(d.move(a, d.root()));
    auto result = accepted(d.compute({400, 100}, p.epoch));
    require(p.calls == before && box(result, a).x == 200 && box(result, b).x == 0,
            "reorder changed IDs or repeated stable measurement");
    LayoutDocument fresh(TextProvider::measure, &p, {3, 1});
    accepted(fresh.set_root({Kind::row}));
    accepted(fresh.create(fresh.root(), text(2)));
    accepted(fresh.create(fresh.root(), text(1, 2)));
    same_geometry(result, accepted(fresh.compute({400, 100}, p.epoch)));
    accepted(d.remove(a));
    const auto reused = accepted(d.create(d.root(), text(1, 2)));
    before = p.calls;
    accepted(d.compute({400, 100}, p.epoch));
    require(reused != a && p.calls == before + 1, "recycled slot retained old node cache");
    rejected(d.input(a), ErrorCode::invalid_node);
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
    auto result = accepted(d.compute({800, 450}, 1));
    require(box(result, a).x == 710 && box(result, a).y == 400 && close(box(result, b).x + box(result, c).x, .8f),
            "anchors or unrounded local geometry");
    const auto owned = d.snapshot();
    for (float dpi : {1.f, 1.5f, 2.f})
        require(std::isfinite(std::round((box(result, b).x + box(result, c).x) * dpi)), "pure pixel consumption");
    require(std::equal(owned.boxes.begin(), owned.boxes.end(), result.boxes.begin()), "pixel consumption wrote back");
    LayoutDocument flow(nullptr, nullptr, {3, 1});
    accepted(flow.set_root({Kind::row}));
    auto filling = fixed_box(0, 10);
    filling.width = Axis::expand();
    const auto normal = accepted(flow.create(flow.root(), filling));
    corner.out_of_flow = true;
    const auto anchored = accepted(flow.create(flow.root(), corner));
    result = accepted(flow.compute({800, 450}, 1));
    require(box(result, normal).width == 800 && box(result, anchored).x == 710,
            "out-of-flow child consumed row allocation");
}

void invalid_inputs_and_dependencies() {
    LayoutDocument d(nullptr, nullptr, {8, 4});
    const auto id = accepted(d.create(d.root(), fixed_box(0, 0)));
    const auto initial = accepted(d.compute({0, 0}, 1));
    const auto revision = d.input_revision();
    auto value = fixed_box(1, 1);
    value.width = Axis::content(30, 20);
    rejected(d.update(id, value), ErrorCode::invalid_input);
    value = fixed_box(1, 1);
    value.padding.left = 2;
    rejected(d.update(id, value), ErrorCode::invalid_input);
    value = fixed_box(1, 1);
    value.width = Axis::expand(0);
    rejected(d.update(id, value), ErrorCode::invalid_input);
    value = fixed_box(1, 1);
    value.offset_x = std::numeric_limits<float>::quiet_NaN();
    rejected(d.update(id, value), ErrorCode::invalid_input);
    value = column();
    value.kind = Kind::overlay;
    rejected(d.update(id, value), ErrorCode::unsupported_dependency);
    rejected(d.set_root({Kind::measured}), ErrorCode::invalid_input);
    rejected(d.compute({-1, 0}, 1), ErrorCode::invalid_input);
    rejected(d.compute({std::numeric_limits<float>::infinity(), 0}, 1), ErrorCode::invalid_input);
    require(d.input_revision() == revision && d.view().publication == initial.publication,
            "invalid operation changed active state");
    value = fixed_box(0, 0);
    value.kind = Kind::overlay;
    value.width = Axis::expand();
    value.height = Axis::expand();
    const auto parent = accepted(d.create(d.root(), column()));
    accepted(d.create(parent, value));
    rejected(d.compute({100, 100}, 1), ErrorCode::unsupported_dependency);
    require(d.view().publication == initial.publication, "unsupported dependency published partial geometry");
    TextProvider p;
    LayoutDocument missing(TextProvider::measure, &p, {2, 1});
    accepted(missing.create(missing.root(), text(999)));
    const auto failure = missing.compute({100, 100}, 1);
    rejected(failure, ErrorCode::measurement_failed);
    require(failure.error().provider_code == 91 && missing.view().publication == 0 && missing.view().boxes.empty(),
            "missing content returned fallback geometry");
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
        require(second.publication == first.publication + 1 && box(second, id).x == 9.5f && owned.boxes[1].x == 2.25f,
                "snapshot changed after publication");
    }
    require(owned.boxes.size() == 2 && owned.boxes[1].x == 2.25f && owned.boxes[1].width == 10.25f &&
                owned.boxes[1].height == 5.5f,
            "snapshot did not survive document destruction");
}

void explicit_deep_tree() {
    constexpr std::uint32_t depth = 4096;
    TextProvider p;
    p.content[1] = {L"Real text at the end of an explicitly scheduled deep tree.", 16, 1};
    LayoutDocument d(TextProvider::measure, &p, {depth + 1, depth});
    auto parent = d.root();
    LayoutNodeId first;
    for (std::uint32_t i = 1; i < depth; ++i) {
        parent = accepted(d.create(parent, column()));
        if (i == 1)
            first = parent;
    }
    const auto leaf = accepted(d.create(parent, text(1)));
    auto result = accepted(d.compute({180, 40}, p.epoch));
    require(result.boxes.size() == depth + 1 && close(box(result, leaf).height, p.fresh(1, 180).height) &&
                d.stats().measure_calls == 1,
            "deep explicit schedule failed");
    const auto snapshot = d.snapshot();
    const auto visits = d.stats().node_visits;
    accepted(d.remove(first));
    require(d.node_count() == 1, "deep subtree removal failed");
    rejected(d.input(leaf), ErrorCode::invalid_node);
    result = accepted(d.compute({180, 40}, p.epoch));
    require(result.boxes.size() == 1 && snapshot.boxes.size() == depth + 1, "deep old snapshot lost");
    std::printf("{\"kind\":\"deep\",\"depth\":%u,\"nodes\":%u,\"visits\":%llu,\"measure_calls\":1}\n", depth, depth + 1,
                static_cast<unsigned long long>(visits));
}
#include "practical.inl"
#include "scaling.inl"
#include "editor.inl"
#include "flex_grid.inl"
#include "audit.inl"
#include "topology.inl"
} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        require(mi_version() == MI_MALLOC_VERSION && mi_is_redirected(), "shared mimalloc version/redirection");
        if (argc == 2 && std::string_view(argv[1]) == "--performance") {
            performance();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--text-costs") {
            text_costs();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--virtual-performance") {
            virtual_performance();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--editor-performance") {
            editor_performance();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--flex-grid-performance") {
            flex_grid_performance();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--audit-performance") {
            audit_performance();
            return 0;
        }
        require(
            argc == 1,
            "usage: atelier_ui_layout_acceptance [--performance|--text-costs|--virtual-performance|--editor-performance|--flex-grid-performance|--audit-performance]");
        const std::pair<const char*, void (*)()> cases[]{
            {"identity_and_reuse", identity_and_reuse},
            {"atomic_ordered_edits", atomic_ordered_edits},
            {"capacity_and_depth", capacity_and_depth},
            {"bounded_allocation", bounded_allocation},
            {"weight_dynamic_range", weight_dynamic_range},
            {"slots_padding_and_columns", slots_padding_and_columns},
            {"known_width_natural_height", known_width_natural_height},
            {"independent_content_width", independent_content_width},
            {"demand_driven_measurement", demand_driven_measurement},
            {"provider_metrics_reuse", provider_metrics_reuse},
            {"transactional_failure_and_reentry", transactional_failure_and_reentry},
            {"geometry_failure_cache_rollback", geometry_failure_cache_rollback},
            {"local_cache_and_slot_reuse", local_cache_and_slot_reuse},
            {"anchors_and_logical_precision", anchors_and_logical_precision},
            {"invalid_inputs_and_dependencies", invalid_inputs_and_dependencies},
            {"owned_geometry_lifetime", owned_geometry_lifetime},
            {"explicit_deep_tree", explicit_deep_tree},
            {"publication_lookup_lifetime", publication_lookup_lifetime},
            {"descriptors_and_collapsed_subtrees", descriptors_and_collapsed_subtrees},
            {"shared_inspector_dynamic", shared_inspector_dynamic},
            {"shared_columns_structure_and_intrinsic", shared_columns_structure_and_intrinsic},
            {"wrap_flow_and_repeated_grid", wrap_flow_and_repeated_grid},
            {"nested_wrap_and_shared_columns", nested_wrap_and_shared_columns},
            {"grid_text_and_failure_recovery", grid_text_and_failure_recovery},
            {"content_invalidation_scope", content_invalidation_scope},
            {"virtual_range_and_anchor", virtual_range_and_anchor},
            {"virtual_inspector_end_to_end", virtual_inspector_end_to_end},
            {"retained_text_frames", retained_text_frames},
            {"editor_session_end_to_end", editor_session_end_to_end},
            {"flex_allocation_and_distribution", flex_allocation_and_distribution},
            {"flex_wrapping_and_alignment", flex_wrapping_and_alignment},
            {"flex_real_text_baseline", flex_real_text_baseline},
            {"grid_tracks_spans_and_placement", grid_tracks_spans_and_placement},
            {"grid_limits_and_nested_content", grid_limits_and_nested_content},
            {"flex_grid_editor_end_to_end", flex_grid_editor_end_to_end},
            {"allocation_rounded_saturation", allocation_rounded_saturation},
            {"flex_baseline_boundaries", flex_baseline_boundaries},
            {"grid_sizing_batches", grid_sizing_batches},
            {"grid_sparse_spans", grid_sparse_spans},
            {"grid_collision_boundaries", grid_collision_boundaries},
            {"collapsed_publication_indices", collapsed_publication_indices},
            {"final_tree_transactions", final_tree_transactions},
            {"final_tree_validation_atomicity", final_tree_validation_atomicity},
            {"final_tree_capacity_reuse", final_tree_capacity_reuse},
            {"final_tree_measurement_recovery", final_tree_measurement_recovery},
            {"final_tree_linear_siblings", final_tree_linear_siblings}};
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

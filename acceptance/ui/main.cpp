#include "memory/fatal.hpp"
#include "ui/session.hpp"
#include "ui/text.hpp"
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace atelier;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class T>
T accepted(ui::Result<T> result) {
    if (!result)
        throw std::runtime_error("UI error " + std::to_string(static_cast<int>(result.error().code)) + " native " +
                                 std::to_string(result.error().native_code));
    return std::move(*result);
}

void accepted(ui::Result<void> result) {
    require(result.has_value(), "UI operation failed");
}

const ui::DrawQuad& first_text(const ui::UiFrame& frame) {
    for (const auto& draw : frame.drawing->draws)
        if (draw.coverage)
            return draw;
    throw std::runtime_error("frame has no text");
}

ui::Control leaf(std::uint64_t key, ui::ControlKind kind, float y, std::wstring text = {}) {
    ui::Control value;
    value.key = key;
    value.kind = kind;
    value.text = std::move(text);
    value.layout = ui_layout::NodeInput::box(180, 30);
    value.layout.offset_x = 10;
    value.layout.offset_y = y;
    if (kind == ui::ControlKind::label)
        value.layout.padding = {6, 4, 6, 4};
    return value;
}

struct Consumer {
    ui::Session ui;

    std::vector<ui::Control> controls;

    std::shared_ptr<const ui::UiFrame> frame;

    Consumer() { accepted(ui.set_surface({{640, 480}, 1, 1})); }

    void prepare() {
        accepted(ui.set_controls(controls));
        frame = accepted(ui.prepare());
        require(ui.accept(frame), "candidate not accepted");
    }

    void click(std::uint64_t key) {
        const auto* box = frame->find(key);
        require(box, "control missing");
        const float x = box->bounds.x + box->bounds.width / 2;
        const float y = box->bounds.y + box->bounds.height / 2;
        ui.input({.kind = ui::EventKind::pointer_down, .x = x, .y = y});
        ui.input({.kind = ui::EventKind::pointer_up, .x = x, .y = y});
    }
};

void identity_and_lifetime() {
    std::shared_ptr<const ui::UiFrame> held;
    std::weak_ptr<const ui::Coverage> released;
    {
        Consumer consumer;
        consumer.controls = {leaf(1, ui::ControlKind::button, 10, L"Original")};
        consumer.prepare();
        held = consumer.frame;
        released = first_text(*held).coverage;
        consumer.controls.clear();
        accepted(consumer.ui.set_controls(consumer.controls));
        consumer.controls = {leaf(1, ui::ControlKind::button, 10, L"Reused key")};
        accepted(consumer.ui.set_controls(consumer.controls));
        consumer.prepare();
    }
    require(held && held->drawing && !held->drawing->draws.empty(), "UI frame failed to outlive Session");
    for (const auto& draw : held->drawing->draws)
        if (draw.coverage)
            require(!draw.coverage->pixels.empty(), "frame lost text pixels after teardown");
    held.reset();
    require(released.expired(), "accounting retained pixels after the last frame was released");
}

#include "text_edit.inl"
#include "style_snapshots.inl"
#include "style_sheets.inl"
#include "tooltip.inl"
}

int main() {
    atelier::install_fatal_handler();
    unsigned failures = 0;
    const std::pair<const char*, void (*)()> cases[] = {{"identity_and_lifetime", identity_and_lifetime},
                                                        {"accepted_text_style_snapshot", accepted_text_style_snapshot},
                                                        {"style_sheets", style_sheets},
                                                        {"tooltip_contracts", tooltip_contracts}};
    for (const auto& [name, run] : cases)
        try {
            run();
            std::printf("UI %s: PASS\n", name);
        } catch (const std::exception& error) {
            ++failures;
            std::printf("UI %s: FAIL %s\n", name, error.what());
        }
    std::printf("UI acceptance: %s (%zu/%zu)\n", failures ? "FAIL" : "PASS", std::size(cases) - failures,
                std::size(cases));
    return failures ? 1 : 0;
}

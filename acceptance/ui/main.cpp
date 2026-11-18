#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "ui/session.hpp"
#include "ui/text.hpp"
#include "ui_language/component.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace atelier;

void verify_ui_error_recovery();
void verify_ui_entry_dependencies();

namespace {
using acceptance::helpers::require;

template <class T>
T accepted(ui::Result<T> result) {
    if (!result) {
        char message[128];
        std::snprintf(message, sizeof(message), "UI error %d native %u", static_cast<int>(result.error().code),
                      static_cast<unsigned>(result.error().native_code));
        fatal(message);
    }
    return std::move(*result);
}

void accepted(ui::Result<void> result) {
    require(result.has_value(), "UI operation failed");
}

const ui::DrawQuad& first_text(const ui::UiFrame& frame) {
    for (const auto& draw : frame.drawing->draws)
        if (draw.coverage)
            return draw;
    fatal("frame has no text");
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

    Consumer() {
        accepted(ui.initialize());
        accepted(ui.set_surface({{640, 480}, 1, 1}));
    }

    void prepare() {
        accepted(ui.set_controls(controls));
        frame = accepted(ui.prepare());
        require(ui.accept(frame), "candidate not accepted");
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

void repeat_updates() {
    using namespace ui_language;
    using acceptance::helpers::take;
    Runtime runtime;
    take(register_types(runtime));
    const auto list = take(Component::compile(runtime, "repeat.ui.source", R"(
ui Detail() { Text content { text: "Detail"; height: 18; } }
ui Entry(in ActionBinding command) {
    state bool open = false;
    Column row {
        Checkbox fold { text: "Group"; width: 150; height: 24; selected: state.open; on activate { state.open = event.checked; } }
        Button run { text: "Run"; width: 120; height: 24; action: input.command; }
        Detail detail { mounted: state.open; }
    }
}
ui Group(in Items<Entry> entries) { Column view { Repeat rows { items: input.entries; } } }
ui ListView(in Items<Group> groups) { Repeat groups { items: input.groups; } }
)",
                                              "ListView"));
    const auto row = [](ui::ControlKey key, ui::ControlKey fold, ui::ControlKey target) {
        return Item{key, {ActionBinding{target}}, {{"fold", fold}, {"run", key + 2}}};
    };
    const auto inputs = [](std::vector<Item> rows) {
        auto entries = ItemList{std::make_shared<const std::vector<Item>>(std::move(rows))};
        std::vector<Item> groups{{50, {std::move(entries)}}};
        return std::array<InputValue, 1>{ItemList{std::make_shared<const std::vector<Item>>(std::move(groups))}};
    };
    auto first = row(100, 101, 501);
    auto second = row(200, 201, 502);
    ui::ControlKey next = 1000;
    auto root = take(Instance::create(list, 0, next, {}, inputs({first, second})));
    Consumer consumer;
    const auto accept_frame = [&] {
        consumer.frame = accepted(consumer.ui.prepare());
        require(consumer.ui.accept(consumer.frame), "Repeat frame was refused.");
    };
    const auto present = [&] {
        accepted(consumer.ui.set_controls(root->controls()));
        root->commit_controls();
        accept_frame();
    };
    const auto fold = [&](ui::ControlKey key, bool checked) {
        return take(root->event({ui::ActionKind::activate, key, checked}));
    };
    const auto selected = [&](ui::ControlKey key) {
        const auto at = std::ranges::find(root->controls(), key, &ui::Control::key);
        require(at != root->controls().end(), "Repeat fold key is missing.");
        return at->selected;
    };
    const auto has_detail = [&](ui::ControlKey row) {
        return std::ranges::any_of(root->controls(), [row](const auto& control) {
            return control.parent == row && control.text == L"Detail";
        });
    };
    present();
    const auto initial = root->controls().size();
    require(fold(201, true) && fold(101, true) && fold(201, true), "Repeat did not handle both rows.");
    take(root->update());
    require(root->controls().size() == initial + 2 && selected(101) && selected(201),
            "Batched row events patched the wrong control ranges.");
    present();
    require(fold(101, false), "First fold event was lost.");
    take(root->update());
    require(root->controls().size() == initial + 1, "Fold did not remove its child.");
    root->reject_controls();
    require(root->controls().size() == initial + 2 && selected(101), "Refusal lost the accepted child or fold.");
    take(root->update());
    present();
    require(fold(201, false), "Second fold event was lost.");
    take(root->update());
    present();
    const auto point = [](const ui::UiFrame& frame, ui::EventKind kind) {
        const auto* run = frame.find(102);
        require(run && run->bounds.width > 0 && run->bounds.height > 0, "Missing command geometry.");
        return ui::Event{kind, run->bounds.x + run->bounds.width / 2, run->bounds.y + run->bounds.height / 2};
    };
    const auto command = [](const ui::Reply& reply) -> ui::ControlKey {
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect);
                action && action->kind == ui::ActionKind::activate)
                return action->target;
        return 0;
    };
    const auto old_frame = consumer.frame;
    require(consumer.ui.input(point(*old_frame, ui::EventKind::pointer_down)).capture == ui::Capture::acquire,
            "Command press was not captured.");
    first = row(100, 109, 701);
    take(root->update(inputs({second, first})));
    const auto controls = root->controls();
    require(std::ranges::find(controls, 200, &ui::Control::key) < std::ranges::find(controls, 100, &ui::Control::key) &&
                selected(109) && !selected(201),
            "Reorder moved fold state away from its item key.");
    accepted(consumer.ui.set_controls(controls));
    require(!consumer.ui.interaction_state().pointer, "Rebinding kept the old command pressed.");
    root->commit_controls();
    require(!command(consumer.ui.input(point(*old_frame, ui::EventKind::pointer_up))),
            "Old press executed after rebind.");
    accept_frame();
    const auto* rebound = consumer.frame->find(102);
    require(rebound && rebound->action_target == 701 && old_frame->find(102)->action_target == 501,
            "Command rebind changed the accepted old frame.");
    consumer.ui.input(point(*consumer.frame, ui::EventKind::pointer_down));
    require(command(consumer.ui.input(point(*consumer.frame, ui::EventKind::pointer_up))) == 701,
            "Fresh click did not use the rebound target.");
    require(!fold(101, false) && fold(109, false) && fold(201, true), "Rebound fold aliases routed to the wrong row.");
    auto invalid = first;
    invalid.inputs = {ActionBinding{0}};
    require(!root->update(inputs({second, invalid})), "Invalid row command was accepted.");
    take(root->update());
    present();
    require(selected(109) && !selected(201) && has_detail(100) && !has_detail(200) &&
                root->controls().size() == initial + 1,
            "Failed list update did not restore both rows and their mounted children.");
    require(fold(109, false) && fold(201, true), "Recovered rows no longer handled events.");
    take(root->update());
    present();
    require(!selected(109) && selected(201) && !has_detail(100) && has_detail(200) &&
                root->controls().size() == initial + 1,
            "Local patches used positions from before reorder or rollback.");
    require(fold(201, false) && fold(201, true), "Removed row did not receive its last events.");
    take(root->update(inputs({first})));
    present();
    require(!fold(201, true), "Retired item still received events.");
    take(root->update(inputs({first, second})));
    present();
    require(!selected(201) && root->controls().size() == initial, "Reinserted item retained retired state.");
}
}

int main() {
    atelier::install_fatal_handler();
    identity_and_lifetime();
    repeat_updates();
    verify_ui_error_recovery();
    verify_ui_entry_dependencies();
    std::puts("UI acceptance: PASS (accepted frames, Repeat, error recovery, editor entry dependencies)");
}

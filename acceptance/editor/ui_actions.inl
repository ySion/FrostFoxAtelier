void verify_ui_actions() {
    namespace language = ui_language;
    editor::Session document;
    take(document.initialize());
    Runtime runtime;
    take(language::register_types(runtime));
    constexpr auto source = R"(
ui Actions(in ActionBinding first, in ActionBinding second, in bool available) {
    state double count = 0.0d;
    Column body {
        Button first { text: "Create"; action: input.first; enabled: input.available; height: 24; }
        Button second { text: "Create again"; action: input.second; height: 24; }
        Button local { text: "Local state"; height: 24; on activate { state.count = state.count + 1.0d; } }
        Text value { text: "Count"; width: 100.0d + state.count; height: 24; }
    }
}
)";
    auto component = take(language::Component::compile(runtime, "actions.ui.source", source));
    ui::ControlKey next = 2000;
    const std::array<language::KeyBinding, 4> keys{{{"first", 501}, {"second", 502}, {"local", 503}, {"value", 504}}};
    const ui::ControlKey create = 10;
    const ui::ControlKey undo = 12;
    std::array<language::InputValue, 3> inputs{language::ActionBinding{create, true},
                                               language::ActionBinding{create, true}, true};
    auto instance = take(language::Instance::create(component, 0, next, keys, inputs));
    ui::Session surface;
    require(surface.set_surface({{500, 240}, 1, 1}).has_value(), "Action surface failed.");
    std::vector<ui::Action> emitted;
    const auto dispatch = [&](ui::Reply reply) {
        for (const auto& effect : reply.effects)
            if (const auto* action = std::get_if<ui::Action>(&effect)) {
                emitted.push_back(*action);
                if (action->target == create)
                    changed(document.add_entity());
                else if (action->target == undo)
                    changed(document.undo());
                else
                    take(instance->event(*action));
            }
    };
    const auto accept = [&] {
        take(instance->update(inputs));
        auto updated = surface.set_controls(instance->controls());
        require(updated.has_value(), "Action descriptors failed.");
        dispatch(std::move(*updated));
        instance->commit_controls();
        auto frame = surface.prepare();
        require(frame.has_value() && surface.accept(*frame), "Action frame failed.");
    };
    const auto pointer = [&](ui::EventKind kind, ui::ControlKey key) {
        const auto box = surface.presented()->find(key)->bounds;
        dispatch(surface.input({.kind = kind, .x = box.x + 8, .y = box.y + 8}));
    };
    const auto click = [&](ui::ControlKey key) {
        pointer(ui::EventKind::pointer_down, key);
        pointer(ui::EventKind::pointer_up, key);
    };
    accept();
    require(surface.presented()->find(501)->action_target == create &&
                surface.presented()->find(502)->action_target == create,
            "Repeated command presentations did not keep independent visual keys.");
    click(501);
    click(502);
    require(take(document.world().members()).size() == 2 && emitted.size() == 2 && emitted[0].key == 501 &&
                emitted[1].key == 502 && emitted[0].target == create &&
                std::holds_alternative<std::monostate>(emitted[0].value),
            "Source buttons did not produce one command each with independent source identities.");
    click(503);
    accept();
    require(surface.presented()->find(504)->bounds.width == 101, "A local Button event did not update Source state.");
    require(!take(instance->event({ui::ActionKind::activate, 503, {}, create})),
            "A host-routed action also ran a local Source handler.");

    emitted.clear();
    pointer(ui::EventKind::pointer_down, 501);
    inputs[0] = language::ActionBinding{undo, true};
    take(instance->update(inputs));
    auto retarget = surface.set_controls(instance->controls());
    require(retarget && retarget->capture == ui::Capture::release,
            "Retargeting a pressed button did not cancel its capture.");
    dispatch(std::move(*retarget));
    instance->commit_controls();
    // The previously accepted pixels must not dispatch the newly bound command.
    pointer(ui::EventKind::pointer_up, 501);
    click(501);
    require(emitted.empty(), "Old accepted geometry dispatched a new action target.");
    accept();
    pointer(ui::EventKind::pointer_up, 501);
    require(emitted.empty(), "A retargeted press survived publication.");
    click(501);
    require(take(document.world().members()).size() == 1 && emitted.back().target == undo,
            "A new press did not use the new target.");

    emitted.clear();
    inputs[0] = language::ActionBinding{create, true};
    accept();
    auto invalid = inputs;
    invalid[0] = language::ActionBinding{0, true};
    static_cast<void>(instance->update(invalid));
    instance->reject_controls();
    accept();
    click(501);
    require(take(document.world().members()).size() == 2, "Refusing an invalid binding damaged the live command.");
    require(surface.set_controls({}).has_value(), "Action unmount failed.");
    instance->close();
    std::puts(
        "UI action acceptance: PASS (independent keys, real commands, local event, retarget cancellation, rejected binding)");
}

// Included by document.cpp inside its existing acceptance namespace.
std::string reload_source(unsigned version) {
    return "ui Leaf(in double offset) { Text label { text: \"Version " + std::to_string(version) +
           "\"; height: input.offset + " + std::to_string(version) +
           ".0d; } }\nui Host(in double offset) { Column root { Leaf child { offset: input.offset; } } }";
}

void verify_ui_code_reuse() {
    namespace language = ui_language;
    Runtime runtime;
    take(ui_language::register_types(runtime));
    auto first = take(language::Module::compile(runtime, "reuse.ui.source", reload_source(1)));
    ui::ControlKey keys = 30000;
    const std::array<language::InputValue, 1> initial{0.0};
    auto old = take(language::Instance::create(first->find("Host"), 0, keys, {}, initial));
    auto second = take(language::Module::compile(runtime, "reuse.ui.source", reload_source(2), first.get()));
    auto current = take(language::Instance::create(second->find("Host"), 0, keys, {}, initial));
    const auto has_text = [](const language::Instance& instance, std::wstring_view text) {
        return std::ranges::any_of(instance.controls(), [&](const auto& control) { return control.text == text; });
    };
    require(has_text(*current, L"Version 2") && has_text(*old, L"Version 1"),
            "Reused parent retained stale child links or changed old leased code.");
    const std::array<language::InputValue, 1> flipped{7.0};
    take(old->update(flipped));
    require(has_text(*old, L"Version 1") && std::ranges::any_of(old->controls(),
                                                                [](const auto& control) {
                                                                    return control.text == L"Version 1" &&
                                                                           control.layout.height.value == 8;
                                                                }),
            "Old instance lost its executable lease after replacement.");

    const std::string mapped = "ui Meter(in double size) {\n Text label { height: input.size; }\n}";
    auto meter = take(language::Module::compile(runtime, "old.ui.source", mapped));
    auto relocated =
        take(language::Module::compile(runtime, "new.ui.source", "// relocation\n\n" + mapped, meter.get()));
    const std::array<language::InputValue, 1> invalid{40000.0};
    auto failed = language::Instance::create(relocated->find("Meter"), 0, keys, {}, invalid);
    require(!failed, "Reused dispatch skipped runtime property validation.");
    const auto& declaration = relocated->find("Meter")->declaration();
    const auto mapping =
        std::ranges::find_if(declaration.mappings, [](const auto& value) { return value.origin.line == 4; });
    require(mapping != declaration.mappings.end(), "Relocated dispatch has no current property mapping.");
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t i = 0; i < mapping->begin; ++i) {
        if (declaration.generated_source[i] == '\n') {
            ++line;
            column = 1;
        } else
            ++column;
    }
    require(declaration
                .diagnostic("old.ui.source[request]:" + std::to_string(line) + ":" + std::to_string(column) +
                            ": mapped failure")
                .starts_with("new.ui.source:4:"),
            "Reused dispatch retained stale diagnostic mapping.");

    const auto helpers = [](unsigned value) {
        return "double measure() { return " + std::to_string(value) +
               ".0d; }\n"
               "ui A() { Text a { height: measure(); } }\nui B() { Text b { height: measure(); } }";
    };
    auto helper_old = take(language::Module::compile(runtime, "helper.ui.source", helpers(1)));
    auto helper_new = take(language::Module::compile(runtime, "helper.ui.source", helpers(2), helper_old.get()));
    auto helper = take(language::Instance::create(helper_new->find("A"), 0, keys, {}, {}));
    require(helper->controls().front().layout.height.value == 2.0,
            "Changed helper body reused stale generated dispatch.");
    std::printf("UI dispatch reuse acceptance: PASS (leaf/parent, old lease, source mapping, helper)\n");
}

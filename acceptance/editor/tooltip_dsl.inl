// Real authoring file -> Source/JIT -> composed/repeated controls -> accepted UI
// frame. Only the file watch debounce clock advances; no GPU or fake UI backend.
void verify_tooltip_dsl() {
    namespace language = ui_language;
    Runtime runtime;
    take(language::register_types(runtime));
    const auto folder = std::filesystem::temp_directory_path() /
                        ("atelier-tooltip-dsl-" + std::to_string(GetCurrentProcessId()) + "-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(folder), "Tooltip DSL evidence folder was not unique.");
    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    const auto path = folder / "tooltip.ui.source";
    const std::string source = R"(
ui HelpLeaf(in Text label, in TooltipBinding help) {
    Text field { text: input.label; single_line: true; tooltip: input.help; }
}
ui HelpEntry(in Text label, in TooltipBinding help) {
    Column root { HelpLeaf child { label: input.label; help: input.help; } }
}
ui TooltipDsl(in TooltipBinding help, in Items<HelpEntry> entries, in EditBinding vector) {
    Column root {
        Text literal {
            text: "Literal help"; single_line: true;
            tooltip_title: "Position 位置";
            tooltip_identity: "Entity.position · Vector3";
            tooltip: "相对于 Space Reference 的位置。\n单位 m，改变它会移动 Entity。";
        }
        HelpLeaf direct { label: "Direct help"; help: input.help; }
        NumberRow position { text: "Position"; edit: input.vector; tooltip: input.help; }
        ParameterRow flag {
            text: "Enabled"; tooltip: input.help;
            Checkbox value { text: "Visible"; selected: true; }
        }
        Repeat repeated { items: input.entries; }
    }
}
)";
    take(editor::write_file(path, source));
    ui::ControlKey next_key = 10000;
    language::LiveDocument live(runtime, path, next_key);
    const std::array fonts{(editor::asset_directory() / "fonts/SarasaUiSC-Regular.ttf").wstring()};
    ui::Session surface(fonts);
    require(surface.set_surface({{960, 720}, 1, 1}).has_value(), "Tooltip DSL surface failed.");
    const language::TooltipBinding original = std::make_shared<const ui::Tooltip>(
        ui::Tooltip{L"Position 位置", L"Entity.position · Vector3", L"相对于 Space Reference 的位置。\n单位 m。"});
    auto help = original;
    const language::EditBinding vector{6000,
                                       true,
                                       false,
                                       {{6001, L"X", 1, 0, -10000, 10000, .01, .001, ui::VectorAxis::x},
                                        {6002, L"Y", 2, 0, -10000, 10000, .01, .001, ui::VectorAxis::y},
                                        {6003, L"Z", 3, 0, -10000, 10000, .01, .001, ui::VectorAxis::z}}};
    const auto inputs = [&] {
        return std::array<language::InputValue, 3>{
            help,
            language::ItemList{std::make_shared<const std::vector<language::Item>>(std::vector<language::Item>{
                {1001, {std::wstring{L"Repeated A"}, help}}, {1002, {std::wstring{L"Repeated B"}, help}}})},
            vector};
    };
    std::vector<ui::Control> controls;
    const auto draw = [&] {
        controls.clear();
        take(live.append("TooltipDsl", 0, {}, inputs(), controls));
        require(!live.candidate_failed(), "Valid tooltip source failed while materializing controls.");
        const auto submitted = surface.set_controls(controls);
        require(submitted.has_value(), "Tooltip DSL controls were rejected by Session.");
        auto frame = surface.prepare();
        require(frame.has_value(), "Tooltip DSL frame failed.");
        live.commit_controls();
        require(surface.accept(*frame), "Tooltip DSL frame was not accepted.");
    };
    const auto find = [&](std::wstring_view text) -> const ui::Control& {
        const auto at = std::ranges::find_if(controls, [&](const auto& control) { return control.text == text; });
        require(at != controls.end(), "Tooltip DSL control was missing.");
        return *at;
    };
    const auto row_help = [&](std::wstring_view label) -> language::TooltipBinding {
        const auto key = find(label).parent;
        const auto at = std::ranges::find(controls, key, &ui::Control::key);
        require(at != controls.end(), "Tooltip DSL parameter row root was missing.");
        return at->tooltip;
    };
    const auto checked_help = [&](const language::TooltipBinding& expected) {
        for (const auto label : {L"Direct help", L"Repeated A", L"Repeated B"}) {
            const auto& control = find(label);
            require(control.tooltip == expected && surface.presented()->find(control.key)->tooltip == expected,
                    "TooltipBinding was lost while forwarding or accepting a composed/repeated node.");
        }
        require(row_help(L"Position") == expected && row_help(L"Enabled") == expected,
                "NumberRow/ParameterRow did not keep help on their shared root.");
    };
    auto now = language::LiveDocument::Clock::now();
    live.poll(now);
    draw();
    const auto& literal = find(L"Literal help");
    require(literal.tooltip && literal.tooltip->title == L"Position 位置" &&
                literal.tooltip->identity == L"Entity.position · Vector3" &&
                literal.tooltip->description == L"相对于 Space Reference 的位置。\n单位 m，改变它会移动 Entity。",
            "Tooltip literal lost UTF-8 text, punctuation or escaped newlines in Source/JIT lowering.");
    checked_help(original);
    const auto literal_snapshot = literal.tooltip;
    const auto first = *live.statistics("TooltipDsl");
    const auto direct_key = find(L"Direct help").key;
    draw();
    require(live.statistics("TooltipDsl")->binding_evaluations == first.binding_evaluations &&
                live.statistics("TooltipDsl")->publications == first.publications &&
                find(L"Literal help").tooltip == literal_snapshot,
            "Unchanged tooltip inputs reevaluated bindings, republished controls or allocated a new literal snapshot.");
    help = std::make_shared<const ui::Tooltip>(*original);
    draw();
    checked_help(original);
    require(live.statistics("TooltipDsl")->publications == first.publications,
            "Equal tooltip text with a different shared pointer unnecessarily republished controls.");
    help.reset();
    draw();
    checked_help({});
    require(find(L"Literal help").tooltip == literal_snapshot, "Null bound help cleared unrelated literal help.");
    help = original;
    draw();
    checked_help(original);

    const auto save = [&](const std::string& text) {
        take(editor::write_file(path, text));
        now += std::chrono::milliseconds(350);
        live.poll(now);
        now += std::chrono::milliseconds(250);
        live.poll(now);
        draw();
    };
    const std::string binding = "tooltip: input.help;";
    auto removed = source;
    const auto binding_at = removed.find(binding);
    require(binding_at != removed.npos, "Tooltip source marker was missing.");
    removed.erase(binding_at, binding.size());
    save(removed);
    require(
        live.revision() == 2 && find(L"Direct help").key == direct_key && !find(L"Direct help").tooltip &&
            !find(L"Repeated A").tooltip && !find(L"Repeated B").tooltip && row_help(L"Position") == original,
        "Removing a tooltip property on save retained stale help, changed keys or cleared another component's help.");
    save(source);
    checked_help(original);
    const auto accepted_revision = live.revision();
    const auto accepted = controls;
    for (const auto& mixed :
         {std::string{"tooltip_title: \"Wrong\"; "} + binding, binding + " tooltip_title: \"Wrong\";"}) {
        auto rejected = source;
        rejected.replace(rejected.find(binding), binding.size(), mixed);
        save(rejected);
        require(live.revision() == accepted_revision &&
                    live.diagnostic().find("TooltipBinding owns") != std::string_view::npos &&
                    std::ranges::equal(controls, accepted) && live.committed_source() == source,
                "Conflicting TooltipBinding/title syntax replaced the accepted file or depended on declaration order.");
    }
    require(surface.set_controls({}).has_value(), "Tooltip DSL unmount failed.");
    std::puts(
        "Editor Tooltip DSL acceptance: PASS (UTF-8/newlines, composition/Repeat, optional help, stable snapshots, file reload and rejected changes)");
}

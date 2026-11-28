#include "reload_fixture.hpp"
#include "platform/utf.hpp"
#include "source/snapshot.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

using namespace atelier;

void verify_ui_error_recovery() {
    using namespace ui_language;
    using acceptance::helpers::require;
    using acceptance::helpers::take;
    auto now = LiveDocument::Clock::now();
    const auto file_name = String::concat("ui-errors-", now.time_since_epoch().count(), ".ui.source");
    const auto path = std::filesystem::temp_directory_path() / file_name.c_str();
    Runtime runtime;
    take(register_types(runtime));
    ui::ControlKey next = 100;
    LiveDocument document(runtime, path, next);
    using namespace acceptance::ui_reload;
    const auto reload = [&](const auto& text) {
        save(path, StringView{text});
        begin(document, now);
        drain(document, now);
    };
    String source = R"(ui Probe(in double divisor) {
    state double count = 7.0d;
    Button run { text: "Run"; width: 120; height: 30.0d + state.count; on activate {
        state.count = 12.0d;
        state.count = 1.0d / input.divisor;
    } }
})"_s;
    reload(source);
    ui::Session session;
    take(session.initialize());
    take(session.set_surface({{320, 200}, 1, 1}));
    ui::ControlList controls;
    const std::array<KeyBinding, 1> keys{{{"run", 42}}};
    const auto stage = [&](double value) {
        controls.begin();
        const InputValue input_values[]{value};
        auto staged = document.append("Probe", 0, keys, input_values, controls);
        controls.finish();
        return staged;
    };
    const auto present = [&](double count) {
        require(controls.controls().size() == 1 && controls.controls().front().layout.height.value == 30.0 + count,
                "Unexpected accepted UI state.");
        require(session.set_controls(controls).has_value(), "UI error recovery controls were refused.");
        auto frame = session.prepare();
        require(frame && session.accept(*frame), "UI error recovery frame was refused.");
        document.commit_controls();
    };
    const auto click = [&] { return document.event({ui::ActionKind::activate, 42, {}}); };
    const auto check_failure = [&](StringView current) {
        auto failed = click();
        require(!failed && failed.error().source.has_value(), "Failed UI event lost its source.");
        const auto offset = current.rfind("state.count = ");
        const auto line = current.slice(0, offset).count('\n') + 1;
        const auto column = offset - current.rfind('\n', offset);
        const auto& location = *failed.error().source;
        const auto name = take(platform::utf16_to_utf8(path.filename().native()));
        require(location.path == name && location.version == source::content_hash(current) &&
                    location.offset == offset && location.line == line && location.column == column &&
                    failed.error().message ==
                        String::concat(name, ":", line, ":", column, ": Invalid component state write."),
                failed.error().message);
        take(stage(0));
        present(7);
    };
    take(stage(0));
    present(7);
    check_failure(source);
    const auto initial = document.component("Probe");
    const auto initial_stats = document.statistics("Probe");
    require(initial_stats.has_value(), "Missing initial Probe statistics.");
    const auto identity = initial_stats->instance;
    require(source.insert(source.find("state.count = 1.0d"), "// moved within the same handler\n            ").has_value(),
            "insert comment into Probe source");
    reload(source);
    take(stage(0));
    present(7);
    const auto moved_stats = document.statistics("Probe");
    require(initial->declaration().generated_source == document.component("Probe")->declaration().generated_source &&
                moved_stats && moved_stats->instance == identity,
            "Position-only edit changed the dispatch or instance.");
    check_failure(source);
    reload(String::concat("\n\n", source));
    require(document.reject_controls("Refused UI revision."), "Expected a staged UI revision.");
    check_failure(source);
    require(source.replace(source.find("1.0d /"), 4, "2.0d").has_value(), "replace divisor numerator");
    require(source.insert(0, "// recompiled\n").has_value(), "insert recompiled header");
    reload(source);
    take(stage(0));
    present(7);
    check_failure(source);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    require(!stage(nan), "UI accepted a NaN input.");
    take(stage(nan));
    present(7);
    take(stage(2));
    present(7);
    require(document.diagnostic().empty() && take(click()), "Valid input did not recover the UI.");
    take(stage(2));
    present(1);
    require(source.replace(source.find("2.0d / input.divisor"), StringView{"2.0d / input.divisor"}.size(),
                           "double(divide(int(input.divisor)))")
                .has_value(),
            "replace divide call");
    require(source.insert(0, "int unused(){ return 1; }\nint divide(int value){ return 9 / value; }\n").has_value(),
            "insert divide helpers");
    const auto divide_failure = [&](Component::Compilation expected) {
        reload(source);
        take(stage(0));
        present(1);
        require(document.component("Probe")->compilation() == expected, "Unexpected event entry replacement.");
        auto error = click();
        const auto slash = source.find("9 / value") + 2;
        require(!error && error.error().source && error.error().message.contains("nonzero divisor") &&
                    error.error().source->offset == slash &&
                    error.error().source->version == source::content_hash(source) &&
                    error.error().source->line == source.slice(0, slash).count('\n') + 1 &&
                    error.error().source->column == slash - source.rfind('\n', slash),
                "Division fault points to stale source.");
    };
    divide_failure(Component::Compilation::compiled);
    require(source.replace(source.find("return 1;"), 9, "int longer = 1234567;\n return longer + 2;").has_value(),
            "replace unused helper body");
    divide_failure(Component::Compilation::reused_dependencies);
    require(source.insert(0, "// same code, move the fault again\n").has_value(), "insert comment before helpers");
    divide_failure(Component::Compilation::reused_source);
    take(stage(-1.0));
    present(1);
    reload(R"(
        double height(double n) {
            if (n < 0.0d) { return 37.0d; }
            if ((1.0d/n) < 0.0d) { return 42.0d; }
            return -1.0d;
        }
        ui Probe(in double divisor) { Button run { width: 120; height: height(input.divisor); } }
    )");
    take(stage(-1.0));
    present(7);
    require(!stage(0.0), "Positive zero must reject this binding.");
    take(stage(-0.0));
    present(12);
    require(document.diagnostic().empty(), "A failed positive-zero input prevented recovery with negative zero.");
    {
        // Borrowed ControlRegion slices preserve non-zero region revisions across split insertions
        // while forwarding hot-reload candidate full-submission requirements.
        ui::ControlList split_controls;
        const auto mid_revision = ui::ControlList::new_region_revision();
        ui::Control mid;
        mid.key = 77;
        mid.kind = ui::ControlKind::label;
        mid.text = L"Mid";
        const auto stage_split = [&](double value, std::size_t& decorated) {
            split_controls.begin();
            ControlRegion region{&split_controls};
            const InputValue input_values[]{value};
            take(document.append("Probe", 0, keys, input_values, region));
            require(!region.controls.empty() && region.revision != 0, "ControlRegion missed the region revision.");
            split_controls.append(region.controls.first(1), region.revision);
            split_controls.append(Span{&mid, 1}, mid_revision);
            split_controls.append(region.controls.slice(1), region.revision);
            decorated = 0;
            split_controls.finish([&](ui::Control&) { ++decorated; });
        };
        std::size_t decorated{};
        stage_split(-0.0, decorated);
        require(decorated == 2 && session.set_controls(split_controls).has_value(),
                "Initial split ControlRegion failed.");
        auto first_split = session.prepare();
        require(first_split && session.accept(*first_split), "Initial split ControlRegion frame was refused.");
        document.commit_controls();
        const auto stats_before = session.statistics();
        stage_split(-0.0, decorated);
        require(decorated == 0 && session.set_controls(split_controls).has_value(),
                "Unchanged split ControlRegion dirtied ControlList.");
        const auto stats_after = session.statistics();
        require(stats_after.control_batches == stats_before.control_batches &&
                    stats_after.controls_updated == stats_before.controls_updated,
                "Unchanged split ControlRegion triggered Session control updates.");
        reload("// candidate reload\n"
               "ui Probe(in double divisor) { Button run { width: 140; height: 42.0d; } }");
        stage_split(-0.0, decorated);
        require(decorated == 1 && session.set_controls(split_controls).has_value(),
                "Candidate reload through ControlRegion did not refresh Probe.");
        auto reloaded_split = session.prepare();
        require(reloaded_split && session.accept(*reloaded_split) && (*reloaded_split)->find(42)->bounds.width == 140.f,
                "Candidate reload through ControlRegion did not apply updated width.");
        document.commit_controls();
    }
    std::filesystem::remove(path);
}

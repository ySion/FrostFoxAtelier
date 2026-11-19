#include "reload_fixture.hpp"
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
    const auto path = std::filesystem::temp_directory_path() /
                      ("ui-errors-" + std::to_string(now.time_since_epoch().count()) + ".ui.source");
    Runtime runtime;
    take(register_types(runtime));
    ui::ControlKey next = 100;
    LiveDocument document(runtime, path, next);
    using namespace acceptance::ui_reload;
    const auto reload = [&](std::string_view text) {
        save(path, text);
        begin(document, now);
        drain(document, now);
    };
    std::string source = R"(ui Probe(in double divisor) {
    state double count = 7.0d;
    Button run { text: "Run"; width: 120; height: 30.0d + state.count; on activate {
        state.count = 12.0d;
        state.count = 1.0d / input.divisor;
    } }
})";
    reload(source);
    ui::Session session;
    take(session.initialize());
    take(session.set_surface({{320, 200}, 1, 1}));
    std::vector<ui::Control> controls;
    const std::array<KeyBinding, 1> keys{{{"run", 42}}};
    const auto stage = [&](double value) {
        controls.clear();
        return document.append("Probe", 0, keys, std::array<InputValue, 1>{value}, controls);
    };
    const auto present = [&](double count) {
        require(controls.size() == 1 && controls.front().layout.height.value == 30.0 + count,
                "Unexpected accepted UI state.");
        require(session.set_controls(controls).has_value(), "UI error recovery controls were refused.");
        auto frame = session.prepare();
        require(frame && session.accept(*frame), "UI error recovery frame was refused.");
        document.commit_controls();
    };
    const auto click = [&] { return document.event({ui::ActionKind::activate, 42, {}}); };
    const auto check_failure = [&](std::string_view current) {
        auto failed = click();
        require(!failed && failed.error().source.has_value(), "Failed UI event lost its source.");
        const auto offset = current.rfind("state.count = ");
        const auto line = static_cast<std::size_t>(std::ranges::count(current.substr(0, offset), '\n')) + 1;
        const auto column = offset - current.rfind('\n', offset);
        const auto& location = *failed.error().source;
        const auto name = path.filename().string();
        require(location.path == name && location.version == source::content_hash(current) &&
                    location.offset == offset && location.line == line && location.column == column &&
                    failed.error().message == name + ":" + std::to_string(line) + ":" + std::to_string(column) +
                                                  ": Invalid component state write.",
                failed.error().message);
        take(stage(0));
        present(7);
    };
    take(stage(0));
    present(7);
    check_failure(source);
    const auto initial = document.component("Probe");
    const auto identity = document.statistics("Probe")->instance;
    source.insert(source.find("state.count = 1.0d"), "// moved within the same handler\n            ");
    reload(source);
    take(stage(0));
    present(7);
    require(initial->declaration().generated_source == document.component("Probe")->declaration().generated_source &&
                document.statistics("Probe")->instance == identity,
            "Position-only edit changed the dispatch or instance.");
    check_failure(source);
    reload("\n\n" + source);
    require(document.reject_controls("Refused UI revision."), "Expected a staged UI revision.");
    check_failure(source);
    source.replace(source.find("1.0d /"), 4, "2.0d");
    source.insert(0, "// recompiled\n");
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
    source.replace(source.find("2.0d / input.divisor"), std::string_view("2.0d / input.divisor").size(),
                   "double(divide(int(input.divisor)))");
    source.insert(0, "int unused(){ return 1; }\nint divide(int value){ return 9 / value; }\n");
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
                    error.error().source->line ==
                        static_cast<std::size_t>(std::ranges::count(source.substr(0, slash), '\n')) + 1 &&
                    error.error().source->column == slash - source.rfind('\n', slash),
                "Division fault points to stale source.");
    };
    divide_failure(Component::Compilation::compiled);
    source.replace(source.find("return 1;"), 9, "int longer = 1234567;\n return longer + 2;");
    divide_failure(Component::Compilation::reused_dependencies);
    source.insert(0, "// same code, move the fault again\n");
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
    std::filesystem::remove(path);
}

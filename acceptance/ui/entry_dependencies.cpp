#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "ui_language/live_document.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>

using namespace atelier;
using namespace ui_language;
using acceptance::helpers::require;
using acceptance::helpers::take;

namespace {
void verify_component_environments(Runtime& runtime, ui::ControlKey& next) {
    std::string source = "const double margin = 2.0d;\n"
                         "double extent(){return input.size + margin;}\n"
                         "ui A(in double size){Button a{height:extent();}}\n"
                         "ui B(in bool flag,in double size){Button b{height:extent();}}";
    auto initial = take(Module::compile(runtime, "environment", source));
    auto a = take(Instance::create(initial->find("A"), 0, next, {}, std::array<InputValue, 1>{30.}));
    auto b = take(Instance::create(initial->find("B"), 0, next, {}, std::array<InputValue, 2>{true, 43.}));
    require(a->controls().front().layout.height.value == 32 && b->controls().front().layout.height.value == 45,
            "A helper captured another component's input slot.");
    auto invalid = Module::compile(runtime, "environment", source + "\nui C(){Button c{height:20;}}", initial.get());
    require(!invalid && invalid.error().message.contains("Unknown UI input"), "An unused capture escaped validation.");
    source.replace(source.find("2.0d"), 4, "5.0d");
    auto changed = take(Module::compile(runtime, "environment", source, initial.get()));
    take(a->update(std::array<InputValue, 1>{30.}, changed->find("A")));
    require(changed->compilation().compiled == 2 && a->controls().front().layout.height.value == 35,
            "Changed module constant was dropped from the entry dependencies.");
    struct Extra {
        std::uint32_t id;
    };
    take(runtime.register_type(
        NativeRegistration<Extra>(runtime, "ui.AcceptanceExtra", TypeKind::record)
            .field<&Extra::id>("id", {TypeReference::native<std::uint32_t>()}, offsetof(Extra, id))
            .build()));
    auto rebound = take(Module::compile(runtime, "environment", source, changed.get()));
    require(rebound->compilation().compiled == 2, "Code was reused across a changed type catalog.");
}
}

void verify_ui_entry_dependencies() {
    std::ifstream editor(ATELIER_EDITOR_UI_SOURCE, std::ios::binary);
    std::string source{std::istreambuf_iterator<char>{editor}, {}};
    const auto declarations = take(lower_module("editor.ui.source", source));
    require(declarations.size() == 44, "Update the real-editor dependency workload when its component set changes.");
    auto now = LiveDocument::Clock::now();
    const auto path = std::filesystem::temp_directory_path() /
                      ("editor-dependencies-" + std::to_string(now.time_since_epoch().count()) + ".ui.source");
    Runtime runtime;
    take(register_types(runtime));
    ui::ControlKey next = 100;
    LiveDocument document(runtime, path, next);
    ui::Session session;
    take(session.initialize());
    take(session.set_surface({{1100, 600}, 1, 1}));
    const std::array<KeyBinding, 1> keys{{{"shelf", 11}}};
    const std::array<InputValue, 12> inputs{
        false, true, ItemList{}, ItemList{}, ActionBinding{41}, ActionBinding{42}, ActionBinding{43}, 36.,
        24.,   16.,  0.,         0.};
    const auto reload = [&](std::string_view text) {
        std::ofstream file(path, std::ios::binary);
        file << text;
        file.close();
        require(file.good(), "Cannot write the editor hot-reload fixture.");
        now += std::chrono::seconds(1);
        document.poll(now);
        now += std::chrono::milliseconds(300);
        document.poll(now);
    };
    const auto present = [&](float height) {
        std::vector<ui::Control> controls;
        take(document.append("ToolShelf", 0, keys, inputs, controls));
        take(session.set_controls(controls));
        auto frame = take(session.prepare());
        require(session.accept(frame), "Editor UI frame was refused.");
        document.commit_controls();
        const auto* shelf = frame->find(11);
        require(shelf && shelf->bounds.height == height, "Accepted ToolShelf height differs from the source.");
    };
    using Compilation = Component::Compilation;
    const auto changed = [&](std::initializer_list<std::string_view> compiled, Compilation reused) {
        require(document.diagnostic().empty(), document.diagnostic());
        for (const auto& declaration : declarations) {
            const auto component = document.component(declaration.name);
            const auto expected =
                std::ranges::find(compiled, declaration.name) == compiled.end() ? reused : Compilation::compiled;
            require(component && component->compilation() == expected, declaration.name);
        }
    };
    const auto replace = [&](std::string_view original, std::string_view replacement) {
        const auto at = source.find(original);
        require(at != source.npos && source.find(original, at + 1) == source.npos, original);
        source.replace(at, original.size(), replacement);
    };
    reload(source);
    present(56);
    const auto old = document.component("ToolShelf");
    const auto instance = document.statistics("ToolShelf")->instance;
    const auto types = runtime.type_count();
    replace("return 56.0d;", "return 57.0d;");
    reload(source);
    present(57);
    changed({"ToolShelf"}, Compilation::reused_dependencies);
    source.insert(0, "// move all author positions\n");
    reload(source);
    present(57);
    changed({}, Compilation::reused_source);
    const auto revision = document.revision();
    reload("double unused_probe() { return \"wrong\"; }\n" + source);
    require(document.diagnostic().contains(":1:32: Type mismatch") && document.revision() == revision &&
                document.committed_source() == source,
            document.diagnostic());
    present(57);
    source.insert(0, "double unused_probe() { return 1.0d; }\n");
    reload(source);
    present(57);
    changed({}, Compilation::reused_dependencies);
    replace("return 57.0d;", "return rail_button_length(180.0d);");
    reload(source);
    present(36);
    changed({"ToolShelf"}, Compilation::reused_dependencies);
    replace("return 36.0d;", "return 38.0d;");
    reload(source);
    present(38);
    changed({"PaneDivider", "ToolShelf"}, Compilation::reused_dependencies);
    require(document.statistics("ToolShelf")->instance == instance && runtime.type_count() == types,
            "Helper changes replaced the accepted UI context or type catalog.");
    auto pinned = take(Instance::create(old, 0, next, keys, inputs));
    require(pinned->controls().front().layout.height.value == 56, "Old ToolShelf lost its pinned entry.");
    std::filesystem::remove(path);
    verify_component_environments(runtime, next);
}

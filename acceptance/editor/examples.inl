void verify_examples() {
    namespace fs = std::filesystem;
    const fs::path source = ATELIER_EXAMPLE_PROJECT;
    const auto output = fs::absolute("out/examples-document") / std::to_string(GetTickCount64());
    auto project = take(editor::Project::open(source));
    const auto path = source / "Content/Sponza/Scenes/Sponza.scene";
    const auto original = take(assets::read_file(path));
    auto document = take(editor::Session::deserialize(original, project, {}, editor::LoadPolicy::strict));
    const auto saved = output / "Sponza.scene";
    take(editor::write_file(saved, take(document->serialize())));
    auto reopened =
        take(editor::Session::deserialize(take(assets::read_file(saved)), project, {}, editor::LoadPolicy::strict));
    const auto authored = take(assets::json_format::parse(original));
    const auto restored = take(assets::json_format::parse(take(reopened->serialize())));
    require(*restored.root().at("entities")->at(0)->at("components")->at(0)->at("mesh") ==
                    *authored.root().at("entities")->at(0)->at("components")->at(0)->at("mesh") &&
                take(assets::read_file(path)) == original,
            "Sponza save/reopen lost its authored mesh identity or changed the source scene.");
    std::puts("Examples document acceptance: PASS (Sponza authored mesh identity, isolated save/reopen)");
}

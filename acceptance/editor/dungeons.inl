void verify_dungeons() {
    namespace fs = std::filesystem;
    const fs::path root = ATELIER_EXAMPLE_PROJECT;
    auto project = take(editor::Project::open(root));
    const auto path = root / "Content/Dungeons/Scenes/GenericOverview.scene";
    const auto original = take(assets::read_file(path));
    auto document = take(editor::Session::deserialize(original, project));
    const auto saved = fs::absolute("out/dungeons-document") / std::to_string(GetTickCount64()) / "Overview.scene";
    take(editor::write_file(saved, take(document->serialize())));
    auto reopened = take(editor::Session::deserialize(take(assets::read_file(saved)), project));
    const auto authored = take(assets::json_format::parse(original));
    const auto restored = take(assets::json_format::parse(take(reopened->serialize())));
    const auto authored_mesh = *authored.root().at("entities")->at(0)->at("components")->at(0);
    const auto restored_mesh = *restored.root().at("entities")->at(0)->at("components")->at(0);
    require(*restored_mesh.at("mesh") == *authored_mesh.at("mesh") &&
                *restored_mesh.at("materials") == *authored_mesh.at("materials") &&
                *restored.root().at("materials")->at(0)->at("entries")->at(7)->path({"value", "asset_key"}) ==
                    *authored.root().at("materials")->at(0)->at("entries")->at(7)->path({"value", "asset_key"}) &&
                take(assets::read_file(path)) == original,
            "Dungeons save/reopen lost authored asset identities or changed the source scene.");
    std::puts("Dungeons document acceptance: PASS (GenericOverview authored asset identities, save/reopen)");
}

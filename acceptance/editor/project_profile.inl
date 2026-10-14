// Optional measurement through the existing document acceptance executable.
// It creates its own fixture; it never opens or changes a user's project.
void profile_project_json(const std::filesystem::path& output) {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    namespace fs = std::filesystem;
    require(!fs::exists(output), "Choose a new JSON measurement output directory.");
    fs::create_directories(output);
    auto project = take(editor::Project::create(output / "Project"));
    Json report = {{"schema", 1}, {"json_library", "nlohmann/json 3.12.0"},
        {"rounds", 21}, {"clock", "steady_clock"},
        {"notes", "Warm file-system cache; same process. Parse excludes DOM destruction. Scene load includes parse, validation, resource resolution and fresh Runtime/World construction; excludes document destruction. No GUI or GPU work."},
        {"scenes", Json::array()}};
#ifdef NDEBUG
    report["configuration"] = "release";
#else
    report["configuration"] = "dev";
#endif
    const auto measure = [](auto&& operation) {
        std::vector<double> samples;
        for (int round = -2; round < 21; ++round) {
            const auto start = Clock::now();
            auto result = operation();
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (round >= 0) samples.push_back(elapsed);
            // Keep the operation's output alive until after the end timestamp.
            (void)result;
        }
        std::ranges::sort(samples);
        return Json{{"median_ms", samples[10]}, {"p95_ms", samples[19]}, {"max_ms", samples.back()}};
    };
    for (const auto [entities, components] : {std::pair{1, 1}, {64, 1}, {256, 1}, {256, 16}}) {
        editor::Session document;
        for (int entity = 0; entity < entities; ++entity) {
            changed(document.create_static_mesh(editor::mesh_assets()[static_cast<std::size_t>(entity) % editor::mesh_assets().size()]));
            take(document.rename(document.selection().entity, L"测量 Entity " + std::to_wstring(entity)));
            for (int component = 1; component < components; ++component)
                changed(document.add_component(document.types().component));
        }
        const auto text = take(document.serialize());
        const auto path = output / (std::to_string(entities) + "x" + std::to_string(components) + ".scene");
        take(editor::write_file(path, text));
        auto roundtrip = take(editor::Session::deserialize(text, project));
        require(take(roundtrip->serialize()) == text, "JSON measurement fixture did not roundtrip.");
        roundtrip.reset();
        auto dom = Json::parse(text);
        Json row = {{"entities", entities}, {"components_per_entity", components}, {"bytes", text.size()},
            {"read_cached", measure([&] { return take(editor::read_file(path)); })},
            {"parse_dom", measure([&] { return Json::parse(text); })},
            {"parse_format", measure([&] { return editor::json_format::parse(text); })},
            {"dump_dom", measure([&] { return dom.dump(2); })},
            {"save_scene_to_text", measure([&] { return take(document.serialize()); })},
            {"load_scene_from_text", measure([&] { return take(editor::Session::deserialize(text, project)); })},
            {"load_scene_from_file", measure([&] { return take(editor::Session::deserialize(take(editor::read_file(path)), project)); })}};
        row["parse_mb_per_second"] = static_cast<double>(text.size()) / row["parse_dom"]["median_ms"].get<double>() / 1000.;
        std::printf("JSON %dx%d: %zu bytes, parse %.3f ms (%.1f MB/s), scene load %.3f ms\n",
            entities, components, text.size(), row["parse_dom"]["median_ms"].get<double>(),
            row["parse_mb_per_second"].get<double>(), row["load_scene_from_text"]["median_ms"].get<double>());
        report["scenes"].push_back(std::move(row));
    }
    take(editor::write_file(output / "json-profile.json", report.dump(2) + "\n"));
}

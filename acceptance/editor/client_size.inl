// Real CLI -> native HWND/DPI -> accepted renderer surface -> Control inspection.
// Each process is an owned empty project; no global input or user's window is touched.
void client_size(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    require(previous != nullptr, "Cannot inspect client pixels with per-monitor DPI awareness");
    struct DpiScope {
        DPI_AWARENESS_CONTEXT previous;

        ~DpiScope() { SetThreadDpiAwarenessContext(previous); }
    } dpi_scope{previous};
    const auto project = root / "Project";
    project_fixture(project);
    const auto original_scene = sha256_file(project / "Content/Scenes/Main.scene");
    require(bool(original_scene), original_scene ? "" : original_scene.error());
    struct Extent {
        unsigned width;

        unsigned height;
    };
    const std::array cases{Extent{}, Extent{1601, 1003}};
    json::Builder report{json::Document::literal<R"({"status":"passed","windows":[]})">()};
    const auto observations = *report.root().at("windows");
    for (std::size_t i = 0; i < cases.size(); ++i) {
        LASTINPUTINFO input_before{sizeof(LASTINPUTINFO), 0};
        GetLastInputInfo(&input_before);
        const auto requested = cases[i];
        const auto directory = root / ("Control" + std::to_string(i));
        auto arguments =
            L"--project " + quote(project) + L" --control-directory " + quote(directory) + L" --no-activate";
        if (requested.width)
            arguments +=
                L" --client-size " + std::to_wstring(requested.width) + L" " + std::to_wstring(requested.height);
        Process child(executable, std::move(arguments), root / ("editor-" + std::to_string(i) + ".log"));
        Driver driver(discover_editor(child, directory));
        const auto window = child.window();
        require(window != nullptr && GetForegroundWindow() != window, "Client-size launch activated its window");
        const auto dpi = GetDpiForWindow(window);
        require(dpi != 0, "Owned editor has no display DPI");
        const auto expected = requested.width
                                  ? requested
                                  : Extent{static_cast<unsigned>(MulDiv(1280, static_cast<int>(GetDpiForSystem()), 96)),
                                           static_cast<unsigned>(MulDiv(840, static_cast<int>(GetDpiForSystem()), 96))};
        RECT client{};
        require(GetClientRect(window, &client) && client.right - client.left == static_cast<LONG>(expected.width) &&
                    client.bottom - client.top == static_cast<LONG>(expected.height),
                "Native client dimensions differ from the physical-pixel contract");
        json::Document current;
        json::Document stable_viewport;
        unsigned stable{};
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (stable < 3 && std::chrono::steady_clock::now() < deadline) {
            current = driver.inspect();
            const auto frame = *current.root().at("last_frame");
            if (frame.size() != 0 && frame.at("extent")->size() == 2 &&
                frame.at("extent")->at(0)->u32() == expected.width &&
                frame.at("extent")->at(1)->u32() == expected.height) {
                const auto viewport = *frame.at("viewport");
                require(viewport.kind() == json::Kind::array && viewport.size() == 4,
                        "Control omitted the accepted viewport bounds");
                const double x = *viewport.at(0)->number();
                const double y = *viewport.at(1)->number();
                const double width = *viewport.at(2)->number();
                const double height = *viewport.at(3)->number();
                require(std::isfinite(x + y + width + height) && x >= 0 && y >= 0 && width > 0 && height > 0 &&
                            x + width <= expected.width && y + height <= expected.height,
                        "Accepted viewport escaped its client area");
                stable = viewport == stable_viewport.root() ? stable + 1 : 1;
                stable_viewport = json::Document{viewport};
            } else
                stable = 0;
            if (stable < 3)
                std::this_thread::sleep_for(30ms);
        }
        require(stable == 3, "Control did not publish stable matching client and viewport dimensions");
        require(current.root().boolean({"dirty"}) == false && current.root().u64({"undo"}) == 0,
                "Sizing changed the scene or its history");
        LASTINPUTINFO input_after{sizeof(LASTINPUTINFO), 0};
        GetLastInputInfo(&input_after);
        require(GetForegroundWindow() != window,
                "Size inspection found its editor foreground (case " + std::to_string(i) + ", user input tick " +
                    std::to_string(input_before.dwTime) + " -> " + std::to_string(input_after.dwTime) + ")");
        const auto observation = report.append_object(observations);
        const std::array requested_extent{requested.width, requested.height};
        const std::array client_extent{expected.width, expected.height};
        require(bool(requested.width ? report.numbers(observation, "requested", std::span{requested_extent})
                                     : report.set_null(observation, "requested")),
                "Invalid requested extent");
        require(report.numbers(observation, "client", std::span{client_extent}) &&
                    report.set(observation, "dpi", dpi) && report.set(observation, "fractional_dpi", dpi % 96 != 0) &&
                    report.set(observation, "viewport", stable_viewport.root()) &&
                    report.set(observation, "foreground_preserved", true),
                "Invalid client-size observation");
        driver.client.reset();
        child.close();
        const auto remaining = discover(directory);
        require(bool(remaining), remaining ? "" : remaining.error().what());
        require(remaining->root().size() == 0, "Closed size fixture retained its Control endpoint");
        require(sha256_file(project / "Content/Scenes/Main.scene") == *original_scene,
                "Size fixture modified its saved scene");
        const auto viewport_bytes = json::write(stable_viewport.root());
        std::cout << "Client-size case " << i << ": " << expected.width << 'x' << expected.height << ", DPI " << dpi
                  << ", viewport " << std::string_view{viewport_bytes.data(), viewport_bytes.size()} << std::endl;
    }
    const auto bytes = json::write(report.root(), {.pretty = true});
    write(root / "result.json", {bytes.data(), bytes.size()});
    const auto root_text = platform::utf16_to_utf8(root.native());
    require(bool(root_text), "Invalid UTF-16 client-size evidence path");
    std::cout << "Editor client-size E2E: PASS | " << *root_text << '\n';
}

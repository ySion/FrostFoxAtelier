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
    const std::array invalid{L"--client-size",
                             L"--client-size 1600",
                             L"--client-size 0 1000",
                             L"--client-size -1 1000",
                             L"--client-size 1600 1.5",
                             L"--client-size +1600 1000",
                             L"--client-size 32769 1000",
                             L"--client-size 99999999999999999999 1000",
                             L"--client-size 1600px 1000",
                             L"--client-size 1600 1000 --client-size 1600 1000",
                             L"--client-size 1 1"};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        const auto forbidden = root / ("InvalidProject" + std::to_string(i));
        const auto directory = root / ("InvalidControl" + std::to_string(i));
        Process child(executable,
                      L"--no-activate --new-project " + quote(forbidden) + L" --control-directory " + quote(directory) +
                          L" " + invalid[i],
                      root / ("invalid-" + std::to_string(i) + ".log"));
        require(WaitForSingleObject(child.process, 10000) == WAIT_OBJECT_0,
                "Invalid client size did not exit before startup");
        DWORD code{};
        require(GetExitCodeProcess(child.process, &code) && code != 0, "Invalid client size was accepted");
        require(child.window() == nullptr && !std::filesystem::exists(forbidden) && discover(directory).empty(),
                "Rejected client size opened a window, created a project or published Control");
    }
    struct Extent {
        unsigned width, height;
    };
    const std::array cases{Extent{}, Extent{1601, 1003}, Extent{3840, 2054}};
    auto observations = Json::array();
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
        Json current, stable_viewport;
        unsigned stable{};
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (stable < 3 && std::chrono::steady_clock::now() < deadline) {
            current = driver.inspect();
            const auto& frame = current.at("last_frame");
            if (!frame.empty() && frame.at("extent") == Json::array({expected.width, expected.height})) {
                const auto& viewport = frame.at("viewport");
                require(viewport.is_array() && viewport.size() == 4, "Control omitted the accepted viewport bounds");
                const double x = viewport[0], y = viewport[1], width = viewport[2], height = viewport[3];
                require(std::isfinite(x + y + width + height) && x >= 0 && y >= 0 && width > 0 && height > 0 &&
                            x + width <= expected.width && y + height <= expected.height,
                        "Accepted viewport escaped its client area");
                stable = viewport == stable_viewport ? stable + 1 : 1;
                stable_viewport = viewport;
            } else
                stable = 0;
            if (stable < 3)
                std::this_thread::sleep_for(30ms);
        }
        require(stable == 3, "Control did not publish stable matching client and viewport dimensions");
        require(!current.at("dirty").get<bool>() && current.at("undo") == 0, "Sizing changed the scene or its history");
        LASTINPUTINFO input_after{sizeof(LASTINPUTINFO), 0};
        GetLastInputInfo(&input_after);
        require(GetForegroundWindow() != window,
                "Size inspection found its editor foreground (case " + std::to_string(i) + ", user input tick " +
                    std::to_string(input_before.dwTime) + " -> " + std::to_string(input_after.dwTime) + ")");
        observations.push_back(
            {{"requested", requested.width ? Json::array({requested.width, requested.height}) : Json(nullptr)},
             {"client", {expected.width, expected.height}},
             {"dpi", dpi},
             {"fractional_dpi", dpi % 96 != 0},
             {"viewport", stable_viewport},
             {"foreground_preserved", true}});
        driver.client.reset();
        child.close();
        require(discover(directory).empty(), "Closed size fixture retained its Control endpoint");
        require(sha256_file(project / "Content/Scenes/Main.scene") == original_scene,
                "Size fixture modified its saved scene");
        std::cout << "Client-size case " << i << ": " << expected.width << 'x' << expected.height << ", DPI " << dpi
                  << ", viewport " << stable_viewport.dump() << std::endl;
    }
    write(root / "result.json",
          Json{{"status", "passed"}, {"invalid_cases", invalid.size()}, {"windows", observations}}.dump(2));
    std::cout << "Editor client-size E2E: PASS | " << utf8(root.wstring()) << '\n';
}

void scene_lit_display(Application& app, ControlKey option) {
    using Access = atelier::application::EditorAcceptanceAccess;
    const auto history = app.document().undo_count();
    const auto document = model(app.document().serialize());
    click(app, control::scene_shading);
    click(app, option);
    tick(app, 2);
    require(!app.menu_frame() && app.document().undo_count() == history &&
                model(app.document().serialize()) == document,
            "Native Scene Lit/MatCap display choice changed authored scene data or history.");
    const auto expected = option == control::shading_scene_lit ? atelier::renderer::Shading::scene_lit
                                                               : atelier::renderer::Shading::matcap;
    require(Access::scene_lit_view(app).shading == expected,
            "Native display choice did not reach the accepted rendering policy.");
}

void verify_scene_lit_window(Application& app, const std::filesystem::path& output,
                             const std::filesystem::path& requested) {
    using Access = atelier::application::EditorAcceptanceAccess;
    const auto source =
        std::filesystem::canonical(requested.empty() ? std::filesystem::path{ATELIER_EXAMPLE_PROJECT} : requested);
    const auto outside = output.lexically_relative(source);
    require(!outside.empty() && *outside.begin() == L"..",
            "Scene Lit evidence output must be outside the source example project.");
    auto source_project = model(atelier::editor::Project::open(source));
    const auto source_scene = model(source_project.resolve("Content/Scene Lit Room/Scenes/Main.scene"));
    const auto source_text = model(atelier::assets::read_file(source_scene));
    const auto source_config = model(atelier::assets::read_file(source / "Config/Project.json"));
    const auto copy =
        output / (L"Project-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    model(atelier::editor::Project::create(copy));
    std::filesystem::copy(source / "Content/Scene Lit Room", copy / "Content/Scene Lit Room",
                          std::filesystem::copy_options::recursive);
    Access::mark_saved(app);
    model(app.open_project(copy));
    model(app.open_scene("Content/Scene Lit Room/Scenes/Main.scene"));
    tick(app, 4);
    // Native editor mode changes stay within the editing document's lifecycle.
    const auto history = app.document().undo_count();
    const auto authored = model(app.document().serialize());
    scene_lit_display(app, control::shading_first + 2);
    scene_lit_display(app, control::shading_scene_lit);
    require(!app.document().world().running() &&
                Access::scene_lit_view(app).shading == atelier::renderer::Shading::scene_lit &&
                app.document().undo_count() == history && model(app.document().serialize()) == authored,
            "Native display mode changes altered the editing lifecycle, authored scene or history.");
    capture(app, output / "07-scene-lit-edit.png");
    // Exercise the real native middle-drag input path from a known front view.
    auto orbit = centre(app, control::viewport);
    Access::move_pointer(app, orbit);
    tick(app, 1);
    key(app, VK_NUMPAD1);
    const auto eye = Access::camera_eye(app);
    mouse(app, WM_MBUTTONDOWN, orbit);
    orbit.x += 32;
    mouse(app, WM_MOUSEMOVE, orbit);
    mouse(app, WM_MBUTTONUP, orbit);
    require(Access::camera_eye(app).x < eye.x && Access::camera_mode(app) == atelier::editor::CameraView::perspective,
            "Dragging middle mouse right still reverses horizontal orbit.");
    mouse(app, WM_MBUTTONDOWN, orbit);
    orbit.x -= 32;
    mouse(app, WM_MOUSEMOVE, orbit);
    mouse(app, WM_MBUTTONUP, orbit);
    require(std::abs(Access::camera_eye(app).x - eye.x) < .001f && std::abs(Access::camera_eye(app).z - eye.z) < .001f,
            "Opposite horizontal middle drag did not return the camera to its starting side.");
    capture(app, output / "09-middle-orbit.png");
    require(model(atelier::assets::read_file(source_scene)) == source_text &&
                model(atelier::assets::read_file(source / "Config/Project.json")) == source_config,
            "Scene Lit product acceptance modified the source example project.");
}

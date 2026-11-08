// Raster and ray geometry share this one retirement/re-entry sequence.
void quality_contracts(Fixture& f) {
    using atelier::geometry::Vertex;
    using atelier::geometry::Mesh;
    const auto plane = [](float z) {
        return take(Mesh::create(std::vector<Vertex>{{{-6, -4, z}, {0, 0, -1}},
                                                     {{6, -4, z}, {0, 0, -1}},
                                                     {{6, 4, z}, {0, 0, -1}},
                                                     {{-6, -4, z}, {0, 0, -1}},
                                                     {{6, 4, z}, {0, 0, -1}},
                                                     {{-6, 4, z}, {0, 0, -1}}}));
    };
    f.restore();
    f.room.meshes.assign(1, {});
    f.room.meshes.front().geometry = take(Mesh::create(std::vector<Vertex>{{{-.4f, 0, -.4f}, {0, 1, 0}},
                                                                           {{.4f, 0, -.4f}, {0, 1, 0}},
                                                                           {{.4f, 0, .4f}, {0, 1, 0}},
                                                                           {{-.4f, 0, -.4f}, {0, 1, 0}},
                                                                           {{.4f, 0, .4f}, {0, 1, 0}},
                                                                           {{-.4f, 0, .4f}, {0, 1, 0}}}));
    f.room.meshes.front().stable_key = 1;
    f.room.meshes.front().color = {.7f, .7f, .7f, 1};
    f.room.meshes.front().material.roughness = 1;
    f.lights.clear();
    f.frame.environment = {.1f, .1f, .1f};
    f.frame.camera.view = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 1, 1};
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera.projection = {1 / (2.5f * aspect), 0, 0, 0, 0,          1 / 2.5f, 0, 0, 0, 0,
                                 -1 / 39.9f,          0, 0, 0, 40 / 39.9f, 1};
    f.frame.view.lit.gtao = f.frame.view.lit.sssr = f.frame.view.lit.bloom = false;
    f.frame.view.lit.antialiasing = render::Antialiasing::none;
    f.frame.view.lit.probe_volumes[0].minimum = {-1, -1, -1};
    f.frame.view.lit.probe_volumes[0].maximum = {1, 3, 1};
    f.frame.view.lit.probe_volumes[0].counts = {2, 2, 2};
    f.frame.view.lit.probe_update_budget = 8;
    f.frame.view.lit.probe_rays = 1;
    f.edited();
    f.step();
    const std::weak_ptr<const Mesh> observed = f.room.meshes.front().geometry;
    f.room.meshes.clear();
    f.edited();
    render::Capture empty;
    f.step(&empty);
    check(observed.expired() && std::ranges::all_of(empty.depth, [](float d) { return d == 0; }),
          "accepted empty scene retires raster/ray ownership and captures background depth");
    render::MeshInstance returned;
    returned.stable_key = 1;
    returned.geometry = plane(4);
    returned.color = {.4f, .4f, .4f, 1};
    f.room.meshes.push_back(returned);
    f.edited();
    f.frame.view.lit.ddgi = false;
    f.frame.camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 4, 1};
    f.step();
    render::Capture reentered;
    f.step(&reentered);
    check(depth(reentered, f.drawing.extent.width / 2, f.drawing.extent.height / 2) > 0,
          "same Renderer rebuilds the authored plane after scene-resource retirement");
    // The caller releases the material batch and closes once before HWND destruction.
    std::puts("Renderer SceneLit lifetime: PASS (raster/ray retirement and re-entry)");
}

void environment_and_group_writes() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world;
    take(world.initialize(runtime, types));
    const auto scene = take(p::Scene::create(world, types));
    const auto field = [&](TypeId type, std::string_view name) { return take(runtime.find_field(type, name)); };
    const auto path = [&](std::string_view name) { return w::FieldPath{field(types.environment_component, name)}; };
    auto owner = take(w::create_entity(runtime));
    auto environment = take(w::create_component(runtime, types.environment_component));
    const std::array environment_group{w::Attachment{environment.id()}};
    committed(world.attach_components(owner.id(), environment_group));
    committed(world.attach(owner.id()));
    const auto intensity_path = path("sky_intensity");

    // Reject after all leaves are written, then verify their original bytes, including NaN and -0.
    take(world.add_admission(
        types.environment_component,
        [&](const w::World& owner_world, std::span<const w::World::AdmissionCandidate> candidates) -> Result<void> {
            for (const auto& candidate : candidates) {
                ATELIER_TRY_BIND(intensity, owner_world.runtime().read(candidate.component, intensity_path));
                if (std::get<float>(*intensity) > 5)
                    return std::unexpected(Error{ErrorCode::invalid_member, "Test admission rejects bright skies."});
            }
            return {};
        }));
    const auto payload = std::bit_cast<float>(0x7fc00123u);
    take(runtime.with_native<p::EnvironmentComponent>(environment.id(), [&](auto& value) -> Result<void> {
        value.sky_color.x = payload;
        value.exposure_ev = -0.f;
        value.sky_intensity = 2.f;
        return {};
    }));
    const std::array admitted_paths{
        w::FieldPath{field(types.environment_component, "sky_color"), field(types.vector3, "x")}, path("exposure_ev"),
        path("sky_intensity")};
    const std::array<Scalar, 3> admitted_values{1.f, 2.f, 10.f};
    const auto rejected = world.write(environment.id(), admitted_paths, admitted_values);
    require(!rejected, "A group write bypassed admission.");
    require(take(runtime.with_native<const p::EnvironmentComponent>(
                environment.id(),
                [](const auto& value) -> Result<bool> {
                    return std::bit_cast<std::uint32_t>(value.sky_color.x) == 0x7fc00123u &&
                           std::bit_cast<std::uint32_t>(value.exposure_ev) == 0x80000000u && value.sky_intensity == 2.f;
                })),
            "A rejected group write did not restore every leaf's exact bytes.");
    committed(world.close());
    std::puts("[pass] Environment: admission failure restores every group-write leaf's exact bytes");
}

void material_dependency_updates() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world;
    take(world.initialize(runtime, types));
    const auto scene = take(p::Scene::create(world, types));
    auto source = take(material::create_source(runtime, "source", {mesh_material_definition(), {}}));
    auto a = take(material::create_instance(runtime, "a", {source, {}, {}}));
    auto b = take(material::create_instance(runtime, "b", {source, {}, {}}));
    auto leaf = take(material::create_instance(runtime, "leaf", {a, {}, {}}));
    auto first = take(p::create_static_mesh_entity(world, {geometry::cube_mesh(), {}, {}, {{"slot/0", leaf, false}}}));
    committed(world.attach(first.id()));
    (void)take(scene.update());
    take(material::update_instance(runtime, leaf.id(), {b, {}, {}}));
    (void)take(scene.update());
    take(material::update_instance(runtime, b.id(), {source, {{"roughness", .7f}}, {{}}}));
    (void)take(scene.update());
    require(near(take(scene.mesh_slots()[0].item->surfaces->front()->evaluate({})).roughness, .7f),
            "New-parent edit did not reach the rebound slot.");
    committed(world.close());
    std::puts("[pass] Material dependencies: same-Surface reparent observes the new parent's roughness edit");
}

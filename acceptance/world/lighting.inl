struct DerivedLight : p::LightComponent {
    float extra{3.f};
};
struct DerivedEnvironment : p::EnvironmentComponent {};
void lit_materials_and_lights() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    const auto derived = take(NativeRegistration<DerivedLight>{runtime, "lighting.DerivedLight", TypeKind::object}
                                  .base<p::LightComponent>()
                                  .field<&DerivedLight::extra>("extra")
                                  .publish());
    require(take(runtime.native_type<p::LightKind>()) == types.light_kind &&
                take(runtime.native_type<p::LightSettings>()) == types.light_settings &&
                take(runtime.find_type("world.LightComponent")) == types.light_component &&
                take(runtime.is_a(derived, types.component)),
            "Light types did not enter the shared reflected hierarchy.");
    source::Program program(runtime, types.light_component);
    publish(program, "void tick(float dt) { self.settings.intensity = self.settings.intensity + dt; }");
    const auto lease = take(program.pin());
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    std::vector<ObjectId> begins;
    std::vector<ObjectId> ends;
    take(world.bind_behavior(types.light_component, {[&](Runtime&, ObjectId object) -> Result<void> {
                                                         begins.push_back(object);
                                                         return {};
                                                     },
                                                     [lease](Runtime&, ObjectId object, float dt) -> Result<void> {
                                                         const std::array<Scalar, 1> arguments{dt};
                                                         auto invoked = lease.invoke(object, arguments);
                                                         if (!invoked)
                                                             return std::unexpected(invoked.error());
                                                         return {};
                                                     },
                                                     [&](Runtime&, ObjectId object) -> Result<void> {
                                                         ends.push_back(object);
                                                         return {};
                                                     }}));
    auto source_material = take(material::create_source(runtime, "world/source", {mesh_material_definition(), {}}));
    auto instance = take(material::create_instance(runtime, "", {source_material, {}, {}}));
    const auto initial_surface = take(material::capture_program(runtime, instance.id()));
    auto entity =
        take(p::create_static_mesh_entity(world, {geometry::cube_mesh(), {}, {}, {{"slot/0", instance, false}}}));
    const auto root = take(w::components(runtime, entity.id())).front().id();
    auto point_entity = take(w::create_entity(runtime));
    auto rectangle_entity = take(w::create_entity(runtime));
    auto directional_entity = take(w::create_entity(runtime));
    auto point = take(w::create_component(runtime, types.light_component));
    auto rectangle = take(w::create_component(runtime, derived));
    auto directional = take(w::create_component(runtime, types.light_component));
    const auto point_id = point.id();
    const auto rectangle_id = rectangle.id();
    const auto directional_id = directional.id();
    p::LightSettings point_settings;
    point_settings.intensity = 20;
    point_settings.range = 25;
    point_settings.source_radius = .2f;
    p::LightSettings rect_settings;
    rect_settings.kind = p::LightKind::rectangle;
    rect_settings.color = {2, 1, .5f};
    rect_settings.intensity = 50;
    rect_settings.width = 2;
    rect_settings.height = 3;
    rect_settings.two_sided = true;
    rect_settings.casts_shadow = false;
    p::LightSettings sun_settings;
    sun_settings.kind = p::LightKind::directional;
    sun_settings.intensity = 1000;
    sun_settings.range = 0;
    sun_settings.source_radius = .01f;
    take(p::set_light(world, point_id, point_settings));
    take(p::set_light(world, rectangle_id, rect_settings));
    take(p::set_light(world, directional_id, sun_settings));
    require(take(p::read_light(runtime, rectangle_id)) == rect_settings && take(scene.light_items()).empty(),
            "Detached lights entered presentation.");
    const float half = std::sqrt(.5f);
    const math::Quaternion parent_rotation{0, half, 0, half};
    const math::Quaternion child_rotation{0, 0, std::sin(.3f), std::cos(.3f)};
    const math::Quaternion rectangle_rotation{std::sin(.2f), 0, 0, std::cos(.2f)};
    take(world.set_transform(entity.id(), {{10, 20, 30}, parent_rotation, {2, 3, 4}}));
    take(world.set_transform(point_entity.id(), {{1, 0, 0}, child_rotation, {5, 6, 7}}));
    take(world.set_transform(rectangle_entity.id(), {{0, 1, 0}, rectangle_rotation, {8, 9, 10}}));
    take(world.set_transform(directional_entity.id(), {}));
    const std::array rect_group{w::Attachment{rectangle_id}};
    const std::array point_group{w::Attachment{point_id}};
    const std::array sun_group{w::Attachment{directional_id}};
    committed(world.attach_components(rectangle_entity.id(), rect_group));
    committed(world.attach_components(point_entity.id(), point_group));
    committed(world.attach_components(directional_entity.id(), sun_group));
    committed(world.reparent(rectangle_entity.id(), entity.id(), w::ReparentRule::keep_local));
    committed(world.reparent(point_entity.id(), entity.id(), w::ReparentRule::keep_local));
    committed(world.reparent(directional_entity.id(), entity.id(), w::ReparentRule::keep_local));
    // Spatial reference is independent of composition order: the rectangle
    // precedes its point origin in the authoritative lifecycle tree.
    committed(world.set_space_reference(rectangle_entity.id(), point_entity.id(), w::ReparentRule::keep_local));
    committed(world.attach(entity.id()));
    const auto roots = runtime.root_count();
    const auto snapshot = take(scene.light_items());
    const auto meshes = take(scene.render_items());
    require(runtime.root_count() == roots && snapshot.size() == 3 && meshes.size() == 1 &&
                meshes[0].surfaces->front() == initial_surface,
            "Lighting snapshots retained business roots or mixed mesh/light projections.");
    require(snapshot[0].component == rectangle_id && snapshot[1].component == point_id &&
                snapshot[2].component == directional_id && snapshot[0].entity == rectangle_entity.id() &&
                snapshot[0].settings == rect_settings,
            "Light projection lost identity, order or authored data.");
    const auto matrix = take(w::world_matrix(runtime, rectangle_entity.id()));
    require(snapshot[0].position == math::Vector3{matrix[12], matrix[13], matrix[14]} &&
                near(snapshot[1].position.z, 28),
            "Light position ignored the actual TRS hierarchy.");
    const auto rotation = *math::try_normalize(parent_rotation * child_rotation * rectangle_rotation);
    const auto forward = math::rotate(rotation, math::Vector3{0, 0, 1});
    const auto& projected = snapshot[0];
    require(near(math::dot(projected.direction, forward), 1) && near(math::dot(projected.right, projected.right), 1) &&
                near(math::dot(projected.up, projected.up), 1) && near(math::dot(projected.right, projected.up), 0) &&
                near(math::dot(math::cross(projected.right, projected.up), projected.direction), 1) &&
                projected.settings.width == 2 && projected.settings.height == 3,
            "Parent scale/shear distorted light orientation or emitter dimensions.");
    require(presented(world, scene) == 4 && presented(world, scene, point_id) == 1 &&
                take(world.query(types.light_component)).size() == 3 &&
                take(world.query(types.light_component, false)).size() == 2,
            "Light registration bypassed World presentation or inherited type queries.");
    w::World other(runtime, types);
    reject(p::set_light(other, point_id, {}), "Another World edited a registered light.");
    const auto light_field = take(runtime.find_field(types.light_component, "settings"));
    const auto light_path = [&](std::string_view name) {
        return w::FieldPath{light_field, take(runtime.find_field(types.light_settings, name))};
    };
    // Hard bounds clamp scalar edits; type validators reject an edit that breaks a valid light and keep its value.
    take(world.write(point_id, light_path("intensity"), -1.f));
    require(take(p::read_light(runtime, point_id)).intensity == 0,
            "Negative intensity was not clamped to its hard minimum.");
    take(world.write(point_id, light_path("intensity"), point_settings.intensity));
    reject(world.write(point_id, light_path("intensity"), std::numeric_limits<float>::quiet_NaN()),
           "NaN intensity was accepted.");
    reject(world.write(point_id, light_path("intensity"), 1e38f),
           "Finite intensity beyond the analytic sampling range was accepted.");
    reject(world.write(point_id, light_path("range"), 0.f), "Zero point range was accepted.");
    reject(world.write(point_id, light_path("kind"), EnumValue{types.light_kind, 99}),
           "Unknown LightKind was accepted.");
    reject(world.write(point_id, light_path("kind"), std::uint32_t{1}), "Raw uint bypassed enum identity.");
    reject(world.write(point_id, light_path("casts_shadow"), 1.f), "Numeric shadow flag was accepted.");
    reject(world.write(rectangle_id, light_path("width"), 0.f), "Zero rectangle width was accepted.");
    reject(world.write(directional_id, light_path("source_radius"), 2.f),
           "Directional angular radius beyond a hemisphere was accepted.");
    require(take(p::read_light(runtime, point_id)) == point_settings &&
                take(p::read_light(runtime, rectangle_id)) == rect_settings &&
                take(p::read_light(runtime, directional_id)) == sun_settings,
            "Rejected light edits changed authored settings.");
    // A light already broken outside World authoring stays editable, so its fault can be repaired.
    take(runtime.write(point_id, light_path("range"), 0.f));
    take(world.write(point_id, light_path("intensity"), 3.f));
    take(world.write(point_id, light_path("range"), point_settings.range));
    take(world.write(point_id, light_path("intensity"), point_settings.intensity));
    require(take(p::read_light(runtime, point_id)) == point_settings,
            "An invalid light could not be repaired through World authoring.");
    {
        material::InstanceState authored{
            source_material, {{"roughness", 0.f}, {"emission", math::Vector3{32, 8, 16}}}, {{}, {}}};
        take(material::update_instance(runtime, instance.id(), authored));
        const auto changed = take(scene.render_items()).front().surfaces->front();
        const auto output = take(changed->evaluate({}));
        require(output.emissive.x == 32 && output.roughness == 0 &&
                    take(initial_surface->evaluate({})).roughness == .25f,
                "Program material edits lost HDR/zero roughness or mutated a retained capture.");
        authored.entries[1].value = math::Vector3{material::emission_limit, 0, 0};
        take(material::update_instance(runtime, instance.id(), authored));
        require(take(take(scene.render_items()).front().surfaces->front()->evaluate({})).emissive.x ==
                    material::emission_limit,
                "Representable emission boundary did not survive World extraction.");
        authored.entries[0].value = std::numeric_limits<float>::quiet_NaN();
        take(material::update_instance(runtime, instance.id(), authored));
        const auto diagnosed = take(scene.render_items()).front().surfaces->front();
        require(take(diagnosed->evaluate({})).roughness == .25f && !diagnosed->diagnostics().empty() &&
                    std::isnan(std::get<float>(take(material::read_instance(runtime, instance.id())).entries[0].value)),
                "Presentation did not preserve the program material's diagnosed fallback and authored override.");
        take(material::update_instance(runtime, instance.id(), {source_material, {}, {}}));
    }
    auto boundary_light = sun_settings;
    boundary_light.intensity = static_cast<float>(atelier::rendering::analytic_light_radiance_limit);
    take(p::set_light(world, directional_id, boundary_light));
    require(take(p::read_light(runtime, directional_id)) == boundary_light,
            "Analytic range incorrectly used the smaller HDR storage limit.");
    boundary_light.intensity = std::nextafter(boundary_light.intensity, std::numeric_limits<float>::infinity());
    reject(p::set_light(world, directional_id, boundary_light), "Analytic radiance above the boundary was accepted.");
    take(p::set_light(world, directional_id, sun_settings));
    boundary_light = point_settings;
    boundary_light.intensity = 1e20f;
    boundary_light.color = {1e-20f, 1e-20f, 1e-20f};
    take(p::set_light(world, point_id, boundary_light)); // Validate the product, not an arbitrary intensity cap.
    take(p::set_light(world, point_id, point_settings));
    boundary_light = rect_settings;
    boundary_light.color = {1, 1, 1};
    boundary_light.width = boundary_light.height = 2;
    boundary_light.intensity = static_cast<float>(atelier::rendering::analytic_light_radiance_limit / 4);
    take(p::set_light(world, rectangle_id, boundary_light));
    boundary_light.width = std::nextafter(2.f, std::numeric_limits<float>::infinity());
    reject(p::set_light(world, rectangle_id, boundary_light),
           "Rectangle area-weighted radiance exceeded the shared sampling range.");
    take(p::set_light(world, rectangle_id, rect_settings));
    auto bad_light = point_settings;
    bad_light.color.z = std::numeric_limits<float>::quiet_NaN();
    reject(p::set_light(world, point_id, bad_light), "Nonfinite light color was accepted.");
    // The typed entries reject a value outside its hard bounds instead of clamping it as a field write does.
    auto dim = point_settings;
    dim.intensity = -1.f;
    reject(p::set_light(world, point_id, dim), "set_light clamped a negative intensity instead of rejecting it.");
    require(take(p::read_light(runtime, point_id)) == point_settings, "A rejected typed edit changed its field.");
    // A light already breaking its rules, as a native write can leave it, still has the new value's rules checked.
    take(runtime.with_native<p::LightComponent>(point_id, [](auto& light) -> Result<void> {
        light.settings.range = 0;
        return {};
    }));
    auto flat = rect_settings;
    flat.width = 0;
    reject(p::set_light(world, point_id, flat),
           "set_light skipped the light rules of a new value for an invalid light.");
    require(take(p::read_light(runtime, point_id)).kind == p::LightKind::point &&
                take(p::read_light(runtime, point_id)).range == 0,
            "A rejected typed edit of an invalid light changed it.");
    take(p::set_light(world, point_id, point_settings));
    take(world.write(point_id, light_path("intensity"), 0.f));
    require(take(scene.light_items()).size() == 3 && take(scene.light_items())[1].settings.intensity == 0,
            "Zero intensity dropped stable light identity.");
    take(p::set_light(world, point_id, point_settings));
    committed(world.set_enabled(point_id, false));
    require(take(scene.light_items()).size() == 2, "Disabled Component still illuminated the World.");
    committed(world.set_enabled(entity.id(), false));
    require(take(scene.light_items()).empty(), "Disabled Entity still illuminated the World.");
    committed(world.set_enabled(entity.id(), true));
    committed(world.set_enabled(point_id, true));
    auto removal = take(world.remove_component(rectangle_id));
    require(take(scene.light_items()).size() == 2, "Removed light stayed registered.");
    committed(world.restore(removal));
    removal.root.reset();
    require(take(scene.light_items())[0].component == rectangle_id, "Undo changed light identity/order.");
    committed(world.start());
    committed(world.start());
    require(begins == std::vector<ObjectId>{rectangle_id, point_id, directional_id},
            "Light inheritance bypassed ordered Begin.");
    take(p::set_light(world, point_id, point_settings));
    take(world.update(2));
    require(take(scene.light_items())[0].settings.intensity == 52 &&
                take(scene.light_items())[1].settings.intensity == 22,
            "Source behavior could not access reflected LightSettings on a derived light.");
    committed(world.stop());
    committed(world.stop());
    require(ends == std::vector<ObjectId>{directional_id, point_id, rectangle_id}, "Light End order/pairing is wrong.");
    take(runtime.with_native<p::LightComponent>(point_id, [](auto& light) -> Result<void> {
        light.settings.intensity = -1;
        return {};
    }));
    const auto isolated_lights = take(scene.light_items());
    const auto light_report = world.take_diagnostics();
    require(isolated_lights.size() == 2 &&
                std::ranges::none_of(isolated_lights, [&](const auto& light) { return light.component == point_id; }) &&
                light_report.size() == 1 && light_report[0].object == point_id &&
                light_report[0].entity == point_entity.id() &&
                light_report[0].kind == w::DiagnosticKind::invalid_value &&
                light_report[0].phase == w::DiagnosticPhase::lighting &&
                light_report[0].field == std::vector<std::string>{"settings", "intensity"} &&
                light_report[0].value == "-1" && light_report[0].type == "world.LightComponent",
            "Isolated light extraction did not skip and report only the invalid light.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    reject(scene.light_items(), "Unsafe native light mutation escaped extraction validation.");
    take(p::set_light(world, point_id, point_settings));
    take(runtime.with_native<p::LightComponent>(point_id, [](auto& light) -> Result<void> {
        light.settings.intensity = 1e38f;
        return {};
    }));
    reject(scene.light_items(), "Finite overflowing native light mutation escaped extraction validation.");
    take(p::set_light(world, point_id, point_settings));
    // The negative intensity persisted from the isolated pass; the analytic overflow (a
    // LightSettings validator failure) is a new cause.
    const auto halted = world.take_diagnostics();
    require(halted.size() == 1 && halted[0].object == point_id &&
                halted[0].field == std::vector<std::string>{"settings"} && halted[0].value.empty(),
            "Halted extraction failures were not reported once per persisting cause.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    auto twin = take(p::create_static_mesh_entity(world, {geometry::cube_mesh()}));
    const auto twin_mesh = take(w::components(runtime, twin.id())).front().id();
    committed(world.attach(twin.id()));
    const auto unbound = take(scene.render_items());
    require(unbound.size() == 2 && unbound[1].surfaces->front() == take(material::default_world_surface()) &&
                !unbound[1].surfaces->front()->uses_default_material(),
            "An unbound mesh did not use the ordinary world-grid MS.");
    // Each mesh resolves the slot materials bound to it within its single extraction borrow.
    {
        const auto warm = take(material::create_instance(runtime, "", {source_material, {{"roughness", .2f}}, {{}}}));
        const auto cool = take(material::create_instance(runtime, "", {source_material, {{"roughness", .8f}}, {{}}}));
        const std::array<p::MaterialBinding, 1> warm_binding{{{"slot/0", warm, true}}};
        const std::array<p::MaterialBinding, 1> cool_binding{{{"slot/0", cool, true}}};
        take(p::set_material_bindings(runtime, root, warm_binding));
        take(p::set_material_bindings(runtime, twin_mesh, warm_binding));
        const auto shared = take(scene.render_items());
        require(shared[0].surfaces->front() == shared[1].surfaces->front(),
                "Shared MI binding duplicated immutable surface identity.");
        take(p::set_material_bindings(runtime, twin_mesh, cool_binding));
    }
    const auto slotted = take(scene.render_items());
    const auto roughness = [](const p::RenderItem& item) {
        return take(item.surfaces->front()->evaluate({})).roughness;
    };
    require(slotted.size() == 2 && roughness(slotted[0]) == .2f && roughness(slotted[1]) == .8f,
            "Meshes bound to different slot materials did not keep their own surfaces.");
    take(p::set_material_bindings(runtime, root, {}));
    require(roughness(take(scene.render_items()).front()) == .25f,
            "Clearing an explicit binding did not restore its mesh default.");
    take(runtime.destroy(source_material.id()));
    const auto fallback = take(scene.render_items());
    require(fallback.size() == 2 && fallback[0].surfaces->front()->uses_default_material() &&
                !fallback[0].surfaces->front()->diagnostics().empty() &&
                take(p::read_static_mesh(runtime, root)).mesh.defaults.front().resource.id() == instance.id(),
            "A lost material ancestor did not preserve its binding and capture's diagnosed default.");
    auto parked = take(world.detach(entity.id()));
    const auto recovered = take(scene.render_items());
    require(recovered.size() == 1 && recovered[0].component == twin_mesh &&
                recovered[0].surfaces == fallback[1].surfaces && world.take_diagnostics().empty(),
            "Moving a mesh to another bucket position changed its captured content.");
    committed(world.restore(parked));
    parked.root.reset();
    auto twin_removal = take(world.detach(twin.id()));
    twin_removal.root.reset();
    twin.reset();
    require(take(scene.render_items()).size() == 1 && take(scene.light_items()).size() == 3,
            "Restoring the parked Entity changed extraction.");
    auto detached = take(world.detach(entity.id()));
    require(take(scene.light_items()).empty() && take(world.query(types.light_component)).empty(),
            "Detached history graph remained a light source.");
    committed(world.restore(detached));
    detached.root.reset();
    auto light_entity = take(w::create_entity(runtime));
    auto light_root = take(w::create_component(runtime, types.light_component));
    const std::array only_light{w::Attachment{light_root.id()}};
    take(world.set_transform(light_entity.id(), {}));
    committed(world.attach_components(light_entity.id(), only_light));
    committed(world.attach(light_entity.id()));
    require(take(scene.render_items()).size() == 1 && presented(world, scene, light_entity.id()) == 1 &&
                take(scene.light_items()).size() == 4,
            "A light-only Entity required a mesh or another root type.");
    // Every light is spatial: its owner keeps the Transform, and a light cannot join an Entity without one.
    reject(world.remove_transform(light_entity.id()), "A light's owner lost its Transform.");
    auto bare = take(w::create_entity(runtime));
    auto stray = take(w::create_component(runtime, types.light_component));
    const std::array stray_group{w::Attachment{stray.id()}};
    committed(world.attach(bare.id()));
    reject(world.attach_components(bare.id(), stray_group), "A light joined an Entity without a Transform.");
    reject(world.write(light_root.id(), light_path("kind"), EnumValue{types.light_kind, 3}),
           "The removed Sky light kind was accepted.");
    require(take(w::has_transform(runtime, light_entity.id())) && take(scene.light_items()).size() == 4,
            "Rejected spatial Light edits damaged presentation.");
    committed(world.close());
    committed(world.close());
    reject(scene.light_items(), "Closed World exposed a stale light directory.");
    const auto saved_identity = snapshot.front().component;
    entity.reset();
    point.reset();
    rectangle.reset();
    directional.reset();
    point_entity.reset();
    rectangle_entity.reset();
    directional_entity.reset();
    light_entity.reset();
    light_root.reset();
    bare.reset();
    stray.reset();
    collect(runtime);
    source_material.reset();
    instance.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && !runtime.type_of(saved_identity) &&
                snapshot.front().settings == rect_settings,
            "Light snapshots retained business objects or depended on their later state.");
    std::puts(
        "[pass] Lit data: MS/MI ProgramSurface captures, default grid, shared/explicit slots and diagnosed fallback, reflected LightSettings, HDR validation, Source inheritance, TRS extraction, identity/history and lifecycle");
}
void environment_and_group_writes() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    using Settings = atelier::rendering::SceneLightingSettings;
    require(take(runtime.find_type("world.EnvironmentComponent")) == types.environment_component &&
                take(runtime.is_a(types.environment_component, types.component)) &&
                take(scene.environment()) == Settings{},
            "EnvironmentComponent is not a registered Component, or a World without one did not use the defaults.");
    const auto field = [&](TypeId type, std::string_view name) { return take(runtime.find_field(type, name)); };
    const auto volume_type = take(runtime.native_type<p::ProbeVolume>());
    const auto path = [&](std::string_view name) { return w::FieldPath{field(types.environment_component, name)}; };
    const auto axis = [&](std::string_view volume, std::string_view bound, std::string_view component) {
        return w::FieldPath{field(types.environment_component, volume), field(volume_type, bound),
                            field(types.vector3, component)};
    };
    // A nonspatial owner: the environment needs no Transform.
    auto owner = take(w::create_entity(runtime));
    auto environment = take(w::create_component(runtime, types.environment_component));
    const std::array environment_group{w::Attachment{environment.id()}};
    committed(world.attach_components(owner.id(), environment_group));
    committed(world.attach(owner.id()));
    require(take(scene.environment()) == Settings{},
            "A default Environment did not reproduce the default World settings.");
    // At most one per World, a disabled one included.
    committed(world.set_enabled(environment.id(), false));
    require(take(scene.environment()) == Settings{}, "A disabled Environment changed the World settings.");
    auto second_owner = take(w::create_entity(runtime));
    auto second = take(w::create_component(runtime, types.environment_component));
    const std::array second_group{w::Attachment{second.id()}};
    committed(world.attach_components(second_owner.id(), second_group));
    reject(world.attach(second_owner.id()), "A second Environment entered a World holding a disabled one.");
    auto extra = take(w::create_component(runtime, types.environment_component));
    const std::array extra_group{w::Attachment{extra.id()}};
    reject(world.attach_components(owner.id(), extra_group), "A second Environment joined the first one's Entity.");
    committed(world.set_enabled(environment.id(), true));

    // Moving a volume past its old bounds: written one leaf at a time, min first,
    // the first leaf already fails min < max; written as one group it succeeds.
    const std::array names{"x", "y", "z"};
    std::vector<w::FieldPath> paths;
    std::vector<Scalar> values;
    for (std::size_t i = 0; i < 3; ++i) {
        paths.push_back(axis("volume0", "minimum", names[i]));
        values.push_back(20.f + float(i));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        paths.push_back(axis("volume0", "maximum", names[i]));
        values.push_back(32.f + float(i));
    }
    reject(world.write(environment.id(), paths[0], values[0]), "A leaf write accepted a minimum above the maximum.");
    take(world.write(environment.id(), paths, values));
    auto moved = Settings{};
    moved.probe_volumes[0].minimum = {20, 21, 22};
    moved.probe_volumes[0].maximum = {32, 33, 34};
    require(take(scene.environment()) == moved, "A group write did not move the volume as one edit.");
    const std::array sky_paths{path("sky_enabled"), path("sky_intensity")};
    const std::array<Scalar, 2> too_bright{true, 65504.5f};
    const std::array<Scalar, 2> sky_values{true, 2.f};
    reject(world.write(environment.id(), sky_paths, too_bright),
           "Sky radiance above the HDR scene range was accepted.");
    take(world.write(environment.id(), sky_paths, sky_values));
    moved.sky_enabled = true;
    moved.sky_intensity = 2;
    require(take(scene.environment()) == moved && atelier::rendering::sky_radiance(moved) == std::array{2.f, 2.f, 2.f},
            "The enabled sky did not reach the World settings.");
    const std::array duplicate_paths{path("exposure_ev"), path("exposure_ev")};
    const std::array<Scalar, 2> duplicate_values{1.f, 2.f};
    reject(world.write(environment.id(), duplicate_paths, duplicate_values),
           "A group write accepted the same field twice.");
    auto spatial = take(w::create_entity(runtime));
    take(world.set_transform(spatial.id(), {}));
    committed(world.attach(spatial.id()));
    const std::array transform_paths{w::FieldPath{types.position_fields[0].begin(), types.position_fields[0].end()}};
    const std::array<Scalar, 1> transform_values{1.f};
    reject(world.write(spatial.id(), transform_paths, transform_values),
           "A group write bypassed Transform validation.");
    require(take(scene.environment()) == moved && take(w::read_transform(runtime, spatial.id())).position.x == 0,
            "Rejected group writes changed a field.");

    // An admission rejection restores every leaf to its exact bytes, including a
    // NaN payload and -0 that no write can produce; the rejection is the admission's.
    take(world.add_admission(
        types.environment_component,
        [&](const w::World& owner_world, std::span<const w::World::AdmissionCandidate> candidates) -> Result<void> {
            for (const auto& candidate : candidates)
                if (std::get<float>(take(owner_world.runtime().read(candidate.component, path("sky_intensity")))) > 5)
                    return std::unexpected(Error{ErrorCode::invalid_member, "Test admission rejects bright skies."});
            return {};
        }));
    const auto payload = std::bit_cast<float>(0x7fc00123u);
    take(runtime.with_native<p::EnvironmentComponent>(environment.id(), [&](auto& value) -> Result<void> {
        value.sky_color.x = payload;
        value.exposure_ev = -0.f;
        return {};
    }));
    const std::array admitted_paths{
        w::FieldPath{field(types.environment_component, "sky_color"), field(types.vector3, "x")}, path("exposure_ev"),
        path("sky_intensity")};
    const std::array<Scalar, 3> admitted_values{1.f, 2.f, 10.f};
    const auto rejected = world.write(environment.id(), admitted_paths, admitted_values);
    require(!rejected && rejected.error().message == "Test admission rejects bright skies.",
            "A rejected group write did not return the admission's own error.");
    take(runtime.with_native<const p::EnvironmentComponent>(environment.id(), [&](const auto& value) -> Result<void> {
        require(std::bit_cast<std::uint32_t>(value.sky_color.x) == 0x7fc00123u &&
                    std::bit_cast<std::uint32_t>(value.exposure_ev) == 0x80000000u && value.sky_intensity == 2.f,
                "A rejected group write did not restore every leaf's exact bytes.");
        return {};
    }));
    // The NaN came from outside the write paths: extraction reports it and uses the defaults.
    require(take(scene.environment()) == Settings{},
            "An invalid Environment did not fall back to the default settings.");
    const auto report = world.take_diagnostics();
    require(report.size() == 1 && report[0].object == environment.id() && report[0].entity == owner.id() &&
                report[0].kind == w::DiagnosticKind::invalid_value && report[0].phase == w::DiagnosticPhase::lighting &&
                report[0].field == std::vector<std::string>{"sky_color", "x"} &&
                report[0].type == "world.EnvironmentComponent",
            "An invalid Environment was not reported on its component.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    reject(scene.environment(), "Halted extraction accepted an invalid Environment.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    take(world.write(environment.id(), admitted_paths[0], 1.f));
    require(take(scene.environment()).sky_color == std::array{1.f, 1.f, 1.f},
            "Repairing the Environment did not restore its settings.");
    // The sky map is a hidden key and the map it resolved to, set only as a pair.
    require(!runtime.find_field(types.environment_component, "sky_map"), "The sky map key is addressable as a field.");
    const std::array<float, 6> texels{1.f, 1.f, 1.f, 4.f, 4.f, 4.f};
    const auto sky = std::make_shared<const atelier::rendering::SkyMap>(2u, 1u, std::span<const float>(texels));
    reject(p::set_sky_map_binding(runtime, environment.id(), {"", sky}),
           "A sky map without its asset key was accepted.");
    reject(p::set_sky_map_binding(runtime, environment.id(), {std::string("sky\0key", 7), {}}),
           "A sky map key holding NUL was accepted.");
    take(p::set_sky_map_binding(runtime, environment.id(), {"project/sky", sky}));
    take(world.write(environment.id(), path("sky_rotation"), 200.f));
    const auto bound = take(p::read_sky_map(runtime, environment.id()));
    const auto lit = take(scene.environment());
    require(bound.source == "project/sky" && bound.resolved == sky && lit.sky_map == sky && lit.sky_rotation == 180.f,
            "The sky map binding or the clamped sky rotation did not reach the World settings.");
    std::optional<Result<void>> nested_sky;
    take(world.add_admission(
        types.environment_component,
        [&](const w::World&, std::span<const w::World::AdmissionCandidate> candidates) -> Result<void> {
            if (!nested_sky)
                nested_sky = p::set_sky_map_binding(runtime, candidates.front().component, {});
            return {};
        }));
    take(world.write(environment.id(), path("sky_rotation"), -90.f));
    require(nested_sky && !*nested_sky && nested_sky->error().code == ErrorCode::busy &&
                take(p::read_sky_map(runtime, environment.id())).resolved == sky,
            "A sky map binding changed while the World ran an admission.");
    take(p::set_sky_map_binding(runtime, environment.id(), {}));
    require(take(p::read_sky_map(runtime, environment.id())).source.empty() && !take(scene.environment()).sky_map &&
                take(scene.environment()).sky_rotation == -90.f,
            "Clearing the sky map left its key or map, or changed the rotation.");
    committed(world.close());
    // Content attached before a Scene exists is checked when it is created, disabled and derived Environments included.
    {
        const auto derived =
            take(NativeRegistration<DerivedEnvironment>{runtime, "lighting.DerivedEnvironment", TypeKind::object}
                     .base<p::EnvironmentComponent>()
                     .publish());
        w::World loaded(runtime, types);
        const auto add = [&](TypeId type) {
            auto holder = take(w::create_entity(runtime));
            auto component = take(w::create_component(runtime, type));
            const std::array group{w::Attachment{component.id()}};
            committed(loaded.attach_components(holder.id(), group));
            committed(loaded.attach(holder.id()));
            return std::pair{std::move(holder), component.id()};
        };
        const auto disabled = add(types.environment_component);
        committed(loaded.set_enabled(disabled.second, false));
        const auto active = add(derived);
        reject(p::Scene::create(loaded, types), "A Scene was created over a World that already held two Environments.");
        take(loaded.detach(active.first.id()));
        const auto accepted = p::Scene::create(loaded, types);
        require(accepted && take(accepted->environment()) == Settings{},
                "A Scene over one disabled Environment was rejected or did not use the default settings.");
        committed(loaded.close());
    }
    std::puts(
        "[pass] Environment: defaults without one, uniqueness with disabled ones also for content before the Scene, invalid values fall back, atomic group writes with exact restore, the sky map set only as a key-and-map pair");
}
void material_dependency_updates() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    w::World other_world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    const auto other = take(p::Scene::create(other_world, types));
    auto source = take(material::create_source(runtime, "source", {mesh_material_definition(), {}}));
    auto a = take(material::create_instance(runtime, "a", {source, {}, {}}));
    auto b = take(material::create_instance(runtime, "b", {source, {}, {}}));
    auto leaf = take(material::create_instance(runtime, "leaf", {a, {}, {}}));
    auto first = take(p::create_static_mesh_entity(world, {geometry::cube_mesh(), {}, {}, {{"slot/0", leaf, false}}}));
    auto second = take(p::create_static_mesh_entity(world, {geometry::cube_mesh(), {}, {}, {{"slot/0", leaf, false}}}));
    auto foreign =
        take(p::create_static_mesh_entity(other_world, {geometry::cube_mesh(), {}, {}, {{"slot/0", leaf, false}}}));
    const auto first_mesh = take(w::components(runtime, first.id())).front().id();
    const auto second_mesh = take(w::components(runtime, second.id())).front().id();
    committed(world.attach(first.id()));
    committed(world.attach(second.id()));
    committed(other_world.attach(foreign.id()));
    auto version = take(scene.update());
    (void)take(other.update());
    const auto old = scene.mesh_slots()[0].item->surfaces;
    take(material::update_instance(runtime, leaf.id(), {b, {}, {}}));
    require(take(scene.update()) == version && scene.mesh_slots()[0].item->surfaces == old,
            "Same-Surface reparent invented a visible change.");
    (void)take(other.update());
    take(material::update_instance(runtime, a.id(), {source, {{"roughness", .9f}}, {{}}}));
    require(take(scene.update()) == version, "Former parent still changed the rebound material.");
    take(material::update_instance(runtime, b.id(), {source, {{"roughness", .7f}}, {{}}}));
    version = take(scene.update());
    (void)take(other.update());
    for (const auto& slot : scene.mesh_slots())
        require(near(take(slot.item->surfaces->front()->evaluate({})).roughness, .7f),
                "Shared-parent edit did not reach every rebound slot.");
    require(near(take(other.mesh_slots()[0].item->surfaces->front()->evaluate({})).roughness, .7f) &&
                near(take(old->front()->evaluate({})).roughness, .25f),
            "One Scene consumed another's changes or mutated an old snapshot.");
    take(runtime.write(first.id(), types.position_fields[0], Scalar{3.f}));
    const auto surfaces = scene.mesh_slots()[0].item->surfaces;
    version = take(scene.update());
    require(scene.mesh_slots()[0].item->world[12] == 3 && scene.mesh_slots()[0].item->surfaces == surfaces &&
                take(scene.update()) == version,
            "Pure space movement replaced material content or failed to settle.");
    // The hot/empty-dirty path must still reject reference-traversal admission.
    std::pair<const p::Scene*, unsigned> visits{&scene, 0};
    take(runtime.visit_references(first_mesh, {&visits, [](void* context, const ObjectReference&) -> Result<void> {
                                                   auto& state = *static_cast<decltype(visits)*>(context);
                                                   const auto result = state.first->update();
                                                   require(!result && result.error().code == ErrorCode::busy,
                                                           "Hot Scene bypassed Runtime traversal admission.");
                                                   ++state.second;
                                                   return {};
                                               }}));
    require(visits.second != 0, "Traversal admission fixture did not visit a reference.");
    (void)take(scene.update());
    take(runtime.destroy(b.id()));
    (void)take(scene.update());
    require(scene.mesh_slots()[0].item->surfaces->front()->uses_default_material(),
            "A dead parent missing from changed_objects did not refresh the Scene.");
    // A new identity is observed only after the live leaf republishes its edge.
    auto replacement = take(material::create_instance(runtime, "replacement", {source, {{"roughness", .4f}}, {{}}}));
    take(material::update_instance(runtime, leaf.id(), {replacement, {}, {}}));
    (void)take(scene.update());
    require(near(take(scene.mesh_slots()[0].item->surfaces->front()->evaluate({})).roughness, .4f),
            "A replacement parent did not recover the live leaf.");
    auto removal = take(world.remove_component(first_mesh));
    (void)take(scene.update());
    take(material::update_instance(runtime, replacement.id(), {source, {{"roughness", .8f}}, {{}}}));
    (void)take(scene.update());
    require(scene.mesh_slots().size() == 1 && scene.mesh_slots()[0].item->component == second_mesh &&
                near(take(scene.mesh_slots()[0].item->surfaces->front()->evaluate({})).roughness, .8f),
            "Layout removal left dependency edges pointing to old bucket positions.");
    committed(world.restore(removal));
    (void)take(scene.update());
    const auto first_failure = scene.mesh_slots()[0].item->component;
    take(runtime.destroy(leaf.id()));
    (void)take(scene.update());
    (void)take(scene.update());
    const auto reports = world.take_diagnostics();
    require(reports.size() == 2 && reports[0].object == first_failure && reports[0].repeats == 2 &&
                reports[1].repeats == 2 && reports[0].field == std::vector<std::string>{"materials"},
            "Failed material slots did not retry in bucket order with their original diagnostics.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    const auto failed = scene.update();
    const auto direct = material::capture_program(runtime, leaf.id());
    require(!failed && !direct && failed.error().code == direct.error().code &&
                failed.error().message == direct.error().message,
            "Halt did not preserve the material capture's first error.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    for (const auto mesh : {first_mesh, second_mesh})
        take(p::set_mesh_binding(runtime, mesh, {"", geometry::cube_mesh(), {{"slot/0", replacement, false}}}));
    (void)take(scene.update());
    require(scene.mesh_slots().size() == 2 && scene.mesh_slots()[0].item && scene.mesh_slots()[1].item,
            "A failed whole update did not recover after rebinding.");
    // Live empty images and disabled overrides matter even when they are not sampled.
    Runtime compiler;
    take(material::register_program_types(compiler));
    const auto program = take(source::compile_material(
        compiler, take(compiler.native_type<material::Context>()), take(compiler.native_type<material::Output>()),
        take(compiler.native_type<material::TextureInput>()), "dependency-texture.source", R"(
            struct Params { float roughness; };
            struct Textures { material.TextureInput color; };
            Params defaults() { Params p; p.roughness=0.5; return p; }
            material.Output shade(material.Context c, Params p, Textures t) {
                vector4 color=sample(t.color,c.uv); material.Output o;
                o.base_color=vector3(color.x,color.y,color.z); o.roughness=p.roughness;
                o.normal_world=c.normal_world; return o;
            }
        )"));
    const std::array<material::TextureDefault, 1> inputs{
        {{"color", {"base"}, material::TextureColor::linear, {.2f, .3f, .4f, 1}}}};
    const auto definition = take(material::make_definition(program, {}, inputs));
    auto image = take(material::create_texture(runtime, "base", {}));
    auto disabled = take(material::create_texture(runtime, "disabled", {}));
    auto textured_source = take(material::create_source(runtime, "textured", {definition, {image}}));
    auto textured = take(material::create_instance(
        runtime, "textured-child",
        {textured_source, {{"color", material::TextureBinding{"disabled"}, false}}, {disabled}}));
    for (const auto mesh : {first_mesh, second_mesh})
        take(p::set_mesh_binding(runtime, mesh, {"", geometry::cube_mesh(), {{"slot/0", textured, false}}}));
    (void)take(scene.update());
    const auto empty_surfaces = scene.mesh_slots()[0].item->surfaces;
    require(empty_surfaces->front()->diagnostics().size() == 2, "Missing-image fixture did not report both edges.");
    const auto red = std::make_shared<const material::Image>(1, 1, std::vector<std::uint8_t>{255, 0, 0, 255});
    take(material::update_texture(runtime, image.id(), red));
    (void)take(scene.update());
    for (const auto& slot : scene.mesh_slots())
        require(near(take(slot.item->surfaces->front()->evaluate({})).base_color.x, 1.f) &&
                    slot.item->surfaces->front()->diagnostics().size() == 1,
                "Live empty texture recovery did not reach every dependent slot.");
    take(material::update_texture(runtime, disabled.id(), red));
    (void)take(scene.update());
    require(scene.mesh_slots()[0].item->surfaces->front()->diagnostics().empty(),
            "Unsampled disabled texture recovery did not clear its diagnostic.");
    const auto red_surfaces = scene.mesh_slots()[0].item->surfaces;
    take(runtime.destroy(image.id()));
    (void)take(scene.update());
    require(near(take(scene.mesh_slots()[0].item->surfaces->front()->evaluate({})).base_color.x, .2f) &&
                near(take(red_surfaces->front()->evaluate({})).base_color.x, 1.f) &&
                near(take(empty_surfaces->front()->evaluate({})).base_color.x, .2f),
            "Texture death did not restore typed defaults or changed old image leases.");
    version = take(scene.update());
    require(take(scene.update()) == version, "A successful dead-texture fallback did not settle.");
    committed(other_world.close());
    committed(world.close());
    std::puts(
        "[pass] Material dependencies: equal-owner reparent, shared changes, independent Scenes, death/recovery, layout, old leases, busy admission and ordered isolate/halt retry");
}

// Scene::update keeps one cached extraction: an unchanged update keeps the version;
// every presented input advances it, values written past World included, and only
// the slots that changed take the new serial. Failures are reported on every update.
void presentation_cache() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    auto parent = take(w::create_entity(runtime));
    take(world.set_transform(parent.id(), {}));
    auto mesh_entity = take(p::create_static_mesh_entity(world, {geometry::cube_mesh(), {{1, 0, 0}, {}, {1, 1, 1}}}));
    const auto mesh = take(w::components(runtime, mesh_entity.id())).front().id();
    auto still_entity = take(p::create_static_mesh_entity(world, {geometry::cube_mesh(), {{5, 0, 0}, {}, {1, 1, 1}}}));
    const auto still = take(w::components(runtime, still_entity.id())).front().id();
    auto light_entity = take(w::create_entity(runtime));
    take(world.set_transform(light_entity.id(), {{0, 2, 0}, {}, {1, 1, 1}}));
    auto light = take(w::create_component(runtime, types.light_component));
    const auto light_id = light.id();
    const std::array light_group{w::Attachment{light_id}};
    committed(world.attach_components(light_entity.id(), light_group));
    auto environment_entity = take(w::create_entity(runtime));
    auto environment = take(w::create_component(runtime, types.environment_component));
    const std::array environment_group{w::Attachment{environment.id()}};
    committed(world.attach_components(environment_entity.id(), environment_group));
    committed(world.reparent(mesh_entity.id(), parent.id(), w::ReparentRule::keep_local));
    committed(world.reparent(light_entity.id(), parent.id(), w::ReparentRule::keep_local));
    committed(world.attach(parent.id()));
    committed(world.attach(still_entity.id()));
    committed(world.attach(environment_entity.id()));
    const auto source_material = take(material::create_source(runtime, "", {mesh_material_definition(), {}}));
    material::InstanceState defaults{source_material, {{"roughness", .2f}}, {{}}};
    const auto default_material = take(material::create_instance(runtime, "", defaults));
    take(p::set_mesh_binding(runtime, mesh,
                             {"cache/mesh", geometry::cube_mesh(), {{"slot/0", default_material, false}}}));
    const auto field = [&](TypeId type, std::string_view name) { return take(runtime.find_field(type, name)); };
    const w::FieldPath intensity{field(types.light_component, "settings"), field(types.light_settings, "intensity")};
    const w::FieldPath range{field(types.light_component, "settings"), field(types.light_settings, "range")};
    const w::FieldPath exposure{field(types.environment_component, "exposure_ev")};
    const auto& x = types.position_fields[0];
    const auto slot = [&](ObjectId component) -> const p::MeshSlot& {
        const auto members = take(world.members(scene.meshes()));
        return scene.mesh_slots()[std::size_t(std::ranges::find(members, component, &w::CapabilityMember::component) -
                                              members.begin())];
    };
    auto version = take(scene.update());
    const auto still_cursor = take(runtime.change_cursor());
    for (int frame = 0; frame < 3; ++frame)
        require(take(scene.update()) == version, "An unchanged presentation advanced its version.");
    std::vector<ObjectId> written;
    runtime.changed_objects(still_cursor, written);
    require(written.empty(), "Updating an unchanged presentation wrote an object.");
    const auto advanced = [&](const char* message) {
        const auto next = take(scene.update());
        require(next != version, message);
        require(take(scene.update()) == next, "Updating a changed presentation again advanced its version.");
        version = next;
    };
    const auto untouched = slot(still).changed;
    const auto unbound_surfaces = slot(still).item->surfaces;
    defaults.entries[0].value = .25f;
    take(material::update_instance(runtime, default_material.id(), defaults));
    advanced("Editing a mesh default material did not advance the version.");
    require(slot(mesh).changed == version && take(slot(mesh).item->surfaces->front()->evaluate({})).roughness == .25f &&
                slot(still).changed == untouched && slot(still).item->surfaces == unbound_surfaces,
            "A mesh default edit did not rebuild exactly its own slot.");
    defaults.entries[0].value = .3f;
    take(material::update_instance(runtime, default_material.id(), defaults));
    advanced("Editing a mesh default resource did not advance the version.");
    take(runtime.write(light_id, intensity, Scalar{7.f}));
    advanced("A light edit past World did not advance the version.");
    require(scene.lights_changed() == version && scene.light_snapshot().front().settings.intensity == 7 &&
                slot(still).changed == untouched,
            "A light edit did not refresh the lights alone.");
    take(world.write(environment.id(), exposure, 1.f));
    advanced("An Environment edit did not advance the version.");
    take(runtime.write(parent.id(), x, Scalar{3.f}));
    advanced("An ancestor TRS write did not advance the version.");
    require(slot(mesh).item->world[12] == 4 && slot(still).changed == untouched,
            "An ancestor move did not rebuild exactly the slots below it.");
    committed(world.set_enabled(light_id, false));
    advanced("Disabling a light did not advance the version.");
    committed(world.set_enabled(light_id, true));
    advanced("Enabling a light did not advance the version.");
    auto removal = take(world.remove_component(light_id));
    advanced("Removing a light did not advance the version.");
    committed(world.restore(removal));
    advanced("Restoring a light did not advance the version.");
    committed(world.reparent(light_entity.id(), {}, w::ReparentRule::keep_local));
    advanced("Reparenting a light did not advance the version.");
    require(scene.mesh_layout() == version, "A structural edit did not rebuild the mesh layout.");
    material::InstanceState shared{source_material, {{"roughness", .2f}}, {{}}};
    const auto resource = take(material::create_instance(runtime, "", shared));
    const std::array<p::MaterialBinding, 1> binding{{{"slot/0", resource, true}}};
    take(p::set_material_bindings(runtime, mesh, binding));
    advanced("Binding a slot material did not advance the version.");
    const auto before_edit = slot(mesh).item->surfaces;
    shared.entries[0].value = .6f;
    take(material::update_instance(runtime, resource.id(), shared));
    advanced("Editing a bound material resource did not advance the version.");
    require(take(slot(mesh).item->surfaces->front()->evaluate({})).roughness == .6f &&
                take(before_edit->front()->evaluate({})).roughness == .2f,
            "The bound material edit did not reach the cache or mutated an older capture.");
    const auto bound_surfaces = slot(mesh).item->surfaces;
    const auto bound_cursor = take(runtime.change_cursor());
    for (int frame = 0; frame < 3; ++frame)
        require(take(scene.update()) == version && slot(mesh).item->surfaces == bound_surfaces,
                "An unchanged bound material rebuilt its surface vector or advanced the version.");
    runtime.changed_objects(bound_cursor, written);
    require(written.empty(), "An unchanged bound material capture wrote Runtime objects.");
    defaults.entries[0].value = .4f;
    take(material::update_instance(runtime, default_material.id(), defaults));
    require(take(scene.update()) == version && slot(mesh).item->surfaces == bound_surfaces,
            "An overridden mesh default replaced the explicit material candidate.");
    take(p::set_material_bindings(runtime, mesh, {}));
    advanced("Clearing an explicit material did not restore the current mesh default.");
    require(take(slot(mesh).item->surfaces->front()->evaluate({})).roughness == .4f,
            "Clearing a binding restored a stale default material.");
    require(world.take_diagnostics().empty(), "Valid presentation edits reported diagnostics.");
    // An isolated item leaves the cache once; while it stays isolated the version
    // holds and every update reports it again.
    take(runtime.write(light_id, range, Scalar{0.f}));
    advanced("Isolating a light did not advance the version.");
    auto reports = world.take_diagnostics();
    require(reports.size() == 1 && reports[0].repeats == 2 && scene.light_snapshot().empty(),
            "An isolated light stayed presented or was not reported on each update.");
    take(runtime.write(light_id, range, Scalar{10.f}));
    advanced("Repairing an isolated light did not advance the version.");
    const auto placed = slot(still).changed; // The reparent above rebuilt the layout.
    take(runtime.write(parent.id(), x, Scalar{std::numeric_limits<float>::infinity()}));
    advanced("Isolating a space did not advance the version.");
    reports = world.take_diagnostics();
    require(reports.size() == 1 && reports[0].repeats == 2 && !slot(mesh).item && slot(still).item &&
                slot(still).changed == placed,
            "A broken space did not isolate exactly its mesh, or was not reported on each update.");
    take(runtime.write(parent.id(), x, Scalar{3.f}));
    advanced("Repairing an isolated space did not advance the version.");
    require(scene.light_snapshot().size() == 1 && slot(mesh).item && take(scene.render_items()).size() == 2 &&
                world.take_diagnostics().empty(),
            "Repaired items did not return to the cache.");
    // The lights are not extracted again here; the invalid Environment still counts every update.
    const w::FieldPath sky{field(types.environment_component, "sky_intensity")};
    take(runtime.write(environment.id(), sky, Scalar{1e6f}));
    (void)take(scene.update());
    (void)take(scene.update());
    reports = world.take_diagnostics();
    require(reports.size() == 1 && reports[0].repeats == 2,
            "A persistent invalid Environment was not reported on each update.");
    committed(world.close());
    std::puts(
        "[pass] Presentation cache: unchanged updates keep the version; World, Runtime, Environment, TRS, enablement, composition, reparent and material resource edits advance it and rebuild only their slots; isolated items and an invalid Environment are reported on every update");
}

struct DerivedCamera : p::CameraComponent {};
void scene_cameras() {
    Runtime runtime;
    const auto types = register_all_types(runtime);
    const auto derived = take(NativeRegistration<DerivedCamera>{runtime, "camera.DerivedCamera", TypeKind::object}
                                  .base<p::CameraComponent>()
                                  .publish());
    w::World world(runtime, types);
    const auto scene = take(p::Scene::create(world, types));
    const auto make_camera = [&](TypeId type) {
        auto entity = take(w::create_entity(runtime));
        auto camera = take(w::create_component(runtime, type));
        take(world.set_transform(entity.id(), {}));
        committed(world.attach_components(entity.id(), std::array{w::Attachment{camera.id()}}));
        return std::pair{std::move(entity), std::move(camera)};
    };
    auto [a, ca] = make_camera(types.camera_component);
    auto [b, cb] = make_camera(derived);
    auto group = take(w::create_entity(runtime));
    auto origin = take(w::create_entity(runtime));
    // Deliberately different composition and spatial parents; zero and negative
    // scales affect translation, but camera axes are rotations only.
    const auto quarter = std::sqrt(.5f);
    take(world.set_transform(group.id(), {{99, 88, 77}, {quarter, 0, 0, quarter}, {2, 3, 4}}));
    take(world.set_transform(origin.id(), {{4, 5, 6}, {0, quarter, 0, quarter}, {-2, 0, 3}}));
    take(world.set_transform(a.id(), {{1, 2, 3}, {0, 0, quarter, quarter}, {-1, 4, 2}}));
    committed(world.reparent(a.id(), group.id(), w::ReparentRule::keep_local));
    committed(world.set_space_reference(a.id(), origin.id(), w::ReparentRule::keep_local));
    committed(world.attach_batch(std::array{group.id(), origin.id(), b.id()}));
    auto environment = take(w::create_component(runtime, types.environment_component));
    committed(world.attach_components(group.id(), std::array{w::Attachment{environment.id()}}));
    const auto selected = take(runtime.find_field(types.environment_component, "main_camera"));
    require(take(runtime.read_reference(environment.id(), selected)).is_null(), "Main camera did not default to null.");
    reject(scene.main_camera(), "An unselected scene chose a camera implicitly.");
    take(runtime.write_reference(environment.id(), selected, a.reference()));
    const auto roots = runtime.root_count();
    const auto snapshot = take(scene.main_camera());
    const auto forward = math::rotate(snapshot.rotation, math::Vector3{0, 0, 1});
    const auto up = math::rotate(snapshot.rotation, math::Vector3{0, 1, 0});
    require(snapshot.entity == a.id() && near(snapshot.position.x, 13) && near(snapshot.position.y, 5) &&
                near(snapshot.position.z, 8) && near(forward.x, 1) && near(forward.y, 0) && near(forward.z, 0) &&
                near(up.x, 0) && near(up.y, 0) && near(up.z, 1) && runtime.root_count() == roots,
            "Camera lost its true spatial chain, incorporated scale into rotation, or retained roots.");
    const auto matrices = take(snapshot.matrices(1.7f));
    const auto origin_view = math::transform_point(snapshot.position, matrices.view);
    const auto near_clip = math::Vector4{0, 0, snapshot.near_clip, 1} * matrices.projection;
    const auto far_clip = math::Vector4{0, 0, snapshot.far_clip, 1} * matrices.projection;
    const auto inverse = math::try_inverse(matrices.projection);
    require(bool(inverse), "Infinite camera projection was not invertible.");
    const auto direction = math::Vector4{0, 0, 0, 1} * (*inverse);
    require(near(origin_view.x, 0) && near(origin_view.y, 0) && near(origin_view.z, 0) &&
                near(near_clip.z / near_clip.w, 1) &&
                near(far_clip.z / far_clip.w, snapshot.near_clip / snapshot.far_clip) &&
                near(matrices.far_depth, snapshot.near_clip / snapshot.far_clip) && direction.w == 0 &&
                direction.z > 0 && near(matrices.projection[5] / matrices.projection[0], 1.7f),
            "Camera matrices lost the eye, aspect ratio or infinite reversed-Z depth mapping.");
    reject(snapshot.matrices(0), "A zero camera aspect ratio was accepted.");
    const auto revision = world.revision();
    take(runtime.write_reference(environment.id(), selected, b.reference()));
    require(world.revision() == revision && take(scene.main_camera()).entity == b.id(),
            "A camera switch waited for a structural revision.");
    take(runtime.write_reference(environment.id(), selected, a.reference()));
    for (const auto object : std::array{a.id(), group.id(), ca.id(), environment.id()}) {
        committed(world.set_enabled(object, false));
        reject(scene.main_camera(), "A disabled camera chain remained usable.");
        require(take(runtime.read_reference(environment.id(), selected)).id() == a.id(),
                "Consumption cleared a disabled selection.");
        committed(world.set_enabled(object, true));
        require(take(scene.main_camera()).entity == a.id(), "Re-enabling did not restore the main camera.");
    }
    auto removed = take(world.remove_component(ca.id()));
    reject(scene.main_camera(), "A selected Entity without its CameraComponent remained usable.");
    committed(world.restore(removed));
    const auto fov = take(runtime.find_field(types.camera_component, "vertical_fov"));
    const auto near_field = take(runtime.find_field(types.camera_component, "near_clip"));
    const auto far_field = take(runtime.find_field(types.camera_component, "far_clip"));
    take(world.write(ca.id(), std::array{fov}, 65.f));
    require(take(scene.main_camera()).vertical_fov == 65.f, "Camera projection edits were cached by World revision.");
    reject(world.write(ca.id(), std::array{near_field}, 30000.f), "Equal near/far was accepted by authoring.");
    require(take(scene.main_camera()).near_clip == .05f, "Rejected projection edit was not restored.");
    take(runtime.write(ca.id(), far_field, .05f));
    reject(scene.main_camera(), "A direct invalid projection reached the camera snapshot.");
    take(runtime.write(ca.id(), far_field, 30000.f));
    reject(runtime.write(ca.id(), fov, std::numeric_limits<float>::infinity()),
           "The Runtime accepted non-finite bounded FOV.");
    take(runtime.with_native<p::CameraComponent>(ca.id(), [](auto& camera) -> Result<void> {
        camera.vertical_fov = std::numeric_limits<float>::infinity();
        return {};
    }));
    reject(scene.main_camera(), "Non-finite native camera FOV was accepted by the consumer.");
    take(runtime.write(ca.id(), fov, 65.f));
    take(runtime.write(origin.id(), types.position_fields[0], std::numeric_limits<float>::infinity()));
    reject(scene.main_camera(), "A non-finite camera ancestor was accepted.");
    take(runtime.write(origin.id(), types.position_fields[0], 4.f));
    require(take(scene.main_camera()).entity == a.id(), "Repairing the spatial chain did not restore the camera.");

    // Domain admission rejects the whole incoming batch before publication, and
    // receives the proposed owner even while a component has no owner yet.
    auto extra = take(w::create_component(runtime, derived));
    committed(world.set_enabled(extra.id(), false));
    reject(world.attach_components(a.id(), std::array{w::Attachment{extra.id()}}),
           "A disabled derived second camera was accepted.");
    require(take(w::inspect_component(runtime, extra.id())).owner == ObjectId{} &&
                take(world.members(scene.cameras())).size() == 2,
            "Rejected camera attachment partially published ownership or a capability.");
    auto empty = take(w::create_entity(runtime));
    take(world.set_transform(empty.id(), {}));
    committed(world.attach(empty.id()));
    auto extra2 = take(w::create_component(runtime, types.camera_component));
    reject(world.attach_components(empty.id(), std::array{w::Attachment{extra.id()}, w::Attachment{extra2.id()}}),
           "Two incoming cameras on the same owner were accepted.");
    require(take(w::components(runtime, empty.id())).empty(),
            "A rejected camera batch partially published components.");
    auto old = take(world.remove_component(cb.id()));
    committed(world.attach_components(b.id(), std::array{w::Attachment{extra.id()}}));
    reject(world.restore(old), "Restoring a second camera bypassed admission.");
    auto detached = take(world.detach(b.id()));
    committed(world.attach_components(b.id(), std::array{w::Attachment{cb.id()}}));
    reject(world.restore(detached), "Restoring a detached graph with duplicate cameras bypassed admission.");
    require(!world.contains(b.id()), "Rejected forest restore published membership.");
    auto discarded = take(world.remove_component(cb.id()));
    committed(world.restore(detached));
    auto duplicate = take(w::create_entity(runtime));
    take(world.set_transform(duplicate.id(), {}));
    auto extra3 = take(w::create_component(runtime, types.camera_component));
    committed(world.attach_components(duplicate.id(), std::array{w::Attachment{cb.id()}, w::Attachment{extra3.id()}}));
    auto innocent = take(w::create_entity(runtime));
    reject(world.attach_batch(std::array{innocent.id(), duplicate.id()}),
           "A forest with duplicate cameras was accepted.");
    require(!world.contains(innocent.id()) && !world.contains(duplicate.id()),
            "A rejected forest published an unrelated root.");
    {
        w::World unguarded(runtime, types);
        committed(unguarded.attach(duplicate.id()));
        reject(p::Scene::create(unguarded, types), "Scene creation accepted existing duplicate cameras.");
        committed(unguarded.close());
    }
    {
        w::World foreign(runtime, types);
        auto [other, other_camera] = make_camera(types.camera_component);
        committed(foreign.attach(other.id()));
        take(runtime.write_reference(environment.id(), selected, other.reference()));
        reject(scene.main_camera(), "A same-Runtime foreign World camera was adopted.");
        require(take(runtime.read_reference(environment.id(), selected)).id() == other.id(),
                "Consumer rewrote a foreign selection.");
        committed(foreign.close());
    }
    // Native/generated game code uses Runtime's reference barrier. A selection
    // made during Late Tick is invalidated by that phase's queued disable.
    committed(world.set_enabled(extra.id(), true));
    bool disable_camera{};
    take(world.bind_behavior(types.entity, {.late_tick = [&](Runtime& rt, ObjectId id, float) -> Result<void> {
                                 if (id != group.id())
                                     return {};
                                 take(rt.write_reference(environment.id(), selected, b.reference()));
                                 if (disable_camera)
                                     take(world.request(
                                         {.kind = w::RequestKind::entity_enabled, .target = b.id(), .enabled = false}));
                                 return {};
                             }}));
    committed(world.start());
    const auto before_tick = world.revision();
    take(world.update(.016f));
    require(world.revision() == before_tick && take(scene.main_camera()).entity == b.id(),
            "Late Tick reference write waited for a World revision.");
    disable_camera = true;
    take(world.update(.016f));
    reject(scene.main_camera(), "Camera consumption ignored the final Late Tick flush.");
    require(take(runtime.read_reference(environment.id(), selected)).id() == b.id(),
            "Late flush rejection cleared the game's weak selection.");
    committed(world.stop());
    take(runtime.write_reference(environment.id(), selected, a.reference()));
    const auto stale = a.id();
    committed(w::destroy_entity(runtime, a.id()));
    reject(scene.main_camera(), "A destroyed camera weak target was adopted.");
    require(take(runtime.read_reference(environment.id(), selected)).id() == stale,
            "A stale weak selection lost its identity.");
    take(runtime.write_reference(environment.id(), selected,
                                 take(runtime.null_reference(types.entity, ValueForm::weak_reference))));
    reject(scene.main_camera(), "Explicitly clearing the main camera chose a fallback.");
    committed(world.close());
    std::puts(
        "[pass] Scene cameras: explicit weak selection, no fallback, fresh snapshots, spatial rotation independent of signed/zero scale, projection validation, atomic per-Entity uniqueness across attach/restore/existing graphs");
}

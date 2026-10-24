struct DerivedLight : p::LightComponent { float extra{3.f}; };
struct DerivedEnvironment : p::EnvironmentComponent {};
void lit_materials_and_lights() {
    Runtime runtime; const auto types=register_all_types(runtime);
    const auto derived=take(NativeRegistration<DerivedLight>{runtime,"lighting.DerivedLight",TypeKind::object}
        .base<p::LightComponent>().field<&DerivedLight::extra>("extra").publish());
    require(take(runtime.native_type<p::LightKind>())==types.light_kind &&
        take(runtime.native_type<p::LightSettings>())==types.light_settings &&
        take(runtime.find_type("world.LightComponent"))==types.light_component && take(runtime.is_a(derived,types.component)),
        "Light types did not enter the shared reflected hierarchy.");
    source::Program program(runtime,types.light_component);
    publish(program,"void tick(float dt) { self.settings.intensity = self.settings.intensity + dt; }");
    const auto lease=take(program.pin());
    w::World world(runtime,types);
    const auto scene=take(p::Scene::create(world,types));
    std::vector<ObjectId> begins,ends;
    take(world.bind_behavior(types.light_component,{
        [&](Runtime&,ObjectId object)->Result<void>{begins.push_back(object);return {};},
        [lease](Runtime&,ObjectId object,float dt)->Result<void>{
            const std::array<Scalar,1> arguments{dt};auto invoked=lease.invoke(object,arguments);
            if (!invoked) return std::unexpected(invoked.error()); return {};
        },
        [&](Runtime&,ObjectId object)->Result<void>{ends.push_back(object);return {};}
    }));
    require(p::Material{}.roughness==.65f && p::Material{}.metallic==0 && p::Material{}.emissive==math::Vector3{},"Material defaults changed unexpectedly.");
    const p::Material material{{.2f,.3f,.4f},.25f,.75f,{4,8,16}};
    auto entity=take(p::create_static_mesh_entity(world,{geometry::cube_mesh(),{},material}));
    const auto root=take(w::components(runtime,entity.id())).front().id();
    auto point_entity=take(w::create_entity(runtime));
    auto rectangle_entity=take(w::create_entity(runtime));
    auto directional_entity=take(w::create_entity(runtime));
    auto point=take(w::create_component(runtime,types.light_component));
    auto rectangle=take(w::create_component(runtime,derived));
    auto directional=take(w::create_component(runtime,types.light_component));
    const auto point_id=point.id(), rectangle_id=rectangle.id(), directional_id=directional.id();
    p::LightSettings point_settings;point_settings.intensity=20;point_settings.range=25;point_settings.source_radius=.2f;
    p::LightSettings rect_settings;rect_settings.kind=p::LightKind::rectangle;rect_settings.color={2,1,.5f};
    rect_settings.intensity=50;rect_settings.width=2;rect_settings.height=3;rect_settings.two_sided=true;rect_settings.casts_shadow=false;
    p::LightSettings sun_settings;sun_settings.kind=p::LightKind::directional;sun_settings.intensity=1000;sun_settings.range=0;sun_settings.source_radius=.01f;
    take(p::set_light(world,point_id,point_settings));take(p::set_light(world,rectangle_id,rect_settings));take(p::set_light(world,directional_id,sun_settings));
    require(take(p::read_light(runtime,rectangle_id))==rect_settings && take(scene.light_items()).empty(),"Detached lights entered presentation.");
    const float half=std::sqrt(.5f);
    const math::Quaternion parent_rotation{0,half,0,half};
    const math::Quaternion child_rotation{0,0,std::sin(.3f),std::cos(.3f)};
    const math::Quaternion rectangle_rotation{std::sin(.2f),0,0,std::cos(.2f)};
    take(world.set_transform(entity.id(),{{10,20,30},parent_rotation,{2,3,4}}));
    take(world.set_transform(point_entity.id(),{{1,0,0},child_rotation,{5,6,7}}));
    take(world.set_transform(rectangle_entity.id(),{{0,1,0},rectangle_rotation,{8,9,10}}));
    take(world.set_transform(directional_entity.id(),{}));
    const std::array rect_group{w::Attachment{rectangle_id}},point_group{w::Attachment{point_id}},sun_group{w::Attachment{directional_id}};
    committed(world.attach_components(rectangle_entity.id(),rect_group));
    committed(world.attach_components(point_entity.id(),point_group));
    committed(world.attach_components(directional_entity.id(),sun_group));
    committed(world.reparent(rectangle_entity.id(),entity.id(),w::ReparentRule::keep_local));
    committed(world.reparent(point_entity.id(),entity.id(),w::ReparentRule::keep_local));
    committed(world.reparent(directional_entity.id(),entity.id(),w::ReparentRule::keep_local));
    // Spatial reference is independent of composition order: the rectangle
    // precedes its point origin in the authoritative lifecycle tree.
    committed(world.set_space_reference(rectangle_entity.id(),point_entity.id(),w::ReparentRule::keep_local));
    committed(world.attach(entity.id()));
    const auto roots=runtime.root_count();const auto snapshot=take(scene.light_items());const auto meshes=take(scene.render_items());
    require(runtime.root_count()==roots && snapshot.size()==3 && meshes.size()==1 && meshes[0].material==material,
        "Lighting snapshots retained business roots or mixed mesh/light projections.");
    require(snapshot[0].component==rectangle_id && snapshot[1].component==point_id && snapshot[2].component==directional_id &&
        snapshot[0].entity==rectangle_entity.id() && snapshot[0].settings==rect_settings,"Light projection lost identity, order or authored data.");
    const auto matrix=take(w::world_matrix(runtime,rectangle_entity.id()));
    require(snapshot[0].position==math::Vector3{matrix[12],matrix[13],matrix[14]} && near(snapshot[1].position.z,28),"Light position ignored the actual TRS hierarchy.");
    const auto rotation=*math::try_normalize(parent_rotation*child_rotation*rectangle_rotation);
    const auto forward=math::rotate(rotation,math::Vector3{0,0,1});
    const auto& projected=snapshot[0];
    require(near(math::dot(projected.direction,forward),1) && near(math::dot(projected.right,projected.right),1) &&
        near(math::dot(projected.up,projected.up),1) && near(math::dot(projected.right,projected.up),0) &&
        near(math::dot(math::cross(projected.right,projected.up),projected.direction),1) && projected.settings.width==2 && projected.settings.height==3,
        "Parent scale/shear distorted light orientation or emitter dimensions.");
    require(presented(world,scene)==4 && presented(world,scene,point_id)==1 &&
        take(world.query(types.light_component)).size()==3 && take(world.query(types.light_component,false)).size()==2,
        "Light registration bypassed World presentation or inherited type queries.");
    w::World other(runtime,types);reject(p::set_light(other,point_id,{}),"Another World edited a registered light.");
    const auto light_field=take(runtime.find_field(types.light_component,"settings"));
    const auto material_field=take(runtime.find_field(types.static_mesh_component,"material"));
    const auto light_path=[&](std::string_view name){return w::FieldPath{light_field,take(runtime.find_field(types.light_settings,name))};};
    const auto material_path=[&](std::string_view name){return w::FieldPath{material_field,take(runtime.find_field(types.material,name))};};
    // Hard bounds clamp scalar edits; type validators reject an edit that breaks a valid light and keep its value.
    take(world.write(point_id,light_path("intensity"),-1.f));
    require(take(p::read_light(runtime,point_id)).intensity==0,"Negative intensity was not clamped to its hard minimum.");
    take(world.write(point_id,light_path("intensity"),point_settings.intensity));
    reject(world.write(point_id,light_path("intensity"),std::numeric_limits<float>::quiet_NaN()),"NaN intensity was accepted.");
    reject(world.write(point_id,light_path("intensity"),1e38f),"Finite intensity beyond the analytic sampling range was accepted.");
    reject(world.write(point_id,light_path("range"),0.f),"Zero point range was accepted.");
    reject(world.write(point_id,light_path("kind"),EnumValue{types.light_kind,99}),"Unknown LightKind was accepted.");
    reject(world.write(point_id,light_path("kind"),std::uint32_t{1}),"Raw uint bypassed enum identity.");
    reject(world.write(point_id,light_path("casts_shadow"),1.f),"Numeric shadow flag was accepted.");
    reject(world.write(rectangle_id,light_path("width"),0.f),"Zero rectangle width was accepted.");
    reject(world.write(directional_id,light_path("source_radius"),2.f),"Directional angular radius beyond a hemisphere was accepted.");
    require(take(p::read_light(runtime,point_id))==point_settings && take(p::read_light(runtime,rectangle_id))==rect_settings &&
        take(p::read_light(runtime,directional_id))==sun_settings,"Rejected light edits changed authored settings.");
    // A light already broken outside World authoring stays editable, so its fault can be repaired.
    take(runtime.write(point_id,light_path("range"),0.f));
    take(world.write(point_id,light_path("intensity"),3.f));
    take(world.write(point_id,light_path("range"),point_settings.range));
    take(world.write(point_id,light_path("intensity"),point_settings.intensity));
    require(take(p::read_light(runtime,point_id))==point_settings,"An invalid light could not be repaired through World authoring.");
    take(world.write(root,material_path("roughness"),1.1f));
    take(world.write(root,material_path("metallic"),-.1f));
    auto emissive=material_path("emissive");emissive.push_back(take(runtime.find_field(types.vector3,"x")));
    take(world.write(root,emissive,-1.f));
    auto clamped=take(p::read_static_mesh(runtime,root)).material;
    require(clamped.roughness==1 && clamped.metallic==0 && clamped.emissive.x==0,"Material edits were not clamped to their hard bounds.");
    take(world.write(root,emissive,std::nextafter(atelier::material::emission_limit,std::numeric_limits<float>::infinity())));
    require(take(p::read_static_mesh(runtime,root)).material.emissive.x==atelier::material::emission_limit,
        "Emission above the HDR range was not clamped to the material capture limit.");
    reject(world.write(root,emissive,std::numeric_limits<float>::infinity()),"Infinite emission was accepted.");
    take(p::set_material(world,root,material));
    require(take(p::read_static_mesh(runtime,root)).material==material,"Restoring the Material changed the value.");
    take(world.write(root,emissive,32.f));take(world.write(root,material_path("roughness"),0.f));
    require(take(scene.render_items()).front().material.emissive.x==32 && take(scene.render_items()).front().material.roughness==0,"HDR emission or zero roughness was clamped away.");
    take(p::set_material(world,root,material));
    auto boundary_material=material;boundary_material.emissive={atelier::material::emission_limit,0,0};
    take(p::set_material(world,root,boundary_material));
    require(take(scene.render_items()).front().material==boundary_material,"Representable emission boundary did not survive World extraction.");
    take(p::set_material(world,root,material));
    auto boundary_light=sun_settings;
    boundary_light.intensity=static_cast<float>(atelier::rendering::analytic_light_radiance_limit);
    take(p::set_light(world,directional_id,boundary_light));
    require(take(p::read_light(runtime,directional_id))==boundary_light,"Analytic range incorrectly used the smaller HDR storage limit.");
    boundary_light.intensity=std::nextafter(boundary_light.intensity,std::numeric_limits<float>::infinity());
    reject(p::set_light(world,directional_id,boundary_light),"Analytic radiance above the boundary was accepted.");
    take(p::set_light(world,directional_id,sun_settings));
    boundary_light=point_settings;boundary_light.intensity=1e20f;boundary_light.color={1e-20f,1e-20f,1e-20f};
    take(p::set_light(world,point_id,boundary_light)); // Validate the product, not an arbitrary intensity cap.
    take(p::set_light(world,point_id,point_settings));
    boundary_light=rect_settings;boundary_light.color={1,1,1};boundary_light.width=boundary_light.height=2;
    boundary_light.intensity=static_cast<float>(atelier::rendering::analytic_light_radiance_limit/4);
    take(p::set_light(world,rectangle_id,boundary_light));
    boundary_light.width=std::nextafter(2.f,std::numeric_limits<float>::infinity());
    reject(p::set_light(world,rectangle_id,boundary_light),"Rectangle area-weighted radiance exceeded the shared sampling range.");
    take(p::set_light(world,rectangle_id,rect_settings));
    auto bad_material=material;bad_material.emissive.y=std::numeric_limits<float>::infinity();
    reject(p::set_material(world,root,bad_material),"Nonfinite emissive was accepted.");
    reject(p::create_static_mesh_entity(world,{geometry::cube_mesh(),{},bad_material}),"Factory bypassed expanded Material validation.");
    auto bad_light=point_settings;bad_light.color.z=std::numeric_limits<float>::quiet_NaN();
    reject(p::set_light(world,point_id,bad_light),"Nonfinite light color was accepted.");
    // The typed entries reject a value outside its hard bounds instead of clamping it as a field write does.
    auto rough=material;rough.roughness=1.5f;
    reject(p::set_material(world,root,rough),"set_material clamped an out-of-bounds roughness instead of rejecting it.");
    auto dim=point_settings;dim.intensity=-1.f;
    reject(p::set_light(world,point_id,dim),"set_light clamped a negative intensity instead of rejecting it.");
    require(take(p::read_static_mesh(runtime,root)).material==material && take(p::read_light(runtime,point_id))==point_settings,
        "A rejected typed edit changed its field.");
    // A light already breaking its rules, as a native write can leave it, still has the new value's rules checked.
    take(runtime.with_native<p::LightComponent>(point_id,[](auto& light)->Result<void>{light.settings.range=0;return {};}));
    auto flat=rect_settings;flat.width=0;
    reject(p::set_light(world,point_id,flat),"set_light skipped the light rules of a new value for an invalid light.");
    require(take(p::read_light(runtime,point_id)).kind==p::LightKind::point && take(p::read_light(runtime,point_id)).range==0,
        "A rejected typed edit of an invalid light changed it.");
    take(p::set_light(world,point_id,point_settings));
    take(world.write(point_id,light_path("intensity"),0.f));
    require(take(scene.light_items()).size()==3 && take(scene.light_items())[1].settings.intensity==0,"Zero intensity dropped stable light identity.");
    take(p::set_light(world,point_id,point_settings));
    committed(world.set_enabled(point_id,false));require(take(scene.light_items()).size()==2,"Disabled Component still illuminated the World.");
    committed(world.set_enabled(entity.id(),false));require(take(scene.light_items()).empty(),"Disabled Entity still illuminated the World.");
    committed(world.set_enabled(entity.id(),true));committed(world.set_enabled(point_id,true));
    auto removal=take(world.remove_component(rectangle_id));require(take(scene.light_items()).size()==2,"Removed light stayed registered.");
    committed(world.restore(removal));removal.root.reset();require(take(scene.light_items())[0].component==rectangle_id,"Undo changed light identity/order.");
    committed(world.start());committed(world.start());
    require(begins==std::vector<ObjectId>{rectangle_id,point_id,directional_id},"Light inheritance bypassed ordered Begin.");
    take(p::set_light(world,point_id,point_settings));
    take(world.update(2));
    require(take(scene.light_items())[0].settings.intensity==52 && take(scene.light_items())[1].settings.intensity==22,
        "Source behavior could not access reflected LightSettings on a derived light.");
    committed(world.stop());committed(world.stop());
    require(ends==std::vector<ObjectId>{directional_id,point_id,rectangle_id},"Light End order/pairing is wrong.");
    take(runtime.with_native<p::LightComponent>(point_id,[](auto& light)->Result<void>{light.settings.intensity=-1;return {};}));
    const auto isolated_lights=take(scene.light_items());const auto light_report=world.take_diagnostics();
    require(isolated_lights.size()==2 && std::ranges::none_of(isolated_lights,[&](const auto& light){return light.component==point_id;}) &&
        light_report.size()==1 && light_report[0].object==point_id && light_report[0].entity==point_entity.id() &&
        light_report[0].kind==w::DiagnosticKind::invalid_value && light_report[0].phase==w::DiagnosticPhase::lighting &&
        light_report[0].field==std::vector<std::string>{"settings","intensity"} && light_report[0].value=="-1" && light_report[0].type=="world.LightComponent",
        "Isolated light extraction did not skip and report only the invalid light.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    reject(scene.light_items(),"Unsafe native light mutation escaped extraction validation.");take(p::set_light(world,point_id,point_settings));
    take(runtime.with_native<p::LightComponent>(point_id,[](auto& light)->Result<void>{light.settings.intensity=1e38f;return {};}));
    reject(scene.light_items(),"Finite overflowing native light mutation escaped extraction validation.");take(p::set_light(world,point_id,point_settings));
    take(runtime.with_native<p::StaticMeshComponent>(root,[](auto& mesh)->Result<void>{mesh.material.metallic=2;return {};}));
    reject(scene.render_items(),"Unsafe native Material mutation escaped extraction validation.");take(p::set_material(world,root,material));
    // The negative intensity persisted from the isolated pass; the analytic overflow (a
    // LightSettings validator failure) and the Material bound are new causes.
    const auto halted=world.take_diagnostics();
    require(halted.size()==2 && halted[0].object==point_id && halted[0].field==std::vector<std::string>{"settings"} && halted[0].value.empty() &&
        halted[1].object==root && halted[1].field==std::vector<std::string>{"material","metallic"} && halted[1].value=="2",
        "Halted extraction failures were not reported once per persisting cause.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    // Extraction skips validating bytes that already passed at the same bucket position. A
    // repaired mesh moving onto a position that holds those bytes is drawn and its failure
    // resolves, exactly as when validated.
    auto twin=take(p::create_static_mesh_entity(world,{geometry::cube_mesh(),{},material}));
    const auto twin_mesh=take(w::components(runtime,twin.id())).front().id();
    committed(world.attach(twin.id()));
    const auto set_metallic=[&](float value) {
        take(runtime.with_native<p::StaticMeshComponent>(twin_mesh,[value](auto& mesh)->Result<void>{mesh.material.metallic=value;return {};}));
    };
    require(take(scene.render_items()).size()==2,"A second valid mesh was not drawn.");
    // Each mesh resolves the slot materials bound to it within its single extraction borrow.
    {
        material::State warm_state;warm_state.description.parameters.roughness=.2f;
        material::State cool_state=warm_state;cool_state.description.parameters.roughness=.8f;
        const auto warm=take(material::create_material(runtime,warm_state)),cool=take(material::create_material(runtime,cool_state));
        const std::array<p::MaterialBinding,1> warm_binding{{{"slot/0",warm,true}}},cool_binding{{{"slot/0",cool,true}}};
        take(p::set_material_bindings(runtime,root,warm_binding));take(p::set_material_bindings(runtime,twin_mesh,cool_binding));
    }
    const auto slotted=take(scene.render_items());
    const auto roughness=[](const p::RenderItem& item){return item.surfaces->front()->description().parameters.roughness;};
    require(slotted.size()==2 && roughness(slotted[0])==.2f && roughness(slotted[1])==.8f,
        "Meshes bound to different slot materials did not keep their own surfaces.");
    set_metallic(2);
    const auto excluded=take(scene.render_items());const auto persisting=take(scene.render_items());const auto twin_report=world.take_diagnostics();
    require(excluded.size()==1 && excluded[0].component==root && persisting.size()==1 && twin_report.size()==1 &&
        twin_report[0].object==twin_mesh && twin_report[0].field==std::vector<std::string>{"material","metallic"} && twin_report[0].repeats==2,
        "A mesh broken after a valid pass was not isolated and reported on every extraction.");
    set_metallic(material.metallic);
    auto parked=take(world.detach(entity.id()));
    const auto recovered=take(scene.render_items());
    require(recovered.size()==1 && recovered[0].component==twin_mesh && recovered[0].material==material &&
        world.take_diagnostics().empty(),"A repaired mesh moved onto a validated position was not drawn.");
    set_metallic(2);
    require(take(scene.render_items()).empty() && world.take_diagnostics().size()==1,"The repaired pass did not resolve the earlier failure.");
    set_metallic(material.metallic);
    committed(world.restore(parked));parked.root.reset();
    auto twin_removal=take(world.detach(twin.id()));twin_removal.root.reset();twin.reset();
    require(take(scene.render_items()).size()==1 && take(scene.light_items()).size()==3,"Restoring the parked Entity changed extraction.");
    auto detached=take(world.detach(entity.id()));
    require(take(scene.light_items()).empty() && take(world.query(types.light_component)).empty(),"Detached history graph remained a light source.");
    committed(world.restore(detached));detached.root.reset();
    auto light_entity=take(w::create_entity(runtime));auto light_root=take(w::create_component(runtime,types.light_component));
    const std::array only_light{w::Attachment{light_root.id()}};
    take(world.set_transform(light_entity.id(),{}));committed(world.attach_components(light_entity.id(),only_light));committed(world.attach(light_entity.id()));
    require(take(scene.render_items()).size()==1 && presented(world,scene,light_entity.id())==1 && take(scene.light_items()).size()==4,
        "A light-only Entity required a mesh or another root type.");
    // Every light is spatial: its owner keeps the Transform, and a light cannot join an Entity without one.
    reject(world.remove_transform(light_entity.id()),"A light's owner lost its Transform.");
    auto bare=take(w::create_entity(runtime));auto stray=take(w::create_component(runtime,types.light_component));
    const std::array stray_group{w::Attachment{stray.id()}};
    committed(world.attach(bare.id()));
    reject(world.attach_components(bare.id(),stray_group),"A light joined an Entity without a Transform.");
    reject(world.write(light_root.id(),light_path("kind"),EnumValue{types.light_kind,3}),"The removed Sky light kind was accepted.");
    require(take(w::has_transform(runtime,light_entity.id())) && take(scene.light_items()).size()==4,
        "Rejected spatial Light edits damaged presentation.");
    committed(world.close());committed(world.close());reject(scene.light_items(),"Closed World exposed a stale light directory.");
    const auto saved_identity=snapshot.front().component;
    entity.reset();point.reset();rectangle.reset();directional.reset();point_entity.reset();rectangle_entity.reset();directional_entity.reset();light_entity.reset();light_root.reset();bare.reset();stray.reset();collect(runtime);
    require(runtime.live_objects()==0 && !runtime.type_of(saved_identity) && snapshot.front().settings==rect_settings,
        "Light snapshots retained business objects or depended on their later state.");
    std::puts("[pass] Lit data: reflected Material/LightSettings, HDR validation, Source inheritance, TRS extraction, position-cached validation, slot surfaces, identity/history and lifecycle");
}
void environment_and_group_writes() {
    Runtime runtime; const auto types=register_all_types(runtime);
    w::World world(runtime,types);
    const auto scene=take(p::Scene::create(world,types));
    using Settings=atelier::rendering::SceneLightingSettings;
    require(take(runtime.find_type("world.EnvironmentComponent"))==types.environment_component &&
        take(runtime.is_a(types.environment_component,types.component)) && take(scene.environment())==Settings{},
        "EnvironmentComponent is not a registered Component, or a World without one did not use the defaults.");
    const auto field=[&](TypeId type,std::string_view name){return take(runtime.find_field(type,name));};
    const auto volume_type=take(runtime.native_type<p::ProbeVolume>());
    const auto path=[&](std::string_view name){return w::FieldPath{field(types.environment_component,name)};};
    const auto axis=[&](std::string_view volume,std::string_view bound,std::string_view component) {
        return w::FieldPath{field(types.environment_component,volume),field(volume_type,bound),field(types.vector3,component)};
    };
    // A nonspatial owner: the environment needs no Transform.
    auto owner=take(w::create_entity(runtime));auto environment=take(w::create_component(runtime,types.environment_component));
    const std::array environment_group{w::Attachment{environment.id()}};
    committed(world.attach_components(owner.id(),environment_group));committed(world.attach(owner.id()));
    require(take(scene.environment())==Settings{},"A default Environment did not reproduce the default World settings.");
    // At most one per World, a disabled one included.
    committed(world.set_enabled(environment.id(),false));
    require(take(scene.environment())==Settings{},"A disabled Environment changed the World settings.");
    auto second_owner=take(w::create_entity(runtime));auto second=take(w::create_component(runtime,types.environment_component));
    const std::array second_group{w::Attachment{second.id()}};
    committed(world.attach_components(second_owner.id(),second_group));
    reject(world.attach(second_owner.id()),"A second Environment entered a World holding a disabled one.");
    auto extra=take(w::create_component(runtime,types.environment_component));const std::array extra_group{w::Attachment{extra.id()}};
    reject(world.attach_components(owner.id(),extra_group),"A second Environment joined the first one's Entity.");
    committed(world.set_enabled(environment.id(),true));

    // Moving a volume past its old bounds: written one leaf at a time, min first,
    // the first leaf already fails min < max; written as one group it succeeds.
    const std::array names{"x","y","z"};
    std::vector<w::FieldPath> paths;std::vector<Scalar> values;
    for(std::size_t i=0;i<3;++i) {paths.push_back(axis("volume0","minimum",names[i]));values.push_back(20.f+float(i));}
    for(std::size_t i=0;i<3;++i) {paths.push_back(axis("volume0","maximum",names[i]));values.push_back(32.f+float(i));}
    reject(world.write(environment.id(),paths[0],values[0]),"A leaf write accepted a minimum above the maximum.");
    take(world.write(environment.id(),paths,values));
    auto moved=Settings{};moved.probe_volumes[0].minimum={20,21,22};moved.probe_volumes[0].maximum={32,33,34};
    require(take(scene.environment())==moved,"A group write did not move the volume as one edit.");
    const std::array sky_paths{path("sky_enabled"),path("sky_intensity")};
    const std::array<Scalar,2> too_bright{true,65504.5f},sky_values{true,2.f};
    reject(world.write(environment.id(),sky_paths,too_bright),"Sky radiance above the HDR scene range was accepted.");
    take(world.write(environment.id(),sky_paths,sky_values));
    moved.sky_enabled=true;moved.sky_intensity=2;
    require(take(scene.environment())==moved && atelier::rendering::sky_radiance(moved)==std::array{2.f,2.f,2.f},
        "The enabled sky did not reach the World settings.");
    const std::array duplicate_paths{path("exposure_ev"),path("exposure_ev")};const std::array<Scalar,2> duplicate_values{1.f,2.f};
    reject(world.write(environment.id(),duplicate_paths,duplicate_values),"A group write accepted the same field twice.");
    auto spatial=take(w::create_entity(runtime));take(world.set_transform(spatial.id(),{}));committed(world.attach(spatial.id()));
    const std::array transform_paths{w::FieldPath{types.position_fields[0].begin(),types.position_fields[0].end()}};
    const std::array<Scalar,1> transform_values{1.f};
    reject(world.write(spatial.id(),transform_paths,transform_values),"A group write bypassed Transform validation.");
    require(take(scene.environment())==moved && take(w::read_transform(runtime,spatial.id())).position.x==0,
        "Rejected group writes changed a field.");

    // An admission rejection restores every leaf to its exact bytes, including a
    // NaN payload and -0 that no write can produce; the rejection is the admission's.
    take(world.add_admission(types.environment_component,[&](const w::World& owner_world,std::span<const ObjectId> candidates)->Result<void> {
        for(const auto object:candidates)
            if(std::get<float>(take(owner_world.runtime().read(object,path("sky_intensity"))))>5)
                return std::unexpected(Error{ErrorCode::invalid_member,"Test admission rejects bright skies."});
        return {};
    }));
    const auto payload=std::bit_cast<float>(0x7fc00123u);
    take(runtime.with_native<p::EnvironmentComponent>(environment.id(),[&](auto& value)->Result<void> {
        value.sky_color.x=payload;value.exposure_ev=-0.f;return {};
    }));
    const std::array admitted_paths{w::FieldPath{field(types.environment_component,"sky_color"),field(types.vector3,"x")},
        path("exposure_ev"),path("sky_intensity")};
    const std::array<Scalar,3> admitted_values{1.f,2.f,10.f};
    const auto rejected=world.write(environment.id(),admitted_paths,admitted_values);
    require(!rejected && rejected.error().message=="Test admission rejects bright skies.",
        "A rejected group write did not return the admission's own error.");
    take(runtime.with_native<const p::EnvironmentComponent>(environment.id(),[&](const auto& value)->Result<void> {
        require(std::bit_cast<std::uint32_t>(value.sky_color.x)==0x7fc00123u && std::bit_cast<std::uint32_t>(value.exposure_ev)==0x80000000u &&
            value.sky_intensity==2.f,"A rejected group write did not restore every leaf's exact bytes.");
        return {};
    }));
    // The NaN came from outside the write paths: extraction reports it and uses the defaults.
    require(take(scene.environment())==Settings{},"An invalid Environment did not fall back to the default settings.");
    const auto report=world.take_diagnostics();
    require(report.size()==1 && report[0].object==environment.id() && report[0].entity==owner.id() &&
        report[0].kind==w::DiagnosticKind::invalid_value && report[0].phase==w::DiagnosticPhase::lighting &&
        report[0].field==std::vector<std::string>{"sky_color","x"} && report[0].type=="world.EnvironmentComponent",
        "An invalid Environment was not reported on its component.");
    take(world.set_failure_policy(w::FailurePolicy::halt));
    reject(scene.environment(),"Halted extraction accepted an invalid Environment.");
    take(world.set_failure_policy(w::FailurePolicy::isolate));
    take(world.write(environment.id(),admitted_paths[0],1.f));
    require(take(scene.environment()).sky_color==std::array{1.f,1.f,1.f},"Repairing the Environment did not restore its settings.");
    // The sky map is a hidden key and the map it resolved to, set only as a pair.
    require(!runtime.find_field(types.environment_component,"sky_map"),"The sky map key is addressable as a field.");
    const std::array<float,6> texels{1.f,1.f,1.f,4.f,4.f,4.f};
    const auto sky=std::make_shared<const atelier::rendering::SkyMap>(2u,1u,std::span<const float>(texels));
    reject(p::set_sky_map_binding(runtime,environment.id(),{"",sky}),"A sky map without its asset key was accepted.");
    reject(p::set_sky_map_binding(runtime,environment.id(),{std::string("sky\0key",7),{}}),"A sky map key holding NUL was accepted.");
    take(p::set_sky_map_binding(runtime,environment.id(),{"project/sky",sky}));
    take(world.write(environment.id(),path("sky_rotation"),200.f));
    const auto bound=take(p::read_sky_map(runtime,environment.id()));const auto lit=take(scene.environment());
    require(bound.source=="project/sky" && bound.resolved==sky && lit.sky_map==sky && lit.sky_rotation==180.f,
        "The sky map binding or the clamped sky rotation did not reach the World settings.");
    std::optional<Result<void>> nested_sky;
    take(world.add_admission(types.environment_component,[&](const w::World&,std::span<const ObjectId> candidates)->Result<void> {
        if(!nested_sky) nested_sky=p::set_sky_map_binding(runtime,candidates.front(),{});
        return {};
    }));
    take(world.write(environment.id(),path("sky_rotation"),-90.f));
    require(nested_sky && !*nested_sky && nested_sky->error().code==ErrorCode::busy && take(p::read_sky_map(runtime,environment.id())).resolved==sky,
        "A sky map binding changed while the World ran an admission.");
    take(p::set_sky_map_binding(runtime,environment.id(),{}));
    require(take(p::read_sky_map(runtime,environment.id())).source.empty() && !take(scene.environment()).sky_map &&
        take(scene.environment()).sky_rotation==-90.f,"Clearing the sky map left its key or map, or changed the rotation.");
    committed(world.close());
    // Content attached before a Scene exists is checked when it is created, disabled and derived Environments included.
    {
        const auto derived=take(NativeRegistration<DerivedEnvironment>{runtime,"lighting.DerivedEnvironment",TypeKind::object}
            .base<p::EnvironmentComponent>().publish());
        w::World loaded(runtime,types);
        const auto add=[&](TypeId type) {
            auto holder=take(w::create_entity(runtime));auto component=take(w::create_component(runtime,type));
            const std::array group{w::Attachment{component.id()}};
            committed(loaded.attach_components(holder.id(),group));committed(loaded.attach(holder.id()));
            return std::pair{std::move(holder),component.id()};
        };
        const auto disabled=add(types.environment_component);
        committed(loaded.set_enabled(disabled.second,false));
        const auto active=add(derived);
        reject(p::Scene::create(loaded,types),"A Scene was created over a World that already held two Environments.");
        take(loaded.detach(active.first.id()));
        const auto accepted=p::Scene::create(loaded,types);
        require(accepted && take(accepted->environment())==Settings{},
            "A Scene over one disabled Environment was rejected or did not use the default settings.");
        committed(loaded.close());
    }
    std::puts("[pass] Environment: defaults without one, uniqueness with disabled ones also for content before the Scene, invalid values fall back, atomic group writes with exact restore, the sky map set only as a key-and-map pair");
}
// Scene::update keeps one cached extraction: an unchanged update keeps the version;
// every presented input advances it, values written past World included, and only
// the slots that changed take the new serial. Failures are reported on every update.
void presentation_cache() {
    Runtime runtime; const auto types=register_all_types(runtime);
    w::World world(runtime,types);
    const auto scene=take(p::Scene::create(world,types));
    auto parent=take(w::create_entity(runtime));take(world.set_transform(parent.id(),{}));
    auto mesh_entity=take(p::create_static_mesh_entity(world,{geometry::cube_mesh(),{{1,0,0},{},{1,1,1}}}));
    const auto mesh=take(w::components(runtime,mesh_entity.id())).front().id();
    auto still_entity=take(p::create_static_mesh_entity(world,{geometry::cube_mesh(),{{5,0,0},{},{1,1,1}}}));
    const auto still=take(w::components(runtime,still_entity.id())).front().id();
    auto light_entity=take(w::create_entity(runtime));take(world.set_transform(light_entity.id(),{{0,2,0},{},{1,1,1}}));
    auto light=take(w::create_component(runtime,types.light_component));const auto light_id=light.id();
    const std::array light_group{w::Attachment{light_id}};
    committed(world.attach_components(light_entity.id(),light_group));
    auto environment_entity=take(w::create_entity(runtime));
    auto environment=take(w::create_component(runtime,types.environment_component));
    const std::array environment_group{w::Attachment{environment.id()}};
    committed(world.attach_components(environment_entity.id(),environment_group));
    committed(world.reparent(mesh_entity.id(),parent.id(),w::ReparentRule::keep_local));
    committed(world.reparent(light_entity.id(),parent.id(),w::ReparentRule::keep_local));
    committed(world.attach(parent.id()));committed(world.attach(still_entity.id()));committed(world.attach(environment_entity.id()));
    const auto field=[&](TypeId type,std::string_view name){return take(runtime.find_field(type,name));};
    const w::FieldPath roughness{field(types.static_mesh_component,"material"),field(types.material,"roughness")};
    const w::FieldPath intensity{field(types.light_component,"settings"),field(types.light_settings,"intensity")};
    const w::FieldPath range{field(types.light_component,"settings"),field(types.light_settings,"range")};
    const w::FieldPath exposure{field(types.environment_component,"exposure_ev")};
    const auto& x=types.position_fields[0];
    const auto slot=[&](ObjectId component)->const p::MeshSlot& {
        const auto members=take(world.members(scene.meshes()));
        return scene.mesh_slots()[std::size_t(std::ranges::find(members,component,&w::CapabilityMember::component)-members.begin())];
    };
    auto version=take(scene.update());
    const auto still_cursor=take(runtime.change_cursor());
    for(int frame=0;frame<3;++frame) require(take(scene.update())==version,"An unchanged presentation advanced its version.");
    std::vector<ObjectId> written;runtime.changed_objects(still_cursor,written);
    require(written.empty(),"Updating an unchanged presentation wrote an object.");
    const auto advanced=[&](const char* message) {
        const auto next=take(scene.update());require(next!=version,message);
        require(take(scene.update())==next,"Updating a changed presentation again advanced its version.");
        version=next;
    };
    const auto untouched=slot(still).changed;
    take(world.write(mesh,roughness,.25f));advanced("A Material edit through World did not advance the version.");
    require(slot(mesh).changed==version && slot(mesh).item->material.roughness==.25f && slot(still).changed==untouched,
        "A Material edit did not rebuild exactly its own slot.");
    take(runtime.write(mesh,roughness,Scalar{.3f}));advanced("A Material write past World did not advance the version.");
    take(runtime.write(light_id,intensity,Scalar{7.f}));advanced("A light edit past World did not advance the version.");
    require(scene.lights_changed()==version && scene.light_snapshot().front().settings.intensity==7 && slot(still).changed==untouched,
        "A light edit did not refresh the lights alone.");
    take(world.write(environment.id(),exposure,1.f));advanced("An Environment edit did not advance the version.");
    take(runtime.write(parent.id(),x,Scalar{3.f}));advanced("An ancestor TRS write did not advance the version.");
    require(slot(mesh).item->world[12]==4 && slot(still).changed==untouched,"An ancestor move did not rebuild exactly the slots below it.");
    committed(world.set_enabled(light_id,false));advanced("Disabling a light did not advance the version.");
    committed(world.set_enabled(light_id,true));advanced("Enabling a light did not advance the version.");
    auto removal=take(world.remove_component(light_id));advanced("Removing a light did not advance the version.");
    committed(world.restore(removal));advanced("Restoring a light did not advance the version.");
    committed(world.reparent(light_entity.id(),{},w::ReparentRule::keep_local));advanced("Reparenting a light did not advance the version.");
    require(scene.mesh_layout()==version,"A structural edit did not rebuild the mesh layout.");
    material::State shared;shared.description.parameters.roughness=.2f;
    const auto resource=take(material::create_material(runtime,shared));
    const std::array<p::MaterialBinding,1> binding{{{"slot/0",resource,true}}};
    take(p::set_material_bindings(runtime,mesh,binding));advanced("Binding a slot material did not advance the version.");
    shared.description.parameters.roughness=.6f;take(material::update_material(runtime,resource.id(),shared));
    advanced("Editing a bound material resource did not advance the version.");
    require(slot(mesh).item->surfaces->front()->description().parameters.roughness==.6f,"The bound material edit did not reach the cache.");
    require(world.take_diagnostics().empty(),"Valid presentation edits reported diagnostics.");
    // An isolated item leaves the cache once; while it stays isolated the version
    // holds and every update reports it again.
    take(runtime.write(light_id,range,Scalar{0.f}));advanced("Isolating a light did not advance the version.");
    auto reports=world.take_diagnostics();
    require(reports.size()==1 && reports[0].repeats==2 && scene.light_snapshot().empty(),
        "An isolated light stayed presented or was not reported on each update.");
    take(runtime.write(light_id,range,Scalar{10.f}));advanced("Repairing an isolated light did not advance the version.");
    const auto placed=slot(still).changed; // The reparent above rebuilt the layout.
    take(runtime.write(parent.id(),x,Scalar{std::numeric_limits<float>::infinity()}));advanced("Isolating a space did not advance the version.");
    reports=world.take_diagnostics();
    require(reports.size()==1 && reports[0].repeats==2 && !slot(mesh).item && slot(still).item && slot(still).changed==placed,
        "A broken space did not isolate exactly its mesh, or was not reported on each update.");
    take(runtime.write(parent.id(),x,Scalar{3.f}));advanced("Repairing an isolated space did not advance the version.");
    require(scene.light_snapshot().size()==1 && slot(mesh).item && take(scene.render_items()).size()==2 && world.take_diagnostics().empty(),
        "Repaired items did not return to the cache.");
    // The lights are not extracted again here; the invalid Environment still counts every update.
    const w::FieldPath sky{field(types.environment_component,"sky_intensity")};
    take(runtime.write(environment.id(),sky,Scalar{1e6f}));
    (void)take(scene.update());(void)take(scene.update());
    reports=world.take_diagnostics();
    require(reports.size()==1 && reports[0].repeats==2,"A persistent invalid Environment was not reported on each update.");
    committed(world.close());
    std::puts("[pass] Presentation cache: unchanged updates keep the version; World, Runtime, Environment, TRS, enablement, composition, reparent and material resource edits advance it and rebuild only their slots; isolated items and an invalid Environment are reported on every update");
}

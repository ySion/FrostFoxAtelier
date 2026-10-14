struct DerivedLight : w::LightComponent { float extra{3.f}; };
void lit_materials_and_lights() {
    Runtime runtime; const auto types=take(w::register_types(runtime));
    const auto derived=take(NativeRegistration<DerivedLight>{runtime,"lighting.DerivedLight",TypeKind::object}
        .base<w::LightComponent>().field<&DerivedLight::extra>("extra").publish());
    require(take(runtime.native_type<w::LightKind>())==types.light_kind &&
        take(runtime.native_type<w::LightSettings>())==types.light_settings &&
        take(runtime.find_type("world.LightComponent"))==types.light_component && take(runtime.is_a(derived,types.component)),
        "Light types did not enter the shared reflected hierarchy.");
    source::Program program(runtime,types.light_component);
    publish(program,"void tick(float dt) { self.settings.intensity = self.settings.intensity + dt; }");
    const auto lease=take(program.pin());
    w::World world(runtime,types);
    std::vector<ObjectId> begins,ends;
    take(world.bind_behavior(types.light_component,{
        [&](Runtime&,ObjectId object)->Result<void>{begins.push_back(object);return {};},
        [lease](Runtime&,ObjectId object,float dt)->Result<void>{
            const std::array<Scalar,1> arguments{dt};auto invoked=lease.invoke(object,arguments);
            if (!invoked) return std::unexpected(invoked.error()); return {};
        },
        [&](Runtime&,ObjectId object)->Result<void>{ends.push_back(object);return {};}
    }));
    require(w::Material{}.roughness==.65f && w::Material{}.metallic==0 && w::Material{}.emissive==math::Vector3{},"Material defaults changed unexpectedly.");
    const w::Material material{{.2f,.3f,.4f},.25f,.75f,{4,8,16}};
    auto entity=take(w::create_static_mesh_entity(world,{geometry::cube_mesh(),{},material}));
    const auto root=take(w::components(runtime,entity.id())).front().id();
    auto point_entity=take(w::create_entity(runtime));
    auto rectangle_entity=take(w::create_entity(runtime));
    auto directional_entity=take(w::create_entity(runtime));
    auto point=take(w::create_component(runtime,types.light_component));
    auto rectangle=take(w::create_component(runtime,derived));
    auto directional=take(w::create_component(runtime,types.light_component));
    const auto point_id=point.id(), rectangle_id=rectangle.id(), directional_id=directional.id();
    w::LightSettings point_settings;point_settings.intensity=20;point_settings.range=25;point_settings.source_radius=.2f;
    w::LightSettings rect_settings;rect_settings.kind=w::LightKind::rectangle;rect_settings.color={2,1,.5f};
    rect_settings.intensity=50;rect_settings.width=2;rect_settings.height=3;rect_settings.two_sided=true;rect_settings.casts_shadow=false;
    w::LightSettings sun_settings;sun_settings.kind=w::LightKind::directional;sun_settings.intensity=1000;sun_settings.range=0;sun_settings.source_radius=.01f;
    take(world.set_light(point_id,point_settings));take(world.set_light(rectangle_id,rect_settings));take(world.set_light(directional_id,sun_settings));
    require(take(w::read_light(runtime,rectangle_id))==rect_settings && take(world.light_items()).empty(),"Detached lights entered presentation.");
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
    const auto roots=runtime.root_count();const auto snapshot=take(world.light_items());const auto meshes=take(world.render_items());
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
    require(world.activity().presentation_registrations==4 && take(w::inspect_component(runtime,point_id)).can_present &&
        take(world.query(types.light_component)).size()==3 && take(world.query(types.light_component,false)).size()==2,
        "Light registration bypassed World presentation or inherited type queries.");
    w::World other(runtime,types);reject(other.set_light(point_id,{}),"Another World edited a registered light.");
    const auto light_field=take(runtime.find_field(types.light_component,"settings"));
    const auto material_field=take(runtime.find_field(types.static_mesh_component,"material"));
    const auto light_path=[&](std::string_view name){return w::FieldPath{light_field,take(runtime.find_field(types.light_settings,name))};};
    const auto material_path=[&](std::string_view name){return w::FieldPath{material_field,take(runtime.find_field(types.material,name))};};
    reject(world.write(point_id,light_path("intensity"),-1.f),"Negative intensity was accepted.");
    reject(world.write(point_id,light_path("range"),0.f),"Zero point range was accepted.");
    reject(world.write(point_id,light_path("kind"),EnumValue{types.light_kind,99}),"Unknown LightKind was accepted.");
    reject(world.write(point_id,light_path("kind"),std::uint32_t{1}),"Raw uint bypassed enum identity.");
    reject(world.write(point_id,light_path("casts_shadow"),1.f),"Numeric shadow flag was accepted.");
    reject(world.write(rectangle_id,light_path("width"),0.f),"Zero rectangle width was accepted.");
    reject(world.write(directional_id,light_path("source_radius"),2.f),"Directional angular radius beyond a hemisphere was accepted.");
    require(take(w::read_light(runtime,point_id))==point_settings && take(w::read_light(runtime,rectangle_id))==rect_settings &&
        take(w::read_light(runtime,directional_id))==sun_settings,"Rejected light edits changed authored settings.");
    reject(world.write(root,material_path("roughness"),1.1f),"Roughness exceeded one.");
    reject(world.write(root,material_path("metallic"),-.1f),"Negative metallic was accepted.");
    auto emissive=material_path("emissive");emissive.push_back(take(runtime.find_field(types.vector3,"x")));
    reject(world.write(root,emissive,-1.f),"Negative emissive was accepted.");
    require(take(w::read_static_mesh(runtime,root)).material==material,"Rejected Material edits changed the value.");
    take(world.write(root,emissive,32.f));take(world.write(root,material_path("roughness"),0.f));
    require(take(world.render_items()).front().material.emissive.x==32 && take(world.render_items()).front().material.roughness==0,"HDR emission or zero roughness was clamped away.");
    take(world.set_material(root,material));
    auto bad_material=material;bad_material.emissive.y=std::numeric_limits<float>::infinity();
    reject(world.set_material(root,bad_material),"Nonfinite emissive was accepted.");
    reject(w::create_static_mesh_entity(world,{geometry::cube_mesh(),{},bad_material}),"Factory bypassed expanded Material validation.");
    auto bad_light=point_settings;bad_light.color.z=std::numeric_limits<float>::quiet_NaN();
    reject(world.set_light(point_id,bad_light),"Nonfinite light color was accepted.");
    take(world.write(point_id,light_path("intensity"),0.f));
    require(take(world.light_items()).size()==3 && take(world.light_items())[1].settings.intensity==0,"Zero intensity dropped stable light identity.");
    take(world.set_light(point_id,point_settings));
    committed(world.set_enabled(point_id,false));require(take(world.light_items()).size()==2,"Disabled Component still illuminated the World.");
    committed(world.set_enabled(entity.id(),false));require(take(world.light_items()).empty(),"Disabled Entity still illuminated the World.");
    committed(world.set_enabled(entity.id(),true));committed(world.set_enabled(point_id,true));
    auto removal=take(world.remove_component(rectangle_id));require(take(world.light_items()).size()==2,"Removed light stayed registered.");
    committed(world.restore(removal));removal.root.reset();require(take(world.light_items())[0].component==rectangle_id,"Undo changed light identity/order.");
    committed(world.start());committed(world.start());
    require(begins==std::vector<ObjectId>{rectangle_id,point_id,directional_id},"Light inheritance bypassed ordered Begin.");
    reject(world.set_light(point_id,point_settings),"Stopped-only light setter reentered a running World.");
    take(world.update(2));
    require(take(world.light_items())[0].settings.intensity==52 && take(world.light_items())[1].settings.intensity==22,
        "Source behavior could not access reflected LightSettings on a derived light.");
    committed(world.stop());committed(world.stop());
    require(ends==std::vector<ObjectId>{directional_id,point_id,rectangle_id},"Light End order/pairing is wrong.");
    take(runtime.with_native<w::LightComponent>(point_id,[](auto& light)->Result<void>{light.settings.intensity=-1;return {};}));
    reject(world.light_items(),"Unsafe native light mutation escaped extraction validation.");take(world.set_light(point_id,point_settings));
    take(runtime.with_native<w::StaticMeshComponent>(root,[](auto& mesh)->Result<void>{mesh.material.metallic=2;return {};}));
    reject(world.render_items(),"Unsafe native Material mutation escaped extraction validation.");take(world.set_material(root,material));
    auto detached=take(world.detach(entity.id()));
    require(take(world.light_items()).empty() && take(world.query(types.light_component)).empty(),"Detached history graph remained a light source.");
    committed(world.restore(detached));detached.root.reset();
    auto light_entity=take(w::create_entity(runtime));auto light_root=take(w::create_component(runtime,types.light_component));
    const std::array only_light{w::Attachment{light_root.id()}};
    take(world.set_transform(light_entity.id(),{}));committed(world.attach_components(light_entity.id(),only_light));committed(world.attach(light_entity.id()));
    require(take(world.render_items()).size()==1 && take(w::inspect_entity(runtime,light_entity.id())).can_present && take(world.light_items()).size()==4,
        "A light-only Entity required a mesh or another root type.");
    w::LightSettings sky_settings;sky_settings.kind=w::LightKind::sky;
    take(world.set_light(light_root.id(),sky_settings));take(world.remove_transform(light_entity.id()));
    reject(world.set_light(light_root.id(),{}),"Nonspatial Sky owner accepted a point light without Transform.");
    reject(world.write(light_root.id(),light_path("kind"),EnumValue{types.light_kind,static_cast<std::uint32_t>(w::LightKind::directional)}),
        "Light field authoring bypassed its owner's spatial requirement.");
    require(take(w::read_light(runtime,light_root.id()))==sky_settings && !take(w::has_transform(runtime,light_entity.id())) &&
        take(world.light_items()).size()==4,"Rejected spatial Light edits changed settings or damaged presentation.");
    take(world.set_transform(light_entity.id(),{}));
    take(world.write(light_root.id(),light_path("kind"),EnumValue{types.light_kind,static_cast<std::uint32_t>(w::LightKind::point)}));
    require(take(w::read_light(runtime,light_root.id())).kind==w::LightKind::point && take(world.light_items()).size()==4,
        "Adding Transform did not permit the same spatial Light edit.");
    committed(world.close());committed(world.close());reject(world.light_items(),"Closed World exposed a stale light directory.");
    const auto saved_identity=snapshot.front().component;
    entity.reset();point.reset();rectangle.reset();directional.reset();point_entity.reset();rectangle_entity.reset();directional_entity.reset();light_entity.reset();light_root.reset();collect(runtime);
    require(runtime.live_objects()==0 && !runtime.type_of(saved_identity) && snapshot.front().settings==rect_settings,
        "Light snapshots retained business objects or depended on their later state.");
    std::puts("[pass] Lit data: reflected Material/LightSettings, HDR validation, Source inheritance, TRS extraction, identity/history and lifecycle");
}

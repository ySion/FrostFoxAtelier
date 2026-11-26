#include "detail/detail.hpp"
#include "runtime/foundation.hpp"
#include <cmath>
#include <exception>

namespace atelier::presentation {
using detail::Access;

using detail::error;

namespace {

math::Vector2 safe_uv(math::Vector2 uv) noexcept {
    return math::is_finite(uv) ? uv : math::Vector2{0.f, 0.f};
}

template <class EntityT, class ComponentT, class Setup>
Result<ObjectRoot> create_spatial_entity(world::World& world, const world::Trs& trs, Setup&& setup) {
    if (world.closed())
        return error(ErrorCode::unsupported_operation, "World is closed.");
    ATELIER_TRY_BIND(transform, world::canonical_transform(trs));
    auto& runtime = world.runtime();
    struct Construction {
        Runtime& runtime;

        ObjectRoot entity;

        ObjectRoot component;

        ~Construction() {
            // The graph is unpublished. Either destruction entry also handles
            // ordinary strong-reference cycles; no partial factory result escapes.
            if (entity.id() != ObjectId{})
                (void)world::destroy_entity(runtime, entity.id());
            if (component.id() != ObjectId{} && runtime.type_of(component.id()))
                (void)world::destroy_component(runtime, component.id());
        }
    } created{runtime, {}, {}};
    ATELIER_TRY_BIND(entity_type, runtime.native_type<EntityT>());
    ATELIER_TRY_BIND(component_type, runtime.native_type<ComponentT>());
    ATELIER_TRY_BIND(entity, world::create_entity(runtime, *entity_type));
    created.entity = std::move(*entity);
    ATELIER_TRY_BIND(component, world::create_component(runtime, *component_type));
    created.component = std::move(*component);
    ATELIER_TRY(world.set_transform(created.entity.id(), *transform));
    ATELIER_TRY(setup(runtime, created.component.id()));
    const std::array composition{world::Attachment{created.component.id()}};
    ATELIER_TRY_BIND(composed, world.attach_components(created.entity.id(), composition));
    if (composed->notification_error)
        return std::unexpected(std::move(*composed->notification_error));
    created.component.reset(); // Entity composition now retains its capability.
    return std::move(created.entity);
}

Result<std::pair<std::vector<ProceduralAssetSlotRange>, std::vector<ObjectReference>>>
validate_procedural_assets(Runtime& runtime, std::span<const ProceduralInstanceAsset> resolved_assets) {
    std::vector<ProceduralAssetSlotRange> ranges;
    std::vector<ObjectReference> inst_defaults;
    ranges.reserve(resolved_assets.size());
    for (const auto& asset : resolved_assets) {
        Set<std::string_view> unique;
        unique.reserve(asset.defaults.size());
        ProceduralAssetSlotRange range;
        range.key = asset.key;
        range.mesh = asset.mesh;
        range.default_offset = inst_defaults.size();
        for (const auto& item : asset.defaults) {
            std::size_t matches{};
            if (asset.mesh)
                for (std::size_t j = 0; j < asset.mesh->materials().size(); ++j)
                    matches += material_slot_name(*asset.mesh, j) == item.slot;
            if (matches != 1 || !unique.add(item.slot))
                return error(ErrorCode::argument_mismatch,
                             "Instance default material slot is missing, ambiguous or repeated: " + item.slot);
            ATELIER_TRY(material::capture_program(runtime, item.resource.id()));
            range.default_slots.push_back(item.slot);
            inst_defaults.push_back(item.resource.reference());
        }
        range.default_count = inst_defaults.size() - range.default_offset;
        ranges.push_back(std::move(range));
    }
    return std::pair{std::move(ranges), std::move(inst_defaults)};
}

Result<void> commit_procedural_binding(Runtime& runtime, ObjectId object, String source,
                                       std::shared_ptr<const ProceduralOutput> published,
                                       std::span<const ObjectReference> inst_defaults) {
    ATELIER_TRY_BIND(owner, world::admit_component_edit(runtime, object));
    bool capability_changed{};
    auto changed = runtime.with_native<ProceduralGeneratorComponent>(
        object, [&](ProceduralGeneratorComponent& component) -> Result<void> {
            ATELIER_TRY_BIND(inst_field, runtime.reference_field(object, Access::instance_materials(component)));
            ATELIER_TRY_BIND(inst_count, runtime.sequence_size(*inst_field));
            const ReferenceChange change = SequenceSplice{*inst_field, 0, *inst_count, inst_defaults};
            ATELIER_TRY(runtime.apply_reference_changes({&change, 1}));
            capability_changed =
                detail::has_renderable_output(Access::output(component)) != detail::has_renderable_output(published);
            const auto prev_surface =
                Access::output(component) ? Access::output(component)->surface_mesh : geometry::MeshRef{};
            const auto next_surface = published ? published->surface_mesh : geometry::MeshRef{};
            if (prev_surface != next_surface) {
                Access::surfaces(component).reset();
                Access::surface_geometry(component).reset();
            }
            Access::instance_surfaces(component).clear();
            Access::instance_output(component).reset();
            Access::graph_source(component) = std::move(source);
            Access::output(component) = std::move(published);
            return {};
        });
    if (!changed)
        return changed;
    return (*owner && capability_changed) ? (*owner)->reevaluate_capabilities(object) : Result<void>{};
}

Result<String> validate_procedural_key(std::string_view key) {
    if (key.size() > 4096 || key.find('\0') != std::string_view::npos)
        return error(ErrorCode::invalid_definition, "Invalid procedural graph key.");
    String source;
    if (!source.assign(StringView{key}))
        return error(ErrorCode::invalid_definition, "String requires valid UTF-8.");
    return source;
}

} // namespace

Result<void> ProceduralGeneratorComponent::validate() const {
    if (!std::isfinite(chunk_size) || chunk_size < .1f || chunk_size > 100000.f)
        return error(ErrorCode::argument_mismatch, "Generator chunk size must stay within [0.1, 100000] metres.");
    return {};
}

Result<Types> register_types(Runtime& runtime) {
    ATELIER_TRY(material::register_resource_types(runtime));
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<StaticMeshComponent>::build(runtime));
    definitions.push_back(reflection::Definition<StaticMeshEntity>::build(runtime));
    definitions.push_back(reflection::Definition<LightKind>::build(runtime));
    definitions.push_back(reflection::Definition<LightSettings>::build(runtime));
    definitions.push_back(reflection::Definition<LightComponent>::build(runtime));
    definitions.push_back(reflection::Definition<Antialiasing>::build(runtime));
    definitions.push_back(reflection::Definition<ProbePlacement>::build(runtime));
    definitions.push_back(reflection::Definition<ProbeVolume>::build(runtime));
    definitions.push_back(reflection::Definition<EnvironmentComponent>::build(runtime));
    definitions.push_back(reflection::Definition<CameraComponent>::build(runtime));
    definitions.push_back(reflection::Definition<ProceduralGeneratorComponent>::build(runtime));
    definitions.push_back(reflection::Definition<ProceduralGeneratorEntity>::build(runtime));
    if (!runtime.find_type("core.String"))
        definitions.push_back(string_definition(runtime));
    ATELIER_TRY_BIND(ids, runtime.register_types(std::move(definitions)));
    Types types;
    types.static_mesh_component = (*ids)[0];
    types.static_mesh_entity = (*ids)[1];
    types.light_kind = (*ids)[2];
    types.light_settings = (*ids)[3];
    types.light_component = (*ids)[4];
    types.environment_component = (*ids)[8];
    types.camera_component = (*ids)[9];
    types.procedural_generator_component = (*ids)[10];
    types.procedural_generator_entity = (*ids)[11];
    return types;
}

Result<ObjectRoot> create_static_mesh_entity(world::World& world, const StaticMeshEntityDesc& description) {
    return create_spatial_entity<StaticMeshEntity, StaticMeshComponent>(
        world, description.transform, [&](Runtime& runtime, ObjectId component) {
            return set_mesh_binding(
                runtime, component,
                {std::string(description.mesh_source), description.mesh, description.default_materials});
        });
}

Result<void> set_geometry(Runtime& runtime, ObjectId object, geometry::MeshRef geometry) {
    return set_mesh_binding(runtime, object, {{}, std::move(geometry)});
}

Result<geometry::MeshSource> read_mesh_source(Runtime& runtime, ObjectId object) {
    ATELIER_TRY_BIND(status, world::inspect_component(runtime, object));
    return runtime.with_native<const StaticMeshComponent>(
        object, [](const StaticMeshComponent& component) -> Result<geometry::MeshSource> {
            return geometry::MeshSource{std::string(Access::mesh_source(component).view().std_view()),
                                        Access::geometry(component)};
        });
}

Result<StaticMeshState> read_static_mesh(Runtime& runtime, ObjectId object) {
    ATELIER_TRY_BIND(mesh, read_mesh_source(runtime, object));
    return runtime.with_native<const StaticMeshComponent>(
        object, [&](const StaticMeshComponent& component) -> Result<StaticMeshState> {
            StaticMeshState state{{std::move(mesh->source), std::move(mesh->resolved)}};
            ATELIER_TRY_BIND(field, runtime.reference_field(object, Access::default_materials(component)));
            const auto& slots = Access::default_slots(component);
            state.mesh.defaults.reserve(slots.size());
            for (std::size_t i = 0; i < slots.size(); ++i) {
                ATELIER_TRY_BIND(reference, runtime.sequence_read(*field, i));
                ATELIER_TRY_BIND(root, runtime.hold(*reference));
                state.mesh.defaults.push_back({slots[i], std::move(*root), false});
            }
            return state;
        });
}

Result<void> set_mesh_binding(Runtime& runtime, ObjectId object, MeshBinding binding) {
    if (binding.source.size() > 4096 || binding.source.find('\0') != std::string::npos)
        return error(ErrorCode::invalid_definition, "Invalid mesh source key.");
    String source;
    if (!source.assign(StringView{binding.source}))
        return error(ErrorCode::invalid_definition, "String requires valid UTF-8.");
    ATELIER_TRY(runtime.check_code_publication());
    // Each default names one slot of the new mesh exactly once and holds a material that captures.
    std::vector<std::string> default_slots;
    std::vector<ObjectReference> defaults;
    Set<std::string_view> unique;
    unique.reserve(binding.defaults.size());
    for (const auto& item : binding.defaults) {
        std::size_t matches{};
        if (binding.resolved)
            for (std::size_t j = 0; j < binding.resolved->materials().size(); ++j)
                matches += material_slot_name(*binding.resolved, j) == item.slot;
        if (matches != 1 || !unique.add(item.slot))
            return error(ErrorCode::argument_mismatch,
                         "Default material slot is missing, ambiguous or repeated: " + item.slot);
        ATELIER_TRY(material::capture_program(runtime, item.resource.id()));
        default_slots.push_back(item.slot);
        defaults.push_back(item.resource.reference());
    }
    ATELIER_TRY_BIND(owner, world::admit_component_edit(runtime, object));
    auto changed =
        runtime.with_native<StaticMeshComponent>(object, [&](StaticMeshComponent& component) -> Result<void> {
            std::vector<std::string> names;
            std::vector<bool> overrides;
            std::vector<ObjectReference> references;
            ATELIER_TRY_BIND(field, runtime.reference_field(object, Access::materials(component)));
            ATELIER_TRY_BIND(count, runtime.sequence_size(*field));
            ATELIER_TRY_BIND(default_field, runtime.reference_field(object, Access::default_materials(component)));
            ATELIER_TRY_BIND(default_count, runtime.sequence_size(*default_field));
            const auto& previous = Access::material_slots(component);
            for (std::size_t i = 0; i < previous.size(); ++i) {
                std::size_t matches{};
                if (binding.resolved)
                    for (std::size_t j = 0; j < binding.resolved->materials().size(); ++j)
                        matches += material_slot_name(*binding.resolved, j) == previous[i];
                if (matches != 1)
                    continue;
                ATELIER_TRY_BIND(reference, runtime.sequence_read(*field, i));
                names.push_back(previous[i]);
                overrides.push_back(Access::material_overrides(component)[i]);
                references.push_back(*reference);
            }
            const std::array<ReferenceChange, 2> changes{SequenceSplice{*field, 0, *count, references},
                                                         SequenceSplice{*default_field, 0, *default_count, defaults}};
            ATELIER_TRY(runtime.apply_reference_changes(changes));
            Access::material_slots(component) = std::move(names);
            Access::material_overrides(component) = std::move(overrides);
            Access::default_slots(component) = std::move(default_slots);
            Access::surfaces(component).reset();
            Access::surface_geometry(component).reset();
            Access::mesh_source(component) = std::move(source);
            Access::geometry(component) = std::move(binding.resolved);
            return {};
        });
    if (!changed)
        return changed;
    return *owner ? (*owner)->reevaluate_capabilities(object) : Result<void>{};
}

geometry::MeshRef build_procedural_surface_mesh(const procedural::Geometry& geometry) {
    const auto prim_count = geometry.count(procedural::Layer::primitive);
    if (prim_count == 0)
        return {};
    const auto& positions = geometry.positions();
    const auto vp = geometry.vertex_points();

    const Array<math::Vector3>* vtx_n = nullptr;
    const Array<math::Vector3>* pt_n = nullptr;
    if (auto found = geometry.read<math::Vector3>("N", procedural::Layer::vertex); found && *found)
        vtx_n = *found;
    else if (auto found_p = geometry.read<math::Vector3>("N", procedural::Layer::point); found_p && *found_p)
        pt_n = *found_p;

    const Array<math::Vector2>* vtx_uv = nullptr;
    const Array<math::Vector2>* pt_uv = nullptr;
    if (auto found = geometry.read<math::Vector2>("uv", procedural::Layer::vertex); found && *found)
        vtx_uv = *found;
    else if (auto found_p = geometry.read<math::Vector2>("uv", procedural::Layer::point); found_p && *found_p)
        pt_uv = *found_p;

    std::vector<geometry::Vertex> vertices;
    std::vector<std::uint8_t> edge_masks;

    for (std::uint32_t p = 0; p < prim_count; ++p) {
        const auto range = geometry.primitive(p);
        if (range.kind != procedural::PrimitiveKind::polygon || range.count < 3)
            continue;
        const std::uint32_t v0 = range.first;
        bool all_finite = true;
        for (std::uint32_t k = 0; k < range.count; ++k) {
            if (!math::is_finite(positions[vp[v0 + k]])) {
                all_finite = false;
                break;
            }
        }
        if (!all_finite)
            continue;

        const math::Vector3 p0 = positions[vp[v0]];
        math::Vector3 face_n{0.f, 1.f, 0.f};
        if (!vtx_n && !pt_n) {
            const auto p0d = static_cast<math::Vector3d>(p0);
            math::Vector3d area_normal{0., 0., 0.};
            for (std::uint32_t i = 1; i + 1 < range.count; ++i) {
                const math::Vector3d e1 = static_cast<math::Vector3d>(positions[vp[v0 + i]]) - p0d;
                const math::Vector3d e2 = static_cast<math::Vector3d>(positions[vp[v0 + i + 1]]) - p0d;
                area_normal = area_normal + math::cross(e1, e2);
            }
            if (const auto unit = math::try_normalize(area_normal))
                face_n = static_cast<math::Vector3>(*unit);
        }

        const auto make_vertex = [&](std::uint32_t v_idx) {
            const std::uint32_t pt_idx = vp[v_idx];
            const math::Vector3 pos = positions[pt_idx];
            const math::Vector3 n = vtx_n  ? math::try_normalize((*vtx_n)[v_idx]).value_or(face_n)
                                    : pt_n ? math::try_normalize((*pt_n)[pt_idx]).value_or(face_n)
                                           : face_n;
            const math::Vector2 uv = vtx_uv  ? safe_uv((*vtx_uv)[v_idx])
                                     : pt_uv ? safe_uv((*pt_uv)[pt_idx])
                                             : math::Vector2{pos.x * .5f, pos.z * .5f};
            geometry::Vertex out{};
            out.position[0] = pos.x;
            out.position[1] = pos.y;
            out.position[2] = pos.z;
            out.normal[0] = n.x;
            out.normal[1] = n.y;
            out.normal[2] = n.z;
            out.uv[0] = uv.x;
            out.uv[1] = uv.y;
            return out;
        };

        for (std::uint32_t i = 1; i + 1 < range.count; ++i) {
            const std::uint32_t v1 = v0 + i;
            const std::uint32_t v2 = v0 + i + 1;
            vertices.push_back(make_vertex(v0));
            vertices.push_back(make_vertex(v1));
            vertices.push_back(make_vertex(v2));
            std::uint8_t mask = 1u;
            if (i + 2 == range.count)
                mask |= 2u;
            if (i == 1)
                mask |= 4u;
            edge_masks.push_back(mask);
        }
    }

    if (vertices.empty())
        return {};
    std::vector<geometry::MaterialSlot> slots{{geometry::MaterialSlot{"default"}}};
    std::vector<geometry::MeshPart> parts{{geometry::MeshPart{0, static_cast<std::uint32_t>(vertices.size()), 0}}};
    auto created =
        geometry::Mesh::create(std::move(vertices), std::move(edge_masks), std::move(slots), std::move(parts));
    return created ? std::move(*created) : geometry::MeshRef{};
}

Result<ObjectRoot> create_procedural_generator_entity(world::World& world,
                                                      const ProceduralGeneratorEntityDesc& description) {
    return create_spatial_entity<ProceduralGeneratorEntity, ProceduralGeneratorComponent>(
        world, description.transform, [&](Runtime& runtime, ObjectId component) -> Result<void> {
            if (!description.graph_source.empty() || description.output)
                return set_procedural_binding(
                    runtime, component,
                    {std::string(description.graph_source), description.output, description.resolved_assets});
            return {};
        });
}

Result<void> set_procedural_binding(Runtime& runtime, ObjectId object, ProceduralBinding binding) {
    ATELIER_TRY_BIND(source, validate_procedural_key(binding.source));
    ATELIER_TRY(runtime.check_code_publication());

    std::vector<ObjectReference> inst_defaults;
    std::shared_ptr<const ProceduralOutput> published;
    if (binding.output) {
        ATELIER_TRY_BIND(validated, validate_procedural_assets(runtime, binding.resolved_assets));
        inst_defaults = std::move(validated->second);
        if (binding.output->assets == validated->first) {
            published = std::move(binding.output);
        } else {
            auto built = std::make_shared<ProceduralOutput>();
            built->generation = binding.output->generation;
            built->geometry = binding.output->geometry;
            built->surface_mesh = binding.output->surface_mesh;
            built->publication = binding.output->publication;
            built->assets = std::move(validated->first);
            built->mesh_dependencies = binding.output->mesh_dependencies;
            published = std::move(built);
        }
    }
    return commit_procedural_binding(runtime, object, std::move(*source), std::move(published), inst_defaults);
}

Result<std::shared_ptr<const ProceduralOutput>>
publish_procedural_binding(Runtime& runtime, ObjectId object, std::string source_key, ProceduralOutput output,
                           std::span<const ProceduralInstanceAsset> resolved_assets) {
    ATELIER_TRY_BIND(source, validate_procedural_key(source_key));
    ATELIER_TRY(runtime.check_code_publication());
    ATELIER_TRY_BIND(validated, validate_procedural_assets(runtime, resolved_assets));
    auto built = std::make_shared<ProceduralOutput>(std::move(output));
    built->assets = std::move(validated->first);
    std::shared_ptr<const ProceduralOutput> published = std::move(built);
    ATELIER_TRY(commit_procedural_binding(runtime, object, std::move(*source), published, validated->second));
    return published;
}

Result<ProceduralSource> read_procedural_source(Runtime& runtime, ObjectId object) {
    ATELIER_TRY(world::inspect_component(runtime, object));
    return runtime.with_native<const ProceduralGeneratorComponent>(
        object, [](const ProceduralGeneratorComponent& component) -> Result<ProceduralSource> {
            return ProceduralSource{std::string(Access::graph_source(component).view().std_view()),
                                    Access::output(component)};
        });
}

Result<ProceduralBinding> read_procedural_binding(Runtime& runtime, ObjectId object) {
    ATELIER_TRY(world::inspect_component(runtime, object));
    return runtime.with_native<const ProceduralGeneratorComponent>(
        object, [&](const ProceduralGeneratorComponent& component) -> Result<ProceduralBinding> {
            ProceduralBinding result;
            result.source = std::string(Access::graph_source(component).view().std_view());
            result.output = Access::output(component);
            if (result.output && !result.output->assets.empty()) {
                ATELIER_TRY_BIND(inst_field, runtime.reference_field(object, Access::instance_materials(component)));
                result.resolved_assets.reserve(result.output->assets.size());
                for (const auto& range : result.output->assets) {
                    ProceduralInstanceAsset asset{range.key, range.mesh, {}};
                    asset.defaults.reserve(range.default_count);
                    for (std::size_t i = 0; i < range.default_count; ++i) {
                        ATELIER_TRY_BIND(ref, runtime.sequence_read(*inst_field, range.default_offset + i));
                        ATELIER_TRY_BIND(root, runtime.hold(*ref));
                        asset.defaults.push_back({range.default_slots[i], std::move(*root), false});
                    }
                    result.resolved_assets.push_back(std::move(asset));
                }
            }
            return result;
        });
}

}

#include "detail/detail.hpp"
#include "runtime/foundation.hpp"
#include <exception>
#include <unordered_set>

namespace atelier::presentation {
using detail::Access;

using detail::error;

Result<Types> register_types(Runtime& runtime) {
    ATELIER_TRY(material::register_resource_types(runtime));
    std::vector<TypeDefinition> definitions;
    definitions.push_back(reflection::Definition<StaticMeshComponent>::build(runtime));
    definitions.push_back(reflection::Definition<StaticMeshEntity>::build(runtime));
    definitions.push_back(reflection::Definition<LightKind>::build(runtime));
    definitions.push_back(reflection::Definition<LightSettings>::build(runtime));
    definitions.back().validate = [](const void* value) {
        return detail::light_rules(*static_cast<const LightSettings*>(value));
    };
    definitions.push_back(reflection::Definition<LightComponent>::build(runtime));
    definitions.push_back(reflection::Definition<Antialiasing>::build(runtime));
    definitions.push_back(reflection::Definition<ProbePlacement>::build(runtime));
    definitions.push_back(reflection::Definition<ProbeVolume>::build(runtime));
    definitions.push_back(reflection::Definition<EnvironmentComponent>::build(runtime));
    definitions.back().validate = [](const void* value) {
        return detail::environment_rules(*static_cast<const EnvironmentComponent*>(value));
    };
    definitions.push_back(reflection::Definition<CameraComponent>::build(runtime));
    definitions.back().validate = [](const void* value) {
        return detail::camera_rules(*static_cast<const CameraComponent*>(value));
    };
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
    return types;
}

Result<ObjectRoot> create_static_mesh_entity(world::World& world, const StaticMeshEntityDesc& description) {
    if (world.closed())
        return error(ErrorCode::unsupported_operation, "World is closed.");
    ATELIER_TRY_BIND(transform, world::canonical_transform(description.transform));
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
    ATELIER_TRY_BIND(entity_type, runtime.native_type<StaticMeshEntity>());
    ATELIER_TRY_BIND(component_type, runtime.native_type<StaticMeshComponent>());
    ATELIER_TRY_BIND(entity, world::create_entity(runtime, *entity_type));
    created.entity = std::move(*entity);
    ATELIER_TRY_BIND(component, world::create_component(runtime, *component_type));
    created.component = std::move(*component);
    ATELIER_TRY(world.set_transform(created.entity.id(), *transform));
    ATELIER_TRY_BIND(geometry, set_mesh_binding(runtime, created.component.id(),
                                                {std::string(description.mesh_source), description.mesh,
                                                 description.default_materials}));
    const std::array composition{world::Attachment{created.component.id()}};
    ATELIER_TRY_BIND(composed, world.attach_components(created.entity.id(), composition));
    if (composed->notification_error)
        return std::unexpected(std::move(*composed->notification_error));
    created.component.reset(); // Entity composition now retains its mesh capability.
    return std::move(created.entity);
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
    try {
        source = String(StringView(binding.source));
    } catch (const std::exception& failure) {
        return error(ErrorCode::invalid_definition, failure.what());
    }
    ATELIER_TRY(runtime.check_code_publication());
    // Each default names one slot of the new mesh exactly once and holds a material that captures.
    std::vector<std::string> default_slots;
    std::vector<ObjectReference> defaults;
    std::unordered_set<std::string> unique;
    for (const auto& item : binding.defaults) {
        std::size_t matches{};
        if (binding.resolved)
            for (std::size_t j = 0; j < binding.resolved->materials().size(); ++j)
                matches += material_slot_name(*binding.resolved, j) == item.slot;
        if (matches != 1 || !unique.insert(item.slot).second)
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

}

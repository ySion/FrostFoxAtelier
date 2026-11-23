#include "detail/detail.hpp"
#include <unordered_set>

namespace atelier::presentation {
using detail::Access;

using detail::error;

namespace {

template <class ComponentT>
Result<std::vector<MaterialBinding>> read_bindings_of(Runtime& runtime, ObjectId id, const ComponentT& value) {
    ATELIER_TRY_BIND(field, runtime.reference_field(id, Access::materials(value)));
    std::vector<MaterialBinding> result;
    result.reserve(Access::material_slots(value).size());
    for (std::size_t i = 0; i < Access::material_slots(value).size(); ++i) {
        ATELIER_TRY_BIND(reference, runtime.sequence_read(*field, i));
        ATELIER_TRY_BIND(root, runtime.hold(*reference));
        result.push_back({Access::material_slots(value)[i], std::move(*root), Access::material_overrides(value)[i]});
    }
    return result;
}

template <class ComponentT>
Result<void> write_bindings_of(Runtime& runtime, ObjectId id, ComponentT& value, std::vector<std::string> names,
                               std::vector<bool> flags, std::span<const ObjectReference> references) {
    ATELIER_TRY_BIND(field, runtime.reference_field(id, Access::materials(value)));
    ATELIER_TRY_BIND(size, runtime.sequence_size(*field));
    const ReferenceChange change = SequenceSplice{*field, 0, *size, references};
    ATELIER_TRY(runtime.apply_reference_changes({&change, 1}));
    Access::material_slots(value) = std::move(names);
    Access::material_overrides(value) = std::move(flags);
    Access::surfaces(value).reset();
    return {};
}

Result<material::ProgramSurfaceRef> capture_slot_surface(Runtime& runtime, const ReferenceField& sequence,
                                                         std::size_t index,
                                                         std::unordered_set<ObjectId>* dependencies) {
    ATELIER_TRY_BIND(reference, runtime.sequence_read(sequence, index));
    if (!dependencies)
        return material::capture_program(runtime, reference->id());
    ATELIER_TRY_BIND(captured, material::capture_program_with_dependencies(runtime, reference->id()));
    dependencies->merge(captured->dependencies);
    return std::move(captured->surface);
}

} // namespace

Result<std::vector<MaterialBinding>> read_material_bindings(Runtime& runtime, ObjectId id) {
    ATELIER_TRY_BIND(type, runtime.type_of(id));
    ATELIER_TRY_BIND(mesh_type, runtime.native_type<StaticMeshComponent>());
    ATELIER_TRY_BIND(mesh, runtime.is_a(*type, *mesh_type));
    if (*mesh)
        return runtime.with_native<const StaticMeshComponent>(
            id, [&](const StaticMeshComponent& value) { return read_bindings_of(runtime, id, value); });
    return runtime.with_native<const ProceduralGeneratorComponent>(
        id, [&](const ProceduralGeneratorComponent& value) { return read_bindings_of(runtime, id, value); });
}

Result<void> set_material_bindings(Runtime& runtime, ObjectId id, std::span<const MaterialBinding> bindings) {
    ATELIER_TRY(runtime.check_code_publication());
    ATELIER_TRY_BIND(component, world::inspect_component(runtime, id));
    if (!component->valid)
        return error(ErrorCode::invalid_member, "Material bindings need a live mesh or generator component.");
    ATELIER_TRY(world::admit_component_edit(runtime, id));
    if (const auto gen_type = runtime.native_type<ProceduralGeneratorComponent>();
        gen_type && runtime.is_a(component->type, *gen_type).value_or(false)) {
        return runtime.with_native<ProceduralGeneratorComponent>(id, [&](auto& value) -> Result<void> {
            if (bindings.size() > 1)
                return error(ErrorCode::argument_mismatch, "Too many material slot bindings.");
            std::vector<std::string> names;
            std::vector<bool> flags;
            std::vector<ObjectReference> references;
            for (const auto& binding : bindings) {
                if (binding.slot != "default")
                    return error(ErrorCode::argument_mismatch,
                                 "Material slot is missing or ambiguous: " + binding.slot);
                ATELIER_TRY_BIND(held, runtime.hold(binding.resource.reference()));
                ATELIER_TRY(material::capture_program(runtime, held->id()));
                names.push_back(binding.slot);
                flags.push_back(binding.overridden);
                references.push_back(held->reference());
            }
            return write_bindings_of(runtime, id, value, std::move(names), std::move(flags), references);
        });
    }
    return runtime.with_native<StaticMeshComponent>(id, [&](auto& value) -> Result<void> {
        const auto& mesh = Access::geometry(value);
        if (bindings.size() > (mesh ? mesh->materials().size() : 0))
            return error(ErrorCode::argument_mismatch, "Too many material slot bindings.");
        std::vector<std::string> names;
        std::vector<bool> flags;
        std::vector<ObjectReference> references;
        names.reserve(bindings.size());
        flags.reserve(bindings.size());
        references.reserve(bindings.size());
        std::unordered_set<std::string> unique;
        for (const auto& binding : bindings) {
            if (!unique.insert(binding.slot).second)
                return error(ErrorCode::argument_mismatch, "Duplicate material slot binding.");
            std::size_t matches{};
            for (std::size_t i = 0; i < mesh->materials().size(); ++i)
                matches += material_slot_name(*mesh, i) == binding.slot;
            if (matches != 1)
                return error(ErrorCode::argument_mismatch, "Material slot is missing or ambiguous: " + binding.slot);
            ATELIER_TRY_BIND(held, runtime.hold(binding.resource.reference()));
            ATELIER_TRY(material::capture_program(runtime, held->id()));
            names.push_back(binding.slot);
            flags.push_back(binding.overridden);
            references.push_back(held->reference());
        }
        return write_bindings_of(runtime, id, value, std::move(names), std::move(flags), references);
    });
}

Result<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>> material_surfaces(Runtime& runtime,
                                                                                          ObjectId id) {
    const auto gen_type = runtime.native_type<ProceduralGeneratorComponent>();
    const auto obj_type = runtime.type_of(id);
    if (gen_type && obj_type && runtime.is_a(*obj_type, *gen_type).value_or(false)) {
        auto captured =
            runtime.with_native<const ProceduralGeneratorComponent>(id, [&](const ProceduralGeneratorComponent& value) {
                return detail::generator_surfaces(runtime, id, value);
            });
        if (!captured)
            return std::unexpected(captured.error());
        if (captured->uncached)
            ATELIER_TRY(detail::store_generator_surfaces(runtime, id, *captured));
        return captured->surface;
    }
    auto snapshot = runtime.with_native<const StaticMeshComponent>(
        id, [&](const StaticMeshComponent& value) { return detail::material_surfaces(runtime, id, value); });
    if (!snapshot)
        return snapshot;
    ATELIER_TRY(detail::store_surfaces(runtime, id, *snapshot));
    return snapshot;
}

Result<void> detail::store_surfaces(Runtime& runtime, ObjectId id,
                                    std::shared_ptr<const std::vector<material::ProgramSurfaceRef>> snapshot) {
    auto cached = runtime.with_native<const StaticMeshComponent>(
        id, [&](const StaticMeshComponent& value) -> Result<bool> { return Access::surfaces(value) == snapshot; });
    if (!cached)
        return std::unexpected(cached.error());
    if (*cached)
        return {};
    return runtime.with_native<StaticMeshComponent>(id, [&](StaticMeshComponent& value) -> Result<void> {
        Access::surfaces(value) = std::move(snapshot);
        Access::surface_geometry(value) = Access::geometry(value);
        return {};
    });
}

Result<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>>
detail::material_surfaces(Runtime& runtime, ObjectId id, const StaticMeshComponent& value,
                          std::unordered_set<ObjectId>* dependencies) {
    using Snapshot = std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>;
    const auto& mesh = Access::geometry(value);
    if (!mesh)
        return Snapshot{};
    const auto& cached = Access::surfaces(value);
    const bool base_changed = !cached || Access::surface_geometry(value) != mesh;
    const auto& slots = Access::material_slots(value);
    const auto& defaults = Access::default_slots(value);
    if (!base_changed && slots.empty() && defaults.empty())
        return cached;
    std::optional<ReferenceField> explicit_field;
    std::optional<ReferenceField> default_field;
    if (!slots.empty()) {
        ATELIER_TRY_BIND(references, runtime.reference_field(id, Access::materials(value)));
        explicit_field = std::move(*references);
    }
    if (!defaults.empty()) {
        ATELIER_TRY_BIND(references, runtime.reference_field(id, Access::default_materials(value)));
        default_field = std::move(*references);
    }
    std::shared_ptr<std::vector<material::ProgramSurfaceRef>> next;
    const auto writable = [&]() -> auto& {
        if (!next)
            next = base_changed ? std::make_shared<std::vector<material::ProgramSurfaceRef>>(mesh->materials().size())
                                : std::make_shared<std::vector<material::ProgramSurfaceRef>>(*cached);
        return *next;
    };
    for (std::size_t i = 0; i < mesh->materials().size(); ++i) {
        material::ProgramSurfaceRef surface;
        const auto name = material_slot_name(*mesh, i);
        if (const auto at = std::ranges::find(slots, name); at != slots.end()) {
            ATELIER_TRY_BIND(captured,
                             capture_slot_surface(runtime, *explicit_field,
                                                  static_cast<std::size_t>(at - slots.begin()), dependencies));
            surface = *captured;
        } else if (const auto at = std::ranges::find(defaults, name); at != defaults.end()) {
            ATELIER_TRY_BIND(captured,
                             capture_slot_surface(runtime, *default_field,
                                                  static_cast<std::size_t>(at - defaults.begin()), dependencies));
            surface = *captured;
        } else if (base_changed) {
            ATELIER_TRY_BIND(captured, material::default_world_surface());
            surface = *captured;
        } else
            surface = (*cached)[i];
        if (base_changed || (*cached)[i] != surface)
            writable()[i] = std::move(surface);
    }
    if (next)
        return Snapshot{std::move(next)};
    return cached;
}

Result<detail::GeneratorCapturedSurfaces> detail::generator_surfaces(Runtime& runtime, ObjectId id,
                                                                     const ProceduralGeneratorComponent& value,
                                                                     std::unordered_set<ObjectId>* dependencies) {
    GeneratorCapturedSurfaces result;
    const auto& output = Access::output(value);
    if (!output)
        return result;

    if (output->surface_mesh) {
        const auto& cached_surf = Access::surfaces(value);
        const bool surf_changed = !cached_surf || Access::surface_geometry(value) != output->surface_mesh;
        const auto& slots = Access::material_slots(value);
        if (!surf_changed && slots.empty()) {
            result.surface = cached_surf;
        } else {
            material::ProgramSurfaceRef surf;
            if (!slots.empty()) {
                ATELIER_TRY_BIND(references, runtime.reference_field(id, Access::materials(value)));
                ATELIER_TRY_BIND(captured, capture_slot_surface(runtime, *references, 0, dependencies));
                surf = std::move(*captured);
            } else {
                ATELIER_TRY_BIND(captured, material::default_world_surface());
                surf = std::move(*captured);
            }
            if (cached_surf && cached_surf->size() == 1 && (*cached_surf)[0] == surf) {
                result.surface = cached_surf;
            } else {
                result.surface = std::make_shared<const std::vector<material::ProgramSurfaceRef>>(
                    std::vector<material::ProgramSurfaceRef>{std::move(surf)});
                result.uncached = true;
            }
        }
    }

    if (!output->assets.empty()) {
        const auto& cached_assets = Access::instance_surfaces(value);
        const bool same_output =
            Access::instance_output(value) == output && cached_assets.size() == output->assets.size();
        std::optional<ReferenceField> inst_field;
        ATELIER_TRY_BIND(references, runtime.reference_field(id, Access::instance_materials(value)));
        inst_field = std::move(*references);
        result.assets.resize(output->assets.size());
        for (std::size_t a = 0; a < output->assets.size(); ++a) {
            const auto& asset = output->assets[a];
            if (!asset.mesh)
                continue;
            if (same_output && asset.default_count == 0 && cached_assets[a]) {
                result.assets[a] = cached_assets[a];
                continue;
            }
            std::vector<material::ProgramSurfaceRef> list(asset.mesh->materials().size());
            for (std::size_t m = 0; m < asset.mesh->materials().size(); ++m) {
                const auto slot_name = material_slot_name(*asset.mesh, m);
                const auto at = std::ranges::find(asset.default_slots, slot_name);
                if (at != asset.default_slots.end()) {
                    const std::size_t idx =
                        asset.default_offset + static_cast<std::size_t>(at - asset.default_slots.begin());
                    ATELIER_TRY_BIND(captured, capture_slot_surface(runtime, *inst_field, idx, dependencies));
                    list[m] = std::move(*captured);
                } else {
                    ATELIER_TRY_BIND(captured, material::default_world_surface());
                    list[m] = std::move(*captured);
                }
            }
            if (same_output && cached_assets[a] && *cached_assets[a] == list) {
                result.assets[a] = cached_assets[a];
            } else {
                result.assets[a] = std::make_shared<const std::vector<material::ProgramSurfaceRef>>(std::move(list));
                result.uncached = true;
            }
        }
        if (!same_output)
            result.uncached = true;
    }

    return result;
}

Result<void> detail::store_generator_surfaces(Runtime& runtime, ObjectId id,
                                              const GeneratorCapturedSurfaces& captured) {
    return runtime.with_native<ProceduralGeneratorComponent>(
        id, [&](ProceduralGeneratorComponent& value) -> Result<void> {
            const auto& output = Access::output(value);
            Access::surfaces(value) = captured.surface;
            Access::surface_geometry(value) = output ? output->surface_mesh : geometry::MeshRef{};
            Access::instance_surfaces(value) = captured.assets;
            Access::instance_output(value) = output;
            return {};
        });
}
}

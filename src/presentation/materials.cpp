#include "presentation/detail.hpp"
#include <unordered_set>

namespace atelier::presentation {
using detail::Access;
using detail::error;
Result<std::vector<MaterialBinding>> read_material_bindings(Runtime& runtime, ObjectId id) {
    return runtime.with_native<const StaticMeshComponent>(
        id, [&](const StaticMeshComponent& value) -> Result<std::vector<MaterialBinding>> {
            auto field = runtime.reference_field(id, Access::materials(value));
            if (!field)
                return std::unexpected(field.error());
            std::vector<MaterialBinding> result;
            result.reserve(Access::material_slots(value).size());
            for (std::size_t i = 0; i < Access::material_slots(value).size(); ++i) {
                auto reference = runtime.sequence_read(*field, i);
                if (!reference)
                    return std::unexpected(reference.error());
                auto root = runtime.hold(*reference);
                if (!root)
                    return std::unexpected(root.error());
                result.push_back(
                    {Access::material_slots(value)[i], std::move(*root), Access::material_overrides(value)[i]});
            }
            return result;
        });
}
Result<void> set_material_bindings(Runtime& runtime, ObjectId id, std::span<const MaterialBinding> bindings) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return ready;
    auto component = world::inspect_component(runtime, id);
    if (!component)
        return std::unexpected(component.error());
    if (!component->valid)
        return error(ErrorCode::invalid_member, "Material bindings need a live mesh component.");
    if (auto admitted = world::admit_component_edit(runtime, id); !admitted)
        return std::unexpected(admitted.error());
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
            auto held = runtime.hold(binding.resource.reference());
            if (!held)
                return std::unexpected(held.error());
            auto surface = material::capture_program(runtime, held->id());
            if (!surface)
                return std::unexpected(surface.error());
            names.push_back(binding.slot);
            flags.push_back(binding.overridden);
            references.push_back(held->reference());
        }
        auto field = runtime.reference_field(id, Access::materials(value));
        if (!field)
            return std::unexpected(field.error());
        auto size = runtime.sequence_size(*field);
        if (!size)
            return std::unexpected(size.error());
        const ReferenceChange change = SequenceSplice{*field, 0, *size, references};
        auto written = runtime.apply_reference_changes({&change, 1});
        if (!written)
            return written;
        Access::material_slots(value) = std::move(names);
        Access::material_overrides(value) = std::move(flags);
        Access::surfaces(value).reset();
        return {};
    });
}
Result<std::shared_ptr<const std::vector<material::ProgramSurfaceRef>>> material_surfaces(Runtime& runtime,
                                                                                          ObjectId id) {
    auto snapshot = runtime.with_native<const StaticMeshComponent>(
        id, [&](const StaticMeshComponent& value) { return detail::material_surfaces(runtime, id, value); });
    if (!snapshot)
        return snapshot;
    if (auto stored = detail::store_surfaces(runtime, id, *snapshot); !stored)
        return std::unexpected(stored.error());
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
    std::optional<ReferenceField> explicit_field, default_field;
    if (!slots.empty()) {
        auto references = runtime.reference_field(id, Access::materials(value));
        if (!references)
            return std::unexpected(references.error());
        explicit_field = std::move(*references);
    }
    if (!defaults.empty()) {
        auto references = runtime.reference_field(id, Access::default_materials(value));
        if (!references)
            return std::unexpected(references.error());
        default_field = std::move(*references);
    }
    std::shared_ptr<std::vector<material::ProgramSurfaceRef>> next;
    const auto writable = [&]() -> auto& {
        if (!next)
            next = base_changed ? std::make_shared<std::vector<material::ProgramSurfaceRef>>(mesh->materials().size())
                                : std::make_shared<std::vector<material::ProgramSurfaceRef>>(*cached);
        return *next;
    };
    const auto capture = [&](const ReferenceField& sequence, std::size_t index) -> Result<material::ProgramSurfaceRef> {
        auto reference = runtime.sequence_read(sequence, index);
        if (!reference)
            return std::unexpected(reference.error());
        if (!dependencies)
            return material::capture_program(runtime, reference->id());
        auto captured = material::capture_program_with_dependencies(runtime, reference->id());
        if (!captured)
            return std::unexpected(captured.error());
        dependencies->merge(captured->dependencies);
        return std::move(captured->surface);
    };
    for (std::size_t i = 0; i < mesh->materials().size(); ++i) {
        material::ProgramSurfaceRef surface;
        const auto name = material_slot_name(*mesh, i);
        if (const auto at = std::ranges::find(slots, name); at != slots.end()) {
            auto captured = capture(*explicit_field, static_cast<std::size_t>(at - slots.begin()));
            if (!captured)
                return std::unexpected(captured.error());
            surface = *captured;
        } else if (const auto at = std::ranges::find(defaults, name); at != defaults.end()) {
            auto captured = capture(*default_field, static_cast<std::size_t>(at - defaults.begin()));
            if (!captured)
                return std::unexpected(captured.error());
            surface = *captured;
        } else if (base_changed) {
            auto captured = material::default_world_surface();
            if (!captured)
                return std::unexpected(captured.error());
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
}

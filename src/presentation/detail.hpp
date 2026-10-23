#pragma once

#include "presentation/presentation.hpp"

namespace atelier::presentation::detail {

struct Access {
    static geometry::MeshRef& geometry(StaticMeshComponent& value) noexcept { return value.geometry_; }
    static const geometry::MeshRef& geometry(const StaticMeshComponent& value) noexcept { return value.geometry_; }
    static String& mesh_source(StaticMeshComponent& value) noexcept { return value.mesh_source_; }
    static const String& mesh_source(const StaticMeshComponent& value) noexcept { return value.mesh_source_; }
    static ReferenceSequence& materials(StaticMeshComponent& value) noexcept {return value.materials_;}
    static const ReferenceSequence& materials(const StaticMeshComponent& value) noexcept {return value.materials_;}
    static std::vector<std::string>& material_slots(StaticMeshComponent& value) noexcept {return value.material_slots_;}
    static const std::vector<std::string>& material_slots(const StaticMeshComponent& value) noexcept {return value.material_slots_;}
    static std::vector<bool>& material_overrides(StaticMeshComponent& value) noexcept {return value.material_overrides_;}
    static const std::vector<bool>& material_overrides(const StaticMeshComponent& value) noexcept {return value.material_overrides_;}
    static ReferenceSequence& default_materials(StaticMeshComponent& value) noexcept {return value.default_materials_;}
    static const ReferenceSequence& default_materials(const StaticMeshComponent& value) noexcept {return value.default_materials_;}
    static std::vector<std::string>& default_slots(StaticMeshComponent& value) noexcept {return value.default_slots_;}
    static const std::vector<std::string>& default_slots(const StaticMeshComponent& value) noexcept {return value.default_slots_;}
    static auto& surfaces(StaticMeshComponent& value) noexcept {return value.surfaces_;}
    static auto& surface_geometry(StaticMeshComponent& value) noexcept {return value.surface_geometry_;}
    static auto& surface_parameters(StaticMeshComponent& value) noexcept {return value.surface_parameters_;}
};

inline std::unexpected<Error> error(ErrorCode code, std::string message) { return std::unexpected(Error{code, std::move(message)}); }
// Validators registered with LightSettings and EnvironmentComponent.
Result<void> light_rules(const LightSettings&);
Result<void> environment_rules(const EnvironmentComponent&);
// Checks a native value against its reflected type's hard bounds and validators.
template<class T> Result<void> check_value(Runtime& runtime, const T& value) {
    auto type = runtime.native_type<T>();
    if (!type) return std::unexpected(type.error());
    auto failure = runtime.validate_value(*type, std::as_bytes(std::span{&value, 1}));
    if (!failure) return std::unexpected(failure.error());
    if (*failure) return std::unexpected((*failure)->error);
    return {};
}
// presentation::material_surfaces inside the caller's borrow of the mesh component.
Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> material_surfaces(Runtime&, ObjectId, StaticMeshComponent&);
// One EnvironmentComponent per World, disabled components included.
world::World::Admission unique_environment(world::CapabilityId environments);
// The diagnostic of an extraction item failure, for World::contain.
world::Diagnostic diagnostic(world::DiagnosticKind, world::DiagnosticPhase, ObjectId object, ObjectId entity,
    std::vector<std::string> field = {});

}

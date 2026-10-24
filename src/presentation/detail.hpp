#pragma once

#include "presentation/presentation.hpp"
#include <unordered_map>

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
    static const auto& surfaces(const StaticMeshComponent& value) noexcept {return value.surfaces_;}
    static const auto& surface_geometry(const StaticMeshComponent& value) noexcept {return value.surface_geometry_;}
    static const auto& surface_parameters(const StaticMeshComponent& value) noexcept {return value.surface_parameters_;}
    static String& sky_map(EnvironmentComponent& value) noexcept {return value.sky_map_;}
    static const String& sky_map(const EnvironmentComponent& value) noexcept {return value.sky_map_;}
    static rendering::SkyMapRef& sky_map_resolved(EnvironmentComponent& value) noexcept {return value.sky_map_resolved_;}
    static const rendering::SkyMapRef& sky_map_resolved(const EnvironmentComponent& value) noexcept {return value.sky_map_resolved_;}
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
// presentation::material_surfaces inside the caller's read-only borrow of the mesh component;
// the result equals the component's cache unless something changed. store_surfaces caches it.
Result<std::shared_ptr<const std::vector<material::SurfaceRef>>> material_surfaces(Runtime&, ObjectId, const StaticMeshComponent&);
Result<void> store_surfaces(Runtime&, ObjectId, std::shared_ptr<const std::vector<material::SurfaceRef>>);
// One EnvironmentComponent per World, disabled components included.
world::World::Admission unique_environment(world::CapabilityId environments);
// The diagnostic of an extraction item failure, for World::contain.
world::Diagnostic diagnostic(world::DiagnosticKind, world::DiagnosticPhase, ObjectId object, ObjectId entity,
    std::vector<std::string> field = {});

// The presented poses of one update as rigid offsets of world space.
class Presented {
public:
    struct Offset {
        math::Matrix4 matrix;     // Row-vector world matrix * matrix is the drawn matrix.
        math::Quaternion rotation;
    };
    explicit Presented(std::span<const PresentedPose>);
    [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
    [[nodiscard]] std::span<const ObjectId> entities() const noexcept { return entities_; }
    // The offset of the nearest listed Entity on the spatial chain of `entity`,
    // itself included; null without one or when that Entity is drawn at its World pose.
    [[nodiscard]] Result<const Offset*> find(world::World::SpatialPass&, ObjectId entity) const;
    // As find, along a chain listed from its own Entity up.
    [[nodiscard]] const Offset* find(std::span<const ObjectId> chain) const;
private:
    std::unordered_map<ObjectId, std::optional<Offset>> offsets_;
    std::vector<ObjectId> entities_;
};

}

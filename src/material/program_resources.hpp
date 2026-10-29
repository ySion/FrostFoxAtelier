#pragma once

#include "material/program.hpp"
#include "material/image.hpp"
#include "source/material_program.hpp"

#include <variant>

namespace atelier::material {
namespace detail { struct ProgramAccess; }

enum class Address : std::uint32_t { repeat, clamp, mirror };
enum class Coverage : std::uint32_t { opaque, mask };

struct Policy {
    Coverage coverage{Coverage::opaque};
    bool double_sided{}, emission_two_sided{};
};
enum class TextureColor : std::uint32_t { linear, srgb };

// Author data has no Runtime or compiled slot identity. Roots travel separately
// in State, so the very same sparse values serve documents and pure resolution.
struct TextureBinding {
    std::string asset_key;
    Address address{Address::repeat};
};
struct TextureDefault {
    std::string path;
    TextureBinding binding;
    TextureColor color{TextureColor::linear};
    math::Vector4 empty_value{}; // Already linear; an explicit clear samples this.
};
using LocalValue = std::variant<float, math::Vector2, math::Vector3, math::Vector4, TextureBinding>;
struct Override { std::string path; LocalValue value; bool enabled{true}; };
struct AuthorDiagnostic {
    std::string path, message;
    bool operator==(const AuthorDiagnostic&) const = default;
};

class SourceDefinition final {
public:
    SourceDefinition(const SourceDefinition&) = delete;
    SourceDefinition(SourceDefinition&&) = delete;
    SourceDefinition& operator=(const SourceDefinition&) = delete;
    SourceDefinition& operator=(SourceDefinition&&) = delete;
    const source::MaterialProgram& program() const noexcept { return *program_; }
    const Policy& policy() const noexcept { return policy_; }
    // Normalized to program().textures() order, not the caller's author order.
    std::span<const TextureDefault> textures() const noexcept { return textures_; }
private:
    friend Result<std::shared_ptr<const SourceDefinition>> make_definition(
        std::shared_ptr<const source::MaterialProgram>, Policy, std::span<const TextureDefault>);
    SourceDefinition(std::shared_ptr<const source::MaterialProgram>, Policy, std::vector<TextureDefault>);
    std::shared_ptr<const source::MaterialProgram> program_;
    Policy policy_;
    std::vector<TextureDefault> textures_;
};
using DefinitionRef = std::shared_ptr<const SourceDefinition>;
Result<DefinitionRef> make_definition(std::shared_ptr<const source::MaterialProgram>, Policy,
    std::span<const TextureDefault>);

struct SurfaceInput {
    ImageRef image;
    Address address{Address::repeat};
    TextureColor color{TextureColor::linear};
    math::Vector4 empty_value{};
};
class ProgramSurface final {
public:
    ProgramSurface(const ProgramSurface&) = delete;
    ProgramSurface(ProgramSurface&&) = delete;
    ProgramSurface& operator=(const ProgramSurface&) = delete;
    ProgramSurface& operator=(ProgramSurface&&) = delete;
    const DefinitionRef& definition() const noexcept { return definition_; }
    const source::MaterialProgram& program() const noexcept { return definition_->program(); }
    const Policy& policy() const noexcept { return definition_->policy(); }
    std::span<const std::byte> parameters() const noexcept { return parameters_; }
    // Same order as program().textures(); use that descriptor's slot, not index.
    std::span<const SurfaceInput> inputs() const noexcept { return inputs_; }
    // Diagnostics belong to this exact resolved content, including ancestors.
    // They do not certify strict asset validity or rewrite any author state.
    std::span<const AuthorDiagnostic> diagnostics() const noexcept { return diagnostics_; }
    bool uses_default_material() const noexcept { return default_material_; }
    // Owns code, exact images and a prebuilt synchronous sampler table. No
    // Runtime, ObjectRoot, borrowed author storage or GPU handles survive here.
    Result<Output> evaluate(const Context&) const;
private:
    friend struct detail::ProgramAccess;
    ProgramSurface(DefinitionRef, std::vector<std::byte>, std::vector<SurfaceInput>, std::vector<AuthorDiagnostic>, bool);
    DefinitionRef definition_;
    std::vector<std::byte> parameters_;
    std::vector<SurfaceInput> inputs_;
    std::vector<AuthorDiagnostic> diagnostics_;
    std::vector<source::MaterialTexture> samplers_;
    bool default_material_{};
};
using ProgramSurfaceRef = std::shared_ptr<const ProgramSurface>;

// Cold preparation compiles the engine's opaque magenta MS once, with its own
// temporary compiler Runtime. Subsequent calls share code/content, without roots.
// Failure to prepare the engine default is an error, not a fallback to bad data.
Result<ProgramSurfaceRef> default_program_surface();

// Normally unbound geometry uses this ordinary world-space grid MS, distinct
// from the diagnostic magenta material. The asset catalogue exposes the same
// source and policy; neither compiler nor Renderer has a grid special case.
inline constexpr Policy default_world_policy{Coverage::opaque,true,true};
std::string_view default_world_source() noexcept;
Result<ProgramSurfaceRef> default_world_surface();

// Pure candidate resolution: no live parent lookup or resource directory access.
// Source images match definition.textures(); instance images match author entries
// (numeric entries have null images). Empty key+null image is explicit clear;
// a nonempty key with no image uses inherited input, then the MS's typed empty
// sample. Invalid overrides inherit current values and retain diagnostics.
// Supplied images must be the exact requested content, never substitute pixels.
// A null definition plus an empty image table explicitly requests the engine
// default. The host retains the original compiler/source error separately.
Result<ProgramSurfaceRef> resolve_source(DefinitionRef, std::span<const ImageRef>);
Result<ProgramSurfaceRef> resolve_instance(ProgramSurfaceRef parent, std::span<const Override>,
    std::span<const ImageRef> local_images);

// The nonempty shared role contains the actual resource edges and publication.
// Runtime constructs/destroys the real registered type; no virtual interface.
class Material {
public:
    Material() = default;
    Material(const Material&) = delete;
    Material(Material&&) = delete;
    Material& operator=(const Material&) = delete;
    Material& operator=(Material&&) = delete;
private:
    friend struct detail::ProgramAccess;
    std::string asset_key_;
    ReferenceSequence textures_;
    ReferenceField texture_field_;
    // Identities validated at publication distinguish later destruction from
    // out-of-band edits to the formal reference fields. These are not roots.
    std::vector<ObjectId> texture_ids_;
    std::vector<ImageRef> images_;
    ProgramSurfaceRef published_;
};
class MaterialSource final : public Material {
public:
    MaterialSource() = default;
    MaterialSource(const MaterialSource&) = delete;
    MaterialSource(MaterialSource&&) = delete;
    MaterialSource& operator=(const MaterialSource&) = delete;
    MaterialSource& operator=(MaterialSource&&) = delete;
private:
    friend struct detail::ProgramAccess;
    DefinitionRef definition_;
};
class MaterialInstance final : public Material {
public:
    MaterialInstance() = default;
    MaterialInstance(const MaterialInstance&) = delete;
    MaterialInstance(MaterialInstance&&) = delete;
    MaterialInstance& operator=(const MaterialInstance&) = delete;
    MaterialInstance& operator=(MaterialInstance&&) = delete;
private:
    friend struct detail::ProgramAccess;
    ReferenceSlot parent_;
    ReferenceField parent_field_;
    ObjectId parent_id_;
    std::vector<Override> entries_;
    ProgramSurfaceRef parent_surface_;
};

struct SourceState {
    // Null explicitly publishes the engine default; author source/error stays
    // with the host. Its default_textures table must then be empty.
    DefinitionRef definition;
    // Size and order must match definition->textures()'s normalized order.
    std::vector<ObjectRoot> default_textures;
};
struct InstanceState {
    // Creation requires a live parent. On an initialized instance, explicit
    // absence or its previously validated parent's destruction uses the engine
    // default. A supplied unrelated invalid/foreign identity remains an error.
    ObjectRoot parent;
    std::vector<Override> entries;
    // One slot per entry, in author order (never program slot order), null for
    // numeric/cleared inputs or when no resource root has yet been resolved.
    // Declaration mismatch does not clear an existing root: dormant/deleted-
    // parameter entries retain captured roots as formal strong edges unchanged.
    std::vector<ObjectRoot> textures;
};
Result<void> register_resource_types(Runtime&);
Result<ObjectRoot> create_source(Runtime&, std::string_view asset_key, const SourceState&);
Result<ObjectRoot> create_instance(Runtime&, std::string_view asset_key, const InstanceState&);
Result<std::string> material_key(Runtime&, ObjectId);
Result<SourceState> read_source(Runtime&, ObjectId);
Result<InstanceState> read_instance(Runtime&, ObjectId);
// Last successful publication's diagnostics, for either MS or MI. Frozen hosts
// consume ProgramSurface::diagnostics(), not this possibly old cache.
Result<std::vector<AuthorDiagnostic>> read_diagnostics(Runtime&, ObjectId);
// Synchronous single-object publication, not adoption by a host's visible group.
// Failure retains this object's author data, edges and Surface. Ancestor caches
// and Runtime write stamps are not rolled back. Reads never resolve/write back.
Result<void> update_source(Runtime&, ObjectId, const SourceState&);
Result<void> update_instance(Runtime&, ObjectId, const InstanceState&);
// Convenience live capture for single-object consumers. Hosts resolving a frozen
// group instead use resolve_source/resolve_instance with explicit parent content.
// Unchanged dependencies return the same owner using only const borrows. Known
// texture/parent loss refreshes content and diagnostics; structural or Runtime
// failures return errors, never the old owner disguised as a newly valid result.
Result<ProgramSurfaceRef> capture_program(Runtime&, ObjectId);

} // namespace atelier::material

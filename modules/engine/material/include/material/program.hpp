#pragma once

#include "math/math.hpp"
#include "runtime/runtime.hpp"

#include <cstdint>
#include <type_traits>

namespace atelier::material {

inline constexpr float emission_limit = 65504.f; // Linear emission captured in the FP16 GBuffer.

// Opaque, copyable computation token, not a texture object or author binding.
// The registered uint32 "slot" field supplies its real CPU offset to layout
// capture. The compiler injects program-local slots using that metadata, not
// an assumed offset or HLSL packing. This field is compiler layout metadata:
// material AST validation must reject authored construction, member reads or
// writes, and integer conversion. Copying/passing an injected token is allowed.
// Native default construction supplies storage only, not an author default.
// No Runtime identity, resource owner or device handle lives in this token.
class TextureInput final {
public:
    TextureInput() = default;

private:
    friend Result<void> register_program_types(Runtime&);

    std::uint32_t slot_;
};

// Values at the authored surface, before viewer-facing normal flips. Positions
// use the engine's left-handed world space (+Y up, meters); UV is the mesh chart.
// Filtering belongs to the evaluator's sample environment, not this value.
struct Context {
    math::Vector2 uv{};

    math::Vector3 position_world{};

    math::Vector3 normal_world{0, 0, 1};

    math::Vector3 geometric_normal_world{0, 0, 1};

    // xyz is the world-transformed mesh tangent; w is its signed handedness,
    // including reflected instances. Zero xyz means no usable tangent chart.
    // The normal-map helper orthogonalizes it and follows the input's UV chart.
    math::Vector4 tangent_world{0, 0, 0, 1};
};

// Fixed engine shading outputs, not a material's author parameter schema.
// RGB is linear; emission is HDR radiance before exposure. The engine still
// owns the BRDF, geometric normal, sidedness and face-forward operation.
struct Output {
    math::Vector3 base_color{1, 1, 1};

    math::Vector3 emissive{};

    float roughness{.65f};

    float metallic{};

    // Signed margin: Masked retains >= 0; Opaque ignores it. StandardPBR uses
    // alpha - cutoff, preserving equality at the authored cutoff boundary.
    float coverage{1};

    // World-space shading normal, before face-forward; never replaces the
    // geometric normal used for visibility, ray offsets or surface closure.
    math::Vector3 normal_world{0, 0, 1};
};

static_assert(std::is_standard_layout_v<TextureInput> && std::is_trivially_copyable_v<TextureInput>);

static_assert(std::is_trivially_default_constructible_v<TextureInput>);

static_assert(sizeof(TextureInput) == sizeof(std::uint32_t) && alignof(TextureInput) == alignof(std::uint32_t));

static_assert(std::is_standard_layout_v<Context> && std::is_trivially_copyable_v<Context>);

static_assert(std::is_standard_layout_v<Output> && std::is_trivially_copyable_v<Output>);

// Register material.TextureInput / Context / Output in the compiler's existing
// type directory. No business objects, roots or evaluator state are created.
// native_type<TextureInput/Context/Output>() supplies the actual input TypeIds.
// Captured field offsets describe CPU storage only, not HLSL packing. Neither
// the compiled program nor a Surface may retain this Runtime or a borrowed value.
[[nodiscard]] Result<void> register_program_types(Runtime&);

} // namespace atelier::material

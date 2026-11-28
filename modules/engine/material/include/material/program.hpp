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

// Dedicated computation token for tangent-space normal maps (BC5_RG_UNORM).
// Statically distinct from TextureInput: rejected by sample(...) and accepted
// exclusively by sample_normal(input, uv) -> vector3 (and sample_orientation).
class NormalTextureInput final {
public:
    NormalTextureInput() = default;

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

// OpenPBR v1.1 fixed engine surface shading outputs, not a material's author parameter schema.
// RGB is linear; emission is HDR radiance before exposure. The engine owns the canonical
// 24B/px slab encoding, BRDF lobes, geometric normal, sidedness and face-forward operation.
struct Output {
    math::Vector3 base_color{1, 1, 1};

    math::Vector3 emissive{};

    float roughness{.65f};

    float metallic{};

    // Signed margin: Masked retains >= 0; Opaque ignores it. StandardPBR/OpenPBR use
    // alpha - cutoff, preserving equality at the authored cutoff boundary.
    float coverage{1};

    // World-space shading normal, before face-forward; never replaces the
    // geometric normal used for visibility, ray offsets or surface closure.
    math::Vector3 normal_world{0, 0, 1};

    // OpenPBR v1.1 base & specular lobe extensions (neutral defaults preserve exact StandardPBR behavior):
    float diffuse_roughness{0.f}; // EON energy-preserving Oren-Nayar diffuse roughness [0..1]

    float specular_weight{1.f}; // Dielectric specular F0 scale [0..1]

    float specular_ior{1.5f}; // Dielectric index of refraction [1..3] (1.5 -> F0 = 0.04)

    float micro_ao{1.f}; // Material micro-cavity occlusion [0..1] for Chan 2018 Aperture Micro-Shadows

    math::Vector3 specular_color{1, 1, 1}; // Conductor F82 edge tint / dielectric specular tint

    // Profile 1: Dielectric Clearcoat
    float coat_weight{0.f};

    float coat_roughness{.1f};

    float coat_darkening{1.f};

    // Profile 2: Charlie Fuzz / Sheen / Velvet
    float fuzz_weight{0.f};

    float fuzz_roughness{.5f};

    math::Vector3 fuzz_color{1, 1, 1};

    // Profile 3: Thin-Walled Foliage Diffuse Transmission
    float transmission_scatter{0.f};

    math::Vector3 transmission_color{1, 1, 1};

    // Profile 4: Belcour & Barla 2017 Thin-Film Iridescence
    float thin_film_weight{0.f};

    float thin_film_thickness{.5f}; // Normalized [0..1] -> [100nm..1000nm]

    float thin_film_ior{1.33f};

    // Profile 5: Separable Screen-Space Subsurface Scattering
    float subsurface_weight{0.f};

    float subsurface_radius{0.f}; // Scatter radius in meters [0..0.05]

    math::Vector3 subsurface_color{1, .35f, .2f};
};

static_assert(std::is_standard_layout_v<TextureInput> && std::is_trivially_copyable_v<TextureInput>);

static_assert(std::is_trivially_default_constructible_v<TextureInput>);

static_assert(sizeof(TextureInput) == sizeof(std::uint32_t) && alignof(TextureInput) == alignof(std::uint32_t));

static_assert(std::is_standard_layout_v<NormalTextureInput> && std::is_trivially_copyable_v<NormalTextureInput>);

static_assert(std::is_trivially_default_constructible_v<NormalTextureInput>);

static_assert(sizeof(NormalTextureInput) == sizeof(std::uint32_t) &&
              alignof(NormalTextureInput) == alignof(std::uint32_t));

static_assert(std::is_standard_layout_v<Context> && std::is_trivially_copyable_v<Context>);

static_assert(std::is_standard_layout_v<Output> && std::is_trivially_copyable_v<Output>);

// Register material.TextureInput / Context / Output in the compiler's existing
// type directory. No business objects, roots or evaluator state are created.
// native_type<TextureInput/Context/Output>() supplies the actual input TypeIds.
// Captured field offsets describe CPU storage only, not HLSL packing. Neither
// the compiled program nor a Surface may retain this Runtime or a borrowed value.
[[nodiscard]] Result<void> register_program_types(Runtime&);

} // namespace atelier::material

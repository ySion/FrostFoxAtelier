#pragma once

#include "runtime/runtime.hpp"
#include "math/math.hpp"

namespace atelier::source {

struct MaterialLayout {
    std::size_t size{};

    std::size_t alignment{};
};

struct MaterialParameter {
    // Persistent B0 key is the declared field path, never a Runtime FieldId or
    // GPU offset. Changing the path is an explicit parameter-interface change.
    String key;

    std::uint32_t components{1}; // float / vector2 / vector3 / vector4

    std::size_t offset{}; // Captured CPU/default byte layout.
};

struct MaterialTextureInput {
    String key; // Declared field path, sharing the numeric parameter key space.

    std::uint32_t slot; // Local to this program version; never persisted as identity.

    bool is_normal_map{}; // Statically true iff declared with material.NormalTextureInput.
};

struct MaterialTexture {
    std::uint32_t slot; // Program-local binding obtained from textures().

    const void* data;

    // Synchronous, read-only LOD-0 sample. RGB is decoded to linear when the
    // binding is a color texture; alpha/data are linear. No derivatives.
    // Both callbacks report recoverable failures through Result, never exceptions.
    Result<math::Vector4> (*sample)(const void*, math::Vector2) noexcept;

    // Same actual binding/addressing, including empty images. Repeat/Clamp
    // return (1,1); Mirror folds u -= floor(u*.5)*2 and returns u>1 ? -1 : +1
    // per axis (fold points choose +1). Required only when executed; a missing
    // callback fails the invocation atomically, never assumes Repeat.
    Result<math::Vector2> (*orientation)(const void*, math::Vector2) noexcept {};
};

// Immutable owning content, independent of the compilation Runtime. Evaluation
// can run concurrently with independent inputs/samplers. The backend DLL and
// sampler implementation must remain loaded; a call borrows inputs only until
// it returns. Holding this owner keeps its old code alive across recompilation.
class MaterialProgram final {
    struct State;

    std::shared_ptr<const State> state_;

    explicit MaterialProgram(std::shared_ptr<const State>);

    friend Result<std::shared_ptr<const MaterialProgram>> compile_material(Runtime&, TypeId, TypeId, TypeId,
                                                                           StringView, StringView, TypeId) noexcept;

public:
    MaterialLayout context_layout() const noexcept;

    MaterialLayout output_layout() const noexcept;

    MaterialLayout parameter_layout() const noexcept;

    Span<const MaterialParameter> parameters() const noexcept;

    Span<const std::byte> default_parameters() const noexcept;

    // All declared inputs require bindings, including currently unsampled ones.
    Span<const MaterialTextureInput> textures() const noexcept;

    // Emits an ASCII-identifier namespace, with ms_Context, ms_Output,
    // ms_Parameters, ms_shade(uint environment,ms_Context,ms_Parameters), and
    // ms_load_parameters(ByteAddressBuffer,uint byte_base). Field names have
    // an ms_ prefix. The loader reads captured CPU offsets, NOT HLSL packing.
    // Compose programs with distinct namespaces. Consumer defines the global
    // float4 atelier_material_sample(uint environment,uint local_slot,float2 uv).
    // Programs using sample_orientation additionally require the global
    // float2 atelier_material_sample_orientation(uint environment,uint local_slot,float2 uv).
    // Each call's environment selects its resource binding base, not global state.
    Result<String> hlsl(StringView name_space) const;

    // Same Context/Parameters and loader, with float ms_coverage(uint environment,
    // ms_Context,ms_Parameters). Emits only the data/control dependencies of the
    // top-level float Output.coverage, including defaults, overwrites and helper
    // returns; unrelated shading/samples are removed before HLSL compilation.
    // Uses the point sampling/orientation bridges above, never SampleGrad.
    // Returns invalid_type for a missing float coverage field or capacity_exceeded
    // when projection exceeds the node budget. Both failures are local to this variant.
    Result<String> hlsl_coverage(StringView name_space) const;

    // Pruned GI hit shading slice: emits ms_GiHit ms_shade_gi_hit(uint,ms_Context,ms_Parameters)
    // with normal_world / normal_texture / TBN removed and OpenPBR §3.10 directional-hemispherical
    // effective albedo + foliage transmission folded (automatically falls back to the un-inlined
    // call graph with the same folding when inline projection exceeds the node budget).
    Result<String> hlsl_gi_hit(StringView name_space) const;

    // Same plain Context/Params/Output and parameter loader, but the entry is
    // ms_shade_footprint(uint,ms_Context center,ms_Context d0,ms_Context d1,ms_Parameters).
    // d0/d1 are zero-initialized changes, NOT neighboring/default Contexts;
    // Params are uniform (zero changes). Branches follow the center only.
    // Consumer defines float4 atelier_material_sample_grad(uint environment,
    // uint local_slot,float2 uv,float2 d0,float2 d1) (and may optionally define
    // ATELIER_MATERIAL_SAMPLE_GRAD_OFFSET_DEFINED + atelier_material_sample_grad_offset
    // for side-effect-free offset samples on chained-UV materials). For a finite UV with any
    // nonfinite gradient, the bridge MUST sample its actual coarsest mip without
    // passing bad gradients to hardware. Texel-scale overflow is also handled
    // by the bridge. The generated code uses the same gradients for up to two
    // offset samples to approximate texture-driven changes; no implicit ddx/ddy.
    // Unavailable changes propagate per component, never via a whole-Context flag.
    // sample_orientation uses the same global bridge as point, evaluated at the
    // center; its two changes are zero (piecewise-constant interval selection).
    Result<String> hlsl_footprint(StringView name_space) const;

    // All required slots must be bound, even if a branch skips sampling. Finite
    // inputs/sample results/outputs only; missing bindings and failures leave
    // output unchanged. Byte spans have the captured sizes, may be unaligned.
    // CPU/HLSL use float math; no bit-identity promise near rounding boundaries.
    Result<void> evaluate(Span<const std::byte> context, Span<const std::byte> parameters, Span<std::byte> output,
                          Span<const MaterialTexture> textures = {}) const noexcept;
};

// Runtime supplies the actual registered Context/Output and value layouts only
// during this synchronous call, including TextureInput's registered uint slot
// field and its real offset. Source declares its own numeric Params/defaults()
// and Output shade(Context, Params[, Resources]); Resources has TextureInput
// leaves. No source type is published
// into Runtime, and no Runtime/root/type-resolution callback is retained.
// Supports float/vectors/plain structs, local bool/conditions, acyclic pure
// functions, sample(typed_input, uv) and sample_orientation(typed_input, uv);
// no numeric slots, loops, objects, async or host calls. Resource defaults,
// sampling and empty values belong to MS author data.
Result<std::shared_ptr<const MaterialProgram>> compile_material(Runtime&, TypeId context, TypeId output,
                                                                TypeId texture_input, StringView source_name,
                                                                StringView source,
                                                                TypeId normal_texture_input = {}) noexcept;

}

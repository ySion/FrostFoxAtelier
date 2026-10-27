#include "material/program.hpp"
#include "runtime/native.hpp"

#include <cstddef>

namespace atelier::material {

Result<void> register_program_types(Runtime& runtime) {
    if (runtime.native_type<Context>() && runtime.native_type<Output>() && runtime.native_type<TextureInput>()) return {};
    auto context = NativeRegistration<Context>{runtime, "material.Context", TypeKind::record}
        .field<&Context::uv>("uv", {TypeReference::native<math::Vector2>()}, offsetof(Context, uv))
        .field<&Context::position_world>("position_world", {TypeReference::native<math::Vector3>()}, offsetof(Context, position_world))
        .field<&Context::normal_world>("normal_world", {TypeReference::native<math::Vector3>()}, offsetof(Context, normal_world))
        .field<&Context::geometric_normal_world>("geometric_normal_world", {TypeReference::native<math::Vector3>()}, offsetof(Context, geometric_normal_world))
        .field<&Context::tangent_world>("tangent_world", {TypeReference::native<math::Vector4>()}, offsetof(Context, tangent_world))
        .build();
    auto output = NativeRegistration<Output>{runtime, "material.Output", TypeKind::record}
        .field<&Output::base_color>("base_color", {TypeReference::native<math::Vector3>()}, offsetof(Output, base_color))
        .field<&Output::emissive>("emissive", {TypeReference::native<math::Vector3>()}, offsetof(Output, emissive))
        .field<&Output::roughness>("roughness", {TypeReference::native<float>()}, offsetof(Output, roughness))
        .field<&Output::metallic>("metallic", {TypeReference::native<float>()}, offsetof(Output, metallic))
        .field<&Output::coverage>("coverage", {TypeReference::native<float>()}, offsetof(Output, coverage))
        .field<&Output::normal_world>("normal_world", {TypeReference::native<math::Vector3>()}, offsetof(Output, normal_world))
        .build();
    auto texture_input = NativeRegistration<TextureInput>{runtime, "material.TextureInput", TypeKind::record}
        .field<&TextureInput::slot_>("slot", {TypeReference::native<std::uint32_t>()}, offsetof(TextureInput, slot_))
        .build();
    auto published = runtime.register_types({std::move(context), std::move(output), std::move(texture_input)});
    if (!published) return std::unexpected(published.error());
    return {};
}

} // namespace atelier::material

#include "material/program.hpp"
#include "runtime/native.hpp"

#include <cstddef>

namespace atelier::material {

Result<void> register_program_types(Runtime& runtime) {
    if (runtime.native_type<Context>() && runtime.native_type<Output>() && runtime.native_type<TextureInput>() &&
        runtime.native_type<NormalTextureInput>())
        return {};
    auto context =
        NativeRegistration<Context>{runtime, "material.Context", TypeKind::record}
            .field<&Context::uv>("uv", {TypeReference::native<math::Vector2>()}, offsetof(Context, uv))
            .field<&Context::position_world>("position_world", {TypeReference::native<math::Vector3>()},
                                             offsetof(Context, position_world))
            .field<&Context::normal_world>("normal_world", {TypeReference::native<math::Vector3>()},
                                           offsetof(Context, normal_world))
            .field<&Context::geometric_normal_world>("geometric_normal_world", {TypeReference::native<math::Vector3>()},
                                                     offsetof(Context, geometric_normal_world))
            .field<&Context::tangent_world>("tangent_world", {TypeReference::native<math::Vector4>()},
                                            offsetof(Context, tangent_world))
            .build();
    auto output =
        NativeRegistration<Output>{runtime, "material.Output", TypeKind::record}
            .field<&Output::base_color>("base_color", {TypeReference::native<math::Vector3>()},
                                        offsetof(Output, base_color))
            .field<&Output::emissive>("emissive", {TypeReference::native<math::Vector3>()}, offsetof(Output, emissive))
            .field<&Output::roughness>("roughness", {TypeReference::native<float>()}, offsetof(Output, roughness))
            .field<&Output::metallic>("metallic", {TypeReference::native<float>()}, offsetof(Output, metallic))
            .field<&Output::coverage>("coverage", {TypeReference::native<float>()}, offsetof(Output, coverage))
            .field<&Output::normal_world>("normal_world", {TypeReference::native<math::Vector3>()},
                                          offsetof(Output, normal_world))
            .field<&Output::diffuse_roughness>("diffuse_roughness", {TypeReference::native<float>()},
                                               offsetof(Output, diffuse_roughness))
            .field<&Output::specular_weight>("specular_weight", {TypeReference::native<float>()},
                                             offsetof(Output, specular_weight))
            .field<&Output::specular_ior>("specular_ior", {TypeReference::native<float>()},
                                          offsetof(Output, specular_ior))
            .field<&Output::micro_ao>("micro_ao", {TypeReference::native<float>()}, offsetof(Output, micro_ao))
            .field<&Output::specular_color>("specular_color", {TypeReference::native<math::Vector3>()},
                                            offsetof(Output, specular_color))
            .field<&Output::coat_weight>("coat_weight", {TypeReference::native<float>()}, offsetof(Output, coat_weight))
            .field<&Output::coat_roughness>("coat_roughness", {TypeReference::native<float>()},
                                            offsetof(Output, coat_roughness))
            .field<&Output::coat_darkening>("coat_darkening", {TypeReference::native<float>()},
                                            offsetof(Output, coat_darkening))
            .field<&Output::fuzz_weight>("fuzz_weight", {TypeReference::native<float>()}, offsetof(Output, fuzz_weight))
            .field<&Output::fuzz_roughness>("fuzz_roughness", {TypeReference::native<float>()},
                                            offsetof(Output, fuzz_roughness))
            .field<&Output::fuzz_color>("fuzz_color", {TypeReference::native<math::Vector3>()},
                                        offsetof(Output, fuzz_color))
            .field<&Output::transmission_scatter>("transmission_scatter", {TypeReference::native<float>()},
                                                  offsetof(Output, transmission_scatter))
            .field<&Output::transmission_color>("transmission_color", {TypeReference::native<math::Vector3>()},
                                                offsetof(Output, transmission_color))
            .field<&Output::thin_film_weight>("thin_film_weight", {TypeReference::native<float>()},
                                              offsetof(Output, thin_film_weight))
            .field<&Output::thin_film_thickness>("thin_film_thickness", {TypeReference::native<float>()},
                                                 offsetof(Output, thin_film_thickness))
            .field<&Output::thin_film_ior>("thin_film_ior", {TypeReference::native<float>()},
                                           offsetof(Output, thin_film_ior))
            .field<&Output::subsurface_weight>("subsurface_weight", {TypeReference::native<float>()},
                                               offsetof(Output, subsurface_weight))
            .field<&Output::subsurface_radius>("subsurface_radius", {TypeReference::native<float>()},
                                               offsetof(Output, subsurface_radius))
            .field<&Output::subsurface_color>("subsurface_color", {TypeReference::native<math::Vector3>()},
                                              offsetof(Output, subsurface_color))
            .build();
    auto texture_input = NativeRegistration<TextureInput>{runtime, "material.TextureInput", TypeKind::record}
                             .field<&TextureInput::slot_>("slot", {TypeReference::native<std::uint32_t>()},
                                                          offsetof(TextureInput, slot_))
                             .build();
    auto normal_texture_input =
        NativeRegistration<NormalTextureInput>{runtime, "material.NormalTextureInput", TypeKind::record}
            .field<&NormalTextureInput::slot_>("slot", {TypeReference::native<std::uint32_t>()},
                                               offsetof(NormalTextureInput, slot_))
            .build();
    Array<TypeDefinition> regs;
    if (!runtime.native_type<Context>())
        regs.add(std::move(context));
    if (!runtime.native_type<Output>())
        regs.add(std::move(output));
    if (!runtime.native_type<TextureInput>())
        regs.add(std::move(texture_input));
    if (!runtime.native_type<NormalTextureInput>())
        regs.add(std::move(normal_texture_input));
    if (!regs.empty())
        ATELIER_TRY(runtime.register_types(std::move(regs)));
    return {};
}

} // namespace atelier::material

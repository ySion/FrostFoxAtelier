#pragma once
#include "material/surface.hpp"
#include "runtime/reflection.hpp"
#include "foundation/string.hpp"
#include "math/math.hpp"
#include "material/resources.gen.hpp"

namespace atelier::material {
namespace detail { struct Access; }
AENUM(Name="material.NormalY")
enum class NormalY : std::uint32_t {positive,negative};

ASTRUCT(Name="material.Parameters")
struct ParameterValues {
    AGENERATED_BODY(ParameterValues)
    AFIELD() math::Vector3 base_color{1,1,1};
    AFIELD() float opacity{1};
    AFIELD() float roughness{.65f};
    AFIELD() float metallic{};
    AFIELD() math::Vector3 emissive{};
    AFIELD() float normal_scale{1};
    AFIELD() NormalY normal_y_sign{NormalY::positive};
    AFIELD() bool mask{};
    AFIELD() float alpha_cutoff{.5f};
    AFIELD() bool double_sided{true};
    AFIELD() bool emission_two_sided{true};
};
AENUM(Name="material.Address")
enum class SamplingAddress : std::uint32_t {repeat,clamp,mirror};
AENUM(Name="material.Channel")
enum class SamplingChannel : std::uint32_t {red,green,blue,alpha};
ASTRUCT(Name="material.Sampling")
struct SamplingValues {
    AGENERATED_BODY(SamplingValues)
    AFIELD() math::Vector2 scale{1,1};
    AFIELD() math::Vector2 offset{};
    AFIELD() float rotation{};
    AFIELD() SamplingAddress address{SamplingAddress::repeat};
    AFIELD() SamplingChannel channel{SamplingChannel::red};
};

ACLASS(Name="material.Texture")
class TextureResource {
    AGENERATED_BODY(TextureResource)
private:
    friend struct detail::Access;
    String source_;
    ImageRef image_;
};

ACLASS(Name="material.Material")
class MaterialResource {
    AGENERATED_BODY(MaterialResource)
public:
    AFIELD() ParameterValues parameters;
    AFIELD() SamplingValues base_color_texture;
    AFIELD() SamplingValues roughness_texture;
    AFIELD() SamplingValues metallic_texture;
    AFIELD() SamplingValues normal_texture;
    AFIELD() SamplingValues emissive_texture;
    AFIELD() SamplingValues opacity_texture;
private:
    friend struct detail::Access;
    String source_;
    AFIELD(Name="textures", Hidden, Sequence="material.Texture")
    ReferenceSequence textures_;
    AFIELD(Name="parent", Hidden, Strong="material.Material")
    ReferenceSlot parent_;
    std::uint32_t overrides_{};
    Description description_;
    SurfaceRef published_;
};

// Persistent source keys identify project assets. ObjectRoot/ReferenceSequence
// provide all business ownership inside the caller's existing Runtime.
struct TextureState { std::string source; ImageRef image; };
struct State {
    std::string source;
    Description description;
    std::array<ObjectRoot,input_count> textures;
    ObjectRoot parent;
    std::uint32_t overrides{};
};
inline constexpr std::array parameter_names{"base_color","opacity","roughness","metallic","emissive",
    "normal_scale","normal_y_sign","mask","alpha_cutoff","double_sided","emission_two_sided"};
inline constexpr std::uint32_t parameter_mask=(1u<<parameter_names.size())-1;
Result<void> register_types(Runtime&);
Result<ObjectRoot> create_texture(Runtime&,std::string_view source,ImageRef);
Result<TextureState> read_texture(Runtime&,ObjectId);
Result<ObjectRoot> create_material(Runtime&,const State&);
Result<ObjectRoot> create_instance(Runtime&,const ObjectRoot& parent);
Result<State> read_material(Runtime&,ObjectId);
// Validate and prepare the whole replacement before publishing. Identity does
// not change. A published capture keeps pixels alive after its Runtime closes.
Result<void> update_material(Runtime&,ObjectId,const State&);
Result<void> write_property(Runtime&,ObjectId,std::span<const FieldId>,Scalar);
Result<std::uint32_t> override_bit(Runtime&,std::span<const FieldId>);
Result<void> set_override(Runtime&,ObjectId,std::span<const FieldId>,bool);
Result<SurfaceRef> capture(Runtime&,ObjectId);
}

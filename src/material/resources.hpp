#pragma once
#include "material/surface.hpp"
#include "runtime/reflection.hpp"
#include "foundation/string.hpp"
#include "math/math.hpp"
#include "material/resources.gen.hpp"

namespace atelier::material {
namespace detail { struct Access; }
AENUM(Name="material.NormalY", DESC="Normal Map 的 Y 通道方向。切换 Positive / Negative 可适配法线贴图的绿色通道约定。")
enum class NormalY : std::uint32_t {positive,negative};

ASTRUCT(Name="material.Parameters", DESC="标准表面的颜色、粗糙度、自发光和覆盖规则。Material 实例只覆盖明确启用的参数组，其余继续继承共享材质。")
struct ParameterValues {
    AGENERATED_BODY(ParameterValues)
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="线性 RGB 基础色因子，每通道 0–1，与 Base Color Texture 的 sRGB 颜色解码结果相乘。") math::Vector3 base_color{1,1,1};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="Alpha 因子，范围 0–1。启用 Mask 时与颜色贴图 Alpha、Opacity Texture 相乘后决定覆盖；当前不产生半透明混合。") float opacity{1};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="Roughness 因子，范围 0–1，与粗糙度贴图指定通道相乘。值越小，反射越集中。") float roughness{.65f};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="Metallic 因子，范围 0–1，与金属度贴图指定通道相乘。0 为非金属，1 为金属。") float metallic{};
    AFIELD(Min="0", Max="10", LockMin, HardMax="65504", DESC="线性 HDR RGB 自发光因子，与 Emissive Texture 的颜色解码结果相乘。它是表面辐射，不是曝光后的屏幕亮度。") math::Vector3 emissive{};
    AFIELD(Min="0", Max="16", LockMin, LockMax, DESC="切线空间 Normal Map 的强度，范围 0–16。0 去掉法线贴图造成的表面起伏；不改变网格几何。") float normal_scale{1};
    AFIELD(DESC="法线贴图绿色通道的方向约定。若凹凸朝向相反，可切换 Positive / Negative；不改变纹理原文件。") NormalY normal_y_sign{NormalY::positive};
    AFIELD(DESC="启用 Alpha 裁切。最终 Alpha 低于 Alpha Cutoff 的位置不覆盖表面；关闭时按 Opaque 处理，不使用 Alpha 裁切。") bool mask{};
    AFIELD(Min="0", Max="1", LockMin, LockMax, DESC="Mask 的覆盖阈值，范围 0–1。最终 Alpha 等于或高于此值时保留表面，否则形成孔洞。") float alpha_cutoff{.5f};
    AFIELD(DESC="是否显示三角形背面。它控制双面显示，双面发光由 Emission Two Sided 单独决定。") bool double_sided{true};
    AFIELD(DESC="是否让表面的两面都发出 Emissive 辐射。关闭后按三角形的作者朝向单面发光。") bool emission_two_sided{true};
};
AENUM(Name="material.Address", DESC="UV 超出 0–1 时的寻址方式：Repeat 重复，Clamp 贴住边缘，Mirror 镜像重复。")
enum class SamplingAddress : std::uint32_t {repeat,clamp,mirror};
AENUM(Name="material.Channel", DESC="标量贴图采样的 R、G、B 或 A 通道。颜色和 Normal Map 使用 RGB，不使用这个通道选择。")
enum class SamplingChannel : std::uint32_t {red,green,blue,alpha};
ASTRUCT(Name="material.Sampling", DESC="一个贴图输入的 UV 变换、寻址和通道选择。每个输入独立设置，不改变共享图片的像素。")
struct SamplingValues {
    AGENERATED_BODY(SamplingValues)
    AFIELD(DESC="UV 的 U、V 缩放比例，无单位。先缩放再旋转；配合 Repeat 可控制贴图重复次数。") math::Vector2 scale{1,1};
    AFIELD(DESC="UV 的 U、V 偏移量。它在缩放和旋转之后应用；偏移 1 表示一个完整纹理坐标周期。") math::Vector2 offset{};
    AFIELD(DESC="绕 UV 原点旋转的角度，单位 rad。它在 Scale 之后、Offset 之前应用。") float rotation{};
    AFIELD(DESC="UV 超出 0–1 时使用 Repeat、Clamp 或 Mirror 寻址。") SamplingAddress address{SamplingAddress::repeat};
    AFIELD(DESC="Roughness、Metallic、Opacity 等标量输入读取的图像通道。颜色和法线输入始终使用 RGB。") SamplingChannel channel{SamplingChannel::red};
};

ACLASS(Name="material.Texture", DESC="共享的图片资源。颜色空间由使用它的材质输入决定：颜色图按 sRGB 解码，法线和标量图按线性数据读取。")
class TextureResource {
    AGENERATED_BODY(TextureResource)
private:
    friend struct detail::Access;
    String source_;
    ImageRef image_;
};

ACLASS(Name="material.Material", DESC="可由多个网格槽共享的标准材质，包含参数和贴图输入。实例通过明确的参数覆盖继承共享材质。")
class MaterialResource {
    AGENERATED_BODY(MaterialResource)
public:
    AFIELD(DESC="表面颜色、Roughness、Metallic、Emissive 和 Alpha 覆盖规则。修改共享材质会影响使用它且未覆盖对应参数的实例。") ParameterValues parameters;
    AFIELD(DESC="Base Color Texture 的 UV 和寻址设置。RGB 按 sRGB 解码，Alpha 参与 Mask 覆盖。") SamplingValues base_color_texture;
    AFIELD(DESC="Roughness Texture 的 UV、寻址和通道选择。指定通道按线性数据读取，并乘以 Roughness 因子。") SamplingValues roughness_texture;
    AFIELD(DESC="Metallic Texture 的 UV、寻址和通道选择。可与粗糙度输入使用同一张图的不同通道。") SamplingValues metallic_texture;
    AFIELD(DESC="切线空间 Normal Map 的 UV 和寻址设置。RGB 按线性数据读取，强度和 Y 方向由 Parameters 控制。") SamplingValues normal_texture;
    AFIELD(DESC="Emissive Texture 的 UV 和寻址设置。颜色按 sRGB 解码后乘以 HDR 自发光因子。") SamplingValues emissive_texture;
    AFIELD(DESC="Opacity Texture 的 UV、寻址和通道选择。指定通道参与 Mask 的最终 Alpha；当前不支持半透明混合。") SamplingValues opacity_texture;
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
// A null image stands in for a missing source file; the input samples as absent.
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

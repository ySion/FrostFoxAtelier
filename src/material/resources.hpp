#pragma once
#include "material/image.hpp"
#include "runtime/reflection.hpp"
#include "foundation/string.hpp"
#include "material/resources.gen.hpp"

namespace atelier::material {
namespace detail {
struct Access;
struct ProgramAccess;
}

ACLASS(Name = "material.Texture",
       DESC = "共享的图片资源。颜色空间由使用它的材质输入决定：颜色图按 sRGB 解码，法线和标量图按线性数据读取。")
class TextureResource {
    AGENERATED_BODY(TextureResource)
public:
    TextureResource() = default;
    TextureResource(const TextureResource&) = delete;
    TextureResource(TextureResource&&) = delete;
    TextureResource& operator=(const TextureResource&) = delete;
    TextureResource& operator=(TextureResource&&) = delete;

private:
    friend struct detail::Access;
    friend struct detail::ProgramAccess;
    String source_;
    ImageRef image_;
};

struct TextureState {
    std::string source;
    ImageRef image;
};
Result<void> register_texture_types(Runtime&);
// A null image retains the missing asset key; each MS input defines its fallback.
Result<ObjectRoot> create_texture(Runtime&, std::string_view source, ImageRef);
Result<TextureState> read_texture(Runtime&, ObjectId);
// Replace current content, not resource identity. Existing captures keep their
// immutable pixels until their consumers retire; parameter history owns no pixels.
Result<void> update_texture(Runtime&, ObjectId, ImageRef);
}

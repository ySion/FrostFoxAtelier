#include "material/resources.hpp"

namespace atelier::material {
namespace detail {
struct Access {
    static String& source(TextureResource& value) { return value.source_; }

    static ImageRef& image(TextureResource& value) { return value.image_; }

    static const String& source(const TextureResource& value) { return value.source_; }

    static const ImageRef& image(const TextureResource& value) { return value.image_; }
};
}

namespace {
using detail::Access;

auto failure(Utf8Text message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}
}

Result<void> register_texture_types(Runtime& runtime) {
    if (runtime.native_type<TextureResource>())
        return {};
    ATELIER_TRY(runtime.register_types({reflection::Definition<TextureResource>::build(runtime)}));
    return {};
}

Result<ObjectRoot> create_texture(Runtime& runtime, StringView source, ImageRef image) {
    ATELIER_TRY(runtime.check_code_publication());
    if (source.size() > 4096 || source.contains('\0'))
        return failure("Invalid texture resource source.");
    if (!image && source.empty())
        return failure("A texture without image content requires the persistent source it stands in for.");
    auto text = String::from_utf8(source);
    if (!text)
        return failure("Texture resource source requires valid UTF-8.");
    ATELIER_TRY_BIND(type, runtime.native_type<TextureResource>());
    ATELIER_TRY_BIND(root, runtime.create(*type));
    auto written = runtime.with_native<TextureResource>(root->id(), [&](auto& value) -> Result<void> {
        Access::source(value) = std::move(*text);
        Access::image(value) = std::move(image);
        return {};
    });
    if (!written)
        return std::unexpected(written.error());
    return std::move(*root);
}

Result<TextureState> read_texture(Runtime& runtime, ObjectId id) {
    return runtime.with_native<const TextureResource>(id, [](const auto& value) -> Result<TextureState> {
        return TextureState{Access::source(value), Access::image(value)};
    });
}

Result<void> update_texture(Runtime& runtime, ObjectId id, ImageRef image) {
    ATELIER_TRY(runtime.check_code_publication());
    return runtime.with_native<TextureResource>(id, [&](auto& value) -> Result<void> {
        if (!image && Access::source(value).empty())
            return failure("A missing texture needs its persistent source key.");
        Access::image(value) = std::move(image);
        return {};
    });
}
}

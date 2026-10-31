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
auto failure(std::string message) {
    return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
}
}
Result<void> register_texture_types(Runtime& runtime) {
    if (runtime.native_type<TextureResource>())
        return {};
    auto result = runtime.register_types({reflection::Definition<TextureResource>::build(runtime)});
    if (!result)
        return std::unexpected(result.error());
    return {};
}
Result<ObjectRoot> create_texture(Runtime& runtime, std::string_view source, ImageRef image) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return std::unexpected(ready.error());
    if (source.size() > 4096 || source.find('\0') != source.npos)
        return failure("Invalid texture resource source.");
    if (!image && source.empty())
        return failure("A texture without image content requires the persistent source it stands in for.");
    auto type = runtime.native_type<TextureResource>();
    if (!type)
        return std::unexpected(type.error());
    auto root = runtime.create(*type);
    if (!root)
        return std::unexpected(root.error());
    auto written = runtime.with_native<TextureResource>(root->id(), [&](auto& value) -> Result<void> {
        Access::source(value) = String(StringView(source));
        Access::image(value) = std::move(image);
        return {};
    });
    if (!written)
        return std::unexpected(written.error());
    return std::move(*root);
}
Result<TextureState> read_texture(Runtime& runtime, ObjectId id) {
    return runtime.with_native<const TextureResource>(id, [](const auto& value) -> Result<TextureState> {
        return TextureState{std::string(Access::source(value).view().std_view()), Access::image(value)};
    });
}
Result<void> update_texture(Runtime& runtime, ObjectId id, ImageRef image) {
    if (auto ready = runtime.check_code_publication(); !ready)
        return ready;
    return runtime.with_native<TextureResource>(id, [&](auto& value) -> Result<void> {
        if (!image && Access::source(value).empty())
            return failure("A missing texture needs its persistent source key.");
        Access::image(value) = std::move(image);
        return {};
    });
}
}

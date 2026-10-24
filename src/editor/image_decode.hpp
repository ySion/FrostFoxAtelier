#pragma once
#include "material/surface.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace atelier::editor {
// Shared WIC decoding for project texture assets and model-import dependencies.
material::ImageRef decode_image(std::string_view bytes);
// Project-owned: source bytes with the same SHA-256 share immutable decoded pixels.
// Weak ownership allows retired meshes and their images to leave independently.
struct ImageCache {
    std::unordered_map<std::string,std::weak_ptr<const material::Image>> entries; // Keyed by SHA-256 hex.
};
// Decodes on up to eight threads. The first failure stops the remaining work and is rethrown
// after every worker has finished. Returned pixels, shared ones included, may total at most
// pixel_budget bytes; each decode is charged before it allocates.
std::vector<material::ImageRef> decode_images(std::span<const std::string_view> sources,ImageCache* cache = nullptr,
                                              std::uint64_t pixel_budget = ~std::uint64_t{});
}

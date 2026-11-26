#pragma once
#include "foundation/map.hpp"
#include "material/image.hpp"
#include "runtime/runtime.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace atelier::assets {
// Shared WIC decoding for project texture assets and model-import dependencies.
Result<material::ImageRef> decode_image(std::string_view bytes);

// Reader-owned: source bytes with the same SHA-256 share immutable decoded pixels.
// Weak ownership allows retired meshes and their images to leave independently.
struct ImageCache {
    Map<std::string, std::weak_ptr<const material::Image>> entries; // Keyed by SHA-256 hex.
};

// Decodes on up to eight threads; thread startup failure is fatal, with no reduced pool.
// The first decoding failure stops the remaining work and is returned
// after every worker has finished. Returned pixels, shared ones included, may total at most
// pixel_budget bytes; each decode is charged before it allocates. No partial result is published.
Result<std::vector<material::ImageRef>> decode_images(std::span<const std::string_view> sources,
                                                      ImageCache* cache = nullptr,
                                                      std::uint64_t pixel_budget = ~std::uint64_t{});
}

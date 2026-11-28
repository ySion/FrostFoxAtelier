#pragma once
#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "material/image.hpp"
#include "runtime/runtime.hpp"
#include <cstddef>
#include <cstdint>
namespace atelier::assets {
// Shared WIC decoding for project texture assets and model-import dependencies.
Result<material::ImageRef> decode_image(Span<const std::byte> bytes);

// Reader-owned: source bytes with the same SHA-256 share immutable decoded pixels.
// Weak ownership allows retired meshes and their images to leave independently.
struct ImageCache {
    Map<String, std::weak_ptr<const material::Image>> entries; // Keyed by SHA-256 hex.
};

// Decodes on up to eight threads; thread startup failure is fatal, with no reduced pool.
// The first decoding failure stops the remaining work and is returned
// after every worker has finished. Returned pixels, shared ones included, may total at most
// pixel_budget bytes; each decode is charged before it allocates. No partial result is published.
Result<Array<material::ImageRef>> decode_images(Span<const Span<const std::byte>> sources, ImageCache* cache = nullptr,
                                                std::uint64_t pixel_budget = ~std::uint64_t{});
}

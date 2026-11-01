#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Original, unmodified lookup data; provenance and MIT license are recorded in
// third_party/smaa. Only scene_lit.cpp needs this header, so the upstream static
// arrays have one owner in the renderer rather than a second asset load path.
namespace atelier::renderer::detail::smaa_lut {
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"

inline constexpr std::uint32_t area_width = AREATEX_WIDTH;

inline constexpr std::uint32_t area_height = AREATEX_HEIGHT;

inline constexpr std::uint32_t area_pitch = AREATEX_PITCH;

inline constexpr std::uint32_t search_width = SEARCHTEX_WIDTH;

inline constexpr std::uint32_t search_height = SEARCHTEX_HEIGHT;

inline constexpr std::uint32_t search_pitch = SEARCHTEX_PITCH;

inline std::span<const unsigned char> area_bytes() noexcept {
    return areaTexBytes;
}

inline std::span<const unsigned char> search_bytes() noexcept {
    return searchTexBytes;
}

static_assert(sizeof(areaTexBytes) == area_height * area_pitch);

static_assert(sizeof(searchTexBytes) == search_height * search_pitch);
}

#undef AREATEX_WIDTH
#undef AREATEX_HEIGHT
#undef AREATEX_PITCH
#undef AREATEX_SIZE
#undef SEARCHTEX_WIDTH
#undef SEARCHTEX_HEIGHT
#undef SEARCHTEX_PITCH
#undef SEARCHTEX_SIZE

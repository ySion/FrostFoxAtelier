#pragma once

#include "foundation/frame_timing.hpp"

namespace atelier::procedural::phases {

// The profile host installs these markers; ordinary unbound evaluation reads no clock.
inline constinit timing::Marker volume{"procedural.volume", "Volume scatter", &timing::frame};

inline constinit timing::Marker classify{"procedural.volume.classify", "Generate and classify", &volume};

inline constexpr std::array<timing::Marker*, 2> all{&volume, &classify};

}

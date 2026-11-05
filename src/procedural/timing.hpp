#pragma once

#include "foundation/frame_timing.hpp"

namespace atelier::procedural::phases {

// The profile host installs these markers; ordinary unbound evaluation reads no clock.
inline constinit timing::Marker volume{"procedural.volume", "Volume scatter", &timing::frame};

inline constinit timing::Marker build{"procedural.volume.build", "Build volume hierarchy", &volume};

inline constinit timing::Marker classify{"procedural.volume.classify", "Generate and classify", &volume};

// Zero-duration records count events without a clock read per candidate.
inline constinit timing::Marker candidate{"procedural.volume.candidate", "Candidate", &classify};

inline constinit std::array<timing::Marker, 8> attempts{{{"procedural.volume.attempt0", "Direction 0", &classify},
                                                         {"procedural.volume.attempt1", "Direction 1", &classify},
                                                         {"procedural.volume.attempt2", "Direction 2", &classify},
                                                         {"procedural.volume.attempt3", "Direction 3", &classify},
                                                         {"procedural.volume.attempt4", "Direction 4", &classify},
                                                         {"procedural.volume.attempt5", "Direction 5", &classify},
                                                         {"procedural.volume.attempt6", "Direction 6", &classify},
                                                         {"procedural.volume.attempt7", "Direction 7", &classify}}};

inline constinit std::array<timing::Marker, 8> retries{{{"procedural.volume.retry0", "Retry 0", &classify},
                                                        {"procedural.volume.retry1", "Retry 1", &classify},
                                                        {"procedural.volume.retry2", "Retry 2", &classify},
                                                        {"procedural.volume.retry3", "Retry 3", &classify},
                                                        {"procedural.volume.retry4", "Retry 4", &classify},
                                                        {"procedural.volume.retry5", "Retry 5", &classify},
                                                        {"procedural.volume.retry6", "Retry 6", &classify},
                                                        {"procedural.volume.retry7", "Retry 7", &classify}}};

inline constinit timing::Marker fallback{"procedural.volume.fallback", "Solid-angle fallback", &classify};

inline constexpr std::array<timing::Marker*, 21> all{
    &volume,      &build,       &classify,    &candidate,   &attempts[0], &attempts[1], &attempts[2],
    &attempts[3], &attempts[4], &attempts[5], &attempts[6], &attempts[7], &retries[0],  &retries[1],
    &retries[2],  &retries[3],  &retries[4],  &retries[5],  &retries[6],  &retries[7],  &fallback};

}

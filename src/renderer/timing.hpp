#pragma once

#include "foundation/frame_timing.hpp"

// CPU phases of Renderer::render on the host thread. GPU passes are timed by
// Scene Lit timestamp queries and reported through FrameStats.
namespace atelier::renderer::phases {
inline constinit timing::Marker render{"render", "Render", &timing::frame};
inline constinit timing::Marker prepare{"render.prepare", "Prepare", &render};
inline constinit timing::Marker encode{"render.encode", "Encode", &render};
inline constinit timing::Marker frame{"render.frame", "RHI frame", &render};
// Stages measured inside the RHI frame call, recorded under render.frame.
inline constinit timing::Marker submit{"render.submit", "Submit", &frame};
inline constinit timing::Marker present{"render.present", "Present", &frame};
inline constinit timing::Marker wait{"render.wait", "Frame slot wait", &frame};
inline constinit timing::Marker recycle{"render.recycle", "Resize / recycle / reclaim", &frame};
inline constexpr std::array<timing::Marker*, 8> markers{&render, &prepare, &encode, &frame, &submit, &present, &wait, &recycle};
}

#pragma once

#include "foundation/frame_timing.hpp"

// CPU phases of Renderer::render on the host thread. GPU passes are timed by
// Scene Lit timestamp queries and reported through FrameStats.
namespace atelier::renderer::phases {
inline constinit timing::Marker render{"render", "Render", &timing::frame};
inline constinit timing::Marker prepare{"render.prepare", "Prepare", &render};
inline constinit timing::Marker encode{"render.encode", "Encode", &render};
// Encode parts. Only in studio modes is raster draws over scene_draw_calls a per-draw cost:
// Scene Lit surface draws count in scene_draw_calls but are timed under .lit.
inline constinit timing::Marker encode_scene{"render.encode.scene", "Scene", &encode};
inline constinit timing::Marker encode_materials{"render.encode.scene.materials", "Materials", &encode_scene};
inline constinit timing::Marker encode_scene_upload{"render.encode.scene.upload", "Geometry / instance upload", &encode_scene};
inline constinit timing::Marker encode_draws{"render.encode.scene.draws", "Raster draws", &encode_scene};
inline constinit timing::Marker encode_lit{"render.encode.scene.lit", "Scene Lit", &encode_scene};
inline constinit timing::Marker encode_ui{"render.encode.ui", "UI", &encode};
inline constinit timing::Marker encode_ui_upload{"render.encode.ui.upload", "UI instance upload", &encode_ui};
inline constinit timing::Marker frame{"render.frame", "RHI frame", &render};
// Stages measured inside the RHI frame call, recorded under render.frame.
inline constinit timing::Marker submit{"render.submit", "Submit", &frame};
inline constinit timing::Marker present{"render.present", "Present", &frame};
inline constinit timing::Marker wait{"render.wait", "Frame slot wait", &frame};
inline constinit timing::Marker recycle{"render.recycle", "Resize / recycle / reclaim", &frame};
inline constexpr std::array<timing::Marker*, 15> markers{&render, &prepare, &encode, &encode_scene, &encode_materials,
    &encode_scene_upload, &encode_draws, &encode_lit, &encode_ui, &encode_ui_upload, &frame, &submit, &present, &wait, &recycle};
}

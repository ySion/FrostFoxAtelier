#pragma once
#include "renderer/renderer.hpp"
#include <filesystem>
#include <optional>
#include <string>

namespace atelier::application {
// %LOCALAPPDATA%\FrostFoxAtelier\ShaderCache, or empty (no cache) when LOCALAPPDATA is unavailable.
std::filesystem::path shader_cache_directory();
// A Lit view draws once its pipelines are prepared. Until then the frame shows the
// last Lit view that drew, which keeps its lighting history, or Smooth before any did.
// A preparation failure fails every Lit view, so the frame shows Smooth and reports it.
// Fallback frames are never captured: a capture belongs to the requested view.
class LitFallback {
public:
    struct Drawn {
        renderer::FrameResult result;
        renderer::ViewSettings view; // The view the result drew.
        bool fallback{};
        std::string failure;
    };
    renderer::Result<Drawn> render(renderer::Renderer&,renderer::Frame,renderer::Capture*);
private:
    std::optional<renderer::ViewSettings> last_lit_;
};
}

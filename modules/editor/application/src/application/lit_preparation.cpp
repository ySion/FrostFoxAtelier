#include "application/lit_preparation.hpp"
#include "editor/assets.hpp"
#include "foundation/result.hpp"
#include <Windows.h>

namespace atelier::application {
namespace {
std::wstring environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size())
        return {};
    value.resize(count);
    return value;
}
}

std::filesystem::path shader_cache_directory() {
    if (auto host = environment(L"ATELIER_SHADER_CACHE"); !host.empty())
        return host;
    const auto local = environment(L"LOCALAPPDATA");
    return local.empty() ? std::filesystem::path{} : std::filesystem::path(local) / L"FrostFoxAtelier" / L"ShaderCache";
}

void isolate_shader_cache() {
    if (environment(L"ATELIER_SHADER_CACHE").empty())
        SetEnvironmentVariableW(L"ATELIER_SHADER_CACHE", (editor::asset_directory() / L"shader-cache").c_str());
}

renderer::Result<LitFallback::Drawn> LitFallback::render(renderer::Renderer& renderer, renderer::Frame frame,
                                                         renderer::Capture* capture,
                                                         renderer::Result<renderer::FrameResult>* first_attempt) {
    auto result = renderer.render(frame, capture);
    if (first_attempt)
        *first_attempt = result;
    frame.command_trace = nullptr;
    if (result && result->status != renderer::FrameStatus::not_ready) {
        if (result->status == renderer::FrameStatus::accepted && frame.view.shading == renderer::Shading::scene_lit)
            last_lit_ = frame.view;
        return Drawn{*result, frame.view, false, {}};
    }
    std::string failure;
    if (!result) {
        // Other recoverable failures keep the caller's retry path.
        if (result.error().terminal || renderer.preparation(frame.view))
            return std::unexpected(result.error());
        failure = result.error().cause.message;
        last_lit_.reset();
    }
    if (last_lit_ && *last_lit_ != frame.view) {
        auto lit = frame;
        lit.view = *last_lit_;
        ATELIER_TRY_BIND(drawn, renderer.render(lit, nullptr));
        if (drawn->status != renderer::FrameStatus::not_ready)
            return Drawn{*drawn, lit.view, true, {}};
    }
    frame.view.shading = renderer::Shading::smooth;
    ATELIER_TRY_BIND(drawn, renderer.render(frame, nullptr));
    return Drawn{*drawn, frame.view, true, std::move(failure)};
}
}

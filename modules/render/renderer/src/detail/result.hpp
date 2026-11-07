#pragma once

#include "renderer/renderer.hpp"
#include "foundation/result.hpp"

#include <string_view>
#include <variant>

namespace atelier::renderer::detail {

inline Result<void> require(GfxResult result, const char* operation, bool terminal = true) {
    if (result != kGfxResult_NoError)
        return std::unexpected(Error{{rhi::ErrorCode::execution_failed, result, operation}, terminal});
    return {};
}

inline Result<void> allocation(bool valid, const char* message = "GPU allocation failed") {
    if (!valid)
        return std::unexpected(Error{{rhi::ErrorCode::creation_failed, kGfxResult_OutOfMemory, message}, true});
    return {};
}

struct FrameCancelled {
    std::string_view phase;
};

// Only the cancellable recording chain uses this private alternative. Public
// Renderer callers continue to receive FrameStatus::cancelled, not an Error.
template <class T>
using CancellableResult = std::expected<T, std::variant<Error, FrameCancelled>>;

} // namespace atelier::renderer::detail

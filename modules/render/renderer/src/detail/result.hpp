#pragma once

#include "renderer/renderer.hpp"
#include "foundation/result.hpp"
#include "foundation/string.hpp"

#include <variant>

namespace atelier::renderer::detail {

inline Result<void> require(GfxResult result, Utf8Text operation, bool terminal = true) {
    if (result != kGfxResult_NoError)
        return std::unexpected(
            Error{{rhi::ErrorCode::execution_failed, result, std::move(operation).into_string()}, terminal});
    return {};
}

inline Result<void> allocation(bool valid, Utf8Text message = "GPU allocation failed") {
    if (!valid)
        return std::unexpected(
            Error{{rhi::ErrorCode::creation_failed, kGfxResult_OutOfMemory, std::move(message).into_string()}, true});
    return {};
}

struct FrameCancelled {
    StringView phase;
};

// Only the cancellable recording chain uses this private alternative. Public
// Renderer callers continue to receive FrameStatus::cancelled, not an Error.
template <class T>
using CancellableResult = std::expected<T, std::variant<Error, FrameCancelled>>;

} // namespace atelier::renderer::detail

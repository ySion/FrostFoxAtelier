#pragma once

#include "editor/session.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace atelier::application {
struct RecoveryRecord {
    std::filesystem::path scene;
    std::string warning; // The scene survived even if the sidecar could not be written.
};

// One attempt per application lifetime. CPU-only: never resolves or copies assets,
// marks the document saved, or replaces its original file. An unfinished edit is
// cancelled so the existing serializer records the last committed author state.
class SceneRecovery final {
public:
    using Outcome = Result<std::optional<RecoveryRecord>>;
    bool attempted() const noexcept { return attempted_; }
    const Outcome& preserve(editor::Session&, const std::filesystem::path& project, const std::filesystem::path& scene,
                            std::string_view reason, const std::filesystem::path& directory = {});

private:
    bool attempted_{};
    Outcome outcome_{std::optional<RecoveryRecord>{}};
};
}

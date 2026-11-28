#pragma once

#include "editor/session.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include "logging/logging.hpp"
#include <filesystem>

namespace atelier::application {
struct RecoveryRecord {
    std::filesystem::path scene;

    String warning; // The scene survived even if the sidecar could not be written.
};

// One attempt per application lifetime. CPU-only: never resolves or copies assets,
// marks the document saved, or replaces its original file. An unfinished edit is
// cancelled so the existing serializer records the last committed author state.
class SceneRecovery final {
public:
    using Outcome = Result<Optional<RecoveryRecord>>;

    bool attempted() const noexcept { return attempted_; }

    bool logged() const noexcept { return logged_; }

    const Outcome& outcome() const noexcept { return outcome_; }

    Optional<logging::Field> diagnostic_field() const;

    void report_stderr() const noexcept;

    const Outcome& preserve(editor::Session&, const std::filesystem::path& project, const std::filesystem::path& scene,
                            StringView reason, const std::filesystem::path& directory = {});

private:
    bool attempted_{};

    bool logged_{};

    Outcome outcome_{Optional<RecoveryRecord>{}};
};
}

#include "application/recovery.hpp"
#include "control/control.hpp"
#include "editor/project.hpp"
#include "assets/file.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "logging/logging.hpp"
#include <chrono>
#include <cstdio>

namespace atelier::application {
const SceneRecovery::Outcome& SceneRecovery::preserve(editor::Session& document, const std::filesystem::path& project,
                                                      const std::filesystem::path& scene, std::string_view reason,
                                                      const std::filesystem::path& directory) {
    if (attempted_)
        return outcome_;
    attempted_ = true;
    struct Report {
        const Outcome& outcome;

        const std::filesystem::path& project;

        const std::filesystem::path& scene;

        std::string_view reason;

        ~Report() {
            logging::Entry entry{.severity = !outcome ? logging::Severity::error : logging::Severity::info,
                                 .category = "recovery",
                                 .event_code = !outcome   ? "recovery.failed"
                                               : *outcome ? "recovery.saved"
                                                          : "recovery.not_needed",
                                 .message = !outcome   ? outcome.error().message
                                            : *outcome ? "Authoring scene recovery saved."
                                                       : "No unsaved scene required recovery."};
            const auto root = platform::utf16_to_utf8(project.generic_wstring());
            const auto original = platform::utf16_to_utf8(scene.generic_wstring());
            if (!root || !original) {
                std::fputs("Recovery diagnostic capture failed; inspect the original business failure.\n", stderr);
                return;
            }
            entry.fields = {{"reason", std::string(reason)}, {"project_root", *root}, {"original_scene", *original}};
            if (outcome && *outcome) {
                const auto saved = platform::utf16_to_utf8(outcome->value().scene.generic_wstring());
                if (!saved) {
                    std::fputs("Recovery diagnostic capture failed; inspect the original business failure.\n", stderr);
                    return;
                }
                entry.fields.push_back({"recovery_scene", *saved});
                entry.fields.push_back({"metadata_warning", outcome->value().warning});
            }
            static_cast<void>(logging::submit(std::move(entry)));
        }
    } report{outcome_, project, scene, reason};
    try {
        if (!document.dirty())
            return outcome_;
        const bool unfinished = document.editing();
        const auto cancelled = document.cancel_edit();
        // Serialization still gets a chance if a lifecycle notification failed
        // after cancellation had already restored a valid authoring state.
        auto bytes = document.serialize();
        if (!bytes) {
            outcome_ = std::unexpected(bytes.error());
            return outcome_;
        }
        if (!document.dirty())
            return outcome_;
        auto root = directory;
        if (root.empty()) {
            const auto storage = control::default_directory();
            if (!storage) {
                outcome_ = std::unexpected(Error{ErrorCode::invalid_storage, storage.error().what()});
                return outcome_;
            }
            root = storage->parent_path() / "Recovery";
        }
        std::error_code error;
        const auto folder = std::filesystem::absolute(root / control::unique_id(), error);
        if (error) {
            outcome_ = std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
            return outcome_;
        }
        RecoveryRecord record{folder / "Scene.scene", {}};
        if (auto saved = editor::write_file(record.scene, *bytes, editor::WriteMode::create); !saved) {
            outcome_ = std::unexpected(saved.error());
            return outcome_;
        }
        // Publish the surviving path before any optional metadata work can fail.
        outcome_ = std::move(record);
        if (!cancelled)
            outcome_->value().warning = cancelled.error().message;
        const auto original_project = platform::utf16_to_utf8(project.generic_wstring());
        const auto original_scene = platform::utf16_to_utf8(scene.generic_wstring());
        if (!original_project || !original_scene) {
            outcome_->value().warning += " Recovery metadata: Invalid UTF-16 text";
            return outcome_;
        }
        const control::Json metadata{{"version", 1},
                                     {"kind", "author_scene_recovery"},
                                     {"scene", "Scene.scene"},
                                     {"original_project", *original_project},
                                     {"original_scene", *original_scene},
                                     {"reason", reason},
                                     {"unix_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::system_clock::now().time_since_epoch())
                                                          .count()},
                                     {"unfinished_edit_cancelled", unfinished},
                                     {"assets_validated", false},
                                     {"dirty_material_assets", document.dirty_materials()}};
        if (auto saved =
                editor::write_file(folder / "recovery.json", metadata.dump(2) + "\n", editor::WriteMode::create);
            !saved)
            outcome_->value().warning += " Recovery metadata: " + saved.error().message;
    } catch (const std::exception& error) {
        if (outcome_ && *outcome_)
            outcome_->value().warning += " Recovery metadata: " + std::string(error.what());
        else
            outcome_ = std::unexpected(Error{ErrorCode::invalid_storage, error.what()});
    }
    return outcome_;
}
}

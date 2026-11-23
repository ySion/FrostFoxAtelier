#include "application/recovery.hpp"
#include "control/control.hpp"
#include "editor/project.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "logging/logging.hpp"
#include "json/json.hpp"
#include <chrono>
#include <cstdio>

namespace atelier::application {
namespace {
void report_recovery(const SceneRecovery::Outcome& outcome, const std::filesystem::path& project,
                     const std::filesystem::path& scene, std::string_view reason) {
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

Result<void> write_metadata(const std::filesystem::path& folder, const std::filesystem::path& project,
                            const std::filesystem::path& scene, std::string_view reason, bool unfinished,
                            const editor::Session& document) {
    const auto original_project = platform::utf16_to_utf8(project.generic_wstring());
    const auto original_scene = platform::utf16_to_utf8(scene.generic_wstring());
    if (!original_project || !original_scene)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text"});

    const auto json_error = [](const json::Error& error) {
        return Error{ErrorCode::invalid_storage, error.describe()};
    };
    json::Builder metadata;
    const auto value = metadata.root();
    ATELIER_TRY(metadata.set(value, "version", std::uint32_t{1}).transform_error(json_error));
    metadata.set_validated(value, "kind", "author_scene_recovery");
    metadata.set_validated(value, "scene", "Scene.scene");
    metadata.set_validated(value, "original_project", *original_project);
    metadata.set_validated(value, "original_scene", *original_scene);
    ATELIER_TRY(metadata.set(value, "reason", reason).transform_error(json_error));
    const auto time =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    ATELIER_TRY(metadata.set(value, "unix_time_ms", static_cast<std::int64_t>(time)).transform_error(json_error));
    ATELIER_TRY(metadata.set(value, "unfinished_edit_cancelled", unfinished).transform_error(json_error));
    ATELIER_TRY(metadata.set(value, "assets_validated", false).transform_error(json_error));
    ATELIER_TRY_BIND(dirty, metadata.array(value, "dirty_material_assets").transform_error(json_error));
    for (const auto& key : document.dirty_materials())
        ATELIER_TRY(metadata.append(*dirty, key).transform_error(json_error));
    const auto encoded = json::write(value, {true, true});
    return editor::write_file(folder / "recovery.json", {encoded.data(), encoded.size()}, editor::WriteMode::create);
}

SceneRecovery::Outcome save_recovery(editor::Session& document, const std::filesystem::path& project,
                                     const std::filesystem::path& scene, std::string_view reason,
                                     std::filesystem::path directory) {
    if (!document.dirty())
        return std::nullopt;
    const bool unfinished = document.editing();
    const auto cancelled = document.cancel_edit();
    // Cancellation can restore the author state before a lifecycle notification fails.
    ATELIER_TRY_BIND(bytes, document.serialize());
    if (!document.dirty())
        return std::nullopt;
    const auto storage_error = [](const control::Failure& error) {
        return Error{ErrorCode::invalid_storage, error.message};
    };
    if (directory.empty()) {
        ATELIER_TRY_BIND(storage, control::default_directory().transform_error(storage_error));
        directory = storage->parent_path() / "Recovery";
    }
    ATELIER_TRY_BIND(id, control::unique_id().transform_error(storage_error));
    std::error_code error;
    const auto folder = std::filesystem::absolute(directory / *id, error);
    if (error)
        return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
    RecoveryRecord record{folder / "Scene.scene", {}};
    ATELIER_TRY(editor::write_file(record.scene, *bytes, editor::WriteMode::create));

    if (!cancelled)
        record.warning = cancelled.error().message;
    if (auto metadata = write_metadata(folder, project, scene, reason, unfinished, document); !metadata) {
        if (!record.warning.empty())
            record.warning += " ";
        record.warning += "Recovery metadata: " + metadata.error().message;
    }
    return record;
}
}

const SceneRecovery::Outcome& SceneRecovery::preserve(editor::Session& document, const std::filesystem::path& project,
                                                      const std::filesystem::path& scene, std::string_view reason,
                                                      const std::filesystem::path& directory) {
    if (!attempted_) {
        attempted_ = true;
        outcome_ = save_recovery(document, project, scene, reason, directory);
        report_recovery(outcome_, project, scene, reason);
    }
    return outcome_;
}
}

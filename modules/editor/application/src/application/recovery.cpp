#include "application/recovery.hpp"
#include "application/diagnostics.hpp"
#include "control/control.hpp"
#include "editor/project.hpp"
#include "assets/file.hpp"
#include "foundation/string.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "json/json.hpp"
#include <chrono>
#include <cstdio>

namespace atelier::application {
namespace {
constexpr std::size_t max_field_bytes = 4096;

StringView bounded_utf8(StringView text, std::size_t limit) noexcept {
    return diagnostic_utf8(text).utf8_prefix(limit);
}

String path_field_utf8(const std::filesystem::path& path) {
    const auto converted = platform::utf16_to_utf8(path.generic_wstring());
    if (!converted)
        return "(invalid UTF-16 path)"_s;
    return String::from_utf8_validated(converted->view().utf8_prefix(max_field_bytes));
}

Result<void> write_metadata(const std::filesystem::path& folder, const std::filesystem::path& project,
                            const std::filesystem::path& scene, StringView reason, bool unfinished,
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
    return editor::write_file(folder / "recovery.json", StringView{encoded.data(), encoded.size()},
                              editor::WriteMode::create);
}

SceneRecovery::Outcome save_recovery(editor::Session& document, const std::filesystem::path& project,
                                     const std::filesystem::path& scene, StringView reason,
                                     std::filesystem::path directory) {
    if (!document.dirty())
        return Optional<RecoveryRecord>{};
    const bool unfinished = document.editing();
    const auto cancelled = document.cancel_edit();
    // Cancellation can restore the author state before a lifecycle notification fails.
    ATELIER_TRY_BIND(bytes, document.serialize());
    if (!document.dirty())
        return Optional<RecoveryRecord>{};
    const auto storage_error = [](const control::Failure& error) {
        return Error{ErrorCode::invalid_storage, error.message};
    };
    if (directory.empty()) {
        ATELIER_TRY_BIND(storage, control::default_directory().transform_error(storage_error));
        directory = storage->parent_path() / "Recovery";
    }
    ATELIER_TRY_BIND(id, control::unique_id().transform_error(storage_error));
    std::error_code error;
    const auto folder = std::filesystem::absolute(directory / platform::utf8_to_utf16_validated(*id), error);
    if (error)
        return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
    RecoveryRecord record{folder / "Scene.scene", {}};
    ATELIER_TRY(editor::write_file(record.scene, *bytes, editor::WriteMode::create));

    if (!cancelled)
        record.warning = cancelled.error().message;
    if (auto metadata = write_metadata(folder, project, scene, reason, unfinished, document); !metadata) {
        if (!record.warning.empty())
            record.warning.append_ascii(' ');
        record.warning.append_validated("Recovery metadata: ");
        record.warning.append(metadata.error().message);
    }
    return Optional<RecoveryRecord>{std::move(record)};
}
}

Optional<logging::Field> SceneRecovery::diagnostic_field() const {
    if (!attempted_ || !outcome_ || !*outcome_)
        return {};
    const auto saved = platform::utf16_to_utf8((*outcome_)->scene.generic_wstring());
    if (!saved)
        return {};
    return logging::Field{"recovery_scene", String::from_utf8_validated(saved->view().utf8_prefix(max_field_bytes))};
}

void SceneRecovery::report_stderr() const noexcept {
    if (!attempted_)
        return;
    if (!outcome_) {
        const auto text = bounded_utf8(outcome_.error().message, logging::maximum_message_bytes);
        std::fprintf(stderr, "Scene recovery failed: %.*s\n", static_cast<int>(text.size()), text.data());
        return;
    }
    if (!*outcome_)
        return;
    if (const auto saved = platform::utf16_to_utf8((*outcome_)->scene.generic_wstring()))
        std::fprintf(stderr, "Scene recovery saved: %s\n", saved->c_str());
    else
        std::fputs("Scene recovery saved under the Recovery directory (path contains invalid UTF-16).\n", stderr);
    if (!(*outcome_)->warning.empty()) {
        const auto warning = bounded_utf8((*outcome_)->warning, logging::maximum_message_bytes);
        std::fprintf(stderr, "Scene recovery warning: %.*s\n", static_cast<int>(warning.size()), warning.data());
    }
}

const SceneRecovery::Outcome& SceneRecovery::preserve(editor::Session& document, const std::filesystem::path& project,
                                                      const std::filesystem::path& scene, StringView reason,
                                                      const std::filesystem::path& directory) {
    if (attempted_)
        return outcome_;
    attempted_ = true;
    outcome_ = save_recovery(document, project, scene, reason, directory);
    const auto& outcome = outcome_;
    String saved;
    if (outcome && *outcome) {
        auto converted = platform::utf16_to_utf8((*outcome)->scene.generic_wstring());
        if (!converted) {
            report_stderr();
            return outcome_;
        }
        saved = std::move(*converted);
    }
    const bool warned = outcome && *outcome && !(*outcome)->warning.empty();
    String message;
    if (!outcome)
        message = String::from_utf8_validated(bounded_utf8(outcome.error().message, logging::maximum_message_bytes));
    else if (*outcome) {
        constexpr StringView prefix = "Scene recovery saved: ";
        message = String::concat(
            String::validated(prefix),
            String::validated(saved.view().utf8_prefix(logging::maximum_message_bytes - prefix.size())));
    } else
        message = "No unsaved scene required recovery."_s;
    logging::Entry entry{.severity = !outcome ? logging::Severity::error
                                     : warned ? logging::Severity::warning
                                              : logging::Severity::info,
                         .category = "recovery"_s,
                         .event_code = !outcome   ? "recovery.failed"_s
                                       : *outcome ? "recovery.saved"_s
                                                  : "recovery.not_needed"_s,
                         .message = std::move(message)};
    entry.fields = {{"reason", String::from_utf8_validated(bounded_utf8(reason, max_field_bytes))},
                    {"project_root", path_field_utf8(project)},
                    {"original_scene", path_field_utf8(scene)}};
    if (outcome && *outcome) {
        entry.fields.add(
            {"recovery_scene", String::from_utf8_validated(saved.view().utf8_prefix(max_field_bytes))});
        entry.fields.add(
            {"metadata_warning", String::from_utf8_validated(bounded_utf8((*outcome)->warning, max_field_bytes))});
    }
    logged_ = logging::submit(std::move(entry)).status == logging::SubmitStatus::accepted;
    if (!logged_)
        report_stderr();
    return outcome_;
}
}

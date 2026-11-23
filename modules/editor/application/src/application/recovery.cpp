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

std::string_view bounded_utf8(std::string_view text, std::size_t limit) noexcept {
    return StringView{diagnostic_utf8(text)}.utf8_prefix(limit).std_view();
}

std::string path_field_utf8(const std::filesystem::path& path) {
    const auto converted = platform::utf16_to_utf8(path.generic_wstring());
    if (!converted)
        return "(invalid UTF-16 path)";
    return std::string(StringView{*converted}.utf8_prefix(max_field_bytes).std_view());
}
}

std::optional<logging::Field> SceneRecovery::diagnostic_field() const {
    if (!attempted_ || !outcome_ || !*outcome_)
        return std::nullopt;
    const auto saved = platform::utf16_to_utf8((*outcome_)->scene.generic_wstring());
    if (!saved)
        return std::nullopt;
    return logging::Field{"recovery_scene", std::string(StringView{*saved}.utf8_prefix(max_field_bytes).std_view())};
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
                                                      const std::filesystem::path& scene, std::string_view reason,
                                                      const std::filesystem::path& directory) {
    if (attempted_)
        return outcome_;
    attempted_ = true;
    struct Report {
        SceneRecovery& self;

        const std::filesystem::path& project;

        const std::filesystem::path& scene;

        std::string_view reason;

        ~Report() {
            const auto& outcome = self.outcome_;
            std::string saved;
            if (outcome && *outcome) {
                auto converted = platform::utf16_to_utf8((*outcome)->scene.generic_wstring());
                if (!converted) {
                    self.report_stderr();
                    return;
                }
                saved = std::move(*converted);
            }
            const bool warned = outcome && *outcome && !(*outcome)->warning.empty();
            std::string message;
            if (!outcome)
                message = std::string(bounded_utf8(outcome.error().message, logging::maximum_message_bytes));
            else if (*outcome) {
                message = "Scene recovery saved: ";
                message += StringView{saved}.utf8_prefix(logging::maximum_message_bytes - message.size()).std_view();
            } else
                message = "No unsaved scene required recovery.";
            logging::Entry entry{.severity = !outcome ? logging::Severity::error
                                             : warned ? logging::Severity::warning
                                                      : logging::Severity::info,
                                 .category = "recovery",
                                 .event_code = !outcome   ? "recovery.failed"
                                               : *outcome ? "recovery.saved"
                                                          : "recovery.not_needed",
                                 .message = std::move(message)};
            entry.fields = {{"reason", std::string(bounded_utf8(reason, max_field_bytes))},
                            {"project_root", path_field_utf8(project)},
                            {"original_scene", path_field_utf8(scene)}};
            if (outcome && *outcome) {
                entry.fields.push_back(
                    {"recovery_scene", std::string(StringView{saved}.utf8_prefix(max_field_bytes).std_view())});
                entry.fields.push_back(
                    {"metadata_warning", std::string(bounded_utf8((*outcome)->warning, max_field_bytes))});
            }
            self.logged_ = logging::submit(std::move(entry)).status == logging::SubmitStatus::accepted;
            if (!self.logged_)
                self.report_stderr();
        }
    } report{*this, project, scene, reason};
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
        json::Builder metadata;
        const auto value = metadata.root();
        static_cast<void>(metadata.set(value, "version", std::uint32_t{1}));
        metadata.set_validated(value, "kind", "author_scene_recovery");
        metadata.set_validated(value, "scene", "Scene.scene");
        metadata.set_validated(value, "original_project", *original_project);
        metadata.set_validated(value, "original_scene", *original_scene);
        metadata.set_validated(value, "reason", diagnostic_utf8(reason));
        const auto time =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        static_cast<void>(metadata.set(value, "unix_time_ms", static_cast<std::int64_t>(time)));
        static_cast<void>(metadata.set(value, "unfinished_edit_cancelled", unfinished));
        static_cast<void>(metadata.set(value, "assets_validated", false));
        const auto dirty = *metadata.array(value, "dirty_material_assets");
        for (const auto& key : document.dirty_materials()) {
            if (auto written = metadata.append(dirty, key); !written) {
                outcome_->value().warning += " Recovery metadata: " + written.error().describe();
                return outcome_;
            }
        }
        const auto encoded = json::write(value, {true, true});
        if (auto saved = editor::write_file(folder / "recovery.json", {encoded.data(), encoded.size()},
                                            editor::WriteMode::create);
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

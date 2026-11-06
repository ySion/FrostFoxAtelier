#include "editor/project_ui.hpp"
#include <array>

namespace atelier::editor {
ProjectUI::ProjectUI() {
    const auto registered = ui_language::register_types(runtime_);
    if (!registered)
        throw std::runtime_error(registered.error().message);
}

void ProjectUI::sync(const Project* project) {
    const auto root = project ? project->root() : std::filesystem::path{};
    const auto entry = project ? project->ui_source() : std::filesystem::path{};
    if (root_ != root || entry_ != entry) {
        document_.reset();
        accepted_controls_.clear();
        accepted_ = false;
        error_.clear();
        root_ = root;
        entry_ = entry;
        title_ = project ? project->name() : L"";
        if (project && !entry.empty()) {
            auto path = project->resolve(entry);
            if (!path)
                error_ = path.error().message;
            else
                document_ = std::make_unique<ui_language::LiveDocument>(runtime_, *path, next_key_);
        }
    }
}

Result<std::shared_ptr<const ui::UiFrame>> ProjectUI::prepare(ui::Session& session, ui::Surface surface, double elapsed,
                                                              std::size_t entities,
                                                              const std::function<void(const ui::Reply&)>& effects) {
    const auto failed = [](std::string message) {
        return std::unexpected(Error{ErrorCode::invalid_definition, std::move(message)});
    };
    const auto describe = [](std::string_view stage, const ui::Error& error) {
        return "Project UI " + std::string(stage) + " failed at control " + std::to_string(error.key) + " (UI error " +
               std::to_string(static_cast<unsigned>(error.code)) + ").";
    };
    auto resized = session.set_surface(surface);
    if (!resized)
        return failed(describe("surface", resized.error()));
    effects(*resized);
    // Do not publish a candidate that cannot yet be checked by the real layout
    // engine. In particular, minimizing cannot turn a bad source into active UI.
    if (!surface.extent.width || !surface.extent.height)
        return std::shared_ptr<const ui::UiFrame>{};
    const auto restore = [&](std::string message) -> Result<std::shared_ptr<const ui::UiFrame>> {
        if (document_)
            document_->reject_controls(message);
        error_ = std::move(message);
        if (!accepted_)
            return failed(error_);
        auto restored = session.set_controls(accepted_controls_);
        if (!restored)
            return failed(describe("restore", restored.error()));
        effects(*restored);
        auto frame = session.prepare();
        if (!frame)
            return failed(describe("restored layout", frame.error()));
        ATELIER_TRY(collect());
        return *frame;
    };
    std::vector<ui::Control> controls;
    if (document_) {
        document_->poll();
        const std::array<ui_language::InputValue, 3> inputs{title_, elapsed, static_cast<double>(entities)};
        auto appended = document_->append("ProjectUI", 0, {}, inputs, controls);
        if (!appended)
            return restore(appended.error().message);
        if (document_->candidate_failed())
            return restore(std::string(document_->diagnostic()));
    } else if (configured())
        return failed(error_);
    auto described = session.set_controls(controls);
    if (!described)
        return restore(describe("controls", described.error()));
    effects(*described);
    auto frame = session.prepare();
    if (!frame)
        return restore(describe("layout", frame.error()));
    if (document_)
        document_->commit_controls();
    accepted_controls_ = std::move(controls);
    accepted_ = true;
    error_.clear();
    ATELIER_TRY(collect());
    return *frame;
}

Result<bool> ProjectUI::event(const ui::Action& action) {
    return document_ ? document_->event(action) : Result<bool>{false};
}

Result<void> ProjectUI::collect() {
    if (runtime_.collection_phase() == CollectionPhase::idle) {
        ATELIER_TRY_BIND(started, runtime_.begin_collection());
    }
    ATELIER_TRY_BIND(collected, runtime_.collect_step({32}));
    return {};
}

std::string_view ProjectUI::diagnostic() const noexcept {
    return error_.empty() ? (document_ ? document_->diagnostic() : std::string_view{}) : error_;
}
}

#pragma once
#include "editor/project.hpp"
#include "editor/session.hpp"
#include "source/program.hpp"
#include "physics/physics.hpp"
#include <chrono>

namespace atelier::editor {
enum class BuildState { ready, failed, rebuild_required };

struct ProjectBuild {
    std::shared_ptr<const source::SourceSnapshot> sources;

    std::optional<source::SourceFile> ui_source;

    BuildState state{BuildState::ready};

    std::optional<Error> error;

    std::uint64_t published_version{};
};

// One project compilation unit in one World/Runtime. Session retains this owner
// until its World has ended and releases it before destroying the Runtime.
class ProjectCode final {
public:
    explicit ProjectCode(world::World&);

    Result<void> build(const Project&);

    Result<void> poll(const Project&, bool force = false);

    const std::shared_ptr<const ProjectBuild>& report() const noexcept { return report_; }

    // Last successful publication, independent of a later failed build report.
    const std::shared_ptr<const ProjectBuild>& published_build() const noexcept { return published_; }

    static Result<source::SourceSnapshot> snapshot(const Project&);

    static Result<std::optional<source::SourceFile>> ui_snapshot(const Project&);

    static std::string component_source(std::string_view name);

    static std::string ui_source();

private:
    struct Stamp {
        std::filesystem::path path;

        std::filesystem::file_time_type modified;

        std::uintmax_t bytes{};

        bool operator==(const Stamp&) const = default;
    };

    world::World& world_;

    std::unique_ptr<source::Program> program_;

    std::shared_ptr<const ProjectBuild> report_;

    std::shared_ptr<const ProjectBuild> published_;

    std::vector<Stamp> stamps_;

    std::chrono::steady_clock::time_point next_poll_{};

    bool bound_{};

    bool adds_behavior(const source::Program::Candidate&) const;

    Result<void> validate(const source::Program::Candidate&) const;

    Result<void> bind();
};

struct ProjectDocument {
    std::unique_ptr<Session> session;

    // Borrow from session's preparation owner. Never retained across replacement.
    ProjectCode* code{};

    std::shared_ptr<const ProjectBuild> build;

    std::optional<Error> error;
};

// With allow_failed_build, a Source build failure still produces a document without
// a code owner: project types are absent, so their objects load as placeholders.
ProjectDocument load_project_scene(Project&, std::string_view, LoadPolicy = LoadPolicy::tolerant,
                                   bool allow_failed_build = false, SessionRole = SessionRole::runtime);

// One Project Output line for a World object diagnostic.
std::wstring describe_diagnostic(const Session&, const world::Diagnostic&);
}

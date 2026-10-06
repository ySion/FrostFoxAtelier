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
    double milliseconds{};
};

// One project compilation unit in one World/Runtime. Session retains this owner
// until its World has ended and releases it before destroying the Runtime.
class ProjectCode final {
public:
    explicit ProjectCode(world::World&);
    Result<void> build(const Project&);
    Result<void> poll(const Project&,bool force=false);
    const std::shared_ptr<const ProjectBuild>& report() const noexcept {return report_;}
    const physics::Types& physics_types() const noexcept {return physics_;}
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
    physics::Types physics_;
    std::unique_ptr<source::Program> program_;
    std::shared_ptr<const ProjectBuild> report_;
    std::shared_ptr<const source::SourceSnapshot> published_sources_;
    std::vector<Stamp> stamps_;
    std::chrono::steady_clock::time_point next_poll_{};
    bool bound_{};
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
ProjectDocument load_project_scene(Project&,std::string_view);
}

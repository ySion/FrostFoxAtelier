#include "editor/project_code.hpp"
#include "platform/windows_path.hpp"
#include "ui_language/component.hpp"
#include <algorithm>
#include <array>
#include <charconv>

namespace atelier::editor {
namespace {
auto fail(std::string message) {return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message)});}
std::uint64_t hash(std::string_view text,std::uint64_t seed=14695981039346656037ULL) {
    for (const auto c:text) {seed^=static_cast<unsigned char>(c);seed*=1099511628211ULL;}
    return seed?seed:1;
}
bool same(const source::SourceSnapshot& a,const source::SourceSnapshot& b) {
    if (a.files.size()!=b.files.size()) return false;
    for (std::size_t i=0;i<a.files.size();++i)
        if (a.files[i].path!=b.files[i].path || a.files[i].source!=b.files[i].source) return false;
    return true;
}
Result<void> completed(Result<std::optional<Scalar>> result) {
    if (!result) return std::unexpected(result.error());
    return {};
}
Result<void> validate_ui(const source::SourceFile& file) {
    // Compilation only. Project expressions and event handlers are evaluated
    // exclusively by the isolated runner, including UI previews.
    Runtime runtime;auto registered=ui_language::register_types(runtime);if(!registered) return std::unexpected(registered.error());
    auto module=ui_language::Module::compile(runtime,file.path,file.source);
    const auto reject=[&](std::string message)->Result<void> {
        SourceLocation location{file.path,file.version};
        const auto prefix=file.path+":";
        if(message.starts_with(prefix)) {
            auto text=std::string_view(message).substr(prefix.size());
            const auto row=std::from_chars(text.data(),text.data()+text.size(),location.line);
            if(row.ec==std::errc{} && row.ptr!=text.data()+text.size() && *row.ptr==':')
                static_cast<void>(std::from_chars(row.ptr+1,text.data()+text.size(),location.column));
        }
        return std::unexpected(Error{ErrorCode::invalid_definition,std::move(message),std::move(location)});
    };
    if(!module) return reject(module.error().message);
    const auto entry=(*module)->find("ProjectUI");
    if(!entry) return reject("Project UI requires ui ProjectUI(in Text title, in double elapsed, in double actors).");
    const auto& inputs=entry->declaration().inputs;
    if(inputs.size()!=3 || inputs[0].name!="title" || inputs[0].kind!=ui_language::InputKind::text ||
        inputs[1].name!="elapsed" || inputs[1].kind!=ui_language::InputKind::number ||
        inputs[2].name!="actors" || inputs[2].kind!=ui_language::InputKind::number)
        return reject("ProjectUI inputs must be title (Text), elapsed (double), actors (double).");
    return {};
}
}
ProjectCode::ProjectCode(world::World& world):world_(world),report_(std::make_shared<ProjectBuild>()) {

}
Result<source::SourceSnapshot> ProjectCode::snapshot(const Project& project) {
    auto paths=project.source_paths();if (!paths) return std::unexpected(paths.error());
    source::SourceSnapshot result;result.name=utf8(project.name());result.version=14695981039346656037ULL;
    std::size_t bytes{};
    for (const auto& relative:*paths) {
        auto path=project.resolve(relative);if (!path) return std::unexpected(path.error());
        auto text=read_file(*path,1024*1024);if (!text) return std::unexpected(text.error());
        bytes+=text->size();if (bytes>16*1024*1024) return fail("Project Source exceeds the 16 MiB compilation limit.");
        try {static_cast<void>(wide(*text));}catch(const std::exception& error){return fail(error.what());}
        const auto version=hash(*text);const auto name=utf8(relative.generic_wstring());
        result.version=hash(name,hash(*text,result.version));
        if (text->starts_with("\xef\xbb\xbf")) text->erase(0,3);
        result.files.push_back({name,version,std::move(*text)});
    }
    return result;
}
Result<std::optional<source::SourceFile>> ProjectCode::ui_snapshot(const Project& project) {
    if(project.ui_source().empty()) return std::optional<source::SourceFile>{};
    auto path=project.resolve(project.ui_source());if(!path) return std::unexpected(path.error());
    auto content=read_file(*path,1024*1024);if(!content) return std::unexpected(content.error());
    try {static_cast<void>(wide(*content));}catch(const std::exception& error){return fail(error.what());}
    const auto version=hash(*content);if(content->starts_with("\xef\xbb\xbf")) content->erase(0,3);
    return source::SourceFile{utf8(project.ui_source().generic_wstring()),version,std::move(*content)};
}
Result<void> ProjectCode::build(const Project& project) {
    const auto start=std::chrono::steady_clock::now();
    auto next=std::make_shared<ProjectBuild>();next->published_version=report_->published_version;
    const auto reject=[&](Error error)->Result<void> {
        if(error.source && !error.message.starts_with(error.source->path+":"))
            error.message=error.source->path+":"+std::to_string(error.source->line)+":"+std::to_string(error.source->column)+": "+error.message;
        next->state=error.code==ErrorCode::rebuild_required?BuildState::rebuild_required:BuildState::failed;
        next->error=std::move(error);
        next->milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        report_=next;return std::unexpected(*next->error);
    };
    auto input=snapshot(project);if (!input) return reject(input.error());
    next->sources=std::make_shared<source::SourceSnapshot>(std::move(*input));
    auto ui_source=ui_snapshot(project);if(!ui_source) return reject(ui_source.error());
    next->ui_source=std::move(*ui_source);
    const bool ui_same=(!report_->ui_source && !next->ui_source) || (report_->ui_source && next->ui_source &&
        report_->ui_source->path==next->ui_source->path && report_->ui_source->source==next->ui_source->source);
    if (report_->sources && same(*next->sources,*report_->sources) && ui_same) {
        if (report_->error) return std::unexpected(*report_->error);
        return {};
    }
    if(next->ui_source && (!ui_same || report_->error)) {
        auto checked=validate_ui(*next->ui_source);if(!checked) return reject(checked.error());
    }
    if (!published_ || !same(*published_->sources,*next->sources)) {
        while (world_.runtime().collection_phase()!=CollectionPhase::idle) {
            auto step=world_.runtime().collect_step({1024});if (!step) return reject(step.error());
        }
        if (!program_) program_=std::make_unique<source::Program>(world_.runtime());
        auto request=program_->request(*next->sources);if (!request) return reject(request.error());
        auto candidate=source::Program::prepare(*request);if (!candidate) return reject(candidate.error());
        auto valid=validate(*candidate);if(!valid) return reject(valid.error());
        auto published=program_->try_publish(*candidate);if (!published) return reject(published.error());
        if (!bound_) {auto binding=bind();if (!binding) return reject(binding.error());bound_=true;}
    }
    next->published_version=next->ui_source?hash(next->ui_source->source,next->sources->version):next->sources->version;
    next->milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    published_=next;report_=std::move(next);return {};
}
Result<void> ProjectCode::validate(const source::Program::Candidate& candidate) const {
    auto& runtime=world_.runtime();
    for(const auto& type:candidate.classes()) {
        const auto actor=runtime.is_a(type.base,world_.types().actor),component=runtime.is_a(type.base,world_.types().component);
        if(!actor || !component) return fail("Cannot resolve a project class's published native base.");
        if(!*actor && !*component) continue;
        if(!bound_ && world_.running()) return std::unexpected(Error{ErrorCode::rebuild_required,"Stop and rebuild the project before adding its behavior bindings."});

    }
    return {};
}
Result<void> ProjectCode::bind() {
    auto& runtime=world_.runtime();
    for (const auto& type:program_->classes()) {
        const auto actor=runtime.is_a(type.type,world_.types().actor);
        const auto component=runtime.is_a(type.type,world_.types().component);
        if (!actor || !component) return fail("Cannot resolve a project behavior's base type.");
        if (!*actor && !*component) continue;
        world::World::Behavior behavior;
        for (const auto& method:program_->entries()) {
            const auto prefix=type.name+".";if (!method.name.starts_with(prefix)) continue;
            const auto name=std::string_view(method.name).substr(prefix.size());
            if (name!="begin" && name!="tick" && name!="end") continue;
            const bool tick=name=="tick";
            auto entry=program_->entry(method.name);if (!entry) return std::unexpected(entry.error());
            if (tick) behavior.tick=[entry=*entry](Runtime&,ObjectId self,float dt) {
                const std::array<Scalar,1> args{dt};return completed(entry.invoke(self,args));
            };
            else {
                auto call=[entry=*entry](Runtime&,ObjectId self){return completed(entry.invoke(self,std::span<const Scalar>{}));};
                if (name=="begin") behavior.begin=std::move(call);else behavior.end=std::move(call);
            }
        }
        if(behavior.begin || behavior.tick || behavior.end)
            if (auto bound=world_.bind_behavior(type.type,std::move(behavior));!bound) return bound;
    }
    return {};
}
Result<void> ProjectCode::poll(const Project& project,bool force) {
    const auto now=std::chrono::steady_clock::now();if (!force && now<next_poll_) return {};
    next_poll_=now+std::chrono::milliseconds(500);
    auto paths=project.source_paths();if (!paths) return build(project);
    if(!project.ui_source().empty()) paths->push_back(project.ui_source());
    std::vector<Stamp> stamps;stamps.reserve(paths->size());
    try {
        for (const auto& relative:*paths) {
            const auto path=project.resolve(relative);if (!path) return build(project);
            const auto native=platform::windows_file_path(*path);
            stamps.push_back({relative,std::filesystem::last_write_time(native),std::filesystem::file_size(native)});
        }
    } catch(const std::exception&) {return build(project);}
    if (!force && stamps==stamps_) return {};
    stamps_=std::move(stamps);return build(project);
}
ProjectDocument load_project_scene(Project& project,std::string_view source) {
    ProjectDocument result;
    auto document=Session::deserialize(source,project,[&](world::World& world)->Result<std::shared_ptr<void>> {
        auto code=std::make_shared<ProjectCode>(world);const auto built=code->build(project);result.build=code->report();
        if (!built) return std::unexpected(built.error());
        result.code=code.get();return std::static_pointer_cast<void>(std::move(code));
    });
    if (document) result.session=std::move(*document);
    else {result.error=result.build && result.build->error?*result.build->error:document.error();result.code=nullptr;}
    return result;
}
std::string ProjectCode::component_source(std::string_view name) {
    return "class project."+std::string(name)+" : world.Component {\n    float speed = 1.0f;\n\n    void tick(float dt) {\n        self.translate_owner(self.speed * dt, 0.0f, 0.0f);\n    }\n}\n";
}
std::string ProjectCode::ui_source() {
    return "ui ProjectUI(in Text title, in double elapsed, in double actors) {\n    Column hud {\n        padding_left: 8; padding_top: 8; gap: 4;\n        Text title { text: input.title; }\n        Text hint { text: \"Edit this UI in your project.\"; }\n    }\n}\n";
}
}

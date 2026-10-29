#include "application/diagnostics.hpp"
#include "assets/file.hpp"
#include <array>
#include <cstdio>
#include <limits>

namespace atelier::application {
namespace {
void bridge_failed() noexcept {std::fputs("Host diagnostic capture failed; business result is unchanged.\n",stderr);}
std::string path_field(std::span<const std::string> fields) {
    std::string result;
    for(const auto& field:fields) {
        result+='/';
        for(const auto c:field) {if(c=='~') result+="~0";else if(c=='/') result+="~1";else result+=c;}
    }
    return result;
}
}
logging::SourceLocation diagnostic_source(const SourceLocation& value) {
    logging::SourceLocation result;
    result.file=value.path;result.disk_path=value.disk_path;
    result.line=value.line<=UINT32_MAX?static_cast<std::uint32_t>(value.line):0;
    result.column=value.column<=UINT32_MAX?static_cast<std::uint32_t>(value.column):0;
    result.version=value.version;result.offset=value.offset;
    return result;
}
logging::Entry world_diagnostic(const editor::Session& session,const world::Diagnostic& item,
    const std::filesystem::path& root,std::string_view origin) {
    logging::Entry entry;
    entry.severity=item.severity==world::Severity::error?logging::Severity::error:
        item.severity==world::Severity::warning?logging::Severity::warning:logging::Severity::info;
    entry.category="world";entry.event_code="world."+std::string(world::name(item.kind));entry.message=item.message;
    if(item.source) entry.source=diagnostic_source(*item.source);
    const auto document=session.presentation().identity();
    entry.fields={{"document",document},{"project_root",assets::utf8(root.generic_wstring())},{"origin",std::string(origin)},{"serial",item.serial},
        {"phase",std::string(world::name(item.phase))},{"object",item.object.diagnostic_identity()},
        {"entity",item.entity.diagnostic_identity()},{"type",item.type},{"field_path",path_field(item.field)},
        {"value",item.value},{"repeats",std::uint64_t{item.repeats}}};
    if(const auto local=session.document_identity(item.object)) entry.fields.push_back({"scene_local_id",*local});
    // Presentation repeat groups survive a resolved/recurrent value, but are
    // scoped to the actual document and object, never just rendered text.
    const auto identity=std::to_string(document)+"/"+std::string(origin)+"/"+item.object.diagnostic_identity()+"/"+
        std::string(world::name(item.phase))+"/"+std::string(world::name(item.kind))+"/"+path_field(item.field);
    const bool consumer=item.phase==world::DiagnosticPhase::presentation || item.phase==world::DiagnosticPhase::lighting ||
        item.phase==world::DiagnosticPhase::physics || item.phase==world::DiagnosticPhase::command;
    if(origin=="world" && consumer && identity.size()<=256) entry.repeat=logging::Repeat{identity,item.repeats};
    return entry;
}
void report_world(const editor::Session& session,std::span<const world::Diagnostic> items,
    const std::filesystem::path& root,std::string_view origin) noexcept {
    try {for(const auto& item:items) static_cast<void>(logging::submit(world_diagnostic(session,item,root,origin)));}
    catch(...) {bridge_failed();}
}
void drain_world(editor::Session& session,const std::filesystem::path& root,std::uint64_t& dropped) noexcept {
    try {
        report_world(session,session.take_diagnostics(),root);
        const auto total=session.world().dropped_diagnostics();
        if(total!=dropped) {
            logging::Entry entry{.severity=logging::Severity::warning,.category="world",.event_code="world.diagnostics_dropped",
                .message="World diagnostics exceeded their retention capacity."};
            entry.fields={{"document",session.presentation().identity()},{"dropped_total",total},{"dropped_delta",total-dropped}};
            static_cast<void>(logging::submit(std::move(entry)));dropped=total;
        }
    } catch(...) {bridge_failed();}
}
void report_load(const editor::Session& session,const std::filesystem::path& root) noexcept {
    try {
        logging::Entry entry{.category="world",.event_code="world.scene_loaded",.message="Scene loaded."};
        entry.fields={{"document",session.presentation().identity()},{"placeholders",std::uint64_t{session.placeholder_count()}},
            {"issues",std::uint64_t{session.load_issues().size()}},{"project_root",assets::utf8(root.generic_wstring())}};
        static_cast<void>(logging::submit(std::move(entry)));
        report_world(session,session.load_issues(),root,"load");
    } catch(...) {bridge_failed();}
}
void report_source_capture(const source::DiagnosticReport& report,const std::filesystem::path& root) noexcept {
    try {
        constexpr std::array status{"unused","ready","running","suspended","completed","failed","cancelled","rejected"};
        constexpr std::array counters{"begin","finish","field_read","field_write","native_call","poll","object_create",
            "reference","reference_compare","root_clear","value_construct","value_assign","value_destroy","sequence_mutation",
            "sequence_view","coroutine_allocate","coroutine_free","source_call","debug_hook"};
        static_assert(counters.size()==static_cast<std::size_t>(source::DiagnosticCall::count));
        logging::Entry entry{.severity=report.error?logging::Severity::error:logging::Severity::info,
            .category="source",.event_code="source.capture",.message=report.error?report.error->message:"Explicit Source diagnostic capture."};
        entry.fields={{"program_id",report.program_id},{"code_version",report.code_version},{"entry",report.entry},
            {"project_root",assets::utf8(root.generic_wstring())},
            {"source_name",report.source_name},{"status",std::string(status.at(static_cast<std::size_t>(report.status)))},
            {"resumes",report.resumes},{"peak_owned_values",report.peak_owned_values},
            {"remaining_owned_values",std::uint64_t{report.remaining_owned_values}},{"resources_cleared",report.resources_cleared},
            {"timing",report.timing},{"active_time_ns",std::int64_t{report.active_time.count()}}};
        for(std::size_t i=0;i<counters.size();++i) entry.fields.push_back({std::string("counter.")+counters[i],report.calls.values[i]});
        const auto resources=[&](const char* prefix,const source::DiagnosticResources& value) {
            entry.fields.push_back({std::string(prefix)+".objects",std::uint64_t{value.objects}});
            entry.fields.push_back({std::string(prefix)+".roots",std::uint64_t{value.roots}});
            entry.fields.push_back({std::string(prefix)+".pending_reclamation",std::uint64_t{value.pending_reclamation}});
            entry.fields.push_back({std::string(prefix)+".collection_phase",static_cast<std::uint64_t>(value.collection_phase)});
        };
        resources("before",report.before);resources("after",report.after);
        if(report.location) {entry.source=diagnostic_source(report.location->source);entry.source.function=report.location->function;}
        if(report.error) {
            entry.fields.push_back({"error_code",static_cast<std::uint64_t>(report.error->code)});
            // Error and suspended/current locations are independently meaningful.
            if(report.error->source) {
                const auto& location=*report.error->source;
                entry.fields.insert(entry.fields.end(),{{"error.path",location.path},{"error.version",location.version},
                    {"error.offset",std::uint64_t{location.offset}},{"error.line",std::uint64_t{location.line}},
                    {"error.column",std::uint64_t{location.column}},{"error.disk_path",location.disk_path}});
                if(!report.location) entry.source=diagnostic_source(location);
            }
        }
        static_cast<void>(logging::submit(std::move(entry)));
    } catch(...) {bridge_failed();}
}
}

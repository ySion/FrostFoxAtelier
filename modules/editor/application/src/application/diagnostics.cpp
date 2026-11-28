#include "application/diagnostics.hpp"
#include "foundation/span.hpp"
#include "foundation/string.hpp"
#include "platform/utf.hpp"
#include "renderer/renderer.hpp"
#include <array>
#include <cstdio>
#include <limits>

namespace atelier::application {
Error ui_error(const ui::Error& error) {
    const auto detail = String::concat("UI error ", static_cast<unsigned>(error.code), " at control ", error.key,
                                       " (native ", error.native_code, ")");
    const auto msg = String::validated(error.message);
    return {ErrorCode::invalid_definition, error.code == ui::ErrorCode::text_failed
                                               ? String::concat(msg, "; ", detail)
                                               : String::concat(detail, ": ", msg)};
}

Error renderer_error(const renderer::Error& error) {
    return {ErrorCode::invalid_storage,
            String::concat(error.cause.message, " (Renderer error ", static_cast<unsigned>(error.cause.code),
                           ", native ", static_cast<int>(error.cause.native_result),
                           String::validated(error.terminal ? ", terminal)." : ")."))};
}

std::wstring diagnostic_text(StringView message) noexcept {
    auto text = platform::utf8_to_utf16(message).value_or(L"(诊断文本不是有效 UTF-8)");
    for (auto& ch : text)
        if (ch < 32 && ch != L'\n' && ch != L'\r' && ch != L'\t')
            ch = static_cast<wchar_t>(0x2400 + ch); // Unicode control pictures keep embedded bytes visible.
    return text;
}

String diagnostic_text(std::wstring_view message) noexcept {
    return platform::utf16_to_utf8(message).value_or("(诊断文本不是有效 UTF-16)"_s);
}

StringView diagnostic_utf8(StringView message) noexcept {
    return message.is_valid_utf8() ? message : "(诊断文本不是有效 UTF-8)";
}

namespace {
void bridge_failed() noexcept {
    std::fputs("Host diagnostic capture failed; business result is unchanged.\n", stderr);
}

void error_source(logging::Entry& entry, const SourceLocation& source) {
    entry.source = diagnostic_source(source);
    // Navigation uses bounded line/column values. The diagnostic retains the
    // original six fields even when a location cannot be navigated by the UI.
    entry.fields.append({{"error.path", source.path},
                         {"error.version", source.version},
                         {"error.offset", std::uint64_t{source.offset}},
                         {"error.line", std::uint64_t{source.line}},
                         {"error.column", std::uint64_t{source.column}},
                         {"error.disk_path", source.disk_path}});
}

void error_element(logging::Entry& entry, const ErrorElement& element) {
    entry.fields.add({"error.element_layer", String::from_utf8_validated(diagnostic_utf8(element.layer.view()))});
    entry.fields.add({"error.element_index", std::uint64_t{element.index}});
}

String path_field(Span<const String> fields) {
    String result;
    for (const auto& field : fields) {
        result.append_ascii('/');
        const auto sv = field.view();
        std::size_t plain = 0;
        for (std::size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '~' || sv[i] == '/') {
                if (i > plain)
                    result.append_validated(sv.slice(plain, i - plain));
                result.append_validated(sv[i] == '~' ? "~0" : "~1");
                plain = i + 1;
            }
        }
        if (sv.size() > plain)
            result.append_validated(sv.slice(plain));
    }
    return result;
}
}

logging::SourceLocation diagnostic_source(const SourceLocation& value) {
    logging::SourceLocation result;
    result.file = value.path;
    result.disk_path = value.disk_path;
    result.line = value.line <= UINT32_MAX ? static_cast<std::uint32_t>(value.line) : 0;
    result.column = value.column <= UINT32_MAX ? static_cast<std::uint32_t>(value.column) : 0;
    result.version = value.version;
    result.offset = value.offset;
    return result;
}

Result<logging::Entry> error_diagnostic(const Error& error, StringView event, const std::filesystem::path& root) {
    logging::Entry entry{.severity = logging::Severity::error,
                         .category = "editor"_s,
                         .event_code = String::from_utf8_validated(event),
                         .message = error.message};
    auto path = platform::utf16_to_utf8(root.generic_wstring());
    if (!path)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
    entry.fields = {{"error_code", static_cast<std::uint64_t>(error.code)}, {"project_root", std::move(*path)}};
    if (error.source)
        error_source(entry, *error.source);
    if (error.element)
        error_element(entry, *error.element);
    return entry;
}

Result<logging::Entry> world_diagnostic(const editor::Session& session, const world::Diagnostic& item,
                                        const std::filesystem::path& root, StringView origin) {
    logging::Entry entry;
    entry.severity = item.severity == world::Severity::error     ? logging::Severity::error
                     : item.severity == world::Severity::warning ? logging::Severity::warning
                                                                 : logging::Severity::info;
    entry.category = "world"_s;
    entry.event_code = String::concat("world.", String::validated(world::name(item.kind)));
    entry.message = item.message;
    if (item.source)
        entry.source = diagnostic_source(*item.source);
    const auto document = session.presentation().identity();
    auto path = platform::utf16_to_utf8(root.generic_wstring());
    if (!path)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
    auto field_path = path_field(item.field);
    entry.fields = {{"document", document},
                    {"project_root", std::move(*path)},
                    {"origin", String::from_utf8_validated(origin)},
                    {"serial", item.serial},
                    {"phase", String::from_utf8_validated(world::name(item.phase))},
                    {"object", item.object.diagnostic_identity()},
                    {"entity", item.entity.diagnostic_identity()},
                    {"type", item.type},
                    {"field_path", field_path},
                    {"value", item.value},
                    {"repeats", std::uint64_t{item.repeats}}};
    if (item.error_code) {
        entry.fields.add({"error_code", static_cast<std::uint64_t>(*item.error_code)});
        if (item.source)
            error_source(entry, *item.source);
    }
    if (item.element)
        error_element(entry, *item.element);
    if (const auto local = session.document_identity(item.object))
        entry.fields.add({"scene_local_id", *local});
    // Presentation repeat groups survive a resolved/recurrent value, but are
    // scoped to the actual document and object, never just rendered text.
    auto identity = String::concat(document, "/", String::validated(origin), "/", item.object.diagnostic_identity(),
                                   "/", String::validated(world::name(item.phase)), "/",
                                   String::validated(world::name(item.kind)), "/", field_path);
    const bool consumer =
        item.phase == world::DiagnosticPhase::presentation || item.phase == world::DiagnosticPhase::lighting ||
        item.phase == world::DiagnosticPhase::physics || item.phase == world::DiagnosticPhase::command;
    if (origin == "world" && consumer && identity.size() <= 256)
        entry.repeat = logging::Repeat{std::move(identity), item.repeats};
    return entry;
}

void report_world(const editor::Session& session, Span<const world::Diagnostic> items,
                  const std::filesystem::path& root, StringView origin) noexcept {
    for (const auto& item : items) {
        auto entry = world_diagnostic(session, item, root, origin);
        if (!entry) {
            bridge_failed();
            return;
        }
        static_cast<void>(logging::submit(std::move(*entry)));
    }
}

void drain_world(editor::Session& session, const std::filesystem::path& root, std::uint64_t& dropped) noexcept {
    const auto diagnostics = session.take_diagnostics();
    report_world(session, diagnostics, root);
    const auto total = session.world().dropped_diagnostics();
    if (total != dropped) {
        logging::Entry entry{.severity = logging::Severity::warning,
                             .category = "world"_s,
                             .event_code = "world.diagnostics_dropped"_s,
                             .message = "World diagnostics exceeded their retention capacity."_s};
        entry.fields = {{"document", session.presentation().identity()},
                        {"dropped_total", total},
                        {"dropped_delta", total - dropped}};
        static_cast<void>(logging::submit(std::move(entry)));
        dropped = total;
    }
}

void report_load(const editor::Session& session, const std::filesystem::path& root) noexcept {
    auto path = platform::utf16_to_utf8(root.generic_wstring());
    if (!path) {
        bridge_failed();
        return;
    }
    logging::Entry entry{.category = "world"_s, .event_code = "world.scene_loaded"_s, .message = "Scene loaded."_s};
    entry.fields = {{"document", session.presentation().identity()},
                    {"placeholders", std::uint64_t{session.placeholder_count()}},
                    {"issues", std::uint64_t{session.load_issues().size()}},
                    {"project_root", std::move(*path)}};
    static_cast<void>(logging::submit(std::move(entry)));
    report_world(session, session.load_issues(), root, "load");
}

void report_source_capture(const source::DiagnosticReport& report, const std::filesystem::path& root) noexcept {
    constexpr std::array status{"unused",    "ready",  "running",   "suspended",
                                "completed", "failed", "cancelled", "rejected"};
    constexpr std::array counters{"field_read",         "field_write",       "native_call",       "object_create",
                                  "reference",          "reference_compare", "root_clear",        "value_construct",
                                  "value_assign",       "value_destroy",     "sequence_mutation", "sequence_view",
                                  "coroutine_allocate", "coroutine_free",    "source_call",       "debug_hook"};
    static_assert(counters.size() == static_cast<std::size_t>(source::DiagnosticCall::count));
    logging::Entry entry{.severity = report.error ? logging::Severity::error : logging::Severity::info,
                         .category = "source"_s,
                         .event_code = "source.capture"_s,
                         .message = report.error ? report.error->message : "Explicit Source diagnostic capture."_s};
    auto path = platform::utf16_to_utf8(root.generic_wstring());
    const auto ordinal = static_cast<std::size_t>(report.status);
    if (!path || ordinal >= status.size()) {
        bridge_failed();
        return;
    }
    entry.fields = {{"program_id", report.program_id},
                    {"code_version", report.code_version},
                    {"entry", report.entry},
                    {"project_root", std::move(*path)},
                    {"source_name", report.source_name},
                    {"status", String::from_utf8_validated(status[ordinal])},
                    {"resumes", report.resumes},
                    {"peak_owned_values", report.peak_owned_values},
                    {"remaining_owned_values", std::uint64_t{report.remaining_owned_values}},
                    {"resources_cleared", report.resources_cleared},
                    {"timing", report.timing},
                    {"active_time_ns", std::int64_t{report.active_time.count()}}};
    for (std::size_t i = 0; i < counters.size(); ++i)
        entry.fields.add({String::concat("counter.", String::validated(counters[i])), report.calls.values[i]});
    const auto resources = [&](const char* prefix, const source::DiagnosticResources& value) {
        const auto p = String::validated(prefix);
        entry.fields.add({String::concat(p, ".objects"), std::uint64_t{value.objects}});
        entry.fields.add({String::concat(p, ".roots"), std::uint64_t{value.roots}});
        entry.fields.add(
            {String::concat(p, ".pending_reclamation"), std::uint64_t{value.pending_reclamation}});
        entry.fields.add(
            {String::concat(p, ".collection_phase"), static_cast<std::uint64_t>(value.collection_phase)});
    };
    resources("before", report.before);
    resources("after", report.after);
    if (report.location) {
        entry.source = diagnostic_source(report.location->source);
        entry.source.function = report.location->function;
    }
    if (report.error) {
        entry.fields.add({"error_code", static_cast<std::uint64_t>(report.error->code)});
        // Error and suspended/current locations are independently meaningful.
        if (report.error->source) {
            const auto& location = *report.error->source;
            entry.fields.append({{"error.path", location.path},
                                 {"error.version", location.version},
                                 {"error.offset", std::uint64_t{location.offset}},
                                 {"error.line", std::uint64_t{location.line}},
                                 {"error.column", std::uint64_t{location.column}},
                                 {"error.disk_path", location.disk_path}});
            if (!report.location)
                entry.source = diagnostic_source(location);
        }
        if (report.error->element)
            error_element(entry, *report.error->element);
    }
    static_cast<void>(logging::submit(std::move(entry)));
}
}

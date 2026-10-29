#include "logging/format.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace atelier::logging {
std::string_view name(Severity severity) noexcept {
    constexpr std::array names{"trace", "debug", "info", "warning", "error", "critical", "off"};
    const auto index = static_cast<std::size_t>(severity);
    return index < names.size() ? names[index] : "unknown";
}
}

namespace atelier::logging::detail {
namespace {
[[noreturn]] void invalid(const char* message) { throw FormatError{ReaderFault::invalid_record, message}; }
void members(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object() || value.size() != names.size()) invalid("Unexpected object members");
    for (auto key : names) if (!value.contains(std::string{key})) invalid("Missing object member");
}
std::string text(const Json& value) {
    if (!value.is_string()) invalid("Expected UTF-8 string");
    auto result = value.get<std::string>();
    if (!valid_utf8(result)) invalid("Invalid UTF-8");
    return result;
}
template<class T> T integer(const Json& value) {
    const auto encoded = text(value);
    T result{};
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size() ||
        std::to_string(result) != encoded) invalid("Noncanonical or out-of-range integer");
    return result;
}
std::uint32_t number(const Json& value) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) invalid("Expected unsigned JSON integer");
    if (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0)
        invalid("Negative unsigned integer");
    const auto result = value.get<std::uint64_t>();
    if (result > (std::numeric_limits<std::uint32_t>::max)()) invalid("Integer exceeds uint32");
    return static_cast<std::uint32_t>(result);
}
bool boolean(const Json& value) {
    if (!value.is_boolean()) invalid("Expected boolean");
    return value.get<bool>();
}
Severity severity(const Json& value) {
    const auto spelling = text(value);
    for (unsigned i = 0; i < static_cast<unsigned>(Severity::off); ++i)
        if (name(static_cast<Severity>(i)) == spelling) return static_cast<Severity>(i);
    invalid("Invalid record severity");
}
bool identifier(std::string_view value, std::size_t limit) noexcept {
    if (value.empty() || value.size() > limit) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}
bool bounded_text(std::string_view value, std::size_t limit, bool nul = false) noexcept {
    return value.size() <= limit && valid_utf8(value) && (nul || value.find('\0') == std::string_view::npos);
}
Json optional_integer(const std::optional<std::uint64_t>& value) {
    return value ? Json(std::to_string(*value)) : Json{};
}
std::optional<std::uint64_t> read_optional_integer(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional{integer<std::uint64_t>(value)};
}
constexpr std::array fault_names{"encoding_failed", "output_too_large", "invalid_utf8",
    "file_open_failed", "file_write_failed", "file_flush_failed", "rotation_failed",
    "retention_failed", "summary_failed", "allocation_failed", "lifecycle_violation"};
constexpr std::array state_names{"running", "finishing", "finished"};
template<class T, std::size_t N> T enumeration(const Json& value, const std::array<const char*, N>& names) {
    const auto spelling = text(value);
    for (std::size_t i = 0; i < N; ++i) if (names[i] == spelling) return static_cast<T>(i);
    invalid("Unknown enum spelling");
}
Json capture_json(const CapturedDiagnostic& value) {
    return {{"entry", entry_json(value.entry)}, {"producer_id", std::to_string(value.producer_id)},
        {"producer_sequence", std::to_string(value.producer_sequence)},
        {"message_is_template", value.message_is_template}, {"truncated", value.truncated}};
}
CapturedDiagnostic read_capture(const Json& json) {
    members(json, {"entry", "producer_id", "producer_sequence", "message_is_template", "truncated"});
    CapturedDiagnostic value;
    value.entry = read_entry(json.at("entry"));
    value.producer_id = integer<std::uint64_t>(json.at("producer_id"));
    value.producer_sequence = integer<std::uint64_t>(json.at("producer_sequence"));
    value.message_is_template = boolean(json.at("message_is_template"));
    value.truncated = boolean(json.at("truncated"));
    return value;
}
}

bool valid_utf8(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        unsigned count{}, code{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (count > value.size() - i) return false;
        for (unsigned n = 0; n < count; ++n) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
std::string path_text(const std::filesystem::path& path) {
    auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::filesystem::path text_path(std::string_view value) {
    if (!valid_utf8(value) || value.find('\0') != std::string_view::npos) invalid("Invalid UTF-8 path");
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}
std::uint64_t unix_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
}
namespace atelier::logging {
std::string_view entry_problem(const Entry& value) {
    using namespace detail;
    if (value.severity >= Severity::off || !identifier(value.category, 64) ||
        !identifier(value.event_code, 128)) return "Invalid category, event code or severity";
    if (!valid_utf8(value.message)) return "Invalid UTF-8 message";
    const auto& source = value.source;
    if (!bounded_text(source.file, 4096) || !bounded_text(source.root, 4096) ||
        !bounded_text(source.function, 4096) || !bounded_text(source.disk_path, 4096)) return "Invalid source text";
    if ((!source.root.empty() && !text_path(source.root).is_absolute()) ||
        (!source.disk_path.empty() && !text_path(source.disk_path).is_absolute())) return "Source mapping is not absolute";
    if (value.fields.size() > maximum_fields) return "Too many fields";
    for (std::size_t i = 0; i < value.fields.size(); ++i) {
        const auto& field = value.fields[i];
        if (!identifier(field.key, 48)) return "Invalid field name";
        for (std::size_t j = 0; j < i; ++j)
            if (value.fields[j].key == field.key) return "Duplicate field name";
        if (const auto* string = std::get_if<std::string>(&field.value);
            string && !bounded_text(*string, 4096, true)) return "Invalid string field";
        if (const auto* real = std::get_if<double>(&field.value); real && !std::isfinite(*real))
            return "Nonfinite field";
    }
    if (value.repeat && (value.repeat->identity.empty() ||
        !bounded_text(value.repeat->identity, 256) || value.repeat->count == 0)) return "Invalid repeat group";
    return {};
}
}
namespace atelier::logging::detail {
Json parse_json(std::string_view value) {
    if (value.starts_with("\xef\xbb\xbf")) throw FormatError{ReaderFault::invalid_json, "Logging JSON must not contain a BOM"};
    std::vector<std::unordered_set<std::string>> objects;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 16) throw FormatError{ReaderFault::invalid_json, "JSON nesting exceeds logging schema limit"};
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(parsed.get<std::string>()).second)
            throw FormatError{ReaderFault::invalid_json, "Duplicate JSON key"};
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    };
    return Json::parse(value.begin(), value.end(), callback);
}
Json entry_json(const Entry& value, bool omit_message) {
    const auto& source = value.source;
    Json fields = Json::object();
    for (const auto& field : value.fields) {
        Json encoded;
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) encoded = {{"type", "bool"}, {"value", item}};
            else if constexpr (std::is_same_v<T, std::int64_t>) encoded = {{"type", "i64"}, {"value", std::to_string(item)}};
            else if constexpr (std::is_same_v<T, std::uint64_t>) encoded = {{"type", "u64"}, {"value", std::to_string(item)}};
            else if constexpr (std::is_same_v<T, double>) encoded = {{"type", "f64"}, {"value", item}};
            else encoded = {{"type", "string"}, {"value", item}};
        }, field.value);
        fields[field.key] = std::move(encoded);
    }
    Json repeat;
    if (value.repeat) repeat = {{"identity", value.repeat->identity}, {"count", std::to_string(value.repeat->count)}};
    return {{"severity", name(value.severity)}, {"category", value.category}, {"event_code", value.event_code},
        {"message", omit_message ? std::string_view{} : std::string_view{value.message}},
        {"source", {{"file", source.file}, {"root", source.root}, {"function", source.function},
            {"line", source.line}, {"column", source.column}, {"version", optional_integer(source.version)},
            {"offset", optional_integer(source.offset)}, {"disk_path", source.disk_path}}}, {"fields", std::move(fields)}, {"repeat", std::move(repeat)}};
}
namespace {
Entry read_entry_fields(const Json& json) {
    Entry value;
    value.severity = severity(json.at("severity"));
    value.category = text(json.at("category")); value.event_code = text(json.at("event_code"));
    value.message = text(json.at("message"));
    const auto& source = json.at("source");
    members(source, {"file", "root", "function", "line", "column", "version", "offset", "disk_path"});
    value.source = {text(source.at("file")), text(source.at("root")), text(source.at("function")),
        number(source.at("line")), number(source.at("column")), read_optional_integer(source.at("version")),
        read_optional_integer(source.at("offset")), text(source.at("disk_path"))};
    const auto& fields = json.at("fields");
    if (!fields.is_object() || fields.size() > maximum_fields) invalid("Invalid fields object");
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        members(it.value(), {"type", "value"});
        const auto type = text(it.value().at("type"));
        const auto& item = it.value().at("value");
        FieldValue field;
        if (type == "bool") field = boolean(item);
        else if (type == "i64") field = integer<std::int64_t>(item);
        else if (type == "u64") field = integer<std::uint64_t>(item);
        else if (type == "f64") {
            if (!item.is_number()) invalid("Expected float field");
            field = item.get<double>();
        } else if (type == "string") field = text(item);
        else invalid("Unknown field type");
        value.fields.push_back({it.key(), std::move(field)});
    }
    if (const auto& repeat = json.at("repeat"); !repeat.is_null()) {
        members(repeat, {"identity", "count"});
        value.repeat = Repeat{text(repeat.at("identity")), integer<std::uint64_t>(repeat.at("count"))};
    }
    if (const auto problem = entry_problem(value); !problem.empty()) invalid(problem.data());
    if (value.message.size() > maximum_message_bytes) invalid("Message exceeds limit");
    return value;
}
}
Entry read_entry(const Json& json) {
    members(json, {"severity", "category", "event_code", "message", "source", "fields", "repeat"});
    return read_entry_fields(json);
}
Json record_envelope(const Record& value, const Entry& entry) {
    Json json = entry_json(entry, true);
    json.update({{"schema", schema_version}, {"run_id", value.run_id}, {"process_id", value.process_id},
        {"thread_id", std::to_string(value.thread_id)}, {"producer_id", std::to_string(value.producer_id)},
        {"producer_sequence", std::to_string(value.producer_sequence)},
        {"record_sequence", std::to_string(value.record_sequence)}, {"timestamp_unix_ns", std::to_string(value.timestamp_unix_ns)}});
    return json;
}
Record read_record(const Json& json) {
    members(json, {"schema", "run_id", "process_id", "thread_id", "producer_id", "producer_sequence",
        "record_sequence", "timestamp_unix_ns", "severity", "category", "event_code", "message", "source", "fields", "repeat"});
    if (number(json.at("schema")) != schema_version) throw FormatError{ReaderFault::unsupported_schema, "Unsupported logging schema"};
    Record value;
    value.run_id = text(json.at("run_id")); value.process_id = number(json.at("process_id"));
    value.thread_id = integer<std::uint64_t>(json.at("thread_id"));
    value.producer_id = integer<std::uint64_t>(json.at("producer_id"));
    value.producer_sequence = integer<std::uint64_t>(json.at("producer_sequence"));
    value.record_sequence = integer<std::uint64_t>(json.at("record_sequence"));
    value.timestamp_unix_ns = integer<std::uint64_t>(json.at("timestamp_unix_ns"));
    if (!bounded_text(value.run_id, 128) || value.run_id.empty() || !value.process_id || !value.producer_id ||
        !value.producer_sequence || !value.record_sequence) invalid("Invalid record identity");
    value.entry = read_entry_fields(json);
    value.persistence = Persistence::jsonl;
    return value;
}
Json run_json(const RunInfo& value) {
    return {{"schema", schema_version}, {"run", {{"run_id", value.run_id}, {"process_name", value.process_name},
        {"process_id", value.process_id}, {"started_unix_ns", std::to_string(value.started_unix_ns)},
        {"directory", path_text(value.directory)}, {"parent_run_id", value.parent_run_id ? Json(*value.parent_run_id) : Json{}}}}};
}
RunInfo read_run(const Json& json) {
    members(json, {"schema", "run"});
    if (number(json.at("schema")) != schema_version) throw FormatError{ReaderFault::unsupported_schema, "Unsupported run schema"};
    const auto& run = json.at("run");
    members(run, {"run_id", "process_name", "process_id", "started_unix_ns", "directory", "parent_run_id"});
    RunInfo value{text(run.at("run_id")), text(run.at("process_name")), number(run.at("process_id")),
        integer<std::uint64_t>(run.at("started_unix_ns")), text_path(text(run.at("directory"))), {}};
    if (!run.at("parent_run_id").is_null()) value.parent_run_id = text(run.at("parent_run_id"));
    if (!identifier(value.run_id, 128) || !value.process_id || !value.directory.is_absolute() ||
        !bounded_text(value.process_name, 128) || value.process_name.empty() ||
        (value.parent_run_id && !identifier(*value.parent_run_id, 128))) invalid("Invalid run manifest");
    return value;
}
Json health_json(const RunInfo& info, const PublishedHealth& value) {
    const auto& health = value.report.health;
    Json state{{"state", state_names[static_cast<std::size_t>(health.state)]}};
#define ATELIER_HEALTH_WRITE(member) state[#member] = std::to_string(health.member)
    ATELIER_HEALTH_WRITE(accepted); ATELIER_HEALTH_WRITE(rejected_queue_full);
    ATELIER_HEALTH_WRITE(rejected_producer_limit); ATELIER_HEALTH_WRITE(rejected_record_too_large);
    ATELIER_HEALTH_WRITE(rejected_invalid_record); ATELIER_HEALTH_WRITE(rejected_internal_failure);
    ATELIER_HEALTH_WRITE(facility_failures); ATELIER_HEALTH_WRITE(backend_notifications);
    ATELIER_HEALTH_WRITE(last_record_sequence);
#undef ATELIER_HEALTH_WRITE
    state["first_backend_notification"] = health.first_backend_notification;
    state["first_error"] = health.first_error ? capture_json(*health.first_error) : Json{};
    Json issue;
    if (health.first_facility_issue) {
        const auto& first = *health.first_facility_issue;
        issue = {{"fault", fault_names[static_cast<std::size_t>(first.fault)]}, {"message", first.message},
            {"native_code", first.native_code ? Json(*first.native_code) : Json{}}};
    }
    state["first_facility_issue"] = std::move(issue);
    return {{"schema", schema_version}, {"run_id", info.run_id}, {"process_id", info.process_id},
        {"updated_unix_ns", std::to_string(value.updated_unix_ns)}, {"final", value.final},
        {"report", {{"health", std::move(state)}, {"backend_acknowledged", value.report.backend_acknowledged},
            {"sink_flush_succeeded", value.report.sink_flush_succeeded}, {"summary_published", value.report.summary_published}}}};
}
PublishedHealth read_health(const Json& json, const RunInfo& info) {
    members(json, {"schema", "run_id", "process_id", "updated_unix_ns", "final", "report"});
    if (number(json.at("schema")) != schema_version || text(json.at("run_id")) != info.run_id ||
        number(json.at("process_id")) != info.process_id) invalid("Health identity/schema mismatch");
    PublishedHealth value;
    value.updated_unix_ns = integer<std::uint64_t>(json.at("updated_unix_ns"));
    value.final = boolean(json.at("final"));
    const auto& report = json.at("report");
    members(report, {"health", "backend_acknowledged", "sink_flush_succeeded", "summary_published"});
    value.report.backend_acknowledged = boolean(report.at("backend_acknowledged"));
    value.report.sink_flush_succeeded = boolean(report.at("sink_flush_succeeded"));
    value.report.summary_published = boolean(report.at("summary_published"));
    const auto& state = report.at("health");
    members(state, {"state", "accepted", "rejected_queue_full", "rejected_producer_limit", "rejected_record_too_large",
        "rejected_invalid_record", "rejected_internal_failure", "facility_failures", "backend_notifications",
        "last_record_sequence", "first_backend_notification", "first_error", "first_facility_issue"});
    auto& health = value.report.health;
    health.state = enumeration<RunState>(state.at("state"), state_names);
#define ATELIER_HEALTH_READ(member) health.member = integer<std::uint64_t>(state.at(#member))
    ATELIER_HEALTH_READ(accepted); ATELIER_HEALTH_READ(rejected_queue_full);
    ATELIER_HEALTH_READ(rejected_producer_limit); ATELIER_HEALTH_READ(rejected_record_too_large);
    ATELIER_HEALTH_READ(rejected_invalid_record); ATELIER_HEALTH_READ(rejected_internal_failure);
    ATELIER_HEALTH_READ(facility_failures); ATELIER_HEALTH_READ(backend_notifications);
    ATELIER_HEALTH_READ(last_record_sequence);
#undef ATELIER_HEALTH_READ
    health.first_backend_notification = text(state.at("first_backend_notification"));
    if (!state.at("first_error").is_null()) health.first_error = read_capture(state.at("first_error"));
    if (const auto& first = state.at("first_facility_issue"); !first.is_null()) {
        members(first, {"fault", "message", "native_code"});
        health.first_facility_issue = FacilityIssue{enumeration<FacilityFault>(first.at("fault"), fault_names), text(first.at("message")), {}};
        if (!first.at("native_code").is_null()) health.first_facility_issue->native_code = number(first.at("native_code"));
    }
    if (value.final && health.state != RunState::finished) invalid("Final summary without finished state");
    if (!value.report.summary_published) invalid("Unpublished health snapshot");
    return value;
}
}

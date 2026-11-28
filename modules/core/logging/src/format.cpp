#include "detail/format.hpp"

#include "foundation/string.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>

namespace atelier::logging {
StringView name(Severity severity) noexcept {
    constexpr std::array<StringView, 7> names{"trace", "debug", "info", "warning", "error", "critical", "off"};
    const auto index = static_cast<std::size_t>(severity);
    return index < names.size() ? names[index] : "unknown";
}
}

namespace atelier::logging::detail {
FormatError::FormatError(json::Error error)
    : fault(error.fault == json::Fault::syntax || error.fault == json::Fault::depth ||
                    error.fault == json::Fault::duplicate_key
                ? ReaderFault::invalid_json
                : ReaderFault::invalid_record),
      message(error.describe()) {}

bool valid_utf8(StringView value) noexcept {
    return value.is_valid_utf8();
}

FormatResult<String> path_text(const std::filesystem::path& path) {
    auto value = platform::utf16_to_utf8(path.generic_wstring());
    if (!value)
        return std::unexpected(
            FormatError{ReaderFault::invalid_record, "Invalid UTF-16 path"_s, value.error().system_code});
    return std::move(*value);
}

std::uint64_t unix_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

namespace {
bool identifier(StringView value, std::size_t limit) noexcept {
    return !value.empty() && value.size() <= limit && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-';
    });
}

bool bounded(StringView value, std::size_t limit, bool nul = false) noexcept {
    return value.size() <= limit && (nul || !value.contains('\0'));
}

StringView entry_shape(const Entry& value) {
    if (value.severity >= Severity::off || !identifier(value.category, 64) || !identifier(value.event_code, 128))
        return "Invalid category, event code or severity";
    const auto& source = value.source;
    if (!bounded(source.file, 4096) || !bounded(source.root, 4096) || !bounded(source.function, 4096) ||
        !bounded(source.disk_path, 4096))
        return "Invalid source text";
    // UTF-8 is guaranteed by String and the JSON parser.
    const auto absolute = [](StringView text) {
        return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(text.data()), text.size()}}
            .is_absolute();
    };
    if ((!source.root.empty() && !absolute(source.root)) || (!source.disk_path.empty() && !absolute(source.disk_path)))
        return "Source mapping is not absolute";
    for (const auto& field : value.fields) {
        if (!identifier(field.key, 48))
            return "Invalid field name";
        if (const auto* text = std::get_if<String>(&field.value); text && !bounded(*text, 4096, true))
            return "Invalid string field";
    }
    if (value.repeat &&
        (value.repeat->identity.empty() || !bounded(value.repeat->identity, 256) || !value.repeat->count))
        return "Invalid repeat group";
    return {};
}

std::unexpected<FormatError> invalid(StringView message) {
    return std::unexpected(
        FormatError{ReaderFault::invalid_record, String::from_utf8(message).value_or("Invalid record"_s)});
}

template <class T>
FormatResult<T> integer(json::Value value, StringView key) {
    ATELIER_TRY_BIND(node, value.at(key));
    ATELIER_TRY_BIND(encoded, node->string());
    T result{};
    const auto parsed = std::from_chars(encoded->data(), encoded->data() + encoded->size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded->data() + encoded->size() ||
        String::from_number(result) != *encoded)
        return std::unexpected(node->error(json::Fault::range, "Noncanonical or out-of-range integer"));
    return result;
}

FormatResult<Optional<std::uint64_t>> optional_integer(json::Value value, StringView key) {
    ATELIER_TRY_BIND(node, value.at(key));
    if (node->is_null())
        return Optional<std::uint64_t>{};
    ATELIER_TRY_BIND(number, integer<std::uint64_t>(value, key));
    return *number;
}

FormatResult<void> optional_integer(json::Builder& builder, json::Value parent, StringView key,
                                    const Optional<std::uint64_t>& value) {
    if (value)
        return builder.set(parent, key, String::from_number(*value));
    return builder.set_null(parent, key);
}

constexpr std::array fault_names{"encoding_failed",   "output_too_large",  "invalid_utf8",       "file_open_failed",
                                 "file_write_failed", "file_flush_failed", "rotation_failed",    "retention_failed",
                                 "summary_failed",    "allocation_failed", "lifecycle_violation"};

constexpr std::array state_names{"running", "finishing", "finished"};

template <class T, std::size_t N>
FormatResult<T> enumeration(json::Value value, StringView key, const std::array<const char*, N>& names) {
    ATELIER_TRY_BIND(text, value.string({key}));
    for (std::size_t i = 0; i < N; ++i)
        if (*text == names[i])
            return static_cast<T>(i);
    return invalid("Unknown enum spelling");
}

FormatResult<void> entry_json(json::Builder& builder, json::Value root, const Entry& value, bool omit_message) {
    // entry_problem validated this Entry before admission; capture only copies
    // whole code points. Neither path mutates these bytes before encoding.
    builder.set_validated(root, "severity", name(value.severity));
    builder.set_validated(root, "category", value.category);
    builder.set_validated(root, "event_code", value.event_code);
    builder.set_validated(root, "message", omit_message ? StringView{} : value.message.view());
    ATELIER_TRY_BIND(source, builder.object(root, "source"));
    builder.set_validated(*source, "file", value.source.file);
    builder.set_validated(*source, "root", value.source.root);
    builder.set_validated(*source, "function", value.source.function);
    ATELIER_TRY(builder.set(*source, "line", value.source.line));
    ATELIER_TRY(builder.set(*source, "column", value.source.column));
    ATELIER_TRY(optional_integer(builder, *source, "version", value.source.version));
    ATELIER_TRY(optional_integer(builder, *source, "offset", value.source.offset));
    builder.set_validated(*source, "disk_path", value.source.disk_path);
    ATELIER_TRY_BIND(fields, builder.object(root, "fields"));
    for (const auto& field : value.fields) {
        const auto encoded = builder.object_validated(*fields, field.key);
        const auto result = std::visit(
            [&](const auto& item) -> FormatResult<void> {
                using T = std::decay_t<decltype(item)>;
                StringView type;
                if constexpr (std::is_same_v<T, bool>)
                    type = "bool";
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    type = "i64";
                else if constexpr (std::is_same_v<T, std::uint64_t>)
                    type = "u64";
                else if constexpr (std::is_same_v<T, double>)
                    type = "f64";
                else
                    type = "string";
                builder.set_validated(encoded, "type", type);
                if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>) {
                    builder.set_validated(encoded, "value", String::from_number(item));
                    return {};
                } else if constexpr (std::is_same_v<T, String>) {
                    builder.set_validated(encoded, "value", item);
                    return {};
                } else
                    return builder.set(encoded, "value", item);
            },
            field.value);
        ATELIER_TRY(result);
    }
    if (value.repeat) {
        ATELIER_TRY_BIND(repeat, builder.object(root, "repeat"));
        builder.set_validated(*repeat, "identity", value.repeat->identity);
        builder.set_validated(*repeat, "count", String::from_number(value.repeat->count));
    } else
        ATELIER_TRY(builder.set_null(root, "repeat"));
    return {};
}

FormatResult<Entry> read_entry_fields(json::Value root) {
    Entry value;
    ATELIER_TRY_BIND(severity, root.string({"severity"}));
    value.severity = Severity::off;
    for (unsigned i = 0; i < static_cast<unsigned>(Severity::off); ++i)
        if (name(static_cast<Severity>(i)) == *severity)
            value.severity = static_cast<Severity>(i);
    ATELIER_TRY_BIND(category, root.string({"category"}));
    value.category = String::from_utf8_validated(*category);
    ATELIER_TRY_BIND(event, root.string({"event_code"}));
    value.event_code = String::from_utf8_validated(*event);
    ATELIER_TRY_BIND(message, root.string({"message"}));
    value.message = String::from_utf8_validated(*message);
    ATELIER_TRY_BIND(source, root.at("source"));
    ATELIER_TRY(source->members({"file", "root", "function", "line", "column", "version", "offset", "disk_path"}));
    ATELIER_TRY_BIND(file, source->string({"file"}));
    ATELIER_TRY_BIND(origin, source->string({"root"}));
    ATELIER_TRY_BIND(function, source->string({"function"}));
    ATELIER_TRY_BIND(line, source->u32({"line"}));
    ATELIER_TRY_BIND(column, source->u32({"column"}));
    ATELIER_TRY_BIND(version, optional_integer(*source, "version"));
    ATELIER_TRY_BIND(offset, optional_integer(*source, "offset"));
    ATELIER_TRY_BIND(disk, source->string({"disk_path"}));
    value.source = {String::from_utf8_validated(*file),     String::from_utf8_validated(*origin),
                    String::from_utf8_validated(*function), *line,
                    *column,                                *version,
                    *offset,                                String::from_utf8_validated(*disk)};
    ATELIER_TRY_BIND(fields, root.at("fields"));
    if (fields->size() > maximum_fields)
        return invalid("Invalid fields object");
    ATELIER_TRY_BIND(items, fields->object_items());
    for (const auto& [key, encoded] : *items) {
        ATELIER_TRY(encoded.members({"type", "value"}));
        ATELIER_TRY_BIND(type, encoded.string({"type"}));
        FieldValue field;
        if (*type == "bool") {
            ATELIER_TRY_BIND(item, encoded.boolean({"value"}));
            field = *item;
        } else if (*type == "i64") {
            ATELIER_TRY_BIND(item, integer<std::int64_t>(encoded, "value"));
            field = *item;
        } else if (*type == "u64") {
            ATELIER_TRY_BIND(item, integer<std::uint64_t>(encoded, "value"));
            field = *item;
        } else if (*type == "f64") {
            ATELIER_TRY_BIND(item, encoded.number({"value"}));
            field = *item;
        } else if (*type == "string") {
            ATELIER_TRY_BIND(item, encoded.string({"value"}));
            field = String::from_utf8_validated(*item);
        } else
            return invalid("Unknown field type");
        value.fields.add({String::from_utf8_validated(key), std::move(field)});
    }
    // The old ordered DOM exposed fields in byte order, independent of input.
    std::ranges::sort(value.fields, {}, &Field::key);
    ATELIER_TRY_BIND(repeat, root.at("repeat"));
    if (!repeat->is_null()) {
        ATELIER_TRY(repeat->members({"identity", "count"}));
        ATELIER_TRY_BIND(identity, repeat->string({"identity"}));
        ATELIER_TRY_BIND(count, integer<std::uint64_t>(*repeat, "count"));
        value.repeat = Repeat{String::from_utf8_validated(*identity), *count};
    }
    if (auto problem = entry_shape(value); !problem.empty())
        return invalid(problem);
    if (value.message.size() > maximum_message_bytes)
        return invalid("Message exceeds limit");
    return value;
}

FormatResult<CapturedDiagnostic> read_capture(json::Value root) {
    ATELIER_TRY(root.members({"entry", "producer_id", "producer_sequence", "message_is_template", "truncated"}));
    ATELIER_TRY_BIND(entry, root.at("entry"));
    ATELIER_TRY(entry->members({"severity", "category", "event_code", "message", "source", "fields", "repeat"}));
    ATELIER_TRY_BIND(decoded, read_entry_fields(*entry));
    ATELIER_TRY_BIND(producer, integer<std::uint64_t>(root, "producer_id"));
    ATELIER_TRY_BIND(sequence, integer<std::uint64_t>(root, "producer_sequence"));
    ATELIER_TRY_BIND(is_template, root.boolean({"message_is_template"}));
    ATELIER_TRY_BIND(truncated, root.boolean({"truncated"}));
    return CapturedDiagnostic{std::move(*decoded), *producer, *sequence, *is_template, *truncated};
}

FormatResult<void> validate_run(const RunInfo& value) {
    if (!identifier(value.run_id, 128) || !value.process_id || !value.directory.is_absolute() ||
        !bounded(value.process_name, 128) || value.process_name.empty() ||
        (value.parent_run_id && !identifier(*value.parent_run_id, 128)))
        return invalid("Invalid run manifest");
    return {};
}
}
}

namespace atelier::logging {
StringView entry_problem(const Entry& value) {
    using namespace detail;
    if (value.fields.size() > maximum_fields)
        return "Too many fields";
    for (std::size_t i = 0; i < value.fields.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j)
            if (value.fields[j].key == value.fields[i].key)
                return "Duplicate field name";
        if (const auto* real = std::get_if<double>(&value.fields[i].value); real && !std::isfinite(*real))
            return "Nonfinite field";
    }
    return entry_shape(value);
}
}

namespace atelier::logging::detail {
FormatResult<json::Builder> record_envelope(const Record& value, const Entry& entry) {
    json::Builder builder;
    auto root = builder.root();
    ATELIER_TRY(entry_json(builder, root, entry, true));
    ATELIER_TRY(builder.set(root, "schema", schema_version));
    ATELIER_TRY(builder.set(root, "run_id", value.run_id));
    ATELIER_TRY(builder.set(root, "process_id", value.process_id));
    ATELIER_TRY(builder.set(root, "thread_id", String::from_number(value.thread_id)));
    ATELIER_TRY(builder.set(root, "producer_id", String::from_number(value.producer_id)));
    ATELIER_TRY(builder.set(root, "producer_sequence", String::from_number(value.producer_sequence)));
    ATELIER_TRY(builder.set(root, "record_sequence", String::from_number(value.record_sequence)));
    ATELIER_TRY(builder.set(root, "timestamp_unix_ns", String::from_number(value.timestamp_unix_ns)));
    return builder;
}

FormatResult<Record> read_record(StringView bytes) {
    ATELIER_TRY_BIND(document, json::Document::parse(bytes, {16}));
    auto root = document->root();
    ATELIER_TRY(root.members({"schema", "run_id", "process_id", "thread_id", "producer_id", "producer_sequence",
                              "record_sequence", "timestamp_unix_ns", "severity", "category", "event_code", "message",
                              "source", "fields", "repeat"}));
    ATELIER_TRY_BIND(schema, root.u32({"schema"}));
    if (*schema != schema_version)
        return std::unexpected(FormatError{ReaderFault::unsupported_schema, "Unsupported logging schema"_s});
    Record value;
    ATELIER_TRY_BIND(run, root.string({"run_id"}));
    value.run_id = String::from_utf8_validated(*run);
    ATELIER_TRY_BIND(process, root.u32({"process_id"}));
    value.process_id = *process;
    ATELIER_TRY_BIND(thread, integer<std::uint64_t>(root, "thread_id"));
    value.thread_id = *thread;
    ATELIER_TRY_BIND(producer, integer<std::uint64_t>(root, "producer_id"));
    value.producer_id = *producer;
    ATELIER_TRY_BIND(sequence, integer<std::uint64_t>(root, "producer_sequence"));
    value.producer_sequence = *sequence;
    ATELIER_TRY_BIND(record, integer<std::uint64_t>(root, "record_sequence"));
    value.record_sequence = *record;
    ATELIER_TRY_BIND(time, integer<std::uint64_t>(root, "timestamp_unix_ns"));
    value.timestamp_unix_ns = *time;
    if (!bounded(value.run_id, 128) || value.run_id.empty() || !value.process_id || !value.producer_id ||
        !value.producer_sequence || !value.record_sequence)
        return invalid("Invalid record identity");
    ATELIER_TRY_BIND(entry, read_entry_fields(root));
    value.entry = std::move(*entry);
    value.persistence = Persistence::jsonl;
    return value;
}

FormatResult<json::Builder> run_json(const RunInfo& value) {
    ATELIER_TRY(validate_run(value));
    json::Builder builder;
    ATELIER_TRY(builder.set(builder.root(), "schema", schema_version));
    ATELIER_TRY_BIND(run, builder.object(builder.root(), "run"));
    ATELIER_TRY(builder.set(*run, "run_id", value.run_id));
    ATELIER_TRY(builder.set(*run, "process_name", value.process_name));
    ATELIER_TRY(builder.set(*run, "process_id", value.process_id));
    ATELIER_TRY(builder.set(*run, "started_unix_ns", String::from_number(value.started_unix_ns)));
    ATELIER_TRY_BIND(directory, path_text(value.directory));
    ATELIER_TRY(builder.set(*run, "directory", *directory));
    if (value.parent_run_id)
        ATELIER_TRY(builder.set(*run, "parent_run_id", *value.parent_run_id));
    else
        ATELIER_TRY(builder.set_null(*run, "parent_run_id"));
    return builder;
}

FormatResult<RunInfo> read_run(StringView bytes) {
    ATELIER_TRY_BIND(document, json::Document::parse(bytes, {16}));
    auto root = document->root();
    ATELIER_TRY(root.members({"schema", "run"}));
    ATELIER_TRY_BIND(schema, root.u32({"schema"}));
    if (*schema != schema_version)
        return std::unexpected(FormatError{ReaderFault::unsupported_schema, "Unsupported run schema"_s});
    ATELIER_TRY_BIND(run, root.at("run"));
    ATELIER_TRY(
        run->members({"run_id", "process_name", "process_id", "started_unix_ns", "directory", "parent_run_id"}));
    ATELIER_TRY_BIND(id, run->string({"run_id"}));
    ATELIER_TRY_BIND(name, run->string({"process_name"}));
    ATELIER_TRY_BIND(process, run->u32({"process_id"}));
    ATELIER_TRY_BIND(start, integer<std::uint64_t>(*run, "started_unix_ns"));
    ATELIER_TRY_BIND(directory, run->string({"directory"}));
    if (directory->contains('\0'))
        return invalid("Invalid UTF-8 path");
    RunInfo value{
        String::from_utf8_validated(*id),
        String::from_utf8_validated(*name),
        *process,
        *start,
        std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(directory->data()), directory->size()}},
        {}};
    ATELIER_TRY_BIND(parent, run->at("parent_run_id"));
    if (!parent->is_null()) {
        ATELIER_TRY_BIND(text, parent->string());
        value.parent_run_id = String::from_utf8_validated(*text);
    }
    ATELIER_TRY(validate_run(value));
    return value;
}

FormatResult<json::Builder> health_json(const RunInfo& info, const PublishedHealth& value) {
    json::Builder builder;
    auto root = builder.root();
    ATELIER_TRY(builder.set(root, "schema", schema_version));
    ATELIER_TRY(builder.set(root, "run_id", info.run_id));
    ATELIER_TRY(builder.set(root, "process_id", info.process_id));
    ATELIER_TRY(builder.set(root, "updated_unix_ns", String::from_number(value.updated_unix_ns)));
    ATELIER_TRY(builder.set(root, "final", value.final));
    ATELIER_TRY_BIND(report, builder.object(root, "report"));
    ATELIER_TRY(builder.set(*report, "backend_acknowledged", value.report.backend_acknowledged));
    ATELIER_TRY(builder.set(*report, "sink_flush_succeeded", value.report.sink_flush_succeeded));
    ATELIER_TRY(builder.set(*report, "summary_published", value.report.summary_published));
    ATELIER_TRY_BIND(state, builder.object(*report, "health"));
    const auto& health = value.report.health;
    ATELIER_TRY(builder.set(*state, "state", StringView{state_names[static_cast<std::size_t>(health.state)]}));
#define ATELIER_HEALTH_WRITE(member) ATELIER_TRY(builder.set(*state, #member, String::from_number(health.member)))
    ATELIER_HEALTH_WRITE(accepted);
    ATELIER_HEALTH_WRITE(rejected_queue_full);
    ATELIER_HEALTH_WRITE(rejected_producer_limit);
    ATELIER_HEALTH_WRITE(rejected_record_too_large);
    ATELIER_HEALTH_WRITE(rejected_invalid_record);
    ATELIER_HEALTH_WRITE(rejected_internal_failure);
    ATELIER_HEALTH_WRITE(facility_failures);
    ATELIER_HEALTH_WRITE(backend_notifications);
    ATELIER_HEALTH_WRITE(last_record_sequence);
#undef ATELIER_HEALTH_WRITE
    ATELIER_TRY(builder.set(*state, "first_backend_notification", health.first_backend_notification));
    if (health.first_error) {
        const auto& first = *health.first_error;
        ATELIER_TRY_BIND(capture, builder.object(*state, "first_error"));
        ATELIER_TRY_BIND(entry, builder.object(*capture, "entry"));
        ATELIER_TRY(entry_json(builder, *entry, first.entry, false));
        ATELIER_TRY(builder.set(*capture, "producer_id", String::from_number(first.producer_id)));
        ATELIER_TRY(builder.set(*capture, "producer_sequence", String::from_number(first.producer_sequence)));
        ATELIER_TRY(builder.set(*capture, "message_is_template", first.message_is_template));
        ATELIER_TRY(builder.set(*capture, "truncated", first.truncated));
    } else
        ATELIER_TRY(builder.set_null(*state, "first_error"));
    if (health.first_facility_issue) {
        const auto& first = *health.first_facility_issue;
        ATELIER_TRY_BIND(issue, builder.object(*state, "first_facility_issue"));
        ATELIER_TRY(builder.set(*issue, "fault", StringView{fault_names[static_cast<std::size_t>(first.fault)]}));
        ATELIER_TRY(builder.set(*issue, "message", first.message));
        if (first.native_code)
            ATELIER_TRY(builder.set(*issue, "native_code", *first.native_code));
        else
            ATELIER_TRY(builder.set_null(*issue, "native_code"));
    } else
        ATELIER_TRY(builder.set_null(*state, "first_facility_issue"));
    return builder;
}

FormatResult<PublishedHealth> read_health(StringView bytes, const RunInfo& info) {
    ATELIER_TRY_BIND(document, json::Document::parse(bytes, {16}));
    auto root = document->root();
    ATELIER_TRY(root.members({"schema", "run_id", "process_id", "updated_unix_ns", "final", "report"}));
    ATELIER_TRY_BIND(schema, root.u32({"schema"}));
    ATELIER_TRY_BIND(run, root.string({"run_id"}));
    ATELIER_TRY_BIND(process, root.u32({"process_id"}));
    if (*schema != schema_version || info.run_id != *run || *process != info.process_id)
        return invalid("Health identity/schema mismatch");
    PublishedHealth value;
    ATELIER_TRY_BIND(time, integer<std::uint64_t>(root, "updated_unix_ns"));
    value.updated_unix_ns = *time;
    ATELIER_TRY_BIND(final, root.boolean({"final"}));
    value.final = *final;
    ATELIER_TRY_BIND(report, root.at("report"));
    ATELIER_TRY(report->members({"health", "backend_acknowledged", "sink_flush_succeeded", "summary_published"}));
    ATELIER_TRY_BIND(acknowledged, report->boolean({"backend_acknowledged"}));
    value.report.backend_acknowledged = *acknowledged;
    ATELIER_TRY_BIND(flushed, report->boolean({"sink_flush_succeeded"}));
    value.report.sink_flush_succeeded = *flushed;
    ATELIER_TRY_BIND(published, report->boolean({"summary_published"}));
    value.report.summary_published = *published;
    ATELIER_TRY_BIND(state, report->at("health"));
    ATELIER_TRY(state->members({"state", "accepted", "rejected_queue_full", "rejected_producer_limit",
                                "rejected_record_too_large", "rejected_invalid_record", "rejected_internal_failure",
                                "facility_failures", "backend_notifications", "last_record_sequence",
                                "first_backend_notification", "first_error", "first_facility_issue"}));
    auto& health = value.report.health;
    ATELIER_TRY_BIND(run_state, enumeration<RunState>(*state, "state", state_names));
    health.state = *run_state;
#define ATELIER_HEALTH_READ(member)                                                                                    \
    ATELIER_TRY_BIND(member, integer<std::uint64_t>(*state, #member));                                                 \
    health.member = *member
    ATELIER_HEALTH_READ(accepted);
    ATELIER_HEALTH_READ(rejected_queue_full);
    ATELIER_HEALTH_READ(rejected_producer_limit);
    ATELIER_HEALTH_READ(rejected_record_too_large);
    ATELIER_HEALTH_READ(rejected_invalid_record);
    ATELIER_HEALTH_READ(rejected_internal_failure);
    ATELIER_HEALTH_READ(facility_failures);
    ATELIER_HEALTH_READ(backend_notifications);
    ATELIER_HEALTH_READ(last_record_sequence);
#undef ATELIER_HEALTH_READ
    ATELIER_TRY_BIND(notification, state->string({"first_backend_notification"}));
    health.first_backend_notification = String::from_utf8_validated(*notification);
    ATELIER_TRY_BIND(first_error, state->at("first_error"));
    if (!first_error->is_null()) {
        ATELIER_TRY_BIND(capture, read_capture(*first_error));
        health.first_error = std::move(*capture);
    }
    ATELIER_TRY_BIND(first, state->at("first_facility_issue"));
    if (!first->is_null()) {
        ATELIER_TRY(first->members({"fault", "message", "native_code"}));
        ATELIER_TRY_BIND(fault, enumeration<FacilityFault>(*first, "fault", fault_names));
        ATELIER_TRY_BIND(message, first->string({"message"}));
        health.first_facility_issue = FacilityIssue{*fault, String::from_utf8_validated(*message), {}};
        ATELIER_TRY_BIND(native, first->at("native_code"));
        if (!native->is_null()) {
            ATELIER_TRY_BIND(code, native->u32());
            health.first_facility_issue->native_code = *code;
        }
    }
    if (value.final && health.state != RunState::finished)
        return invalid("Final summary without finished state");
    if (!value.report.summary_published)
        return invalid("Unpublished health snapshot");
    return value;
}
}

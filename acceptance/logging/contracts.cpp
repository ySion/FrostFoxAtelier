#include "support.hpp"
#include "logging/native.hpp"
#include "json/json.hpp"
#include <limits>

namespace logging_acceptance {
namespace {
namespace json = atelier::json;

template <class T>
T decoded(json::Result<T> result) {
    require(result.has_value(), "JSON contract failed.");
    return std::move(*result);
}

std::vector<json::Document> read_json_lines(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(input.is_open(), "JSON consumer cannot open events.");
    std::vector<json::Document> result;
    for (std::string line; std::getline(input, line);) {
        require(!line.empty(), "JSONL contains an empty physical line.");
        result.push_back(decoded(json::Document::parse(line, {16})));
    }
    require(input.eof() && !input.bad(), "JSON consumer did not reach EOF.");
    return result;
}

void writer_contract() {
    const auto document = decoded(
        json::Document::parse(R"({"z":[{"b":false,"a":null},{}],"é":"中文/\u001f\n","a":18446744073709551615})", {16}));
    const auto compact = json::write(document.root());
    require(std::string_view{compact.data(), compact.size()} ==
                R"({"a":18446744073709551615,"z":[{"a":null,"b":false},{}],"é":"中文/\u001f\n"})",
            "Compact writer ordering, escaping or integer precision changed.");
    const auto pretty = json::write(document.root(), {true, true});
    require(
        std::string_view{pretty.data(), pretty.size()} ==
            "{\n  \"a\": 18446744073709551615,\n  \"z\": [\n    {\n      \"a\": null,\n      \"b\": false\n    },\n    {}\n  ],\n  \"é\": \"中文/\\u001f\\n\"\n}\n",
        "Pretty writer must use sorted keys, two spaces and one requested terminal LF.");
    json::Builder built;
    require(built.set(built.root(), "empty", std::string_view{}).has_value() &&
                built.set(built.root(), "literal", "text").has_value() &&
                built.set(built.root(), std::string_view{}, "key").has_value(),
            "String construction failed.");
    require(decoded(decoded(built.root().at(std::string_view{})).string()) == "key", "Empty key lookup failed.");
    const auto strings = json::write(built.root());
    require(std::string_view{strings.data(), strings.size()} == R"({"":"key","empty":"","literal":"text"})",
            "Empty views and string literals must remain strings, not missing members or booleans.");
}
}

void roundtrip(const fs::path& root) {
    writer_contract();
    const auto directory = root / "run";
    auto reader = take(log::Reader::open(directory));
    auto run = start(directory);

    auto original = entry("text.owned", "中文🙂\"\\\r\n");
    original.message.push_back('\0');
    original.message += "NUL-tail";
    original.source = {"Source/行为.source",
                       "E:/OriginalProject",
                       "动态函数",
                       17,
                       11,
                       std::uint64_t{9007199254741017ULL},
                       std::uint64_t{9007199254741089ULL},
                       "E:/OriginalProject/Source/行为.source"};
    original.fields.push_back({"identity", std::numeric_limits<std::uint64_t>::max() - 3});
    original.fields.push_back({"negative", std::numeric_limits<std::int64_t>::min() + 17});
    original.fields.push_back({"positive_signed", std::int64_t{19}});
    original.fields.push_back({"flag", false});
    original.fields.push_back({"fraction", -0.125});
    original.fields.push_back({"label", std::string{"中文\0field", sizeof("中文\0field") - 1}});
    original.repeat = log::Repeat{"world-7/object-19/diagnostic-31", 3};
    const auto expected = original;
    require(log::submit(original).status == log::SubmitStatus::accepted, "Owned record was not admitted.");
    original.message.assign(original.message.size(), 'x');
    original.source = {};
    original.fields.clear();
    original.repeat.reset();
    healthy(run->flush());

    const char unterminated[]{'b', 'a', 'd'};
    const auto rejected = ATELIER_LOG(log::Severity::error, "acceptance.cpu", "text.invalid", "{}", unterminated);
    require(rejected.status == log::SubmitStatus::invalid_record && rejected.producer_id == 0 &&
                rejected.producer_sequence == 0 && run->health().rejected_invalid_record == 1 &&
                !run->health().first_error,
            "An unterminated native string must be rejected before identity or diagnostic capture.");

    std::string native_text{"原生\0text", sizeof("原生\0text") - 1};
    std::vector<log::Field> fields{{"phase", std::string{"native"}}, {"context", std::uint64_t{97}}};
    const log::native::Context context{fields, log::Repeat{"native/97", 5}};
    // Keep the next two physical lines adjacent: source_location must match __LINE__ + 1.
    // clang-format off
    const auto native_line = __LINE__ + 1;
    const auto native = ATELIER_LOG_WITH(log::Severity::error, "acceptance.cpu", "text.native", context, "native {} / {}", std::string_view{native_text}, 37);
    // clang-format on
    require(native.status == log::SubmitStatus::accepted && native.producer_sequence == 2,
            "Native format record was not admitted exactly after the owned record.");
    const auto expected_native = std::string{"native "} + native_text + " / 37";
    native_text.assign(native_text.size(), 'z');
    fields[0].value = std::string{"destroyed"};

    const auto final = run->finish();
    require(final.health.accepted == 2 && final.health.state == log::RunState::finished,
            "Final host accounting changed the two submitted records.");
    auto read = drain(*reader);
    clean_reader(read);
    const log::FlushReport* reports[]{&final, &read.health->report};
    for (const auto* report : reports) {
        const auto& h = report->health;
        require(report->backend_acknowledged && report->sink_flush_succeeded && report->summary_published &&
                    h.facility_failures == 0 && !h.first_facility_issue && h.backend_notifications == 0 &&
                    h.rejected_queue_full == 0 && h.rejected_producer_limit == 0 && h.rejected_record_too_large == 0 &&
                    h.rejected_invalid_record == 1 && h.rejected_internal_failure == 0 && h.first_error &&
                    h.first_error->entry.event_code == "text.native",
                "Final health must contain only the expected input rejection and valid native diagnostic.");
    }
    require(read.records.size() == 2, "Reader lost or duplicated a record.");
    same_entry(read.records[0].entry, expected);
    const auto& formatted = read.records[1];
    require(formatted.entry.category == "acceptance.cpu" && formatted.entry.event_code == "text.native" &&
                formatted.entry.severity == log::Severity::error && formatted.entry.message == expected_native &&
                field<std::string>(formatted.entry, "phase") == "native" &&
                field<std::uint64_t>(formatted.entry, "context") == 97 && formatted.entry.repeat &&
                formatted.entry.repeat->identity == "native/97" && formatted.entry.repeat->count == 5 &&
                formatted.entry.source.line == static_cast<std::uint32_t>(native_line) &&
                fs::path{formatted.entry.source.file}.filename() == "contracts.cpp",
            "Native formatting, call-site source or context did not survive asynchronous delivery.");

    // Check the public wire schema separately from logging's domain decoder.
    const auto documents = read_json_lines(directory / "events.jsonl");
    require(documents.size() == 2 && decoded(documents[0].root().string({"message"})) == expected.message &&
                decoded(documents[1].root().string({"message"})) == expected_native,
            "JSON text roundtrip failed.");
    const auto values = decoded(documents[0].root().at("fields"));
    require(values.size() == expected.fields.size() && decoded(values.string({"identity", "type"})) == "u64" &&
                decoded(values.string({"identity", "value"})) == "18446744073709551612" &&
                decoded(values.string({"negative", "value"})) == "-9223372036854775791" &&
                decoded(values.string({"positive_signed", "type"})) == "i64" &&
                decoded(values.string({"positive_signed", "value"})) == "19" &&
                decoded(documents[0].root().string({"source", "version"})) == "9007199254741017" &&
                decoded(documents[0].root().string({"repeat", "count"})) == "3",
            "JSON consumer lost scalar types or 64-bit identity.");
    const auto health = decoded(json::Document::parse(read_bytes(directory / "health.json"), {16}));
    require(decoded(health.root().boolean({"final"})) &&
                decoded(health.root().string({"report", "health", "accepted"})) == "2",
            "Final health lost the two submitted records.");
    reader.reset();
    run.reset();
    same_entry(read.records[0].entry, expected);
}
}

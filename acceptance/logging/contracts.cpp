#include "support.hpp"
#include "logging/native.hpp"
#include <nlohmann/json.hpp>
#include <limits>

namespace logging_acceptance {
namespace {
using Json = nlohmann::json;

std::vector<Json> independent_json(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(input.is_open(), "Independent JSON consumer cannot open events.");
    std::vector<Json> result;
    for (std::string line; std::getline(input, line);) {
        require(!line.empty(), "JSONL contains an empty physical line.");
        result.push_back(Json::parse(line));
    }
    require(input.eof() && !input.bad(), "Independent JSON consumer did not reach EOF.");
    return result;
}
}

void roundtrip(const fs::path& root) {
    const auto directory = root / "run";
    auto reader = take(log::Reader::open(directory));
    const auto waiting = reader->poll();
    require(waiting.status.waiting_for_run && !waiting.status.finished && waiting.records.empty(),
        "Reader treated a not-yet-created run as completed evidence.");
    auto run = start(directory);
    require(log::current_run() == run.get() && !run->info().run_id.empty(), "Explicit host did not become active.");

    auto original = entry("text.owned", "中文🙂\"\\\r\n");
    original.message.push_back('\0');
    original.message += "NUL-tail";
    original.message.append(70013 - original.message.size(), 'q');
    original.message.back() = '!';
    original.source = {"Source/行为.source", "E:/OriginalProject", "动态函数", 17, 11,
        std::uint64_t{9007199254741017ULL}, std::uint64_t{9007199254741089ULL}, "E:/OriginalProject/Source/行为.source"};
    original.fields.push_back({"identity", std::numeric_limits<std::uint64_t>::max() - 3});
    original.fields.push_back({"negative", std::numeric_limits<std::int64_t>::min() + 17});
    original.fields.push_back({"positive_signed", std::int64_t{19}});
    original.fields.push_back({"flag", false});
    original.fields.push_back({"fraction", -0.125});
    original.fields.push_back({"label", std::string{"中文\0field", sizeof("中文\0field") - 1}});
    for (std::size_t i = original.fields.size(); i != log::maximum_fields; ++i)
        original.fields.push_back({"field_" + std::to_string(i), static_cast<std::uint64_t>(i * 31 + 7)});
    original.repeat = log::Repeat{"world-7/object-19/diagnostic-31", 3};
    const auto expected = original;
    const auto first = log::submit(original);
    require(first.status == log::SubmitStatus::accepted, "Owned record was not admitted.");
    original.message.assign(original.message.size(), 'x');
    original.source = {};
    original.fields.clear();
    original.repeat.reset();
    healthy(run->flush());

    const auto boundary = entry("text.maximum", std::string(log::maximum_message_bytes, 'm'), 73);
    require(log::submit(boundary).status == log::SubmitStatus::accepted, "Exact maximum message was rejected.");
    healthy(run->flush());

    std::string native_text{"原生\0text", sizeof("原生\0text") - 1};
    std::vector<log::Field> fields{{"phase", std::string{"native"}}, {"context", std::uint64_t{97}}};
    const log::native::Context context{fields, log::Repeat{"native/97", 5}};
    const auto native_line = __LINE__ + 1;
    const auto native = ATELIER_LOG_WITH(log::Severity::error, "acceptance.cpu", "text.native", context, "native {} / {}", std::string_view{native_text}, 37);
    require(native.status == log::SubmitStatus::accepted, "Native format record was not admitted.");
    const auto expected_native = std::string{"native "} + native_text + " / 37";
    native_text.assign(native_text.size(), 'z');
    fields[0].value = std::string{"destroyed"};
    const auto synchronous = run->health();
    require(synchronous.first_error && synchronous.first_error->message_is_template &&
        synchronous.first_error->entry.message == "native {} / {}" &&
        synchronous.first_error->producer_sequence == native.producer_sequence &&
        field<std::string>(synchronous.first_error->entry, "phase") == "native",
        "Native first error did not synchronously preserve its template and owned fields.");

    const auto final = run->finish();
    healthy(final);
    require(final.health.accepted == 3 && final.health.state == log::RunState::finished,
        "Final host accounting changed the three submitted records.");
    auto read = drain(*reader);
    clean_reader(read);
    healthy(read.health->report);
    require(read.records.size() == 3 && read.run->run_id == run->info().run_id &&
        read.run->process_id == run->info().process_id, "Reader lost the run identity or a record.");
    same_entry(read.records[0].entry, expected);
    same_entry(read.records[1].entry, boundary);
    const auto& formatted = read.records[2];
    require(formatted.entry.category == "acceptance.cpu" && formatted.entry.event_code == "text.native" &&
        formatted.entry.severity == log::Severity::error &&
        formatted.entry.message == expected_native && field<std::string>(formatted.entry, "phase") == "native" &&
        field<std::uint64_t>(formatted.entry, "context") == 97 &&
        formatted.entry.repeat && formatted.entry.repeat->identity == "native/97" && formatted.entry.repeat->count == 5 &&
        formatted.entry.source.line == static_cast<std::uint32_t>(native_line) &&
        fs::path{formatted.entry.source.file}.filename() == "contracts.cpp",
        "Native formatting, call-site source or context did not survive asynchronous delivery.");
    for (std::size_t i = 0; i != read.records.size(); ++i) {
        const auto& record = read.records[i];
        require(record.persistence == log::Persistence::jsonl && record.record_sequence == i + 1 &&
            record.producer_id == first.producer_id && record.producer_sequence == first.producer_sequence + i &&
            record.thread_id != 0 && record.timestamp_unix_ns != 0,
            "Record identity/order/persistence metadata was not preserved.");
    }

    // Independent parser: catches Writer and Reader sharing the same lossy conversion.
    const auto json = independent_json(directory / "events.jsonl");
    require(json.size() == 3 && json[0].at("message").get<std::string>() == expected.message &&
        json[2].at("message").get<std::string>() == expected_native, "Independent JSON text roundtrip failed.");
    const auto& values = json[0].at("fields");
    require(values.size() == log::maximum_fields && values.at("identity").at("type") == "u64" &&
        values.at("identity").at("value") == "18446744073709551612" &&
        values.at("negative").at("value") == "-9223372036854775791" &&
        values.at("positive_signed").at("type") == "i64" && values.at("positive_signed").at("value") == "19" &&
        json[0].at("source").at("version") == "9007199254741017" &&
        json[0].at("repeat").at("count") == "3", "Independent JSON consumer lost scalar types or 64-bit identity.");
    const auto manifest = Json::parse(read_bytes(directory / "run.json"));
    const auto health = Json::parse(read_bytes(directory / "health.json"));
    require(manifest.at("run").at("run_id") == run->info().run_id && health.at("final") == true &&
        health.at("report").at("health").at("accepted") == "3", "Independent manifest/final health disagrees with the host.");
    reader.reset();
    run.reset();
    same_entry(read.records[0].entry, expected);
}
}

#include "support.hpp"
#include "json/json.hpp"
#include "logging/native.hpp"

namespace logging_acceptance {
void roundtrip(const fs::path& root) {
    const auto directory = root / "run";
    auto run = start(directory);
    auto expected = entry("roundtrip", "中文🙂\nowned text");
    expected.fields.add({"identity"_s, std::uint64_t{9007199254741017ULL}});
    require(log::submit(expected).status == log::SubmitStatus::accepted, "Record was not admitted.");
    const atelier::String owned_arg = "owned-arg"_s;
    const atelier::StringView view_arg{"view-arg"};
    require(ATELIER_LOG(log::Severity::info, "acceptance.cpu", "native.bridge", "{} + {}", owned_arg, view_arg).status ==
                log::SubmitStatus::accepted,
            "ATELIER_LOG String/StringView bridge was not admitted.");
    healthy(run->finish());
    const auto read = read_finished(directory);
    clean_reader(read);
    require(read.records.size() == 2, "Reader lost or duplicated the authored records.");
    same_entry(read.records[0].entry, expected);
    require(read.records[1].entry.message == "owned-arg + view-arg",
            "ATELIER_LOG String/StringView formatted message mismatch.");
    // Decode the wire independently of the logging Reader; compare to authored input.
    const auto raw_bytes = read_bytes(directory / "events.jsonl");
    const auto first_nl = atelier::StringView{raw_bytes.data(), raw_bytes.size()}.find('\n');
    require(first_nl != atelier::StringView::npos, "Missing newline in events.jsonl.");
    const auto document = atelier::json::Document::parse({raw_bytes.data(), first_nl}, {16});
    require(document.has_value(), "Recorded JSON is not readable.");
    const auto message = document->root().string({"message"});
    const auto identity = document->root().string({"fields", "identity", "value"});
    require(message && expected.message == *message && identity && *identity == "9007199254741017",
            "Current logging format lost UTF-8 or the independent 64-bit identity.");
}
}

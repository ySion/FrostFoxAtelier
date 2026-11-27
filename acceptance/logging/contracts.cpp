#include "support.hpp"
#include "json/json.hpp"

namespace logging_acceptance {
void roundtrip(const fs::path& root) {
    const auto directory = root / "run";
    auto run = start(directory);
    auto expected = entry("roundtrip", "中文🙂\nowned text");
    expected.fields.add({"identity", std::uint64_t{9007199254741017ULL}});
    require(log::submit(expected).status == log::SubmitStatus::accepted, "Record was not admitted.");
    healthy(run->finish());
    const auto read = read_finished(directory);
    clean_reader(read);
    require(read.records.size() == 1, "Reader lost or duplicated the authored record.");
    same_entry(read.records[0].entry, expected);
    // Decode the wire independently of the logging Reader; compare to authored input.
    const auto document = atelier::json::Document::parse(read_bytes(directory / "events.jsonl"), {16});
    require(document.has_value(), "Recorded JSON is not readable.");
    const auto message = document->root().string({"message"});
    const auto identity = document->root().string({"fields", "identity", "value"});
    require(message && *message == expected.message && identity && *identity == "9007199254741017",
            "Current logging format lost UTF-8 or the independent 64-bit identity.");
}
}

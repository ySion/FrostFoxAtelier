#include "support.hpp"

namespace logging_acceptance {
void reader_cases(const fs::path& root) {
    const auto source = root / "run";
    auto run = start(source);
    const auto expected = entry("reader.first", "中文🙂 first", 211);
    require(log::submit(expected).status == log::SubmitStatus::accepted, "Reader source fixture was not admitted.");
    healthy(run->finish());
    const auto encoded = read_bytes(source / "events.jsonl");
    const auto chinese = encoded.find("中文");
    require(chinese != std::string::npos, "UTF-8 was unexpectedly sanitized before the partial-line check.");
    const auto split = chinese + 1; // Deliberately in the middle of a UTF-8 scalar.

    const auto partial = root / "partial-live";
    fs::create_directory(partial);
    fs::copy_file(source / "run.json", partial / "run.json");
    write_bytes(partial / "events.jsonl", std::string_view{encoded}.substr(0, split));
    auto reader = take(log::Reader::open(partial));
    const auto head = drain(*reader, false, {47, 1, std::chrono::milliseconds{50}});
    require(head.records.empty() && head.status.partial_line && !head.status.finished &&
                head.status.invalid_lines == 0 && head.status.incomplete_lines == 0,
            "Reader parsed an active partial UTF-8 line as a record or a failure.");
    write_bytes(partial / "events.jsonl", std::string_view{encoded}.substr(split), true);
    auto completed = drain(*reader, false, {47, 1, std::chrono::milliseconds{50}});
    require(completed.records.size() == 1 && !completed.status.partial_line && !completed.status.finished &&
                completed.status.invalid_lines == 0,
            "Appending the rest of a line lost or duplicated a record.");
    same_entry(completed.records.front().entry, expected);
    fs::copy_file(source / "health.json", partial / "health.json");
    const auto ended = drain(*reader);
    clean_reader(ended);
    require(ended.records.empty(), "Final health caused a completed record to be delivered twice.");
    reader.reset();
    same_entry(completed.records.front().entry, expected);
}

void rotation(const fs::path& root) {
    const auto directory = root / "run";
    auto run = start(directory);
    auto reader = take(log::Reader::open(directory));
    const auto message_size = log::maximum_message_bytes - 64;
    const auto count = log::rotation_bytes / message_size + 1;
    for (std::size_t i = 0; i != count; ++i) {
        auto value = entry("rotation.record", std::string(message_size, static_cast<char>('a' + i % 23)), i + 10001);
        value.fields.push_back({"ordinal", static_cast<std::uint64_t>(i)});
        require(log::submit(value).status == log::SubmitStatus::accepted,
                "Rotation fixture unexpectedly rejected a record.");
        healthy(run->flush());
        const auto visible = drain(*reader, false);
        require(!visible.status.incomplete && visible.status.invalid_lines == 0 &&
                    visible.status.rotation_losses == 0 && visible.status.sequence_gaps == 0 &&
                    visible.status.duplicate_records == 0 && visible.records.size() == 1,
                "Live Reader lost or duplicated a record while rotation renamed files.");
        same_entry(visible.records.front().entry, value);
    }
    healthy(run->finish());
    const auto final = drain(*reader);
    clean_reader(final);
    require(final.records.empty() && fs::exists(directory / "events.1.jsonl") &&
                !fs::exists(directory / "events.2.jsonl"),
            "Rotation fixture failed to cross exactly one real rename boundary.");
    const auto offline = read_finished(directory);
    clean_reader(offline);
    require(offline.records.size() == count, "Archived rotated files did not reconstruct the complete stream.");
    for (std::size_t i = 0; i != count; ++i) {
        auto expected = entry("rotation.record", std::string(message_size, static_cast<char>('a' + i % 23)), i + 10001);
        expected.fields.push_back({"ordinal", static_cast<std::uint64_t>(i)});
        same_entry(offline.records[i].entry, expected);
    }
}
}

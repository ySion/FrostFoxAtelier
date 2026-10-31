#include "support.hpp"
#include <nlohmann/json.hpp>
#include <array>

namespace logging_acceptance {
namespace {
using Json = nlohmann::json;

void archive(const fs::path& source, const fs::path& destination, std::string_view events, bool final) {
    fs::create_directory(destination);
    // Archive copies retain their original manifest/run identity. This is not
    // another start_run or a handcrafted in-memory substitute for the Reader.
    fs::copy_file(source / "run.json", destination / "run.json");
    if (final)
        fs::copy_file(source / "health.json", destination / "health.json");
    write_bytes(destination / "events.jsonl", events);
}

std::vector<std::string> lines(std::string_view bytes) {
    std::vector<std::string> result;
    std::size_t start{};
    while (start != bytes.size()) {
        const auto end = bytes.find('\n', start);
        require(end != std::string_view::npos, "Healthy source fixture has a partial JSONL line.");
        result.emplace_back(bytes.substr(start, end + 1 - start));
        start = end + 1;
    }
    return result;
}
}

void reader_cases(const fs::path& root) {
    const auto source = root / "run";
    auto run = start(source);
    std::array<log::Entry, 4> expected{entry("reader.first", "中文🙂 first", 211),
                                       entry("reader.second", "second", 223), entry("reader.third", "third", 227),
                                       entry("reader.fourth", "fourth", 229)};
    for (const auto& value : expected)
        require(log::submit(value).status == log::SubmitStatus::accepted, "Reader source fixture was not admitted.");
    healthy(run->finish());
    const auto baseline = read_finished(source);
    clean_reader(baseline);
    require(baseline.records.size() == expected.size(), "Reader source fixture is incomplete.");
    for (std::size_t i = 0; i != expected.size(); ++i)
        same_entry(baseline.records[i].entry, expected[i]);
    const auto encoded = lines(read_bytes(source / "events.jsonl"));
    require(encoded.size() == expected.size(), "Reader fixture physical line count is incorrect.");
    const auto chinese = encoded[0].find("中文");
    require(chinese != std::string::npos, "UTF-8 was unexpectedly sanitized before the partial-line check.");
    const auto split = chinese + 1; // Deliberately in the middle of a UTF-8 scalar.

    const auto partial = root / "partial-live";
    archive(source, partial, std::string_view{encoded[0]}.substr(0, split), false);
    auto reader = take(log::Reader::open(partial));
    const auto head = drain(*reader, false, {47, 1, std::chrono::milliseconds{50}});
    require(head.records.empty() && head.status.partial_line && !head.status.finished && !head.status.source_ended &&
                head.status.invalid_lines == 0 && head.status.incomplete_lines == 0,
            "Reader parsed an active partial UTF-8 line as a record or a failure.");
    write_bytes(partial / "events.jsonl", std::string_view{encoded[0]}.substr(split), true);
    auto completed = drain(*reader, false, {47, 1, std::chrono::milliseconds{50}});
    require(completed.records.size() == 1 && !completed.status.partial_line && !completed.status.finished &&
                completed.status.invalid_lines == 0,
            "Appending the rest of a line lost or duplicated a record.");
    same_entry(completed.records.front().entry, expected[0]);
    reader->source_ended(); // This archive is now actually closed, not merely asked to stop.
    reader->source_ended();
    const auto ended = drain(*reader);
    require(ended.records.empty() && ended.status.finished && ended.status.source_ended &&
                !ended.status.has_final_summary && ended.status.incomplete && ended.status.incomplete_lines == 0 &&
                ended.status.latest_issue &&
                ended.status.latest_issue->fault == log::ReaderFault::missing_final_summary,
            "External source_ended fabricated normal completion or kept waiting for final health.");
    reader.reset();
    same_entry(completed.records.front().entry, expected[0]);

    const auto tail = root / "ended-partial";
    archive(source, tail, std::string_view{encoded[0]}.substr(0, split), false);
    auto torn = take(log::Reader::open(tail));
    torn->source_ended();
    const auto torn_read = drain(*torn);
    require(torn_read.records.empty() && torn_read.status.finished && torn_read.status.source_ended &&
                !torn_read.status.has_final_summary && torn_read.status.incomplete &&
                torn_read.status.incomplete_lines == 1 && !torn_read.status.partial_line,
            "Ended partial line was treated as a complete record or an endless wait.");
    const auto again = torn->poll();
    require(again.records.empty() && again.status.incomplete_lines == 1 && again.status.incomplete,
            "Repeated EOF polling double-counted or forgot a torn tail.");

    const auto final_tail = root / "final-partial";
    archive(source, final_tail, encoded[0] + encoded[1] + encoded[2] + encoded[3].substr(0, encoded[3].size() / 2),
            true);
    const auto final_torn = read_finished(final_tail);
    require(final_torn.status.finished && final_torn.status.has_final_summary && final_torn.status.incomplete &&
                final_torn.status.incomplete_lines == 1 && final_torn.records.size() == 3,
            "Valid final health hid a partial final record.");
    for (std::size_t i = 0; i != final_torn.records.size(); ++i)
        same_entry(final_torn.records[i].entry, expected[i]);

    auto absent = take(log::Reader::open(root / "never-created"));
    require(absent->poll().status.waiting_for_run, "Absent run did not wait while its source was active.");
    absent->source_ended();
    const auto no_run = drain(*absent);
    require(no_run.status.finished && no_run.status.incomplete && !no_run.status.has_final_summary &&
                no_run.records.empty(),
            "Exited source without a manifest waited forever or fabricated records.");

    auto wrong_schema = Json::parse(encoded[2]);
    wrong_schema["schema"] = log::schema_version + 1;
    const auto damaged = root / "damaged-lines";
    archive(source, damaged, encoded[0] + "{not-json}\n" + wrong_schema.dump() + "\n" + encoded[3], true);
    const auto damaged_read = read_finished(damaged);
    require(damaged_read.status.finished && damaged_read.status.incomplete && damaged_read.status.invalid_lines == 2 &&
                damaged_read.status.sequence_gaps > 0 && damaged_read.records.size() == 2,
            "Bad JSON/schema was silently accepted or prevented recovery to a later complete record.");
    same_entry(damaged_read.records.front().entry, expected[0]);
    same_entry(damaged_read.records.back().entry, expected[3]);

    const auto duplicate = root / "duplicate-line";
    archive(source, duplicate, encoded[0] + encoded[0] + encoded[1] + encoded[2] + encoded[3], true);
    const auto repeated = read_finished(duplicate);
    require(repeated.status.finished && repeated.status.incomplete && repeated.status.duplicate_records == 1 &&
                repeated.records.size() == expected.size(),
            "Duplicate unique record identity was delivered twice or hidden.");
    for (std::size_t i = 0; i != expected.size(); ++i)
        same_entry(repeated.records[i].entry, expected[i]);

    const auto oversized = root / "oversized-line";
    archive(source, oversized, std::string(log::maximum_json_line_bytes + 113, 'x') + "\n" + encoded[3], true);
    const auto large = read_finished(oversized);
    require(large.status.finished && large.status.incomplete && large.status.oversized_lines == 1 &&
                large.records.size() == 1 && large.status.io_failures == 0,
            "Oversized line was unbounded, silently truncated or swallowed the following record.");
    same_entry(large.records.front().entry, expected[3]);
}

void rotation(const fs::path& root) {
    const auto directory = root / "run";
    auto run = start(directory);
    auto reader = take(log::Reader::open(directory));
    const auto message_size = log::maximum_message_bytes - 64;
    const auto count = 2 * log::rotation_bytes / message_size + 5;
    std::size_t next{};
    for (std::size_t i = 0; i != count; ++i) {
        auto value = entry("rotation.record", std::string(message_size, static_cast<char>('a' + i % 23)), i + 10001);
        value.fields.push_back({"ordinal", static_cast<std::uint64_t>(i)});
        require(log::submit(std::move(value)).status == log::SubmitStatus::accepted,
                "Rotation fixture unexpectedly rejected a record.");
        healthy(run->flush());
        const auto visible = drain(*reader, false);
        require(!visible.status.incomplete && visible.status.invalid_lines == 0 &&
                    visible.status.rotation_losses == 0 && visible.status.sequence_gaps == 0 &&
                    visible.status.duplicate_records == 0,
                "Live Reader lost its file identity while rotation renamed files.");
        for (const auto& record : visible.records) {
            require(record.entry.severity == log::Severity::info && record.entry.category == "acceptance.cpu" &&
                        record.entry.event_code == "rotation.record" &&
                        field<std::string>(record.entry, "phase") == "validation" &&
                        field<std::uint64_t>(record.entry, "ordinal") == next &&
                        field<std::uint64_t>(record.entry, "context") == next + 10001 &&
                        record.entry.message == std::string(message_size, static_cast<char>('a' + next % 23)),
                    "Live rotation duplicated, skipped or spliced records.");
            ++next;
        }
    }
    healthy(run->finish());
    const auto final = drain(*reader);
    clean_reader(final);
    require(final.records.empty() && next == count && fs::exists(directory / "events.2.jsonl"),
            "Rotation fixture failed to cross two real rename boundaries.");
    const auto offline = read_finished(directory);
    clean_reader(offline);
    require(offline.records.size() == count, "Archived rotated files did not reconstruct the complete stream.");
    for (std::size_t i = 0; i != count; ++i)
        require(field<std::uint64_t>(offline.records[i].entry, "ordinal") == i,
                "Archive files were ordered by changing names, not stream order.");

    const auto gap = root / "missing-middle-segment";
    fs::create_directory(gap);
    for (const auto& file : fs::directory_iterator(directory)) {
        const auto name = file.path().filename();
        if (name == "run.json" || name == "health.json" || file.path().extension() == ".jsonl")
            fs::copy_file(file.path(), gap / name);
    }
    require(fs::remove(gap / "events.1.jsonl"), "Could not create the missing-segment archive.");
    const auto missing = read_finished(gap);
    require(missing.status.finished && missing.status.has_final_summary && missing.status.incomplete &&
                missing.status.sequence_gaps > 0 && missing.records.size() < count,
            "Reader treated a missing middle segment as normal filtering or complete evidence.");
    std::printf("Rotation evidence: records=%zu real_rotations=2 missing_segment_gap=%llu\n", count,
                static_cast<unsigned long long>(missing.status.sequence_gaps));
}
}

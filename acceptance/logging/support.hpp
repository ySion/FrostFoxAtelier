#pragma once

#include "logging/logging.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace logging_acceptance {
namespace log = atelier::logging;

namespace fs = std::filesystem;

inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T>
T take(log::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

inline log::Entry entry(std::string code, std::string message, std::uint64_t context = 71) {
    log::Entry value;
    value.category = "acceptance.cpu";
    value.event_code = std::move(code);
    value.message = std::move(message);
    value.fields = {{"phase", std::string{"validation"}}, {"context", context}};
    return value;
}

inline std::unique_ptr<log::Run> start(const fs::path& directory, log::RunOptions options = {}) {
    options.directory = fs::absolute(directory);
    options.process_name = "logging-acceptance";
    return take(log::start_run(std::move(options)));
}

template <class T>
const T& field(const log::Entry& value, std::string_view key) {
    const auto found = std::ranges::find(value.fields, key, &log::Field::key);
    require(found != value.fields.end(), "Required structured field is missing.");
    return std::get<T>(found->value);
}

inline void healthy(const log::FlushReport& report) {
    require(report.backend_acknowledged && report.sink_flush_succeeded && report.summary_published,
            "Backend acknowledgement, real sink flush or health publication failed.");
    const auto& h = report.health;
    require(h.facility_failures == 0 && !h.first_facility_issue && h.backend_notifications == 0,
            "Unexpected logging facility failure or unclassified backend notification.");
    require(h.rejected_queue_full == 0 && h.rejected_producer_limit == 0 && h.rejected_record_too_large == 0 &&
                h.rejected_invalid_record == 0 && h.rejected_internal_failure == 0,
            "Unexpected logging rejection.");
}

inline void clean_reader(const log::ReadBatch& batch) {
    const auto& s = batch.status;
    require(s.finished && s.source_ended && s.has_final_summary && !s.incomplete && !s.partial_line,
            "Reader did not establish complete normal termination.");
    require(s.invalid_lines == 0 && s.invalid_metadata == 0 && s.oversized_lines == 0 && s.incomplete_lines == 0 &&
                s.rotation_losses == 0 && s.sequence_gaps == 0 && s.duplicate_records == 0 &&
                s.out_of_order_records == 0 && s.io_failures == 0 && !s.unquantified_loss,
            "Reader observed damaged, duplicate, out-of-order or missing evidence.");
    require(batch.run && batch.health && batch.health->final &&
                batch.health->report.health.state == log::RunState::finished,
            "Reader lost the run identity or final health report.");
}

// This is a synchronous consumer of the public Reader, not a backend barrier.
// Call with final=true only after finish(), or after source_ended() on a closed archive.
inline log::ReadBatch drain(log::Reader& reader, bool final = true,
                            log::ReadBudget budget = {1024 * 1024, 4096, std::chrono::milliseconds{50}}) {
    log::ReadBatch collected;
    for (unsigned polls = 0; polls != 20000; ++polls) {
        auto next = reader.poll(budget);
        for (auto& record : next.records)
            collected.records.push_back(std::move(record));
        collected.status = next.status;
        if (next.run)
            collected.run = std::move(next.run);
        if (next.health)
            collected.health = std::move(next.health);
        if (final ? collected.status.finished : collected.status.caught_up && !collected.status.waiting_for_run)
            return collected;
    }
    throw std::runtime_error("Reader made no bounded completion progress; see retained case files.");
}

inline log::ReadBatch read_finished(const fs::path& directory) {
    auto reader = take(log::Reader::open(directory));
    return drain(*reader);
}

inline std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Cannot open acceptance evidence.");
    std::string bytes{std::istreambuf_iterator<char>{input}, {}};
    require(!input.bad(), "Cannot read acceptance evidence.");
    return bytes;
}

inline void write_bytes(const fs::path& path, std::string_view bytes, bool append = false) {
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    require(output.is_open(), "Cannot create acceptance evidence.");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(!output.fail(), "Cannot finish writing acceptance evidence.");
}

inline void same_entry(const log::Entry& actual, const log::Entry& expected) {
    require(actual.severity == expected.severity && actual.category == expected.category &&
                actual.event_code == expected.event_code && actual.message == expected.message,
            "Rendered UTF-8/NUL text or stable event metadata changed.");
    const auto& a = actual.source;
    const auto& b = expected.source;
    require(a.file == b.file && a.root == b.root && a.function == b.function && a.line == b.line &&
                a.column == b.column && a.version == b.version && a.offset == b.offset && a.disk_path == b.disk_path,
            "Owned source path, position or version changed.");
    require(actual.fields.size() == expected.fields.size(), "Structured field count changed.");
    for (const auto& item : expected.fields) {
        const auto found = std::ranges::find(actual.fields, item.key, &log::Field::key);
        require(found != actual.fields.end() && found->value == item.value,
                "Typed field lost its type or exact value.");
    }
    require(actual.repeat.has_value() == expected.repeat.has_value(), "Explicit repeat metadata changed.");
    if (expected.repeat)
        require(actual.repeat->identity == expected.repeat->identity && actual.repeat->count == expected.repeat->count,
                "Explicit repeat identity/count changed.");
}

void roundtrip(const fs::path&);

void boundaries(const fs::path&);

void file_failures(const fs::path&);

void threads(const fs::path&);

void producer_churn(const fs::path&);

void reader_cases(const fs::path&);

void rotation(const fs::path&);

void dll(const fs::path&, const fs::path&);

int unexpected_warning(const fs::path&);

int pressure(const fs::path&);
}

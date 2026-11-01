#include "logging/format.hpp"
#include "logging/io.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <set>

namespace atelier::logging {
namespace {
using namespace detail;
bool missing(DWORD code) noexcept {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}
struct SnapshotChanged {};
struct Fingerprint {
    FileIdentity identity;
    std::uint64_t size{};
    std::uint64_t modified{};
    bool operator==(const Fingerprint&) const = default;
};
Fingerprint fingerprint(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        io_error("Inspect log input");
    return {{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow},
            (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
            (std::uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) | info.ftLastWriteTime.dwLowDateTime};
}
struct Budget {
    ReadBudget limit;
    std::size_t bytes{};
    std::size_t lines{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    bool worked{};
    // Permit one bounded input unit before yielding, otherwise a slow directory
    // scan could consume every poll's slice forever without advancing the cursor.
    bool expired() const {
        return limit.time <= std::chrono::milliseconds::zero() ||
               (worked && std::chrono::steady_clock::now() - start >= limit.time);
    }
    bool available() const { return bytes < limit.bytes && lines < limit.records && !expired(); }
};
struct Metadata {
    Handle handle;
    std::optional<Fingerprint> loaded;
    Fingerprint reading;
    std::string buffer;
};
struct Segment {
    Handle handle;
    FileIdentity identity;
    std::filesystem::path path;
    unsigned index{};
};
}

struct Reader::Impl {
    std::filesystem::path directory;
    ReaderStatus status;
    std::optional<RunInfo> run;
    std::optional<PublishedHealth> health;
    Metadata manifest;
    Metadata summary;
    std::optional<Segment> current;
    std::deque<FileIdentity> completed;
    std::vector<FileIdentity> pending;
    std::string chunk;
    std::string tail;
    std::size_t chunk_cursor{};
    std::uint64_t offset{};
    std::uint64_t consumed{};
    std::uint64_t line_start{};
    std::uint64_t high_water{};
    bool oversized{};
    bool missing_final_reported{};
    bool lost_current_reported{};
    bool final_gap_reported{};
    bool missing_segment_reported{};
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    std::deque<Key> recent;
    std::set<Key> keys;

    void issue(ReaderFault fault, std::string message, const std::filesystem::path& file, std::uint64_t at = 0) {
        status.latest_issue = ReaderIssue{fault, std::move(message), file, at};
        status.incomplete = true;
        switch (fault) {
        case ReaderFault::io_failed:
            ++status.io_failures;
            break;
        case ReaderFault::rotation_loss:
            ++status.rotation_losses;
            status.unquantified_loss = true;
            break;
        case ReaderFault::sequence_gap:
            ++status.sequence_gaps;
            break;
        case ReaderFault::duplicate_record:
            ++status.duplicate_records;
            break;
        case ReaderFault::out_of_order:
            ++status.out_of_order_records;
            break;
        case ReaderFault::incomplete_line:
            ++status.incomplete_lines;
            break;
        case ReaderFault::oversized_line:
            ++status.oversized_lines;
            break;
        default:
            break;
        }
    }
    // True means the current metadata version was fully examined (or absent).
    // Every byte read, including metadata, consumes the same per-poll budget.
    bool metadata(Metadata& item, const std::filesystem::path& path, bool is_manifest, Budget& budget) {
        try {
            if (!item.handle) {
                auto file = open_read(path);
                if (!file) {
                    const auto code = GetLastError();
                    if (missing(code))
                        return true;
                    io_error("Open reader metadata", code);
                }
                const auto stamp = fingerprint(file.get());
                if (item.loaded == stamp)
                    return true;
                item.reading = stamp;
                item.handle = std::move(file);
                item.buffer.clear();
                if (stamp.size > 256 * 1024) {
                    item.loaded = stamp;
                    item.handle.reset();
                    ++status.invalid_metadata;
                    issue(is_manifest ? ReaderFault::invalid_record : ReaderFault::invalid_summary,
                          "Metadata exceeds 256 KiB", path);
                    return true;
                }
            }
            while (item.buffer.size() < item.reading.size && budget.available()) {
                std::array<char, 16 * 1024> buffer;
                const auto count =
                    (std::min)(buffer.size(),
                               (std::min)(budget.limit.bytes - budget.bytes,
                                          static_cast<std::size_t>(item.reading.size - item.buffer.size())));
                DWORD read{};
                if (!ReadFile(item.handle.get(), buffer.data(), static_cast<DWORD>(count), &read, nullptr))
                    io_error("Read metadata");
                if (read == 0)
                    io_error("Metadata truncated during read", ERROR_HANDLE_EOF);
                budget.bytes += read;
                budget.worked = true;
                status.bytes_read += read;
                item.buffer.append(buffer.data(), read);
            }
            if (item.buffer.size() != item.reading.size)
                return false;
            item.handle.reset();
            try {
                if (is_manifest) {
                    auto decoded = read_run(parse_json(item.buffer));
                    if (run && (decoded.run_id != run->run_id || decoded.process_id != run->process_id))
                        throw FormatError{ReaderFault::invalid_record, "Run manifest changed identity"};
                    run = std::move(decoded);
                } else if (run) {
                    auto decoded = read_health(parse_json(item.buffer), *run);
                    if (health && health->final && !decoded.final)
                        throw FormatError{ReaderFault::invalid_summary, "Health regressed after final evidence"};
                    // Atomic file replacement orders these snapshots. Wall-clock
                    // corrections must not make a later final snapshot disappear.
                    health = std::move(decoded);
                    if (health->final) {
                        status.has_final_summary = true;
                        status.source_ended = true;
                    }
                }
            } catch (const FormatError& error) {
                ++status.invalid_metadata;
                issue(is_manifest ? error.fault : ReaderFault::invalid_summary, error.what(), path);
            } catch (const Json::exception& error) {
                ++status.invalid_metadata;
                issue(is_manifest ? ReaderFault::invalid_json : ReaderFault::invalid_summary, error.what(), path);
            }
            // An allocation exception must leave this version retryable rather
            // than permanently marking an unexamined manifest as loaded.
            item.loaded = item.reading;
            item.buffer.clear();
            return true;
        } catch (const std::system_error& error) {
            item.handle.reset();
            item.buffer.clear();
            issue(ReaderFault::io_failed, error.what(), path);
            return false;
        }
    }
    std::vector<Segment> scan() {
        std::vector<Segment> result;
        for (unsigned i = static_cast<unsigned>(retained_rotated_files) + 1; i-- != 0;) {
            auto path = directory / (i == 0 ? "events.jsonl" : "events." + std::to_string(i) + ".jsonl");
            auto handle = open_read(path);
            if (!handle) {
                const auto code = GetLastError();
                if (missing(code))
                    continue;
                io_error("Open log segment", code);
            }
            auto identity = file_identity(handle.get());
            if (std::ranges::any_of(result, [&](const auto& item) { return item.identity == identity; })) {
                if (!status.source_ended)
                    throw SnapshotChanged{};
                issue(ReaderFault::rotation_loss, "Closed archive aliases two segment names to one file", path);
                continue;
            }
            result.push_back({std::move(handle), identity, std::move(path), i});
        }
        // Verify that names still identify this snapshot. A rename race is a retry,
        // not corruption or permission to splice two different file tails. Verify
        // absent names too, so a newly rotated oldest segment cannot be skipped.
        for (unsigned i = 0; i <= retained_rotated_files; ++i) {
            const auto item =
                std::find_if(result.begin(), result.end(), [&](const auto& value) { return value.index == i; });
            const auto path = directory / (i == 0 ? "events.jsonl" : "events." + std::to_string(i) + ".jsonl");
            auto check = open_read(path);
            if (!check) {
                const auto code = GetLastError();
                if (missing(code)) {
                    if (item == result.end())
                        continue;
                    throw SnapshotChanged{};
                }
                io_error("Recheck log segment", code);
            }
            if (item == result.end()) {
                if (status.source_ended && std::ranges::any_of(result, [&](const auto& value) {
                        return value.identity == file_identity(check.get());
                    }))
                    continue;
                throw SnapshotChanged{};
            }
            if (file_identity(check.get()) != item->identity)
                throw SnapshotChanged{};
        }
        return result;
    }
    bool was_completed(FileIdentity identity) const {
        return std::find(completed.begin(), completed.end(), identity) != completed.end();
    }
    void finish_segment() {
        if (!tail.empty() || oversized)
            issue(ReaderFault::incomplete_line, "Segment ended with an incomplete line", current->path, line_start);
        tail.clear();
        oversized = false;
        chunk.clear();
        chunk_cursor = 0;
        completed.push_back(current->identity);
        if (completed.size() > 32)
            completed.pop_front();
        current.reset();
        offset = consumed = line_start = 0;
        lost_current_reported = false;
    }
    void line(std::vector<Record>& records) {
        try {
            auto value = read_record(parse_json(tail));
            if (value.run_id != run->run_id || value.process_id != run->process_id)
                throw FormatError{ReaderFault::invalid_record, "Record identity does not match run manifest"};
            const Key key{value.producer_id, value.producer_sequence};
            if (keys.contains(key)) {
                issue(ReaderFault::duplicate_record, "Duplicate unique record identity", current->path, line_start);
                return;
            }
            if (value.record_sequence <= high_water) {
                issue(ReaderFault::out_of_order, "Record precedes output high-water mark", current->path, line_start);
                return;
            }
            if (value.record_sequence != high_water + 1)
                issue(ReaderFault::sequence_gap, "Output sequence discontinuity; not a queue drop count", current->path,
                      line_start);
            const auto previous = std::find_if(recent.rbegin(), recent.rend(),
                                               [&](const auto& prior) { return prior.first == key.first; });
            if (previous != recent.rend() && key.second != previous->second + 1)
                issue(ReaderFault::sequence_gap, "Producer sequence discontinuity; cause is not inferred",
                      current->path, line_start);
            high_water = value.record_sequence;
            recent.push_back(key);
            keys.insert(key);
            if (recent.size() > 4096) {
                keys.erase(recent.front());
                recent.pop_front();
            }
            records.push_back(std::move(value));
        } catch (const FormatError& error) {
            ++status.invalid_lines;
            issue(error.fault, error.what(), current->path, line_start);
        } catch (const Json::exception& error) {
            ++status.invalid_lines;
            issue(ReaderFault::invalid_json, error.what(), current->path, line_start);
        }
    }
    void consume(Budget& budget, std::vector<Record>& records) {
        while (chunk_cursor < chunk.size() && budget.lines < budget.limit.records && !budget.expired()) {
            const auto newline = chunk.find('\n', chunk_cursor);
            const auto end = newline == std::string::npos ? chunk.size() : newline;
            const auto count = end - chunk_cursor;
            if (!oversized) {
                if (tail.size() + count + 1 > maximum_json_line_bytes) {
                    issue(ReaderFault::oversized_line, "JSONL line exceeds 2 MiB; discarding through LF", current->path,
                          line_start);
                    tail.clear();
                    oversized = true;
                } else
                    tail.append(chunk.data() + chunk_cursor, count);
            }
            chunk_cursor = end;
            consumed += count;
            budget.worked = true;
            if (newline != std::string::npos) {
                ++budget.lines;
                if (!oversized)
                    line(records);
                ++chunk_cursor;
                ++consumed;
                line_start = consumed;
                tail.clear();
                oversized = false;
            }
        }
        if (chunk_cursor == chunk.size()) {
            chunk.clear();
            chunk_cursor = 0;
        }
    }
    void end_input() {
        status.caught_up = true;
        if (!status.source_ended)
            return;
        if (current)
            finish_segment();
        if (!status.has_final_summary && !missing_final_reported) {
            issue(ReaderFault::missing_final_summary, "Input ended without a valid final health summary",
                  directory / L"health.json");
            missing_final_reported = true;
        }
        if (health && health->final && high_water != health->report.health.last_record_sequence &&
            !final_gap_reported) {
            issue(ReaderFault::sequence_gap, "Final output sequence disagrees with retained input", directory);
            final_gap_reported = true;
        }
        status.finished = true;
    }
    ReadBatch poll(ReadBudget limit) {
        Budget budget{limit};
        status.caught_up = false;
        status.budget_exhausted = false;
        std::vector<Record> records;
        if (!budget.available()) {
            status.budget_exhausted = true;
            return {std::move(records), status, run, health};
        }
        const bool manifest_ready = metadata(manifest, directory / L"run.json", true, budget);
        const bool summary_ready = run && metadata(summary, directory / L"health.json", false, budget);
        status.waiting_for_run = !run;
        if (!run) {
            if (manifest_ready)
                end_input();
        } else if (manifest_ready && summary_ready && budget.available()) {
            try {
                auto files = scan();
                if (status.source_ended && !missing_segment_reported && !files.empty()) {
                    bool gap = files.back().index != 0;
                    for (std::size_t i = 1; i < files.size(); ++i)
                        gap |= files[i - 1].index != files[i].index + 1;
                    if (gap) {
                        issue(ReaderFault::rotation_loss, "Closed archive is missing a numbered log segment",
                              directory);
                        missing_segment_reported = true;
                    }
                }
                for (const auto identity : pending) {
                    if ((!current || current->identity != identity) && !was_completed(identity) &&
                        !std::ranges::any_of(files, [&](const auto& item) { return item.identity == identity; }))
                        issue(ReaderFault::rotation_loss, "Previously observed unread segment disappeared", directory);
                }
                pending.clear();
                for (const auto& item : files)
                    if (!was_completed(item.identity))
                        pending.push_back(item.identity);
                while (budget.available() || (!chunk.empty() && budget.lines < limit.records && !budget.expired())) {
                    if (!current) {
                        auto next = std::find_if(files.begin(), files.end(),
                                                 [&](const auto& item) { return !was_completed(item.identity); });
                        if (next == files.end()) {
                            end_input();
                            break;
                        }
                        current = std::move(*next);
                        offset = consumed = line_start = 0;
                        status.finished = false;
                    }
                    consume(budget, records);
                    if (!chunk.empty() || !budget.available())
                        break;
                    const auto stamp = fingerprint(current->handle.get());
                    if (stamp.size < offset) {
                        issue(ReaderFault::rotation_loss, "Log segment was truncated in place", current->path, offset);
                        finish_segment();
                        continue;
                    }
                    if (offset < stamp.size) {
                        std::array<char, 16 * 1024> buffer;
                        const auto count =
                            (std::min)(buffer.size(), (std::min)(limit.bytes - budget.bytes,
                                                                 static_cast<std::size_t>(stamp.size - offset)));
                        DWORD read{};
                        if (!ReadFile(current->handle.get(), buffer.data(), static_cast<DWORD>(count), &read, nullptr))
                            io_error("Read log segment");
                        if (!read)
                            io_error("Log file shortened while reading", ERROR_HANDLE_EOF);
                        offset += read;
                        budget.bytes += read;
                        budget.worked = true;
                        status.bytes_read += read;
                        chunk.assign(buffer.data(), read);
                        consume(budget, records);
                        continue;
                    }
                    const auto location = std::find_if(files.begin(), files.end(), [&](const auto& item) {
                        return item.identity == current->identity;
                    });
                    if (location == files.end() && !lost_current_reported) {
                        // Our open handle preserves this segment, but arbitrarily many
                        // intervening segments may already have been retained away.
                        issue(ReaderFault::rotation_loss,
                              "Cursor segment left retention window; intervening loss is unquantified", current->path,
                              offset);
                        lost_current_reported = true;
                    }
                    const bool has_later = std::ranges::any_of(files, [&](const auto& item) {
                        return item.identity != current->identity && !was_completed(item.identity) &&
                               (location == files.end() || item.index < location->index);
                    });
                    if (has_later) {
                        finish_segment();
                        continue;
                    }
                    end_input();
                    break;
                }
            } catch (const std::system_error& error) {
                issue(ReaderFault::io_failed, error.what(), current ? current->path : directory, offset);
            } catch (const SnapshotChanged&) {
                // A scan observed rotation in progress; preserve the cursor and
                // retry next poll. No fabricated loss or EOF from a racing name.
            }
        }
        status.partial_line = !tail.empty() || oversized;
        status.budget_exhausted =
            !status.caught_up && (!budget.available() || !manifest_ready || (run && !summary_ready));
        return {std::move(records), status, run, health};
    }
};

Reader::Reader() : impl_(std::make_unique<Impl>()) {}
Reader::~Reader() = default;
Result<std::unique_ptr<Reader>> Reader::open(std::filesystem::path directory) {
    if (directory.empty() || !directory.is_absolute())
        return std::unexpected(Error{ErrorCode::invalid_options, "Reader directory must be absolute", {}});
    std::error_code error;
    const auto exists = std::filesystem::exists(directory, error);
    if (error)
        return std::unexpected(Error{ErrorCode::io_failed, error.message(), static_cast<std::uint32_t>(error.value())});
    if (exists) {
        const auto is_directory = std::filesystem::is_directory(directory, error);
        if (error)
            return std::unexpected(
                Error{ErrorCode::io_failed, error.message(), static_cast<std::uint32_t>(error.value())});
        if (!is_directory)
            return std::unexpected(Error{ErrorCode::invalid_options, "Reader input is not a directory", {}});
    }
    auto reader = std::unique_ptr<Reader>{new Reader};
    reader->impl_->directory = directory.lexically_normal();
    reader->impl_->status.waiting_for_run = true;
    return reader;
}
ReadBatch Reader::poll(ReadBudget budget) {
    try {
        return impl_->poll(budget);
    } catch (...) {
        // Producing the owning batch can allocate after advancing the cursor.
        // Preserve that delivery uncertainty even when no further allocation is
        // possible; do not replace the original exception with another message.
        impl_->status.incomplete = true;
        impl_->status.unquantified_loss = true;
        throw;
    }
}
void Reader::source_ended() noexcept {
    impl_->status.source_ended = true;
}
}

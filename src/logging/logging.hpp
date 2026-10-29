#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace atelier::logging {

// Windows x64 process logging. Ownership, schema and shutdown guarantees are in
// docs/modules/logging/README.md; this header has no backend compile dependency.
inline constexpr std::uint32_t schema_version = 1;

// Initial fixed policy, not measured performance or a process-memory guarantee.
inline constexpr std::size_t producer_queue_bytes = 256 * 1024;
inline constexpr std::size_t maximum_producers = 128; // Live or not-yet-drained producers.
inline constexpr std::size_t maximum_record_bytes = 192 * 1024;
inline constexpr std::size_t maximum_message_bytes = 128 * 1024;
inline constexpr std::size_t maximum_json_line_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t maximum_fields = 64;
inline constexpr std::size_t maximum_diagnostic_snapshot_bytes = 8 * 1024;
inline constexpr std::size_t rotation_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t retained_rotated_files = 8;
inline constexpr std::size_t retained_completed_runs = 20;

enum class Severity : std::uint8_t { trace, debug, info, warning, error, critical, off };
[[nodiscard]] std::string_view name(Severity) noexcept;

// All text is owning UTF-8. Line/column are 1-based; column counts UTF-8 bytes,
// not UTF-16 units or graphemes. Zero means unknown. Relative file names with
// no root/disk_path are virtual. Never resolve against a later active project.
struct SourceLocation {
    std::string file{};
    std::string root{};
    std::string function{};
    std::uint32_t line{};
    std::uint32_t column{};
    std::optional<std::uint64_t> version{};
    std::optional<std::uint64_t> offset{};
    // Saved Source snapshots preserve their absolute origin separately from
    // the compiler's file name. Only with this mapping is version a disk hash.
    std::string disk_path{};
};
[[nodiscard]] SourceLocation native_source(
    std::source_location location = std::source_location::current());

// At most 64 unique, named scalar fields, also bounded by the record byte budget.
// No callbacks, pointers, reflection,
// formatter objects or new object IDs. Producers define keys/units per event code.
using FieldValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;
struct Field {
    std::string key;
    FieldValue value;
};

// Explicit domain coalescing, never equality of rendered text. Identity is
// producer-supplied and scoped by run/process/category/event_code; count is this record's
// occurrence increment (including its first occurrence), not a lifetime total.
struct Repeat {
    std::string identity;
    std::uint64_t count{1};
};
struct Entry {
    Severity severity{Severity::info};
    std::string category{};
    std::string event_code{};
    std::string message{};
    SourceLocation source{};
    std::vector<Field> fields{};
    std::optional<Repeat> repeat{};
};
// One schema check for owned producers and standalone projections. Empty means
// valid; the returned diagnostic is static. Admission separately checks encoded
// and message byte budgets (native messages may still be format templates).
[[nodiscard]] std::string_view entry_problem(const Entry&);

struct RunInfo {
    std::string run_id;
    std::string process_name;
    std::uint32_t process_id{};
    std::uint64_t started_unix_ns{};
    std::filesystem::path directory;
    std::optional<std::string> parent_run_id;
};

enum class Persistence : std::uint8_t { not_persisted, jsonl };

// Unique identity: (run_id, process_id, producer_id, producer_sequence).
// record_sequence is output order, not identity or cross-thread causality.
struct Record {
    std::uint32_t schema{schema_version};
    std::string run_id;
    std::uint32_t process_id{};
    std::uint64_t thread_id{};
    std::uint64_t producer_id{};
    std::uint64_t producer_sequence{};
    std::uint64_t record_sequence{};
    std::uint64_t timestamp_unix_ns{};
    Entry entry;
    // Reader sets jsonl after decoding a complete line; this is not fsync proof.
    // Standalone document consumers may inject the same type into Output with
    // not_persisted. A hosted record reaches Output only through Reader.
    Persistence persistence{Persistence::not_persisted};
};

enum class ErrorCode : std::uint8_t {
    invalid_options, already_started, directory_in_use, io_failed,
    backend_start_failed, unsupported_schema, invalid_manifest
};
struct Error {
    ErrorCode code{};
    std::string message;
    std::optional<std::uint32_t> native_code{};
};
template<class T> using Result = std::expected<T, Error>;

enum class SubmitStatus : std::uint8_t {
    accepted, filtered, no_active_run, queue_full, producer_limit,
    record_too_large, invalid_record, internal_failure
};
struct SubmitResult {
    SubmitStatus status{SubmitStatus::no_active_run};
    // Zero when no producer/sequence was assigned. A queue rejection consumes
    // its sequence; producer IDs are never reused within a run, even after exit.
    std::uint64_t producer_id{};
    std::uint64_t producer_sequence{};
};

enum class FacilityFault : std::uint8_t {
    encoding_failed, output_too_large, invalid_utf8, file_open_failed,
    file_write_failed, file_flush_failed, rotation_failed, retention_failed,
    summary_failed, allocation_failed, lifecycle_violation
};
struct FacilityIssue {
    FacilityFault fault{};
    std::string message; // Bounded first-fault detail; not a business result.
    std::optional<std::uint32_t> native_code;
};

struct CapturedDiagnostic {
    Entry entry;
    std::uint64_t producer_id{};
    std::uint64_t producer_sequence{};
    // Native diagnostics preserve the format template, not a synchronously
    // rendered message. Owned submit() preserves its supplied message.
    bool message_is_template{};
    bool truncated{};
};
enum class RunState : std::uint8_t { running, finishing, finished };
struct Health {
    RunState state{RunState::running};
    std::uint64_t accepted{};
    std::uint64_t rejected_queue_full{};
    std::uint64_t rejected_producer_limit{};
    std::uint64_t rejected_record_too_large{};
    std::uint64_t rejected_invalid_record{};
    std::uint64_t rejected_internal_failure{};
    std::uint64_t facility_failures{};
    // Raw Quill notifications have no typed cause. Never infer rejection counts
    // or I/O success from their English text, or silently discard uncertainty.
    std::uint64_t backend_notifications{};
    std::string first_backend_notification;
    std::uint64_t last_record_sequence{}; // Assigned by serialization, not durable I/O.
    std::optional<FacilityIssue> first_facility_issue;
    std::optional<CapturedDiagnostic> first_error;
};

struct CategoryThreshold {
    std::string category;
    Severity minimum{Severity::info};
};
struct RunOptions {
    // Empty selects a fresh directory under the per-user product log root.
    // Otherwise an absolute, not-yet-existing directory, suitable for a Runner
    // path chosen by its Editor before launch. Never reuse/truncate an old run.
    std::filesystem::path directory{};
    std::string process_name{"application"};
    std::optional<std::string> parent_run_id{};
    Severity minimum{Severity::info};
    std::vector<CategoryThreshold> categories{};
};

struct FlushReport {
    Health health;
    bool backend_acknowledged{};
    bool sink_flush_succeeded{};
    bool summary_published{};
};

class Run;
[[nodiscard]] Result<std::unique_ptr<Run>> start_run(RunOptions options = {});

// One successful run per process. Host owner thread starts it before creating
// Application and finishes it after business destruction, worker join and outer
// catch. No concurrent start/finish, no lazy initialization, no restart in-process.
class Run final {
public:
    ~Run(); // Emergency cleanup only; an omitted finish() is not audited success.
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    [[nodiscard]] const RunInfo& info() const noexcept;
    [[nodiscard]] Health health() const;
    // Blocking, owner-thread only, not a concurrent-producer global barrier.
    // No deadline, crash guarantee, fsync guarantee or implicit business success.
    [[nodiscard]] FlushReport flush();
    // Requires all producers quiescent. Idempotent; final health stays queryable.
    [[nodiscard]] FlushReport finish();
private:
    friend Result<std::unique_ptr<Run>> start_run(RunOptions);
    Run();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Borrow lasts only within the explicitly synchronized run lifetime. Null before
// start and after finish. These accessors do not allocate or create a service.
[[nodiscard]] Run* current_run() noexcept;
[[nodiscard]] bool enabled(Severity severity, std::string_view category) noexcept;

// Stable, non-template host entry for dynamic/JIT/unloadable producers. All
// encoding and deferred code live in the non-unloadable host. No argument borrow
// survives return. Same-toolchain C++ ABI, not a language or cross-CRT ABI.
// Internal failures become status + host health; constructing Entry can throw
// before entering this function. Argument evaluation is not lazily filtered.
[[nodiscard]] SubmitResult submit(Entry entry) noexcept;

struct ReadBudget {
    std::size_t bytes{256 * 1024};
    std::size_t records{256}; // Complete lines examined, including invalid lines.
    // Cooperative checks between work units, not a filesystem-I/O deadline.
    std::chrono::milliseconds time{2};
};
enum class ReaderFault : std::uint8_t {
    io_failed, invalid_json, invalid_record, unsupported_schema, oversized_line,
    incomplete_line, rotation_loss, sequence_gap, duplicate_record, out_of_order,
    invalid_summary, missing_final_summary
};
struct ReaderIssue {
    ReaderFault fault{};
    std::string message;
    std::filesystem::path file;
    std::uint64_t offset{};
    bool operator==(const ReaderIssue&) const = default;
};
struct ReaderStatus {
    bool waiting_for_run{};
    bool caught_up{}; // Current visible EOF; not a producer barrier.
    bool partial_line{};
    bool budget_exhausted{};
    bool source_ended{}; // Host notification or valid final summary.
    bool finished{}; // Input ended and all available retained data examined.
    bool has_final_summary{}; // Normal termination evidence, independent of finished.
    bool incomplete{}; // Sticky missing termination evidence or observed data loss.
    // Cumulative per Reader; loss with no provable quantity is explicitly marked.
    std::uint64_t bytes_read{};
    std::uint64_t invalid_lines{};
    std::uint64_t invalid_metadata{}; // Manifest/health failures, not JSONL lines.
    std::uint64_t oversized_lines{};
    std::uint64_t incomplete_lines{};
    std::uint64_t rotation_losses{};
    std::uint64_t sequence_gaps{}; // Discontinuities, NOT a dropped-record count.
    std::uint64_t duplicate_records{};
    std::uint64_t out_of_order_records{};
    std::uint64_t io_failures{};
    bool unquantified_loss{};
    std::optional<ReaderIssue> latest_issue;
    bool operator==(const ReaderStatus&) const = default;
};
struct PublishedHealth {
    std::uint64_t updated_unix_ns{};
    bool final{};
    FlushReport report;
};
struct ReadBatch {
    std::vector<Record> records; // Owns values; Reader retains no UI history.
    ReaderStatus status;
    std::optional<RunInfo> run;
    std::optional<PublishedHealth> health; // Last valid disk snapshot, possibly stale.
};

// One directory, one owner thread. Independent of a local Run/Quill backend.
// Missing directory is allowed so the Editor can attach before Runner startup.
// Returned record storage survives poll(), rotation, Reader and process teardown.
class Reader final {
public:
    [[nodiscard]] static Result<std::unique_ptr<Reader>> open(std::filesystem::path directory);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    [[nodiscard]] ReadBatch poll(ReadBudget budget = {});
    // Owner-thread, idempotent, no I/O. Call only when the producer process has
    // actually exited (or an archive is known closed), not on a stop request.
    // Next polls drain available input, classify a partial tail/missing final
    // summary as incomplete, and can finish without a normal-shutdown marker.
    void source_ended() noexcept;
private:
    Reader();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

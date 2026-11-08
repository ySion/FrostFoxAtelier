#include "logging/detail/native.hpp"
#include "detail/format.hpp"
#include "detail/io.hpp"
#include "foundation/scope_exit.hpp"
#include "foundation/string.hpp"
#include "platform/utf.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/Sink.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>

namespace atelier::logging {
namespace {
using namespace detail;

using Frontend = quill::FrontendImpl<detail::FrontendOptions>;

std::atomic<Run*> active_run{};

bool started_once{}; // Owner-thread lifecycle, not a lazy singleton.

void emergency() noexcept {
    constexpr char message[] = "Atelier logging facility failed; inspect run health.\n";
    DWORD written{};
    const auto output = GetStdHandle(STD_ERROR_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE)
        WriteFile(output, message, static_cast<DWORD>(sizeof(message) - 1), &written, nullptr);
    OutputDebugStringA(message);
}

void copy_detail(std::string& destination, std::string_view input) noexcept {
    if (valid_utf8(input)) {
        destination.assign(StringView{input}.utf8_prefix(2048).std_view());
        return;
    }
    // Preserve unclassified bytes in a reversible representation instead of
    // guessing a cause, replacing bytes, or making health.json invalid UTF-8.
    destination.assign("raw-bytes-hex:");
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char byte : input) {
        if (destination.size() + 2 > 2048)
            break;
        destination.push_back(digits[byte >> 4]);
        destination.push_back(digits[byte & 15]);
    }
}

struct State {
    RunOptions options;

    RunInfo info;

    detail::Logger* logger{};

    std::atomic<RunState> lifecycle{RunState::running};

    std::array<std::atomic<std::uint64_t>, 8> submissions{};

    std::atomic<std::uint64_t> failures{};

    std::atomic<std::uint64_t> notifications{};

    std::atomic<std::uint64_t> sequence{};

    std::atomic<bool> last_flush{};

    std::atomic<bool> explicit_flush{};

    std::mutex first_mutex;

    FacilityIssue first_issue;

    std::string first_notice;

    CapturedDiagnostic first_error;

    bool has_issue{};

    bool has_notice{};

    bool has_error{};

    std::mutex producers_mutex;

    std::array<quill::detail::ThreadContext*, maximum_producers> producers{};

    std::uint64_t next_producer{1};

    std::mutex publication_mutex;

    State() {
        first_issue.message.reserve(2048);
        first_notice.reserve(2048);
        auto& entry = first_error.entry;
        entry.category.reserve(64);
        entry.event_code.reserve(128);
        entry.message.reserve(maximum_diagnostic_snapshot_bytes);
        entry.source.file.reserve(4096);
        entry.source.root.reserve(4096);
        entry.source.function.reserve(4096);
        entry.source.disk_path.reserve(4096);
        entry.repeat.emplace();
        entry.repeat->identity.reserve(256);
        entry.fields.resize(maximum_fields);
        for (auto& field : entry.fields) {
            field.key.reserve(48);
            field.value = std::string{};
            std::get<std::string>(field.value).reserve(4096);
        }
    }

    void fault(FacilityFault kind, std::string_view message, std::optional<std::uint32_t> code = {}) noexcept {
        failures.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock{first_mutex};
        if (!has_issue) {
            first_issue.fault = kind;
            first_issue.native_code = code;
            copy_detail(first_issue.message, message);
            has_issue = true;
            emergency();
        }
    }

    void notice(std::string_view message) noexcept {
        notifications.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock{first_mutex};
        if (!has_notice) {
            copy_detail(first_notice, message);
            has_notice = true;
        }
    }

    void capture(const Entry& source, SubmitResult identity, bool is_template) noexcept {
        if (source.severity < Severity::error)
            return;
        std::lock_guard lock{first_mutex};
        if (has_error)
            return;
        auto& target = first_error.entry;
        std::size_t remaining = maximum_diagnostic_snapshot_bytes - 128;
        auto copy = [&](std::string& destination, std::string_view input) {
            const auto part = StringView{input}.utf8_prefix(remaining).std_view();
            destination.assign(part);
            remaining -= part.size();
            first_error.truncated |= part.size() != input.size();
        };
        target.severity = source.severity;
        copy(target.category, source.category);
        copy(target.event_code, source.event_code);
        const auto source_bytes = source.source.file.size() + source.source.root.size() +
                                  source.source.function.size() + source.source.disk_path.size();
        if (source_bytes <= remaining) {
            copy(target.source.file, source.source.file);
            copy(target.source.root, source.source.root);
            copy(target.source.function, source.source.function);
            copy(target.source.disk_path, source.source.disk_path);
            target.source.line = source.source.line;
            target.source.column = source.source.column;
            target.source.version = source.source.version;
            target.source.offset = source.source.offset;
        } else
            first_error.truncated = true; // Never publish an invalid partial relative path/root pair.
        if (source.repeat && source.repeat->identity.size() + 16 <= remaining) {
            remaining -= 16;
            copy(target.repeat->identity, source.repeat->identity);
            target.repeat->count = source.repeat->count;
        } else {
            first_error.truncated |= source.repeat.has_value();
            target.repeat.reset();
        }
        std::size_t count{};
        for (const auto& field : source.fields) {
            if (field.key.size() + 32 > remaining) {
                first_error.truncated = true;
                break;
            }
            auto& destination = target.fields[count++];
            remaining -= 32;
            copy(destination.key, field.key);
            if (const auto* string = std::get_if<std::string>(&field.value))
                copy(std::get<std::string>(destination.value), *string);
            else
                std::visit(
                    [&](const auto& item) {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(item)>, std::string>)
                            destination.value = item;
                    },
                    field.value);
        }
        target.fields.resize(count);
        copy(target.message, source.message);
        first_error.producer_id = identity.producer_id;
        first_error.producer_sequence = identity.producer_sequence;
        first_error.message_is_template = is_template;
        has_error = true;
    }

    Health health() {
        Health result;
        result.state = lifecycle.load();
        auto counter = [&](SubmitStatus status) { return submissions[static_cast<std::size_t>(status)].load(); };
        result.accepted = counter(SubmitStatus::accepted);
        result.rejected_queue_full = counter(SubmitStatus::queue_full);
        result.rejected_producer_limit = counter(SubmitStatus::producer_limit);
        result.rejected_record_too_large = counter(SubmitStatus::record_too_large);
        result.rejected_invalid_record = counter(SubmitStatus::invalid_record);
        result.rejected_internal_failure = counter(SubmitStatus::internal_failure);
        result.facility_failures = failures.load();
        result.backend_notifications = notifications.load();
        result.last_record_sequence = sequence.load();
        std::lock_guard lock{first_mutex};
        if (has_issue)
            result.first_facility_issue = first_issue;
        if (has_notice)
            result.first_backend_notification = first_notice;
        if (has_error)
            result.first_error = first_error;
        return result;
    }

    bool publish(bool final, bool acknowledged, bool flushed) noexcept {
        std::lock_guard lock{publication_mutex};
        PublishedHealth published{unix_ns(), final, {health(), acknowledged, flushed, true}};
        auto document = health_json(info, published);
        if (!document) {
            fault(FacilityFault::summary_failed, document.error().message);
            return false;
        }
        const auto bytes = json::write(document->root());
        const auto result = atomic_write(info.directory / L"health.json", {bytes.data(), bytes.size()});
        if (!result)
            fault(FacilityFault::summary_failed, result.error().message, result.error().native_code);
        return result.has_value();
    }
};

std::atomic<State*> active_state{};

struct Producer {
    State* state{};

    std::uint64_t id{};

    std::uint64_t sequence{};
};

thread_local Producer producer;

bool admit(State& state) noexcept {
    if (producer.state == &state && producer.id != 0)
        return true;
    std::lock_guard lock{state.producers_mutex};
    std::array<bool, maximum_producers> present{};
    // Manager lock pins contexts during this comparison. Do not dereference stale
    // addresses; the backend removes a context only after queue AND transit drain.
    quill::detail::ThreadContextManager::instance().for_each_thread_context([&](auto* context) {
        for (std::size_t i = 0; i < maximum_producers; ++i)
            if (state.producers[i] == context)
                present[i] = true;
    });
    for (std::size_t i = 0; i < maximum_producers; ++i)
        if (!present[i])
            state.producers[i] = nullptr;
    auto slot = std::find(state.producers.begin(), state.producers.end(), nullptr);
    if (slot == state.producers.end())
        return false;
    // The backend can retire a context after the sweep releases its lock, and
    // this new TLS allocation can reuse that address. No other admission can
    // run here, so any matching old slot is retired, not another live producer.
    // Remove that stale identity before publishing the new context's slot.
    auto* context = quill::detail::get_local_thread_context<detail::FrontendOptions>();
    for (auto& existing : state.producers)
        if (existing == context)
            existing = nullptr;
    *slot = context;
    producer = {&state, state.next_producer++, 0};
    return true;
}

void retire_logger(State& state) noexcept {
    // Used only before a backend ever starts, or after its full join; there are
    // no queued references. The sole frontend is this module's fixed policy.
    if (!state.logger)
        return;
    Frontend::remove_logger(state.logger);
    std::vector<std::string> removed;
    auto retired = quill::detail::LoggerManager::instance().cleanup_invalidated_loggers([] { return true; }, removed);
    static_cast<void>(retired);
}

// The pinned rotating sink swallows some remove failures. Own the small fixed
// Win32 file policy here instead: every write/rename/delete/close has a typed result.
class JsonSink final : public quill::Sink {
public:
    explicit JsonSink(std::shared_ptr<State> state) : state_(std::move(state)) {
        for (std::size_t i = 0; i <= retained_rotated_files; ++i)
            paths_[i] = state_->info.directory / (i == 0 ? "events.jsonl" : "events." + std::to_string(i) + ".jsonl");
    }

    Result<void> open() {
        file_.reset(CreateFileW(paths_[0].c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file_) {
            const auto code = GetLastError();
            state_->fault(FacilityFault::file_open_failed, "Create log file", code);
            return std::unexpected(io_failure("Create log file", code));
        }
        size_ = 0;
        return {};
    }

    void close() noexcept {
        if (file_ && !CloseHandle(file_.release()))
            state_->fault(FacilityFault::file_flush_failed, "Close log file", GetLastError());
    }

    void flush_sink() override {
        const auto check_file = [&]() -> Result<void> {
            if (!file_)
                return std::unexpected(io_failure("Log file unavailable", ERROR_INVALID_HANDLE));
            // Writes go directly to the OS, with no C stdio/userspace buffering.
            // This checks reachability, not durable media (no FlushFileBuffers).
            auto visible = open_read(paths_[0]);
            if (!visible)
                return std::unexpected(io_failure("Current log file is no longer reachable"));
            ATELIER_TRY_BIND(visible_identity, file_identity(visible.get()));
            ATELIER_TRY_BIND(open_identity, file_identity(file_.get()));
            if (*visible_identity != *open_identity)
                return std::unexpected(io_failure("Current log file was replaced", ERROR_FILE_INVALID));
            return {};
        };
        const auto result = check_file();
        if (!result)
            state_->fault(FacilityFault::file_flush_failed, result.error().message, result.error().native_code);
        state_->last_flush.store(result && !write_failed_);
    }

    void flush_sink(quill::SinkFlushReason reason) override {
        flush_sink();
        if (reason == quill::SinkFlushReason::Explicit)
            state_->explicit_flush.store(state_->last_flush.load());
    }

protected:
    void write_log(const quill::MacroMetadata* metadata, std::uint64_t, std::string_view, std::string_view,
                   const std::string&, std::string_view, quill::LogLevel, std::string_view, std::string_view,
                   const std::vector<std::pair<std::string, std::string>>*, std::string_view message,
                   std::string_view) override {
        if (!valid_utf8(message)) {
            state_->fault(FacilityFault::invalid_utf8, "Rendered message is not UTF-8");
            return;
        }
        if (message.size() > maximum_message_bytes) {
            state_->fault(FacilityFault::output_too_large, "Rendered message exceeds 128 KiB");
            return;
        }
        auto document = json::Document::parse(metadata->tags(), {16});
        if (!document) {
            state_->fault(FacilityFault::encoding_failed, document.error().message);
            return;
        }
        json::Builder builder{*document};
        builder.set_validated(builder.root(), "message", message);
        [[maybe_unused]] const auto sequence =
            builder.set(builder.root(), "record_sequence", std::to_string(state_->sequence.fetch_add(1) + 1));
        assert(sequence); // Generated decimal ASCII cannot fail UTF-8 validation.
        auto line = json::write(builder.root(), {.trailing_lf = true});
        if (line.size() > maximum_json_line_bytes) {
            state_->fault(FacilityFault::output_too_large, "Encoded JSONL line exceeds limit");
            return;
        }
        if (!file_) {
            state_->fault(FacilityFault::file_write_failed, "No writable log file");
            return;
        }
        if (size_ && size_ + line.size() > rotation_bytes && !rotate())
            return;
        const auto result = write_all(file_.get(), {line.data(), line.size()});
        if (!result) {
            write_failed_ = true;
            state_->fault(FacilityFault::file_write_failed, result.error().message, result.error().native_code);
            // A partial line must never be followed by a seemingly valid record.
            close();
            return;
        }
        size_ += line.size();
    }

    void run_periodic_tasks() override {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_publication_ >= std::chrono::seconds{1}) {
            last_publication_ = now;
            state_->publish(false, false, state_->last_flush.load());
        }
    }

private:
    bool rotate() noexcept {
        close();
        const auto& oldest = paths_.back();
        if (!DeleteFileW(oldest.c_str())) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) {
                state_->fault(FacilityFault::retention_failed, "Remove oldest log segment", code);
                return false;
            }
        }
        for (std::size_t i = retained_rotated_files; i != 0; --i) {
            if (!MoveFileExW(paths_[i - 1].c_str(), paths_[i].c_str(), 0)) {
                const auto code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND || i == 1) {
                    state_->fault(FacilityFault::rotation_failed, "Rotate log segment", code);
                    return false;
                }
            }
        }
        return open().has_value();
    }

    std::shared_ptr<State> state_;

    std::array<std::filesystem::path, retained_rotated_files + 1> paths_;

    Handle file_;

    std::size_t size_{};

    bool write_failed_{};

    std::chrono::steady_clock::time_point last_publication_{std::chrono::steady_clock::now()};
};

Result<std::string> make_run_id() {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return std::unexpected(Error{ErrorCode::invalid_options, "Cannot generate run identity"});
    std::string result;
    result.reserve(32);
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : random) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

Result<std::filesystem::path> default_root() {
    const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!count)
        return std::unexpected(io_failure("LOCALAPPDATA is not available"));
    std::wstring value(count, L'\0');
    const auto copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), count);
    if (!copied || copied >= count)
        return std::unexpected(io_failure("LOCALAPPDATA changed"));
    value.resize(copied);
    auto result = std::filesystem::path{value} / L"FrostFoxAtelier" / L"Logs";
    if (!result.is_absolute())
        return std::unexpected(Error{ErrorCode::invalid_options, "LOCALAPPDATA must be absolute"});
    return result;
}

void retain_runs(State& state, const std::filesystem::path& root) noexcept {
    struct Candidate {
        std::filesystem::path path;

        std::uint64_t time;
    };
    std::vector<Candidate> completed;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{root, error};
         !error && iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
        const auto& item = *iterator;
        const auto directory = item.is_directory(error);
        if (error)
            break;
        if (!directory)
            continue;
        const auto symlink = item.is_symlink(error);
        if (error)
            break;
        if (symlink || item.path() == state.info.directory)
            continue;
        const auto manifest = read_small(item.path() / L"run.json");
        if (!manifest)
            continue; // Unknown/incomplete directories are never candidates.
        const auto info = read_run(*manifest);
        if (!info || info->directory != item.path())
            continue;
        const auto summary = read_small(item.path() / L"health.json", 256 * 1024);
        if (!summary)
            continue;
        const auto health = read_health(*summary, *info);
        if (health && health->final)
            completed.push_back({item.path(), info->started_unix_ns});
    }
    if (error) {
        state.fault(FacilityFault::retention_failed, "Enumerate completed run directories");
        return; // Never delete from a partial candidate list.
    }
    std::sort(completed.begin(), completed.end(), [](const auto& a, const auto& b) { return a.time > b.time; });
    for (std::size_t i = retained_completed_runs; i < completed.size(); ++i) {
        const auto& directory = completed[i].path;
        Handle lock{CreateFileW((directory / L"active.lock").c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!lock) {
            const auto code = GetLastError();
            if (code != ERROR_SHARING_VIOLATION)
                state.fault(FacilityFault::retention_failed, "Acquire completed run lock", code);
            continue;
        }
        std::filesystem::remove_all(directory, error);
        if (error)
            state.fault(FacilityFault::retention_failed, "Remove completed run directory",
                        static_cast<std::uint32_t>(error.value()));
    }
}
}

struct Run::Impl {
    std::shared_ptr<State> state{std::make_shared<State>()};

    std::shared_ptr<JsonSink> sink;

    Handle lock;

    bool backend_started{};

    bool emergency_cleanup{};

    bool summary_published{};

    bool acknowledged{};

    bool flushed{};

    DWORD owner{GetCurrentThreadId()};
};

Run::Run() : impl_(std::make_unique<Impl>()) {}

Run::~Run() {
    if (!impl_->backend_started || impl_->state->lifecycle.load() == RunState::finished)
        return;
    impl_->emergency_cleanup = true;
    impl_->state->fault(FacilityFault::lifecycle_violation,
                        "Run destroyed without explicit finish; producers must already be stopped");
    static_cast<void>(finish());
    active_run.store(nullptr, std::memory_order_release);
    active_state.store(nullptr, std::memory_order_release);
}

const RunInfo& Run::info() const noexcept {
    return impl_->state->info;
}

Health Run::health() const {
    return impl_->state->health();
}

Run* current_run() noexcept {
    return active_run.load(std::memory_order_acquire);
}

bool enabled(Severity severity, std::string_view category) noexcept {
    const auto* state = active_state.load(std::memory_order_acquire);
    if (!state || severity >= Severity::off || state->lifecycle.load() != RunState::running)
        return false;
    auto minimum = state->options.minimum;
    for (const auto& rule : state->options.categories)
        if (rule.category == category) {
            minimum = rule.minimum;
            break;
        }
    return severity >= minimum && minimum != Severity::off;
}

Result<SourceLocation> native_source(std::source_location location) noexcept {
    SourceLocation result;
    result.file = location.file_name();
    result.function = location.function_name();
    result.line = location.line();
    // Clang's source_location column is not promised to be a UTF-8 byte column.
    result.column = 0;
    if (!result.file.empty()) {
        auto path = platform::utf8_to_utf16(result.file);
        if (!path)
            return std::unexpected(
                Error{ErrorCode::invalid_options, "Invalid UTF-8 source path", path.error().system_code});
        if (!std::filesystem::path{*path}.is_absolute())
            result.root = ATELIER_LOGGING_SOURCE_ROOT;
    }
    return result;
}

Result<std::unique_ptr<Run>> start_run(RunOptions options) noexcept {
    if (started_once || current_run())
        return std::unexpected(Error{ErrorCode::already_started, "A logging run has already started", {}});
    std::unique_ptr<Run> run;
    bool created{};
    if (options.directory.native().find(L'\0') != std::wstring::npos)
        return std::unexpected(Error{ErrorCode::invalid_options, "Invalid UTF-8 path", {}});
    ScopeExit rollback{[&]() noexcept {
        if (run) {
            retire_logger(*run->impl_->state);
            if (run->impl_->sink)
                run->impl_->sink->close();
            run->impl_->sink.reset();
            run->impl_->lock.reset();
            producer = {};
            if (created) {
                std::error_code ignored;
                std::filesystem::remove_all(run->info().directory, ignored);
            }
        }
        emergency();
    }};
    Entry probe;
    probe.category = "logging";
    probe.event_code = "logging.options";
    if (options.minimum > Severity::off || options.categories.size() > 64)
        return std::unexpected(Error{ErrorCode::invalid_options, "Invalid thresholds"});
    for (std::size_t i = 0; i < options.categories.size(); ++i) {
        const auto& rule = options.categories[i];
        probe.category = rule.category;
        if (const auto problem = entry_problem(probe); !problem.empty())
            return std::unexpected(Error{ErrorCode::invalid_options, std::string{problem}});
        if (rule.minimum > Severity::off)
            return std::unexpected(Error{ErrorCode::invalid_options, "Invalid category threshold"});
        for (std::size_t j = 0; j < i; ++j)
            if (options.categories[j].category == rule.category)
                return std::unexpected(Error{ErrorCode::invalid_options, "Duplicate category threshold"});
    }
    if (!options.directory.empty() && !options.directory.is_absolute())
        return std::unexpected(Error{ErrorCode::invalid_options, "Run directory must be absolute"});
    run.reset(new Run);
    auto& state = *run->impl_->state;
    state.options = std::move(options);
    ATELIER_TRY_BIND(identity, make_run_id());
    state.info = {std::move(*identity),       state.options.process_name, GetCurrentProcessId(), unix_ns(), {},
                  state.options.parent_run_id};
    const bool default_directory = state.options.directory.empty();
    if (default_directory) {
        auto root = default_root();
        if (!root) {
            root.error().code = ErrorCode::invalid_options;
            return std::unexpected(std::move(root.error()));
        }
        state.info.directory = *root / state.info.run_id;
    } else
        state.info.directory = state.options.directory.lexically_normal();
    auto manifest = run_json(state.info);
    if (!manifest)
        return std::unexpected(
            Error{ErrorCode::invalid_options, manifest.error().message, manifest.error().native_code});
    const auto manifest_bytes = json::write(manifest->root());
    std::error_code error;
    std::filesystem::create_directories(state.info.directory.parent_path(), error);
    if (error)
        return std::unexpected(io_failure("Create run parent directories", static_cast<DWORD>(error.value())));
    if (!CreateDirectoryW(state.info.directory.c_str(), nullptr)) {
        const auto code = GetLastError();
        auto rejected = io_failure("Create fresh run directory", code);
        if (code == ERROR_ALREADY_EXISTS)
            rejected.code = ErrorCode::directory_in_use;
        return std::unexpected(std::move(rejected));
    }
    created = true;
    run->impl_->lock.reset(CreateFileW((state.info.directory / L"active.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!run->impl_->lock)
        return std::unexpected(io_failure("Create active run lock"));
    ATELIER_TRY(atomic_write(state.info.directory / L"run.json", {manifest_bytes.data(), manifest_bytes.size()}));
    if (default_directory)
        retain_runs(state, state.info.directory.parent_path());
    run->impl_->sink = std::make_shared<JsonSink>(run->impl_->state);
    ATELIER_TRY(run->impl_->sink->open());
    state.logger =
        Frontend::create_or_get_logger("atelier-" + state.info.run_id, run->impl_->sink,
                                       quill::PatternFormatterOptions{"%(message)"}, quill::ClockSourceType::System);
    state.logger->set_log_level(quill::LogLevel::TraceL3);
    // Reserve the owner/control producer before other threads can submit.
    static_cast<void>(admit(state));
    quill::BackendOptions backend;
    backend.transit_event_buffer_initial_capacity = 8;
    backend.transit_events_hard_limit = 8;
    // Quill v13.1.0 requires soft <= hard even though soft is global and hard
    // is per producer. Keep the small per-producer staging allocation.
    backend.transit_events_soft_limit = 8;
    backend.sleep_duration = std::chrono::microseconds{100};
    backend.wait_for_queues_to_empty_before_exit = true;
    // Only the resident host owns Quill; DLL/JIT producers use submit().
    backend.check_backend_singleton_instance = false;
    backend.check_printable_char = {};
    backend.error_notifier = [state = run->impl_->state](const std::string& message) noexcept {
        state->notice(message);
    };
    quill::Backend::start(backend);
    if (!quill::Backend::is_running())
        fatal("Quill backend did not start.");
    run->impl_->backend_started = true;
    started_once = true;
    active_state.store(&state, std::memory_order_release);
    active_run.store(run.get(), std::memory_order_release);
    state.publish(false, false, false);
    rollback.release();
    return run;
}

FlushReport Run::flush() noexcept {
    auto& state = *impl_->state;
    if (state.lifecycle.load() == RunState::finished)
        return {state.health(), impl_->acknowledged, impl_->flushed, impl_->summary_published};
    if (GetCurrentThreadId() != impl_->owner) {
        state.fault(FacilityFault::lifecycle_violation, "flush must be called by the run owner");
        return {state.health(), false, false, false};
    }
    state.explicit_flush.store(false);
    state.logger->flush_log();
    const bool acknowledged = true;
    const bool flushed = acknowledged && state.explicit_flush.load();
    const bool published = state.publish(false, acknowledged, flushed);
    return {state.health(), acknowledged, flushed, published};
}

FlushReport Run::finish() noexcept {
    auto& state = *impl_->state;
    if (state.lifecycle.load() == RunState::finished)
        return {state.health(), impl_->acknowledged, impl_->flushed, impl_->summary_published};
    if (GetCurrentThreadId() != impl_->owner) {
        state.fault(FacilityFault::lifecycle_violation, "finish must be called by the run owner");
        return {state.health(), false, false, false};
    }
    state.lifecycle.store(RunState::finishing);
    // The host has joined all producers. Backend::stop drains every queue/transit
    // buffer, unlike treating one producer's flush as a global barrier.
    state.logger->flush_log();
    impl_->acknowledged = true;
    quill::Backend::stop();
    if (quill::Backend::is_running())
        fatal("Quill backend still running after stop.");
    impl_->sink->flush_sink();
    const auto failures_before_close = state.failures.load();
    impl_->sink->close();
    impl_->flushed = state.last_flush.load() && state.failures.load() == failures_before_close;
    retire_logger(state);
    state.logger = nullptr;
    state.lifecycle.store(RunState::finished);
    impl_->summary_published = state.publish(!impl_->emergency_cleanup, impl_->acknowledged, impl_->flushed);
    active_run.store(nullptr, std::memory_order_release);
    active_state.store(nullptr, std::memory_order_release);
    return {state.health(), impl_->acknowledged, impl_->flushed, impl_->summary_published};
}

SubmitResult submit(Entry entry) noexcept {
    if (!current_run())
        return {SubmitStatus::no_active_run};
    if (entry.severity < Severity::off && !enabled(entry.severity, entry.category))
        return {SubmitStatus::filtered};
    return detail::enqueue(entry, false, "{}", std::string_view{entry.message});
}

namespace detail {
const quill::MacroMetadata* metadata() noexcept {
    static constexpr quill::MacroMetadata value{"logging:0",
                                                "submit",
                                                "{}",
                                                nullptr,
                                                quill::LogLevel::Info,
                                                quill::MacroMetadata::Event::LogWithRuntimeMetadataDeepCopy};
    return &value;
}

quill::LogLevel quill_level(Severity severity) noexcept {
    constexpr std::array values{quill::LogLevel::TraceL3, quill::LogLevel::Debug, quill::LogLevel::Info,
                                quill::LogLevel::Warning, quill::LogLevel::Error, quill::LogLevel::Critical,
                                quill::LogLevel::None};
    return values[static_cast<std::size_t>(severity)];
}

SubmitResult complete(SubmitResult result, SubmitStatus status) noexcept {
    result.status = status;
    if (auto* state = active_state.load(std::memory_order_acquire);
        state && status != SubmitStatus::filtered && status != SubmitStatus::no_active_run)
        state->submissions[static_cast<std::size_t>(status)].fetch_add(1, std::memory_order_relaxed);
    return result;
}

void prepare(Ticket& ticket, const Entry& entry, bool is_template) {
    auto* state = active_state.load(std::memory_order_acquire);
    if (!state || state->lifecycle.load() != RunState::running)
        return;
    if (!entry_problem(entry).empty()) {
        ticket.result = complete({}, SubmitStatus::invalid_record);
        return;
    }
    if (!enabled(entry.severity, entry.category)) {
        ticket.result.status = SubmitStatus::filtered;
        return;
    }
    if (!admit(*state)) {
        ticket.result = complete({}, SubmitStatus::producer_limit);
        return;
    }
    ticket.result = {SubmitStatus::accepted, producer.id, ++producer.sequence};
    state->capture(entry, ticket.result, is_template);
    Record record;
    record.run_id = state->info.run_id;
    record.process_id = state->info.process_id;
    record.thread_id = GetCurrentThreadId();
    record.producer_id = producer.id;
    record.producer_sequence = producer.sequence;
    record.timestamp_unix_ns = unix_ns();
    // Do not copy the message into the envelope; Quill owns its encoded argument.
    auto envelope = record_envelope(record, entry);
    assert(envelope); // Entry was validated before admission and capture.
    const auto bytes = json::write(envelope->root());
    // Quill's tag storage is an owning std::string, not a validated String.
    ticket.envelope.assign(bytes.data(), bytes.size());
    ticket.logger = state->logger;
}
}
}

#pragma once
#include "application/play_link.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "runtime/runtime.hpp"
#include <Windows.h>
#include <array>
#include <chrono>
#include <filesystem>

namespace atelier::application {
// Owns one project run and its descendants. Runtime identities and GPU objects
// remain in that process; only scene values, stop/ack events, text and change
// messages cross here. With live, the run gets a change link and *live its editor end.
class PlayProcess final {
public:
    struct StreamStatus {
        std::uint64_t received_bytes{};

        std::uint64_t dropped_bytes{}; // Read, then evicted from the bounded buffer.

        std::uint64_t abandoned_bytes{}; // Still in the pipe when close discarded it, as measured by PeekNamedPipe.

        DWORD read_error{}; // First unexpected pipe error; ERROR_BROKEN_PIPE is EOF, not a failure.

        bool eof{}; // Transport EOF was observed, not a guarantee that the producer flushed its own buffers.

        bool tail_loss_unknown{}; // Forced termination, a read failure, or closure before EOF may lose more bytes.

        bool operator==(const StreamStatus&) const = default;
    };

    struct OutputStatus {
        StreamStatus standard_output;

        StreamStatus standard_error;

        bool process_exited{};

        bool pending{}; // Pipe handles still need polling, independently of buffered bytes already available to take.
    };

    struct RawStream {
        Array<char> bytes; // Unmodified bytes: no decoding, line splitting, severity inference or newline insertion.

        std::uint64_t offset{}; // Byte offset in this stream; gaps identify precisely where retention dropped bytes.
    };

    struct RawOutput {
        RawStream standard_output;

        RawStream standard_error;

        OutputStatus status;
    };

    static constexpr std::size_t output_capacity = 64 * 1024; // Retained bytes per stream.

    static constexpr std::size_t output_budget =
        64 * 1024; // At most 16 reads and this many bytes per poll, shared by both streams.

    PlayProcess() = default;

    ~PlayProcess();

    PlayProcess(const PlayProcess&) = delete;

    PlayProcess& operator=(const PlayProcess&) = delete;

    // A successful start resets the previous run's output and counters; take its remaining bytes first.
    Result<void> start(const std::filesystem::path& project, const std::filesystem::path& scene, bool headless = false,
                       std::uint64_t frames = 0, const std::filesystem::path& report = {}, bool preview_ui = false,
                       bool halt = false, std::unique_ptr<LiveLink>* live = nullptr);

    void stop() noexcept;

    // Never waits for output. After process exit, subsequent polls finish the pipes before running becomes false.
    void poll() noexcept;

    // Shutdown only: 500 ms for stop acknowledgement or exit; acknowledged cleanup
    // and forced-exit observation share a 4 s total budget measured from close entry.
    // At most eight poll budgets are read, without waiting for pipe data. Remaining bytes are accounted
    // as abandoned where measurable; unobserved EOF is marked unknown. Buffered bytes remain available.
    // Unobserved exit logs an error and retains the process handle for nonblocking poll;
    // repeated close/destruction never starts another wait budget.
    void close() noexcept;

    bool running() const noexcept { return process_ != nullptr; }

    bool stopping() const noexcept { return stopping_; }

    bool forced() const noexcept { return forced_; }

    Optional<DWORD> exit_code() const noexcept { return exit_code_; }

    const std::filesystem::path& log_directory() const noexcept { return log_directory_; }

    OutputStatus output_status() const noexcept;

    RawOutput take_raw_output();

private:
    struct OutputPipe {
        HANDLE handle{};

        Array<char> bytes; // Capacity is reserved before launch; byte draining never allocates.

        StreamStatus status;
    };

    HANDLE process_{};

    HANDLE job_{};

    HANDLE stop_{};

    HANDLE stop_ack_{};

    std::filesystem::path log_directory_;

    std::array<OutputPipe, 2> output_;

    bool stopping_{};

    bool acknowledged_{};

    bool forced_{};

    bool exited_{};

    bool closing_{};

    Optional<DWORD> exit_code_;

    std::chrono::steady_clock::time_point stop_began_{};

    void drain() noexcept;

    void observe_acknowledgement() noexcept;

    DWORD observe_exit(DWORD timeout_ms = 0) noexcept;

    void force() noexcept;

    void abandon(OutputPipe&) noexcept;

    void release() noexcept;
};
}

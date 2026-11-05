#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string_view>

// Always-on frame phase timing. Modules declare their own phase markers; the
// host assembles them into one process catalog at startup. A Recorder is bound
// to its host thread only while a frame is open, so threads without one (for
// example physics workers) record nothing and unbound scopes cost a TLS read.
namespace atelier::timing {

using Clock = std::chrono::steady_clock;

inline constexpr std::size_t capacity = 64;

inline constexpr std::uint8_t unregistered = 0xff;

class Recorder;

// A static phase declaration owned by the module that opens it. Declare markers
// as `inline constinit`; the parent must be a marker of the same or a lower
// module. install() assigns the catalog index once per process.
class Marker {
public:
    constexpr Marker(std::string_view key, std::string_view title, const Marker* parent) noexcept
        : key_(key), title_(title), parent_(parent) {}

    Marker(const Marker&) = delete;

    Marker& operator=(const Marker&) = delete;

    [[nodiscard]] constexpr std::string_view key() const noexcept { return key_; }

    [[nodiscard]] constexpr std::string_view title() const noexcept { return title_; }

    [[nodiscard]] constexpr const Marker* parent() const noexcept { return parent_; }

    [[nodiscard]] std::uint8_t index() const noexcept { return index_; }

    [[nodiscard]] bool registered() const noexcept { return index_ != unregistered; }

private:
    friend class Recorder;

    friend class Scope;

    friend void install(std::initializer_list<std::span<Marker* const>>);

    friend void record(const Marker&, std::chrono::nanoseconds, std::uint32_t) noexcept;

    std::string_view key_;

    std::string_view title_;

    const Marker* parent_;

    std::uint8_t index_{unregistered};

    std::uint8_t parent_index_{unregistered};
};

// The root of every host frame and the only marker owned by foundation.
inline constinit Marker frame{"frame", "Frame", nullptr};

// Registers `frame` followed by each module's markers in order, then freezes the
// catalog. Call once per process at host startup, before any thread records.
// Throws std::logic_error on a second call and std::invalid_argument for a null
// or repeated marker, a duplicate key, a parent not registered earlier, or more
// than `capacity` markers; a rejected call registers nothing.
void install(std::initializer_list<std::span<Marker* const>> modules);

// Installed markers in index order (frame first); empty before install().
[[nodiscard]] std::span<const Marker* const> catalog() noexcept;

[[nodiscard]] std::size_t depth(const Marker&) noexcept;

// Inclusive wall time per catalog index, summed over every entry in one frame.
struct FrameRecord {
    std::uint64_t sequence{}; // Ordinal of this frame within its Recorder.

    Clock::time_point begin{};

    std::array<std::chrono::nanoseconds, capacity> duration{};

    std::array<std::uint32_t, capacity> entries{};

    // Scopes or records that were not registered or not opened under their
    // declared parent, zero/overflowing entry counts, and nested host frames.
    // None of them was recorded.
    std::uint32_t structure_errors{};

    const Marker* first_error{};

    [[nodiscard]] std::chrono::nanoseconds time(const Marker& marker) const noexcept {
        return marker.registered() ? duration[marker.index()] : std::chrono::nanoseconds{};
    }

    [[nodiscard]] std::uint32_t count(const Marker& marker) const noexcept {
        return marker.registered() ? entries[marker.index()] : 0;
    }
};

namespace detail {
extern constinit thread_local Recorder* bound;
}

// One host thread's frame record. Not thread-safe: it is only written by the
// thread that bound it through Frame.
class Recorder {
public:
    Recorder() = default;

    Recorder(const Recorder&) = delete;

    Recorder& operator=(const Recorder&) = delete;

    // The last finished frame; a frame in progress is incomplete.
    [[nodiscard]] const FrameRecord& last() const noexcept { return record_; }

    // Binds the Recorder to the calling thread and opens `frame`. Must not nest:
    // an inner Frame is counted as a structure error of the outer one and records nothing.
    class Frame {
    public:
        explicit Frame(Recorder&) noexcept;

        ~Frame() { finish(); }

        Frame(const Frame&) = delete;

        Frame& operator=(const Frame&) = delete;

        // Closes the frame and unbinds the Recorder. Every Scope must already be closed.
        const FrameRecord& finish() noexcept;

    private:
        Recorder* recorder_;
    };

private:
    friend class Scope;

    friend class Suspend;

    friend void record(const Marker&, std::chrono::nanoseconds, std::uint32_t) noexcept;

    bool accepts(const Marker& marker, std::uint32_t entries) noexcept {
        if (marker.index_ == unregistered || marker.parent_index_ != current_ || entries == 0 ||
            entries > std::numeric_limits<std::uint32_t>::max() - record_.entries[marker.index_]) {
            fail(marker);
            return false;
        }
        return true;
    }

    void fail(const Marker& marker) noexcept {
        ++record_.structure_errors;
        if (!record_.first_error)
            record_.first_error = &marker;
    }

    FrameRecord record_;

    std::uint64_t frames_{};

    std::uint8_t current_{unregistered};

    bool open_{};
};

// Times one entry of `marker` while a Recorder is bound on this thread and the
// marker's parent is the open phase. No allocation, lookup or lock.
class Scope {
public:
    explicit Scope(const Marker& marker) noexcept : recorder_(detail::bound) {
        if (!recorder_)
            return;
        if (!recorder_->accepts(marker, 1)) {
            recorder_ = nullptr;
            return;
        }
        index_ = marker.index_;
        previous_ = recorder_->current_;
        recorder_->current_ = index_;
        start_ = Clock::now();
    }

    ~Scope() {
        if (!recorder_)
            return;
        recorder_->record_.duration[index_] += Clock::now() - start_;
        ++recorder_->record_.entries[index_];
        recorder_->current_ = previous_;
    }

    Scope(const Scope&) = delete;

    Scope& operator=(const Scope&) = delete;

private:
    Recorder* recorder_;

    Clock::time_point start_{};

    std::uint8_t index_{};

    std::uint8_t previous_{};
};

// Adds a total duration once and an event/entry count, not a frame sample count.
// The bound host thread submits after workers join, under the marker's direct parent.
// Omit zero-event batches; entries and the accumulated frame count must fit uint32.
// Invalid batches record one structure error and no duration or entries.
void record(const Marker&, std::chrono::nanoseconds total_duration, std::uint32_t entries = 1) noexcept;

// Attributes nested work, such as an auxiliary surface, to the open phase as a whole.
class Suspend {
public:
    Suspend() noexcept : saved_(detail::bound) { detail::bound = nullptr; }

    // A frame finished while suspended stays unbound.
    ~Suspend() { detail::bound = saved_ && saved_->open_ ? saved_ : nullptr; }

    Suspend(const Suspend&) = delete;

    Suspend& operator=(const Suspend&) = delete;

private:
    Recorder* saved_;
};

}

#pragma once
#include "foundation/ring_deque.hpp"
#include "foundation/set.hpp"
#include "logging/logging.hpp"
#include "ui_language/live_document.hpp"
#include <array>
#include <tuple>

namespace atelier::editor {
namespace output_control {
inline constexpr ui::ControlKey table = 691;

inline constexpr ui::ControlKey base = 1ULL << 38;

inline constexpr ui::ControlKey level = base;

inline constexpr ui::ControlKey category = base + 1;

inline constexpr ui::ControlKey search = base + 2;

inline constexpr ui::ControlKey body = base + 3;

inline constexpr ui::ControlKey locate = base + 4;

inline constexpr ui::ControlKey open_directory = base + 5;

inline constexpr ui::ControlKey health = base + 6;

inline constexpr ui::ControlKey clear_filters = base + 7;

inline constexpr ui::ControlKey empty = base + 8;
}

// One UI-thread projection, independent of the logging backend's worker. Disk
// readers and explicitly unpersisted owned records share the same cache/model.
class Output {
public:
    static constexpr std::size_t maximum_records = 4096;

    static constexpr std::size_t maximum_body_bytes = 8 * 1024 * 1024;

    struct Item {
        ui::TableKey key{};

        logging::Record record;

        std::filesystem::path directory; // Original run, never the current project.

        std::optional<logging::SubmitStatus> rejection;
    };

    struct Row {
        ui::TableKey key{};

        ui::TableKey record_key{};

        std::uint64_t count{1};
    };

    struct Statistics {
        std::size_t body_bytes{};

        std::uint64_t evicted_records{};

        std::uint64_t evicted_body_bytes{};

        std::uint64_t invalid_owned{};

        std::uint64_t duplicate_owned{};

        std::uint64_t uncertain_submissions{};
    };

    Output();

    ~Output();

    Output(const Output&) = delete;

    Output& operator=(const Output&) = delete;

    struct Stream; // Borrow from add_run until this Output is destroyed.

    [[nodiscard]] logging::Result<Stream*> add_run(std::filesystem::path);

    void source_ended(Stream&);

    logging::SubmitResult submit_owned(logging::Entry);

    // Capture transport chunks as host-owned raw.stdout/raw.stderr events, not
    // child JSONL. Retains at most three UTF-8 bytes per stream across chunks;
    // invalid bytes and literal backslashes are reversibly escaped. Offsets are
    // original byte positions; ended flushes a final fragment without a newline.
    void capture_raw(Stream&, bool standard_error, std::string_view bytes, std::uint64_t offset, bool ended);

    // Standalone consumer seam only. Missing stream identity is assigned here;
    // recent 4096 supplied identities are checked before repeat counting. The
    // local assigned stream never accepts caller replay, even after eviction.
    // Hosted mode refuses this path, including caller-claimed JSONL records.
    bool accept_owned(logging::Record);

    bool poll(); // At most one Reader budget (256 KiB / 256 lines) per call.

    bool apply(const ui::Action&);

    void filter(logging::Severity minimum, std::wstring category = {}, std::wstring text = {});

    bool select(ui::TableKey);

    const RingDeque<Item>& records() const noexcept { return records_; }

    const std::vector<Row>& rows() const;

    const logging::Record* record(ui::TableKey) const;

    const logging::Record* selected_record() const;

    std::optional<logging::SourceLocation> selected_source() const;

    std::filesystem::path selected_directory() const;

    const logging::ReaderStatus* source_status(const std::filesystem::path&) const;

    const Statistics& statistics() const noexcept { return statistics_; }

    const std::wstring& health_text() const;

    std::shared_ptr<const ui::TableData> table() const;

    // Failure retains the old cache, but never returns it for a new selection.
    ui::Result<std::shared_ptr<ui::CodeDocument>> body() const;

    ui::Result<void> append(ui_language::ControlOutput, ui::ControlKey, ui_language::LiveDocument&, float height);

private:
    std::vector<std::unique_ptr<Stream>> streams_;

    std::size_t next_stream_{};

    bool pending_change_{};

    std::string memory_run_;

    std::optional<logging::RunInfo> host_;

    std::uint64_t sequence_{};

    std::uint64_t next_key_{1};

    using Identity = std::tuple<std::string, std::uint32_t, std::uint64_t, std::uint64_t>;

    Set<Identity> owned_identities_;

    RingDeque<Identity> owned_order_;

    RingDeque<Item> records_;

    Statistics statistics_;

    std::size_t unpersisted_{};

    logging::Severity minimum_{logging::Severity::trace};

    std::array<float, 5> widths_{82, 130, 600, 240, 220};

    std::wstring category_;

    std::wstring query_;

    std::wstring attachment_error_;

    mutable bool dirty_{true};

    mutable bool health_dirty_{true};

    mutable std::vector<Row> rows_;

    mutable std::shared_ptr<const ui::TableData> table_;

    mutable std::shared_ptr<ui::CodeDocument> body_;

    mutable ui::TableKey selected_{};

    mutable ui::TableKey body_key_{};

    mutable std::wstring health_;

    void retain(logging::Record, std::filesystem::path = {}, std::optional<logging::SubmitStatus> = {});

    void rebuild() const;

    void update_health() const;

    const Item* item(ui::TableKey) const;
};
}

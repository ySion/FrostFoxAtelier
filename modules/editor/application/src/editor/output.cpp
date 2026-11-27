#include "editor/output.hpp"
#include "application/diagnostics.hpp"
#include "foundation/map.hpp"
#include "foundation/string.hpp"
#include "memory/fatal.hpp"
#include "platform/utf.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <limits>
#include <tuple>
#include <utility>

namespace atelier::editor {
namespace {
std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring visible_text(std::wstring_view input, bool one_line = false) {
    std::wstring result;
    result.reserve(input.size());
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto c : input) {
        if (one_line && (c == L'\n' || c == L'\t')) {
            result += c == L'\n' ? L"\\n" : L"\\t";
        } else if ((c < 32 && c != L'\n' && c != L'\t') || (c >= 127 && c <= 159)) {
            result += L"\\u";
            for (int shift = 12; shift >= 0; shift -= 4)
                result += hex[(c >> shift) & 15];
        } else
            result += c;
    }
    return result;
}

std::wstring visible_text(std::string_view text, bool one_line = false) {
    return visible_text(platform::utf8_to_utf16_validated(text), one_line);
}

std::wstring short_text(std::wstring text, std::size_t limit = 256) {
    if (text.size() > limit) {
        text.resize(limit);
        if (text.back() >= 0xd800 && text.back() <= 0xdbff)
            text.pop_back();
        text += L"…";
    }
    return text;
}

bool contains_folded(std::wstring_view text, std::wstring_view needle) {
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }) != text.end() ||
           needle.empty();
}

bool displayable(const logging::Record& record) {
    return record.schema == logging::schema_version && record.entry.message.size() <= logging::maximum_message_bytes &&
           logging::entry_problem(record.entry).empty();
}

std::wstring submission_name(logging::SubmitStatus status) {
    using enum logging::SubmitStatus;
    switch (status) {
    case filtered:
        return L"filtered";
    case no_active_run:
        return L"no active run";
    case queue_full:
        return L"queue full";
    case producer_limit:
        return L"producer limit";
    case record_too_large:
        return L"record too large";
    case invalid_record:
        return L"invalid record";
    case internal_failure:
        return L"unknown internal failure";
    case accepted:
        return L"accepted";
    }
    return L"unknown";
}
}

struct Output::Stream {
    std::filesystem::path directory;

    std::string directory_text;

    std::unique_ptr<logging::Reader> reader;

    logging::ReaderStatus status;

    Optional<logging::PublishedHealth> health;

    struct RawTail {
        std::string bytes;

        std::uint64_t offset{};
    };

    std::array<RawTail, 2> raw;
};

Output::Output() {
    static std::atomic<std::uint64_t> sessions{};
    memory_run_ = "memory-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(now_ns()) + "-" +
                  std::to_string(++sessions);
    body_ = std::make_shared<ui::CodeDocument>();
    if (const auto* run = logging::current_run()) {
        // Mode is chosen once, not silently switched on each log operation.
        host_ = run->info();
        static_cast<void>(add_run(host_->directory)); // add_run retains any attachment error for display.
    }
}

Output::~Output() = default;

logging::Result<Output::Stream*> Output::add_run(std::filesystem::path directory) {
    directory = directory.lexically_normal();
    const auto existing = std::ranges::find(streams_, directory, &Stream::directory);
    if (existing != streams_.end())
        return existing->get();
    health_dirty_ = pending_change_ = true;
    auto opened = logging::Reader::open(directory);
    if (!opened) {
        attachment_error_ = application::diagnostic_text(opened.error().message);
        return std::unexpected(opened.error());
    }
    auto text = platform::utf16_to_utf8(directory.generic_wstring());
    if (!text) {
        attachment_error_ = L"Cannot attach run: invalid UTF-16 directory.";
        return std::unexpected(logging::Error{logging::ErrorCode::io_failed, "Invalid UTF-16 text."});
    }
    auto stream = std::make_unique<Stream>();
    stream->directory = std::move(directory);
    stream->directory_text = std::move(*text);
    stream->reader = std::move(*opened);
    streams_.add(std::move(stream));
    attachment_error_.clear();
    return streams_.back().get();
}

void Output::source_ended(Stream& stream) {
    if (stream.reader)
        stream.reader->source_ended();
}

void Output::retain(logging::Record record, std::filesystem::path directory,
                    Optional<logging::SubmitStatus> rejection) {
    const auto bytes = record.entry.message.size();
    while (!records_.empty() &&
           (records_.size() >= maximum_records || statistics_.body_bytes + bytes > maximum_body_bytes)) {
        statistics_.body_bytes -= records_.front().record.entry.message.size();
        statistics_.evicted_body_bytes += records_.front().record.entry.message.size();
        ++statistics_.evicted_records;
        unpersisted_ -= records_.front().record.persistence == logging::Persistence::not_persisted;
        records_.pop_front();
    }
    statistics_.body_bytes += bytes;
    unpersisted_ += record.persistence == logging::Persistence::not_persisted;
    records_.emplace_back(next_key_++, std::move(record), std::move(directory), rejection);
    dirty_ = health_dirty_ = pending_change_ = true;
}

bool Output::accept_owned(logging::Record record) {
    health_dirty_ = pending_change_ = true;
    if (host_ || record.persistence != logging::Persistence::not_persisted || !displayable(record)) {
        ++statistics_.invalid_owned;
        return false;
    }
    const bool assigned = record.run_id.empty();
    if (assigned) {
        record.run_id = memory_run_;
        record.process_id = GetCurrentProcessId();
        record.thread_id = GetCurrentThreadId();
        record.producer_id = 1;
        record.producer_sequence = ++sequence_;
        record.record_sequence = record.producer_sequence;
    }
    if (record.run_id.size() > 256 || record.run_id.find('\0') != std::string::npos || !record.process_id ||
        !record.producer_id || !record.producer_sequence) {
        ++statistics_.invalid_owned;
        return false;
    }
    if (!StringView(record.run_id).is_valid_utf8()) {
        ++statistics_.invalid_owned;
        return false;
    }
    if (!assigned) {
        Identity key{record.run_id, record.process_id, record.producer_id, record.producer_sequence};
        if (record.run_id == memory_run_ || owned_identities_.contains(key)) {
            ++statistics_.duplicate_owned;
            return false;
        }
        // A bound on recent identities, not a lifetime producer quota. The
        // logging host's concurrent admission budget does not belong here.
        if (owned_order_.size() == maximum_records) {
            owned_identities_.remove(owned_order_.front());
            owned_order_.pop_front();
        }
        owned_identities_.add(key);
        owned_order_.emplace_back(std::move(key));
    }
    if (!record.timestamp_unix_ns)
        record.timestamp_unix_ns = now_ns();
    retain(std::move(record));
    return true;
}

logging::SubmitResult Output::submit_owned(logging::Entry entry) {
    if (!host_) {
        logging::Record record;
        record.entry = std::move(entry);
        return {accept_owned(std::move(record)) ? logging::SubmitStatus::no_active_run
                                                : logging::SubmitStatus::invalid_record};
    }
    // Keep the owned payload until the receipt is known. Never retry an accepted
    // or uncertain submission, and never directly project an accepted record.
    const auto receipt = logging::submit(entry);
    if (receipt.status == logging::SubmitStatus::accepted || receipt.status == logging::SubmitStatus::filtered)
        return receipt;
    health_dirty_ = pending_change_ = true;
    if (receipt.status == logging::SubmitStatus::internal_failure) {
        ++statistics_.uncertain_submissions;
        return receipt;
    }
    logging::Record record;
    record.run_id = memory_run_;
    record.process_id = GetCurrentProcessId();
    record.thread_id = GetCurrentThreadId();
    record.producer_id = 1;
    record.producer_sequence = ++sequence_;
    record.timestamp_unix_ns = now_ns();
    record.entry = std::move(entry);
    if (!displayable(record)) {
        ++statistics_.invalid_owned;
        record.entry = {
            .severity = logging::Severity::error,
            .category = "editor.output",
            .event_code = "output.unprojectable",
            .message =
                "A user message was rejected and cannot be displayed: invalid Unicode/schema or body exceeds 128 KiB. No truncation or retry was performed."};
    }
    retain(std::move(record), host_->directory, receipt.status);
    return receipt;
}

void Output::capture_raw(Stream& stream, bool standard_error, std::string_view bytes, std::uint64_t offset,
                         bool ended) {
    auto& tail = stream.raw[standard_error ? 1 : 0];
    // A retention gap must not splice an old prefix into an unrelated character.
    if (!tail.bytes.empty() && tail.offset + tail.bytes.size() != offset)
        capture_raw(stream, standard_error, {}, tail.offset + tail.bytes.size(), true);
    if (tail.bytes.empty())
        tail.offset = offset;
    tail.bytes.append(bytes);
    std::size_t consumed{};
    constexpr char hex[] = "0123456789abcdef";
    while (consumed < tail.bytes.size()) {
        const auto begin = consumed;
        std::string message;
        while (consumed < tail.bytes.size() && consumed - begin < 16 * 1024) {
            const auto c = static_cast<unsigned char>(tail.bytes[consumed]);
            const std::size_t count = c < 0x80                 ? 1
                                      : c >= 0xc2 && c <= 0xdf ? 2
                                      : c >= 0xe0 && c <= 0xef ? 3
                                      : c >= 0xf0 && c <= 0xf4 ? 4
                                                               : 1;
            const auto remaining = std::string_view(tail.bytes).substr(consumed);
            if (count > remaining.size() && !ended)
                break;
            if (count <= remaining.size() && StringView(remaining.substr(0, count)).is_valid_utf8()) {
                if (c == '\\')
                    message += '\\';
                message.append(remaining.substr(0, count));
                consumed += count;
            } else {
                message += "\\x";
                message += hex[c >> 4];
                message += hex[c & 15];
                ++consumed;
            }
        }
        if (consumed == begin)
            break;
        logging::Entry entry{.category = "process.raw",
                             .event_code = standard_error ? "raw.stderr" : "raw.stdout",
                             .message = std::move(message)};
        entry.fields = {{"directory", stream.directory_text},
                        {"offset", tail.offset + begin},
                        {"bytes", static_cast<std::uint64_t>(consumed - begin)},
                        {"encoding", std::string("utf8-escaped")}};
        static_cast<void>(submit_owned(std::move(entry)));
    }
    tail.bytes.erase(0, consumed);
    tail.offset += consumed;
}

bool Output::poll() {
    if (!streams_.empty()) {
        // One shared budget, not a per-run multiplier. Completed sources retain
        // their health but no longer consume a reading slot.
        for (std::size_t examined = 0; examined < streams_.size(); ++examined) {
            auto& stream = *streams_[next_stream_++ % streams_.size()];
            if (stream.status.finished)
                continue;
            auto batch =
                stream.reader->poll({.bytes = 256 * 1024, .records = 256, .time = std::chrono::milliseconds(2)});
            for (auto& record : batch.records)
                retain(std::move(record), stream.directory);
            if (stream.status != batch.status || stream.health.has_value() != batch.health.has_value() ||
                (batch.health && (stream.health->updated_unix_ns != batch.health->updated_unix_ns ||
                                  stream.health->final != batch.health->final)))
                health_dirty_ = pending_change_ = true;
            stream.status = std::move(batch.status);
            stream.health = std::move(batch.health);
            if (stream.status.finished)
                stream.reader.reset();
            break;
        }
    }
    return std::exchange(pending_change_, false);
}

void Output::filter(logging::Severity minimum, std::wstring category, std::wstring text) {
    if (minimum_ == minimum && category_ == category && query_ == text)
        return;
    minimum_ = minimum;
    category_ = std::move(category);
    query_ = std::move(text);
    dirty_ = true;
}

const Output::Item* Output::item(ui::TableKey key) const {
    // Cache keys are contiguous: only the oldest end is ever evicted.
    return !records_.empty() && key >= records_.front().key && key - records_.front().key < records_.size()
               ? &records_[static_cast<std::size_t>(key - records_.front().key)]
               : nullptr;
}

void Output::rebuild() const {
    if (!dirty_)
        return;
    auto table = std::make_shared<ui::TableData>();
    table->columns = {{1, L"Level", widths_[0], 64, ui::TextAlign::left, false},
                      {2, L"Category", widths_[1], 72, ui::TextAlign::left, false},
                      {3, L"Message", widths_[2], 120, ui::TextAlign::left, false},
                      {4, L"Source", widths_[3], 80, ui::TextAlign::left, false},
                      {5, L"Run / delivery", widths_[4], 80, ui::TextAlign::left, false}};
    rows_.clear();
    using Group = std::tuple<std::string, std::uint32_t, std::string, std::string, std::string>;
    Map<Group, std::size_t> groups;
    for (const auto& value : records_) {
        const auto& record = value.record;
        const auto& entry = record.entry;
        if (entry.severity < minimum_ ||
            (!category_.empty() && !contains_folded(platform::utf8_to_utf16_validated(entry.category), category_)) ||
            (!query_.empty() && !contains_folded(visible_text(entry.message), query_) &&
             !contains_folded(platform::utf8_to_utf16_validated(entry.event_code), query_)))
            continue;
        if (entry.repeat) {
            Group group{record.run_id, record.process_id, entry.category, entry.event_code, entry.repeat->identity};
            const auto [at, inserted] = groups.try_emplace(std::move(group), rows_.size());
            if (!inserted) {
                auto& row = rows_[*at];
                row.count = std::numeric_limits<std::uint64_t>::max() - row.count < entry.repeat->count
                                ? std::numeric_limits<std::uint64_t>::max()
                                : row.count + entry.repeat->count;
                row.record_key = value.key;
                continue;
            }
        }
        rows_.add({value.key, value.key, entry.repeat ? entry.repeat->count : 1});
    }
    for (const auto& row : rows_) {
        const auto& value = *item(row.record_key);
        const auto& record = value.record;
        const auto& entry = record.entry;
        // A 256-unit row preview never needs the entire 128 KiB body decoded.
        // Four UTF-8 bytes per code point suffice; don't split the last one.
        auto bytes = std::min(entry.message.size(), std::size_t{4 * 256});
        while (bytes < entry.message.size() && (static_cast<unsigned char>(entry.message[bytes]) & 0xc0) == 0x80)
            --bytes;
        auto message = short_text(visible_text(std::string_view(entry.message).substr(0, bytes), true));
        if (row.count > 1)
            message += L" (×" + std::to_wstring(row.count) + L")";
        auto source = visible_text(entry.source.file, true);
        if (entry.source.line)
            source += L":" + std::to_wstring(entry.source.line) +
                      (entry.source.column ? L":" + std::to_wstring(entry.source.column) : L"");
        auto delivery = record.persistence == logging::Persistence::jsonl
                            ? L"JSONL · " + platform::utf8_to_utf16_validated(record.run_id)
                            : L"Not persisted";
        if (value.rejection)
            delivery += L" · " + submission_name(*value.rejection);
        // Cell identity is table-wide, unlike the stable column keys above.
        const auto cell = row.key * 8;
        table->rows.add({row.key,
                         {{cell + 1, platform::utf8_to_utf16_validated(logging::name(entry.severity))},
                          {cell + 2, platform::utf8_to_utf16_validated(entry.category)},
                          {cell + 3, std::move(message)},
                          {cell + 4, short_text(std::move(source), 1024)},
                          {cell + 5, short_text(std::move(delivery))}}});
    }
    if (std::ranges::find(rows_, selected_, &Row::key) == rows_.end())
        selected_ = 0;
    table_ = std::move(table);
    dirty_ = false;
}

const Array<Output::Row>& Output::rows() const {
    rebuild();
    return rows_;
}

const logging::Record* Output::record(ui::TableKey key) const {
    rebuild();
    const auto at = std::ranges::find(rows_, key, &Row::key);
    if (at == rows_.end())
        return nullptr;
    const auto* value = item(at->record_key);
    return value ? &value->record : nullptr;
}

bool Output::select(ui::TableKey key) {
    rebuild();
    if (!record(key))
        return false;
    selected_ = key;
    return true;
}

const logging::Record* Output::selected_record() const {
    return record(selected_);
}

Optional<logging::SourceLocation> Output::selected_source() const {
    const auto* selected = selected_record();
    return selected && !selected->entry.source.file.empty() ? Optional<logging::SourceLocation>{selected->entry.source}
                                                            : Optional<logging::SourceLocation>{};
}

std::filesystem::path Output::selected_directory() const {
    rebuild();
    const auto at = std::ranges::find(rows_, selected_, &Row::key);
    if (at != rows_.end())
        if (const auto* value = item(at->record_key))
            return value->directory;
    return host_ ? host_->directory : std::filesystem::path{};
}

const logging::ReaderStatus* Output::source_status(const std::filesystem::path& directory) const {
    const auto normalized = directory.lexically_normal();
    for (const auto& stream : streams_)
        if (stream->directory == normalized)
            return &stream->status;
    return nullptr;
}

std::shared_ptr<const ui::TableData> Output::table() const {
    rebuild();
    return table_;
}

ui::Result<std::shared_ptr<ui::CodeDocument>> Output::body() const {
    rebuild();
    const auto at = std::ranges::find(rows_, selected_, &Row::key);
    const auto key = at == rows_.end() ? 0 : at->record_key;
    if (key != body_key_) {
        auto document = ui::CodeDocument::create(key ? visible_text(item(key)->record.entry.message) : L"");
        if (!document) {
            if (document.error().code == ui::ErrorCode::invalid_text)
                fatal(document.error().message); // Escaped, bounded log text must satisfy the document contract.
            return std::unexpected(document.error());
        }
        body_ = std::make_shared<ui::CodeDocument>(std::move(*document));
        body_key_ = key;
    }
    return body_;
}

void Output::update_health() const {
    if (!health_dirty_)
        return;
    std::uint64_t gaps{};
    std::uint64_t losses{};
    std::uint64_t invalid{};
    std::uint64_t duplicates{};
    std::uint64_t io{};
    std::uint64_t rejected{};
    std::uint64_t facilities{};
    std::uint64_t notices{};
    std::uint64_t updated{};
    std::size_t waiting{};
    std::size_t behind{};
    std::size_t finished{};
    std::size_t incomplete{};
    std::size_t finals{};
    bool unknown_loss{};
    std::wstring issue;
    for (const auto& stream : streams_) {
        const auto& s = stream->status;
        gaps += s.sequence_gaps;
        losses += s.rotation_losses;
        invalid += s.invalid_lines + s.invalid_metadata + s.oversized_lines + s.incomplete_lines;
        duplicates += s.duplicate_records + s.out_of_order_records;
        io += s.io_failures;
        unknown_loss |= s.unquantified_loss;
        waiting += s.waiting_for_run;
        behind += s.budget_exhausted || (!s.caught_up && !s.waiting_for_run && !s.finished);
        finished += s.finished;
        incomplete += s.incomplete;
        finals += s.has_final_summary;
        if (s.latest_issue)
            issue = short_text(visible_text(application::diagnostic_text(s.latest_issue->message), true), 200);
        if (stream->health) {
            const auto& health = stream->health->report.health;
            rejected += health.rejected_queue_full + health.rejected_producer_limit + health.rejected_record_too_large +
                        health.rejected_invalid_record + health.rejected_internal_failure;
            facilities += health.facility_failures;
            notices += health.backend_notifications;
            updated = std::max(updated, stream->health->updated_unix_ns);
            if (health.first_facility_issue)
                issue = short_text(visible_text(health.first_facility_issue->message, true), 200);
        }
    }
    health_ =
        L"Evicted " + std::to_wstring(statistics_.evicted_records) + L" | Rejected " + std::to_wstring(rejected) +
        L" | Loss " + std::to_wstring(losses) + (unknown_loss ? L" (quantity unknown)" : L"") + L" | Incomplete " +
        std::to_wstring(incomplete) + L"\n" + (host_ ? L"File-backed" : L"Memory consumer · not persisted") +
        std::wstring(L" | Cache ") + std::to_wstring(records_.size()) + L"/4096 · " +
        std::to_wstring(statistics_.body_bytes) + L"/8388608 body bytes | Evicted body bytes " +
        std::to_wstring(statistics_.evicted_body_bytes) + L" | Not persisted " + std::to_wstring(unpersisted_) + L"\n" +
        L"Runs " + std::to_wstring(streams_.size()) + L" · waiting " + std::to_wstring(waiting) + L" · behind " +
        std::to_wstring(behind) + L" · drained " + std::to_wstring(finished) + L" · final " + std::to_wstring(finals) +
        L" · incomplete " + std::to_wstring(incomplete) + L" | Reader gaps " + std::to_wstring(gaps) +
        L", rotation loss " + std::to_wstring(losses) + (unknown_loss ? L" (quantity unknown)" : L"") + L", invalid " +
        std::to_wstring(invalid) + L", replay " + std::to_wstring(duplicates) + L", I/O " + std::to_wstring(io) +
        L" | Producer rejected " + std::to_wstring(rejected) + L" · facilities " + std::to_wstring(facilities) +
        L" · notices " + std::to_wstring(notices) + L" | Owned invalid " + std::to_wstring(statistics_.invalid_owned) +
        L" · duplicate " + std::to_wstring(statistics_.duplicate_owned) + L" · uncertain (not retried) " +
        std::to_wstring(statistics_.uncertain_submissions) + L" | Newest disk health snapshot ns " +
        std::to_wstring(updated);
    if (!issue.empty())
        health_ += L" | " + issue;
    if (!attachment_error_.empty())
        health_ += L" | " + attachment_error_;
    health_ = short_text(std::move(health_), 4090);
    health_dirty_ = false;
}

const std::wstring& Output::health_text() const {
    update_health();
    return health_;
}

bool Output::apply(const ui::Action& action) {
    if (action.key == output_control::table) {
        if (const auto* selected = std::get_if<ui::TableSelection>(&action.value))
            select(selected->row);
        if (const auto* resized = std::get_if<ui::TableColumnResize>(&action.value);
            resized && std::isfinite(resized->width)) {
            rebuild();
            const auto column = std::ranges::find(table_->columns, resized->column, &ui::TableColumn::key);
            if (column != table_->columns.end()) {
                widths_[static_cast<std::size_t>(column - table_->columns.begin())] =
                    std::clamp(resized->width, column->min_width, 32768.f);
                dirty_ = true;
            }
        }
        return true;
    }
    if (action.key == output_control::category || action.key == output_control::search) {
        if (const auto* text = std::get_if<std::wstring>(&action.value))
            filter(minimum_, action.key == output_control::category ? *text : category_,
                   action.key == output_control::search ? *text : query_);
        return true;
    }
    if (action.kind != ui::ActionKind::activate)
        return false;
    if (action.key == output_control::clear_filters) {
        filter(logging::Severity::trace);
        return true;
    }
    if (action.key == output_control::level) {
        filter(minimum_ == logging::Severity::critical
                   ? logging::Severity::trace
                   : static_cast<logging::Severity>(static_cast<unsigned>(minimum_) + 1),
               category_, query_);
        return true;
    }
    return false;
}

ui::Result<void> Output::append(ui_language::ControlOutput controls, ui::ControlKey parent,
                                ui_language::LiveDocument& live, float height) {
    using namespace ui_language;
    rebuild();
    ATELIER_TRY_BIND(document, body());
    const auto* selected = selected_record();
    const auto detail =
        selected ? short_text(L"Read only · " + platform::utf8_to_utf16_validated(selected->entry.event_code) + L" · " +
                                  visible_text(selected->entry.source.file, true),
                              2048)
                 : L"Select a record to inspect its complete body.";
    const auto level = minimum_ == logging::Severity::trace
                           ? L"Level: All"
                           : L"Level: " + platform::utf8_to_utf16_validated(logging::name(minimum_)) + L"+";
    const double list_height = std::max(40.f, selected ? (height - 124) * .4f : height - 96);
    const std::array<InputValue, 15> inputs{
        TableBinding{table_, {selected_, 3}},
        CodeBinding{std::move(*document), true, true},
        TextEditBinding{output_control::category, category_},
        TextEditBinding{output_control::search, query_},
        level,
        health_text(),
        detail,
        rows_.empty(),
        selected != nullptr,
        list_height,
        ActionBinding{output_control::level},
        ActionBinding{output_control::clear_filters},
        ActionBinding{output_control::locate, selected_source().has_value()},
        ActionBinding{output_control::open_directory, !selected_directory().empty()},
        std::wstring(records_.empty() ? L"No records yet. Waiting for output." : L"No records match these filters.")};
    const std::array<KeyBinding, 8> keys{{{"messages", output_control::table},
                                          {"body", output_control::body},
                                          {"health", output_control::health},
                                          {"empty", output_control::empty},
                                          {"level", output_control::level},
                                          {"locate", output_control::locate},
                                          {"logs", output_control::open_directory},
                                          {"clear", output_control::clear_filters}}};
    static_cast<void>(live.append("ProjectOutput", parent, keys, inputs, controls));
    return {};
}
}

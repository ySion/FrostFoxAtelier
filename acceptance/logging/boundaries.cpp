#include "support.hpp"
#include "logging/native.hpp"

namespace logging_acceptance {
void boundaries(const fs::path& root) {
    require(log::current_run() == nullptr && !log::enabled(log::Severity::info, "acceptance.cpu") &&
        log::submit(entry("before.start", "not admitted")).status == log::SubmitStatus::no_active_run,
        "Submitting without a host lazily started logging.");
    log::RunOptions options;
    options.minimum = log::Severity::warning;
    options.categories = {{"acceptance.cpu", log::Severity::info}, {"acceptance.off", log::Severity::off}};
    auto run = start(root / "run", options);

    unsigned categories{}, codes{}, contexts{}, arguments{};
    const auto category = [&] { ++categories; return "acceptance.filtered"; };
    const auto code = [&] { ++codes; return "filtered.native"; };
    const auto context = [&] { ++contexts; return log::native::Context{}; };
    const auto argument = [&] { ++arguments; return 43; };
    const auto filtered = ATELIER_LOG_WITH(log::Severity::info, category(), code(), context(), "{}", argument());
    require(filtered.status == log::SubmitStatus::filtered && filtered.producer_sequence == 0 &&
        categories == 1 && codes == 0 && contexts == 0 && arguments == 0,
        "Runtime filtering evaluated hidden arguments or assigned a producer sequence.");
    categories = 0;
    const auto debug = ATELIER_LOG(log::Severity::debug, category(), code(), "{}", argument());
    require(debug.status == log::SubmitStatus::filtered && codes == 0 && arguments == 0,
        "Filtered debug evaluated its message arguments.");
#ifdef NDEBUG
    require(categories == 0, "Release compile-time filtering evaluated category.");
#else
    require(categories == 1, "Dev runtime filtering did not evaluate category exactly once.");
#endif
    auto disabled = entry("filtered.owned", "not lost");
    disabled.category = "acceptance.off";
    disabled.severity = log::Severity::error;
    require(log::submit(disabled).status == log::SubmitStatus::filtered && !run->health().first_error,
        "Filtered owned error became a rejection or captured a business first error.");

    const auto visible = ATELIER_LOG_WITH(log::Severity::info, "acceptance.cpu", code(), context(), "value={}", argument());
    require(visible.status == log::SubmitStatus::accepted && visible.producer_sequence == 1 &&
        codes == 1 && contexts == 1 && arguments == 1, "Enabled native arguments were not evaluated exactly once.");
    healthy(run->flush());
    auto filtered_read = take(log::Reader::open(root / "run"));
    const auto before_rejections = drain(*filtered_read, false);
    require(before_rejections.records.size() == 1 && before_rejections.records[0].entry.message == "value=43" &&
        before_rejections.status.sequence_gaps == 0 && !before_rejections.status.incomplete &&
        !before_rejections.status.finished, "Information filtering created loss or false termination evidence.");

    auto invalid = entry("invalid.utf8", std::string{"\xc0\x80", 2});
    require(log::submit(invalid).status == log::SubmitStatus::invalid_record, "Invalid UTF-8 was silently sanitized.");
    invalid = entry("invalid.fields", "duplicate");
    invalid.fields.push_back({"phase", std::string{"duplicate"}});
    require(log::submit(invalid).status == log::SubmitStatus::invalid_record, "Duplicate field keys were accepted.");
    invalid = entry("invalid.count", "too many fields");
    for (std::size_t i = invalid.fields.size(); i <= log::maximum_fields; ++i)
        invalid.fields.push_back({"f" + std::to_string(i), std::uint64_t{1}});
    require(log::submit(invalid).status == log::SubmitStatus::invalid_record, "Field-count bound was bypassed.");

    std::string oversized_text;
    oversized_text.reserve(log::maximum_message_bytes + 3);
    while (oversized_text.size() <= log::maximum_message_bytes) oversized_text += "中";
    auto oversized = entry("rejected.first_error", std::move(oversized_text));
    oversized.severity = log::Severity::error;
    const auto size_rejection = log::submit(oversized);
    require(size_rejection.status == log::SubmitStatus::record_too_large && size_rejection.producer_sequence == 2,
        "Oversized message was truncated, accepted, or assigned the wrong sequence.");
    const auto captured = run->health();
    require(captured.first_error && captured.first_error->truncated && !captured.first_error->message_is_template &&
        captured.first_error->entry.event_code == "rejected.first_error" &&
        captured.first_error->producer_sequence == size_rejection.producer_sequence &&
        !captured.first_error->entry.message.empty() &&
        captured.first_error->entry.message.size() <= log::maximum_diagnostic_snapshot_bytes &&
        captured.first_error->entry.message.size() % 3 == 0 &&
        oversized.message.starts_with(captured.first_error->entry.message),
        "Size rejection erased the synchronous, explicitly truncated first-error snapshot.");

    // Individually valid fields can still exceed the whole-record encoding budget.
    auto envelope = entry("rejected.envelope", "small body");
    envelope.fields.clear();
    for (std::size_t i = 0; i != log::maximum_fields; ++i)
        envelope.fields.push_back({"f" + std::to_string(i), std::string(4096, 'v')});
    require(log::submit(std::move(envelope)).status == log::SubmitStatus::record_too_large,
        "Record budget counted only the message and ignored owned fields.");

    auto expected_error = entry("expected.error", "This error is expected; business outcome remains independent.", 83);
    expected_error.severity = log::Severity::error;
    require(log::submit(expected_error).status == log::SubmitStatus::accepted, "Expected error was not accepted.");
    const auto final = run->finish();
    const auto& h = final.health;
    require(final.backend_acknowledged && final.sink_flush_succeeded && final.summary_published &&
        h.state == log::RunState::finished && h.accepted == 2 && h.rejected_invalid_record == 3 &&
        h.rejected_record_too_large == 2 && h.rejected_queue_full == 0 && h.rejected_producer_limit == 0 &&
        h.rejected_internal_failure == 0 && h.facility_failures == 0 && h.backend_notifications == 0,
        "Declared rejection counts did not exactly match submit results.");
    require(h.first_error && h.first_error->entry.event_code == "rejected.first_error",
        "A later accepted error replaced the first error.");
    require(log::current_run() == nullptr && !log::enabled(log::Severity::error, "acceptance.cpu") &&
        log::submit(entry("after.finish", "must not queue")).status == log::SubmitStatus::no_active_run,
        "Submission after finish used a stale service.");
    const auto repeated = run->finish();
    require(repeated.health.accepted == h.accepted && repeated.health.rejected_record_too_large == 2 &&
        repeated.health.state == log::RunState::finished, "Idempotent finish changed final accounting.");
    log::RunOptions restart;
    restart.directory = root / "forbidden-restart";
    const auto restarted = log::start_run(restart);
    require(!restarted && restarted.error().code == log::ErrorCode::already_started && !fs::exists(restart.directory),
        "One-successful-start-per-process contract was bypassed.");

    const auto read = read_finished(root / "run");
    require(read.status.finished && read.status.has_final_summary && read.records.size() == 2 && read.health &&
        read.health->report.health.rejected_invalid_record == 3 &&
        read.health->report.health.rejected_record_too_large == 2 &&
        read.status.invalid_lines == 0 && read.status.invalid_metadata == 0 && read.status.io_failures == 0,
        "Rejected input became a record or disappeared from published health.");
    require(read.records[0].entry.severity == log::Severity::info &&
        read.records[0].entry.category == "acceptance.cpu" && read.records[0].entry.event_code == "filtered.native" &&
        read.records[0].entry.message == "value=43", "Native information control became an unexpected diagnostic.");
    same_entry(read.records[1].entry, expected_error);
    // A consumed producer sequence can show a gap after rejection. It is not a
    // count of lost accepted records; exact admission/record checks above decide that.
}

int unexpected_warning(const fs::path& root) {
    auto run = start(root / "run");
    require(log::submit(entry("innocent.info", "The word error alone is not a failure.")).status == log::SubmitStatus::accepted,
        "Informational control was not admitted.");
    auto warning = entry("unexpected.warning", "No failure keyword is needed.");
    warning.severity = log::Severity::warning;
    require(log::submit(warning).status == log::SubmitStatus::accepted, "Unexpected-warning witness was not admitted.");
    healthy(run->finish());
    const auto read = read_finished(root / "run");
    clean_reader(read);
    require(read.records.size() == 2 && read.records[0].entry.severity == log::Severity::info,
        "Warning counterexample did not execute its informational control.");
    unsigned unexpected{};
    for (const auto& record : read.records) {
        if (record.entry.severity < log::Severity::warning) continue;
        require(record.entry.category == "acceptance.cpu" && record.entry.event_code == "unexpected.warning" &&
            field<std::string>(record.entry, "phase") == "validation" &&
            field<std::uint64_t>(record.entry, "context") == 71,
            "Warning counterexample received a different diagnostic.");
        ++unexpected;
    }
    require(unexpected == 1, "Warning counterexample did not observe exactly one unexpected diagnostic.");
    std::puts("Logging acceptance: REJECT case=unexpected-warning reason=unexpected-warning category=acceptance.cpu event=unexpected.warning phase=validation context=71 count=1");
    return 23;
}
}

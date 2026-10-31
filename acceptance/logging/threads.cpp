#include "support.hpp"
#include <array>
#include <barrier>
#include <exception>
#include <set>
#include <thread>

namespace logging_acceptance {
void threads(const fs::path& root) {
    auto run = start(root / "run");
    constexpr std::array<std::size_t, 4> counts{257, 263, 271, 277};
    std::array<std::vector<log::SubmitResult>, 4> submissions;
    std::array<std::exception_ptr, 4> failures;
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker != counts.size(); ++worker) {
        submissions[worker].resize(counts[worker]);
        workers.emplace_back([&, worker] {
            try {
                for (std::size_t i = 0; i != counts[worker]; ++i) {
                    auto value =
                        entry("thread.record", "thread=" + std::to_string(worker) + ";ordinal=" + std::to_string(i),
                              1009 + worker * 37);
                    value.fields.push_back({"ordinal", static_cast<std::uint64_t>(i)});
                    value.fields.push_back({"worker", static_cast<std::uint64_t>(worker)});
                    submissions[worker][i] = log::submit(std::move(value));
                }
            } catch (...) {
                failures[worker] = std::current_exception();
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    // Do not pre-flush: finish must drain all joined producers itself.
    const auto final = run->finish();
    for (const auto& failure : failures)
        if (failure)
            std::rethrow_exception(failure);
    healthy(final);
    require(final.health.accepted == 1068, "Normal stop did not account for all four producers.");
    for (const auto& producer : submissions)
        for (const auto& submitted : producer)
            require(submitted.status == log::SubmitStatus::accepted,
                    "Finite normal workload unexpectedly rejected a record.");
    const auto read = read_finished(root / "run");
    clean_reader(read);
    require(read.records.size() == 1068, "Normal stop lost or duplicated a record.");
    std::array<std::size_t, 4> next{};
    std::set<std::uint64_t> producer_ids;
    for (const auto& producer : submissions)
        producer_ids.insert(producer.front().producer_id);
    require(producer_ids.size() == 4 && !producer_ids.contains(0), "Distinct live producers shared an identity.");
    for (const auto& record : read.records) {
        const auto worker = field<std::uint64_t>(record.entry, "worker");
        require(worker < counts.size(), "Thread context was corrupted.");
        const auto ordinal = field<std::uint64_t>(record.entry, "ordinal");
        require(ordinal == next[worker] && ordinal < counts[worker] &&
                    record.producer_id == submissions[worker][ordinal].producer_id &&
                    record.producer_sequence == submissions[worker][ordinal].producer_sequence &&
                    field<std::uint64_t>(record.entry, "context") == 1009 + worker * 37 &&
                    field<std::string>(record.entry, "phase") == "validation" &&
                    record.entry.category == "acceptance.cpu" && record.entry.event_code == "thread.record" &&
                    record.entry.severity == log::Severity::info &&
                    record.entry.message == "thread=" + std::to_string(worker) + ";ordinal=" + std::to_string(ordinal),
                "Per-producer ordering, payload or context changed during stop.");
        ++next[worker];
    }
    require(next == counts, "A producer's tail was missing at normal stop.");
}

void producer_churn(const fs::path& root) {
    auto run = start(root / "run");
    constexpr auto count = log::maximum_producers + 3;
    std::set<std::uint64_t> identities;
    for (std::size_t i = 0; i != count; ++i) {
        log::SubmitResult submitted;
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                submitted = log::submit(entry("thread.retired", "short-lived", i + 7001));
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure)
            std::rethrow_exception(failure);
        require(submitted.status == log::SubmitStatus::accepted && submitted.producer_id != 0 &&
                    identities.insert(submitted.producer_id).second,
                "Drained producer slots were permanently exhausted or IDs were reused.");
        // The public quiescent checkpoint proves this producer's admitted work
        // is drained; it does not inspect Quill TLS, queues or retirement lists.
        healthy(run->flush());
    }
    healthy(run->finish());
    const auto read = read_finished(root / "run");
    clean_reader(read);
    require(read.records.size() == count, "Short-lived producer retirement lost evidence.");
    for (std::size_t i = 0; i != count; ++i)
        require(read.records[i].entry.severity == log::Severity::info &&
                    read.records[i].entry.category == "acceptance.cpu" &&
                    read.records[i].entry.event_code == "thread.retired" &&
                    field<std::string>(read.records[i].entry, "phase") == "validation" &&
                    field<std::uint64_t>(read.records[i].entry, "context") == i + 7001 &&
                    read.records[i].entry.message == "short-lived" &&
                    identities.erase(read.records[i].producer_id) == 1,
                "Reader confused a retired producer with a later thread.");
    require(identities.empty(), "Reader did not observe every retired producer.");
}

int pressure(const fs::path& root) {
    auto run = start(root / "run");
    constexpr std::size_t producer_count = 4, attempts = 1024;
    std::array<std::vector<log::SubmitResult>, producer_count> results;
    std::array<std::exception_ptr, producer_count> failures;
    std::barrier ready{static_cast<std::ptrdiff_t>(producer_count)};
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker != producer_count; ++worker) {
        results[worker].resize(attempts);
        workers.emplace_back([&, worker] {
            ready.arrive_and_wait();
            try {
                for (std::size_t i = 0; i != attempts; ++i) {
                    auto value =
                        entry("pressure.record", std::string(16 * 1024, static_cast<char>('a' + worker)), worker + 301);
                    value.fields.push_back({"worker", static_cast<std::uint64_t>(worker)});
                    value.fields.push_back({"ordinal", static_cast<std::uint64_t>(i)});
                    if (i == 7) {
                        value.severity = log::Severity::error;
                        value.message = "pressure-first-error";
                    }
                    results[worker][i] = log::submit(std::move(value));
                }
            } catch (...) {
                failures[worker] = std::current_exception();
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    const auto final = run->finish();
    for (const auto& failure : failures)
        if (failure)
            std::rethrow_exception(failure);
    std::uint64_t accepted{}, rejected{};
    std::set<std::pair<std::uint64_t, std::uint64_t>> pending;
    for (const auto& producer : results)
        for (const auto& result : producer) {
            require(result.producer_id != 0 && result.producer_sequence != 0,
                    "Pressure lost an admitted attempt's identity.");
            if (result.status == log::SubmitStatus::accepted) {
                ++accepted;
                require(pending.emplace(result.producer_id, result.producer_sequence).second,
                        "Submit reused a record identity.");
            } else {
                require(result.status == log::SubmitStatus::queue_full,
                        "Pressure failed for a reason other than queue capacity.");
                ++rejected;
            }
        }
    const auto& h = final.health;
    require(accepted + rejected == producer_count * attempts && h.accepted == accepted &&
                h.rejected_queue_full == rejected && h.rejected_producer_limit == 0 &&
                h.rejected_record_too_large == 0 && h.rejected_invalid_record == 0 &&
                h.rejected_internal_failure == 0 && h.facility_failures == 0 && !h.first_facility_issue &&
                final.backend_acknowledged && final.sink_flush_succeeded && final.summary_published,
            "Pressure admission, rejection or real file delivery health disagreed.");
    require(h.first_error && h.first_error->entry.event_code == "pressure.record" &&
                h.first_error->entry.severity == log::Severity::error &&
                field<std::uint64_t>(h.first_error->entry, "ordinal") == 7,
            "Overload erased the synchronous first-error snapshot.");
    const auto read = read_finished(root / "run");
    require(read.status.finished && read.status.has_final_summary && read.health && read.records.size() == accepted &&
                read.status.invalid_lines == 0 && read.status.invalid_metadata == 0 &&
                read.status.rotation_losses == 0 && read.status.duplicate_records == 0 &&
                read.status.out_of_order_records == 0 && read.status.io_failures == 0,
            "Pressure lost accepted records or generated unreadable/duplicate evidence.");
    for (const auto& record : read.records) {
        const auto worker = field<std::uint64_t>(record.entry, "worker");
        const auto ordinal = field<std::uint64_t>(record.entry, "ordinal");
        require(worker < producer_count && ordinal < attempts &&
                    results[worker][ordinal].status == log::SubmitStatus::accepted &&
                    results[worker][ordinal].producer_id == record.producer_id &&
                    results[worker][ordinal].producer_sequence == record.producer_sequence &&
                    pending.erase({record.producer_id, record.producer_sequence}) == 1 &&
                    record.entry.category == "acceptance.cpu" && record.entry.event_code == "pressure.record" &&
                    field<std::string>(record.entry, "phase") == "validation" &&
                    field<std::uint64_t>(record.entry, "context") == worker + 301 &&
                    record.entry.severity == (ordinal == 7 ? log::Severity::error : log::Severity::info) &&
                    record.entry.message == (ordinal == 7 ? std::string{"pressure-first-error"}
                                                          : std::string(16 * 1024, static_cast<char>('a' + worker))),
                "Pressure returned a rejected, altered or repeated record.");
    }
    require(pending.empty(), "Accepted pressure records vanished without a submit rejection.");
    // Notices remain visible and unclassified; they are not the rejection ledger.
    // This is an exercised overload contract, not a claim of clean health or a benchmark.
    std::printf("Pressure evidence: accepted=%llu queue_full=%llu backend_notifications=%llu reader_gaps=%llu\n",
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(rejected),
                static_cast<unsigned long long>(h.backend_notifications),
                static_cast<unsigned long long>(read.status.sequence_gaps));
    if (!h.first_backend_notification.empty())
        std::printf("First backend notification: %s\n", h.first_backend_notification.c_str());
    if (!rejected) {
        std::puts("Logging acceptance: NOT-COVERED case=pressure reason=queue-full-not-observed");
        return 77;
    }
    std::puts("Logging acceptance: PASS case=pressure reason=queue-full-observed-and-accounted");
    return 0;
}
}

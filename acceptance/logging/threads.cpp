#include "support.hpp"
#include <array>
#include <exception>
#include <thread>

namespace logging_acceptance {
void producer_churn(const fs::path& root) {
    auto run = start(root / "run");
    const std::array expected{entry("thread.retired", "first producer", 7001),
                              entry("thread.retired", "second producer", 7002)};
    for (const auto& value : expected) {
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                require(log::submit(value).status == log::SubmitStatus::accepted,
                        "Short-lived producer record was not admitted.");
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure)
            std::rethrow_exception(failure);
    }
    // No pre-flush: finish must drain both producers after their threads exit.
    healthy(run->finish());
    const auto repeated = run->finish();
    healthy(repeated);
    require(repeated.health.state == log::RunState::finished && repeated.health.accepted == 2,
            "Repeated finish changed the two-record terminal state.");
    const auto read = read_finished(root / "run");
    clean_reader(read);
    require(read.records.size() == expected.size(), "Producer retirement or finish lost or duplicated a record.");
    std::array<bool, 2> seen{};
    // Cross-producer ordering is not a contract. Match only our fixed input contexts.
    for (const auto& record : read.records) {
        const auto context = field<std::uint64_t>(record.entry, "context");
        require(context == 7001 || context == 7002, "Retired producer context was corrupted.");
        const auto index = static_cast<std::size_t>(context - 7001);
        require(!seen[index], "Retired producer record was delivered twice.");
        same_entry(record.entry, expected[index]);
        seen[index] = true;
    }
}
}

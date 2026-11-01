#include "support.hpp"
#include <Windows.h>

namespace logging_acceptance {
namespace {
struct FileLock {
    HANDLE handle{INVALID_HANDLE_VALUE};

    OVERLAPPED offset{};

    bool locked{};

    ~FileLock() {
        if (locked)
            UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &offset);
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
}

void file_failures(const fs::path& root) {
    write_bytes(root / "parent-is-file", "unmodified sentinel");
    log::RunOptions blocked;
    blocked.directory = root / "parent-is-file" / "run";
    const auto failed = log::start_run(blocked);
    require(!failed && failed.error().code == log::ErrorCode::io_failed && !failed.error().message.empty() &&
                log::current_run() == nullptr,
            "A real path-open failure was not returned independently of the logger.");
    require(read_bytes(root / "parent-is-file") == "unmodified sentinel",
            "Failed initialization overwrote an existing file.");
    fs::create_directory(root / "occupied");
    write_bytes(root / "occupied" / "sentinel", "old run");
    blocked.directory = root / "occupied";
    const auto occupied = log::start_run(blocked);
    require(!occupied && occupied.error().code == log::ErrorCode::directory_in_use &&
                read_bytes(root / "occupied" / "sentinel") == "old run",
            "Existing run directory was reused or truncated.");

    auto run = start(root / "run"); // Failed starts must have rolled back completely.
    const auto baseline = entry("file.before_lock", "baseline");
    require(log::submit(baseline).status == log::SubmitStatus::accepted, "File-failure baseline was not admitted.");
    healthy(run->flush());
    const auto file = root / "run" / "events.jsonl";
    const auto before = fs::file_size(file);
    auto business_error = entry("file.original_error", "original business failure, not a logging failure", 101);
    business_error.severity = log::Severity::error;
    log::FlushReport failed_flush;
    {
        FileLock lock;
        lock.handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        require(lock.handle != INVALID_HANDLE_VALUE,
                "Cannot open a second real handle for the lock-write counterexample.");
        require(LockFileEx(lock.handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD,
                           &lock.offset) != FALSE,
                "Cannot lock the real log file.");
        lock.locked = true;
        require(log::submit(business_error).status == log::SubmitStatus::accepted,
                "Locked-file record did not reach the real backend.");
        failed_flush = run->flush();
        require(failed_flush.backend_acknowledged && failed_flush.health.facility_failures > 0 &&
                    failed_flush.health.first_facility_issue &&
                    failed_flush.health.first_facility_issue->fault == log::FacilityFault::file_write_failed &&
                    failed_flush.health.first_facility_issue->native_code == ERROR_LOCK_VIOLATION,
                "Backend flush acknowledgement hid a real Windows locked-write failure.");
        // A sink can detect the failed WriteFile while consuming the record,
        // before its later flush operation. Even a successful flush cannot erase
        // that failure; do not equate a per-operation bool with delivery health.
        require(fs::file_size(file) == before, "Locked-file write unexpectedly changed the file.");
        require(failed_flush.health.first_error &&
                    failed_flush.health.first_error->entry.message == business_error.message &&
                    failed_flush.health.first_error->entry.event_code == business_error.event_code,
                "Logging infrastructure failure replaced the captured business first error.");
        require(UnlockFileEx(lock.handle, 0, MAXDWORD, MAXDWORD, &lock.offset) != FALSE,
                "Cannot release the file lock.");
        lock.locked = false;
    }
    const auto final = run->finish();
    require(final.backend_acknowledged && final.summary_published && final.health.state == log::RunState::finished &&
                final.health.facility_failures >= failed_flush.health.facility_failures &&
                final.health.first_facility_issue &&
                final.health.first_facility_issue->fault == failed_flush.health.first_facility_issue->fault &&
                final.health.first_facility_issue->native_code ==
                    failed_flush.health.first_facility_issue->native_code &&
                final.health.first_error && final.health.first_error->entry.message == business_error.message,
            "Finish/recovery erased the original file failure or business first error.");
    const auto read = read_finished(root / "run");
    require(read.status.finished && read.status.has_final_summary && read.health && read.health->final &&
                read.health->report.health.facility_failures >= failed_flush.health.facility_failures &&
                read.health->report.health.accepted == 2 && read.health->report.health.first_error &&
                read.status.invalid_lines == 0 && read.status.duplicate_records == 0,
            "Disk health hid the facility failure or recovery produced corrupt/duplicate records.");
    require(!read.records.empty() && read.records.size() <= 2,
            "File failure destroyed previously committed evidence or produced an unexpected record.");
    same_entry(read.records.front().entry, baseline);
    if (read.records.size() == 2)
        same_entry(read.records.back().entry, business_error);
    same_entry(read.health->report.health.first_error->entry, business_error);
    // Retry is not promised. An absent locked-write record is never relabelled as
    // complete delivery: this case accepts only the explicitly observed facility fault.
    std::printf(
        "Locked-write witness: windows=%u backend_acknowledged=1 sink_flush_succeeded=%u facility_failures=%llu\n",
        static_cast<unsigned>(ERROR_LOCK_VIOLATION), static_cast<unsigned>(failed_flush.sink_flush_succeeded),
        static_cast<unsigned long long>(failed_flush.health.facility_failures));
}
}

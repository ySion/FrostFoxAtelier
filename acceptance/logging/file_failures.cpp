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
    auto run = start(root / "run");
    const auto baseline = entry("file.before_lock", "baseline");
    require(log::submit(baseline).status == log::SubmitStatus::accepted, "File-failure baseline was not admitted.");
    healthy(run->flush());
    const auto file = root / "run" / "events.jsonl";
    auto business_error = entry("file.original_error", "original business failure, not a logging failure", 101);
    business_error.severity = log::Severity::error;
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
        const auto failed_flush = run->flush();
        require(failed_flush.backend_acknowledged && !failed_flush.sink_flush_succeeded &&
                    failed_flush.health.facility_failures > 0 && failed_flush.health.first_facility_issue &&
                    failed_flush.health.first_facility_issue->fault == log::FacilityFault::file_write_failed &&
                    failed_flush.health.first_facility_issue->native_code == ERROR_LOCK_VIOLATION,
                "Backend flush acknowledgement hid a real Windows locked-write failure.");
        require(UnlockFileEx(lock.handle, 0, MAXDWORD, MAXDWORD, &lock.offset) != FALSE,
                "Cannot release the file lock.");
        lock.locked = false;
    }
    const auto final = run->finish();
    require(final.backend_acknowledged && !final.sink_flush_succeeded && final.summary_published &&
                final.health.state == log::RunState::finished && log::current_run() == nullptr,
            "Locked-write failure prevented normal shutdown.");
    const auto read = read_finished(root / "run");
    require(read.status.finished && read.status.has_final_summary && read.health && read.health->final &&
                !read.health->report.sink_flush_succeeded && read.health->report.health.facility_failures > 0 &&
                read.health->report.health.first_facility_issue &&
                read.health->report.health.first_facility_issue->fault == log::FacilityFault::file_write_failed &&
                read.health->report.health.first_facility_issue->native_code == ERROR_LOCK_VIOLATION &&
                read.status.invalid_lines == 0 && read.status.duplicate_records == 0,
            "Shutdown hid the injected file failure or left unreadable/duplicate records.");
    require(!read.records.empty() && read.records.size() <= 2,
            "File failure destroyed previously committed evidence or produced an unexpected record.");
    same_entry(read.records.front().entry, baseline);
    if (read.records.size() == 2)
        same_entry(read.records.back().entry, business_error);
    // Retry is not promised. An absent locked-write record is never relabelled as
    // complete delivery: this case accepts only the explicitly observed facility fault.
}
}

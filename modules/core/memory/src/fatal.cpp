#include "memory/fatal.hpp"

#include <windows.h>
#include <intrin.h>
#include <crtdbg.h>
#include <atomic>
#include <csignal>
#include <cstdlib>

namespace atelier {
namespace {

std::atomic_flag reporting = ATOMIC_FLAG_INIT;

void abort_handler(int) noexcept {
    fatal("abort");
}

[[maybe_unused]] int __cdecl report_handler(int type, char* message, int*) noexcept {
    if (type == _CRT_ASSERT || type == _CRT_ERROR)
        fatal(message);
    return 0;
}

}

[[noreturn]] void fatal(std::string_view reason) noexcept {
    // A concurrent/recursive failure must never wait for another reporter.
    if (!reporting.test_and_set(std::memory_order_relaxed)) {
        const auto output = GetStdHandle(STD_ERROR_HANDLE);
        // Only write where we can enable nonblocking I/O. Console QuickEdit,
        // files and devices can block synchronous writes indefinitely too.
        DWORD pipe_mode = PIPE_NOWAIT;
        if (GetFileType(output) != FILE_TYPE_PIPE || !SetNamedPipeHandleState(output, &pipe_mode, nullptr, nullptr))
            __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        constexpr char prefix[] = "Atelier fatal: ";
        const auto count = static_cast<DWORD>(reason.size() < 1024 ? reason.size() : 1024);
        DWORD written{};
        WriteFile(output, prefix, sizeof(prefix) - 1, &written, nullptr);
        if (count)
            WriteFile(output, reason.data(), count, &written, nullptr);
        WriteFile(output, "\n", 1, &written, nullptr);
    }
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

void install_fatal_handler() noexcept {
    // Debug CRT emits its report before raising SIGABRT unless this is disabled.
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    if (_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, report_handler) == -1)
        fatal("could not install CRT report hook");
    if (std::signal(SIGABRT, abort_handler) == SIG_ERR)
        fatal("could not install SIGABRT handler");
}

}

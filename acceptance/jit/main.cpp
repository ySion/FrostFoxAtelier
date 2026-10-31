#include "backend.hpp"
#include "business.hpp"

#include <windows.h>

#include <cstdio>
#include <exception>

namespace {

std::uint32_t host_calls{};

std::uint64_t host_bias(std::uint64_t value) noexcept {
    ++host_calls;
    return value * 2 + 1;
}

void log_result(const char* text) noexcept {
    std::puts(text);
    std::fflush(stdout);
}

}

int main() {
    HMODULE native_module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&atelier_jit_native_calculate), &native_module) ||
        native_module == GetModuleHandleW(nullptr)) {
        std::fprintf(stderr, "JIT acceptance: FAIL: native callback must reside in a separate DLL.\n");
        return 1;
    }
    const atelier::jit_acceptance::ProbeCallbacks callbacks{&host_bias, &atelier_jit_native_calculate,
                                                            &atelier_jit_native_capture, &log_result};
    char error[4096]{};
    if (atelier_run_jit_acceptance(&callbacks, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "JIT acceptance: FAIL: %s\n", error);
        return 1;
    }
    if (host_calls == 0) {
        std::fprintf(stderr, "JIT acceptance: FAIL: generated code did not call the host.\n");
        return 1;
    }
    try {
        atelier::jit_acceptance::verify_business_execution();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Business execution acceptance: FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("JIT acceptance: PASS\n");
    return 0;
}

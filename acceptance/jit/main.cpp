#include "memory/fatal.hpp"
#include "backend.hpp"
#include "business.hpp"

#include <windows.h>

#include <cstdio>

namespace {

std::uint64_t host_bias(std::uint64_t value) noexcept {
    return value * 2 + 1;
}

void log_result(const char* text) noexcept {
    std::puts(text);
    std::fflush(stdout);
}

}

int main() {
    atelier::install_fatal_handler();
    HMODULE native_module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&atelier_jit_native_calculate), &native_module) ||
        native_module == GetModuleHandleW(nullptr))
        atelier::fatal("JIT acceptance: native callback must reside in a separate DLL.");
    const atelier::jit_acceptance::ProbeCallbacks callbacks{&host_bias, &atelier_jit_native_calculate,
                                                            &atelier_jit_native_capture, &log_result};
    atelier_run_jit_acceptance(&callbacks);
    atelier::jit_acceptance::verify_business_execution();
    std::printf("JIT acceptance: PASS\n");
    return 0;
}

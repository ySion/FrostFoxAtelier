#pragma once

#include <string_view>

namespace atelier {

// Best-effort bounded diagnostic, then fail-fast. No allocation, callbacks,
// unwinding or normal shutdown; the borrowed reason must remain readable.
[[noreturn]] void fatal(std::string_view reason) noexcept;

// Call before business work in each executable and each private static-CRT
// domain. SIGABRT is shared by threads in that CRT domain, not across CRTs.
void install_fatal_handler() noexcept;

}

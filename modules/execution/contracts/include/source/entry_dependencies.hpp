#pragma once
#include <cstdint>

namespace atelier::source {
// In declaration order. Byte ranges include the signature and closing brace,
// and refer to the exact input whose bodies have all passed semantic analysis.
struct EntryFunction {
    std::uint32_t begin;
    std::uint32_t end;
    bool reachable;
};
}

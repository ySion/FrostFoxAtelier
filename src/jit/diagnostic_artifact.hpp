#pragma once

#include <string>

namespace llvm {
class MemoryBufferRef;
}

namespace atelier::source::detail {

// Reads the borrowed, pre-link x64 COFF object after JIT target initialization.
// Returns owned text; malformed objects or unavailable decoding throw runtime_error.
std::string disassemble_object(llvm::MemoryBufferRef object);

} // namespace atelier::source::detail

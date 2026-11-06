#pragma once

#include <llvm/Support/Error.h>
#include <string>

namespace llvm {
class MemoryBufferRef;
}

namespace atelier::source::detail {

// Reads the borrowed, pre-link x64 COFF object after JIT target initialization.
// Returns owned text or the decoding error without publishing partial text.
llvm::Expected<std::string> disassemble_object(llvm::MemoryBufferRef object) noexcept;

} // namespace atelier::source::detail

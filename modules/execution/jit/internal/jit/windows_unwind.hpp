#pragma once

#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>

namespace atelier::jit {
std::unique_ptr<llvm::orc::LinkGraphLinkingLayer::Plugin> make_windows_unwind_plugin();
}

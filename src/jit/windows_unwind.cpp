#include "jit/windows_unwind.hpp"

#include <windows.h>
#include <llvm/ExecutionEngine/JITLink/COFF.h>
#include <llvm/ExecutionEngine/Orc/Shared/AllocationActions.h>
#include <llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h>
#include <llvm/Support/ErrorHandling.h>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace atelier::jit {
namespace {
using llvm::orc::ExecutorAddr;

// In-process COFF only. LLVM owns code, .pdata and .xdata; allocation actions
// register after relocation and revoke the table before freeing those pages.
llvm::orc::shared::CWrapperFunctionBuffer register_unwind_table(const char* data, std::size_t size) {
    using namespace llvm::orc;
    using namespace llvm::orc::shared;
    return WrapperFunction<SPSError(SPSExecutorAddrRange, SPSExecutorAddr)>::handle(
        data, size, [](ExecutorAddrRange range, ExecutorAddr base) -> llvm::Error {
            auto* table = range.Start.toPtr<RUNTIME_FUNCTION*>();
            if (!RtlAddFunctionTable(table, static_cast<DWORD>(range.size() / sizeof(*table)), base.getValue()))
                return llvm::make_error<llvm::StringError>("RtlAddFunctionTable failed.",
                                                           llvm::inconvertibleErrorCode());
            return llvm::Error::success();
        }).release();
}

llvm::orc::shared::CWrapperFunctionBuffer unregister_unwind_table(const char* data, std::size_t size) {
    using namespace llvm::orc::shared;
    return WrapperFunction<SPSError(SPSExecutorAddr)>::handle(
        data, size, [](ExecutorAddr table) -> llvm::Error {
            if (!RtlDeleteFunctionTable(table.toPtr<RUNTIME_FUNCTION*>()))
                llvm::report_fatal_error("Cannot revoke Windows unwind table before freeing JIT code.");
            return llvm::Error::success();
        }).release();
}

class WindowsUnwindPlugin final : public llvm::orc::LinkGraphLinkingLayer::Plugin {
public:
    void modifyPassConfig(llvm::orc::MaterializationResponsibility&, llvm::jitlink::LinkGraph&,
                          llvm::jitlink::PassConfiguration& config) override {
        config.PostAllocationPasses.push_back([](llvm::jitlink::LinkGraph& graph) -> llvm::Error {
            auto* image_base = llvm::jitlink::GetImageBaseSymbol{}(graph);
            if (!image_base) return llvm::Error::success();
            auto start = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t end{};
            for (const auto* block : graph.blocks()) {
                start = std::min(start, block->getAddress().getValue());
                end = std::max(end, block->getAddress().getValue() + block->getSize());
            }
            if (start == std::numeric_limits<std::uint64_t>::max() || end - start > MAXDWORD)
                return llvm::make_error<llvm::StringError>("COFF image exceeds the 32-bit unwind RVA range.",
                                                           llvm::inconvertibleErrorCode());
            graph.makeAbsolute(*image_base, ExecutorAddr(start));
            return llvm::Error::success();
        });
        config.PostFixupPasses.push_back([](llvm::jitlink::LinkGraph& graph) -> llvm::Error {
            using namespace llvm::orc::shared;
            auto* section = graph.findSectionByName(".pdata");
            if (!section || section->blocks().empty()) return llvm::Error::success();
            const auto range = llvm::jitlink::SectionRange(*section).getRange();
            std::uint64_t bytes{};
            for (const auto* block : section->blocks()) {
                if (block->isZeroFill() || block->getSize() % sizeof(RUNTIME_FUNCTION) != 0 ||
                    block->getContent().data() != block->getAddress().toPtr<const char*>())
                    return llvm::make_error<llvm::StringError>("Unexpected in-process COFF unwind storage.",
                                                               llvm::inconvertibleErrorCode());
                bytes += block->getSize();
            }
            auto* image_base = llvm::jitlink::GetImageBaseSymbol{}(graph);
            if (!image_base || bytes == 0 || bytes != range.size() ||
                bytes / sizeof(RUNTIME_FUNCTION) > MAXDWORD ||
                range.Start.getValue() % alignof(RUNTIME_FUNCTION) != 0)
                return llvm::make_error<llvm::StringError>("Invalid or noncontiguous COFF unwind table.",
                                                           llvm::inconvertibleErrorCode());
            auto* table = range.Start.toPtr<RUNTIME_FUNCTION*>();
            const auto count = bytes / sizeof(*table);
            // Working and executor storage coincide for the selected in-process manager.
            // Final page protection has not been applied yet.
            std::sort(table, table + count, [](const auto& a, const auto& b) {
                return a.BeginAddress < b.BeginAddress;
            });
            for (std::uint64_t i = 0; i < count; ++i) {
                if (table[i].BeginAddress >= table[i].EndAddress ||
                    (i != 0 && table[i - 1].EndAddress > table[i].BeginAddress))
                    return llvm::make_error<llvm::StringError>("Invalid COFF unwind function ranges.",
                                                               llvm::inconvertibleErrorCode());
            }
            const auto base = image_base->getAddress();
            auto install = WrapperFunctionCall::Create<SPSArgList<SPSExecutorAddrRange, SPSExecutorAddr>>(
                ExecutorAddr::fromPtr(&register_unwind_table), range, base);
            if (!install) return install.takeError();
            auto revoke = WrapperFunctionCall::Create<SPSArgList<SPSExecutorAddr>>(
                ExecutorAddr::fromPtr(&unregister_unwind_table), range.Start);
            if (!revoke) return revoke.takeError();
            // Both payloads fit the pinned SDK's 24-byte inline buffer.
            if (install->getArgData().capacity() > 24 || revoke->getArgData().capacity() > 24)
                return llvm::make_error<llvm::StringError>("Unwind actions spilled their inline buffers.",
                                                           llvm::inconvertibleErrorCode());
            graph.allocActions().push_back({std::move(*install), std::move(*revoke)});
            return llvm::Error::success();
        });
    }

    // Ownership, failure cleanup and tracker transfers follow the allocation,
    // not a second plugin-side registry.
    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility&) override {
        return llvm::Error::success();
    }
    llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override {
        return llvm::Error::success();
    }
    void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey,
                                    llvm::orc::ResourceKey) override {}
};

}

std::unique_ptr<llvm::orc::LinkGraphLinkingLayer::Plugin> make_windows_unwind_plugin() {
    return std::make_unique<WindowsUnwindPlugin>();
}

}

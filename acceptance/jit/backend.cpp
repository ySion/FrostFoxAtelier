#include "backend.hpp"
#include "jit/windows_unwind.hpp"
#include "memory/fatal.hpp"

#include <windows.h>

#include <llvm/ExecutionEngine/JITLink/COFF.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/Constants.h>
#include <llvm/ExecutionEngine/Orc/Shared/AllocationActions.h>
#include <llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace atelier::jit_acceptance;

using llvm::orc::ExecutorAddr;

using llvm::orc::LLJIT;

using llvm::orc::ResourceTrackerSP;

using llvm::orc::ThreadSafeModule;

class DiagnosticBuffer final : public llvm::raw_ostream {
public:
    DiagnosticBuffer() : llvm::raw_ostream(true) {}

    std::string_view view() const { return {bytes_, size_}; }

private:
    void write_impl(const char* bytes, std::size_t size) override {
        const auto count = std::min(size, sizeof(bytes_) - size_);
        std::memcpy(bytes_ + size_, bytes, count);
        size_ += count;
    }

    std::uint64_t current_pos() const override { return size_; }

    char bytes_[1024];

    std::size_t size_{};
};

void require(bool condition, const char* message) {
    if (!condition)
        atelier::fatal(message);
}

void take(llvm::Error error) {
    if (error) {
        DiagnosticBuffer diagnostic;
        llvm::handleAllErrors(std::move(error), [&](const llvm::ErrorInfoBase& info) { info.log(diagnostic); });
        atelier::fatal(diagnostic.view());
    }
}

template <class T>
T take(llvm::Expected<T> result) {
    if (!result)
        take(result.takeError());
    return std::move(*result);
}

void pass(const ProbeCallbacks& callbacks, const char* message) {
    const auto line = std::string("[pass] ") + message;
    callbacks.log(line.c_str());
}

enum class FailureStage { none, finalization, emission };

struct FailureProbe {
    std::string module;

    FailureStage stage{FailureStage::none};

    ExecutorAddr code;

    bool registered_before_failure{};

    llvm::orc::MaterializationResponsibility* responsibility{};
};

llvm::orc::shared::CWrapperFunctionBuffer reject_finalization(const char* data, std::size_t size) {
    using namespace llvm::orc::shared;
    return WrapperFunction<SPSError(SPSExecutorAddr)>::handle(
               data, size,
               [](ExecutorAddr address) -> llvm::Error {
                   auto& probe = *address.toPtr<FailureProbe*>();
                   DWORD64 image_base{};
                   probe.registered_before_failure =
                       RtlLookupFunctionEntry(probe.code.getValue(), &image_base, nullptr) != nullptr;
                   return llvm::make_error<llvm::StringError>("Injected allocation finalization failure.",
                                                              llvm::inconvertibleErrorCode());
               })
        .release();
}

// Exercise actual ORC failure boundaries, after the real unwind install action.
class MaterializationFailurePlugin final : public llvm::orc::LinkGraphLinkingLayer::Plugin {
public:
    explicit MaterializationFailurePlugin(FailureProbe& probe) : probe_(probe) {}

    void modifyPassConfig(llvm::orc::MaterializationResponsibility& responsibility, llvm::jitlink::LinkGraph& graph,
                          llvm::jitlink::PassConfiguration& config) override {
        if (probe_.module.empty() || !graph.getName().starts_with(probe_.module))
            return;
        probe_.responsibility = &responsibility;
        config.PostAllocationPasses.push_back([this](llvm::jitlink::LinkGraph& allocated) -> llvm::Error {
            auto* entry = allocated.findDefinedSymbolByName(allocated.intern(probe_.module + "_entry"));
            if (!entry)
                return llvm::make_error<llvm::StringError>("Failure probe entry is missing.",
                                                           llvm::inconvertibleErrorCode());
            probe_.code = entry->getAddress();
            return llvm::Error::success();
        });
        if (probe_.stage == FailureStage::finalization) {
            config.PostFixupPasses.push_back([this](llvm::jitlink::LinkGraph& fixed) -> llvm::Error {
                using namespace llvm::orc::shared;
                auto action = WrapperFunctionCall::Create<SPSArgList<SPSExecutorAddr>>(
                    ExecutorAddr::fromPtr(&reject_finalization), ExecutorAddr::fromPtr(&probe_));
                if (!action)
                    return action.takeError();
                fixed.allocActions().push_back({std::move(*action), {}});
                return llvm::Error::success();
            });
        }
    }

    llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility& responsibility) override {
        if (probe_.stage == FailureStage::emission && probe_.responsibility == &responsibility) {
            DWORD64 image_base{};
            probe_.registered_before_failure =
                RtlLookupFunctionEntry(probe_.code.getValue(), &image_base, nullptr) != nullptr;
            return llvm::make_error<llvm::StringError>("Injected emission failure.", llvm::inconvertibleErrorCode());
        }
        return llvm::Error::success();
    }

    llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility& responsibility) override {
        if (probe_.responsibility == &responsibility)
            probe_.responsibility = nullptr;
        return llvm::Error::success();
    }

    llvm::Error notifyRemovingResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey) override {
        return llvm::Error::success();
    }

    void notifyTransferringResources(llvm::orc::JITDylib&, llvm::orc::ResourceKey, llvm::orc::ResourceKey) override {}

private:
    FailureProbe& probe_;
};

struct CodeRange {
    std::uint64_t begin;

    std::uint64_t end;

    bool contains(const void* address) const noexcept {
        const auto pc = reinterpret_cast<std::uintptr_t>(address);
        return pc >= begin && pc < end;
    }
};

CodeRange unwind_range(ExecutorAddr address) {
    DWORD64 image_base{};
    const auto* entry = RtlLookupFunctionEntry(address.getValue(), &image_base, nullptr);
    require(entry != nullptr, "Windows did not register the generated function's unwind table.");
    // UnwindData is an RVA, so zero can name the allocation's first byte.
    require(entry->BeginAddress < entry->EndAddress, "Invalid generated RUNTIME_FUNCTION entry.");
    const CodeRange range{image_base + entry->BeginAddress, image_base + entry->EndAddress};
    require(range.begin <= address.getValue() && address.getValue() < range.end,
            "The unwind entry does not cover the generated function.");
    return range;
}

bool executable(DWORD protection) noexcept {
    const auto access = protection & 0xff;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

bool writable(DWORD protection) noexcept {
    const auto access = protection & 0xff;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

MEMORY_BASIC_INFORMATION memory_info(ExecutorAddr address) {
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(address.toPtr<const void*>(), &info, sizeof(info)) == sizeof(info),
            "VirtualQuery failed for generated storage.");
    return info;
}

void verify_retired(ExecutorAddr address) {
    DWORD64 image_base{};
    require(RtlLookupFunctionEntry(address.getValue(), &image_base, nullptr) == nullptr,
            "An unloaded code allocation still has a Windows unwind entry.");
    const auto info = memory_info(address);
    require(info.State != MEM_COMMIT || !executable(info.Protect), "An unloaded code allocation remains executable.");
}

void configure_function(llvm::Function& function) {
    function.addFnAttr(llvm::Attribute::NoInline);
    function.addFnAttr(llvm::Attribute::NoUnwind);
    function.addFnAttr("disable-tail-calls", "true");
    function.setUWTableKind(llvm::UWTableKind::Async);
}

struct Unit {
    std::string stem;

    ResourceTrackerSP tracker;

    ExecutorAddr entry;

    ExecutorAddr outer;

    ExecutorAddr inner;

    ExecutorAddr label;

    ExecutorAddr counter;
};

ThreadSafeModule create_module(LLJIT& jit, const std::string& stem, std::uint32_t version,
                               std::size_t stack_bytes = sizeof(StackSnapshot)) {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(stem, *context);
    module->setSourceFileName(stem + ".ll");
    module->setTargetTriple(jit.getTargetTriple());
    module->setDataLayout(jit.getDataLayout());
    llvm::IRBuilder<> builder(*context);
    auto* i32 = builder.getInt32Ty();
    auto* i64 = builder.getInt64Ty();
    auto* ptr = builder.getPtrTy();
    auto* record = llvm::StructType::get(*context, {builder.getDoubleTy(), i64, builder.getFloatTy(), i32,
                                                    llvm::ArrayType::get(builder.getInt8Ty(), 8)});
    const auto* layout = module->getDataLayout().getStructLayout(record);
    require(layout->getSizeInBytes() == sizeof(ProbeRecord) &&
                layout->getElementOffset(0) == offsetof(ProbeRecord, total) &&
                layout->getElementOffset(1) == offsetof(ProbeRecord, checksum) &&
                layout->getElementOffset(2) == offsetof(ProbeRecord, amount) &&
                layout->getElementOffset(3) == offsetof(ProbeRecord, count),
            "LLVM record layout differs from the native record.");

    auto* label_data = llvm::ConstantDataArray::getString(*context, stem);
    new llvm::GlobalVariable(*module, label_data->getType(), true, llvm::GlobalValue::ExternalLinkage, label_data,
                             stem + "_label");
    auto* counter = new llvm::GlobalVariable(*module, i64, false, llvm::GlobalValue::ExternalLinkage,
                                             builder.getInt64(0), stem + "_counter");
    auto* signature = llvm::FunctionType::get(
        i32, {i32, i32, builder.getFloatTy(), builder.getDoubleTy(), i64, ptr, ptr, ptr}, false);
    auto native_calculate = module->getOrInsertFunction("atelier_jit_native_calculate", signature);
    auto host_bias = module->getOrInsertFunction("atelier_jit_host_bias", llvm::FunctionType::get(i64, {i64}, false));
    auto* entry = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, stem + "_entry", *module);
    configure_function(*entry);
    auto* begin = llvm::BasicBlock::Create(*context, "entry", entry);
    auto* success = llvm::BasicBlock::Create(*context, "success", entry);
    auto* failure = llvm::BasicBlock::Create(*context, "failure", entry);
    builder.SetInsertPoint(begin);
    auto* temporary = builder.CreateAlloca(record, nullptr, "native_result");
    temporary->setAlignment(llvm::Align(alignof(ProbeRecord)));
    llvm::SmallVector<llvm::Value*, 8> arguments;
    for (auto& argument : entry->args())
        arguments.push_back(&argument);
    arguments[6] = temporary;
    auto* status = builder.CreateCall(native_calculate, arguments);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(native_success)), success, failure);
    builder.SetInsertPoint(failure);
    builder.CreateRet(status);
    builder.SetInsertPoint(success);
    auto* bias = builder.CreateCall(host_bias, {entry->getArg(4)});
    auto* checksum = builder.CreateStructGEP(record, temporary, 1);
    builder.CreateStore(builder.CreateAdd(builder.CreateLoad(i64, checksum), bias), checksum);
    auto* count = builder.CreateStructGEP(record, temporary, 3);
    builder.CreateStore(builder.CreateAdd(builder.CreateLoad(i32, count), builder.getInt32(version)), count);
    builder.CreateMemCpy(entry->getArg(6), llvm::MaybeAlign(alignof(ProbeRecord)), temporary,
                         llvm::MaybeAlign(alignof(ProbeRecord)), sizeof(ProbeRecord));
    builder.CreateStore(builder.CreateAdd(builder.CreateLoad(i64, counter), builder.getInt64(1)), counter);
    builder.CreateRet(builder.getInt32(native_success));

    auto capture =
        module->getOrInsertFunction("atelier_jit_native_capture", llvm::FunctionType::get(i32, {ptr}, false));
    auto* inner = llvm::Function::Create(llvm::FunctionType::get(i32, {i32, ptr}, false),
                                         llvm::GlobalValue::ExternalLinkage, stem + "_inner", *module);
    configure_function(*inner);
    auto* inner_begin = llvm::BasicBlock::Create(*context, "entry", inner);
    auto* recurse = llvm::BasicBlock::Create(*context, "recurse", inner);
    auto* leaf = llvm::BasicBlock::Create(*context, "capture", inner);
    builder.SetInsertPoint(inner_begin);
    builder.CreateCondBr(builder.CreateICmpEQ(inner->getArg(0), builder.getInt32(0)), leaf, recurse);
    builder.SetInsertPoint(recurse);
    auto* next = builder.CreateSub(inner->getArg(0), builder.getInt32(1));
    auto* nested = builder.CreateCall(inner, {next, inner->getArg(1)});
    builder.CreateRet(builder.CreateAdd(nested, builder.getInt32(1)));
    builder.SetInsertPoint(leaf);
    builder.CreateRet(builder.CreateCall(capture, {inner->getArg(1)}));

    auto* outer = llvm::Function::Create(llvm::FunctionType::get(i32, {ptr}, false), llvm::GlobalValue::ExternalLinkage,
                                         stem + "_outer", *module);
    configure_function(*outer);
    builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", outer));
    auto* snapshot = builder.CreateAlloca(llvm::ArrayType::get(builder.getInt8Ty(), stack_bytes));
    snapshot->setAlignment(llvm::Align(32));
    auto* frames = builder.CreateCall(inner, {builder.getInt32(3), snapshot});
    builder.CreateMemCpy(outer->getArg(0), llvm::MaybeAlign(alignof(StackSnapshot)), snapshot, llvm::MaybeAlign(32),
                         sizeof(StackSnapshot));
    builder.CreateRet(builder.CreateAdd(frames, builder.getInt32(17)));

    DiagnosticBuffer diagnostic;
    if (llvm::verifyModule(*module, &diagnostic))
        atelier::fatal(diagnostic.view());
    return ThreadSafeModule(std::move(module), std::move(context));
}

void define_callbacks(LLJIT& jit, const ProbeCallbacks& callbacks) {
    llvm::orc::SymbolMap symbols;
    const auto flags = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
    symbols[jit.mangleAndIntern("atelier_jit_host_bias")] =
        llvm::orc::ExecutorSymbolDef(ExecutorAddr::fromPtr(callbacks.host_bias), flags);
    symbols[jit.mangleAndIntern("atelier_jit_native_calculate")] =
        llvm::orc::ExecutorSymbolDef(ExecutorAddr::fromPtr(callbacks.native_calculate), flags);
    symbols[jit.mangleAndIntern("atelier_jit_native_capture")] =
        llvm::orc::ExecutorSymbolDef(ExecutorAddr::fromPtr(callbacks.native_capture), flags);
    take(jit.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))));
}

Unit add_unit(LLJIT& jit, std::uint32_t version, std::size_t stack_bytes = sizeof(StackSnapshot)) {
    Unit unit;
    unit.stem = "probe_v" + std::to_string(version);
    unit.tracker = jit.getMainJITDylib().createResourceTracker();
    take(jit.addIRModule(unit.tracker, create_module(jit, unit.stem, version, stack_bytes)));
    unit.entry = take(jit.lookup(unit.stem + "_entry"));
    unit.outer = take(jit.lookup(unit.stem + "_outer"));
    unit.inner = take(jit.lookup(unit.stem + "_inner"));
    unit.label = take(jit.lookup(unit.stem + "_label"));
    unit.counter = take(jit.lookup(unit.stem + "_counter"));
    return unit;
}

void verify_calls(const Unit& unit) {
    using Entry = std::uint32_t (*)(std::uint32_t, std::int32_t, float, double, std::uint64_t, const ProbeRecord*,
                                    ProbeRecord*, ProbeDiagnostic*);
    const auto function = unit.entry.toPtr<Entry>();
    const ProbeRecord input{2.5, 100, 3.0f, 6};
    ProbeRecord output;
    ProbeDiagnostic diagnostic;
    constexpr std::uint64_t offset = 0x100000028ull;
    require(function(0, -3, 1.25f, 2.0, offset, &input, &output, &diagnostic) == native_success,
            "Mixed-register/stack native call failed.");
    require(output.total == 2.0 && output.amount == 4.25f && output.checksum == 101 + offset * 3 && output.count == 8 &&
                diagnostic.cleanups == 1,
            "Mixed scalar, 64-bit return, aligned-record ABI or cleanup mismatch.");
    output = ProbeRecord{77.0, 99, 18.0f, 25};
    require(function(1, -3, 1.25f, 2.0, 40, &input, &output, &diagnostic) == native_failure,
            "Native failure did not return its explicit status.");
    require(diagnostic.cleanups == 1 && std::strcmp(diagnostic.message, "Requested native failure.") == 0,
            "Native failure lost its diagnostic or cleanup.");
    require(output.total == 77.0 && output.checksum == 99 && output.amount == 18.0f && output.count == 25,
            "Failed native call exposed or copied an uninitialized result.");
    require(*unit.counter.toPtr<const std::uint64_t*>() == 1, "Failed invocation changed generated mutable state.");
}

__declspec(noinline) void verify_stack(const Unit& unit, std::size_t minimum_stack = 0) {
    const auto host_range = unwind_range(ExecutorAddr::fromPtr(&verify_stack));
    const auto entry_range = unwind_range(unit.entry);
    const auto outer_range = unwind_range(unit.outer);
    const auto inner_range = unwind_range(unit.inner);
    require(entry_range.begin != outer_range.begin && outer_range.begin != inner_range.begin,
            "Generated functions unexpectedly share an unwind range.");
    StackSnapshot snapshot;
    using Trace = std::uint32_t (*)(StackSnapshot*);
    const auto returned = unit.outer.toPtr<Trace>()(&snapshot);
    require(snapshot.count <= std::size(snapshot.frames) && returned == snapshot.count + 20,
            "Nested generated calls returned an invalid trace result.");
    require(snapshot.storage_address != 0 && snapshot.storage_address % 32 == 0,
            "Generated stack storage did not retain its requested alignment.");
    const auto host_storage = reinterpret_cast<std::uintptr_t>(&snapshot);
    require(host_storage > snapshot.storage_address && host_storage - snapshot.storage_address >= minimum_stack,
            "Generated stack allocation did not reach the requested size.");
    std::uint32_t outer_frames{};
    std::uint32_t inner_frames{};
    std::uint32_t host_frames{};
    for (std::uint32_t i = 0; i < snapshot.count; ++i) {
        outer_frames += outer_range.contains(snapshot.frames[i]) ? 1u : 0u;
        inner_frames += inner_range.contains(snapshot.frames[i]) ? 1u : 0u;
        host_frames += host_range.contains(snapshot.frames[i]) ? 1u : 0u;
    }
    require(outer_frames == 1 && inner_frames == 4 && host_frames == 1,
            "Windows stack walking did not cross every JIT frame and return to the native caller.");
}

__declspec(noinline) void verify_cross_unit(LLJIT& jit, const Unit& callee) {
    const auto host_range = unwind_range(ExecutorAddr::fromPtr(&verify_cross_unit));
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("cross_unit", *context);
    module->setTargetTriple(jit.getTargetTriple());
    module->setDataLayout(jit.getDataLayout());
    llvm::IRBuilder<> builder(*context);
    auto* signature = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()}, false);
    auto target = module->getOrInsertFunction(callee.stem + "_outer", signature);
    auto* entry = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, "cross_unit_entry", *module);
    configure_function(*entry);
    builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", entry));
    auto* result = builder.CreateCall(target, {entry->getArg(0)});
    builder.CreateRet(builder.CreateAdd(result, builder.getInt32(1)));
    auto tracker = jit.getMainJITDylib().createResourceTracker();
    take(jit.addIRModule(tracker, ThreadSafeModule(std::move(module), std::move(context))));
    const auto address = take(jit.lookup("cross_unit_entry"));
    const auto caller_range = unwind_range(address);
    const auto callee_range = unwind_range(callee.outer);
    StackSnapshot snapshot;
    const auto returned = address.toPtr<std::uint32_t (*)(StackSnapshot*)>()(&snapshot);
    require(snapshot.count <= std::size(snapshot.frames) && returned == snapshot.count + 21,
            "Cross-unit generated call returned an invalid result.");
    std::uint32_t caller_frames{};
    std::uint32_t callee_frames{};
    std::uint32_t host_frames{};
    for (std::uint32_t i = 0; i < snapshot.count; ++i) {
        caller_frames += caller_range.contains(snapshot.frames[i]) ? 1u : 0u;
        callee_frames += callee_range.contains(snapshot.frames[i]) ? 1u : 0u;
        host_frames += host_range.contains(snapshot.frames[i]) ? 1u : 0u;
    }
    require(caller_frames == 1 && callee_frames == 1 && host_frames == 1,
            "Windows stack walking lost a generated frame or the native caller.");
    // Retire the dependent before its callee; LLVM tracking does not establish
    // application-level quiescence or make stale raw function pointers safe.
    take(tracker->remove());
    verify_retired(address);
}

void verify_protection(const Unit& unit) {
    const auto code = memory_info(unit.entry);
    const auto constant = memory_info(unit.label);
    const auto data = memory_info(unit.counter);
    require(code.State == MEM_COMMIT && executable(code.Protect) && !writable(code.Protect),
            "Published machine code is not executable and non-writable.");
    require(constant.State == MEM_COMMIT && !executable(constant.Protect) && !writable(constant.Protect),
            "JIT constant storage is executable or writable.");
    require(data.State == MEM_COMMIT && !executable(data.Protect) && writable(data.Protect),
            "JIT mutable data has incorrect page protection.");
    require(std::strcmp(unit.label.toPtr<const char*>(), unit.stem.c_str()) == 0,
            "Code version metadata points at the wrong unit.");
}

void verify_failed_finalization(LLJIT& jit, FailureProbe& probe) {
    for (const auto stage : {FailureStage::finalization, FailureStage::emission}) {
        probe = {};
        probe.stage = stage;
        probe.module = stage == FailureStage::finalization ? "reject_finalize" : "reject_emission";
        auto tracker = jit.getMainJITDylib().createResourceTracker();
        take(jit.addIRModule(tracker, create_module(jit, probe.module, 9)));
        auto result = jit.lookup(probe.module + "_entry");
        require(!result, "Injected failed materialization published an entry.");
        llvm::consumeError(result.takeError());
        require(probe.registered_before_failure, "Failure probe did not reach the real post-registration boundary.");
        DWORD64 image_base{};
        require(RtlLookupFunctionEntry(probe.code.getValue(), &image_base, nullptr) == nullptr,
                "Failed materialization retained a Windows unwind table.");
        require(memory_info(probe.code).State == MEM_FREE,
                "Failed materialization retained committed or reserved code pages.");
        take(tracker->remove());
        probe = {};
    }
}

void run(const ProbeCallbacks& callbacks) {
    require(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
            "LLVM native target initialization failed.");
    auto target = take(llvm::orc::JITTargetMachineBuilder::detectHost());
    // Host/DLL addresses need not lie within 2 GiB of JIT allocations.
    target.setCodeModel(llvm::CodeModel::Large);
    target.setRelocationModel(llvm::Reloc::PIC_);
    FailureProbe failure_probe;
    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(std::move(target));
    builder.setObjectLinkingLayerCreator(
        [&failure_probe](llvm::orc::ExecutionSession& session, llvm::jitlink::JITLinkMemoryManager& memory)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
            auto layer = std::make_unique<llvm::orc::ObjectLinkingLayer>(session, memory);
            layer->addPlugin(atelier::jit::make_windows_unwind_plugin());
            layer->addPlugin(std::make_unique<MaterializationFailurePlugin>(failure_probe));
            return layer;
        });
    // These generated modules have no C++ CRT, constructors or TLS initialization.
    builder.setPlatformSetUp(llvm::orc::setUpInactivePlatform);
    auto jit = take(builder.create());
    jit->getExecutionSession().setErrorReporter([](llvm::Error error) { llvm::consumeError(std::move(error)); });
    define_callbacks(*jit, callbacks);
    auto first = add_unit(*jit, 1, 32768);
    auto second = add_unit(*jit, 2);
    verify_calls(first);
    pass(callbacks, "Host -> JIT -> host/DLL: mixed scalars, wide values and aligned records");
    pass(callbacks, "Native failures return explicit status and clean up once; failed results stay untouched");
    verify_cross_unit(*jit, first);
    pass(callbacks, "Cross-unit generated calls preserve results and Windows unwind metadata");
    verify_protection(first);
    pass(callbacks, "Published code is RX, constants are RO, mutable data is RW and non-executable");
    verify_failed_finalization(*jit, failure_probe);
    pass(callbacks, "Finalize/emission failures revoke unwind registration and free their code allocation");
    auto transferred = jit->getMainJITDylib().createResourceTracker();
    first.tracker->transferTo(*transferred);
    first.tracker = std::move(transferred);
    verify_stack(first, 32768);
    pass(callbacks, "Transferred 32-KiB aligned stack unwinds through recursive JIT frames to the host");

    const std::string retained_version = first.label.toPtr<const char*>();
    const std::array retired_addresses{first.entry, first.outer, first.inner};
    // All calls have returned. ResourceTracker does not establish this quiescence for us.
    take(first.tracker->remove());
    auto removed = jit->lookup(first.stem + "_entry");
    require(!removed, "Removed code remains visible in the symbol table.");
    llvm::consumeError(removed.takeError());
    for (const auto address : retired_addresses)
        verify_retired(address);
    require(retained_version == "probe_v1", "Independent version metadata was lost during code retirement.");
    const ProbeRecord input{2.5, 100, 3.0f, 6};
    ProbeRecord output;
    ProbeDiagnostic diagnostic;
    require(second.entry.toPtr<decltype(&atelier_jit_native_calculate)>()(0, -3, 1.25f, 2.0, 40, &input, &output,
                                                                          &diagnostic) == native_success &&
                output.total == 2.0 && output.checksum == 221 && output.amount == 4.25f && output.count == 9,
            "Retiring the first unit damaged the second unit.");
    pass(callbacks,
         "Tracker removal revokes symbols, unwind entries and executable storage without damaging another unit");

    const auto shutdown_address = second.entry;
    // Release tracker handles before destroying their owning execution session.
    first.tracker.reset();
    second.tracker.reset();
    jit.reset();
    verify_retired(shutdown_address);
    pass(callbacks, "JIT shutdown releases still-owned code and Windows unwind entries");
}

}

std::uint32_t atelier_run_jit_acceptance(const ProbeCallbacks* callbacks, char* error,
                                         std::uint32_t error_capacity) noexcept {
    static const bool handlers_installed = [] {
        atelier::install_fatal_handler();
        llvm::install_fatal_error_handler([](void*, const char*, bool) { atelier::fatal("llvm fatal error"); });
        return true;
    }();
    (void)handlers_installed;
    if (!error || error_capacity == 0)
        return 1;
    error[0] = '\0';
    require(callbacks && callbacks->host_bias && callbacks->native_calculate && callbacks->native_capture &&
                callbacks->log,
            "Invalid JIT acceptance callbacks.");
    run(*callbacks);
    return 0;
}

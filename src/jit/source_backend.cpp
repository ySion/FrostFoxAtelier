#include "jit/source_backend.hpp"
#include "jit/windows_unwind.hpp"
#include "jit/math_lowering.hpp"
#include "jit/diagnostic_artifact.hpp"
#include "language/source.hpp"
#include "runtime/value_storage.hpp"

#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
#include "jit/source_backend_verification.hpp"
#endif

#include <llvm/ADT/SmallVector.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Coroutines/CoroEarly.h>
#include <llvm/Transforms/Coroutines/CoroSplit.h>
#include <llvm/Transforms/Coroutines/CoroCleanup.h>
#include <llvm/Transforms/Scalar/Scalarizer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <bit>
#include <cstring>
#include <algorithm>
#include <limits>
#include <cstdio>
#include <memory>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atelier::source::detail {
namespace {
// Keep generated failure reasons separate from opaque host service status codes.
// This entry is private to the DLL; the exported invocation ABI stays unchanged.
using Entry = std::uint32_t (*)(void*, const BusinessValue*, void*, EntryFailure*) noexcept;
using CoroutineEntry = void* (*)(void*, const BusinessValue*, void*, EntryFailure*,
    std::uint32_t*, std::uint32_t*) noexcept;
using DiagnosticEntry = std::uint32_t (*)(void*, const BusinessValue*, void*, EntryFailure*, BusinessTrace*) noexcept;
using DiagnosticCoroutineEntry = void* (*)(void*, const BusinessValue*, void*, EntryFailure*,
    std::uint32_t*, std::uint32_t*, BusinessTrace*) noexcept;
using CoroutineAction = void (*)(void*) noexcept;
using llvm::orc::LLJIT;
using llvm::orc::ExecutorAddr;

struct DiagnosticCatalog {
    std::vector<std::string> functions;
    std::vector<BusinessLocation> locations;
    std::vector<std::vector<std::string>> variable_names;
    std::vector<std::vector<BusinessDebugVariable>> variables;
    std::vector<BusinessDebugFunction> debug_functions;
    std::vector<std::uint32_t> debug_sites;
};
struct Artifacts {
    std::string target_triple, cpu_features, original_ir, optimized_ir, assembly;
    std::vector<std::byte> object_code;
};
struct CompiledBusiness {
    std::shared_ptr<std::string> diagnostic{std::make_shared<std::string>()};
    std::string object_type_name;
    std::string function_name;
    std::string source_name;
    BusinessServices services{};
    CompileOptions options;
    std::vector<BusinessParameter> parameters;
    std::vector<std::string> field_names;
    std::vector<BusinessFieldBinding> fields;
    std::vector<std::string> method_names;
    std::vector<std::vector<BusinessParameter>> method_parameters;
    std::vector<BusinessMethodBinding> methods;
    std::vector<std::string> enum_names;
    std::vector<std::vector<std::string>> enum_item_names;
    std::vector<std::vector<BusinessEnumItem>> enum_items;
    std::vector<BusinessEnumBinding> enumerations;
    std::vector<std::string> value_names;
    std::vector<std::vector<std::string>> value_field_names;
    std::vector<std::vector<BusinessValueField>> value_fields;
    std::vector<BusinessValueBinding> values;
    BusinessSignature signature{};
    BusinessEnvironment environment{};
    bool requires_receiver{};
    BusinessFrameLayout layout{};
    std::shared_ptr<LLJIT> jit;
    Entry entry{};
    CoroutineEntry coroutine_entry{};
    DiagnosticEntry diagnostic_entry{};
    DiagnosticCoroutineEntry diagnostic_coroutine_entry{};
    std::shared_ptr<DiagnosticCatalog> diagnostics;
    std::shared_ptr<Artifacts> artifacts;
    std::string entry_symbol;
    CoroutineAction resume{}, destroy{};
};

void* allocate_coroutine(std::uint64_t size, std::uint32_t alignment) noexcept {
    // coro.id(0) promises at least twice pointer alignment, even if the
    // computed frame layout itself needs less. Match this in deallocation.
    constexpr std::uint32_t minimum = 2 * sizeof(void*);
    return ::operator new(static_cast<std::size_t>(size),
        std::align_val_t{alignment < minimum ? minimum : alignment}, std::nothrow);
}
void free_coroutine(void* storage, std::uint32_t alignment) noexcept {
    constexpr std::uint32_t minimum = 2 * sizeof(void*);
    ::operator delete(storage, std::align_val_t{alignment < minimum ? minimum : alignment});
}

BusinessType business_type(language::ValueType type) {
    switch (type.kind) {
    case language::ValueType::void_value: return ValueKind::void_value;
    case language::ValueType::f32: return ValueKind::f32;
    case language::ValueType::f64: return ValueKind::f64;
    case language::ValueType::u32: return ValueKind::u32;
    case language::ValueType::i32: return ValueKind::i32;
    case language::ValueType::boolean: return ValueKind::boolean;
    case language::ValueType::enumeration: return {ValueKind::enumeration, type.type_index};
    case language::ValueType::record: return {ValueKind::record, type.type_index};
    case language::ValueType::array: return {ValueKind::array, type.type_index};
    case language::ValueType::string: return {ValueKind::string, type.type_index};
    default: throw std::logic_error("Unsupported business scalar type.");
    }
}

language::ValueType source_type(BusinessType kind) {
    if (kind.kind != ValueKind::enumeration && !aggregate(kind) && kind.type_index != 0)
        throw std::runtime_error("Scalar catalog entry has an unexpected enum identity.");
    switch (kind.kind) {
    case ValueKind::f32: return language::ValueType::f32;
    case ValueKind::f64: return language::ValueType::f64;
    case ValueKind::u32: return language::ValueType::u32;
    case ValueKind::i32: return language::ValueType::i32;
    case ValueKind::boolean: return language::ValueType::boolean;
    case ValueKind::enumeration: return {language::ValueType::enumeration, kind.type_index};
    case ValueKind::record: return {language::ValueType::record, kind.type_index};
    case ValueKind::array: return {language::ValueType::array, kind.type_index};
    case ValueKind::string: return {language::ValueType::string, kind.type_index};
    default: throw std::runtime_error("Host fields, parameters and non-void results require float or uint kinds.");
    }
}

constexpr std::size_t diagnostic_source_limit = 195;

std::string diagnostic_source_name(std::string_view name) {
    constexpr std::size_t edge_bytes = (diagnostic_source_limit - 3) / 2;
    if (name.size() <= diagnostic_source_limit) return std::string(name);
    auto prefix = name.substr(0, edge_bytes);
    auto suffix = name.substr(name.size() - edge_bytes);
    // Keep the request suffix and leave room for location/cause in the host's
    // diagnostic buffer. Full source identity remains in compiled metadata.
    // Do not split UTF-8 characters at either display boundary.
    const auto continuation = [](char byte) { return (static_cast<unsigned char>(byte) & 0xc0) == 0x80; };
    while (!prefix.empty() && continuation(name[prefix.size()])) prefix.remove_suffix(1);
    while (!suffix.empty() && continuation(suffix.front())) suffix.remove_prefix(1);
    return std::string(prefix) + "..." + std::string(suffix);
}

void check(llvm::Error error) {
    if (error) throw std::runtime_error(llvm::toString(std::move(error)));
}

template<class T>
T take(llvm::Expected<T> value) {
    if (!value) throw std::runtime_error(llvm::toString(value.takeError()));
    return std::move(*value);
}

class SourceLowering {
public:
    SourceLowering(llvm::LLVMContext& context, llvm::Module& module, const language::Program& source,
        std::uint32_t function, const std::vector<llvm::Function*>& functions, std::uint32_t strong_base, std::uint32_t weak_base,
        DiagnosticCatalog* catalog, CompileOptions options)
        : function_(source.functions[function]), source_(source), functions_(functions), context_(&context), module_(&module), ir_(context),
          entry_(functions[function]), local_slots_(function_.locals.size(), null_reference_slot),
          temporary_slots_(function_.expressions.size(), null_reference_slot),
          strong_slots_(strong_base), weak_slots_(weak_base), first_strong_slot_(strong_base), first_weak_slot_(weak_base),
          public_entry_(source.exported(function)), protected_values_(function_.uses_scoped_values),
          diagnostics_(options.diagnostics ? catalog : nullptr), catalog_(catalog),
          debugging_(options.debugging && (source.exports.empty() || !public_entry_)),
          function_index_(function), position_(function_.position) {
        for (std::size_t index = 0; index < function_.locals.size(); ++index) {
            const auto type = function_.locals[index].type;
            if (is_reference(type)) local_slots_[index] = allocate_reference(type);
        }
        for (std::size_t index = 0; index < function_.expressions.size(); ++index) {
            const auto& node = function_.expressions[index];
            using Kind = language::Expression::Kind;
            if (node.kind == Kind::create_reference || node.kind == Kind::observe_reference ||
                node.kind == Kind::acquire_reference)
                temporary_slots_[index] = allocate_reference(node.type);
            has_source_calls_ |= node.kind == Kind::source_call;
            if (node.kind == Kind::call) {
                max_call_arguments_ = std::max(max_call_arguments_, static_cast<std::uint32_t>(node.arguments.size()));
                has_call_result_ |= node.type != language::ValueType::void_value;
            }
        }
        auto* i32 = ir_.getInt32Ty();
        auto* f32 = ir_.getFloatTy();
        auto* ptr = ir_.getPtrTy();
        const auto declare = [&](const char* name, llvm::Type* result,
                                 std::initializer_list<llvm::Type*> parameters) {
            auto function = module_->getOrInsertFunction(name,
                llvm::FunctionType::get(result, llvm::ArrayRef(parameters.begin(), parameters.size()), false));
            llvm::cast<llvm::Function>(function.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
            return function;
        };
        abi_value_type_ = llvm::StructType::get(*context_, {i32, i32, ir_.getInt64Ty(), i32, ptr, i32, i32});
        begin_ = declare("business_begin", i32, {ptr});
        create_ = declare("business_create", i32, {ptr, i32});
        read_float_ = declare("business_read_float", i32, {ptr, i32, i32, ptr});
        write_float_ = declare("business_write_float", i32, {ptr, i32, i32, f32});
        read_double_ = declare("business_read_double", i32, {ptr, i32, i32, ptr});
        write_double_ = declare("business_write_double", i32, {ptr, i32, i32, ir_.getDoubleTy()});
        read_value_ = declare("business_read_value", i32, {ptr, i32, i32, ptr});
        write_value_ = declare("business_write_value", i32, {ptr, i32, i32, ptr});
        read_uint_ = declare("business_read_uint", i32, {ptr, i32, i32, ptr});
        write_uint_ = declare("business_write_uint", i32, {ptr, i32, i32, i32});
        read_bool_ = declare("business_read_bool", i32, {ptr, i32, i32, ptr});
        write_bool_ = declare("business_write_bool", i32, {ptr, i32, i32, i32});
        call_ = declare("business_call", i32, {ptr, i32, i32, ptr, i32, ptr});
        poll_ = declare("business_poll", i32, {ptr});
        finish_call_ = declare("business_finish", i32, {ptr});
        clear_ = declare("business_clear", ir_.getVoidTy(), {ptr, i32});
        reference_ = declare("business_reference", i32, {ptr, i32, i32, i32});
        same_reference_ = declare("business_same_reference", i32, {ptr, i32, i32, ptr});
        construct_value_ = declare("business_construct_value", i32, {ptr, i32, ptr, ptr});
        assign_value_ = declare("business_assign_value", i32, {ptr, i32, ptr, ptr});
        destroy_value_ = declare("business_destroy_value", ir_.getVoidTy(), {ptr, i32, ptr});
        mutate_sequence_ = declare("business_mutate_sequence", i32, {ptr, i32, i32, ptr, ptr, ir_.getInt64Ty()});
        if (debugging_) {
            debug_enter_ = declare("business_debug_enter", i32, {ptr, ptr});
            debug_point_ = declare("business_debug_point", i32, {ptr, ptr, i32, i32, i32});
            debug_leave_ = declare("business_debug_leave", ir_.getVoidTy(), {ptr, ptr});
            debug_local_type_ = llvm::StructType::get(*context_, {ptr, i32, i32});
            debug_frame_type_ = llvm::StructType::get(*context_, {ptr, ptr, ir_.getInt64Ty(), i32, i32});
        }
        invocation_ = entry_->getArg(0);
        result_output_ = entry_->getArg(public_entry_ ? 2 : 1);
        failure_output_ = entry_->getArg(public_entry_ ? 3 : 2);
        // A shared helper can run under either a borrowed-value entry or an
        // ordinary entry. Carry that call-chain effect through the internal ABI;
        // O2 folds it when the helper is inlined into a known entry.
        if (public_entry_ || protected_values_) borrow_active_ = ir_.getInt1(protected_values_);
        else borrow_active_ = entry_->getArg(3);
        if (diagnostics_) {
            trace_ = entry_->getArg(public_entry_ && function_.is_coroutine ? 6 : 4);
            trace_->setName("diagnostic.trace");
        }
    }

    BusinessFrameLayout layout() const noexcept { return {strong_slots_, weak_slots_, nullptr, max_call_arguments_}; }

    void generate() {
        auto* start = block("entry");
        failure_ = block("cleanup.failure");
        finish_ = block("finish");
        ir_.SetInsertPoint(failure_);
        failure_status_ = ir_.CreatePHI(ir_.getInt32Ty(), 0, "failure.status");
        failure_reason_ = ir_.CreatePHI(ir_.getInt32Ty(), 0, "failure.reason");
        if (catalog_) failure_location_ = ir_.CreatePHI(ir_.getInt32Ty(), 0, "failure.location");
        ir_.SetInsertPoint(start);
        if (function_.is_coroutine) setup_coroutine();
        else prepare_debug_storage();
        float_result_ = ir_.CreateAlloca(ir_.getFloatTy());
        double_result_ = ir_.CreateAlloca(ir_.getDoubleTy());
        double_result_->setAlignment(llvm::Align(8));
        math_field_result_ = ir_.CreateAlloca(llvm::ArrayType::get(ir_.getInt8Ty(), 128), nullptr, "field.math");
        math_field_result_->setAlignment(llvm::Align(8));
        uint_result_ = ir_.CreateAlloca(ir_.getInt32Ty());
        float_result_->setAlignment(llvm::Align(4));
        uint_result_->setAlignment(llvm::Align(4));
        if (has_source_calls_) {
            source_failure_ = ir_.CreateAlloca(ir_.getInt32Ty(), nullptr, "callee.failure");
            source_result_ = ir_.CreateAlloca(llvm::ArrayType::get(ir_.getInt8Ty(), 128), nullptr, "callee.result");
            source_result_->setAlignment(llvm::Align(8));
        }
        // All host-call scratch is allocated once, never on a loop backedge.
        if (max_call_arguments_ != 0) {
            call_arguments_ = ir_.CreateAlloca(abi_value_type_, ir_.getInt32(max_call_arguments_), "host.arguments");
            call_arguments_->setAlignment(llvm::Align(8));
        }
        if (has_call_result_) {
            call_result_ = ir_.CreateAlloca(abi_value_type_, nullptr, "host.result");
            call_result_->setAlignment(llvm::Align(8));
            call_math_result_ = ir_.CreateAlloca(llvm::ArrayType::get(ir_.getInt8Ty(), 128), nullptr, "host.math.result");
            call_math_result_->setAlignment(llvm::Align(8));
        }
        owned_local_slots_.resize(function_.locals.size(), language::no_node);
        expression_value_slots_.resize(function_.expressions.size(), language::no_node);
        source_value_results_.resize(function_.expressions.size());
        argument_value_slots_.resize(function_.expressions.size());
        for (std::size_t index = 0; index < function_.locals.size(); ++index) {
            const auto& local = function_.locals[index];
            const bool parameter = index < function_.parameters.size();
            if (managed_value(local.type) && (!parameter ||
                (public_entry_ && function_.parameters[index].passing == ParameterPassing::value))) {
                const auto slot = allocate_owned(local.type, local.name);
                owned_local_slots_[index] = slot;
                locals_.push_back(owned_[slot].storage);
                continue;
            }
            if (is_reference(local.type) || (parameter &&
                (function_.parameters[index].passing != ParameterPassing::value || managed_value(local.type)))) {
                locals_.push_back(nullptr);
                continue;
            }
            auto* storage = ir_.CreateAlloca(storage_type(local.type), nullptr, local.name);
            storage->setAlignment(llvm::Align(storage_alignment(local.type)));
            locals_.push_back(storage);
        }
        std::vector<bool> direct_text_inputs(function_.expressions.size());
        const auto mark_text = [&](language::NodeId id) {
            if (id != language::no_node) direct_text_inputs[id] = true;
        };
        for (const auto& node : function_.statements) mark_text(direct_text_input(node));
        for (const auto& node : function_.expressions) mark_text(direct_text_input(node));
        for (std::size_t index = 0; index < function_.expressions.size(); ++index) {
            const auto& node = function_.expressions[index];
            if (node.kind == language::Expression::Kind::string_literal && !direct_text_inputs[index])
                expression_value_slots_[index] = allocate_owned(node.type, "text.literal");
            if (managed_value(node.type) && node.kind == language::Expression::Kind::source_call)
                source_value_results_[index] = allocate_value_storage(node.type, "function.value.result");
            if (managed_value(node.type) && node.kind == language::Expression::Kind::call)
                expression_value_slots_[index] = allocate_owned(node.type, "value.result");
            if (node.kind == language::Expression::Kind::call || node.kind == language::Expression::Kind::source_call) {
                auto& slots = argument_value_slots_[index];
                slots.resize(node.arguments.size(), language::no_node);
                for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                    const auto type = function_.expressions[node.arguments[i]].type;
                    if (node.argument_passing[i] == ParameterPassing::value && managed_value(type))
                        slots[i] = allocate_owned(type, "value.argument");
                }
            }
        }
        open_debug_frame();
        for (std::size_t index = 0; index < function_.parameters.size(); ++index) {
            if (!public_entry_) {
                auto* argument = entry_->getArg(static_cast<unsigned>(index + 4 + (diagnostics_ ? 1 : 0)));
                // Internal record arguments already own a caller-created
                // snapshot. It cannot escape this synchronous call chain.
                if (function_.parameters[index].passing == ParameterPassing::value && !managed_value(function_.parameters[index].type))
                    store_value(argument, locals_[index], function_.parameters[index].type);
                else locals_[index] = argument;
                debug_local(index, DebugAvailability::available);
                continue;
            }
            auto* argument = ir_.CreateInBoundsGEP(abi_value_type_, entry_->getArg(1),
                ir_.getInt32(static_cast<std::uint32_t>(index)));
            if (function_.parameters[index].passing != ParameterPassing::value) {
                locals_[index] = ir_.CreateLoad(ir_.getPtrTy(), ir_.CreateStructGEP(abi_value_type_, argument, 4));
                debug_local(index, DebugAvailability::available);
                continue;
            }
            const auto type = function_.parameters[index].type;
            if (managed_value(type)) {
                construct_owned(owned_local_slots_[index],
                    ir_.CreateLoad(ir_.getPtrTy(), ir_.CreateStructGEP(abi_value_type_, argument, 4)));
                debug_local(index, DebugAvailability::available);
                continue;
            }
            auto* value = math_type(type) ? load_value(ir_.CreateLoad(ir_.getPtrTy(),
                ir_.CreateStructGEP(abi_value_type_, argument, 4)), type) :
                decode_bits(ir_.CreateLoad(ir_.getInt64Ty(), ir_.CreateStructGEP(abi_value_type_, argument, 2)), type);
            store_value(value, locals_.at(index), type);
            debug_local(index, DebugAvailability::available);
        }
        if (public_entry_ && !protected_values_) checked_call(begin_, {invocation_});
        parameter_views_.resize(function_.parameters.size());
        for (std::size_t i = 0; i < function_.parameters.size(); ++i) {
            const auto type = function_.parameters[i].type;
            if (type.kind == language::ValueType::array || type.kind == language::ValueType::string)
                parameter_views_[i] = sequence_view(locals_[i], type);
        }
        safe_point();
        debug_stop(DebugPoint::entry);
        statement(function_.body);
        if (!terminated()) {
            if (function_.result == language::ValueType::void_value) ir_.CreateBr(finish_);
            else ir_.CreateUnreachable(); // The typed tree rejects every reachable value fallthrough.
        }
        ir_.SetInsertPoint(finish_);
        // Every normal exit releases user resources before the host finish
        // policy; keep the receiver alive until that policy has completed.
        clear_user_values();
        if (public_entry_ && !protected_values_) checked_call(finish_call_, {invocation_});
        if (public_entry_) clear_slot(receiver_slot);
        close_debug_frame();
        complete(ir_.getInt32(0));
        ir_.SetInsertPoint(failure_);
        trace_failure();
        debug_failure();
        clear_execution_resources();
        close_debug_frame();
        ir_.CreateStore(failure_reason_, failure_output_);
        complete(failure_status_);
        if (function_.is_coroutine) finish_coroutine();

    }
private:
    void prepare_debug_storage() {
        if (!debugging_) return;
        // Async calls initialize this after coro.begin and before the initial
        // suspend, so LLVM can place both tables and their interior pointers in
        // the coroutine frame, including the ready-cancellation path.
        debug_frame_ = ir_.CreateAlloca(debug_frame_type_, nullptr, "debug.frame");
        debug_frame_->setAlignment(llvm::Align(8));
        ir_.CreateStore(llvm::Constant::getNullValue(debug_frame_type_), debug_frame_);
        auto* table_type = llvm::ArrayType::get(debug_local_type_, std::max<std::size_t>(1, function_.locals.size()));
        debug_locals_ = ir_.CreateAlloca(table_type, nullptr, "debug.locals");
        debug_locals_->setAlignment(llvm::Align(8));
        ir_.CreateStore(llvm::Constant::getNullValue(table_type), debug_locals_);
        ir_.CreateStore(debug_locals_, ir_.CreateStructGEP(debug_frame_type_, debug_frame_, 1));
        ir_.CreateStore(ir_.getInt32(function_index_), ir_.CreateStructGEP(debug_frame_type_, debug_frame_, 3));
        ir_.CreateStore(ir_.getInt32(location()), ir_.CreateStructGEP(debug_frame_type_, debug_frame_, 4));
    }
    void debug_local(std::size_t index, DebugAvailability availability) {
        if (!debugging_) return;
        auto* value = ir_.CreateInBoundsGEP(debug_local_type_, debug_locals_, ir_.getInt32(static_cast<std::uint32_t>(index)));
        if (availability == DebugAvailability::available && locals_[index])
            ir_.CreateStore(locals_[index], ir_.CreateStructGEP(debug_local_type_, value, 0));
        ir_.CreateStore(ir_.getInt32(local_slots_[index]), ir_.CreateStructGEP(debug_local_type_, value, 1));
        ir_.CreateStore(ir_.getInt32(static_cast<std::uint32_t>(availability)), ir_.CreateStructGEP(debug_local_type_, value, 2));
    }
    void open_debug_frame() {
        if (!debugging_) return;
        for (std::size_t i = 0; i < function_.locals.size(); ++i)
            debug_local(i, i < function_.parameters.size() ? DebugAvailability::uninitialized : DebugAvailability::out_of_scope);
        checked_call(debug_enter_, {invocation_, debug_frame_});
    }
    void debug_location() {
        if (debugging_) ir_.CreateStore(ir_.getInt32(location()), ir_.CreateStructGEP(debug_frame_type_, debug_frame_, 4));
    }
    void debug_stop(DebugPoint kind) {
        if (!debugging_) return;
        const auto site = location();
        if (kind == DebugPoint::statement || kind == DebugPoint::entry) catalog_->debug_sites.push_back(site);
        checked_call(debug_point_, {invocation_, debug_frame_, ir_.getInt32(site),
            ir_.getInt32(static_cast<std::uint32_t>(kind)), ir_.getInt32(0)});
    }
    void debug_failure() {
        if (!debugging_) return;
        trace_event(DiagnosticCall::debug_hook);
        // An error stop cannot recover the failed operation. Its command is
        // observed by the host, then the original failure continues to cleanup.
        ir_.CreateCall(debug_point_, {invocation_, debug_frame_, failure_location_,
            ir_.getInt32(static_cast<std::uint32_t>(DebugPoint::error)), failure_reason_});
    }
    void close_debug_frame() {
        if (!debugging_) return;
        trace_event(DiagnosticCall::debug_hook);
        ir_.CreateCall(debug_leave_, {invocation_, debug_frame_});
    }
    // Positions follow the typed expression/statement recursion. Nested operand
    // lowering cannot leave its location on a parent's callback or failure edge.
    struct PositionScope {
        language::SourcePosition& current;
        language::SourcePosition saved;
        PositionScope(language::SourcePosition& target, language::SourcePosition next)
            : current(target), saved(std::exchange(target, next)) {}
        ~PositionScope() { current = saved; }
    };
    std::uint32_t location() {
        if (!catalog_) return no_diagnostic_location;
        if (const auto found = location_ids_.find(position_.offset); found != location_ids_.end()) return found->second;
        if (catalog_->locations.size() >= no_diagnostic_location)
            throw std::runtime_error("Source diagnostic location table exceeds its index range.");
        const auto id = static_cast<std::uint32_t>(catalog_->locations.size());
        catalog_->locations.push_back({catalog_->functions[function_index_].c_str(),
            position_.offset, position_.line, position_.column});
        location_ids_.emplace(position_.offset, id);
        return id;
    }
    llvm::Value* trace_address(std::size_t offset) {
        return ir_.CreateInBoundsGEP(ir_.getInt8Ty(), trace_, ir_.getInt64(offset));
    }
    template<class F> void record(F&& write) {
        if (!diagnostics_) return;
        auto* capture = block("diagnostic.record");
        auto* next = block("diagnostic.continue");
        ir_.CreateCondBr(ir_.CreateIsNotNull(trace_), capture, next);
        ir_.SetInsertPoint(capture);
        write();
        ir_.CreateBr(next);
        ir_.SetInsertPoint(next);
    }
    void trace_position(std::size_t member = offsetof(BusinessTrace, current_location)) {
        if (!diagnostics_) return;
        const auto id = location();
        record([&] { ir_.CreateStore(ir_.getInt32(id), trace_address(member)); });
    }
    void trace_event(DiagnosticCall kind) {
        if (!diagnostics_) return;
        const auto id = location();
        record([&] {
            auto* counter = trace_address(offsetof(BusinessTrace, calls) +
                offsetof(DiagnosticCounters, values) + static_cast<std::size_t>(kind) * sizeof(std::uint64_t));
            auto* value = ir_.CreateLoad(ir_.getInt64Ty(), counter);
            auto* increment = ir_.CreateSelect(ir_.CreateICmpEQ(value, ir_.getInt64(UINT64_MAX)),
                value, ir_.CreateAdd(value, ir_.getInt64(1)));
            ir_.CreateStore(increment, counter);
            ir_.CreateStore(ir_.getInt32(id), trace_address(offsetof(BusinessTrace, current_location)));
        });
    }
    void failure_edge(llvm::Value* status, llvm::Value* reason) {
        failure_status_->addIncoming(status, ir_.GetInsertBlock());
        failure_reason_->addIncoming(reason, ir_.GetInsertBlock());
        if (catalog_) failure_location_->addIncoming(ir_.getInt32(location()), ir_.GetInsertBlock());
    }
    void trace_failure() {
        record([&] {
            auto* address = trace_address(offsetof(BusinessTrace, failure_location));
            auto* previous = ir_.CreateLoad(ir_.getInt32Ty(), address);
            ir_.CreateStore(ir_.CreateSelect(ir_.CreateICmpEQ(previous, ir_.getInt32(no_diagnostic_location)),
                failure_location_, previous), address);
        });
    }
    DiagnosticCall service_kind(llvm::FunctionCallee function) {
        const auto name = llvm::cast<llvm::Function>(function.getCallee())->getName();
        if (name.starts_with("business_read_")) return DiagnosticCall::field_read;
        if (name.starts_with("business_write_")) return DiagnosticCall::field_write;
        if (name.starts_with("business_debug_")) return DiagnosticCall::debug_hook;
        if (function.getCallee() == begin_.getCallee()) return DiagnosticCall::begin;
        if (function.getCallee() == finish_call_.getCallee()) return DiagnosticCall::finish;
        if (function.getCallee() == call_.getCallee()) return DiagnosticCall::native_call;
        if (function.getCallee() == poll_.getCallee()) return DiagnosticCall::poll;
        if (function.getCallee() == create_.getCallee()) return DiagnosticCall::object_create;
        if (function.getCallee() == reference_.getCallee()) return DiagnosticCall::reference;
        if (function.getCallee() == same_reference_.getCallee()) return DiagnosticCall::reference_compare;
        if (function.getCallee() == construct_value_.getCallee()) return DiagnosticCall::value_construct;
        if (function.getCallee() == assign_value_.getCallee()) return DiagnosticCall::value_assign;
        if (function.getCallee() == mutate_sequence_.getCallee()) return DiagnosticCall::sequence_mutation;
        throw std::logic_error("Unclassified source service call.");
    }
    static bool floating_type(language::ValueType type) noexcept {
        return type == language::ValueType::f32 || type == language::ValueType::f64;
    }
    math::Type math_type(language::ValueType type) const {
        return type.kind == language::ValueType::record ? source_.value_layouts.at(type.type_index).math : math::Type{};
    }
    bool managed_value(language::ValueType type) const { return language::aggregate(type) && !math_type(type); }
    llvm::Type* storage_type(language::ValueType type) {
        if (type == language::ValueType::boolean) return ir_.getInt8Ty();
        if (const auto math = math_type(type)) return llvm::ArrayType::get(
            MathLowering::scalar_type(*context_, math), math.components());
        return value_type(type);
    }
    llvm::Value* load_value(llvm::Value* address, language::ValueType type) {
        if (const auto math = math_type(type)) return MathLowering{ir_, *module_}.load(address, math);
        if (type == language::ValueType::boolean)
            return ir_.CreateTrunc(ir_.CreateAlignedLoad(ir_.getInt8Ty(), address, llvm::Align(1)), ir_.getInt1Ty());
        return ir_.CreateAlignedLoad(value_type(type), address, llvm::Align(storage_alignment(type)));
    }
    void store_value(llvm::Value* value, llvm::Value* address, language::ValueType type) {
        if (type == language::ValueType::boolean) value = ir_.CreateZExt(value, ir_.getInt8Ty());
        if (const auto math = math_type(type)) MathLowering{ir_, *module_}.store(value, address, math);
        else ir_.CreateAlignedStore(value, address, llvm::Align(storage_alignment(type)));
    }
    llvm::Value* encode_bits(llvm::Value* value, language::ValueType type) {
        if (type == language::ValueType::f64) return ir_.CreateBitCast(value, ir_.getInt64Ty());
        if (type == language::ValueType::f32) value = ir_.CreateBitCast(value, ir_.getInt32Ty());
        return ir_.CreateZExt(value, ir_.getInt64Ty());
    }
    llvm::Value* decode_bits(llvm::Value* bits, language::ValueType type) {
        if (type == language::ValueType::f64) return ir_.CreateBitCast(bits, ir_.getDoubleTy());
        if (type == language::ValueType::boolean) return ir_.CreateTrunc(bits, ir_.getInt1Ty());
        auto* value = ir_.CreateTrunc(bits, ir_.getInt32Ty());
        return type == language::ValueType::f32 ? ir_.CreateBitCast(value, ir_.getFloatTy()) : value;
    }
    llvm::Value* default_math(language::ValueType type) {
        return MathLowering{ir_, *module_}.construct(math_type(type), {});
    }
    llvm::Value* math_binary(const language::Expression& node, llvm::Value* left, llvm::Value* right) {
        return MathLowering{ir_, *module_}.binary(node.operation, left, right,
            math_type(function_.expressions[node.left].type), math_type(function_.expressions[node.right].type));
    }
    llvm::Function* intrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> types = {}) {
        return llvm::Intrinsic::getOrInsertDeclaration(module_, id, types);
    }
    void suspend(bool final) {
        auto* resume = block(final ? "coroutine.final.unreachable" : "coroutine.continue");
        auto* state = ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_suspend),
            {llvm::ConstantTokenNone::get(*context_), ir_.getInt1(final)});
        auto* select = ir_.CreateSwitch(state, coroutine_return_, 2);
        select->addCase(ir_.getInt8(0), resume);
        select->addCase(ir_.getInt8(1), final ? coroutine_free_ : coroutine_cancel_);
        ir_.SetInsertPoint(resume);
        if (final) ir_.CreateUnreachable();
    }
    void complete(llvm::Value* status) {
        if (!function_.is_coroutine) { ir_.CreateRet(status); return; }
        ir_.CreateStore(status, entry_->getArg(4));
        ir_.CreateStore(ir_.getInt32(1), entry_->getArg(5));
        ir_.CreateBr(coroutine_final_);
    }
    void setup_coroutine() {
        coroutine_return_ = block("coroutine.return");
        coroutine_final_ = block("coroutine.final");
        coroutine_cancel_ = block("coroutine.cancel");
        coroutine_free_ = block("coroutine.free");
        auto* null = llvm::ConstantPointerNull::get(ir_.getPtrTy());
        coroutine_id_ = ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_id), {ir_.getInt32(0), null, null, null});
        auto* size = ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_size, {ir_.getInt64Ty()}));
        coroutine_alignment_ = ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_align, {ir_.getInt32Ty()}));
        auto allocate = module_->getOrInsertFunction("business_coroutine_allocate",
            llvm::FunctionType::get(ir_.getPtrTy(), {ir_.getInt64Ty(), ir_.getInt32Ty()}, false));
        trace_event(DiagnosticCall::coroutine_allocate);
        auto* memory = ir_.CreateCall(allocate, {size, coroutine_alignment_});
        auto* allocated = block("coroutine.allocated");
        auto* exhausted = block("coroutine.allocation_failed");
        ir_.CreateCondBr(ir_.CreateICmpNE(memory, null), allocated, exhausted);
        ir_.SetInsertPoint(exhausted);
        ir_.CreateRet(null);
        ir_.SetInsertPoint(allocated);
        coroutine_handle_ = ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_begin), {coroutine_id_, memory});
        prepare_debug_storage();
        suspend(false); // Creation owns a frame; the first resume enters the body.
    }
    void finish_coroutine() {
        ir_.SetInsertPoint(coroutine_final_);
        suspend(true);
        ir_.SetInsertPoint(coroutine_cancel_);
        clear_execution_resources();
        close_debug_frame();
        ir_.CreateBr(coroutine_free_);
        ir_.SetInsertPoint(coroutine_free_);
        auto* memory = ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_free), {coroutine_id_, coroutine_handle_});
        auto release = module_->getOrInsertFunction("business_coroutine_free",
            llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy(), ir_.getInt32Ty()}, false));
        trace_event(DiagnosticCall::coroutine_free);
        ir_.CreateCall(release, {memory, coroutine_alignment_});
        ir_.CreateBr(coroutine_return_);
        ir_.SetInsertPoint(coroutine_return_);
        ir_.CreateCall(intrinsic(llvm::Intrinsic::coro_end),
            {coroutine_handle_, ir_.getFalse(), llvm::ConstantTokenNone::get(*context_)});
        ir_.CreateRet(coroutine_handle_);
        for (const auto& [name, action] : {std::pair{"atelier_source_resume", llvm::Intrinsic::coro_resume},
                 std::pair{"atelier_source_destroy", llvm::Intrinsic::coro_destroy}}) {
            auto* wrapper = llvm::Function::Create(llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy()}, false),
                llvm::GlobalValue::ExternalLinkage, name, *module_);
            wrapper->addFnAttr(llvm::Attribute::NoUnwind);
            wrapper->setUWTableKind(llvm::UWTableKind::Async);
            ir_.SetInsertPoint(llvm::BasicBlock::Create(*context_, "entry", wrapper));
            ir_.CreateCall(intrinsic(action), {wrapper->getArg(0)});
            ir_.CreateRetVoid();
        }
    }
    static bool is_reference(language::ValueType type) noexcept {
        return type == language::ValueType::strong_reference || type == language::ValueType::weak_reference ||
            type == language::ValueType::null_reference;
    }
    std::uint32_t allocate_reference(language::ValueType type) {
        if (type == language::ValueType::strong_reference) {
            if (strong_slots_ >= observation_slot_bit)
                throw std::runtime_error("Business strong reference layout exceeds the slot encoding.");
            return strong_slots_++;
        }
        if (type == language::ValueType::weak_reference) {
            // The last tagged index is reserved for the null operand.
            if (weak_slots_ >= observation_slot_bit - 1)
                throw std::runtime_error("Business weak reference layout exceeds the slot encoding.");
            return observation_slot_bit | weak_slots_++;
        }
        throw std::logic_error("A null reference cannot own invocation storage.");
    }
    llvm::Type* value_type(language::ValueType value) {
        if (const auto type = math_type(value)) return llvm::FixedVectorType::get(
            type.precision == math::Precision::f64 ? ir_.getDoubleTy() : ir_.getFloatTy(),
            static_cast<unsigned>(type.components()));
        switch (value.kind) {
        case language::ValueType::f32: return ir_.getFloatTy();
        case language::ValueType::f64: return ir_.getDoubleTy();
        case language::ValueType::u32:
        case language::ValueType::i32:
        case language::ValueType::enumeration: return ir_.getInt32Ty();
        case language::ValueType::boolean: return ir_.getInt1Ty();
        case language::ValueType::void_value: return ir_.getVoidTy();
        case language::ValueType::record:
        case language::ValueType::array:
        case language::ValueType::string: return ir_.getPtrTy();
        case language::ValueType::strong_reference:
        case language::ValueType::weak_reference:
        case language::ValueType::null_reference: break;
        }
        throw std::logic_error("Reference values do not use LLVM value storage.");
    }
    llvm::BasicBlock* block(const char* name) { return llvm::BasicBlock::Create(*context_, name, entry_); }
    bool terminated() const { return ir_.GetInsertBlock()->getTerminatorOrNull() != nullptr; }
    void checked_call(llvm::FunctionCallee function, llvm::ArrayRef<llvm::Value*> arguments) {
        if (diagnostics_) trace_event(service_kind(function));
        auto* status = ir_.CreateCall(function, arguments);
        failure_edge(status, ir_.getInt32(static_cast<std::uint32_t>(EntryFailure::none)));
        auto* next = block("service.ok");
        ir_.CreateCondBr(ir_.CreateICmpEQ(status, ir_.getInt32(0)), next, failure_);
        ir_.SetInsertPoint(next);
    }
    void clear_slot(std::uint32_t slot) {
        trace_event(DiagnosticCall::root_clear);
        ir_.CreateCall(clear_, {invocation_, ir_.getInt32(slot)});
    }
    struct OwnedValue { language::ValueType type; llvm::Value* storage; llvm::AllocaInst* live; };
    llvm::AllocaInst* allocate_value_storage(language::ValueType type, const llvm::Twine& name) {
        const auto& layout = source_.value_layouts.at(type.type_index);
        auto* storage = ir_.CreateAlloca(llvm::ArrayType::get(ir_.getInt8Ty(), layout.size), nullptr, name);
        storage->setAlignment(llvm::Align(layout.alignment));
        return storage;
    }
    std::size_t allocate_owned(language::ValueType type, const llvm::Twine& name) {
        auto* storage = allocate_value_storage(type, name);
        auto* live = ir_.CreateAlloca(ir_.getInt1Ty(), nullptr, "value.live");
        ir_.CreateStore(ir_.getFalse(), live);
        owned_.push_back({type, storage, live});
        return owned_.size() - 1;
    }
    void construct_owned(std::size_t slot, llvm::Value* source = nullptr) {
        const auto& value = owned_.at(slot);
        checked_call(construct_value_, {invocation_, ir_.getInt32(value.type.type_index), value.storage,
            source ? source : llvm::ConstantPointerNull::get(ir_.getPtrTy())});
        ir_.CreateStore(ir_.getTrue(), value.live);
    }
    llvm::Value* temporary_value(std::size_t slot, llvm::Value* source = nullptr) {
        construct_owned(slot, source);
        value_temporaries_.push_back(slot);
        return owned_[slot].storage;
    }
    void destroy_owned(std::size_t slot) {
        const auto& value = owned_.at(slot);
        auto* destroy = block("value.destroy");
        auto* next = block("value.cleared");
        ir_.CreateCondBr(ir_.CreateLoad(ir_.getInt1Ty(), value.live), destroy, next);
        ir_.SetInsertPoint(destroy);
        ir_.CreateStore(ir_.getFalse(), value.live);
        trace_event(DiagnosticCall::value_destroy);
        ir_.CreateCall(destroy_value_, {invocation_, ir_.getInt32(value.type.type_index), value.storage});
        ir_.CreateBr(next);
        ir_.SetInsertPoint(next);
    }
    bool string_literal(language::NodeId id) const {
        return id != language::no_node && function_.expressions[id].kind == language::Expression::Kind::string_literal;
    }
    // Use the same decision for entry storage and lowering. A typed String
    // declaration/assignment or append can consume bytes without a temporary.
    language::NodeId direct_text_input(const language::Statement& node) const {
        using Kind = language::Statement::Kind;
        return (node.kind == Kind::declare || node.kind == Kind::assign) && string_literal(node.expression) ?
            node.expression : language::no_node;
    }
    language::NodeId direct_text_input(const language::Expression& node) const {
        return node.kind == language::Expression::Kind::sequence_mutation && node.mutation == SequenceMutation::append &&
            string_literal(node.right) ? node.right : language::no_node;
    }
    void write_text(language::ValueType type, llvm::Value* storage, std::uint32_t text, SequenceMutation operation) {
        const auto& bytes = function_.strings.at(text);
        auto* data = ir_.CreateGlobalString(llvm::StringRef(bytes.data(), bytes.size()));
        checked_call(mutate_sequence_, {invocation_, ir_.getInt32(type.type_index), ir_.getInt32(static_cast<std::uint32_t>(operation)),
            storage, data, ir_.getInt64(bytes.size())});
    }
    llvm::Value* input_address(language::NodeId id) {
        const auto type = function_.expressions[id].type;
        auto* value = expression(id);
        if (managed_value(type)) return value;
        llvm::IRBuilder<> entry_ir(&entry_->getEntryBlock(), entry_->getEntryBlock().begin());
        auto* storage = entry_ir.CreateAlloca(storage_type(type), nullptr, "value.temporary");
        storage->setAlignment(llvm::Align(storage_alignment(type)));
        store_value(value, storage, type);
        return storage;
    }
    void clear_user_values() {
        for (auto slot = owned_.size(); slot != 0;) destroy_owned(--slot);
        for (auto slot = weak_slots_; slot > first_weak_slot_;) clear_slot(observation_slot_bit | --slot);
        for (auto slot = strong_slots_; slot > first_strong_slot_;) clear_slot(--slot);
        if (debugging_) for (std::size_t i = 0; i < function_.locals.size(); ++i)
            debug_local(i, DebugAvailability::out_of_scope);
    }
    void clear_execution_resources() {
        clear_user_values();
        if (public_entry_) clear_slot(receiver_slot);
    }
    void clear_temporaries() {
        for (auto slot = temporaries_.rbegin(); slot != temporaries_.rend(); ++slot) clear_slot(*slot);
        temporaries_.clear();
        for (auto slot = value_temporaries_.rbegin(); slot != value_temporaries_.rend(); ++slot) destroy_owned(*slot);
        value_temporaries_.clear();
    }
    void copy_reference(std::uint32_t destination, llvm::Value* source) {
        checked_call(reference_, {invocation_, ir_.getInt32(static_cast<std::uint32_t>(ReferenceOperation::copy)),
            ir_.getInt32(destination), source});
    }
    llvm::Value* read_field(llvm::Value* slot, language::ValueType type, std::uint32_t field) {
        if (math_type(type)) {
            checked_call(read_value_, {invocation_, slot, ir_.getInt32(field), math_field_result_});
            return load_value(math_field_result_, type);
        }
        if (type == language::ValueType::f32) {
            checked_call(read_float_, {invocation_, slot, ir_.getInt32(field), float_result_});
            return ir_.CreateLoad(ir_.getFloatTy(), float_result_);
        }
        if (type == language::ValueType::f64) {
            checked_call(read_double_, {invocation_, slot, ir_.getInt32(field), double_result_});
            return ir_.CreateLoad(ir_.getDoubleTy(), double_result_);
        }
        if (type == language::ValueType::boolean) {
            checked_call(read_bool_, {invocation_, slot, ir_.getInt32(field), uint_result_});
            auto* value = ir_.CreateLoad(ir_.getInt32Ty(), uint_result_);
            check_value(ir_.CreateICmpULE(value, ir_.getInt32(1)), EntryFailure::invalid_call_result);
            return ir_.CreateTrunc(value, ir_.getInt1Ty());
        }
        if (type != language::ValueType::u32 && type != language::ValueType::i32 && type.kind != language::ValueType::enumeration) throw std::logic_error("Unsupported business field binding.");
        checked_call(read_uint_, {invocation_, slot, ir_.getInt32(field), uint_result_});
        return ir_.CreateLoad(ir_.getInt32Ty(), uint_result_);
    }
    void write_field(llvm::Value* slot, language::ValueType type, std::uint32_t field, llvm::Value* value) {
        if (math_type(type)) {
            store_value(value, math_field_result_, type);
            checked_call(write_value_, {invocation_, slot, ir_.getInt32(field), math_field_result_});
            return;
        }
        if (type == language::ValueType::f32) {
            checked_call(write_float_, {invocation_, slot, ir_.getInt32(field), value});
        } else if (type == language::ValueType::f64) {
            checked_call(write_double_, {invocation_, slot, ir_.getInt32(field), value});
        } else if (type == language::ValueType::boolean) {
            checked_call(write_bool_, {invocation_, slot, ir_.getInt32(field), ir_.CreateZExt(value, ir_.getInt32Ty())});
        } else {
            if (type != language::ValueType::u32 && type != language::ValueType::i32 && type.kind != language::ValueType::enumeration) throw std::logic_error("Unsupported business field binding.");
            checked_call(write_uint_, {invocation_, slot, ir_.getInt32(field), value});
        }
    }
    void safe_point() {
        if (protected_values_) return;
        if (public_entry_) { checked_call(poll_, {invocation_}); return; }
        auto* poll = block("safe_point.poll");
        auto* next = block("safe_point.continue");
        ir_.CreateCondBr(borrow_active_, next, poll);
        ir_.SetInsertPoint(poll);
        checked_call(poll_, {invocation_});
        ir_.CreateBr(next);
        ir_.SetInsertPoint(next);
    }
    void check_value(llvm::Value* valid, EntryFailure reason) {
        failure_edge(ir_.getInt32(1), ir_.getInt32(static_cast<std::uint32_t>(reason)));
        auto* next = block("borrow.valid");
        ir_.CreateCondBr(valid, next, failure_);
        ir_.SetInsertPoint(next);
    }
    struct Sequence { llvm::Value *data{}, *size{}; };
    Sequence sequence_view(llvm::Value* value, language::ValueType type) {
        const auto& layout = source_.value_layouts.at(type.type_index);
        // Entry allocas are reused by accesses in loops; projection is a trusted
        // leaf operation on the native container, with no business callbacks.
        llvm::IRBuilder<> entry_ir(&entry_->getEntryBlock(), entry_->getEntryBlock().begin());
        auto* data = entry_ir.CreateAlloca(ir_.getPtrTy(), nullptr, "sequence.data");
        auto* size = entry_ir.CreateAlloca(ir_.getInt64Ty(), nullptr, "sequence.size");
        auto* function_type = llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy(), ir_.getPtrTy(), ir_.getPtrTy()}, false);
        auto* address = llvm::ConstantExpr::getIntToPtr(ir_.getInt64(reinterpret_cast<std::uintptr_t>(layout.sequence_view)), ir_.getPtrTy());
        trace_event(DiagnosticCall::sequence_view);
        auto* call = ir_.CreateCall(function_type, address, {value, data, size});
        call->setDoesNotThrow();
        Sequence result{ir_.CreateLoad(ir_.getPtrTy(), data), ir_.CreateLoad(ir_.getInt64Ty(), size)};
        const auto stride = type.kind == language::ValueType::string ? 1 : storage_size(layout.element);
        const auto alignment = type.kind == language::ValueType::string ? 1 : storage_alignment(layout.element);
        auto* pointer = ir_.CreatePtrToInt(result.data, ir_.getInt64Ty());
        auto* live = ir_.CreateAnd(ir_.CreateICmpNE(pointer, ir_.getInt64(0)),
            ir_.CreateICmpEQ(ir_.CreateAnd(pointer, ir_.getInt64(alignment - 1)), ir_.getInt64(0)));
        auto* valid = ir_.CreateAnd(ir_.CreateICmpULE(result.size, ir_.getInt64(std::min<std::uint64_t>(UINT32_MAX, PTRDIFF_MAX / stride))),
            ir_.CreateOr(ir_.CreateICmpEQ(result.size, ir_.getInt64(0)), live));
        check_value(valid, EntryFailure::sequence_layout);
        return result;
    }
    std::size_t storage_size(language::ValueType type) const {
        return language::aggregate(type) ? source_.value_layouts.at(type.type_index).size :
            type == language::ValueType::boolean ? 1 : type == language::ValueType::f64 ? 8 : 4;
    }
    std::size_t storage_alignment(language::ValueType type) const {
        return language::aggregate(type) ? source_.value_layouts.at(type.type_index).alignment :
            type == language::ValueType::boolean ? 1 : type == language::ValueType::f64 ? 8 : 4;
    }
    Sequence sequence(language::NodeId id) {
        const auto& node = function_.expressions[id];
        if (node.kind == language::Expression::Kind::local && node.value < parameter_views_.size() && parameter_views_[node.value].data)
            return parameter_views_[node.value];
        return sequence_view(value_address(id), node.type);
    }
    llvm::Value* value_address(language::NodeId id) {
        const auto& node = function_.expressions[id];
        PositionScope position(position_, node.position);
        if (node.kind == language::Expression::Kind::local) return locals_.at(node.value);
        if (node.kind == language::Expression::Kind::string_literal) return expression(id);
        if (node.kind == language::Expression::Kind::value_field)
            return ir_.CreateInBoundsGEP(ir_.getInt8Ty(), value_address(node.left), ir_.getInt64(node.offset));
        if (node.kind == language::Expression::Kind::index) {
            const auto input = sequence(node.left);
            auto* index = expression(node.right);
            auto* offset = function_.expressions[node.right].type == language::ValueType::i32 ?
                ir_.CreateSExt(index, ir_.getInt64Ty()) : ir_.CreateZExt(index, ir_.getInt64Ty());
            check_value(ir_.CreateICmpULT(offset, input.size), EntryFailure::sequence_bounds);
            const auto stride = function_.expressions[node.left].type.kind == language::ValueType::string ? 1 : storage_size(node.type);
            return ir_.CreateInBoundsGEP(ir_.getInt8Ty(), input.data, ir_.CreateMul(offset, ir_.getInt64(stride)));
        }
        if (managed_value(node.type) && (node.kind == language::Expression::Kind::call ||
            node.kind == language::Expression::Kind::source_call))
            return expression(id);
        if (math_type(node.type)) return input_address(id);
        throw std::logic_error("Expression has no borrowed native storage.");
    }
    llvm::Value* expression(language::NodeId index) {
        const auto& node = function_.expressions[index];
        PositionScope position(position_, node.position);
        using Kind = language::Expression::Kind;
        switch (node.kind) {
        case Kind::scalar_cast: {
            auto* value = expression(node.left);
            const auto input = function_.expressions[node.left].type;
            if (input == node.type) return value;
            if (floating_type(node.type)) {
                if (floating_type(input)) return ir_.CreateFPCast(value, value_type(node.type));
                return input == language::ValueType::i32 ? ir_.CreateSIToFP(value, value_type(node.type)) :
                    ir_.CreateUIToFP(value, value_type(node.type));
            }
            const bool signed_result = node.type == language::ValueType::i32;
            if (floating_type(input)) {
                // Check the interval whose truncation is representable. Extend
                // f32 first so the lower int boundary is not rounded to INT_MIN.
                auto* wide = ir_.CreateFPCast(value, ir_.getDoubleTy());
                auto* lower = llvm::ConstantFP::get(ir_.getDoubleTy(), signed_result ? -2147483649.0 : -1.0);
                auto* upper = llvm::ConstantFP::get(ir_.getDoubleTy(), signed_result ? 2147483648.0 : 4294967296.0);
                check_value(ir_.CreateAnd(ir_.CreateFCmpOGT(wide, lower), ir_.CreateFCmpOLT(wide, upper)),
                    EntryFailure::integer_conversion);
                return signed_result ? ir_.CreateFPToSI(value, ir_.getInt32Ty()) : ir_.CreateFPToUI(value, ir_.getInt32Ty());
            }
            check_value(signed_result ? ir_.CreateICmpULE(value, ir_.getInt32(INT32_MAX)) :
                ir_.CreateICmpSGE(value, ir_.getInt32(0)), EntryFailure::integer_conversion);
            return value;
        }
        case Kind::math_construct:
        case Kind::math_intrinsic: {
            llvm::SmallVector<llvm::Value*, 16> arguments;
            for (const auto id : node.arguments) arguments.push_back(expression(id));
            MathLowering lowering{ir_, *module_};
            if (node.kind == Kind::math_construct) return lowering.construct(math_type(node.type), arguments);
            return lowering.operation(node.math_operation, arguments, math_type(node.type));
        }
        case Kind::literal:
            if (node.type == language::ValueType::f32)
                return llvm::ConstantFP::get(ir_.getFloatTy(), std::bit_cast<float>(static_cast<std::uint32_t>(node.bits)));
            if (node.type == language::ValueType::f64)
                return llvm::ConstantFP::get(ir_.getDoubleTy(), std::bit_cast<double>(node.bits));
            if (node.type == language::ValueType::boolean) return ir_.getInt1(node.bits != 0);
            return ir_.getInt32(static_cast<std::uint32_t>(node.bits));
        case Kind::string_literal: {
            auto* storage = temporary_value(expression_value_slots_.at(index));
            write_text(node.type, storage, node.value, SequenceMutation::assign_text);
            return storage;
        }
        case Kind::sequence_mutation: {
            const auto type = function_.expressions[node.left].type;
            auto* target = value_address(node.left);
            if (const auto text = direct_text_input(node); text != language::no_node) {
                write_text(type, target, function_.expressions[text].value, SequenceMutation::append_text);
                return nullptr;
            }
            llvm::Value* input = llvm::ConstantPointerNull::get(ir_.getPtrTy());
            llvm::Value* count = ir_.getInt64(0);
            if (node.mutation == SequenceMutation::reserve || node.mutation == SequenceMutation::resize) {
                auto* number = expression(node.right);
                if (function_.expressions[node.right].type == language::ValueType::i32)
                    check_value(ir_.CreateICmpSGE(number, ir_.getInt32(0)), EntryFailure::sequence_size);
                count = ir_.CreateZExt(number, ir_.getInt64Ty());
            } else if (node.right != language::no_node) input = input_address(node.right);
            checked_call(mutate_sequence_, {invocation_, ir_.getInt32(type.type_index),
                ir_.getInt32(static_cast<std::uint32_t>(node.mutation)), target, input, count});
            return nullptr;
        }
        case Kind::length: return ir_.CreateTrunc(sequence(node.left).size, ir_.getInt32Ty());
        case Kind::value_field:
        case Kind::index: {
            auto* address = value_address(index);
            if (managed_value(node.type)) return address;
            if (node.kind == Kind::index && function_.expressions[node.left].type.kind == language::ValueType::string)
                return ir_.CreateZExt(ir_.CreateAlignedLoad(ir_.getInt8Ty(), address, llvm::Align(1)), ir_.getInt32Ty());
            return load_value(address, node.type);
        }
        case Kind::local:
            if (managed_value(node.type)) return locals_[node.value];
            if (is_reference(node.type)) return ir_.getInt32(local_slots_[node.value]);
            return load_value(locals_[node.value], node.type);
        case Kind::receiver: return ir_.getInt32(receiver_slot);
        case Kind::null_reference: return ir_.getInt32(null_reference_slot);
        case Kind::create_reference: {
            const auto slot = temporary_slots_[index];
            checked_call(create_, {invocation_, ir_.getInt32(slot)});
            temporaries_.push_back(slot);
            return ir_.getInt32(slot);
        }
        case Kind::observe_reference:
        case Kind::acquire_reference: {
            auto* source = expression(node.left);
            const auto slot = temporary_slots_[index];
            const auto operation = node.kind == Kind::observe_reference ? ReferenceOperation::observe :
                ReferenceOperation::try_acquire;
            checked_call(reference_, {invocation_, ir_.getInt32(static_cast<std::uint32_t>(operation)),
                ir_.getInt32(slot), source});
            temporaries_.push_back(slot);
            return ir_.getInt32(slot);
        }
        case Kind::field: return read_field(expression(node.receiver), node.type, node.value);
        case Kind::source_call: {
            llvm::SmallVector<llvm::Value*, 8> arguments{invocation_, source_result_, source_failure_, borrow_active_};
            if (diagnostics_) arguments.push_back(trace_);
            for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                const auto id = node.arguments[i];
                const auto& argument = function_.expressions[id];
                if (node.argument_passing[i] == ParameterPassing::value) {
                    auto* value = expression(id);
                    if (managed_value(argument.type)) value = temporary_value(argument_value_slots_[index][i], value);
                    arguments.push_back(value);
                }
                else if (language::aggregate(argument.type)) arguments.push_back(value_address(id));
                else if (argument.kind == Kind::local || argument.kind == Kind::value_field ||
                    (argument.kind == Kind::index && function_.expressions[argument.left].type.kind != language::ValueType::string))
                    arguments.push_back(value_address(id));
                else arguments.push_back(input_address(id));
            }
            llvm::Value* result_storage = source_result_;
            // Plain source results are fully copied by the callee. Their
            // trivial lifetime needs no default construction or cleanup.
            if (managed_value(node.type)) result_storage = source_value_results_.at(index);
            arguments[1] = result_storage;
            ir_.CreateStore(ir_.getInt32(0), source_failure_);
            debug_location();
            trace_event(DiagnosticCall::source_call);
            auto* status = ir_.CreateCall(functions_.at(node.value), arguments);
            failure_edge(status, ir_.CreateLoad(ir_.getInt32Ty(), source_failure_));
            auto* next = block("function.ok");
            ir_.CreateCondBr(ir_.CreateICmpEQ(status, ir_.getInt32(0)), next, failure_);
            ir_.SetInsertPoint(next);
            trace_position();
            debug_stop(DebugPoint::call_return);
            safe_point();
            if (node.type == language::ValueType::void_value) return nullptr;
            if (managed_value(node.type)) return result_storage;
            return math_type(node.type) ? load_value(source_result_, node.type) :
                decode_bits(ir_.CreateLoad(ir_.getInt64Ty(), source_result_), node.type);
        }
        case Kind::call: {
            auto* owner = expression(node.receiver);
            // Nested calls reuse the same entry scratch. Finish every operand
            // left-to-right and retain its SSA value before packing this call.
            struct Operand { llvm::Value* value; ArgumentStorage storage; std::uint32_t field; };
            llvm::SmallVector<Operand, 8> values;
            for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                const auto id = node.arguments[i];
                const auto& argument = function_.expressions[id];
                const auto passing = node.argument_passing[i];
                if (passing != ParameterPassing::value && argument.kind == Kind::local)
                    values.push_back({locals_[argument.value], passing == ParameterPassing::reference ?
                        ArgumentStorage::reference : ArgumentStorage::read_only, 0});
                else if (passing != ParameterPassing::value && argument.kind == Kind::field)
                    values.push_back({expression(argument.receiver), ArgumentStorage::field, argument.value});
                else if (passing != ParameterPassing::value && (language::aggregate(argument.type) ||
                    argument.kind == Kind::value_field || (argument.kind == Kind::index &&
                    function_.expressions[argument.left].type.kind != language::ValueType::string)))
                    values.push_back({value_address(id),
                        passing == ParameterPassing::reference ? ArgumentStorage::reference : ArgumentStorage::read_only, 0});
                else {
                    auto* value = math_type(argument.type) ? input_address(id) : expression(id);
                    if (managed_value(argument.type)) value = temporary_value(argument_value_slots_[index][i], value);
                    values.push_back({value, ArgumentStorage::value, 0});
                }
            }
            for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                const auto type = function_.expressions[node.arguments[i]].type;
                const auto& operand = values[i];
                auto* storage = ir_.CreateInBoundsGEP(abi_value_type_, call_arguments_, ir_.getInt32(static_cast<std::uint32_t>(i)));
                const auto put = [&](unsigned member, llvm::Value* value) { ir_.CreateStore(value, ir_.CreateStructGEP(abi_value_type_, storage, member)); };
                const auto kind = business_type(type);
                put(0, ir_.getInt32(static_cast<std::uint32_t>(kind.kind)));
                put(1, ir_.getInt32(kind.type_index));
                llvm::Value* bits = ir_.getInt64(0);
                llvm::Value* data = llvm::ConstantPointerNull::get(ir_.getPtrTy());
                llvm::Value* slot = ir_.getInt32(0);
                if (operand.storage == ArgumentStorage::value) {
                    if (language::aggregate(type)) data = operand.value;
                    else bits = encode_bits(operand.value, type);
                }
                else if (operand.storage == ArgumentStorage::field) slot = operand.value;
                else data = operand.value;
                put(2, bits);
                put(3, ir_.getInt32(static_cast<std::uint32_t>(operand.storage)));
                put(4, data);
                put(5, slot);
                put(6, ir_.getInt32(operand.field));
            }
            llvm::Value* arguments = node.arguments.empty() ? llvm::ConstantPointerNull::get(ir_.getPtrTy()) :
                static_cast<llvm::Value*>(call_arguments_);
            llvm::Value* output = llvm::ConstantPointerNull::get(ir_.getPtrTy());
            llvm::Value* value_result = call_math_result_;
            if (managed_value(node.type)) value_result = temporary_value(expression_value_slots_.at(index));
            if (node.type != language::ValueType::void_value) {
                output = call_result_;
                // A successful callback that omits its value must not reuse a
                // previous nested call's result or expose uninitialized bits.
                ir_.CreateStore(llvm::Constant::getNullValue(abi_value_type_), output);
                if (language::aggregate(node.type)) ir_.CreateStore(value_result, ir_.CreateStructGEP(abi_value_type_, output, 4));
            }
            checked_call(call_, {invocation_, owner, ir_.getInt32(node.value), arguments,
                ir_.getInt32(static_cast<std::uint32_t>(node.arguments.size())), output});
            if (node.type == language::ValueType::void_value) return nullptr;
            auto* kind = ir_.CreateLoad(ir_.getInt32Ty(), ir_.CreateStructGEP(abi_value_type_, output, 0));
            auto* next = block("result.ok");
            failure_edge(ir_.getInt32(1), ir_.getInt32(static_cast<std::uint32_t>(EntryFailure::invalid_call_result)));
            const auto expected = business_type(node.type);
            auto* type_index = ir_.CreateLoad(ir_.getInt32Ty(), ir_.CreateStructGEP(abi_value_type_, output, 1));
            auto* storage_kind = ir_.CreateLoad(ir_.getInt32Ty(), ir_.CreateStructGEP(abi_value_type_, output, 3));
            auto* valid = ir_.CreateAnd(ir_.CreateICmpEQ(kind, ir_.getInt32(static_cast<std::uint32_t>(expected.kind))),
                ir_.CreateICmpEQ(type_index, ir_.getInt32(expected.type_index)));
            valid = ir_.CreateAnd(valid, ir_.CreateICmpEQ(storage_kind, ir_.getInt32(0)));
            if (language::aggregate(node.type)) {
                auto* data = ir_.CreateLoad(ir_.getPtrTy(), ir_.CreateStructGEP(abi_value_type_, output, 4));
                valid = ir_.CreateAnd(valid, ir_.CreateICmpEQ(data, value_result));
            } else if (node.type != language::ValueType::f64) {
                auto* bits = ir_.CreateLoad(ir_.getInt64Ty(), ir_.CreateStructGEP(abi_value_type_, output, 2));
                valid = ir_.CreateAnd(valid, ir_.CreateICmpULE(bits,
                    ir_.getInt64(node.type == language::ValueType::boolean ? 1 : UINT32_MAX)));
            }
            ir_.CreateCondBr(valid, next, failure_);
            ir_.SetInsertPoint(next);
            // Load now: later operands may invoke another method using this slot.
            if (managed_value(node.type)) return value_result;
            if (math_type(node.type)) return load_value(call_math_result_, node.type);
            return decode_bits(ir_.CreateLoad(ir_.getInt64Ty(), ir_.CreateStructGEP(abi_value_type_, output, 2)), node.type);
        }
        case Kind::unary: {
            auto* value = expression(node.left);
            if (node.operation == language::Operation::logical_not || node.operation == language::Operation::bitwise_not)
                return ir_.CreateNot(value);
            return node.operation == language::Operation::positive ? value : (floating_type(node.type) || math_type(node.type)) ? ir_.CreateFNeg(value) : ir_.CreateNeg(value);
        }
        case Kind::binary: break;
        }
        auto* left = expression(node.left);
        using Op = language::Operation;
        if (node.operation == Op::logical_and || node.operation == Op::logical_or) {
            auto* left_end = ir_.GetInsertBlock();
            auto* evaluate = block("logical.rhs");
            auto* merge = block("logical.end");
            if (node.operation == Op::logical_and) ir_.CreateCondBr(left, evaluate, merge);
            else ir_.CreateCondBr(left, merge, evaluate);
            ir_.SetInsertPoint(evaluate);
            auto* right = expression(node.right);
            auto* right_end = ir_.GetInsertBlock();
            ir_.CreateBr(merge);
            ir_.SetInsertPoint(merge);
            auto* result = ir_.CreatePHI(ir_.getInt1Ty(), 2, "logical.result");
            result->addIncoming(left, left_end);
            result->addIncoming(right, right_end);
            return result;
        }
        auto* right = expression(node.right);
        const auto left_type = function_.expressions[node.left].type;
        if (is_reference(left_type)) {
            if (node.operation != Op::equal && node.operation != Op::not_equal)
                throw std::logic_error("Invalid typed reference comparison.");
            checked_call(same_reference_, {invocation_, left, right, uint_result_});
            auto* same = ir_.CreateLoad(ir_.getInt32Ty(), uint_result_);
            return node.operation == Op::equal ? ir_.CreateICmpNE(same, ir_.getInt32(0)) :
                ir_.CreateICmpEQ(same, ir_.getInt32(0));
        }
        if (math_type(left_type) || math_type(function_.expressions[node.right].type))
            return math_binary(node, left, right);
        const bool floating = floating_type(left_type);
        switch (node.operation) {
        case Op::add: return floating ? ir_.CreateFAdd(left, right) : ir_.CreateAdd(left, right);
        case Op::subtract: return floating ? ir_.CreateFSub(left, right) : ir_.CreateSub(left, right);
        case Op::multiply: return floating ? ir_.CreateFMul(left, right) : ir_.CreateMul(left, right);
        case Op::divide:
        case Op::remainder: {
            if (floating) return ir_.CreateFDiv(left, right);
            auto* valid = ir_.CreateICmpNE(right, ir_.getInt32(0));
            const bool signed_integer = left_type == language::ValueType::i32;
            if (signed_integer) valid = ir_.CreateAnd(valid, ir_.CreateNot(ir_.CreateAnd(
                ir_.CreateICmpEQ(left, ir_.getInt32(0x80000000u)), ir_.CreateICmpEQ(right, ir_.getInt32(UINT32_MAX)))));
            check_value(valid, EntryFailure::integer_division);
            if (node.operation == Op::remainder) return signed_integer ? ir_.CreateSRem(left, right) : ir_.CreateURem(left, right);
            return signed_integer ? ir_.CreateSDiv(left, right) : ir_.CreateUDiv(left, right);
        }
        case Op::bitwise_and: return ir_.CreateAnd(left, right);
        case Op::bitwise_or: return ir_.CreateOr(left, right);
        case Op::bitwise_xor: return ir_.CreateXor(left, right);
        case Op::shift_left:
        case Op::shift_right:
            check_value(ir_.CreateICmpULT(right, ir_.getInt32(32)), EntryFailure::integer_shift);
            if (node.operation == Op::shift_left) return ir_.CreateShl(left, right);
            return left_type == language::ValueType::i32 ? ir_.CreateAShr(left, right) : ir_.CreateLShr(left, right);
        case Op::equal: return floating ? ir_.CreateFCmpOEQ(left, right) : ir_.CreateICmpEQ(left, right);
        case Op::not_equal: return floating ? ir_.CreateFCmpUNE(left, right) : ir_.CreateICmpNE(left, right);
        case Op::less: return floating ? ir_.CreateFCmpOLT(left, right) : left_type == language::ValueType::i32 ? ir_.CreateICmpSLT(left, right) : ir_.CreateICmpULT(left, right);
        case Op::less_equal: return floating ? ir_.CreateFCmpOLE(left, right) : left_type == language::ValueType::i32 ? ir_.CreateICmpSLE(left, right) : ir_.CreateICmpULE(left, right);
        case Op::greater: return floating ? ir_.CreateFCmpOGT(left, right) : left_type == language::ValueType::i32 ? ir_.CreateICmpSGT(left, right) : ir_.CreateICmpUGT(left, right);
        case Op::greater_equal: return floating ? ir_.CreateFCmpOGE(left, right) : left_type == language::ValueType::i32 ? ir_.CreateICmpSGE(left, right) : ir_.CreateICmpUGE(left, right);
        case Op::positive:
        case Op::negate:
        case Op::logical_not:
        case Op::bitwise_not:
        case Op::logical_and:
        case Op::logical_or: break;
        }
        throw std::logic_error("Invalid typed binary operation.");
    }
    void clear_scope(const language::Statement& scope) {
        for (auto local = scope.locals.rbegin(); local != scope.locals.rend(); ++local) {
            if (is_reference(function_.locals[*local].type)) clear_slot(local_slots_[*local]);
            else if (owned_local_slots_[*local] != language::no_node) destroy_owned(owned_local_slots_[*local]);
            debug_local(*local, DebugAvailability::out_of_scope);
        }
    }
    void statement(language::NodeId index) {
        const auto& node = function_.statements[index];
        PositionScope position(position_, node.position);
        using Kind = language::Statement::Kind;
        if (node.kind != Kind::block && node.kind != Kind::loop) debug_stop(DebugPoint::statement);
        switch (node.kind) {
        case Kind::block:
            scopes_.push_back(index);
            for (const auto local : node.locals) debug_local(local, DebugAvailability::uninitialized);
            for (const auto child : node.children) {
                if (terminated()) break;
                statement(child);
            }
            if (!terminated()) clear_scope(node);
            scopes_.pop_back();
            return;
        case Kind::declare: {
            if (owned_local_slots_[node.target] != language::no_node) {
                const auto slot = owned_local_slots_[node.target];
                if (const auto text = direct_text_input(node); text != language::no_node) {
                    construct_owned(slot);
                    write_text(owned_[slot].type, owned_[slot].storage, function_.expressions[text].value, SequenceMutation::assign_text);
                } else construct_owned(slot, node.expression == language::no_node ? nullptr : expression(node.expression));
                clear_temporaries();
                debug_local(node.target, DebugAvailability::available);
                return;
            }
            const auto type = function_.locals[node.target].type;
            auto* value = node.expression == language::no_node ? default_math(type) : expression(node.expression);
            if (is_reference(type)) copy_reference(local_slots_[node.target], value);
            else store_value(value, locals_[node.target], type);
            clear_temporaries();
            debug_local(node.target, DebugAvailability::available);
            return;
        }
        case Kind::assign: {
            const auto& target = function_.expressions[node.target];
            // Evaluate a field's owner before its RHS and keep any owner
            // temporary rooted through the store, even if the RHS calls out.
            auto* owner = target.kind == language::Expression::Kind::field ? expression(target.receiver) : nullptr;
            const bool direct = target.kind == language::Expression::Kind::value_field || target.kind == language::Expression::Kind::index;
            auto* address = direct ? value_address(node.target) : nullptr;
            if (managed_value(target.type)) {
                if (const auto text = direct_text_input(node); text != language::no_node)
                    write_text(target.type, direct ? address : locals_[target.value], function_.expressions[text].value, SequenceMutation::assign_text);
                else checked_call(assign_value_, {invocation_, ir_.getInt32(target.type.type_index),
                    direct ? address : locals_[target.value], expression(node.expression)});
                clear_temporaries();
                return;
            }
            auto* value = expression(node.expression);
            if (target.kind == language::Expression::Kind::local) {
                if (is_reference(target.type)) copy_reference(local_slots_[target.value], value);
                else store_value(value, locals_[target.value], target.type);
            } else if (direct) store_value(value, address, target.type);
            else write_field(owner, target.type, target.value, value);
            clear_temporaries();
            return;
        }
        case Kind::call:
            (void)expression(node.expression);
            clear_temporaries();
            return;
        case Kind::yield:
            trace_position(offsetof(BusinessTrace, suspended_location));
            suspend(false);
            safe_point();
            return;
        case Kind::return_value:
            if (node.expression != language::no_node) {
                auto* value = expression(node.expression);
                if (managed_value(function_.result)) {
                    const auto& layout = source_.value_layouts.at(function_.result.type_index);
                    ir_.CreateMemCpy(result_output_, llvm::Align(layout.alignment), value,
                        llvm::Align(layout.alignment), layout.size);
                } else if (math_type(function_.result)) store_value(value, result_output_, function_.result);
                else ir_.CreateStore(encode_bits(value, function_.result), result_output_);
            }
            clear_temporaries();
            // Public result storage stays untouched until shared cleanup and
            // the host finish policy have both succeeded.
            ir_.CreateBr(finish_);
            return;
        case Kind::condition: {
            auto* condition = expression(node.expression);
            clear_temporaries();
            auto* yes = block("if.then");
            auto* no = block("if.else");
            auto* merge = block("if.end");
            ir_.CreateCondBr(condition, yes, no);
            ir_.SetInsertPoint(yes);
            statement(node.body);
            if (!terminated()) ir_.CreateBr(merge);
            ir_.SetInsertPoint(no);
            if (node.alternative != language::no_node) statement(node.alternative);
            if (!terminated()) ir_.CreateBr(merge);
            ir_.SetInsertPoint(merge);
            return;
        }
        case Kind::loop: {
            auto* condition = block("loop.condition");
            auto* body = block("loop.body");
            auto* step = block("loop.step");
            auto* end = block("loop.end");
            ir_.CreateBr(condition);
            ir_.SetInsertPoint(condition);
            debug_stop(DebugPoint::statement);
            auto* value = expression(node.expression);
            clear_temporaries();
            ir_.CreateCondBr(value, body, end);
            ir_.SetInsertPoint(body);
            loops_.push_back({end, step, scopes_.size()});
            statement(node.body);
            loops_.pop_back();
            if (!terminated()) ir_.CreateBr(step);
            ir_.SetInsertPoint(step);
            if (node.step != language::no_node) statement(node.step);
            safe_point();
            ir_.CreateBr(condition);
            ir_.SetInsertPoint(end);
            return;
        }
        case Kind::break_loop:
        case Kind::continue_loop: {
            if (loops_.empty()) throw std::logic_error("Loop transfer has no enclosing loop.");
            const auto target = loops_.back();
            clear_temporaries();
            // The for initializer lives in its enclosing block. Only the body
            // scopes being left end here; continue reaches step before polling.
            for (auto scope = scopes_.size(); scope > target.scope_depth;)
                clear_scope(function_.statements[scopes_[--scope]]);
            ir_.CreateBr(node.kind == Kind::break_loop ? target.end : target.step);
            return;
        }
        }
        throw std::logic_error("Invalid typed statement.");
    }

    const language::Function& function_;
    const language::Program& source_;
    const std::vector<llvm::Function*>& functions_;
    llvm::LLVMContext* context_;
    llvm::Module* module_;
    llvm::IRBuilder<> ir_;
    llvm::Function* entry_{};
    llvm::Value *invocation_{}, *result_output_{}, *failure_output_{};
    llvm::BasicBlock* failure_{};
    llvm::BasicBlock* finish_{};
    llvm::PHINode* failure_status_{};
    llvm::PHINode* failure_reason_{};
    llvm::BasicBlock *coroutine_return_{}, *coroutine_final_{}, *coroutine_cancel_{}, *coroutine_free_{};
    llvm::Value *coroutine_id_{}, *coroutine_handle_{}, *coroutine_alignment_{};
    llvm::StructType* abi_value_type_{};
    llvm::AllocaInst* call_arguments_{};
    llvm::AllocaInst* call_result_{};
    llvm::AllocaInst* float_result_{};
    llvm::AllocaInst *double_result_{}, *source_result_{}, *call_math_result_{}, *math_field_result_{};
    llvm::AllocaInst* uint_result_{};
    llvm::AllocaInst* source_failure_{};
    std::vector<llvm::Value*> locals_;
    std::vector<llvm::AllocaInst*> source_value_results_;
    std::vector<OwnedValue> owned_;
    std::vector<std::size_t> owned_local_slots_, expression_value_slots_, value_temporaries_;
    std::vector<std::vector<std::size_t>> argument_value_slots_;
    std::vector<Sequence> parameter_views_;
    std::vector<std::uint32_t> local_slots_;
    std::vector<std::uint32_t> temporary_slots_;
    std::vector<std::uint32_t> temporaries_;
    struct LoopTarget { llvm::BasicBlock *end, *step; std::size_t scope_depth; };
    std::vector<LoopTarget> loops_;
    std::vector<language::NodeId> scopes_;
    std::uint32_t strong_slots_{business_root_slots};
    std::uint32_t weak_slots_{};
    const std::uint32_t first_strong_slot_, first_weak_slot_;
    const bool public_entry_, protected_values_;
    DiagnosticCatalog *diagnostics_{}, *catalog_{};
    bool debugging_{};
    std::uint32_t function_index_{};
    language::SourcePosition position_;
    std::map<std::size_t, std::uint32_t> location_ids_;
    llvm::Value* trace_{};
    llvm::PHINode* failure_location_{};
    llvm::Value* borrow_active_{};
    std::uint32_t max_call_arguments_{};
    bool has_call_result_{}, has_source_calls_{};
    llvm::FunctionCallee begin_, create_, read_float_, write_float_, read_double_, write_double_, read_value_, write_value_, read_uint_, write_uint_;
    llvm::FunctionCallee read_bool_, write_bool_;
    llvm::FunctionCallee call_, poll_, finish_call_, clear_, reference_, same_reference_;
    llvm::FunctionCallee construct_value_, assign_value_, destroy_value_, mutate_sequence_;
    llvm::FunctionCallee debug_enter_, debug_point_, debug_leave_;
    llvm::StructType *debug_frame_type_{}, *debug_local_type_{};
    llvm::AllocaInst *debug_frame_{}, *debug_locals_{};
};

llvm::orc::ThreadSafeModule generate_business(LLJIT& jit, const language::Program& program,
    BusinessFrameLayout& layout, DiagnosticCatalog* catalog, CompileOptions options, Artifacts* artifacts) {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(program.source_name, *context);
    module->setSourceFileName(diagnostic_source_name(program.source_name));
    module->setTargetTriple(jit.getTargetTriple());
    module->setDataLayout(jit.getDataLayout());
    auto* ptr = llvm::PointerType::getUnqual(*context);
    auto* i32 = llvm::Type::getInt32Ty(*context);
    std::vector<llvm::Function*> functions(program.functions.size());
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (!program.reachable[i]) continue;
        const bool entry = program.exported(static_cast<std::uint32_t>(i));
        const bool coroutine = program.functions[i].is_coroutine;
        llvm::SmallVector<llvm::Type*, 8> parameters;
        if (entry) {
            parameters.assign(coroutine ? 6 : 4, ptr);
            if (options.diagnostics) parameters.push_back(ptr);
        }
        else {
            parameters.assign(3, ptr); // invocation, scalar result bits, failure reason
            parameters.push_back(llvm::Type::getInt1Ty(*context)); // caller's active borrow scope
            if (options.diagnostics) parameters.push_back(ptr);
            for (const auto& parameter : program.functions[i].parameters) {
                if (parameter.passing != ParameterPassing::value) parameters.push_back(ptr);
                else if (parameter.type.kind == language::ValueType::record) {
                    const auto math = program.value_layouts.at(parameter.type.type_index).math;
                    parameters.push_back(math ? static_cast<llvm::Type*>(MathLowering::value_type(*context, math)) : ptr);
                }
                else if (parameter.type == language::ValueType::f32) parameters.push_back(llvm::Type::getFloatTy(*context));
                else if (parameter.type == language::ValueType::f64) parameters.push_back(llvm::Type::getDoubleTy(*context));
                else if (parameter.type == language::ValueType::boolean) parameters.push_back(llvm::Type::getInt1Ty(*context));
                else parameters.push_back(i32);
            }
        }
        auto* type = llvm::FunctionType::get(coroutine ? static_cast<llvm::Type*>(ptr) : i32, parameters, false);
        auto* function = llvm::Function::Create(type, entry ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage,
            entry ? (program.exports.empty() ? "atelier_source_entry" : "atelier_source_entry_" + std::to_string(i))
                : "atelier_source_function_" + std::to_string(i), *module);
        if (entry || options.debugging) function->addFnAttr(llvm::Attribute::NoInline);
        function->addFnAttr(llvm::Attribute::NoUnwind);
        function->addFnAttr("disable-tail-calls", "true");
        function->setUWTableKind(llvm::UWTableKind::Async);
        if (coroutine) function->addFnAttr(llvm::Attribute::PresplitCoroutine);
        functions[i] = function;
    }
    layout = {business_root_slots, 0, nullptr, 0};
    for (std::uint32_t i = 0; i < functions.size(); ++i) {
        if (!program.reachable[i]) continue;
        SourceLowering lowering(*context, *module, program, i, functions, layout.strong_slots, layout.weak_slots, catalog, options);
        const auto demand = lowering.layout();
        layout.strong_slots = demand.strong_slots;
        layout.weak_slots = demand.weak_slots;
        layout.max_call_arguments = std::max(layout.max_call_arguments, demand.max_call_arguments);
        lowering.generate();
    }
    if (catalog) {
        auto& sites = catalog->debug_sites;
        std::sort(sites.begin(), sites.end());
        sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
    }
    std::string verification;
    llvm::raw_string_ostream stream(verification);
    if (llvm::verifyModule(*module, &stream)) throw std::runtime_error(verification);
    if (artifacts) {
        llvm::raw_string_ostream original(artifacts->original_ir);
        module->print(original, nullptr);
    }
    {
        llvm::LoopAnalysisManager loops;
        llvm::FunctionAnalysisManager functions;
        llvm::CGSCCAnalysisManager graph;
        llvm::ModuleAnalysisManager modules;
        auto target = take(llvm::orc::JITTargetMachineBuilder::detectHost());
        auto machine = take(target.createTargetMachine());
        llvm::PassBuilder passes(machine.get());
        passes.registerModuleAnalyses(modules);
        passes.registerCGSCCAnalyses(graph);
        passes.registerFunctionAnalyses(functions);
        passes.registerLoopAnalyses(loops);
        passes.crossRegisterProxies(loops, functions, graph, modules);
        llvm::ModulePassManager pipeline;
        if (program.functions[program.entry].is_coroutine) {
            pipeline.addPass(llvm::CoroEarlyPass());
            pipeline.addPass(llvm::createModuleToPostOrderCGSCCPassAdaptor(llvm::CoroSplitPass()));
            pipeline.addPass(llvm::CoroCleanupPass());
        }
        // Expose component operations before loop vectorization. Keeping a
        // three-lane vector per element can prevent packing several elements
        // into the target's native SIMD width. O2 chooses the final grouping.
        pipeline.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::ScalarizerPass{}));
        // Optimize the complete typed module. Ordinary FP operations retain
        // their NaN/Inf and rounding semantics; no fast-math flags are added.
        pipeline.addPass(passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2));
        pipeline.run(*module, modules);
        verification.clear();
        if (llvm::verifyModule(*module, &stream)) throw std::runtime_error(verification);
    }
    if (artifacts) {
        llvm::raw_string_ostream optimized(artifacts->optimized_ir);
        module->print(optimized, nullptr);
    }
    return {std::move(module), std::move(context)};
}

std::shared_ptr<CompiledBusiness> compile(const BusinessServices& services, const language::Program& program,
    language::Environment environment, CompileOptions options
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
    , bool omit_poll_for_verification = false
#endif
    , const std::shared_ptr<CompiledBusiness>& shared = {}
) {
    const auto& entry = program.functions.at(program.entry);
    auto state = std::make_shared<CompiledBusiness>();
    state->function_name = entry.function_name;
    state->source_name = program.source_name;
    state->services = services;
    state->options = options;
    state->object_type_name = program.object_type_name;
    state->parameters.reserve(entry.parameters.size());
    for (const auto parameter : entry.parameters) state->parameters.push_back({business_type(parameter.type), parameter.passing});
    state->signature = {state->parameters.empty() ? nullptr : state->parameters.data(),
        static_cast<std::uint32_t>(state->parameters.size()), business_type(entry.result), entry.is_coroutine};
    state->field_names.reserve(environment.fields.size());
    state->fields.reserve(environment.fields.size());
    for (const auto& field : environment.fields) {
        state->field_names.emplace_back(field.name);
        state->fields.push_back({state->field_names.back().c_str(), business_type(field.type)});
    }
    state->method_names.reserve(environment.methods.size());
    state->method_parameters.reserve(environment.methods.size());
    state->methods.reserve(environment.methods.size());
    for (const auto& method : environment.methods) {
        state->method_names.emplace_back(method.name);
        auto& parameters = state->method_parameters.emplace_back();
        parameters.reserve(method.parameters.size());
        for (const auto parameter : method.parameters) parameters.push_back({business_type(parameter.type), parameter.passing});
        state->methods.push_back({state->method_names.back().c_str(), parameters.empty() ? nullptr : parameters.data(),
            static_cast<std::uint32_t>(parameters.size()), business_type(method.result), method.receiver_read_only, method.preserves_borrows});
    }
    state->enum_names.reserve(environment.enumerations.size());
    state->enum_item_names.reserve(environment.enumerations.size());
    state->enum_items.reserve(environment.enumerations.size());
    state->enumerations.reserve(environment.enumerations.size());
    for (const auto& enumeration : environment.enumerations) {
        state->enum_names.emplace_back(enumeration.name);
        auto& names = state->enum_item_names.emplace_back();
        auto& items = state->enum_items.emplace_back();
        names.reserve(enumeration.values.size());
        items.reserve(enumeration.values.size());
        for (const auto& value : enumeration.values) {
            names.emplace_back(value.name);
            items.push_back({names.back().c_str(), value.bits});
        }
        state->enumerations.push_back({state->enum_names.back().c_str(), business_type(enumeration.underlying).kind,
            items.data(), static_cast<std::uint32_t>(items.size())});
    }
    state->value_names.reserve(environment.values.size());
    state->value_field_names.reserve(environment.values.size());
    state->value_fields.reserve(environment.values.size());
    state->values.reserve(environment.values.size());
    for (const auto& value : environment.values) {
        state->value_names.push_back(value.name);
        auto& names = state->value_field_names.emplace_back();
        auto& fields = state->value_fields.emplace_back();
        names.reserve(value.fields.size()); fields.reserve(value.fields.size());
        for (const auto& field : value.fields) {
            names.push_back(field.name);
            fields.push_back({names.back().c_str(), business_type(field.type), field.offset});
        }
        state->values.push_back({state->value_names.back().c_str(), business_type({value.kind}).kind, value.size, value.alignment,
            business_type(value.element), fields.data(), static_cast<std::uint32_t>(fields.size()), value.sequence_view, value.can_own, value.can_mutate, value.math, value.plain_value});
    }
    state->environment = {state->fields.empty() ? nullptr : state->fields.data(),
        static_cast<std::uint32_t>(state->fields.size()), state->methods.empty() ? nullptr : state->methods.data(),
        static_cast<std::uint32_t>(state->methods.size()), state->object_type_name.c_str(),
        state->enumerations.data(), static_cast<std::uint32_t>(state->enumerations.size()),
        state->values.data(), static_cast<std::uint32_t>(state->values.size())};
    state->requires_receiver = entry.requires_receiver;
    if (shared) {
        state->jit = shared->jit; state->layout = shared->layout; state->diagnostic = shared->diagnostic;
        state->diagnostics = shared->diagnostics; state->artifacts = shared->artifacts;
    } else {
        if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter())
            throw std::runtime_error("LLVM native target initialization failed.");
        auto target = take(llvm::orc::JITTargetMachineBuilder::detectHost());
        target.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
        target.setCodeModel(llvm::CodeModel::Large);
        target.setRelocationModel(llvm::Reloc::PIC_);
        if (options.diagnostics || options.debugging) {
            state->diagnostics = std::make_shared<DiagnosticCatalog>();
            state->diagnostics->functions.reserve(program.functions.size());
            for (const auto& function : program.functions)
                state->diagnostics->functions.push_back(function.function_name);
        }
        if (options.debugging) {
            auto& catalog = *state->diagnostics;
            catalog.variable_names.resize(program.functions.size());
            catalog.variables.resize(program.functions.size());
            catalog.debug_functions.resize(program.functions.size());
            for (std::size_t i = 0; i < program.functions.size(); ++i) {
                const auto& function = program.functions[i];
                auto& names = catalog.variable_names[i];
                auto& variables = catalog.variables[i];
                names.reserve(function.locals.size());
                variables.reserve(function.locals.size());
                for (std::size_t local = 0; local < function.locals.size(); ++local) {
                    const auto& value = function.locals[local];
                    names.push_back(value.name);
                    const auto storage = value.type == language::ValueType::strong_reference ? DebugStorage::strong_reference :
                        value.type == language::ValueType::weak_reference ? DebugStorage::weak_reference : DebugStorage::value;
                    variables.push_back({names.back().c_str(), storage == DebugStorage::value ? business_type(value.type) :
                        BusinessType{ValueKind::void_value}, storage,
                        local < function.parameters.size() ? function.parameters[local].passing : ParameterPassing::value});
                }
                catalog.debug_functions[i] = {variables.data(), static_cast<std::uint32_t>(variables.size())};
            }
        }
        if (options.artifacts) {
            state->artifacts = std::make_shared<Artifacts>();
            state->artifacts->target_triple = target.getTargetTriple().str();
            state->artifacts->cpu_features = target.getFeatures().getString();
        }
        llvm::orc::LLJITBuilder builder;
        builder.setJITTargetMachineBuilder(std::move(target));
        builder.setObjectLinkingLayerCreator(
            [](llvm::orc::ExecutionSession& session, llvm::jitlink::JITLinkMemoryManager& memory)
                -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
                auto layer = std::make_unique<llvm::orc::ObjectLinkingLayer>(session, memory);
                layer->addPlugin(atelier::jit::make_windows_unwind_plugin());
                return layer;
            });
        builder.setPlatformSetUp(llvm::orc::setUpInactivePlatform);
        state->jit = take(builder.create());
        if (state->artifacts) state->jit->getObjTransformLayer().setTransform(
            [artifacts = state->artifacts](std::unique_ptr<llvm::MemoryBuffer> object)
                -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
                try {
                    const auto bytes = object->getBuffer();
                    const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
                    artifacts->object_code.assign(begin, begin + bytes.size());
                    artifacts->assembly = disassemble_object(object->getMemBufferRef());
                    return object;
                } catch (const std::exception& error) {
                    return llvm::make_error<llvm::StringError>(error.what(), llvm::inconvertibleErrorCode());
                }
            });
        state->jit->getExecutionSession().setErrorReporter([diagnostic = state->diagnostic](llvm::Error error) {
            *diagnostic = llvm::toString(std::move(error));
        });
        llvm::orc::SymbolMap symbols;
        const auto bind = [&](const char* name, auto address) {
            symbols[state->jit->mangleAndIntern(name)] =
                llvm::orc::ExecutorSymbolDef(ExecutorAddr::fromPtr(address), llvm::JITSymbolFlags::Exported);
        };
        bind("business_begin", state->services.begin);
        bind("business_create", state->services.create);
        bind("business_read_float", state->services.read_float);
        bind("business_write_float", state->services.write_float);
        if (services.read_double) bind("business_read_double", services.read_double);
        if (services.write_double) bind("business_write_double", services.write_double);
        if (services.read_value) bind("business_read_value", services.read_value);
        if (services.write_value) bind("business_write_value", services.write_value);
        if (services.read_bool) bind("business_read_bool", services.read_bool);
        if (services.write_bool) bind("business_write_bool", services.write_bool);
        bind("business_read_uint", state->services.read_uint);
        bind("business_write_uint", state->services.write_uint);
        bind("business_call", state->services.call);
    #if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
        if (!omit_poll_for_verification)
    #endif
            bind("business_poll", state->services.poll);
        bind("business_finish", state->services.finish);
        bind("business_clear", state->services.clear);
        bind("business_reference", state->services.reference);
        bind("business_same_reference", state->services.same_reference);
        if (services.construct_value) bind("business_construct_value", services.construct_value);
        if (services.assign_value) bind("business_assign_value", services.assign_value);
        if (services.destroy_value) bind("business_destroy_value", services.destroy_value);
        if (services.mutate_sequence) bind("business_mutate_sequence", services.mutate_sequence);
        if (options.debugging) {
            bind("business_debug_enter", services.debug_enter);
            bind("business_debug_point", services.debug_point);
            bind("business_debug_leave", services.debug_leave);
        }
        if (entry.is_coroutine) {
            bind("business_coroutine_allocate", &allocate_coroutine);
            bind("business_coroutine_free", &free_coroutine);
        }
        check(state->jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))));
        check(state->jit->addIRModule(generate_business(*state->jit, program, state->layout,
            state->diagnostics.get(), options, state->artifacts.get())));
    }
    state->layout.object_type_name = state->object_type_name.c_str();
    state->entry_symbol = program.exports.empty() ? "atelier_source_entry" : "atelier_source_entry_" + std::to_string(program.entry);
    auto address = state->jit->lookup(state->entry_symbol);
    if (!address) {
        auto diagnostic = llvm::toString(address.takeError());
        if (!state->diagnostic->empty()) diagnostic += "\n" + *state->diagnostic;
        throw std::runtime_error(diagnostic_source_name(program.source_name) + ": JIT materialization: " + diagnostic);
    }
    if (entry.is_coroutine) {
        if (options.diagnostics) state->diagnostic_coroutine_entry = address->toPtr<DiagnosticCoroutineEntry>();
        else state->coroutine_entry = address->toPtr<CoroutineEntry>();
        state->resume = take(state->jit->lookup("atelier_source_resume")).toPtr<CoroutineAction>();
        state->destroy = take(state->jit->lookup("atelier_source_destroy")).toPtr<CoroutineAction>();
    } else if (options.diagnostics) state->diagnostic_entry = address->toPtr<DiagnosticEntry>();
    else state->entry = address->toPtr<Entry>();
    return state;
}
}

struct BusinessCode { std::shared_ptr<CompiledBusiness> state; };
struct BusinessCoroutineState {
    std::shared_ptr<CompiledBusiness> code;
    std::vector<BusinessValue> arguments;
    void* handle{};
    std::uint64_t bits{};
    std::uint32_t status{}, completed{};
    EntryFailure failure{EntryFailure::none};
    bool running{};
    ~BusinessCoroutineState() { if (handle) code->destroy(handle); }
};
struct BusinessCoroutine { std::shared_ptr<BusinessCoroutineState> state; };
}

namespace {
std::uint32_t compile_business_impl(const atelier::source::detail::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::source::detail::BusinessEnvironment* environment,
    atelier::source::detail::BusinessCode** output,
    char* error, std::uint32_t error_capacity, const char* entry_name, atelier::source::CompileOptions options
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
    , bool omit_poll_for_verification = false
#endif
    , const char* const* entries = nullptr, std::uint32_t entry_count = 0, bool receiver = false,
    atelier::source::detail::CompileLocation* location = nullptr, bool declarations_only = false
) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    try {
        if (!services || !source_name || !source || (!output && !declarations_only) || !services->begin || !services->create ||
            !services->read_float || !services->write_float || !services->read_uint || !services->write_uint ||
            !services->call || !services->poll || !services->finish || !services->clear ||
            !services->reference || !services->same_reference)
            throw std::runtime_error("Invalid business compiler input, callbacks or output storage.");
        if (options.debugging && (!services->debug_enter || !services->debug_point || !services->debug_leave))
            throw std::runtime_error("Source debugging requires its frame and stop services.");
        using namespace atelier;
        using namespace atelier::source::detail;
        if (entry_count && !entries) throw std::runtime_error("Missing source export names.");
        for (std::uint32_t i = 0; i < entry_count; ++i)
            if (!entries[i] || !*entries[i]) throw std::runtime_error("Source export names must not be empty.");
        std::vector<language::FieldBinding> fields;
        std::vector<std::vector<language::ParameterType>> method_parameters;
        std::vector<language::MethodBinding> methods;
        std::vector<std::vector<language::EnumItem>> enum_items;
        std::vector<language::EnumBinding> enumerations;
        std::vector<language::ValueLayout> values;
        if (environment) {
            if ((environment->field_count != 0 && !environment->fields) ||
                (environment->method_count != 0 && !environment->methods))
                throw std::runtime_error("Host binding count requires catalog storage.");
            if (environment->enumeration_count && !environment->enumerations) throw std::runtime_error("Missing enum catalog.");
            enum_items.reserve(environment->enumeration_count);
            enumerations.reserve(environment->enumeration_count);
            for (std::uint32_t i = 0; i < environment->enumeration_count; ++i) {
                const auto& enumeration = environment->enumerations[i];
                if (!enumeration.name || (enumeration.value_count && !enumeration.values)) throw std::runtime_error("Invalid enum catalog.");
                auto& items = enum_items.emplace_back();
                for (std::uint32_t j = 0; j < enumeration.value_count; ++j) {
                    if (!enumeration.values[j].name) throw std::runtime_error("Invalid enum item name.");
                    items.push_back({enumeration.values[j].name, enumeration.values[j].bits});
                }
                enumerations.push_back({enumeration.name, source_type(enumeration.underlying), items});
            }
            if (environment->value_count && !environment->values) throw std::runtime_error("Missing native value catalog.");
            values.reserve(environment->value_count);
            for (std::uint32_t i = 0; i < environment->value_count; ++i) {
                const auto& value = environment->values[i];
                if (!value.name || (value.field_count && !value.fields)) throw std::runtime_error("Invalid native value catalog.");
                language::ValueLayout layout{value.name, source_type({value.kind, i}).kind, value.size, value.alignment,
                    value.kind == ValueKind::array ? source_type(value.element) : language::ValueType::void_value, {}, value.sequence_view, value.can_own, value.can_mutate, value.math, value.plain_value};
                for (std::uint32_t j = 0; j < value.field_count; ++j) {
                    const auto& field = value.fields[j];
                    if (!field.name) throw std::runtime_error("Missing native field name.");
                    layout.fields.push_back({field.name, source_type(field.type), field.offset});
                }
                values.push_back(std::move(layout));
            }
            fields.reserve(environment->field_count);
            for (std::uint32_t index = 0; index < environment->field_count; ++index) {
                const auto& field = environment->fields[index];
                if (!field.name) throw std::runtime_error("Host field name is null.");
                fields.push_back({field.name, source_type(field.kind)});
            }
            method_parameters.reserve(environment->method_count);
            methods.reserve(environment->method_count);
            for (std::uint32_t index = 0; index < environment->method_count; ++index) {
                const auto& method = environment->methods[index];
                if (!method.name || (method.parameter_count != 0 && !method.parameters))
                    throw std::runtime_error("Host method name or parameter storage is null.");
                auto& parameters = method_parameters.emplace_back();
                parameters.reserve(method.parameter_count);
                for (std::uint32_t parameter = 0; parameter < method.parameter_count; ++parameter)
                    parameters.push_back({source_type(method.parameters[parameter].type), method.parameters[parameter].passing});
                const auto result_type = method.result == ValueKind::void_value ? language::ValueType::void_value :
                    source_type(method.result);
                methods.push_back({method.name, parameters, result_type, method.receiver_read_only, method.preserves_borrows});
            }
        }
        const language::Environment source_environment{fields, methods,
            environment && environment->object_type_name ? std::string_view(environment->object_type_name) : std::string_view{}, enumerations, values};
        auto program = language::compile_source(source_name, std::string_view(source, source_size), source_environment, entry_name ? entry_name : "", declarations_only);
        if (!program) {
            const auto& diagnostic = program.error();
            if (location) *location = {diagnostic.position.offset, diagnostic.position.line, diagnostic.position.column};
            const auto display_name = diagnostic_source_name(source_name);
            if (location) std::snprintf(error, error_capacity, "%s", diagnostic.message.c_str());
            else std::snprintf(error, error_capacity, "%s:%zu:%zu: %s", display_name.c_str(),
                    diagnostic.position.line, diagnostic.position.column, diagnostic.message.c_str());
            return 1;
        }
        if (declarations_only) {
            if (!program->functions.empty()) throw std::runtime_error("Source functions require declared exports.");
            return 0;
        }
        if (entry_count) {
            std::vector<std::string_view> names;
            for (std::uint32_t i = 0; i < entry_count; ++i) names.emplace_back(entries[i]);
            auto exported = language::export_functions(*program, names, receiver);
            if (!exported) {
                if (location) *location = {exported.error().position.offset, exported.error().position.line, exported.error().position.column};
                std::snprintf(error, error_capacity, "%s", exported.error().message.c_str());
                return 1;
            }
        }
        bool owns_values = false;
        bool mutates_sequences = false;
        for (std::size_t i = 0; i < program->functions.size(); ++i) {
            if (!program->reachable[i]) continue;
            const auto& function = program->functions[i];
            const auto owned_type = [&](language::ValueType type) {
                return language::aggregate(type) && !program->value_layouts[type.type_index].math;
            };
            for (std::size_t local = 0; local < function.locals.size(); ++local)
                owns_values |= owned_type(function.locals[local].type) && (local >= function.parameters.size() ||
                    (program->exported(static_cast<std::uint32_t>(i)) && function.parameters[local].passing == ParameterPassing::value));
            owns_values |= !function.strings.empty();
            mutates_sequences |= !function.strings.empty();
            for (const auto& expression : function.expressions) {
                using Kind = language::Expression::Kind;
                mutates_sequences |= expression.kind == Kind::sequence_mutation;
                const bool call = expression.kind == Kind::call || expression.kind == Kind::source_call;
                owns_values |= owned_type(expression.type) && expression.kind == Kind::call;
                if (call)
                    for (std::size_t argument = 0; argument < expression.arguments.size(); ++argument)
                        owns_values |= expression.argument_passing[argument] == ParameterPassing::value &&
                            owned_type(function.expressions[expression.arguments[argument]].type);
            }
        }
        if (owns_values && (!services->construct_value || !services->assign_value ||
            !services->destroy_value))
            throw std::runtime_error("Source-owned values require the native value lifecycle services.");
        if (mutates_sequences && !services->mutate_sequence)
            throw std::runtime_error("Source sequence mutation requires its native service.");
        auto state = atelier::source::detail::compile(*services, *program, source_environment, options
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
            , omit_poll_for_verification
#endif
        );
        std::vector<std::unique_ptr<BusinessCode>> codes;
        codes.push_back(std::make_unique<BusinessCode>(state));
        for (std::uint32_t i = 1; i < entry_count; ++i) {
            program->entry = program->exports[i];
            auto next = atelier::source::detail::compile(*services, *program, source_environment, options
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
                , false
#endif
                , state);
            codes.push_back(std::make_unique<BusinessCode>(std::move(next)));
        }
        for (std::size_t i = 0; i < codes.size(); ++i) output[i] = codes[i].release();
        return 0;
    } catch (const std::exception& failure) {
        std::snprintf(error, error_capacity, "%s", failure.what());
    } catch (...) {
        std::snprintf(error, error_capacity, "%s", "Unknown business compilation failure.");
    }
    return 1;
}
}

std::uint32_t atelier_compile_business_entries(const atelier::source::detail::BusinessServices* services,
    const char* name, const char* source, std::uint32_t size,
    const atelier::source::detail::BusinessEnvironment* environment, const char* const* entries,
    std::uint32_t count, bool receiver, atelier::source::detail::BusinessCode** output,
    atelier::source::detail::CompileLocation* location, char* error, std::uint32_t capacity,
    atelier::source::CompileOptions options) noexcept {
    if (location) *location = {};
    return compile_business_impl(services, name, source, size, environment, output, error, capacity, entries && count ? entries[0] : nullptr, options
#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
        , false
#endif
        , entries, count, receiver, location, count == 0);
}

std::uint32_t atelier_compile_business(const atelier::source::detail::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::source::detail::BusinessEnvironment* environment,
    atelier::source::detail::BusinessCode** output,
    char* error, std::uint32_t error_capacity, const char* entry_name, atelier::source::CompileOptions options) noexcept {
    return compile_business_impl(services, source_name, source, source_size, environment,
        output, error, error_capacity, entry_name, options);
}

#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
std::uint32_t atelier_compile_business_without_poll_for_verification(
    const atelier::source::detail::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::source::detail::BusinessEnvironment* environment,
    atelier::source::detail::BusinessCode** output,
    char* error, std::uint32_t error_capacity, const char* entry_name, atelier::source::CompileOptions options) noexcept {
    return compile_business_impl(services, source_name, source, source_size, environment,
        output, error, error_capacity, entry_name, options, true);
}
#endif

void atelier_query_business_info(atelier::source::detail::BusinessCode* code,
    atelier::source::detail::BusinessCodeInfo* output) noexcept {
    if (!output) return;
    if (!code) { *output = {}; return; }
    const auto& state = *code->state;
    const auto address = state.signature.is_coroutine ?
        (state.options.diagnostics ? reinterpret_cast<std::uintptr_t>(state.diagnostic_coroutine_entry) : reinterpret_cast<std::uintptr_t>(state.coroutine_entry)) :
        (state.options.diagnostics ? reinterpret_cast<std::uintptr_t>(state.diagnostic_entry) : reinterpret_cast<std::uintptr_t>(state.entry));
    *output = {state.function_name.c_str(), state.source_name.c_str(), address, state.services, state.signature, state.environment,
        state.requires_receiver, state.options.diagnostics, state.diagnostics ? state.diagnostics->locations.data() : nullptr,
        state.diagnostics ? static_cast<std::uint32_t>(state.diagnostics->locations.size()) : 0,
        state.options.debugging, state.options.debugging ? state.diagnostics->debug_functions.data() : nullptr,
        state.options.debugging ? static_cast<std::uint32_t>(state.diagnostics->debug_functions.size()) : 0,
        state.options.debugging ? state.diagnostics->debug_sites.data() : nullptr,
        state.options.debugging ? static_cast<std::uint32_t>(state.diagnostics->debug_sites.size()) : 0};
}

bool atelier_query_business_artifacts(atelier::source::detail::BusinessCode* code,
    atelier::source::detail::BusinessArtifacts* output) noexcept {
    if (!output) return false;
    *output = {};
    if (!code || !code->state->artifacts) return false;
    const auto& state = *code->state;
    const auto& artifacts = *state.artifacts;
    *output = {state.entry_symbol.c_str(), artifacts.target_triple.c_str(), artifacts.cpu_features.c_str(),
        artifacts.original_ir.data(), artifacts.original_ir.size(), artifacts.optimized_ir.data(), artifacts.optimized_ir.size(),
        artifacts.assembly.data(), artifacts.assembly.size(), artifacts.object_code.data(), artifacts.object_code.size()};
    return true;
}

atelier::source::detail::BusinessFrameLayout atelier_business_layout(
    atelier::source::detail::BusinessCode* code) noexcept {
    return code ? code->state->layout : atelier::source::detail::BusinessFrameLayout{};
}

std::uint32_t atelier_invoke_business(atelier::source::detail::BusinessCode* code, void* invocation,
    const atelier::source::detail::BusinessValue* arguments, std::uint32_t argument_count,
    atelier::source::detail::BusinessValue* result, char* error, std::uint32_t error_capacity,
    atelier::source::detail::BusinessTrace* trace) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    if (!code || !invocation) {
        std::snprintf(error, error_capacity, "%s", "Invalid business invocation.");
        return 1;
    }
    // Do not read code again: a synchronous callback may destroy its handle.
    const auto active_code = code->state;
    if (trace && !active_code->options.diagnostics) {
        std::snprintf(error, error_capacity, "%s", "Source code was compiled without diagnostics.");
        return 1;
    }
    const auto& signature = active_code->signature;
    if (signature.is_coroutine) {
        std::snprintf(error, error_capacity, "%s", "Coroutine code requires a persistent execution instance.");
        return 1;
    }
    using atelier::source::detail::ValueKind;
    if (argument_count != signature.parameter_count || (argument_count != 0 && !arguments)) {
        std::snprintf(error, error_capacity, "%s", "Business argument count or storage does not match the compiled signature.");
        return 1;
    }
    if (signature.result != ValueKind::void_value && !result) {
        std::snprintf(error, error_capacity, "%s", "A business result requires output storage.");
        return 1;
    }
    for (std::uint32_t index = 0; index < argument_count; ++index) {
        const auto mode = signature.parameters[index].passing;
        const auto& argument = arguments[index];
        const auto type = signature.parameters[index].type;
        const auto alignment = atelier::source::detail::aggregate(type) ? active_code->values.at(type.type_index).alignment :
            type == ValueKind::boolean ? 1 : type == ValueKind::f64 ? 8 : 4;
        const bool valid_storage = mode == atelier::ParameterPassing::value ? argument.storage == atelier::source::detail::ArgumentStorage::value :
            argument.data && reinterpret_cast<std::uintptr_t>(argument.data) % alignment == 0 &&
            (argument.storage == atelier::source::detail::ArgumentStorage::reference || (mode == atelier::ParameterPassing::read_only && argument.storage == atelier::source::detail::ArgumentStorage::read_only));
        const bool aggregate_value = atelier::source::detail::aggregate(type);
        const bool valid_value = mode != atelier::ParameterPassing::value || (aggregate_value ?
            argument.data && reinterpret_cast<std::uintptr_t>(argument.data) % alignment == 0 :
            type == ValueKind::f64 || argument.bits <= (type == ValueKind::boolean ? 1 : UINT32_MAX));
        if (argument.kind != signature.parameters[index].type || !valid_storage || !valid_value) {
            std::snprintf(error, error_capacity, "Business argument %u does not match its compiled kind or storage.", index);
            return 1;
        }
    }
    const auto* value_result = signature.result.kind == ValueKind::record ?
        &active_code->values.at(signature.result.type_index) : nullptr;
    if (value_result && (!result->data || reinterpret_cast<std::uintptr_t>(result->data) % value_result->alignment != 0)) {
        std::snprintf(error, error_capacity, "%s", "Record result requires aligned typed output storage.");
        return 1;
    }
    std::uint64_t staged_bits{};
    atelier::detail::ValueStorage result_storage;
    void* staged_value = nullptr;
    try {
        if (value_result) staged_value = result_storage.prepare(value_result->size, value_result->alignment);
    } catch (const std::bad_alloc&) {
        std::snprintf(error, error_capacity, "%s", "Could not allocate record result staging.");
        return 1;
    }
    auto failure_reason = atelier::source::detail::EntryFailure::none;
    const auto status = active_code->options.diagnostics ? active_code->diagnostic_entry(invocation, arguments,
        value_result ? staged_value : &staged_bits, &failure_reason, trace) :
        active_code->entry(invocation, arguments, value_result ? staged_value : &staged_bits, &failure_reason);
    if (const auto* message = atelier::source::detail::failure_message(failure_reason))
        std::snprintf(error, error_capacity, "%s", message);
    if (status == 0 && result) {
        if (value_result) {
            std::memcpy(const_cast<void*>(result->data), staged_value, value_result->size);
            *result = {signature.result, 0, atelier::source::detail::ArgumentStorage::value, result->data};
        } else *result = {signature.result, staged_bits};
    }
    return status;
}

void atelier_destroy_business(atelier::source::detail::BusinessCode* code) noexcept { delete code; }

std::uint32_t atelier_create_business_coroutine(atelier::source::detail::BusinessCode* code, void* invocation,
    const atelier::source::detail::BusinessValue* arguments, std::uint32_t argument_count,
    atelier::source::detail::BusinessCoroutine** output, char* error, std::uint32_t error_capacity,
    atelier::source::detail::BusinessTrace* trace) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    using namespace atelier::source::detail;
    if (!code || !invocation || !output || !code->state->signature.is_coroutine) {
        std::snprintf(error, error_capacity, "%s", "Coroutine creation requires compiled coroutine code and owned invocation storage.");
        return 1;
    }
    const auto active = code->state;
    if (trace && !active->options.diagnostics) {
        std::snprintf(error, error_capacity, "%s", "Source code was compiled without diagnostics.");
        return 1;
    }
    if (argument_count != active->signature.parameter_count || (argument_count && !arguments)) {
        std::snprintf(error, error_capacity, "%s", "Coroutine arguments do not match its signature.");
        return 1;
    }
    for (std::uint32_t i = 0; i < argument_count; ++i) {
        if (arguments[i].kind != active->signature.parameters[i].type || arguments[i].storage != ArgumentStorage::value ||
            (arguments[i].kind != ValueKind::f64 && arguments[i].bits > (arguments[i].kind == ValueKind::boolean ? 1 : UINT32_MAX)) ||
            aggregate(arguments[i].kind)) {
            std::snprintf(error, error_capacity, "%s", "Coroutine argument has the wrong scalar kind.");
            return 1;
        }
    }
    try {
        auto state = std::make_shared<BusinessCoroutineState>();
        state->code = active;
        if (argument_count) state->arguments.assign(arguments, arguments + argument_count);
        auto owner = std::make_unique<BusinessCoroutine>();
        owner->state = state;
        state->handle = active->options.diagnostics ? active->diagnostic_coroutine_entry(invocation, state->arguments.data(), &state->bits,
            &state->failure, &state->status, &state->completed, trace) :
            active->coroutine_entry(invocation, state->arguments.data(), &state->bits, &state->failure, &state->status, &state->completed);
        if (!state->handle) throw std::bad_alloc();
        *output = owner.release();
        return 0;
    } catch (const std::exception& failure) {
        std::snprintf(error, error_capacity, "%s", failure.what());
    } catch (...) { std::snprintf(error, error_capacity, "%s", "Coroutine creation failed."); }
    return 1;
}

std::uint32_t atelier_resume_business_coroutine(atelier::source::detail::BusinessCoroutine* coroutine,
    bool* suspended, atelier::source::detail::BusinessValue* result, char* error, std::uint32_t error_capacity) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    if (!coroutine || !suspended || !result) {
        std::snprintf(error, error_capacity, "%s", "Coroutine resume requires an instance and output storage.");
        return 1;
    }
    const auto active = coroutine->state;
    if (active->running || active->completed) {
        std::snprintf(error, error_capacity, "%s", "Coroutine is running or has already completed.");
        return 1;
    }
    active->running = true;
    active->code->resume(active->handle);
    active->running = false;
    if (const auto* message = atelier::source::detail::failure_message(active->failure))
        std::snprintf(error, error_capacity, "%s", message);
    if (active->status != 0) return active->status;
    *suspended = active->completed == 0;
    if (active->completed) *result = {active->code->signature.result, active->bits};
    return 0;
}

void atelier_destroy_business_coroutine(atelier::source::detail::BusinessCoroutine* coroutine) noexcept { delete coroutine; }

#if defined(ATELIER_BUILD_JIT_ACCEPTANCE)
std::uint32_t atelier_verify_business_link_failure(
    const atelier::source::detail::BusinessServices* services, char* error, std::uint32_t error_capacity) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    try {
        if (!services) throw std::runtime_error("Missing business services for link verification.");
        auto program = atelier::language::compile_source("business-link-failure.source",
            "void candidate() {}", {});
        if (!program) throw std::runtime_error(program.error().message);
        try {
            (void)atelier::source::detail::compile(*services, *program, {}, {}, true);
        } catch (const std::exception& failure) {
            const std::string_view diagnostic(failure.what());
            if (diagnostic.find("business_poll") != std::string_view::npos &&
                diagnostic.find("business-link-failure.source") != std::string_view::npos &&
                diagnostic.find("materialization") != std::string_view::npos) return 0;
            throw std::runtime_error("Business link failure lost its source or root cause: " + std::string(diagnostic));
        }
        throw std::runtime_error("Candidate with a missing business service unexpectedly compiled.");
    } catch (const std::exception& failure) {
        std::snprintf(error, error_capacity, "%s", failure.what());
    } catch (...) {
        std::snprintf(error, error_capacity, "%s", "Unknown business link verification failure.");
    }
    return 1;
}
#endif

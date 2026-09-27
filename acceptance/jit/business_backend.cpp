#include "business_backend.hpp"
#include "jit/windows_unwind.hpp"
#include "language/source.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <bit>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atelier::jit_acceptance {
namespace {
using Entry = std::uint32_t (*)(void*, const BusinessScalar*, std::uint32_t*) noexcept;
using llvm::orc::LLJIT;
using llvm::orc::ExecutorAddr;

struct CompiledBusiness {
    std::string diagnostic;
    std::string object_type_name;
    std::string function_name;
    std::string source_name;
    BusinessServices services{};
    std::vector<ScalarKind> parameters;
    std::vector<std::string> field_names;
    std::vector<BusinessFieldBinding> fields;
    std::vector<std::string> method_names;
    std::vector<std::vector<ScalarKind>> method_parameters;
    std::vector<BusinessMethodBinding> methods;
    BusinessSignature signature{};
    BusinessEnvironment environment{};
    bool requires_receiver{};
    BusinessFrameLayout layout{};
    std::unique_ptr<LLJIT> jit;
    Entry entry{};
};

ScalarKind scalar_kind(language::ValueType type) {
    switch (type) {
    case language::ValueType::void_value: return ScalarKind::void_value;
    case language::ValueType::f32: return ScalarKind::f32;
    case language::ValueType::u32: return ScalarKind::u32;
    default: throw std::logic_error("Unsupported business scalar type.");
    }
}

language::ValueType scalar_type(ScalarKind kind) {
    switch (kind) {
    case ScalarKind::f32: return language::ValueType::f32;
    case ScalarKind::u32: return language::ValueType::u32;
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
    SourceLowering(LLJIT& jit, const language::Program& program)
        : program_(program), context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(program.function_name, *context_)), ir_(*context_),
          local_slots_(program.locals.size(), null_reference_slot),
          temporary_slots_(program.expressions.size(), null_reference_slot) {
        for (std::size_t index = 0; index < program_.locals.size(); ++index) {
            const auto type = program_.locals[index].type;
            if (is_reference(type)) local_slots_[index] = allocate_reference(type);
        }
        for (std::size_t index = 0; index < program_.expressions.size(); ++index) {
            const auto& node = program_.expressions[index];
            using Kind = language::Expression::Kind;
            if (node.kind == Kind::create_reference || node.kind == Kind::observe_reference ||
                node.kind == Kind::acquire_reference)
                temporary_slots_[index] = allocate_reference(node.type);
            if (node.kind == Kind::call) {
                if (node.arguments.size() > max_call_arguments_)
                    max_call_arguments_ = static_cast<std::uint32_t>(node.arguments.size());
                has_call_result_ = has_call_result_ || node.type != language::ValueType::void_value;
            }
        }
        // Bound the COFF file label as well as formatted errors; the full
        // source identity remains in Program and CompiledBusiness metadata.
        if (program.source_name.size() <= diagnostic_source_limit)
            module_->setSourceFileName(program.source_name);
        else
            module_->setSourceFileName(diagnostic_source_name(program.source_name));
        module_->setTargetTriple(jit.getTargetTriple());
        module_->setDataLayout(jit.getDataLayout());
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
        scalar_type_ = llvm::StructType::get(*context_, {i32, i32});
        begin_ = declare("business_begin", i32, {ptr});
        create_ = declare("business_create", i32, {ptr, i32});
        read_float_ = declare("business_read_float", i32, {ptr, i32, i32, ptr});
        write_float_ = declare("business_write_float", i32, {ptr, i32, i32, f32});
        read_uint_ = declare("business_read_uint", i32, {ptr, i32, i32, ptr});
        write_uint_ = declare("business_write_uint", i32, {ptr, i32, i32, i32});
        call_ = declare("business_call", i32, {ptr, i32, i32, ptr, i32, ptr});
        poll_ = declare("business_poll", i32, {ptr});
        finish_call_ = declare("business_finish", i32, {ptr});
        clear_ = declare("business_clear", ir_.getVoidTy(), {ptr, i32});
        reference_ = declare("business_reference", i32, {ptr, i32, i32, i32});
        same_reference_ = declare("business_same_reference", i32, {ptr, i32, i32, ptr});
        entry_ = llvm::Function::Create(llvm::FunctionType::get(i32, {ptr, ptr, ptr}, false),
            llvm::GlobalValue::ExternalLinkage, "atelier_source_entry", *module_);
        entry_->addFnAttr(llvm::Attribute::NoInline);
        entry_->addFnAttr(llvm::Attribute::NoUnwind);
        entry_->addFnAttr("disable-tail-calls", "true");
        entry_->setUWTableKind(llvm::UWTableKind::Async);
        invocation_ = entry_->getArg(0);
    }

    BusinessFrameLayout layout() const noexcept { return {strong_slots_, weak_slots_, nullptr, max_call_arguments_}; }

    llvm::orc::ThreadSafeModule generate() {
        auto* start = block("entry");
        failure_ = block("cleanup.failure");
        finish_ = block("finish");
        ir_.SetInsertPoint(failure_);
        failure_status_ = ir_.CreatePHI(ir_.getInt32Ty(), 0, "failure.status");
        clear_roots();
        ir_.CreateRet(failure_status_);
        ir_.SetInsertPoint(start);
        float_result_ = ir_.CreateAlloca(ir_.getFloatTy());
        uint_result_ = ir_.CreateAlloca(ir_.getInt32Ty());
        float_result_->setAlignment(llvm::Align(4));
        uint_result_->setAlignment(llvm::Align(4));
        // All host-call scratch is allocated once, never on a loop backedge.
        if (max_call_arguments_ != 0) {
            call_arguments_ = ir_.CreateAlloca(scalar_type_, ir_.getInt32(max_call_arguments_), "host.arguments");
            call_arguments_->setAlignment(llvm::Align(4));
        }
        if (has_call_result_) {
            call_result_ = ir_.CreateAlloca(scalar_type_, nullptr, "host.result");
            call_result_->setAlignment(llvm::Align(4));
        }
        for (const auto& local : program_.locals) {
            if (is_reference(local.type)) {
                locals_.push_back(nullptr);
                continue;
            }
            auto* storage = ir_.CreateAlloca(value_type(local.type), nullptr, local.name);
            storage->setAlignment(llvm::Align(4));
            locals_.push_back(storage);
        }
        for (std::size_t index = 0; index < program_.parameters.size(); ++index) {
            auto* argument = ir_.CreateInBoundsGEP(scalar_type_, entry_->getArg(1),
                ir_.getInt32(static_cast<std::uint32_t>(index)));
            auto* bits = ir_.CreateLoad(ir_.getInt32Ty(), ir_.CreateStructGEP(scalar_type_, argument, 1));
            llvm::Value* value = bits;
            if (program_.parameters[index] == language::ValueType::f32)
                value = ir_.CreateBitCast(bits, ir_.getFloatTy());
            ir_.CreateStore(value, locals_.at(index));
        }
        checked_call(begin_, {invocation_});
        safe_point();
        statement(program_.body);
        if (!terminated()) {
            if (program_.result == language::ValueType::void_value) ir_.CreateBr(finish_);
            else ir_.CreateUnreachable(); // The typed tree rejects every reachable scalar fallthrough.
        }
        ir_.SetInsertPoint(finish_);
        checked_call(finish_call_, {invocation_});
        clear_roots();
        ir_.CreateRet(ir_.getInt32(0));
        std::string diagnostics;
        llvm::raw_string_ostream stream(diagnostics);
        if (llvm::verifyModule(*module_, &stream)) throw std::runtime_error(diagnostics);
        return {std::move(module_), std::move(context_)};
    }
private:
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
        switch (value) {
        case language::ValueType::f32: return ir_.getFloatTy();
        case language::ValueType::u32: return ir_.getInt32Ty();
        case language::ValueType::predicate: return ir_.getInt1Ty();
        case language::ValueType::void_value: return ir_.getVoidTy();
        case language::ValueType::strong_reference:
        case language::ValueType::weak_reference:
        case language::ValueType::null_reference: break;
        }
        throw std::logic_error("Reference values do not use LLVM value storage.");
    }
    llvm::BasicBlock* block(const char* name) { return llvm::BasicBlock::Create(*context_, name, entry_); }
    bool terminated() const { return ir_.GetInsertBlock()->getTerminatorOrNull() != nullptr; }
    void checked_call(llvm::FunctionCallee function, llvm::ArrayRef<llvm::Value*> arguments) {
        auto* status = ir_.CreateCall(function, arguments);
        failure_status_->addIncoming(status, ir_.GetInsertBlock());
        auto* next = block("service.ok");
        ir_.CreateCondBr(ir_.CreateICmpEQ(status, ir_.getInt32(0)), next, failure_);
        ir_.SetInsertPoint(next);
    }
    void clear_slot(std::uint32_t slot) {
        ir_.CreateCall(clear_, {invocation_, ir_.getInt32(slot)});
    }
    void clear_user_references() {
        for (auto slot = weak_slots_; slot != 0;) clear_slot(observation_slot_bit | --slot);
        for (auto slot = strong_slots_; slot > business_root_slots;) clear_slot(--slot);
    }
    void clear_roots() {
        clear_user_references();
        clear_slot(snapshot_slot);
        clear_slot(receiver_slot);
    }
    void clear_temporaries() {
        for (auto slot = temporaries_.rbegin(); slot != temporaries_.rend(); ++slot) clear_slot(*slot);
        temporaries_.clear();
    }
    void copy_reference(std::uint32_t destination, llvm::Value* source) {
        checked_call(reference_, {invocation_, ir_.getInt32(static_cast<std::uint32_t>(ReferenceOperation::copy)),
            ir_.getInt32(destination), source});
    }
    llvm::Value* read_field(llvm::Value* slot, language::ValueType type, std::uint32_t field) {
        if (type == language::ValueType::f32) {
            checked_call(read_float_, {invocation_, slot, ir_.getInt32(field), float_result_});
            return ir_.CreateLoad(ir_.getFloatTy(), float_result_);
        }
        if (type != language::ValueType::u32) throw std::logic_error("Unsupported business field binding.");
        checked_call(read_uint_, {invocation_, slot, ir_.getInt32(field), uint_result_});
        return ir_.CreateLoad(ir_.getInt32Ty(), uint_result_);
    }
    void write_field(llvm::Value* slot, language::ValueType type, std::uint32_t field, llvm::Value* value) {
        if (type == language::ValueType::f32) {
            checked_call(write_float_, {invocation_, slot, ir_.getInt32(field), value});
        } else {
            if (type != language::ValueType::u32) throw std::logic_error("Unsupported business field binding.");
            checked_call(write_uint_, {invocation_, slot, ir_.getInt32(field), value});
        }
    }
    void safe_point() { checked_call(poll_, {invocation_}); }
    llvm::Value* expression(language::NodeId index) {
        const auto& node = program_.expressions[index];
        using Kind = language::Expression::Kind;
        switch (node.kind) {
        case Kind::literal:
            if (node.type == language::ValueType::f32)
                return llvm::ConstantFP::get(ir_.getFloatTy(), std::bit_cast<float>(node.value));
            return ir_.getInt32(node.value);
        case Kind::local:
            if (is_reference(node.type)) return ir_.getInt32(local_slots_[node.value]);
            return ir_.CreateLoad(value_type(node.type), locals_[node.value]);
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
        case Kind::call: {
            auto* owner = expression(node.receiver);
            // Nested calls reuse the same entry scratch. Finish every operand
            // left-to-right and retain its SSA value before packing this call.
            llvm::SmallVector<llvm::Value*, 8> values;
            values.reserve(node.arguments.size());
            for (const auto argument : node.arguments) values.push_back(expression(argument));
            for (std::size_t index = 0; index < node.arguments.size(); ++index) {
                const auto type = program_.expressions[node.arguments[index]].type;
                auto* value = values[index];
                auto* storage = ir_.CreateInBoundsGEP(scalar_type_, call_arguments_,
                    ir_.getInt32(static_cast<std::uint32_t>(index)));
                ir_.CreateStore(ir_.getInt32(static_cast<std::uint32_t>(scalar_kind(type))),
                    ir_.CreateStructGEP(scalar_type_, storage, 0));
                auto* bits = type == language::ValueType::f32 ? ir_.CreateBitCast(value, ir_.getInt32Ty()) : value;
                ir_.CreateStore(bits, ir_.CreateStructGEP(scalar_type_, storage, 1));
            }
            llvm::Value* arguments = node.arguments.empty() ? llvm::ConstantPointerNull::get(ir_.getPtrTy()) :
                static_cast<llvm::Value*>(call_arguments_);
            llvm::Value* output = llvm::ConstantPointerNull::get(ir_.getPtrTy());
            if (node.type != language::ValueType::void_value) {
                output = call_result_;
                // A successful callback that omits its value must not reuse a
                // previous nested call's result or expose uninitialized bits.
                ir_.CreateStore(ir_.getInt32(static_cast<std::uint32_t>(ScalarKind::void_value)),
                    ir_.CreateStructGEP(scalar_type_, output, 0));
                ir_.CreateStore(ir_.getInt32(0), ir_.CreateStructGEP(scalar_type_, output, 1));
            }
            checked_call(call_, {invocation_, owner, ir_.getInt32(node.value), arguments,
                ir_.getInt32(static_cast<std::uint32_t>(node.arguments.size())), output});
            if (node.type == language::ValueType::void_value) return nullptr;
            auto* kind = ir_.CreateLoad(ir_.getInt32Ty(), ir_.CreateStructGEP(scalar_type_, output, 0));
            auto* next = block("result.ok");
            failure_status_->addIncoming(ir_.getInt32(1), ir_.GetInsertBlock());
            ir_.CreateCondBr(ir_.CreateICmpEQ(kind, ir_.getInt32(static_cast<std::uint32_t>(scalar_kind(node.type)))),
                next, failure_);
            ir_.SetInsertPoint(next);
            // Load now: later operands may invoke another method using this slot.
            auto* bits = ir_.CreateLoad(ir_.getInt32Ty(), ir_.CreateStructGEP(scalar_type_, output, 1));
            return node.type == language::ValueType::f32 ? ir_.CreateBitCast(bits, ir_.getFloatTy()) : bits;
        }
        case Kind::unary: {
            auto* value = expression(node.left);
            return node.operation == language::Operation::positive ? value : ir_.CreateFNeg(value);
        }
        case Kind::binary: break;
        }
        auto* left = expression(node.left);
        auto* right = expression(node.right);
        const auto left_type = program_.expressions[node.left].type;
        using Op = language::Operation;
        if (is_reference(left_type)) {
            if (node.operation != Op::equal && node.operation != Op::not_equal)
                throw std::logic_error("Invalid typed reference comparison.");
            checked_call(same_reference_, {invocation_, left, right, uint_result_});
            auto* same = ir_.CreateLoad(ir_.getInt32Ty(), uint_result_);
            return node.operation == Op::equal ? ir_.CreateICmpNE(same, ir_.getInt32(0)) :
                ir_.CreateICmpEQ(same, ir_.getInt32(0));
        }
        const bool floating = left_type == language::ValueType::f32;
        switch (node.operation) {
        case Op::add: return floating ? ir_.CreateFAdd(left, right) : ir_.CreateAdd(left, right);
        case Op::subtract: return floating ? ir_.CreateFSub(left, right) : ir_.CreateSub(left, right);
        case Op::multiply: return floating ? ir_.CreateFMul(left, right) : ir_.CreateMul(left, right);
        case Op::equal: return floating ? ir_.CreateFCmpOEQ(left, right) : ir_.CreateICmpEQ(left, right);
        case Op::not_equal: return floating ? ir_.CreateFCmpUNE(left, right) : ir_.CreateICmpNE(left, right);
        case Op::less: return floating ? ir_.CreateFCmpOLT(left, right) : ir_.CreateICmpULT(left, right);
        case Op::less_equal: return floating ? ir_.CreateFCmpOLE(left, right) : ir_.CreateICmpULE(left, right);
        case Op::greater: return floating ? ir_.CreateFCmpOGT(left, right) : ir_.CreateICmpUGT(left, right);
        case Op::greater_equal: return floating ? ir_.CreateFCmpOGE(left, right) : ir_.CreateICmpUGE(left, right);
        case Op::positive:
        case Op::negate: break;
        }
        throw std::logic_error("Invalid typed binary operation.");
    }
    void statement(language::NodeId index) {
        const auto& node = program_.statements[index];
        using Kind = language::Statement::Kind;
        switch (node.kind) {
        case Kind::block:
            for (const auto child : node.children) {
                if (terminated()) break;
                statement(child);
            }
            if (!terminated()) {
                for (auto local = node.locals.rbegin(); local != node.locals.rend(); ++local) {
                    if (is_reference(program_.locals[*local].type)) clear_slot(local_slots_[*local]);
                }
            }
            return;
        case Kind::declare: {
            auto* value = expression(node.expression);
            if (is_reference(program_.locals[node.target].type)) copy_reference(local_slots_[node.target], value);
            else ir_.CreateStore(value, locals_[node.target]);
            clear_temporaries();
            return;
        }
        case Kind::assign: {
            const auto& target = program_.expressions[node.target];
            // Evaluate a field's owner before its RHS and keep any owner
            // temporary rooted through the store, even if the RHS calls out.
            auto* owner = target.kind == language::Expression::Kind::field ? expression(target.receiver) : nullptr;
            auto* value = expression(node.expression);
            if (target.kind == language::Expression::Kind::local) {
                if (is_reference(target.type)) copy_reference(local_slots_[target.value], value);
                else ir_.CreateStore(value, locals_[target.value]);
            } else write_field(owner, target.type, target.value, value);
            clear_temporaries();
            return;
        }
        case Kind::call:
            (void)expression(node.expression);
            clear_temporaries();
            return;
        case Kind::return_value:
            if (node.expression != language::no_node) {
                auto* value = expression(node.expression);
                auto* bits = program_.result == language::ValueType::f32 ?
                    ir_.CreateBitCast(value, ir_.getInt32Ty()) : value;
                ir_.CreateStore(bits, entry_->getArg(2));
            }
            clear_temporaries();
            // Stage before shared cleanup; public result storage is untouched
            // until finish and all-root cleanup have both succeeded.
            if (!return_cleanup_) {
                auto* origin = ir_.GetInsertBlock();
                return_cleanup_ = block("cleanup.return");
                ir_.SetInsertPoint(return_cleanup_);
                clear_user_references();
                ir_.CreateBr(finish_);
                ir_.SetInsertPoint(origin);
            }
            ir_.CreateBr(return_cleanup_);
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
            auto* condition = block("while.condition");
            auto* body = block("while.body");
            auto* end = block("while.end");
            ir_.CreateBr(condition);
            ir_.SetInsertPoint(condition);
            auto* value = expression(node.expression);
            clear_temporaries();
            ir_.CreateCondBr(value, body, end);
            ir_.SetInsertPoint(body);
            statement(node.body);
            if (!terminated()) { safe_point(); ir_.CreateBr(condition); }
            ir_.SetInsertPoint(end);
            return;
        }
        }
        throw std::logic_error("Invalid typed statement.");
    }

    const language::Program& program_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> ir_;
    llvm::Function* entry_{};
    llvm::Value* invocation_{};
    llvm::BasicBlock* failure_{};
    llvm::BasicBlock* finish_{};
    llvm::BasicBlock* return_cleanup_{};
    llvm::PHINode* failure_status_{};
    llvm::StructType* scalar_type_{};
    llvm::AllocaInst* call_arguments_{};
    llvm::AllocaInst* call_result_{};
    llvm::AllocaInst* float_result_{};
    llvm::AllocaInst* uint_result_{};
    std::vector<llvm::AllocaInst*> locals_;
    std::vector<std::uint32_t> local_slots_;
    std::vector<std::uint32_t> temporary_slots_;
    std::vector<std::uint32_t> temporaries_;
    std::uint32_t strong_slots_{business_root_slots};
    std::uint32_t weak_slots_{};
    std::uint32_t max_call_arguments_{};
    bool has_call_result_{};
    llvm::FunctionCallee begin_, create_, read_float_, write_float_, read_uint_, write_uint_;
    llvm::FunctionCallee call_, poll_, finish_call_, clear_, reference_, same_reference_;
};

llvm::orc::ThreadSafeModule generate_business(LLJIT& jit, const language::Program& program,
    BusinessFrameLayout& layout) {
    SourceLowering lowering(jit, program);
    layout = lowering.layout();
    return lowering.generate();
}

std::shared_ptr<CompiledBusiness> compile(const BusinessServices& services, const language::Program& program,
    language::Environment environment, bool omit_poll_for_verification = false) {
    if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter())
        throw std::runtime_error("LLVM native target initialization failed.");
    auto target = take(llvm::orc::JITTargetMachineBuilder::detectHost());
    target.setCodeModel(llvm::CodeModel::Large);
    target.setRelocationModel(llvm::Reloc::PIC_);
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
    auto state = std::make_shared<CompiledBusiness>();
    state->function_name = program.function_name;
    state->source_name = program.source_name;
    state->services = services;
    state->object_type_name = program.object_type_name;
    state->parameters.reserve(program.parameters.size());
    for (const auto parameter : program.parameters) state->parameters.push_back(scalar_kind(parameter));
    state->signature = {state->parameters.empty() ? nullptr : state->parameters.data(),
        static_cast<std::uint32_t>(state->parameters.size()), scalar_kind(program.result)};
    state->field_names.reserve(environment.fields.size());
    state->fields.reserve(environment.fields.size());
    for (const auto& field : environment.fields) {
        state->field_names.emplace_back(field.name);
        state->fields.push_back({state->field_names.back().c_str(), scalar_kind(field.type)});
    }
    state->method_names.reserve(environment.methods.size());
    state->method_parameters.reserve(environment.methods.size());
    state->methods.reserve(environment.methods.size());
    for (const auto& method : environment.methods) {
        state->method_names.emplace_back(method.name);
        auto& parameters = state->method_parameters.emplace_back();
        parameters.reserve(method.parameters.size());
        for (const auto parameter : method.parameters) parameters.push_back(scalar_kind(parameter));
        state->methods.push_back({state->method_names.back().c_str(), parameters.empty() ? nullptr : parameters.data(),
            static_cast<std::uint32_t>(parameters.size()), scalar_kind(method.result), method.receiver_read_only});
    }
    state->environment = {state->fields.empty() ? nullptr : state->fields.data(),
        static_cast<std::uint32_t>(state->fields.size()), state->methods.empty() ? nullptr : state->methods.data(),
        static_cast<std::uint32_t>(state->methods.size()), state->object_type_name.c_str()};
    for (const auto& expression : program.expressions)
        if (expression.kind == language::Expression::Kind::receiver) { state->requires_receiver = true; break; }
    state->jit = take(builder.create());
    state->jit->getExecutionSession().setErrorReporter([owner = state.get()](llvm::Error error) {
        owner->diagnostic = llvm::toString(std::move(error));
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
    bind("business_read_uint", state->services.read_uint);
    bind("business_write_uint", state->services.write_uint);
    bind("business_call", state->services.call);
    if (!omit_poll_for_verification) bind("business_poll", state->services.poll);
    bind("business_finish", state->services.finish);
    bind("business_clear", state->services.clear);
    bind("business_reference", state->services.reference);
    bind("business_same_reference", state->services.same_reference);
    check(state->jit->getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols))));
    check(state->jit->addIRModule(generate_business(*state->jit, program, state->layout)));
    state->layout.object_type_name = state->object_type_name.c_str();
    auto address = state->jit->lookup("atelier_source_entry");
    if (!address) {
        auto diagnostic = llvm::toString(address.takeError());
        if (!state->diagnostic.empty()) diagnostic += "\n" + state->diagnostic;
        throw std::runtime_error(diagnostic_source_name(program.source_name) + ": JIT materialization: " + diagnostic);
    }
    state->entry = address->toPtr<Entry>();
    return state;
}
}

struct BusinessCode { std::shared_ptr<CompiledBusiness> state; };
}

namespace {
std::uint32_t compile_business_impl(const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::jit_acceptance::BusinessEnvironment* environment,
    atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity, bool omit_poll_for_verification) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    try {
        if (!services || !source_name || !source || !output || !services->begin || !services->create ||
            !services->read_float || !services->write_float || !services->read_uint || !services->write_uint ||
            !services->call || !services->poll || !services->finish || !services->clear ||
            !services->reference || !services->same_reference)
            throw std::runtime_error("Invalid business compiler input, callbacks or output storage.");
        using namespace atelier;
        using namespace atelier::jit_acceptance;
        std::vector<language::FieldBinding> fields;
        std::vector<std::vector<language::ValueType>> method_parameters;
        std::vector<language::MethodBinding> methods;
        if (environment) {
            if ((environment->field_count != 0 && !environment->fields) ||
                (environment->method_count != 0 && !environment->methods))
                throw std::runtime_error("Host binding count requires catalog storage.");
            fields.reserve(environment->field_count);
            for (std::uint32_t index = 0; index < environment->field_count; ++index) {
                const auto& field = environment->fields[index];
                if (!field.name) throw std::runtime_error("Host field name is null.");
                fields.push_back({field.name, scalar_type(field.kind)});
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
                    parameters.push_back(scalar_type(method.parameters[parameter]));
                const auto result_type = method.result == ScalarKind::void_value ? language::ValueType::void_value :
                    scalar_type(method.result);
                methods.push_back({method.name, parameters, result_type, method.receiver_read_only});
            }
        }
        const language::Environment source_environment{fields, methods,
            environment && environment->object_type_name ? std::string_view(environment->object_type_name) : std::string_view{}};
        auto program = language::compile_source(source_name, std::string_view(source, source_size), source_environment);
        if (!program) {
            const auto& diagnostic = program.error();
            const auto display_name = diagnostic_source_name(source_name);
            std::snprintf(error, error_capacity, "%s:%zu:%zu: %s", display_name.c_str(),
                diagnostic.position.line, diagnostic.position.column, diagnostic.message.c_str());
            return 1;
        }
        auto state = atelier::jit_acceptance::compile(*services, *program, source_environment, omit_poll_for_verification);
        *output = new atelier::jit_acceptance::BusinessCode{std::move(state)};
        return 0;
    } catch (const std::exception& failure) {
        std::snprintf(error, error_capacity, "%s", failure.what());
    } catch (...) {
        std::snprintf(error, error_capacity, "%s", "Unknown business compilation failure.");
    }
    return 1;
}
}

std::uint32_t atelier_compile_business(const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::jit_acceptance::BusinessEnvironment* environment,
    atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity) noexcept {
    return compile_business_impl(services, source_name, source, source_size, environment,
        output, error, error_capacity, false);
}

std::uint32_t atelier_compile_business_without_poll_for_verification(
    const atelier::jit_acceptance::BusinessServices* services,
    const char* source_name, const char* source, std::uint32_t source_size,
    const atelier::jit_acceptance::BusinessEnvironment* environment,
    atelier::jit_acceptance::BusinessCode** output,
    char* error, std::uint32_t error_capacity) noexcept {
    return compile_business_impl(services, source_name, source, source_size, environment,
        output, error, error_capacity, true);
}

atelier::jit_acceptance::BusinessCodeInfo atelier_business_info(
    atelier::jit_acceptance::BusinessCode* code) noexcept {
    if (!code) return {};
    const auto& state = *code->state;
    return {state.function_name.c_str(), state.source_name.c_str(),
        reinterpret_cast<std::uintptr_t>(state.entry), state.services, state.signature, state.environment,
        state.requires_receiver};
}

atelier::jit_acceptance::BusinessFrameLayout atelier_business_layout(
    atelier::jit_acceptance::BusinessCode* code) noexcept {
    return code ? code->state->layout : atelier::jit_acceptance::BusinessFrameLayout{};
}

std::uint32_t atelier_invoke_business(atelier::jit_acceptance::BusinessCode* code, void* invocation,
    const atelier::jit_acceptance::BusinessScalar* arguments, std::uint32_t argument_count,
    atelier::jit_acceptance::BusinessScalar* result, char* error, std::uint32_t error_capacity) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    if (!code || !invocation) {
        std::snprintf(error, error_capacity, "%s", "Invalid business invocation.");
        return 1;
    }
    // Do not read code again: a synchronous callback may destroy its handle.
    const auto active_code = code->state;
    const auto& signature = active_code->signature;
    using atelier::jit_acceptance::ScalarKind;
    if (argument_count != signature.parameter_count || (argument_count != 0 && !arguments)) {
        std::snprintf(error, error_capacity, "%s", "Business argument count or storage does not match the compiled signature.");
        return 1;
    }
    if (signature.result != ScalarKind::void_value && !result) {
        std::snprintf(error, error_capacity, "%s", "A scalar business result requires output storage.");
        return 1;
    }
    for (std::uint32_t index = 0; index < argument_count; ++index) {
        if (arguments[index].kind != signature.parameters[index]) {
            std::snprintf(error, error_capacity, "Business argument %u does not match its compiled scalar kind.", index);
            return 1;
        }
    }
    std::uint32_t staged_bits{};
    const auto status = active_code->entry(invocation, arguments, &staged_bits);
    if (status == 0 && result) *result = {signature.result, staged_bits};
    return status;
}

void atelier_destroy_business(atelier::jit_acceptance::BusinessCode* code) noexcept { delete code; }

std::uint32_t atelier_verify_business_link_failure(
    const atelier::jit_acceptance::BusinessServices* services, char* error, std::uint32_t error_capacity) noexcept {
    if (!error || error_capacity == 0) return 1;
    error[0] = '\0';
    try {
        if (!services) throw std::runtime_error("Missing business services for link verification.");
        auto program = atelier::language::compile_source("business-link-failure.source",
            "void candidate() {}", {});
        if (!program) throw std::runtime_error(program.error().message);
        try {
            (void)atelier::jit_acceptance::compile(*services, *program, {}, true);
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

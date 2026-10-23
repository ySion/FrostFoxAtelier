#pragma once

#include "language/source.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Host.h>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>

namespace atelier::source::detail {

// Mathematical values have a vector SSA representation, but their memory is
// the exact compact C++ record. All boundary accesses touch only real lanes.
class MathLowering {
public:
    MathLowering(llvm::IRBuilder<>& ir, llvm::Module& module) : ir_(ir), module_(module) {}
    static llvm::Type* scalar_type(llvm::LLVMContext& context, math::Type type) {
        return type.precision == math::Precision::f64 ? llvm::Type::getDoubleTy(context) : llvm::Type::getFloatTy(context);
    }
    static llvm::FixedVectorType* value_type(llvm::LLVMContext& context, math::Type type) {
        return llvm::FixedVectorType::get(scalar_type(context, type), static_cast<unsigned>(type.components()));
    }
    llvm::Value* load(llvm::Value* address, math::Type type) {
        llvm::Value* result = llvm::PoisonValue::get(value_type(ir_.getContext(), type));
        for (unsigned i = 0; i < type.components(); ++i) {
            auto* source = ir_.CreateInBoundsGEP(ir_.getInt8Ty(), address, ir_.getInt64(i * type.scalar_size()));
            result = ir_.CreateInsertElement(result, ir_.CreateAlignedLoad(scalar_type(ir_.getContext(), type),
                source, llvm::Align(type.scalar_size())), i);
        }
        return result;
    }
    void store(llvm::Value* value, llvm::Value* address, math::Type type) {
        for (unsigned i = 0; i < type.components(); ++i) {
            auto* target = ir_.CreateInBoundsGEP(ir_.getInt8Ty(), address, ir_.getInt64(i * type.scalar_size()));
            ir_.CreateAlignedStore(lane(value, i), target, llvm::Align(type.scalar_size()));
        }
    }
    llvm::Value* construct(math::Type type, std::span<llvm::Value* const> values) {
        llvm::Value* result = llvm::Constant::getNullValue(value_type(ir_.getContext(), type));
        if (values.empty()) {
            if (type.shape == math::Shape::quaternion) result = ir_.CreateInsertElement(result,
                llvm::ConstantFP::get(scalar_type(ir_.getContext(), type), 1), 3);
            return result;
        }
        if (type.shape == math::Shape::matrix4 && values.size() == 1) {
            for (unsigned i = 0; i < 16; i += 5) result = ir_.CreateInsertElement(result, values[0], i);
        } else for (unsigned i = 0; i < values.size(); ++i) result = ir_.CreateInsertElement(result, values[i], i);
        return result;
    }
    llvm::Value* binary(language::Operation operation, llvm::Value* left, llvm::Value* right,
        math::Type left_type, math::Type right_type) {
        using Op = language::Operation;
        if (operation == Op::equal || operation == Op::not_equal) {
            auto* comparisons = operation == Op::equal ? ir_.CreateFCmpOEQ(left, right) : ir_.CreateFCmpUNE(left, right);
            llvm::Value* result = ir_.getInt1(operation == Op::equal);
            for (unsigned i = 0; i < left_type.components(); ++i)
                result = operation == Op::equal ? ir_.CreateAnd(result, lane(comparisons, i)) : ir_.CreateOr(result, lane(comparisons, i));
            return result;
        }
        if (operation == Op::multiply && left_type && right_type) {
            if (left_type.shape == math::Shape::quaternion) return quaternion_product(left, right);
            if (left_type.shape == math::Shape::matrix4) return matrix_product(left, right);
        }
        const auto type = left_type ? left_type : right_type;
        if (!left_type) left = ir_.CreateVectorSplat(static_cast<unsigned>(type.components()), left);
        if (!right_type) right = ir_.CreateVectorSplat(static_cast<unsigned>(type.components()), right);
        switch (operation) {
        case Op::add: return ir_.CreateFAdd(left, right);
        case Op::subtract: return ir_.CreateFSub(left, right);
        case Op::multiply: return ir_.CreateFMul(left, right);
        case Op::divide: return ir_.CreateFDiv(left, right);
        default: throw std::logic_error("Invalid mathematical binary operation.");
        }
    }
    llvm::Value* operation(language::MathOperation operation, std::span<llvm::Value* const> args,
        math::Type result_type) {
        using Op = language::MathOperation;
        if (operation >= Op::square_root) return elementwise(operation, args);
        switch (operation) {
        case Op::dot: return dot(args[0], args[1]);
        case Op::cross: return cross(args[0], args[1]);
        case Op::length: return norm(args[0], false);
        case Op::normalize_or_zero: return norm(args[0], true);
        case Op::conjugate: {
            auto* result = ir_.CreateFNeg(args[0]);
            return ir_.CreateInsertElement(result, lane(args[0], 3), 3);
        }
        case Op::rotate: {
            llvm::Value* xyz = llvm::PoisonValue::get(value_type(ir_.getContext(), result_type));
            for (unsigned i = 0; i < 3; ++i) xyz = ir_.CreateInsertElement(xyz, lane(args[0], i), i);
            auto* twice = ir_.CreateFMul(cross(xyz, args[1]), llvm::ConstantFP::get(xyz->getType(), 2));
            return ir_.CreateFAdd(ir_.CreateFAdd(args[1], ir_.CreateFMul(twice,
                ir_.CreateVectorSplat(3, lane(args[0], 3)))), cross(xyz, twice));
        }
        case Op::transpose: {
            llvm::Value* result = llvm::PoisonValue::get(args[0]->getType());
            for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col)
                result = ir_.CreateInsertElement(result, lane(args[0], col * 4 + row), row * 4 + col);
            return result;
        }
        case Op::transform_point:
        case Op::transform_direction: {
            llvm::Value* result = llvm::PoisonValue::get(value_type(ir_.getContext(), result_type));
            for (unsigned col = 0; col < 3; ++col) {
                auto* value = ir_.CreateFMul(lane(args[0], 0), lane(args[1], col));
                for (unsigned row = 1; row < 3; ++row)
                    value = ir_.CreateFAdd(value, ir_.CreateFMul(lane(args[0], row), lane(args[1], row * 4 + col)));
                if (operation == Op::transform_point) value = ir_.CreateFAdd(value, lane(args[1], 12 + col));
                result = ir_.CreateInsertElement(result, value, col);
            }
            return result;
        }
        default: break;
        }
        throw std::logic_error("Invalid mathematical intrinsic.");
    }
private:
    llvm::Value* lane(llvm::Value* value, unsigned index) { return ir_.CreateExtractElement(value, index); }
    llvm::Value* intrinsic(llvm::Intrinsic::ID id, llvm::Value* input) {
        return ir_.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&module_, id, {input->getType()}), {input});
    }
    llvm::Value* rounding(llvm::Intrinsic::ID id, llvm::Value* input) {
        // Without SSE4.1 these lower to CRT calls, which the JIT does not resolve.
        static const bool supported = llvm::sys::getHostCPUFeatures().lookup("sse4.1");
        if (!supported)
            throw std::runtime_error("math.floor, math.ceil, math.trunc and math.fract require a host CPU with SSE4.1.");
        return intrinsic(id, input);
    }
    // Scalar arguments apply to every lane of the vector argument's type.
    llvm::Value* elementwise(language::MathOperation operation, std::span<llvm::Value* const> input) {
        using Op = language::MathOperation;
        llvm::Type* type = input[0]->getType();
        for (auto* value : input) if (value->getType()->isVectorTy()) { type = value->getType(); break; }
        llvm::SmallVector<llvm::Value*, 3> a;
        for (auto* value : input) a.push_back(value->getType() == type ? value :
            ir_.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(type)->getNumElements(), value));
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        // GLSL operand order: each select is one x64 minps/maxps, and only a NaN first argument propagates.
        const auto min = [&](llvm::Value* x, llvm::Value* y) { return ir_.CreateSelect(ir_.CreateFCmpOLT(y, x), y, x); };
        const auto max = [&](llvm::Value* x, llvm::Value* y) { return ir_.CreateSelect(ir_.CreateFCmpOLT(x, y), y, x); };
        const auto clamp = [&](llvm::Value* x, llvm::Value* low, llvm::Value* high) { return max(low, min(x, high)); };
        switch (operation) {
        case Op::square_root: return intrinsic(llvm::Intrinsic::sqrt, a[0]);
        case Op::floor: return rounding(llvm::Intrinsic::floor, a[0]);
        case Op::ceil: return rounding(llvm::Intrinsic::ceil, a[0]);
        case Op::trunc: return rounding(llvm::Intrinsic::trunc, a[0]);
        case Op::fract: {
            // Capped below one so fract(x) * n stays a valid index.
            const double below_one = type->getScalarType()->isDoubleTy() ?
                std::nextafter(1.0, 0.0) : static_cast<double>(std::nextafter(1.0f, 0.0f));
            return min(ir_.CreateFSub(a[0], rounding(llvm::Intrinsic::floor, a[0])), constant(below_one));
        }
        case Op::abs: return intrinsic(llvm::Intrinsic::fabs, a[0]);
        case Op::sign: return ir_.CreateSelect(ir_.CreateFCmpOGT(a[0], constant(0)), constant(1),
            ir_.CreateSelect(ir_.CreateFCmpOLT(a[0], constant(0)), constant(-1), a[0]));
        case Op::min: return min(a[0], a[1]);
        case Op::max: return max(a[0], a[1]);
        case Op::clamp: return clamp(a[0], a[1], a[2]);
        case Op::lerp: return ir_.CreateFAdd(ir_.CreateFMul(a[0], ir_.CreateFSub(constant(1), a[2])), ir_.CreateFMul(a[1], a[2]));
        case Op::step: return ir_.CreateSelect(ir_.CreateFCmpOLT(a[1], a[0]), constant(0), constant(1));
        case Op::smoothstep: {
            auto* t = clamp(ir_.CreateFDiv(ir_.CreateFSub(a[2], a[0]), ir_.CreateFSub(a[1], a[0])), constant(0), constant(1));
            return ir_.CreateFMul(ir_.CreateFMul(t, t), ir_.CreateFSub(constant(3), ir_.CreateFMul(constant(2), t)));
        }
        default: throw std::logic_error("Invalid element-wise math operation.");
        }
    }
    llvm::Value* dot(llvm::Value* a, llvm::Value* b) {
        auto* products = ir_.CreateFMul(a, b);
        auto* result = lane(products, 0);
        const auto count = llvm::cast<llvm::FixedVectorType>(a->getType())->getNumElements();
        for (unsigned i = 1; i < count; ++i) result = ir_.CreateFAdd(result, lane(products, i));
        return result;
    }
    llvm::Value* cross(llvm::Value* a, llvm::Value* b) {
        auto* ayzx = ir_.CreateShuffleVector(a, {1, 2, 0});
        auto* azxy = ir_.CreateShuffleVector(a, {2, 0, 1});
        auto* byzx = ir_.CreateShuffleVector(b, {1, 2, 0});
        auto* bzxy = ir_.CreateShuffleVector(b, {2, 0, 1});
        return ir_.CreateFSub(ir_.CreateFMul(ayzx, bzxy), ir_.CreateFMul(azxy, byzx));
    }
    llvm::Value* norm(llvm::Value* input, bool normalize) {
        auto* type = llvm::cast<llvm::FixedVectorType>(input->getType());
        auto* scalar = type->getElementType();
        const auto count = type->getNumElements();
        auto* zero = llvm::ConstantFP::get(scalar, 0);
        auto* one = llvm::ConstantFP::get(scalar, 1);
        auto* limit = llvm::ConstantFP::get(scalar, scalar->isDoubleTy() ? std::numeric_limits<double>::max() : std::numeric_limits<float>::max());
        llvm::Value* maximum = zero;
        llvm::Value* finite = ir_.getTrue();
        llvm::Value* has_nan = ir_.getFalse();
        auto* magnitudes = intrinsic(llvm::Intrinsic::fabs, input);
        for (unsigned i = 0; i < count; ++i) {
            auto* x = lane(magnitudes, i);
            maximum = ir_.CreateSelect(ir_.CreateFCmpOGT(x, maximum), x, maximum);
            finite = ir_.CreateAnd(finite, ir_.CreateFCmpOLE(x, limit));
            has_nan = ir_.CreateOr(has_nan, ir_.CreateFCmpUNO(x, x));
        }
        auto* valid = ir_.CreateAnd(finite, ir_.CreateFCmpOGT(maximum, zero));
        auto* divisor = ir_.CreateVectorSplat(count, ir_.CreateSelect(valid, maximum, one));
        auto* scaled = ir_.CreateFDiv(ir_.CreateSelect(valid, input, llvm::Constant::getNullValue(type)), divisor);
        auto* magnitude = intrinsic(llvm::Intrinsic::sqrt, dot(scaled, scaled));
        if (normalize) return ir_.CreateFDiv(scaled, ir_.CreateVectorSplat(count, ir_.CreateSelect(valid, magnitude, one)));
        auto* result = ir_.CreateFMul(maximum, magnitude);
        result = ir_.CreateSelect(finite, result, llvm::ConstantFP::getInfinity(scalar));
        auto* infinity = llvm::ConstantFP::getInfinity(scalar);
        return ir_.CreateSelect(ir_.CreateAnd(has_nan, ir_.CreateFCmpONE(maximum, infinity)),
            llvm::ConstantFP::getNaN(scalar), result);
    }
    llvm::Value* quaternion_product(llvm::Value* a, llvm::Value* b) {
        llvm::Value* result = llvm::PoisonValue::get(a->getType());
        const auto multiply = [&](unsigned i, unsigned j) { return ir_.CreateFMul(lane(a, i), lane(b, j)); };
        auto* x = ir_.CreateFSub(ir_.CreateFAdd(ir_.CreateFAdd(multiply(3, 0), multiply(0, 3)), multiply(1, 2)), multiply(2, 1));
        auto* y = ir_.CreateFAdd(ir_.CreateFAdd(ir_.CreateFSub(multiply(3, 1), multiply(0, 2)), multiply(1, 3)), multiply(2, 0));
        auto* z = ir_.CreateFAdd(ir_.CreateFSub(ir_.CreateFAdd(multiply(3, 2), multiply(0, 1)), multiply(1, 0)), multiply(2, 3));
        result = ir_.CreateInsertElement(result, x, std::uint64_t{0});
        result = ir_.CreateInsertElement(result, y, 1);
        result = ir_.CreateInsertElement(result, z, 2);
        auto* w = ir_.CreateFMul(lane(a, 3), lane(b, 3));
        for (unsigned i = 0; i < 3; ++i) w = ir_.CreateFSub(w, ir_.CreateFMul(lane(a, i), lane(b, i)));
        return ir_.CreateInsertElement(result, w, 3);
    }
    llvm::Value* matrix_product(llvm::Value* a, llvm::Value* b) {
        llvm::Value* result = llvm::PoisonValue::get(a->getType());
        for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col) {
            auto* v = ir_.CreateFMul(lane(a, row * 4), lane(b, col));
            for (unsigned k = 1; k < 4; ++k) v = ir_.CreateFAdd(v, ir_.CreateFMul(lane(a, row * 4 + k), lane(b, k * 4 + col)));
            result = ir_.CreateInsertElement(result, v, row * 4 + col);
        }
        return result;
    }
    llvm::IRBuilder<>& ir_;
    llvm::Module& module_;
};

}

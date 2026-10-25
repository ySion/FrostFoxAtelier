#pragma once

#include "language/source.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
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
    llvm::Value* fma(llvm::Value* a, llvm::Value* b, llvm::Value* c) {
        return ir_.CreateIntrinsic(llvm::Intrinsic::fma, {a->getType()}, {a, b, c});
    }
    // Horner evaluation, highest coefficient last.
    llvm::Value* polynomial(llvm::Value* x, std::span<const double> coefficients) {
        llvm::Value* result = llvm::ConstantFP::get(x->getType(), coefficients.back());
        for (auto i = coefficients.size() - 1; i-- > 0;)
            result = fma(x, result, llvm::ConstantFP::get(x->getType(), coefficients[i]));
        return result;
    }
    // Estrin evaluation: the same polynomial with a dependency chain of log2(n) FMAs, for latency-bound double paths.
    llvm::Value* estrin(llvm::Value* x, std::span<const double> coefficients) {
        llvm::SmallVector<llvm::Value*, 8> terms;
        for (std::size_t i = 0; i < coefficients.size(); ++i) terms.push_back(llvm::ConstantFP::get(x->getType(), coefficients[i]));
        for (llvm::Value* power = x;; power = ir_.CreateFMul(power, power)) {
            llvm::SmallVector<llvm::Value*, 8> pairs;
            for (std::size_t i = 0; i < terms.size(); i += 2) pairs.push_back(i + 1 < terms.size() ? fma(power, terms[i + 1], terms[i]) : terms[i]);
            terms = std::move(pairs);
            if (terms.size() == 1) return terms[0];
        }
    }
    // GLSL operand order: each select is one x64 minps/maxps, and only a NaN first argument propagates.
    llvm::Value* minimum(llvm::Value* x, llvm::Value* y) { return ir_.CreateSelect(ir_.CreateFCmpOLT(y, x), y, x); }
    llvm::Value* maximum(llvm::Value* x, llvm::Value* y) { return ir_.CreateSelect(ir_.CreateFCmpOLT(x, y), y, x); }
    // Scalar arguments apply to every lane of the vector argument's type.
    llvm::Value* elementwise(language::MathOperation operation, std::span<llvm::Value* const> input) {
        using Op = language::MathOperation;
        llvm::Type* type = input[0]->getType();
        for (auto* value : input) if (value->getType()->isVectorTy()) { type = value->getType(); break; }
        llvm::SmallVector<llvm::Value*, 3> a;
        for (auto* value : input) a.push_back(value->getType() == type ? value :
            ir_.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(type)->getNumElements(), value));
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        const auto clamp = [&](llvm::Value* x, llvm::Value* low, llvm::Value* high) { return maximum(low, minimum(x, high)); };
        switch (operation) {
        case Op::square_root: return intrinsic(llvm::Intrinsic::sqrt, a[0]);
        case Op::floor: return intrinsic(llvm::Intrinsic::floor, a[0]);
        case Op::ceil: return intrinsic(llvm::Intrinsic::ceil, a[0]);
        case Op::trunc: return intrinsic(llvm::Intrinsic::trunc, a[0]);
        case Op::fract: {
            // Capped below one so fract(x) * n stays a valid index.
            const double below_one = type->getScalarType()->isDoubleTy() ?
                std::nextafter(1.0, 0.0) : static_cast<double>(std::nextafter(1.0f, 0.0f));
            return minimum(ir_.CreateFSub(a[0], intrinsic(llvm::Intrinsic::floor, a[0])), constant(below_one));
        }
        case Op::abs: return intrinsic(llvm::Intrinsic::fabs, a[0]);
        case Op::sign: return ir_.CreateSelect(ir_.CreateFCmpOGT(a[0], constant(0)), constant(1),
            ir_.CreateSelect(ir_.CreateFCmpOLT(a[0], constant(0)), constant(-1), a[0]));
        case Op::min: return minimum(a[0], a[1]);
        case Op::max: return maximum(a[0], a[1]);
        case Op::clamp: return clamp(a[0], a[1], a[2]);
        case Op::lerp: return ir_.CreateFAdd(ir_.CreateFMul(a[0], ir_.CreateFSub(constant(1), a[2])), ir_.CreateFMul(a[1], a[2]));
        case Op::step: return ir_.CreateSelect(ir_.CreateFCmpOLT(a[1], a[0]), constant(0), constant(1));
        case Op::smoothstep: {
            auto* t = clamp(ir_.CreateFDiv(ir_.CreateFSub(a[2], a[0]), ir_.CreateFSub(a[1], a[0])), constant(0), constant(1));
            return ir_.CreateFMul(ir_.CreateFMul(t, t), ir_.CreateFSub(constant(3), ir_.CreateFMul(constant(2), t)));
        }
        case Op::sin: return sine(a[0], false);
        case Op::cos: return sine(a[0], true);
        case Op::atan2: return arctangent(a[0], a[1]);
        case Op::exp: return exponential(a[0]);
        case Op::log: return logarithm(a[0]);
        case Op::pow: return power(a[0], a[1]);
        default: throw std::logic_error("Invalid element-wise math operation.");
        }
    }
    // Float transcendental functions: minimax polynomials over Cody-Waite reduced arguments, FMA throughout.
    // Rounding to an integer adds 1.5·2^23 (2^52 for double): the sum's low mantissa bits hold that integer.
    llvm::Value* integers(llvm::Type* type, std::int64_t value, unsigned bits = 32) {
        return llvm::ConstantInt::getSigned(type->getWithNewType(ir_.getIntNTy(bits)), value);
    }
    llvm::Value* as_integers(llvm::Value* value) {
        return ir_.CreateBitCast(value, value->getType()->getWithNewType(ir_.getIntNTy(value->getType()->getScalarSizeInBits())));
    }
    llvm::Value* sine(llvm::Value* x, bool cosine) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        // Coefficient fits cover |r| <= 0.83: pi/4 plus the quadrant rounding slack up to |x| = 2^20.
        static constexpr double sin_terms[] = {-0x1.55553ep-3, 0x1.1104cap-7, -0x1.9877dp-13};
        static constexpr double cos_terms[] = {0x1.555546p-5, -0x1.6c098cp-10, 0x1.9930d4p-16};
        auto* t = fma(x, constant(0x1.45f306p-1), constant(0x1.8p23));
        auto* q = ir_.CreateFSub(t, constant(0x1.8p23));
        llvm::Value* quadrant = as_integers(t);
        if (cosine) quadrant = ir_.CreateAdd(quadrant, integers(type, 1));
        auto* r = fma(q, constant(-0x1.921fb6p+0), x);
        r = fma(q, constant(0x1.777a5cp-25), r);
        r = fma(q, constant(0x1.ee59dap-50), r);
        // Beyond 2^20 the reduction is meaningless; the clamp keeps the result finite. NaN passes through.
        r = minimum(maximum(r, constant(-0.83)), constant(0.83));
        auto* z = ir_.CreateFMul(r, r);
        // The leading sine coefficient is negative, so r + r^3 * P turns -0 into +0; zero returns x itself.
        auto* s = ir_.CreateSelect(ir_.CreateFCmpOEQ(x, constant(0)), x, fma(ir_.CreateFMul(r, z), polynomial(z, sin_terms), r));
        auto* c = fma(z, fma(z, polynomial(z, cos_terms), constant(-0.5)), constant(1));
        auto* odd = ir_.CreateICmpNE(ir_.CreateAnd(quadrant, integers(type, 1)), integers(type, 0));
        auto* sign = ir_.CreateShl(ir_.CreateAnd(quadrant, integers(type, 2)), integers(type, 30));
        return ir_.CreateBitCast(ir_.CreateXor(as_integers(ir_.CreateSelect(odd, c, s)), sign), type);
    }
    llvm::Value* exponential(llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {0x1.fffffcp-2, 0x1.555492p-3, 0x1.5558f2p-5, 0x1.1239ep-7, 0x1.6a243ap-10};
        // exp(89) overflows and exp(-104) rounds to zero; NaN passes through.
        x = minimum(maximum(x, constant(-104)), constant(89));
        auto* t = fma(x, constant(0x1.715476p+0), constant(0x1.8p23));
        auto* n = ir_.CreateFSub(t, constant(0x1.8p23));
        auto* k = ir_.CreateSub(as_integers(t), integers(type, 0x4b400000));
        auto* r = fma(n, constant(-0x1.62e43p-1), x);
        r = fma(n, constant(0x1.05c61p-29), r);
        auto* p = ir_.CreateFAdd(constant(1), fma(ir_.CreateFMul(r, r), polynomial(r, terms), r));
        // 2^k in two exact factors, so subnormal results round once.
        auto* half = ir_.CreateAShr(k, integers(type, 1));
        const auto power_of_two = [&](llvm::Value* e) {
            return ir_.CreateBitCast(ir_.CreateShl(ir_.CreateAdd(e, integers(type, 127)), integers(type, 23)), type);
        };
        return ir_.CreateFMul(ir_.CreateFMul(p, power_of_two(half)), power_of_two(ir_.CreateSub(k, half)));
    }
    llvm::Value* logarithm(llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {0x1.5556d8p-2, -0x1.000382p-2, 0x1.98d7f2p-3, -0x1.53824ap-3, 0x1.317456p-3, -0x1.2432bap-3, 0x1.645eb4p-4};
        // x = 2^e * m with m in [sqrt(1/2), sqrt(2)); subnormals are scaled into the normal range first.
        auto* subnormal = ir_.CreateFCmpOLT(x, constant(0x1p-126));
        auto* bits = ir_.CreateSub(as_integers(ir_.CreateSelect(subnormal, ir_.CreateFMul(x, constant(0x1p23)), x)), integers(type, 0x3f3504f3));
        auto* e = ir_.CreateSIToFP(ir_.CreateSub(ir_.CreateAShr(bits, integers(type, 23)),
            ir_.CreateSelect(subnormal, integers(type, 23), integers(type, 0))), type);
        auto* m = ir_.CreateBitCast(ir_.CreateAdd(ir_.CreateAnd(bits, integers(type, 0x7fffff)), integers(type, 0x3f3504f3)), type);
        auto* f = ir_.CreateFSub(m, constant(1));
        auto* w = fma(ir_.CreateFMul(f, f), fma(f, polynomial(f, terms), constant(-0.5)), f);
        auto* result = fma(e, constant(0x1.62e43p-1), fma(e, constant(-0x1.05c61p-29), w));
        auto* special = ir_.CreateSelect(ir_.CreateFCmpOEQ(x, constant(0)), constant(-INFINITY),
            ir_.CreateSelect(ir_.CreateFCmpOLT(x, constant(0)), constant(NAN), x));
        return ir_.CreateSelect(ir_.CreateAnd(ir_.CreateFCmpOGT(x, constant(0)), ir_.CreateFCmpOLT(x, constant(INFINITY))), result, special);
    }
    llvm::Value* arctangent(llvm::Value* y, llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {-0x1.555512p-2, 0x1.997b54p-3, -0x1.224de2p-3, 0x1.a0277ep-4, -0x1.a47668p-5};
        // atan(u / v) with u <= v; infinities and zeros reduce to the same ratio's limit.
        auto* ay = intrinsic(llvm::Intrinsic::fabs, y);
        auto* ax = intrinsic(llvm::Intrinsic::fabs, x);
        auto* swap = ir_.CreateFCmpOGT(ay, ax);
        llvm::Value* u = ir_.CreateSelect(swap, ax, ay);
        llvm::Value* v = ir_.CreateSelect(swap, ay, ax);
        auto* infinite = ir_.CreateFCmpOEQ(v, constant(INFINITY));
        u = ir_.CreateSelect(infinite, ir_.CreateSelect(ir_.CreateFCmpOEQ(u, constant(INFINITY)), constant(1), constant(0)), u);
        v = ir_.CreateSelect(ir_.CreateOr(infinite, ir_.CreateFCmpOEQ(v, constant(0))), constant(1), v);
        auto* scale = ir_.CreateSelect(ir_.CreateFCmpOGT(v, constant(0x1p126)), constant(0.25), constant(1));
        u = ir_.CreateFMul(u, scale);
        v = ir_.CreateFMul(v, scale);
        // Above a ratio of 1/2, atan(u/v) = pi/4 + atan((u - v)/(u + v)); u - v is then exact. 2u is exact where v/2 may round.
        auto* upper = ir_.CreateFCmpOGT(ir_.CreateFMul(u, constant(2)), v);
        auto* t = ir_.CreateFDiv(ir_.CreateSelect(upper, ir_.CreateFSub(u, v), u), ir_.CreateSelect(upper, ir_.CreateFAdd(u, v), v));
        auto* z = ir_.CreateFMul(t, t);
        auto* a = fma(ir_.CreateFMul(t, z), polynomial(z, terms), t);
        // The angle is k*pi/4 +- a for the swap, the sign of x and the upper range.
        auto* negative = ir_.CreateICmpSLT(as_integers(x), integers(type, 0));
        auto* flip = ir_.CreateXor(swap, negative);
        auto* base = ir_.CreateSelect(swap, constant(2), ir_.CreateSelect(negative, constant(4), constant(0)));
        auto* step = ir_.CreateSelect(upper, constant(1), constant(0));
        auto* k = ir_.CreateSelect(flip, ir_.CreateFSub(base, step), ir_.CreateFAdd(base, step));
        auto* angle = fma(k, constant(0x1.921fb6p-1), fma(k, constant(-0x1.777a5cp-26), ir_.CreateSelect(flip, ir_.CreateFNeg(a), a)));
        auto* result = ir_.CreateIntrinsic(llvm::Intrinsic::copysign, {type}, {angle, y});
        return ir_.CreateSelect(ir_.CreateFCmpUNO(x, y), ir_.CreateFAdd(x, y), result);
    }
    // exp2(y * log2|x|) in double, rounded to float once.
    llvm::Value* power(llvm::Value* x, llvm::Value* y) {
        auto* type = x->getType();
        auto* wide = type->getWithNewType(ir_.getDoubleTy());
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        const auto precise = [&](double value) { return llvm::ConstantFP::get(wide, value); };
        // Truncation errors: 5e-11 relative in log2 and 2e-10 in exp2, both far below half a float ULP after |t| <= 152.
        static constexpr double log_terms[] = {2.0, 2.0 / 3, 2.0 / 5, 2.0 / 7, 2.0 / 9, 2.0 / 11};
        static constexpr double exp2_terms[] = {1.0, 0x1.62e42fefa39efp-1, 0x1.ebfbdff82c58fp-3, 0x1.c6b08d704a0c0p-5, 0x1.3b2ab6fba4e77p-7,
            0x1.5d87fe78a6731p-10, 0x1.430912f86c787p-13, 0x1.ffcbfc588b0c7p-17, 0x1.62c0223a5c824p-20};
        constexpr std::int64_t sqrt_half = 0x3fe6a09e667f3bcd;
        auto* a = ir_.CreateFPExt(intrinsic(llvm::Intrinsic::fabs, x), wide);
        auto* bits = ir_.CreateSub(as_integers(a), integers(wide, sqrt_half, 64));
        auto* e = ir_.CreateSIToFP(ir_.CreateTrunc(ir_.CreateAShr(bits, integers(wide, 52, 64)), wide->getWithNewType(ir_.getInt32Ty())), wide);
        auto* m = ir_.CreateBitCast(ir_.CreateAdd(ir_.CreateAnd(bits, integers(wide, 0xfffffffffffff, 64)), integers(wide, sqrt_half, 64)), wide);
        auto* s = ir_.CreateFDiv(ir_.CreateFSub(m, precise(1)), ir_.CreateFAdd(m, precise(1)));
        llvm::Value* log2 = fma(ir_.CreateFMul(s, estrin(ir_.CreateFMul(s, s), log_terms)), precise(0x1.71547652b82fep+0), e);
        log2 = ir_.CreateSelect(ir_.CreateFCmpOEQ(a, precise(0)), precise(-INFINITY),
            ir_.CreateSelect(ir_.CreateFCmpOEQ(a, precise(INFINITY)), a, log2));
        // 2^129 overflows float and 2^-152 rounds to zero; NaN passes through.
        auto* t = minimum(maximum(ir_.CreateFMul(ir_.CreateFPExt(y, wide), log2), precise(-152)), precise(129));
        auto* rounded = ir_.CreateFAdd(t, precise(0x1.8p52));
        auto* f = ir_.CreateFSub(t, ir_.CreateFSub(rounded, precise(0x1.8p52)));
        auto* n = ir_.CreateSub(as_integers(rounded), integers(wide, 0x4338000000000000, 64));
        auto* scale = ir_.CreateBitCast(ir_.CreateShl(ir_.CreateAdd(n, integers(wide, 1023, 64)), integers(wide, 52, 64)), wide);
        llvm::Value* result = ir_.CreateFPTrunc(ir_.CreateFMul(estrin(f, exp2_terms), scale), type);
        result = ir_.CreateSelect(ir_.CreateOr(ir_.CreateFCmpOLT(x, constant(0)), ir_.CreateFCmpUNO(x, y)), constant(NAN), result);
        return ir_.CreateSelect(ir_.CreateOr(ir_.CreateFCmpOEQ(y, constant(0)), ir_.CreateFCmpOEQ(x, constant(1))), constant(1), result);
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

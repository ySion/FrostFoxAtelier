#pragma once

#include "language/source.hpp"
#include "memory/fatal.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace atelier::source::detail {

// Mathematical values have a vector SSA representation, but their memory is
// the exact compact C++ record. All boundary accesses touch only real lanes.
class MathLowering {
public:
    MathLowering(llvm::IRBuilder<>& ir, llvm::Module& module) : ir_(ir), module_(module) {}

    static llvm::Type* scalar_type(llvm::LLVMContext& context, math::Type type) {
        return type.precision == math::Precision::f64 ? llvm::Type::getDoubleTy(context)
                                                      : llvm::Type::getFloatTy(context);
    }

    static llvm::FixedVectorType* value_type(llvm::LLVMContext& context, math::Type type) {
        return llvm::FixedVectorType::get(scalar_type(context, type), static_cast<unsigned>(type.components()));
    }

    llvm::Value* load(llvm::Value* address, math::Type type) {
        if (type.components() != 3)
            return ir_.CreateAlignedLoad(value_type(ir_.getContext(), type), address, llvm::Align(type.scalar_size()));
        llvm::Value* result = llvm::PoisonValue::get(value_type(ir_.getContext(), type));
        for (unsigned i = 0; i < 3; ++i) {
            auto* source = ir_.CreateInBoundsGEP(ir_.getInt8Ty(), address, ir_.getInt64(i * type.scalar_size()));
            result = ir_.CreateInsertElement(
                result,
                ir_.CreateAlignedLoad(scalar_type(ir_.getContext(), type), source, llvm::Align(type.scalar_size())), i);
        }
        return result;
    }

    void store(llvm::Value* value, llvm::Value* address, math::Type type) {
        if (type.components() != 3) {
            ir_.CreateAlignedStore(value, address, llvm::Align(type.scalar_size()));
            return;
        }
        for (unsigned i = 0; i < 3; ++i) {
            auto* target = ir_.CreateInBoundsGEP(ir_.getInt8Ty(), address, ir_.getInt64(i * type.scalar_size()));
            ir_.CreateAlignedStore(lane(value, i), target, llvm::Align(type.scalar_size()));
        }
    }

    llvm::Value* construct(math::Type type, std::span<llvm::Value* const> values) {
        llvm::Value* result = llvm::Constant::getNullValue(value_type(ir_.getContext(), type));
        if (values.empty()) {
            if (type.shape == math::Shape::quaternion)
                result =
                    ir_.CreateInsertElement(result, llvm::ConstantFP::get(scalar_type(ir_.getContext(), type), 1), 3);
            return result;
        }
        if (type.shape == math::Shape::matrix4 && values.size() == 1) {
            for (unsigned i = 0; i < 16; i += 5)
                result = ir_.CreateInsertElement(result, values[0], i);
        } else
            for (unsigned i = 0; i < values.size(); ++i)
                result = ir_.CreateInsertElement(result, values[i], i);
        return result;
    }

    llvm::Value* binary(language::Operation operation, llvm::Value* left, llvm::Value* right, math::Type left_type,
                        math::Type right_type) {
        using Op = language::Operation;
        if (operation == Op::equal || operation == Op::not_equal) {
            auto* comparisons =
                operation == Op::equal ? ir_.CreateFCmpOEQ(left, right) : ir_.CreateFCmpUNE(left, right);
            llvm::Value* result = ir_.getInt1(operation == Op::equal);
            for (unsigned i = 0; i < left_type.components(); ++i)
                result = operation == Op::equal ? ir_.CreateAnd(result, lane(comparisons, i))
                                                : ir_.CreateOr(result, lane(comparisons, i));
            return result;
        }
        if (operation == Op::multiply && left_type && right_type) {
            if (left_type.shape == math::Shape::quaternion)
                return quaternion_product(left, right);
            if (left_type.shape == math::Shape::matrix4)
                return matrix_product(left, right);
        }
        const auto type = left_type ? left_type : right_type;
        if (!left_type)
            left = ir_.CreateVectorSplat(static_cast<unsigned>(type.components()), left);
        if (!right_type)
            right = ir_.CreateVectorSplat(static_cast<unsigned>(type.components()), right);
        switch (operation) {
        case Op::add:
            return ir_.CreateFAdd(left, right);
        case Op::subtract:
            return ir_.CreateFSub(left, right);
        case Op::multiply:
            return ir_.CreateFMul(left, right);
        case Op::divide:
            return ir_.CreateFDiv(left, right);
        default:
            fatal("Invalid mathematical binary operation.");
        }
    }

    llvm::Value* operation(language::MathOperation operation, std::span<llvm::Value* const> args) {
        using Op = language::MathOperation;
        if (operation >= Op::square_root)
            return elementwise(operation, args);
        switch (operation) {
        case Op::dot:
            return dot(args[0], args[1]);
        case Op::cross:
            return cross(args[0], args[1]);
        case Op::length:
            return norm(args[0], false);
        case Op::normalize_or_zero:
            return norm(args[0], true);
        case Op::fast_normalize_or_zero:
            return fast_normalize(args[0]);
        case Op::conjugate:
            return ir_.CreateShuffleVector(ir_.CreateFNeg(args[0]), args[0], {0, 1, 2, 7});
        case Op::slerp01:
            return slerp01(args[0], args[1], args[2]);
        case Op::fast_slerp01:
            return fast_slerp01(args[0], args[1], args[2]);
        case Op::rotate: {
            auto* xyz = ir_.CreateShuffleVector(args[0], {0, 1, 2});
            auto* twice = ir_.CreateFMul(cross(xyz, args[1]), llvm::ConstantFP::get(xyz->getType(), 2));
            return ir_.CreateFAdd(
                ir_.CreateFAdd(args[1], ir_.CreateFMul(twice, ir_.CreateShuffleVector(args[0], {3, 3, 3}))),
                cross(xyz, twice));
        }
        case Op::transpose:
            if (args[0]->getType()->getScalarType()->isDoubleTy())
                return ir_.CreateShuffleVector(args[0], {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15});
            return ir_.CreateIntrinsic(llvm::Intrinsic::matrix_transpose, {args[0]->getType()},
                                       {args[0], ir_.getInt32(4), ir_.getInt32(4)});
        case Op::transform_point:
        case Op::transform_direction: {
            auto* r0 = ir_.CreateShuffleVector(args[1], {0, 1, 2});
            auto* r1 = ir_.CreateShuffleVector(args[1], {4, 5, 6});
            auto* r2 = ir_.CreateShuffleVector(args[1], {8, 9, 10});
            auto* result = ir_.CreateFMul(ir_.CreateShuffleVector(args[0], {0, 0, 0}), r0);
            result = ir_.CreateFAdd(result, ir_.CreateFMul(ir_.CreateShuffleVector(args[0], {1, 1, 1}), r1));
            result = ir_.CreateFAdd(result, ir_.CreateFMul(ir_.CreateShuffleVector(args[0], {2, 2, 2}), r2));
            if (operation == Op::transform_point)
                result = ir_.CreateFAdd(result, ir_.CreateShuffleVector(args[1], {12, 13, 14}));
            return result;
        }
        case Op::is_finite: {
            auto* type = args[0]->getType();
            auto* finite =
                ir_.CreateFCmpOLT(intrinsic(llvm::Intrinsic::fabs, args[0]), llvm::ConstantFP::getInfinity(type));
            return type->isVectorTy() ? ir_.CreateAndReduce(finite) : finite;
        }
        case Op::as_uint:
        case Op::as_int:
            return ir_.CreateBitCast(args[0], ir_.getInt32Ty());
        default:
            break;
        }
        fatal("Invalid mathematical intrinsic.");
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
        for (std::size_t i = 0; i < coefficients.size(); ++i)
            terms.push_back(llvm::ConstantFP::get(x->getType(), coefficients[i]));
        for (llvm::Value* power = x;; power = ir_.CreateFMul(power, power)) {
            llvm::SmallVector<llvm::Value*, 8> pairs;
            for (std::size_t i = 0; i < terms.size(); i += 2)
                pairs.push_back(i + 1 < terms.size() ? fma(power, terms[i + 1], terms[i]) : terms[i]);
            terms = std::move(pairs);
            if (terms.size() == 1)
                return terms[0];
        }
    }

    // GLSL operand order: each select is one x64 minps/maxps, and only a NaN first argument propagates.
    llvm::Value* minimum(llvm::Value* x, llvm::Value* y) { return ir_.CreateSelect(ir_.CreateFCmpOLT(y, x), y, x); }

    llvm::Value* maximum(llvm::Value* x, llvm::Value* y) { return ir_.CreateSelect(ir_.CreateFCmpOLT(x, y), y, x); }

    // Scalar arguments apply to every lane of the vector argument's type.
    llvm::Value* elementwise(language::MathOperation operation, std::span<llvm::Value* const> input) {
        using Op = language::MathOperation;
        llvm::Type* type = input[0]->getType();
        for (auto* value : input)
            if (value->getType()->isVectorTy()) {
                type = value->getType();
                break;
            }
        llvm::SmallVector<llvm::Value*, 3> a;
        for (auto* value : input)
            a.push_back(value->getType() == type
                            ? value
                            : ir_.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(type)->getNumElements(), value));
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        const auto clamp = [&](llvm::Value* x, llvm::Value* low, llvm::Value* high) {
            return maximum(low, minimum(x, high));
        };
        const bool wide = type->getScalarType()->isDoubleTy();
        switch (operation) {
        case Op::square_root:
            return intrinsic(llvm::Intrinsic::sqrt, a[0]);
        case Op::fast_rsqrt:
            return fast_rsqrt(a[0]);
        case Op::floor:
            return intrinsic(llvm::Intrinsic::floor, a[0]);
        case Op::ceil:
            return intrinsic(llvm::Intrinsic::ceil, a[0]);
        case Op::trunc:
            return intrinsic(llvm::Intrinsic::trunc, a[0]);
        case Op::fract: {
            // Capped below one so fract(x) * n stays a valid index.
            const double below_one = type->getScalarType()->isDoubleTy()
                                         ? std::nextafter(1.0, 0.0)
                                         : static_cast<double>(std::nextafter(1.0f, 0.0f));
            return minimum(ir_.CreateFSub(a[0], intrinsic(llvm::Intrinsic::floor, a[0])), constant(below_one));
        }
        case Op::abs:
            return intrinsic(llvm::Intrinsic::fabs, a[0]);
        case Op::sign:
            return ir_.CreateSelect(ir_.CreateFCmpOGT(a[0], constant(0)), constant(1),
                                    ir_.CreateSelect(ir_.CreateFCmpOLT(a[0], constant(0)), constant(-1), a[0]));
        case Op::min:
            return minimum(a[0], a[1]);
        case Op::max:
            return maximum(a[0], a[1]);
        case Op::clamp:
            return clamp(a[0], a[1], a[2]);
        case Op::lerp:
            return ir_.CreateFAdd(ir_.CreateFMul(a[0], ir_.CreateFSub(constant(1), a[2])), ir_.CreateFMul(a[1], a[2]));
        case Op::step:
            return ir_.CreateSelect(ir_.CreateFCmpOLT(a[1], a[0]), constant(0), constant(1));
        case Op::smoothstep: {
            auto* t =
                clamp(ir_.CreateFDiv(ir_.CreateFSub(a[2], a[0]), ir_.CreateFSub(a[1], a[0])), constant(0), constant(1));
            return ir_.CreateFMul(ir_.CreateFMul(t, t), ir_.CreateFSub(constant(3), ir_.CreateFMul(constant(2), t)));
        }
        case Op::sin:
            return wide ? sine_double(a[0], false) : sine(a[0], false);
        case Op::cos:
            return wide ? sine_double(a[0], true) : sine(a[0], true);
        case Op::atan2:
            return wide ? arctangent_double(a[0], a[1]) : arctangent(a[0], a[1]);
        case Op::exp:
            return wide ? exponential_double(minimum(maximum(a[0], constant(-746)), constant(710)), nullptr)
                        : exponential(a[0]);
        case Op::log:
            return wide ? logarithm_double(a[0]) : logarithm(a[0]);
        case Op::pow:
            return wide ? power_double(a[0], a[1]) : power(a[0], a[1]);
        default:
            fatal("Invalid element-wise math operation.");
        }
    }

    // Float transcendental functions: minimax polynomials over Cody-Waite reduced arguments, FMA throughout.
    // Rounding to an integer adds 1.5·2^23 (2^52 for double): the sum's low mantissa bits hold that integer.
    llvm::Value* integers(llvm::Type* type, std::int64_t value, unsigned bits = 32) {
        return llvm::ConstantInt::getSigned(type->getWithNewType(ir_.getIntNTy(bits)), value);
    }

    llvm::Value* as_integers(llvm::Value* value) {
        return ir_.CreateBitCast(
            value, value->getType()->getWithNewType(ir_.getIntNTy(value->getType()->getScalarSizeInBits())));
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
        if (cosine)
            quadrant = ir_.CreateAdd(quadrant, integers(type, 1));
        auto* r = fma(q, constant(-0x1.921fb6p+0), x);
        r = fma(q, constant(0x1.777a5cp-25), r);
        r = fma(q, constant(0x1.ee59dap-50), r);
        // Beyond 2^20 the reduction is meaningless; the clamp keeps the result finite. NaN passes through.
        r = minimum(maximum(r, constant(-0.83)), constant(0.83));
        auto* z = ir_.CreateFMul(r, r);
        // The leading sine coefficient is negative, so r + r^3 * P turns -0 into +0; zero returns x itself.
        auto* s = ir_.CreateSelect(ir_.CreateFCmpOEQ(x, constant(0)), x,
                                   fma(ir_.CreateFMul(r, z), polynomial(z, sin_terms), r));
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
        static constexpr double terms[] = {0x1.5556d8p-2, -0x1.000382p-2, 0x1.98d7f2p-3, -0x1.53824ap-3,
                                           0x1.317456p-3, -0x1.2432bap-3, 0x1.645eb4p-4};
        // x = 2^e * m with m in [sqrt(1/2), sqrt(2)); subnormals are scaled into the normal range first.
        auto* subnormal = ir_.CreateFCmpOLT(x, constant(0x1p-126));
        auto* bits = ir_.CreateSub(as_integers(ir_.CreateSelect(subnormal, ir_.CreateFMul(x, constant(0x1p23)), x)),
                                   integers(type, 0x3f3504f3));
        auto* e = ir_.CreateSIToFP(ir_.CreateSub(ir_.CreateAShr(bits, integers(type, 23)),
                                                 ir_.CreateSelect(subnormal, integers(type, 23), integers(type, 0))),
                                   type);
        auto* m = ir_.CreateBitCast(
            ir_.CreateAdd(ir_.CreateAnd(bits, integers(type, 0x7fffff)), integers(type, 0x3f3504f3)), type);
        auto* f = ir_.CreateFSub(m, constant(1));
        auto* w = fma(ir_.CreateFMul(f, f), fma(f, polynomial(f, terms), constant(-0.5)), f);
        auto* result = fma(e, constant(0x1.62e43p-1), fma(e, constant(-0x1.05c61p-29), w));
        auto* special = ir_.CreateSelect(ir_.CreateFCmpOEQ(x, constant(0)), constant(-INFINITY),
                                         ir_.CreateSelect(ir_.CreateFCmpOLT(x, constant(0)), constant(NAN), x));
        return ir_.CreateSelect(
            ir_.CreateAnd(ir_.CreateFCmpOGT(x, constant(0)), ir_.CreateFCmpOLT(x, constant(INFINITY))), result,
            special);
    }

    llvm::Value* arctangent(llvm::Value* y, llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {-0x1.555512p-2, 0x1.997b54p-3, -0x1.224de2p-3, 0x1.a0277ep-4,
                                           -0x1.a47668p-5};
        // atan(u / v) with u <= v; infinities and zeros reduce to the same ratio's limit.
        auto* ay = intrinsic(llvm::Intrinsic::fabs, y);
        auto* ax = intrinsic(llvm::Intrinsic::fabs, x);
        auto* swap = ir_.CreateFCmpOGT(ay, ax);
        llvm::Value* u = ir_.CreateSelect(swap, ax, ay);
        llvm::Value* v = ir_.CreateSelect(swap, ay, ax);
        auto* infinite = ir_.CreateFCmpOEQ(v, constant(INFINITY));
        u = ir_.CreateSelect(infinite,
                             ir_.CreateSelect(ir_.CreateFCmpOEQ(u, constant(INFINITY)), constant(1), constant(0)), u);
        v = ir_.CreateSelect(ir_.CreateOr(infinite, ir_.CreateFCmpOEQ(v, constant(0))), constant(1), v);
        auto* scale = ir_.CreateSelect(ir_.CreateFCmpOGT(v, constant(0x1p126)), constant(0.25), constant(1));
        u = ir_.CreateFMul(u, scale);
        v = ir_.CreateFMul(v, scale);
        // Above a ratio of 1/2, atan(u/v) = pi/4 + atan((u - v)/(u + v)); u - v is then exact. 2u is exact where v/2 may round.
        auto* upper = ir_.CreateFCmpOGT(ir_.CreateFMul(u, constant(2)), v);
        auto* t = ir_.CreateFDiv(ir_.CreateSelect(upper, ir_.CreateFSub(u, v), u),
                                 ir_.CreateSelect(upper, ir_.CreateFAdd(u, v), v));
        auto* z = ir_.CreateFMul(t, t);
        auto* a = fma(ir_.CreateFMul(t, z), polynomial(z, terms), t);
        // The angle is k*pi/4 +- a for the swap, the sign of x and the upper range.
        auto* negative = ir_.CreateICmpSLT(as_integers(x), integers(type, 0));
        auto* flip = ir_.CreateXor(swap, negative);
        auto* base = ir_.CreateSelect(swap, constant(2), ir_.CreateSelect(negative, constant(4), constant(0)));
        auto* step = ir_.CreateSelect(upper, constant(1), constant(0));
        auto* k = ir_.CreateSelect(flip, ir_.CreateFSub(base, step), ir_.CreateFAdd(base, step));
        auto* angle = fma(k, constant(0x1.921fb6p-1),
                          fma(k, constant(-0x1.777a5cp-26), ir_.CreateSelect(flip, ir_.CreateFNeg(a), a)));
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
        static constexpr double exp2_terms[] = {1.0,
                                                0x1.62e42fefa39efp-1,
                                                0x1.ebfbdff82c58fp-3,
                                                0x1.c6b08d704a0c0p-5,
                                                0x1.3b2ab6fba4e77p-7,
                                                0x1.5d87fe78a6731p-10,
                                                0x1.430912f86c787p-13,
                                                0x1.ffcbfc588b0c7p-17,
                                                0x1.62c0223a5c824p-20};
        constexpr std::int64_t sqrt_half = 0x3fe6a09e667f3bcd;
        auto* a = ir_.CreateFPExt(intrinsic(llvm::Intrinsic::fabs, x), wide);
        auto* bits = ir_.CreateSub(as_integers(a), integers(wide, sqrt_half, 64));
        auto* e = ir_.CreateSIToFP(
            ir_.CreateTrunc(ir_.CreateAShr(bits, integers(wide, 52, 64)), wide->getWithNewType(ir_.getInt32Ty())),
            wide);
        auto* m = ir_.CreateBitCast(
            ir_.CreateAdd(ir_.CreateAnd(bits, integers(wide, 0xfffffffffffff, 64)), integers(wide, sqrt_half, 64)),
            wide);
        auto* s = ir_.CreateFDiv(ir_.CreateFSub(m, precise(1)), ir_.CreateFAdd(m, precise(1)));
        llvm::Value* log2 =
            fma(ir_.CreateFMul(s, estrin(ir_.CreateFMul(s, s), log_terms)), precise(0x1.71547652b82fep+0), e);
        log2 = ir_.CreateSelect(ir_.CreateFCmpOEQ(a, precise(0)), precise(-INFINITY),
                                ir_.CreateSelect(ir_.CreateFCmpOEQ(a, precise(INFINITY)), a, log2));
        // 2^129 overflows float and 2^-152 rounds to zero; NaN passes through.
        auto* t = minimum(maximum(ir_.CreateFMul(ir_.CreateFPExt(y, wide), log2), precise(-152)), precise(129));
        auto* rounded = ir_.CreateFAdd(t, precise(0x1.8p52));
        auto* f = ir_.CreateFSub(t, ir_.CreateFSub(rounded, precise(0x1.8p52)));
        auto* n = ir_.CreateSub(as_integers(rounded), integers(wide, 0x4338000000000000, 64));
        auto* scale =
            ir_.CreateBitCast(ir_.CreateShl(ir_.CreateAdd(n, integers(wide, 1023, 64)), integers(wide, 52, 64)), wide);
        llvm::Value* result = ir_.CreateFPTrunc(ir_.CreateFMul(estrin(f, exp2_terms), scale), type);
        result = ir_.CreateSelect(ir_.CreateOr(ir_.CreateFCmpOLT(x, constant(0)), ir_.CreateFCmpUNO(x, y)),
                                  constant(NAN), result);
        return ir_.CreateSelect(ir_.CreateOr(ir_.CreateFCmpOEQ(y, constant(0)), ir_.CreateFCmpOEQ(x, constant(1))),
                                constant(1), result);
    }

    // Double transcendental functions keep the reduced argument and leading terms as double-doubles
    // (pairs whose exact sum is the value), built from the error-free transformations below.
    struct Pair {
        llvm::Value* hi;

        llvm::Value* lo;
    };

    Pair two_product(llvm::Value* a, llvm::Value* b) {
        auto* hi = ir_.CreateFMul(a, b);
        return {hi, fma(a, b, ir_.CreateFNeg(hi))};
    }

    Pair two_sum(llvm::Value* a, llvm::Value* b) {
        auto* s = ir_.CreateFAdd(a, b);
        auto* b_part = ir_.CreateFSub(s, a);
        return {s, ir_.CreateFAdd(ir_.CreateFSub(a, ir_.CreateFSub(s, b_part)), ir_.CreateFSub(b, b_part))};
    }

    // Exact when |a| >= |b| or a = 0.
    Pair fast_two_sum(llvm::Value* a, llvm::Value* b) {
        auto* s = ir_.CreateFAdd(a, b);
        return {s, ir_.CreateFSub(b, ir_.CreateFSub(s, a))};
    }

    llvm::Value* sine_double(llvm::Value* x, bool cosine) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        // Fits cover |r| <= 0.786: pi/4 plus the quadrant rounding slack up to |x| = 2^40.
        static constexpr double sin_terms[] = {-0x1.5555555555548p-3, 0x1.111111110f730p-7,   -0x1.a01a019be9217p-13,
                                               0x1.71de35552b514p-19, -0x1.ae5e4b83e0f08p-26, 0x1.5d8b55929778bp-33};
        static constexpr double cos_terms[] = {0x1.555555555554bp-5,   -0x1.6c16c16c15009p-10, 0x1.a01a019c8d25cp-16,
                                               -0x1.27e4f7efabb1cp-22, 0x1.1ee9da45ca970p-29,  -0x1.8fa59dc43a59cp-37};
        auto* t = fma(x, constant(0x1.45f306dc9c883p-1), constant(0x1.8p52));
        auto* q = ir_.CreateFSub(t, constant(0x1.8p52));
        llvm::Value* quadrant = as_integers(t);
        if (cosine)
            quadrant = ir_.CreateAdd(quadrant, integers(type, 1, 64));
        // r = x - q*pi/2 with pi/2 in three double parts: x - q*C1 is exact, q*C2 is kept whole, and the
        // truncation q*2^-163 stays far below the smallest |r| (about 2^-60.5) of any |x| <= 2^40.
        auto* r1 = fma(q, constant(-0x1.921fb54442d18p+0), x);
        const auto middle = two_product(q, constant(0x1.1a62633145c07p-54));
        const auto head = two_sum(r1, ir_.CreateFNeg(middle.hi));
        const auto r = fast_two_sum(head.hi, ir_.CreateFSub(ir_.CreateFSub(head.lo, middle.lo),
                                                            ir_.CreateFMul(q, constant(-0x1.f1976b7ed8fbcp-110))));
        // Beyond 2^40 the reduction is meaningless; the clamps keep the result finite. NaN passes through.
        auto* rh = minimum(maximum(r.hi, constant(-0.786)), constant(0.786));
        auto* rl = minimum(maximum(r.lo, constant(-0x1p-53)), constant(0x1p-53));
        auto* z = ir_.CreateFMul(rh, rh);
        // sin(rh + rl) = rh + rh^3 P(z) + rl (1 - z/2); zero returns x itself to keep its sign.
        auto* sine_tail =
            fma(ir_.CreateFMul(rh, z), polynomial(z, sin_terms), fma(ir_.CreateFMul(z, constant(-0.5)), rl, rl));
        auto* s = ir_.CreateSelect(ir_.CreateFCmpOEQ(x, constant(0)), x, ir_.CreateFAdd(rh, sine_tail));
        // cos(rh + rl) = 1 - rh^2/2 + z^2 Q(z) - rh rl; w + (1 - w - half) recovers the rounding of 1 - half.
        auto* half = ir_.CreateFMul(z, constant(0.5));
        auto* w = ir_.CreateFSub(constant(1), half);
        auto* square_error = fma(rh, rh, ir_.CreateFNeg(z));
        auto* cosine_tail =
            ir_.CreateFAdd(ir_.CreateFSub(ir_.CreateFSub(constant(1), w), half),
                           fma(ir_.CreateFMul(z, z), polynomial(z, cos_terms),
                               fma(rh, ir_.CreateFNeg(rl), ir_.CreateFMul(square_error, constant(-0.5)))));
        auto* c = ir_.CreateFAdd(w, cosine_tail);
        auto* odd = ir_.CreateICmpNE(ir_.CreateAnd(quadrant, integers(type, 1, 64)), integers(type, 0, 64));
        auto* sign = ir_.CreateShl(ir_.CreateAnd(quadrant, integers(type, 2, 64)), integers(type, 62, 64));
        return ir_.CreateBitCast(ir_.CreateXor(as_integers(ir_.CreateSelect(odd, c, s)), sign), type);
    }

    // e^(x + low) for x in [-746, 710]: 2^k e^r with r = x - k ln2 kept as a double-double.
    llvm::Value* exponential_double(llvm::Value* x, llvm::Value* low) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {0.5,
                                           0x1.5555555555556p-3,
                                           0x1.5555555555558p-5,
                                           0x1.1111111110d5ep-7,
                                           0x1.6c16c16c14e67p-10,
                                           0x1.a01a01a1e25fcp-13,
                                           0x1.a01a01ae786eap-16,
                                           0x1.71de341e6ae1dp-19,
                                           0x1.27e4c90ddc825p-22,
                                           0x1.ae784a274513ap-26,
                                           0x1.1f8d2d60a7f33p-29,
                                           0x1.46f16b92e082cp-33};
        auto* t = fma(x, constant(0x1.71547652b82fep+0), constant(0x1.8p52));
        auto* n = ir_.CreateFSub(t, constant(0x1.8p52));
        auto* k = ir_.CreateSub(as_integers(t), integers(type, 0x4338000000000000, 64));
        // The high part of ln2 has 32 significant bits, so n times it is exact and x - n*high cancels exactly.
        auto* r1 = fma(n, constant(-0x1.62e42fee00000p-1), x);
        const auto product = two_product(n, constant(0x1.a39ef35793c76p-33));
        const auto r = two_sum(r1, ir_.CreateFNeg(product.hi));
        llvm::Value* rl = ir_.CreateFSub(r.lo, product.lo);
        if (low)
            rl = ir_.CreateFAdd(rl, low);
        // e^(rh + rl) = (1 + q)(1 + rl) with q = e^rh - 1.
        auto* q = fma(ir_.CreateFMul(r.hi, r.hi), estrin(r.hi, terms), r.hi);
        auto* p = ir_.CreateFAdd(constant(1), ir_.CreateFAdd(q, fma(rl, q, rl)));
        // 2^k in two exact factors, so subnormal results round once; k + 2048 is never negative.
        auto* half = ir_.CreateSub(ir_.CreateLShr(ir_.CreateAdd(k, integers(type, 2048, 64)), integers(type, 1, 64)),
                                   integers(type, 1024, 64));
        const auto power_of_two = [&](llvm::Value* e) {
            return ir_.CreateBitCast(ir_.CreateShl(ir_.CreateAdd(e, integers(type, 1023, 64)), integers(type, 52, 64)),
                                     type);
        };
        return ir_.CreateFMul(ir_.CreateFMul(p, power_of_two(half)), power_of_two(ir_.CreateSub(k, half)));
    }

    // x > 0 as 2^e m with m in [sqrt(1/2), sqrt(2)); subnormals are scaled into the normal range first.
    std::pair<llvm::Value*, llvm::Value*> decompose_double(llvm::Value* x) {
        auto* type = x->getType();
        constexpr std::int64_t sqrt_half = 0x3fe6a09e667f3bcd;
        auto* subnormal = ir_.CreateFCmpOLT(x, llvm::ConstantFP::get(type, 0x1p-1022));
        auto* scaled = ir_.CreateSelect(subnormal, ir_.CreateFMul(x, llvm::ConstantFP::get(type, 0x1p54)), x);
        auto* bits = ir_.CreateAdd(as_integers(scaled), integers(type, 0x3ff0000000000000 - sqrt_half, 64));
        auto* exponent = ir_.CreateSub(
            ir_.CreateTrunc(ir_.CreateLShr(bits, integers(type, 52, 64)), type->getWithNewType(ir_.getInt32Ty())),
            ir_.CreateSelect(subnormal, integers(type, 1023 + 54), integers(type, 1023)));
        auto* m = ir_.CreateBitCast(
            ir_.CreateAdd(ir_.CreateAnd(bits, integers(type, 0xfffffffffffff, 64)), integers(type, sqrt_half, 64)),
            type);
        return {ir_.CreateSIToFP(exponent, type), m};
    }

    // log(m) = f - (f^2/2 - s (f^2/2 + R)) with f = m - 1, s = f / (2 + f) and R = 2s^2/3 + 2s^4/5 + ...
    llvm::Value* logarithm_double(llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {0x1.5555555555592p-1, 0x1.999999997fed7p-2, 0x1.24924941e74dfp-2,
                                           0x1.c71c520e0fc89p-3, 0x1.74663e71e84f2p-3, 0x1.39a1bc704cd63p-3,
                                           0x1.2f05a786558fap-3};
        const auto [e, m] = decompose_double(x);
        auto* f = ir_.CreateFSub(m, constant(1));
        auto* s = ir_.CreateFDiv(f, ir_.CreateFAdd(constant(2), f));
        auto* z = ir_.CreateFMul(s, s);
        auto* half_square = ir_.CreateFMul(ir_.CreateFMul(f, constant(0.5)), f);
        auto* correction = fma(s, ir_.CreateFAdd(half_square, ir_.CreateFMul(z, polynomial(z, terms))),
                               ir_.CreateFMul(e, constant(0x1.a39ef35793c76p-33)));
        // The high part of ln2 has 32 significant bits, so e times it is exact.
        auto* result =
            fma(e, constant(0x1.62e42fee00000p-1), ir_.CreateFSub(f, ir_.CreateFSub(half_square, correction)));
        auto* special = ir_.CreateSelect(ir_.CreateFCmpOEQ(x, constant(0)), constant(-INFINITY),
                                         ir_.CreateSelect(ir_.CreateFCmpOLT(x, constant(0)), constant(NAN), x));
        return ir_.CreateSelect(
            ir_.CreateAnd(ir_.CreateFCmpOGT(x, constant(0)), ir_.CreateFCmpOLT(x, constant(INFINITY))), result,
            special);
    }

    // atan(u / v) for 0 <= u <= v from the nearest of atan(c), c in {0, 1/4, 1/2, 1}: the reduced ratio
    // (u - c v) / (v + c u) stays below 0.163, u - c v is exact and the division keeps its remainder.
    llvm::Value* arctangent_double(llvm::Value* y, llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {-0x1.555555555553ep-2, 0x1.999999998e94cp-3,  -0x1.249249167c3ddp-3,
                                           0x1.c71c60b028052p-4,  -0x1.74578976aa206p-4, 0x1.3a14d4e1d8978p-4,
                                           -0x1.f2576a8a15e55p-5};
        auto* ay = intrinsic(llvm::Intrinsic::fabs, y);
        auto* ax = intrinsic(llvm::Intrinsic::fabs, x);
        auto* swap = ir_.CreateFCmpOGT(ay, ax);
        llvm::Value* u = ir_.CreateSelect(swap, ax, ay);
        llvm::Value* v = ir_.CreateSelect(swap, ay, ax);
        // Infinities and zeros reduce to the same ratio's limit.
        auto* infinite = ir_.CreateFCmpOEQ(v, constant(INFINITY));
        u = ir_.CreateSelect(infinite,
                             ir_.CreateSelect(ir_.CreateFCmpOEQ(u, constant(INFINITY)), constant(1), constant(0)), u);
        v = ir_.CreateSelect(ir_.CreateOr(infinite, ir_.CreateFCmpOEQ(v, constant(0))), constant(1), v);
        // Power-of-two scaling keeps v + c u finite and the division remainder accurate wherever it still matters.
        auto* scale = ir_.CreateSelect(
            ir_.CreateFCmpOGT(v, constant(0x1p1000)), constant(0x1p-8),
            ir_.CreateSelect(ir_.CreateFCmpOLT(v, constant(0x1p-900)), constant(0x1p100), constant(1)));
        u = ir_.CreateFMul(u, scale);
        v = ir_.CreateFMul(v, scale);
        auto* quarter = ir_.CreateFCmpOGE(ir_.CreateFMul(u, constant(8)), v);
        auto* middle = ir_.CreateFCmpOGE(u, ir_.CreateFMul(v, constant(0.37)));
        auto* upper = ir_.CreateFCmpOGE(u, ir_.CreateFMul(v, constant(0.7208)));
        const auto pick = [&](double one, double half, double quarter_value) {
            return ir_.CreateSelect(upper, constant(one),
                                    ir_.CreateSelect(middle, constant(half),
                                                     ir_.CreateSelect(quarter, constant(quarter_value), constant(0))));
        };
        auto* c = pick(1, 0.5, 0.25);
        auto* numerator = ir_.CreateFSub(u, ir_.CreateFMul(c, v));
        const auto denominator = fast_two_sum(v, ir_.CreateFMul(c, u));
        auto* qh = ir_.CreateFDiv(numerator, denominator.hi);
        auto* ql = ir_.CreateFDiv(
            ir_.CreateFSub(fma(ir_.CreateFNeg(qh), denominator.hi, numerator), ir_.CreateFMul(qh, denominator.lo)),
            denominator.hi);
        auto* z = ir_.CreateFMul(qh, qh);
        const auto a = fast_two_sum(pick(0x1.921fb54442d18p-1, 0x1.dac670561bb4fp-2, 0x1.f5b75f92c80ddp-3), qh);
        auto* a_low = ir_.CreateFAdd(
            a.lo, ir_.CreateFAdd(pick(0x1.1a62633145c07p-55, 0x1.a2b7f222f65e2p-56, 0x1.8ab6e3cf7afbdp-57),
                                 fma(ir_.CreateFMul(qh, z), polynomial(z, terms), ql)));
        // The angle is base +- a: 0 + a, pi/2 -+ a for the swap, pi - a for negative x, as a double-double sum.
        auto* negative = ir_.CreateICmpSLT(as_integers(x), integers(type, 0, 64));
        auto* flip = ir_.CreateXor(swap, negative);
        auto* base = ir_.CreateSelect(swap, constant(0x1.921fb54442d18p+0),
                                      ir_.CreateSelect(negative, constant(0x1.921fb54442d18p+1), constant(0)));
        auto* base_low = ir_.CreateSelect(swap, constant(0x1.1a62633145c07p-54),
                                          ir_.CreateSelect(negative, constant(0x1.1a62633145c07p-53), constant(0)));
        const auto angle = fast_two_sum(base, ir_.CreateSelect(flip, ir_.CreateFNeg(a.hi), a.hi));
        auto* result = ir_.CreateFAdd(
            angle.hi,
            ir_.CreateFAdd(angle.lo, ir_.CreateFAdd(base_low, ir_.CreateSelect(flip, ir_.CreateFNeg(a_low), a_low))));
        result = ir_.CreateIntrinsic(llvm::Intrinsic::copysign, {type}, {result, y});
        return ir_.CreateSelect(ir_.CreateFCmpUNO(x, y), ir_.CreateFAdd(x, y), result);
    }

    // exp(y log|x|) with log|x| to about 2^-68 relative, so |y log x| up to 745 still rounds within an ULP.
    llvm::Value* power_double(llvm::Value* x, llvm::Value* y) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        static constexpr double terms[] = {0x1.24924924927cep-2, 0x1.c71c71c659e15p-3, 0x1.745d17d77da5ap-3,
                                           0x1.3b13782bbdb2fp-3, 0x1.111dcfd891ec9p-3, 0x1.de9e4568b0ee8p-4,
                                           0x1.e7e20b08b6a65p-4};
        auto* a = intrinsic(llvm::Intrinsic::fabs, x);
        const auto [e, m] = decompose_double(a);
        // log(m) = 2s + s^3 (2/3 + 2s^2/5 + s^4 P(s^2)) with s = f / (2 + f) as a double-double; every term
        // above s^7 P keeps 106 bits because y multiplies its rounding.
        auto* f = ir_.CreateFSub(m, constant(1));
        const auto d = fast_two_sum(constant(2), f);
        auto* sh = ir_.CreateFDiv(f, d.hi);
        auto* sl = ir_.CreateFDiv(ir_.CreateFSub(fma(ir_.CreateFNeg(sh), d.hi, f), ir_.CreateFMul(sh, d.lo)), d.hi);
        const auto z = two_product(sh, sh);
        auto* zl = fma(ir_.CreateFMul(sh, constant(2)), sl, z.lo);
        const auto cube = two_product(sh, z.hi);
        auto* cube_low = fma(sh, zl, fma(sl, z.hi, cube.lo));
        const auto fifth = two_product(z.hi, constant(0x1.999999999999ap-2));
        auto* fifth_low =
            fma(z.hi, constant(-0x1.999999999999ap-56), fma(zl, constant(0x1.999999999999ap-2), fifth.lo));
        const auto g = fast_two_sum(constant(0x1.5555555555555p-1), fifth.hi);
        auto* g_low = ir_.CreateFAdd(
            ir_.CreateFAdd(g.lo, constant(0x1.5555555555555p-55)),
            ir_.CreateFAdd(fifth_low, ir_.CreateFMul(ir_.CreateFMul(z.hi, z.hi), polynomial(z.hi, terms))));
        const auto tail = two_product(cube.hi, g.hi);
        auto* tail_low = fma(cube.hi, g_low, fma(cube_low, g.hi, tail.lo));
        const auto scaled = two_product(e, constant(0x1.62e42fefa39efp-1));
        const auto lead = two_sum(scaled.hi, ir_.CreateFMul(sh, constant(2)));
        const auto sum = fast_two_sum(lead.hi, tail.hi);
        auto* low = ir_.CreateFAdd(
            ir_.CreateFAdd(ir_.CreateFAdd(lead.lo, sum.lo), ir_.CreateFAdd(ir_.CreateFMul(sl, constant(2)), tail_low)),
            fma(e, constant(0x1.abc9e3b39803fp-56), scaled.lo));
        const auto log_x = fast_two_sum(sum.hi, low);
        auto* finite = ir_.CreateAnd(ir_.CreateFCmpOGT(a, constant(0)), ir_.CreateFCmpOLT(a, constant(INFINITY)));
        auto* log_hi = ir_.CreateSelect(finite, log_x.hi,
                                        ir_.CreateSelect(ir_.CreateFCmpOEQ(a, constant(0)), constant(-INFINITY), a));
        auto* log_lo = ir_.CreateSelect(finite, log_x.lo, constant(0));
        // t = y log|x| as a double-double; e^t overflows above 710 and rounds to zero below -746. NaN passes through.
        const auto t = two_product(y, log_hi);
        auto* t_low = ir_.CreateSelect(ir_.CreateFCmpOLT(intrinsic(llvm::Intrinsic::fabs, t.hi), constant(746)),
                                       fma(y, log_lo, t.lo), constant(0));
        llvm::Value* result = exponential_double(minimum(maximum(t.hi, constant(-746)), constant(710)), t_low);
        result = ir_.CreateSelect(ir_.CreateOr(ir_.CreateFCmpOLT(x, constant(0)), ir_.CreateFCmpUNO(x, y)),
                                  constant(NAN), result);
        return ir_.CreateSelect(ir_.CreateOr(ir_.CreateFCmpOEQ(y, constant(0)), ir_.CreateFCmpOEQ(x, constant(1))),
                                constant(1), result);
    }

    llvm::Value* dot(llvm::Value* a, llvm::Value* b) {
        auto* products = ir_.CreateFMul(a, b);
        auto* result = lane(products, 0);
        const auto count = llvm::cast<llvm::FixedVectorType>(a->getType())->getNumElements();
        for (unsigned i = 1; i < count; ++i)
            result = ir_.CreateFAdd(result, lane(products, i));
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
        auto* limit = llvm::ConstantFP::get(scalar, scalar->isDoubleTy() ? std::numeric_limits<double>::max()
                                                                         : std::numeric_limits<float>::max());
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
        if (normalize)
            return ir_.CreateFDiv(scaled, ir_.CreateVectorSplat(count, ir_.CreateSelect(valid, magnitude, one)));
        auto* result = ir_.CreateFMul(maximum, magnitude);
        result = ir_.CreateSelect(finite, result, llvm::ConstantFP::getInfinity(scalar));
        auto* infinity = llvm::ConstantFP::getInfinity(scalar);
        return ir_.CreateSelect(ir_.CreateAnd(has_nan, ir_.CreateFCmpONE(maximum, infinity)),
                                llvm::ConstantFP::getNaN(scalar), result);
    }

    llvm::Value* fast_rsqrt_positive(llvm::Value* x) {
        auto* type = x->getType();
        if (!type->getScalarType()->isDoubleTy()) {
            llvm::FastMathFlags flags;
            flags.setAllowReciprocal();
            flags.setApproxFunc();
            flags.setAllowReassoc();
            flags.setAllowContract();
            auto* root = llvm::cast<llvm::CallInst>(intrinsic(llvm::Intrinsic::sqrt, x));
            root->setFastMathFlags(flags);
            auto* reciprocal = llvm::cast<llvm::Instruction>(ir_.CreateFDiv(llvm::ConstantFP::get(type, 1), root));
            reciprocal->setFastMathFlags(flags);
            return reciprocal;
        }
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        auto* magic = integers(type, 0x5fe6eb50c7b537a9LL, 64);
        auto* y = ir_.CreateBitCast(ir_.CreateSub(magic, ir_.CreateLShr(as_integers(x), integers(type, 1, 64))), type);
        auto* neg_half_x = ir_.CreateFMul(x, constant(-0.5));
        for (int step = 0; step < 3; ++step)
            y = ir_.CreateFMul(y, fma(ir_.CreateFMul(neg_half_x, y), y, constant(1.5)));
        return y;
    }

    static llvm::Constant* min_normal(llvm::Type* type) {
        return llvm::ConstantFP::get(type, type->getScalarType()->isDoubleTy() ? std::numeric_limits<double>::min()
                                                                               : std::numeric_limits<float>::min());
    }

    llvm::Value* fast_rsqrt(llvm::Value* x) {
        auto* type = x->getType();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(type, value); };
        auto* infinity = constant(INFINITY);
        auto* signed_infinity = ir_.CreateIntrinsic(llvm::Intrinsic::copysign, {type}, {infinity, x});
        auto* minimum = min_normal(type);
        auto* normal = ir_.CreateAnd(ir_.CreateFCmpOGE(x, minimum), ir_.CreateFCmpOLT(x, infinity));
        auto* refined = fast_rsqrt_positive(ir_.CreateSelect(normal, x, constant(1)));
        auto* special =
            ir_.CreateSelect(ir_.CreateFCmpOEQ(x, infinity), constant(0),
                             ir_.CreateSelect(ir_.CreateFCmpOGE(x, constant(0)), signed_infinity, constant(NAN)));
        return ir_.CreateSelect(normal, refined, special);
    }

    llvm::Value* fast_normalize(llvm::Value* input) {
        auto* type = llvm::cast<llvm::FixedVectorType>(input->getType());
        auto* scalar = type->getElementType();
        const auto count = type->getNumElements();
        auto* one = llvm::ConstantFP::get(scalar, 1);
        auto* infinity = llvm::ConstantFP::getInfinity(scalar);
        auto* squared = dot(input, input);
        auto* valid =
            ir_.CreateAnd(ir_.CreateFCmpOGE(squared, min_normal(scalar)), ir_.CreateFCmpOLT(squared, infinity));
        auto* inv_length = fast_rsqrt_positive(ir_.CreateSelect(valid, squared, one));
        auto* safe = ir_.CreateSelect(valid, input, llvm::Constant::getNullValue(type));
        return ir_.CreateFMul(safe, ir_.CreateVectorSplat(count, inv_length));
    }

    llvm::Value* slerp01(llvm::Value* a, llvm::Value* b, llvm::Value* t) {
        auto* qtype = llvm::cast<llvm::FixedVectorType>(a->getType());
        auto* scalar = qtype->getElementType();
        const bool wide = scalar->isDoubleTy();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(scalar, value); };
        auto* clamped = maximum(constant(0), minimum(t, constant(1)));
        auto* u = ir_.CreateFSub(constant(1), clamped);
        auto* raw_cosine = dot(a, b);
        auto* flip = ir_.CreateFCmpOLT(raw_cosine, constant(0));
        auto* target = ir_.CreateSelect(flip, ir_.CreateFNeg(b), b);
        auto* cosine = ir_.CreateSelect(flip, ir_.CreateFNeg(raw_cosine), raw_cosine);
        const double threshold = wide ? (1.0 - 1e-10) : static_cast<double>(1.0f - 1e-4f);
        auto* near_collinear = ir_.CreateFCmpOGT(cosine, constant(threshold));
        auto* linear = ir_.CreateFAdd(ir_.CreateFMul(a, ir_.CreateVectorSplat(4, u)),
                                      ir_.CreateFMul(target, ir_.CreateVectorSplat(4, clamped)));
        auto* sin_sq = maximum(constant(0), fma(ir_.CreateFNeg(cosine), cosine, constant(1)));
        auto* safe_norm =
            intrinsic(llvm::Intrinsic::sqrt, ir_.CreateSelect(near_collinear, dot(linear, linear), sin_sq));
        auto* sine_val = ir_.CreateSelect(near_collinear, constant(0), safe_norm);
        auto* angle = wide ? arctangent_double(sine_val, cosine) : arctangent(sine_val, cosine);
        // <2 x float> cues SubtargetVectorScalarizerPass so quaternion loops vectorize 8-wide in AVX2,
        // while single calls re-vectorize via SLP and <2 x double> evaluates both sines in one XMM pass.
        auto* pair_type = llvm::FixedVectorType::get(scalar, 2);
        llvm::Value* angles = llvm::PoisonValue::get(pair_type);
        angles = ir_.CreateInsertElement(angles, ir_.CreateFMul(u, angle), std::uint64_t{0});
        angles = ir_.CreateInsertElement(angles, ir_.CreateFMul(clamped, angle), std::uint64_t{1});
        auto* sines = wide ? sine_double(angles, false) : sine(angles, false);
        auto* wa = ir_.CreateShuffleVector(sines, {0, 0, 0, 0});
        auto* wb = ir_.CreateShuffleVector(sines, {1, 1, 1, 1});
        auto* numerator = ir_.CreateSelect(near_collinear, linear, fma(a, wa, ir_.CreateFMul(target, wb)));
        return ir_.CreateFDiv(numerator, ir_.CreateVectorSplat(4, safe_norm));
    }

    llvm::Value* fast_slerp01(llvm::Value* a, llvm::Value* b, llvm::Value* t) {
        auto* qtype = llvm::cast<llvm::FixedVectorType>(a->getType());
        auto* scalar = qtype->getElementType();
        const bool wide = scalar->isDoubleTy();
        const auto constant = [&](double value) { return llvm::ConstantFP::get(scalar, value); };
        auto* clamped = maximum(constant(0), minimum(t, constant(1)));
        auto* u = ir_.CreateFSub(constant(1), clamped);
        auto* raw_cosine = dot(a, b);
        auto* flip = ir_.CreateFCmpOLT(raw_cosine, constant(0));
        auto* target = ir_.CreateSelect(flip, ir_.CreateFNeg(b), b);
        auto* cosine = ir_.CreateSelect(flip, ir_.CreateFNeg(raw_cosine), raw_cosine);
        auto* z =
            ir_.CreateSelect(ir_.CreateFCmpOGT(cosine, constant(1)), constant(0), ir_.CreateFSub(constant(1), cosine));
        auto* pair_type = llvm::FixedVectorType::get(scalar, 2);
        llvm::Value* s = llvm::PoisonValue::get(pair_type);
        s = ir_.CreateInsertElement(s, u, std::uint64_t{0});
        s = ir_.CreateInsertElement(s, clamped, std::uint64_t{1});
        auto* s2 = ir_.CreateFMul(s, s);
        llvm::Value* poly = nullptr;
        if (!wide) {
            static constexpr double c0[] = {-0x1.5597fap-2, -0x1.06ea0cp-3, -0x1.581d72p-4, 0x1.f29a3cp-6,
                                            -0x1.c41f92p-5};
            static constexpr double c1[] = {-0x1.bf9492p-11, 0x1.67037p-5, -0x1.948e78p-7, 0x1.6b391ep-5};
            static constexpr double c2[] = {-0x1.12b68p-10, 0x1.65b8dcp-8, -0x1.1bd13cp-7};
            auto* p0 = ir_.CreateVectorSplat(2, polynomial(z, c0));
            auto* p1 = ir_.CreateVectorSplat(2, polynomial(z, c1));
            auto* p2 = ir_.CreateVectorSplat(2, polynomial(z, c2));
            poly = fma(s2, fma(s2, p2, p1), p0);
        } else {
            static constexpr double c0[] = {-0x1.55556e5087672p-2, -0x1.1106eee24dc3bp-3, -0x1.d6bc55d25c563p-5,
                                            -0x1.77e80b37c0b13p-6, -0x1.634b85c2b89d3p-6, 0x1.345eda48891bap-6,
                                            -0x1.2670d071ac7bdp-5, 0x1.8ef7d6468286bp-6,  -0x1.304f363d0c806p-7};
            static constexpr double c1[] = {-0x1.82e7604a3c924p-20, 0x1.1188acce1be50p-5,  0x1.4640a775ca28ep-6,
                                            0x1.e9f53308a08bep-7,   -0x1.ea01c405b9f51p-8, 0x1.8cbdc98df78b7p-6,
                                            -0x1.27ace869c9084p-6,  0x1.0d69dd3f8496ep-7};
            static constexpr double c2[] = {-0x1.125d965a3dee4p-19, 0x1.ed6b7e46b09fdp-15, -0x1.1764fb499fad1p-9,
                                            0x1.056b857d13e06p-10,  -0x1.6e763331d2ab3p-8, 0x1.351c9fe1cf82dp-8,
                                            -0x1.511e13b8c042ap-9};
            static constexpr double c3[] = {-0x1.3bbf40807e05fp-20, 0x1.9bc814adb423bp-16,  -0x1.50fba5662a71fp-13,
                                            0x1.06311f349fab1p-11,  -0x1.246b1d81fd286p-11, 0x1.70cace8f38834p-12};
            static constexpr double c4[] = {-0x1.7faf9b6e594d7p-22, 0x1.347c850fd5f82p-18, -0x1.380eea723bbc5p-16,
                                            0x1.0091e063c6f80p-15, -0x1.495a4a063e900p-16};
            auto* p0 = ir_.CreateVectorSplat(2, polynomial(z, c0));
            auto* p1 = ir_.CreateVectorSplat(2, polynomial(z, c1));
            auto* p2 = ir_.CreateVectorSplat(2, polynomial(z, c2));
            auto* p3 = ir_.CreateVectorSplat(2, polynomial(z, c3));
            auto* p4 = ir_.CreateVectorSplat(2, polynomial(z, c4));
            poly = fma(s2, fma(s2, fma(s2, fma(s2, p4, p3), p2), p1), p0);
        }
        auto* factor = ir_.CreateFMul(ir_.CreateFMul(s, ir_.CreateFSub(s2, llvm::ConstantFP::get(pair_type, 1))),
                                      ir_.CreateVectorSplat(2, z));
        auto* weights = fma(factor, poly, s);
        auto* wa = ir_.CreateShuffleVector(weights, {0, 0, 0, 0});
        auto* wb = ir_.CreateShuffleVector(weights, {1, 1, 1, 1});
        return fma(a, wa, ir_.CreateFMul(target, wb));
    }

    llvm::Value* quaternion_product(llvm::Value* a, llvm::Value* b) {
        auto* t0 = ir_.CreateFMul(ir_.CreateShuffleVector(a, {3, 3, 3, 3}), b);
        auto* t1 = ir_.CreateFMul(ir_.CreateShuffleVector(a, {0, 0, 0, 0}), ir_.CreateShuffleVector(b, {3, 2, 1, 0}));
        auto* s1 = ir_.CreateShuffleVector(ir_.CreateFAdd(t0, t1), ir_.CreateFSub(t0, t1), {0, 5, 2, 7});
        auto* t2 = ir_.CreateFMul(ir_.CreateShuffleVector(a, {1, 1, 1, 1}), ir_.CreateShuffleVector(b, {2, 3, 0, 1}));
        auto* s2 = ir_.CreateShuffleVector(ir_.CreateFAdd(s1, t2), ir_.CreateFSub(s1, t2), {0, 1, 6, 7});
        auto* t3 = ir_.CreateFMul(ir_.CreateShuffleVector(a, {2, 2, 2, 2}), ir_.CreateShuffleVector(b, {1, 0, 3, 2}));
        return ir_.CreateShuffleVector(ir_.CreateFSub(s2, t3), ir_.CreateFAdd(s2, t3), {0, 5, 6, 3});
    }

    llvm::Value* matrix_product(llvm::Value* a, llvm::Value* b) {
        auto* a0 = ir_.CreateShuffleVector(a, {0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 12});
        auto* b0 = ir_.CreateShuffleVector(b, {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3});
        auto* a1 = ir_.CreateShuffleVector(a, {1, 1, 1, 1, 5, 5, 5, 5, 9, 9, 9, 9, 13, 13, 13, 13});
        auto* b1 = ir_.CreateShuffleVector(b, {4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7, 4, 5, 6, 7});
        auto* a2 = ir_.CreateShuffleVector(a, {2, 2, 2, 2, 6, 6, 6, 6, 10, 10, 10, 10, 14, 14, 14, 14});
        auto* b2 = ir_.CreateShuffleVector(b, {8, 9, 10, 11, 8, 9, 10, 11, 8, 9, 10, 11, 8, 9, 10, 11});
        auto* a3 = ir_.CreateShuffleVector(a, {3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15});
        auto* b3 = ir_.CreateShuffleVector(b, {12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15});
        auto* acc = ir_.CreateFMul(a0, b0);
        acc = ir_.CreateFAdd(acc, ir_.CreateFMul(a1, b1));
        acc = ir_.CreateFAdd(acc, ir_.CreateFMul(a2, b2));
        return ir_.CreateFAdd(acc, ir_.CreateFMul(a3, b3));
    }

    llvm::IRBuilder<>& ir_;

    llvm::Module& module_;
};

}

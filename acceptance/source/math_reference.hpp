#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

extern "C" {
double cr_sin(double);
double cr_cos(double);
double cr_exp(double);
double cr_log(double);
double cr_atan2(double, double);
double cr_pow(double, double);
}

namespace atelier::math_reference {

// CORE-MATH's correctly rounded binary64 functions, with the source contract's pow: y = 0 or x = 1
// gives 1, NaN and x < 0 give NaN, and -0 counts as +0.
struct Function {
    std::string_view name;
    bool binary;
    double (*value)(double, double);
};
inline constexpr Function functions[] = {
    {"sin", false, [](double x, double) { return cr_sin(x); }},
    {"cos", false, [](double x, double) { return cr_cos(x); }},
    {"exp", false, [](double x, double) { return cr_exp(x); }},
    {"log", false, [](double x, double) { return cr_log(x); }},
    {"atan2", true, [](double y, double x) { return cr_atan2(y, x); }},
    {"pow", true,
     [](double x, double y) {
         if (y == 0 || x == 1)
             return 1.0;
         if (std::isnan(x) || std::isnan(y) || x < 0)
             return std::numeric_limits<double>::quiet_NaN();
         return cr_pow(std::fabs(x), y);
     }},
};
// sin and cos are accurate up to this magnitude; beyond it they only promise a finite result of magnitude at most 1.
inline constexpr double periodic_limit = 0x1p40;

// Units in the last place between a result and the correctly rounded value, counting subnormal steps;
// a zero of the wrong sign never matches, and NaN matches only NaN.
inline std::uint64_t ulp_distance(double result, double reference) {
    if (std::isnan(result) || std::isnan(reference))
        return std::isnan(result) && std::isnan(reference) ? 0 : UINT64_MAX;
    if (result == 0 && reference == 0)
        return std::signbit(result) == std::signbit(reference) ? 0 : UINT64_MAX;
    const auto key = [](double value) {
        const auto bits = std::bit_cast<std::int64_t>(value);
        return bits < 0 ? std::numeric_limits<std::int64_t>::min() - bits : bits;
    };
    const auto a = key(result), b = key(reference);
    return a < b ? static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a)
                 : static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b);
}

// One random argument pair: a third of the draws are uniform bit patterns, the rest follow where each
// function's reduction or special handling changes.
template <class Random>
std::pair<double, double> random_arguments(std::string_view name, Random& random) {
    const auto unit = [&] { return static_cast<double>(random() >> 11) * 0x1p-53; };
    const auto uniform = [&](double low, double high) { return low + (high - low) * unit(); };
    const auto pattern = [&](std::uint64_t exponent_limit) {
        // A random sign and mantissa with a biased exponent below the limit: every binade equally often.
        const auto bits = random();
        return std::bit_cast<double>((bits & 0x800fffffffffffffull) | (bits >> 11) % exponent_limit << 52);
    };
    const auto kind = random() % 3;
    if (name == "sin" || name == "cos") {
        if (kind == 0)
            return {pattern(1023 + 40), 0};
        if (kind == 1)
            return {uniform(-1e4, 1e4), 0};
        // Near k pi/2 for k up to 2^40 / (pi/2), where the reduced argument cancels most.
        const double k = std::floor(std::exp2(uniform(0, 39.3)));
        const double x = std::fma(k, 0x1.921fb54442d18p+0, k * 0x1.1a62633145c07p-54);
        return {
            std::bit_cast<double>(std::bit_cast<std::int64_t>(x) + static_cast<std::int64_t>(random() % 2049) - 1024),
            0};
    }
    if (name == "exp")
        return {kind == 0 ? pattern(1023 + 10) : uniform(-746, 710), 0};
    if (name == "log")
        return {kind == 0 ? std::fabs(pattern(2047)) : kind == 1 ? uniform(0.5, 2) : 1 + uniform(-0x1p-20, 0x1p-20), 0};
    if (name == "atan2") {
        const double x = pattern(2047);
        return {kind == 0 ? pattern(2047) : x * uniform(-2, 2), x};
    }
    // pow: |y log x| spread over [-750, 712], which covers overflow, subnormal results and underflow,
    // from any x, from x near 1 and from x within a factor of 2.
    const double x = kind == 0   ? std::fabs(pattern(2047))
                     : kind == 1 ? uniform(0.5, 2)
                                 : 1 + uniform(-0x1p-24, 0x1p-24);
    const double log_x = std::log(x);
    return {x, log_x == 0 ? uniform(-4, 4) : uniform(-750, 712) / log_x};
}

}

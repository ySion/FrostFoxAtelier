#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

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
    const auto a = key(result);
    const auto b = key(reference);
    return a < b ? static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a)
                 : static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b);
}

}

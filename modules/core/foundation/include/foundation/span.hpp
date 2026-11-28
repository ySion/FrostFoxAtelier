#pragma once

#include "memory/fatal.hpp"

#include <algorithm>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>

namespace atelier {

template <class T>
class Span;

namespace detail {
template <class T>
inline constexpr bool is_atelier_span_v = false;

template <class T>
inline constexpr bool is_atelier_span_v<Span<T>> = true;

template <class T>
inline constexpr bool is_std_span_v = false;

template <class T, std::size_t Extent>
inline constexpr bool is_std_span_v<std::span<T, Extent>> = true;

template <class From, class To>
concept SpanCompatiblePointer = std::is_convertible_v<From (*)[], To (*)[]>;

template <class T, class U>
concept SpanEqualityComparable = requires(const T& left, const U& right) {
    { left == right } -> std::convertible_to<bool>;
    { right == left } -> std::convertible_to<bool>;
};

template <class V>
concept BuiltinComparableEnum =
    std::is_enum_v<V> && !requires(V a) { operator==(a, a); } && !requires(V a) { operator<=>(a, a); };

template <class V>
concept BitwiseScalar = std::integral<V> || std::same_as<V, std::byte> || BuiltinComparableEnum<V> || std::is_pointer_v<V>;

template <class V, class U>
concept SimdValueCompatible =
    std::same_as<V, std::remove_cv_t<U>> ||
    (std::is_pointer_v<V> &&
     (std::is_pointer_v<std::remove_cv_t<U>> || std::is_null_pointer_v<std::remove_cv_t<U>> ||
      std::is_array_v<std::remove_cv_t<U>>) &&
     std::convertible_to<const U&, V>);

template <class V>
[[nodiscard]] inline std::size_t simd_find_first(const V* data, std::size_t count, V value) noexcept {
    constexpr std::size_t not_found = std::numeric_limits<std::size_t>::max();
    std::size_t i = 0;
    if constexpr (BitwiseScalar<V> && sizeof(V) == 1) {
        if (count >= 16) {
            const auto needle = _mm_set1_epi8(std::bit_cast<char>(value));
            while (count - i >= 32) {
                const auto c0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                const auto c1 =
                    _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 16)), needle);
                if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                    const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                    if (m0 != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m0));
                    const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                    [[assume(m1 != 0)]];
                    return i + 16 + static_cast<std::size_t>(std::countr_zero(m1));
                }
                i += 32;
            }
            if (count - i >= 16) {
                const auto m = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle)));
                if (m != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(m));
                i += 16;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 2) {
        if (count >= 8) {
            const auto needle = _mm_set1_epi16(std::bit_cast<short>(value));
            while (count - i >= 16) {
                const auto c0 = _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                const auto c1 =
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 8)), needle);
                if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                    const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                    if (m0 != 0)
                        return i + (static_cast<std::size_t>(std::countr_zero(m0)) >> 1);
                    const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                    [[assume(m1 != 0)]];
                    return i + 8 + (static_cast<std::size_t>(std::countr_zero(m1)) >> 1);
                }
                i += 16;
            }
            if (count - i >= 8) {
                const auto m = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle)));
                if (m != 0)
                    return i + (static_cast<std::size_t>(std::countr_zero(m)) >> 1);
                i += 8;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 4) {
        if (count >= 4) {
            const auto needle = _mm_set1_epi32(std::bit_cast<int>(value));
            while (count - i >= 8) {
                const auto c0 = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                const auto c1 =
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 4)), needle);
                if (_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(c0, c1))) != 0) {
                    const auto m0 = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(c0)));
                    if (m0 != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m0));
                    const auto m1 = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(c1)));
                    [[assume(m1 != 0)]];
                    return i + 4 + static_cast<std::size_t>(std::countr_zero(m1));
                }
                i += 8;
            }
            if (count - i >= 4) {
                const auto c = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(c)));
                if (m != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(m));
                i += 4;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 8) {
        if (count >= 2) {
            const auto needle = _mm_set1_epi64x(std::bit_cast<long long>(value));
            while (count - i >= 4) {
                const auto e0 =
                    _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle));
                const auto e1 = _mm_castsi128_ps(
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 2)), needle));
                const auto m = static_cast<unsigned>(_mm_movemask_ps(
                    _mm_and_ps(_mm_shuffle_ps(e0, e1, _MM_SHUFFLE(2, 0, 2, 0)),
                               _mm_shuffle_ps(e0, e1, _MM_SHUFFLE(3, 1, 3, 1)))));
                if (m != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(m));
                i += 4;
            }
            if (count - i >= 2) {
                const auto eq32 = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                const auto eq64 = _mm_and_si128(eq32, _mm_shuffle_epi32(eq32, _MM_SHUFFLE(2, 3, 0, 1)));
                const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_castsi128_pd(eq64)));
                if (m != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(m));
                i += 2;
            }
        }
    } else if constexpr (std::same_as<V, float>) {
        if (value != value)
            return not_found;
        if (count >= 4) {
            const auto needle = _mm_set1_ps(value);
            while (count - i >= 8) {
                const auto c0 = _mm_cmpeq_ps(_mm_loadu_ps(data + i), needle);
                const auto c1 = _mm_cmpeq_ps(_mm_loadu_ps(data + i + 4), needle);
                if (_mm_movemask_ps(_mm_or_ps(c0, c1)) != 0) {
                    const auto m0 = static_cast<unsigned>(_mm_movemask_ps(c0));
                    if (m0 != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m0));
                    const auto m1 = static_cast<unsigned>(_mm_movemask_ps(c1));
                    [[assume(m1 != 0)]];
                    return i + 4 + static_cast<std::size_t>(std::countr_zero(m1));
                }
                i += 8;
            }
            if (count - i >= 4) {
                const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(data + i), needle)));
                if (m != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(m));
                i += 4;
            }
        }
    } else if constexpr (std::same_as<V, double>) {
        if (value != value)
            return not_found;
        if (count >= 2) {
            const auto needle = _mm_set1_pd(value);
            while (count - i >= 4) {
                const auto c0 = _mm_cmpeq_pd(_mm_loadu_pd(data + i), needle);
                const auto c1 = _mm_cmpeq_pd(_mm_loadu_pd(data + i + 2), needle);
                if (_mm_movemask_pd(_mm_or_pd(c0, c1)) != 0) {
                    const auto m0 = static_cast<unsigned>(_mm_movemask_pd(c0));
                    if (m0 != 0)
                        return i + static_cast<std::size_t>(std::countr_zero(m0));
                    const auto m1 = static_cast<unsigned>(_mm_movemask_pd(c1));
                    [[assume(m1 != 0)]];
                    return i + 2 + static_cast<std::size_t>(std::countr_zero(m1));
                }
                i += 4;
            }
            if (count - i >= 2) {
                const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(data + i), needle)));
                if (m != 0)
                    return i + static_cast<std::size_t>(std::countr_zero(m));
                i += 2;
            }
        }
    }
    for (; i != count; ++i)
        if (data[i] == value)
            return i;
    return not_found;
}

template <class V>
[[nodiscard]] inline std::size_t simd_find_last(const V* data, std::size_t count, V value) noexcept {
    constexpr std::size_t not_found = std::numeric_limits<std::size_t>::max();
    std::size_t end = count;
    if constexpr (BitwiseScalar<V> && sizeof(V) == 1) {
        if (end >= 16) {
            const auto needle = _mm_set1_epi8(std::bit_cast<char>(value));
            while (end >= 32) {
                const auto base = end - 32;
                const auto c0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle);
                const auto c1 =
                    _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base + 16)), needle);
                if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                    const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                    if (m1 != 0)
                        return base + 16 + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m1)));
                    const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                    [[assume(m0 != 0)]];
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m0)));
                }
                end = base;
            }
            if (end >= 16) {
                const auto base = end - 16;
                const auto m = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle)));
                if (m != 0)
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m)));
                end = base;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 2) {
        if (end >= 8) {
            const auto needle = _mm_set1_epi16(std::bit_cast<short>(value));
            while (end >= 16) {
                const auto base = end - 16;
                const auto c0 = _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle);
                const auto c1 =
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base + 8)), needle);
                if (_mm_movemask_epi8(_mm_or_si128(c0, c1)) != 0) {
                    const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(c1));
                    if (m1 != 0)
                        return base + 8 +
                               (static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m1))) >> 1);
                    const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(c0));
                    [[assume(m0 != 0)]];
                    return base + (static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m0))) >> 1);
                }
                end = base;
            }
            if (end >= 8) {
                const auto base = end - 8;
                const auto m = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle)));
                if (m != 0)
                    return base + (static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m))) >> 1);
                end = base;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 4) {
        if (end >= 4) {
            const auto needle = _mm_set1_epi32(std::bit_cast<int>(value));
            while (end >= 8) {
                const auto base = end - 8;
                const auto c0 = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle);
                const auto c1 =
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base + 4)), needle);
                if (_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(c0, c1))) != 0) {
                    const auto m1 = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(c1)));
                    if (m1 != 0)
                        return base + 4 + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m1)));
                    const auto m0 = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(c0)));
                    [[assume(m0 != 0)]];
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m0)));
                }
                end = base;
            }
            if (end >= 4) {
                const auto base = end - 4;
                const auto c = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle);
                const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(c)));
                if (m != 0)
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m)));
                end = base;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 8) {
        if (end >= 2) {
            const auto needle = _mm_set1_epi64x(std::bit_cast<long long>(value));
            while (end >= 4) {
                const auto base = end - 4;
                const auto e0 = _mm_castsi128_ps(
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle));
                const auto e1 = _mm_castsi128_ps(
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base + 2)), needle));
                const auto m = static_cast<unsigned>(_mm_movemask_ps(
                    _mm_and_ps(_mm_shuffle_ps(e0, e1, _MM_SHUFFLE(2, 0, 2, 0)),
                               _mm_shuffle_ps(e0, e1, _MM_SHUFFLE(3, 1, 3, 1)))));
                if (m != 0)
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m)));
                end = base;
            }
            if (end >= 2) {
                const auto base = end - 2;
                const auto eq32 =
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base)), needle);
                const auto eq64 = _mm_and_si128(eq32, _mm_shuffle_epi32(eq32, _MM_SHUFFLE(2, 3, 0, 1)));
                const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_castsi128_pd(eq64)));
                if (m != 0)
                    return base + (m >> 1);
                end = base;
            }
        }
    } else if constexpr (std::same_as<V, float>) {
        if (value != value)
            return not_found;
        if (end >= 4) {
            const auto needle = _mm_set1_ps(value);
            while (end >= 8) {
                const auto base = end - 8;
                const auto c0 = _mm_cmpeq_ps(_mm_loadu_ps(data + base), needle);
                const auto c1 = _mm_cmpeq_ps(_mm_loadu_ps(data + base + 4), needle);
                if (_mm_movemask_ps(_mm_or_ps(c0, c1)) != 0) {
                    const auto m1 = static_cast<unsigned>(_mm_movemask_ps(c1));
                    if (m1 != 0)
                        return base + 4 + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m1)));
                    const auto m0 = static_cast<unsigned>(_mm_movemask_ps(c0));
                    [[assume(m0 != 0)]];
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m0)));
                }
                end = base;
            }
            if (end >= 4) {
                const auto base = end - 4;
                const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(data + base), needle)));
                if (m != 0)
                    return base + static_cast<std::size_t>(31u - static_cast<unsigned>(std::countl_zero(m)));
                end = base;
            }
        }
    } else if constexpr (std::same_as<V, double>) {
        if (value != value)
            return not_found;
        if (end >= 2) {
            const auto needle = _mm_set1_pd(value);
            while (end >= 4) {
                const auto base = end - 4;
                const auto c0 = _mm_cmpeq_pd(_mm_loadu_pd(data + base), needle);
                const auto c1 = _mm_cmpeq_pd(_mm_loadu_pd(data + base + 2), needle);
                if (_mm_movemask_pd(_mm_or_pd(c0, c1)) != 0) {
                    const auto m1 = static_cast<unsigned>(_mm_movemask_pd(c1));
                    if (m1 != 0)
                        return base + 2 + (m1 >> 1);
                    const auto m0 = static_cast<unsigned>(_mm_movemask_pd(c0));
                    [[assume(m0 != 0)]];
                    return base + (m0 >> 1);
                }
                end = base;
            }
            if (end >= 2) {
                const auto base = end - 2;
                const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(data + base), needle)));
                if (m != 0)
                    return base + (m >> 1);
                end = base;
            }
        }
    }
    while (end != 0)
        if (data[--end] == value)
            return end;
    return not_found;
}

[[nodiscard]] inline bool simd_float_equals(const float* left, const float* right, std::size_t count) noexcept {
    std::size_t i = 0;
    while (count - i >= 8) {
        const auto eq0 = _mm_cmpeq_ps(_mm_loadu_ps(left + i), _mm_loadu_ps(right + i));
        const auto eq1 = _mm_cmpeq_ps(_mm_loadu_ps(left + i + 4), _mm_loadu_ps(right + i + 4));
        if (_mm_movemask_ps(_mm_and_ps(eq0, eq1)) != 0xf)
            return false;
        i += 8;
    }
    if (count - i >= 4) {
        if (_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(left + i), _mm_loadu_ps(right + i))) != 0xf)
            return false;
        i += 4;
    }
    for (; i != count; ++i)
        if (left[i] != right[i])
            return false;
    return true;
}

[[nodiscard]] inline bool simd_double_equals(const double* left, const double* right, std::size_t count) noexcept {
    std::size_t i = 0;
    while (count - i >= 4) {
        const auto eq0 = _mm_cmpeq_pd(_mm_loadu_pd(left + i), _mm_loadu_pd(right + i));
        const auto eq1 = _mm_cmpeq_pd(_mm_loadu_pd(left + i + 2), _mm_loadu_pd(right + i + 2));
        if (_mm_movemask_pd(_mm_and_pd(eq0, eq1)) != 0x3)
            return false;
        i += 4;
    }
    if (count - i >= 2) {
        if (_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(left + i), _mm_loadu_pd(right + i))) != 0x3)
            return false;
        i += 2;
    }
    for (; i != count; ++i)
        if (left[i] != right[i])
            return false;
    return true;
}

template <class V>
[[nodiscard]] inline std::size_t simd_count(const V* data, std::size_t count, V value) noexcept {
    std::size_t i = 0;
    std::size_t hits = 0;
    if constexpr (BitwiseScalar<V> && sizeof(V) == 1) {
        if (count >= 16) {
            const auto needle = _mm_set1_epi8(std::bit_cast<char>(value));
            const auto zero = _mm_setzero_si128();
            while (count - i >= 32) {
                auto acc = zero;
                std::size_t batch = (count - i) >> 5;
                if (batch > 126)
                    batch = 126;
                for (std::size_t step = 0; step != batch; ++step) {
                    const auto c0 =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                    const auto c1 =
                        _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 16)), needle);
                    acc = _mm_sub_epi8(acc, c0);
                    acc = _mm_sub_epi8(acc, c1);
                    i += 32;
                }
                const auto sad = _mm_sad_epu8(acc, zero);
                hits += static_cast<std::size_t>(_mm_cvtsi128_si64(sad)) +
                        static_cast<std::size_t>(_mm_cvtsi128_si64(_mm_srli_si128(sad, 8)));
            }
            if (count - i >= 16) {
                const auto m = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle)));
                hits += static_cast<std::size_t>(std::popcount(m));
                i += 16;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 2) {
        if (count >= 8) {
            const auto needle = _mm_set1_epi16(std::bit_cast<short>(value));
            while (count - i >= 16) {
                const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle)));
                const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 8)), needle)));
                hits += static_cast<std::size_t>(std::popcount((m1 << 16) | m0)) >> 1;
                i += 16;
            }
            if (count - i >= 8) {
                const auto m = static_cast<unsigned>(_mm_movemask_epi8(
                    _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle)));
                hits += static_cast<std::size_t>(std::popcount(m)) >> 1;
                i += 8;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 4) {
        if (count >= 4) {
            const auto needle = _mm_set1_epi32(std::bit_cast<int>(value));
            while (count - i >= 8) {
                const auto m0 = static_cast<unsigned>(_mm_movemask_ps(
                    _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle))));
                const auto m1 = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 4)), needle))));
                hits += static_cast<std::size_t>(std::popcount((m1 << 4) | m0));
                i += 8;
            }
            if (count - i >= 4) {
                const auto m = static_cast<unsigned>(_mm_movemask_ps(
                    _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle))));
                hits += static_cast<std::size_t>(std::popcount(m));
                i += 4;
            }
        }
    } else if constexpr (BitwiseScalar<V> && sizeof(V) == 8) {
        if (count >= 2) {
            const auto needle = _mm_set1_epi64x(std::bit_cast<long long>(value));
            while (count - i >= 4) {
                const auto e0 = _mm_castsi128_ps(
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle));
                const auto e1 = _mm_castsi128_ps(
                    _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 2)), needle));
                const auto m = static_cast<unsigned>(_mm_movemask_ps(
                    _mm_and_ps(_mm_shuffle_ps(e0, e1, _MM_SHUFFLE(2, 0, 2, 0)),
                               _mm_shuffle_ps(e0, e1, _MM_SHUFFLE(3, 1, 3, 1)))));
                hits += static_cast<std::size_t>(std::popcount(m));
                i += 4;
            }
            if (count - i >= 2) {
                const auto eq32 = _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i)), needle);
                const auto eq64 = _mm_and_si128(eq32, _mm_shuffle_epi32(eq32, _MM_SHUFFLE(2, 3, 0, 1)));
                const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_castsi128_pd(eq64)));
                hits += static_cast<std::size_t>(std::popcount(m));
                i += 2;
            }
        }
    } else if constexpr (std::same_as<V, float>) {
        if (value != value)
            return 0;
        if (count >= 4) {
            const auto needle = _mm_set1_ps(value);
            while (count - i >= 8) {
                const auto m0 = static_cast<unsigned>(_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(data + i), needle)));
                const auto m1 =
                    static_cast<unsigned>(_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(data + i + 4), needle)));
                hits += static_cast<std::size_t>(std::popcount((m1 << 4) | m0));
                i += 8;
            }
            if (count - i >= 4) {
                const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(data + i), needle)));
                hits += static_cast<std::size_t>(std::popcount(m));
                i += 4;
            }
        }
    } else if constexpr (std::same_as<V, double>) {
        if (value != value)
            return 0;
        if (count >= 2) {
            const auto needle = _mm_set1_pd(value);
            while (count - i >= 4) {
                const auto m0 = static_cast<unsigned>(_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(data + i), needle)));
                const auto m1 =
                    static_cast<unsigned>(_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(data + i + 2), needle)));
                hits += static_cast<std::size_t>(std::popcount((m1 << 2) | m0));
                i += 4;
            }
            if (count - i >= 2) {
                const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(data + i), needle)));
                hits += static_cast<std::size_t>(std::popcount(m));
                i += 2;
            }
        }
    }
    for (; i != count; ++i)
        if (data[i] == value)
            ++hits;
    return hits;
}

[[nodiscard]] inline std::size_t simd_byte_mismatch(const void* left, const void* right,
                                                    std::size_t byte_count) noexcept {
    const auto* a = static_cast<const char*>(left);
    const auto* b = static_cast<const char*>(right);
    std::size_t i = 0;
    while (byte_count - i >= 32) {
        const auto eq0 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i)));
        const auto eq1 = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i + 16)),
                                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i + 16)));
        if (_mm_movemask_epi8(_mm_and_si128(eq0, eq1)) != 0xffff) {
            const auto m0 = static_cast<unsigned>(_mm_movemask_epi8(eq0));
            if (m0 != 0xffffu)
                return i + static_cast<std::size_t>(std::countr_zero(~m0 & 0xffffu));
            const auto m1 = static_cast<unsigned>(_mm_movemask_epi8(eq1));
            [[assume(m1 != 0xffffu)]];
            return i + 16 + static_cast<std::size_t>(std::countr_zero(~m1 & 0xffffu));
        }
        i += 32;
    }
    if (byte_count - i >= 16) {
        const auto m = static_cast<unsigned>(_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                           _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i)))));
        if (m != 0xffffu)
            return i + static_cast<std::size_t>(std::countr_zero(~m & 0xffffu));
        i += 16;
    }
    if (byte_count - i >= 8) {
        std::uint64_t wa{};
        std::uint64_t wb{};
        std::memcpy(&wa, a + i, 8);
        std::memcpy(&wb, b + i, 8);
        if (const auto diff = wa ^ wb; diff != 0)
            return i + (static_cast<std::size_t>(std::countr_zero(diff)) >> 3);
        i += 8;
    }
    for (; i != byte_count; ++i)
        if (a[i] != b[i])
            return i;
    return byte_count;
}

[[nodiscard]] inline std::size_t simd_float_common_prefix(const float* left, const float* right,
                                                          std::size_t count) noexcept {
    std::size_t i = 0;
    while (count - i >= 8) {
        const auto eq0 = _mm_cmpeq_ps(_mm_loadu_ps(left + i), _mm_loadu_ps(right + i));
        const auto eq1 = _mm_cmpeq_ps(_mm_loadu_ps(left + i + 4), _mm_loadu_ps(right + i + 4));
        if (_mm_movemask_ps(_mm_and_ps(eq0, eq1)) != 0xf) {
            const auto m0 = static_cast<unsigned>(_mm_movemask_ps(eq0));
            if (m0 != 0xfu)
                return i + static_cast<std::size_t>(std::countr_zero(~m0 & 0xfu));
            const auto m1 = static_cast<unsigned>(_mm_movemask_ps(eq1));
            [[assume(m1 != 0xfu)]];
            return i + 4 + static_cast<std::size_t>(std::countr_zero(~m1 & 0xfu));
        }
        i += 8;
    }
    if (count - i >= 4) {
        const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(left + i), _mm_loadu_ps(right + i))));
        if (m != 0xfu)
            return i + static_cast<std::size_t>(std::countr_zero(~m & 0xfu));
        i += 4;
    }
    for (; i != count; ++i)
        if (!(left[i] == right[i]))
            return i;
    return count;
}

[[nodiscard]] inline std::size_t simd_double_common_prefix(const double* left, const double* right,
                                                           std::size_t count) noexcept {
    std::size_t i = 0;
    while (count - i >= 4) {
        const auto eq0 = _mm_cmpeq_pd(_mm_loadu_pd(left + i), _mm_loadu_pd(right + i));
        const auto eq1 = _mm_cmpeq_pd(_mm_loadu_pd(left + i + 2), _mm_loadu_pd(right + i + 2));
        if (_mm_movemask_pd(_mm_and_pd(eq0, eq1)) != 0x3) {
            const auto m0 = static_cast<unsigned>(_mm_movemask_pd(eq0));
            if (m0 != 0x3u)
                return i + static_cast<std::size_t>(std::countr_zero(~m0 & 0x3u));
            const auto m1 = static_cast<unsigned>(_mm_movemask_pd(eq1));
            [[assume(m1 != 0x3u)]];
            return i + 2 + static_cast<std::size_t>(std::countr_zero(~m1 & 0x3u));
        }
        i += 4;
    }
    if (count - i >= 2) {
        const auto m = static_cast<unsigned>(_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(left + i), _mm_loadu_pd(right + i))));
        if (m != 0x3u)
            return i + static_cast<std::size_t>(std::countr_zero(~m & 0x3u));
        i += 2;
    }
    for (; i != count; ++i)
        if (!(left[i] == right[i]))
            return i;
    return count;
}

template <class V>
inline constexpr bool span_no_padding_v =
    std::has_unique_object_representations_v<V> || std::is_floating_point_v<V> || std::same_as<V, bool>;

template <class V>
inline void simd_fill_trivial(V* destination, std::size_t count, const V& value) noexcept {
    if constexpr (sizeof(V) == 1) {
        unsigned char byte{};
        std::memcpy(&byte, std::addressof(value), 1);
        std::memset(destination, byte, count);
    } else if constexpr (sizeof(V) == 2) {
        std::uint16_t pattern{};
        std::memcpy(&pattern, std::addressof(value), 2);
        if (static_cast<std::uint16_t>(static_cast<std::uint8_t>(pattern) * UINT16_C(0x0101)) == pattern) {
            std::memset(destination, static_cast<unsigned char>(pattern), count * 2);
            return;
        }
        const auto vec = _mm_set1_epi16(static_cast<short>(pattern));
        std::size_t i = 0;
        while (count - i >= 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i + 8), vec);
            i += 16;
        }
        if (count - i >= 8) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            i += 8;
        }
        for (; i != count; ++i)
            std::memcpy(destination + i, &pattern, 2);
    } else if constexpr (sizeof(V) == 4) {
        std::uint32_t pattern{};
        std::memcpy(&pattern, std::addressof(value), 4);
        if (static_cast<std::uint32_t>(static_cast<std::uint8_t>(pattern)) * UINT32_C(0x01010101) == pattern) {
            std::memset(destination, static_cast<unsigned char>(pattern), count * 4);
            return;
        }
        const auto vec = _mm_set1_epi32(static_cast<int>(pattern));
        std::size_t i = 0;
        while (count - i >= 8) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i + 4), vec);
            i += 8;
        }
        if (count - i >= 4) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            i += 4;
        }
        for (; i != count; ++i)
            std::memcpy(destination + i, &pattern, 4);
    } else if constexpr (sizeof(V) == 8) {
        std::uint64_t pattern{};
        std::memcpy(&pattern, std::addressof(value), 8);
        if (static_cast<std::uint64_t>(static_cast<std::uint8_t>(pattern)) * UINT64_C(0x0101010101010101) == pattern) {
            std::memset(destination, static_cast<unsigned char>(pattern), count * 8);
            return;
        }
        const auto vec = _mm_set1_epi64x(static_cast<long long>(pattern));
        std::size_t i = 0;
        while (count - i >= 4) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i + 2), vec);
            i += 4;
        }
        if (count - i >= 2) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            i += 2;
        }
        if (i != count)
            std::memcpy(destination + i, &pattern, 8);
    } else if constexpr (sizeof(V) == 16) {
        const auto vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(std::addressof(value)));
        char byte0{};
        std::memcpy(&byte0, std::addressof(value), 1);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(vec, _mm_set1_epi8(byte0))) == 0xffff) {
            std::memset(destination, static_cast<unsigned char>(byte0), count * 16);
            return;
        }
        std::size_t i = 0;
        while (count - i >= 2) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i + 1), vec);
            i += 2;
        }
        if (i != count)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), vec);
    }
}
}

// Synchronous borrow. Constness belongs to T, not to the descriptor.
template <class T>
class Span {
    static_assert(std::is_object_v<T> && !std::is_volatile_v<T>);

    [[nodiscard]] static consteval std::size_t max_elements() noexcept {
        static_assert(!std::is_abstract_v<T>);
        return static_cast<std::size_t>(PTRDIFF_MAX) / sizeof(T);
    }

public:
    using Value = T;

    using ReverseIterator = std::reverse_iterator<T*>;

    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    constexpr Span() noexcept = default;

    constexpr Span(const Span&) noexcept = default;

    constexpr Span& operator=(const Span&) noexcept = default;

    constexpr Span(std::nullptr_t, std::size_t size) noexcept {
        static_assert(!std::is_abstract_v<T>);
        if (size != 0)
            fatal("Invalid Span storage.");
    }

    template <std::contiguous_iterator It>
        requires detail::SpanCompatiblePointer<std::remove_reference_t<std::iter_reference_t<It>>, T>
    constexpr Span(It first, std::size_t count) noexcept : data_(std::to_address(first)), size_(count) {
        if ((!data_ && count) || count > max_elements())
            fatal("Invalid Span storage.");
    }

    template <std::contiguous_iterator It, std::sized_sentinel_for<It> End>
        requires detail::SpanCompatiblePointer<std::remove_reference_t<std::iter_reference_t<It>>, T> &&
                 (!std::is_convertible_v<End, std::size_t>)
    constexpr Span(It first, End last) noexcept {
        const auto distance = last - first;
        data_ = std::to_address(first);
        if (distance < 0 || (!data_ && distance != 0) ||
            static_cast<std::make_unsigned_t<decltype(distance)>>(distance) > max_elements())
            fatal("Invalid Span storage.");
        size_ = static_cast<std::size_t>(distance);
    }

    template <std::size_t N>
        requires(N <= max_elements())
    constexpr Span(std::type_identity_t<T> (&data [[clang::lifetimebound]])[N]) noexcept : data_(data), size_(N) {}

    template <class U, std::size_t Extent>
        requires detail::SpanCompatiblePointer<U, T>
    explicit constexpr Span(std::span<U, Extent> values) noexcept : Span(values.data(), values.size()) {}

    template <class U>
        requires(!std::same_as<U, T>) && detail::SpanCompatiblePointer<U, T>
    constexpr Span(Span<U> other) noexcept : data_(other.data()), size_(other.size()) {
        static_assert(!std::is_abstract_v<T>);
    }

    template <class R>
        requires(!detail::is_atelier_span_v<std::remove_cvref_t<R>>) &&
                (!detail::is_std_span_v<std::remove_cvref_t<R>>) && (!std::is_array_v<std::remove_cvref_t<R>>) &&
                (!std::ranges::borrowed_range<std::remove_cvref_t<R>>) && std::ranges::contiguous_range<R&> &&
                std::ranges::sized_range<R&> &&
                detail::SpanCompatiblePointer<std::remove_reference_t<std::ranges::range_reference_t<R&>>, T>
    constexpr Span(R& range [[clang::lifetimebound]]) noexcept
        : Span(std::ranges::data(range), std::ranges::size(range)) {}

    template <class R>
        requires(!std::is_lvalue_reference_v<R>) && (!detail::is_atelier_span_v<std::remove_cvref_t<R>>) &&
                (!detail::is_std_span_v<std::remove_cvref_t<R>>) && (!std::is_array_v<std::remove_cvref_t<R>>) &&
                (!std::ranges::borrowed_range<std::remove_cvref_t<R>>) && std::ranges::contiguous_range<R> &&
                std::ranges::sized_range<R> &&
                detail::SpanCompatiblePointer<std::remove_reference_t<std::ranges::range_reference_t<R>>, T>
    Span(R&&) = delete;

    template <class R>
        requires(!detail::is_atelier_span_v<std::remove_cvref_t<R>>) &&
                (!detail::is_std_span_v<std::remove_cvref_t<R>>) && (!std::is_array_v<std::remove_cvref_t<R>>) &&
                std::ranges::borrowed_range<std::remove_cvref_t<R>> && std::ranges::contiguous_range<R> &&
                std::ranges::sized_range<R> &&
                detail::SpanCompatiblePointer<std::remove_reference_t<std::ranges::range_reference_t<R>>, T>
    explicit constexpr Span(R&& range) noexcept : Span(std::ranges::data(range), std::ranges::size(range)) {}

    [[nodiscard]] constexpr T* data() const noexcept { return data_; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

    [[nodiscard]] constexpr std::size_t size_bytes() const noexcept { return size_ * sizeof(T); }

    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr T* begin() const noexcept { return data_; }

    [[nodiscard]] constexpr T* end() const noexcept { return data_ + size_; }

    [[nodiscard]] constexpr ReverseIterator rbegin() const noexcept { return ReverseIterator{end()}; }

    [[nodiscard]] constexpr ReverseIterator rend() const noexcept { return ReverseIterator{begin()}; }

    [[nodiscard]] constexpr T& operator[](std::size_t index) const noexcept {
        if (index >= size_)
            fatal("Span index.");
        return data_[index];
    }

    [[nodiscard]] constexpr T& front() const noexcept { return (*this)[0]; }

    [[nodiscard]] constexpr T& back() const noexcept { return (*this)[size_ - 1]; }

    [[nodiscard]] constexpr T* try_at(std::size_t index) const noexcept {
        return index < size_ ? data_ + index : nullptr;
    }

    [[nodiscard]] constexpr Span first(std::size_t count) const noexcept {
        if (count > size_)
            fatal("Span slice.");
        return {Unchecked{}, data_, count};
    }

    [[nodiscard]] constexpr Span last(std::size_t count) const noexcept {
        if (count > size_)
            fatal("Span slice.");
        return {Unchecked{}, data_ + (size_ - count), count};
    }

    [[nodiscard]] constexpr Span slice(std::size_t first, std::size_t count = npos) const noexcept {
        if (first > size_ || (count != npos && count > size_ - first))
            fatal("Span slice.");
        return {Unchecked{}, data_ + first, count == npos ? size_ - first : count};
    }

    constexpr void remove_prefix(std::size_t count) noexcept {
        if (count > size_)
            fatal("Span slice.");
        data_ += count;
        size_ -= count;
    }

    constexpr void remove_suffix(std::size_t count) noexcept {
        if (count > size_)
            fatal("Span slice.");
        size_ -= count;
    }

    [[nodiscard]] constexpr bool starts_with(Span<const std::remove_cv_t<T>> prefix) const noexcept
        requires detail::SpanEqualityComparable<T, T>
    {
        return size_ >= prefix.size() && first(prefix.size()) == prefix;
    }

    template <class U>
        requires(!std::is_convertible_v<const U&, Span<const std::remove_cv_t<T>>>) &&
                detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr bool starts_with(const U& value) const noexcept {
        return size_ != 0 && data_[0] == value;
    }

    [[nodiscard]] constexpr bool ends_with(Span<const std::remove_cv_t<T>> suffix) const noexcept
        requires detail::SpanEqualityComparable<T, T>
    {
        return size_ >= suffix.size() && last(suffix.size()) == suffix;
    }

    template <class U>
        requires(!std::is_convertible_v<const U&, Span<const std::remove_cv_t<T>>>) &&
                detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr bool ends_with(const U& value) const noexcept {
        return size_ != 0 && data_[size_ - 1] == value;
    }

    constexpr bool strip_prefix(Span<const std::remove_cv_t<T>> prefix) noexcept
        requires detail::SpanEqualityComparable<T, T>
    {
        if (!starts_with(prefix))
            return false;
        data_ += prefix.size();
        size_ -= prefix.size();
        return true;
    }

    template <class U>
        requires(!std::is_convertible_v<const U&, Span<const std::remove_cv_t<T>>>) &&
                detail::SpanEqualityComparable<T, U>
    constexpr bool strip_prefix(const U& value) noexcept {
        if (!starts_with(value))
            return false;
        ++data_;
        --size_;
        return true;
    }

    constexpr bool strip_suffix(Span<const std::remove_cv_t<T>> suffix) noexcept
        requires detail::SpanEqualityComparable<T, T>
    {
        if (!ends_with(suffix))
            return false;
        size_ -= suffix.size();
        return true;
    }

    template <class U>
        requires(!std::is_convertible_v<const U&, Span<const std::remove_cv_t<T>>>) &&
                detail::SpanEqualityComparable<T, U>
    constexpr bool strip_suffix(const U& value) noexcept {
        if (!ends_with(value))
            return false;
        --size_;
        return true;
    }

    template <class U = std::remove_cv_t<T>>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr std::size_t find(const U& value, std::size_t from = 0) const noexcept {
        if (from >= size_)
            return npos;
        using V = std::remove_cv_t<T>;
        if !consteval {
            if constexpr (detail::SimdValueCompatible<V, U> &&
                          ((detail::BitwiseScalar<V> &&
                            (sizeof(V) == 1 || sizeof(V) == 2 || sizeof(V) == 4 || sizeof(V) == 8)) ||
                           std::same_as<V, float> || std::same_as<V, double>)) {
                const auto hit = detail::simd_find_first<V>(data_ + from, size_ - from, static_cast<V>(value));
                return hit == npos ? npos : from + hit;
            }
        }
        for (std::size_t index = from; index != size_; ++index)
            if (data_[index] == value)
                return index;
        return npos;
    }

    template <class U = std::remove_cv_t<T>>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr std::size_t find_last(const U& value, std::size_t from = npos) const noexcept {
        if (size_ == 0)
            return npos;
        const auto limit = from >= size_ - 1 ? size_ : from + 1;
        using V = std::remove_cv_t<T>;
        if !consteval {
            if constexpr (detail::SimdValueCompatible<V, U> &&
                          ((detail::BitwiseScalar<V> &&
                            (sizeof(V) == 1 || sizeof(V) == 2 || sizeof(V) == 4 || sizeof(V) == 8)) ||
                           std::same_as<V, float> || std::same_as<V, double>)) {
                return detail::simd_find_last<V>(data_, limit, static_cast<V>(value));
            }
        }
        for (auto index = limit; index;)
            if (data_[--index] == value)
                return index;
        return npos;
    }

    template <class U = std::remove_cv_t<T>>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr bool contains(const U& value) const noexcept {
        return find(value) != npos;
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] constexpr std::size_t find_if(Predicate&& predicate) const noexcept {
        for (std::size_t index = 0; index != size_; ++index)
            if (std::invoke(predicate, std::as_const(data_[index])))
                return index;
        return npos;
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] constexpr std::size_t find_last_if(Predicate&& predicate) const noexcept {
        for (auto index = size_; index;)
            if (std::invoke(predicate, std::as_const(data_[--index])))
                return index;
        return npos;
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] constexpr bool contains_if(Predicate&& predicate) const noexcept {
        return find_if(std::forward<Predicate>(predicate)) != npos;
    }

    template <class U = std::remove_cv_t<T>>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr std::size_t count(const U& value) const noexcept {
        if (size_ == 0)
            return 0;
        using V = std::remove_cv_t<T>;
        if !consteval {
            if constexpr (detail::SimdValueCompatible<V, U> &&
                          ((detail::BitwiseScalar<V> &&
                            (sizeof(V) == 1 || sizeof(V) == 2 || sizeof(V) == 4 || sizeof(V) == 8)) ||
                           std::same_as<V, float> || std::same_as<V, double>)) {
                return detail::simd_count<V>(data_, size_, static_cast<V>(value));
            }
        }
        std::size_t hits = 0;
        for (std::size_t index = 0; index != size_; ++index)
            if (data_[index] == value)
                ++hits;
        return hits;
    }

    template <class Predicate>
        requires std::predicate<Predicate&, const T&>
    [[nodiscard]] constexpr std::size_t count_if(Predicate&& predicate) const noexcept {
        std::size_t hits = 0;
        for (std::size_t index = 0; index != size_; ++index)
            if (std::invoke(predicate, std::as_const(data_[index])))
                ++hits;
        return hits;
    }

    [[nodiscard]] constexpr std::size_t common_prefix_length(Span<const std::remove_cv_t<T>> other) const noexcept
        requires detail::SpanEqualityComparable<T, T>
    {
        return compute_common_prefix(other);
    }

    template <class R>
        requires(!std::is_convertible_v<R&&, Span<const std::remove_cv_t<T>>>) && std::ranges::contiguous_range<R> &&
                std::ranges::sized_range<R> &&
                (!std::is_volatile_v<std::remove_reference_t<std::ranges::range_reference_t<R>>>) &&
                detail::SpanEqualityComparable<T, std::remove_reference_t<std::ranges::range_reference_t<R>>>
    [[nodiscard]] constexpr std::size_t common_prefix_length(R&& other) const noexcept {
        using Elem = std::remove_cvref_t<std::ranges::range_reference_t<R>>;
        return compute_common_prefix(Span<const Elem>{std::ranges::data(other), std::ranges::size(other)});
    }

    template <class Compare = std::less<>>
        requires std::strict_weak_order<Compare&, const T&, const T&>
    [[nodiscard]] constexpr bool is_sorted(Compare&& compare = {}) const noexcept {
        for (std::size_t index = 1; index < size_; ++index)
            if (std::invoke(compare, std::as_const(data_[index]), std::as_const(data_[index - 1])))
                return false;
        return true;
    }

    template <class U = std::remove_cv_t<T>, class Compare = std::less<>>
        requires std::predicate<Compare&, const T&, const U&>
    [[nodiscard]] constexpr std::size_t lower_bound(const U& value, Compare&& compare = {}) const noexcept {
        std::size_t first = 0;
        std::size_t count = size_;
        while (count != 0) {
            const std::size_t step = count >> 1;
            const std::size_t mid = first + step;
            if (std::invoke(compare, std::as_const(data_[mid]), value)) {
                first = mid + 1;
                count -= step + 1;
            } else {
                count = step;
            }
        }
        return first;
    }

    template <class U = std::remove_cv_t<T>, class Compare = std::less<>>
        requires std::predicate<Compare&, const U&, const T&>
    [[nodiscard]] constexpr std::size_t upper_bound(const U& value, Compare&& compare = {}) const noexcept {
        std::size_t first = 0;
        std::size_t count = size_;
        while (count != 0) {
            const std::size_t step = count >> 1;
            const std::size_t mid = first + step;
            if (!std::invoke(compare, value, std::as_const(data_[mid]))) {
                first = mid + 1;
                count -= step + 1;
            } else {
                count = step;
            }
        }
        return first;
    }

    template <class U = std::remove_cv_t<T>, class Compare = std::less<>>
        requires std::predicate<Compare&, const T&, const U&> && std::predicate<Compare&, const U&, const T&>
    [[nodiscard]] constexpr bool binary_search(const U& value, Compare&& compare = {}) const noexcept {
        const auto index = lower_bound(value, compare);
        return index != size_ && !static_cast<bool>(std::invoke(compare, value, std::as_const(data_[index])));
    }

    template <class U = T>
        requires std::same_as<U, T> && (!std::is_const_v<U>) && std::is_copy_assignable_v<U>
    constexpr void fill(const std::type_identity_t<T>& value) const noexcept {
        if (size_ == 0)
            return;
        if !consteval {
            if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_copy_assignable_v<T> &&
                          detail::span_no_padding_v<T> &&
                          (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8 || sizeof(T) == 16)) {
                detail::simd_fill_trivial(data_, size_, value);
                return;
            }
        }
        if (size_ == 1 && std::addressof(value) == data_)
            return;
        if constexpr (std::is_copy_constructible_v<T>) {
            if constexpr (std::is_move_assignable_v<T>) {
                T saved(value);
                for (std::size_t index = 0; index + 1 < size_; ++index)
                    data_[index] = saved;
                data_[size_ - 1] = std::move(saved);
            } else {
                const T saved(value);
                for (std::size_t index = 0; index != size_; ++index)
                    data_[index] = saved;
            }
        } else {
            for (std::size_t index = 0; index != size_; ++index)
                data_[index] = value;
        }
    }

    template <class U = T>
        requires std::same_as<U, T> && (!std::is_const_v<U>) && std::swappable<U>
    constexpr void reverse() const noexcept {
        std::size_t lo = 0;
        std::size_t hi = size_;
        while (lo + 1 < hi)
            std::ranges::swap(data_[lo++], data_[--hi]);
    }

    template <class U = T>
        requires std::same_as<U, T> && (!std::is_const_v<U>) && std::equality_comparable<U> &&
                std::is_copy_constructible_v<U> && std::is_copy_assignable_v<U>
    constexpr std::size_t replace(const std::type_identity_t<T>& old_value,
                                  const std::type_identity_t<T>& new_value) const noexcept {
        const auto first = find(old_value);
        if (first == npos)
            return 0;
        const T saved_old(old_value);
        const T saved_new(new_value);
        data_[first] = saved_new;
        std::size_t replaced = 1;
        for (std::size_t index = first + 1; index != size_; ++index) {
            if (data_[index] == saved_old) {
                data_[index] = saved_new;
                ++replaced;
            }
        }
        return replaced;
    }

    template <class U = T>
        requires std::same_as<U, T> && (!std::is_const_v<U>) && std::swappable<U>
    constexpr void swap_elements(std::size_t first, std::size_t second) const noexcept {
        if (first >= size_ || second >= size_)
            fatal("Span index.");
        if (first != second)
            std::ranges::swap(data_[first], data_[second]);
    }

    template <class Compare = std::less<>, class U = T>
        requires std::same_as<U, T> && (!std::is_const_v<U>) &&
                std::sortable<U*, std::reference_wrapper<std::remove_reference_t<Compare>>>
    constexpr void sort(Compare&& compare = {}) const noexcept {
        if (size_ > 1)
            std::ranges::sort(data_, data_ + size_, std::ref(compare));
    }

    // Maps a live element address, not an equal value, back to its index.
    // Null, foreign, misaligned and one-past addresses are absent.
    [[nodiscard]] std::size_t index_of(const T* element) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(element);
        const auto first_address = reinterpret_cast<std::uintptr_t>(data_);
        if (address < first_address || address - first_address >= size_ * sizeof(T) ||
            (address - first_address) % sizeof(T))
            return npos;
        return (address - first_address) / sizeof(T);
    }

    template <class U>
        requires detail::SpanEqualityComparable<T, U>
    [[nodiscard]] constexpr bool operator==(Span<U> other) const noexcept {
        if (size_ != other.size())
            return false;
        using V = std::remove_cv_t<T>;
        if !consteval {
            if constexpr (std::same_as<V, std::remove_cv_t<U>>) {
                if constexpr (detail::BitwiseScalar<V>)
                    return size_ == 0 || data_ == other.data() ||
                           std::memcmp(data_, other.data(), size_ * sizeof(V)) == 0;
                else if constexpr (std::same_as<V, float>)
                    return detail::simd_float_equals(data_, other.data(), size_);
                else if constexpr (std::same_as<V, double>)
                    return detail::simd_double_equals(data_, other.data(), size_);
            }
        }
        for (std::size_t index = 0; index != size_; ++index)
            if (!(data_[index] == other.data()[index]))
                return false;
        return true;
    }

    template <class U>
        requires std::three_way_comparable_with<T, U>
    [[nodiscard]] constexpr std::compare_three_way_result_t<T, U> operator<=>(Span<U> other) const noexcept {
        using V = std::remove_cv_t<T>;
        if !consteval {
            if constexpr (std::same_as<V, std::remove_cv_t<U>> &&
                          (std::same_as<V, unsigned char> || std::same_as<V, char8_t> || std::same_as<V, std::byte>)) {
                const auto common = std::min(size_, other.size());
                if (common != 0 && data_ != other.data()) {
                    const int cmp = std::memcmp(data_, other.data(), common);
                    if (cmp != 0)
                        return cmp <=> 0;
                }
                return size_ <=> other.size();
            }
        }
        return std::lexicographical_compare_three_way(begin(), end(), other.begin(), other.end());
    }

    [[nodiscard]] constexpr std::span<T> std_span() const noexcept { return {data_, size_}; }

private:
    struct Unchecked {};

    constexpr Span(Unchecked, T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <class U>
    [[nodiscard]] constexpr std::size_t compute_common_prefix(Span<U> other) const noexcept {
        const auto common = std::min(size_, other.size());
        if (common == 0)
            return 0;
        using V = std::remove_cv_t<T>;
        if !consteval {
            if constexpr (std::same_as<V, std::remove_cv_t<U>>) {
                if constexpr (detail::BitwiseScalar<V> && detail::span_no_padding_v<V>) {
                    if (data_ == other.data())
                        return common;
                    return detail::simd_byte_mismatch(data_, other.data(), common * sizeof(V)) / sizeof(V);
                } else if constexpr (std::same_as<V, float>) {
                    return detail::simd_float_common_prefix(data_, other.data(), common);
                } else if constexpr (std::same_as<V, double>) {
                    return detail::simd_double_common_prefix(data_, other.data(), common);
                }
            }
        }
        for (std::size_t index = 0; index != common; ++index)
            if (!(data_[index] == other.data()[index]))
                return index;
        return common;
    }

    T* data_{};

    std::size_t size_{};
};

template <class T, std::size_t N>
Span(T (&)[N]) -> Span<T>;

template <std::contiguous_iterator It, class EndOrSize>
Span(It, EndOrSize) -> Span<std::remove_reference_t<std::iter_reference_t<It>>>;

template <class R>
    requires std::ranges::contiguous_range<R> && std::ranges::sized_range<R>
Span(R&&) -> Span<std::remove_reference_t<std::ranges::range_reference_t<R>>>;

template <class T>
[[nodiscard]] Span<const std::byte> as_bytes(Span<T> values) noexcept {
    return {reinterpret_cast<const std::byte*>(values.data()), values.size_bytes()};
}

template <class T>
    requires(!std::is_const_v<T>)
[[nodiscard]] Span<std::byte> as_writable_bytes(Span<T> values) noexcept {
    return {reinterpret_cast<std::byte*>(values.data()), values.size_bytes()};
}

}

template <class T>
inline constexpr bool std::ranges::enable_borrowed_range<atelier::Span<T>> = true;

template <class T>
inline constexpr bool std::ranges::enable_view<atelier::Span<T>> = true;

// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "xassert.h"
#include "ntd_stdlib/array.h"
#include "ntd_stdlib/limits.h"
#include "ntd_stdlib/type_traits.h"
#include <cstddef>

#define STRINGIFY(x) #x
#define XSTR(x) STRINGIFY(x)

namespace GPRX
{

// Unchecked conversions for values whose range is established at the call site.
template<typename From>
constexpr mpsize_t to_mpsize(From value)
{
    static_assert(ntd::is_integral_v<From>, "to_mpsize requires an integral type");
    return static_cast<mpsize_t>(value);
}

template<typename From>
constexpr uint8_t to_uint8(From value)
{
    static_assert(ntd::is_integral_v<From>, "to_uint8 requires an integral type");
    return static_cast<uint8_t>(value);
}

template<typename From>
constexpr uint16_t to_uint16(From value)
{
    static_assert(ntd::is_integral_v<From>, "to_uint16 requires an integral type");
    return static_cast<uint16_t>(value);
}

template<typename From>
constexpr uint32_t to_uint32(From value)
{
    static_assert(ntd::is_integral_v<From>, "to_uint32 requires an integral type");
    return static_cast<uint32_t>(value);
}

// Check that [off, off + size) fits within limit without evaluating a potentially overflowing sum.
template <typename T>
constexpr bool range_within(T off, T size, T limit)
{
    static_assert(ntd::is_unsigned_v<T>, "range_within requires an unsigned integer type");
    return off <= limit && size <= limit - off;
}

/**
 * @brief Aligns a value up to the next multiple of a given alignment.
 * @tparam Alignment The alignment boundary (must be a power of two).
 * @param value The value to align.
 * @return The aligned value.
 */
template <size_t Alignment, typename T>
constexpr T align_up(T value)
{
    static_assert(ntd::is_integral<T>::value, "align_up only works on integral types.");
    static_assert((Alignment > 0) && ((Alignment & (Alignment - 1)) == 0), "Alignment must be a power of two.");
    return (value + (Alignment - 1)) & ~(Alignment - 1);
}

constexpr mpsize_t pow2(mpsize_t n)
{
    return (n < ntd::numeric_limits<mpsize_t>::digits) ? (mpsize_t{1} << n) : 0;
}

template <typename T>
constexpr bool is_pow2(T x)
{
    static_assert(ntd::is_unsigned_v<T>, "is_pow2 requires an unsigned integer type");
    return (x != 0) && ((x & (x - 1)) == 0);
}

template <typename T>
constexpr uint32_t bool_to_int(T x)
{
    return (x ? 1u : 0u);
}

template <typename T>
constexpr T div_roundup(T numerator, T divisor)
{
    return numerator / divisor + bool_to_int(numerator % divisor);
}

template <typename Container>
void resize_if_needed(Container& container, mpsize_t requiredSize)
{
    if (container.size() < requiredSize)
        container.resize(requiredSize);
}

/**
 * @brief Creates a bitmask of the lower bits.
 */
constexpr uint32_t create_mask_u32(unsigned int num_bits)
{
    return (num_bits >= 32) ? ~0u : (1u << num_bits) - 1;
}

inline uint32_t set_bit(uint32_t value, unsigned int n)
{
    return value | (1u << n);
}

inline uint32_t clear_bit(uint32_t value, unsigned int n)
{
    return value & ~(1u << n);
}

inline bool is_little_endian()
{
    const uint32_t x = 1;
    return *reinterpret_cast<const unsigned char*>(&x) == 1;
}

template <mpsize_t Alignment, typename T>
inline bool is_aligned(const T* ptr)
{
    static_assert(is_pow2(Alignment), "Specified alignment must be a power of 2");
    return (reinterpret_cast<std::uintptr_t>(ptr) & (Alignment - 1)) == 0;
}

template<size_t N, char C>
ntd::array<uint8_t, N> make_filled_array()
{
    ntd::array<uint8_t, N> arr;
    arr.fill(C);
    return arr;
}

// --- Compile-time helpers to convert binary array to a hex string ---
constexpr uint8_t to_hex_char(uint8_t n)
{
    return static_cast<uint8_t>(n < 10 ? ('0' + n) : ('A' + (n - 10)));
}

template<size_t N>
constexpr ntd::array<uint8_t, N * 2> bin_to_hex(const ntd::array<uint8_t, N>& bin)
{
    ntd::array<uint8_t, N * 2> hex{};
    for (size_t i = 0; i < N; ++i)
    {
        hex[i * 2 + 0] = to_hex_char(static_cast<uint8_t>(bin[i] >> 4));
        hex[i * 2 + 1] = to_hex_char(static_cast<uint8_t>(bin[i] & 0x0F));
    }
    return hex;
}

} // GPRX

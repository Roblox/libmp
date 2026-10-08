// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Raw memory and string helpers used by the allocator and other low-level code.
// They must not allocate or depend on the allocator API.

#include <cstddef>
#if defined(_MSC_VER)
    #include <string.h>
#endif

namespace ntd
{
namespace detail
{

#if defined(_MSC_VER)

inline void* CopyBytes(void* dst, const void* src, size_t size) { return ::memcpy(dst, src, size); }
inline void* MoveBytes(void* dst, const void* src, size_t size) { return ::memmove(dst, src, size); }
inline void* SetBytes(void* dst, int value, size_t size) { return ::memset(dst, value, size); }
inline int CompareBytes(const void* lhs, const void* rhs, size_t size) { return ::memcmp(lhs, rhs, size); }

constexpr size_t StringLength(const char* str)
{
    size_t size = 0;
    while (str[size] != '\0')
        ++size;
    return size;
}

#else

inline void* CopyBytes(void* dst, const void* src, size_t size) { return __builtin_memcpy(dst, src, size); }
inline void* MoveBytes(void* dst, const void* src, size_t size) { return __builtin_memmove(dst, src, size); }
inline void* SetBytes(void* dst, int value, size_t size) { return __builtin_memset(dst, value, size); }
inline int CompareBytes(const void* lhs, const void* rhs, size_t size) { return __builtin_memcmp(lhs, rhs, size); }
constexpr size_t StringLength(const char* str) { return __builtin_strlen(str); }

#endif

} // namespace detail
} // namespace ntd

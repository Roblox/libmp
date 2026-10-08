// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Libc-backed host allocator hooks used by tests of the external memory modes.
// Include this header in exactly one translation unit per test executable.

#ifdef LIBMP_AMALGAMATED
#include "libmp.h"
#else
#include "ntd_stdlib/allocator.h"
#endif

#include <cstdlib>

#if NTD_EXTERNAL_PAGES
inline void* test_reserve_pages(size_t minimumBytes, size_t* actualBytes)
{
    constexpr size_t kPageGranularity = 64u * 1024u;
    if (minimumBytes > static_cast<size_t>(-1) - (kPageGranularity - 1))
    {
        *actualBytes = 0;
        return nullptr;
    }

    *actualBytes = (minimumBytes + kPageGranularity - 1) & ~(kPageGranularity - 1);
    void* result = std::calloc(1, *actualBytes);
    if (!result)
        *actualBytes = 0;
    return result;
}

inline void test_release_pages(void* pointer, size_t actualBytes)
{
    constexpr size_t kPageGranularity = 64u * 1024u;
    if (actualBytes == 0 || actualBytes % kPageGranularity != 0)
        std::abort();
    std::free(pointer);
}
#endif

#if (NTD_EXTERNAL_MALLOC || NTD_EXTERNAL_PAGES) && !NTD_EXTERNAL_SETTERS
  #if NTD_EXTERNAL_MALLOC
extern "C" void* NtdHostMalloc(size_t size)
{
    return std::malloc(size);
}

extern "C" void NtdHostFree(void* pointer)
{
    std::free(pointer);
}

extern "C" void* NtdHostRealloc(void* pointer, size_t size)
{
    return std::realloc(pointer, size);
}
  #endif
  #if NTD_EXTERNAL_PAGES
extern "C" void* NtdHostPageReserve(size_t size, size_t* actualSize)
{
    return test_reserve_pages(size, actualSize);
}

extern "C" void NtdHostPageRelease(void* pointer, size_t actualSize)
{
    test_release_pages(pointer, actualSize);
}
  #endif
#endif

inline void test_install_host_allocator()
{
#if NTD_EXTERNAL_SETTERS && NTD_EXTERNAL_MALLOC
    ntd::mem::SetMallocFns(std::malloc, std::free, std::realloc);
#elif NTD_EXTERNAL_SETTERS && NTD_EXTERNAL_PAGES
    ntd::mem::SetPageSource(test_reserve_pages, test_release_pages);
#endif
}

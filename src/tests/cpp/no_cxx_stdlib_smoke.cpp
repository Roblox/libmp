// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Whole-archive link smoke test for the no-C++-standard-library configuration.
#include "libmp_test_api.h"

#if NTD_EXTERNAL_MALLOC || NTD_EXTERNAL_PAGES
    #include <stdlib.h>
#endif

// This gate excludes the C++ standard library, not libc. Supply the optional
// host allocator boundary here so every supported ntd::mem backend can run the
// same smoke test without changing what the gate proves.
#if NTD_EXTERNAL_MALLOC
    #if NTD_EXTERNAL_SETTERS
static void* HostMalloc(size_t bytes)
{
    return malloc(bytes);
}
static void HostFree(void* pointer)
{
    free(pointer);
}
static void* HostRealloc(void* pointer, size_t bytes)
{
    return realloc(pointer, bytes);
}
    #else
extern "C" void* NtdHostMalloc(size_t bytes)
{
    return malloc(bytes);
}
extern "C" void NtdHostFree(void* pointer)
{
    free(pointer);
}
extern "C" void* NtdHostRealloc(void* pointer, size_t bytes)
{
    return realloc(pointer, bytes);
}
    #endif
#endif

#if NTD_EXTERNAL_PAGES
    #if NTD_EXTERNAL_SETTERS
static void* HostPageReserve(size_t minimumBytes, size_t* actualBytes)
{
    void* result = calloc(1, minimumBytes);
    *actualBytes = result ? minimumBytes : 0;
    return result;
}
static void HostPageRelease(void* pointer, size_t)
{
    free(pointer);
}
    #else
extern "C" void* NtdHostPageReserve(size_t minimumBytes, size_t* actualBytes)
{
    void* result = calloc(1, minimumBytes);
    *actualBytes = result ? minimumBytes : 0;
    return result;
}
extern "C" void NtdHostPageRelease(void* pointer, size_t)
{
    free(pointer);
}
    #endif
#endif

// Clang/GCC emit this Itanium C++ ABI hook in abstract-class vtables. A
// freestanding executable normally supplies the same terminating hook; define
// it here so this test can omit libc++/libstdc++ without weakening the library's
// allocation checks. MSVC's corresponding _purecall hook remains available
// from vcruntime, which is compiler ABI support rather than the MSVC STL.
#if !defined(_MSC_VER)
extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    __builtin_trap();
}
#endif

int main()
{
#if NTD_EXTERNAL_MALLOC && NTD_EXTERNAL_SETTERS
    ntd::mem::SetMallocFns(HostMalloc, HostFree, HostRealloc);
#elif NTD_EXTERNAL_PAGES && NTD_EXTERNAL_SETTERS
    ntd::mem::SetPageSource(HostPageReserve, HostPageRelease);
#endif
    MpInit();
    LibMP::FbBuilder builder;
    const auto first = builder.CreateSharedString("deduplicated");
    const auto second = builder.CreateSharedString("deduplicated");
    LibMP::flatbuffers::Allocator* allocator = new LibMP::flatbuffers::DefaultAllocator();
    delete allocator;
    // Leave the channel enabled deliberately: its ntd::unique_ptr owner must
    // destroy allocator-routed state during normal process teardown without
    // introducing a C++ standard-library runtime dependency.
    if (!MpUseControlChannel(1))
        return 1;
    return MpGetDataFormatVersion() == 0 || MpGetLibraryVersion() == 0 || first.o != second.o;
}

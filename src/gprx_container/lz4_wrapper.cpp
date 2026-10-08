// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Hoist the system headers used by the bundled LZ4 sources to global scope.
// The version guard below requires this list to be reviewed when LZ4 changes.
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#ifdef _MSC_VER
    #include <intrin.h>
#endif

#include "utils.h"

#define GPRX_LZ4_MACRO_SCOPE_PUSH
#include "dependencies/support/lz4_macro_scope.h"
#undef GPRX_LZ4_MACRO_SCOPE_PUSH

// Route LZ4's memory allocation through ntd::mem
#include "dependencies/support/config.h"
#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    #include "ntd_stdlib/allocator.h"
    #undef LZ4_USER_MEMORY_FUNCTIONS
    #define LZ4_USER_MEMORY_FUNCTIONS
#endif

// Compile the embedded LZ4 С files first
#if !CFG_USE_EXTERNAL_LZ4
#if !defined(LZ4_DEBUG) || (LZ4_DEBUG < 1)
    // LZ4's release path defines assert as a no-op only when the macro is absent.
    // System headers were hoisted above, so remove their definition here and let
    // the vendored source establish its intended release behavior. The macro-scope
    // pop below restores the surrounding translation unit's definition.
    #ifdef assert
        #undef assert
    #endif
#endif
// Keep diagnostics enabled for the wrapper while treating the vendored C sources as external code.
#include "dependencies/support/warnings_push.h"
namespace GPRX::lz4_impl
{
    // @amalgamate: dependency-begin lz4 CFG_USE_EXTERNAL_LZ4 dependencies/lz4
    #include "dependencies/lz4/lz4.c"
    #include "dependencies/lz4/lz4hc.c"
    #include "dependencies/lz4/lz4frame.c"
    // @amalgamate: dependency-end lz4
    static_assert(
        LZ4_VERSION_MAJOR == 1 &&
        LZ4_VERSION_MINOR == 10 &&
        LZ4_VERSION_RELEASE == 0,
        "LZ4 version changed to "
        XSTR(LZ4_VERSION_MAJOR) "."
        XSTR(LZ4_VERSION_MINOR) "."
        XSTR(LZ4_VERSION_RELEASE)
        ": review the system-header hoist list and lz4_macro_scope.h, then update this guard");
#if !CFG_EXCLUDE_LZ4_ENCODER
    constexpr size_t kFrameHeaderSizeMax = LZ4F_HEADER_SIZE_MAX;
#endif
    #undef LZ4F_HEADER_SIZE_MAX

// xxhash.c uses libc malloc/free directly (its own XXH_malloc/XXH_free, which
// LZ4_USER_MEMORY_FUNCTIONS does not cover). Redirect those calls to ntd::mem for the
// xxhash TU.
#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    #undef malloc
    #undef free
    #define malloc(s) (::ntd::mem::Alloc(s))
    #define free(p)   (::ntd::mem::Free(p))
#endif
    // @amalgamate: dependency-begin lz4 CFG_USE_EXTERNAL_LZ4 dependencies/lz4
    #include "dependencies/lz4/xxhash.c"
    // @amalgamate: dependency-end lz4
#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    #undef malloc
    #undef free
#endif

#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    // Definitions for the LZ4_USER_MEMORY_FUNCTIONS hooks declared in lz4.c.
    void* LZ4_malloc(size_t s) { return ::ntd::mem::Alloc(s); }
    void* LZ4_calloc(size_t n, size_t s)
    {
        if (n != 0 && s > static_cast<size_t>(-1) / n)
            return nullptr;
        return ::ntd::mem::AllocZeroed(n * s);
    }
    void LZ4_free(void* p) { ::ntd::mem::Free(p); }
#endif
}
#include "dependencies/support/warnings_pop.h"
#endif
#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    #undef LZ4_USER_MEMORY_FUNCTIONS
#endif

#define GPRX_LZ4_MACRO_SCOPE_POP
#include "dependencies/support/lz4_macro_scope.h"
#undef GPRX_LZ4_MACRO_SCOPE_POP

// Now include the wrapper definitions
#include "lz4frame_wrapper.h"

namespace GPRX::lz4
{

Lz4Api g_Lz4Api = {};

void InitializeLz4Api(const Lz4Api& api)
{
    g_Lz4Api = api;
}

void InitEmbeddedLz4()
{
#if !CFG_USE_EXTERNAL_LZ4
    Lz4Api api = {};

#if !CFG_EXCLUDE_LZ4_ENCODER
    api.headerSizeMax = ::GPRX::lz4_impl::kFrameHeaderSizeMax;
    // Use reinterpret_cast to bridge the isolated impl structs with our wrapper forward-declarations
    api.compressBound = reinterpret_cast<decltype(api.compressBound)>(&::GPRX::lz4_impl::LZ4F_compressBound);
    api.createCompressionContext = reinterpret_cast<decltype(api.createCompressionContext)>(&::GPRX::lz4_impl::LZ4F_createCompressionContext);
    api.freeCompressionContext = reinterpret_cast<decltype(api.freeCompressionContext)>(&::GPRX::lz4_impl::LZ4F_freeCompressionContext);
    api.compressBegin = reinterpret_cast<decltype(api.compressBegin)>(&::GPRX::lz4_impl::LZ4F_compressBegin);
    api.compressUpdate = reinterpret_cast<decltype(api.compressUpdate)>(&::GPRX::lz4_impl::LZ4F_compressUpdate);
    api.compressEnd = reinterpret_cast<decltype(api.compressEnd)>(&::GPRX::lz4_impl::LZ4F_compressEnd);
#endif
    api.createDecompressionContext = reinterpret_cast<decltype(api.createDecompressionContext)>(&::GPRX::lz4_impl::LZ4F_createDecompressionContext);
    api.freeDecompressionContext = reinterpret_cast<decltype(api.freeDecompressionContext)>(&::GPRX::lz4_impl::LZ4F_freeDecompressionContext);
    api.decompress = reinterpret_cast<decltype(api.decompress)>(&::GPRX::lz4_impl::LZ4F_decompress);
    api.isError = reinterpret_cast<decltype(api.isError)>(&::GPRX::lz4_impl::LZ4F_isError);
    
    InitializeLz4Api(api);
#endif
}

} // namespace GPRX::lz4

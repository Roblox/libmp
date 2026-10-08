// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "dependencies/support/config.h"
#include "mp_api/flatbuffer_types/flatbuffers_core.h"
#include "ntd_stdlib/ntd_config.h" // NTD_TYPES_DISTINCT_FROM_STD
#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
    #include "ntd_stdlib/allocator.h"
#endif
#include <cstddef>
#include <cstdint>

#if !NTD_TYPES_DISTINCT_FROM_STD

    // ntd types equal std types: reuse one schema family

    #include "mp_api/flatbuffer_types/mp_types_std.h"
    namespace LibMP
    {
        namespace TypesNtd = ::MpTypesStd;
    }

#else
// Amalgamation expands the derived NTD_TYPES_DISTINCT_FROM_STD predicate below.
// @amalgamate: config-begin ntd-api-flavor NTD_OWN_STDLIB || NTD_EMBEDDED_ALLOC || NTD_EXTERNAL_MALLOC

    // Allocator-routed standard primitives form a genuinely distinct type
    // family. The reserved embedded mode uses this flavor as well.

    #include "ntd_stdlib/string.h"     // ntd::string (data()/length())
    #include "ntd_stdlib/vector.h"     // ntd::vector (data()/size())
    #include "ntd_stdlib/memory.h"     // ntd::make_unique (UnPack result uses ntd ownership)
    #include "ntd_stdlib/functional.h" // ntd::function (FBSTD::function resolver param; never populated)

    #include "dependencies/support/warnings_push.h"
    #if !CFG_USE_EXTERNAL_FLATBUFFERS
        #define flatbuffers LibMP_flatbuffers
    #endif
    #define FBSTD ntd
    #define FBTYPES MpTypesNtd
        #include "mp_api/flatbuffer_types/mp_types_generated.inc"
    #undef FBTYPES
    #undef FBSTD
    #if !CFG_USE_EXTERNAL_FLATBUFFERS
        #undef flatbuffers
    #endif
    #include "dependencies/support/warnings_pop.h"

    namespace LibMP
    {
#if CFG_USE_EXTERNAL_FLATBUFFERS
        namespace flatbuffers = ::flatbuffers; // host FlatBuffers core
#else
        namespace flatbuffers = ::LibMP_flatbuffers; // same target as the std wrapper (redefinition to same is legal)
#endif
        namespace TypesNtd = ::MpTypesNtd;
    }

    // TypesNtd::*::UnPack() returns storage created by ntd::New; destroy such a
    // raw result with ntd::Delete. Prefer UnPackTo() when practical.

// @amalgamate: config-end ntd-api-flavor
#endif // NTD_TYPES_DISTINCT_FROM_STD

namespace LibMP
{

#if CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD
class NtdFbAllocatorImpl : public flatbuffers::Allocator
{
public:
    uint8_t* allocate(size_t size) override
    {
        static_assert(
            alignof(flatbuffers::largest_scalar_t) <= ntd::mem::kDefaultAlign,
            "FlatBuffers requires stronger alignment than ntd::mem provides by default");
        return static_cast<uint8_t*>(ntd::mem::AllocOrTrap(size));
    }
    void deallocate(uint8_t* p, size_t /*size*/) override { ntd::mem::Free(p); }
    // reallocate_downward inherits the base default (allocate + memcpy_downward + deallocate).
};

// Keep the allocator inside each builder. The allocator base is constructed
// before FlatBufferBuilder and destroyed after it, avoiding a guarded static
// singleton and its C++ ABI initialization/destruction helpers.
class FbBuilder : private NtdFbAllocatorImpl, public flatbuffers::FlatBufferBuilder
{
public:
    explicit FbBuilder(size_t initial_size = 1024)
        : NtdFbAllocatorImpl()
        , flatbuffers::FlatBufferBuilder(
            initial_size,
            static_cast<NtdFbAllocatorImpl*>(this))
    {}
};
#else
class FbBuilder : public flatbuffers::FlatBufferBuilder
{
public:
    explicit FbBuilder(size_t initial_size = 1024)
        : flatbuffers::FlatBufferBuilder(initial_size)
    {}
};
#endif

} // namespace LibMP

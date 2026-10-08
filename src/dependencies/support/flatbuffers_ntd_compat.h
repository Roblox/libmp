// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// FlatBuffers' default allocator and optional shared-string pool use global
// new/delete. Substitute ntd-routed types while parsing bundled or external
// headers without modifying the upstream sources.

#include "ntd_stdlib/allocator.h"
#include "ntd_stdlib/set.h"

// Pre-include every standard header used by flatbuffer_builder.h before the
// temporary `std` namespace remap below. The version guard below requires this
// list to be reviewed whenever FlatBuffers changes.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(FLATBUFFERS_H_) || \
    defined(FLATBUFFERS_FLATBUFFER_BUILDER_H_) || \
    defined(FLATBUFFERS_ALLOCATOR_H_) || \
    defined(FLATBUFFERS_DEFAULT_ALLOCATOR_H_)
    #error "Include LibMP before FlatBuffers when NTD_TYPES_DISTINCT_FROM_STD=1; its allocator compatibility layer must be installed first"
#endif

// @amalgamate: dependency-begin flatbuffers CFG_USE_EXTERNAL_FLATBUFFERS dependencies/flatbuffers
#include <flatbuffers/base.h>
// @amalgamate: dependency-end flatbuffers

#define FLATBUFFERS_NTD_COMPAT_STRINGIFY_IMPL(value) #value
#define FLATBUFFERS_NTD_COMPAT_STRINGIFY(value) \
    FLATBUFFERS_NTD_COMPAT_STRINGIFY_IMPL(value)
static_assert(
    FLATBUFFERS_VERSION_MAJOR == 25 &&
    FLATBUFFERS_VERSION_MINOR == 12 &&
    FLATBUFFERS_VERSION_REVISION == 19,
    "FlatBuffers version changed to "
    FLATBUFFERS_NTD_COMPAT_STRINGIFY(FLATBUFFERS_VERSION_MAJOR) "."
    FLATBUFFERS_NTD_COMPAT_STRINGIFY(FLATBUFFERS_VERSION_MINOR) "."
    FLATBUFFERS_NTD_COMPAT_STRINGIFY(FLATBUFFERS_VERSION_REVISION)
    ": review the standard-header pre-includes in flatbuffers_ntd_compat.h, then update this guard");
#undef FLATBUFFERS_NTD_COMPAT_STRINGIFY
#undef FLATBUFFERS_NTD_COMPAT_STRINGIFY_IMPL

namespace LibMPFlatbuffersStd
{
using namespace ::std;

// FlatBufferBuilder uses std::set only for its opt-in shared-string pool. Route
// that set's storage and the separately allocated container object through
// ntd::mem. All other std names continue to denote their real header-only types
// through the using-directive above.
template <
    class Key,
    class Compare = ::std::less<Key>,
    class Allocator = ::std::allocator<Key>>
class set : public ntd::mem::RoutedAllocation, public ntd::set<Key, Compare>
{
public:
    explicit set(const Compare& compare = Compare()) : ntd::set<Key, Compare>(compare) {}
};
} // namespace LibMPFlatbuffersStd

// Suppress the upstream definitions after providing allocation-compatible
// replacements with the same API. These guards work for bundled and external
// FlatBuffers because both headers use the conventional public guard names.
#define FLATBUFFERS_ALLOCATOR_H_
#define FLATBUFFERS_DEFAULT_ALLOCATOR_H_

namespace flatbuffers
{

class Allocator : public ntd::mem::RoutedAllocation
{
public:
    virtual ~Allocator() = default;
    virtual uint8_t* allocate(size_t size) = 0;
    virtual void deallocate(uint8_t* p, size_t size) = 0;

    virtual uint8_t* reallocate_downward(
        uint8_t* old_p,
        size_t old_size,
        size_t new_size,
        size_t in_use_back,
        size_t in_use_front)
    {
        FLATBUFFERS_ASSERT(new_size > old_size);
        uint8_t* new_p = allocate(new_size);
        memcpy_downward(old_p, old_size, new_p, new_size, in_use_back, in_use_front);
        deallocate(old_p, old_size);
        return new_p;
    }

protected:
    void memcpy_downward(
        uint8_t* old_p,
        size_t old_size,
        uint8_t* new_p,
        size_t new_size,
        size_t in_use_back,
        size_t in_use_front)
    {
        memcpy(new_p + new_size - in_use_back, old_p + old_size - in_use_back, in_use_back);
        memcpy(new_p, old_p, in_use_front);
    }
};

class DefaultAllocator : public Allocator
{
public:
    uint8_t* allocate(size_t size) FLATBUFFERS_OVERRIDE
    {
        static_assert(
            alignof(flatbuffers::largest_scalar_t) <= ntd::mem::kDefaultAlign,
            "FlatBuffers requires stronger alignment than ntd::mem provides by default");
        return static_cast<uint8_t*>(ntd::mem::AllocOrTrap(size));
    }
    void deallocate(uint8_t* p, size_t) FLATBUFFERS_OVERRIDE
    {
        ntd::mem::Free(p);
    }
    static void dealloc(void* p, size_t)
    {
        ntd::mem::Free(p);
    }
};

inline uint8_t* Allocate(Allocator* allocator, size_t size)
{
    return allocator ? allocator->allocate(size) : DefaultAllocator().allocate(size);
}

inline void Deallocate(Allocator* allocator, uint8_t* p, size_t size)
{
    if (allocator)
        allocator->deallocate(p, size);
    else
        DefaultAllocator().deallocate(p, size);
}

inline uint8_t* ReallocateDownward(
    Allocator* allocator,
    uint8_t* old_p,
    size_t old_size,
    size_t new_size,
    size_t in_use_back,
    size_t in_use_front)
{
    return allocator
        ? allocator->reallocate_downward(old_p, old_size, new_size, in_use_back, in_use_front)
        : DefaultAllocator().reallocate_downward(old_p, old_size, new_size, in_use_back, in_use_front);
}

} // namespace flatbuffers

// Keep the remap active only while parsing flatbuffer_builder.h, where the
// shared-string set is declared.
#define LIBMP_FLATBUFFERS_NTD_STD_REMAP 1
#define std LibMPFlatbuffersStd
// @amalgamate: dependency-begin flatbuffers CFG_USE_EXTERNAL_FLATBUFFERS dependencies/flatbuffers
#include <flatbuffers/flatbuffer_builder.h>
// @amalgamate: dependency-end flatbuffers
#undef std
#undef LIBMP_FLATBUFFERS_NTD_STD_REMAP

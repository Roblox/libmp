// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// Single allocation entry point (ntd::mem). Present in every build mode:
//   * no backend flag         -> platform malloc/free/realloc (inline);
//   * NTD_EMBEDDED_ALLOC      -> native arena allocator in allocator.cpp;
//   * NTD_EXTERNAL_MALLOC     -> host-provided malloc/free/realloc callbacks.
//
// ntd containers and typed ntd::New/Delete allocations call this API directly.

#include "ntd_config.h"
#include "compiler.h"
#include <cstddef> // size_t
#include <new>     // placement new (ntd::New)

#if !NTD_HAS_CUSTOM_ALLOCATOR_BACKEND
    #include <cstdlib> // malloc / free / realloc
#endif

namespace ntd
{
namespace detail
{

inline size_t SizeAddOrTrap(size_t a, size_t b)
{
    if (a > static_cast<size_t>(-1) - b)
        Trap();
    return a + b;
}

inline size_t SizeMultiplyOrTrap(size_t a, size_t b)
{
    if (a != 0 && b > static_cast<size_t>(-1) / a)
        Trap();
    return a * b;
}

} // namespace detail

namespace mem
{

// The platform's fundamental alignment, guaranteed by malloc-compatible
// backends. Typed allocations request stronger alignment when necessary.
inline constexpr size_t kDefaultAlign = alignof(std::max_align_t);

inline constexpr bool IsValidAlignment(size_t align)
{
    return align != 0 && (align & (align - 1)) == 0;
}
static_assert(IsValidAlignment(kDefaultAlign), "the platform default alignment must be a power of two");

template <class T>
inline constexpr size_t AlignmentFor()
{
    return alignof(T) > kDefaultAlign ? alignof(T) : kDefaultAlign;
}

#if NTD_HAS_CUSTOM_ALLOCATOR_BACKEND

// Defined out-of-line in allocator.cpp:
//   native embedded allocator -> the page allocator;
//   external malloc    -> a thin forward to the host's malloc/free/realloc.
void* Alloc(size_t size, size_t align = kDefaultAlign);
void* AllocZeroed(size_t size, size_t align = kDefaultAlign);
void Free(void* p);
void* Realloc(void* p, size_t newSize, size_t align = kDefaultAlign);

// Live allocator capacity, reusable freed blocks, and total backing memory held
// from the OS/host. BytesInUse aliases ActiveCapacityBytes;
// PeakBytesInUse is its lifetime peak. All report 0 in external-malloc mode, where
// the host allocator owns accounting. CachedBytes includes complete freed small
// blocks and large reservations, but not uncarved arena tails.
size_t ActiveCapacityBytes();
size_t BytesInUse();
size_t PeakBytesInUse();
size_t ReservedBytes();
size_t CachedBytes();

#if NTD_EMBEDDED_ALLOC
struct LargeCacheConfig
{
    size_t minimum_bytes = 1u << 20;
    size_t maximum_bytes = 32u << 20;
    size_t target_percent = 50;
    size_t operations_per_epoch = 256;
};

// Set exactly once, before the first cache-eligible large allocation. Setting
// both byte limits to zero disables caching; invalid, repeated, or late calls trap.
void SetLargeCacheConfig(LargeCacheConfig config);

// Release all cached large regions. Small-object arenas remain reserved.
void TrimLargeCache();
#endif

#if NTD_EXTERNAL_PAGES && NTD_EXTERNAL_SETTERS
// Runtime page source (mode: external pages + --external-setters). reserve receives
// a minimum size and reports the actual size of its zeroed reservation. The complete
// reported range may be used, and the same size is passed to release. Both callbacks
// are required and the pair may be installed exactly once, before the first allocation;
// using the allocator first traps.
// (Default, without --external-setters: the link-time NtdHostPageReserve /
// NtdHostPageRelease below.)
using PageReserveFn = void* (*)(size_t minimumBytes, size_t* actualBytes);
using PageReleaseFn = void (*)(void* p, size_t actualBytes);
void SetPageSource(PageReserveFn reserve, PageReleaseFn release);
#endif

#if NTD_EXTERNAL_MALLOC && NTD_EXTERNAL_SETTERS
// Runtime host allocator (mode: external malloc + --external-setters). Alloc/Free/
// Realloc forward to these. The callbacks must provide the target platform's
// normal malloc alignment. Register exactly once at startup, before the first
// allocation (nothing here allocates before main()). malloc_fn and free_fn are
// required; realloc_fn may be null
// (Realloc is never called internally). AllocZeroed uses malloc + zero-fill.
// (Default, without --external-setters: the link-time NtdHostMalloc /
// NtdHostFree / NtdHostRealloc below.)
using MallocFn = void* (*)(size_t bytes);
using FreeFn = void (*)(void* p);
using ReallocFn = void* (*)(void* p, size_t bytes);
void SetMallocFns(MallocFn malloc_fn, FreeFn free_fn, ReallocFn realloc_fn = nullptr);
#endif

#else

// Plain malloc promises alignment through max_align_t. Reject stronger requests
// instead of silently returning an under-aligned pointer.
inline void* Alloc(size_t size, size_t align = kDefaultAlign)
{
    if (!IsValidAlignment(align) || align > kDefaultAlign)
        return nullptr;
    return ::malloc(size ? size : 1);
}
inline void* AllocZeroed(size_t size, size_t align = kDefaultAlign)
{
    if (!IsValidAlignment(align) || align > kDefaultAlign)
        return nullptr;
    return ::calloc(1, size ? size : 1);
}
inline void Free(void* p) { ::free(p); }
inline void* Realloc(void* p, size_t newSize, size_t align = kDefaultAlign)
{
    if (!IsValidAlignment(align) || align > kDefaultAlign)
        return nullptr;
    if (!p)
        return Alloc(newSize, align);
    if (newSize == 0)
    {
        Free(p);
        return nullptr;
    }
    return ::realloc(p, newSize);
}
inline size_t BytesInUse() { return 0; }
inline size_t ActiveCapacityBytes() { return 0; }
inline size_t PeakBytesInUse() { return 0; }
inline size_t ReservedBytes() { return 0; }
inline size_t CachedBytes() { return 0; }

#endif

inline void* AllocOrTrap(size_t size, size_t align = kDefaultAlign)
{
    void* p = Alloc(size, align);
    if (!p)
        detail::Trap();
    return p;
}

// Opt-in base for polymorphic classes whose compiler-generated deleting
// destructors would otherwise reference the process-wide operator delete.
// Allocation remains class-scoped: this does not replace or intercept the
// host application's global new/delete operators.
struct RoutedAllocation
{
    static void* operator new(size_t size)
    {
        return AllocOrTrap(size);
    }
    static void operator delete(void* p) noexcept
    {
        Free(p);
    }
    static void operator delete(void* p, size_t) noexcept
    {
        Free(p);
    }
    static void* operator new(size_t size, std::align_val_t align)
    {
        return AllocOrTrap(size, static_cast<size_t>(align));
    }
    static void operator delete(void* p, std::align_val_t) noexcept
    {
        Free(p);
    }
    static void operator delete(void* p, size_t, std::align_val_t) noexcept
    {
        Free(p);
    }
};

} // namespace mem

// Stateless C++ allocator adapter for standard containers used as ntd's
// implementation. It preserves std container behavior while routing storage
// through the selected ntd::mem backend.
template <class T>
class allocator
{
public:
    using value_type = T;
    using size_type = size_t;

    constexpr allocator() noexcept = default;
    template <class U> constexpr allocator(const allocator<U>&) noexcept {}

    T* allocate(size_type count)
    {
        const size_type bytes = detail::SizeMultiplyOrTrap(count, sizeof(T));
        return static_cast<T*>(mem::AllocOrTrap(bytes, mem::AlignmentFor<T>()));
    }
    void deallocate(T* p, size_type) noexcept
    {
        mem::Free(p);
    }

    template <class U>
    struct rebind
    {
        using other = allocator<U>;
    };
};

template <class T, class U>
constexpr bool operator==(const allocator<T>&, const allocator<U>&) noexcept
{
    return true;
}
template <class T, class U>
constexpr bool operator!=(const allocator<T>&, const allocator<U>&) noexcept
{
    return false;
}

// Typed allocation helpers over ntd::mem. They provide explicit construction and
// destruction without changing the process-wide operator new/delete. Present in
// every mode, so New/Delete always pair on the same heap.
template <class T, class... A>
inline T* New(A&&... args)
{
    void* p = mem::AllocOrTrap(sizeof(T), mem::AlignmentFor<T>());
    return ::new (p) T(static_cast<A&&>(args)...);
}
template <class T>
inline void Delete(T* p)
{
    if (p)
    {
        // `p` must equal the original address returned by ntd::New. Unlike a delete
        // expression, mem::Free cannot recover an allocation address from an adjusted
        // secondary-base pointer. The default ntd::unique_ptr converting constructor
        // enforces this requirement when ownership changes pointer type.
        p->~T();
        mem::Free(p);
    }
}

} // namespace ntd

#if (NTD_EXTERNAL_MALLOC || NTD_EXTERNAL_PAGES) && !NTD_EXTERNAL_SETTERS
// Link-time host hooks -- the default for the external modes. The host application
// must DEFINE these; a missing one is a link error (not a runtime surprise). extern
// "C" keeps the names stable and unmangled (definable from C or C++), and they need
// no registration, so they are valid even before main(). They belong to the generic
// ntd allocator layer, hence the Ntd prefix (not tied to any particular consumer).
extern "C"
{
#if NTD_EXTERNAL_MALLOC
// These must provide the target platform's normal malloc alignment.
void* NtdHostMalloc(size_t bytes);            // every allocation forwards here
void NtdHostFree(void* p);
void* NtdHostRealloc(void* p, size_t bytes);
#endif
#if NTD_EXTERNAL_PAGES
// Return a max_align_t-aligned zeroed region of at least minimumBytes and store
// its complete size in actualBytes. NtdHostPageRelease receives that same reported size.
void* NtdHostPageReserve(size_t minimumBytes, size_t* actualBytes);
void NtdHostPageRelease(void* p, size_t actualBytes);
#endif
}
#endif

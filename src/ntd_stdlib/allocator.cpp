// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "allocator.h"

#if NTD_EMBEDDED_ALLOC
// @amalgamate: config-begin NTD_EMBEDDED_ALLOC

// Embedded allocator: segregated free-lists over large OS chunks, with big
// requests served as dedicated OS regions. No libc malloc. It uses a simplified
// size-class free-list and bump-carved-arena design for a small-object-heavy
// workload. Freed blocks return to their class list for O(1) reuse; arenas are
// kept for the process lifetime.
//
// Layout: an 8-byte header immediately precedes every payload. Small blocks store
// only their size-class index; large blocks store their reservation size. An
// over-aligned block stores its base offset in the header and its reservation
// size at the base. Cached blocks use their first payload word as the free-list link.

#include "memory_ops.h"
#include "spin_lock.h"
#include <cstdint>

#if !NTD_EXTERNAL_PAGES
    #if defined(_WIN32)
        #include "windows_api.h"
    #else
        #include <sys/mman.h>
        #include <unistd.h>
    #endif
#endif

namespace ntd
{
namespace mem
{
namespace
{

// ---- tuning -----------------------------------------------------------------
constexpr size_t kPayloadAlign = kDefaultAlign;
constexpr size_t kHeaderAlign = 8;
constexpr size_t kChunkBytes = 1u << 20; // 1 MB arenas for small blocks
static_assert(kPayloadAlign == 8 || kPayloadAlign == 16,
    "the embedded allocator supports 8- or 16-byte platform alignment");
// Small metadata is a class index, over-aligned metadata has the high bit set,
// and every other value is a normal large allocation's reservation size.
constexpr uint64_t kOverAlignedFlag = uint64_t{1} << 63;

struct alignas(kHeaderAlign) BlockHeader
{
    uint64_t metadata;
};
static_assert(sizeof(BlockHeader) == 8, "compact allocation header must remain 8 bytes");
constexpr size_t kDefaultHeaderOffset = kPayloadAlign - sizeof(BlockHeader);
constexpr size_t kDefaultPrefixBytes = kDefaultHeaderOffset + sizeof(BlockHeader);
static_assert(kDefaultPrefixBytes == kPayloadAlign,
    "the default allocation prefix must place payloads at the platform alignment");

// ---- size classes -----------------------------------------------------------
// The first class handles payloads through 8 bytes. Class totals include the header;
// multiples of 16 keep every payload aligned after the initial arena offset.
constexpr size_t kClassBlockSizes[] = {
    16,
    32, 48, 64, 80, 96, 112, 128, 144,
    176, 208, 240, 272,
    336, 400, 464, 528,
    656, 784, 912, 1040,
    1296, 1552, 1808, 2064,
    2576, 3088, 3600, 4112,
    5136, 6160, 7184, 8208,
};
constexpr uint32_t kNumClasses = static_cast<uint32_t>(sizeof(kClassBlockSizes) / sizeof(kClassBlockSizes[0]));
constexpr size_t kClassGranularity = 8;
constexpr size_t kMaxSmall = kClassBlockSizes[kNumClasses - 1] - sizeof(BlockHeader);
static_assert(kClassBlockSizes[0] == 16, "a tiny allocation must fit in 16 bytes");
static_assert(kClassBlockSizes[0] - sizeof(BlockHeader) >= sizeof(void*),
    "a freed small block must hold its free-list link");
static_assert(kClassBlockSizes[kNumClasses - 1] + kDefaultHeaderOffset <= kChunkBytes,
    "the largest small block must fit in a fresh arena");
static_assert(sizeof(size_t) <= sizeof(uint64_t), "allocation metadata requires size_t to fit in 64 bits");

inline size_t ClassCapacity(uint32_t cls)
{
    return kClassBlockSizes[cls] - sizeof(BlockHeader);
}

inline void* Payload(BlockHeader* h)
{
    return reinterpret_cast<char*>(h) + sizeof(BlockHeader);
}

inline BlockHeader* HeaderOf(void* p)
{
    return reinterpret_cast<BlockHeader*>(static_cast<char*>(p) - sizeof(BlockHeader));
}

inline char* LargeAllocationBase(BlockHeader* h)
{
    return reinterpret_cast<char*>(h) - kDefaultHeaderOffset;
}

inline bool IsSmall(const BlockHeader* h)
{
    return h->metadata < kNumClasses;
}

inline bool IsOverAligned(const BlockHeader* h)
{
    return (h->metadata & kOverAlignedFlag) != 0;
}

inline uint32_t ClassIndex(const BlockHeader* h)
{
    return static_cast<uint32_t>(h->metadata);
}

inline size_t LargeReservationBytes(const BlockHeader* h)
{
    return static_cast<size_t>(h->metadata);
}

inline size_t OverAlignedHeaderOffset(const BlockHeader* h)
{
    return static_cast<size_t>(h->metadata & ~kOverAlignedFlag);
}

inline char* OverAlignedBase(BlockHeader* h)
{
    return reinterpret_cast<char*>(h) - OverAlignedHeaderOffset(h);
}

inline size_t OverAlignedReservationBytes(BlockHeader* h)
{
    return static_cast<size_t>(*reinterpret_cast<uint64_t*>(OverAlignedBase(h)));
}

inline size_t PayloadCapacity(BlockHeader* h)
{
    if (IsSmall(h))
        return ClassCapacity(ClassIndex(h));
    if (IsOverAligned(h))
        return OverAlignedReservationBytes(h) - OverAlignedHeaderOffset(h) - sizeof(BlockHeader);
    return LargeReservationBytes(h) - kDefaultPrefixBytes;
}

inline BlockHeader* LargeFreeNext(BlockHeader* h)
{
    BlockHeader* next = nullptr;
    detail::CopyBytes(&next, Payload(h), sizeof(next));
    return next;
}

inline void SetLargeFreeNext(BlockHeader* h, BlockHeader* next)
{
    detail::CopyBytes(Payload(h), &next, sizeof(next));
}

// ---- allocator state --------------------------------------------------------
detail::SpinLock g_lock;
void* g_freeList[kNumClasses] = {};   // heads; free block stores next in its payload
char* g_bumpCur = nullptr;            // current arena carve position
char* g_bumpEnd = nullptr;
size_t g_activeCapacityBytes = 0;
size_t g_peakActiveCapacityBytes = 0;
size_t g_reservedBytes = 0; // OS bytes in arenas and cached/live large regions

// Cache freed large blocks to avoid repeated page reserve/release operations for
// recurring allocation sizes.
BlockHeader* g_largeFree = nullptr;
size_t g_largeCachedBytes = 0;
size_t g_smallCachedBytes = 0;
// Cache-eligible (normally aligned) live large reservations and their recent peak.
size_t g_largeLiveBytes = 0;
size_t g_largeRecentPeakBytes = 0;
size_t g_largeOperationCount = 0;
// Reevaluate toward the soft target once per epoch, or immediately upon
// crossing the hard limit. The floor does not cause allocations by itself.
LargeCacheConfig g_largeCacheConfig;
bool g_largeCacheConfigLocked = false;

// size (<= kMaxSmall) -> class index. Built lazily, O(1) lookup in 8-byte units.
uint8_t g_sizeToClass[kMaxSmall / kClassGranularity + 1];
bool g_sizeMapReady = false;

constexpr size_t kSizeMax = static_cast<size_t>(-1);

void BuildSizeMap()
{
    uint32_t cls = 0;
    for (size_t units = 0; units <= kMaxSmall / kClassGranularity; ++units)
    {
        const size_t bytes = units * kClassGranularity;
        while (cls < kNumClasses && ClassCapacity(cls) < bytes)
            ++cls;
        g_sizeToClass[units] = static_cast<uint8_t>(cls < kNumClasses ? cls : kNumClasses - 1);
    }
    g_sizeMapReady = true;
}

// ---- page source ------------------------------------------------------------
#if NTD_EXTERNAL_PAGES && NTD_EXTERNAL_SETTERS
PageReserveFn g_extReserve = nullptr;
PageReleaseFn g_extRelease = nullptr;
#endif

#if !NTD_EXTERNAL_PAGES
size_t g_nativePageSize = 0;

size_t NativePageSize()
{
    if (g_nativePageSize != 0)
        return g_nativePageSize;

#if defined(_WIN32)
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    g_nativePageSize = static_cast<size_t>(info.dwPageSize);
#else
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0)
        g_nativePageSize = static_cast<size_t>(pageSize);
#endif
    if (g_nativePageSize == 0)
        detail::Trap();
    return g_nativePageSize;
}

bool RoundUpToNativePage(size_t minimumBytes, size_t& roundedBytes)
{
    const size_t pageSize = NativePageSize();
    const size_t remainder = minimumBytes % pageSize;
    if (remainder == 0)
    {
        roundedBytes = minimumBytes;
        return true;
    }

    const size_t padding = pageSize - remainder;
    if (minimumBytes > kSizeMax - padding)
        return false;
    roundedBytes = minimumBytes + padding;
    return true;
}

void* OsReserve(size_t bytes)
{
#if defined(_WIN32)
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void OsRelease(void* p, size_t bytes)
{
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
#endif
}
#endif

void* ReservePages(size_t minimumBytes, size_t& actualBytes)
{
    // Single exit keeps reservation accounting independent of the selected source.
    // Callers hold g_lock, so the counter update is protected.
    actualBytes = 0;
    void* p = nullptr;
#if NTD_EXTERNAL_PAGES
  #if NTD_EXTERNAL_SETTERS
    if (!g_extReserve || !g_extRelease)
        detail::Trap();
    p = g_extReserve(minimumBytes, &actualBytes);
  #else
    p = ::NtdHostPageReserve(minimumBytes, &actualBytes);
  #endif
#else
    if (!RoundUpToNativePage(minimumBytes, actualBytes))
        return nullptr;
    p = OsReserve(actualBytes);
#endif
    if (p)
    {
        if (actualBytes < minimumBytes ||
            (reinterpret_cast<uintptr_t>(p) & (kPayloadAlign - 1)) != 0 ||
            static_cast<uint64_t>(actualBytes) >= kOverAlignedFlag ||
            actualBytes > kSizeMax - g_reservedBytes)
            detail::Trap();
        g_reservedBytes += actualBytes;
    }
    else
    {
        actualBytes = 0;
    }
    return p;
}

void ReleasePages(void* p, size_t bytes)
{
    g_reservedBytes -= bytes; // balances ReservePages above
#if NTD_EXTERNAL_PAGES
  #if NTD_EXTERNAL_SETTERS
    g_extRelease(p, bytes);
  #else
    ::NtdHostPageRelease(p, bytes);
  #endif
#else
    OsRelease(p, bytes);
#endif
}

inline bool AlignUpChecked(size_t v, size_t a, size_t& result)
{
    if (v > kSizeMax - (a - 1))
        return false;
    result = (v + (a - 1)) & ~(a - 1);
    return true;
}

inline bool NormalizeAlignment(size_t& align)
{
    if (!IsValidAlignment(align))
        return false;
    if (align < kPayloadAlign)
        align = kPayloadAlign;
    // Bound the base offset for extreme over-alignment requests.
    return align - 1 <= static_cast<size_t>(UINT32_MAX);
}

size_t LargeCacheTargetBytes()
{
    const size_t whole = (g_largeRecentPeakBytes / 100) * g_largeCacheConfig.target_percent;
    const size_t remainder = ((g_largeRecentPeakBytes % 100) * g_largeCacheConfig.target_percent) / 100;
    size_t target = whole + remainder;
    if (target < g_largeCacheConfig.minimum_bytes)
        target = g_largeCacheConfig.minimum_bytes;
    if (target > g_largeCacheConfig.maximum_bytes)
        target = g_largeCacheConfig.maximum_bytes;
    return target;
}

void TrimLargeCacheTo(size_t targetBytes)
{
    size_t keptBytes = 0;
    BlockHeader* lastKept = nullptr;
    BlockHeader* block = g_largeFree;

    // The list is newest-first. Keep its newest prefix and release the older
    // tail once the next complete reservation no longer fits the target.
    while (block)
    {
        const size_t blockBytes = LargeReservationBytes(block);
        if (blockBytes > targetBytes - keptBytes)
            break;
        keptBytes += blockBytes;
        lastKept = block;
        block = LargeFreeNext(block);
    }

    if (lastKept)
        SetLargeFreeNext(lastKept, nullptr);
    else
        g_largeFree = nullptr;

    while (block)
    {
        BlockHeader* next = LargeFreeNext(block);
        const size_t blockBytes = LargeReservationBytes(block);
        ReleasePages(LargeAllocationBase(block), blockBytes);
        block = next;
    }
    g_largeCachedBytes = keptBytes;
}

void MaintainLargeCacheAfterOperation()
{
    ++g_largeOperationCount;
    if (g_largeCachedBytes <= g_largeCacheConfig.maximum_bytes &&
        g_largeOperationCount < g_largeCacheConfig.operations_per_epoch)
        return;

    size_t targetBytes = LargeCacheTargetBytes();
    if (g_largeFree)
    {
        // A complete reservation is indivisible. Prefer retaining the newest
        // one when it fits the hard limit, even if it exceeds the soft target.
        const size_t newestBytes = LargeReservationBytes(g_largeFree);
        if (targetBytes < newestBytes)
            targetBytes = newestBytes;
    }
    TrimLargeCacheTo(targetBytes);
    g_largeOperationCount = 0;
    g_largeRecentPeakBytes = g_largeLiveBytes;
}

} // namespace

#if NTD_EXTERNAL_PAGES && NTD_EXTERNAL_SETTERS
void SetPageSource(PageReserveFn reserve, PageReleaseFn release)
{
    detail::SpinLockGuard g(g_lock);
    if (!reserve || !release || g_extReserve || g_extRelease)
        detail::Trap();
    g_extReserve = reserve;
    g_extRelease = release;
}
#endif

void SetLargeCacheConfig(LargeCacheConfig config)
{
    detail::SpinLockGuard g(g_lock);
    if (g_largeCacheConfigLocked ||
        config.minimum_bytes > config.maximum_bytes ||
        config.target_percent == 0 || config.target_percent > 100 ||
        config.operations_per_epoch == 0)
        detail::Trap();
    g_largeCacheConfig = config;
    g_largeCacheConfigLocked = true;
}

void TrimLargeCache()
{
    detail::SpinLockGuard g(g_lock);
    TrimLargeCacheTo(0);
    g_largeOperationCount = 0;
    g_largeRecentPeakBytes = g_largeLiveBytes;
}

void* Alloc(size_t size, size_t align)
{
    if (!NormalizeAlignment(align))
        return nullptr;
    if (size == 0)
        size = 1;

    detail::SpinLockGuard g(g_lock);

    if (align > kPayloadAlign)
    {
        constexpr size_t kOverAlignedPrefix = sizeof(uint64_t) + sizeof(BlockHeader);
        const size_t padding = align - 1;
        if (size > kSizeMax - kOverAlignedPrefix ||
            size + kOverAlignedPrefix > kSizeMax - padding)
            return nullptr;
        size_t total = 0;
        if (!AlignUpChecked(kOverAlignedPrefix + size + padding, kPayloadAlign, total))
            return nullptr;

        size_t reservedBytes = 0;
        char* base = static_cast<char*>(ReservePages(total, reservedBytes));
        if (!base)
            return nullptr;
        const uintptr_t first = reinterpret_cast<uintptr_t>(base) + kOverAlignedPrefix;
        if (first > static_cast<uintptr_t>(-1) - (align - 1))
        {
            ReleasePages(base, reservedBytes);
            return nullptr;
        }
        const uintptr_t payloadAddress = (first + (align - 1)) & ~(static_cast<uintptr_t>(align) - 1);
        BlockHeader* h = reinterpret_cast<BlockHeader*>(payloadAddress - sizeof(BlockHeader));
        const size_t headerOffset = static_cast<size_t>(reinterpret_cast<char*>(h) - base);
        *reinterpret_cast<uint64_t*>(base) = static_cast<uint64_t>(reservedBytes);
        h->metadata = kOverAlignedFlag | static_cast<uint64_t>(headerOffset);
        g_activeCapacityBytes += PayloadCapacity(h);
        if (g_activeCapacityBytes > g_peakActiveCapacityBytes)
            g_peakActiveCapacityBytes = g_activeCapacityBytes;
        return reinterpret_cast<void*>(payloadAddress);
    }

    if (size > kMaxSmall)
    {
        if (size > kSizeMax - kDefaultPrefixBytes)
            return nullptr;
        size_t total = 0;
        if (!AlignUpChecked(kDefaultPrefixBytes + size, kPayloadAlign, total))
            return nullptr;
        g_largeCacheConfigLocked = true;

        BlockHeader* h = nullptr;
        // Reuse a cached large block (first-fit, within 2x to bound waste).
        const size_t maxReusable = total > kSizeMax / 2 ? kSizeMax : total * 2;
        BlockHeader* previous = nullptr;
        for (BlockHeader* block = g_largeFree; block; block = LargeFreeNext(block))
        {
            const size_t blockBytes = LargeReservationBytes(block);
            if (blockBytes >= total && blockBytes <= maxReusable)
            {
                BlockHeader* next = LargeFreeNext(block);
                if (previous)
                    SetLargeFreeNext(previous, next);
                else
                    g_largeFree = next;
                g_largeCachedBytes -= blockBytes;
                h = block;
                break;
            }
            previous = block;
        }
        if (!h)
        {
            size_t reservedBytes = 0;
            char* base = static_cast<char*>(ReservePages(total, reservedBytes));
            if (!base)
                return nullptr;
            h = reinterpret_cast<BlockHeader*>(base + kDefaultHeaderOffset);
            h->metadata = static_cast<uint64_t>(reservedBytes);
        }
        const size_t reservationBytes = LargeReservationBytes(h);
        g_largeLiveBytes += reservationBytes;
        if (g_largeLiveBytes > g_largeRecentPeakBytes)
            g_largeRecentPeakBytes = g_largeLiveBytes;
        MaintainLargeCacheAfterOperation();
        g_activeCapacityBytes += PayloadCapacity(h);
        if (g_activeCapacityBytes > g_peakActiveCapacityBytes)
            g_peakActiveCapacityBytes = g_activeCapacityBytes;
        return Payload(h);
    }

    if (!g_sizeMapReady)
        BuildSizeMap();
    const uint32_t cls = g_sizeToClass[(size + kClassGranularity - 1) / kClassGranularity];
    const size_t classBytes = ClassCapacity(cls);

    void* reuse = g_freeList[cls];
    if (reuse)
    {
        g_freeList[cls] = *static_cast<void**>(reuse); // unlink
        g_smallCachedBytes -= kClassBlockSizes[cls];
        g_activeCapacityBytes += classBytes;
        if (g_activeCapacityBytes > g_peakActiveCapacityBytes)
            g_peakActiveCapacityBytes = g_activeCapacityBytes;
        return reuse;
    }

    const size_t blockBytes = kClassBlockSizes[cls];
    if (!g_bumpCur || static_cast<size_t>(g_bumpEnd - g_bumpCur) < blockBytes)
    {
        size_t reservedBytes = 0;
        char* mem = static_cast<char*>(ReservePages(kChunkBytes, reservedBytes));
        if (!mem)
            return nullptr;
        g_bumpCur = mem + kDefaultHeaderOffset;
        g_bumpEnd = mem + reservedBytes;
    }
    BlockHeader* h = reinterpret_cast<BlockHeader*>(g_bumpCur);
    g_bumpCur += blockBytes;
    h->metadata = cls;
    g_activeCapacityBytes += classBytes;
    if (g_activeCapacityBytes > g_peakActiveCapacityBytes)
        g_peakActiveCapacityBytes = g_activeCapacityBytes;
    return Payload(h);
}

void Free(void* p)
{
    if (!p)
        return;
    detail::SpinLockGuard g(g_lock);
    BlockHeader* h = HeaderOf(p);
    if (IsOverAligned(h))
    {
        char* base = OverAlignedBase(h);
        const size_t total = OverAlignedReservationBytes(h);
        g_activeCapacityBytes -= PayloadCapacity(h);
        ReleasePages(base, total);
        return;
    }
    if (!IsSmall(h))
    {
        const size_t total = LargeReservationBytes(h);
        g_activeCapacityBytes -= PayloadCapacity(h);
        g_largeLiveBytes -= total;
        if (total <= g_largeCacheConfig.maximum_bytes)
        {
            SetLargeFreeNext(h, g_largeFree);
            g_largeFree = h;
            g_largeCachedBytes += total;
        }
        else
        {
            ReleasePages(LargeAllocationBase(h), total);
        }
        MaintainLargeCacheAfterOperation();
        return;
    }
    const uint32_t cls = ClassIndex(h);
    g_activeCapacityBytes -= ClassCapacity(cls);
    *static_cast<void**>(p) = g_freeList[cls]; // push onto free list
    g_freeList[cls] = p;
    g_smallCachedBytes += kClassBlockSizes[cls];
}

void* AllocZeroed(size_t size, size_t align)
{
    void* p = Alloc(size, align);
    if (p)
    {
        // Small blocks reused from a free list are not zero; zero the payload.
        detail::SetBytes(p, 0, PayloadCapacity(HeaderOf(p)));
    }
    return p;
}

void* Realloc(void* p, size_t newSize, size_t align)
{
    size_t normalizedAlign = align;
    if (!NormalizeAlignment(normalizedAlign))
        return nullptr;
    if (!p)
        return Alloc(newSize, normalizedAlign);
    if (newSize == 0)
    {
        Free(p);
        return nullptr;
    }
    BlockHeader* h = HeaderOf(p);
    const size_t oldCap = PayloadCapacity(h);
    const bool alignmentSatisfied =
        (reinterpret_cast<uintptr_t>(p) & (static_cast<uintptr_t>(normalizedAlign) - 1)) == 0;
    if (newSize <= oldCap && alignmentSatisfied)
        return p; // in-place; no shrink
    void* np = Alloc(newSize, normalizedAlign);
    if (!np)
        return nullptr;
    detail::CopyBytes(np, p, oldCap < newSize ? oldCap : newSize);
    Free(p);
    return np;
}

size_t ActiveCapacityBytes()
{
    detail::SpinLockGuard g(g_lock);
    return g_activeCapacityBytes;
}
size_t BytesInUse()
{
    return ActiveCapacityBytes();
}
size_t PeakBytesInUse()
{
    detail::SpinLockGuard g(g_lock);
    return g_peakActiveCapacityBytes;
}
size_t ReservedBytes()
{
    detail::SpinLockGuard g(g_lock);
    return g_reservedBytes;
}
size_t CachedBytes()
{
    detail::SpinLockGuard g(g_lock);
    return g_smallCachedBytes + g_largeCachedBytes;
}

} // namespace mem
} // namespace ntd

// @amalgamate: config-end NTD_EMBEDDED_ALLOC
#elif NTD_EXTERNAL_MALLOC
// @amalgamate: config-begin NTD_EXTERNAL_MALLOC

// External-malloc mode: ntd::mem forwards straight to the host's malloc/free/
// realloc. No block headers, no accounting -- the host allocator owns all of that.
//   default             -> the link-time NtdHostMalloc / NtdHostFree / NtdHostRealloc.
//   --external-setters   -> function pointers installed at runtime via SetMallocFns.

#include "memory_ops.h"

namespace ntd
{
namespace mem
{

#if NTD_EXTERNAL_SETTERS // ---- runtime setters ----

namespace
{
MallocFn g_malloc = nullptr;
FreeFn g_free = nullptr;
ReallocFn g_realloc = nullptr;
detail::AtomicFlag g_setterClaimed = 0;

} // namespace

void SetMallocFns(MallocFn malloc_fn, FreeFn free_fn, ReallocFn realloc_fn)
{
    if (!malloc_fn || !free_fn)
        detail::Trap();
    if (detail::AtomicExchangeAcquire(&g_setterClaimed, 1) != 0)
        detail::Trap();

    // Publish free/realloc first; the release store of malloc last guarantees a
    // thread that observes a non-null malloc also sees a consistent free/realloc.
    detail::AtomicStorePointerRelaxed(&g_free, free_fn);
    detail::AtomicStorePointerRelaxed(&g_realloc, realloc_fn);
    detail::AtomicStorePointerRelease(&g_malloc, malloc_fn);
}

void* Alloc(size_t size, size_t align)
{
    if (!IsValidAlignment(align) || align > kDefaultAlign)
        return nullptr;
    if (size == 0)
        size = 1;
    MallocFn m = detail::AtomicLoadPointerAcquire(&g_malloc);
    if (!m)
        detail::Trap(); // no host allocator installed yet -- call SetMallocFns first
    return m(size);
}

void Free(void* p)
{
    if (!p)
        return;
    FreeFn f = detail::AtomicLoadPointerAcquire(&g_free);
    if (!f)
        detail::Trap();
    f(p);
}

void* Realloc(void* p, size_t newSize, size_t align)
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
    ReallocFn r = detail::AtomicLoadPointerAcquire(&g_realloc);
    if (!r)
        detail::Trap(); // Realloc needs a host realloc fn (never called internally)
    return r(p, newSize);
}

#else // ---- link-time host hooks (default) ----

void* Alloc(size_t size, size_t align)
{
    if (!IsValidAlignment(align) || align > kDefaultAlign)
        return nullptr;
    if (size == 0)
        size = 1;
    return ::NtdHostMalloc(size);
}

void Free(void* p)
{
    if (p)
        ::NtdHostFree(p);
}

void* Realloc(void* p, size_t newSize, size_t align)
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
    return ::NtdHostRealloc(p, newSize);
}

#endif // NTD_EXTERNAL_SETTERS

// Shared by both variants.
void* AllocZeroed(size_t size, size_t align)
{
    void* p = Alloc(size, align);
    if (p)
        detail::SetBytes(p, 0, size);
    return p;
}

size_t ActiveCapacityBytes() { return 0; } // host allocator owns accounting
size_t BytesInUse() { return 0; }
size_t PeakBytesInUse() { return 0; }
size_t ReservedBytes() { return 0; }   // host allocator owns its OS reservations
size_t CachedBytes() { return 0; }

} // namespace mem
} // namespace ntd

// @amalgamate: config-end NTD_EXTERNAL_MALLOC
#endif // allocator implementation

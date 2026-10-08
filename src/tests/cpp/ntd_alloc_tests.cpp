// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// ntd_alloc_tests.cpp -- tests for the ntd::mem allocator + ntd::New/Delete.
//
// The general tests exercise the allocator as a black box. Embedded-only sections
// pin the intentional large-cache policy. Byte accounting is meaningful only under
// the embedded allocator, so those checks are guarded to NTD_EMBEDDED_ALLOC.
//
// Surface under test: ntd::mem allocation, accounting, and cache-control APIs,
// plus ntd::New<T>(args...) and ntd::Delete<T>(p).

#include <cstdio>
#include <cstdint>
#include <cstring> // std::memset

#include "ntd_stdlib/allocator.h"
#include "ntd_stdlib/ntd_config.h"
#include "test_host_allocator.h"

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_fail;                                                                                                  \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

static bool aligned(void* p, size_t a)
{
    return (reinterpret_cast<uintptr_t>(p) % a) == 0;
}

struct Obj
{
    static int live;
    int a, b;
    Obj() : a(0), b(0)
    {
        ++live;
    }
    Obj(int x, int y) : a(x), b(y)
    {
        ++live;
    }
    ~Obj()
    {
        --live;
    }
};
int Obj::live = 0;

// External page providers may round reservations arbitrarily, changing which
// cached block satisfies the 2x reuse bound used by this pointer-identity test.
#if NTD_EMBEDDED_ALLOC && !NTD_EXTERNAL_PAGES
// ------------------------------------------------- embedded large-cache policy
static void test_embedded_large_cache_policy()
{
    ntd::mem::TrimLargeCache();

    // Free three distinct large regions, then request a size for which only the
    // middle cache entry is within the first-fit 2x reuse bound.
    void* smaller = ntd::mem::Alloc(20000);
    void* reusable = ntd::mem::Alloc(40000);
    void* larger = ntd::mem::Alloc(80000);
    CHECK(smaller != nullptr && reusable != nullptr && larger != nullptr);

    ntd::mem::Free(smaller);
    ntd::mem::Free(reusable);
    ntd::mem::Free(larger);

    void* reused = ntd::mem::Alloc(35000);
    CHECK(reused == reusable);

    // Removing the middle entry must preserve both adjacent cache links.
    void* reused_larger = ntd::mem::Alloc(70000);
    void* reused_smaller = ntd::mem::Alloc(15000);
    CHECK(reused_larger == larger);
    CHECK(reused_smaller == smaller);

    ntd::mem::Free(reused);
    ntd::mem::Free(reused_larger);
    ntd::mem::Free(reused_smaller);

    CHECK(ntd::mem::CachedBytes() > 0);
    ntd::mem::TrimLargeCache();
    CHECK(ntd::mem::CachedBytes() == 0);
    CHECK(ntd::mem::ReservedBytes() == 0);
}

// ------------------------------------------------- adaptive large-cache limits
static void test_embedded_large_cache_limits()
{
    constexpr ntd::mem::LargeCacheConfig default_config;
    ntd::mem::TrimLargeCache();

    // Crossing the hard limit trims older entries toward half of the recent
    // live-large peak while retaining the newest reusable regions.
    void* large[9];
    for (void*& p : large)
    {
        p = ntd::mem::Alloc(8u << 20);
        CHECK(p != nullptr);
    }
    const size_t peak_reserved = ntd::mem::ReservedBytes();
    for (void* p : large)
        ntd::mem::Free(p);
    CHECK(ntd::mem::ReservedBytes() < peak_reserved);
    CHECK(ntd::mem::CachedBytes() <= default_config.maximum_bytes);
    ntd::mem::TrimLargeCache();
    CHECK(ntd::mem::CachedBytes() == 0);
    CHECK(ntd::mem::ReservedBytes() == 0);

    // An epoch of small large-allocation operations is trimmed to the 1 MiB
    // minimum target even when it never reaches the hard limit.
    void* epoch[128];
    for (void*& p : epoch)
    {
        p = ntd::mem::Alloc(10000);
        CHECK(p != nullptr);
    }
    for (void* p : epoch)
        ntd::mem::Free(p);
    CHECK(ntd::mem::CachedBytes() > 0);
    CHECK(ntd::mem::CachedBytes() <= default_config.minimum_bytes);
    ntd::mem::TrimLargeCache();
    CHECK(ntd::mem::CachedBytes() == 0);
    CHECK(ntd::mem::ReservedBytes() == 0);
}
#endif

// ------------------------------------------------------------ basic Alloc/Free
static void test_alloc_free()
{
    void* p = ntd::mem::Alloc(128);
    CHECK(p != nullptr);
    CHECK(aligned(p, ntd::mem::kDefaultAlign));
    std::memset(p, 0xAB, 128); // writable across the full request
    CHECK(*(reinterpret_cast<unsigned char*>(p) + 127) == 0xAB);
    ntd::mem::Free(p);

    ntd::mem::Free(nullptr); // Free(nullptr) is a safe no-op
    CHECK(true);

    // a spread of sizes, all non-null + aligned
    static const size_t sizes[] = {1, 8, 16, 24, 64, 100, 256, 1000, 4096, 65536, 200000};
    for (size_t sz : sizes)
    {
        void* q = ntd::mem::Alloc(sz);
        CHECK(q != nullptr && aligned(q, ntd::mem::kDefaultAlign));
        reinterpret_cast<unsigned char*>(q)[sz - 1] = 0x5A; // last byte writable
        ntd::mem::Free(q);
    }

    CHECK(ntd::mem::Alloc(8, 3) == nullptr); // alignment must be a power of two

#if NTD_EMBEDDED_ALLOC
    CHECK(ntd::mem::Alloc(static_cast<size_t>(-1)) == nullptr); // size arithmetic must not wrap

    // Over-aligned blocks preserve both alignment and contents when grown.
    static const size_t alignments[] = {
        ntd::mem::kDefaultAlign * 2,
        ntd::mem::kDefaultAlign * 4,
        4096,
    };
    for (size_t a : alignments)
    {
        void* q = ntd::mem::Alloc(37, a);
        CHECK(q != nullptr && aligned(q, a));
        std::memset(q, 0x6B, 37);
        q = ntd::mem::Realloc(q, 9000, a);
        CHECK(q != nullptr && aligned(q, a));
        bool preserved = true;
        for (size_t i = 0; i < 37; ++i)
            if (static_cast<unsigned char*>(q)[i] != 0x6B)
                preserved = false;
        CHECK(preserved);
        ntd::mem::Free(q);
    }
#else
    // Malloc-style backends reject over-alignment rather than returning a
    // pointer that does not satisfy the request.
    CHECK(ntd::mem::Alloc(37, ntd::mem::kDefaultAlign * 2) == nullptr);
#endif
}

// ----------------------------------------------------------------- AllocZeroed
static void test_alloc_zeroed()
{
    const size_t n = 4096;
    unsigned char* p = static_cast<unsigned char*>(ntd::mem::AllocZeroed(n));
    CHECK(p != nullptr && aligned(p, ntd::mem::kDefaultAlign));
    bool all_zero = true;
    for (size_t i = 0; i < n; ++i)
        if (p[i] != 0)
        {
            all_zero = false;
            break;
        }
    CHECK(all_zero);
    ntd::mem::Free(p);
}

// --------------------------------------------------------------------- Realloc
static void test_realloc()
{
    // Realloc(nullptr, n) behaves like Alloc
    unsigned char* p = static_cast<unsigned char*>(ntd::mem::Realloc(nullptr, 64));
    CHECK(p != nullptr);
    for (int i = 0; i < 64; ++i)
        p[i] = static_cast<unsigned char>(i);

    // grow: existing content preserved
    p = static_cast<unsigned char*>(ntd::mem::Realloc(p, 4096));
    CHECK(p != nullptr);
    bool preserved = true;
    for (int i = 0; i < 64; ++i)
        if (p[i] != static_cast<unsigned char>(i))
        {
            preserved = false;
            break;
        }
    CHECK(preserved);
    p[4095] = 0x11; // new tail writable
    CHECK(p[4095] == 0x11);

    // shrink: prefix preserved
    p = static_cast<unsigned char*>(ntd::mem::Realloc(p, 32));
    CHECK(p != nullptr);
    preserved = true;
    for (int i = 0; i < 32; ++i)
        if (p[i] != static_cast<unsigned char>(i))
        {
            preserved = false;
            break;
        }
    CHECK(preserved);
    ntd::mem::Free(p);
}

// ---------------------------------------------------------------- New / Delete
static void test_new_delete()
{
    Obj::live = 0;
    Obj* a = ntd::New<Obj>(); // default ctor
    CHECK(a != nullptr && a->a == 0 && a->b == 0 && Obj::live == 1);
    ntd::Delete(a);
    CHECK(Obj::live == 0);

    Obj* b = ntd::New<Obj>(3, 4); // forwarded args
    CHECK(b != nullptr && b->a == 3 && b->b == 4 && Obj::live == 1);
    CHECK(aligned(b, alignof(Obj)));
    ntd::Delete(b);
    CHECK(Obj::live == 0);

    // many objects, all destroyed
    Obj* arr[64];
    for (int i = 0; i < 64; ++i)
        arr[i] = ntd::New<Obj>(i, i);
    CHECK(Obj::live == 64);
    for (int i = 0; i < 64; ++i)
    {
        CHECK(arr[i]->a == i);
        ntd::Delete(arr[i]);
    }
    CHECK(Obj::live == 0);
}

// --------------------------------------------- byte accounting (embedded only)
static void test_accounting()
{
#if NTD_EMBEDDED_ALLOC
    ntd::mem::TrimLargeCache();
    size_t base = ntd::mem::BytesInUse();

    void* zero = ntd::mem::Alloc(0);
    CHECK(ntd::mem::ActiveCapacityBytes() > base);
    ntd::mem::Free(zero);

    void* tiny = ntd::mem::Alloc(7);
    CHECK(ntd::mem::BytesInUse() == base + 8); // 8-byte payload class
    ntd::mem::Free(tiny);
    CHECK(ntd::mem::BytesInUse() == base);

    #if !NTD_EXTERNAL_PAGES
    // Native reservations account for the complete OS-page-rounded region,
    // including the tail beyond the allocator's minimum request.
    const size_t rounded_reserved_base = ntd::mem::ReservedBytes();
    const size_t rounded_cached_base = ntd::mem::CachedBytes();
    void* rounded = ntd::mem::Alloc(9000);
    const size_t rounded_region = ntd::mem::ReservedBytes() - rounded_reserved_base;
    CHECK(rounded != nullptr);
    CHECK(rounded_region > 9000 + 2 * ntd::mem::kDefaultAlign);
    ntd::mem::Free(rounded);
    CHECK(ntd::mem::CachedBytes() - rounded_cached_base == rounded_region);
    ntd::mem::TrimLargeCache();
    CHECK(ntd::mem::ReservedBytes() == rounded_reserved_base);
    #endif

    void* small_cached = ntd::mem::Alloc(8000);
    const size_t cached_while_live = ntd::mem::CachedBytes();
    ntd::mem::Free(small_cached);
    CHECK(ntd::mem::CachedBytes() > cached_while_live);

    const size_t reserved_base = ntd::mem::ReservedBytes();
    const size_t cached_base = ntd::mem::CachedBytes();
    void* p = ntd::mem::Alloc(100000);
    size_t after = ntd::mem::BytesInUse();
    CHECK(after > base);                        // in-use grew
    CHECK(after >= base + 100000);              // by at least the request
    CHECK(ntd::mem::PeakBytesInUse() >= after); // peak covers current
    CHECK(ntd::mem::ReservedBytes() >= after);  // reserved covers in-use

    ntd::mem::Free(p);
    CHECK(ntd::mem::BytesInUse() == base);            // balanced back to baseline
    CHECK(ntd::mem::ReservedBytes() > reserved_base); // freed large region is cached
    CHECK(ntd::mem::CachedBytes() > cached_base);
    ntd::mem::TrimLargeCache();
    CHECK(ntd::mem::CachedBytes() == cached_base);
    CHECK(ntd::mem::ReservedBytes() == reserved_base);

    // New/Delete participate in accounting too
    size_t b2 = ntd::mem::BytesInUse();
    Obj* o = ntd::New<Obj>(1, 2);
    CHECK(ntd::mem::BytesInUse() > b2);
    ntd::Delete(o);
    CHECK(ntd::mem::BytesInUse() == b2);

    // peak is monotonic non-decreasing across a big-then-small pattern
    size_t peak_before = ntd::mem::PeakBytesInUse();
    void* big = ntd::mem::Alloc(500000);
    size_t peak_mid = ntd::mem::PeakBytesInUse();
    CHECK(peak_mid >= peak_before && peak_mid >= base + 500000);
    ntd::mem::Free(big);
    CHECK(ntd::mem::PeakBytesInUse() == peak_mid); // peak does not drop after free

    // churn: many alloc/free, in-use returns to baseline (no accounting leak)
    size_t churn_base = ntd::mem::BytesInUse();
    void* keep[128];
    for (int i = 0; i < 128; ++i)
        keep[i] = ntd::mem::Alloc((size_t)(i + 1) * 37);
    for (int i = 0; i < 128; ++i)
        ntd::mem::Free(keep[i]);
    CHECK(ntd::mem::BytesInUse() == churn_base);

    #if NTD_EXTERNAL_PAGES
    // The test host rounds every request to 64 KiB. Accounting must use the
    // host-reported region sizes rather than the allocator's minimum requests.
    CHECK(ntd::mem::ReservedBytes() % (64u * 1024u) == 0);
    #endif
#else
    // non-embedded: accounting is a stub (0); just confirm the calls are callable + consistent.
    CHECK(ntd::mem::ActiveCapacityBytes() == ntd::mem::BytesInUse());
    CHECK(ntd::mem::PeakBytesInUse() >= ntd::mem::BytesInUse());
    (void)ntd::mem::ReservedBytes();
    (void)ntd::mem::CachedBytes();
#endif
}

int main()
{
    test_install_host_allocator();
    std::printf("ntd_alloc_tests: %s\n", // note: the two conditional literals concatenate into one arg
#if NTD_OWN_STDLIB
                "ntd containers, "
#else
                "std containers, "
#endif
#if NTD_EMBEDDED_ALLOC
                "embedded allocator (accounting checked)"
#elif NTD_EXTERNAL_MALLOC
                "external malloc"
#elif defined(LIBMP_WASM_BUILD)
                "Emscripten linear-memory allocator (accounting stubbed)"
#else
                "libc malloc (accounting stubbed)"
#endif
    );
#if NTD_EMBEDDED_ALLOC && !NTD_EXTERNAL_PAGES
    test_embedded_large_cache_policy();
    test_embedded_large_cache_limits();
#endif
    test_alloc_free();
    test_alloc_zeroed();
    test_realloc();
    test_new_delete();
    test_accounting();

    std::printf("ntd_alloc_tests: %d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0)
        std::printf("NTD ALLOC TESTS OK\n");
    return g_fail == 0 ? 0 : 1;
}

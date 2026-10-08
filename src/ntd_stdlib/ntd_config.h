// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// INDEPENDENT axes controlling the ntd memory / stdlib / io behavior. Combine freely.
// Each is a plain 0/1 macro; set the ones you want at build time (e.g.
// -DNTD_OWN_STDLIB=1). Unset macros default to 0 below.
// See README.md for detailed behavior and integration guidance.
//
//   Axis A -- containers / std primitives:
//     NTD_OWN_STDLIB=0 (default)     std implementations. Allocator-aware ntd
//                                    types use ntd::allocator when axis B selects
//                                    a custom backend; otherwise they alias std.
//     NTD_OWN_STDLIB=1               reserved extension points for standalone ntd::
//                                    implementations; unavailable until populated.
//
//   Axis B -- ntd::mem backend (pick at most one non-default):
//     (default)                      platform malloc/free/realloc
//     NTD_EMBEDDED_ALLOC=1           the embedded page allocator (no libc malloc).
//                                    ntd containers and ntd::New/Delete route here;
//                                    process-wide operator new is left untouched.
//                                    Requires VirtualAlloc or mmap.
//     NTD_EXTERNAL_PAGES=1           the embedded allocator takes its raw pages from
//                                    the host (requires NTD_EMBEDDED_ALLOC=1 too).
//     NTD_EXTERNAL_MALLOC=1          instead of the embedded allocator, forward ntd
//                                    allocations to host malloc/free/realloc. Excludes
//                                    the embedded allocator.
//
//   The two external modes get the host's functions at LINK TIME by default: the host
//   defines NtdHostMalloc / NtdHostFree / NtdHostRealloc (external malloc) or NtdHost
//   PageReserve / NtdHostPageRelease (external pages). PageReserve receives a minimum
//   size and reports the actual zeroed region size. A missing function is a link error,
//   and no init is needed (works even before main()). Set
//     NTD_EXTERNAL_SETTERS=1         to instead install them at runtime, via
//                                    ntd::mem::SetMallocFns / SetPageSource.
//   Axis C -- native file I/O:
//     (default)                      std::ifstream / std::ofstream
//     NTD_OWN_FILEIO=1               minimal OS-level file I/O (POSIX/Win32), no std.
//
// The axes compose subject to the backend constraints checked below. For example,
// (ntd + malloc) isolates the container change and (std + embedded) isolates the
// allocator change.
//
// The single allocation entry point is ntd::mem::Alloc/Free (allocator.h), present in
// every mode with the selected backend, so all ntd-owned allocation sites call it
// uniformly regardless of the flags above.

// ---- normalize: every axis is a defined 0/1 macro from here on ----
#ifndef NTD_OWN_STDLIB
    #define NTD_OWN_STDLIB 0
#endif
#if NTD_OWN_STDLIB
    #error "NTD_OWN_STDLIB is reserved but its embedded STL primitives are not implemented"
#endif
#ifndef NTD_EMBEDDED_ALLOC
    #define NTD_EMBEDDED_ALLOC 0
#endif
#ifndef NTD_EXTERNAL_PAGES
    #define NTD_EXTERNAL_PAGES 0
#endif
#ifndef NTD_EXTERNAL_MALLOC
    #define NTD_EXTERNAL_MALLOC 0
#endif
#ifndef NTD_EXTERNAL_SETTERS
    #define NTD_EXTERNAL_SETTERS 0
#endif
#ifndef NTD_OWN_FILEIO
    #define NTD_OWN_FILEIO 0
#endif
// Derived backend predicate for implementation/tests; never set it independently.
#ifdef NTD_HAS_CUSTOM_ALLOCATOR_BACKEND
    #error "NTD_HAS_CUSTOM_ALLOCATOR_BACKEND is derived; select NTD_EMBEDDED_ALLOC or NTD_EXTERNAL_MALLOC instead"
#endif
#define NTD_HAS_CUSTOM_ALLOCATOR_BACKEND (NTD_EMBEDDED_ALLOC || NTD_EXTERNAL_MALLOC)

// Derived stdlib/type-flavor predicates. With std implementations plus a custom
// backend, allocator-aware containers use ntd::allocator and are distinct from
// their default-allocator std counterparts.
#ifdef NTD_ROUTED_STD_TYPES
    #error "NTD_ROUTED_STD_TYPES is derived; do not define it independently"
#endif
#define NTD_ROUTED_STD_TYPES (!NTD_OWN_STDLIB && NTD_HAS_CUSTOM_ALLOCATOR_BACKEND)

#ifdef NTD_TYPES_DISTINCT_FROM_STD
    #error "NTD_TYPES_DISTINCT_FROM_STD is derived; do not define it independently"
#endif
#define NTD_TYPES_DISTINCT_FROM_STD (NTD_OWN_STDLIB || NTD_ROUTED_STD_TYPES)

// External pages is the embedded allocator fed by a host page source, so it must be
// enabled alongside it -- require both explicitly rather than implying one.
#if NTD_EXTERNAL_PAGES && !NTD_EMBEDDED_ALLOC
    #error "NTD_EXTERNAL_PAGES requires NTD_EMBEDDED_ALLOC=1 (enable both)"
#endif
#if NTD_EXTERNAL_MALLOC && NTD_EMBEDDED_ALLOC
    #error "NTD_EXTERNAL_MALLOC is mutually exclusive with the embedded allocator (NTD_EMBEDDED_ALLOC / NTD_EXTERNAL_PAGES)"
#endif

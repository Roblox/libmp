# ntd_stdlib — standard-library adaptation and allocator routing

Provides thin wrappers over C++ standard-library primitives and a selectable
allocation layer. With no flags the headers alias standard-library types and
`ntd::mem` forwards to platform malloc.

No embedded STL-primitives implementation is included. `NTD_OWN_STDLIB` and the
corresponding header extension points are reserved for providing one with full
libstdc++ autonomy; `ntd_config.h` rejects enabling that incomplete mode.

## Design

`ntd_config.h` is the authoritative definition of the three independent build
axes and validates their combinations. Each axis uses plain `0/1` macros that
default to 0. The memory backend is `NTD_EMBEDDED_ALLOC` or
`NTD_EXTERNAL_MALLOC`, with `NTD_EXTERNAL_PAGES` and `NTD_EXTERNAL_SETTERS` as
modifiers:

| macro | axis | effect |
|------|------|--------|
| `NTD_OWN_STDLIB` | A | reserved extension points for embedded STL primitives and full libstdc++ autonomy; value 1 is unavailable until those implementations are supplied. Value 0 uses standard implementations parameterized with `ntd::allocator` when axis B selects a custom backend |
| `NTD_EMBEDDED_ALLOC` | B | use the embedded arena allocator, backed by `VirtualAlloc` or `mmap`, as the `ntd::mem` backend. Global `operator new/delete` remains untouched |
| `NTD_EXTERNAL_PAGES` | B | embedded allocator takes raw pages from the host — link-time `NtdHostPageReserve`/`Release` (or `SetPageSource` under `NTD_EXTERNAL_SETTERS`); **requires `NTD_EMBEDDED_ALLOC=1`** |
| `NTD_EXTERNAL_MALLOC` | B | forward `ntd::mem` to the host's `malloc`/`free`/`realloc` — link-time `NtdHostMalloc`/`Free`/`Realloc` (or `SetMallocFns` under `NTD_EXTERNAL_SETTERS`); excludes the embedded allocator |
| `NTD_EXTERNAL_SETTERS` | B | provide the external hooks at runtime via the `Set*` setters instead of the default link-time `NtdHost*` symbols |
| `NTD_OWN_FILEIO` | C | OS-level file I/O instead of `std::ifstream/ofstream` |

Single allocation entry point: `ntd::mem::Alloc/Free` (`allocator.h`), present in
every mode (platform malloc when axis B is default, the embedded heap under
`NTD_EMBEDDED_ALLOC`, or the host's functions under `NTD_EXTERNAL_MALLOC`).
An allocation without an explicit alignment uses the platform's fundamental
`alignof(max_align_t)` alignment on every backend. Typed allocation sites request
`alignof(T)` when it is stronger; the embedded backend supports such over-aligned
requests, while malloc-style backends reject them.
`NTD_EXTERNAL_PAGES` changes the embedded heap's page source rather than replacing
its allocation algorithm. All ntd-owned allocation sites call `ntd::mem` uniformly.
The external modes bind the host's functions at **link time** by default — the host defines the `extern "C"`
`NtdHost*` symbols (missing → link error), which need no init and are valid even
before `main()`; add `NTD_EXTERNAL_SETTERS=1` to install them at runtime via the
`Set*` setters instead. `PageReserve` receives a minimum byte count and reports
the actual size of its zeroed reservation; NTD can use that complete region and
passes the reported size back to `PageRelease`. Runtime callback sets are complete,
one-shot registrations; an incomplete/repeated registration, invalid returned region,
or allocator use before registration traps.
The implementation performs no dynamic initialization or allocation before `main()`.

When `NTD_OWN_STDLIB=0` is combined with a custom allocator backend, ntd uses
standard container implementations parameterized with `ntd::allocator<T>`.
Allocator-aware primitives therefore retain standard behavior while routing
storage through `ntd::mem`; they are intentionally different C++ types from
their default-allocator `std` counterparts. `ntd::function` and the smart-pointer
factories use the small allocator-routed ntd implementations in this combination,
because C++17 `std::function` has no allocator parameter and raw-pointer smart-
pointer construction cannot safely establish allocator provenance.

| `NTD_OWN_STDLIB` | custom backend | internal `ntd` family | same type as default `std`? |
|---:|---:|---|---:|
| 0 | 0 | direct standard aliases | yes |
| 0 | 1 | standard implementations with `ntd::allocator` and routed ownership helpers | no |
| 1 | either | reserved, not implemented | no |

`NTD_OWN_FILEIO` remains independent. Standard streams have no allocator template
parameter, so select the OS-level implementation when file-I/O allocations must
also avoid the host C++ standard library.

## Files

Public compatibility headers mirror `std`'s layout one-to-one (so `<vector>` ↔
`vector.h`, etc.) and are each independently includable. Library code includes
the specific headers it needs and always spells the type `ntd::vector`; in
supported builds that resolves to a standard vector with `ntd::allocator` or an
exact `std::vector` alias. Each header retains the conditional location where a
future `NTD_OWN_STDLIB` implementation belongs.
When no custom backend is selected, `memory.h` keeps scalar smart pointers as
their exact `std` types but filters out array ownership so the supported `ntd`
surface is consistent.

- **Configuration and umbrella:** `ntd_config.h` defines the build axes and
  feature macros. `ntd.h` includes the public allocator, utility, and container
  surface for consumers that prefer one convenience include; `file_io.h` remains
  an explicit include because it has its own build axis. Library code uses
  specific headers.
- **Private implementation foundation:** `compiler.h` contains compiler-specific
  traps and atomics; `memory_ops.h` contains allocation-free raw byte/string
  operations; `spin_lock.h` builds a reusable allocation-free lock and scoped
  guard over those atomics. `hashtable.h` reserves the implementation point for
  future standalone unordered containers. These are implementation details
  rather than user-facing compatibility headers.
  In particular, `memory_ops.h` deliberately cannot depend on the allocator.
- **Public allocation API:** `allocator.h` / `allocator.cpp` provide the memory
  backends: platform malloc by default; the embedded page allocator; or, under
  `NTD_EXTERNAL_MALLOC`, a thin forward to host `malloc`/`free`/`realloc`. The
  embedded allocator uses size-class free-lists over mmap/VirtualAlloc arenas,
  8-byte common block headers, and an adaptive large-region cache. The cache
  targets half the recent live-large peak, clamped to 1–32 MiB and reevaluated
  every 256 large operations; 32 MiB is a hard ceiling. Before the first
  cache-eligible large allocation, `ntd::mem::SetLargeCacheConfig()` may replace
  these defaults or disable the cache. It supports an optional external page source,
  and `ntd::mem::TrimLargeCache()` releases cached regions explicitly.
  Accounting distinguishes live allocation capacity including internal fragmentation
  (`ActiveCapacityBytes`), all reusable freed blocks (`CachedBytes`), and complete
  OS/host reservations (`ReservedBytes`). `BytesInUse` remains a compatibility name
  for `ActiveCapacityBytes`.
  Also defines `ntd::New<T>` /
  `ntd::Delete` (typed alloc + construct / destroy + free over `ntd::mem`), which the
  library uses in place of bare `new`/`delete`. The host application's global
  `operator new/delete` remains untouched.
  `ntd::mem::RoutedAllocation` provides class-scoped new/delete for polymorphic
  implementation bases whose compiler-generated deleting destructors need them;
  it does not alter allocation for unrelated host classes.
- **Public fundamental/value utilities:** `cstring.h` exposes the standard-named
  `ntd::memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strnlen`, and `tolower`
  operations.
  The remaining headers are `type_traits.h`, `utility.h` (move/forward/swap,
  `pair`, index_sequence), `tuple.h`, `algorithm.h`, `iterator.h`, `limits.h`,
  `array.h`, and `optional.h`.
- **Public containers, strings, pointers, and callables:** `vector.h`, `set.h`,
  `string.h`, `string_view.h`, `deque.h`, `list.h`, `unordered_map.h` +
  `unordered_set.h`, `memory.h` (`unique_ptr`/`shared_ptr`), `functional.h`
  (`function`, `hash`, `equal_to`, `less`).
  The default-deleter `ntd::unique_ptr` accepts storage from `ntd::make_unique`
  or an explicitly tagged `ntd::adopt_ntd` pointer; ambiguous
  `ntd::unique_ptr<T>(new T)` construction is intentionally rejected. Default-deleter
  array ownership (`ntd::unique_ptr<T[]>`) is also explicitly unsupported; use an
  `ntd::vector<T>` when dynamic contiguous storage is required. Default-deleter
  derived-to-base ownership conversion is supported only when it preserves the original
  allocation address; pointer-adjusting secondary/virtual-base conversions trap before
  ownership transfers. Likewise, construct `ntd::shared_ptr` through `ntd::make_shared`,
  not a raw pointer. Direct `ntd::Delete` and `adopt_ntd` use require the exact address
  returned by `ntd::New`.
- **Public file I/O:** `file_io.h` provides `ifstream`/`ofstream` plus `ios`,
  `streamoff`, and `streamsize`. It is self-switching like the containers:
  OS-level under `NTD_OWN_FILEIO`, otherwise aliases the `std::` types.

## Build and verify

Build options and verification commands are documented in
[`scripts/BUILDING.txt`](../scripts/BUILDING.txt).

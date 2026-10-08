# Prompt: translate LibMP to `%TARGET_LANGUAGE%`

LibMP provides programmatic access to Roblox MicroProfiler capture data. This
is a base prompt for AI agents translating LibMP into a native target-language
library.

Translate the current LibMP source code into `%TARGET_LANGUAGE%` as a native,
single-file library. If LibMP already has a WebAssembly-backed single-file
library for `%TARGET_LANGUAGE%`, the result must be its drop-in replacement. For
a new target language, reproduce LibMP's public API and observable behavior and
add the new implementation to the parity checks against C++. The result must not
embed or call the LibMP WebAssembly module, native library, another language
package, or command-line tool.

Audit the implementation file by file and class by class, in dependency order,
so no required behavior is missed. Preserve short section comments that map
each translated subsystem back to its original source files and principal
classes, so the translation remains easy to correlate with the C++
implementation. Use the API reference and existing language libraries to verify
the complete public API and its observable behavior; do not expose private
implementation details.

Focus the implementation on consuming and analyzing existing performance
captures rather than producing them. Internal capture-construction,
serialization, and other producer-only paths that are not reachable through the
required public API may be omitted. Keep public API compatibility stubs for
unsupported producer operations as described below.

Preserve supported capture formats and inputs, FlatBuffers data, sessions,
providers, descriptors, iterators, labels, caches, locks, slots, streams,
cleanup, errors, and malformed-data validation. Use target-native integer types
where they preserve the required width and semantics. Where exact native 64-bit
integers are unavailable, use split or equivalent exact representations only
for operations requiring full-width precision; use faster numeric
representations where practical values remain exact. Methods that are part of
the public API but unavailable in a standalone environment must remain present
and return their documented unsupported result.

For a Luau target, reproduce the existing control-channel initialization and
handshake sequence exactly, including backend error handling, version
compatibility checks, the library-warning version check, and its warning
message. Preserve the ordering and fallback behavior rather than replacing the
handshake with an assumed-success stub.

For a Luau target, also preserve external data-change handling: a change
notification for a session's slot marks that session as requiring
synchronization, and its next data access performs the sync. Synchronization may
replace only some data blocks; invalidate caches that depend on changed or
removed blocks while preserving unaffected cached objects, and honor explicit
full-cache invalidation. Apply the corresponding invalidation to label caches as
well.

Use idiomatic `%TARGET_LANGUAGE%` designs for speed and safety instead of
mechanically reproducing C++ templates, pointers, ownership, or containers.
Implementation details such as `HybridLookupTable` and `HybridLookupSet` may be
substantially replaced with suitable native arrays, maps, sets, buffers, and
integer operations. Avoid unnecessary allocation, copying, conversion, and
dynamic lookup in per-entry and per-sample paths. Preserve API identity and
lifetime behavior even when the internal design differs.

Alongside the library, produce `TRANSLATION-NOTES.txt` documenting the
high-level technical decisions and tradeoffs made during the translation and
the reasons for them. Include significant departures from the C++ design,
target-native representations or optimizations, dependency choices, and
intentionally unsupported producer paths. Also record target-runtime and
capability assumptions, reproduction steps, and the test and benchmark setup
and results.

In a sandboxed runtime such as Luau, extra defensive checks, including integer
overflow checks in measured hot paths, may be omitted for performance only when
the user explicitly requests it and confirms that the library will receive
properly formatted input from trusted sources. Document this trust model in
`TRANSLATION-NOTES.txt` and ensure failures remain contained by the sandbox.

Support LZ4 decompression, but not compression. Vendor or translate only the
required decompressor from an implementation whose license permits embedding.
For reference, LibMP's C++ implementation uses vendored upstream LZ4 C sources
under the BSD 2-Clause License. Keep public dump-saving and conversion methods,
but return a clear unsupported result for operations that require LZ4
compression. Do not silently emit another format. Preserve all notices required
by the embedded implementation's license.

The finished library must:

- be one self-contained `%TARGET_LANGUAGE%` source file;
- expose the complete original public API without unrelated functionality or
  convenience extensions;
- follow target-language conventions where they do not break drop-in API
  compatibility;
- use target-native cleanup and, where available, a safe finalizer or GC
  fallback;
- contain no placeholders, test fixtures, build code, machine-specific paths,
  unused generated code, or unexplained behavioral differences; and
- be reproducible from the same source revision, dependency revisions, tools,
  and supplied build timestamp.

Begin the file with a header, written in `%TARGET_LANGUAGE%` comments, that
contains:

- `LibMP for %TARGET_LANGUAGE%` and a brief description;
- `https://github.com/Roblox/libmp`;
- a clear statement that an AI agent translated the artifact from the original
  LibMP source, including the exact source commit;
- the build timestamp and LibMP library and data-format versions;
- a statement that LibMP and the translated LibMP code are distributed under
  the MIT License, with
  `https://github.com/Roblox/libmp/blob/HEAD/LICENSE`;
- every bundled and external dependency actually used, with its license name,
  license link, source link, and pinned version or revision;
- any copyright or notice text required by those dependency licenses;
- supported runtime versions and all runtime dependencies, or a statement that
  no external runtime dependencies are required; and
- a brief executable example that opens `capture.x.html` (noting on the same
  line that `capture.gprx` is also accepted), reads one value, and releases
  resources correctly; and
- at the end of the header, a concise `Compatibility deviations` section that
  completely lists every user-requested or implementation-introduced deviation
  from this prompt or observable LibMP behavior affecting the public API,
  behavior, supported features or formats, trust model, dependencies, or
  runtime requirements; state `None` when there are no such deviations.

State licensing precisely: translated LibMP portions use LibMP's MIT License;
bundled third-party portions retain their own licenses. List only dependencies
that are actually used. Keep internal translation choices that do not affect
compatibility in `TRANSLATION-NOTES.txt` rather than the library header.

Add a target-language smoke test and integrate the implementation into the
cross-language parity test. The result must pass all applicable LibMP tests and
complete parity with C++ for supported capture-reading and analysis behavior,
without skipped reader cases or weakened assertions. Test intentionally
unsupported public producer methods for their documented result. Test malformed
inputs according to the declared trust model, as well as object and cache
lifetimes, cleanup, and unsupported compression paths. Benchmark representative
captures and replace non-native hot-path designs when needed.

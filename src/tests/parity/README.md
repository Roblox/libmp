# Cross-language parity format

`parity_tool.cpp` is the reference traversal for the common public LibMP dump-data model.
It emits a deterministic, line-oriented text format for byte-for-byte comparison
across languages. The Luau, JavaScript, C#, Python, and Go readers emit the same
representation through their public wrapper APIs, and the parity driver compares every
emitted line byte-for-byte.

The runner reconstructs its default GPRX input from the committed source-data fixture; no
binary capture needs to be stored in the repository. Pass `--input path/to/capture.gprx`
to `src/scripts/test_parity.py` or `run_parity.py` to compare the readers on another file.
The option accepts multiple arguments and may be repeated; directories and quoted glob
patterns expand to deterministic, deduplicated file lists.
Fixture-specific event-kind and related-snapshot cache coverage remain attached to the
default run, because an override may legitimately contain different events and has no
corresponding metadata-update snapshots.

The canonical representation covers session metadata, global/general information, groups,
timers and group membership, counters and every stored sample, threads, frames and embedded
thread-log descriptors, place/UTC samples, public name lookup/filter results, the complete
counter traversal, and every public `LogIteratorState` report together with its current
logical-entry count. Raw entry values are intentionally excluded to keep the representation
compact and avoid two extra WASM calls per 64-bit word. Native regression tests compare every
word exactly, while wrapper smoke tests exercise both 32-bit part accesses.

Every implementation opens the capture through its trusted in-memory API and uses `Fetch*`
for owned descriptors, arrays, samples, and frames. Iterator state uses `GetState` because it
is a live view with no fetched counterpart. Keeping those choices aligned makes the parity
comparison exercise the same public API semantics rather than only equivalent output.

Canonical scalar rules are deliberately small and language-neutral:

- ordinary integers and booleans use decimal tokens (`0` or `1` for booleans);
- `uint64` values use 16 lowercase hexadecimal digits;
- floats and doubles use their 8- or 16-digit IEEE bit representation;
- strings use `<byte-count>:<lowercase-hex-bytes>`, with `-` for a missing string;
- array order and iterator order are preserved rather than sorted.

Each wrapper runner also checks the common object-cache contract without adding records to
the canonical output: `Get*` and `Fetch*` values agree, enabled caches preserve observable
object identity, clearing or disabling the cache removes that identity, and independently
created array handles retain equal values. The default related snapshots additionally prove
that synchronization invalidates only the metadata bucket that changed. The language smoke
tests retain their complementary WASM-memory-growth and provider integration checks.

To add a language, port the ordered `Write*` functions in `parity_tool.cpp`, add one
`ReaderRunner` entry in `create_runners()` in `run_parity.py`, and implement the same cache
contract in that language's parity program. Do not weaken the structural validation when a
wrapper lacks an API; document and exclude that field from the common surface instead.

Direct CTest runs require the C++ and Luau baseline and include any other wrapper artifact
whose runtime is available. `src/scripts/test_parity.py` builds all wrappers and enables
the driver's `--require-all` mode so missing JavaScript, C#, Python, or Go coverage fails.

Malformed-input rejection belongs to the native untrusted-reader tests and fuzzers. The
compact WASM wrappers intentionally expose only trusted-input readers, so cross-language
parity does not impose untrusted validation behavior on them.

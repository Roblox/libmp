# Testing and fuzzing

The test suite is intentionally layered around behavior and trust boundaries rather than
a line-coverage target.

## Native regression suite

From the repository root, configure a Release build, compile it, and run the native suite
with:

```sh
python3 src/scripts/test_native.py
```

Add `--clean` for a clean rebuild. The equivalent manual CMake sequence is:

```sh
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIBMP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For complete validation, add `--amalgamation-matrix-tests` to the wrapper command to also
compile the generated single-file library across its supported type configurations.

The suites cover raw/hex headers, every payload codec, memory/file/chunk/external-I/O data
sources, global indexes, loaded-plus-appended containers, KV single/array/log/proxy paths,
public C ABI failure contracts, control framing, engine-service argument validation, and
CaptureBuilder-to-reader operation. The capture tests rebuild 16 frames extracted from a
real profile in `roundtrip_capture_data.inc`; binary dump files are not committed. Its
per-frame, per-thread log spans preserve the source `uint64_t` entries exactly, including
single- and multi-entry Events, and the tests compare and decode those raw records.
They also exercise both native session policies: normal opens reject malformed FlatBuffers
table graphs, trusted opens skip that verification, and iterator configurations remain
verified in native builds.

`flatbuffer_generator_tests` checks the strict block-comment `@control_command` grammar,
duplicate and unknown-type rejection, struct/table registry generation, and that the
checked-in generated command registry matches the schema.

`cross_language_parity` builds that same capture and compares a complete canonical dump
model from C++, Luau, JavaScript, C#, Python, and Go: every descriptor and sample family
exposed by the APIs, complete frame/thread-log metadata, name-based find/filter results,
counter traversal, and every public log-iterator state report. C++ and Luau are the required
baseline; other readers participate when their artifacts and runtimes are available.
The runner independently validates section counts and requires coverage of scopes, Events,
frame boundaries, and thread-end reports so the readers cannot accidentally agree
on an incomplete subset. It also checks wrapper-cache identity/value behavior. Malformed
input rejection is covered by the native tests and fuzzers rather than cross-language
parity. CTest reports parity as skipped (exit 77) if `lute` or the generated `LibMP.luau` is
unavailable; complete validation should build the Luau artifact first rather than accepting
a skip.

For a self-preparing parity-only run from the repository root:

```sh
python3 src/scripts/test_parity.py
```

This verifies `lute`, incrementally builds all five language libraries and the native parity
tool through the existing build runners, reconstructs a GPRX capture from the committed
source-data fixture, requires every reader, and runs only `cross_language_parity`. To compare
the readers on another capture:

```sh
python3 src/scripts/test_parity.py --input path/to/capture.gprx
python3 src/scripts/test_parity.py --input 'path/to/captures/*.gprx'
```

`--input` also accepts multiple paths, may be repeated, and accepts a directory containing
`.gprx` files.

To replace the standard Luau reader rather than run an additional reader, pass either its
single-file artifact or the directory containing it. The default Luau build is skipped:

```sh
python3 src/scripts/test_parity.py --luau-artifact src/build/Luau-AiGen
```

## Sanitizers and fuzz harnesses

```sh
python3 src/tests/fuzz/build_fuzz.py --clean --smoke
python3 src/tests/fuzz/build_fuzz.py --no-assert --clean --smoke
```

These commands compile every harness with ASan+UBSan and execute each reproducibly
generated seed once, with assertions enabled and disabled. `make_seed_corpus` generates
valid control commands, numeric edge cases, valid/invalid codec streams, truncated inputs,
and the real 16-frame capture from the current source revision.

The no-assertion configuration defines `NDEBUG`, disabling `XASSERT_DIAG`, FlatBuffers'
mapped diagnostic assertions, and the bundled C dependencies' standard assertions.
`XASSERT_TRAP` remains active in both configurations.

In an assertion-armed build, focused tests may temporarily install
`GPRX::detail::DiagnosticAssertionHandler`. A handler should return `Continue` only for the
exact expected diagnostic and only when that call site immediately follows the assertion
with a safe fallback. Returning `UseDefault` preserves the normal assert/trap behavior.
The handler is process-wide, so replacement must be serialized and the previous handler
restored. It does not intercept `XASSERT_TRAP`.

The assertion-enabled reader fuzzer similarly continues only past an exact allowlist of
malformed-input diagnostics. A newly reached or internal diagnostic retains the default
behavior and therefore remains visible as a fuzzing failure.

Continuous fuzzing is a separate CI/operations task. For example:

```sh
python3 src/tests/fuzz/build_fuzz.py --run 60
```

Retain and minimize any reproducer, add it to a deterministic C++ regression test, and
run both assertion configurations. The four harnesses cover the control/service buffer
boundary, `GetDataInRange` numeric conversion, complete GPRX/C-API traversal, and codecs.

## Recommended test matrix

- GCC and Clang Release builds with warnings-as-errors.
- Debug and `NDEBUG` behavior.
- ASan+UBSan seed smoke tests in both assertion modes.
- Canonical and amalgamated source builds.
- Valid and malformed-input parity across every supported language implementation.

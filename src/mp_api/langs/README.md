# Single-file wrappers

The JavaScript, C#, Python, and Go wrappers each consist of one generated source
file. That file embeds LibMP's WebAssembly module, the language's FlatBuffers
runtime, all generated `MpTypes`, and the wrapper API. The wrappers implement the
API described in the [API reference](../../../docs/api-reference.md), using each
language's own call syntax.

Build a wrapper from the repository root:

| Language | Build command | Output | Runtime dependency |
|----------|---------------|--------|--------------------|
| [JavaScript](js/README.md) | `python3 src/scripts/build.py --js` | `src/build/js/LibMP.js` | None |
| [C#](csharp/README.md) | `python3 src/scripts/build.py --csharp` | `src/build/csharp/LibMP.cs` | Wasmtime (NuGet) |
| [Python](python/README.md) | `python3 src/scripts/build.py --python` | `src/build/python/LibMP.py` | `wasmtime` (PyPI) |
| [Go](go/README.md) | `python3 src/scripts/build.py --go` | `src/build/go/LibMP.go` | `wasmtime-go` |

Prebuilt wrappers are also available from
[LibMP releases](https://github.com/Roblox/libmp/releases). Each language's
README covers setup, a minimal program, and language-specific behavior.
[examples](../../../examples) contains a runnable script for each language.

## Opening captures

See [Session constructors](../../../docs/api-reference.md#constructors).
`OpenFromFile` reads a capture file. `OpenFromMem` copies the complete capture
into WASM memory. `OpenFromBuffer` keeps the supplied buffer and reads from it
on demand, so do not modify the buffer while the session is open.
`OpenFromExternalProvider` reads through a custom provider; each language's
README describes the provider interface. A session that cannot be opened is
returned as `null` (`None` in Python, `nil` in Go).

`OpenFromSlot` and the `Experimental.V1` streaming APIs are present for API
parity, but return `null` or `false`, because the standalone wrappers have no
Roblox engine slot transport.

## Converting captures

`Tools.ConvertToBuffer` returns the converted capture in memory, and
`Tools.ConvertToFile` writes it to a file. `Tools.SetHtmlAssetsBaseUrl` is
supported for HTML conversion. See [Tools](../../../docs/api-reference.md#tools)
for the container formats.

## Reading data

See [Fetch vs Get](../../../docs/api-reference.md#fetch-vs-get). `Fetch*`
methods return independent, language-native objects. `Get*` methods return
zero-copy FlatBuffers views into WASM memory; each wrapper keeps those views
valid when WASM memory grows, but the lifetime rules in the API reference still
apply.

Session, iterator, and array methods use the API reference's names in every
language. Field and accessor names follow each language's generated FlatBuffers
code:

| Language | `Fetch*` object field | `Get*` view and iterator-state accessor |
|----------|-----------------------|------------------------------------------|
| JavaScript | `timer.timerName` | `state.timerId()` |
| C# | `timer.TimerName` | `state.TimerId` |
| Python | `timer.timerName` | `state.TimerId()` |
| Go | `timer.TimerName` | `state.TimerId()` |

## Releasing objects

See [Memory Management](../../../docs/api-reference.md#memory-management).
Dispose sessions, iterators, and arrays returned by `Get*` methods. An iterator
may outlive its session: disposing the session detaches the iterator, which then
stops stepping but must still be disposed. In C#, Python, and Go, dispose the
top-level LibMP instance last. C# `using` declarations, Python `with` blocks,
and Go `defer` statements release objects in reverse order of creation, which
satisfies this order. Do not dispose the top-level instance from one of its own
callbacks; C#, Python, and Go reject that operation while the WASM store is
executing the callback.

## Trusted input

The wrappers use LibMP's trusted-input reader. WebAssembly isolates its linear
memory from the host, but malformed input may trap that LibMP instance; use
captures from a trusted source.

## Testing

Each wrapper has a smoke test:

```text
python3 src/scripts/test_luau.py
python3 src/scripts/test_js.py
python3 src/scripts/test_csharp.py
python3 src/scripts/test_python.py
python3 src/scripts/test_go.py
```

By default, a smoke test builds its wrapper and generates fixture captures with
the native `parity_tool`, which requires CMake and a C++17 compiler; see
[BUILDING.txt](../../scripts/BUILDING.txt). Pass `--input capture.x.html` to test
an existing capture instead (`.gprx` is also accepted), and `--skip-build` to
test the existing wrapper artifact.

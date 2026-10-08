# Python wrapper

`LibMP.py` is a single-file module for Python 3.10+. Build it with
`python3 src/scripts/build.py --python` or take it from
[LibMP releases](https://github.com/Roblox/libmp/releases). See
[Single-file wrappers](../README.md) for behavior shared by all wrappers, the
[API reference](../../../../docs/api-reference.md) for the complete API, and
[examples/python](../../../../examples/python) for a complete script.

Place `LibMP.py` on the import path, for example beside your script, and
install its only runtime dependency:

```text
python -m pip install "wasmtime>=20.0.0,<50.0.0"
```

If Wasmtime is not installed, importing `LibMP` reports the exact `pip`
command.

```python
import LibMP

with LibMP.create() as libmp:
    with libmp.Session.OpenFromFile("capture.x.html") as session:  # or "capture.gprx"
        print(session.FetchGlobalDesc().tickToMsCpu)
```

Python-specific behavior:

- Custom providers passed to `OpenFromExternalProvider` implement `get_size()`
  and `read(destination, block_id, offset, size)`, and optionally `open()` and
  `close()`; `read` returns the number of bytes copied.
- `Tools.ConvertToBuffer` returns `bytes`; `Tools.ConvertToFile` streams
  directly to a file.
- The top-level instance, sessions, arrays, and iterators are context managers.
  Use `with` blocks or call `Dispose()` explicitly. Sessions, arrays, and
  iterators that are garbage-collected without `Dispose()` are released at the
  start of the next call on the same instance.

## Wasmtime call path

Tested Wasmtime releases use a signature-checked native call path for LibMP's
integer-only exports. For other releases, or if that internal capability is
unavailable, the wrapper automatically falls back to Wasmtime's slower public
function-call API.

The upper version bound is the newest tested release, not a hard limit; newer
Wasmtime releases should work through that fallback. Advanced users can opt in
to the native call path on a newer release by raising
`WASMTIME_FAST_MAX_MAJOR_EXCLUSIVE` near the top of `LibMP.py` (or
`python_fast_max_major_exclusive` in `src/scripts/build-config.json` before
building). The wrapper still checks the native call signature before using it,
but such combinations are untested.

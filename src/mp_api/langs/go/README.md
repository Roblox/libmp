# Go wrapper

`LibMP.go` is a single-file Go package for Go 1.18+. Build it with
`python3 src/scripts/build.py --go` or take it from
[LibMP releases](https://github.com/Roblox/libmp/releases). See
[Single-file wrappers](../README.md) for behavior shared by all wrappers, the
[API reference](../../../../docs/api-reference.md) for the complete API, and
[examples/go](../../../../examples/go) for a complete program.

`LibMP.go` declares `package libmp`. Place it in a `libmp` subdirectory of your
module, import it as `<module>/libmp`, and run `go mod tidy` to add its only
dependency, `github.com/bytecodealliance/wasmtime-go/v39`:

```text
my-app/
  go.mod          module my-app
  main.go         import libmp "my-app/libmp"
  libmp/LibMP.go
```

wasmtime-go uses cgo, so a C compiler must be available; on Windows, use a
64-bit MinGW-w64 GCC. Go runs `gcc` by default; set `CC` to use a compiler with
another name. For example, Cygwin's own `gcc` does not target native Windows,
so install Cygwin's `mingw64-x86_64-gcc-core` package, add Cygwin's `bin`
directory to `PATH`, and set `CC=x86_64-w64-mingw32-gcc`.

```go
library, err := libmp.New()
if err != nil { /* handle error */ }
defer library.Dispose()

session, err := library.OpenFromFile("capture.x.html") // or "capture.gprx"
if err != nil || session == nil { /* handle error */ }
defer session.Dispose()
fmt.Println(session.FetchGlobalDesc().TickToMsCpu)
```

Go-specific behavior:

- `libmp.New()` returns an `error` on failure. `OpenFromFile` returns an
  `error` if the file cannot be read, and a `nil` session if the capture cannot
  be opened; other `OpenFrom*` methods return only the session.
- Custom providers implement `DataProvider`; the optional
  `OpenableDataProvider` and `CloseableDataProvider` interfaces add `Open` and
  `Close` hooks.
- `Tools.ConvertToBuffer` returns a byte slice; `Tools.ConvertToFile` streams
  directly to a file. Both also return an `error`.
- Finalizers release unreachable sessions, iterators, and arrays during the
  next LibMP call on the same instance. The instance itself is released only
  by its `Dispose()`. Call `Dispose()`, usually with `defer`, to release
  objects promptly and in order.

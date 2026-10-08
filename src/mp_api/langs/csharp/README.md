# C# wrapper

`LibMP.cs` is a single-source library for .NET 8. Build it with
`python3 src/scripts/build.py --csharp` or take it from
[LibMP releases](https://github.com/Roblox/libmp/releases). See
[Single-file wrappers](../README.md) for behavior shared by all wrappers, the
[API reference](../../../../docs/api-reference.md) for the complete API, and
[examples/csharp](../../../../examples/csharp) for a complete program and
project file.

Add `LibMP.cs` to your project, enable unsafe code, and reference Wasmtime:

```xml
<PropertyGroup>
  <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
</PropertyGroup>
<ItemGroup>
  <PackageReference Include="Wasmtime" Version="44.0.0" />
</ItemGroup>
```

```csharp
using LibMP;

using var libmp = LibMPInstance.Create();
using var session = libmp.OpenFromFile("capture.x.html"); // or "capture.gprx"
Console.WriteLine(session?.FetchGlobalDesc()?.TickToMsCpu);
```

C#-specific behavior:

- `LibMPInstance.Create()` creates the top-level instance; sessions open
  through its `OpenFrom*` methods.
- Custom providers implement `IDataProvider`: `Open()`, `Close()`,
  `GetSize()`, and `Read(destination, blockId, offset, size)`. Capture sizes
  and offsets are 64-bit (`ulong`).
- `Tools.ConvertToBuffer` returns a `byte[]`; `Tools.ConvertToFile` streams
  directly to a file.
- Sessions, arrays, iterators, and `LibMPInstance` implement `IDisposable`.
  Finalizers release sessions, arrays, and iterators that were not disposed
  when a later call on the same instance returns. Dispose objects to release
  them promptly and in order.

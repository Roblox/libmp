# LibMP

LibMP is a Luau-first library for programmatic access to Roblox MicroProfiler data. Use it to profile CPU/GPU performance, analyze frame times, inspect counters, iterate scope timelines, and create snapshots from scripts running in Roblox clients and servers or in command-line Luau. LibMP is also available for JavaScript, C#, Python, Go, and C++.

**Download:** [LibMP releases](https://github.com/Roblox/libmp/releases) | [Lute](https://github.com/luau-lang/lute/releases) (for CLI/offline analysis)  
**Play:** [Demo Game](https://www.roblox.com/games/70526018540413/Profile-Me) | **Discuss:** [DevForum](https://devforum.roblox.com/t/studio-beta-introducing-libmp-the-microprofiler-api/4703136)

[API Reference](#go-api) • [AI Skill](#go-api) • [Building](#go-build)

## Quick Start

In Roblox Studio, add the `LibMP` ModuleScript into `ReplicatedStorage`. You can do this by getting it from the [Creator Store](https://create.roblox.com/store/asset/109009378620272/LibMP) or by manually pasting the [Luau Script](https://github.com/Roblox/libmp/releases).  
Add a new LocalScript under `StarterPlayer/StarterPlayerScripts`, then paste the following code into it:

```luau
local LibMP = require("@game/ReplicatedStorage/LibMP")

LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local session = LibMP.Session.OpenFromLiveData()
local counterId = session:FindCounterId("**/Luau/heap")

local sample = session:FetchLastCounterSample(counterId)
print(string.format("Luau heap: %.2f MB", sample.Value / (1024 * 1024)))
session = session:Dispose()
```

It will print the Luau heap size counter value to the Output window when switching to Play mode.  
The same script works in Lute - just use `require("./LibMP")`. See [Examples](#go-examples).

## Core Concepts

### Sessions

A `Session` is the main object you work with. Create one from live data, a snapshot buffer, or a dump file:

```luau
local session = LibMP.Session.OpenFromLiveData()          -- Real-time
local session = LibMP.Session.OpenFromBuffer(buf)         -- From snapshot
local session = LibMP.Session.OpenFromFile("dump.x.html") -- From file (Lute only)
```

Check `session ~= nil` and `session:IsValid()` before interacting with it.  
Live data sessions sync automatically with the MicroProfiler. New frames are being added, old frames evicted.  
It's a good idea to use live sessions for lightweight checks and snapshots for detailed analysis spanning many frames.

### Entities

| Entity | Description |
|--------|-------------|
| **Frame** | A unit of the timeline. Contains CPU and GPU timestamps. Up to 256 recent frames are retained. |
| **Timer/Scope** | An instrumented code section. Each enter/exit pair is a scope instance on the timeline. |
| **Group** | A collection of timers (Physics, Scripts, Network, etc.) |
| **Thread** | An OS thread with its own timeline. The GPU "thread" represents GPU work. |
| **Counter** | A numeric value tracked per frame (memory usage, allocations, instance counts). Counters form a hierarchy. |

All entities have numeric IDs (starting from 1; 0 means "not found"). Use `Find***` methods to look up IDs by name mask.

### Iterators

**Log Iterator** – walks thread logs entry by entry, frame by frame. Tracks the scope stack as scopes are entered and exited. Configurable to filter specific threads, timers, or frame ranges. A configured iterator can be reused by calling `RewindTo` with a new frame range.

```luau
local iter = session:CreateLogIterator()
local state = iter:GetState() -- Get once, read on every Step()
iter:Configure({ ThreadIds = {} }) -- Empty = all threads

iter:RewindTo(startFrame, endFrame)
while iter:Step() do
    if state:IsExit() then -- A scope just closed
        print(session:FetchTimerDesc(state:TimerId()).TimerName)
    end
end
iter:Dispose()
```

**Counter Iterator** – traverses the hierarchical counter tree depth-first.

```luau
local cIter = session:CreateCounterIterator()
local state = cIter:GetState()
cIter:Configure({ RootCounterIds = {} }) -- Empty = all counters

while cIter:Step() do
    local name = session:FetchCounterDesc(state:CounterId()).CounterName
    local indent = string.rep("  ", state:RelativeLevel())
    print(indent .. name)
end
cIter:Dispose()
```

### Fetch vs Get

If you see both `Fetch***` and `Get***` variants of a function, they provide two different access patterns for the same underlying data:

- **`Fetch***`** – Returns a native Luau table with direct field access. Simple and safe. **Use this by default.**
- **`Get***`** – Returns a view with getter methods. Faster for single-field access, but **it is invalidated by the next `Get` or `Fetch` call on the same session**. Primarily intended for internal use.

### Memory Management

Sessions, iterators, and arrays from `Get***` methods must be disposed manually when you are done with them:

```luau
iter:Dispose()
session:Dispose()
```

Monitor live allocations with `LibMP.GetMemUsed()` and allocator-reserved memory
with `LibMP.GetMemReserved()`.

## Search Masks

`Find***` methods for timers and threads accept simple `*` masks and can also be set up as case sensitive and insensitive.
`Find***` methods for counters accept glob-like path masks:

| Pattern | Meaning |
|---------|---------|
| `name` | Match anywhere in hierarchy |
| `name$` | Leaf node only |
| `/root/child` | Root-anchored path |
| `*` | Single-level wildcard |
| `**` | Recursive wildcard (any depth) |
| `pattern1\|pattern2` | Multiple patterns |

IDs start from 1 – a value of 0 means the absence of an entity.

```luau
session:FindCounterId("**/Luau/heap", true) -- case-sensitive (true)
session:FindTimerIds("*render*|*physics*", false) -- case-insensitive (false)
session:FindThreadIds("*worker*") -- case-insensitive by default (false by default)
```

## Profiler Control

```luau
LibMP.Control:IsBackendReady()     -- Check whether the MicroProfiler is ready to receive commands and return data
LibMP.Control:SetMemTargetMB(64)   -- Target size, in MB, of the MicroProfiler capture buffers
LibMP.Control:EnableProfiler(true) -- Turn profiler on, make sure it allocated its resources and initialized
LibMP.Control:EnableCapture(true)  -- Start/resume capture (false = pause capture)
LibMP.Control:SetFrameLimit(256)   -- Rolling frame limit (256 max). Lower values produce smaller snapshots.
LibMP.Control:ShowUI(true)         -- Show MP overlay (Studio only)
```

## Captures

```luau
LibMP.Control:SetFrameLimit(64)

local buf1 = LibMP.Control:CaptureToBufferSync() -- Captures the current profiler data into a new Luau buffer
print("raw capture buffer len = " .. buffer.len(buf1))

local buf2 = LibMP.Control:CaptureToBufferSync({
    ContainerFormat = LibMP.Enums.ContainerFormat.Compressed,
}) -- Captures the current data in a compressed form that is better for network transfer
print("compressed capture buffer len = " .. buffer.len(buf2))
```

**Manual Dumping:** From the MicroProfiler top menu, click **Dump** ➔ **Dump in binary format**. This captures the same data exposed in-game, respecting your set frame limit. The result is saved as a `.gprx` file in your `Logs` folder (`%localappdata%\Roblox\Logs` on Windows, `~/Library/Logs/Roblox` on Mac). The format name stands for Game Performance Recording eXchange.

**Dump** ➔ **NN frames** and **Start/Stop recording** produce `.x.html` files (short for `.gprx.html`) that can be opened in a browser or parsed with LibMP.

<a name="go-examples"></a>
## Examples

Examples 1–7 work in both Roblox Engine and Lute (CLI). Examples 8–10 are Engine-only use cases.

| # | Example | Description |
|---|---------|-------------|
| 01 | [01-hello-libmp.luau](examples/luau/01-hello-libmp.luau) | Minimal: open session, read a counter |
| 02 | [02-basic-frame.luau](examples/luau/02-basic-frame.luau) | Read CPU and GPU frame times |
| 03 | [03-basic-counter.luau](examples/luau/03-basic-counter.luau) | All samples, last sample, find by frame, disposal |
| 04 | [04-basic-iteration.luau](examples/luau/04-basic-iteration.luau) | Iterate scopes in one frame/thread |
| 05 | [05-advanced-iteration.luau](examples/luau/05-advanced-iteration.luau) | **Reusable module**: frame-local, full-reconstruct, hybrid iterators |
| 06 | [06-advanced-frame-stats.luau](examples/luau/06-advanced-frame-stats.luau) | Per-timer inclusive times, top 20 averages |
| 07 | [07-advanced-counter.luau](examples/luau/07-advanced-counter.luau) | Traverse the counter hierarchy tree |
| 08 | [08-usecase-snapshot-capturer.luau](examples/luau/08-usecase-snapshot-capturer.luau) | Periodic 16-frame capture, store last 3 |
| 09 | [09-usecase-snapshot-analyzer.luau](examples/luau/09-usecase-snapshot-analyzer.luau) | Incremental analysis (max N steps/frame) |
| 10 | [10-usecase-snapshot-sender.luau](examples/luau/10-usecase-snapshot-sender.luau) | Server-side capture + HTTP POST |

## Extra features

- Counter history is available through `FetchCounterSamples` and `FetchSampleByFrameId`, and in the `.x.html` viewer's **Counters** mode.
- Creating captures is two orders of magnitude faster than with the previous dump path, enabling continuous recording of long captures.

## Limitations

- Only raw profiling data is provided. Averages, min/max, and other derived statistics must be computed manually at this time.
- Exact hardware information is excluded from captures made by game clients. Future versions might include CPU, GPU, and memory performance buckets.

<a name="go-api"></a>
## API Reference

See [docs/api-reference.md](docs/api-reference.md) for the complete API with all methods, types, and configuration options.  
Use [docs/SKILL.md](docs/SKILL.md) for AI-assisted development and automated analysis of captures. Studio MCP and Assistant include this skill under the name `perf-profiling`, and it works across Roblox places without requiring an explicit LibMP import. It contains heavily commented example scripts and can be used as an additional reference for developers.

<a name="go-build"></a>
## Build from source

See [src/scripts/BUILDING.txt](src/scripts/BUILDING.txt).
LibMP can be built as a C++ library or as
[single-file libraries](src/mp_api/langs/README.md) for Luau, JavaScript, C#,
Python, and Go. Each language library embeds and uses the C++ core internally.
Prebuilt libraries are also available from
[LibMP releases](https://github.com/Roblox/libmp/releases). The
[examples](examples) directory contains a runnable script for each language.

## Third-party dependencies

Third-party dependency and licensing information is available under
[`src/dependencies/`](src/dependencies/).

## License

LibMP is licensed under the [MIT License](LICENSE).

# LibMP API Reference

LibMP is a cross-language library for programmatic access to Roblox MicroProfiler data.

Examples use Luau call syntax: instance methods use `:`, while constructors such
as `Session.OpenFrom***` and direct `LibMP.***` functions use `.`. Other wrappers
use their language's ordinary method-call syntax.

Some functions have both `Fetch***` and `Get***` variants. See the **"Fetch vs Get"** section below for details. **TL;DR:** prefer `Fetch***`.

---

## LibMP (Top-Level)

| Member | Type | Description |
|--------|------|-------------|
| `LibMP.IsCliMode` | `boolean` | `true` if running in a CLI environment (Lute) |
| `LibMP.Versions` | `Versions` | Version info. Contains `Versions.Library` and `Versions.DataFormat` |
| `LibMP.Enums` | `table` | Public enum constants, including `ContainerFormat`, `ContentMode`, `AppType`, and `DataSlot` |
| `LibMP.GetMemUsed()` | `number` | Current memory usage of the WASM instance |
| `LibMP.GetMemReserved()` | `number` | Memory currently reserved by the WASM allocator |

---

## Control

Global profiler control. Access via `LibMP.Control`.

| Method | Returns | Description |
|--------|---------|-------------|
| `:IsBackendReady()` | `boolean` | API accessible and version-compatible |
| `:IsBackendAccessible()` | `boolean` | Remote API is reachable |
| `:IsBackendVersionCompatible()` | `boolean` | Version check only |
| `:SetMemTargetMB(targetMB)` | `boolean` | Request the engine MicroProfiler memory target in megabytes. Useful for controlling memory overhead. |
| `:EnableProfiler(enable)` | `boolean` | Toggle the profiler on/off |
| `:EnableCapture(enable)` | `boolean` | Activate/pause data capture |
| `:SetFrameLimit(num)` | `boolean` | Set the rolling frame limit (max 256) |
| `:ShowUI(show)` | `boolean` | Toggle on-screen UI (Studio only) |
| `:CaptureToBufferSync([captureParams])` | `buffer` | Snapshot live data into a buffer |
| `:CaptureToSlotSync(targetSlotId, [captureParams])` | `boolean` | Snapshot live data into a user-managed slot (1–4). In Studio, these slots persist when switching between Play and Edit modes. |
| `:SaveSlotToBuffer(slotId)` | `buffer` | Copy slot contents into a Luau buffer |
| `:ResetSlot(slotId)` | `boolean` | Clear a stored slot |
| `:UseControlChannel(use)` | `boolean` | Enable/disable control channel |

`captureParams` accepts optional `MaxFrameCount` and `ContainerFormat` fields.
`MaxFrameCount` is reserved for capture-window selection, which is not
implemented by the game-engine backend; `0` selects the current live window.
`ContainerFormat` defaults to `LibMP.Enums.ContainerFormat.Raw`.

---

## Tools

Container conversion is available through `LibMP.Tools`.

| Method | Returns | Description |
|--------|---------|-------------|
| `:ConvertToBuffer(session, format)` | `buffer?` | Converts a session and returns the complete container |
| `:ConvertToFile(session, format, outputName)` | `boolean` | Converts a session and streams the result to a file |
| `:SetHtmlAssetsBaseUrl(baseUrl)` | `boolean` | Overrides the base URL used by `.x.html` viewer-script references; an empty string restores the default |

Use a value from `LibMP.Enums.ContainerFormat`:

| Value | Output |
|-------|--------|
| `Raw` | Raw headers and payloads, with a raw global index |
| `Compressed` | Raw headers and LZ4 payloads, with an LZ4 global index |
| `CompressedAscii` | Hex headers and LZ4+Base64 payloads with an encoded global index |
| `CompressedHtml` | `.x.html`: the ASCII encoding plus viewer-script includes |

`ConvertToFile` is available in command-line Luau (Lute). JavaScript supports
Node.js files and browser downloads; its method is asynchronous. Native language
wrappers use their customary return/error conventions for these same methods.

---

## Experimental.V1

Experimental profiler controls. These APIs may change without compatibility guarantees.

| Method | Returns | Description |
|--------|---------|-------------|
| `.StartFileStream([filenamePostfix[, targetSlotId]])` | `boolean` | Queue a Studio file streaming capture for the next frame flip; the optional postfix is at most 16 ASCII letters or digits, and an optional writable slot receives the finalized file without copying it into memory |
| `.StartMemoryStream(targetSlotId)` | `boolean` | Queue a compressed streaming capture into a memory slot; the slot becomes readable only after finalization |
| `.StopStream()` | `boolean` | Queue the current streaming capture to stop and finalize on the next frame flip |
| `.SetStreamMarker(marker, enabled)` | `boolean` | Enable or disable stream marker 1-5 |
| `.GetStreamStatus()` | `ExperimentalStreamStatus` | Return status for a stream initiated through this API, or an all-zero status when none is active |
| `.SetContentMode(contentMode)` | `boolean` | Select `Enums.ContentMode.Full` (default) or `CountersOnly`. Changing the mode resets the current live-data session. File and memory streams latch this mode when their start request is accepted. |

`ExperimentalStreamStatus` contains `Active`, `StreamFrameId`, `AbsoluteFrameId`,
`MarkerMask`, `RecordedSizeKb`, `MaxSizeKb`, and `ElapsedMs`. Sizes are in KiB;
`MaxSizeKb == 0` means unlimited.

---

## Session

Represents a MicroProfiler session with internal caching.

### Constructors

| Method | Returns | Description |
|--------|---------|-------------|
| `Session.OpenFromLiveData()` | `Session?` | Opens from slot 0 (Engine) or CLI file argument |
| `Session.OpenFromBuffer(b: buffer)` | `Session?` | Opens from a raw Luau buffer |
| `Session.OpenFromMem(data: string \| buffer)` | `Session?` | Opens from a string or buffer (copies internally) |
| `Session.OpenFromSlot(id: number, [cacheSize])` | `Session?` | Opens from a specific slot. User-managed slots 1–4 can store captures. |
| `Session.OpenFromFile(path: string, [cacheSize])` | `Session?` | Opens from a `.x.html` or `.gprx` capture file |
| `Session.OpenFromExternalProvider(io: table, [cacheSize])` | `Session?` | Opens with a custom IO handler |

### Lifecycle & State

| Method | Returns | Description |
|--------|---------|-------------|
| `:Dispose()` | `nil` | Cleans up the session. **Must be called when done.** |
| `:IsValid()` | `boolean` | Whether the session is usable |
| `:GetDataFormatVersion()` | `number` | Data format version of this session |
| `:GetObjSize()` | `number` | Size of the last accessed object |
| `:WithDataLock(callback)` | `(boolean, any)` | Locks the first `Get***` view inside the callback so it remains valid across subsequent calls |

Data locks may be nested after each outer lock has claimed its view. The supported order is
`push -> Get A -> push -> Get B -> pop -> pop`, with LIFO release. This behavior is also
supported when object caching is enabled.

### Synchronization & Caching

| Method | Returns | Description |
|--------|---------|-------------|
| `:SyncWithDataSource()` | `boolean` | Manually sync with the data source |
| `:SetSyncRequired()` | `nil` | Marks session as needing sync on next access |
| `:SetClearCacheOnSyncRequired()` | `nil` | Flags cache to clear on next sync |
| `:SyncIfRequired()` | `nil` | Syncs if previously marked required |
| `:EnableObjectCache(enabled)` | `nil` | Toggle descriptor caching (default: on) |
| `:ClearObjectCache()` | `nil` | Force-clear the cache |
| `:EnableLabelCache(enabled)` | `nil` | Toggle the label-string cache |
| `:ClearLabelCache()` | `nil` | Clear the label-string cache |

### Tracks & Segments

| Method | Returns | Description |
|--------|---------|-------------|
| `:GetTrackCount()` | `number` | Number of independent recordings in the container |
| `:GetTracks()` | `StructArray<TrackDesc>` | Track descriptions; must be disposed |
| `:FetchTracks()` | `Array of TrackDesc` | Owned track descriptions |
| `:GetSegmentCount(trackIndex)` | `number` | Number of segments in a track |
| `:GetSegments(trackIndex)` | `StructArray<SegmentDesc>` | Segment descriptions; must be disposed |
| `:FetchSegments(trackIndex)` | `Array of SegmentDesc` | Owned segment descriptions |
| `:SelectTrack(trackIndex)` | `boolean` | Select a track and its first segment, if present |
| `:SelectSegment(segmentIndex)` | `boolean` | Select a segment in the active track |

### Global & General Info

| Method | Returns |
|--------|---------|
| `:GetGlobalDesc()` | `View (GlobalDesc)` |
| `:FetchGlobalDesc()` | `Luau table (GlobalDesc)` |
| `:GetGeneralInfo()` | `View (GeneralInfo)` |
| `:FetchGeneralInfo()` | `Luau table (GeneralInfo)` |

### Groups

| Method | Returns |
|--------|---------|
| `:GetGroupIdMax()` | `number` |
| `:GetGroupDesc(groupId)` | `View (GroupDesc)` |
| `:FetchGroupDesc(groupId)` | `Luau table (GroupDesc)` |

### Timers

| Method | Returns |
|--------|---------|
| `:GetTimerIds()` | `ScalarArray<number>` |
| `:FetchTimerIds()` | `Array of numbers` |
| `:GetTimerDesc(timerId)` | `View (TimerDesc)` |
| `:FetchTimerDesc(timerId)` | `Luau table (TimerDesc)` |

### Counters

| Method | Returns |
|--------|---------|
| `:GetCounterIds()` | `ScalarArray<number>` |
| `:FetchCounterIds()` | `Array of numbers` |
| `:GetCounterDesc(counterId)` | `View (CounterDesc)` |
| `:FetchCounterDesc(counterId)` | `Luau table (CounterDesc)` |

### Threads

| Method | Returns |
|--------|---------|
| `:GetThreadIds()` | `ScalarArray<number>` |
| `:FetchThreadIds()` | `Array of numbers` |
| `:GetThreadIdGpu()` | `number` |
| `:GetThreadDesc(threadId)` | `View (ThreadDesc)` |
| `:FetchThreadDesc(threadId)` | `Luau table (ThreadDesc)` |

### Frames

| Method | Returns |
|--------|---------|
| `:GetFrameIdMin()` | `number` |
| `:GetFrameIdMax()` | `number` |
| `:GetFrameDesc(frameId)` | `View (FrameDesc)` |
| `:FetchFrameDesc(frameId)` | `Luau table (FrameDesc)` |

### Counter Samples

| Method | Returns | Description |
|--------|---------|-------------|
| `:GetCounterSamples(counterId)` | `CounterSampleArray` | **Must be Disposed!** |
| `:FetchCounterSamples(counterId)` | `Array of Luau tables` | Deep copy, no disposal needed |
| `:GetLastCounterSample(counterId)` | `View (CounterSample)` | Last sample only (cheap) |
| `:FetchLastCounterSample(counterId)` | `Luau table (CounterSample)` | Last sample as native table |

### Other Samples

| Method | Returns |
|--------|---------|
| `:GetPlaceIdSamples()` | `StructArray<U64Sample>` |
| `:FetchPlaceIdSamples()` | `Array of Luau tables (U64Sample)` |
| `:GetUtcTimestampSamples()` | `StructArray<U64Sample>` |
| `:FetchUtcTimestampSamples()` | `Array of Luau tables (U64Sample)` |

### Search & Filtering

All `Find*` methods accept a `nameMask` string and an optional `caseSensitive` boolean (default: `false`).

Methods for timers and threads accept simple `*` masks. Methods for counters accept glob-like path masks.

**Mask syntax:**
- `name` — matches anywhere in the hierarchy
- `name$` — leaf node only
- `/root/child` — root-anchored path
- `*` — single-level wildcard
- `**` — recursive wildcard (any depth)
- `pattern1|pattern2` — multiple patterns

IDs start from 1 – a value of 0 means the absence of an entity.

| Method | Returns | Description |
|--------|---------|-------------|
| `:FindGroupId(mask, [caseSensitive])` | `number` | First matching group (0 = not found) |
| `:FindGroupIds(mask, [caseSensitive])` | `Array of numbers` | All matching groups |
| `:FindTimerId(mask, [caseSensitive])` | `number` | First matching timer |
| `:FindTimerIds(mask, [caseSensitive])` | `Array of numbers` | All matching timers |
| `:FindCounterId(mask, [caseSensitive])` | `number` | First matching counter |
| `:FindCounterIds(mask, [caseSensitive])` | `Array of numbers` | All matching counters |
| `:FindThreadId(mask, [caseSensitive])` | `number` | First matching thread |
| `:FindThreadIds(mask, [caseSensitive])` | `Array of numbers` | All matching threads |
| `:GetTimerIdsByGroupId(groupId)` | `ScalarArray<number>` | Timers in a group |
| `:FetchTimerIdsByGroupId(groupId)` | `Array of numbers` | Timers in a group (native) |
| `:GetTimerIdsByGroupNames(mask, [caseSensitive])` | `ScalarArray<number>` | Timers by group name mask |
| `:FetchTimerIdsByGroupNames(mask, [caseSensitive])` | `Array of numbers` | Timers by group name mask (native) |

### Iterators

| Method | Returns |
|--------|---------|
| `:CreateLogIterator()` | `LogIterator` |
| `:CreateCounterIterator()` | `CounterIterator` |

---

## LogIterator

Iterates over thread log entries (scope enters/exits) frame by frame.

| Method | Returns | Description |
|--------|---------|-------------|
| `:Configure(config)` | `nil` | Apply a `LogIteratorConfig` table |
| `:Step()` | `boolean` | Advance one entry. `false` = finished |
| `:Rewind()` | `nil` | Reset to configured start state |
| `:RewindTo(frameIdMin, frameIdMax)` | `nil` | Rewind to a specific frame range |
| `:GetState()` | `LogIteratorState view` | Persistent state object (get once, read many) |
| `:GetThreadStackState(threadId)` | `LogIteratorThreadStackState view` | Stack state for any thread |
| `:GetThreadStackElement(threadId, level)` | `LogIteratorThreadStackElement view` | Stack element (0-based level) |
| `:GetCurrentThreadStackElement(level)` | `LogIteratorThreadStackElement view` | Current thread's stack element |
| `:GetCurrentEntryCount()` | `number` | Number of physical 64-bit entries in the current logical step |
| `:GetCurrentEntryPart(entryIndex, partIndex)` | `number` | Low (`0`) or high (`1`) 32-bit half of an entry, indexed from 0 |
| `:FetchCurrentLabel()` | `string?` | Text for the current label entry, or `nil` when unavailable |
| `:GetCurrentScopeLabelCount()` | `number` | Number of tracked labels attached to the current scope |
| `:FetchCurrentScopeLabel(index)` | `string?` | Text of a tracked current-scope label, indexed from 0 |
| `:GetScopeLabelCount(threadId, level)` | `number` | Tracked labels attached to a stack element |
| `:FetchScopeLabel(threadId, level, index)` | `string?` | Text of a tracked scope label |
| `:Dispose()` | `nil` | Releases the iterator. **Must be called when done.** |

`FetchCurrentLabel` is intended for a step where `LogIteratorState:IsLabel()` is
true. To associate labels with their containing scopes, enable
`TrackScopeLabels`. The four scope-label count/fetch methods require that option;
on an exit step, "current scope" means the scope that just exited. Label and
stack-level indices are zero-based, and a fetch returns `nil` when text is
unavailable.

### LogIteratorConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `StartFrameId` | `number` | `0` | Start frame (0 = first available) |
| `EndFrameId` | `number` | `0` | End frame (0 = last available) |
| `ThreadIds` | `{number}` | all | Filter to specific threads |
| `GroupIds` | `{number}` | all | Filter to specific groups |
| `TimerIds` | `{number}` | all | Filter to specific timers |
| `SkipTimerIds` | `{number}` | none | Exclude specific timers |
| `SkipGpuThreads` | `boolean` | `false` | Skip GPU threads |
| `SkipEvents` | `boolean` | `false` | Skip event entries |
| `SkipPausedFrames` | `boolean` | `false` | Skip frames marked as paused |
| `SkipFrameBoundaries` | `boolean` | `false` | Suppress frame boundary notifications |
| `SkipTimestampNormalization` | `boolean` | `false` | Keep raw timestamps |
| `SkipThreadStackResume` | `boolean` | `false` | Don't resume stack across frames |
| `SkipThreadStackDepthSync` | `boolean` | `false` | Don't reset the min absolute depth on a new frame |
| `EmitThreadFrameEnd` | `boolean` | `false` | Emit notification when a thread's log ends in a frame |
| `TrackScopeLabels` | `boolean` | `false` | Retain up to 16 labels per active scope |

### LogIteratorState

Obtained once via `iter:GetState()`, then read on each `Step()`.

| Getter | Type | Description |
|--------|------|-------------|
| `:Started()` | `boolean` | Iterator has begun |
| `:Finished()` | `boolean` | Iterator is exhausted |
| `:FrameId()` | `number` | Current frame ID |
| `:FrameAbsoluteId()` | `number` | Absolute frame ID (detects gaps) |
| `:ThreadId()` | `number` | Current thread ID |
| `:IsGpuThread()` | `boolean` | Whether current thread is GPU |
| `:TimerId()` | `number` | Timer ID of current entry |
| `:Timestamp()` | `number` | Timestamp of current entry |
| `:TimestampGpu()` | `number` | GPU timestamp (on frame boundaries) |
| `:FrameChanged()` | `boolean` | Frame just changed |
| `:ThreadChanged()` | `boolean` | Thread just changed |
| `:IsEnter()` | `boolean` | Current entry is a scope enter |
| `:IsExit()` | `boolean` | Current entry is a scope exit |
| `:IsLabel()` | `boolean` | Current entry is a label |
| `:IsEvent()` | `boolean` | Current entry is an event |
| `:IsUnknown()` | `boolean` | Current entry is of unknown type |
| `:IsFrameStitching()` | `boolean` | Stitch detected (capture gap) |
| `:IsFrameBoundary()` | `boolean` | Frame boundary notification |
| `:IsThreadFrameEnd()` | `boolean` | Thread-frame-end notification |
| `:ThreadStackDepth()` | `number` | Current thread stack depth |
| `:ThreadStackIsUnderflowed()` | `boolean` | Stack underflowed (exit without matching enter) |
| `:ThreadStackIsOverflowed()` | `boolean` | Stack currently overflowed |
| `:ThreadStackWasOverflowed()` | `boolean` | Stack was overflowed in the previous step |
| `:ThreadPrevTimestamp()` | `number` | Previous entry's timestamp |
| `:EntryLocator()` | `number` | Internal entry locator |
| `:EntryTypeRaw()` | `number` | Raw entry type value |
| `:EventType()` | `number` | Event type (when entry is an event) |

### LogIteratorThreadStackState

| Getter | Type | Description |
|--------|------|-------------|
| `:Depth()` | `number` | Current local stack depth |
| `:DepthAbsolute()` | `number` | Absolute depth (tracks across frames) |
| `:DepthAbsoluteMin()` | `number` | Minimum absolute depth seen |
| `:IsUnderflowed()` | `boolean` | Stack is underflowed |
| `:IsOverflowed()` | `boolean` | Stack is overflowed |
| `:WasOverflowed()` | `boolean` | Was overflowed in the previous step |

### LogIteratorThreadStackElement

| Getter | Type | Description |
|--------|------|-------------|
| `:EnterFrameId()` | `number` | Frame where this scope began |
| `:EnterTimerId()` | `number` | Timer ID of this scope |
| `:EnterTimestamp()` | `number` | Original enter timestamp |
| `:EnterTimestampFrameLocal()` | `number` | Enter timestamp clamped to current frame start |
| `:EnterEntryLocator()` | `number` | Internal locator |

---

## CounterIterator

Traverses the hierarchical counter tree.

| Method | Returns | Description |
|--------|---------|-------------|
| `:Configure(config)` | `nil` | Apply a `CounterIteratorConfig` table |
| `:Step()` | `boolean` | Advance to next counter. `false` = finished |
| `:Rewind()` | `nil` | Reset the iterator |
| `:GetState()` | `CounterIteratorState view` | Persistent state object |
| `:GetStackElement(level)` | `number` | Counter ID at stack level (0-based) |
| `:Dispose()` | `nil` | Clean up resources |

### CounterIteratorConfig

| Field | Type | Description |
|-------|------|-------------|
| `RootCounterIds` | `{number}` | Root counters to iterate from (empty = all) |

### CounterIteratorState

| Getter | Type | Description |
|--------|------|-------------|
| `:Started()` | `boolean` | Iterator has begun |
| `:Finished()` | `boolean` | Iterator is exhausted |
| `:CounterId()` | `number` | Current counter ID |
| `:ParentId()` | `number` | Parent counter ID |
| `:Level()` | `number` | Absolute depth in the tree |
| `:RelativeLevel()` | `number` | Depth relative to specified root |
| `:RootLevel()` | `number` | Level of the root counter |
| `:IndexInSiblings()` | `number` | Position among siblings |
| `:IsLeaf()` | `boolean` | Whether this is a leaf node |
| `:HierarchyChanged()` | `boolean` | Hierarchy context changed since last step |
| `:CounterStackIsOverflowed()` | `boolean` | Stack overflow condition |

---

## Array Classes

Returned by `Get***` list methods. **Must be Disposed.**

### Common Methods (all array types)

| Method | Returns | Description |
|--------|---------|-------------|
| `:Size()` | `number` | Element count |
| `:Get(index)` | `View \| number` | Element at 0-based index |
| `:Fetch(index)` | `Owned object \| number` | Deep-copied element |
| `:FetchAll()` | `Array` | All elements in a language-native collection |
| `:Dispose()` | `nil` | **Release resources** |

### Array Types

| Type | Element | Use Case |
|------|---------|----------|
| `ScalarArray<T>` | `number` | ID lists |
| `StructArray<T>` | Struct view | U64Sample arrays |
| `CounterSampleArray` | `CounterSample` view | Counter sample data |

### CounterSampleArray (additional methods)

| Method | Returns |
|--------|---------|
| `:GetSampleByFrameId(frameId)` | `View (CounterSample)` |
| `:FetchSampleByFrameId(frameId)` | `Luau table (CounterSample)` |
| `:GetLastSample()` | `View (CounterSample)` |
| `:FetchLastSample()` | `Luau table (CounterSample)` |

---

## Data Types

Owned generated objects returned by `Fetch***` methods.

### GlobalDesc

| Field | Type |
|-------|------|
| `TickToMsCpu` | `number` (float) |
| `TickToMsGpu` | `number` (float) |

### GeneralInfo

| Field | Type |
|-------|------|
| `PlatformInfoJson` | `string` |

`PlatformInfoJson` contains platform metadata supplied by the capture producer.
Programmatically triggered client captures leave it empty; Studio, server, and
manually triggered captures may include it.

### GroupDesc

| Field | Type |
|-------|------|
| `GroupId` | `number` |
| `GroupName` | `string` |
| `Color` | `number` |
| `IsGpu` | `boolean` |

### TimerDesc

| Field | Type |
|-------|------|
| `TimerId` | `number` |
| `TimerName` | `string` |
| `GroupId` | `number` |
| `Color` | `number` |
| `IsUserTimer` | `boolean` |

### CounterDesc

| Field | Type |
|-------|------|
| `CounterId` | `number` |
| `CounterName` | `string` |
| `Parent` | `number` |
| `Sibling` | `number` |
| `FirstChild` | `number` |
| `Level` | `number` |

### ThreadDesc

| Field | Type |
|-------|------|
| `ThreadId` | `number` |
| `ThreadName` | `string` |
| `BufferSize` | `number` |
| `IsGpu` | `boolean` |

### FrameDesc

| Field | Type |
|-------|------|
| `FrameId` | `number` |
| `FrameAbsoluteId` | `number` |
| `TickStartCpu` | `number` |
| `TickEndCpu` | `number` |
| `TickStartGpu` | `number` |
| `TickEndGpu` | `number` |
| `LabelOffsetMin` | `number` |
| `LabelOffsetMax` | `number` |
| `IsPaused` | `boolean` |
| `IsIncomplete` | `boolean` |
| `ThreadLogs` | `Array of ThreadLogDescr` |

### ThreadLogDescr

| Field | Type |
|-------|------|
| `ThreadId` | `number` |
| `LogEntriesNum` | `number` |
| `LogStartLocator` | `number` |
| `IsRemoved` | `boolean` |

### CounterSample

| Field | Type |
|-------|------|
| `FrameId` | `number` |
| `Value` | `number` (double) |

### U64Sample

| Field | Type |
|-------|------|
| `FrameId` | `number` |
| `Value` | `number` |

### TrackDesc

| Field | Type |
|-------|------|
| `TimestampStart` | `64-bit integer` |
| `TimestampStop` | `64-bit integer` |
| `AppType` | `Enums.AppType` |

`Enums.AppType` contains `Unknown`, `Client`, `Studio`, and `Server`.

### SegmentDesc

| Field | Type |
|-------|------|
| `TimestampStart` | `64-bit integer` |
| `TimestampStop` | `64-bit integer` |
| `FrameIdBegin` | `number` |
| `FrameIdEnd` | `number` |
| `FrameAbsoluteIdBegin` | `number` |
| `Markers` | `number` bit mask |

---

## Fetch vs Get

LibMP offers two access patterns for the same data:

| Aspect | `Get***` | `Fetch***` |
|--------|----------|------------|
| Returns | View (getter methods) | Language-native owned object |
| Lifetime | **Invalidated by next `Get`/`Fetch` call** | Permanent (owned copy) |
| Performance | Faster for single-field access | Slight overhead but simpler |
| String fields | Allocates on every access | Allocated once |

**Recommendation:** Use `Fetch***` by default. Use `Get***` only when performance is critical and you access the data immediately.

**Exception:** Iterator state objects (from `iter:GetState()`) persist for the iterator's lifetime and are updated in-place on each `Step()`.

**Exception:** Arrays returned by `Get***` methods persist until explicitly Disposed. Their individual elements follow normal `Get***` rules.

---

## Memory Management

Objects that **must be Disposed** manually:

- `Session` — call `:Dispose()` when analysis is complete
- `LogIterator`, `CounterIterator` — call `:Dispose()` when iteration is complete.
- Arrays from `Get***` methods (`GetCounterSamples`, `GetTimerIds`, etc.)

Failing to dispose arrays will leak memory. Monitor with `LibMP.GetMemUsed()`.

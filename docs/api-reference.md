# LibMP API Reference

LibMP is a Luau library for programmatic access to Roblox MicroProfiler data. Works in all game clients, servers, and in command line Luau.

Syntax Note: All API methods are dynamic (called via `:`), with the exception of constructors like `Session.OpenFrom***` and direct global methods like `LibMP.***`, which use a dot (`.`).

Some functions have both `Fetch***` and `Get***` variants. See the **"Fetch vs Get"** section below for details. **TL;DR:** prefer `Fetch***`.

---

## LibMP (Top-Level)

| Member | Type | Description |
|--------|------|-------------|
| `LibMP.IsCliMode` | `boolean` | `true` if running in a CLI environment (Lute) |
| `LibMP.Versions` | `Versions` | Version info. Contains `Versions.Library` and `Versions.DataFormat` |
| `LibMP.GetMemUsed()` | `number` | Current memory usage of the WASM instance |

---

## Control

Global profiler control. Access via `LibMP.Control`.

| Method | Returns | Description |
|--------|---------|-------------|
| `:IsBackendReady()` | `boolean` | API accessible and version-compatible |
| `:IsBackendAccessible()` | `boolean` | Remote API is reachable |
| `:IsBackendVersionCompatible()` | `boolean` | Version check only |
| `:EnableProfiler(enable)` | `boolean` | Toggle the profiler on/off |
| `:EnableCapture(enable)` | `boolean` | Activate/pause data capture |
| `:SetFrameLimit(num)` | `boolean` | Set rolling frame buffer size (max 256) |
| `:ShowUI(show)` | `boolean` | Toggle on-screen UI (Studio only) |
| `:CaptureToBufferSync()` | `buffer` | Snapshot live data into a buffer |
| `:UseControlChannel(use)` | `boolean` | Enable/disable control channel |

---

## Session

Represents a MicroProfiler session with internal caching.

### Constructors

| Method | Returns | Description |
|--------|---------|-------------|
| `Session.OpenFromLiveData()` | `Session?` | Opens from slot 0 (Engine) or CLI file argument |
| `Session.OpenFromBuffer(b: buffer)` | `Session?` | Opens from a raw Luau buffer |
| `Session.OpenFromMem(data: string \| buffer)` | `Session?` | Opens from a string or buffer (copies internally) |
| `Session.OpenFromSlot(id: number)` | `Session?` | Opens from a specific MicroProfiler Service slot |
| `Session.OpenFromFile(path: string, [cacheSize])` | `Session?` | Opens from a `.gprx` dump file |
| `Session.OpenFromExternalProvider(io: table, [cacheSize])` | `Session?` | Opens with a custom IO handler |

### Lifecycle & State

| Method | Returns | Description |
|--------|---------|-------------|
| `:Dispose()` | `nil` | Cleans up the session. **Must be called when done.** |
| `:IsValid()` | `boolean` | Whether the session is usable |
| `:GetDataFormatVersion()` | `number` | Data format version of this session |
| `:GetObjSize()` | `number` | Size of the last accessed object |
| `:WithDataLock(callback)` | `(boolean, any)` | Locks the first `Get***` view inside the callback so it remains valid across subsequent calls |

### Synchronization & Caching

| Method | Returns | Description |
|--------|---------|-------------|
| `:SyncWithDataSource()` | `boolean` | Manually sync with the data source |
| `:SetSyncRequired()` | `nil` | Marks session as needing sync on next access |
| `:SetClearCacheOnSyncRequired()` | `nil` | Flags cache to clear on next sync |
| `:SyncIfRequired()` | `nil` | Syncs if previously marked required |
| `:EnableObjectCache(enabled)` | `nil` | Toggle descriptor caching (default: on) |
| `:ClearObjectCache()` | `nil` | Force-clear the cache |
| `:ClearLuauCacheBucket(bucketIdx)` | `nil` | Clear specific cache bucket (1=ShortTerm, 2=LongTerm) |

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
| `:Dispose()` | `nil` | **Must be called before session disposal** |

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
| `:Fetch(index)` | `Luau table \| number` | Deep-copied element |
| `:FetchAll()` | `Array` | All elements as a 1-based Luau array |
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

Native Luau tables returned by `Fetch***` methods.

### GlobalDesc

| Field | Type |
|-------|------|
| `TickToMsCpu` | `number` (float) |
| `TickToMsGpu` | `number` (float) |

### GeneralInfo

| Field | Type |
|-------|------|
| `PlatformInfoJson` | `string` |

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

---

## Fetch vs Get

LibMP offers two access patterns for the same data:

| Aspect | `Get***` | `Fetch***` |
|--------|----------|------------|
| Returns | View (getter methods) | Native Luau table (direct fields) |
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
- `LogIterator` — dispose **before** its parent session
- `CounterIterator` — dispose before its parent session
- Arrays from `Get***` methods (`GetCounterSamples`, `GetTimerIds`, etc.)

Failing to dispose arrays will leak memory. Monitor with `LibMP.GetMemUsed()`.

---

## Known Issues

`GeneralInfo.PlatformInfoJson` - not implemented; always empty  
`TimerDesc.IsUserTimer` - not implemented; always false  

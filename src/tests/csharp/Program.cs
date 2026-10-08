// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

using System.Runtime.CompilerServices;
using LibMP;
using MpTypes;

static void Check(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

if (args.Length != 1 && args.Length != 3)
{
    Console.Error.WriteLine(
        "usage: LibMPSmoke <capture.gprx> [<short-update.gprx> <long-update.gprx>]");
    return 2;
}

using var libmp = LibMPInstance.Create();
Check(libmp.Versions.Library != 0, "library version is missing");
_ = libmp.GetMemReserved();
Check(!libmp.Control.EnableProfiler(false), "default control channel must be a no-op");
Check(!libmp.Control.SetMemTargetMB(64), "default memory-target control must be a no-op");
Check(Enums.DataSlot.Live == 0 && Enums.DataSlot.Temporary == 5,
    "data-slot enum is unavailable");
Check(Enums.ContentMode.Full == MpTypes.ContentMode.Full &&
      Enums.ContentMode.CountersOnly == MpTypes.ContentMode.CountersOnly,
    "content-mode enum is unavailable");
Check(libmp.Control.CaptureToBufferSync() == null &&
    !libmp.Control.CaptureToSlotSync(Enums.DataSlot.FirstUserManaged) &&
    libmp.Control.SaveSlotToBuffer(Enums.DataSlot.FirstUserManaged) == null &&
    !libmp.Control.ResetSlot(Enums.DataSlot.FirstUserManaged),
    "standalone slot controls must be no-op stubs");
Check(libmp.OpenFromSlot(Enums.DataSlot.Live, 4096) == null,
    "standalone slot session must be a no-op stub");
Check(!libmp.Experimental.V1.StartFileStream() &&
      !libmp.Experimental.V1.StartMemoryStream(1) &&
      !libmp.Experimental.V1.StopStream() &&
      !libmp.Experimental.V1.SetStreamMarker(1, true) &&
      !libmp.Experimental.V1.SetContentMode(Enums.ContentMode.CountersOnly),
    "standalone experimental controls must be no-op stubs");
var streamStatus = libmp.Experimental.V1.GetStreamStatus();
Check(!streamStatus.Active && streamStatus.RecordedSizeKb == 0 &&
    streamStatus.MaxSizeKb == 0 && streamStatus.ElapsedMs == 0,
    "standalone stream status must be empty");
Check(libmp.Tools.SetHtmlAssetsBaseUrl(""), "HTML asset-base setter failed");

byte[] capture = File.ReadAllBytes(args[0]);
using var session = libmp.OpenFromMem(capture)
    ?? throw new InvalidOperationException("OpenFromMem failed");
Check(session.IsValid(), "OpenFromMem returned an invalid session");
Check(session.GetFrameIdMax() >= session.GetFrameIdMin(), "invalid frame bounds");
Check(session.FetchTimerIds().Length != 0, "timer array is empty");
Check(session.FetchCounterIds().Length != 0, "counter array is empty");
Check(session.FetchThreadIds().Length != 0, "thread array is empty");

var globalView = session.GetGlobalDesc()
    ?? throw new InvalidOperationException("global descriptor is missing");
float expectedTick = globalView.TickToMsCpu;

// Opening another copied capture forces growth from the four-MiB initial heap.
using var growthSession = libmp.OpenFromMem(capture);
Check(growthSession?.IsValid() == true, "second session failed");
Check(globalView.TickToMsCpu == expectedTick,
    "FlatBuffers view did not follow grown WASM memory");

uint timerId = session.FetchTimerIds()[0];
var shortTermCached = session.FetchGlobalDesc()
    ?? throw new InvalidOperationException("global descriptor is missing");
var longTermCached = session.FetchTimerDesc(timerId)
    ?? throw new InvalidOperationException("timer descriptor is missing");
Check(ReferenceEquals(session.FetchGlobalDesc(), shortTermCached),
    "short-term object cache did not reuse a fetched object");
Check(ReferenceEquals(session.FetchTimerDesc(timerId), longTermCached),
    "long-term object cache did not reuse a fetched object");
session.SetClearCacheOnSyncRequired();
session.SetSyncRequired();
Check(!ReferenceEquals(session.FetchGlobalDesc(), shortTermCached),
    "sync callback did not clear the short-term wrapper cache bucket");
Check(!ReferenceEquals(session.FetchTimerDesc(timerId), longTermCached),
    "sync callback did not clear the long-term wrapper cache bucket");

var lockCachedView = session.GetGlobalDesc()
    ?? throw new InvalidOperationException("global descriptor is missing");
var lockCachedObject = session.FetchTimerDesc(timerId)
    ?? throw new InvalidOperationException("timer descriptor is missing");
Check(session.WithDataLock(() =>
{
    var cachedView = session.GetGlobalDesc();
    Check(cachedView.HasValue && cachedView.Value.TickToMsCpu == lockCachedView.TickToMsCpu,
        "locked cached view changed");
    Check(session.WithDataLock(() =>
    {
        Check(ReferenceEquals(session.FetchTimerDesc(timerId), lockCachedObject),
            "nested locked cache hit did not preserve fetched-object identity");
    }), "nested data lock failed");
}), "outer data lock failed");

using (var counters = session.CreateCounterIterator())
{
    Check(counters.Configure(new CounterIteratorConfigT()), "counter configuration failed");
    Check(counters.Step(), "counter iterator is empty");
    Check(counters.GetState().HasValue, "counter iterator state is missing");
}

using (var log = session.CreateLogIterator())
{
    Check(log.Configure(new LogIteratorConfigT { StartFrameId = session.GetFrameIdMin() }),
        "log configuration failed");
    uint entryCount = 0;
    while (log.Step() && entryCount == 0)
    {
        Check(log.GetState().HasValue, "log iterator state is missing");
        entryCount = log.GetCurrentEntryCount();
    }
    Check(entryCount != 0, "log iterator has no physical entry");
    for (uint index = 0; index < entryCount; ++index)
    {
        _ = log.GetCurrentEntryPart(index, 0);
        _ = log.GetCurrentEntryPart(index, 1);
    }
}

byte[]? rawConversion = null;
foreach (MpTypes.ContainerFormat format in Enum.GetValues<MpTypes.ContainerFormat>())
{
    byte[] converted = libmp.Tools.ConvertToBuffer(session, format)
        ?? throw new InvalidOperationException("buffer conversion failed");
    using var convertedSession = libmp.OpenFromMem(converted)
        ?? throw new InvalidOperationException("converted buffer could not be opened");
    Check(convertedSession.IsValid(), "converted buffer is invalid");
    Check(convertedSession.GetFrameIdMax() == session.GetFrameIdMax(),
        "converted buffer changed frame bounds");
    if (format == Enums.ContainerFormat.Raw)
        rawConversion = converted;
}
string convertedPath = args[0] + ".csharp-converted.gprx";
try
{
    Check(libmp.Tools.ConvertToFile(
        session, Enums.ContainerFormat.Raw, convertedPath), "file conversion failed");
    Check(File.ReadAllBytes(convertedPath).AsSpan().SequenceEqual(rawConversion),
        "file and buffer conversion differ");
}
finally
{
    File.Delete(convertedPath);
}
Check(session.IsValid(), "conversion invalidated its source session");

using var externalSession = libmp.OpenFromBuffer(capture, 64 * 1024);
Check(externalSession?.IsValid() == true, "OpenFromBuffer failed");
using var fileSession = libmp.OpenFromFile(args[0]);
Check(fileSession?.IsValid() == true, "OpenFromFile failed");

// Closing a session detaches its iterators: they stop stepping but stay disposable
var closedSession = libmp.OpenFromMem(capture)
    ?? throw new InvalidOperationException("OpenFromMem failed");
var detachedCounters = closedSession.CreateCounterIterator();
var detachedLog = closedSession.CreateLogIterator();
Check(detachedCounters.Configure(new CounterIteratorConfigT()) && detachedCounters.Step(),
    "counter iterator did not step");
Check(detachedLog.Configure(new LogIteratorConfigT { StartFrameId = closedSession.GetFrameIdMin() }) &&
    detachedLog.Step(), "log iterator did not step");
closedSession.Dispose();
Check(!detachedCounters.Step() && !detachedLog.Step(), "detached iterator kept stepping");
Check(detachedCounters.GetState().HasValue && detachedLog.GetState().HasValue,
    "detached iterator lost its state");
Check(!detachedCounters.Configure(new CounterIteratorConfigT()) &&
    !detachedLog.Configure(new LogIteratorConfigT()),
    "detached iterator accepted a configuration");
detachedCounters.Rewind();
detachedLog.Rewind();
detachedCounters.Dispose();
detachedLog.Dispose();

// Arrays read nothing once they or their session are disposed
var arraySession = libmp.OpenFromMem(capture)
    ?? throw new InvalidOperationException("OpenFromMem failed");
uint counterId = arraySession.FetchCounterIds()[0];
var samples = arraySession.GetCounterSamples(counterId);
var released = arraySession.GetCounterSamples(counterId);
Check(samples.Fetch(0) != null && released.Fetch(0) != null, "counter samples are empty");
released.Dispose();
Check(released.Fetch(0) == null, "a disposed array read its elements");
arraySession.Dispose();
Check(samples.Fetch(0) == null && samples.FetchLastSample() == null,
    "an array read its closed session");
samples.Dispose();

// Finalizers queue the objects that were not disposed, and the next LibMP call frees them
[MethodImpl(MethodImplOptions.NoInlining)]
static void CreateGarbage(Session session, bool dispose)
{
    var objects = new List<IDisposable>();
    for (int index = 0; index < 64; index++)
    {
        objects.Add(session.CreateLogIterator());
        objects.Add(session.CreateCounterIterator());
        objects.Add(session.GetTimerIds());
    }
    if (dispose)
        objects.ForEach(item => item.Dispose());
}

[MethodImpl(MethodImplOptions.NoInlining)]
static void CreateGarbageSessions(LibMPInstance libmp, byte[] capture, IDataProvider provider)
{
    CreateGarbage(libmp.OpenFromMem(capture)!, false);
    CreateGarbage(libmp.OpenFromExternalProvider(provider)!, false);
}

static uint MemUsedAfterFinalizers(LibMPInstance libmp)
{
    GC.Collect();
    GC.WaitForPendingFinalizers();
    libmp.GetMemUsed(); // frees the queued objects as it returns
    return libmp.GetMemUsed();
}

uint memUsed = MemUsedAfterFinalizers(libmp);
var finalizedSession = libmp.OpenFromMem(capture)
    ?? throw new InvalidOperationException("OpenFromMem failed");
CreateGarbage(finalizedSession, true); // grows the session's iterator lists first
uint sessionMemUsed = libmp.GetMemUsed();
CreateGarbage(finalizedSession, false);
Check(MemUsedAfterFinalizers(libmp) == sessionMemUsed,
    "finalizers did not release unreachable objects");
CreateGarbage(finalizedSession, false);
finalizedSession.Dispose();
Check(MemUsedAfterFinalizers(libmp) == memUsed,
    "finalizers did not release objects that outlived their session");
var finalizedProvider = new MutableDataProvider(capture);
CreateGarbageSessions(libmp, capture, finalizedProvider);
Check(MemUsedAfterFinalizers(libmp) == memUsed && finalizedProvider.Closed,
    "finalizers did not release unreachable sessions");

if (args.Length == 3)
{
    var provider = new MutableDataProvider(capture);
    using var relatedSession = libmp.OpenFromExternalProvider(provider)
        ?? throw new InvalidOperationException("related-snapshot session failed");
    uint relatedTimerId = relatedSession.FetchTimerIds()[0];
    var baselineGlobal = relatedSession.FetchGlobalDesc()!;
    var baselineGeneral = relatedSession.FetchGeneralInfo()!;
    var baselineTimer = relatedSession.FetchTimerDesc(relatedTimerId)!;

    provider.Data = File.ReadAllBytes(args[1]);
    relatedSession.SetSyncRequired();
    var shortGeneral = relatedSession.FetchGeneralInfo()!;
    var shortGlobal = relatedSession.FetchGlobalDesc()!;
    var timerAfterShortUpdate = relatedSession.FetchTimerDesc(relatedTimerId)!;
    Check(shortGeneral.PlatformInfoJson != baselineGeneral.PlatformInfoJson,
        "short-term fixture did not change its metadata");
    Check(!ReferenceEquals(shortGlobal, baselineGlobal),
        "short-term update did not invalidate the short-term cache bucket");
    Check(ReferenceEquals(timerAfterShortUpdate, baselineTimer),
        "short-term update invalidated the long-term cache bucket");

    provider.Data = File.ReadAllBytes(args[2]);
    relatedSession.SetSyncRequired();
    var generalAfterLongUpdate = relatedSession.FetchGeneralInfo()!;
    var globalAfterLongUpdate = relatedSession.FetchGlobalDesc()!;
    var longTimer = relatedSession.FetchTimerDesc(relatedTimerId)!;
    Check(ReferenceEquals(generalAfterLongUpdate, shortGeneral) &&
        ReferenceEquals(globalAfterLongUpdate, shortGlobal),
        "long-term update invalidated the short-term cache bucket");
    Check(!ReferenceEquals(longTimer, timerAfterShortUpdate),
        "long-term update did not invalidate the long-term cache bucket");
    Check(longTimer.TimerName != timerAfterShortUpdate.TimerName,
        "long-term fixture did not change its metadata");
}

// Disposing an instance closes the sessions still open on it
var disposedInstance = LibMPInstance.Create();
var openProvider = new MutableDataProvider(capture);
var openSession = disposedInstance.OpenFromExternalProvider(openProvider)
    ?? throw new InvalidOperationException("OpenFromExternalProvider failed");
var disposedView = openSession.GetGlobalDesc()
    ?? throw new InvalidOperationException("global descriptor is missing");
disposedInstance.Dispose();
Check(openProvider.Closed, "disposing an instance left its sessions open");
bool disposedViewRejected = false;
try
{
    _ = disposedView.TickToMsCpu;
}
catch (ObjectDisposedException)
{
    disposedViewRejected = true;
}
Check(disposedViewRejected, "a FlatBuffers view read disposed WASM memory");
openSession.Dispose(); // already closed, so it does not call into the disposed instance

// A callback runs inside a Wasmtime call, so it must not close that call's store.
var callbackInstance = LibMPInstance.Create();
var callbackProvider = new MutableDataProvider(capture);
bool callbackDisposeRejected = false;
callbackProvider.BeforeRead = () =>
{
    callbackProvider.BeforeRead = null;
    try
    {
        callbackInstance.Dispose();
    }
    catch (InvalidOperationException)
    {
        callbackDisposeRejected = true;
    }
};
var callbackSession = callbackInstance.OpenFromExternalProvider(callbackProvider)
    ?? throw new InvalidOperationException("OpenFromExternalProvider failed");
Check(callbackDisposeRejected, "disposing a LibMP instance from its callback was allowed");
Check(callbackSession.IsValid(), "callback disposal attempt damaged the LibMP instance");
callbackSession.Dispose();
callbackInstance.Dispose();

// Separate WASM instances intentionally reuse small callback handles. Their
// provider/session registries must remain instance-local.
using var otherA = LibMPInstance.Create();
using var otherB = LibMPInstance.Create();
using var otherSessionA = otherA.OpenFromBuffer(capture);
using var otherSessionB = otherB.OpenFromBuffer(capture);
Check(otherSessionA?.IsValid() == true && otherSessionB?.IsValid() == true,
    "multiple LibMP instances interfered with external IO routing");

Console.WriteLine("LibMP C# smoke test passed");
return 0;

sealed class MutableDataProvider : IDataProvider
{
    public byte[] Data { get; set; }
    public bool Closed { get; private set; }
    public Action? BeforeRead { get; set; }

    public MutableDataProvider(byte[] data) { Data = data; }
    public bool Open() => true;
    public bool Close()
    {
        Closed = true;
        return true;
    }
    public ulong GetSize() => (ulong)Data.LongLength;

    public int Read(Span<byte> destination, uint blockId, ulong offset, int size)
    {
        BeforeRead?.Invoke();
        if (blockId != 0 || offset > (ulong)Data.Length || size < 0 ||
            (ulong)size > (ulong)Data.Length - offset)
            return 0;
        Data.AsSpan((int)offset, size).CopyTo(destination);
        return size;
    }
}

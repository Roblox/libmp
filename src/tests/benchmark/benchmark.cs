// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

using System.Diagnostics;
using System.Numerics;
using System.Text;
using LibMP;

static uint Mix(uint hash, uint value) => (BitOperations.RotateLeft(hash, 5) ^ value) + 0x9e3779b9u;
static uint ByteLength(string? value) => value == null ? 0u : checked((uint)Encoding.UTF8.GetByteCount(value));

static (uint Checksum, ulong ItemCount) Workload(Session session, int passes)
{
    uint hash = 0x811c9dc5u;
    ulong itemCount = 0;
    for (int pass = 0; pass < passes; ++pass)
    {
        hash = Mix(hash, session.GetFrameIdMin());
        hash = Mix(hash, session.GetFrameIdMax());
        hash = Mix(hash, session.FetchGlobalDesc() == null ? 0u : 1u);
        var general = session.FetchGeneralInfo();
        hash = Mix(hash, general == null ? 0u : ByteLength(general.PlatformInfoJson));
        itemCount += 2;

        uint groupIdMax = session.GetGroupIdMax();
        hash = Mix(hash, groupIdMax);
        itemCount += groupIdMax;
        for (uint id = 1; id <= groupIdMax; ++id)
        {
            var value = session.FetchGroupDesc(id);
            if (value == null)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value.GroupId);
            hash = Mix(hash, ByteLength(value.GroupName));
            hash = Mix(hash, value.Color);
            hash = Mix(hash, value.IsGpu ? 1u : 0u);
        }

        uint[] timerIds = session.FetchTimerIds();
        hash = Mix(hash, checked((uint)timerIds.Length));
        itemCount += checked((ulong)timerIds.Length);
        foreach (uint id in timerIds)
        {
            hash = Mix(hash, id);
            var value = session.FetchTimerDesc(id);
            if (value == null)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value.TimerId);
            hash = Mix(hash, ByteLength(value.TimerName));
            hash = Mix(hash, value.GroupId);
            hash = Mix(hash, value.Color);
            hash = Mix(hash, value.IsUserTimer ? 1u : 0u);
        }

        uint[] counterIds = session.FetchCounterIds();
        hash = Mix(hash, checked((uint)counterIds.Length));
        itemCount += checked((ulong)counterIds.Length);
        foreach (uint id in counterIds)
        {
            hash = Mix(hash, id);
            var value = session.FetchCounterDesc(id);
            if (value == null)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value.CounterId);
            hash = Mix(hash, ByteLength(value.CounterName));
            hash = Mix(hash, value.Parent);
            hash = Mix(hash, value.Sibling);
            hash = Mix(hash, value.FirstChild);
            hash = Mix(hash, value.Level);
        }

        uint[] threadIds = session.FetchThreadIds();
        hash = Mix(hash, checked((uint)threadIds.Length));
        itemCount += checked((ulong)threadIds.Length);
        foreach (uint id in threadIds)
        {
            hash = Mix(hash, id);
            var value = session.FetchThreadDesc(id);
            if (value == null)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value.ThreadId);
            hash = Mix(hash, ByteLength(value.ThreadName));
            hash = Mix(hash, value.BufferSize);
            hash = Mix(hash, value.IsGpu ? 1u : 0u);
        }
    }
    return (hash, itemCount);
}

static (uint Checksum, ulong ItemCount) LogWorkload(Session session, int passes)
{
    uint hash = 0x811c9dc5u;
    ulong itemCount = 0;
    for (int pass = 0; pass < passes; ++pass)
    {
        using var iterator = session.CreateLogIterator();
        if (!iterator.Configure(new MpTypes.LogIteratorConfigT { EmitThreadFrameEnd = true }))
            throw new InvalidOperationException("failed to configure log iterator");
        var state = iterator.GetState()
            ?? throw new InvalidOperationException("log iterator has no state");
        uint entryCount = 0;
        while (iterator.Step())
        {
            hash = Mix(hash, state.FrameId);
            hash = Mix(hash, state.ThreadId);
            hash = Mix(hash, state.TimerId);
            hash = Mix(hash, state.EntryTypeRaw);
            hash = Mix(hash, state.EventType);
            hash = Mix(hash, state.IsEvent ? 1u : 0u);
            var timer = session.FetchTimerDesc(state.TimerId);
            if (timer != null)
            {
                hash = Mix(hash, timer.TimerId);
                hash = Mix(hash, ByteLength(timer.TimerName));
                hash = Mix(hash, timer.Color);
            }
            else
            {
                hash = Mix(hash, 0);
            }
            ++entryCount;
        }
        hash = Mix(hash, entryCount);
        itemCount += entryCount;
    }
    return (hash, itemCount);
}

static Session OpenSession(LibMPInstance libmp, byte[] capture, bool cache)
{
    var session = libmp.OpenFromMem(capture)
        ?? throw new InvalidOperationException("failed to open capture");
    if (!session.IsValid())
    {
        session.Dispose();
        throw new InvalidOperationException("capture opened as an invalid session");
    }
    session.EnableObjectCache(cache);
    return session;
}

static long ElapsedNanoseconds(long start) => checked((long)Math.Round(
    (Stopwatch.GetTimestamp() - start) * (1_000_000_000.0 / Stopwatch.Frequency)));

static void Emit(string scenario, bool cache, (uint Checksum, ulong ItemCount) result,
    List<long> samples) =>
    Console.WriteLine($"BENCH\t{scenario}\t{(cache ? "on" : "off")}\t{result.Checksum}\t" +
        $"{result.ItemCount}\t{string.Join(',', samples)}");

static void RunShared(LibMPInstance libmp, byte[] capture, bool cache,
    int warmups, int iterations, int passes)
{
    using var session = OpenSession(libmp, capture, cache);
    for (int index = 0; index < warmups; ++index)
        _ = Workload(session, passes);
    var samples = new List<long>(iterations);
    (uint Checksum, ulong ItemCount)? result = null;
    for (int index = 0; index < iterations; ++index)
    {
        long start = Stopwatch.GetTimestamp();
        var current = Workload(session, passes);
        samples.Add(ElapsedNanoseconds(start));
        if (result != null && current != result.Value)
            throw new InvalidOperationException("workload checksum changed");
        result = current;
    }
    Emit("shared", cache, result!.Value, samples);
}

static void RunFresh(LibMPInstance libmp, byte[] capture, bool cache,
    int warmups, int iterations, int passes)
{
    (uint Checksum, ulong ItemCount) Run()
    {
        using var session = OpenSession(libmp, capture, cache);
        return Workload(session, passes);
    }

    for (int index = 0; index < warmups; ++index)
        _ = Run();
    var samples = new List<long>(iterations);
    (uint Checksum, ulong ItemCount)? result = null;
    for (int index = 0; index < iterations; ++index)
    {
        long start = Stopwatch.GetTimestamp();
        var current = Run();
        samples.Add(ElapsedNanoseconds(start));
        if (result != null && current != result.Value)
            throw new InvalidOperationException("workload checksum changed");
        result = current;
    }
    Emit("fresh", cache, result!.Value, samples);
}

static void RunLogs(LibMPInstance libmp, byte[] capture, bool cache,
    int warmups, int iterations, int passes)
{
    using var session = OpenSession(libmp, capture, cache);
    for (int index = 0; index < warmups; ++index)
        _ = LogWorkload(session, passes);
    var samples = new List<long>(iterations);
    (uint Checksum, ulong ItemCount)? result = null;
    for (int index = 0; index < iterations; ++index)
    {
        long start = Stopwatch.GetTimestamp();
        var current = LogWorkload(session, passes);
        samples.Add(ElapsedNanoseconds(start));
        if (result != null && current != result.Value)
            throw new InvalidOperationException("log workload checksum changed");
        result = current;
    }
    Emit("logs", cache, result!.Value, samples);
}

static int Positive(string text, string name, bool allowZero)
{
    if (!int.TryParse(text, out int value) || value < 0 || (!allowZero && value == 0))
        throw new ArgumentException($"invalid {name}");
    return value;
}

if (args.Length != 7)
{
    Console.Error.WriteLine(
        "usage: LibMPBenchmark <capture> <warmups> <iterations> " +
        "<metadata-passes> <log-warmups> <log-iterations> <log-passes>");
    return 2;
}

byte[] capture = File.ReadAllBytes(args[0]);
int warmups = Positive(args[1], "warmup count", true);
int iterations = Positive(args[2], "iteration count", false);
int metadataPasses = Positive(args[3], "metadata pass count", false);
int logWarmups = Positive(args[4], "log warmup count", true);
int logIterations = Positive(args[5], "log iteration count", false);
int logPasses = Positive(args[6], "log pass count", false);
using var libmp = LibMPInstance.Create();
foreach (bool cache in new[] { false, true })
{
    Console.Error.WriteLine($"  cache={(cache ? "on" : "off")}: metadata / reused session");
    RunShared(libmp, capture, cache, warmups, iterations, metadataPasses);
    Console.Error.WriteLine($"  cache={(cache ? "on" : "off")}: metadata / new session");
    RunFresh(libmp, capture, cache, warmups, iterations, metadataPasses);
    Console.Error.WriteLine(
        $"  cache={(cache ? "on" : "off")}: full log traversal / reused session");
    RunLogs(libmp, capture, cache, logWarmups, logIterations, logPasses);
}
return 0;

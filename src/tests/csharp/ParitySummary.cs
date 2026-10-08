// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

using System.Globalization;
using System.Text;
using LibMP;

sealed class ParitySummary
{
    private readonly LibMPInstance _libmp;
    private readonly Session _session;
    private readonly StringBuilder _output = new();

    public ParitySummary(LibMPInstance libmp, Session session)
    {
        _libmp = libmp;
        _session = session;
    }

    private static int Bool(bool value) => value ? 1 : 0;
    private static string HexU32(uint value) => value.ToString("x8", CultureInfo.InvariantCulture);
    private static string HexU64(ulong value) => value.ToString("x16", CultureInfo.InvariantCulture);
    private static string FloatBits(float value) => HexU32(BitConverter.SingleToUInt32Bits(value));
    private static string DoubleBits(double value) => HexU64(BitConverter.DoubleToUInt64Bits(value));

    private static string HexBytes(string? value)
    {
        if (value == null)
            return "-";
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        return $"{bytes.Length}:{Convert.ToHexString(bytes).ToLowerInvariant()}";
    }

    private void Emit(params object[] fields)
    {
        for (int index = 0; index < fields.Length; ++index)
        {
            if (index != 0)
                _output.Append(' ');
            _output.Append(Convert.ToString(fields[index], CultureInfo.InvariantCulture));
        }
        _output.Append('\n');
    }

    public string Write()
    {
        uint frameIdMin = _session.GetFrameIdMin();
        uint frameIdMax = _session.GetFrameIdMax();
        Emit("LIBMP_PARITY", 3);
        Emit("SESSION", _session.GetDataFormatVersion(), _libmp.Versions.Library,
            frameIdMin, frameIdMax);

        var global = _session.FetchGlobalDesc();
        if (global != null)
            Emit("GLOBAL", 1, FloatBits(global.TickToMsCpu), FloatBits(global.TickToMsGpu));
        else
            Emit("GLOBAL", 0);
        var general = _session.FetchGeneralInfo();
        if (general != null)
            Emit("GENERAL", 1, HexBytes(general.PlatformInfoJson));
        else
            Emit("GENERAL", 0);

        uint groupIdMax = _session.GetGroupIdMax();
        Emit("GROUPS", groupIdMax);
        for (uint queryId = 1; queryId <= groupIdMax; ++queryId)
        {
            var group = _session.FetchGroupDesc(queryId);
            if (group != null)
                Emit("GROUP", queryId, 1, group.GroupId, HexBytes(group.GroupName),
                    HexU32(group.Color), Bool(group.IsGpu));
            else
                Emit("GROUP", queryId, 0);
        }

        uint[] timerIds = _session.FetchTimerIds();
        Emit("TIMER_IDS", timerIds.Length);
        for (int index = 0; index < timerIds.Length; ++index)
        {
            uint id = timerIds[index];
            Emit("TIMER_ID", index, id);
            var timer = _session.FetchTimerDesc(id);
            if (timer != null)
                Emit("TIMER", id, 1, timer.TimerId, HexBytes(timer.TimerName),
                    timer.GroupId, HexU32(timer.Color), Bool(timer.IsUserTimer));
            else
                Emit("TIMER", id, 0);
        }
        for (uint groupId = 1; groupId <= groupIdMax; ++groupId)
        {
            uint[] ids = _session.FetchTimerIdsByGroupId(groupId);
            var fields = new object[3 + ids.Length];
            fields[0] = "GROUP_TIMER_IDS";
            fields[1] = groupId;
            fields[2] = ids.Length;
            for (int index = 0; index < ids.Length; ++index)
                fields[3 + index] = ids[index];
            Emit(fields);
        }

        uint[] counterIds = _session.FetchCounterIds();
        Emit("COUNTER_IDS", counterIds.Length);
        for (int index = 0; index < counterIds.Length; ++index)
        {
            uint id = counterIds[index];
            Emit("COUNTER_ID", index, id);
            var counter = _session.FetchCounterDesc(id);
            if (counter == null)
            {
                Emit("COUNTER", id, 0);
                continue;
            }
            Emit("COUNTER", id, 1, counter.CounterId, HexBytes(counter.CounterName),
                counter.Parent, counter.Sibling, counter.FirstChild, counter.Level);
            var samples = _session.FetchCounterSamples(id);
            Emit("COUNTER_SAMPLES", id, samples.Length);
            for (int sampleIndex = 0; sampleIndex < samples.Length; ++sampleIndex)
            {
                var sample = samples[sampleIndex];
                Emit("COUNTER_SAMPLE", id, sampleIndex, 1,
                    sample.FrameId, DoubleBits(sample.Value));
            }
            var last = _session.FetchLastCounterSample(id);
            if (last != null)
                Emit("COUNTER_LAST", id, 1, last.FrameId, DoubleBits(last.Value));
            else
                Emit("COUNTER_LAST", id, 0);
        }

        Emit("GPU_THREAD", _session.GetThreadIdGpu());
        uint[] threadIds = _session.FetchThreadIds();
        Emit("THREAD_IDS", threadIds.Length);
        for (int index = 0; index < threadIds.Length; ++index)
        {
            uint id = threadIds[index];
            Emit("THREAD_ID", index, id);
            var thread = _session.FetchThreadDesc(id);
            if (thread != null)
                Emit("THREAD", id, 1, thread.ThreadId, HexBytes(thread.ThreadName),
                    thread.BufferSize, Bool(thread.IsGpu));
            else
                Emit("THREAD", id, 0);
        }

        ulong frameCount = frameIdMax >= frameIdMin ? (ulong)frameIdMax - frameIdMin + 1 : 0;
        Emit("FRAMES", frameCount);
        for (ulong offset = 0; offset < frameCount; ++offset)
        {
            uint queryId = frameIdMin + (uint)offset;
            var frame = _session.FetchFrameDesc(queryId);
            if (frame == null)
            {
                Emit("FRAME", queryId, 0);
                continue;
            }
            var threadLogs = frame.ThreadLogs ?? new List<MpTypes.ThreadLogDescrT>();
            Emit("FRAME", queryId, 1, frame.FrameId, frame.FrameAbsoluteId,
                HexU64(frame.TickStartCpu), HexU64(frame.TickEndCpu),
                HexU64(frame.TickStartGpu), HexU64(frame.TickEndGpu),
                HexU64(frame.LabelOffsetMin), HexU64(frame.LabelOffsetMax),
                Bool(frame.IsPaused), Bool(frame.IsIncomplete), threadLogs.Count);
            for (int logIndex = 0; logIndex < threadLogs.Count; ++logIndex)
            {
                var log = threadLogs[logIndex];
                Emit("FRAME_THREAD_LOG", queryId, logIndex, log.ThreadId, log.LogEntriesNum,
                    HexU64(log.LogStartLocator), Bool(log.IsRemoved));
            }
        }

        WriteU64Samples("PLACE_ID", _session.FetchPlaceIdSamples());
        WriteU64Samples("UTC", _session.FetchUtcTimestampSamples());
        WriteFindQueries();
        WriteCounterIterator();
        WriteLogIterator();
        Emit("END");
        return _output.ToString();
    }

    private void WriteU64Samples(string name, MpTypes.U64SampleT[] samples)
    {
        Emit($"{name}_SAMPLES", samples.Length);
        for (int index = 0; index < samples.Length; ++index)
            Emit($"{name}_SAMPLE", index, 1, samples[index].FrameId,
                HexU64(samples[index].Value));
    }

    private void WriteFindQueries()
    {
        (string Mask, bool Sensitive)[] probes =
        {
            ("*", false), ("*", true), ("*A*", false), ("*A*", true),
            ("__libmp_parity_missing__", false),
        };
        Emit("FIND_QUERIES", probes.Length);
        WriteFindFamily("GROUP", probes, _session.FindGroupId, _session.FindGroupIds);
        WriteFindFamily("TIMER", probes, _session.FindTimerId, _session.FindTimerIds);
        WriteFindFamily("COUNTER", probes, _session.FindCounterId, _session.FindCounterIds);
        WriteFindFamily("THREAD", probes, _session.FindThreadId, _session.FindThreadIds);

        Emit("GROUP_NAME_TIMER_QUERIES", probes.Length);
        for (int probeIndex = 0; probeIndex < probes.Length; ++probeIndex)
        {
            var probe = probes[probeIndex];
            uint[] ids = _session.FetchTimerIdsByGroupNames(probe.Mask, probe.Sensitive);
            var fields = new object[5 + ids.Length];
            fields[0] = "GROUP_NAME_TIMERS";
            fields[1] = probeIndex;
            fields[2] = Bool(probe.Sensitive);
            fields[3] = HexBytes(probe.Mask);
            fields[4] = ids.Length;
            for (int index = 0; index < ids.Length; ++index)
                fields[5 + index] = ids[index];
            Emit(fields);
        }
    }

    private void WriteFindFamily(string family, (string Mask, bool Sensitive)[] probes,
        Func<string?, bool, uint> findFirst, Func<string?, bool, uint[]> findAll)
    {
        for (int probeIndex = 0; probeIndex < probes.Length; ++probeIndex)
        {
            var probe = probes[probeIndex];
            uint[] ids = findAll(probe.Mask, probe.Sensitive);
            var fields = new object[7 + ids.Length];
            fields[0] = "FIND";
            fields[1] = family;
            fields[2] = probeIndex;
            fields[3] = Bool(probe.Sensitive);
            fields[4] = HexBytes(probe.Mask);
            fields[5] = findFirst(probe.Mask, probe.Sensitive);
            fields[6] = ids.Length;
            for (int index = 0; index < ids.Length; ++index)
                fields[7 + index] = ids[index];
            Emit(fields);
        }
    }

    private void WriteCounterIterator()
    {
        Emit("COUNTER_ITERATOR_BEGIN");
        using var iterator = _session.CreateCounterIterator();
        if (!iterator.Configure(new MpTypes.CounterIteratorConfigT
            { RootCounterIds = new List<uint>() }))
            throw new InvalidOperationException("failed to configure counter iterator");
        var state = iterator.GetState()
            ?? throw new InvalidOperationException("counter iterator has no state");
        int reportIndex = 0;
        while (iterator.Step())
        {
            Emit("CI", reportIndex++, Bool(state.Started), Bool(state.Finished),
                state.CounterId, state.ParentId, state.Level, state.RelativeLevel,
                state.RootLevel, state.IndexInSiblings, Bool(state.IsLeaf),
                Bool(state.HierarchyChanged), Bool(state.CounterStackIsOverflowed));
        }
        Emit("COUNTER_ITERATOR_END", reportIndex, Bool(state.Finished));
    }

    private void WriteLogIterator()
    {
        Emit("LOG_ITERATOR_BEGIN");
        using var iterator = _session.CreateLogIterator();
        if (!iterator.Configure(new MpTypes.LogIteratorConfigT
            { EmitThreadFrameEnd = true, TrackScopeLabels = true }))
            throw new InvalidOperationException("failed to configure log iterator");
        var state = iterator.GetState()
            ?? throw new InvalidOperationException("log iterator has no state");
        int reportIndex = 0;
        bool checkedLabelApis = false;
        while (iterator.Step())
        {
            if (!checkedLabelApis && !state.IsLabel)
            {
                if (iterator.FetchCurrentLabel() != null ||
                    iterator.GetCurrentScopeLabelCount() != 0 ||
                    iterator.GetScopeLabelCount(state.ThreadId, 0) != 0 ||
                    iterator.FetchCurrentScopeLabel(0) != null ||
                    iterator.FetchScopeLabel(state.ThreadId, 0, 0) != null)
                    throw new InvalidOperationException("non-label entry exposed label data");
                checkedLabelApis = true;
            }
            Emit("LI", reportIndex++, Bool(state.Started), Bool(state.Finished),
                state.ThreadStackDepth, Bool(state.ThreadStackIsUnderflowed),
                Bool(state.ThreadStackIsOverflowed), Bool(state.ThreadStackWasOverflowed),
                HexU64(state.ThreadPrevTimestamp), state.FrameId, state.FrameAbsoluteId,
                HexU64(state.FrameDescLocator), state.ThreadId, Bool(state.IsGpuThread),
                Bool(state.IsFrameStitching), Bool(state.FrameChanged), Bool(state.ThreadChanged),
                HexU64(state.EntryLocator), state.TimerId, HexU64(state.Timestamp),
                HexU64(state.TimestampGpu), state.EntryTypeRaw, state.EventType,
                Bool(state.IsUnknown), Bool(state.IsEnter), Bool(state.IsExit),
                Bool(state.IsLabel), Bool(state.IsEvent), Bool(state.IsThreadFrameEnd),
                Bool(state.IsFrameBoundary), iterator.GetCurrentEntryCount());
        }
        if (!checkedLabelApis)
            throw new InvalidOperationException("log iterator has no non-label entry");
        Emit("LOG_ITERATOR_END", reportIndex, Bool(state.Finished));
    }
}

static class CacheChecks
{
    private static void Check(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static string FloatBits(float value) =>
        BitConverter.SingleToUInt32Bits(value).ToString("x8", CultureInfo.InvariantCulture);

    private static void CheckCachedDescriptor<TView, TCopy>(
        Session session, string label, uint? firstId, uint? secondId,
        Func<uint?, TView?> get, Func<uint?, TCopy?> fetch,
        Func<TView, string> viewValue, Func<TCopy, string> fetchedValue)
        where TView : struct
        where TCopy : class
    {
        TView? firstView = get(firstId);
        TView? repeatedView = get(firstId);
        if (!firstView.HasValue || !repeatedView.HasValue)
            throw new InvalidOperationException($"{label}: Get returned null");
        string expected = viewValue(firstView.Value);
        Check(viewValue(repeatedView.Value) == expected,
            $"{label}: repeated Get changed value");
        if (secondId.HasValue)
        {
            _ = get(secondId);
            Check(get(firstId) is TView interleaved && viewValue(interleaved) == expected,
                $"{label}: interleaved Get changed value");
        }

        TCopy? firstCopy = fetch(firstId);
        TCopy? repeatedCopy = fetch(firstId);
        Check(firstCopy != null && ReferenceEquals(firstCopy, repeatedCopy),
            $"{label}: Fetch identity miss");
        Check(fetchedValue(firstCopy!) == expected, $"{label}: Get/Fetch value mismatch");
        if (secondId.HasValue)
        {
            _ = fetch(secondId);
            Check(ReferenceEquals(fetch(firstId), firstCopy),
                $"{label}: interleaved Fetch identity miss");
        }

        session.ClearObjectCache();
        TView? clearedView = get(firstId);
        TCopy? clearedCopy = fetch(firstId);
        Check(clearedView.HasValue && viewValue(clearedView.Value) == expected,
            $"{label}: ClearObjectCache changed Get value");
        Check(clearedCopy != null && !ReferenceEquals(clearedCopy, firstCopy),
            $"{label}: ClearObjectCache kept Fetch object");
        Check(fetchedValue(clearedCopy!) == expected,
            $"{label}: ClearObjectCache changed Fetch value");
    }

    public static void Run(
        LibMPInstance libmp, Session session, byte[] capture,
        string? shortUpdatePath, string? longUpdatePath)
    {
        session.EnableObjectCache(true);
        uint[] timerIds = session.FetchTimerIds();
        uint[] counterIds = session.FetchCounterIds();
        uint[] threadIds = session.FetchThreadIds();
        var groupIds = new List<uint>();
        for (uint id = 1; id <= session.GetGroupIdMax(); ++id)
        {
            if (session.FetchGroupDesc(id) != null)
                groupIds.Add(id);
        }

        if (session.FetchGlobalDesc() != null)
            CheckCachedDescriptor<MpTypes.GlobalDesc, MpTypes.GlobalDescT>(
                session, "GlobalDesc", null, null,
                _ => session.GetGlobalDesc(), _ => session.FetchGlobalDesc(),
                value => FloatBits(value.TickToMsCpu),
                value => FloatBits(value.TickToMsCpu));
        if (session.FetchGeneralInfo() != null)
            CheckCachedDescriptor<MpTypes.GeneralInfo, MpTypes.GeneralInfoT>(
                session, "GeneralInfo", null, null,
                _ => session.GetGeneralInfo(), _ => session.FetchGeneralInfo(),
                value => value.PlatformInfoJson ?? "", value => value.PlatformInfoJson ?? "");
        if (groupIds.Count != 0)
            CheckCachedDescriptor<MpTypes.GroupDesc, MpTypes.GroupDescT>(
                session, "GroupDesc", groupIds[0], groupIds.Count > 1 ? groupIds[1] : null,
                id => session.GetGroupDesc(id!.Value), id => session.FetchGroupDesc(id!.Value),
                value => $"{value.GroupId}:{value.GroupName}",
                value => $"{value.GroupId}:{value.GroupName}");
        if (timerIds.Length != 0)
            CheckCachedDescriptor<MpTypes.TimerDesc, MpTypes.TimerDescT>(
                session, "TimerDesc", timerIds[0], timerIds.Length > 1 ? timerIds[1] : null,
                id => session.GetTimerDesc(id!.Value), id => session.FetchTimerDesc(id!.Value),
                value => $"{value.TimerId}:{value.TimerName}",
                value => $"{value.TimerId}:{value.TimerName}");
        if (counterIds.Length != 0)
            CheckCachedDescriptor<MpTypes.CounterDesc, MpTypes.CounterDescT>(
                session, "CounterDesc", counterIds[0],
                counterIds.Length > 1 ? counterIds[1] : null,
                id => session.GetCounterDesc(id!.Value), id => session.FetchCounterDesc(id!.Value),
                value => $"{value.CounterId}:{value.CounterName}",
                value => $"{value.CounterId}:{value.CounterName}");
        if (threadIds.Length != 0)
            CheckCachedDescriptor<MpTypes.ThreadDesc, MpTypes.ThreadDescT>(
                session, "ThreadDesc", threadIds[0], threadIds.Length > 1 ? threadIds[1] : null,
                id => session.GetThreadDesc(id!.Value), id => session.FetchThreadDesc(id!.Value),
                value => $"{value.ThreadId}:{value.ThreadName}",
                value => $"{value.ThreadId}:{value.ThreadName}");

        if (threadIds.Length != 0)
        {
            session.EnableObjectCache(false);
            var view1 = session.GetThreadDesc(threadIds[0]);
            var view2 = session.GetThreadDesc(threadIds[0]);
            var copy1 = session.FetchThreadDesc(threadIds[0]);
            var copy2 = session.FetchThreadDesc(threadIds[0]);
            Check(view1.HasValue && view2.HasValue &&
                view1.Value.ThreadId == view2.Value.ThreadId,
                "disabled cache changed Get value");
            Check(copy1 != null && copy2 != null && !ReferenceEquals(copy1, copy2),
                "disabled cache reused Fetch object");
            Check(copy1!.ThreadId == copy2!.ThreadId,
                "disabled cache changed Fetch value");
            session.EnableObjectCache(true);
        }

        using (var timerArray1 = session.GetTimerIds())
        using (var timerArray2 = session.GetTimerIds())
        {
            Check(!ReferenceEquals(timerArray1, timerArray2),
                "array wrappers unexpectedly share identity");
            Check(timerArray1.Size() == timerArray2.Size(), "array wrapper sizes differ");
            if (timerArray1.Size() != 0)
                Check(timerArray1.Get(0) == timerArray2.Get(0),
                    "array wrapper values differ");
        }

        if (shortUpdatePath != null && longUpdatePath != null)
        {
            var provider = new MutableDataProvider(capture);
            using var related = libmp.OpenFromExternalProvider(provider)
                ?? throw new InvalidOperationException("related-snapshot session failed");
            uint id = related.FetchTimerIds()[0];
            var baselineGlobal = related.FetchGlobalDesc()!;
            var baselineGeneral = related.FetchGeneralInfo()!;
            var baselineTimer = related.FetchTimerDesc(id)!;

            provider.Data = File.ReadAllBytes(shortUpdatePath);
            related.SetSyncRequired();
            var shortGeneral = related.FetchGeneralInfo()!;
            var shortGlobal = related.FetchGlobalDesc()!;
            var timerAfterShort = related.FetchTimerDesc(id)!;
            Check(shortGeneral.PlatformInfoJson != baselineGeneral.PlatformInfoJson,
                "short-term fixture did not change its metadata");
            Check(!ReferenceEquals(shortGlobal, baselineGlobal),
                "short-term update did not invalidate the short-term cache bucket");
            Check(ReferenceEquals(timerAfterShort, baselineTimer),
                "short-term update invalidated the long-term cache bucket");

            provider.Data = File.ReadAllBytes(longUpdatePath);
            related.SetSyncRequired();
            var generalAfterLong = related.FetchGeneralInfo()!;
            var globalAfterLong = related.FetchGlobalDesc()!;
            var longTimer = related.FetchTimerDesc(id)!;
            Check(ReferenceEquals(generalAfterLong, shortGeneral) &&
                ReferenceEquals(globalAfterLong, shortGlobal),
                "long-term update invalidated the short-term cache bucket");
            Check(!ReferenceEquals(longTimer, timerAfterShort),
                "long-term update did not invalidate the long-term cache bucket");
            Check(longTimer.TimerName != timerAfterShort.TimerName,
                "long-term fixture did not change its metadata");
        }
    }
}

sealed class MutableDataProvider : IDataProvider
{
    public byte[] Data { get; set; }

    public MutableDataProvider(byte[] data) { Data = data; }
    public bool Open() => true;
    public bool Close() => true;
    public ulong GetSize() => (ulong)Data.LongLength;

    public int Read(Span<byte> destination, uint blockId, ulong offset, int size)
    {
        if (blockId != 0 || offset > (ulong)Data.Length || size < 0 ||
            (ulong)size > (ulong)Data.Length - offset)
            return 0;
        Data.AsSpan((int)offset, size).CopyTo(destination);
        return size;
    }
}

static class Program
{
    private static void Check(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static void CheckTrackAndLabelApis(LibMPInstance libmp, Session session)
    {
        Check((int)Enums.AppType.Unknown == 0 && (int)Enums.AppType.Server == 3,
            "application-type enum is unavailable");
        using (var tracks = session.GetTracks())
            Check((uint)tracks.Size() == session.GetTrackCount(), "track count mismatch");
        var fetchedTracks = session.FetchTracks();
        Check((uint)fetchedTracks.Length == session.GetTrackCount(), "fetched track count mismatch");
        if (fetchedTracks.Length != 0)
        {
            using (var segments = session.GetSegments(0))
                Check((uint)segments.Size() == session.GetSegmentCount(0), "segment count mismatch");
            Check((uint)session.FetchSegments(0).Length == session.GetSegmentCount(0),
                "fetched segment count mismatch");
            Check(session.SelectTrack(0), "failed to select first track");
            if (session.GetSegmentCount(0) != 0)
                Check(session.SelectSegment(0), "failed to select first segment");
        }

        session.EnableLabelCache(true);
        session.ClearLabelCache();
    }

    public static int Main(string[] args)
    {
        if (args.Length != 1 && args.Length != 3)
        {
            Console.Error.WriteLine(
                "usage: LibMPParity <capture.gprx> [<short-update.gprx> <long-update.gprx>]");
            return 2;
        }

        using var libmp = LibMPInstance.Create();
        byte[] capture = File.ReadAllBytes(args[0]);
        using var session = libmp.OpenFromMem(capture)
            ?? throw new InvalidOperationException($"failed to open {args[0]}");
        if (!session.IsValid())
            throw new InvalidOperationException($"invalid session for {args[0]}");
        CheckTrackAndLabelApis(libmp, session);
        Console.Write(new ParitySummary(libmp, session).Write());
        CacheChecks.Run(libmp, session, capture,
            args.Length == 3 ? args[1] : null,
            args.Length == 3 ? args[2] : null);
        return 0;
    }
}

#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.


import importlib.util
import struct
import sys
from pathlib import Path


lines = []


def boolean(value):
    return 1 if value else 0


def emit(*fields):
    lines.append(" ".join(str(field) for field in fields))


def hex_u32(value):
    return f"{int(value) & 0xffffffff:08x}"


def hex_u64(value):
    return f"{int(value) & 0xffffffffffffffff:016x}"


def float_bits(value):
    return hex_u32(struct.unpack("<I", struct.pack("<f", value))[0])


def double_bits(value):
    return hex_u64(struct.unpack("<Q", struct.pack("<d", value))[0])


def hex_bytes(value):
    if value is None:
        return "-"
    data = value if isinstance(value, bytes) else value.encode("utf-8")
    return f"{len(data)}:{data.hex()}"


def write_summary(libmp, session):
    frame_id_min = session.GetFrameIdMin()
    frame_id_max = session.GetFrameIdMax()
    emit("LIBMP_PARITY", 3)
    emit("SESSION", session.GetDataFormatVersion(), libmp.Versions.Library,
         frame_id_min, frame_id_max)

    global_desc = session.FetchGlobalDesc()
    if global_desc:
        emit("GLOBAL", 1, float_bits(global_desc.tickToMsCpu),
             float_bits(global_desc.tickToMsGpu))
    else:
        emit("GLOBAL", 0)
    general = session.FetchGeneralInfo()
    if general:
        emit("GENERAL", 1, hex_bytes(general.platformInfoJson))
    else:
        emit("GENERAL", 0)

    group_id_max = session.GetGroupIdMax()
    emit("GROUPS", group_id_max)
    for query_id in range(1, group_id_max + 1):
        group = session.FetchGroupDesc(query_id)
        if group:
            emit("GROUP", query_id, 1, group.groupId, hex_bytes(group.groupName),
                 hex_u32(group.color), boolean(group.isGpu))
        else:
            emit("GROUP", query_id, 0)

    timer_ids = session.FetchTimerIds()
    emit("TIMER_IDS", len(timer_ids))
    for index, timer_id in enumerate(timer_ids):
        emit("TIMER_ID", index, timer_id)
        timer = session.FetchTimerDesc(timer_id)
        if timer:
            emit("TIMER", timer_id, 1, timer.timerId, hex_bytes(timer.timerName),
                 timer.groupId, hex_u32(timer.color), boolean(timer.isUserTimer))
        else:
            emit("TIMER", timer_id, 0)
    for group_id in range(1, group_id_max + 1):
        ids = session.FetchTimerIdsByGroupId(group_id)
        emit("GROUP_TIMER_IDS", group_id, len(ids), *ids)

    counter_ids = session.FetchCounterIds()
    emit("COUNTER_IDS", len(counter_ids))
    for index, counter_id in enumerate(counter_ids):
        emit("COUNTER_ID", index, counter_id)
        counter = session.FetchCounterDesc(counter_id)
        if not counter:
            emit("COUNTER", counter_id, 0)
            continue
        emit("COUNTER", counter_id, 1, counter.counterId, hex_bytes(counter.counterName),
             counter.parent, counter.sibling, counter.firstChild, counter.level)
        samples = session.FetchCounterSamples(counter_id)
        emit("COUNTER_SAMPLES", counter_id, len(samples))
        for sample_index, sample in enumerate(samples):
            emit("COUNTER_SAMPLE", counter_id, sample_index, 1,
                 sample.frameId, double_bits(sample.value))
        last = session.FetchLastCounterSample(counter_id)
        if last:
            emit("COUNTER_LAST", counter_id, 1, last.frameId, double_bits(last.value))
        else:
            emit("COUNTER_LAST", counter_id, 0)

    emit("GPU_THREAD", session.GetThreadIdGpu())
    thread_ids = session.FetchThreadIds()
    emit("THREAD_IDS", len(thread_ids))
    for index, thread_id in enumerate(thread_ids):
        emit("THREAD_ID", index, thread_id)
        thread = session.FetchThreadDesc(thread_id)
        if thread:
            emit("THREAD", thread_id, 1, thread.threadId, hex_bytes(thread.threadName),
                 thread.bufferSize, boolean(thread.isGpu))
        else:
            emit("THREAD", thread_id, 0)

    frame_count = frame_id_max - frame_id_min + 1 if frame_id_max >= frame_id_min else 0
    emit("FRAMES", frame_count)
    for offset in range(frame_count):
        query_id = frame_id_min + offset
        frame = session.FetchFrameDesc(query_id)
        if not frame:
            emit("FRAME", query_id, 0)
            continue
        thread_logs = frame.threadLogs or []
        emit("FRAME", query_id, 1, frame.frameId, frame.frameAbsoluteId,
             hex_u64(frame.tickStartCpu), hex_u64(frame.tickEndCpu),
             hex_u64(frame.tickStartGpu), hex_u64(frame.tickEndGpu),
             hex_u64(frame.labelOffsetMin), hex_u64(frame.labelOffsetMax),
             boolean(frame.isPaused), boolean(frame.isIncomplete), len(thread_logs))
        for log_index, log in enumerate(thread_logs):
            emit("FRAME_THREAD_LOG", query_id, log_index, log.threadId, log.logEntriesNum,
                 hex_u64(log.logStartLocator), boolean(log.isRemoved))

    def write_u64_samples(name, samples):
        emit(f"{name}_SAMPLES", len(samples))
        for index, sample in enumerate(samples):
            emit(f"{name}_SAMPLE", index, 1, sample.frameId, hex_u64(sample.value))

    write_u64_samples("PLACE_ID", session.FetchPlaceIdSamples())
    write_u64_samples("UTC", session.FetchUtcTimestampSamples())

    probes = [
        ("*", False), ("*", True), ("*A*", False), ("*A*", True),
        ("__libmp_parity_missing__", False),
    ]
    emit("FIND_QUERIES", len(probes))

    def write_find_family(family, find_first, find_all):
        for probe_index, (mask, sensitive) in enumerate(probes):
            ids = find_all(mask, sensitive)
            emit("FIND", family, probe_index, boolean(sensitive), hex_bytes(mask),
                 find_first(mask, sensitive), len(ids), *ids)

    write_find_family("GROUP", session.FindGroupId, session.FindGroupIds)
    write_find_family("TIMER", session.FindTimerId, session.FindTimerIds)
    write_find_family("COUNTER", session.FindCounterId, session.FindCounterIds)
    write_find_family("THREAD", session.FindThreadId, session.FindThreadIds)

    emit("GROUP_NAME_TIMER_QUERIES", len(probes))
    for probe_index, (mask, sensitive) in enumerate(probes):
        ids = session.FetchTimerIdsByGroupNames(mask, sensitive)
        emit("GROUP_NAME_TIMERS", probe_index, boolean(sensitive), hex_bytes(mask),
             len(ids), *ids)

    emit("COUNTER_ITERATOR_BEGIN")
    with session.CreateCounterIterator() as iterator:
        if not iterator.Configure({"RootCounterIds": []}):
            raise RuntimeError("failed to configure counter iterator")
        state = iterator.GetState()
        report_index = 0
        while iterator.Step():
            emit("CI", report_index, boolean(state.Started()), boolean(state.Finished()),
                 state.CounterId(), state.ParentId(), state.Level(), state.RelativeLevel(),
                 state.RootLevel(), state.IndexInSiblings(), boolean(state.IsLeaf()),
                 boolean(state.HierarchyChanged()), boolean(state.CounterStackIsOverflowed()))
            report_index += 1
        emit("COUNTER_ITERATOR_END", report_index, boolean(state.Finished()))

    emit("LOG_ITERATOR_BEGIN")
    with session.CreateLogIterator() as iterator:
        if not iterator.Configure({"EmitThreadFrameEnd": True, "TrackScopeLabels": True}):
            raise RuntimeError("failed to configure log iterator")
        state = iterator.GetState()
        report_index = 0
        checked_label_apis = False
        while iterator.Step():
            if not checked_label_apis and not state.IsLabel():
                check(iterator.FetchCurrentLabel() is None, "unexpected label")
                check(iterator.GetCurrentScopeLabelCount() == 0, "unexpected scope label")
                check(iterator.GetScopeLabelCount(state.ThreadId(), 0) == 0,
                      "unexpected explicit scope label")
                check(iterator.FetchCurrentScopeLabel(0) is None,
                      "unexpected current scope label")
                check(iterator.FetchScopeLabel(state.ThreadId(), 0, 0) is None,
                      "unexpected explicit scope label")
                checked_label_apis = True
            emit("LI", report_index, boolean(state.Started()), boolean(state.Finished()),
                 state.ThreadStackDepth(), boolean(state.ThreadStackIsUnderflowed()),
                 boolean(state.ThreadStackIsOverflowed()),
                 boolean(state.ThreadStackWasOverflowed()),
                 hex_u64(state.ThreadPrevTimestamp()), state.FrameId(),
                 state.FrameAbsoluteId(), hex_u64(state.FrameDescLocator()), state.ThreadId(),
                 boolean(state.IsGpuThread()), boolean(state.IsFrameStitching()),
                 boolean(state.FrameChanged()), boolean(state.ThreadChanged()),
                 hex_u64(state.EntryLocator()), state.TimerId(), hex_u64(state.Timestamp()),
                 hex_u64(state.TimestampGpu()), state.EntryTypeRaw(), state.EventType(),
                 boolean(state.IsUnknown()), boolean(state.IsEnter()), boolean(state.IsExit()),
                 boolean(state.IsLabel()), boolean(state.IsEvent()),
                 boolean(state.IsThreadFrameEnd()), boolean(state.IsFrameBoundary()),
                 iterator.GetCurrentEntryCount())
            report_index += 1
        check(checked_label_apis, "log iterator has no non-label entry")
        emit("LOG_ITERATOR_END", report_index, boolean(state.Finished()))
    emit("END")


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_track_and_label_apis(libmp, session):
    check(libmp.Enums.AppType.Unknown == 0 and libmp.Enums.AppType.Server == 3,
          "application-type enum is unavailable")
    with session.GetTracks() as tracks:
        check(tracks.Size() == session.GetTrackCount(), "track count mismatch")
    fetched_tracks = session.FetchTracks()
    check(len(fetched_tracks) == session.GetTrackCount(), "fetched track count mismatch")
    if fetched_tracks:
        with session.GetSegments(0) as segments:
            check(segments.Size() == session.GetSegmentCount(0), "segment count mismatch")
        check(len(session.FetchSegments(0)) == session.GetSegmentCount(0),
              "fetched segment count mismatch")
        check(session.SelectTrack(0), "failed to select first track")
        if session.GetSegmentCount(0):
            check(session.SelectSegment(0), "failed to select first segment")

    session.EnableLabelCache(True)
    session.ClearLabelCache()


def check_cached_descriptor(session, label, first_id, second_id,
                            get, fetch, view_value, fetched_value):
    first_view = get(first_id)
    repeated_view = get(first_id)
    check(first_view is not None and first_view is repeated_view,
          f"{label}: Get identity miss")
    expected = view_value(first_view)
    if second_id is not None:
        get(second_id)
        check(get(first_id) is first_view, f"{label}: interleaved Get identity miss")

    first_copy = fetch(first_id)
    repeated_copy = fetch(first_id)
    check(first_copy is not None and first_copy is repeated_copy,
          f"{label}: Fetch identity miss")
    check(fetched_value(first_copy) == expected, f"{label}: Get/Fetch value mismatch")
    if second_id is not None:
        fetch(second_id)
        check(fetch(first_id) is first_copy, f"{label}: interleaved Fetch identity miss")

    session.ClearObjectCache()
    cleared_view = get(first_id)
    cleared_copy = fetch(first_id)
    check(cleared_view is not first_view, f"{label}: ClearObjectCache kept Get wrapper")
    check(cleared_copy is not first_copy, f"{label}: ClearObjectCache kept Fetch object")
    check(view_value(cleared_view) == expected and fetched_value(cleared_copy) == expected,
          f"{label}: ClearObjectCache changed value")


def check_cache_contract(libmp, session, capture, short_update_path, long_update_path):
    session.EnableObjectCache(True)
    timer_ids = session.FetchTimerIds()
    counter_ids = session.FetchCounterIds()
    thread_ids = session.FetchThreadIds()
    group_ids = [group_id for group_id in range(1, session.GetGroupIdMax() + 1)
                 if session.FetchGroupDesc(group_id) is not None]

    if session.FetchGlobalDesc() is not None:
        check_cached_descriptor(
            session, "GlobalDesc", None, None,
            lambda _id: session.GetGlobalDesc(), lambda _id: session.FetchGlobalDesc(),
            lambda value: float_bits(value.TickToMsCpu()),
            lambda value: float_bits(value.tickToMsCpu))
    if session.FetchGeneralInfo() is not None:
        check_cached_descriptor(
            session, "GeneralInfo", None, None,
            lambda _id: session.GetGeneralInfo(), lambda _id: session.FetchGeneralInfo(),
            lambda value: hex_bytes(value.PlatformInfoJson()),
            lambda value: hex_bytes(value.platformInfoJson))
    if group_ids:
        check_cached_descriptor(
            session, "GroupDesc", group_ids[0], group_ids[1] if len(group_ids) > 1 else None,
            session.GetGroupDesc, session.FetchGroupDesc,
            lambda value: (value.GroupId(), hex_bytes(value.GroupName())),
            lambda value: (value.groupId, hex_bytes(value.groupName)))
    if timer_ids:
        check_cached_descriptor(
            session, "TimerDesc", timer_ids[0], timer_ids[1] if len(timer_ids) > 1 else None,
            session.GetTimerDesc, session.FetchTimerDesc,
            lambda value: (value.TimerId(), hex_bytes(value.TimerName())),
            lambda value: (value.timerId, hex_bytes(value.timerName)))
    if counter_ids:
        check_cached_descriptor(
            session, "CounterDesc", counter_ids[0],
            counter_ids[1] if len(counter_ids) > 1 else None,
            session.GetCounterDesc, session.FetchCounterDesc,
            lambda value: (value.CounterId(), hex_bytes(value.CounterName())),
            lambda value: (value.counterId, hex_bytes(value.counterName)))
    if thread_ids:
        check_cached_descriptor(
            session, "ThreadDesc", thread_ids[0], thread_ids[1] if len(thread_ids) > 1 else None,
            session.GetThreadDesc, session.FetchThreadDesc,
            lambda value: (value.ThreadId(), hex_bytes(value.ThreadName())),
            lambda value: (value.threadId, hex_bytes(value.threadName)))

        session.EnableObjectCache(False)
        view1 = session.GetThreadDesc(thread_ids[0])
        view2 = session.GetThreadDesc(thread_ids[0])
        copy1 = session.FetchThreadDesc(thread_ids[0])
        copy2 = session.FetchThreadDesc(thread_ids[0])
        check(view1 is not None and view2 is not None and view1 is not view2,
              "disabled cache reused Get wrapper")
        check(copy1 is not None and copy2 is not None and copy1 is not copy2,
              "disabled cache reused Fetch object")
        check(view1.ThreadId() == view2.ThreadId() and copy1.threadId == copy2.threadId,
              "disabled cache changed descriptor value")
        session.EnableObjectCache(True)

    with session.GetTimerIds() as timer_array1, session.GetTimerIds() as timer_array2:
        check(timer_array1 is not timer_array2, "array wrappers unexpectedly share identity")
        check(timer_array1.Size() == timer_array2.Size(), "array wrapper sizes differ")
        if timer_array1.Size():
            check(timer_array1.Get(0) == timer_array2.Get(0),
                  "array wrapper values differ")

    if short_update_path is not None and long_update_path is not None:
        class MutableProvider:
            def __init__(self, data):
                self.data = data

            def get_size(self):
                return len(self.data)

            def read(self, destination, block_id, offset, size):
                if block_id != 0 or offset > len(self.data) or size > len(self.data) - offset:
                    return 0
                destination[:size] = self.data[offset:offset + size]
                return size

        provider = MutableProvider(capture)
        with libmp.Session.OpenFromExternalProvider(provider) as related:
            check(related.IsValid(), "related-snapshot session failed")
            timer_id = related.FetchTimerIds()[0]
            baseline_global = related.FetchGlobalDesc()
            baseline_general = related.FetchGeneralInfo()
            baseline_timer = related.FetchTimerDesc(timer_id)

            provider.data = short_update_path.read_bytes()
            related.SetSyncRequired()
            short_general = related.FetchGeneralInfo()
            short_global = related.FetchGlobalDesc()
            timer_after_short = related.FetchTimerDesc(timer_id)
            check(short_general.platformInfoJson != baseline_general.platformInfoJson,
                  "short-term fixture did not change its metadata")
            check(short_global is not baseline_global,
                  "short-term update did not invalidate the short-term cache bucket")
            check(timer_after_short is baseline_timer,
                  "short-term update invalidated the long-term cache bucket")

            provider.data = long_update_path.read_bytes()
            related.SetSyncRequired()
            general_after_long = related.FetchGeneralInfo()
            global_after_long = related.FetchGlobalDesc()
            long_timer = related.FetchTimerDesc(timer_id)
            check(general_after_long is short_general and global_after_long is short_global,
                  "long-term update invalidated the short-term cache bucket")
            check(long_timer is not timer_after_short,
                  "long-term update did not invalidate the long-term cache bucket")
            check(long_timer.timerName != timer_after_short.timerName,
                  "long-term fixture did not change its metadata")


def main():
    if len(sys.argv) not in (3, 5):
        raise SystemExit(
            "usage: parity_summary.py <LibMP.py> <capture.gprx> "
            "[<short-update.gprx> <long-update.gprx>]")
    spec = importlib.util.spec_from_file_location("LibMP", sys.argv[1])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    capture = Path(sys.argv[2]).read_bytes()
    with module.create() as libmp:
        with libmp.Session.OpenFromMem(capture) as session:
            if not session.IsValid():
                raise RuntimeError(f"failed to open {sys.argv[2]}")
            check_track_and_label_apis(libmp, session)
            write_summary(libmp, session)
            check_cache_contract(
                libmp, session, capture,
                Path(sys.argv[3]) if len(sys.argv) == 5 else None,
                Path(sys.argv[4]) if len(sys.argv) == 5 else None)
    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()

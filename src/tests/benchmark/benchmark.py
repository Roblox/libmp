#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.


import importlib.util
import sys
import time
from pathlib import Path


def mix(hash_value, value):
    hash_value = ((hash_value << 5) | (hash_value >> 27)) & 0xffffffff
    return ((hash_value ^ (int(value) & 0xffffffff)) + 0x9e3779b9) & 0xffffffff


def byte_length(value):
    if value is None:
        return 0
    return len(value if isinstance(value, bytes) else value.encode("utf-8"))


def workload(session, passes):
    hash_value = 0x811c9dc5
    item_count = 0
    for _ in range(passes):
        hash_value = mix(hash_value, session.GetFrameIdMin())
        hash_value = mix(hash_value, session.GetFrameIdMax())
        hash_value = mix(hash_value, 1 if session.FetchGlobalDesc() else 0)
        general = session.FetchGeneralInfo()
        hash_value = mix(hash_value, byte_length(general.platformInfoJson) if general else 0)
        item_count += 2

        group_id_max = session.GetGroupIdMax()
        hash_value = mix(hash_value, group_id_max)
        item_count += group_id_max
        for group_id in range(1, group_id_max + 1):
            value = session.FetchGroupDesc(group_id)
            if value is None:
                hash_value = mix(hash_value, 0)
                continue
            hash_value = mix(hash_value, value.groupId)
            hash_value = mix(hash_value, byte_length(value.groupName))
            hash_value = mix(hash_value, value.color)
            hash_value = mix(hash_value, value.isGpu)

        timer_ids = session.FetchTimerIds()
        hash_value = mix(hash_value, len(timer_ids))
        item_count += len(timer_ids)
        for timer_id in timer_ids:
            hash_value = mix(hash_value, timer_id)
            value = session.FetchTimerDesc(timer_id)
            if value is None:
                hash_value = mix(hash_value, 0)
                continue
            hash_value = mix(hash_value, value.timerId)
            hash_value = mix(hash_value, byte_length(value.timerName))
            hash_value = mix(hash_value, value.groupId)
            hash_value = mix(hash_value, value.color)
            hash_value = mix(hash_value, value.isUserTimer)

        counter_ids = session.FetchCounterIds()
        hash_value = mix(hash_value, len(counter_ids))
        item_count += len(counter_ids)
        for counter_id in counter_ids:
            hash_value = mix(hash_value, counter_id)
            value = session.FetchCounterDesc(counter_id)
            if value is None:
                hash_value = mix(hash_value, 0)
                continue
            hash_value = mix(hash_value, value.counterId)
            hash_value = mix(hash_value, byte_length(value.counterName))
            hash_value = mix(hash_value, value.parent)
            hash_value = mix(hash_value, value.sibling)
            hash_value = mix(hash_value, value.firstChild)
            hash_value = mix(hash_value, value.level)

        thread_ids = session.FetchThreadIds()
        hash_value = mix(hash_value, len(thread_ids))
        item_count += len(thread_ids)
        for thread_id in thread_ids:
            hash_value = mix(hash_value, thread_id)
            value = session.FetchThreadDesc(thread_id)
            if value is None:
                hash_value = mix(hash_value, 0)
                continue
            hash_value = mix(hash_value, value.threadId)
            hash_value = mix(hash_value, byte_length(value.threadName))
            hash_value = mix(hash_value, value.bufferSize)
            hash_value = mix(hash_value, value.isGpu)
    return hash_value, item_count


def log_workload(session, passes):
    hash_value = 0x811c9dc5
    item_count = 0
    for _ in range(passes):
        with session.CreateLogIterator() as iterator:
            if not iterator.Configure({"EmitThreadFrameEnd": True}):
                raise RuntimeError("failed to configure log iterator")
            state = iterator.GetState()
            if state is None:
                raise RuntimeError("log iterator has no state")
            entry_count = 0
            while iterator.Step():
                hash_value = mix(hash_value, state.FrameId())
                hash_value = mix(hash_value, state.ThreadId())
                hash_value = mix(hash_value, state.TimerId())
                hash_value = mix(hash_value, state.EntryTypeRaw())
                hash_value = mix(hash_value, state.EventType())
                hash_value = mix(hash_value, state.IsEvent())
                timer = session.FetchTimerDesc(state.TimerId())
                if timer is not None:
                    hash_value = mix(hash_value, timer.timerId)
                    hash_value = mix(hash_value, byte_length(timer.timerName))
                    hash_value = mix(hash_value, timer.color)
                else:
                    hash_value = mix(hash_value, 0)
                entry_count = (entry_count + 1) & 0xffffffff
            hash_value = mix(hash_value, entry_count)
            item_count += entry_count
    return hash_value, item_count


def open_session(libmp, capture, cache):
    session = libmp.Session.OpenFromMem(capture)
    if session is None or not session.IsValid():
        raise RuntimeError("failed to open capture")
    session.EnableObjectCache(cache)
    return session


def emit(scenario, cache, result, samples):
    checksum, item_count = result
    print(f"BENCH\t{scenario}\t{'on' if cache else 'off'}\t{checksum}\t{item_count}\t" +
          ",".join(str(value) for value in samples))


def run_shared(libmp, capture, cache, warmups, iterations, passes):
    session = open_session(libmp, capture, cache)
    try:
        for _ in range(warmups):
            workload(session, passes)
        samples = []
        result = None
        for _ in range(iterations):
            start = time.perf_counter_ns()
            current = workload(session, passes)
            samples.append(time.perf_counter_ns() - start)
            if result is not None and current != result:
                raise RuntimeError("workload checksum changed")
            result = current
        emit("shared", cache, result, samples)
    finally:
        session.Dispose()


def run_fresh(libmp, capture, cache, warmups, iterations, passes):
    def run():
        session = open_session(libmp, capture, cache)
        try:
            return workload(session, passes)
        finally:
            session.Dispose()

    for _ in range(warmups):
        run()
    samples = []
    result = None
    for _ in range(iterations):
        start = time.perf_counter_ns()
        current = run()
        samples.append(time.perf_counter_ns() - start)
        if result is not None and current != result:
            raise RuntimeError("workload checksum changed")
        result = current
    emit("fresh", cache, result, samples)


def run_logs(libmp, capture, cache, warmups, iterations, passes):
    session = open_session(libmp, capture, cache)
    try:
        for _ in range(warmups):
            log_workload(session, passes)
        samples = []
        result = None
        for _ in range(iterations):
            start = time.perf_counter_ns()
            current = log_workload(session, passes)
            samples.append(time.perf_counter_ns() - start)
            if result is not None and current != result:
                raise RuntimeError("log workload checksum changed")
            result = current
        emit("logs", cache, result, samples)
    finally:
        session.Dispose()


def main():
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: benchmark.py <LibMP.py> <capture> <warmups> <iterations> "
            "<metadata-passes> <log-warmups> <log-iterations> <log-passes>")
    spec = importlib.util.spec_from_file_location("LibMP", sys.argv[1])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    capture = Path(sys.argv[2]).read_bytes()
    (warmups, iterations, metadata_passes,
     log_warmups, log_iterations, log_passes) = map(int, sys.argv[3:])
    with module.create() as libmp:
        for cache in (False, True):
            print(f"  cache={'on' if cache else 'off'}: metadata / reused session",
                  file=sys.stderr, flush=True)
            run_shared(libmp, capture, cache, warmups, iterations, metadata_passes)
            print(f"  cache={'on' if cache else 'off'}: metadata / new session",
                  file=sys.stderr, flush=True)
            run_fresh(libmp, capture, cache, warmups, iterations, metadata_passes)
            print(f"  cache={'on' if cache else 'off'}: full log traversal / reused session",
                  file=sys.stderr, flush=True)
            run_logs(libmp, capture, cache, log_warmups, log_iterations, log_passes)


if __name__ == "__main__":
    main()

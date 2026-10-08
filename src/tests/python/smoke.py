#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.


import gc
import importlib.util
import sys
import threading
import weakref
from pathlib import Path


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_fast_call_errors(module):
    class ExpectedCallbackError(Exception):
        pass

    engine = module.wasmtime.Engine()
    store = module.wasmtime.Store(engine)
    wasm_module = module.wasmtime.Module(engine, module.wasmtime.wat2wasm("""
        (module
          (import "env" "fail" (func $fail))
          (func (export "CallHost") call $fail)
          (func (export "Trap") unreachable))
    """))

    def fail():
        raise ExpectedCallbackError("expected host callback failure")

    host = module.wasmtime.Func(
        store, module.wasmtime.FuncType([], []), fail)
    instance = module.wasmtime.Instance(store, wasm_module, [host])
    exports = instance.exports(store)
    fast_calls = module._FastWasmtimeCalls.create(
        store, {name: exports[name] for name in exports})
    check(fast_calls is not None, "fast Wasmtime calls are unavailable")

    try:
        fast_calls.call("CallHost", ())
    except ExpectedCallbackError:
        pass
    else:
        raise RuntimeError("fast call did not propagate a host callback exception")

    try:
        fast_calls.call("Trap", ())
    except module.wasmtime.Trap:
        pass
    else:
        raise RuntimeError("fast call did not propagate a WASM trap")


def main():
    if len(sys.argv) not in (3, 5):
        raise SystemExit(
            "usage: smoke.py <LibMP.py> <capture.gprx> "
            "[<short-update.gprx> <long-update.gprx>]")

    spec = importlib.util.spec_from_file_location("LibMP", sys.argv[1])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    check(module.Types.FrameDesc is not None, "generated FlatBuffers types are missing")
    check(module.FlatBuffers.Builder is not None, "FlatBuffers runtime is missing")
    check_fast_call_errors(module)
    with module.create() as libmp:
        check(libmp._fast_calls is not None, "fast Wasmtime calls are unavailable")
        check(libmp.Versions.Library != 0, "library version is missing")
        check(isinstance(libmp.GetMemReserved(), int), "reserved memory value is missing")
        fast_calls = libmp._fast_calls
        libmp._fast_calls = None
        try:
            check(isinstance(libmp.GetMemUsed(), int),
                  "public Wasmtime fallback failed")
        finally:
            libmp._fast_calls = fast_calls
        check(not libmp.Control.EnableProfiler(False),
              "default control channel must be a no-op")
        check(not libmp.Control.SetMemTargetMB(64),
              "default memory-target control must be a no-op")
        check(module.Enums.DataSlot.Live == 0 and module.Enums.DataSlot.Temporary == 5,
              "data-slot enum is unavailable")
        check(module.Enums.ContentMode.Full == 0 and module.Enums.ContentMode.CountersOnly == 1,
              "content-mode enum is unavailable")
        check(libmp.Control.CaptureToBufferSync() is None and
              not libmp.Control.CaptureToSlotSync(
                  module.Enums.DataSlot.FirstUserManaged) and
              libmp.Control.SaveSlotToBuffer(
                  module.Enums.DataSlot.FirstUserManaged) is None and
              not libmp.Control.ResetSlot(module.Enums.DataSlot.FirstUserManaged),
              "standalone slot controls must be no-op stubs")
        check(libmp.Session.OpenFromSlot(module.Enums.DataSlot.Live, 4096) is None,
              "standalone slot session must be a no-op stub")
        check(not libmp.Experimental.V1.StartFileStream() and
              not libmp.Experimental.V1.StartMemoryStream(1) and
              not libmp.Experimental.V1.StopStream() and
              not libmp.Experimental.V1.SetStreamMarker(1, True) and
              not libmp.Experimental.V1.SetContentMode(module.Enums.ContentMode.CountersOnly),
              "standalone experimental controls must be no-op stubs")
        stream_status = libmp.Experimental.V1.GetStreamStatus()
        check(not stream_status.active and stream_status.recordedSizeKb == 0 and
              stream_status.maxSizeKb == 0 and stream_status.elapsedMs == 0,
              "standalone stream status must be empty")
        check(libmp.Tools.SetHtmlAssetsBaseUrl(""), "HTML asset-base setter failed")

        capture = Path(sys.argv[2]).read_bytes()
        with libmp.Session.OpenFromMem(capture) as session:
            check(session.IsValid(), "OpenFromMem failed")
            check(session.GetFrameIdMax() >= session.GetFrameIdMin(), "invalid frame bounds")
            check(session.FetchTimerIds(), "timer array is empty")
            check(session.FetchCounterIds(), "counter array is empty")
            check(session.FetchThreadIds(), "thread array is empty")

            global_view = session.GetGlobalDesc()
            check(global_view is not None, "global descriptor is missing")
            expected_tick = global_view.TickToMsCpu()
            raw_global_view = global_view._raw_view
            memory_before_growth = libmp._memory_bytes()
            generation_before_growth = libmp._memory_generation
            check(global_view.TickToMsCpu() == expected_tick and
                  libmp._memory_bytes() is memory_before_growth,
                  "FlatBuffers field access recreated the WASM memory view")
            frame_view = session.GetFrameDesc(session.GetFrameIdMin())
            check(frame_view is not None, "frame descriptor is missing")
            thread_log = frame_view.ThreadLogs(0) if frame_view.ThreadLogsLength() else None
            expected_thread_id = thread_log.ThreadId() if thread_log else None
            allocation = libmp._call("malloc", 16 * 1024 * 1024)
            try:
                check(allocation != 0, "growth allocation failed")
                check(libmp._memory_generation > generation_before_growth,
                      "growth callback did not refresh WASM memory")
                check(libmp._memory_bytes() is not memory_before_growth,
                      "WASM memory view was not replaced after growth")
                check(global_view.TickToMsCpu() == expected_tick,
                      "FlatBuffers view did not follow grown WASM memory")
                check(global_view._raw_view is raw_global_view,
                      "FlatBuffers access recreated its generated view")
                if thread_log:
                    check(thread_log.ThreadId() == expected_thread_id,
                          "nested FlatBuffers view did not follow grown WASM memory")
            finally:
                libmp._call("free", allocation)

            check(session.GetGlobalDesc() is global_view,
                  "object cache did not reuse a view")
            session.ClearObjectCache()
            check(session.GetGlobalDesc() is not global_view,
                  "object cache did not clear a view")

            timer_id = session.FetchTimerIds()[0]
            short_term_cached = session.FetchGlobalDesc()
            long_term_cached = session.FetchTimerDesc(timer_id)
            check(session.FetchGlobalDesc() is short_term_cached,
                  "short-term object cache did not reuse a fetched object")
            check(session.FetchTimerDesc(timer_id) is long_term_cached,
                  "long-term object cache did not reuse a fetched object")
            session.SetClearCacheOnSyncRequired()
            session.SetSyncRequired()
            check(session.FetchGlobalDesc() is not short_term_cached,
                  "sync callback did not clear the short-term wrapper cache bucket")
            check(session.FetchTimerDesc(timer_id) is not long_term_cached,
                  "sync callback did not clear the long-term wrapper cache bucket")

            lock_cached_view = session.GetGlobalDesc()
            lock_cached_object = session.FetchTimerDesc(timer_id)

            def inner_lock() -> bool:
                check(session.FetchTimerDesc(timer_id) is lock_cached_object,
                      "nested locked cache hit did not preserve fetched-object identity")
                return True

            def outer_lock() -> bool:
                check(session.GetGlobalDesc() is lock_cached_view,
                      "locked cache hit did not preserve view identity")
                check(session.WithDataLock(inner_lock) is True, "nested data lock failed")
                return True

            check(session.WithDataLock(outer_lock) is True, "outer data lock failed")

            with session.CreateCounterIterator() as iterator:
                check(iterator.Configure({"RootCounterIds": []}),
                      "counter configuration failed")
                check(iterator.Step(), "counter iterator is empty")
                check(iterator.GetState() is not None,
                      "counter iterator state is missing")

            with session.CreateLogIterator() as iterator:
                check(iterator.Configure({"StartFrameId": session.GetFrameIdMin()}),
                      "log configuration failed")
                entry_count = 0
                while iterator.Step() and entry_count == 0:
                    check(iterator.GetState() is not None,
                          "log iterator state is missing")
                    entry_count = iterator.GetCurrentEntryCount()
                check(entry_count != 0, "log iterator has no physical entry")
                for index in range(entry_count):
                    check(isinstance(iterator.GetCurrentEntryPart(index, 0), int) and
                          isinstance(iterator.GetCurrentEntryPart(index, 1), int),
                          "log entry parts are unavailable")

            raw_conversion = None
            for format in module.Enums.ContainerFormat:
                converted = libmp.Tools.ConvertToBuffer(session, format)
                check(converted, "buffer conversion failed")
                with libmp.Session.OpenFromMem(converted) as converted_session:
                    check(converted_session.IsValid(), "converted buffer is invalid")
                    check(converted_session.GetFrameIdMax() == session.GetFrameIdMax(),
                          "converted buffer changed frame bounds")
                if format == module.Enums.ContainerFormat.Raw:
                    raw_conversion = converted

            converted_path = Path(sys.argv[2]).with_name("python-converted.gprx")
            try:
                check(libmp.Tools.ConvertToFile(
                    session, module.Enums.ContainerFormat.Raw, converted_path),
                    "file conversion failed")
                check(converted_path.read_bytes() == raw_conversion,
                      "file and buffer conversion differ")
            finally:
                converted_path.unlink(missing_ok=True)
            check(session.IsValid(), "conversion invalidated its source session")

        with libmp.Session.OpenFromBuffer(capture, 64 * 1024) as session:
            check(session.IsValid(), "OpenFromBuffer failed")
        with libmp.Session.OpenFromFile(sys.argv[2]) as session:
            check(session.IsValid(), "OpenFromFile failed")

        # Closing a session detaches its iterators: they stop stepping but stay disposable
        session = libmp.Session.OpenFromMem(capture)
        counter_iterator = session.CreateCounterIterator()
        log_iterator = session.CreateLogIterator()
        check(counter_iterator.Configure({"RootCounterIds": []}) and counter_iterator.Step(),
              "counter iterator did not step")
        check(log_iterator.Configure({"StartFrameId": session.GetFrameIdMin()}) and
              log_iterator.Step(), "log iterator did not step")
        session.Dispose()
        for iterator in (counter_iterator, log_iterator):
            check(not iterator.Step(), "detached iterator kept stepping")
            check(iterator.GetState() is not None, "detached iterator lost its state")
            check(not iterator.Configure(), "detached iterator accepted a configuration")
            iterator.Rewind()
            iterator.Dispose()

        # An iterator keeps its session alive while the iterator is reachable
        for create, config in (("CreateCounterIterator", {"RootCounterIds": []}),
                               ("CreateLogIterator", None)):
            session = libmp.Session.OpenFromMem(capture)
            iterator = getattr(session, create)()
            del session
            gc.collect()
            check(iterator.Configure(config) and iterator.Step(),
                  f"{create}: the iterator lost its session")
            iterator.Dispose()

        # Arrays read nothing once they or their session are disposed
        session = libmp.Session.OpenFromMem(capture)
        counter_id = session.FetchCounterIds()[0]
        samples = session.GetCounterSamples(counter_id)
        released = session.GetCounterSamples(counter_id)
        check(samples.Fetch(0) is not None and released.Fetch(0) is not None,
              "counter samples are empty")
        released.Dispose()
        check(released.Fetch(0) is None, "a disposed array read its elements")
        session.Dispose()
        check(samples.Fetch(0) is None and samples.FetchLastSample() is None,
              "an array read its closed session")
        samples.Dispose()

        # An array keeps its session alive while the array is reachable
        session = libmp.Session.OpenFromMem(capture)
        samples = session.GetCounterSamples(counter_id)
        del session
        gc.collect()
        check(samples.Fetch(0) is not None, "the array lost its session")
        samples.Dispose()

        # Finalizers leave the objects that were not disposed to the next top-level call.
        # They make no LibMP calls themselves: they can run inside a LibMP callback or on
        # another thread.
        class ClosingProvider:
            def __init__(self, data):
                self.data = data
                self.collect = False
                self.reading = False
                self.closed = False

            def get_size(self):
                return len(self.data)

            def read(self, destination, block_id, offset, size):
                if self.collect:
                    self.reading = True
                    try:
                        gc.collect()
                    finally:
                        self.reading = False
                destination[:size] = self.data[offset:offset + size]
                return size

            def close(self):
                self.closed = True
                return True

        def step_all(session):
            with session.CreateLogIterator() as iterator:
                check(iterator.Configure({"StartFrameId": session.GetFrameIdMin()}),
                      "log configuration failed")
                while iterator.Step():
                    pass

        def create_garbage(session):
            for _ in range(16):
                iterator = session.CreateLogIterator()
                check(iterator.Configure({"StartFrameId": session.GetFrameIdMin()}) and
                      iterator.Step(), "log iterator did not step")
                cycle = [iterator, session.CreateCounterIterator(), session.GetTimerIds()]
                cycle.append(cycle)  # only the cyclic collector frees it, at any allocation

        releases = []
        call = libmp._call

        def record_releases(name, *args):
            if name in ("MpDeleteLogIterator", "MpDeleteCounterIterator",
                        "MpReleaseArray", "MpCloseSession", "free"):
                releases.append((name, threading.get_ident(), provider.reading))
            return call(name, *args)

        gc.collect()
        mem_used = libmp.GetMemUsed()
        session = libmp.Session.OpenFromMem(capture)
        create_garbage(session)  # grows the session's iterator lists first
        gc.collect()
        session_mem_used = libmp.GetMemUsed()
        provider = ClosingProvider(capture)
        libmp._call = record_releases
        try:
            create_garbage(session)
            collector = threading.Thread(target=gc.collect)
            collector.start()
            collector.join()
            check(libmp.GetMemUsed() == session_mem_used,
                  "finalizers did not release unreachable objects")
            external_session = libmp.Session.OpenFromExternalProvider(provider, 64 * 1024)
            provider.collect = True  # its reads collect garbage in the middle of LibMP calls
            create_garbage(session)
            create_garbage(external_session)
            step_all(external_session)
            provider.collect = False
            check(all(thread == threading.get_ident() and not reading
                      for _, thread, reading in releases),
                  "finalizers called LibMP from a callback or another thread")
            check(("MpDeleteLogIterator", threading.get_ident(), False) in releases,
                  "finalizers released nothing")
            cycle = [session, external_session]
            cycle.append(cycle)
            del session, external_session, cycle
            gc.collect()
            check(libmp.GetMemUsed() == mem_used and provider.closed,
                  "finalizers did not release unreachable sessions")
        finally:
            libmp._call = call

        # A callback cannot dispose the instance that is running it
        errors = []
        disposed_instance = module.create(on_error=lambda error, _: errors.append(type(error)))

        def dispose_instance(*_):
            disposed_instance.Dispose()

        provider = ClosingProvider(capture)
        provider.read = dispose_instance
        session = disposed_instance.Session.OpenFromExternalProvider(provider)
        if session is not None:
            session.Dispose()
        check(errors == [RuntimeError], "a callback disposed the instance running it")

        # Disposing an instance releases the sessions its finalizers left to it, and its
        # views read nothing afterwards
        session = disposed_instance.Session.OpenFromMem(capture)
        view = session.GetGlobalDesc()
        provider = ClosingProvider(capture)
        cycle = [disposed_instance.Session.OpenFromExternalProvider(provider)]
        cycle.append(cycle)
        del cycle
        gc.collect()
        check(not provider.closed, "a finalizer closed a session without a LibMP call")
        disposed_instance.Dispose()
        check(provider.closed, "disposing an instance left a finalized session open")
        try:
            view.TickToMsCpu()
            read_freed_memory = True
        except RuntimeError:
            read_freed_memory = False
        check(not read_freed_memory, "a view read the memory of a disposed instance")

        # Then the instance and its WASM memory are freed
        instance_ref = weakref.ref(disposed_instance)
        del disposed_instance, session, view
        gc.collect()
        check(instance_ref() is None, "a disposed instance stayed in memory")

        if len(sys.argv) == 5:
            class MutableProvider:
                def __init__(self, data):
                    self.data = data

                def get_size(self):
                    return len(self.data)

                def read(self, destination, block_id, offset, size):
                    if block_id != 0 or offset > len(self.data) or \
                            size > len(self.data) - offset:
                        return 0
                    destination[:size] = self.data[offset:offset + size]
                    return size

            provider = MutableProvider(Path(sys.argv[2]).read_bytes())
            with libmp.Session.OpenFromExternalProvider(provider) as session:
                check(session.IsValid(), "related-snapshot session failed")
                timer_id = session.FetchTimerIds()[0]
                baseline_global = session.FetchGlobalDesc()
                baseline_general = session.FetchGeneralInfo()
                baseline_timer = session.FetchTimerDesc(timer_id)

                provider.data = Path(sys.argv[3]).read_bytes()
                session.SetSyncRequired()
                short_general = session.FetchGeneralInfo()
                short_global = session.FetchGlobalDesc()
                timer_after_short_update = session.FetchTimerDesc(timer_id)
                check(short_general.platformInfoJson != baseline_general.platformInfoJson,
                      "short-term fixture did not change its metadata")
                check(short_global is not baseline_global,
                      "short-term update did not invalidate the short-term cache bucket")
                check(timer_after_short_update is baseline_timer,
                      "short-term update invalidated the long-term cache bucket")

                provider.data = Path(sys.argv[4]).read_bytes()
                session.SetSyncRequired()
                general_after_long_update = session.FetchGeneralInfo()
                global_after_long_update = session.FetchGlobalDesc()
                long_timer = session.FetchTimerDesc(timer_id)
                check(general_after_long_update is short_general and
                      global_after_long_update is short_global,
                      "long-term update invalidated the short-term cache bucket")
                check(long_timer is not timer_after_short_update,
                      "long-term update did not invalidate the long-term cache bucket")
                check(long_timer.timerName != timer_after_short_update.timerName,
                      "long-term fixture did not change its metadata")

        with module.create() as other_a, module.create() as other_b:
            with other_a.Session.OpenFromBuffer(capture) as session_a, \
                    other_b.Session.OpenFromBuffer(capture) as session_b:
                check(session_a.IsValid() and session_b.IsValid(),
                      "multiple LibMP instances interfered with external IO routing")

    print("LibMP Python smoke test passed")


if __name__ == "__main__":
    main()

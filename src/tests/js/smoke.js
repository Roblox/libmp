// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"use strict";

const fs = require("node:fs");
const vm = require("node:vm");

function check(condition, message) {
    if (!condition)
        throw new Error(message);
}

async function main() {
    if (process.argv.length !== 4 && process.argv.length !== 6) {
        console.error(
            "usage: node smoke.js <LibMP.js> <capture.gprx> " +
            "[<short-update.gprx> <long-update.gprx>]");
        process.exit(2);
    }

    const LibMP = require(process.argv[2]);
    const capture = fs.readFileSync(process.argv[3]);
    const libmp = await LibMP.create();

    check(typeof LibMP.Types.FrameDesc === "function", "generated FlatBuffers types are missing");
    check(typeof LibMP.FlatBuffers.ByteBuffer === "function", "FlatBuffers runtime is missing");
    check(libmp.Versions.Library !== 0, "library version is missing");
    check(Number.isInteger(libmp.GetMemReserved()), "reserved memory value is missing");
    check(libmp.Control.EnableProfiler(false) === false,
        "default control channel must be a no-op");
    check(libmp.Control.SetMemTargetMB(64) === false,
        "default memory-target control must be a no-op");
    check(LibMP.Enums.DataSlot.Live === 0 && LibMP.Enums.DataSlot.Temporary === 5,
        "data-slot enum is unavailable");
    check(LibMP.Enums.ContentMode.Full === 0 && LibMP.Enums.ContentMode.CountersOnly === 1,
        "content-mode enum is unavailable");
    check(libmp.Control.CaptureToBufferSync() === null &&
        libmp.Control.CaptureToSlotSync(LibMP.Enums.DataSlot.FirstUserManaged) === false &&
        libmp.Control.SaveSlotToBuffer(LibMP.Enums.DataSlot.FirstUserManaged) === null &&
        libmp.Control.ResetSlot(LibMP.Enums.DataSlot.FirstUserManaged) === false,
        "standalone slot controls must be no-op stubs");
    check(libmp.Session.OpenFromSlot(LibMP.Enums.DataSlot.Live, 4096) === null,
        "standalone slot session must be a no-op stub");
    check(libmp.Experimental.V1.StartFileStream() === false &&
        libmp.Experimental.V1.StartMemoryStream(1) === false &&
        libmp.Experimental.V1.StopStream() === false &&
        libmp.Experimental.V1.SetStreamMarker(1, true) === false &&
        libmp.Experimental.V1.SetContentMode(LibMP.Enums.ContentMode.CountersOnly) === false,
        "standalone experimental controls must be no-op stubs");
    const streamStatus = libmp.Experimental.V1.GetStreamStatus();
    check(!streamStatus.active && streamStatus.recordedSizeKb === 0 &&
        streamStatus.maxSizeKb === 0 && streamStatus.elapsedMs === 0,
        "standalone stream status must be empty");
    check(libmp.Tools.SetHtmlAssetsBaseUrl(""), "HTML asset-base setter failed");

    const session = libmp.Session.OpenFromMem(capture);
    check(session?.IsValid(), "OpenFromMem failed");
    check(session.GetFrameIdMax() >= session.GetFrameIdMin(), "invalid frame bounds");
    check(session.FetchTimerIds().length !== 0, "timer array is empty");
    check(session.FetchCounterIds().length !== 0, "counter array is empty");
    check(session.FetchThreadIds().length !== 0, "thread array is empty");

    const globalView = session.GetGlobalDesc();
    check(globalView !== null, "global descriptor is missing");
    const expectedTick = globalView.tickToMsCpu();
    const oldMemory = globalView.bb.bytes().buffer;
    const allocation = libmp._exports.malloc(16 * 1024 * 1024);
    check(globalView.bb.bytes().buffer !== oldMemory, "test allocation did not grow WASM memory");
    check(globalView.tickToMsCpu() === expectedTick,
        "FlatBuffers view did not follow grown WASM memory");
    libmp._exports.free(allocation);

    const cached = session.GetGlobalDesc();
    check(cached === globalView, "object cache did not reuse a view");
    session.ClearObjectCache();
    check(session.GetGlobalDesc() !== cached, "object cache did not clear a view");

    const timerId = session.FetchTimerIds()[0];
    const shortTermCached = session.FetchGlobalDesc();
    const longTermCached = session.FetchTimerDesc(timerId);
    check(session.FetchGlobalDesc() === shortTermCached,
        "short-term object cache did not reuse a fetched object");
    check(session.FetchTimerDesc(timerId) === longTermCached,
        "long-term object cache did not reuse a fetched object");
    session.SetClearCacheOnSyncRequired();
    session.SetSyncRequired();
    check(session.FetchGlobalDesc() !== shortTermCached,
        "sync callback did not clear the short-term wrapper cache bucket");
    check(session.FetchTimerDesc(timerId) !== longTermCached,
        "sync callback did not clear the long-term wrapper cache bucket");

    const lockCachedView = session.GetGlobalDesc();
    const lockCachedObject = session.FetchTimerDesc(timerId);
    const outerLock = session.WithDataLock(() => {
        check(session.GetGlobalDesc() === lockCachedView,
            "locked cache hit did not preserve view identity");
        const innerLock = session.WithDataLock(() => {
            check(session.FetchTimerDesc(timerId) === lockCachedObject,
                "nested locked cache hit did not preserve fetched-object identity");
        });
        check(innerLock.locked, "nested data lock failed");
    });
    check(outerLock.locked, "outer data lock failed");

    const counterIterator = session.CreateCounterIterator();
    check(counterIterator.Configure({ RootCounterIds: [] }), "counter configuration failed");
    check(counterIterator.Step(), "counter iterator is empty");
    check(counterIterator.GetState() !== null, "counter iterator state is missing");
    counterIterator.Dispose();

    const logIterator = session.CreateLogIterator();
    check(logIterator.Configure({ StartFrameId: session.GetFrameIdMin() }),
        "log configuration failed");
    let entryCount = 0;
    while (logIterator.Step() && entryCount === 0) {
        check(logIterator.GetState() !== null, "log iterator state is missing");
        entryCount = logIterator.GetCurrentEntryCount();
    }
    check(entryCount !== 0, "log iterator has no physical entry");
    for (let index = 0; index < entryCount; ++index) {
        check(Number.isInteger(logIterator.GetCurrentEntryPart(index, 0)) &&
            Number.isInteger(logIterator.GetCurrentEntryPart(index, 1)),
            "log entry parts are unavailable");
    }
    logIterator.Dispose();

    let rawConversion;
    for (const format of Object.values(LibMP.Enums.ContainerFormat)) {
        const converted = libmp.Tools.ConvertToBuffer(session, format);
        check(converted instanceof Uint8Array && converted.byteLength !== 0,
            "buffer conversion failed");
        const convertedSession = libmp.Session.OpenFromMem(converted);
        check(convertedSession?.IsValid(), "converted buffer is invalid");
        check(convertedSession.GetFrameIdMax() === session.GetFrameIdMax(),
            "converted buffer changed frame bounds");
        convertedSession.Dispose();
        if (format === LibMP.Enums.ContainerFormat.Raw)
            rawConversion = converted;
    }
    const convertedPath = process.argv[3] + ".js-converted.gprx";
    try {
        check(await libmp.Tools.ConvertToFile(
            session, LibMP.Enums.ContainerFormat.Raw, convertedPath),
        "file conversion failed");
        check(Buffer.compare(fs.readFileSync(convertedPath), rawConversion) === 0,
            "file and buffer conversion differ");
    } finally {
        fs.rmSync(convertedPath, { force: true });
    }
    check(session.IsValid(), "conversion invalidated its source session");
    session.Dispose();

    const externalSession = libmp.Session.OpenFromBuffer(capture, 64 * 1024);
    check(externalSession?.IsValid(), "OpenFromBuffer failed");
    externalSession.Dispose();

    const fileSession = await libmp.Session.OpenFromFile(process.argv[3]);
    check(fileSession?.IsValid(), "OpenFromFile failed");
    fileSession.Dispose();

    // Closing a session detaches its iterators: they stop stepping but stay disposable
    const closedSession = libmp.Session.OpenFromMem(capture);
    const detachedCounters = closedSession.CreateCounterIterator();
    const detachedLog = closedSession.CreateLogIterator();
    check(detachedCounters.Configure({ RootCounterIds: [] }) && detachedCounters.Step(),
        "counter iterator did not step");
    check(detachedLog.Configure({ StartFrameId: closedSession.GetFrameIdMin() }) &&
        detachedLog.Step(), "log iterator did not step");
    closedSession.Dispose();
    for (const iterator of [detachedCounters, detachedLog]) {
        check(!iterator.Step(), "detached iterator kept stepping");
        check(iterator.GetState() !== null, "detached iterator lost its state");
        check(!iterator.Configure(), "detached iterator accepted a configuration");
        iterator.Rewind();
        iterator.Dispose();
    }

    // Arrays read nothing once they or their session are disposed
    const arraySession = libmp.Session.OpenFromMem(capture);
    const counterId = arraySession.FetchCounterIds()[0];
    const samples = arraySession.GetCounterSamples(counterId);
    const released = arraySession.GetCounterSamples(counterId);
    check(samples.Fetch(0) !== null && released.Fetch(0) !== null, "counter samples are empty");
    released.Dispose();
    check(released.Fetch(0) === null, "a disposed array read its elements");
    arraySession.Dispose();
    check(samples.Fetch(0) === null && samples.FetchLastSample() === null,
        "an array read its closed session");
    samples.Dispose();

    if (process.argv.length === 6) {
        const provider = {
            data: fs.readFileSync(process.argv[3]),
            getSize() { return this.data.byteLength; },
            read(destination, blockId, offset, size) {
                const start = Number(offset);
                if (blockId !== 0 || !Number.isSafeInteger(start) ||
                    start > this.data.byteLength || size > this.data.byteLength - start)
                    return 0;
                destination.set(this.data.subarray(start, start + size));
                return size;
            },
        };
        const relatedSession = libmp.Session.OpenFromExternalProvider(provider);
        check(relatedSession?.IsValid(), "related-snapshot session failed");
        const relatedTimerId = relatedSession.FetchTimerIds()[0];
        const baselineGlobal = relatedSession.FetchGlobalDesc();
        const baselineGeneral = relatedSession.FetchGeneralInfo();
        const baselineTimer = relatedSession.FetchTimerDesc(relatedTimerId);

        provider.data = fs.readFileSync(process.argv[4]);
        relatedSession.SetSyncRequired();
        const shortGeneral = relatedSession.FetchGeneralInfo();
        const shortGlobal = relatedSession.FetchGlobalDesc();
        const timerAfterShortUpdate = relatedSession.FetchTimerDesc(relatedTimerId);
        check(shortGeneral.platformInfoJson !== baselineGeneral.platformInfoJson,
            "short-term fixture did not change its metadata");
        check(shortGlobal !== baselineGlobal,
            "short-term update did not invalidate the short-term cache bucket");
        check(timerAfterShortUpdate === baselineTimer,
            "short-term update invalidated the long-term cache bucket");

        provider.data = fs.readFileSync(process.argv[5]);
        relatedSession.SetSyncRequired();
        const generalAfterLongUpdate = relatedSession.FetchGeneralInfo();
        const globalAfterLongUpdate = relatedSession.FetchGlobalDesc();
        const longTimer = relatedSession.FetchTimerDesc(relatedTimerId);
        check(generalAfterLongUpdate === shortGeneral && globalAfterLongUpdate === shortGlobal,
            "long-term update invalidated the short-term cache bucket");
        check(longTimer !== timerAfterShortUpdate,
            "long-term update did not invalidate the long-term cache bucket");
        check(longTimer.timerName !== timerAfterShortUpdate.timerName,
            "long-term fixture did not change its metadata");
        relatedSession.Dispose();
    }

    const browserContext = {
        WebAssembly, TextDecoder, TextEncoder, Uint8Array, ArrayBuffer, BigInt,
        FinalizationRegistry, Symbol, Map, Object, Number, String, Boolean,
        TypeError, RangeError, Error, console,
        atob: (value) => Buffer.from(value, "base64").toString("binary"),
    };
    browserContext.globalThis = browserContext;
    vm.createContext(browserContext);
    vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), browserContext);
    const browserLibmp = await browserContext.LibMP.create();
    check(browserLibmp.Versions.Library === libmp.Versions.Library,
        "browser-global library initialization failed");

    console.log("LibMP JavaScript smoke test passed");
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});

// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"use strict";

const fs = require("node:fs");

const lines = [];
const bool = value => value ? 1 : 0;
const emit = (...fields) => lines.push(fields.map(String).join(" "));

function check(condition, message) {
    if (!condition)
        throw new Error(message);
}

function hexU32(value) {
    return (Number(value) >>> 0).toString(16).padStart(8, "0");
}

function hexU64(value) {
    return BigInt.asUintN(64, BigInt(value)).toString(16).padStart(16, "0");
}

function floatBits(value) {
    const data = Buffer.allocUnsafe(4);
    data.writeFloatLE(value);
    return hexU32(data.readUInt32LE());
}

function doubleBits(value) {
    const data = Buffer.allocUnsafe(8);
    data.writeDoubleLE(value);
    return hexU64(data.readBigUInt64LE());
}

function hexBytes(value) {
    if (value == null)
        return "-";
    const bytes = Buffer.from(value, "utf8");
    return `${bytes.length}:${bytes.toString("hex")}`;
}

function checkTrackAndLabelApis(libmp, session) {
    check(libmp.Enums.AppType.Unknown === 0 && libmp.Enums.AppType.Server === 3,
        "application-type enum is unavailable");
    const tracks = session.GetTracks();
    try {
        check(tracks.Size() === session.GetTrackCount(), "track count mismatch");
    } finally {
        tracks.Dispose();
    }
    const fetchedTracks = session.FetchTracks();
    check(fetchedTracks.length === session.GetTrackCount(), "fetched track count mismatch");
    if (fetchedTracks.length !== 0) {
        const segments = session.GetSegments(0);
        try {
            check(segments.Size() === session.GetSegmentCount(0), "segment count mismatch");
        } finally {
            segments.Dispose();
        }
        check(session.FetchSegments(0).length === session.GetSegmentCount(0),
            "fetched segment count mismatch");
        check(session.SelectTrack(0), "failed to select first track");
        if (session.GetSegmentCount(0) !== 0)
            check(session.SelectSegment(0), "failed to select first segment");
    }

    session.EnableLabelCache(true);
    session.ClearLabelCache();
}

function writeSummary(libmp, session) {
    const frameIdMin = session.GetFrameIdMin();
    const frameIdMax = session.GetFrameIdMax();
    emit("LIBMP_PARITY", 3);
    emit("SESSION", session.GetDataFormatVersion(), libmp.Versions.Library,
        frameIdMin, frameIdMax);

    const global = session.FetchGlobalDesc();
    if (global)
        emit("GLOBAL", 1, floatBits(global.tickToMsCpu), floatBits(global.tickToMsGpu));
    else
        emit("GLOBAL", 0);
    const general = session.FetchGeneralInfo();
    if (general)
        emit("GENERAL", 1, hexBytes(general.platformInfoJson));
    else
        emit("GENERAL", 0);

    const groupIdMax = session.GetGroupIdMax();
    emit("GROUPS", groupIdMax);
    for (let queryId = 1; queryId <= groupIdMax; ++queryId) {
        const group = session.FetchGroupDesc(queryId);
        if (group)
            emit("GROUP", queryId, 1, group.groupId, hexBytes(group.groupName),
                hexU32(group.color), bool(group.isGpu));
        else
            emit("GROUP", queryId, 0);
    }

    const timerIds = session.FetchTimerIds();
    emit("TIMER_IDS", timerIds.length);
    timerIds.forEach((id, index) => {
        emit("TIMER_ID", index, id);
        const timer = session.FetchTimerDesc(id);
        if (timer)
            emit("TIMER", id, 1, timer.timerId, hexBytes(timer.timerName),
                timer.groupId, hexU32(timer.color), bool(timer.isUserTimer));
        else
            emit("TIMER", id, 0);
    });
    for (let groupId = 1; groupId <= groupIdMax; ++groupId) {
        const ids = session.FetchTimerIdsByGroupId(groupId);
        emit("GROUP_TIMER_IDS", groupId, ids.length, ...ids);
    }

    const counterIds = session.FetchCounterIds();
    emit("COUNTER_IDS", counterIds.length);
    counterIds.forEach((id, index) => {
        emit("COUNTER_ID", index, id);
        const counter = session.FetchCounterDesc(id);
        if (!counter) {
            emit("COUNTER", id, 0);
            return;
        }
        emit("COUNTER", id, 1, counter.counterId, hexBytes(counter.counterName),
            counter.parent, counter.sibling, counter.firstChild, counter.level);
        const samples = session.FetchCounterSamples(id);
        emit("COUNTER_SAMPLES", id, samples.length);
        for (let sampleIndex = 0; sampleIndex < samples.length; ++sampleIndex) {
            const sample = samples[sampleIndex];
            emit("COUNTER_SAMPLE", id, sampleIndex, 1,
                sample.frameId, doubleBits(sample.value));
        }
        const last = session.FetchLastCounterSample(id);
        if (last)
            emit("COUNTER_LAST", id, 1, last.frameId, doubleBits(last.value));
        else
            emit("COUNTER_LAST", id, 0);
    });

    emit("GPU_THREAD", session.GetThreadIdGpu());
    const threadIds = session.FetchThreadIds();
    emit("THREAD_IDS", threadIds.length);
    threadIds.forEach((id, index) => {
        emit("THREAD_ID", index, id);
        const thread = session.FetchThreadDesc(id);
        if (thread)
            emit("THREAD", id, 1, thread.threadId, hexBytes(thread.threadName),
                thread.bufferSize, bool(thread.isGpu));
        else
            emit("THREAD", id, 0);
    });

    const frameCount = frameIdMax >= frameIdMin ? frameIdMax - frameIdMin + 1 : 0;
    emit("FRAMES", frameCount);
    for (let offset = 0; offset < frameCount; ++offset) {
        const queryId = frameIdMin + offset;
        const frame = session.FetchFrameDesc(queryId);
        if (!frame) {
            emit("FRAME", queryId, 0);
            continue;
        }
        const threadLogs = frame.threadLogs || [];
        emit("FRAME", queryId, 1, frame.frameId, frame.frameAbsoluteId,
            hexU64(frame.tickStartCpu), hexU64(frame.tickEndCpu),
            hexU64(frame.tickStartGpu), hexU64(frame.tickEndGpu),
            hexU64(frame.labelOffsetMin), hexU64(frame.labelOffsetMax),
            bool(frame.isPaused), bool(frame.isIncomplete), threadLogs.length);
        threadLogs.forEach((log, logIndex) => {
            emit("FRAME_THREAD_LOG", queryId, logIndex, log.threadId, log.logEntriesNum,
                hexU64(log.logStartLocator), bool(log.isRemoved));
        });
    }

    function writeU64Samples(name, samples) {
        emit(`${name}_SAMPLES`, samples.length);
        samples.forEach((sample, index) => {
            emit(`${name}_SAMPLE`, index, 1, sample.frameId, hexU64(sample.value));
        });
    }
    writeU64Samples("PLACE_ID", session.FetchPlaceIdSamples());
    writeU64Samples("UTC", session.FetchUtcTimestampSamples());

    const probes = [
        ["*", false], ["*", true], ["*A*", false], ["*A*", true],
        ["__libmp_parity_missing__", false],
    ];
    emit("FIND_QUERIES", probes.length);
    function writeFindFamily(family, findFirst, findAll) {
        probes.forEach(([mask, sensitive], probeIndex) => {
            const ids = findAll(mask, sensitive);
            emit("FIND", family, probeIndex, bool(sensitive), hexBytes(mask),
                findFirst(mask, sensitive), ids.length, ...ids);
        });
    }
    writeFindFamily("GROUP",
        (mask, sensitive) => session.FindGroupId(mask, sensitive),
        (mask, sensitive) => session.FindGroupIds(mask, sensitive));
    writeFindFamily("TIMER",
        (mask, sensitive) => session.FindTimerId(mask, sensitive),
        (mask, sensitive) => session.FindTimerIds(mask, sensitive));
    writeFindFamily("COUNTER",
        (mask, sensitive) => session.FindCounterId(mask, sensitive),
        (mask, sensitive) => session.FindCounterIds(mask, sensitive));
    writeFindFamily("THREAD",
        (mask, sensitive) => session.FindThreadId(mask, sensitive),
        (mask, sensitive) => session.FindThreadIds(mask, sensitive));

    emit("GROUP_NAME_TIMER_QUERIES", probes.length);
    probes.forEach(([mask, sensitive], probeIndex) => {
        const ids = session.FetchTimerIdsByGroupNames(mask, sensitive);
        emit("GROUP_NAME_TIMERS", probeIndex, bool(sensitive), hexBytes(mask),
            ids.length, ...ids);
    });

    emit("COUNTER_ITERATOR_BEGIN");
    const counterIterator = session.CreateCounterIterator();
    try {
        if (!counterIterator.Configure({ RootCounterIds: [] }))
            throw new Error("failed to configure counter iterator");
        const state = counterIterator.GetState();
        let reportIndex = 0;
        while (counterIterator.Step()) {
            emit("CI", reportIndex++, bool(state.started()), bool(state.finished()),
                state.counterId(), state.parentId(), state.level(), state.relativeLevel(),
                state.rootLevel(), state.indexInSiblings(), bool(state.isLeaf()),
                bool(state.hierarchyChanged()), bool(state.counterStackIsOverflowed()));
        }
        emit("COUNTER_ITERATOR_END", reportIndex, bool(state.finished()));
    } finally {
        counterIterator.Dispose();
    }

    emit("LOG_ITERATOR_BEGIN");
    const logIterator = session.CreateLogIterator();
    try {
        if (!logIterator.Configure({ EmitThreadFrameEnd: true, TrackScopeLabels: true }))
            throw new Error("failed to configure log iterator");
        const state = logIterator.GetState();
        let reportIndex = 0;
        let checkedLabelApis = false;
        while (logIterator.Step()) {
            if (!checkedLabelApis && !state.isLabel()) {
                check(logIterator.FetchCurrentLabel() === null);
                check(logIterator.GetCurrentScopeLabelCount() === 0);
                check(logIterator.GetScopeLabelCount(state.threadId(), 0) === 0);
                check(logIterator.FetchCurrentScopeLabel(0) === null);
                check(logIterator.FetchScopeLabel(state.threadId(), 0, 0) === null);
                checkedLabelApis = true;
            }
            emit("LI", reportIndex++, bool(state.started()), bool(state.finished()),
                state.threadStackDepth(), bool(state.threadStackIsUnderflowed()),
                bool(state.threadStackIsOverflowed()), bool(state.threadStackWasOverflowed()),
                hexU64(state.threadPrevTimestamp()), state.frameId(), state.frameAbsoluteId(),
                hexU64(state.frameDescLocator()), state.threadId(), bool(state.isGpuThread()),
                bool(state.isFrameStitching()), bool(state.frameChanged()),
                bool(state.threadChanged()), hexU64(state.entryLocator()), state.timerId(),
                hexU64(state.timestamp()), hexU64(state.timestampGpu()), state.entryTypeRaw(),
                state.eventType(), bool(state.isUnknown()), bool(state.isEnter()),
                bool(state.isExit()), bool(state.isLabel()), bool(state.isEvent()),
                bool(state.isThreadFrameEnd()), bool(state.isFrameBoundary()),
                logIterator.GetCurrentEntryCount());
        }
        check(checkedLabelApis, "log iterator has no non-label entry");
        emit("LOG_ITERATOR_END", reportIndex, bool(state.finished()));
    } finally {
        logIterator.Dispose();
    }
    emit("END");
}

function checkCachedDescriptor(
        session, label, firstId, secondId, get, fetch, viewValue, fetchedValue) {
    const firstView = get(firstId);
    const repeatedView = get(firstId);
    check(firstView && firstView === repeatedView, `${label}: Get identity miss`);
    const expected = viewValue(firstView);
    if (secondId !== undefined) {
        get(secondId);
        check(get(firstId) === firstView, `${label}: interleaved Get identity miss`);
    }

    const firstCopy = fetch(firstId);
    const repeatedCopy = fetch(firstId);
    check(firstCopy && firstCopy === repeatedCopy, `${label}: Fetch identity miss`);
    check(fetchedValue(firstCopy) === expected, `${label}: Get/Fetch value mismatch`);
    if (secondId !== undefined) {
        fetch(secondId);
        check(fetch(firstId) === firstCopy, `${label}: interleaved Fetch identity miss`);
    }

    session.ClearObjectCache();
    const clearedView = get(firstId);
    const clearedCopy = fetch(firstId);
    check(clearedView !== firstView, `${label}: ClearObjectCache kept Get wrapper`);
    check(clearedCopy !== firstCopy, `${label}: ClearObjectCache kept Fetch object`);
    check(viewValue(clearedView) === expected && fetchedValue(clearedCopy) === expected,
        `${label}: ClearObjectCache changed value`);
}

function checkCacheContract(libmp, session, capture, shortUpdatePath, longUpdatePath) {
    session.EnableObjectCache(true);
    const timerIds = session.FetchTimerIds();
    const counterIds = session.FetchCounterIds();
    const threadIds = session.FetchThreadIds();
    const groupIds = [];
    for (let id = 1; id <= session.GetGroupIdMax(); ++id) {
        if (session.FetchGroupDesc(id))
            groupIds.push(id);
    }

    if (session.FetchGlobalDesc())
        checkCachedDescriptor(session, "GlobalDesc", undefined, undefined,
            () => session.GetGlobalDesc(), () => session.FetchGlobalDesc(),
            value => floatBits(value.tickToMsCpu()), value => floatBits(value.tickToMsCpu));
    if (session.FetchGeneralInfo())
        checkCachedDescriptor(session, "GeneralInfo", undefined, undefined,
            () => session.GetGeneralInfo(), () => session.FetchGeneralInfo(),
            value => hexBytes(value.platformInfoJson()), value => hexBytes(value.platformInfoJson));
    if (groupIds.length)
        checkCachedDescriptor(session, "GroupDesc", groupIds[0], groupIds[1],
            id => session.GetGroupDesc(id), id => session.FetchGroupDesc(id),
            value => `${value.groupId()}:${hexBytes(value.groupName())}`,
            value => `${value.groupId}:${hexBytes(value.groupName)}`);
    if (timerIds.length)
        checkCachedDescriptor(session, "TimerDesc", timerIds[0], timerIds[1],
            id => session.GetTimerDesc(id), id => session.FetchTimerDesc(id),
            value => `${value.timerId()}:${hexBytes(value.timerName())}`,
            value => `${value.timerId}:${hexBytes(value.timerName)}`);
    if (counterIds.length)
        checkCachedDescriptor(session, "CounterDesc", counterIds[0], counterIds[1],
            id => session.GetCounterDesc(id), id => session.FetchCounterDesc(id),
            value => `${value.counterId()}:${hexBytes(value.counterName())}`,
            value => `${value.counterId}:${hexBytes(value.counterName)}`);
    if (threadIds.length)
        checkCachedDescriptor(session, "ThreadDesc", threadIds[0], threadIds[1],
            id => session.GetThreadDesc(id), id => session.FetchThreadDesc(id),
            value => `${value.threadId()}:${hexBytes(value.threadName())}`,
            value => `${value.threadId}:${hexBytes(value.threadName)}`);

    if (threadIds.length) {
        session.EnableObjectCache(false);
        const view1 = session.GetThreadDesc(threadIds[0]);
        const view2 = session.GetThreadDesc(threadIds[0]);
        const copy1 = session.FetchThreadDesc(threadIds[0]);
        const copy2 = session.FetchThreadDesc(threadIds[0]);
        check(view1 && view2 && view1 !== view2, "disabled cache reused Get wrapper");
        check(copy1 && copy2 && copy1 !== copy2, "disabled cache reused Fetch object");
        check(view1.threadId() === view2.threadId() && copy1.threadId === copy2.threadId,
            "disabled cache changed descriptor value");
        session.EnableObjectCache(true);
    }

    const timerArray1 = session.GetTimerIds();
    const timerArray2 = session.GetTimerIds();
    try {
        check(timerArray1 !== timerArray2, "array wrappers unexpectedly share identity");
        check(timerArray1.Size() === timerArray2.Size(), "array wrapper sizes differ");
        if (timerArray1.Size())
            check(timerArray1.Get(0) === timerArray2.Get(0), "array wrapper values differ");
    } finally {
        timerArray1.Dispose();
        timerArray2.Dispose();
    }

    if (shortUpdatePath && longUpdatePath) {
        const provider = {
            data: capture,
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
        const related = libmp.Session.OpenFromExternalProvider(provider);
        check(related?.IsValid(), "related-snapshot session failed");
        try {
            const id = related.FetchTimerIds()[0];
            const baselineGlobal = related.FetchGlobalDesc();
            const baselineGeneral = related.FetchGeneralInfo();
            const baselineTimer = related.FetchTimerDesc(id);

            provider.data = fs.readFileSync(shortUpdatePath);
            related.SetSyncRequired();
            const shortGeneral = related.FetchGeneralInfo();
            const shortGlobal = related.FetchGlobalDesc();
            const timerAfterShort = related.FetchTimerDesc(id);
            check(shortGeneral.platformInfoJson !== baselineGeneral.platformInfoJson,
                "short-term fixture did not change its metadata");
            check(shortGlobal !== baselineGlobal,
                "short-term update did not invalidate the short-term cache bucket");
            check(timerAfterShort === baselineTimer,
                "short-term update invalidated the long-term cache bucket");

            provider.data = fs.readFileSync(longUpdatePath);
            related.SetSyncRequired();
            const generalAfterLong = related.FetchGeneralInfo();
            const globalAfterLong = related.FetchGlobalDesc();
            const longTimer = related.FetchTimerDesc(id);
            check(generalAfterLong === shortGeneral && globalAfterLong === shortGlobal,
                "long-term update invalidated the short-term cache bucket");
            check(longTimer !== timerAfterShort,
                "long-term update did not invalidate the long-term cache bucket");
            check(longTimer.timerName !== timerAfterShort.timerName,
                "long-term fixture did not change its metadata");
        } finally {
            related.Dispose();
        }
    }
}

async function main() {
    if (process.argv.length !== 4 && process.argv.length !== 6)
        throw new Error(
            "usage: parity_summary.js <LibMP.js> <capture.gprx> " +
            "[<short-update.gprx> <long-update.gprx>]");
    const LibMP = require(process.argv[2]);
    const capture = fs.readFileSync(process.argv[3]);
    const libmp = await LibMP.create();
    const session = libmp.Session.OpenFromMem(capture);
    if (!session?.IsValid())
        throw new Error(`failed to open ${process.argv[3]}`);
    try {
        checkTrackAndLabelApis(libmp, session);
        writeSummary(libmp, session);
        checkCacheContract(libmp, session, capture, process.argv[4], process.argv[5]);
    } finally {
        session.Dispose();
    }
    process.stdout.write(lines.join("\n") + "\n");
}

main().catch(error => {
    console.error(error?.stack || String(error));
    process.exitCode = 1;
});

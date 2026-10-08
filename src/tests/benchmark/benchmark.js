// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"use strict";

const fs = require("node:fs");

function mix(hash, value) {
    hash = ((hash << 5) | (hash >>> 27)) >>> 0;
    return ((hash ^ (Number(value) >>> 0)) + 0x9e3779b9) >>> 0;
}

function byteLength(value) {
    return value == null ? 0 : Buffer.byteLength(value, "utf8");
}

function workload(session, passes) {
    let hash = 0x811c9dc5;
    let itemCount = 0;
    for (let pass = 0; pass < passes; ++pass) {
        hash = mix(hash, session.GetFrameIdMin());
        hash = mix(hash, session.GetFrameIdMax());
        hash = mix(hash, session.FetchGlobalDesc() ? 1 : 0);
        const general = session.FetchGeneralInfo();
        hash = mix(hash, general ? byteLength(general.platformInfoJson) : 0);
        itemCount += 2;

        const groupIdMax = session.GetGroupIdMax();
        hash = mix(hash, groupIdMax);
        itemCount += groupIdMax;
        for (let id = 1; id <= groupIdMax; ++id) {
            const value = session.FetchGroupDesc(id);
            if (!value) {
                hash = mix(hash, 0);
                continue;
            }
            hash = mix(hash, value.groupId);
            hash = mix(hash, byteLength(value.groupName));
            hash = mix(hash, value.color);
            hash = mix(hash, value.isGpu ? 1 : 0);
        }

        const timerIds = session.FetchTimerIds();
        hash = mix(hash, timerIds.length);
        itemCount += timerIds.length;
        for (const id of timerIds) {
            hash = mix(hash, id);
            const value = session.FetchTimerDesc(id);
            if (!value) {
                hash = mix(hash, 0);
                continue;
            }
            hash = mix(hash, value.timerId);
            hash = mix(hash, byteLength(value.timerName));
            hash = mix(hash, value.groupId);
            hash = mix(hash, value.color);
            hash = mix(hash, value.isUserTimer ? 1 : 0);
        }

        const counterIds = session.FetchCounterIds();
        hash = mix(hash, counterIds.length);
        itemCount += counterIds.length;
        for (const id of counterIds) {
            hash = mix(hash, id);
            const value = session.FetchCounterDesc(id);
            if (!value) {
                hash = mix(hash, 0);
                continue;
            }
            hash = mix(hash, value.counterId);
            hash = mix(hash, byteLength(value.counterName));
            hash = mix(hash, value.parent);
            hash = mix(hash, value.sibling);
            hash = mix(hash, value.firstChild);
            hash = mix(hash, value.level);
        }

        const threadIds = session.FetchThreadIds();
        hash = mix(hash, threadIds.length);
        itemCount += threadIds.length;
        for (const id of threadIds) {
            hash = mix(hash, id);
            const value = session.FetchThreadDesc(id);
            if (!value) {
                hash = mix(hash, 0);
                continue;
            }
            hash = mix(hash, value.threadId);
            hash = mix(hash, byteLength(value.threadName));
            hash = mix(hash, value.bufferSize);
            hash = mix(hash, value.isGpu ? 1 : 0);
        }
    }
    return { checksum: hash >>> 0, itemCount };
}

function logWorkload(session, passes) {
    let hash = 0x811c9dc5;
    let itemCount = 0;
    for (let pass = 0; pass < passes; ++pass) {
        const iterator = session.CreateLogIterator();
        try {
            if (!iterator.Configure({ EmitThreadFrameEnd: true }))
                throw new Error("failed to configure log iterator");
            const state = iterator.GetState();
            if (!state)
                throw new Error("log iterator has no state");
            let entryCount = 0;
            while (iterator.Step()) {
                hash = mix(hash, state.frameId());
                hash = mix(hash, state.threadId());
                hash = mix(hash, state.timerId());
                hash = mix(hash, state.entryTypeRaw());
                hash = mix(hash, state.eventType());
                hash = mix(hash, state.isEvent() ? 1 : 0);
                const timer = session.FetchTimerDesc(state.timerId());
                if (timer) {
                    hash = mix(hash, timer.timerId);
                    hash = mix(hash, byteLength(timer.timerName));
                    hash = mix(hash, timer.color);
                } else {
                    hash = mix(hash, 0);
                }
                entryCount = (entryCount + 1) >>> 0;
            }
            hash = mix(hash, entryCount);
            itemCount += entryCount;
        } finally {
            iterator.Dispose();
        }
    }
    return { checksum: hash >>> 0, itemCount };
}

function openSession(libmp, capture, cache) {
    const session = libmp.Session.OpenFromMem(capture);
    if (!session?.IsValid())
        throw new Error("failed to open capture");
    session.EnableObjectCache(cache);
    return session;
}

function emit(scenario, cache, result, samples) {
    console.log(`BENCH\t${scenario}\t${cache ? "on" : "off"}\t${result.checksum}\t${result.itemCount}\t${samples.join(",")}`);
}

function sameResult(left, right) {
    return left.checksum === right.checksum && left.itemCount === right.itemCount;
}

function runShared(libmp, capture, cache, warmups, iterations, passes) {
    const session = openSession(libmp, capture, cache);
    for (let i = 0; i < warmups; ++i)
        workload(session, passes);
    const samples = [];
    let result;
    for (let i = 0; i < iterations; ++i) {
        const start = process.hrtime.bigint();
        const current = workload(session, passes);
        const elapsed = process.hrtime.bigint() - start;
        if (result !== undefined && !sameResult(current, result))
            throw new Error("workload checksum changed");
        result = current;
        samples.push(Number(elapsed));
    }
    session.Dispose();
    emit("shared", cache, result, samples);
}

function runFresh(libmp, capture, cache, warmups, iterations, passes) {
    const run = () => {
        const session = openSession(libmp, capture, cache);
        const checksum = workload(session, passes);
        session.Dispose();
        return checksum;
    };
    for (let i = 0; i < warmups; ++i)
        run();
    const samples = [];
    let result;
    for (let i = 0; i < iterations; ++i) {
        const start = process.hrtime.bigint();
        const current = run();
        const elapsed = process.hrtime.bigint() - start;
        if (result !== undefined && !sameResult(current, result))
            throw new Error("workload checksum changed");
        result = current;
        samples.push(Number(elapsed));
    }
    emit("fresh", cache, result, samples);
}

function runLogs(libmp, capture, cache, warmups, iterations, passes) {
    const session = openSession(libmp, capture, cache);
    for (let i = 0; i < warmups; ++i)
        logWorkload(session, passes);
    const samples = [];
    let result;
    for (let i = 0; i < iterations; ++i) {
        const start = process.hrtime.bigint();
        const current = logWorkload(session, passes);
        const elapsed = process.hrtime.bigint() - start;
        if (result !== undefined && !sameResult(current, result))
            throw new Error("log workload checksum changed");
        result = current;
        samples.push(Number(elapsed));
    }
    session.Dispose();
    emit("logs", cache, result, samples);
}

async function main() {
    if (process.argv.length !== 10)
        throw new Error(
            "usage: benchmark.js <LibMP.js> <capture> <warmups> <iterations> " +
            "<metadata-passes> <log-warmups> <log-iterations> <log-passes>");
    const LibMP = require(process.argv[2]);
    const capture = fs.readFileSync(process.argv[3]);
    const warmups = Number.parseInt(process.argv[4], 10);
    const iterations = Number.parseInt(process.argv[5], 10);
    const metadataPasses = Number.parseInt(process.argv[6], 10);
    const logWarmups = Number.parseInt(process.argv[7], 10);
    const logIterations = Number.parseInt(process.argv[8], 10);
    const logPasses = Number.parseInt(process.argv[9], 10);
    const libmp = await LibMP.create();
    for (const cache of [false, true]) {
        console.error(`  cache=${cache ? "on" : "off"}: metadata / reused session`);
        runShared(libmp, capture, cache, warmups, iterations, metadataPasses);
        console.error(`  cache=${cache ? "on" : "off"}: metadata / new session`);
        runFresh(libmp, capture, cache, warmups, iterations, metadataPasses);
        console.error(`  cache=${cache ? "on" : "off"}: full log traversal / reused session`);
        runLogs(libmp, capture, cache, logWarmups, logIterations, logPasses);
    }
}

main().catch(error => {
    console.error(error?.stack || String(error));
    process.exitCode = 1;
});

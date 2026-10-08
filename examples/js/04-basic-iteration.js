/**
 * 04-basic-iteration.js -- Iterating Scopes (Single Frame, Single Thread)
 *
 * Setup (Node.js):
 * - Download LibMP.js from https://github.com/Roblox/libmp/releases, or build it
 *   from the repository root with `python3 src/scripts/build.py --js`
 *   (output: src/build/js/LibMP.js).
 * - Place LibMP.js beside this file; it has no other dependencies.
 * - Run: node 04-basic-iteration.js /path/to/capture.x.html (or capture.gprx)
 *
 * This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.
 */

"use strict";

const LibMP = require("./LibMP.js");

async function main() {
    if (process.argv.length !== 3)
        throw new Error("usage: node 04-basic-iteration.js <capture.x.html|capture.gprx>");

    const libmp = await LibMP.create();
    const session = await libmp.Session.OpenFromFile(process.argv[2]);
    if (!session?.IsValid()) {
        session?.Dispose();
        throw new Error("Failed to open session");
    }

    try {
        const frameId = session.GetFrameIdMax();
        if (frameId === 0) {
            console.log("No frames available");
            return;
        }

        const mainThreadId = session.FindThreadId("*rbx main*");
        if (mainThreadId === 0) {
            console.log("No Main thread found");
            return;
        }

        const globalDesc = session.FetchGlobalDesc();
        const threadDesc = session.FetchThreadDesc(mainThreadId);
        if (!globalDesc || !threadDesc)
            throw new Error("Required capture metadata is missing");

        console.log(`Frame ${frameId}, Thread: '${threadDesc.threadName}'`);
        console.log("-".repeat(60));

        const iterator = session.CreateLogIterator();
        try {
            if (!iterator.Configure({ ThreadIds: [mainThreadId] }))
                throw new Error("Failed to configure log iterator");

            const state = iterator.GetState();
            iterator.RewindTo(frameId, frameId);

            const scopeLimit = 50;
            let scopeCounter = 0;
            while (iterator.Step()) {
                const timer = session.FetchTimerDesc(state.timerId());
                const timerName = timer?.timerName ?? `<timer ${state.timerId()}>`;
                if (state.isEnter()) {
                    console.log(`${"  ".repeat(state.threadStackDepth())}> ${timerName}`);
                } else if (state.isExit()) {
                    if (++scopeCounter > scopeLimit)
                        break;

                    const depth = state.threadStackDepth();
                    let duration = "";
                    if (!state.threadStackIsUnderflowed()) {
                        const element = iterator.GetCurrentThreadStackElement(depth);
                        if (element) {
                            const ticks = state.timestamp() - element.enterTimestamp();
                            duration = ` (${(Number(ticks) * globalDesc.tickToMsCpu).toFixed(3)} ms)`;
                        }
                    }
                    console.log(`${"  ".repeat(depth + 1)}< ${timerName}${duration}`);
                }
            }
        } finally {
            iterator.Dispose();
        }
    } finally {
        session.Dispose();
    }

    console.log("\nDone.");
}

main().catch((error) => {
    console.error(error.message);
    process.exitCode = 1;
});

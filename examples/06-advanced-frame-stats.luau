--[[
    06-advanced-frame-stats.luau -- Per-Timer Per-Frame Statistics

    Calculates inclusive scope times for each timer in each frame using the
    advanced iterator from 05-advanced-iteration for precise cross-frame
    scope attribution (depth is not needed, so full reconstruction is skipped).

    Result: a table of [timerId][frameId] -> inclusive time (ms).
    Prints the top 20 timers by average, then the last 10 frame values
    of the top timer.

    Works in both the Roblox Engine and Lute (CLI).
    See example #1 for setup instructions.
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")
local LogIteration = if game
    then require("@game/ReplicatedStorage/05-advanced-iteration")
    else require("./05-advanced-iteration")

LibMP.Control:SetFrameLimit(16)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local function printAdvancedFrameStats()
    local session = LibMP.Session.OpenFromLiveData()
    if not session or not session:IsValid() then
        print("Failed to open session")
        if session then session:Dispose() end
        return
    end

    local globalDesc = session:FetchGlobalDesc()
    local frameIdMin = session:GetFrameIdMin()
    local frameIdMax = session:GetFrameIdMax()

    if frameIdMin == 0 and frameIdMax == 0 then
        print("No frames available")
        session:Dispose()
        return
    end

    -- Result: timerInclusivePerFrame[timerId][frameId] = inclusive time in ms
    local timerInclusivePerFrame: { [number]: { [number]: number } } = {}

    local iter = session:CreateLogIterator()
    iter:Configure({
        SkipGpuThreads = true,
        SkipEvents = true,
        EmitThreadFrameEnd = true,
    })

    -- Main routine: calculates inclusive scope times for each timer in each frame
    LogIteration.iterateFrameLocal(session, iter, frameIdMin, frameIdMax,
        function(frameId, threadId, depth, timerId, enterTs, exitTs)
            local durationMs = (exitTs - enterTs) * globalDesc.TickToMsCpu
            
            local perFrame = timerInclusivePerFrame[timerId]
            if not perFrame then
                perFrame = {}
                timerInclusivePerFrame[timerId] = perFrame
            end
            
            perFrame[frameId] = (perFrame[frameId] or 0) + durationMs
        end
    )

    iter:Dispose()

    -- Calculate averages and sort
    local totalFrames = frameIdMax - frameIdMin + 1

    type TimerResult = { timerId: number, avgInclusive: number }
    local results: { TimerResult } = {}

    for timerId, perFrame in timerInclusivePerFrame do
        local total = 0
        for _, ms in perFrame do
            total += ms
        end
        table.insert(results, {
            timerId = timerId,
            avgInclusive = total / totalFrames,
        })
    end

    table.sort(results, function(a, b) return a.avgInclusive > b.avgInclusive end)

    -- Print top 20
    local topN = math.min(20, #results)
    print(string.format("Top %d timers by average inclusive time (%d frames analyzed):", topN, totalFrames))
    print()
    print(string.format("%-40s %s", "Timer", "Avg Inclusive (ms)"))
    print(string.rep("-", 60))

    for i = 1, topN do
        local r = results[i]
        local timerName = session:FetchTimerDesc(r.timerId).TimerName
        print(string.format("%-40s %.3f", timerName, r.avgInclusive))
    end

    -- Print last 10 frame values of the top timer
    local framesToPrint = 10
    if #results > 0 then
        local topTimerId = results[1].timerId
        local topTimerName = session:FetchTimerDesc(topTimerId).TimerName
        local perFrame = timerInclusivePerFrame[topTimerId]

        print()
        print(string.format("Last frames of top timer \"%s\":", topTimerName))
        print(string.format("%-8s %s", "Frame", "Inclusive (ms)"))
        print(string.rep("-", 30))

        local startFrame = math.max(frameIdMin, frameIdMax - framesToPrint + 1)     
        for frameId = startFrame, frameIdMax do
            local ms = perFrame[frameId] or 0
            print(string.format("%-8d %.3f", frameId, ms))
        end
    end

    session:Dispose()
    print("\nDone.")
end

if game then
    -- Delay execution to allow the engine to accumulate
    -- frame history on the first activation of MicroProfiler
    task.delay(2, printAdvancedFrameStats)
else
    printAdvancedFrameStats()
end

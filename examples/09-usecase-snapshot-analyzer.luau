--[[
    09-usecase-snapshot-analyzer.luau -- Incremental Snapshot Analyzer

    Captures a snapshot periodically and performs "slow" analysis on it,
    processing at most N=100 iterator steps per game frame to avoid
    impacting frame time. Does not capture a new snapshot until the
    current analysis is complete.

    This example is designed for the Roblox Engine only.
    See example #1 for setup instructions.
]]

if not game then
    print("This example is intended for the Roblox Engine only.")
    return
end

local RunService = game:GetService("RunService")
local LibMP = require("@game/ReplicatedStorage/LibMP")

-- Configuration
local CAPTURE_INTERVAL = 60 -- 1 minute between captures
local FRAME_LIMIT = 16
local MAX_STEPS_PER_FRAME = 150

LibMP.Control:SetFrameLimit(FRAME_LIMIT)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

-- Analysis state
local analysisSession: any = nil
local analysisIter: any = nil
local iterState: any = nil
local isAnalyzing = false
local analysisResults: { [number]: number } = {} -- timerId -> total count

local function startAnalysis(buf: buffer): boolean
    analysisSession = LibMP.Session.OpenFromBuffer(buf)
    if not analysisSession or not analysisSession:IsValid() then
        print("[Analyzer] Failed to open session from snapshot")
        if analysisSession then analysisSession:Dispose() end
        return false
    end

    analysisIter = analysisSession:CreateLogIterator()
    analysisIter:Configure({
        SkipGpuThreads = true,
        SkipEvents = true,
        StartFrameId = 0, -- zero means start from the first available frame (default)
        EndFrameId = 0, -- zero means stop at the last available frame (default)
    })

    iterState = analysisIter:GetState()
    table.clear(analysisResults)
    isAnalyzing = true

    print(string.format("[Analyzer] Started analysis (frames %d-%d)",
        analysisSession:GetFrameIdMin(), analysisSession:GetFrameIdMax()))
    return true
end

-- Processes up to MAX_STEPS_PER_FRAME steps. Returns true when done.
local function stepAnalysis(): boolean
    for _ = 1, MAX_STEPS_PER_FRAME do
        if not analysisIter:Step() then
            return true -- done
        end

        if iterState:IsExit() then
            local timerId = iterState:TimerId()
            analysisResults[timerId] = (analysisResults[timerId] or 0) + 1
        end
    end

    return false -- more work to do
end

local function finishAnalysis()
    -- Print results: top 10 most frequent timers
    type Entry = { timerId: number, count: number }
    local sorted: { Entry } = {}
    
    for timerId, count in analysisResults do
        table.insert(sorted, { timerId = timerId, count = count })
    end
    
    table.sort(sorted, function(a, b) return a.count > b.count end)

    local topN = math.min(10, #sorted)
    print(string.format("[Analyzer] Complete. Top %d most frequent scopes:", topN))
    
    for i = 1, topN do
        local entry = sorted[i]
        local name = analysisSession:FetchTimerDesc(entry.timerId).TimerName
        print(string.format("  %d. %s (%d occurrences)", i, name, entry.count))
    end
    print()

    -- Cleanup
    analysisIter:Dispose()
    analysisIter = nil
    analysisSession:Dispose()
    analysisSession = nil
    iterState = nil
    isAnalyzing = false
end

-- Per-frame processing
RunService.Heartbeat:Connect(function()
    if not isAnalyzing then
        return
    end

    if stepAnalysis() then
        finishAnalysis()
    end
end)

-- Capture loop: waits for analysis to finish before capturing again
task.spawn(function()
    task.wait(5) -- Initial settle time

    while true do
        -- Capture snapshot
        local buf = LibMP.Control:CaptureToBufferSync()
        
        if buf then
            startAnalysis(buf)
            
            -- Wait until analysis completes
            while isAnalyzing do
                task.wait(0.1)
            end
        else
            print("[Analyzer] Failed to capture buffer.")
        end

        -- Wait for next interval
        task.wait(CAPTURE_INTERVAL)
    end
end)

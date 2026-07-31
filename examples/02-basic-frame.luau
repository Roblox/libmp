--[[ 02-basic-frame.luau -- Reading Frame Times
Demonstrates how to read CPU and GPU frame durations. 
Works in both the Roblox Engine and Lute (CLI). 
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

LibMP.Control:SetFrameLimit(64)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local function printFrameTimes()
    local session = LibMP.Session.OpenFromLiveData()
    if not session or not session:IsValid() then
        print("Failed to open session")
        if session then session:Dispose() end
        return false -- Signal failure to the runner
    end

    local globalDesc = session:FetchGlobalDesc()
    local frameIdMin = session:GetFrameIdMin()
    local frameIdMax = session:GetFrameIdMax()

    -- Fix: If frames aren't ready yet, dispose and tell the caller to retry
    if frameIdMin == 0 and frameIdMax == 0 then
        session:Dispose()
        return false 
    end

    local framesToPrint = 10
    local startFrame = math.max(frameIdMin, frameIdMax - framesToPrint + 1)

    print(string.format("%-8s %-12s %-12s", "Frame", "CPU (ms)", "GPU (ms)"))
    print(string.rep("-", 34))

    for frameId = startFrame, frameIdMax do
        local desc = session:FetchFrameDesc(frameId)
        local cpuMs = (desc.TickEndCpu - desc.TickStartCpu) * globalDesc.TickToMsCpu
        local gpuMs = (desc.TickEndGpu - desc.TickStartGpu) * globalDesc.TickToMsGpu

        -- GPU times can sometimes be inaccurate; clamp to zero
        gpuMs = math.max(gpuMs, 0)
        print(string.format("%-8d %-12.2f %-12.2f", frameId, cpuMs, gpuMs))
    end

    session:Dispose()
    return true -- Signal success
end

if game then
    -- Safely poll the session state frame-by-frame until the buffer is active
    local RunService = game:GetService("RunService")
    task.defer(function()
        while not printFrameTimes() do
            RunService.Heartbeat:Wait()
        end
    end)
else
    -- In Lute CLI, execute immediately assuming dump history is populated
    printFrameTimes()
end

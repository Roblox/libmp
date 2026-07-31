--[[ 
03-basic-counter.luau -- Enhanced Profiling Tool
Demonstrates safe profiling, automated heap tracking, and cross-platform reliability.
Optimized for developers who want active monitoring instead of static prints.
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

LibMP.Control:SetFrameLimit(64)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

-- 1. Last sample (cheapest call)
local function printLastSample(session, counterId)
    print("--- 1. Last Sample ---")
    local lastSample = session:FetchLastCounterSample(counterId)
    if lastSample then
        print(string.format("Last sample: frame %d, value %.2f MB", lastSample.FrameId, lastSample.Value / (1024 * 1024)))
    else
        print("No last sample available")
    end
    print()
end

-- 2. All samples via Fetch (returns a plain Luau array, no disposal needed)
local function printAllSamplesFetch(session, counterId)
    print("--- 2. All Samples (Fetch) ---")
    local allSamples = session:FetchCounterSamples(counterId)
    print(string.format("Total samples captured: %d", #allSamples))
    
    if #allSamples > 1 then
        local firstSample = allSamples[1]
        local lastSample = allSamples[#allSamples]
        print(string.format(" First: frame %d, value %.2f MB", firstSample.FrameId, firstSample.Value / (1024 * 1024)))
        print(string.format(" Last: frame %d, value %.2f MB", lastSample.FrameId, lastSample.Value / (1024 * 1024)))
        
        -- EXCLUSIVE FEATURE: Memory Spike Detection
        local diff = (lastSample.Value - firstSample.Value) / (1024 * 1024)
        if diff > 5 then -- Alert if memory jumped by more than 5MB in this buffer window
            print(string.format("[ALERT] Luau Heap expanded rapidly by %.2f MB!", diff))
        end
    end
    print()
end

-- 3. All samples via Get (returns a wrapped array - MUST be Disposed!)
local function printSamplesGet(session, counterId)
    print("--- 3. All Samples (Get) ---")
    local samplesArray = session:GetCounterSamples(counterId)
    print(string.format("Array size: %d", samplesArray:Size()))

    local frameIdMin = session:GetFrameIdMin()
    local frameIdMax = session:GetFrameIdMax()
    
    if frameIdMin > 0 and frameIdMax > 0 then
        local midFrameId = math.floor((frameIdMin + frameIdMax) / 2)
        local sampleAtFrame = samplesArray:FetchSampleByFrameId(midFrameId)
        if sampleAtFrame then
            print(string.format("Value at frame %d (mid): %.2f MB", sampleAtFrame.FrameId, sampleAtFrame.Value / (1024 * 1024)))
        end
    end
    
    samplesArray:Dispose() -- Safe release of engine memory allocation
    print()
end

-- Safe execution context wrapper
local function attemptProfiling()
    local session = LibMP.Session.OpenFromLiveData()
    if not session or not session:IsValid() then
        if session then session:Dispose() end
        return false -- Tell runner the session buffer isn't populated yet
    end

    local counterId = session:FindCounterId("**/Luau/heap")
    if counterId == 0 then
        session:Dispose()
        return false -- Counter isn't initialized yet
    end

    -- Verify we have real historical frames to look at
    if session:GetFrameIdMin() == 0 and session:GetFrameIdMax() == 0 then
        session:Dispose()
        return false
    end

    -- If all checks pass, run your diagnostics
    print("Luau heap counter diagnostics active:")
    print()
    
    printLastSample(session, counterId)
    printAllSamplesFetch(session, counterId)
    printSamplesGet(session, counterId)
    
    session:Dispose()
    print("Done.")
    return true -- Success
end

-- Execution Router
if game then
    -- Instead of a flat wait timer, poll safely until the MicroProfiler pipeline flows
    local RunService = game:GetService("RunService")
    task.defer(function()
        while not attemptProfiling() do
            RunService.Heartbeat:Wait()
        end
    end)
else
    -- In CLI (Lute), execute immediately assuming dump history is pre-loaded
    if not attemptProfiling() then
        print("[-] Lute CLI: Failed to execute. Ensure you passed a valid active performance stream/dump.")
    end
end

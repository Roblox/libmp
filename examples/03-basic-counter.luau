--[[
    03-basic-counter.luau -- Working with Counter Samples

    Demonstrates fetching all counter samples, the last sample, looking up
    a sample by frame ID, and proper disposal of Get*** arrays.
    Works in both the Roblox Engine and Lute (CLI).
    See example #1 for setup instructions.
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

LibMP.Control:SetFrameLimit(64)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local session = LibMP.Session.OpenFromLiveData()
if not session or not session:IsValid() then
    print("Failed to open session")
    if session then session:Dispose() end
    return
end

local counterId = session:FindCounterId("**/Luau/heap")
if counterId == 0 then
    print("Luau heap counter not found")
    session:Dispose()
    return
end

-- 1. Last sample (cheapest call)
local function printLastSample(session, counterId)
    print("--- 1. Last Sample ---")
    local lastSample = session:FetchLastCounterSample(counterId)

    if lastSample then
        print(string.format("Last sample: frame %d, value %.2f MB",
            lastSample.FrameId, lastSample.Value / (1024 * 1024)))
    else
        print("No last sample available")
    end
    print()
end

-- 2. All samples via Fetch (returns a plain Luau array, no disposal needed)
local function printAllSamplesFetch(session, counterId)
    print("--- 2. All Samples (Fetch) ---")
    local allSamples = session:FetchCounterSamples(counterId)
    print(string.format("Total samples: %d", #allSamples))

    if #allSamples > 0 then
        local firstSample = allSamples[1]
        local lastSample = allSamples[#allSamples]
        print(string.format("  First: frame %d, value %.2f MB",
            firstSample.FrameId, firstSample.Value / (1024 * 1024)))
        print(string.format("  Last:  frame %d, value %.2f MB",
            lastSample.FrameId, lastSample.Value / (1024 * 1024)))
    end
    print()
end

-- 3. All samples via Get (returns a wrapped array - MUST be Disposed!)
local function printSamplesGet(session, counterId)
    print("--- 3. All Samples (Get) ---")
    local samplesArray = session:GetCounterSamples(counterId)
    print(string.format("Array size: %d", samplesArray:Size()))

    -- Find the middle frame ID
    local frameIdMin = session:GetFrameIdMin()
    local frameIdMax = session:GetFrameIdMax()
    local midFrameId = math.floor((frameIdMin + frameIdMax) / 2)
    
    -- Look up a sample by frame ID
    local sampleAtFrame = samplesArray:FetchSampleByFrameId(midFrameId)
    if sampleAtFrame then
        print(string.format("Value at frame %d (mid): %.2f MB",
            sampleAtFrame.FrameId, sampleAtFrame.Value / (1024 * 1024)))
    end

    samplesArray:Dispose()
    print()
end

-- Main runner
local function runCounterExamples()
    print("Luau heap counter:")
    print()
    
    printLastSample(session, counterId)
    printAllSamplesFetch(session, counterId)
    printSamplesGet(session, counterId)

    session = session:Dispose()
    print("Done.")
end

if game then
    -- Delay execution to allow the engine to accumulate
    -- frame history on the first activation of MicroProfiler
    task.delay(2, runCounterExamples)
else
    runCounterExamples()
end

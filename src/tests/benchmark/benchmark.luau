-- This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

local fs = require("@std/fs")
local process = require("@lute/process")
local LibMP = require("./LibMP")

local U32_SCALE = 4294967296
local MIX_CONSTANT = 2654435769

local function rotateLeft5(value)
    local high = math.floor(value / 134217728)
    return (value * 32) % U32_SCALE + high
end

local function mix(hash, value)
    return (bit32.bxor(rotateLeft5(hash), value % U32_SCALE) + MIX_CONSTANT) % U32_SCALE
end

local function bool(value)
    return value and 1 or 0
end

local function stringSize(value)
    return value and #value or 0
end

local function workload(session, passes)
    local hash = 2166136261
    local itemCount = 0
    for _ = 1, passes do
        hash = mix(hash, session:GetFrameIdMin())
        hash = mix(hash, session:GetFrameIdMax())
        hash = mix(hash, bool(session:FetchGlobalDesc() ~= nil))
        local general = session:FetchGeneralInfo()
        hash = mix(hash, general and stringSize(general.PlatformInfoJson) or 0)
        itemCount += 2

        local groupIdMax = session:GetGroupIdMax()
        hash = mix(hash, groupIdMax)
        itemCount += groupIdMax
        for id = 1, groupIdMax do
            local value = session:FetchGroupDesc(id)
            if value then
                hash = mix(hash, value.GroupId)
                hash = mix(hash, stringSize(value.GroupName))
                hash = mix(hash, value.Color)
                hash = mix(hash, bool(value.IsGpu))
            else
                hash = mix(hash, 0)
            end
        end

        local timerIds = session:FetchTimerIds()
        hash = mix(hash, #timerIds)
        itemCount += #timerIds
        for _, id in ipairs(timerIds) do
            hash = mix(hash, id)
            local value = session:FetchTimerDesc(id)
            if value then
                hash = mix(hash, value.TimerId)
                hash = mix(hash, stringSize(value.TimerName))
                hash = mix(hash, value.GroupId)
                hash = mix(hash, value.Color)
                hash = mix(hash, bool(value.IsUserTimer))
            else
                hash = mix(hash, 0)
            end
        end

        local counterIds = session:FetchCounterIds()
        hash = mix(hash, #counterIds)
        itemCount += #counterIds
        for _, id in ipairs(counterIds) do
            hash = mix(hash, id)
            local value = session:FetchCounterDesc(id)
            if value then
                hash = mix(hash, value.CounterId)
                hash = mix(hash, stringSize(value.CounterName))
                hash = mix(hash, value.Parent)
                hash = mix(hash, value.Sibling)
                hash = mix(hash, value.FirstChild)
                hash = mix(hash, value.Level)
            else
                hash = mix(hash, 0)
            end
        end

        local threadIds = session:FetchThreadIds()
        hash = mix(hash, #threadIds)
        itemCount += #threadIds
        for _, id in ipairs(threadIds) do
            hash = mix(hash, id)
            local value = session:FetchThreadDesc(id)
            if value then
                hash = mix(hash, value.ThreadId)
                hash = mix(hash, stringSize(value.ThreadName))
                hash = mix(hash, value.BufferSize)
                hash = mix(hash, bool(value.IsGpu))
            else
                hash = mix(hash, 0)
            end
        end
    end
    return hash, itemCount
end

local function logWorkload(session, passes)
    local hash = 2166136261
    local itemCount = 0
    for _ = 1, passes do
        local iterator = session:CreateLogIterator()
        iterator:Configure({EmitThreadFrameEnd = true})
        local state = iterator:GetState()
        assert(state, "log iterator has no state")
        local entryCount = 0
        while iterator:Step() do
            hash = mix(hash, state:FrameId())
            hash = mix(hash, state:ThreadId())
            hash = mix(hash, state:TimerId())
            hash = mix(hash, state:EntryTypeRaw())
            hash = mix(hash, state:EventType())
            hash = mix(hash, bool(state:IsEvent()))
            local timer = session:FetchTimerDesc(state:TimerId())
            if timer then
                hash = mix(hash, timer.TimerId)
                hash = mix(hash, stringSize(timer.TimerName))
                hash = mix(hash, timer.Color)
            else
                hash = mix(hash, 0)
            end
            entryCount = (entryCount + 1) % U32_SCALE
        end
        iterator:Dispose()
        hash = mix(hash, entryCount)
        itemCount += entryCount
    end
    return hash, itemCount
end

local function openSession(capture, cache)
    local session = LibMP.Session.OpenFromMem(capture)
    assert(session and session:IsValid(), "failed to open capture")
    session:EnableObjectCache(cache)
    return session
end

local function emit(scenario, cache, checksum, itemCount, samples)
    local values = table.create(#samples)
    for index, value in ipairs(samples) do
        values[index] = tostring(value)
    end
    print(string.format("BENCH\t%s\t%s\t%.0f\t%.0f\t%s",
        scenario, cache and "on" or "off", checksum, itemCount, table.concat(values, ",")))
end

local function progress(cache, workload)
    print(string.format("PROGRESS\tcache=%s: %s", cache and "on" or "off", workload))
end

local function nowNanoseconds()
    return os.clock() * 1000000000
end

local function runShared(capture, cache, warmups, iterations, passes)
    local session = openSession(capture, cache)
    for _ = 1, warmups do
        workload(session, passes)
    end
    local samples = table.create(iterations)
    local checksum = nil
    local itemCount = nil
    for index = 1, iterations do
        local start = nowNanoseconds()
        local current, currentItemCount = workload(session, passes)
        samples[index] = math.floor(nowNanoseconds() - start + 0.5)
        assert(checksum == nil or current == checksum, "workload checksum changed")
        assert(itemCount == nil or currentItemCount == itemCount, "workload item count changed")
        checksum = current
        itemCount = currentItemCount
    end
    session:Dispose()
    emit("shared", cache, checksum, itemCount, samples)
end

local function runFresh(capture, cache, warmups, iterations, passes)
    local function run()
        local session = openSession(capture, cache)
        local checksum, itemCount = workload(session, passes)
        session:Dispose()
        return checksum, itemCount
    end
    for _ = 1, warmups do
        run()
    end
    local samples = table.create(iterations)
    local checksum = nil
    local itemCount = nil
    for index = 1, iterations do
        local start = nowNanoseconds()
        local current, currentItemCount = run()
        samples[index] = math.floor(nowNanoseconds() - start + 0.5)
        assert(checksum == nil or current == checksum, "workload checksum changed")
        assert(itemCount == nil or currentItemCount == itemCount, "workload item count changed")
        checksum = current
        itemCount = currentItemCount
    end
    emit("fresh", cache, checksum, itemCount, samples)
end

local function runLogs(capture, cache, warmups, iterations, passes)
    local session = openSession(capture, cache)
    for _ = 1, warmups do
        logWorkload(session, passes)
    end
    local samples = table.create(iterations)
    local checksum = nil
    local itemCount = nil
    for index = 1, iterations do
        local start = nowNanoseconds()
        local current, currentItemCount = logWorkload(session, passes)
        samples[index] = math.floor(nowNanoseconds() - start + 0.5)
        assert(checksum == nil or current == checksum, "log workload checksum changed")
        assert(itemCount == nil or currentItemCount == itemCount,
            "log workload item count changed")
        checksum = current
        itemCount = currentItemCount
    end
    session:Dispose()
    emit("logs", cache, checksum, itemCount, samples)
end

assert(#process.args == 8,
    "usage: benchmark.luau <capture> <warmups> <iterations> " ..
    "<metadata-passes> <log-warmups> <log-iterations> <log-passes>")
local captureFile = assert(fs.open(process.args[2], "r"))
local capture = buffer.fromstring(fs.read(captureFile))
fs.close(captureFile)
local warmups = assert(tonumber(process.args[3]))
local iterations = assert(tonumber(process.args[4]))
local metadataPasses = assert(tonumber(process.args[5]))
local logWarmups = assert(tonumber(process.args[6]))
local logIterations = assert(tonumber(process.args[7]))
local logPasses = assert(tonumber(process.args[8]))
assert(warmups >= 0 and iterations > 0 and metadataPasses > 0
    and logWarmups >= 0 and logIterations > 0 and logPasses > 0,
    "invalid benchmark count")

for _, cache in ipairs({false, true}) do
    progress(cache, "metadata / reused session")
    runShared(capture, cache, warmups, iterations, metadataPasses)
    progress(cache, "metadata / new session")
    runFresh(capture, cache, warmups, iterations, metadataPasses)
    progress(cache, "full log traversal / reused session")
    runLogs(capture, cache, logWarmups, logIterations, logPasses)
end

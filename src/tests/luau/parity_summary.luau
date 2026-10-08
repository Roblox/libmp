-- This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

--[[
    Complete Luau side of the cross-language dump parity check.

    Keep the section order and record fields aligned with parity_tool.cpp. The format uses
    decimal integers, length-prefixed hexadecimal strings, IEEE float bits, and hexadecimal
    uint64 values. This intentionally straightforward traversal can be translated to future
    language wrappers without reproducing C++ implementation details.
]]

local LibMP = require("./LibMP")
local fs = require("@std/fs")
assert(type(LibMP.GetMemReserved) == "function" and LibMP.GetMemReserved() >= 0,
    "reserved-memory query is unavailable")
assert(LibMP.Enums.DataSlot.Live == 0 and LibMP.Enums.DataSlot.Temporary == 5,
    "data-slot enum is unavailable")
assert(LibMP.Enums.ContentMode.Full == 0 and LibMP.Enums.ContentMode.CountersOnly == 1,
    "content-mode enum is unavailable")
assert(type(LibMP.Control.CaptureToBufferSync) == "function" and
    type(LibMP.Control.SetMemTargetMB) == "function" and
    type(LibMP.Control.CaptureToSlotSync) == "function" and
    type(LibMP.Control.SaveSlotToBuffer) == "function" and
    type(LibMP.Control.ResetSlot) == "function" and
    type(LibMP.Session.OpenFromSlot) == "function",
    "slot API surface is unavailable")
assert(type(LibMP.Experimental.V1.StartFileStream) == "function" and
    type(LibMP.Experimental.V1.StartMemoryStream) == "function" and
    type(LibMP.Experimental.V1.StopStream) == "function" and
    type(LibMP.Experimental.V1.SetStreamMarker) == "function" and
    type(LibMP.Experimental.V1.GetStreamStatus) == "function" and
    type(LibMP.Experimental.V1.SetContentMode) == "function",
    "experimental API surface is unavailable")
local streamStatus = LibMP.Experimental.V1.GetStreamStatus()
assert(streamStatus.Active == false and streamStatus.RecordedSizeKb == 0 and
    streamStatus.MaxSizeKb == 0 and streamStatus.ElapsedMs == 0,
    "standalone stream status must be empty")
assert(LibMP.Experimental.V1.SetContentMode(LibMP.Enums.ContentMode.CountersOnly) == false,
    "standalone content-mode control must be a no-op")
assert(LibMP.Tools:SetHtmlAssetsBaseUrl(""), "HTML asset-base setter failed")

local PATH = "@PARITY_CAPTURE@"
local SHORT_UPDATE_PATH = "@PARITY_SHORT_UPDATE@"
local LONG_UPDATE_PATH = "@PARITY_LONG_UPDATE@"
local U32_SCALE = 4294967296

local lines = {}

local function bool(value)
    return value and 1 or 0
end

local function emitFields(fields)
    for i = 1, #fields do
        fields[i] = tostring(fields[i])
    end
    lines[#lines + 1] = table.concat(fields, " ")
end

local function emit(...)
    emitFields({...})
end

local function hexU32(value)
    return string.format("%08x", value)
end

local function hexU64(value)
    local high = math.floor(value / U32_SCALE)
    local low = value - high * U32_SCALE
    return hexU32(high) .. hexU32(low)
end

local bitBuffer = buffer.create(8)

local function floatBits(value)
    buffer.writef32(bitBuffer, 0, value)
    return hexU32(buffer.readu32(bitBuffer, 0))
end

local function doubleBits(value)
    buffer.writef64(bitBuffer, 0, value)
    return hexU32(buffer.readu32(bitBuffer, 4)) .. hexU32(buffer.readu32(bitBuffer, 0))
end

local function hexBytes(value)
    if value == nil then
        return "-"
    end
    local encoded = table.create(#value)
    for i = 1, #value do
        encoded[i] = string.format("%02x", string.byte(value, i))
    end
    return tostring(#value) .. ":" .. table.concat(encoded)
end

local function readBuffer(path)
    local file = assert(fs.open(path, "r"), "failed to open capture " .. path)
    local data = fs.read(file)
    fs.close(file)
    return buffer.fromstring(data)
end

local session = LibMP.Session.OpenFromMem(readBuffer(PATH))
if not session or not session:IsValid() then
    error("parity_summary: failed to open " .. PATH)
end

assert(LibMP.Enums.AppType.Unknown == 0 and LibMP.Enums.AppType.Server == 3,
    "application-type enum is unavailable")
local tracks = session:GetTracks()
assert(tracks:Size() == session:GetTrackCount(), "track count mismatch")
tracks:Dispose()
local fetchedTracks = session:FetchTracks()
assert(#fetchedTracks == session:GetTrackCount(), "fetched track count mismatch")
if #fetchedTracks > 0 then
    local segments = session:GetSegments(0)
    assert(segments:Size() == session:GetSegmentCount(0), "segment count mismatch")
    segments:Dispose()
    assert(#session:FetchSegments(0) == session:GetSegmentCount(0),
        "fetched segment count mismatch")
    assert(session:SelectTrack(0), "failed to select first track")
    if session:GetSegmentCount(0) > 0 then
        assert(session:SelectSegment(0), "failed to select first segment")
    end
end

session:EnableLabelCache(true)
session:ClearLabelCache()

for _, format in pairs(LibMP.Enums.ContainerFormat) do
    local converted = LibMP.Tools:ConvertToBuffer(session, format)
    assert(converted and buffer.len(converted) > 0, "buffer conversion failed")
    local convertedSession = LibMP.Session.OpenFromMem(converted)
    assert(convertedSession and convertedSession:IsValid(), "converted buffer is invalid")
    assert(convertedSession:GetFrameIdMax() == session:GetFrameIdMax(),
        "converted buffer changed frame bounds")
    convertedSession:Dispose()
end
assert(session:IsValid(), "conversion invalidated its source session")

-- Closing a session detaches its iterators: they stop stepping but stay disposable
do
    local closedSession = LibMP.Session.OpenFromMem(readBuffer(PATH))
    local counters = closedSession:CreateCounterIterator()
    local log = closedSession:CreateLogIterator()
    counters:Configure({RootCounterIds = {}})
    log:Configure({})
    assert(counters:Step() and log:Step(), "iterators did not step")
    closedSession:Dispose()
    for _, iterator in {counters, log} do
        assert(not iterator:Step(), "detached iterator kept stepping")
        assert(iterator:GetState(), "detached iterator lost its state")
        iterator:Configure({})
        iterator:Rewind()
        assert(not iterator:Step(), "reconfigured detached iterator stepped")
        iterator:Dispose()
    end
end

-- Arrays read nothing once they or their session are disposed
do
    local arraySession = LibMP.Session.OpenFromMem(readBuffer(PATH))
    local counterId = arraySession:FetchCounterIds()[1]
    local samples = arraySession:GetCounterSamples(counterId)
    local released = arraySession:GetCounterSamples(counterId)
    assert(samples:Fetch(0) and released:Fetch(0), "counter samples are empty")
    released:Dispose()
    assert(released:Fetch(0) == nil, "a disposed array read its elements")
    arraySession:Dispose()
    assert(samples:Fetch(0) == nil and samples:FetchLastSample() == nil,
        "an array read its closed session")
    samples:Dispose()
end

-- Session/global data ------------------------------------------------------------------
local frameIdMin = session:GetFrameIdMin()
local frameIdMax = session:GetFrameIdMax()
emit("LIBMP_PARITY", 3)
emit("SESSION", session:GetDataFormatVersion(), LibMP.Versions.Library, frameIdMin, frameIdMax)

local global = session:FetchGlobalDesc()
if global then
    emit("GLOBAL", 1, floatBits(global.TickToMsCpu), floatBits(global.TickToMsGpu))
else
    emit("GLOBAL", 0)
end

local general = session:FetchGeneralInfo()
if general then
    emit("GENERAL", 1, hexBytes(general.PlatformInfoJson))
else
    emit("GENERAL", 0)
end

-- Groups -------------------------------------------------------------------------------
local groupIdMax = session:GetGroupIdMax()
emit("GROUPS", groupIdMax)
for queryId = 1, groupIdMax do
    local group = session:FetchGroupDesc(queryId)
    if group then
        emit("GROUP", queryId, 1, group.GroupId, hexBytes(group.GroupName),
            hexU32(group.Color), bool(group.IsGpu))
    else
        emit("GROUP", queryId, 0)
    end
end

-- Timers and group membership ----------------------------------------------------------
local timerIds = session:FetchTimerIds()
emit("TIMER_IDS", #timerIds)
for index, id in ipairs(timerIds) do
    emit("TIMER_ID", index - 1, id)
    local timer = session:FetchTimerDesc(id)
    if timer then
        emit("TIMER", id, 1, timer.TimerId, hexBytes(timer.TimerName), timer.GroupId,
            hexU32(timer.Color), bool(timer.IsUserTimer))
    else
        emit("TIMER", id, 0)
    end
end

for groupId = 1, groupIdMax do
    local ids = session:FetchTimerIdsByGroupId(groupId)
    local fields = {"GROUP_TIMER_IDS", groupId, #ids}
    for _, id in ipairs(ids) do
        fields[#fields + 1] = id
    end
    emitFields(fields)
end

-- Counters and every stored sample -----------------------------------------------------
local counterIds = session:FetchCounterIds()
emit("COUNTER_IDS", #counterIds)
for index, id in ipairs(counterIds) do
    emit("COUNTER_ID", index - 1, id)
    local counter = session:FetchCounterDesc(id)
    if counter then
        emit("COUNTER", id, 1, counter.CounterId, hexBytes(counter.CounterName),
            counter.Parent, counter.Sibling, counter.FirstChild, counter.Level)

        local samples = session:FetchCounterSamples(id)
        emit("COUNTER_SAMPLES", id, #samples)
        for sampleIndex, sample in ipairs(samples) do
            emit("COUNTER_SAMPLE", id, sampleIndex - 1, 1,
                sample.FrameId, doubleBits(sample.Value))
        end

        local last = session:FetchLastCounterSample(id)
        if last then
            emit("COUNTER_LAST", id, 1, last.FrameId, doubleBits(last.Value))
        else
            emit("COUNTER_LAST", id, 0)
        end
    else
        emit("COUNTER", id, 0)
    end
end

-- Threads ------------------------------------------------------------------------------
emit("GPU_THREAD", session:GetThreadIdGpu())
local threadIds = session:FetchThreadIds()
emit("THREAD_IDS", #threadIds)
for index, id in ipairs(threadIds) do
    emit("THREAD_ID", index - 1, id)
    local thread = session:FetchThreadDesc(id)
    if thread then
        emit("THREAD", id, 1, thread.ThreadId, hexBytes(thread.ThreadName),
            thread.BufferSize, bool(thread.IsGpu))
    else
        emit("THREAD", id, 0)
    end
end

-- Frames and their embedded ThreadLogDescr vectors ------------------------------------
local frameCount = frameIdMax >= frameIdMin and (frameIdMax - frameIdMin + 1) or 0
emit("FRAMES", frameCount)
for offset = 0, frameCount - 1 do
    local queryId = frameIdMin + offset
    local frame = session:FetchFrameDesc(queryId)
    if frame then
        local threadLogs = frame.ThreadLogs or {}
        emit("FRAME", queryId, 1, frame.FrameId, frame.FrameAbsoluteId,
            hexU64(frame.TickStartCpu), hexU64(frame.TickEndCpu),
            hexU64(frame.TickStartGpu), hexU64(frame.TickEndGpu),
            hexU64(frame.LabelOffsetMin), hexU64(frame.LabelOffsetMax),
            bool(frame.IsPaused), bool(frame.IsIncomplete), #threadLogs)
        for logIndex, log in ipairs(threadLogs) do
            emit("FRAME_THREAD_LOG", queryId, logIndex - 1, log.ThreadId, log.LogEntriesNum,
                hexU64(log.LogStartLocator), bool(log.IsRemoved))
        end
    else
        emit("FRAME", queryId, 0)
    end
end

-- Optional U64 sample families ---------------------------------------------------------
local function writeU64Samples(name, samples)
    emit(name .. "_SAMPLES", #samples)
    for index, sample in ipairs(samples) do
        emit(name .. "_SAMPLE", index - 1, 1, sample.FrameId, hexU64(sample.Value))
    end
end

writeU64Samples("PLACE_ID", session:FetchPlaceIdSamples())
writeU64Samples("UTC", session:FetchUtcTimestampSamples())

-- Public name lookup and filtering -----------------------------------------------------
local findProbes = {
    {Mask = "*", CaseSensitive = false},
    {Mask = "*", CaseSensitive = true},
    {Mask = "*A*", CaseSensitive = false},
    {Mask = "*A*", CaseSensitive = true},
    {Mask = "__libmp_parity_missing__", CaseSensitive = false},
}

emit("FIND_QUERIES", #findProbes)
local function writeFindFamily(family, findFirst, findAll)
    for probeIndex, probe in ipairs(findProbes) do
        local ids = findAll(probe.Mask, probe.CaseSensitive)
        local fields = {"FIND", family, probeIndex - 1, bool(probe.CaseSensitive),
            hexBytes(probe.Mask), findFirst(probe.Mask, probe.CaseSensitive), #ids}
        for _, id in ipairs(ids) do
            fields[#fields + 1] = id
        end
        emitFields(fields)
    end
end

writeFindFamily("GROUP",
    function(mask, sensitive) return session:FindGroupId(mask, sensitive) end,
    function(mask, sensitive) return session:FindGroupIds(mask, sensitive) end)
writeFindFamily("TIMER",
    function(mask, sensitive) return session:FindTimerId(mask, sensitive) end,
    function(mask, sensitive) return session:FindTimerIds(mask, sensitive) end)
writeFindFamily("COUNTER",
    function(mask, sensitive) return session:FindCounterId(mask, sensitive) end,
    function(mask, sensitive) return session:FindCounterIds(mask, sensitive) end)
writeFindFamily("THREAD",
    function(mask, sensitive) return session:FindThreadId(mask, sensitive) end,
    function(mask, sensitive) return session:FindThreadIds(mask, sensitive) end)

emit("GROUP_NAME_TIMER_QUERIES", #findProbes)
for probeIndex, probe in ipairs(findProbes) do
    local ids = session:FetchTimerIdsByGroupNames(probe.Mask, probe.CaseSensitive)
    local fields = {"GROUP_NAME_TIMERS", probeIndex - 1, bool(probe.CaseSensitive),
        hexBytes(probe.Mask), #ids}
    for _, id in ipairs(ids) do
        fields[#fields + 1] = id
    end
    emitFields(fields)
end

-- Complete counter traversal -----------------------------------------------------------
emit("COUNTER_ITERATOR_BEGIN")
do
    local iterator = session:CreateCounterIterator()
    local state = iterator:GetState()
    iterator:Configure({RootCounterIds = {}})
    local reportIndex = 0
    while iterator:Step() do
        emit("CI", reportIndex, bool(state:Started()), bool(state:Finished()),
            state:CounterId(), state:ParentId(), state:Level(), state:RelativeLevel(),
            state:RootLevel(), state:IndexInSiblings(), bool(state:IsLeaf()),
            bool(state:HierarchyChanged()), bool(state:CounterStackIsOverflowed()))
        reportIndex += 1
    end
    emit("COUNTER_ITERATOR_END", reportIndex, bool(state:Finished()))
    iterator:Dispose()
end

-- Every public LogIteratorState report -------------------------------------------------
emit("LOG_ITERATOR_BEGIN")
do
    local iterator = session:CreateLogIterator()
    local state = iterator:GetState()
    iterator:Configure({EmitThreadFrameEnd = true, TrackScopeLabels = true})
    local reportIndex = 0
    local checkedEntryParts = false
    local checkedLabelApis = false
    while iterator:Step() do
        local entryCount = iterator:GetCurrentEntryCount()
        if not state:IsLabel() then
            assert(iterator:FetchCurrentLabel() == nil,
                "non-label entry exposed label text")
            if not checkedLabelApis then
                assert(iterator:GetCurrentScopeLabelCount() == 0)
                assert(iterator:GetScopeLabelCount(state:ThreadId(), 0) == 0)
                assert(iterator:FetchCurrentScopeLabel(0) == nil)
                assert(iterator:FetchScopeLabel(state:ThreadId(), 0, 0) == nil)
                checkedLabelApis = true
            end
        end
        if not checkedEntryParts and entryCount > 0 then
            for entryIndex = 0, entryCount - 1 do
                assert(type(iterator:GetCurrentEntryPart(entryIndex, 0)) == "number")
                assert(type(iterator:GetCurrentEntryPart(entryIndex, 1)) == "number")
            end
            checkedEntryParts = true
        end
        emit("LI", reportIndex, bool(state:Started()), bool(state:Finished()),
            state:ThreadStackDepth(), bool(state:ThreadStackIsUnderflowed()),
            bool(state:ThreadStackIsOverflowed()), bool(state:ThreadStackWasOverflowed()),
            hexU64(state:ThreadPrevTimestamp()), state:FrameId(), state:FrameAbsoluteId(),
            hexU64(state:FrameDescLocator()), state:ThreadId(), bool(state:IsGpuThread()),
            bool(state:IsFrameStitching()), bool(state:FrameChanged()), bool(state:ThreadChanged()),
            hexU64(state:EntryLocator()), state:TimerId(), hexU64(state:Timestamp()),
            hexU64(state:TimestampGpu()), state:EntryTypeRaw(), state:EventType(),
            bool(state:IsUnknown()), bool(state:IsEnter()), bool(state:IsExit()),
            bool(state:IsLabel()), bool(state:IsEvent()), bool(state:IsThreadFrameEnd()),
            bool(state:IsFrameBoundary()), entryCount)
        reportIndex += 1
    end
    assert(checkedEntryParts, "log iterator has no physical entry")
    assert(checkedLabelApis, "log iterator has no non-label entry")
    emit("LOG_ITERATOR_END", reportIndex, bool(state:Finished()))
    iterator:Dispose()
end

emit("END")
print(table.concat(lines, "\n"))

-- Wrapper cache checks -----------------------------------------------------------------
local function checkCachedDescriptor(label, firstId, secondId, get, fetch, getValue, fetchValue)
    local firstView = get(firstId)
    local repeatedView = get(firstId)
    assert(firstView and rawequal(firstView, repeatedView), label .. ": Get identity miss")
    local expectedViewValue = getValue(firstView)

    if secondId ~= nil then
        get(secondId)
        assert(rawequal(firstView, get(firstId)), label .. ": interleaved Get identity miss")
    end

    local firstCopy = fetch(firstId)
    local repeatedCopy = fetch(firstId)
    assert(firstCopy and rawequal(firstCopy, repeatedCopy), label .. ": Fetch identity miss")
    local expectedCopyValue = fetchValue(firstCopy)
    assert(expectedViewValue == expectedCopyValue, label .. ": Get/Fetch value mismatch")
    if secondId ~= nil then
        fetch(secondId)
        assert(rawequal(firstCopy, fetch(firstId)), label .. ": interleaved Fetch identity miss")
    end

    session:ClearObjectCache()
    local clearedView = get(firstId)
    local clearedCopy = fetch(firstId)
    assert(not rawequal(firstView, clearedView), label .. ": ClearObjectCache kept Get wrapper")
    assert(not rawequal(firstCopy, clearedCopy), label .. ": ClearObjectCache kept Fetch table")
    assert(getValue(clearedView) == expectedViewValue, label .. ": cleared Get changed value")
    assert(fetchValue(clearedCopy) == expectedCopyValue, label .. ": cleared Fetch changed value")
end

session:EnableObjectCache(true)
if global then
    checkCachedDescriptor("GlobalDesc", nil, nil,
        function() return session:GetGlobalDesc() end,
        function() return session:FetchGlobalDesc() end,
        function(value) return floatBits(value:TickToMsCpu()) end,
        function(value) return floatBits(value.TickToMsCpu) end)
end
if general then
    checkCachedDescriptor("GeneralInfo", nil, nil,
        function() return session:GetGeneralInfo() end,
        function() return session:FetchGeneralInfo() end,
        function(value) return value:PlatformInfoJson() or "" end,
        function(value) return value.PlatformInfoJson or "" end)
end
local groupIds = {}
for id = 1, groupIdMax do
    if session:FetchGroupDesc(id) then
        groupIds[#groupIds + 1] = id
    end
end
if #groupIds > 0 then
    checkCachedDescriptor("GroupDesc", groupIds[1], groupIds[2],
        function(id) return session:GetGroupDesc(id) end,
        function(id) return session:FetchGroupDesc(id) end,
        function(value) return value:GroupId() .. ":" .. (value:GroupName() or "") end,
        function(value) return value.GroupId .. ":" .. (value.GroupName or "") end)
end
if #timerIds > 0 then
    checkCachedDescriptor("TimerDesc", timerIds[1], timerIds[2],
        function(id) return session:GetTimerDesc(id) end,
        function(id) return session:FetchTimerDesc(id) end,
        function(value) return value:TimerId() .. ":" .. (value:TimerName() or "") end,
        function(value) return value.TimerId .. ":" .. (value.TimerName or "") end)
end
if #counterIds > 0 then
    checkCachedDescriptor("CounterDesc", counterIds[1], counterIds[2],
        function(id) return session:GetCounterDesc(id) end,
        function(id) return session:FetchCounterDesc(id) end,
        function(value) return value:CounterId() .. ":" .. (value:CounterName() or "") end,
        function(value) return value.CounterId .. ":" .. (value.CounterName or "") end)
end
if #threadIds > 0 then
    checkCachedDescriptor("ThreadDesc", threadIds[1], threadIds[2],
        function(id) return session:GetThreadDesc(id) end,
        function(id) return session:FetchThreadDesc(id) end,
        function(value) return value:ThreadId() .. ":" .. (value:ThreadName() or "") end,
        function(value) return value.ThreadId .. ":" .. (value.ThreadName or "") end)
end

-- Native synchronization reports invalidated zero-based bucket IDs through the
-- host callback. Force one sync to verify both wrapper bucket mappings.
if global and #timerIds > 0 then
    local shortTermCached = session:FetchGlobalDesc()
    local longTermCached = session:FetchTimerDesc(timerIds[1])
    assert(rawequal(session:FetchGlobalDesc(), shortTermCached),
        "short-term object cache did not reuse a fetched table")
    assert(rawequal(session:FetchTimerDesc(timerIds[1]), longTermCached),
        "long-term object cache did not reuse a fetched table")
    session:SetClearCacheOnSyncRequired()
    session:SetSyncRequired()
    assert(not rawequal(session:FetchGlobalDesc(), shortTermCached),
        "sync callback did not clear the short-term wrapper cache bucket")
    assert(not rawequal(session:FetchTimerDesc(timerIds[1]), longTermCached),
        "sync callback did not clear the long-term wrapper cache bucket")
end

if SHORT_UPDATE_PATH ~= "" and LONG_UPDATE_PATH ~= "" then
    local provider = {
        buf = readBuffer(PATH),
        open = function() return true end,
        close = function() return true end,
        getSize = function(self) return buffer.len(self.buf) end,
        read = function(self, dstBuf, dstOffset, blockId, offset, size)
            if blockId ~= 0 or offset < 0 or size < 0 or
                offset > buffer.len(self.buf) or size > buffer.len(self.buf) - offset then
                return 0
            end
            buffer.copy(dstBuf, dstOffset, self.buf, offset, size)
            return size
        end,
    }
    local relatedSession = LibMP.Session.OpenFromExternalProvider(provider, 0)
    assert(relatedSession and relatedSession:IsValid(), "related-snapshot session failed")
    local relatedTimerIds = relatedSession:FetchTimerIds()
    assert(#relatedTimerIds > 0, "related-snapshot timer array is empty")
    local relatedTimerId = relatedTimerIds[1]
    local baselineGlobal = relatedSession:FetchGlobalDesc()
    local baselineGeneral = relatedSession:FetchGeneralInfo()
    local baselineTimer = relatedSession:FetchTimerDesc(relatedTimerId)

    provider.buf = readBuffer(SHORT_UPDATE_PATH)
    relatedSession:SetSyncRequired()
    local shortGeneral = relatedSession:FetchGeneralInfo()
    local shortGlobal = relatedSession:FetchGlobalDesc()
    local timerAfterShortUpdate = relatedSession:FetchTimerDesc(relatedTimerId)
    assert(shortGeneral.PlatformInfoJson ~= baselineGeneral.PlatformInfoJson,
        "short-term fixture did not change its metadata")
    assert(not rawequal(shortGlobal, baselineGlobal),
        "short-term update did not invalidate the short-term cache bucket")
    assert(rawequal(timerAfterShortUpdate, baselineTimer),
        "short-term update invalidated the long-term cache bucket")

    provider.buf = readBuffer(LONG_UPDATE_PATH)
    relatedSession:SetSyncRequired()
    local generalAfterLongUpdate = relatedSession:FetchGeneralInfo()
    local globalAfterLongUpdate = relatedSession:FetchGlobalDesc()
    local longTimer = relatedSession:FetchTimerDesc(relatedTimerId)
    assert(rawequal(generalAfterLongUpdate, shortGeneral) and
        rawequal(globalAfterLongUpdate, shortGlobal),
        "long-term update invalidated the short-term cache bucket")
    assert(not rawequal(longTimer, timerAfterShortUpdate),
        "long-term update did not invalidate the long-term cache bucket")
    assert(longTimer.TimerName ~= timerAfterShortUpdate.TimerName,
        "long-term fixture did not change its metadata")
    relatedSession:Dispose()
end

-- Disabling the cache must stop wrapper/table identity reuse without changing values.
if #threadIds > 0 then
    session:EnableObjectCache(false)
    local view1 = session:GetThreadDesc(threadIds[1])
    local view2 = session:GetThreadDesc(threadIds[1])
    local copy1 = session:FetchThreadDesc(threadIds[1])
    local copy2 = session:FetchThreadDesc(threadIds[1])
    assert(view1 and view2 and not rawequal(view1, view2), "disabled cache reused Get wrapper")
    assert(copy1 and copy2 and not rawequal(copy1, copy2), "disabled cache reused Fetch table")
    assert(view1:ThreadId() == view2:ThreadId() and copy1.ThreadId == copy2.ThreadId,
        "disabled cache changed descriptor value")
    session:EnableObjectCache(true)
end

-- Array wrappers are independent handles and must expose the same ordered values.
local timerArray1 = session:GetTimerIds()
local timerArray2 = session:GetTimerIds()
assert(not rawequal(timerArray1, timerArray2), "array wrappers unexpectedly share identity")
assert(timerArray1:Size() == timerArray2:Size(), "array wrapper sizes differ")
if timerArray1:Size() > 0 then
    assert(timerArray1:Get(0) == timerArray2:Get(0), "array wrapper values differ")
end
timerArray1:Dispose()
timerArray2:Dispose()

session:Dispose()

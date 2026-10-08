-- This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

-- LibMP for Luau
-- Access Roblox MicroProfiler data in Roblox Engine and standalone Lute CLI programs.
-- %LIBMP_SHARED_METADATA%
--
-- Requirements:
-- - Roblox Engine: install LibMP as a ModuleScript in ReplicatedStorage.
-- - Lute CLI: place LibMP.luau beside the script.
--
-- Minimal usage in Roblox Engine:
--   local LibMP = require("@game/ReplicatedStorage/LibMP")
--   local session = LibMP.Session.OpenFromLiveData()
--   if session then session:Dispose() end
--
-- Minimal usage in Lute CLI:
--   local LibMP = require("./LibMP")
--   local session = LibMP.Session.OpenFromFile("capture.x.html") -- or "capture.gprx"
--   if session then session:Dispose() end

local LibMpCoreFactory = require("./LibMpCore")
local MpTypes = require("./LibMpTypes")
local LibMP = {}

local isCliMode = (typeof(game) == "nil")
local fs = isCliMode and require("@std/fs") or nil
local process = isCliMode and require("@lute/process") or nil

local W
local W_mem

local UnpackCache = {}

local sessionIdCounter = 0;
local sessionIdRegistry = {}
local sessionPtrRegistry = {}
local activeToolOutput = nil
local activeToolOutputError = nil

local BLOCK_MARKER_BIT = 2^52
local SHIFT_32_BITS = 2^32
local MAX_PACKED_BLOCK_ID = 2^20 - 1
local MAX_ABSOLUTE_OFFSET_HIGH = 2^20 - 1
local MAX_EXTERNAL_DATA_SIZE = 2^52
local LIVE_SLOT_ID = MpTypes.DataSlot.Live
local TEMPORARY_SLOT_ID = MpTypes.DataSlot.Temporary

local function getMemory()
    return W["memory"][1]
end

local function W_mem_update()
    W_mem = getMemory()
    MpTypes.FlatBuffers.SetGlobalBuffer(W_mem)
end

local DataChangedFlags = table.freeze({
    CaptureData = bit32.lshift(1, 0),
})

local SessionNotificationFlags = table.freeze({
    LabelRegionsChanged = bit32.lshift(1, 0),
    LabelCacheReset = bit32.lshift(1, 1),
})

local Enums = table.freeze({
    AppType = table.freeze(MpTypes.AppType),
    ContainerFormat = table.freeze(MpTypes.ContainerFormat),
    ContentMode = table.freeze(MpTypes.ContentMode),
    DataSlot = table.freeze(MpTypes.DataSlot),
})

if not isCliMode then
    MicroProfilerService = game:GetService("MicroProfilerService")

    MicroProfilerService.DataChanged:Connect(function(slotId, flags)
        if bit32.band(flags, DataChangedFlags.CaptureData) ~= 0 then
            for _, session in pairs(sessionIdRegistry) do
                if session.io.slotId == slotId then
                    session:SetSyncRequired()
                end
            end
        end
    end)
end

local function _fullSyncAllSessions()
    for _, session in pairs(sessionIdRegistry) do
        if session.io.slotId ~= nil then 
            session:SetClearCacheOnSyncRequired()
            session:SetSyncRequired()
        end
    end
end

local function _ToInt32(value)
    -- If the WASM function result has the 32nd bit set, it was meant to be a negative int32
    if value >= 0x80000000 then
        return value - 0x100000000
    end
    return value
end

local function _arrayFetchAllAndDispose(arr)
    local res = arr:FetchAll()
    arr:Dispose()
    return res
end

-- =============================================================================
-- INITIALIZE WASM
-- =============================================================================

local imports = {
    env = {
        emscripten_notify_memory_growth = function(index)
            W_mem_update()
        end,

        LibMpExtConsoleLog = function(logLevel, strPtr, len)
            local s = buffer.readstring(W_mem, strPtr, len)
            if logLevel >= 2 then
                warn(s)
            else
                print(s)
            end
        end,

        LibMpExtControlMsg = function(msgBufPtr, msgSz, respBufPtr, respMaxSz)
            return MicroProfilerService:ProcessCommand(W_mem, msgBufPtr, msgSz, W_mem, respBufPtr, respMaxSz)
        end,

        LibMpExtToolOutput = function(dataPtr, size)
            if activeToolOutput == nil then
                return 0
            end
            local success, result = pcall(activeToolOutput, dataPtr, size)
            if not success then
                activeToolOutputError = result
                return 0
            end
            return result == false and 0 or 1
        end,

        LibMpExtClearObjCacheCallback = function(handle, bucketIdsPtr, bucketCount)
            -- handle contains a session ptr
            local session = sessionPtrRegistry[handle]
            if session then
                for i = 0, bucketCount - 1 do
                    -- Convert 0-based C API bucket ID to 1-based Luau index 
                    local bucketId = buffer.readu32(W_mem, bucketIdsPtr + (i * 4))
                    session:_ClearCacheBucket(bucketId + 1)
                end
            end
        end,

        LibMpExtSessionNotificationCallback = function(handle, notificationFlags)
            local session = sessionPtrRegistry[handle]
            if session then
                local clearAll = bit32.band(
                    notificationFlags,
                    SessionNotificationFlags.LabelCacheReset) ~= 0
                if clearAll or bit32.band(
                    notificationFlags,
                    SessionNotificationFlags.LabelRegionsChanged) ~= 0 then
                    session:_InvalidateLabelCache(clearAll)
                end
            end
        end,

        GprxExtDataOpen = function(handle)
            local session = sessionIdRegistry[handle]
            if session then
                return session.io:open() and 1 or 0
            end
            return 0
        end,

        GprxExtDataClose = function(handle)
            local session = sessionIdRegistry[handle]
            if session then
                return session.io:close() and 1 or 0
            end
            return 0
        end,

        GprxExtDataGetSize = function(handle, outSizeLowPtr, outSizeHighPtr)
            local session = sessionIdRegistry[handle]
            if not session then
                return 0
            end
            local size = session.io:getSize()
            if size < 0 or size > MAX_EXTERNAL_DATA_SIZE then
                -- Invalid external data size: it exceeds the 52-bit Luau provider limit.
                return 0
            end
            buffer.writeu32(W_mem, outSizeLowPtr, size % SHIFT_32_BITS)
            buffer.writeu32(W_mem, outSizeHighPtr, math.floor(size / SHIFT_32_BITS))
            return 1
        end,

        GprxExtDataRead = function(handle, dst, block_id, offsetLow, offsetHigh, sz)
            local session = sessionIdRegistry[handle]
            if session then
                if block_id == 0 then
                    if offsetHigh > MAX_ABSOLUTE_OFFSET_HIGH then
                        -- Invalid absolute offset: it overlaps the packed block marker.
                        return 0
                    end
                elseif offsetHigh ~= 0 then
                    -- Invalid block-relative offset: it exceeds 32 bits.
                    return 0
                end
                local offset = offsetLow + offsetHigh * SHIFT_32_BITS
                return session.io:read(W_mem, dst, block_id, offset, sz)
            end
            return 0
        end,
    },
    
    wasi_snapshot_preview1 = {
        proc_exit = function(c) error("WASM Exit Code: " .. tostring(c)) end,
        fd_write = function() return 0 end,
        fd_close = function() return 0 end,
        fd_seek = function() return 0 end,
        fd_read = function() return 0 end,
        environ_sizes_get = function() return 0 end,
        environ_get = function() return 0 end,
    }
}

W = LibMpCoreFactory(imports)
W_mem_update()

local LABEL_CACHE_CHUNK_SIZE = W.MpInternalGetLabelCacheChunkSize()

-- =============================================================================
-- HELPERS
-- =============================================================================

-- Attaches a GC proxy to an object, calling its :Dispose() method on garbage collection.
-- Not implemented in Luau.
local function withGC(obj)
    return obj
end

-- Wrap a pointer into a FlatBuffer Table View (Simulates getRootAs)
local function wrapTable(ptr, typeClass)
    if not ptr or ptr == 0 then return nil end
    local offset = buffer.readi32(W_mem, ptr) -- Read preamble offset
    local obj = typeClass.New()
    obj:Init(W_mem, ptr + offset)
    return obj
end

-- Wrap a pointer directly into a FlatBuffer Struct View
local function wrapStruct(ptr, typeClass)
    if not ptr or ptr == 0 then return nil end
    local obj = typeClass.New()
    obj:Init(W_mem, ptr)
    return obj
end

-- Universal helper to pack a Lua table into a FlatBuffer object
local function PackObject(builder, TypeClass, data)
    local vectorOffsets = {}

    -- Phase 1: Pre-pack Vectors (Must be done before starting the object)
    for key, value in pairs(data) do
        if type(value) == "table" then
            local startVecMethod = TypeClass["Start" .. key .. "Vector"]
            local addVecMethod = TypeClass["Add" .. key]

            if startVecMethod and addVecMethod then
                local count = #value
                startVecMethod(builder, count)
                -- Assume generic number arrays are Uint32 (IDs)
                for i = count, 1, -1 do
                    local elem = value[i]
                    if type(elem) == "number" then
                        builder:PrependUint32(elem)
                    elseif type(elem) == "boolean" then
                        builder:PrependBool(elem)
                    else
                        error("PackObject: Unsupported vector element type for key: " .. key)
                    end
                end
                vectorOffsets[key] = builder:EndVector(count)
            end
        end
    end

    -- Phase 2: Start Object and Add Fields
    TypeClass.Start(builder)

    for key, value in pairs(data) do
        local addMethod = TypeClass["Add" .. key]
        if addMethod then
            if type(value) == "table" then
                if vectorOffsets[key] then
                    addMethod(builder, vectorOffsets[key])
                end
            else
                addMethod(builder, value)
            end
        end
    end

    return TypeClass.End(builder)
end

-- =============================================================================
-- REFLECTION & UNPACKING
-- =============================================================================

-- Recursive function to convert FlatBuffers objects to Lua tables
local function Unpack(obj)
    -- Base case: Primitive values or nil
    if type(obj) ~= "table" and type(obj) ~= "userdata" then
        return obj
    end

    -- Get Metatable
    local mt = getmetatable(obj)
    if not mt then return obj end

    -- If the metatable is a wrapper { __index = {Methods...} }, unwrap it.
    -- The generated types use setmetatable(o, {__index = mt}), so we need mt.__index.
    if type(mt) == "table" and type(mt.__index) == "table" then
        mt = mt.__index
    end

    -- Check Cache (using the actual method table as key)
    local schema = UnpackCache[mt]
    
    -- Build Schema on first run
    if not schema then
        schema = { scalars = {}, vectors = {} }
        local vectorKeys = {}

        -- Pass 1: Detect Vectors via "Length" suffix
        for k, v in pairs(mt) do
            if type(v) == "function" and type(k) == "string" then
                if k:sub(-6) == "Length" then
                    local baseName = k:sub(1, -7)
                    -- Check if the corresponding getter exists (e.g. ThreadIds for ThreadIdsLength)
                    if mt[baseName] then
                        table.insert(schema.vectors, { name = baseName, lenFunc = k })
                        vectorKeys[baseName] = true
                        vectorKeys[k] = true
                    end
                end
            end
        end

        -- Pass 2: Detect Scalars (Everything else that isn't internal)
        for k, v in pairs(mt) do
            if type(v) == "function" and type(k) == "string" then
                -- Filter out Init, internal meta-methods, and vector accessors found in Pass 1
                if k ~= "Init" and not k:match("^__") and not vectorKeys[k] then
                    table.insert(schema.scalars, k)
                end
            end
        end
        
        UnpackCache[mt] = schema
    end

    -- Execute Unpack
    local result = {}
    
    -- 1. Scalars
    for _, name in ipairs(schema.scalars) do
        local val = obj[name](obj)
        result[name] = Unpack(val)
    end
    
    -- 2. Vectors
    for _, vecInfo in ipairs(schema.vectors) do
        local name = vecInfo.name
        local len = obj[vecInfo.lenFunc](obj)
        local list = table.create(len)
        for i = 1, len do
            -- FlatBuffers Lua vectors use 1-based index in the wrappers
            local val = obj[name](obj, i)
            table.insert(list, Unpack(val))
        end
        result[name] = list
    end
    
    return result
end

-- =============================================================================
-- ARRAY CLASSES
-- =============================================================================

local ArrayBase = {}
ArrayBase.__index = ArrayBase

function ArrayBase.new(session, arrayPtr)
    local self = setmetatable({}, ArrayBase)
    self.session = session
    self.arrayPtr = arrayPtr
    return withGC(self)
end

-- Element getters take the session; after its Dispose they get 0 and return nil
local function arraySessionPtr(array)
    return array.session.ptr or 0
end

function ArrayBase:Dispose()
    if self.arrayPtr ~= 0 then
        W.MpReleaseArray(self.arrayPtr)
        self.arrayPtr = 0
    end
end

function ArrayBase:Size()
    return (self.arrayPtr ~= 0) and W.MpGetArraySize(self.arrayPtr) or 0
end

function ArrayBase:FetchAll()
    local count = self:Size()
    local result = table.create(count)
    -- C-API uses 0-based indexing
    for i = 0, count - 1 do
        result[i + 1] = self:Fetch(i)
    end
    return result
end

local ScalarArray = setmetatable({}, ArrayBase)
ScalarArray.__index = ScalarArray

function ScalarArray.new(session, arrayPtr, getterFunc)
    local self = ArrayBase.new(session, arrayPtr)
    setmetatable(self, ScalarArray)
    self.getter = getterFunc
    return self
end

function ScalarArray:Get(index)
    return self.getter(arraySessionPtr(self), self.arrayPtr, index)
end

function ScalarArray:Fetch(index)
    return self:Get(index)
end

local TableArray = setmetatable({}, ArrayBase)
TableArray.__index = TableArray

function TableArray.new(session, arrayPtr, getterFunc, typeClass)
    local self = ArrayBase.new(session, arrayPtr)
    setmetatable(self, TableArray)
    self.getter = getterFunc
    self.typeClass = typeClass
    return self
end

function TableArray:Get(index)
    local ptr = self.getter(arraySessionPtr(self), self.arrayPtr, index)
    return wrapTable(ptr, self.typeClass)
end

function TableArray:Fetch(index)
    return Unpack(self:Get(index))
end

local StructArray = setmetatable({}, ArrayBase)
StructArray.__index = StructArray

function StructArray.new(session, arrayPtr, getterFunc, typeClass)
    local self = ArrayBase.new(session, arrayPtr)
    setmetatable(self, StructArray)
    self.getter = getterFunc
    self.typeClass = typeClass
    return self
end

function StructArray:Get(index)
    local ptr = self.getter(arraySessionPtr(self), self.arrayPtr, index)
    return wrapStruct(ptr, self.typeClass)
end

function StructArray:Fetch(index)
    return Unpack(self:Get(index))
end

local CounterSampleArray = setmetatable({}, ArrayBase)
CounterSampleArray.__index = CounterSampleArray

function CounterSampleArray.new(session, arrayPtr)
    local self = ArrayBase.new(session, arrayPtr)
    setmetatable(self, CounterSampleArray)
    return self
end

function CounterSampleArray:Get(index)
    local ptr = W.MpGetCounterSampleFromArray(arraySessionPtr(self), self.arrayPtr, index)
    return wrapStruct(ptr, MpTypes.CounterSample)
end

function CounterSampleArray:Fetch(index)
    return Unpack(self:Get(index))
end

function CounterSampleArray:GetSampleByFrameId(frameId)
    local ptr = W.MpGetCounterSampleFromArrayByFrameId(arraySessionPtr(self), self.arrayPtr, frameId)
    return wrapStruct(ptr, MpTypes.CounterSample)
end

function CounterSampleArray:FetchSampleByFrameId(frameId)
    return Unpack(self:GetSampleByFrameId(frameId))
end

function CounterSampleArray:GetLastSample()
    local ptr = W.MpGetLastCounterSampleFromArray(arraySessionPtr(self), self.arrayPtr)
    return wrapStruct(ptr, MpTypes.CounterSample)
end

function CounterSampleArray:FetchLastSample()
    return Unpack(self:GetLastSample())
end

-- =============================================================================
-- LOG ITERATOR CLASS
-- =============================================================================

local LogIterator = {}
LogIterator.__index = LogIterator

function LogIterator.new(sessionOrHandle)
    local self = setmetatable({}, LogIterator)
    if type(sessionOrHandle) == "table" then
        self.session = sessionOrHandle
        self.handle = sessionOrHandle.ptr
    else
        self.session = nil
        self.handle = sessionOrHandle
    end
    self.iterPtr = W.MpCreateLogIterator(self.handle)
    return withGC(self)
end

function LogIterator:Dispose()
    if self.iterPtr ~= 0 then
        W.MpDeleteLogIterator(self.iterPtr)
        self.iterPtr = 0
    end
end

function LogIterator:Configure(configObj)
    local builder = MpTypes.FlatBuffers.Builder(1024)
    local rootOffset = PackObject(builder, MpTypes.LogIteratorConfig, configObj)
    builder:Finish(rootOffset)
    
    local luaBuf = builder:Output(false)
    local bufLen = buffer.len(luaBuf)

    local destPtr = W.MpGetLogIteratorConfigBuffer(self.iterPtr, bufLen)
    buffer.copy(W_mem, destPtr, luaBuf, 0, bufLen)

    -- useDefault = false = 0
    W.MpConfigureLogIterator(self.iterPtr, 0)
end

function LogIterator:Step()
    return W.MpStepLogIterator(self.iterPtr) ~= 0
end

function LogIterator:Rewind()
    W.MpRewindLogIterator(self.iterPtr)
end

function LogIterator:RewindTo(frameIdMin, frameIdMax)
    W.MpRewindLogIteratorTo(self.iterPtr, frameIdMin, frameIdMax)
end

function LogIterator:GetState()
    local ptr = W.MpGetLogIteratorState(self.iterPtr)
    return wrapStruct(ptr, MpTypes.LogIteratorState)
end

function LogIterator:GetThreadStackState(threadId)
    local ptr = W.MpGetLogIteratorThreadStackState(self.iterPtr, threadId)
    return wrapStruct(ptr, MpTypes.LogIteratorThreadStackState)
end

function LogIterator:GetThreadStackElement(threadId, stackLevel)
    local ptr = W.MpGetLogIteratorThreadStackElement(self.iterPtr, threadId, stackLevel)
    return wrapStruct(ptr, MpTypes.LogIteratorThreadStackElement)
end

function LogIterator:GetCurrentThreadStackElement(stackLevel)
    local ptr = W.MpGetLogIteratorCurrentThreadStackElement(self.iterPtr, stackLevel)
    return wrapStruct(ptr, MpTypes.LogIteratorThreadStackElement)
end

function LogIterator:GetCurrentEntryCount()
    return W.MpGetLogIteratorCurrentEntryCount(self.iterPtr)
end

function LogIterator:GetCurrentEntryPart(entryIndex, partIndex)
    return W.MpGetLogIteratorCurrentEntryPart(self.iterPtr, entryIndex, partIndex)
end

local function fetchLabelOffset(offsetPtr)
    if not offsetPtr or offsetPtr == 0 then
        return nil
    end
    local offsetLow = buffer.readu32(W_mem, offsetPtr)
    local offsetHigh = buffer.readu32(W_mem, offsetPtr + 4)
    if offsetLow == 0 and offsetHigh == 0 then
        return nil
    end
    return offsetLow, offsetHigh
end

local fetchLabelFromOffset

function LogIterator:FetchCurrentLabel()
    if not self.session then
        return nil
    end
    local offsetLow, offsetHigh = fetchLabelOffset(
        W.MpGetLogIteratorCurrentLabelOffset(self.iterPtr))
    return offsetLow and fetchLabelFromOffset(self.session, offsetLow, offsetHigh) or nil
end

function LogIterator:GetScopeLabelCount(threadId, stackLevel)
    return W.MpGetLogIteratorScopeLabelCount(self.iterPtr, threadId, stackLevel)
end

function LogIterator:GetCurrentScopeLabelCount()
    return W.MpGetLogIteratorCurrentScopeLabelCount(self.iterPtr)
end

function LogIterator:FetchScopeLabel(threadId, stackLevel, labelIndex)
    if not self.session then
        return nil
    end
    local offsetLow, offsetHigh = fetchLabelOffset(W.MpGetLogIteratorScopeLabelOffset(
        self.iterPtr, threadId, stackLevel, labelIndex))
    return offsetLow and fetchLabelFromOffset(self.session, offsetLow, offsetHigh) or nil
end

function LogIterator:FetchCurrentScopeLabel(labelIndex)
    if not self.session then
        return nil
    end
    local offsetLow, offsetHigh = fetchLabelOffset(
        W.MpGetLogIteratorCurrentScopeLabelOffset(self.iterPtr, labelIndex))
    return offsetLow and fetchLabelFromOffset(self.session, offsetLow, offsetHigh) or nil
end

-- =============================================================================
-- COUNTER ITERATOR CLASS
-- =============================================================================

local CounterIterator = {}
CounterIterator.__index = CounterIterator

function CounterIterator.new(sessionOrHandle)
    local self = setmetatable({}, CounterIterator)
    if type(sessionOrHandle) == "table" then
        self.session = sessionOrHandle
        self.handle = sessionOrHandle.ptr
    else
        self.session = nil
        self.handle = sessionOrHandle
    end
    self.iterPtr = W.MpCreateCounterIterator(self.handle)
    return withGC(self)
end

function CounterIterator:Dispose()
    if self.iterPtr ~= 0 then
        W.MpDeleteCounterIterator(self.iterPtr)
        self.iterPtr = 0
    end
end

function CounterIterator:Configure(configObj)
    local builder = MpTypes.FlatBuffers.Builder(1024)
    local rootOffset = PackObject(builder, MpTypes.CounterIteratorConfig, configObj)
    builder:Finish(rootOffset)
    
    local luaBuf = builder:Output(false)
    local bufLen = buffer.len(luaBuf)

    local destPtr = W.MpGetCounterIteratorConfigBuffer(self.iterPtr, bufLen)
    buffer.copy(W_mem, destPtr, luaBuf, 0, bufLen)

    -- useDefault = false = 0
    W.MpConfigureCounterIterator(self.iterPtr, 0)
end

function CounterIterator:Step()
    return W.MpStepCounterIterator(self.iterPtr) ~= 0
end

function CounterIterator:Rewind()
    W.MpRewindCounterIterator(self.iterPtr)
end

function CounterIterator:GetState()
    local ptr = W.MpGetCounterIteratorState(self.iterPtr)
    return wrapStruct(ptr, MpTypes.CounterIteratorState)
end

function CounterIterator:GetStackElement(stackLevel)
    return W.MpGetCounterIteratorStackElement(self.iterPtr, stackLevel)
end

-- =============================================================================
-- CONTROL CLASS
-- =============================================================================

local Control = {}
Control.__index = Control

function Control.new(sessionPtr)
    local self = setmetatable({}, Control)
    self.ptr = sessionPtr
    return self
end

function Control:_syncIfGlobal()
    if self.ptr == 0 then
        _fullSyncAllSessions()
    end
end

local function isInteger(value)
    return type(value) == "number" and value % 1 == 0
end

local function unpackCaptureParams(captureParams)
    if captureParams == nil then
        return 0, Enums.ContainerFormat.Raw
    end
    if type(captureParams) ~= "table" then
        return nil, nil
    end

    local maxFrameCount = captureParams.MaxFrameCount or 0
    local containerFormat = captureParams.ContainerFormat or Enums.ContainerFormat.Raw
    if type(maxFrameCount) ~= "number" or maxFrameCount % 1 ~= 0 or maxFrameCount < 0 or
        type(containerFormat) ~= "number" or containerFormat % 1 ~= 0 or
        containerFormat < Enums.ContainerFormat.Raw or containerFormat > Enums.ContainerFormat.CompressedHtml then
        return nil, nil
    end
    return maxFrameCount, containerFormat
end

function Control:SaveSlotToBuffer(slotId)
    if not isInteger(slotId) then
        return nil
    end
    local size = MicroProfilerService:GetDataSize(slotId)
    local buf = buffer.create(size)
    if size > 0 and MicroProfilerService:GetDataInRange(slotId, 0, size, buf, 0) ~= size then
        return nil
    end
    return buf
end

function Control:CaptureToSlotSync(targetSlotId, captureParams)
    if not isInteger(targetSlotId) then
        return false
    end
    local maxFrameCount, containerFormat = unpackCaptureParams(captureParams)
    if maxFrameCount == nil then
        return false
    end
    return W.MpControlCaptureToSlot(self.ptr, targetSlotId, maxFrameCount, containerFormat) ~= 0
end

function Control:ResetSlot(slotId)
    if not isInteger(slotId) then
        return false
    end
    return W.MpControlResetSlot(self.ptr, slotId) ~= 0
end

function Control:CaptureToBufferSync(captureParams)
    local maxFrameCount, containerFormat = unpackCaptureParams(captureParams)
    if maxFrameCount == nil then
        return nil
    end
    if maxFrameCount == 0 and containerFormat == Enums.ContainerFormat.Raw then
        return self:SaveSlotToBuffer(LIVE_SLOT_ID)
    end
    if not self:CaptureToSlotSync(TEMPORARY_SLOT_ID, captureParams) then
        return nil
    end
    local result = self:SaveSlotToBuffer(TEMPORARY_SLOT_ID)
    self:ResetSlot(TEMPORARY_SLOT_ID)
    return result
end

function Control:EnableProfiler(enable)
    self:_syncIfGlobal()
    return W.MpControlEnableProfiler(self.ptr, enable and 1 or 0) ~= 0
end

function Control:EnableCapture(enable)
    return W.MpControlEnableCapture(self.ptr, enable and 1 or 0) ~= 0
end

function Control:ShowUI(show)
    return W.MpControlShowUI(self.ptr, show and 1 or 0) ~= 0
end

function Control:SetFrameLimit(num)
    self:_syncIfGlobal()
    return W.MpControlSetFrameLimit(self.ptr, num) ~= 0
end

function Control:SetMemTargetMB(targetMB)
    self:_syncIfGlobal()
    return W.MpControlSetMemTargetMB(self.ptr, targetMB) ~= 0
end

function Control:UseControlChannel(use)
    return W.MpUseControlChannel(use and 1 or 0) ~= 0
end

function Control:IsBackendVersionCompatible()
    return W.MpIsBackendVersionCompatible() ~= 0
end

function Control:IsBackendAccessible()
    return W.MpIsBackendAccessible() ~= 0
end

function Control:IsBackendReady()
    return W.MpIsBackendReady() ~= 0
end

-- =============================================================================
-- EXPERIMENTAL API
-- =============================================================================

local ExperimentalV1 = {}

function ExperimentalV1.StartFileStream(filenamePostfix, targetSlotId)
    filenamePostfix = filenamePostfix or ""
    targetSlotId = targetSlotId or 0
    local len = #filenamePostfix
    local stackStart = W.emscripten_stack_get_current()
    local strPtr = W._emscripten_stack_alloc(len + 1)
    buffer.writestring(W_mem, strPtr, filenamePostfix)
    buffer.writeu8(W_mem, strPtr + len, 0)
    local result = W.MpControlExperimentalV1StartFileStream(strPtr, len, targetSlotId) ~= 0
    W._emscripten_stack_restore(stackStart)
    return result
end

function ExperimentalV1.StartMemoryStream(targetSlotId)
    return W.MpControlExperimentalV1StartMemoryStream(targetSlotId) ~= 0
end

function ExperimentalV1.StopStream()
    return W.MpControlExperimentalV1StopStream() ~= 0
end

function ExperimentalV1.SetStreamMarker(marker, enabled)
    if not isInteger(marker) or type(enabled) ~= "boolean" then
        return false
    end
    return W.MpControlExperimentalV1SetStreamMarker(marker, enabled and 1 or 0) ~= 0
end

function ExperimentalV1.GetStreamStatus()
    local status = wrapStruct(W.MpControlExperimentalV1GetStreamStatus(), MpTypes.ExperimentalStreamStatus)
    if status then
        return Unpack(status)
    end
    return {
        Active = false,
        StreamFrameId = 0,
        AbsoluteFrameId = 0,
        MarkerMask = 0,
        RecordedSizeKb = 0,
        MaxSizeKb = 0,
        ElapsedMs = 0,
    }
end

function ExperimentalV1.SetContentMode(contentMode)
    local result = W.MpControlExperimentalV1SetContentMode(contentMode) ~= 0
    if result then
        _fullSyncAllSessions()
    end
    return result
end

-- =============================================================================
-- SESSION CLASS
-- =============================================================================

local Session = {}
Session.__index = Session

-- Bucket Enums matching C API MpSession::CacheBucket
local CacheBucket = {
    ShortTerm = 1,
    LongTerm = 2
}

local function newCacheEntry()
    return { wraps = {}, objs = {} }
end

function Session.new(ptr, id)
    local self = setmetatable({}, Session)
    self.ptr = ptr
    self.objectCacheEnabled = false
    self.dataLockDepth = 0
    self.needCheckSyncLocalCache = false
    self.labelCacheEnabled = false
    self.labelCacheDirty = false
    self._labelCache = {}

    -- Attach Control Channel interface
    self.Control = Control.new(ptr)

    self._cache = {
        [CacheBucket.ShortTerm] = {
            GlobalDesc  = newCacheEntry(),
            GeneralInfo = newCacheEntry(),
        },
        [CacheBucket.LongTerm] = {
            GroupDesc   = newCacheEntry(),
            TimerDesc   = newCacheEntry(),
            CounterDesc = newCacheEntry(),
            ThreadDesc  = newCacheEntry(),
        }
    }

    if id == nil then
        sessionIdCounter = sessionIdCounter + 1
        id = sessionIdCounter
    end

    -- Register the session instance using its raw pointer
    self.sessionId = id
    sessionIdRegistry[self.sessionId] = self
    sessionPtrRegistry[self.ptr] = self

    self:EnableObjectCache(true)
    return withGC(self)
end

function Session.OpenFromMem(data)
    local isBuf = (type(data) == "userdata" or type(data) == "buffer")
    local len = isBuf and buffer.len(data) or #data

    local ptr = W.malloc(len)

    if isBuf then
        buffer.copy(W_mem, ptr, data, 0, len)
    else
        buffer.writestring(W_mem, ptr, data)
    end

    local sessionPtr = W.MpOpenFromMemTrusted(ptr, len)
    if sessionPtr ~= 0 then
        local session = Session.new(sessionPtr)
        session.io = {
            ownedMemPtr = ptr
        }
        return session
    end

    W.free(ptr)
    return nil
end

function Session.OpenFromExternalProvider(io, cacheSize)
    sessionIdCounter = sessionIdCounter + 1
    local handle = sessionIdCounter
    sessionIdRegistry[handle] = {
        io = io
    }

    -- Arguments: handle_ptr, use_block_fetch, mem_page_cache_size
    local sessionPtr = W.MpOpenFromExternalDataTrusted(handle, 0, cacheSize or 0)
    
    if sessionPtr == 0 then
        sessionIdRegistry[handle] = nil
        return nil
    end

    local session = Session.new(sessionPtr, handle)
    session.io = io
    return session
end

function Session.OpenFromBuffer(b)
    local io = {
        buf = b,
        
        open = function(self)
            return true
        end,
        
        close = function(self)
            self.buf = nil
            return true
        end,
        
        getSize = function(self)
            return self.buf and buffer.len(self.buf) or 0
        end,
        
        read = function(self, dstBuf, dstOffset, block_id, offset, sz)
            buffer.copy(dstBuf, dstOffset, self.buf, offset, sz)
            return sz
        end
    }

    return Session.OpenFromExternalProvider(io, 0)
end

function Session.OpenFromSlot(id, cacheSize)
    if not isInteger(id) then
        return nil
    end
    local io = {
        slotId = id,

        open = function(self) 
            return true
        end,
        
        close = function(self) 
            return true
        end,
        
        getSize = function(self)
            return MicroProfilerService:GetDataSize(self.slotId)
        end,
        
        read = function(self, dstBuf, dstOffset, blockId, offset, sz)
            local blockIdAndOffset = offset
            if blockId ~= 0 then
                if blockId > MAX_PACKED_BLOCK_ID then
                    -- Invalid block ID: it exceeds the 20-bit packed provider limit.
                    return 0
                end
                blockIdAndOffset = BLOCK_MARKER_BIT + (blockId * SHIFT_32_BITS) + offset
            end
            return MicroProfilerService:GetDataInRange(self.slotId, blockIdAndOffset, sz, dstBuf, dstOffset)
        end
    }

    return Session.OpenFromExternalProvider(io, cacheSize)
end

function Session.OpenFromFile(path, cacheSize)
    local io = {
        filePath = path,
        fileHandle = nil,
        fileSize = 0,

        open = function(self)
            local f = fs.open(self.filePath, "r")
            if not f then
                return false
            end
            local ok, metadata = pcall(fs.metadata, self.filePath)
            if not ok or not metadata then
                fs.close(f)
                return false
            end
            self.fileHandle = f
            self.fileSize = metadata.size
            return true
        end,
        
        close = function(self)
            if self.fileHandle then
                fs.close(self.fileHandle)
                self.fileHandle = nil
            end
            self.fileSize = 0
            return true
        end,
        
        getSize = function(self)
            return self.fileSize
        end,
        
        read = function(self, dstBuf, dstOffset, block_id, offset, sz)
            if not self.fileHandle or block_id ~= 0 or offset > self.fileSize or sz > self.fileSize - offset then
                return 0
            end
            return fs.readIntoBuffer(self.fileHandle, dstBuf, offset, sz, dstOffset)
        end
    }

    return Session.OpenFromExternalProvider(io, cacheSize)
end

function Session.OpenFromLiveData()
    if isCliMode then
        local path = process.args[2]
        return Session.OpenFromFile(path)
    end
    return Session.OpenFromSlot(0)
end

function Session:Dispose()
    if self.ptr then
        local ptr = self.ptr
        W.MpCloseSession(ptr)

        -- Remove from the registry
        sessionIdRegistry[self.sessionId] = nil
        sessionPtrRegistry[ptr] = nil
        self.ptr = nil
        -- Its commands must not flag the freed session for resync
        self.Control.ptr = 0
        table.clear(self._labelCache)

        if (self.io ~= nil) and (self.io.ownedMemPtr ~= nil) then
            W.free(self.io.ownedMemPtr)
            self.io.ownedMemPtr = nil
        end
    end
end

-- Syncs with the data source and clears caches through the imported callbacks.
function Session:SyncWithDataSource()
    local success = W.MpSyncWithDataSource(self.ptr) ~= 0
    self.needCheckSyncLocalCache = false
    return success
end

function Session:SetSyncRequired()
    W.MpSetSyncRequired(self.ptr)
    self.needCheckSyncLocalCache = true
end

function Session:SetClearCacheOnSyncRequired()
    W.MpSetClearCacheOnSyncRequired(self.ptr)
end

function Session:SyncIfRequired()
    if self.needCheckSyncLocalCache then
        W.MpSyncIfRequired(self.ptr)
        self.needCheckSyncLocalCache = false
    end
end

function Session:IsValid()
    return W.MpIsSessionValid(self.ptr) ~= 0
end

function Session:GetDataFormatVersion()
    return W.MpGetSessionDataFormatVersion(self.ptr)
end

function Session:GetTrackCount()
    return W.MpGetTrackCount(self.ptr)
end

function Session:GetTracks()
    local arrPtr = W.MpGetTracks(self.ptr)
    return StructArray.new(self, arrPtr, W.MpGetTrackDescFromArray, MpTypes.TrackDesc)
end

function Session:FetchTracks()
    return _arrayFetchAllAndDispose(self:GetTracks())
end

function Session:GetSegmentCount(trackIndex)
    return W.MpGetSegmentCount(self.ptr, trackIndex)
end

function Session:GetSegments(trackIndex)
    local arrPtr = W.MpGetSegments(self.ptr, trackIndex)
    return StructArray.new(self, arrPtr, W.MpGetSegmentDescFromArray, MpTypes.SegmentDesc)
end

function Session:FetchSegments(trackIndex)
    return _arrayFetchAllAndDispose(self:GetSegments(trackIndex))
end

function Session:SelectTrack(trackIndex)
    return W.MpSelectTrack(self.ptr, trackIndex) ~= 0
end

function Session:SelectSegment(segmentIndex)
    return W.MpSelectSegment(self.ptr, segmentIndex) ~= 0
end

function Session:WithDataLock(callback)
    -- 1. Attempt to acquire the lock and check if it succeeded
    local isLocked = W.MpPushDataLock(self.ptr) ~= 0
    
    if not isLocked then
        return false -- Lock failed, do not execute the callback
    end

    self.dataLockDepth += 1

    -- 2. Execute callback safely
    local success, result = pcall(callback)
    
    -- 3. Safely release lock
    self.dataLockDepth -= 1
    W.MpPopDataLock(self.ptr)
  
    if not success then
        error(result)
    end
    
    -- 4. Return true (lock succeeded) and the callback's result
    return true, result
end

-- Generic Caching Helper for "Get" (returns FlatBuffer Object)
-- isStruct: true if using wrapStruct (Init at pos), false if wrapTable (Init at pos + offset)
local function getCached(self, cache, id, apiFunc, TypeClass, isStruct)
    self:SyncIfRequired()

    local lookupId

    -- 1. Try Cache Hit (Only if enabled)
    if self.objectCacheEnabled then
        lookupId = id or "single"
        local cachedWrap = cache.wraps[lookupId]
        if cachedWrap then
            if self.dataLockDepth > 0 then
                W.MpConsumeDataLock(self.ptr)
            end
            return cachedWrap
        end
    end

    -- 2. Fetch Pointer
    local ptr
    if id ~= nil then
        ptr = apiFunc(self.ptr, id)
    else
        ptr = apiFunc(self.ptr)
    end

    if not ptr or ptr == 0 then
        return nil
    end

    -- 3. Wrap Object
    local obj
    if isStruct then
        obj = wrapStruct(ptr, TypeClass)
    else
        obj = wrapTable(ptr, TypeClass)
    end

    -- 4. Save to Cache (Only if enabled)
    if self.objectCacheEnabled then
        cache.wraps[lookupId] = obj
    end

    return obj
end

-- Generic Caching Helper for "Fetch" (returns Unpacked Lua Table)
local function fetchCached(self, cache, id, getMethod)
    self:SyncIfRequired()

    local lookupId

    -- 1. Try Cache Hit (Only if enabled)
    if self.objectCacheEnabled then
        lookupId = id or "single"
        local cachedObj = cache.objs[lookupId]
        if cachedObj then
            if self.dataLockDepth > 0 then
                W.MpConsumeDataLock(self.ptr)
            end
            return cachedObj
        end
    end

    -- 2. Get the wrapper (this will trigger the buffer cache if needed)
    local obj = getMethod(self, id)
    if not obj then return nil end

    -- 3. Unpack
    local val = Unpack(obj)

    -- 4. Save to Cache (Only if enabled)
    if self.objectCacheEnabled then
        cache.objs[lookupId] = val
    end

    return val
end

function Session:_ClearCacheBucket(bucketIdx)
    local bucket = self._cache[bucketIdx]
    if bucket then
        for _, entry in pairs(bucket) do
            table.clear(entry.wraps)
            table.clear(entry.objs)
        end
    end
end

function Session:_ClearCache()
    for i = 1, #self._cache do
        self:_ClearCacheBucket(i)
    end
end

function Session:_InvalidateLabelCache(clearAll)
    self.labelCacheDirty = true
    if clearAll then
        table.clear(self._labelCache)
    end
end

function Session:_RefreshLabelCache()
    if not self.labelCacheEnabled or not self.labelCacheDirty then
        return
    end

    local arrPtr = W.MpInternalGetLabelBufferRegions(self.ptr)
    local regions = StructArray.new(
        self, arrPtr, W.MpInternalGetLabelBufferRegionFromArray,
        MpTypes.LabelBufferRegion)
    for chunkId in pairs(self._labelCache) do
        local chunkStart = chunkId * LABEL_CACHE_CHUNK_SIZE
        local chunkEnd = chunkStart + LABEL_CACHE_CHUNK_SIZE
        local covered = false
        for i = 0, regions:Size() - 1 do
            local region = regions:Get(i)
            if region and region:StartOffset() <= chunkStart and
                region:EndOffset() >= chunkEnd then
                covered = true
                break
            end
        end
        if not covered then
            self._labelCache[chunkId] = nil
        end
    end
    regions:Dispose()
    self.labelCacheDirty = false
end

local function readLabel(call)
    local stackStart = W.emscripten_stack_get_current()
    local lengthPtr = W._emscripten_stack_alloc(4)
    buffer.writeu32(W_mem, lengthPtr, 0)
    local ptr = call(lengthPtr)
    local byteLength = buffer.readu32(W_mem, lengthPtr)
    local value = nil
    if ptr and ptr ~= 0 then
        value = buffer.readstring(W_mem, ptr, byteLength)
    end
    W._emscripten_stack_restore(stackStart)
    return value
end

function Session:_GetLabelCached(offsetLow, offsetHigh, call)
    self:SyncIfRequired()
    self:_RefreshLabelCache()

    local chunkId = offsetHigh * (SHIFT_32_BITS / LABEL_CACHE_CHUNK_SIZE) +
        math.floor(offsetLow / LABEL_CACHE_CHUNK_SIZE)
    local key = tostring(offsetHigh) .. ":" .. tostring(offsetLow)
    if self.labelCacheEnabled then
        local chunk = self._labelCache[chunkId]
        if chunk and chunk[key] ~= nil then
            return chunk[key]
        end
    end

    local value = readLabel(call)
    if value ~= nil and self.labelCacheEnabled then
        local chunk = self._labelCache[chunkId]
        if not chunk then
            chunk = {}
            self._labelCache[chunkId] = chunk
        end
        chunk[key] = value
    end
    return value
end

function Session:EnableLabelCache(enabled)
    self.labelCacheEnabled = enabled
    if not enabled then
        table.clear(self._labelCache)
    end
end

function Session:ClearLabelCache()
    table.clear(self._labelCache)
    self.labelCacheDirty = false
end

fetchLabelFromOffset = function(self, offsetLow, offsetHigh)
    return self:_GetLabelCached(offsetLow, offsetHigh, function(lengthPtr)
        return W.MpGetLabelFromOffsetParts(self.ptr, offsetLow, offsetHigh, lengthPtr)
    end)
end

function Session:EnableObjectCache(enabled)
    self.objectCacheEnabled = enabled
    local e = enabled and 1 or 0
    W.MpEnableObjectCache(self.ptr, e, e, e, e)
    if not enabled then
        self:_ClearCache()
    end
end

function Session:ClearObjectCache()
    W.MpClearObjectCache(self.ptr)
    if self.objectCacheEnabled then
        self:_ClearCache()
    end
end

-- =============================================================================
-- SESSION - CORE GETTERS
-- =============================================================================

function Session:GetObjSize()
    return W.MpGetObjSize(self.ptr)
end

-- GlobalDesc (Struct)
function Session:GetGlobalDesc()
    return getCached(self, self._cache[CacheBucket.ShortTerm].GlobalDesc, nil, W.MpGetGlobalDesc, MpTypes.GlobalDesc, true)
end
function Session:FetchGlobalDesc()
    return fetchCached(self, self._cache[CacheBucket.ShortTerm].GlobalDesc, nil, self.GetGlobalDesc)
end

-- General Info (Table)
function Session:GetGeneralInfo()
    return getCached(self, self._cache[CacheBucket.ShortTerm].GeneralInfo, nil, W.MpGetGeneralInfo, MpTypes.GeneralInfo, false)
end
function Session:FetchGeneralInfo()
    return fetchCached(self, self._cache[CacheBucket.ShortTerm].GeneralInfo, nil, self.GetGeneralInfo)
end

-- Groups
function Session:GetGroupIdMax()
    return W.MpGetGroupIdMax(self.ptr)
end

function Session:GetGroupDesc(groupId)
    return getCached(self, self._cache[CacheBucket.LongTerm].GroupDesc, groupId, W.MpGetGroupDesc, MpTypes.GroupDesc, false)
end
function Session:FetchGroupDesc(groupId)
    return fetchCached(self, self._cache[CacheBucket.LongTerm].GroupDesc, groupId, self.GetGroupDesc)
end

-- Timers
function Session:GetTimerIds()
    local arrPtr = W.MpGetTimerIds(self.ptr)
    return ScalarArray.new(self, arrPtr, W.MpGetIdFromArray)
end
function Session:FetchTimerIds()
    return _arrayFetchAllAndDispose(self:GetTimerIds())
end

function Session:GetTimerDesc(timerId)
    return getCached(self, self._cache[CacheBucket.LongTerm].TimerDesc, timerId, W.MpGetTimerDesc, MpTypes.TimerDesc, false)
end
function Session:FetchTimerDesc(timerId)
    return fetchCached(self, self._cache[CacheBucket.LongTerm].TimerDesc, timerId, self.GetTimerDesc)
end

-- Counters
function Session:GetCounterIds()
    local arrPtr = W.MpGetCounterIds(self.ptr)
    return ScalarArray.new(self, arrPtr, W.MpGetIdFromArray)
end
function Session:FetchCounterIds()
    return _arrayFetchAllAndDispose(self:GetCounterIds())
end

function Session:GetCounterDesc(counterId)
    return getCached(self, self._cache[CacheBucket.LongTerm].CounterDesc, counterId, W.MpGetCounterDesc, MpTypes.CounterDesc, false)
end
function Session:FetchCounterDesc(counterId)
    return fetchCached(self, self._cache[CacheBucket.LongTerm].CounterDesc, counterId, self.GetCounterDesc)
end

function Session:GetCounterSamples(counterId)
    local arrPtr = W.MpGetCounterSamples(self.ptr, counterId)
    return CounterSampleArray.new(self, arrPtr)
end
function Session:FetchCounterSamples(counterId)
    return _arrayFetchAllAndDispose(self:GetCounterSamples(counterId))
end

function Session:GetLastCounterSample(counterId)
    local ptr = W.MpGetLastCounterSample(self.ptr, counterId)
    return wrapStruct(ptr, MpTypes.CounterSample)
end
function Session:FetchLastCounterSample(counterId)
    return Unpack(self:GetLastCounterSample(counterId))
end

-- Threads
function Session:GetThreadDesc(threadId)
    return getCached(self, self._cache[CacheBucket.LongTerm].ThreadDesc, threadId, W.MpGetThreadDesc, MpTypes.ThreadDesc, false)
end
function Session:FetchThreadDesc(threadId)
    return fetchCached(self, self._cache[CacheBucket.LongTerm].ThreadDesc, threadId, self.GetThreadDesc)
end

function Session:GetThreadIdGpu()
    return W.MpGetThreadIdGpu(self.ptr)
end

function Session:GetThreadIds()
    local arrPtr = W.MpGetThreadIds(self.ptr)
    return ScalarArray.new(self, arrPtr, W.MpGetIdFromArray)
end
function Session:FetchThreadIds()
    return _arrayFetchAllAndDispose(self:GetThreadIds())
end

-- Frames
function Session:GetFrameIdMin()
    return W.MpGetFrameIdMin(self.ptr)
end

function Session:GetFrameIdMax()
    return W.MpGetFrameIdMax(self.ptr)
end

function Session:GetFrameDesc(frameId)
    local ptr = W.MpGetFrameDesc(self.ptr, frameId)
    return wrapTable(ptr, MpTypes.FrameDesc)
end
function Session:FetchFrameDesc(frameId)
    return Unpack(self:GetFrameDesc(frameId))
end

-- Place IDs
function Session:GetPlaceIdSamples()
    local arrPtr = W.MpGetPlaceIdSamples(self.ptr)
    return StructArray.new(self, arrPtr, W.MpGetU64SampleFromArray, MpTypes.U64Sample)
end
function Session:FetchPlaceIdSamples()
    return _arrayFetchAllAndDispose(self:GetPlaceIdSamples())
end

-- UTC Timestamps
function Session:GetUtcTimestampSamples()
    local arrPtr = W.MpGetUtcTimestampSamples(self.ptr)
    return StructArray.new(self, arrPtr, W.MpGetU64SampleFromArray, MpTypes.U64Sample)
end
function Session:FetchUtcTimestampSamples()
    return _arrayFetchAllAndDispose(self:GetUtcTimestampSamples())
end

-- =============================================================================
-- SESSION - ITERATORS
-- =============================================================================

-- Iterators
function Session:CreateLogIterator()
    return LogIterator.new(self)
end

function Session:CreateCounterIterator()
    return CounterIterator.new(self)
end

-- =============================================================================
-- SESSION - FIND / FILTER METHODS
-- =============================================================================

-- Helper to manage C-String memory for search masks
function Session:_CallWithMask(func, nameMask, caseSensitive)
    nameMask = tostring(nameMask or "")
    local len = #nameMask
    
    local stackStart = W.emscripten_stack_get_current()
    local strPtr = W._emscripten_stack_alloc(len + 1)
    
    -- Write string and null terminator
    buffer.writestring(W_mem, strPtr, nameMask)
    buffer.writeu8(W_mem, strPtr + len, 0) 

    local cs = (caseSensitive) and 1 or 0
    local result = func(self.ptr, strPtr, cs)

    W._emscripten_stack_restore(stackStart)
    return result
end

-- Groups
function Session:FindGroupId(nameMask, caseSensitive)
    return self:_CallWithMask(W.MpFindGroupId, nameMask, caseSensitive)
end

function Session:FindGroupIds(nameMask, caseSensitive)
    local arrPtr = self:_CallWithMask(W.MpFindGroupIds, nameMask, caseSensitive)
    return _arrayFetchAllAndDispose(ScalarArray.new(self, arrPtr, W.MpGetIdFromArray))
end

-- Timers
function Session:FindTimerId(nameMask, caseSensitive)
    return self:_CallWithMask(W.MpFindTimerId, nameMask, caseSensitive)
end

function Session:FindTimerIds(nameMask, caseSensitive)
    local arrPtr = self:_CallWithMask(W.MpFindTimerIds, nameMask, caseSensitive)
    return _arrayFetchAllAndDispose(ScalarArray.new(self, arrPtr, W.MpGetIdFromArray))
end

function Session:GetTimerIdsByGroupId(groupId)
    local arrPtr = W.MpGetTimerIdsByGroupId(self.ptr, groupId)
    return ScalarArray.new(self, arrPtr, W.MpGetIdFromArray)
end
function Session:FetchTimerIdsByGroupId(groupId)
    return _arrayFetchAllAndDispose(self:GetTimerIdsByGroupId(groupId))
end

function Session:GetTimerIdsByGroupNames(nameMask, caseSensitive)
    local arrPtr = self:_CallWithMask(W.MpGetTimerIdsByGroupNames, nameMask, caseSensitive)
    return ScalarArray.new(self, arrPtr, W.MpGetIdFromArray)
end
function Session:FetchTimerIdsByGroupNames(nameMask, caseSensitive)
    return _arrayFetchAllAndDispose(self:GetTimerIdsByGroupNames(nameMask, caseSensitive))
end

-- Counters
function Session:FindCounterId(nameMask, caseSensitive)
    return self:_CallWithMask(W.MpFindCounterId, nameMask, caseSensitive)
end

function Session:FindCounterIds(nameMask, caseSensitive)
    local arrPtr = self:_CallWithMask(W.MpFindCounterIds, nameMask, caseSensitive)
    return _arrayFetchAllAndDispose(ScalarArray.new(self, arrPtr, W.MpGetIdFromArray))
end

-- Threads
function Session:FindThreadId(nameMask, caseSensitive)
    return self:_CallWithMask(W.MpFindThreadId, nameMask, caseSensitive)
end

function Session:FindThreadIds(nameMask, caseSensitive)
    local arrPtr = self:_CallWithMask(W.MpFindThreadIds, nameMask, caseSensitive)
    return _arrayFetchAllAndDispose(ScalarArray.new(self, arrPtr, W.MpGetIdFromArray))
end

-- =============================================================================
-- TOOLS
-- =============================================================================

local Tools = {}
Tools.__index = Tools

function Tools:SetHtmlAssetsBaseUrl(baseUrl)
    baseUrl = tostring(baseUrl or "")
    local len = #baseUrl
    local stackStart = W.emscripten_stack_get_current()
    local strPtr = W._emscripten_stack_alloc(len + 1)
    buffer.writestring(W_mem, strPtr, baseUrl)
    buffer.writeu8(W_mem, strPtr + len, 0)
    local result = W.MpToolSetHtmlAssetsBaseUrl(strPtr, len) ~= 0
    W._emscripten_stack_restore(stackStart)
    return result
end

local function runTool(session, format, output)
    assert(session and session.ptr ~= 0, "expected a live Session")
    assert(activeToolOutput == nil, "a LibMP tool operation is already active")
    activeToolOutput = output
    activeToolOutputError = nil
    local callSuccess, result = pcall(W.MpToolConvertSession, session.ptr, format)
    local outputError = activeToolOutputError
    activeToolOutput = nil
    activeToolOutputError = nil
    if not callSuccess then
        error(result)
    end
    if outputError ~= nil then
        error(outputError)
    end
    return result ~= 0
end

function Tools:ConvertToBuffer(session, format)
    local chunks = {}
    local success = runTool(session, format, function(dataPtr, size)
        chunks[#chunks + 1] = buffer.readstring(W_mem, dataPtr, size)
        return true
    end)
    if not success then
        return nil
    end
    return buffer.fromstring(table.concat(chunks))
end

function Tools:ConvertToFile(session, format, outputName)
    if not isCliMode then
        return false
    end
    local file = fs.open(outputName, "w")
    if file == nil then
        return false
    end
    local callSuccess, result = pcall(runTool, session, format, function(dataPtr, size)
        fs.write(file, buffer.readstring(W_mem, dataPtr, size))
        return true
    end)
    fs.close(file)
    if not callSuccess then
        error(result)
    end
    return result
end

-- =============================================================================
-- VERSIONS CLASS
-- =============================================================================

local Versions = {}
Versions.__index = Versions

function Versions.new()
    local self = setmetatable({}, Versions)

    self.DataFormat = W.MpGetDataFormatVersion()
    self.Library = W.MpGetLibraryVersion()

    return self
end

-- =============================================================================
-- EXPORTS
-- =============================================================================

W.MpInit()

LibMP.Session = Session
LibMP.LogIterator = LogIterator
LibMP.CounterIterator = CounterIterator
LibMP.init = function() end

LibMP.IsCliMode = isCliMode
LibMP.GetMemUsed = W.MpGetMemUsed
LibMP.GetMemReserved = W.MpGetMemReserved
LibMP.Enums = Enums
LibMP.Tools = setmetatable({}, Tools)
LibMP.Versions = Versions.new()

LibMP.Control = Control.new(0)
LibMP.Control.Global = LibMP.Control
LibMP.Control:UseControlChannel(not isCliMode)
LibMP.Experimental = table.freeze({
    V1 = table.freeze(ExperimentalV1),
})

return LibMP
-- END OF FILE

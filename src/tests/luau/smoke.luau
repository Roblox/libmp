-- This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

local LibMP = require("./LibMP")
local fs = require("@std/fs")

local CAPTURE_PATH = "@SMOKE_CAPTURE@"
local OUTPUT_PATH = "@SMOKE_OUTPUT@"

local function readFile(path)
    local file = assert(fs.open(path, "r"), "failed to open " .. path)
    local data = fs.read(file)
    fs.close(file)
    return data
end

local captureBytes = readFile(CAPTURE_PATH)
local capture = buffer.fromstring(captureBytes)

assert(LibMP.Versions.Library ~= 0, "library version is missing")
assert(type(LibMP.GetMemReserved()) == "number", "reserved-memory query is unavailable")

local session = assert(LibMP.Session.OpenFromMem(capture), "OpenFromMem failed")
assert(session:IsValid(), "OpenFromMem returned an invalid session")

-- Existing FlatBuffer views must follow replacement of the translated WASM memory buffer.
local globalView = assert(session:GetGlobalDesc(), "global descriptor is missing")
local expectedTick = globalView:TickToMsCpu()
local reservedBeforeGrowth = LibMP.GetMemReserved()
local growthSessions = {}
while LibMP.GetMemReserved() == reservedBeforeGrowth and #growthSessions < 16 do
    local growthSession = assert(LibMP.Session.OpenFromMem(capture),
        "opening a growth session failed")
    growthSessions[#growthSessions + 1] = growthSession
end
assert(LibMP.GetMemReserved() > reservedBeforeGrowth, "test sessions did not grow WASM memory")
assert(globalView:TickToMsCpu() == expectedTick,
    "FlatBuffers view did not follow grown WASM memory")
for index = #growthSessions, 1, -1 do
    growthSessions[index]:Dispose()
end

local timerId = assert(session:FetchTimerIds()[1], "timer array is empty")
local lockCachedView = assert(session:GetGlobalDesc(), "global descriptor is missing")
local lockCachedObject = assert(session:FetchTimerDesc(timerId), "timer descriptor is missing")
local outerLocked = session:WithDataLock(function()
    assert(rawequal(session:GetGlobalDesc(), lockCachedView),
        "locked cache hit did not preserve view identity")
    local innerLocked = session:WithDataLock(function()
        assert(rawequal(session:FetchTimerDesc(timerId), lockCachedObject),
            "nested locked cache hit did not preserve fetched-object identity")
    end)
    assert(innerLocked, "nested data lock failed")
end)
assert(outerLocked, "outer data lock failed")

local bufferSession = assert(LibMP.Session.OpenFromBuffer(capture), "OpenFromBuffer failed")
assert(bufferSession:IsValid(), "OpenFromBuffer returned an invalid session")
bufferSession:Dispose()

local fileSession = assert(LibMP.Session.OpenFromFile(CAPTURE_PATH), "OpenFromFile failed")
assert(fileSession:IsValid(), "OpenFromFile returned an invalid session")
fileSession:Dispose()

local provider = {
    data = capture,
    openCount = 0,
    readCount = 0,
    closeCount = 0,
}
function provider:open()
    self.openCount += 1
    return true
end
function provider:getSize()
    return buffer.len(self.data)
end
function provider:read(destination, destinationOffset, blockId, offset, size)
    self.readCount += 1
    if blockId ~= 0 or offset < 0 or size < 0 or
        offset > buffer.len(self.data) or size > buffer.len(self.data) - offset then
        return 0
    end
    buffer.copy(destination, destinationOffset, self.data, offset, size)
    return size
end
function provider:close()
    self.closeCount += 1
    return true
end

local providerSession = assert(LibMP.Session.OpenFromExternalProvider(provider),
    "OpenFromExternalProvider failed")
assert(providerSession:IsValid(), "external provider returned an invalid session")
assert(provider.openCount == 1 and provider.readCount > 0,
    "external provider callbacks were not invoked")
providerSession:Dispose()
providerSession:Dispose()
assert(provider.closeCount == 1, "external provider was not closed exactly once")

local raw = assert(LibMP.Tools:ConvertToBuffer(session, LibMP.Enums.ContainerFormat.Raw),
    "ConvertToBuffer failed")
assert(LibMP.Tools:ConvertToFile(session, LibMP.Enums.ContainerFormat.Raw, OUTPUT_PATH),
    "ConvertToFile failed")
assert(readFile(OUTPUT_PATH) == buffer.readstring(raw, 0, buffer.len(raw)),
    "file and buffer conversion differ")
assert(session:IsValid(), "conversion invalidated its source session")

session:Dispose()
session:Dispose()
print("LibMP Luau smoke test passed")

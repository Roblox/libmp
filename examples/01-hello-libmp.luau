--[[
    01-hello-libmp.luau -- Hello, LibMP!

    The simplest example that works in both the Roblox Engine and Lute (CLI):
    Open a session and read a counter value.

    Setup in Roblox Studio:
    - Add the LibMP ModuleScript to ReplicatedStorage
    - Switch to Play mode

    Setup in Lute (CLI Luau, https://lute.luau.org/):
    - Ensure LibMP.luau is placed in the same directory as this script file
    - Run in command line: `lute 1-hello-libmp.luau /path/to/dump.gprx`
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

-- In Lute, Control methods are no-ops
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
    print("Luau heap counter is not found")
    session:Dispose()
    return
end

local function printHeapSize()
    local sample = session:FetchLastCounterSample(counterId)
    if sample then
        print(string.format("Luau heap: %.2f MB", sample.Value / (1024 * 1024)))
    else
        print("Luau heap counter has no samples")
    end
    session:Dispose()
end

if game then
    task.delay(0, printHeapSize)
else
    printHeapSize()
end

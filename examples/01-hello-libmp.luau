--[[ 
01-hello-libmp.luau
Cross-platform performance counter reader for Roblox & Lute CLI
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

-- Initialize profiling controls (No-ops in Lute CLI)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local function runSession()
    local session = LibMP.Session.OpenFromLiveData()
    
    if not session or not session:IsValid() then
        print("[-] Failed to open session")
        if session then session:Dispose() end
        return
    end

    -- Cleanup function to guarantee session disposal on exit/errors
    local function cleanup()
        if session and session:IsValid() then
            session:Dispose()
        end
    end

    local counterId = session:FindCounterId("**/Luau/heap")
    if counterId == 0 then
        print("[-] Luau heap counter not found")
        cleanup()
        return
    end

    local sample = session:FetchLastCounterSample(counterId)
    if sample then
        local heapSizeMB = sample.Value / (1024 * 1024)
        print(string.format("[+] Luau heap: %.2f MB", heapSizeMB))
    else
        print("[-] Luau heap counter has no samples yet")
    end

    cleanup()
end

-- Execution Router
if game then
    -- Wait exactly one engine frame to guarantee data sample capture availability
    game:GetService("RunService").Heartbeat:Once(runSession)
else
    -- In Lute CLI, execute immediately (Ensure your CLI setup feeds a stream or change to file open)
    runSession()
end

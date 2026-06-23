--[[
    10-usecase-snapshot-sender.luau -- Server-Side Snapshot Sender

    Periodically captures MicroProfiler snapshots on the server and sends
    them via HTTP POST to an external endpoint for remote analysis.

    Strategy:
    - Profiler stays enabled but capture is paused between intervals.
    - Every 5 minutes, activate capture, wait until 64 new frames are collected,
      then snapshot and pause capture again.

    This example is designed for the Roblox Engine (Server Script only).
    An endpoint that receives uploaded dumps is required, see below.
    
    Setup in Roblox Studio:
    - Add the LibMP ModuleScript to ReplicatedStorage
    - Add a new Script to ServerScriptService, paste this example code into it
    - Enable HTTP requests for the game: Game Settings -> Security -> Allow HTTP Requests.
    - Switch to Play mode
]]

--[[
--  BEGIN OF ENDPOINT SCRIPT
--
--  Local Dump Upload Endpoint
--
--  Setup in Lute (https://lute.luau.org/):
--  - Create a new `endpoint.luau` file and paste the contents of this comment
--    section into it (not the entire example script).
--  - Ensure `LibMP.luau` is located in the same directory as `endpoint.luau`.
--  - Run the command: `lute endpoint.luau`

    local net = require("@lute/net")
    local fs = require("@lute/fs")
    local task = require("@lute/task")

    local LibMP = require("./LibMP")

    local UPLOAD_DIR = "uploads"

    if not fs.exists(UPLOAD_DIR) then
        fs.mkdir(UPLOAD_DIR)
    end

    local function analyzeSnapshot(buf)
        local session = LibMP.Session.OpenFromBuffer(buf)
        if not session or not session:IsValid() then
            print("  Failed to open LibMP session from buffer")
            if session then session:Dispose() end
            return
        end

        local counterId = session:FindCounterId("**/Luau/heap")
        local sample = session:FetchLastCounterSample(counterId)
        if sample ~= nil then
            print(string.format("  Roblox Server Luau heap: %.2f MB", sample.Value / (1024 * 1024)))
        end

        session:Dispose()
    end

    local instance = net.serve(function(req)
        print(`[REQUEST] method={req.method} body_len={if req.body then #req.body else 0}`)
        if req.method == "POST" then
            local filename = `snapshot-{os.time()}.gprx`
            local filepath = `{UPLOAD_DIR}/{filename}`

            local handle = fs.open(filepath, "w")
            fs.write(handle, req.body)
            fs.close(handle)

            print(`Saved {#req.body} bytes to {filepath}`)
            analyzeSnapshot(buffer.fromstring(req.body))
        end
        return { status = 200, body = "ok" }
    end)

    print(`Server running on http://{instance.hostname}:{instance.port}`)

    while true do
        task.wait(1)
    end
    
--  END OF ENDPOINT SCRIPT
]]

local RunService = game and game:GetService("RunService")

if not game or not RunService or not RunService:IsServer() then
    print("This example must be run in a Server Script within the Roblox Engine.")
    return
end

local HttpService = game:GetService("HttpService")
local LibMP = require("@game/ReplicatedStorage/LibMP")

-- Configuration
local ENDPOINT_URL = "http://localhost:3000/upload"
local CAPTURE_INTERVAL = 5 * 60 -- 5 minutes
local TARGET_FRAMES = 64

-- Initialize: profiler on, capture paused
LibMP.Control:SetFrameLimit(TARGET_FRAMES)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(false)

--- Activates capture, waits for TARGET_FRAMES engine ticks, then snapshots and pauses.
local function captureFrames(): buffer?
    -- Activate capture
    LibMP.Control:EnableCapture(true)

    -- The MicroProfiler records one frame per engine tick.
    -- Count Heartbeat events to know how many frames have been captured.
    for _ = 1, TARGET_FRAMES do
        RunService.Heartbeat:Wait()
    end

    -- Pause capture and snapshot
    LibMP.Control:EnableCapture(false)
    return LibMP.Control:CaptureToBufferSync()
end

local function sendSnapshot(buf: buffer)
    local headers = {
        ["Content-Type"] = "application/octet-stream",
        ["X-Timestamp"] = tostring(os.time()),
        ["X-Place-Id"] = tostring(game.PlaceId),
        ["X-Server-Id"] = game.JobId,
    }

    local success, response = pcall(function()
        return HttpService:RequestAsync({
            Url = ENDPOINT_URL,
            Method = "POST",
            Headers = headers,
            Body = buffer.tostring(buf)
        })
    end)

    if success then
        if response.Success then
            print(string.format("[SnapshotSender] Sent %d bytes, response code: %s",
                buffer.len(buf), tostring(response.StatusCode)))
        else
            warn(string.format("[SnapshotSender] HTTP Error %d: %s", 
                response.StatusCode, tostring(response.StatusMessage)))
        end
    else
        warn(string.format("[SnapshotSender] Request failed: %s", tostring(response)))
    end
end

local function startSnapshotSender()
    print(string.format("[SnapshotSender] Started. Capturing %d frames every %d seconds...", TARGET_FRAMES, CAPTURE_INTERVAL))

    -- Main loop
    task.spawn(function()
        -- Initial delay to let the server settle
        task.wait(10)

        while true do
            local ok, err = pcall(function()
                local buf = captureFrames()
                if buf then
                    sendSnapshot(buf)
                else
                    warn("[SnapshotSender] Failed to capture buffer.")
                end
            end)

            if not ok then
                warn("[SnapshotSender] Error: " .. tostring(err))
            end

            task.wait(CAPTURE_INTERVAL)
        end
    end)
end

startSnapshotSender()

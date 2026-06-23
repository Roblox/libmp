--[[
    08-usecase-snapshot-capturer.luau -- Periodic Snapshot Capturer

    Captures a 16-frame MicroProfiler snapshot once per minute.
    The profiler and capture are always active, so by the time we snapshot
    we're saving the frames that have already been recorded - no waiting needed.
    Stores the last 3 snapshots in a rotating buffer.

    This example is designed for the Roblox Engine only.
    See example #1 for setup instructions.
]]

if not game then
    print("This example is intended for the Roblox Engine only.")
    return
end

local LibMP = require("@game/ReplicatedStorage/LibMP")

-- Configuration
local CAPTURE_INTERVAL = 60 -- 1 minute
local FRAME_LIMIT = 16
local MAX_STORED = 3

LibMP.Control:SetFrameLimit(FRAME_LIMIT)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

-- Storage for snapshots (ring buffer)
local snapshots: { { buf: buffer, timestamp: number } } = {}

local function startPeriodicCapturer()
    print(string.format("[SnapshotCapturer] Started. Capturing every %d seconds...", CAPTURE_INTERVAL))
    
    task.spawn(function()
        while true do
            task.wait(CAPTURE_INTERVAL)

            -- Take a snapshot
            local buf = LibMP.Control:CaptureToBufferSync()
            
            if buf == nil then
                print("[SnapshotCapturer] Failed to capture buffer.")
                continue
            end

            table.insert(snapshots, {
                buf = buf,
                timestamp = os.time(),
            })

            -- Evict oldest if over limit
            while #snapshots > MAX_STORED do
                table.remove(snapshots, 1)
            end

            print(string.format("[SnapshotCapturer] Captured %d bytes (%d/%d stored)",
                buffer.len(buf), #snapshots, MAX_STORED))
                
            -- Extract basic info from the snapshot
            local session = LibMP.Session.OpenFromBuffer(buf)
            if not session or not session:IsValid() then
                print("Failed to open session")
                if session then session:Dispose() end
                continue
            end

            print(string.format("[SnapshotCapturer] Max frame ID = %d", session:GetFrameIdMax()))
            session:Dispose()
        end
    end)
end

startPeriodicCapturer()

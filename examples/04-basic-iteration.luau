--[[
    04-basic-iteration.luau -- Iterating Scopes (Single Frame, Single Thread)

    Demonstrates the log iterator with a minimal setup: one frame, one thread.
    Captures scope enter/exit entries and prints them with depth indentation.
    Works in both the Roblox Engine and Lute (CLI).
    See example #1 for setup instructions.
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

LibMP.Control:SetFrameLimit(64)
LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local function iterateFrameScopes()
    local session = LibMP.Session.OpenFromLiveData()
    if not session or not session:IsValid() then
        print("Failed to open session")
        if session then session:Dispose() end
        return
    end

    -- Pick the last available frame
    local frameId = session:GetFrameIdMax()
    if frameId == 0 then
        print("No frames available")
        session:Dispose()
        return
    end

    -- Find the main thread
    local mainThreadId = session:FindThreadId("*rbx main*")
    if mainThreadId == 0 then
        print("No Main thread found")
        session:Dispose()
        return
    end
    
    local globalDesc = session:FetchGlobalDesc()
    local threadName = session:FetchThreadDesc(mainThreadId).ThreadName
    print(string.format("Frame %d, Thread: '%s'", frameId, threadName))
    print(string.rep("-", 60))

    -- Create iterator filtered to our single thread
    local iter = session:CreateLogIterator()
    iter:Configure({
        ThreadIds = { mainThreadId },
        -- TimerIds = {},     -- Iterate only over these timers (all timers by default)
        -- SkipTimerIds = {}, -- Skip these timers (none by default)
    })

    local iterState = iter:GetState() -- Iterator state is updated on each step
    iter:RewindTo(frameId, frameId) -- Iterate over a single frame
    
    local scopeLimit = 50
    local scopeCounter = 0
    
    while iter:Step() do
        if iterState:IsEnter() then
            -- Entered a scope
            local depth = iterState:ThreadStackDepth()
            local timerName = session:FetchTimerDesc(iterState:TimerId()).TimerName

            local indent = string.rep("  ", depth)
            print(string.format("%s> %s", indent, timerName))

        elseif iterState:IsExit() then
            -- Exited a scope
            scopeCounter = scopeCounter + 1
            if scopeCounter > scopeLimit then
                break
            end
            
            -- Depth is the number of scopes currently on the stack
            local depth = iterState:ThreadStackDepth()
            local timerName = session:FetchTimerDesc(iterState:TimerId()).TimerName

            -- Calculate duration if we have the matching enter
            local durationStr = ""
            if not iterState:ThreadStackIsUnderflowed() then
                -- Get the element that was just popped from the scope stack.
                -- Stack elements are indexed from 0.
                local el = iter:GetCurrentThreadStackElement(depth)
                if el then
                    local durationMs = (iterState:Timestamp() - el:EnterTimestamp()) * globalDesc.TickToMsCpu
                    durationStr = string.format(" (%.3f ms)", durationMs)
                end
            end

            local indent = string.rep("  ", depth + 1)
            print(string.format("%s< %s%s", indent, timerName, durationStr))
        end
    end

    iter:Dispose()
    session:Dispose()
    print("\nDone.")
end

if game then
    -- Delay execution to allow the engine to accumulate
    -- frame history on the first activation of MicroProfiler
    task.delay(2, iterateFrameScopes)
else
    iterateFrameScopes()
end

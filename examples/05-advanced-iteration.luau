--!strict
--[[
    05-advanced-iteration.luau -- Reusable Thread Log Iteration Module

    A library module exporting three advanced iteration strategies:
      - iterateFrameLocal:      Frame-local single-pass iteration with cross-frame scope attribution.
      - iterateFullReconstruct: Two-pass full stack reconstruction for isolated frames.
      - iterateOptimal:         Full reconstruction for first frame, then single-pass for the rest.

    All callbacks receive: (frameId, threadId, depth, timerId, enterTimestamp, exitTimestamp)

    Usage:
        local LogIteration = require("./05-advanced-iteration")
        LogIteration.iterateOptimal(session, iter, startFrameId, endFrameId,
            function(frameId, threadId, depth, timerId, enterTs, exitTs)
                -- process scope
            end
        )

    The iterator's LogIteratorConfig MUST have EmitThreadFrameEnd = true.
    Works in both Roblox Engine and Lute (CLI).
]]

--[[
    Technical Details

    Scopes can cross frame boundaries.
    The iterator progresses frame by frame, and thread by thread within each frame.
    In the example below, we iterate only over entries in frame #N.
    
    * Scope A0 enters and exits within the frame boundaries. If we only worked with this
      kind of scope, the simple iterator from example #4 would be enough.
      
    * Scope B0 enters before frame #N, so only the second part of its duration should be
      attributed to frame #N. `iterateFrameLocal` can handle this.
      
    * Scope B1 enters and exits within frame #N, but it is a child of B0, and we don't
      know that until we encounter the exit entry of B0. `iterateFullReconstruct`
      can reconstruct this parent-child hierarchy (scope depths).
      
    * Scope stack C0/C1 remains active at the moment of leaving frame #N. LibMP's
      LogIterator remembers that stack state, so when we enter the next frame (#N+1),
      we reuse the known stack state and do not need to start from scratch.
      
               (#N-1) |          (Frame #N)          | (#N+1)
                      |                              |
    Thread A:         |     [.A0................]    |
                      |                              |
    Thread B:  [.B0...|..................]           |
                      |  [.B1.........]              |
                      |                              |
    Thread C:         |                     [.C0.....|...]
                      |                      [.C1....|.]
]]

export type ScopeCallback = (
    frameId: number,
    threadId: number,
    depth: number,
    timerId: number,
    enterTimestamp: number,
    exitTimestamp: number
) -> ()

type Session = any
type LogIterator = any

local LogIteration = {}

--[[
    Enhanced frame-local iteration.
    Attributes cross-frame scopes to each frame proportionally.
    Cannot reconstruct stack depths at the very start of startFrame
    (depths may appear as 0 for scopes that started before the range).
]]
function LogIteration.iterateFrameLocal(
    session: Session,
    iter: LogIterator,
    startFrame: number,
    endFrame: number,
    processScope: ScopeCallback
)
    local iterState = iter:GetState()
    iter:RewindTo(startFrame, endFrame)

    while iter:Step() do
        if iterState:IsThreadFrameEnd() then
            -- The current thread log has reached its end for the current frame,
            -- so we can scan the current scope stack to account for the portions
            -- of those scopes that belong to the current frame.
            
            local exitTimestamp = iterState:Timestamp()
            local maxLocalDepth = iterState:ThreadStackDepth()

            if maxLocalDepth > 0 then
                for localDepth = maxLocalDepth - 1, 0, -1 do
                    local el = iter:GetCurrentThreadStackElement(localDepth)
                    if el then
                        processScope(
                            iterState:FrameId(),
                            iterState:ThreadId(),
                            localDepth,
                            el:EnterTimerId(),
                            el:EnterTimestampFrameLocal(),
                            exitTimestamp
                        )
                    end
                end
            end

        elseif iterState:IsExit() and not iterState:ThreadStackWasOverflowed() then
            local depth = iterState:ThreadStackDepth()
            local enterTimestamp: number

            if not iterState:ThreadStackIsUnderflowed() then
                -- The scope enter timestamp is available, so use it
                -- clamped to the current frame's start time.
                local el = iter:GetCurrentThreadStackElement(depth)
                enterTimestamp = el:EnterTimestampFrameLocal()
            else
                -- The scope entry timestamp is unavailable, meaning the scope
                -- started before the current frame. Use the frame's start time
                -- as the scope entry timestamp for this frame.
                local frameDesc = session:FetchFrameDesc(iterState:FrameId())
                enterTimestamp = if iterState:IsGpuThread()
                    then frameDesc.TickStartGpu
                    else frameDesc.TickStartCpu
            end

            processScope(
                iterState:FrameId(),
                iterState:ThreadId(),
                depth,
                iterState:TimerId(),
                enterTimestamp,
                iterState:Timestamp()
            )
        end
    end
end

--[[
    Full stack reconstruction.
    Two-pass: collects scopes, then normalizes depths using absolute tracking.
    Recovers correct depths even for the first frame where the stack is unknown.
    If used for more than one frame, requires SkipThreadStackDepthSync = true.
]]
function LogIteration.iterateFullReconstruct(
    session: Session,
    iter: LogIterator,
    startFrame: number,
    endFrame: number,
    processScope: ScopeCallback
)
    local scopes: { {
        absDepth: number,
        timerId: number,
        enterTimestamp: number,
        exitTimestamp: number
    } } = {}

    local frameStartTickCpu: number = 0
    local frameStartTickGpu: number = 0
    local frameStartTickCurThread: number = 0

    local frameId: number = 0
    local threadId: number = 0
    local st: any = nil

    local iterState = iter:GetState()
    iter:RewindTo(startFrame, endFrame)

    while iter:Step() do
        -- IsFrameBoundary() is a special notification, as
        -- it is not related to any specific thread.
        if iterState:IsFrameBoundary() then
            -- If you pause and resume MicroProfiler capturing, the capture will contain
            -- stitches. You can check for them here using `iterState:IsFrameStitching()`.
            frameId = iterState:FrameId()
            frameStartTickCpu = iterState:Timestamp()
            frameStartTickGpu = iterState:TimestampGpu()
            continue
        end

        -- ThreadChanged() is part of the state when iterating over regular
        -- log entries (scope enter/exit entries) and is not a separate notification.
        if iterState:ThreadChanged() then
            frameStartTickCurThread = if iterState:IsGpuThread()
                then frameStartTickGpu
                else frameStartTickCpu
            threadId = iterState:ThreadId()
            st = iter:GetThreadStackState(threadId)
        end

        if iterState:IsThreadFrameEnd() then
            local exitTimestamp = iterState:Timestamp()
            local maxLocalDepth = iterState:ThreadStackDepth()
            local currentAbsDepth = st:DepthAbsolute()

            -- Append active stack elements with their absolute depths
            if maxLocalDepth > 0 then
                for localDepth = maxLocalDepth - 1, 0, -1 do
                    local el = iter:GetCurrentThreadStackElement(localDepth)
                    if el then
                        local elAbsDepth = currentAbsDepth - (maxLocalDepth - localDepth)
                        table.insert(scopes, {
                            absDepth = elAbsDepth,
                            timerId = el:EnterTimerId(),
                            enterTimestamp = el:EnterTimestampFrameLocal(),
                            exitTimestamp = exitTimestamp,
                        })
                    end
                end
            end

            -- End of frame for this thread: normalize depths and process scopes
            local virtualZero = st:DepthAbsoluteMin()
            
            for _, scope in ipairs(scopes) do
                processScope(
                    frameId,
                    threadId,
                    scope.absDepth - virtualZero,
                    scope.timerId,
                    scope.enterTimestamp,
                    scope.exitTimestamp
                )
            end
            
            -- Clear the buffer for the next thread in this frame
            table.clear(scopes)

        elseif iterState:IsExit() and not iterState:ThreadStackWasOverflowed() then
            local exitTimestamp = iterState:Timestamp()
            local currentAbsDepth = st:DepthAbsolute()
            
            -- Underflow fallback defaults to our perfectly tracked frameStartTickCurThread
            local enterTimestamp = frameStartTickCurThread

            if not iterState:ThreadStackIsUnderflowed() then
                local localDepth = iterState:ThreadStackDepth()
                local el = iter:GetCurrentThreadStackElement(localDepth)
                enterTimestamp = el:EnterTimestampFrameLocal()
            end

            -- Append the scope with its absolute depth
            table.insert(scopes, {
                absDepth = currentAbsDepth,
                timerId = iterState:TimerId(),
                enterTimestamp = enterTimestamp,
                exitTimestamp = exitTimestamp,
            })
        end
    end
end

--[[
    Hybrid iteration: full reconstruction for the first frame,
    then single-pass for subsequent frames.
    Best balance of accuracy and performance.
]]
function LogIteration.iterateOptimal(
    session: Session,
    iter: LogIterator,
    startFrame: number,
    endFrame: number,
    processScope: ScopeCallback
)
    LogIteration.iterateFullReconstruct(session, iter, startFrame, startFrame, processScope)
    if endFrame > startFrame then
        LogIteration.iterateFrameLocal(session, iter, startFrame + 1, endFrame, processScope)
    end
end

return LogIteration

--[[
    07-advanced-counter.luau -- Counter Hierarchy Traversal

    Demonstrates the CounterIterator to walk the hierarchical counter tree.
    Prints counter names with indentation, marks leaf nodes, and shows values.
    Works in both the Roblox Engine and Lute (CLI).
    See example #1 for setup instructions.
]]

local LibMP = require(game and "@game/ReplicatedStorage/LibMP" or "./LibMP")

LibMP.Control:EnableProfiler(true)
LibMP.Control:EnableCapture(true)

local session = LibMP.Session.OpenFromLiveData()
if not session or not session:IsValid() then
    print("Failed to open session")
    if session then session:Dispose() end
    return
end

--- Prints a counter subtree rooted at the given IDs.
--- Pass an empty table to print the entire tree.
local function printTree(session, rootIds: { number })
    local cIter = session:CreateCounterIterator()
    local state = cIter:GetState()

    cIter:Configure({ RootCounterIds = rootIds })

    while cIter:Step() do
        local counterId = state:CounterId()
        local desc = session:FetchCounterDesc(counterId)
        local indent = string.rep("  ", state:RelativeLevel())

        local valueStr = ""
        if state:IsLeaf() then
            local sample = session:FetchLastCounterSample(counterId)
            if sample then
                valueStr = string.format(" = %d", sample.Value)
            else
                valueStr = " = (no data)"
            end
        end

        print(string.format("%s%s%s", indent, desc.CounterName, valueStr))
    end

    cIter:Dispose()
end

local function runCounterTraversal()
    print("====================================")
    print("====== Full Counter Hierarchy ======")
    print("====================================")
    printTree(session, {})

    print()
    print("====================================")
    print("======== Memory/GUI Subtree ========")
    print("====================================")

    local memoryId = session:FindCounterId("memory/gui")
    if memoryId ~= 0 then
        printTree(session, { memoryId })
    else
        print("No memory/gui counter found")
    end

    session:Dispose()
    print("\nDone.")
end

if game then
    task.delay(0, runCounterTraversal)
else
    runCounterTraversal()
end

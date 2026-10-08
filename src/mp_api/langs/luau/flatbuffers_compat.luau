-- FlatBuffers library core (adapted for Luau buffer type) ---------------------
-- Based on the FlatBuffers Lua runtime and modified for Luau.
-- Licensed under Apache License 2.0; see src/dependencies/flatbuffers/LICENSE.

local Compat = (function()
    local m = {}
    local b = buffer
    local bit32 = bit32
    local band, rshift, bnot = bit32.band, bit32.rshift, bit32.bnot

    m.GetAlignSize = function(k, size)
        return band(bnot(k) + 1, size - 1)
    end

    -- Native mappings
    m.readu8 = b.readu8
    m.readu16 = b.readu16
    m.readu32 = b.readu32 
    m.readf32 = b.readf32
    m.readf64 = b.readf64
    m.readi8 = b.readi8
    m.readi16 = b.readi16
    m.readi32 = b.readi32        

    m.writeu8 = b.writeu8
    m.writeu16 = b.writeu16
    m.writeu32 = b.writeu32
    m.writef32 = b.writef32
    m.writef64 = b.writef64
    m.writei8 = b.writei8
    m.writei16 = b.writei16
    m.writei32 = b.writei32
    
    m.writestring = b.writestring
    m.readstring = b.readstring
    m.copy = b.copy
    m.create = b.create
    m.len = b.len
    m.fill = b.fill

    -- Polyfill: u64 / i64
    -- Warning: Luau doubles only support integer precision up to 2^53.
    -- Values larger than that will lose precision.
    m.readu64 = function(buf, offset)
        local l = m.readu32(buf, offset)
        local h = m.readu32(buf, offset + 4)
        return l + (h * 4294967296)
    end

    m.writeu64 = function(buf, offset, val)
        -- Split 64-bit number
        local l = val % 4294967296
        local h = math.floor(val / 4294967296)
        m.writeu32(buf, offset, l)
        m.writeu32(buf, offset + 4, h)
    end

    -- For i64, manually handle the 2's complement sign bit
    m.readi64 = function(buf, offset)
        local l = m.readu32(buf, offset)
        local h = m.readu32(buf, offset + 4)
        local v = l + (h * 4294967296)
        
        -- If the highest bit (0x80000000) is set, it's negative.
        -- We must subtract 2^64 to get the correct negative Luau double.
        if h >= 2147483648 then
            v = v - 18446744073709551616 
        end
        return v
    end

    m.writei64 = function(buf, offset, val)
        -- If writing a negative number, simulate 2's complement underflow
        if val < 0 then
            val = val + 18446744073709551616
        end
        
        local l = val % 4294967296
        local h = math.floor(val / 4294967296)
        m.writeu32(buf, offset, l)
        m.writeu32(buf, offset + 4, h)
    end

    return m
end)()

local BinaryArray = (function()
    local m = {}
    local compat = Compat

    -- Create a new binary array (native buffer)
    function m.New(size)
        return compat.create(size)
    end

    -- Grow the buffer, placing existing data at the END (for FlatBuffers backward building)
    function m.Grow(buf, newsize)
        local oldsize = compat.len(buf)
        local newBuf = compat.create(newsize)
        local offset = newsize - oldsize
        
        if oldsize > 0 then
            compat.copy(newBuf, offset, buf, 0, oldsize)
        end
        return newBuf
    end
    
    return m
end)()

local NumTypes = (function()
    local m = {}
    local op = Compat

    local type_mt = {}
    type_mt.__index = type_mt

    function type_mt:Pack(value)
        error("Legacy Pack called on buffer-based implementation")
    end

    function type_mt:Unpack(buf, pos)
        return self.Read(buf, pos)
    end

    function type_mt:ValidNumber(n)
        if not self.min_value and not self.max_value then return true end
        return self.min_value <= n and n <= self.max_value
    end

    function type_mt:EnforceNumber(n)
        if not self.min_value and not self.max_value then return end
        if self.min_value <= n and n <= self.max_value then return end
        error("Number is not in the valid range")
    end

    function type_mt:EnforceNumbers(a, b)
        self:EnforceNumber(a)
        self:EnforceNumber(b)
    end
    
    -- Helper to define types
    -- We assign Read/Write directly to the raw buffer functions (no colon syntax)
    local function def(name, width, min, max, readFunc, writeFunc, luaType)
        local t = {
            bytewidth = width,
            min_value = min,
            max_value = max,
            lua_type = luaType or type(1),
            name = name,
            Read = readFunc,
            Write = writeFunc
        }
        setmetatable(t, type_mt)
        return t
    end

    -- Type Definitions
    m.Bool = def("bool", 1, false, true, 
        function(b, p) return op.readu8(b, p) ~= 0 end,
        function(b, p, v) op.writeu8(b, p, v and 1 or 0) end,
        type(true)
    )
    -- Override validation for Bool
    m.Bool.ValidNumber = function() return true end
    m.Bool.EnforceNumber = function() end
    m.Bool.EnforceNumbers = function() end
    m.Bool.EnforceNumberAndPack = function(self, n) return n end

    m.Uint8    = def("uint8",   1, 0, 2^8-1,     op.readu8,   op.writeu8)
    m.Uint16   = def("uint16",  2, 0, 2^16-1,    op.readu16,  op.writeu16)
    m.Uint32   = def("uint32",  4, 0, 2^32-1,    op.readu32,  op.writeu32)
    m.Uint64   = def("uint64",  8, 0, 2^64-1,    op.readu64,  op.writeu64)
    
    m.Int8     = def("int8",    1, -2^7, 2^7-1,  op.readi8,   op.writei8)
    m.Int16    = def("int16",   2, -2^15, 2^15-1,op.readi16,  op.writei16)
    m.Int32    = def("int32",   4, -2^31, 2^31-1,op.readi32,  op.writei32)
    m.Int64    = def("int64",   8, -2^63, 2^63-1,op.readi64,  op.writei64)
    
    m.Float32  = def("float32", 4, nil, nil,     op.readf32,  op.writef32)
    m.Float64  = def("float64", 8, nil, nil,     op.readf64,  op.writef64)

    m.UOffsetT = m.Uint32
    m.VOffsetT = m.Uint16
    m.SOffsetT = m.Int32
    
    -- GenerateTypes compatibility hook; no-op in this implementation.
    local GenerateTypes = function(listOfTypes) end

    return m
end)()

local View = (function()
    local m = {}
    local mt = {}
    local mt_name = "flatbuffers.view.mt"
    local N = NumTypes
    local op = Compat

    -- The shared Global Buffer
    local GlobalBuffer = nil

    -- The method to replace the buffer for all objects
    function m.SetGlobalBuffer(buf)
        GlobalBuffer = buf
    end

    function m.New(buf, pos)
        local o = {
            -- Generated code passes 'buf', so keep it in the interface.
            pos = pos,
        }
        setmetatable(o, {__index = mt, __metatable = mt_name})
        return o
    end

    function mt:Offset(vtableOffset)
        local vtable = self.vtable
        if not vtable then
            -- vtable = pos - SOffset (Int32)
            vtable = self.pos - op.readi32(GlobalBuffer, self.pos)
            self.vtable = vtable
            -- vtableEnd = VOffset (Uint16)
            self.vtableEnd = op.readu16(GlobalBuffer, vtable)
        end
        if vtableOffset < self.vtableEnd then
            -- VOffset (Uint16)
            return op.readu16(GlobalBuffer, vtable + vtableOffset)
        end
        return 0
    end

    function mt:Indirect(off)
        return off + op.readu32(GlobalBuffer, off)
    end

    function mt:String(off)
        off = off + op.readu32(GlobalBuffer, off)
        local start = off + 4
        local length = op.readu32(GlobalBuffer, off)
        return op.readstring(GlobalBuffer, start, length)
    end

    function mt:VectorLen(off)
        off = off + self.pos
        off = off + op.readu32(GlobalBuffer, off)
        return op.readu32(GlobalBuffer, off)
    end

    function mt:Vector(off)
        off = off + self.pos
        return off + self:Get(N.UOffsetT, off) + 4
    end

    function mt:VectorAsString(off, start, stop)
        local o = self:Offset(off)
        if o ~= 0 then
            start = start or 1
            stop = stop or self:VectorLen(o)
            local a = self:Vector(o) + start - 1
            return op.readstring(GlobalBuffer, a, stop - start + 1)
        end
        return nil
    end

    function mt:Union(t2, off)
        assert(getmetatable(t2) == mt_name)
        off = off + self.pos
        t2.pos = off + self:Get(N.UOffsetT, off)
        -- Removed `t2.bytes = self.bytes` since all views use GlobalBuffer
    end

    function mt:Get(flags, off)
        -- Calling the raw buffer function
        return flags.Read(GlobalBuffer, off)
    end

    function mt:GetSlot(slot, d, validatorFlags)
        local off = self:Offset(slot)
        if off == 0 then
            return d
        end
        return self:Get(validatorFlags, self.pos + off)
    end

    function mt:GetVOffsetTSlot(slot, d)
        local off = self:Offset(slot)
        if off == 0 then
            return d
        end
        return off
    end

    return m
end)()

local Builder = (function()
    local N = NumTypes
    local ba = BinaryArray
    local op = Compat
    local getAlignSize = Compat.GetAlignSize

    local m = {}
    local mt = {}

    -- Locals for faster access
    local VOffsetT  = N.VOffsetT
    local UOffsetT  = N.UOffsetT
    local SOffsetT  = N.SOffsetT
    local Bool      = N.Bool
    local Uint8     = N.Uint8
    local Uint16    = N.Uint16
    local Uint32    = N.Uint32
    local Uint64    = N.Uint64
    local Int8      = N.Int8
    local Int16     = N.Int16
    local Int32     = N.Int32
    local Int64     = N.Int64
    local Float32   = N.Float32
    local Float64   = N.Float64

    local MAX_BUFFER_SIZE = 0x8000000 -- 128 MB
    local VtableMetadataFields = 2

    -- Compare current vtable against one stored in the buffer
    local function vtableEqual(a, objectStart, buf, vt2Start)
        -- Check length first
        local vt2Len = op.readu16(buf, vt2Start)
        if (#a * 2) ~= vt2Len then
            return false
        end

        for i, elem in ipairs(a) do
            local offsetInVtable = vt2Start + 4 + (i - 1) * 2
            local x = op.readu16(buf, offsetInVtable)
            
            -- x is offset from object start to field
            -- elem is offset from current buffer end to field
            if x ~= 0 or elem ~= 0 then
                local y = objectStart - elem
                if x ~= y then
                    return false
                end
            end
        end
        return true
    end

    function m.New(initialSize)
        local o = {
            finished = false,
            bytes = ba.New(initialSize),
            nested = false,
            head = initialSize,
            minalign = 1,
            vtables = {}
        }
        setmetatable(o, {__index = mt})
        return o
    end

    function mt:Clear()
        self.finished = false
        self.nested = false
        self.minalign = 1
        self.currentVTable = nil
        self.objectEnd = nil
        self.head = op.len(self.bytes)

        local vtable = self.vtables
        for i = 1, #vtable do vtable[i] = nil end
    end

    function mt:Output(full)
        assert(self.finished, "Builder Not Finished")
        local len = op.len(self.bytes)
        if full then
            local out = op.create(len)
            op.copy(out, 0, self.bytes, 0, len)
            return out
        else
            local sz = len - self.head
            local out = op.create(sz)
            op.copy(out, 0, self.bytes, self.head, sz)
            return out
        end
    end

    function mt:StartObject(numFields)
        assert(not self.nested)
        local vtable = {}
        for _ = 1, numFields do
            table.insert(vtable, 0)
        end
        self.currentVTable = vtable
        self.objectEnd = self:Offset()
        self.nested = true
    end

    function mt:WriteVtable()
        self:PrependSOffsetTRelative(0)
        local objectOffset = self:Offset()
        local existingVTable

        -- Cleanup empty vtables from cache
        local i = #self.vtables
        while i >= 1 do
            if self.vtables[i] == 0 then
                table.remove(self.vtables, i)
            end
            i = i - 1
        end

        -- Search for existing vtable
        i = #self.vtables
        while i >= 1 do
            local vt2Offset = self.vtables[i]
            local vt2Start = op.len(self.bytes) - vt2Offset
            
            if vtableEqual(self.currentVTable, objectOffset, self.bytes, vt2Start) then
                existingVTable = vt2Offset
                break
            end
            i = i - 1
        end

        if not existingVTable then
            -- Write new vtable
            i = #self.currentVTable
            while i >= 1 do
                local off = 0
                local a = self.currentVTable[i]
                if a and a ~= 0 then
                    off = objectOffset - a
                end
                self:PrependVOffsetT(off)
                i = i - 1
            end

            local objectSize = objectOffset - self.objectEnd
            self:PrependVOffsetT(objectSize)

            local vBytes = (#self.currentVTable + VtableMetadataFields) * 2
            self:PrependVOffsetT(vBytes)

            local objectStart = op.len(self.bytes) - objectOffset
            
            -- Patch the SOffsetT to point to this new vtable
            op.writei32(self.bytes, objectStart, self:Offset() - objectOffset)

            table.insert(self.vtables, self:Offset())
        else
            -- Point to existing vtable
            local objectStart = op.len(self.bytes) - objectOffset
            self.head = objectStart
            op.writei32(self.bytes, self.head, existingVTable - objectOffset)
        end

        self.currentVTable = nil
        return objectOffset
    end

    function mt:EndObject()
        assert(self.nested)
        self.nested = false
        return self:WriteVtable()
    end

    local function growByteBuffer(self, desiredSize)
        local s = op.len(self.bytes)
        assert(s < MAX_BUFFER_SIZE, "Flat Buffers cannot grow buffer beyond 128 MB")
        local newsize = s
        repeat
            newsize = math.min(newsize * 2, MAX_BUFFER_SIZE)
            if newsize == 0 then newsize = 1 end
        until newsize > desiredSize

        self.bytes = ba.Grow(self.bytes, newsize)
    end

    function mt:Head()
        return self.head
    end

    function mt:Offset()
        return op.len(self.bytes) - self.head
    end

    function mt:Pad(n)
        if n > 0 then
            local h = self.head - n
            self.head = h
            op.fill(self.bytes, h, 0, n)
        end
    end

    function mt:Prep(size, additionalBytes)
        if size > self.minalign then
            self.minalign = size
        end

        local h = self.head
        local k = op.len(self.bytes) - h + additionalBytes
        local alignsize = getAlignSize(k, size)
        local desiredSize = alignsize + size + additionalBytes

        while self.head < desiredSize do
            local oldBufSize = op.len(self.bytes)
            growByteBuffer(self, desiredSize)
            self.head = self.head + op.len(self.bytes) - oldBufSize
        end

        self:Pad(alignsize)
    end

    function mt:PrependSOffsetTRelative(off)
        self:Prep(4, 0)
        local off2 = self:Offset() - off + 4
        self:Place(off2, SOffsetT)
    end

    function mt:PrependUOffsetTRelative(off)
        self:Prep(4, 0)
        local off2 = self:Offset() - off + 4
        self:Place(off2, UOffsetT)
    end

    function mt:StartVector(elemSize, numElements, alignment)
        assert(not self.nested)
        self.nested = true
        local elementSize = elemSize * numElements
        self:Prep(4, elementSize)
        self:Prep(alignment, elementSize)
        return self:Offset()
    end

    function mt:EndVector(vectorNumElements)
        assert(self.nested)
        self.nested = false
        self:Place(vectorNumElements, UOffsetT)
        return self:Offset()
    end

    function mt:CreateString(s)
        assert(not self.nested)
        self.nested = true
        assert(type(s) == "string")

        self:Prep(4, #s + 1)
        self:Place(0, Uint8) -- Null terminator

        local l = #s
        self.head = self.head - l
        op.writestring(self.bytes, self.head, s)

        return self:EndVector(l)
    end

    function mt:CreateByteVector(x)
        assert(not self.nested)
        self.nested = true
        
        local l = #x
        self:Prep(4, l)
        self.head = self.head - l
        
        -- If x is a string, write directly
        if type(x) == "string" then
            op.writestring(self.bytes, self.head, x)
        else
            -- If x is a table/array
            for i = 1, l do
                op.writeu8(self.bytes, self.head + i - 1, x[i])
            end
        end

        return self:EndVector(l)
    end

    function mt:Slot(slotnum)
        assert(self.nested)
        self.currentVTable[slotnum + 1] = self:Offset()
    end

    local function finish(self, rootTable, sizePrefix)
        UOffsetT:EnforceNumber(rootTable)
        self:Prep(self.minalign, sizePrefix and 8 or 4)
        self:PrependUOffsetTRelative(rootTable)
        if sizePrefix then
            local size = op.len(self.bytes) - self.head
            Int32:EnforceNumber(size)
            self:PrependInt32(size)
        end
        self.finished = true
        return self.head
    end

    function mt:Finish(rootTable)
        return finish(self, rootTable, false)
    end

    function mt:FinishSizePrefixed(rootTable)
        return finish(self, rootTable, true)
    end

    function mt:Prepend(flags, off)
        self:Prep(flags.bytewidth, 0)
        self:Place(off, flags)
    end

    function mt:PrependSlot(flags, o, x, d)
        flags:EnforceNumbers(x, d)
        if x ~= d then
            self:Prepend(flags, x)
            self:Slot(o)
        end
    end

    function mt:PrependBoolSlot(...)    self:PrependSlot(Bool, ...) end
    function mt:PrependByteSlot(...)    self:PrependSlot(Uint8, ...) end
    function mt:PrependUint8Slot(...)   self:PrependSlot(Uint8, ...) end
    function mt:PrependUint16Slot(...)  self:PrependSlot(Uint16, ...) end
    function mt:PrependUint32Slot(...)  self:PrependSlot(Uint32, ...) end
    function mt:PrependUint64Slot(...)  self:PrependSlot(Uint64, ...) end
    function mt:PrependInt8Slot(...)    self:PrependSlot(Int8, ...) end
    function mt:PrependInt16Slot(...)   self:PrependSlot(Int16, ...) end
    function mt:PrependInt32Slot(...)   self:PrependSlot(Int32, ...) end
    function mt:PrependInt64Slot(...)   self:PrependSlot(Int64, ...) end
    function mt:PrependFloat32Slot(...) self:PrependSlot(Float32, ...) end
    function mt:PrependFloat64Slot(...) self:PrependSlot(Float64, ...) end

    function mt:PrependUOffsetTRelativeSlot(o,x,d)
        if x~=d then
            self:PrependUOffsetTRelative(x)
            self:Slot(o)
        end
    end

    function mt:PrependStructSlot(v,x,d)
        UOffsetT:EnforceNumber(d)
        if x~=d then
            UOffsetT:EnforceNumber(x)
            assert(x == self:Offset(), "Tried to write a Struct at an Offset that is different from the current Offset of the Builder.")
            self:Slot(v)
        end
    end

    function mt:PrependBool(x)     self:Prepend(Bool, x) end
    function mt:PrependByte(x)     self:Prepend(Uint8, x) end
    function mt:PrependUint8(x)    self:Prepend(Uint8, x) end
    function mt:PrependUint16(x)   self:Prepend(Uint16, x) end
    function mt:PrependUint32(x)   self:Prepend(Uint32, x) end
    function mt:PrependUint64(x)   self:Prepend(Uint64, x) end
    function mt:PrependInt8(x)     self:Prepend(Int8, x) end
    function mt:PrependInt16(x)    self:Prepend(Int16, x) end
    function mt:PrependInt32(x)    self:Prepend(Int32, x) end
    function mt:PrependInt64(x)    self:Prepend(Int64, x) end
    function mt:PrependFloat32(x)  self:Prepend(Float32, x) end
    function mt:PrependFloat64(x)  self:Prepend(Float64, x) end
    function mt:PrependVOffsetT(x) self:Prepend(VOffsetT, x) end

    function mt:Place(x, flags)
        flags:EnforceNumber(x)
        local h = self.head - flags.bytewidth
        self.head = h
        -- Calling the raw buffer function
        flags.Write(self.bytes, h, x)
    end

    function mt:EnforceNumberAndPack(x, flags)
        -- Compatibility hook; unused by the buffer implementation.
        return x
    end

    return m
end)()

local FlatBuffers = {}
FlatBuffers.Builder = Builder.New
FlatBuffers.N = NumTypes
FlatBuffers.view = View
FlatBuffers.binaryArray = BinaryArray

-- Expose the global buffer replacement method
FlatBuffers.SetGlobalBuffer = View.SetGlobalBuffer

return FlatBuffers

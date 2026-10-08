// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/data_context.h"
#include "gprx_container/types.h"
#include "gprx_container/utils.h"

// Shared, side-effect-free implementation of the engine's "read a range of the live
// capture" surface (MicroProfilerService:GetDataInRange). It lives in LibMP's
// integration layer so the engine bridge, the standalone tests, and the fuzz harness all
// exercise ONE copy of the offset decoding and bounds validation instead of
// re-implementing them. The engine
// wrapper only needs to extract the Luau arguments and forward them here.
//
// The offset is a single Luau number (an integer exact up to bit 52). Bit 52 selects the
// addressing mode: when set, the value packs a block id and a block-relative offset; when
// clear, the value is an absolute container offset.

namespace LibMP
{

// Luau numbers are IEEE-754 doubles. Integers through 2^53-1 are exactly
// representable; values outside that range, fractions, infinities, and NaNs must be
// rejected before a cast to an unsigned native type.
constexpr uint64_t MAX_EXACT_LUAU_INTEGER = (uint64_t{1} << 53) - 1;

inline bool TryConvertLuauUnsignedInteger(double value, uint64_t limit, uint64_t& out)
{
    // The ordered comparisons reject NaN as well as negatives and infinities.
    if (!(value >= 0.0) || value > static_cast<double>(limit))
        return false;

    const uint64_t converted = static_cast<uint64_t>(value);
    if (static_cast<double>(converted) != value)
        return false;

    out = converted;
    return true;
}

inline bool TryConvertSlotId(int64_t value, uint32_t& out)
{
    if (value < 0 || static_cast<uint64_t>(value) > GPRX::max_u32)
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

struct ValidatedDataRangeArguments
{
    uint32_t slot_id = 0;
    uint64_t packed_offset = 0;
    uint32_t size = 0;
    uint32_t destination_offset = 0;
};

// Validates the exact values crossing the Luau MicroProfilerService boundary before
// any narrowing or pointer arithmetic. Keeping this independent of lua_State lets the
// engine wrapper and standalone tests share the security-critical policy.
inline bool ValidateDataRangeArguments(
    int64_t slot_id,
    double packed_offset,
    int64_t size,
    uint64_t destination_capacity,
    int64_t destination_offset,
    ValidatedDataRangeArguments& out)
{
    uint64_t converted_offset = 0;
    uint32_t converted_slot = 0;
    if (!TryConvertSlotId(slot_id, converted_slot) ||
        !TryConvertLuauUnsignedInteger(packed_offset, MAX_EXACT_LUAU_INTEGER, converted_offset) ||
        size < 0 || static_cast<uint64_t>(size) > GPRX::max_mpsize_t ||
        destination_offset < 0 || static_cast<uint64_t>(destination_offset) > GPRX::max_mpsize_t)
        return false;

    const uint64_t unsigned_size = static_cast<uint64_t>(size);
    const uint64_t unsigned_destination_offset = static_cast<uint64_t>(destination_offset);
    if (!GPRX::range_within(unsigned_destination_offset, unsigned_size, destination_capacity))
        return false;

    out.slot_id = converted_slot;
    out.packed_offset = converted_offset;
    out.size = static_cast<uint32_t>(unsigned_size);
    out.destination_offset = static_cast<uint32_t>(unsigned_destination_offset);
    return true;
}

struct DecodedDataOffset
{
    bool is_block_relative = false;
    uint32_t block_id = 0;    // meaningful only when is_block_relative
    uint64_t offset = 0;      // block-relative offset, or absolute container offset
};

inline DecodedDataOffset DecodeDataOffset(uint64_t packed_offset)
{
    constexpr uint64_t BLOCK_MARKER_BIT = uint64_t(1) << 52;
    DecodedDataOffset decoded;
    if (packed_offset & BLOCK_MARKER_BIT)
    {
        // Higher bits (excluding the marker) hold the block id; the low 32 bits hold the
        // block-relative offset. Block id zero is the existing API's absolute-addressing
        // sentinel, even when the marker is present, so preserve that wire behavior.
        decoded.block_id = static_cast<uint32_t>((packed_offset & ~BLOCK_MARKER_BIT) >> 32);
        decoded.is_block_relative = decoded.block_id != 0;
        decoded.offset = packed_offset & 0xFFFFFFFFull;
    }
    else
    {
        decoded.offset = packed_offset;
    }
    return decoded;
}

// Reads `size` bytes selected by `packed_offset` into `dest + dest_offset`, validating the
// destination window and delegating the block/absolute range check to the data context
// (which owns the block metadata). Returns the number of bytes read, or 0 on any rejection.
//
// Unlike an inline uint64->uint32 narrowing, the absolute path keeps the 64-bit offset:
// a value up to bit 52 cannot be represented in the block-relative offset field, so the
// block path rejects it rather than truncating.
inline uint32_t ReadDataInRange(
    GPRX::IDataContext& context,
    uint64_t packed_offset,
    uint32_t size,
    uint8_t* dest,
    uint32_t dest_capacity,
    uint32_t dest_offset)
{
    if (packed_offset > MAX_EXACT_LUAU_INTEGER ||
        !dest || !GPRX::range_within(dest_offset, size, dest_capacity))
        return 0;

    const DecodedDataOffset decoded = DecodeDataOffset(packed_offset);
    uint8_t* out = dest + dest_offset;

    if (decoded.is_block_relative)
    {
        // The block-relative offset field is 32 bits; a wider value is out of range.
        if (decoded.offset > GPRX::max_mpsize_t)
            return 0;
        return context.ReadBlockDataToBuffer(
            decoded.block_id, static_cast<mpsize_t>(decoded.offset), size, out, size);
    }

    return context.ReadAbsDataToBuffer(decoded.offset, size, out, size);
}

} // namespace LibMP

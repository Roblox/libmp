// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/block.h"
#include "ntd_stdlib/type_traits.h"

namespace LibMP
{

enum class BlockTypeEx : ntd::underlying_type_t<GPRX::BlockType>
{
    GPRX_BLOCK_TYPES_BASIC

    BasicMeta = 2050,
    TrackStart = 2070,
    TrackFinish = 2071,
    SegmentStart = 2080,
    SegmentFinish = 2081,
};

enum class BlockTypeMeta : uint8_t // Matches GPRX::BlockHeader.type_id
{
    Generic = 0,

    Data_ShortTerm = 10,
    Data_LongTerm = 20,
    Data_Frames = 30,
    Data_FramesEx = 31,
    Data_ThreadLogs = 40,
    Data_Extra = 50,

    /*
     * Used in MpSession to build a lookup table mapping query types to KV stores
     */
    Kv_ShortTerm = 1,
    Kv_LongTerm = 2,
    Kv_Frames = 3,
    Kv_FramesEx = 4,
    Kv_Counters = 5,
    Kv_Extra = 6,
    Kv_SubTypes_Count,
};

class BlockEx
{
public:
    inline static uint32_t GetDataBlockFrameIdFromMeta(const GPRX::BlockHeader* headerPtr)
    {
        return GPRX::Block::GetMeta1View(headerPtr)->parts.high;
    }

    inline static void PutDataBlockFrameIdToMeta(const GPRX::BlockHeader* headerPtr, uint32_t frameId)
    {
        GPRX::Block::GetMeta1View(headerPtr)->parts.high = frameId;
    }

    inline static bool IsKvStoreCacheLongTerm(uint8_t subType)
    {
        return subType == static_cast<uint8_t>(BlockTypeMeta::Kv_LongTerm);
    }

    inline static bool IsKvStoreFrameDependent(uint8_t subType)
    {
        return subType == static_cast<uint8_t>(BlockTypeMeta::Kv_Frames) ||
            subType == static_cast<uint8_t>(BlockTypeMeta::Kv_FramesEx);
    }
};

class BlockBasicMeta
{
public:
    inline static uint32_t GetBasicMetaBlockFrameIdMinFromMeta(const GPRX::BlockHeader* headerPtr)
    {
        return GPRX::Block::GetMeta1View(headerPtr)->parts.low;
    }

    inline static uint32_t GetBasicMetaBlockFrameIdMaxFromMeta(const GPRX::BlockHeader* headerPtr)
    {
        return GPRX::Block::GetMeta1View(headerPtr)->parts.high;
    }

    inline static void PutBasicMetaBlockFrameIdMinToMeta(GPRX::BlockHeader* headerPtr, uint32_t frameIdMin)
    {
        GPRX::Block::GetMeta1View(headerPtr)->parts.low = frameIdMin;
    }

    inline static void PutBasicMetaBlockFrameIdMaxToMeta(GPRX::BlockHeader* headerPtr, uint32_t frameIdMax)
    {
        GPRX::Block::GetMeta1View(headerPtr)->parts.high = frameIdMax;
    }
};

} // LibMP

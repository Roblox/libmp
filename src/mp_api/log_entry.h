// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/types.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Definitions related to the MicroProfiler generic thread log entry format

namespace LibMP
{

#define MP_LOG_TICK_MASK 0x0000ffffffffffff
#define MP_LOG_INDEX_MASK 0x1fff000000000000
#define MP_LOG_BEGIN_MASK 0xe000000000000000
#define MP_LOG_EVENT 0x6
#define MP_LOG_LABEL_LITERAL 0x5
#define MP_LOG_GPU_EXTRA 0x4
#define MP_LOG_LABEL 0x3
#define MP_LOG_META 0x2
#define MP_LOG_ENTER 0x1
#define MP_LOG_LEAVE 0x0

inline uint64_t MicroProfilerLogMakeEntry(uint64_t entryType, uint64_t timerId, uint64_t tick)
{
    return (entryType << 61) | (MP_LOG_INDEX_MASK & (timerId << 48)) | (MP_LOG_TICK_MASK & tick);
}

inline uint32_t MicroProfilerLogTypeU32(const uint64_t* entry)
{
    const GPRX::U64Split* split_view = reinterpret_cast<const GPRX::U64Split*>(entry);
    uint32_t entry_high = split_view->parts.high;
    // Original: 0xe000000000000000 (bits 61-63) -> High word bits 29-31
    return entry_high >> 29;
}

inline uint32_t MicroProfilerLogTimerIdU32(const uint64_t* entry)
{
    const GPRX::U64Split* split_view = reinterpret_cast<const GPRX::U64Split*>(entry);
    uint32_t entry_high = split_view->parts.high;
    // Original: 0x1fff000000000000 (bits 48-60) -> High word bits 16-28
    return (entry_high >> 16) & 0x1FFF;
}

inline uint32_t MicroProfilerLogType(uint64_t entry)
{
    return static_cast<uint32_t>((MP_LOG_BEGIN_MASK & entry) >> 61);
}

inline uint32_t MicroProfilerLogTimerId(uint64_t entry)
{
    return static_cast<uint32_t>((MP_LOG_INDEX_MASK & entry) >> 48);
}

inline uint64_t MicroProfilerLogTick(uint64_t entry)
{
    return MP_LOG_TICK_MASK & entry;
}

inline int64_t MicroProfilerLogTickDiff(uint64_t start, uint64_t end)
{
    int64_t difference = static_cast<int64_t>((end - start) << 16);
    return difference >> 16;
}

} // LibMP

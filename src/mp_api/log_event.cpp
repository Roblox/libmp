// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "log_event.h"
#include "gprx_container/utils.h"
#include "ntd_stdlib/cstring.h"

namespace LibMP
{

inline static mpsize_t DecodeEventExtraData(uint8_t* extraData, const uint64_t* entry, uint32_t eventSize)
{
    const uint32_t entryDataLen = MP_EVENT_LOGENTRY_DATALEN;
    const uint32_t entriesCount = MicroProfilerEventSizeToEntryCount(eventSize);
    const uint32_t extraDataSize = entryDataLen * entriesCount;

    XASSERT_TRAP(GPRX::is_little_endian());
    const uint64_t* dataStart = entry + 1;
    for (uint32_t i = 0; i < entriesCount; i++)
    {
        ntd::memcpy(extraData, &dataStart[i], entryDataLen);
        extraData += entryDataLen;
    }

    return extraDataSize;
}

void MicroProfilerDecodeEvent(const uint64_t* entry, uint32_t eventSize, bool hasComment, DecodedMpEvent& decodedEvent)
{
    auto& e = decodedEvent;

    // Extract main data from the first entry
    const uint64_t firstEntry = entry[0];
    e.data = MicroProfilerEventEntryData(firstEntry);
    e.eventType = MicroProfilerEventEntryType(firstEntry);

    // Extract extraData from dependent entries
    e.extraDataSize = DecodeEventExtraData(&e.extraData[0], entry, eventSize);

    // Extract optional comment entry
    if (hasComment)
    {
        uint32_t lastOffset = MicroProfilerEventEntryLastOffset(eventSize, true);
        const uint64_t lastEntry = entry[lastOffset];
        ntd::tie(e.stringToken.isGlobal, e.stringToken.isLiteral) = MicroProfilerEventCommentInfo(lastEntry);
        e.stringToken.token = MicroProfilerLogTick(lastEntry);
        e.stringToken.isPresent = true;
    }
    else
    {
        e.stringToken.isPresent = false;
    }
}

} // LibMP

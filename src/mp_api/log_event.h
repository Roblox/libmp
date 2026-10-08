// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "log_entry.h"
#include "log_event_types.h"
#include "ntd_stdlib/tuple.h"

namespace LibMP
{

////////////////////////////////////////////////////////////////////////////////////////////////////
// Definitions related to the MicroProfiler event record format

// The Event can accommodate more than one entry, depending on the payload length:
// Size | Max payload                          | Memory usage
// S    | 50 bits            + optional string | 1 Log entry   + 1 optional
// M    | 50 bits + 10 bytes + optional string | 3 Log entries + 1 optional
// L    | 50 bits + 20 bytes + optional string | 5 Log entries + 1 optional
#define MP_EVENT_SIZE_S       0
#define MP_EVENT_SIZE_M       1
#define MP_EVENT_SIZE_L       2
#define MP_EVENT_SIZE_INVALID 3 // Marker of a dependent entry

#define MP_EVENT_SIZESTEP_DATALEN 10 // Each size step adds 10 bytes to the payload
#define MP_EVENT_LOGENTRY_DATALEN (MP_EVENT_SIZESTEP_DATALEN / 2) // One size step corresponds to 2 Log entries
#define MP_EVENT_LOGENTRY_DATAGAP (sizeof(uint64_t) - MP_EVENT_LOGENTRY_DATALEN)
#define MP_EVENT_EXTRABUFLEN_S (MP_EVENT_SIZE_S * MP_EVENT_SIZESTEP_DATALEN)
#define MP_EVENT_EXTRABUFLEN_M (MP_EVENT_SIZE_M * MP_EVENT_SIZESTEP_DATALEN)
#define MP_EVENT_EXTRABUFLEN_L (MP_EVENT_SIZE_L * MP_EVENT_SIZESTEP_DATALEN)

#define MP_LOG_ENTRY_MAXNUM 6 // L-size MP_LOG_EVENT occupies 6 Log entries max

////////////////////////////////////////////////////////////////////////////////////////////////////
// The log entry of type MP_LOG_EVENT has a custom layout:
#define MP_LOG_EVENT_DATA_MASK     0x0003ffffffffffff //50 bits: data
#define MP_LOG_EVENT_TYPE_MASK     0x03fc000000000000 // 8 bits: event type, or custom flags for dependent entries
#define MP_LOG_EVENT_SIZE_MASK     0x0c00000000000000 // 2 bits: event size (S, M, L), or marker of a dependent entry
#define MP_LOG_EVENT_HASLABEL_MASK 0x1000000000000000 // 1 bit : marker of the presence of the attached label

// Field layout for the Event in Log:
// [main:hasLabel,size,type,data] [dependent:data5bytes] [dependent:data5bytes] [dependent:label]

////////////////////////////////////////////////////////////////////////////////////////////////////
inline uint32_t MicroProfilerEventSizeToEntryCount(uint32_t eventSize)
{
    return eventSize * 2;
}

inline uint32_t MicroProfilerEventEntryLastOffset(uint32_t eventSize, bool hasComment)
{
    return MicroProfilerEventSizeToEntryCount(eventSize) + (hasComment ? 1 : 0);
}

inline ntd::tuple<bool, uint32_t, bool> MicroProfilerEventEntryInfo(uint64_t e)
{
    bool hasComment = !!((e & MP_LOG_EVENT_HASLABEL_MASK) >> 60);
    uint32_t eventSize = static_cast<uint32_t>((e & MP_LOG_EVENT_SIZE_MASK) >> 58);
    bool isExtraEntry = (hasComment && eventSize == MP_EVENT_SIZE_INVALID);
    return ntd::make_tuple(hasComment, eventSize, isExtraEntry);
}

inline ntd::tuple<bool, bool> MicroProfilerEventCommentInfo(uint64_t e)
{
    bool isGlobal = !!((e >> 57) & 0x1);
    bool isLiteral = !!((e >> 56) & 0x1);
    return ntd::make_tuple(isGlobal, isLiteral);
}

inline MpEventType MicroProfilerEventEntryType(uint64_t e)
{
    return static_cast<MpEventType>((e & MP_LOG_EVENT_TYPE_MASK) >> 50);
}

inline uint64_t MicroProfilerEventEntryData(uint64_t e)
{
    return e & MP_LOG_EVENT_DATA_MASK;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
struct DecodedMpEvent
{
    struct StringToken
    {
        uint64_t token;
        bool isPresent;
        bool isGlobal;
        bool isLiteral;
    };

    MpEventType eventType;
    uint64_t data;
    alignas(uint64_t) uint8_t extraData[MP_EVENT_EXTRABUFLEN_L];
    mpsize_t extraDataSize;
    StringToken stringToken;
};

// Convert log entries into original Event
void MicroProfilerDecodeEvent(const uint64_t* entry, uint32_t eventSize, bool hasComment, DecodedMpEvent& decodedEvent);

} // LibMP

// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/uid.h"
#include "ntd_stdlib/type_traits.h"

namespace LibMP
{

enum class QueryEx : ntd::underlying_type_t<GPRX::Query>
{
    GPRX_QUERIES_BASIC

    GlobalDesc = 1,
    GeneralInfo,

    CounterIds = 10,
    CounterDesc_u24CounterId,
    GroupIdMax,
    GroupDesc_u24GroupId,
    TimerIds,
    TimerDesc_u24TimerId,
    ThreadIds,
    ThreadDesc_u24ThreadId,
    ThreadIdGpu,

    FrameIdMin = 30,
    FrameIdMax,
    FrameDesc_u24FrameId,

    LabelBufferRegions = 42,

    CounterSamples_u24CounterId = 50,
    PlaceIdSamples,
    UtcTimestampSamples,

    _CompactUidThreshold = GPRX::Uid::BigOperationIdMarker, // 127
};

} // LibMP

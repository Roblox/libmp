// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "gprx_container/types.h"

// Global namespace

enum MpNetReplica_Subtype : uint8_t
{
    MpNetReplica_Min = 0,
    MpNetReplica_General = 0,

    MpNetReplica_Instance_Min = 1,
    MpNetReplica_Physics = 1,
    MpNetReplica_PhysicsTouch,
    MpNetReplica_Input,
    MpNetReplica_ItemTypeNew,
    MpNetReplica_ItemTypeDelete,
    MpNetReplica_ItemTypeJoinData,
    MpNetReplica_ItemTypeChangeProperty,
    MpNetReplica_ItemTypeChangeAttribute,
    MpNetReplica_InstanceOther,
    MpNetReplica_ItemTypeMarker,
    MpNetReplica_ItemTypePing,
    MpNetReplica_ItemTypeEventInvocation,
    MpNetReplica_ItemTypeTag,
    MpNetReplica_ItemTypeStats,
    MpNetReplica_Terrain,
    MpNetReplica_Instance_Max = 15,

    // Streaming Items
    MpNetReplica_Streaming_Min = 16,
    MpNetReplica_ItemTypeStreamData = 16,
    MpNetReplica_ItemTypeStreamDataInfo,
    MpNetReplica_ItemTypeRegionRemoval,
    MpNetReplica_ItemTypeInstanceRemoval,
    MpNetReplica_ItemTypeStreamTerrain,
    MpNetReplica_StreamingOther,
    MpNetReplica_Streaming_Max = 21,

    // Large Replicator Data
    MpNetReplica_LRData = 30,

    // Collection Event
    MpNetReplica_BatchData = 31,

    // Max value, 5 bits
    MpNetReplica_Max = 31,
};

enum MpNetCurl_Subtype : uint8_t
{
    MpNetCurl_Min = 0,

    MpNetCurl_AssetRequest = 0,
    MpNetCurl_AssetResponse,
    MpNetCurl_CdnRequest,
    MpNetCurl_AssetDelivery,
    MpNetCurl_BatchAssetIdReport,
    MpNetCurl_CdnAssetIdReport,

    // Max value, 3 bits
    MpNetCurl_Max = 7,
};

enum MpEventType : uint8_t
{
    MpEvent_Invalid = 0,

    MpEvent_Net_Min = 1,
    MpEvent_NetCurlRx = 1,
    MpEvent_NetCurlTx,
    MpEvent_NetReplicaRx,
    MpEvent_NetReplicaTx,
    MpEvent_Net_Max = 16,

    MpEvent_Mem_Min = 17,
    MpEvent_MemAlloc = 17,
    MpEvent_MemFree,
    MpEvent_MemRealloc,
    MpEvent_MemScopeStats,
    MpEvent_Mem_Max = 28,

    MpEvent_Span_Min = 29,
    MpEvent_SpanBegin = 29,
    MpEvent_SpanEnd,
    MpEvent_Span_Max = 32,

    MpEvent_Multitasking_Min = 33,
    MpEvent_MultitaskingThreadInfo = 33,
    MpEvent_Multitasking_Max = 36,

    MpEvent_Cpu_CoreAndFrequency = 37,
    MpEvent_Cpu_Max = 40,

    MpEvent_Test_Min = 119,
    MpEvent_Test0 = 119,
    MpEvent_Test1,
    MpEvent_Test2,
    MpEvent_Test3,
    MpEvent_Test4,
    MpEvent_Test5,
    MpEvent_Test6,
    MpEvent_Test7,
    MpEvent_Test_Max = 126,

    MpEvent_Max = 127
};

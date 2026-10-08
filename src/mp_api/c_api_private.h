// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "c_api_internal.h"
#include "block_ex.h"
#include "uid_ex.h"
#include "kv_store_ex.h"
#include "gprx_container/gprx.h"
#include "ntd_stdlib/array.h"
#include "ntd_stdlib/deque.h"
#include "ntd_stdlib/memory.h"
#include "ntd_stdlib/optional.h"
#include "ntd_stdlib/string.h"
#include "ntd_stdlib/unordered_map.h"
#include "ntd_stdlib/unordered_set.h"
#include "ntd_stdlib/vector.h"

extern "C"
{

// Private WASM plumbing for wrapper-owned label-cache invalidation.
MP_API_EXPORT MpArray* MpInternalGetLabelBufferRegions(MpSession* session);
MP_API_EXPORT const uint8_t* MpInternalGetLabelBufferRegionFromArray(
    MpSession* session, MpArray* array, mpsize_t index);
MP_API_EXPORT uint32_t MpInternalGetLabelCacheChunkSize();

}

////////////////////////////////////////////////////////////////////////////////////////////////////
struct MpSession
{
    static constexpr uint32_t LABEL_CACHE_CHUNK_SIZE = 8 * 1024;
    static constexpr uint32_t LABEL_REGION_LOOKUP_CHUNK_SIZE = 64 * 1024;
    static constexpr mpsize_t LABEL_REGION_LINEAR_SEARCH_LIMIT = 4;
    static constexpr mpsize_t LABEL_REGION_LOOKUP_CACHE_SIZE = 256;

    struct LabelRegion
    {
        uint64_t startOffset = 0;
        uint64_t endOffset = 0;
        GPRX::Locator bufferStartLocator = {};

        bool operator<(const LabelRegion& other) const
        {
            return startOffset < other.startOffset;
        }
    };

    struct LabelRegionLookupEntry
    {
        // Zero is invalid; valid lookup chunk IDs are stored as chunkId + 1.
        uint64_t encodedChunkId;
        mpsize_t firstRegionIndex;
        mpsize_t onePastLastRegionIndex;
    };

    using LabelCacheChunk = ntd::unordered_map<uint64_t, ntd::string>;

    enum CacheBucket : mpsize_t
    {
        ShortTerm = 0,
        LongTerm = 1,
        Count,
    };

    struct KVStoreProxyDesc 
    {
        ntd::unique_ptr<GPRX::KVStoreReaderProxy> proxy;
        uint32_t frameIdMax = 0;
    };

    struct KVStoreProxyGroup
    {
        ntd::vector<KVStoreProxyDesc> list;
        mpsize_t activeCount = 0;
    };

    static constexpr mpsize_t CACHABLE_OBJECT_TYPES_COUNT = 6;
    static constexpr mpsize_t CACHABLE_ARRAY_TYPES_COUNT = 7;
    static constexpr mpsize_t CACHABLE_SCALAR_TYPES_COUNT = 4;

    struct CachedObject
    {
        const uint8_t* data = nullptr; // non-owning; block persistence owns the bytes
        mpsize_t size = 0;
    };

    using ObjectCacheArray = ntd::array<
        ntd::unordered_map<uint32_t, ntd::optional<CachedObject>>,
        CacheBucket::Count * CACHABLE_OBJECT_TYPES_COUNT>;
    using ArrayCacheArray = ntd::array< // Acquired ref-counted array pointers
        ntd::unordered_map<uint32_t, ntd::optional<MpArray*>>,
        CacheBucket::Count * CACHABLE_ARRAY_TYPES_COUNT>;
    using ScalarCacheArray = ntd::array<ntd::optional<uint32_t>, CACHABLE_SCALAR_TYPES_COUNT>;

    ntd::unique_ptr<ObjectCacheArray> objectCache;
    ntd::unique_ptr<ArrayCacheArray> arrayCache;
    ntd::unique_ptr<ScalarCacheArray> scalarCache;

    using ObjectCacheBucketBlockIds = ntd::array<GPRX::HybridLookupSet, CacheBucket::Count>;
    using ArrayCacheBucketBlockIds = ntd::array<ntd::unordered_set<uint32_t>, CacheBucket::Count>;
    
    ObjectCacheBucketBlockIds objectCacheBucketBlockIds;
    ArrayCacheBucketBlockIds arrayCacheBucketBlockIds;
    ArrayCacheBucketBlockIds arrayCacheBucketBlockIdsNew;

    bool bObjectCacheEnabled = false;
    bool bArrayCacheEnabled = false;
    bool bScalarCacheEnabled = false;
    ntd::vector<uint32_t> syncClearedObjectCacheBuckets;
    ClearObjCacheCallback clearObjCacheCbFn = nullptr;
    uint8_t* clearObjCacheCbHandle = nullptr;
    SessionNotificationCallback sessionNotificationCbFn = nullptr;
    uint8_t* sessionNotificationCbHandle = nullptr;
    bool bSyncRequired = false;
    bool bClearCacheOnSyncRequired = false;

    // Label blocks are indexed only when a label is first requested. The index is
    // invalidated by sync/selection. Region candidate ranges are cached in 64 KiB
    // chunks; cached strings use separate 8 KiB chunks for precise eviction.
    ntd::vector<LabelRegion> labelRegions;
    ntd::vector<uint64_t> labelRegionPrefixMaxEnds;
    mutable ntd::array<
        LabelRegionLookupEntry, LABEL_REGION_LOOKUP_CACHE_SIZE>
        labelRegionLookupCache = {};
    ntd::unordered_map<uint64_t, LabelCacheChunk> labelCache;
    ntd::string labelScratch;
    bool bLabelLookupInitialized = false;
    bool bLabelLookupDirty = true;
    bool bLabelCacheEnabled = false;
    GPRX::Uid global_uid = {};
    uint8_t global_query_subtype = 0;
    uint32_t global_query_frame_id = 0;
    mpsize_t global_obj_size = 0;
    GPRX::Locator global_obj_locator = {};

    LibMP::TypesNtd::U64Sample global_u64_sample = {};
    LibMP::TypesNtd::CounterSample global_counter_sample = {};
    LibMP::TypesNtd::TrackDesc global_track_desc = {};
    LibMP::TypesNtd::SegmentDesc global_segment_desc = {};
    LibMP::ValueBufEx global_val_buf = {};

    ntd::unique_ptr<GPRX::IDataContext> dataSource;
    ntd::unique_ptr<GPRX::ContainerContext> dumpContainer;

    bool IsInputTrusted() const
    {
        return dumpContainer && dumpContainer->IsInputTrusted();
    }

    using BlocksBySubTypeArray = ntd::array<
        ntd::vector<GPRX::BlockContext*>,
        static_cast<mpsize_t>(LibMP::BlockTypeMeta::Kv_SubTypes_Count)>;
    BlocksBySubTypeArray blocksBySubType;

    using SelectedKVStoreArray = ntd::array<
        KVStoreProxyGroup,
        static_cast<mpsize_t>(LibMP::BlockTypeMeta::Kv_SubTypes_Count)>;
    SelectedKVStoreArray selectedKVStores;
    ntd::unique_ptr<GPRX::KVStoreReaderProxy> genericKVStoreProxy;
    
    // Acquired ref-counted array pointers
    MpArray* tracks = nullptr;
    ntd::vector<MpArray*> track_segments;
    ntd::deque<mpsize_t> trackSegmentCounts;
    mpsize_t selected_track = 0;
    mpsize_t selected_segment = 0;
    uint32_t basicMetaFrameIdMax = 0;

    // Live iterators, detached by MpCloseSession so they can outlive the session
    ntd::vector<MpLogIterator*> logIterators;
    ntd::vector<MpCounterIterator*> counterIterators;

    ~MpSession();
    void ClearObjectCache(CacheBucket bucket = CacheBucket::Count);
    void ClearArrayCache(CacheBucket bucket = CacheBucket::Count);
    void ClearScalarCache();
    void ClearTrackSegments();
    void NotifySession(uint32_t notificationFlags);
    void InvalidateLabelLookup(uint32_t notificationFlags = 0);
    static uint64_t GetLabelCacheChunkId(uint64_t offset);
    static uint64_t GetLabelRegionLookupChunkId(uint64_t offset);
    void RebuildLabelRegionLookupIndex();
    bool IsLabelCacheChunkFullyCovered(uint64_t chunkId) const;
    bool RefreshLabelLookup();
    const LabelRegion* FindLabelRegion(uint64_t offset) const;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
inline static bool CheckSync(MpSession* session)
{
    if (!session)
        return false;
    if (session->bSyncRequired)
    {
        MpSyncWithDataSource(session);
        return true;
    }
    return false;
}

// Internal count-only counterpart to MpGetCounterIds; does not materialize an MpArray.
mpsize_t MpGetCounterIdsCountInternal(MpSession* session);

// Detaches the session's iterators from it; their data locks are dropped with the session.
// Detached iterators stop stepping but stay safe to query and delete after the session is closed.
void MpDetachIterators(MpSession* session);

////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
class RefCounted
{
    mpsize_t m_count = 1;

public:
    void Acquire()
    {
        m_count++;
    }

    void Release()
    {
        if (m_count == 0)
        {
            XASSERT_DIAG(false,
                "RefCounted::Release - reference count must be positive");
            return;
        }
        m_count--;
        if (m_count == 0)
        {
            ntd::Delete(static_cast<T*>(this));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
struct MpArray : RefCounted<MpArray>
{
    ntd::vector<uint32_t> entries;
    mpsize_t value_size_in_u32 = 0;
    mpsize_t values_count = 0;
    bool is_valid = false;

    LibMP::ValueBufEx value_buf;

    static MpArray* Create()
    {
        return ntd::New<MpArray>();
    }

    uint32_t GetValueU32(mpsize_t index)
    {
        return entries[index];
    }

    uint64_t GetValueU64(mpsize_t index)
    {
        LibMP::ValueBufEx& value = GetValue(index);
        return value.ToU64();
    }

    int64_t GetValueI64(mpsize_t index)
    {
        LibMP::ValueBufEx& value = GetValue(index);
        return value.ToI64();
    }

    LibMP::ValueBufEx& GetValue(mpsize_t index)
    {
        value_buf.AssignExternalBuf(&entries[index * value_size_in_u32], value_size_in_u32);
        return value_buf;
    }

    static inline void ReleaseAndNull(MpArray*& a)
    {
        if (a)
        {
            a->Release();
            a = nullptr;
        }
    }
};

inline static MpArray* MpCreateArray()
{
    return MpArray::Create();
}

template <typename Func>
inline static void MpIterateAndReleaseArray(MpArray* arr, Func&& fn)
{
    if (!arr)
        return;

    mpsize_t count = MpGetArraySize(arr);
    for (mpsize_t i = 0; i < count; i++)
    {
        fn(arr, i);
    }

    MpReleaseArray(arr);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
class DataLockHelper
{
    MpSession* m_session;
    bool is_valid;

public:
    DataLockHelper(MpSession* session)
        : m_session(session)
    {
        is_valid = MpPushDataLock(m_session);
        XASSERT_DIAG(is_valid, "DataLockHelper: Failed to push data lock");
    }

    ~DataLockHelper()
    {
        if (is_valid)
            MpPopDataLock(m_session);
    }

    bool IsValid() const
    {
        return is_valid;
    }
};

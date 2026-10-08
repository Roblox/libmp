// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "c_api_private.h"
#include "control_channel.h"
#include "ntd_stdlib/tuple.h"
#include "ntd_stdlib/utility.h"

using namespace GPRX;
using namespace LibMP;

////////////////////////////////////////////////////////////////////////////////////////////////////
struct TrackSegment
{
    static constexpr mpsize_t RECORD_SIZE_IN_U32 = 4;

    struct BlockIdNumber
    {
        uint32_t id;
        mpsize_t number;
    };

    BlockIdNumber startBlock;
    BlockIdNumber finishBlock;

    bool IsImplicit() const
    {
        return startBlock.id == 0;
    }

    bool IsFinalized() const
    {
        return startBlock.id != finishBlock.id;
    }

    static void PutToArray(ntd::vector<uint32_t>& entries, const TrackSegment& entry)
    {
        entries.push_back(entry.startBlock.id);
        entries.push_back(entry.startBlock.number);
        entries.push_back(entry.finishBlock.id);
        entries.push_back(entry.finishBlock.number);
    }

    static void ModifyLastFinish(
        ntd::vector<uint32_t>& entries, const BlockIdNumber& finishBlock)
    {
        if (entries.size() < RECORD_SIZE_IN_U32)
            return;
        const mpsize_t i = GPRX::to_mpsize(entries.size()) - RECORD_SIZE_IN_U32;
        entries[i + 2] = finishBlock.id;
        entries[i + 3] = finishBlock.number;
    }

    static bool GetFromArray(const MpArray* array, mpsize_t index, TrackSegment& result)
    {
        if (!array || array->value_size_in_u32 != RECORD_SIZE_IN_U32 ||
            index >= array->values_count ||
            index > GPRX::max_mpsize_t / RECORD_SIZE_IN_U32)
            return false;

        const mpsize_t i = index * RECORD_SIZE_IN_U32;
        if (i > array->entries.size() ||
            array->entries.size() - i < RECORD_SIZE_IN_U32)
            return false;

        const auto& entries = array->entries;
        result.startBlock.id      = entries[i + 0];
        result.startBlock.number  = entries[i + 1];
        result.finishBlock.id     = entries[i + 2];
        result.finishBlock.number = entries[i + 3];
        return true;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
void MpSession::ClearObjectCache(MpSession::CacheBucket bucket)
{
    if (!objectCache)
        return;
    
    auto clearBucket = [&](mpsize_t b)
    {
        for (mpsize_t i = 0; i < CACHABLE_OBJECT_TYPES_COUNT; ++i)
        {
            (*objectCache)[b * CACHABLE_OBJECT_TYPES_COUNT + i].clear();
        }
        objectCacheBucketBlockIds[b].Clear();
    };

    if (bucket != MpSession::CacheBucket::Count)
    {
        clearBucket(bucket);
        return;
    }

    for (mpsize_t b = 0; b < MpSession::CacheBucket::Count; ++b)
    {
        clearBucket(b);
    }
}

void MpSession::ClearArrayCache(MpSession::CacheBucket bucket)
{
    if (!arrayCache)
        return;
    
    auto clearBucket = [&](mpsize_t b)
    {
        for (mpsize_t i = 0; i < CACHABLE_ARRAY_TYPES_COUNT; ++i)
        {
            auto& map = (*arrayCache)[b * CACHABLE_ARRAY_TYPES_COUNT + i];
            for (auto& pair : map)
            {
                auto& opt = pair.second;
                if (opt.has_value() && opt.value())
                    MpReleaseArray(opt.value());
            }
            map.clear();
        }
    };

    if (bucket != MpSession::CacheBucket::Count)
    {
        clearBucket(bucket);
        return;
    }

    for (mpsize_t b = 0; b < MpSession::CacheBucket::Count; ++b)
    {
        clearBucket(b);
    }
}

void MpSession::ClearScalarCache()
{
    if (!scalarCache)
        return;

    for (mpsize_t i = 0; i < CACHABLE_SCALAR_TYPES_COUNT; ++i)
    {
        (*scalarCache)[i].reset();
    }
}

void MpSession::ClearTrackSegments()
{
    MpArray::ReleaseAndNull(tracks);
    for (mpsize_t i = 0; i < track_segments.size(); ++i)
    {
        track_segments[i]->Release();
    }
    track_segments.clear();
    trackSegmentCounts.clear();
    selected_track = 0;
    selected_segment = 0;
}

void MpSession::NotifySession(uint32_t notificationFlags)
{
    if (notificationFlags == 0)
        return;

    if (sessionNotificationCbFn)
        sessionNotificationCbFn(sessionNotificationCbHandle, notificationFlags);
}

MpSession::~MpSession()
{
    // objectCache's and scalarCache's contents are cleared automatically,
    // while reference-counted arrays must be managed manually
    ClearArrayCache();
    labelCache.clear();
    labelRegions.clear();
    ClearTrackSegments();
}

static void MpClearCachesImpl(MpSession* session, bool descrCacheClear = true, bool arrayCacheClear = true, bool scalarCacheClear = true)
{
    if (session->bObjectCacheEnabled && descrCacheClear)
    {
        session->dumpContainer->UnloadForceLoadedBlockData();
        session->ClearObjectCache();
    }

    if (session->bArrayCacheEnabled && arrayCacheClear)
    {
        session->ClearArrayCache();
    }

    if (session->bScalarCacheEnabled && scalarCacheClear)
    {
        session->ClearScalarCache();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
static bool MpCheckBasicMeta(MpSession* session)
{
    constexpr mpsize_t basicMetaBlockNum = 1;
    constexpr uint32_t frameSkipLimit = GPRX::Block::BLOCK_ID_DIFF_OPTIMIZED_MAX / 2;

    bool frameSkipLimitExceeded = false;

    const auto& blocks = session->dumpContainer->GetBlocks();
    if (blocks.size() > basicMetaBlockNum)
    {
        auto& b = blocks[basicMetaBlockNum];
        if (b.headerPtr->type_id == ToBlockType(BlockTypeEx::BasicMeta))
        {
            uint32_t frameIdMax = BlockBasicMeta::GetBasicMetaBlockFrameIdMaxFromMeta(b.headerPtr);
            frameSkipLimitExceeded = (frameIdMax > session->basicMetaFrameIdMax && frameIdMax - session->basicMetaFrameIdMax > frameSkipLimit);
            session->basicMetaFrameIdMax = frameIdMax;
        }
    }
    
    return frameSkipLimitExceeded;
}

static bool MpScanTracks(MpSession* session)
{
    const mpsize_t previouslySelectedTrack = session->selected_track;
    const mpsize_t previouslySelectedSegment = session->selected_segment;
    session->ClearTrackSegments();
    const auto& blocks = session->dumpContainer->GetBlocks();
    if (blocks.size() > GPRX::max_mpsize_t)
    {
        XASSERT_DIAG(false, "MpScanTracks - block count exceeds mpsize_t");
        return false;
    }

    auto& trackSegmentCounts = session->trackSegmentCounts;
    
    // 1. Scan the number of tracks and the number of segments in each track
    for (const auto& block : blocks)
    {
        auto type = block.headerPtr->type_id;
        if (type == ToBlockType(BlockTypeEx::TrackStart))
        {
            trackSegmentCounts.push_back(0);
        } 
        else if (type == ToBlockType(BlockTypeEx::SegmentStart))
        {
            if (trackSegmentCounts.empty())
                trackSegmentCounts.push_back(0); // Implicit Track 0
            trackSegmentCounts.back()++;
        }
    }

    mpsize_t tracksCount = GPRX::to_mpsize(trackSegmentCounts.size());
    if (tracksCount == 0)
        return false;

    // 2. Allocate arrays for tracks and segments
    const mpsize_t recordSize = TrackSegment::RECORD_SIZE_IN_U32;
    if (!session->IsInputTrusted())
    {
        if (tracksCount > GPRX::max_mpsize_t / recordSize)
        {
            XASSERT_DIAG(false, "MpScanTracks - track record storage exceeds mpsize_t");
            session->ClearTrackSegments();
            return false;
        }
        for (mpsize_t count : trackSegmentCounts)
        {
            if (count > GPRX::max_mpsize_t / recordSize)
            {
                XASSERT_DIAG(false,
                    "MpScanTracks - segment record storage exceeds mpsize_t");
                session->ClearTrackSegments();
                return false;
            }
        }
    }

    session->tracks = MpCreateArray();
    session->tracks->values_count = tracksCount;
    session->tracks->value_size_in_u32 = recordSize;
    session->tracks->is_valid = true;
    session->tracks->entries.reserve(tracksCount * recordSize);
    session->track_segments.reserve(tracksCount);
    
    for (mpsize_t count : trackSegmentCounts)
    {
        MpArray* segments = MpCreateArray();
        segments->values_count = count;
        segments->value_size_in_u32 = recordSize;
        segments->is_valid = true;
        segments->entries.reserve(count * recordSize);
        session->track_segments.push_back(segments);
    }

    auto makeUnfinalized = [](uint32_t id, mpsize_t num) -> TrackSegment
    {
        return { {id, num}, {id, num} };
    };

    // 3. Fill allocated arrays
    mpsize_t blockNumber = 0;
    mpsize_t currentTrack = 0;

    for (const auto& block : blocks)
    {
        const auto* header = block.headerPtr;
        uint32_t blockId = header->block_id;
        auto type = header->type_id;
        auto& trackEntries = session->tracks->entries;

        if (type == ToBlockType(BlockTypeEx::TrackStart))
        {
            if (!trackEntries.empty())
                currentTrack++;
            TrackSegment::PutToArray(trackEntries, makeUnfinalized(blockId, blockNumber));
        }
        else if (type == ToBlockType(BlockTypeEx::SegmentStart))
        {
            if (trackEntries.empty())
                TrackSegment::PutToArray(trackEntries, makeUnfinalized(0, 0)); // Implicit Track 0
            auto& segmentEntries = session->track_segments[currentTrack]->entries;
            TrackSegment::PutToArray(segmentEntries, makeUnfinalized(blockId, blockNumber));
        }
        else if (type == ToBlockType(BlockTypeEx::TrackFinish) && !trackEntries.empty())
        {
            TrackSegment::ModifyLastFinish(trackEntries, {blockId, blockNumber});
        }
        else if (type == ToBlockType(BlockTypeEx::SegmentFinish) && !trackEntries.empty())
        {
            auto& segmentEntries = session->track_segments[currentTrack]->entries;
            if (!segmentEntries.empty())
                TrackSegment::ModifyLastFinish(segmentEntries, {blockId, blockNumber});
        }
        blockNumber++;
    }

    session->selected_track = ntd::min(previouslySelectedTrack, tracksCount - 1);
    const mpsize_t selectedSegmentCount =
        session->track_segments[session->selected_track]->values_count;
    session->selected_segment = selectedSegmentCount > 0
        ? ntd::min(previouslySelectedSegment, selectedSegmentCount - 1)
        : 0;
    
    return true;
}

static bool MpGetActiveSegmentBounds(MpSession* session, mpsize_t* outStartBlockNum, mpsize_t* outFinishBlockNum)
{
    if (!outStartBlockNum || !outFinishBlockNum)
        return false;

    *outStartBlockNum = 0;
    *outFinishBlockNum = 0;

    const auto& blocks = session->dumpContainer->GetBlocks();
    if (blocks.size() > GPRX::max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "MpGetActiveSegmentBounds - block count exceeds mpsize_t");
        return false;
    }
    if (blocks.empty())
        return false;

    // 1. If no tracks were scanned/found, the entire dump is essentially one segment
    if (!session->tracks || session->track_segments.empty())
    {
        *outFinishBlockNum = GPRX::to_mpsize(blocks.size() - 1);
        return true;
    }

    // 2. Safely resolve the Track index
    mpsize_t trackIdx = session->selected_track;
    mpsize_t maxTracks = GPRX::to_mpsize(session->track_segments.size());
    if (trackIdx >= maxTracks)
    {
        trackIdx = maxTracks > 0 ? maxTracks - 1 : 0; // Clamp track
    }

    TrackSegment foundRange;
    MpArray* segmentsArray = session->track_segments[trackIdx];
    mpsize_t segmentsCount = segmentsArray->values_count;

    // 3. Safely resolve the Segment index
    if (segmentsCount > 0)
    {
        mpsize_t segmentIdx = session->selected_segment;
        if (segmentIdx >= segmentsCount)
        {
            segmentIdx = segmentsCount - 1;
        }
        if (!TrackSegment::GetFromArray(segmentsArray, segmentIdx, foundRange))
            return false;
    }
    else
    {
        // Fallback: If the track exists but has 0 segments, return the bounds of the track itself
        if (!TrackSegment::GetFromArray(session->tracks, trackIdx, foundRange))
            return false;
    }

    *outStartBlockNum = foundRange.startBlock.number;
    *outFinishBlockNum = foundRange.finishBlock.number;
    return true;
}

static bool MpFindKVStore(MpSession* session, bool forceClearCaches = false)
{
    const auto& blocks = session->dumpContainer->GetBlocks();
    if (blocks.size() > GPRX::max_mpsize_t)
    {
        XASSERT_DIAG(false, "MpFindKVStore - block count exceeds mpsize_t");
        return false;
    }
    if (blocks.empty())
        return false;

    // Prepare array cache buckets
    auto& bucketBlockIdsNew = session->arrayCacheBucketBlockIdsNew;
    auto& bucketShortTerm = bucketBlockIdsNew[MpSession::CacheBucket::ShortTerm];
    auto& bucketLongTerm = bucketBlockIdsNew[MpSession::CacheBucket::LongTerm];
    bucketShortTerm.clear();
    bucketLongTerm.clear();

    auto addBlockToCacheBucket = [&](uint32_t blockId, uint8_t subType)
    {
        auto& bucket = BlockEx::IsKvStoreCacheLongTerm(subType) ? bucketLongTerm : bucketShortTerm;
        bucket.insert(blockId);
    };

    auto createAddKVStore = [&](const ntd::unique_ptr<GPRX::KVStoreReaderProxy>& proxy, BlockContext* block)
    {
        KVStore* store = ntd::New<KVStore>(*session->dumpContainer, block);
        if (proxy->AddSource(store))
            return true;
        ntd::Delete(store);
        return false;
    };

    // 1. Group active KVStore blocks by subtype using the reused buffer
    auto& blocksBySubType = session->blocksBySubType;
    for (auto& vec : blocksBySubType)
    {
        vec.clear();
    }

    const uint8_t genericIdx = 0;
    
    {
        mpsize_t startBlockNum = 0;
        mpsize_t finishBlockNum = 0;
        MpGetActiveSegmentBounds(session, &startBlockNum, &finishBlockNum);

        using BlockDifference = ntd::deque<BlockContext>::difference_type;
        auto it = blocks.begin() + static_cast<BlockDifference>(startBlockNum);
        mpsize_t blockNum = startBlockNum;
        for (; blockNum <= finishBlockNum; ++it, ++blockNum)
        {
            const auto& block = *it;
            if (block.headerPtr->type_id != ToBlockType(BlockTypeEx::KVStore))
                continue;
            
            uint8_t subType = block.headerPtr->type_meta;
            const bool subTypeOverflow =
                subType >= static_cast<uint8_t>(BlockTypeMeta::Kv_SubTypes_Count);
            if (subTypeOverflow)
            {
                XASSERT_DIAG(false,
                    "MpFindKVStore - KVStore block subtype must be within the "
                    "BlockTypeMeta::Kv_SubTypes_Count range");
                // File-provided subtypes outside the local lookup table use the generic
                // store, which keeps lookup bounded and preserves generic-key fallback.
                subType = static_cast<uint8_t>(BlockTypeMeta::Generic);
            }
            
            blocksBySubType[subType].push_back(const_cast<GPRX::BlockContext*>(&block));
            addBlockToCacheBucket(block.headerPtr->block_id, subType);
        }
    }

    // 2. Setup the Generic proxy
    auto& selectedKVStores = session->selectedKVStores;
    auto& genericProxy = session->genericKVStoreProxy;
    bool bGenericUsedAsLongTerm = false;
    
    if (!genericProxy)
    {
        genericProxy = ntd::make_unique<KVStoreReaderProxy>();
        genericProxy->SetOwnsSources(true);
    }
    
    auto& genericBlocks = blocksBySubType[genericIdx];
    for (mpsize_t i = 0; i < genericBlocks.size(); ++i)
    {
        auto* block = genericBlocks[i];
        if (i < genericProxy->GetSourcesNum())
        {
            genericProxy->SetBlock(block, i);
        }
        else
        {
            if (!createAddKVStore(genericProxy, block))
                return false;
        }
    }
    genericProxy->SetActiveSourcesNum(GPRX::to_mpsize(genericBlocks.size()));

    // We intentionally leave selectedKVStores[genericIdx] empty.
    // Queries targeting Generic will see activeCount == 0 and fallback to genericProxy.
    selectedKVStores[genericIdx].activeCount = 0;

    // 3. Process remaining subtypes
    for (uint8_t subType = genericIdx + 1; subType < static_cast<uint8_t>(BlockTypeMeta::Kv_SubTypes_Count); ++subType)
    {
        auto& subTypeBlocks = blocksBySubType[subType];
        auto& proxyGroup = selectedKVStores[subType];
        auto& proxyList = proxyGroup.list;

        // If subtype group is empty, rely entirely on MpGetActiveProxy's fallback
        if (subTypeBlocks.empty())
        {
            proxyGroup.activeCount = 0;
            // Deactivate remaining placeholders just to be clean
            for (auto& desc : proxyList)
            {
                desc.proxy->SetActiveSourcesNum(0);
            }

            // If this empty subtype is LongTerm, it will fall back to the Generic proxy.
            // Therefore, Generic blocks must trigger LongTerm cache invalidation.
            if (BlockEx::IsKvStoreCacheLongTerm(subType) && !bGenericUsedAsLongTerm)
            {
                bGenericUsedAsLongTerm = true;
                for (auto* block : genericBlocks)
                {
                    bucketLongTerm.insert(block->headerPtr->block_id);
                }
            }
            continue;
        }

        if (BlockEx::IsKvStoreFrameDependent(subType))
        {
            // 1 proxy per block, allowing multiple proxies
            mpsize_t bIdx = 0;
            for (auto* block : subTypeBlocks)
            {
                uint32_t frameIdMax = KVStoreEx::GetKvStoreFrameIdFromMeta(block->headerPtr);
                if (bIdx < proxyList.size())
                {
                    // Reuse existing proxy
                    auto& proxyDesc = proxyList[bIdx];
                    if (proxyDesc.proxy->GetSourcesNum() > 0)
                    {
                        proxyDesc.proxy->SetBlock(block, 0);
                    }
                    else
                    {
                        if (!createAddKVStore(proxyDesc.proxy, block))
                            return false;
                    }
                    proxyDesc.proxy->SetActiveSourcesNum(1);
                    proxyDesc.frameIdMax = frameIdMax;
                }
                else
                {
                    // Create new proxy
                    auto newProxy = ntd::make_unique<KVStoreReaderProxy>();
                    newProxy->SetOwnsSources(true);
                    if (!createAddKVStore(newProxy, block))
                        return false;
                    proxyList.push_back({ntd::move(newProxy), frameIdMax});
                }
                bIdx++;
            }
            
            proxyGroup.activeCount = bIdx;

            // Deactivate leftover proxies
            for (; bIdx < proxyList.size(); ++bIdx)
            {
                proxyList[bIdx].proxy->SetActiveSourcesNum(0);
                proxyList[bIdx].frameIdMax = 0;
            }
        }
        else
        {
            // Other subtypes: 1 proxy containing multiple block sources
            if (proxyList.empty())
            {
                auto newProxy = ntd::make_unique<KVStoreReaderProxy>();
                newProxy->SetOwnsSources(true);
                proxyList.push_back({ntd::move(newProxy), max_mpsize_t});
            }
            
            auto& proxyDesc = proxyList[0];
            proxyDesc.frameIdMax = max_mpsize_t;
            proxyGroup.activeCount = 1;

            for (mpsize_t i = 0; i < subTypeBlocks.size(); ++i)
            {
                auto* block = subTypeBlocks[i];
                if (i < proxyDesc.proxy->GetSourcesNum())
                {
                    proxyDesc.proxy->SetBlock(block, i);
                }
                else
                {
                    if (!createAddKVStore(proxyDesc.proxy, block))
                        return false;
                }
            }
            proxyDesc.proxy->SetActiveSourcesNum(GPRX::to_mpsize(subTypeBlocks.size()));
            XASSERT_DIAG(proxyList.size() == 1, "MpFindKVStore - Non-frame-dependent KVStore proxy list should only ever have 1 proxy");
        }
    }

    // 4. Clear KV store caches if requested
    if (forceClearCaches)
    {
        if (session->genericKVStoreProxy)
        {
            session->genericKVStoreProxy->ClearCaches();
        }

        for (auto& proxyGroup : session->selectedKVStores)
        {
            for (auto& proxyDesc : proxyGroup.list)
            {
                if (proxyDesc.proxy)
                {
                    proxyDesc.proxy->ClearCaches();
                }
            }
        }
    }

    // 5. Clear array cache buckets that depend on KV store block IDs that differ from the new set
    auto checkAndSwapBuckets = [&](
        MpSession::ArrayCacheBucketBlockIds& a1,
        MpSession::ArrayCacheBucketBlockIds& a2)
    {
        if (session->bArrayCacheEnabled)
        {
            for (mpsize_t i = 0; i < a1.size(); ++i)
            {
                bool notEqual = forceClearCaches;

                if (!notEqual)
                {
                    const auto& s1 = a1[i];
                    const auto& s2 = a2[i];
                    if (s1.size() != s2.size())
                    {
                        notEqual = true;
                    }
                    else
                    {
                        for (const auto& elem : s1)
                        {
                            if (!s2.count(elem))
                            {
                                notEqual = true;
                                break;
                            }
                        }
                    }
                }

                if (notEqual)
                {
                    session->ClearArrayCache(static_cast<MpSession::CacheBucket>(i));
                }
            }
        }

        ntd::swap(a1, a2);
    };

    checkAndSwapBuckets(session->arrayCacheBucketBlockIds, bucketBlockIdsNew);
    return true;
}

static const GPRX::BlockHeader* MpGetTrackSegmentHeader(
    MpSession* session,
    const TrackSegment::BlockIdNumber& location,
    GPRX::BlockType expectedType)
{
    if (!session || !session->dumpContainer || location.id == 0)
        return nullptr;

    const auto& blocks = session->dumpContainer->GetBlocks();
    if (location.number >= blocks.size())
        return nullptr;

    const GPRX::BlockHeader* header = blocks[location.number].headerPtr;
    if (!header || header->block_id != location.id || header->type_id != expectedType)
        return nullptr;
    return header;
}

static bool MpApplyTrackSegmentSelection(
    MpSession* session, mpsize_t trackIndex, mpsize_t segmentIndex)
{
    if (session->selected_track == trackIndex &&
        session->selected_segment == segmentIndex)
        return true;

    const mpsize_t previousTrack = session->selected_track;
    const mpsize_t previousSegment = session->selected_segment;
    session->selected_track = trackIndex;
    session->selected_segment = segmentIndex;

    MpClearCachesImpl(session);
    bool success = MpFindKVStore(session, true);
    if (!success)
    {
        session->selected_track = previousTrack;
        session->selected_segment = previousSegment;
        MpFindKVStore(session, true);
    }

    if (success)
    {
        session->InvalidateLabelLookup(MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED);
    }

    if (session->clearObjCacheCbFn)
    {
        const uint32_t clearedBuckets[] = {
            MpSession::CacheBucket::ShortTerm,
            MpSession::CacheBucket::LongTerm,
        };
        session->clearObjCacheCbFn(
            session->clearObjCacheCbHandle,
            clearedBuckets,
            GPRX::to_mpsize(ntd::size(clearedBuckets)));
    }
    return success;
}

static void MpObjectCacheSetStartBlockId(MpSession* session)
{
    uint32_t blockIdMin = session->dumpContainer->GetBlockIdMin();
    for (mpsize_t i = 0; i < session->objectCacheBucketBlockIds.size(); ++i)
    {
        auto& lookup = session->objectCacheBucketBlockIds[i];
        lookup.SetStartId(blockIdMin);
    }
}

static MpSession* MpOpenFromDataContextImpl(
    ntd::unique_ptr<IDataContext> dataSource,
    mpsize_t cachingProxySize,
    GPRX::InputTrust inputTrust)
{
    if (!dataSource)
        return nullptr;
    constexpr mpsize_t CACHE_PAGE_SIZE = 16 * 1024;
    
    MpSession* res = ntd::New<MpSession>();

    if (cachingProxySize)
    {
        mpsize_t cachePageCount = div_roundup(cachingProxySize, CACHE_PAGE_SIZE);
        auto proxy = ntd::make_unique<DataContextCachingProxy>(dataSource.release(), CACHE_PAGE_SIZE, cachePageCount);
        proxy->SetOwnsWrappedContext(true);
        res->dataSource = ntd::move(proxy);
    }
    else
    {
        res->dataSource = ntd::move(dataSource);
    }

    res->dumpContainer = ntd::make_unique<ContainerContext>(
        *res->dataSource, inputTrust);
    uint8_t* handle = reinterpret_cast<uint8_t*>(res);
    MpSetClearObjCacheCallback(res, LibMpExtClearObjCacheCallback, handle);
    MpSetSessionNotificationCallback(
        res, LibMpExtSessionNotificationCallback, handle);

    MpCheckBasicMeta(res);
    MpObjectCacheSetStartBlockId(res);
    MpScanTracks(res);
    MpFindKVStore(res);
    return res;
}

static MpSession* MpOpenFromExternalProviderImpl(
    const ExternalIO* ioInterface,
    uint8_t* handle,
    mpsize_t cachingProxySize,
    GPRX::InputTrust inputTrust)
{
    auto dataSource = ntd::make_unique<DataContextExternalIO>();
    if (ioInterface)
        dataSource->AssignIO(*ioInterface);
    if (!dataSource->Open(handle))
        return nullptr;
    return MpOpenFromDataContextImpl(ntd::move(dataSource), cachingProxySize, inputTrust);
}

static MpSession* MpOpenFromMemImpl(
    const uint8_t* mem, mpsize_t size, GPRX::InputTrust inputTrust)
{
    if (!mem && size > 0)
        return nullptr;
    auto dataSource = ntd::make_unique<DataContextSingleBuffer>();
    dataSource->AssignBuffer(mem, size);
    return MpOpenFromDataContextImpl(ntd::move(dataSource), 0, inputTrust);
}

static MpSession* MpOpenFromFileImpl(
    const char* file_name, mpsize_t cachingProxySize, GPRX::InputTrust inputTrust)
{
#ifndef EXCLUDE_FILE_DATA_CONTEXT
    if (!file_name)
        return nullptr;
    auto dataSource = ntd::make_unique<DataContextFile>();
    if (!dataSource->Open(file_name))
        return nullptr;
    return MpOpenFromDataContextImpl(ntd::move(dataSource), cachingProxySize, inputTrust);
#else
    (void)file_name;
    (void)cachingProxySize;
    (void)inputTrust;
    return nullptr;
#endif
}

static MpSession* MpOpenFromExternalDataImpl(
    uint8_t* handle,
    uint32_t use_block_fetch,
    mpsize_t cachingProxySize,
    GPRX::InputTrust inputTrust)
{
    if (!MpIsBackendReady())
        return nullptr;
    ExternalIO io = DataContextExternalIO::GetDefaultIO();
    io.isBlockFetchSupported = !!use_block_fetch;
    return MpOpenFromExternalProviderImpl(&io, handle, cachingProxySize, inputTrust);
}

#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-session-api !LIBMP_TRUSTED_INPUTS_ONLY
MpSession* MpOpenFromDataContext(
    ntd::unique_ptr<IDataContext> dataSource, mpsize_t cachingProxySize)
{
    return MpOpenFromDataContextImpl(
        ntd::move(dataSource), cachingProxySize, GPRX::InputTrust::Untrusted);
}

MpSession* MpOpenFromExternalProvider(
    const ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize)
{
    return MpOpenFromExternalProviderImpl(
        ioInterface, handle, cachingProxySize, GPRX::InputTrust::Untrusted);
}

MpSession* MpOpenFromMem(const uint8_t* mem, mpsize_t size)
{
    return MpOpenFromMemImpl(mem, size, GPRX::InputTrust::Untrusted);
}

MpSession* MpOpenFromFile(const char* file_name, mpsize_t cachingProxySize)
{
    return MpOpenFromFileImpl(file_name, cachingProxySize, GPRX::InputTrust::Untrusted);
}

MpSession* MpOpenFromExternalData(
    uint8_t* handle, uint32_t use_block_fetch, mpsize_t cachingProxySize)
{
    return MpOpenFromExternalDataImpl(
        handle, use_block_fetch, cachingProxySize, GPRX::InputTrust::Untrusted);
}
// @amalgamate: config-end untrusted-session-api
#endif

MpSession* MpOpenFromDataContextTrusted(
    ntd::unique_ptr<IDataContext> dataSource, mpsize_t cachingProxySize)
{
    return MpOpenFromDataContextImpl(
        ntd::move(dataSource), cachingProxySize, GPRX::InputTrust::Trusted);
}

MpSession* MpOpenFromExternalProviderTrusted(
    const ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize)
{
    return MpOpenFromExternalProviderImpl(
        ioInterface, handle, cachingProxySize, GPRX::InputTrust::Trusted);
}

MpSession* MpOpenFromMemTrusted(const uint8_t* mem, mpsize_t size)
{
    return MpOpenFromMemImpl(mem, size, GPRX::InputTrust::Trusted);
}

MpSession* MpOpenFromFileTrusted(const char* file_name, mpsize_t cachingProxySize)
{
    return MpOpenFromFileImpl(file_name, cachingProxySize, GPRX::InputTrust::Trusted);
}

MpSession* MpOpenFromExternalDataTrusted(
    uint8_t* handle, uint32_t use_block_fetch, mpsize_t cachingProxySize)
{
    return MpOpenFromExternalDataImpl(
        handle, use_block_fetch, cachingProxySize, GPRX::InputTrust::Trusted);
}

bool MpCloseSession(MpSession* session)
{
    if (!session)
        return false;
    MpDetachIterators(session);
    ntd::Delete(session);
    return true;
}

bool MpSyncWithDataSource(MpSession* session)
{
    if (!session || !session->dumpContainer)
        return false;
    const bool changeWasAlreadyNotified = session->bSyncRequired;
    auto& clearedBuckets = session->syncClearedObjectCacheBuckets;
    clearedBuckets.clear();

    uint32_t* removedBlockIds = nullptr;
    mpsize_t removedBlockCount = 0;
    bool forceClearCaches = false;
    bool success = session->dumpContainer->SyncWithDataSource(&removedBlockIds, &removedBlockCount, &forceClearCaches);

    bool frameSkipLimitExceeded = MpCheckBasicMeta(session);
    if (!forceClearCaches && (frameSkipLimitExceeded || session->bClearCacheOnSyncRequired))
    {
        forceClearCaches = true;
        session->dumpContainer->ClearCaches();
    }

    if (session->bScalarCacheEnabled)
        session->ClearScalarCache();

    // Clear object cache buckets that depend on removed data block IDs
    if (session->bObjectCacheEnabled)
    {
        for (mpsize_t i = 0; i < session->objectCacheBucketBlockIds.size(); ++i)
        {
            bool needToClear = forceClearCaches;

            if (!needToClear)
            {
                auto& lookup = session->objectCacheBucketBlockIds[i];
                for (mpsize_t j = 0; j < removedBlockCount; ++j)
                {
                    if (lookup.Contains(removedBlockIds[j]))
                    {
                        needToClear = true;
                        break;
                    }
                }
            }

            if (needToClear)
            {
                session->ClearObjectCache(MpSession::CacheBucket{i});
                clearedBuckets.push_back(i);
            }
        }
    }

    MpObjectCacheSetStartBlockId(session);
    MpScanTracks(session);
    MpFindKVStore(session, forceClearCaches);
    uint32_t labelNotificationFlags = changeWasAlreadyNotified
        ? 0u
        : static_cast<uint32_t>(MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED);
    if (forceClearCaches)
    {
        MpClearLabelCache(session);
        labelNotificationFlags |= MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED |
            MP_SESSION_NOTIFICATION_LABEL_CACHE_RESET;
    }
    session->InvalidateLabelLookup(labelNotificationFlags);

    if (session->clearObjCacheCbFn)
        session->clearObjCacheCbFn(
            session->clearObjCacheCbHandle,
            clearedBuckets.data(),
            GPRX::to_mpsize(clearedBuckets.size()));

    session->bSyncRequired = false;
    session->bClearCacheOnSyncRequired = false;
    return success;
}

bool MpSyncIfRequired(MpSession* session)
{
    return CheckSync(session);
}

void MpSetSyncRequired(MpSession* session)
{
    if (!session || session->bSyncRequired)
        return;
    session->bSyncRequired = true;
    session->InvalidateLabelLookup(MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED);
}

void MpSetClearCacheOnSyncRequired(MpSession* session)
{
    if (session)
        session->bClearCacheOnSyncRequired = true;
}

uint32_t MpIsSessionValid(MpSession* session)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return 0;
    return GPRX::bool_to_int(session->dumpContainer->GetValidationStatus().is_valid);
}

uint32_t MpGetSessionDataFormatVersion(MpSession* session)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return 0;
    const auto& validation = session->dumpContainer->GetValidationStatus();
    return LibMP::Version::BuildFrom(validation.dataformat_major_version, validation.dataformat_minor_version);
}

uint32_t MpGetDataFormatVersion()
{
    return LibMP::LocalVersion::DATA_FORMAT;
}

uint32_t MpGetLibraryVersion()
{
    return LibMP::LocalVersion::LIBRARY;
}

mpsize_t MpGetTrackCount(MpSession* session)
{
    CheckSync(session);
    return session && session->tracks ? session->tracks->values_count : 0;
}

MpArray* MpGetTracks(MpSession* session)
{
    CheckSync(session);
    if (!session || !session->tracks)
        return nullptr;
    session->tracks->Acquire();
    return session->tracks;
}

const uint8_t* MpGetTrackDescFromArray(
    MpSession* session, MpArray* array, mpsize_t index)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return nullptr;
    session->global_obj_size = 0;

    TrackSegment track;
    if (array != session->tracks ||
        !TrackSegment::GetFromArray(array, index, track) ||
        index >= session->track_segments.size())
        return nullptr;

    auto appType = LibMP::TypesNtd::AppType_Unknown;
    if (!track.IsImplicit())
    {
        const GPRX::BlockHeader* startHeader = MpGetTrackSegmentHeader(
            session, track.startBlock, ToBlockType(BlockTypeEx::TrackStart));
        if (!startHeader)
            return nullptr;
        const uint8_t rawAppType = static_cast<uint8_t>(startHeader->metadata0);
        if (rawAppType <= LibMP::TypesNtd::AppType_MAX)
            appType = static_cast<LibMP::TypesNtd::AppType>(rawAppType);
    }

    const GPRX::BlockHeader* firstSegmentStart = nullptr;
    const GPRX::BlockHeader* lastSegmentFinish = nullptr;
    MpArray* segments = session->track_segments[index];
    if (segments && segments->values_count > 0)
    {
        TrackSegment firstSegment;
        TrackSegment lastSegment;
        if (!TrackSegment::GetFromArray(segments, 0, firstSegment) ||
            !TrackSegment::GetFromArray(
                segments, segments->values_count - 1, lastSegment))
            return nullptr;
        firstSegmentStart = MpGetTrackSegmentHeader(
            session, firstSegment.startBlock, ToBlockType(BlockTypeEx::SegmentStart));
        if (!firstSegmentStart)
            return nullptr;
        if (lastSegment.IsFinalized())
            lastSegmentFinish = MpGetTrackSegmentHeader(
                session, lastSegment.finishBlock,
                ToBlockType(BlockTypeEx::SegmentFinish));
        if (lastSegment.IsFinalized() && !lastSegmentFinish)
            return nullptr;
    }

    session->global_track_desc = LibMP::TypesNtd::TrackDesc(
        firstSegmentStart ? firstSegmentStart->metadata1 : 0,
        lastSegmentFinish ? lastSegmentFinish->metadata1 : 0,
        appType);
    session->global_obj_size = sizeof(LibMP::TypesNtd::TrackDesc);
    return reinterpret_cast<const uint8_t*>(&session->global_track_desc);
}

mpsize_t MpGetSegmentCount(MpSession* session, mpsize_t trackIndex)
{
    CheckSync(session);
    if (!session || trackIndex >= session->track_segments.size())
        return 0;
    return session->track_segments[trackIndex]->values_count;
}

MpArray* MpGetSegments(MpSession* session, mpsize_t trackIndex)
{
    CheckSync(session);
    if (!session || trackIndex >= session->track_segments.size())
        return nullptr;
    MpArray* segments = session->track_segments[trackIndex];
    segments->Acquire();
    return segments;
}

const uint8_t* MpGetSegmentDescFromArray(
    MpSession* session, MpArray* array, mpsize_t index)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return nullptr;
    session->global_obj_size = 0;

    TrackSegment segment;
    if (!TrackSegment::GetFromArray(array, index, segment))
        return nullptr;

    const GPRX::BlockHeader* startHeader = MpGetTrackSegmentHeader(
        session, segment.startBlock, ToBlockType(BlockTypeEx::SegmentStart));
    if (!startHeader)
        return nullptr;

    uint64_t timestampStop = 0;
    uint32_t frameIdEnd = 0;
    uint32_t markerMask = 0;
    if (segment.IsFinalized())
    {
        const GPRX::BlockHeader* finishHeader = MpGetTrackSegmentHeader(
            session, segment.finishBlock, ToBlockType(BlockTypeEx::SegmentFinish));
        if (!finishHeader)
            return nullptr;
        const GPRX::U64Split* finishMeta = GPRX::Block::GetMeta0View(finishHeader);
        timestampStop = finishHeader->metadata1;
        frameIdEnd = finishMeta->parts.low;
        markerMask = finishMeta->parts.high;
    }

    const GPRX::U64Split* startMeta = GPRX::Block::GetMeta0View(startHeader);
    session->global_segment_desc = LibMP::TypesNtd::SegmentDesc(
        startHeader->metadata1,
        timestampStop,
        startMeta->parts.low,
        frameIdEnd,
        startMeta->parts.high,
        markerMask);
    session->global_obj_size = sizeof(LibMP::TypesNtd::SegmentDesc);
    return reinterpret_cast<const uint8_t*>(&session->global_segment_desc);
}

bool MpSelectTrack(MpSession* session, mpsize_t trackIndex)
{
    CheckSync(session);
    if (!session || trackIndex >= session->track_segments.size())
        return false;
    return MpApplyTrackSegmentSelection(session, trackIndex, 0);
}

bool MpSelectSegment(MpSession* session, mpsize_t segmentIndex)
{
    CheckSync(session);
    if (!session || session->selected_track >= session->track_segments.size())
        return false;
    MpArray* segments = session->track_segments[session->selected_track];
    if (!segments || segmentIndex >= segments->values_count)
        return false;
    return MpApplyTrackSegmentSelection(
        session, session->selected_track, segmentIndex);
}

ContainerContext* MpGetContainerFromSession(MpSession* session)
{
    return session ? session->dumpContainer.get() : nullptr;
}

void MpSetClearObjCacheCallback(
    MpSession* session, ClearObjCacheCallback fn, uint8_t* handle)
{
    if (!session)
        return;
    session->clearObjCacheCbFn = fn;
    session->clearObjCacheCbHandle = handle;
}

void MpSetSessionNotificationCallback(
    MpSession* session, SessionNotificationCallback fn, uint8_t* handle)
{
    if (!session)
        return;
    session->sessionNotificationCbFn = fn;
    session->sessionNotificationCbHandle = handle;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool MpPushDataLock(MpSession* session)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return false;
    return session->dumpContainer->PushDataLock();
}

bool MpConsumeDataLock(MpSession* session)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return false;
    return session->dumpContainer->ConsumeDataLock();
}

bool MpPopDataLock(MpSession* session)
{
    CheckSync(session);
    if (!session || !session->dumpContainer)
        return false;
    return session->dumpContainer->PopDataLock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
inline static GPRX::IKVStoreReader* MpGetActiveProxy(MpSession* session)
{
    auto& proxyGroup = session->selectedKVStores[session->global_query_subtype];
    const mpsize_t activeCount = proxyGroup.activeCount;
    if (activeCount == 0)
        return session->genericKVStoreProxy.get();

    uint32_t frameId = session->global_query_frame_id;
    for (mpsize_t i = 0; i < activeCount - 1; ++i)
    {
        if (frameId <= proxyGroup.list[i].frameIdMax)
        {
            return proxyGroup.list[i].proxy.get();
        }
    }

    return proxyGroup.list[activeCount - 1].proxy.get();
}

template <typename... Args>
inline static void MpSetQuery(MpSession* session, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx operation_id, bool isCompactFormatAllowed = false, Args... args)
{
    session->global_uid.Set(ToQuery(operation_id), isCompactFormatAllowed, ntd::forward<Args>(args)...);
    session->global_query_subtype = static_cast<uint8_t>(kv_subtype);
    session->global_query_frame_id = kv_frame_id;
}

inline static const uint8_t* MpGetObjPtrGlobal(MpSession* session, mpsize_t size = 0)
{
    session->global_obj_size = size;
    return const_cast<const uint8_t*>(KVHelper::GetObj(
        session->global_uid, &session->global_obj_size,
        *session->dumpContainer, MpGetActiveProxy(session),
        &session->global_obj_locator
    ));
}

template <typename T>
inline static const uint8_t* MpVerifyTable(MpSession* session, const uint8_t* object)
{
    if (!session || !object || session->global_obj_size == 0 ||
        session->global_obj_size > max_mpsize_t - sizeof(uint32_t))
    {
        if (session)
            session->global_obj_size = 0;
        return nullptr;
    }

#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-input-verification !LIBMP_TRUSTED_INPUTS_ONLY
    if (!session->IsInputTrusted())
    {
        // GetDataByLocator guarantees that a size-prefixed object's four-byte prefix
        // remains adjacent even when a copying provider is used. Retaining that origin is
        // required for the alignment rules of tables containing 64-bit fields.
        flatbuffers::Verifier verifier(
            object - sizeof(uint32_t), session->global_obj_size + sizeof(uint32_t));
        if (!verifier.VerifySizePrefixedBuffer<T>(nullptr))
        {
            session->global_obj_size = 0;
            return nullptr;
        }
    }
// @amalgamate: config-end untrusted-input-verification
#endif
    return object;
}

inline static bool MpGetVal(MpSession* session)
{
    const auto& uid = session->global_uid;
    auto& val = session->global_val_buf;
    auto* kvstore = MpGetActiveProxy(session);
    if (!kvstore)
        return false;
    bool res = kvstore->Get(
        &uid.entries[0], uid.size, &uid.precomputed_hash,
        val.elements_ptr, &val.size_in_u32
    );
    return res;
}

static uint32_t MpGetU32(MpSession* session)
{
    bool res = MpGetVal(session);
    return res ? session->global_val_buf.ToU32() : 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
static MpArray* MpGetArray(MpSession* session)
{
    auto* kvstore = MpGetActiveProxy(session);
    if (!kvstore)
        return nullptr;
    MpArray* array = MpCreateArray();
    const auto& uid = session->global_uid;
    array->is_valid = !!kvstore->GetAllValues(
         &uid.entries[0], uid.size, &uid.precomputed_hash,
         &array->value_size_in_u32, &array->values_count
    );
    auto& origVecRef = kvstore->GetAllValuesBufRef();
    array->entries = ntd::move(origVecRef);
    origVecRef.clear();
    return array;
}

static mpsize_t MpGetArrayValuesCount(MpSession* session)
{
    auto* kvstore = MpGetActiveProxy(session);
    if (!kvstore)
        return 0;

    const auto& uid = session->global_uid;
    mpsize_t values_count = 0;
    if (!kvstore->Contains(
            &uid.entries[0], uid.size, &uid.precomputed_hash, &values_count))
        return 0;
    return values_count;
}

mpsize_t MpGetObjSize(MpSession* session)
{
    return session ? session->global_obj_size : 0;
}

// The array accessors are part of the public C ABI and are reachable with a null handle
// or an out-of-range index (including from the iterators, on a session whose container is
// invalid). Guard both rather than dereferencing/indexing blindly.
mpsize_t MpGetArraySize(MpArray* array)
{
    return array ? array->values_count : 0;
}

void MpReleaseArray(MpArray* array)
{
    if (array)
        array->Release();
}

static void MpAcquireArray(MpArray* array)
{
    if (array)
        array->Acquire();
}

uint32_t MpGetIdFromArray(MpSession* /* session */, MpArray* array, mpsize_t index)
{
    if (!array || index >= array->values_count)
        return 0;
    return array->GetValueU32(index);
}

const uint8_t* MpGetU64SampleFromArray(MpSession* session, MpArray* array, mpsize_t index)
{
    if (!session || !array || index >= array->values_count)
        return nullptr;
    ValueBufEx& value_buf = array->GetValue(index);
    session->global_u64_sample = value_buf.ToU64Sample();
    session->global_obj_size = sizeof(LibMP::TypesNtd::U64Sample);
    return reinterpret_cast<const uint8_t*>(&session->global_u64_sample);
}

const uint8_t* MpGetCounterSampleFromArray(MpSession* session, MpArray* array, mpsize_t index)
{
    if (!session || !array || index >= array->values_count)
        return nullptr;
    ValueBufEx& value_buf = array->GetValue(index);
    session->global_counter_sample = value_buf.ToCounterSample();
    session->global_obj_size = sizeof(LibMP::TypesNtd::CounterSample);
    return reinterpret_cast<const uint8_t*>(&session->global_counter_sample);
}

const uint8_t* MpGetLastCounterSampleFromArray(MpSession* session, MpArray* array)
{
    if (!session || !array || !array->values_count)
        return nullptr;
    return MpGetCounterSampleFromArray(session, array, array->values_count - 1);
}

const uint8_t* MpGetCounterSampleFromArrayByFrameId(MpSession* session, MpArray* array, mpsize_t frameId)
{
    // 1. Safety checks
    if (!session || !array || !array->is_valid || !array->values_count ||
        array->value_size_in_u32 == 0 ||
        !range_within<mpsize_t>(0, array->values_count, max_mpsize_t / array->value_size_in_u32) ||
        array->entries.size() < array->values_count * array->value_size_in_u32)
        return nullptr;

    // Prepare accessors for raw data
    const uint32_t* raw_data = array->entries.data(); 
    const mpsize_t stride = array->value_size_in_u32;
    const mpsize_t count = array->values_count;

    // Helper to get ID from raw index
    auto get_id_at = [&](mpsize_t idx) -> uint32_t
    {
        return *(raw_data + (stride * idx));
    };

    // Helper to copy found data to static struct and return pointer
    auto return_found = [&](mpsize_t idx) -> const uint8_t*
    {
        return MpGetCounterSampleFromArray(session, array, idx);
    };

    // 2. Check bounds
    uint32_t first_id = get_id_at(0);
    uint32_t last_id = get_id_at(count - 1);

    if (frameId < first_id || frameId > last_id)
        return nullptr;

    // 3. Handle small arrays
    if (count == 1)
    {
        if (first_id == frameId)
            return return_found(0);
        return nullptr;
    }

    // 4. Estimate search start position (whole range interpolation)
    uint32_t total_range = last_id - first_id;
    uint32_t offset = frameId - first_id;

    // The formula is: index = (offset / total_range) * (count - 1)
    // We have more than one element, so total_range should be non-zero.
    // Resulting start_index should be less than 'count'.
    uint64_t numerator = static_cast<uint64_t>(offset) * (count - 1);
    mpsize_t start_index = static_cast<mpsize_t>(numerator / total_range);

    // 5. Search Logic (linear scan from estimate)
    uint32_t current_id = get_id_at(start_index);

    if (current_id == frameId)
    {
        return return_found(start_index);
    } 
    else if (current_id < frameId)
    {
        // We landed BEFORE the target. Search forward (-->).
        for (mpsize_t i = start_index + 1; i < count; i++)
        {
            uint32_t val = get_id_at(i);
            if (val == frameId)
                return return_found(i);
            
            // Overshoot check: If we see an ID > target, the target is missing.
            if (val > frameId)
                break; 
        }
    } 
    else
    { 
        // We landed AFTER the target. Search backward (<--).
        // i > 0 check handles unsigned underflow safety
        for (mpsize_t i = start_index; i > 0; )
        {
            i--; // Decrement first
            uint32_t val = get_id_at(i);
            if (val == frameId)
                return return_found(i);
            
            // Overshoot check: If we see an ID < target, the target is missing.
            if (val < frameId)
                break; 
        }
    }

    // 6. Not Found
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void MpEnableObjectCache(MpSession* session, uint32_t kvL1Cache, uint32_t descrCache, uint32_t arrayCache, uint32_t scalarCache)
{
    if (!session || !session->dumpContainer)
        return;
    session->dumpContainer->SetObjectCachingAllowed(kvL1Cache);
    if (descrCache && !session->objectCache)
    {
        session->objectCache = ntd::make_unique<MpSession::ObjectCacheArray>();
        for (auto& bucket : session->objectCacheBucketBlockIds)
        {
            bucket.SetFastLookupSize(GPRX::HybridLookupSet::DIRECT_LOOKUP_THRESHOLD);
        }
    }
    if (arrayCache && !session->arrayCache)
        session->arrayCache = ntd::make_unique<MpSession::ArrayCacheArray>();
    if (scalarCache && !session->scalarCache)
        session->scalarCache = ntd::make_unique<MpSession::ScalarCacheArray>();

    MpClearCachesImpl(session, !descrCache, !arrayCache, !scalarCache); // Clear the caches that were disabled, then reset the unique_ptr

    if (!descrCache)
    {
        session->objectCache.reset();
        for (auto& bucket : session->objectCacheBucketBlockIds)
        {
            bucket.SetFastLookupSize(0);
        }
    }
    if (!arrayCache)
        session->arrayCache.reset();
    if (!scalarCache)
        session->scalarCache.reset();

    session->bObjectCacheEnabled = !!descrCache;
    session->bArrayCacheEnabled = !!arrayCache;
    session->bScalarCacheEnabled = !!scalarCache;
}

void MpClearObjectCache(MpSession* session)
{
    if (session)
        MpClearCachesImpl(session);
}

template <auto GetterFunc, typename... Args>
inline static auto MpQuery(MpSession* session, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx query, Args&&... args)
{
    ScopeExit lock_cleanup([&]() { session->dumpContainer->DeactivateLock(); });
    MpSetQuery(session, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);
    return GetterFunc(session);
}

template <typename... Args>
inline static const uint8_t* MpQueryObj(MpSession* session, mpsize_t size, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx query, Args&&... args)
{
    ScopeExit lock_cleanup([&]() { session->dumpContainer->DeactivateLock(); });
    MpSetQuery(session, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);
    return MpGetObjPtrGlobal(session, size);
}

template <typename... Args>
inline static MpArray* MpQueryArray(MpSession* session, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx query, Args&&... args)
{
    return MpQuery<MpGetArray>(session, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);
}

template <typename... Args>
inline static const uint8_t* MpQueryObjCached(MpSession* session, mpsize_t size, MpSession::CacheBucket bucket, uint32_t type, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx query, Args&&... args)
{
    CheckSync(session);
    static_assert(sizeof...(args) == 0 || sizeof...(args) == 2, "MpQueryObjCached expects either 0 or 2 extra arguments");

    if (!session)
        return nullptr;

    if (!session->bObjectCacheEnabled)
        return MpQueryObj(session, size, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);

    uint32_t numericArg = 0;
    if constexpr (sizeof...(args) == 2)
        numericArg = static_cast<uint32_t>(ntd::get<1>(ntd::tuple<Args...>(args...)));

    auto& slot = (*session->objectCache)[bucket * MpSession::CACHABLE_OBJECT_TYPES_COUNT + type][numericArg];

    if (slot.has_value())
    {
        session->dumpContainer->DeactivateLock();
        session->global_obj_size = slot.value().size;
        return slot.value().data;
    }

    session->dumpContainer->ForceBlockDataPersistence(true);
    const uint8_t* obj = MpQueryObj(session, size, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);
    session->dumpContainer->ForceBlockDataPersistence(false);

    if (obj)
    {
        auto [block_id, _] = DecodeLocator(session->global_obj_locator);
        session->objectCacheBucketBlockIds[bucket].Insert(block_id);
        slot = MpSession::CachedObject{obj, session->global_obj_size};
    }

    return obj;
}

template <typename... Args>
inline static MpArray* MpQueryArrayCached(MpSession* session, MpSession::CacheBucket bucket, mpsize_t type, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx query, Args&&... args)
{
    CheckSync(session);
    static_assert(sizeof...(args) == 0 || sizeof...(args) == 2, "MpQueryArrayCached expects either 0 or 2 extra arguments");

    if (!session)
        return nullptr;

    if (!session->bArrayCacheEnabled)
        return MpQueryArray(session, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);

    uint32_t numericArg = 0;
    if constexpr (sizeof...(args) == 2)
        numericArg = static_cast<uint32_t>(ntd::get<1>(ntd::tuple<Args...>(args...)));

    auto& slot = (*session->arrayCache)[bucket * MpSession::CACHABLE_ARRAY_TYPES_COUNT + type][numericArg];

    MpArray* array = nullptr;
    if (slot.has_value())
    {
        session->dumpContainer->DeactivateLock();
        array = slot.value();
    }
    else
    {
        array = MpQueryArray(session, kv_subtype, kv_frame_id, query, ntd::forward<Args>(args)...);
        slot = array;
    }

    if (array)
        MpAcquireArray(array);
    return array;
}

inline static uint32_t MpQueryU32Cached(MpSession* session, mpsize_t type, BlockTypeMeta kv_subtype, uint32_t kv_frame_id, QueryEx query)
{
    CheckSync(session);
    if (!session)
        return 0;

    if (!session->bScalarCacheEnabled)
        return MpQuery<MpGetU32>(session, kv_subtype, kv_frame_id, query);

    auto& slot = (*session->scalarCache)[type];

    if (slot.has_value())
    {
        session->dumpContainer->DeactivateLock();
        return slot.value();
    }

    uint32_t value = MpQuery<MpGetU32>(session, kv_subtype, kv_frame_id, query);
    slot = value;

    return value;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Cachable objects

static constexpr uint32_t BASE_COUNTER_VALUE_OBJECTS = __COUNTER__;
#define CACHABLE_TYPES_COUNTER_OBJECTS (__COUNTER__ - BASE_COUNTER_VALUE_OBJECTS - 1)

const uint8_t* MpGetGlobalDesc(MpSession* session)
{
    return MpQueryObjCached(session, sizeof(LibMP::TypesNtd::GlobalDesc),
        MpSession::CacheBucket::ShortTerm, CACHABLE_TYPES_COUNTER_OBJECTS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::GlobalDesc);
}

const uint8_t* MpGetGeneralInfo(MpSession* session)
{
    const uint8_t* object = MpQueryObjCached(session, 0,
        MpSession::CacheBucket::ShortTerm, CACHABLE_TYPES_COUNTER_OBJECTS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::GeneralInfo);
    return MpVerifyTable<LibMP::TypesNtd::GeneralInfo>(session, object);
}

const uint8_t* MpGetGroupDesc(MpSession* session, uint32_t groupId)
{
    const uint8_t* object = MpQueryObjCached(session, 0,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_OBJECTS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::GroupDesc_u24GroupId, true, groupId);
    return MpVerifyTable<LibMP::TypesNtd::GroupDesc>(session, object);
}

const uint8_t* MpGetTimerDesc(MpSession* session, uint32_t timerId)
{
    const uint8_t* object = MpQueryObjCached(session, 0,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_OBJECTS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::TimerDesc_u24TimerId, true, timerId);
    return MpVerifyTable<LibMP::TypesNtd::TimerDesc>(session, object);
}

const uint8_t* MpGetCounterDesc(MpSession* session, uint32_t counterId)
{
    const uint8_t* object = MpQueryObjCached(session, 0,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_OBJECTS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::CounterDesc_u24CounterId, true, counterId);
    return MpVerifyTable<LibMP::TypesNtd::CounterDesc>(session, object);
}

const uint8_t* MpGetThreadDesc(MpSession* session, uint32_t threadId)
{
    const uint8_t* object = MpQueryObjCached(session, 0,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_OBJECTS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::ThreadDesc_u24ThreadId, true, threadId);
    return MpVerifyTable<LibMP::TypesNtd::ThreadDesc>(session, object);
}

static constexpr uint32_t CACHABLE_OBJECT_TYPES_DETECTED = CACHABLE_TYPES_COUNTER_OBJECTS;
#undef CACHABLE_TYPES_COUNTER_OBJECTS

////////////////////////////////////////////////////////////////////////////////////////////////////
// Cachable arrays

static constexpr uint32_t BASE_COUNTER_VALUE_ARRAYS = __COUNTER__;
#define CACHABLE_TYPES_COUNTER_ARRAYS (__COUNTER__ - BASE_COUNTER_VALUE_ARRAYS - 1)

MpArray* MpGetTimerIds(MpSession* session)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::TimerIds);
}

MpArray* MpGetCounterIds(MpSession* session)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::CounterIds);
}

mpsize_t MpGetCounterIdsCountInternal(MpSession* session)
{
    CheckSync(session);
    if (!session)
        return 0;
    return MpQuery<MpGetArrayValuesCount>(
        session, BlockTypeMeta::Kv_LongTerm, 0, QueryEx::CounterIds);
}

MpArray* MpGetThreadIds(MpSession* session)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::LongTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::ThreadIds);
}

MpArray* MpGetCounterSamples(MpSession* session, uint32_t counterId)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::ShortTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_Counters, 0,
        QueryEx::CounterSamples_u24CounterId, true, counterId);
}

MpArray* MpInternalGetLabelBufferRegions(MpSession* session)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::ShortTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::LabelBufferRegions);
}

MpArray* MpGetPlaceIdSamples(MpSession* session)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::ShortTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::PlaceIdSamples);
}

MpArray* MpGetUtcTimestampSamples(MpSession* session)
{
    return MpQueryArrayCached(session,
        MpSession::CacheBucket::ShortTerm, CACHABLE_TYPES_COUNTER_ARRAYS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::UtcTimestampSamples);
}

static constexpr uint32_t CACHABLE_ARRAY_TYPES_DETECTED = CACHABLE_TYPES_COUNTER_ARRAYS;
#undef CACHABLE_TYPES_COUNTER_ARRAYS

////////////////////////////////////////////////////////////////////////////////////////////////////
// Cachable scalars

static constexpr uint32_t BASE_COUNTER_VALUE_SCALARS = __COUNTER__;
#define CACHABLE_TYPES_COUNTER_SCALARS (__COUNTER__ - BASE_COUNTER_VALUE_SCALARS - 1)

uint32_t MpGetGroupIdMax(MpSession* session)
{
    return MpQueryU32Cached(session,
        CACHABLE_TYPES_COUNTER_SCALARS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::GroupIdMax);
}

uint32_t MpGetThreadIdGpu(MpSession* session)
{
    return MpQueryU32Cached(session,
        CACHABLE_TYPES_COUNTER_SCALARS,
        BlockTypeMeta::Kv_LongTerm, 0,
        QueryEx::ThreadIdGpu);
}

uint32_t MpGetFrameIdMin(MpSession* session)
{
    return MpQueryU32Cached(session,
        CACHABLE_TYPES_COUNTER_SCALARS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::FrameIdMin);
}

uint32_t MpGetFrameIdMax(MpSession* session)
{
    return MpQueryU32Cached(session,
        CACHABLE_TYPES_COUNTER_SCALARS,
        BlockTypeMeta::Kv_ShortTerm, 0,
        QueryEx::FrameIdMax);
}

static constexpr uint32_t CACHABLE_SCALAR_TYPES_DETECTED = CACHABLE_TYPES_COUNTER_SCALARS;
#undef CACHABLE_TYPES_COUNTER_SCALARS

////////////////////////////////////////////////////////////////////////////////////////////////////
const uint8_t* MpGetFrameDesc(MpSession* session, uint32_t frameId)
{
    CheckSync(session);
    if (!session)
        return nullptr;
    const uint8_t* object = MpQueryObj(session, 0,
        BlockTypeMeta::Kv_Frames, frameId,
        QueryEx::FrameDesc_u24FrameId, true, frameId);
    return MpVerifyTable<LibMP::TypesNtd::FrameDesc>(session, object);
}

const uint8_t* MpGetLastCounterSample(MpSession* session, uint32_t counterId)
{
    CheckSync(session);
    if (!session)
        return nullptr;
    ScopeExit lock_cleanup([&]() { session->dumpContainer->DeactivateLock(); });

    MpSetQuery(session,
        BlockTypeMeta::Kv_Counters, 0,
        QueryEx::CounterSamples_u24CounterId, true, counterId);
    auto* kvstore = MpGetActiveProxy(session);
    if (!kvstore)
        return nullptr;
    const auto& uid = session->global_uid;
    auto& val = session->global_val_buf;
    bool found = kvstore->GetValueAtIndex(
         &uid.entries[0], uid.size, &uid.precomputed_hash,
         max_mpsize_t,
         val.elements_ptr, &val.size_in_u32, nullptr
    );
    if (!found)
        return nullptr;
    session->global_counter_sample = val.ToCounterSample();
    session->global_obj_size = sizeof(LibMP::TypesNtd::CounterSample);
    return reinterpret_cast<const uint8_t*>(&session->global_counter_sample);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
#if !ENABLE_EXTERNAL_CLEAR_OBJCACHE_CALLBACK

void LibMpExtClearObjCacheCallback(
    uint8_t* /*handle*/,
    const uint32_t* /*bucketIds*/,
    mpsize_t /*bucketCount*/)
{
}

#endif // !ENABLE_EXTERNAL_CLEAR_OBJCACHE_CALLBACK

#if !ENABLE_EXTERNAL_SESSION_NOTIFICATION_CALLBACK

void LibMpExtSessionNotificationCallback(
    uint8_t* /*handle*/, uint32_t /*notificationFlags*/)
{
}

#endif // !ENABLE_EXTERNAL_SESSION_NOTIFICATION_CALLBACK

////////////////////////////////////////////////////////////////////////////////////////////////////
static_assert(
    MpSession::CACHABLE_OBJECT_TYPES_COUNT <= CACHABLE_OBJECT_TYPES_DETECTED,
    "CACHABLE_OBJECT_TYPES_COUNT should be smaller"
);
static_assert(
    MpSession::CACHABLE_OBJECT_TYPES_COUNT >= CACHABLE_OBJECT_TYPES_DETECTED,
    "CACHABLE_OBJECT_TYPES_COUNT should be bigger"
);

static_assert(
    MpSession::CACHABLE_ARRAY_TYPES_COUNT <= CACHABLE_ARRAY_TYPES_DETECTED,
    "CACHABLE_ARRAY_TYPES_COUNT should be smaller"
);
static_assert(
    MpSession::CACHABLE_ARRAY_TYPES_COUNT >= CACHABLE_ARRAY_TYPES_DETECTED,
    "CACHABLE_ARRAY_TYPES_COUNT should be bigger"
);

static_assert(
    MpSession::CACHABLE_SCALAR_TYPES_COUNT <= CACHABLE_SCALAR_TYPES_DETECTED,
    "CACHABLE_SCALAR_TYPES_COUNT should be smaller"
);
static_assert(
    MpSession::CACHABLE_SCALAR_TYPES_COUNT >= CACHABLE_SCALAR_TYPES_DETECTED,
    "CACHABLE_SCALAR_TYPES_COUNT should be bigger"
);

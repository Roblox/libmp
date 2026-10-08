// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "c_api_private.h"
#include "log_entry.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/utility.h"

using namespace GPRX;
using namespace LibMP;

////////////////////////////////////////////////////////////////////////////////////////////////////
void MpSession::InvalidateLabelLookup(uint32_t notificationFlags)
{
    bLabelLookupDirty = true;
    NotifySession(notificationFlags);
}

uint64_t MpSession::GetLabelCacheChunkId(uint64_t offset)
{
    return offset / LABEL_CACHE_CHUNK_SIZE;
}

uint64_t MpSession::GetLabelRegionLookupChunkId(uint64_t offset)
{
    return offset / LABEL_REGION_LOOKUP_CHUNK_SIZE;
}

void MpSession::RebuildLabelRegionLookupIndex()
{
    labelRegionPrefixMaxEnds.clear();
    labelRegionPrefixMaxEnds.reserve(labelRegions.size());
    uint64_t maxEndOffset = 0;
    for (const LabelRegion& region : labelRegions)
    {
        maxEndOffset = ntd::max(maxEndOffset, region.endOffset);
        labelRegionPrefixMaxEnds.push_back(maxEndOffset);
    }

    ntd::memset(
        labelRegionLookupCache.data(), 0,
        labelRegionLookupCache.size() * sizeof(LabelRegionLookupEntry));
}

const MpSession::LabelRegion* MpSession::FindLabelRegion(uint64_t offset) const
{
    // Tiny region sets are cheaper to scan, newest starting region first.
    if (labelRegions.size() <= LABEL_REGION_LINEAR_SEARCH_LIMIT)
    {
        for (mpsize_t i = GPRX::to_mpsize(labelRegions.size()); i > 0; --i)
        {
            const LabelRegion& region = labelRegions[i - 1];
            if (region.startOffset <= offset && offset < region.endOffset)
                return &region;
        }
        return nullptr;
    }

    const uint64_t chunkId = GetLabelRegionLookupChunkId(offset);
    const uint64_t encodedChunkId = chunkId + 1;
    LabelRegionLookupEntry& lookup = labelRegionLookupCache[
        static_cast<mpsize_t>(chunkId % LABEL_REGION_LOOKUP_CACHE_SIZE)];
    if (lookup.encodedChunkId != encodedChunkId)
    {
        // On a chunk-cache miss, bound candidates to regions intersecting this
        // 64 KiB chunk. Prefix maximum ends also handle sparse and nested ranges.
        const uint64_t chunkStart = chunkId * LABEL_REGION_LOOKUP_CHUNK_SIZE;
        const bool isLastChunk =
            chunkStart > GPRX::max_u64 - LABEL_REGION_LOOKUP_CHUNK_SIZE;
        const uint64_t chunkEnd = isLastChunk
            ? GPRX::max_u64
            : chunkStart + LABEL_REGION_LOOKUP_CHUNK_SIZE;

        auto firstRegion = ntd::upper_bound(
            labelRegionPrefixMaxEnds.begin(), labelRegionPrefixMaxEnds.end(), chunkStart);
        lookup.firstRegionIndex = static_cast<mpsize_t>(
            firstRegion - labelRegionPrefixMaxEnds.begin());

        if (isLastChunk)
            lookup.onePastLastRegionIndex = GPRX::to_mpsize(labelRegions.size());
        else
        {
            auto onePastLastRegion = ntd::lower_bound(
                labelRegions.begin(), labelRegions.end(), chunkEnd,
                [](const LabelRegion& region, uint64_t value) {
                    return region.startOffset < value;
                });
            lookup.onePastLastRegionIndex = static_cast<mpsize_t>(
                onePastLastRegion - labelRegions.begin());
        }
        lookup.encodedChunkId = encodedChunkId;
    }

    // Find the newest candidate starting no later than the offset, then walk
    // backward only as needed to skip ranges that ended before the offset.
    using RegionDifference = ntd::vector<LabelRegion>::difference_type;
    auto candidate = ntd::upper_bound(
        labelRegions.begin() + static_cast<RegionDifference>(lookup.firstRegionIndex),
        labelRegions.begin() + static_cast<RegionDifference>(lookup.onePastLastRegionIndex),
        offset,
        [](uint64_t value, const LabelRegion& region) {
            return value < region.startOffset;
        });
    mpsize_t first = static_cast<mpsize_t>(candidate - labelRegions.begin());

    while (first > lookup.firstRegionIndex)
    {
        const LabelRegion& region = labelRegions[--first];
        if (offset < region.endOffset)
            return &region;
    }
    return nullptr;
}

bool MpSession::IsLabelCacheChunkFullyCovered(uint64_t chunkId) const
{
    if (chunkId > GPRX::max_u64 / LABEL_CACHE_CHUNK_SIZE)
        return false;
    const uint64_t startOffset = chunkId * LABEL_CACHE_CHUNK_SIZE;
    if (startOffset > GPRX::max_u64 - LABEL_CACHE_CHUNK_SIZE)
        return false;
    const LabelRegion* region = FindLabelRegion(startOffset);
    return region &&
        region->endOffset >= startOffset + LABEL_CACHE_CHUNK_SIZE;
}

bool MpSession::RefreshLabelLookup()
{
    if (bLabelLookupInitialized && !bLabelLookupDirty)
        return true;

    ntd::vector<LabelRegion> refreshed;
    MpArray* regions = MpInternalGetLabelBufferRegions(this);
    const mpsize_t count = MpGetArraySize(regions);
    refreshed.reserve(count);
    for (mpsize_t i = 0; i < count; ++i)
    {
        const uint8_t* ptr = MpInternalGetLabelBufferRegionFromArray(this, regions, i);
        if (!ptr)
            continue;
        const auto* value = reinterpret_cast<const LibMP::TypesNtd::LabelBufferRegion*>(ptr);
        if (value->start_offset() >= value->end_offset() ||
            value->buffer_start_locator() == 0)
            continue;

        LabelRegion region;
        region.startOffset = value->start_offset();
        region.endOffset = value->end_offset();
        region.bufferStartLocator.entry = value->buffer_start_locator();
        refreshed.push_back(region);
    }
    MpReleaseArray(regions);

    if (refreshed.size() > 1)
        ntd::sort(refreshed.begin(), refreshed.end());
    labelRegions = ntd::move(refreshed);
    RebuildLabelRegionLookupIndex();
    for (auto chunk = labelCache.begin(); chunk != labelCache.end();)
    {
        if (!IsLabelCacheChunkFullyCovered(chunk->first))
            chunk = labelCache.erase(chunk);
        else
            ++chunk;
    }

    bLabelLookupInitialized = true;
    bLabelLookupDirty = false;
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
const uint8_t* MpInternalGetLabelBufferRegionFromArray(
    MpSession* session, MpArray* array, mpsize_t index)
{
    CheckSync(session);
    if (!session || !array || index >= array->values_count)
        return nullptr;
    ScopeExit lock_cleanup([&]() { session->dumpContainer->DeactivateLock(); });

    ValueBuf& value_buf = array->GetValue(index);
    Locator locator = value_buf.ToLocator();
    mpsize_t obj_size = sizeof(LibMP::TypesNtd::LabelBufferRegion);
    uint8_t* obj = session->dumpContainer->GetDataByLocator(
        locator,
        ContainerContext::TypeOfData::Default,
        &obj_size
    );
    return obj;
}

static bool MpReadLabelString(
    MpSession* session,
    const MpSession::LabelRegion& region,
    uint64_t offset,
    ntd::string& result)
{
    static constexpr uint64_t MAX_LABEL_BUFFER_SIZE = 1024;

    if (!session || offset < region.startOffset || offset >= region.endOffset)
        return false;
    const auto [blockId, baseOffset] = GPRX::DecodeLocator(region.bufferStartLocator);
    GPRX::BlockContext* block = session->dumpContainer->GetBlock(blockId);
    if (!block)
        return false;

    const uint64_t offsetInRegion = offset - region.startOffset;
    const uint64_t available = region.endOffset - offset;
    if (offsetInRegion > GPRX::max_mpsize_t ||
        baseOffset > GPRX::max_mpsize_t - offsetInRegion)
        return false;

    result.clear();
    const uint64_t scanSize = ntd::min<uint64_t>(available, MAX_LABEL_BUFFER_SIZE);
    const mpsize_t size = static_cast<mpsize_t>(scanSize);
    const mpsize_t readOffset = static_cast<mpsize_t>(
        static_cast<uint64_t>(baseOffset) + offsetInRegion);
    const uint8_t* data = session->dumpContainer->GetPayloadDataPtr(
        *block, readOffset, size, nullptr);
    if (!data)
        return false;

    mpsize_t textSize = 0;
    while (textSize < size && data[textSize] != 0)
        ++textSize;
    if (textSize == size)
        return false;

    result.assign(reinterpret_cast<const char*>(data), textSize);
    return true;
}

static const uint8_t* MpReturnLabel(
    const ntd::string& label, uint32_t* outByteLength)
{
    if (outByteLength)
        *outByteLength = static_cast<uint32_t>(label.size());
    return reinterpret_cast<const uint8_t*>(label.c_str());
}

const uint8_t* MpGetLabelFromOffsetParts(
    MpSession* session,
    uint32_t offsetLow,
    uint32_t offsetHigh,
    uint32_t* outByteLength)
{
    if (outByteLength)
        *outByteLength = 0;
    CheckSync(session);
    if (!session || !session->dumpContainer || !session->RefreshLabelLookup())
        return nullptr;

    GPRX::U64Split split;
    split.parts.low = offsetLow;
    split.parts.high = offsetHigh;
    const uint64_t offset = split.entry;
    if (offset == 0)
        return nullptr;

    if (session->bLabelCacheEnabled)
    {
        const uint64_t chunkId = MpSession::GetLabelCacheChunkId(offset);
        auto chunk = session->labelCache.find(chunkId);
        if (chunk != session->labelCache.end())
        {
            auto cached = chunk->second.find(offset);
            if (cached != chunk->second.end())
                return MpReturnLabel(cached->second, outByteLength);
        }

        const MpSession::LabelRegion* region = session->FindLabelRegion(offset);
        if (!region)
            return nullptr;
        ntd::string label;
        if (!MpReadLabelString(session, *region, offset, label))
            return nullptr;
        auto& labels = session->labelCache[chunkId];
        auto inserted = labels.try_emplace(offset, ntd::move(label));
        return MpReturnLabel(inserted.first->second, outByteLength);
    }

    const MpSession::LabelRegion* region = session->FindLabelRegion(offset);
    if (!region)
        return nullptr;
    if (!MpReadLabelString(session, *region, offset, session->labelScratch))
        return nullptr;
    return MpReturnLabel(session->labelScratch, outByteLength);
}

const uint8_t* MpGetLabelFromOffset(
    MpSession* session,
    const uint64_t* offset,
    uint32_t* outByteLength)
{
    if (outByteLength)
        *outByteLength = 0;
    if (!offset)
        return nullptr;
    GPRX::U64Split split;
    split.entry = *offset;
    return MpGetLabelFromOffsetParts(
        session, split.parts.low, split.parts.high, outByteLength);
}

const uint8_t* MpGetLabelFromEntryParts(
    MpSession* session,
    uint32_t entryLow,
    uint32_t entryHigh,
    uint32_t* outByteLength)
{
    if (outByteLength)
        *outByteLength = 0;
    GPRX::U64Split entry;
    entry.parts.low = entryLow;
    entry.parts.high = entryHigh;
    if (MicroProfilerLogType(entry.entry) != MP_LOG_LABEL)
        return nullptr;

    GPRX::U64Split offset;
    offset.entry = MicroProfilerLogTick(entry.entry);
    return MpGetLabelFromOffsetParts(
        session, offset.parts.low, offset.parts.high, outByteLength);
}

const uint8_t* MpGetLabelFromEntry(
    MpSession* session,
    const uint64_t* entry,
    uint32_t* outByteLength)
{
    if (outByteLength)
        *outByteLength = 0;
    if (!entry)
        return nullptr;
    GPRX::U64Split split;
    split.entry = *entry;
    return MpGetLabelFromEntryParts(
        session, split.parts.low, split.parts.high, outByteLength);
}

uint32_t MpInternalGetLabelCacheChunkSize()
{
    return MpSession::LABEL_CACHE_CHUNK_SIZE;
}

void MpEnableLabelCache(MpSession* session, uint32_t enabled)
{
    if (!session)
        return;
    session->bLabelCacheEnabled = !!enabled;
    if (!enabled)
        session->labelCache.clear();
}

void MpClearLabelCache(MpSession* session)
{
    if (!session)
        return;
    session->labelCache.clear();
    session->labelScratch.clear();
}

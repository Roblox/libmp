// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "capture_builder.h"

#if !LIBMP_EXCLUDE_CAPTURE_BUILDER
// @amalgamate: config-begin capture-builder !LIBMP_EXCLUDE_CAPTURE_BUILDER

#include "gprx_container/utils.h"
#include "gprx_container/utils_ex.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/string_view.h"

namespace LibMP
{

inline static uint32_t CalculateFirstFrameId(uint32_t lastFrameId, uint32_t frameCount)
{
    const uint64_t nextFrameId = static_cast<uint64_t>(lastFrameId) + 1;
    if (nextFrameId <= frameCount)
        return 1;
    return GPRX::to_uint32(ntd::min<uint64_t>(
        nextFrameId - frameCount, GPRX::max_u32));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void CaptureBlockWriter::InitializeBlocks(
    const BlockDescWrapper* blockDescriptors,
    mpsize_t blockCount,
    uint32_t firstBlockId)
{
    if (is_init)
        return;

    MpInit();
    live_container = ntd::make_unique<GPRX::ContainerContext>();
    max_block_id = firstBlockId - 1;

    for (mpsize_t i = 0; i < blockCount; ++i)
        AddBlock(*blockDescriptors[i].blockDesc);

    auto& liveBlocks = live_container->GetBlocks();
    for (mpsize_t i = 0; i < blockCount; ++i)
    {
        const BlockDescWrapper& wrapper = blockDescriptors[i];
        GPRX::BlockContext* block = wrapper.blockDesc->ptr = &liveBlocks[wrapper.blockDesc->num];
        block->headerPtr->type_id = GPRX::ToBlockType(wrapper.type_id);
        block->headerPtr->type_meta = wrapper.type_meta;
    }

    live_container->RebuildDirectLookupTable();
    for (mpsize_t i = 0; i < blockCount; ++i)
    {
        const BlockDescWrapper& wrapper = blockDescriptors[i];
        if (GPRX::ToBlockType(wrapper.type_id) != GPRX::BlockType::KVStore)
            continue;

        BlockDesc& block = *wrapper.blockDesc;
        block.kv = ntd::make_unique<GPRX::KVStore>(*live_container, block.ptr);
        ResetKvStore(block);
    }
    is_init = true;
}

void CaptureBlockWriter::ResetBlocks(
    const BlockDescWrapper* blockDescriptors,
    mpsize_t blockCount,
    uint32_t firstBlockId)
{
    XASSERT_DIAG(is_init && live_container, "CaptureBlockWriter::ResetBlocks - writer is not initialized");
    if (!is_init || !live_container)
        return;

    max_block_id = firstBlockId - 1;
    sel_kv = nullptr;
    sel_block = nullptr;
    obj_buf = nullptr;
    obj_size = 0;
    builder.Clear();

    for (mpsize_t i = 0; i < blockCount; ++i)
    {
        const BlockDescWrapper& wrapper = blockDescriptors[i];
        GPRX::BlockContext* block = wrapper.blockDesc->ptr;
        block->ResetSoft();
        block->bNewWritableBlock = true;
        ntd::memset(block->headerPtr, 0, sizeof(GPRX::BlockHeader));
        block->headerPtr->block_id = GenBlockId();
        block->headerPtr->type_id = GPRX::ToBlockType(wrapper.type_id);
        block->headerPtr->type_meta = wrapper.type_meta;
    }

    for (mpsize_t i = 0; i < blockCount; ++i)
    {
        BlockDesc& block = *blockDescriptors[i].blockDesc;
        if (block.kv)
            ResetKvStore(block);
    }
    live_container->RebuildDirectLookupTable();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool LiveContext::UpdateLabelBuffer(
    const uint8_t* data,
    mpsize_t ringSize,
    uint64_t endOffset)
{
    if (!is_init || !kvshortterm_block.ptr || !kvshortterm_block.kv ||
        !shortterm_block.ptr || !labelbuffer_block.ptr ||
        (endOffset != 0 && (!data || ringSize == 0)))
    {
        return false;
    }

    const bool bufferChanged = !label_buffer_initialized ||
        data != label_buffer_data || ringSize != label_buffer_size ||
        endOffset != label_buffer_end_offset;
    const uint32_t kvBlockId = kvshortterm_block.GetBlockId();

    // LabelBufferRegions is an array-valued KV entry, so it cannot be replaced
    // after it has been registered in the current short-term store.
    if (bufferChanged && label_regions_kv_block_id == kvBlockId)
    {
        XASSERT_DIAG(false,
            "LiveContext::UpdateLabelBuffer - label buffer updated twice in one refresh");
        return false;
    }

    if (bufferChanged)
    {
        labelbuffer_block.ptr->ClearPayloadHard(true);
        label_buffer_region_count = 0;

        if (!label_block_id_rotated)
            IncBlockId(labelbuffer_block);
        label_block_id_rotated = false;

        if (endOffset != 0)
        {
            const mpsize_t guardSize = config.bProtectLiveLabelWrites
                ? ringSize / LABEL_BUFFER_WRITE_GUARD_DIVISOR +
                    (ringSize % LABEL_BUFFER_WRITE_GUARD_DIVISOR != 0)
                : 0;
            const mpsize_t visibleCapacity = ringSize - ntd::min(ringSize, guardSize);
            const uint64_t visibleSize = ntd::min<uint64_t>(endOffset, visibleCapacity);
            const uint64_t startOffset = endOffset - visibleSize;

            if (visibleSize != 0)
            {
                const mpsize_t startInRing = static_cast<mpsize_t>(startOffset % ringSize);
                const mpsize_t bytesToRingEnd = ringSize - startInRing;
                const mpsize_t firstLogicalSize = static_cast<mpsize_t>(
                    ntd::min<uint64_t>(visibleSize, bytesToRingEnd));
                const uint64_t wrapOffset = startOffset + firstLogicalSize;
                const mpsize_t secondSize = static_cast<mpsize_t>(
                    visibleSize - firstLogicalSize);
                if (secondSize != 0 && firstLogicalSize >
                    GPRX::max_mpsize_t - (GPRX::kDataAlignment - 1))
                {
                    live_container->RebuildDirectLookupTable();
                    return false;
                }
                const mpsize_t secondPayloadOffset = secondSize == 0
                    ? firstLogicalSize
                    : GPRX::align_up<GPRX::kDataAlignment>(firstLogicalSize);
                const mpsize_t alignmentPadding = secondPayloadOffset - firstLogicalSize;

                if (!labelbuffer_block.ptr->AddExternalPayloadChunk(
                        const_cast<uint8_t*>(data + startInRing), firstLogicalSize) ||
                    (alignmentPadding != 0 && !labelbuffer_block.ptr->AddExternalPayloadChunk(
                        const_cast<uint8_t*>(GPRX::kGlobalZeroBuffer), alignmentPadding)) ||
                    (secondSize != 0 && !labelbuffer_block.ptr->AddExternalPayloadChunk(
                        const_cast<uint8_t*>(data), secondSize)))
                {
                    labelbuffer_block.ptr->ClearPayloadHard(true);
                    live_container->RebuildDirectLookupTable();
                    return false;
                }

                label_buffer_regions[label_buffer_region_count++] = {
                    startOffset,
                    wrapOffset,
                    0,
                };
                if (secondSize != 0)
                {
                    label_buffer_regions[label_buffer_region_count++] = {
                        wrapOffset,
                        endOffset,
                        secondPayloadOffset,
                    };
                }
            }
        }

        label_buffer_data = data;
        label_buffer_size = ringSize;
        label_buffer_end_offset = endOffset;
        label_buffer_initialized = true;
        live_container->RebuildDirectLookupTable();
    }

    if (label_buffer_region_count == 0)
    {
        label_block_id_rotated = false;
        return true;
    }
    if (label_regions_kv_block_id == kvBlockId)
    {
        label_block_id_rotated = false;
        return true;
    }

    GPRX::KVStore* previousKv = sel_kv;
    GPRX::BlockContext* previousBlock = sel_block;
    SelectKV(kvshortterm_block, shortterm_block);

    bool success = true;
    for (mpsize_t i = 0; i < label_buffer_region_count; ++i)
    {
        const LabelBufferRegionState& regionState = label_buffer_regions[i];
        const GPRX::Locator locator = GPRX::BuildDataLocator(
            labelbuffer_block.GetBlockId(), regionState.payload_offset);
        const LibMP::TypesNtd::LabelBufferRegion region(
            regionState.start_offset, regionState.end_offset, locator.entry);
        SetUid(LibMP::QueryEx::LabelBufferRegions);
        if (!PutArrayStruct(region))
        {
            success = false;
            break;
        }
    }

    sel_kv = previousKv;
    sel_block = previousBlock;
    if (success)
        label_regions_kv_block_id = kvBlockId;
    label_block_id_rotated = false;
    return success;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool LiveContext::PutLabelBufferRegion(uint64_t startOffset, const uint8_t* data, mpsize_t size)
{
    return PutLabelBufferRegion(startOffset, data, size, nullptr, 0);
}

bool LiveContext::PutLabelBufferRegion(
    uint64_t startOffset,
    const uint8_t* firstData,
    mpsize_t firstSize,
    const uint8_t* secondData,
    mpsize_t secondSize)
{
    if (!sel_kv || !sel_block ||
        (firstSize != 0 && !firstData) || (secondSize != 0 && !secondData) ||
        firstSize > GPRX::max_mpsize_t - secondSize)
    {
        return false;
    }

    const mpsize_t size = firstSize + secondSize;
    if (size == 0 || startOffset > GPRX::max_u64 - size ||
        !live_container->AppendAlignmentPadding(*sel_block))
        return false;

    const mpsize_t dataStartOffset =
        sel_block->headerPtr->payload_uncompressed_size;
    if ((firstSize != 0 && !live_container->AppendPayloadData(
            *sel_block, firstData, firstSize)) ||
        (secondSize != 0 && !live_container->AppendPayloadData(
            *sel_block, secondData, secondSize)))
    {
        return false;
    }

    const GPRX::Locator locator = GPRX::BuildDataLocator(
        sel_block->headerPtr->block_id, dataStartOffset);

    const LibMP::TypesNtd::LabelBufferRegion region(
        startOffset, startOffset + size, locator.entry);
    SetUid(LibMP::QueryEx::LabelBufferRegions);
    return PutArrayStruct(region);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void LiveContext::Init()
{
    Init(max_block_id + 1);
}

void LiveContext::Init(uint32_t firstBlockId)
{
    if (is_init)
        return;

    // The header text size is a multiple of 8 (kDataAlignment)
    static constexpr ntd::string_view kHeaderText = "[[ Open with LibMP :: https://github.com/Roblox/libmp ]]";

    live_datacontext = ntd::make_unique<GPRX::DataContextMemChunks>();

    AdjustKvConfigs();
    InitializeBlocks(blocks, sizeof(blocks) / sizeof(blocks[0]), firstBlockId);

    live_container->InitMagicBlock(*magic_block.ptr);
    live_container->AppendPayloadData(
        *magic_block.ptr,
        reinterpret_cast<const uint8_t*>(kHeaderText.data()),
        GPRX::to_mpsize(kHeaderText.size()));
    magic_block.ptr->bFixedEncoding = true;

    // Thread log blocks contain pointers to ring buffers and do not own the data,
    // so mark them as non-writable
    logs0_block.ptr->bNewWritableBlock = false;
    logs1_block.ptr->bNewWritableBlock = false;

    // Temporarily make half of the KV blocks invisible
    kvframes0_block.ptr->headerPtr->type_id = GPRX::BlockType::Data;
    kvcounters0_block.ptr->headerPtr->type_id = GPRX::BlockType::Data;

    live_container->RebuildDirectLookupTable();

}

void LiveContext::AdjustKvConfigs()
{
    config.max_frames_per_kv = ntd::max<uint32_t>(1,
        ntd::min(config.max_frames_per_kv, MAX_FRAMES_PER_KV_LIMIT));
    uint32_t samplesPerKv = ntd::max<uint32_t>(2, GPRX::div_roundup<uint32_t>(config.max_frames_per_kv, 2));
    uint16_t v_entries = GPRX::to_uint16(samplesPerKv * 3 + 4); // 3 u32 entries per CounterSample + V-line meta
    config_counters.v_entries = v_entries;
}

void LiveContext::SetDataChanged()
{
    state.bExportRequired = true;
    state.bChangeNotificationRequired = true;

    if (config.bExportOnChange)
        CheckExport();
}

void LiveContext::CheckExport()
{
    if (!state.bExportRequired)
        return;

    state.bExportRequired = false;
    live_container->BuildGlobalIndex(*globalindex_block.ptr);
    live_container->InitExternalDataContextWithDataChunks(*live_datacontext);
}

bool LiveContext::IsNotificationRequired()
{
    return state.bChangeNotificationRequired;
}

void LiveContext::ResetNotificationFlag()
{
    state.bChangeNotificationRequired = false;
}

void LiveContext::SanitizeThreads(bool sanitizeOnly)
{
    if (!bInitialFillingCompleted || GetFramesInKv() == 0)
        return;

    bool threadLogsRemoved = false;

    uint32_t frameIdMinBlock1 = GetFrameIdMinBlock1();
    uint32_t frameIdMinAbs = GetFrameIdMinAbs();
    for (uint32_t frameId = frameIdMinAbs; frameId <= frames_counter; frameId++)
    {
        auto [kv_block, logs_block, _] = GetKvAndLogBlockRefs(frameId, frameIdMinBlock1);
        SelectKV(kv_block, kv_block);

        SetUid(QueryEx::FrameDesc_u24FrameId, true, frameId);
        const auto* frame_desc = GetTable<LibMP::TypesNtd::FrameDesc>();
        if (!frame_desc)
            continue;

        for (mpsize_t thread_num = 0; thread_num < frame_desc->thread_logs()->size(); thread_num++)
        {
            auto* thread_log = const_cast<LibMP::TypesNtd::ThreadLogDescr*>(frame_desc->thread_logs()->Get(thread_num));
            if (live_threads.count(thread_log->thread_id()) || thread_log->is_removed())
                continue;

            if (thread_log->log_entries_num() > 0)
            {
                GPRX::Locator locator;
                locator.entry = thread_log->log_start_locator();
                const uint32_t offset = DecodeLocator(locator).second;
                
                logs_block.ptr->ZeroPayloadRegion(offset, thread_log->log_entries_num() * sizeof(uint64_t));
            }

            thread_log->mutate_is_removed(true);
            threadLogsRemoved = true;
        }
    }

    if (threadLogsRemoved)
    {
        // logs0_block and logs1_block are likely modified as well, but since parsing data from zeroed regions
        // is an invalid operation, reading garbage data without crashing is expected behavior. Therefore,
        // we don't need to mark these blocks as modified or force eviction of their data from the caches.

        MarkBlockAsModified(frames0_block);
        IncBlockId(kvframes0_block);

        MarkBlockAsModified(frames1_block);
        IncBlockId(kvframes1_block);

        live_container->RebuildDirectLookupTable();

        UpdateKvStore(kvframes0_block);
        UpdateKvStore(kvframes1_block);

        if (sanitizeOnly)
            SetDataChanged();
    }
}

void LiveContext::TrimLogs(uint32_t availFrameNum, uint32_t firstFrameAbsId)
{
    // Helper lambda to "soft clear" a block.
    // Trimming up to the maximum payload size frees the underlying data 
    // but retains the virtual offset.
    auto trimAllData = [](BlockDesc& b)
    {
        b.ptr->TrimPayloadFront(b.ptr->virtualStartOffset + b.ptr->headerPtr->payload_uncompressed_size);
    };

    bool hasFrames = (availFrameNum > 0);
    uint32_t framesInKv = GetFramesInKv();
    storedFramesCount = ntd::min({ availFrameNum, framesInKv, config.max_frames_per_kv });
    bool hasStoredFrames = (storedFramesCount > 0);

    // Initial sequential bounds based purely on count capacity
    frameIdMin = hasStoredFrames ? CalculateFirstFrameId(frames_counter, storedFramesCount) : 0;
    frameIdMax = hasStoredFrames ? frames_counter : 0;

    if (!hasFrames || !hasStoredFrames)
        return;

    uint32_t frameIdMinBlock1 = GetFrameIdMinBlock1();
    bool needsEviction = (availFrameNum < framesInKv);
    
    bool foundDataStart = false;
    bool lockedMin = false;
    uint32_t newFrameIdMin = frameIdMin;
    const uint32_t loopStartFrameId = frameIdMin;

    for (uint32_t frameId = loopStartFrameId; frameId <= frameIdMax; frameId++)
    {
        auto [kv_block, logs_block, isBlock1] = GetKvAndLogBlockRefs(frameId, frameIdMinBlock1);
        SelectKV(kv_block, kv_block);

        SetUid(QueryEx::FrameDesc_u24FrameId, true, frameId);
        const auto* frame_desc = GetTable<LibMP::TypesNtd::FrameDesc>();
        
        // Enforce constraints on the oldest frame
        if (!lockedMin)
        {
            bool tooOld = (!frame_desc || frame_desc->frame_absolute_id() < firstFrameAbsId);

            // Fast-path: If the very first evaluated frame isn't too old, 
            // and we didn't trigger capacity eviction, we have nothing to trim.
            if (frameId == loopStartFrameId && !needsEviction && !tooOld)
                return;

            if (tooOld)
            {
                // This frame is too old, advance min pointer
                newFrameIdMin = frameId + 1;
                // Skip looking for thread data in this evicted frame
                continue;
            }

            // Lock the min pointer: this is our oldest valid frame
            lockedMin = true;
        }

        // Locate the precise trim offset
        if (!frame_desc || !frame_desc->thread_logs())
            continue;

        for (mpsize_t i = 0; i < frame_desc->thread_logs()->size(); i++)
        {
            const auto* thread_log = frame_desc->thread_logs()->Get(i);
            if (thread_log->log_entries_num() == 0)
                continue;

            GPRX::Locator locator;
            locator.entry = thread_log->log_start_locator();
            const uint32_t offset = DecodeLocator(locator).second;
            
            // Trim whichever block holds the first valid data
            logs_block.ptr->TrimPayloadFront(offset);
            
            // If the valid data was found in logs1_block, it guarantees
            // logs0_block data is fully evicted
            if (isBlock1)
            {
                trimAllData(logs0_block);
            }
            
            foundDataStart = true;
            break;
        }

        if (foundDataStart)
            break;
    }

    // Only reached if we actually had to evict frames
    if (!foundDataStart)
    {
        // Edge case: All evaluated frames had absolutely no thread data.
        trimAllData(logs0_block);
        trimAllData(logs1_block);
    }

    // Commit adjusted bounds
    frameIdMin = newFrameIdMin;
    if (frameIdMin > frameIdMax)
    {
        frameIdMin = 0;
        frameIdMax = 0;
        storedFramesCount = 0;
    }
    else
    {
        storedFramesCount = (frameIdMax - frameIdMin) + 1;
    }
}

static bool PrepareLiveUpdate(
    LiveContext& live,
    bool longTermChanged,
    bool updateFrameBlocks,
    bool clearFrames,
    bool rebuild)
{
    if (rebuild)
        longTermChanged = true;

    if (live.bInitialFillingCompleted)
    {
        if (clearFrames)
        {
            live.ClearAndIncBlockId(live.logs0_block);
            live.ClearAndIncBlockId(live.frames0_block);
            live.IncBlockId(live.kvframes0_block);

            live.ClearAndIncBlockId(live.logs1_block);
            live.ClearAndIncBlockId(live.frames1_block);
        }

        if (updateFrameBlocks)
            live.IncBlockId(live.kvframes1_block);
        live.IncBlockId(live.kvcounters1_block);

        if (rebuild)
        {
            live.max_block_id = live.magic_block.GetBlockId();
            live.IncBlockId(live.magic_block);
            live.IncBlockId(live.basicmeta_block);
            live.IncBlockId(live.labelbuffer_block);
            live.label_block_id_rotated = true;
            live.IncBlockId(live.globalindex_block);
        }

        live.ClearAndIncBlockId(live.shortterm_block);
        live.IncBlockId(live.kvshortterm_block);

        if (longTermChanged)
        {
            live.ClearAndIncBlockId(live.longterm_block);
            live.IncBlockId(live.kvlongterm_block);
        }

        live.live_container->RebuildDirectLookupTable();

        live.ResetKvStore(live.kvshortterm_block);
        live.UpdateKvStore(live.kvcounters1_block);

        if (clearFrames)
        {
            live.ResetKvStore(live.kvframes0_block);
            live.ResetKvStore(live.kvframes1_block);
        }
        else if (updateFrameBlocks)
        {
            live.UpdateKvStore(live.kvframes1_block);
        }

        if (longTermChanged)
            live.ResetKvStore(live.kvlongterm_block);
    }

    return longTermChanged;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool LiveContext::StartAddingFrame(bool longTermChanged, bool hasFrames)
{
    frames_counter++;
    const bool clearFrames = !hasFrames && (GetFramesInKv() > 0);
    if (hasFrames)
    {
        frames_in_kv1++;
    }
    else
    {
        frames_in_kv0 = 0;
        frames_in_kv1 = 0;
    }

    const bool rebuild = (frames_counter % config.max_frames_per_kv == 0) &&
        (max_block_id - magic_block.GetBlockId() > MAX_BLOCK_ID_DIFF);

    kvframes1_block.ptr->headerPtr->type_id = GPRX::BlockType::KVStore;

    LibMP::BlockBasicMeta::PutBasicMetaBlockFrameIdMaxToMeta(basicmeta_block.ptr->headerPtr, frames_counter);
    LibMP::KVStoreEx::PutKvStoreFrameIdToMeta(kvframes1_block.ptr->headerPtr, frames_counter);

    return PrepareLiveUpdate(*this, longTermChanged, true, clearFrames, rebuild);
}

bool LiveContext::StartAddingCounterSample(bool longTermChanged)
{
    return StartAddingFrame(longTermChanged, false);
}

bool LiveContext::StartReplacingCounterSnapshot(bool longTermChanged)
{
    const bool rebuild = max_block_id - magic_block.GetBlockId() > MAX_BLOCK_ID_DIFF;
    return PrepareLiveUpdate(*this, longTermChanged, false, false, rebuild);
}

void LiveContext::FinishAddingFrame()
{
    if (config.bRotateFrameBlocks && frames_counter % config.max_frames_per_kv == 0)
    {
        frames_in_kv0 = 0;

        kvframes0_block.ptr->headerPtr->type_id = GPRX::BlockType::Data;

        LibMP::KVStoreEx::PutKvStoreFrameIdToMeta(kvframes0_block.ptr->headerPtr, frames_counter);

        ClearAndIncBlockId(logs0_block);
        ClearAndIncBlockId(frames0_block);
        IncBlockId(kvframes0_block);
        IncBlockId(kvcounters0_block);

        SwapBlocks(logs0_block, logs1_block);
        SwapBlocks(frames0_block, frames1_block);
        SwapBlocks(kvframes0_block, kvframes1_block);
        SwapBlocks(kvcounters0_block, kvcounters1_block);
        ntd::swap(frames_in_kv0, frames_in_kv1);

        live_container->RebuildDirectLookupTable();

        UpdateKvStore(kvframes0_block);
        UpdateKvStore(kvcounters0_block);
        ResetKvStore(kvframes1_block);
        ResetKvStore(kvcounters1_block);

        kvcounters1_block.ptr->headerPtr->type_id = GPRX::BlockType::KVStore;
    }
}

void LiveContext::FinishAddingCounterSample()
{
    FinishAddingFrame();
}

uint32_t LiveContext::GetFramesInKv()
{
    return frames_in_kv0 + frames_in_kv1;
}

uint32_t LiveContext::GetFrameIdMinBlock1()
{
    return CalculateFirstFrameId(frames_counter, frames_in_kv1);
}

uint32_t LiveContext::GetFrameIdMinAbs()
{
    return CalculateFirstFrameId(frames_counter, GetFramesInKv());
}

ntd::tuple<BlockDesc&, BlockDesc&, bool> LiveContext::GetKvAndLogBlockRefs(uint32_t frameId, uint32_t frameIdMinBlock1)
{
    bool isBlock1 = (frameId >= frameIdMinBlock1);
    auto& kv_block = isBlock1 ? kvframes1_block : kvframes0_block;
    auto& logs_block = isBlock1 ? logs1_block : logs0_block;
    return { kv_block, logs_block, isBlock1 };
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void CaptureBlockWriter::AddBlock(BlockDesc& b)
{
    if (live_container->GetBlocks().size() >= GPRX::max_mpsize_t)
    {
        XASSERT_DIAG(false, "CaptureBlockWriter::AddBlock - block count exceeds mpsize_t");
        return;
    }

    b.num = GPRX::to_uint32(live_container->GetBlocks().size());
    uint32_t id = GenBlockId();
    live_container->AddBlock(id, true);
}

void CaptureBlockWriter::MarkBlockAsModified(BlockDesc& b)
{
    // Force eviction of the block's payload from caches by increasing its size
    uint32_t out_offset = 0;
    live_container->AppendPayloadDataContiguous(*b.ptr, nullptr, GPRX::kDataAlignment, out_offset, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
SegmentCaptureContext::SegmentCaptureContext(uint32_t maxFrames)
{
    const uint32_t clampedFrames = ntd::max<uint32_t>(1, ntd::min(maxFrames, MAX_FRAMES));
    const uint32_t samplesPerKv = ntd::max<uint32_t>(2, GPRX::div_roundup<uint32_t>(clampedFrames, 2));
    config_counters.v_entries = GPRX::to_uint16(samplesPerKv * 3 + 4);
}

void SegmentCaptureContext::Reset(uint32_t firstBlockId, uint32_t firstFrameId)
{
    if (!is_init)
        InitializeBlocks(blocks, sizeof(blocks) / sizeof(blocks[0]), firstBlockId);
    else
        ResetBlocks(blocks, sizeof(blocks) / sizeof(blocks[0]), firstBlockId);

    frames_counter = firstFrameId - 1;
    storedFramesCount = 0;
    frameIdMin = 0;
    frameIdMax = 0;
    trackfinish_block.ptr->bDontSave = true;
}

void SegmentCaptureContext::StartAddingFrame()
{
    frames_counter++;
    LibMP::BlockBasicMeta::PutBasicMetaBlockFrameIdMaxToMeta(basicmeta_block.ptr->headerPtr, frames_counter);
    LibMP::KVStoreEx::PutKvStoreFrameIdToMeta(kvframes_block.ptr->headerPtr, frames_counter);
}

} // namespace LibMP

// @amalgamate: config-end capture-builder
#endif // LIBMP_EXCLUDE_CAPTURE_BUILDER

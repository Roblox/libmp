// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "container_context.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/limits.h"

namespace GPRX
{

bool ContainerContext::IsKnownPayloadEncoding(BlockPayloadEncodingMethod method)
{
    return method >= BlockPayloadEncodingMethod::First &&
        method <= BlockPayloadEncodingMethod::Last;
}

static constexpr mpsize_t GlobalIndexEntrySize =
    sizeof(BlockHeader) + sizeof(mpdatasz_t);
static_assert(GlobalIndexEntrySize % kDataAlignment == 0,
    "Global-index entries must preserve payload alignment");

bool ContainerContext::TryGetTrailedPayloadRegionSize(
    mpsize_t payload_actual_size,
    bool raw_headers,
    mpsize_t& out_region_size)
{
    const mpsize_t trailer_size = GetBlockTrailerDiskSize(raw_headers);
    if (payload_actual_size > max_mpsize_t - trailer_size)
        return false;
    const mpsize_t min_region_size = payload_actual_size + trailer_size;
    if (min_region_size > max_mpsize_t - (PAYLOAD_TRAILER_SEARCH_STEP - 1))
        return false;
    out_region_size =
        align_up<PAYLOAD_TRAILER_SEARCH_STEP>(min_region_size);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool BlockContext::AddExternalPayloadChunk(uint8_t* chunk_ptr, mpsize_t chunk_size)
{
    if (!bOwnsHeaderPtr || !headerPtr || (!chunk_ptr && chunk_size > 0) ||
        chunk_size > max_mpsize_t - headerPtr->payload_uncompressed_size ||
        chunk_size > max_mpsize_t - headerPtr->payload_size)
        return false;
    if (chunk_size == 0)
        return true;
    if (!bUsesExternalPayloadChunks &&
        (!payloadChunks.empty() || headerPtr->payload_uncompressed_size != 0 || headerPtr->payload_size != 0))
        return false;

    payloadChunks.emplace_back();
    auto& new_chunk = payloadChunks.back();
    new_chunk.AssignMemUnsafe(chunk_ptr, chunk_size);
    bUsesExternalPayloadChunks = true;
    headerPtr->payload_uncompressed_size += chunk_size;
    headerPtr->payload_size += chunk_size;
    return true;
}

void BlockContext::AssignExternalHeader(BlockHeader* header_ptr)
{
    FreeHeader();
    headerPtr = header_ptr;
}

void BlockContext::AllocateHeader()
{
    if (!bOwnsHeaderPtr)
        headerPtr = ntd::New<BlockHeader>();
    ntd::memset(headerPtr, 0, sizeof(BlockHeader));
    bOwnsHeaderPtr = true;
}

void BlockContext::FreeHeader()
{
    if (bOwnsHeaderPtr)
        ntd::Delete(headerPtr);
    headerPtr = 0;
    bOwnsHeaderPtr = false;
}

void BlockContext::ClearPayloadSoft(bool bAllowNonWritableBlocks)
{
    if ((!bNewWritableBlock && !bAllowNonWritableBlocks) || !bOwnsHeaderPtr)
        return;
    headerPtr->payload_size = 0;
    headerPtr->payload_uncompressed_size = 0;
    Block::PutDataBlockVirtualStartOffsetToMeta(headerPtr, 0);
    virtualStartOffset = 0;
}

void BlockContext::ClearPayloadHard(bool bAllowNonWritableBlocks)
{
    if ((!bNewWritableBlock && !bAllowNonWritableBlocks) || !bOwnsHeaderPtr)
        return;
    ClearPayloadSoft(bAllowNonWritableBlocks);
    payloadChunks.clear();
    bUsesExternalPayloadChunks = false;
}

void BlockContext::TrimPayloadFront(mpsize_t upToVirtualOffset)
{
    if (bNewWritableBlock || !headerPtr)
        return;

    // Ignore if the requested offset is already behind the current start
    if (upToVirtualOffset <= virtualStartOffset)
        return;

    mpsize_t target_bytes_to_trim = upToVirtualOffset - virtualStartOffset;
    mpsize_t actual_bytes_trimmed = 0;
    mpsize_t chunks_to_remove = 0;

    // Determine how many chunks to drop
    for (const auto& chunk : payloadChunks)
    {
        if (actual_bytes_trimmed >= target_bytes_to_trim)
            break;

        const mpsize_t chunk_size = chunk.size();
        const mpsize_t remaining_to_target = target_bytes_to_trim - actual_bytes_trimmed;

        // Check for partial chunk intersection
        if (remaining_to_target < chunk_size)
        {
            XASSERT_DIAG(false, "BlockContext::TrimPayloadFront - Partial chunk removal requested");
        }

        actual_bytes_trimmed += chunk_size;
        chunks_to_remove++;
    }

    // Perform the physical trim and update virtual metadata
    if (chunks_to_remove > 0)
    {
        // Physically remove the chunks from the beginning
        using ChunkDifference = MemChunkList::difference_type;
        auto first_remaining_chunk = payloadChunks.begin()
            + static_cast<ChunkDifference>(chunks_to_remove);
        payloadChunks.erase(payloadChunks.begin(), first_remaining_chunk);
        // Advance the virtual offset by the exact amount of data physically removed
        virtualStartOffset += actual_bytes_trimmed;
        // Store the new virtual offset in the header's metadata1
        Block::PutDataBlockVirtualStartOffsetToMeta(headerPtr, virtualStartOffset);
        // Reflect the removed data in the size trackers
        headerPtr->payload_uncompressed_size -= ntd::min(headerPtr->payload_uncompressed_size, actual_bytes_trimmed);
        headerPtr->payload_size -= ntd::min(headerPtr->payload_size, actual_bytes_trimmed);
    }
}

void BlockContext::ZeroPayloadRegion(mpsize_t start_offset, mpsize_t size)
{
    if (bNewWritableBlock || payloadChunks.empty() || size == 0)
        return;

    if (start_offset > max_mpsize_t - size)
        return;

    // If the region ends before our current virtual start, there's nothing to zero
    if (start_offset + size <= virtualStartOffset)
        return;

    // Convert virtual offsets to relative (exclusive) boundaries
    mpsize_t relative_start = 0;
    if (start_offset > virtualStartOffset)
        relative_start = start_offset - virtualStartOffset;
        
    mpsize_t relative_end = (start_offset + size) - virtualStartOffset;
    mpsize_t current_offset = 0;

    for (auto& chunk : payloadChunks)
    {
        const mpsize_t chunk_size = chunk.size();
        const mpsize_t chunk_start = current_offset;
        const mpsize_t chunk_end = current_offset + chunk_size; // Exclusive bound

        // Check if the chunk overlaps with the target region
        if (chunk_start < relative_end && chunk_end > relative_start)
        {
            // Check for partial overlap
            bool is_partial = (chunk_start < relative_start) || (chunk_end > relative_end);
            if (is_partial)
            {
                XASSERT_DIAG(false, "BlockContext::ZeroPayloadRegion - Partial chunk zeroing requested");
            }

            // Check memory ownership
            if (chunk.bOwnsPtr)
            {
                // Skip the chunk
                XASSERT_DIAG(false, "BlockContext::ZeroPayloadRegion - Cannot zero memory-owning chunks");
            }
            else
            {
                // Mark the chunk
                chunk.bForceZero = true;
            }
        }

        current_offset += chunk_size;

        // Early exit if we have completely passed the target region
        if (current_offset >= relative_end)
            break;
    }
}

BlockContext::~BlockContext()
{
    FreeHeader();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void ContainerContext::ValidationStatus::CheckMatches()
{
    container_version_match = (container_version == GPRX::CONTAINER_VERSION);
    dataformat_major_version_match = (dataformat_major_version == s_dataformat_major_version);
    dataformat_signature_match = (dataformat_signature == s_dataformat_signature);
}

bool ContainerContext::ValidationStatus::IsMatched()
{
    return (container_magic_match && container_version_match && dataformat_major_version_match && dataformat_signature_match);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void ContainerContext::SetDataFormatMeta(uint64_t signature56bit, uint8_t majorVersion8bit, uint8_t minorVersion8bit)
{
    s_dataformat_signature = (signature56bit << 8) >> 8;
    s_dataformat_major_version = majorVersion8bit;
    s_dataformat_minor_version = minorVersion8bit;
}

void ContainerContext::AddBlock(uint32_t block_id, bool bNewWritableBlock)
{
    m_block_map.Clear();

    if (m_blocks.size() >= max_mpsize_t)
    {
        XASSERT_DIAG(false, "ContainerContext::AddBlock - block count exceeds mpsize_t");
        return;
    }

    m_blocks.emplace_back();
    BlockContext& new_block_ctx = m_blocks.back();
    new_block_ctx.AllocateHeader();
    new_block_ctx.headerPtr->block_id = block_id;
    new_block_ctx.bNewWritableBlock = bNewWritableBlock;
}

bool ContainerContext::RemoveBlock(uint32_t /*block_id*/)
{
    return false;
}

BlockContext* ContainerContext::GetBlock(uint32_t block_id)
{
    return m_block_map.Find(block_id);
}

bool ContainerContext::TryCacheReadOnlyBlockPayload(BlockContext& block, bool& out_was_cached)
{
    out_was_cached = false;
    if (!block.headerPtr)
        return false;
    
    mpsize_t uncompressed_size = block.headerPtr->payload_uncompressed_size;
    if (!block.bNewWritableBlock && block.payloadChunks.empty() && uncompressed_size > 0)
    {
        MemChunk uncompressed_payload;
        if (!uncompressed_payload.Allocate(uncompressed_size))
            return false;
        // Cache only a fully populated payload.
        if (!ReadPayloadDataToBuffer(block, block.virtualStartOffset,
                uncompressed_size, uncompressed_payload.data(), uncompressed_size))
            return false;
        block.payloadChunks.emplace_back(ntd::move(uncompressed_payload));
        out_was_cached = true;
    }
    return true;
}

bool ContainerContext::CacheReadOnlyBlockPayload(BlockContext& block)
{
    bool was_cached = false;
    return TryCacheReadOnlyBlockPayload(block, was_cached) && was_cached;
}

void ContainerContext::CacheReadOnlyBlockPayloads()
{
    for (auto& block : m_blocks)
    {
        CacheReadOnlyBlockPayload(block);
    }
}

void ContainerContext::ClearReadOnlyBlockPayloadCaches()
{
    for (auto& block : m_blocks)
    {
        if (!block.bNewWritableBlock && !block.payloadChunks.empty())
            block.payloadChunks.clear();
    }
}

void ContainerContext::InitMagicBlock(BlockContext& block, BlockPayloadEncodingMethod containerEncoding)
{
    if (!block.headerPtr)
        return;
    block.headerPtr->type_id = ToBlockType(GPRX::CONTAINER_VERSION);
    block.headerPtr->metadata0 = IsPayloadAsciiEncoded(containerEncoding) ? MagicFormatHexFull : MagicFormatRawFull;
    block.headerPtr->metadata1 = s_dataformat_signature | (static_cast<uint64_t>(s_dataformat_major_version) << (7 * 8));
    block.headerPtr->type_meta = s_dataformat_minor_version;
}

void ContainerContext::SetContainerEncodingMethod(BlockPayloadEncodingMethod method)
{
    m_global_index_serialization_encoding = method;
    bGlobalIndexSerializationEncodingSet = true;

    for (auto& block : m_blocks)
    {
        if (!block.headerPtr || block.bDontSave)
            continue;
        auto* header = block.headerPtr;
        if (header->type_id == BlockType::GlobalIndex)
            continue;

        if (Block::IsMagic(header))
            InitMagicBlock(block, method);

        BlockPayloadEncodingMethod encoding = BlockPayloadEncodingMethod::Raw;
        bool hasPayload = (header->payload_uncompressed_size > 0);
        if (hasPayload)
            encoding = method;

        if (!block.bFixedEncoding)
            header->encoding_method = encoding;
    }
}

void ContainerContext::RebuildDirectLookupTable()
{
    // Populate the main hash map from the discovered blocks.
    m_block_map.Clear();
    if (m_blocks.empty())
        return;
    if (m_blocks.size() > max_mpsize_t)
    {
        XASSERT_DIAG(false, "ContainerContext::RebuildDirectLookupTable - block count exceeds mpsize_t");
        return;
    }

    m_block_map.Reserve(to_mpsize(m_blocks.size()));
    for (BlockContext& block : m_blocks)
    {
        if (block.headerPtr)
            m_block_map.Insert(block.headerPtr->block_id, &block);
    }

    // Build the direct lookup table
    m_block_map.BuildLookupTable();
}

void ContainerContext::ClearCaches()
{
    ClearReadOnlyBlockPayloadCaches();
    UnloadForceLoadedBlockData();
    m_context->ClearCaches();
}

bool ContainerContext::SyncWithDataSource(uint32_t** out_removed_block_ids, mpsize_t* out_removed_block_count, bool* out_force_clear_caches)
{
    PerformBlockScan();

    if (m_blocks.size() > max_mpsize_t || m_removed_block_ids.size() > max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "ContainerContext::SyncWithDataSource - internal block count exceeds mpsize_t");
        *out_removed_block_ids = nullptr;
        *out_removed_block_count = 0;
        *out_force_clear_caches = true;
        return false;
    }

    // Clear data context caches if block IDs wrapped around and were reset and reused
    if (m_block_id_generation_changed)
    {
        ClearCaches();
    }

    *out_removed_block_ids = m_removed_block_ids.data();
    *out_removed_block_count = to_mpsize(m_removed_block_ids.size());
    *out_force_clear_caches = m_block_id_generation_changed;

    return true;
}

inline mpdatasz_t ContainerContext::ToFullHeaderLayoutOffset(mpdatasz_t physical_offset) const
{
    return m_magic.isFirstHeaderTruncated ? physical_offset + MagicPartSize : physical_offset;
}

inline mpdatasz_t ContainerContext::ToPhysicalLayoutOffset(mpdatasz_t full_layout_offset) const
{
    return m_magic.isFirstHeaderTruncated ? full_layout_offset - MagicPartSize : full_layout_offset;
}

inline bool ContainerContext::TryGetNextAlignedBlockOffset(
    mpdatasz_t payload_end_offset,
    mpdatasz_t& out_next_block_offset) const
{
    // Block alignment is defined as if the first header were stored in full. When its magic
    // prefix is omitted, physical file offsets are smaller by MagicPartSize.
    if (m_magic.isFirstHeaderTruncated && payload_end_offset > max_mpdatasz_t - MagicPartSize)
        return false;

    const mpdatasz_t full_layout_offset = ToFullHeaderLayoutOffset(payload_end_offset);
    if (full_layout_offset > max_mpdatasz_t - (kDataAlignment - 1))
        return false;

    const mpdatasz_t aligned_offset = align_up<kDataAlignment>(full_layout_offset);
    out_next_block_offset = ToPhysicalLayoutOffset(aligned_offset);
    return true;
}

inline BlockContext* ContainerContext::AllocateScanBlock(mpsize_t& scan_blocks_count)
{
    constexpr mpsize_t blocks_chunk_size = 8;
    if (scan_blocks_count == max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "ContainerContext::AllocateScanBlock - block count exceeds mpsize_t");
        return nullptr;
    }

    if (scan_blocks_count >= m_scan_blocks.size())
    {
        const mpdatasz_t desired_size = static_cast<mpdatasz_t>(scan_blocks_count) + blocks_chunk_size;
        m_scan_blocks.resize(static_cast<size_t>(
            ntd::min<mpdatasz_t>(desired_size, max_mpsize_t)));
    }
    return &m_scan_blocks[scan_blocks_count++];
}

inline bool ContainerContext::RegisterScannedBlock(
    BlockContext& ctx, mpdatasz_t orig_offset, bool is_first)
{
    BlockHeader* hdr = ctx.headerPtr;
    ctx.payloadOffsetOrig = orig_offset;
    if (hdr->type_id == BlockType::Data)
    {
        ctx.virtualStartOffset = Block::GetDataBlockVirtualStartOffsetFromMeta(hdr);
    }
    uint32_t blockId = hdr->block_id;
    m_context->RememberBlockInfo(blockId, orig_offset, hdr->payload_size);
    
    m_block_id_max = ntd::max(m_block_id_max, blockId);
    if (is_first)
    {
        m_block_id_min = blockId;
        m_magic_block_id = blockId;

        m_validation.container_version = static_cast<uint16_t>(hdr->type_id);
        m_validation.dataformat_minor_version = hdr->type_meta;
        m_validation.dataformat_major_version = static_cast<uint8_t>(hdr->metadata1 >> (7 * 8));
        m_validation.dataformat_signature = (hdr->metadata1 << 8) >> 8;
        m_validation.CheckMatches();

        if (!m_validation.IsMatched())
            return false;
    }
    else
    {
        m_block_id_min = ntd::min(m_block_id_min, blockId);
    }
    
    return true;
}

bool ContainerContext::ScanGlobalIndex(mpdatasz_t total_size, mpsize_t& scan_blocks_count)
{
    scan_blocks_count = 0;

    const bool raw_headers =
        m_magic.headerSerializationMethod == HeaderSerializationMethod::Raw;
    const mpsize_t headerSize = GetBlockHeaderDiskSize(raw_headers);
    const mpsize_t trailer_disk_size = GetBlockTrailerDiskSize(raw_headers);
    if (total_size < headerSize + trailer_disk_size)
        return false;

    // Jump to the trailer at the very end of the file
    const mpdatasz_t trailer_start_offset = total_size - trailer_disk_size;
    uint8_t* trailer_data_ptr = m_context->GetAbsDataPtr(trailer_start_offset, trailer_disk_size, nullptr);
    if (!trailer_data_ptr)
        return false;

    PayloadTrailer trailer{};
    if (raw_headers)
    {
        if (ntd::memcmp(trailer_data_ptr + sizeof(PayloadTrailer),
                PAYLOAD_TRAILER_GUID.data(), PAYLOAD_TRAILER_GUID.size()) != 0)
            return false;
        ntd::memcpy(&trailer, trailer_data_ptr, sizeof(trailer));
    }
    else
    {
        if (ntd::memcmp(trailer_data_ptr + sizeof(PayloadTrailer) * 2,
                PAYLOAD_TRAILER_GUID_HEX.data(), PAYLOAD_TRAILER_GUID_HEX.size()) != 0 ||
            !DeserializeFromHex(trailer_data_ptr, sizeof(PayloadTrailer) * 2, &trailer))
            return false;
    }

    // Locate and read the global index header.
    if (trailer.distance_between_blocks > total_size ||
        total_size - trailer.distance_between_blocks < headerSize)
        return false;
        
    mpdatasz_t payload_start_offset = total_size - trailer.distance_between_blocks;
    mpdatasz_t header_start_offset = payload_start_offset - headerSize;

    uint8_t* header_data_ptr = m_context->GetAbsDataPtr(header_start_offset, headerSize, nullptr);
    if (!header_data_ptr)
        return false;

    BlockHeader index_header{};
    if (raw_headers)
        ntd::memcpy(&index_header, header_data_ptr, sizeof(index_header));
    else if (!HeaderFromHex(index_header, header_data_ptr))
        return false;

    const bool payload_size_is_deferred =
        index_header.payload_size == BlockPayloadSizeDeferredMarker;
    if (index_header.type_id != BlockType::GlobalIndex ||
        (!payload_size_is_deferred &&
            index_header.payload_size != trailer.distance_between_blocks) ||
        trailer.distance_between_blocks < trailer_disk_size ||
        trailer.payload_actual_size > trailer.distance_between_blocks - trailer_disk_size)
        return false;
    if (!IsKnownPayloadEncoding(index_header.encoding_method) ||
        (index_header.encoding_method == BlockPayloadEncodingMethod::Raw &&
            index_header.payload_uncompressed_size !=
                (payload_size_is_deferred
                    ? trailer.payload_actual_size
                    : trailer.distance_between_blocks)))
        return false;
    if (index_header.payload_uncompressed_size > max_decoded_payload_size)
        return false;

    // Read or decode the global index entries. Raw indexes retain the zero-copy path when
    // input pointers are stable; encoded indexes and unstable inputs require one owned buffer.
    const bool index_is_encoded =
        index_header.encoding_method != BlockPayloadEncodingMethod::Raw;
    const mpsize_t encoded_data_size = trailer.payload_actual_size;
    const mpsize_t actual_data_size = index_is_encoded
        ? index_header.payload_uncompressed_size
        : encoded_data_size;
    using IndexOffset = decltype(BlockContext::payloadOffsetOrig);
    static_assert(sizeof(IndexOffset) == sizeof(mpdatasz_t),
        "Global-index offsets must use the absolute data-size type");
    const mpsize_t indexEntryDataSize = GlobalIndexEntrySize;
    if (actual_data_size < indexEntryDataSize || actual_data_size % indexEntryDataSize != 0)
        return false;

    uint8_t* index_payload_ptr = nullptr;

    if (!index_is_encoded && m_context->ArePointersStable())
    {
        index_payload_ptr = m_context->GetAbsDataPtr(
            payload_start_offset, actual_data_size, nullptr);
        if (index_payload_ptr && is_aligned<kDataAlignment>(index_payload_ptr))
            m_global_index_chunk.AssignMem(index_payload_ptr, actual_data_size);
        else
            index_payload_ptr = nullptr;
    }
    if (!index_is_encoded && !index_payload_ptr)
    {
        if (!m_global_index_chunk.Allocate(actual_data_size))
            return false;
        const mpsize_t bytes_read = m_context->ReadAbsDataToBuffer(
            payload_start_offset, actual_data_size,
            m_global_index_chunk.data(), actual_data_size);
        if (bytes_read != actual_data_size)
            return false;
        index_payload_ptr = m_global_index_chunk.data();
    }
    else if (index_is_encoded)
    {
        // The trailer's actual size excludes terminal padding and the trailer itself.
        // Present that exact payload range through a temporary block so the regular
        // lazy block-loading path performs the decode once.
        BlockHeader payload_header = index_header;
        payload_header.payload_size = encoded_data_size;
        payload_header.payload_uncompressed_size = actual_data_size;

        BlockContext index_block_view;
        index_block_view.AssignExternalHeader(&payload_header);
        index_block_view.payloadOffsetOrig = payload_start_offset;
        index_block_view.payloadCopyMode = BlockContext::PayloadCopyMode::Always;
        if (!GetPayloadDataPtrImpl(
                index_block_view, 0, actual_data_size, nullptr) ||
            index_block_view.payloadChunks.size() != 1)
            return false;

        m_global_index_chunk = ntd::move(index_block_view.payloadChunks.front());
        index_payload_ptr = m_global_index_chunk.data();
    }

    if (!index_payload_ptr)
        return false;

    // Register generic blocks
    mpsize_t offset = 0;
    bool isFirst = true;

#if ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY
    const mpsize_t indexEntryDataSizeMin = sizeof(BlockHeader) + sizeof(uint32_t);
    static_assert(align_up<kDataAlignment>(indexEntryDataSizeMin) == indexEntryDataSize,
        "Global-index u64 offsets must match the aligned minimum u32 offsets");
    const mpsize_t requiredLoopEntrySize = indexEntryDataSizeMin;
#else
    const mpsize_t requiredLoopEntrySize = indexEntryDataSize;
#endif

    mpdatasz_t expected_payload_offset_without_trailer =
        ToPhysicalLayoutOffset(headerSize);
    mpdatasz_t expected_payload_offset_with_trailer =
        expected_payload_offset_without_trailer;
    while (range_within(offset, requiredLoopEntrySize, actual_data_size))
    {
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(index_payload_ptr + offset);
        if (!IsKnownPayloadEncoding(hdr->encoding_method) ||
            (hdr->encoding_method == BlockPayloadEncodingMethod::Raw &&
                hdr->payload_size != hdr->payload_uncompressed_size))
            return false;
        if (hdr->payload_uncompressed_size > max_decoded_payload_size)
            return false;
        offset += sizeof(BlockHeader);

#if ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY
        // Version-1 indexes stored a uint32_t offset followed by four zero alignment bytes.
        // Large-file-capable indexes use those same eight bytes as a uint64_t offset.
        IndexOffset orig_offset = 0;
        const mpsize_t available_offset_bytes = actual_data_size - offset;
        const mpsize_t stored_offset_size = ntd::min<mpsize_t>(
            sizeof(orig_offset),
            available_offset_bytes);
        ntd::memcpy(&orig_offset, index_payload_ptr + offset, stored_offset_size);
        offset += stored_offset_size;
#else
        mpdatasz_t orig_offset = 0;
        ntd::memcpy(&orig_offset, index_payload_ptr + offset, sizeof(orig_offset));
        offset += sizeof(mpdatasz_t);
#endif

        // Even trusted indexes must describe one contiguous, bounded layout.
        const mpdatasz_t physical_orig_offset = ToPhysicalLayoutOffset(orig_offset);
        const bool is_truncated_header = isFirst && m_magic.isFirstHeaderTruncated;
        const mpsize_t physical_header_size = is_truncated_header
            ? headerSize - MagicPartSize
            : headerSize;
        if ((physical_orig_offset != expected_payload_offset_without_trailer &&
                physical_orig_offset != expected_payload_offset_with_trailer) ||
            physical_orig_offset < physical_header_size ||
            physical_orig_offset > header_start_offset ||
            hdr->payload_size > header_start_offset - physical_orig_offset)
            return false;

        // Indexed entries store the actual encoded payload size. Accept either
        // the compact layout or the larger region occupied by a streamed trailer.
        const mpdatasz_t payload_end = physical_orig_offset + hdr->payload_size;
        mpdatasz_t next_header_offset = 0;
        if (!TryGetNextAlignedBlockOffset(payload_end, next_header_offset) ||
            next_header_offset > max_mpdatasz_t - headerSize)
            return false;
        expected_payload_offset_without_trailer = next_header_offset + headerSize;

        mpsize_t trailer_region_size = 0;
        if (!TryGetTrailedPayloadRegionSize(
                hdr->payload_size, raw_headers, trailer_region_size))
            return false;
        if (physical_orig_offset > max_mpdatasz_t - trailer_region_size)
            return false;
        const mpdatasz_t trailer_next_header_offset =
            physical_orig_offset + trailer_region_size;
        if (trailer_next_header_offset > max_mpdatasz_t - headerSize)
            return false;
        expected_payload_offset_with_trailer =
            trailer_next_header_offset + headerSize;

        BlockContext* new_block_ctx_ptr = AllocateScanBlock(scan_blocks_count);
        if (!new_block_ctx_ptr)
            return false;
        BlockContext& new_block_ctx = *new_block_ctx_ptr;
        new_block_ctx.AssignExternalHeader(hdr);
        
        if (!RegisterScannedBlock(new_block_ctx, physical_orig_offset, isFirst))
            return false;
        
        isFirst = false;
        offset = align_up<kDataAlignment>(offset);
    }

    if (offset != actual_data_size || isFirst ||
        (payload_start_offset != expected_payload_offset_without_trailer &&
            payload_start_offset != expected_payload_offset_with_trailer))
        return false;
    
    // Register the global index block itself
    BlockContext* index_ctx_ptr = AllocateScanBlock(scan_blocks_count);
    if (!index_ctx_ptr)
        return false;
    BlockContext& index_ctx = *index_ctx_ptr;
    if (payload_size_is_deferred)
        index_header.payload_size = trailer.payload_actual_size;
    if (raw_headers && !payload_size_is_deferred &&
        m_context->AreSmallStructPointersStable())
    {
        index_ctx.AssignExternalHeader(reinterpret_cast<BlockHeader*>(header_data_ptr));
    }
    else
    {
        index_ctx.AllocateHeader();
        ntd::memcpy(index_ctx.headerPtr, &index_header, sizeof(BlockHeader));
    }

    if (!RegisterScannedBlock(index_ctx, payload_start_offset, isFirst))
        return false;
    
    m_global_index_block_id = index_ctx.headerPtr->block_id;
    return true;
}

bool ContainerContext::ScanSequential(
    mpdatasz_t total_size, bool isRawHeaders, mpsize_t& scan_blocks_count)
{
    bool scanSuccess = true;
    scan_blocks_count = 0;

    const mpsize_t headerSize = GetBlockHeaderDiskSize(isRawHeaders);
    mpdatasz_t current_offset = 0;
    while (current_offset < total_size)
    {
        const bool isFirstBlock = (current_offset == 0);
        const bool isCurHeaderTruncated = (m_magic.isFirstHeaderTruncated && isFirstBlock);
        const mpsize_t curHeaderSize = isCurHeaderTruncated ? headerSize - MagicPartSize : headerSize;

        if (current_offset > total_size || curHeaderSize > total_size - current_offset)
            break;

        uint8_t* header_data_ptr = m_context->GetNextAbsDataPtr(current_offset, curHeaderSize, nullptr);
        if (!header_data_ptr)
            break;

        BlockContext* new_block_ctx_ptr = AllocateScanBlock(scan_blocks_count);
        if (!new_block_ctx_ptr)
        {
            scanSuccess = false;
            break;
        }
        BlockContext& new_block_ctx = *new_block_ctx_ptr;

        // 1. Parse the header first to inspect it.
        bool isPayloadSizeDeferred = false;
        if (isRawHeaders)
        {
            BlockHeader* hdr = reinterpret_cast<BlockHeader*>(header_data_ptr);
            // Decide if we MUST own the header. We must own it if pointers are not stable,
            // or if we need to modify it to insert the extended payload size information.
            isPayloadSizeDeferred = (hdr->payload_size == BlockPayloadSizeDeferredMarker);
            const bool must_own_header = !m_context->AreSmallStructPointersStable() || isPayloadSizeDeferred;
            if (must_own_header)
            {
                new_block_ctx.AllocateHeader();
                ntd::memcpy(new_block_ctx.headerPtr, header_data_ptr, sizeof(BlockHeader));
            }
            else
            {
                new_block_ctx.AssignExternalHeader(hdr);
            }
        }
        else
        {
            new_block_ctx.AllocateHeader();
            if (!HeaderFromHex(*new_block_ctx.headerPtr, header_data_ptr, isFirstBlock, isCurHeaderTruncated))
            {
                // Reject a block with a malformed hex header.
                scan_blocks_count--;
                break;
            }
            isPayloadSizeDeferred = (new_block_ctx.headerPtr->payload_size == BlockPayloadSizeDeferredMarker);
        }

        BlockHeader* header = new_block_ctx.headerPtr;

        if (!IsKnownPayloadEncoding(header->encoding_method) ||
            (header->encoding_method == BlockPayloadEncodingMethod::Raw &&
                !isPayloadSizeDeferred &&
                header->payload_size != header->payload_uncompressed_size))
        {
            scan_blocks_count--;
            break;
        }

        current_offset += curHeaderSize;
        const mpdatasz_t payload_abs_offset = current_offset;
        mpdatasz_t distance_between_blocks = header->payload_size;

        // 2. Check for and handle the extended size trailer
        if (isPayloadSizeDeferred)
        {
            // The size on disk is different for raw vs. hex
            const mpsize_t trailer_disk_size = GetBlockTrailerDiskSize(isRawHeaders);
            
            bool guid_found = false;
            if (payload_abs_offset > total_size ||
                PAYLOAD_TRAILER_SEARCH_STEP > total_size - payload_abs_offset)
            {
                scan_blocks_count--;
                break;
            }
            mpdatasz_t search_region_end = payload_abs_offset + PAYLOAD_TRAILER_SEARCH_STEP;

            while (search_region_end <= total_size)
            {
                const mpdatasz_t trailer_start_offset = search_region_end - trailer_disk_size;

                uint8_t* trailer_data_ptr = m_context->GetNextAbsDataPtr(trailer_start_offset, trailer_disk_size, nullptr);
                if (!trailer_data_ptr)
                    break;

                bool current_guid_matches = false;
                if (isRawHeaders)
                {
                    // Raw comparison: Compare binary GUID
                    if (ntd::memcmp(trailer_data_ptr + sizeof(PayloadTrailer), PAYLOAD_TRAILER_GUID.data(), sizeof(PAYLOAD_TRAILER_GUID)) == 0)
                        current_guid_matches = true;
                }
                else
                {
                    // Hex comparison: Compare raw hex text against our pre-computed hex GUID
                    if (ntd::memcmp(trailer_data_ptr + sizeof(PayloadTrailer) * 2, PAYLOAD_TRAILER_GUID_HEX.data(), sizeof(PAYLOAD_TRAILER_GUID_HEX)) == 0)
                        current_guid_matches = true;
                }

                if (current_guid_matches)
                {
                    PayloadTrailer trailer{};
                    if (isRawHeaders)
                    {
                        ntd::memcpy(&trailer, trailer_data_ptr, sizeof(trailer));
                    }
                    else
                    {
                        if (!DeserializeFromHex(trailer_data_ptr, sizeof(PayloadTrailer) * 2, &trailer))
                            break;
                    }

                    const mpdatasz_t candidate_distance =
                        search_region_end - payload_abs_offset;
                    const bool trailer_is_consistent =
                        trailer.distance_between_blocks == candidate_distance &&
                        trailer.distance_between_blocks >= trailer_disk_size &&
                        trailer.payload_actual_size <=
                            trailer.distance_between_blocks - trailer_disk_size;
                    const bool raw_size_matches =
                        header->encoding_method != BlockPayloadEncodingMethod::Raw ||
                        header->payload_uncompressed_size == trailer.payload_actual_size;
                    if (trailer_is_consistent && raw_size_matches)
                    {
                        header->payload_size = trailer.payload_actual_size;
                        distance_between_blocks = trailer.distance_between_blocks;
                        guid_found = true;
                        break;
                    }
                }

                if (total_size - search_region_end < PAYLOAD_TRAILER_SEARCH_STEP)
                    break;
                search_region_end += PAYLOAD_TRAILER_SEARCH_STEP;
            }

            if (!guid_found)
            {
                scan_blocks_count--;
                break;
            }
        }

        if (header->payload_uncompressed_size > max_decoded_payload_size)
        {
            XASSERT_DIAG(false,
                "ContainerContext::ScanSequential - decoded payload exceeds the per-block limit");
            scan_blocks_count--;
            break;
        }

        // 3. Prepare for the next block
        if (current_offset > total_size || distance_between_blocks > total_size - current_offset)
        {
            scan_blocks_count--;
            break;
        }
        
        if (!RegisterScannedBlock(new_block_ctx, payload_abs_offset, isFirstBlock))
        {
            scanSuccess = false;
            break;
        }

        current_offset += distance_between_blocks;
        mpdatasz_t next_block_offset = 0;
        if (!TryGetNextAlignedBlockOffset(current_offset, next_block_offset))
            break;
        current_offset = next_block_offset;

        if (header->type_id == BlockType::GlobalIndex)
            break;
        if (header->type_id == BlockType::Terminator)
            break;
    }

    return scanSuccess;
}

void ContainerContext::PerformBlockScan()
{
    if (m_blocks.size() > max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "ContainerContext::PerformBlockScan - previous block count exceeds mpsize_t");
        m_validation = {};
        return;
    }

    mpsize_t prev_blocks_count = to_mpsize(m_blocks.size());
    // A previous scan may have returned before its normal cleanup path.
    // Retain capacity, but never combine snapshots from different reload attempts.
    m_block_fingerprints.clear();

    bool hasGlobalIndex = HasGlobalIndex();
    bool hasOldBlocks = (prev_blocks_count > 0);
    if (hasOldBlocks)
    {
        // The scan was already performed earlier
        m_context->SyncWithDataSource();

        if (m_scan_blocks.empty())
            m_scan_blocks.resize(prev_blocks_count);

        m_block_fingerprints.reserve(prev_blocks_count);
        for (auto& block : m_blocks)
        {
            m_block_fingerprints.push_back({
                block.headerPtr->block_id,
                block.headerPtr->payload_size,
                block.headerPtr->payload_uncompressed_size,
                block.virtualStartOffset
            });
        }
    }

    uint32_t old_magic_block_id = m_magic_block_id;
    auto reset_scan_results = [&]()
    {
        m_context->ForgetAllBlockInfo();
        m_context->ReserveBlockInfo(prev_blocks_count);
        m_context->ResetScanState();
        m_validation = {};
        m_block_id_generation_changed = false;
        m_magic_block_id = 0;
        m_global_index_block_id = 0;
        m_block_id_min = 0;
        m_block_id_max = 0;
    };
    reset_scan_results();

    const mpdatasz_t total_size = m_context->GetTotalDataSize();
    if (total_size < BlockHeaderMetadata0Size)
        return;

    uint8_t* magic_bytes = m_context->GetAbsDataPtr(0, BlockHeaderMetadata0Size, nullptr);
    if (!magic_bytes)
        return;

    m_magic = MagicParseData(magic_bytes);
    if (!m_magic.isOk)
        return;

    m_validation.container_magic_match = true;

    const bool isRawHeaders = (m_magic.headerSerializationMethod == HeaderSerializationMethod::Raw);
    mpsize_t scan_blocks_count = 0;

    // --- PATH 1: Fast scan ---
    bool scan_success = false;
    const bool allow_fast_scan = IsInputTrusted() &&
        (hasGlobalIndex || !hasOldBlocks);
    if (allow_fast_scan)
    {
        scan_success = ScanGlobalIndex(total_size, scan_blocks_count);
    }

    // --- PATH 2: Normal scan ---
    if (!scan_success)
    {
        if (allow_fast_scan)
        {
            // A rejected index may already have registered a valid-looking prefix.
            // Sequential scanning must rebuild every derived lookup and ID bound from scratch.
            reset_scan_results();
            m_validation.container_magic_match = true;
        }
        scan_success = ScanSequential(total_size, isRawHeaders, scan_blocks_count);
    }

    m_validation.is_valid = scan_success && m_validation.IsMatched();

    // Finalize the preparation of the new block index
    using BlockDifference = ntd::deque<BlockContext>::difference_type;
    auto first_unused_scan_block = m_scan_blocks.begin()
        + static_cast<BlockDifference>(scan_blocks_count);
    m_scan_blocks.erase(first_unused_scan_block, m_scan_blocks.end());
    ntd::swap(m_blocks, m_scan_blocks);

    m_context->ProcessBlockInfo();
    RebuildDirectLookupTable();

    // Create a list of removed blocks + reset the entries in m_scan_blocks
    m_block_id_generation_changed = (old_magic_block_id != m_magic_block_id);
    m_removed_block_ids.clear();

    if (!m_block_id_generation_changed)
        m_removed_block_ids.reserve(m_scan_blocks.size());

    // After the swap, m_scan_blocks contains the old m_blocks in the same order used
    // to capture m_block_fingerprints before the scan.
    auto old_block_it = m_scan_blocks.begin();
    for (const auto& fp : m_block_fingerprints)
    {
        BlockContext* new_block = GetBlock(fp.block_id);
        const bool payload_cache_matches = new_block &&
            new_block->headerPtr->payload_uncompressed_size == fp.payload_uncompressed_size &&
            new_block->virtualStartOffset == fp.virtualStartOffset;
        if (payload_cache_matches)
        {
            new_block->payloadChunks = ntd::move(old_block_it->payloadChunks);
            new_block->bForcedPersistance = old_block_it->bForcedPersistance;
        }

        if (!m_block_id_generation_changed)
        {
            const bool wasAppended = new_block && !payload_cache_matches;
            if (wasAppended)
            {
                m_context->ClearBlockCache(fp.block_id, fp.payload_size);
            }
            if (!new_block || wasAppended)
            {
                m_removed_block_ids.push_back(fp.block_id);
            }
        }
        ++old_block_it;
    }    
    m_block_fingerprints.clear();

    for (auto& old_block : m_scan_blocks)
    {
        old_block.ResetSoft();
    }
}

void ContainerContext::Init()
{
    // Little-endian specific:
    // * GPRX::Locator
    // * GPRX::Block::GetMeta1View
    XASSERT_TRAP(GPRX::is_little_endian());
    m_block_map.LimitDirectLookupSizeByMaxId(false);
}

ContainerContext::ContainerContext()
{
    Init();
}

ContainerContext::ContainerContext(IDataContext& context)
    : ContainerContext(context, InputTrust::Untrusted)
{
}

ContainerContext::ContainerContext(IDataContext& context, InputTrust input_trust)
    : ILockable(&context), m_context(&context), m_input_trust(input_trust)
{
    Init();
    PerformBlockScan();
}

bool ContainerContext::ProcessBlocksForFlattening(
    ntd::vector<uint8_t*>& out_ptrs,
    ntd::vector<mpsize_t>& out_sizes,
    const ntd::function<bool(uint32_t block_id)>& callback)
{
    if (out_ptrs.size() != out_sizes.size() || out_ptrs.size() > max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "ContainerContext::ProcessBlocksForFlattening - invalid output vector state");
        return false;
    }

    for (const auto& block : m_blocks)
    {
        if (!block.headerPtr)
            continue;
        if (block.bDontSave)
            continue;

        const mpsize_t block_chunks_begin = to_mpsize(out_ptrs.size());
        auto discard_block_chunks = [&]()
        {
            out_ptrs.resize(block_chunks_begin);
            out_sizes.resize(block_chunks_begin);
        };
        auto has_output_slot = [&]()
        {
            if (out_ptrs.size() >= max_mpsize_t)
            {
                XASSERT_DIAG(false,
                    "ContainerContext::ProcessBlocksForFlattening - chunk count exceeds mpsize_t");
                return false;
            }
            return true;
        };

        if (!has_output_slot())
            return false;

        // Add the Header chunk.
        out_ptrs.push_back(reinterpret_cast<uint8_t*>(block.headerPtr));
        out_sizes.push_back(sizeof(BlockHeader));

        // Add the payload chunks and calculate their total size.
        const MemChunkList& target_chunks = block.payloadChunks;
        const mpsize_t target_size = block.headerPtr->payload_uncompressed_size;
        bool chunk_count_overflow = false;

        if (!ProcessChunkSequence(target_chunks, target_size,
            [&](const uint8_t* chunk_data, mpsize_t size_from_chunk) -> bool
            {
                if (!has_output_slot())
                {
                    chunk_count_overflow = true;
                    return false;
                }
                out_ptrs.push_back(const_cast<uint8_t*>(chunk_data));
                out_sizes.push_back(size_from_chunk);
                return true;
            }
        ))
        {
            discard_block_chunks();
            if (chunk_count_overflow)
                return false;
            continue;
        }

        // Add a pointer to a pre-allocated static padding chunk if necessary.
        mpsize_t remainder = target_size % kDataAlignment;
        if (remainder != 0)
        {
            if (!has_output_slot())
            {
                discard_block_chunks();
                return false;
            }
            mpsize_t padding_size = kDataAlignment - remainder;
            out_ptrs.push_back(const_cast<uint8_t*>(s_zero_padding_buffer.data()));
            out_sizes.push_back(padding_size);
        }

        if (!callback(block.headerPtr->block_id))
        {
            discard_block_chunks();
            return false;
        }
    }

    return true;
}

void ContainerContext::GetAllDataChunks(
    uint8_t** &out_chunk_ptrs, 
    mpsize_t* &out_chunk_sizes, 
    mpsize_t &out_num_chunks)
{
    m_flat_chunk_ptrs.clear();
    m_flat_chunk_sizes.clear();

    const bool success = ProcessBlocksForFlattening(
        m_flat_chunk_ptrs,
        m_flat_chunk_sizes,
        [](uint32_t /*block_id*/) { return true; });
    if (!success)
    {
        m_flat_chunk_ptrs.clear();
        m_flat_chunk_sizes.clear();
        out_chunk_ptrs = nullptr;
        out_chunk_sizes = nullptr;
        out_num_chunks = 0;
        return;
    }
    
    out_chunk_ptrs = m_flat_chunk_ptrs.data();
    out_chunk_sizes = m_flat_chunk_sizes.data();
    out_num_chunks = to_mpsize(m_flat_chunk_ptrs.size());
}

ntd::unique_ptr<IStreamProcessor> ContainerContext::CreateDecompressorForBlock(const BlockContext& block, uint8_t* destination_buffer, mpsize_t destination_capacity)
{
    if (!block.headerPtr)
        return nullptr;

    switch (block.headerPtr->encoding_method)
    {
        case BlockPayloadEncodingMethod::Raw:
        {
            auto proc = ntd::make_unique<PassThroughProcessor>();
            proc->SetMemoryOutput(destination_buffer, destination_capacity);
            return proc;
        }
        case BlockPayloadEncodingMethod::LZ4:
        {
            auto proc = ntd::make_unique<LZ4Processor>(IStreamProcessor::Direction::Decode);
            proc->SetMemoryOutput(destination_buffer, destination_capacity);
            return proc;
        }
        case BlockPayloadEncodingMethod::Base64:
        {
            auto proc = ntd::make_unique<Base64Processor>(IStreamProcessor::Direction::Decode);
            proc->SetMemoryOutput(destination_buffer, destination_capacity);
            return proc;
        }
        case BlockPayloadEncodingMethod::LZ4_Base64:
        {
            auto stage2 = ntd::make_unique<LZ4Processor>(IStreamProcessor::Direction::Decode);
            stage2->SetMemoryOutput(destination_buffer, destination_capacity);
            auto res = ntd::make_unique<Base64Processor>(IStreamProcessor::Direction::Decode, stage2.release());
            res->SetOwnsNextStage(true);
            return res;
        }
    }

    return nullptr;
}

uint8_t* ContainerContext::StitchChunks(BlockContext& block, mpsize_t start_chunk_idx, mpsize_t offset_in_start_chunk, mpsize_t end_chunk_idx, mpsize_t data_size, mpsize_t* out_ready_to_access_len)
{
    auto& chunks = block.payloadChunks;

    // --- Optimization Check: Are the involved chunks physically contiguous? ---
    bool is_contiguous = true;
    mpsize_t contiguous_size = chunks[start_chunk_idx].size();
    // bForceZero chunks are virtually contiguous but physically disassociated
    if (chunks[start_chunk_idx].bForceZero)
        is_contiguous = false;
    if (is_contiguous)
    {
        for (mpsize_t i = start_chunk_idx + 1; i <= end_chunk_idx; ++i)
        {
            // 1. Check if the NEXT chunk is a force-zero chunk
            // 2. Check if the NEXT chunk is actually a non-owning view
            // 3. Check if the NEXT chunk's pointer is exactly where it should be in case of contiguous chunks
            if (chunks[i].bForceZero ||
                chunks[i].allocatedBytes != 0 ||
                chunks[i].data() != chunks[i-1].data() + chunks[i-1].totalBytes)
            {
                is_contiguous = false;
                break;
            }
            contiguous_size += chunks[i].size();
        }
    }

    if (is_contiguous)
    {
        // --- FAST PATH: Return direct pointer ---
        if (offset_in_start_chunk + data_size > contiguous_size)
            return nullptr;
        uint8_t* direct_ptr = chunks[start_chunk_idx].data() + offset_in_start_chunk;
        if (out_ready_to_access_len)
            *out_ready_to_access_len = contiguous_size - offset_in_start_chunk;
        return direct_ptr;
    }

    // --- REGULAR PATH: Copy/Stitch into temporary buffer ---
    auto& active_cross_chunk_buffer = m_cross_chunk_buffers[GetActiveLockLevel()];
    resize_if_needed(active_cross_chunk_buffer, data_size);
    mpsize_t bytes_copied = 0;
    for (mpsize_t p_num = start_chunk_idx; p_num <= end_chunk_idx; ++p_num)
    {
        const auto& chunk = chunks[p_num];
        const mpsize_t source_offset = (p_num == start_chunk_idx) ? offset_in_start_chunk : 0;
        const mpsize_t bytes_available = chunk.size() - source_offset;
        const mpsize_t bytes_required = data_size - bytes_copied;
        const mpsize_t bytes_to_copy = ntd::min(bytes_available, bytes_required);
        if (bytes_to_copy == 0)
        {
            if (bytes_required > 0)
                return nullptr;
            break;
        }
        if (chunk.bForceZero)
            ntd::memset(active_cross_chunk_buffer.data() + bytes_copied, 0, bytes_to_copy);
        else
            ntd::memcpy(active_cross_chunk_buffer.data() + bytes_copied, chunk.data() + source_offset, bytes_to_copy);
        bytes_copied += bytes_to_copy;
    }

    if (out_ready_to_access_len)
        *out_ready_to_access_len = data_size;
    return active_cross_chunk_buffer.data();
}

inline uint8_t* ContainerContext::GetPayloadDataPtrImpl(BlockContext& block, mpsize_t relative_offset, mpsize_t data_size, mpsize_t* out_ready_to_access_len)
{
    // PATH 1: Newly created Block
    if (block.bNewWritableBlock)
    {
        auto& chunks = block.payloadChunks;
        mpsize_t start_chunk_idx = 0;
        mpsize_t offset_in_start_chunk = 0;
        mpsize_t end_chunk_idx = 0;

        if (!block.bUsesExternalPayloadChunks)
        {
            start_chunk_idx = relative_offset / PAYLOAD_ALLOC_CHUNK_SIZE;
            offset_in_start_chunk = relative_offset % PAYLOAD_ALLOC_CHUNK_SIZE;
            end_chunk_idx = (relative_offset + data_size - 1) / PAYLOAD_ALLOC_CHUNK_SIZE;
        }
        else
        {
            // External chunks may have arbitrary sizes. Locate the request by logical
            // byte position instead of applying the owned-page indexing shortcut.
            mpsize_t chunk_start = 0;
            bool start_found = false;
            mpsize_t covered = 0;
            for (mpsize_t i = 0; i < chunks.size(); ++i)
            {
                const mpsize_t chunk_size = chunks[i].size();
                if (!start_found && relative_offset >= chunk_start &&
                    relative_offset - chunk_start < chunk_size)
                {
                    start_found = true;
                    start_chunk_idx = i;
                    end_chunk_idx = i;
                    offset_in_start_chunk = relative_offset - chunk_start;
                    covered = chunk_size - offset_in_start_chunk;
                }
                else if (start_found)
                {
                    end_chunk_idx = i;
                    covered += chunk_size;
                }

                if (start_found && covered >= data_size)
                    break;
                chunk_start += chunk_size;
            }
            if (!start_found || covered < data_size)
                return nullptr;
        }
        
        if (end_chunk_idx >= chunks.size())
            return nullptr;

        // Subcase A: The entire request fits within a single fixed-size chunk.
        if (start_chunk_idx == end_chunk_idx)
        {
            auto& chunk = chunks[start_chunk_idx];
            if (offset_in_start_chunk + data_size > chunk.size())
                return nullptr;

            if (out_ready_to_access_len)
                *out_ready_to_access_len = chunk.size() - offset_in_start_chunk;
            return chunk.data() + offset_in_start_chunk;
        }
        // Subcase B: The data is split across multiple chunks and may require copying.
        else
        {
            return StitchChunks(block, start_chunk_idx, offset_in_start_chunk, end_chunk_idx, data_size, out_ready_to_access_len);
        }
    }
    // PATH 2: Decompression/copying is required OR direct read
    else if (block.payloadChunks.empty())
    {
        bool should_force_copy =
            (bBlockPersistanceRequired && !m_lock_ignore_counter) ||
            (block.payloadCopyMode == BlockContext::PayloadCopyMode::Always) ||
            (block.payloadCopyMode == BlockContext::PayloadCopyMode::ForNonStablePointerDataContext && !m_context->ArePointersStable());
        const bool needs_full_copy = (block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw) || should_force_copy;
        // Subcase A: Decompression/copying is required
        if (needs_full_copy)
        {
            block.bForcedPersistance = bBlockPersistanceRequired;

            if (block.headerPtr->payload_uncompressed_size > max_decoded_payload_size)
            {
                XASSERT_DIAG(false,
                    "ContainerContext::GetPayloadDataPtrImpl - decoded payload exceeds the per-block limit");
                return nullptr;
            }

            IDataContext& data_context = GetDataContext();
            MemChunk uncompressed_payload;
            if (!uncompressed_payload.Allocate(block.headerPtr->payload_uncompressed_size))
                return nullptr;
            
            // Direct read into uncompressed_payload
            if (block.headerPtr->encoding_method == BlockPayloadEncodingMethod::Raw)
            {
                mpsize_t source_bytes_read = 0;
                const mpsize_t source_total_size = block.headerPtr->payload_size;

                while (source_bytes_read < source_total_size)
                {
                    mpsize_t bytes_left = source_total_size - source_bytes_read;
                    mpsize_t bytes_to_read = ntd::min(PAYLOAD_READ_CHUNK_SIZE, bytes_left);

                    data_context.PushIgnoreLock();
                    // Avoid block-level payload page caching (if enabled) by reading from absolute offsets
                    mpsize_t actual_read = data_context.ReadAbsDataToBuffer(
                        block.payloadOffsetOrig + source_bytes_read,
                        bytes_to_read,
                        uncompressed_payload.data() + source_bytes_read,
                        bytes_to_read
                    );
                    data_context.PopIgnoreLock();

                    if (actual_read != bytes_to_read)
                        return nullptr;
                    source_bytes_read += actual_read;
                }
            }
            // Decompress/decode into uncompressed_payload
            else
            {
                auto decompressor = CreateDecompressorForBlock(block, uncompressed_payload.data(), uncompressed_payload.size());
                if (!decompressor)
                    return nullptr;

                decompressor->BeginStream(true);

                mpsize_t source_bytes_read = 0;
                const mpsize_t source_total_size = block.headerPtr->payload_size;

                while (source_bytes_read < source_total_size)
                {
                    mpsize_t bytes_left = source_total_size - source_bytes_read;
                    mpsize_t bytes_to_read = ntd::min(PAYLOAD_READ_CHUNK_SIZE, bytes_left);
                    
                    mpsize_t bytes_ready_to_access = 0;
                    data_context.PushIgnoreLock();
                    // Avoid block-level payload page caching (if enabled) by reading from absolute offsets
                    const uint8_t* read_buffer_data = data_context.GetAbsDataPtr(
                        block.payloadOffsetOrig + source_bytes_read,
                        bytes_to_read,
                        &bytes_ready_to_access
                    );
                    data_context.PopIgnoreLock();
                    if (!read_buffer_data)
                        return nullptr;

                    mpsize_t actual_bytes_read = ntd::min(bytes_ready_to_access, bytes_left);
                    if (actual_bytes_read == 0 && bytes_to_read > 0)
                        return nullptr;
                    if (!decompressor->ProcessChunk(read_buffer_data, actual_bytes_read))
                        return nullptr;
                    source_bytes_read += actual_bytes_read;
                }

                if (!decompressor->EndStream(true))
                    return nullptr;
                if (decompressor->GetBytesWritten() !=
                    block.headerPtr->payload_uncompressed_size)
                {
                    XASSERT_DIAG(false,
                        "ContainerContext::GetPayloadDataPtrImpl - decoded payload size does not match header");
                    return nullptr;
                }
            }

            block.payloadChunks.emplace_back(ntd::move(uncompressed_payload));
            // Read it below
        }
        // Subcase B: Direct read
        else
        {
            IDataContext& data_context = GetDataContext();
            if (data_context.IsBlockFetchSupported())
                return data_context.GetBlockDataPtr(block.headerPtr->block_id, relative_offset, data_size, out_ready_to_access_len);
            else
                return data_context.GetAbsDataPtr(block.payloadOffsetOrig + relative_offset, data_size, out_ready_to_access_len);
        }
    }

    // !block.payloadChunks.empty()
    // Uncompressed/copied payload in a single mem chunk OR payload is in externally assigned mem chunks
    auto& chunks = block.payloadChunks;
    mpsize_t running_offset = 0;

    for (mpsize_t i = 0; i < chunks.size(); ++i)
    {
        auto& chunk = chunks[i];
        
        if (relative_offset < running_offset + chunk.size())
        {
            const mpsize_t offset_in_chunk = relative_offset - running_offset;
            const mpsize_t available_in_chunk = chunk.size() - offset_in_chunk;

            // Case A: The entire request fits within this single chunk.
            if (data_size <= available_in_chunk)
            {
                if (chunk.bForceZero)
                {
                    // If requested zeroes fit in our shared block, return that pointer directly
                    if (data_size <= GLOBAL_ZERO_BUFFER_LEN)
                    {
                        if (out_ready_to_access_len)
                            *out_ready_to_access_len = ntd::min(available_in_chunk, GLOBAL_ZERO_BUFFER_LEN);
                        return const_cast<uint8_t*>(kGlobalZeroBuffer);
                    }
                    else
                    {
                        // Needs to return a contiguous pointer larger than our static pool. Build it.
                        auto& active_cross_chunk_buffer = m_cross_chunk_buffers[GetActiveLockLevel()];
                        resize_if_needed(active_cross_chunk_buffer, data_size);
                        ntd::memset(active_cross_chunk_buffer.data(), 0, data_size);
                        if (out_ready_to_access_len)
                            *out_ready_to_access_len = data_size;
                        return active_cross_chunk_buffer.data();
                    }
                }
                else
                {
                    // Standard zero-copy direct pointer return
                    if (out_ready_to_access_len)
                        *out_ready_to_access_len = available_in_chunk;
                    return chunk.data() + offset_in_chunk;
                }
            }
            // Case B: The data is split across this and subsequent chunks.
            else
            {
                return StitchChunks(
                    block,
                    i,
                    offset_in_chunk,
                    to_mpsize(chunks.size() - 1),
                    data_size,
                    out_ready_to_access_len);
            }
        }

        running_offset += chunk.size();
    }

    return nullptr;
}

uint8_t* ContainerContext::GetPayloadDataPtr(BlockContext& block, mpsize_t relative_offset, mpsize_t data_size, mpsize_t* out_ready_to_access_len)
{
    if (!block.headerPtr || data_size == 0)
    {
        if (out_ready_to_access_len)
            *out_ready_to_access_len = 0;
        DeactivateLock();
        return nullptr;
    }

    if (relative_offset < block.virtualStartOffset)
    {
        DeactivateLock();
        return nullptr;
    }

    relative_offset -= block.virtualStartOffset;

    if (!range_within(relative_offset, data_size, block.headerPtr->payload_uncompressed_size))
    {
        DeactivateLock();
        return nullptr;
    }

    // The validated range makes this subtraction safe.
    mpsize_t max_data_size = block.headerPtr->payload_uncompressed_size - relative_offset;
    uint8_t* res = GetPayloadDataPtrImpl(block, relative_offset, data_size, out_ready_to_access_len);
    DeactivateLock();

    if (res && out_ready_to_access_len && *out_ready_to_access_len > max_data_size)
        *out_ready_to_access_len = max_data_size;
    return res;
}

bool ContainerContext::ReadPayloadDataToBuffer(BlockContext& block, mpsize_t relative_offset, mpsize_t data_size, uint8_t* buf, mpsize_t buf_size)
{
    if ((!buf && data_size > 0) || data_size > buf_size || !block.headerPtr)
        return false;
    if (data_size == 0)
        return relative_offset >= block.virtualStartOffset &&
            relative_offset - block.virtualStartOffset <= block.headerPtr->payload_uncompressed_size;

    bool res = false;
    PushIgnoreLock();
    if (block.bNewWritableBlock || block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw || !block.payloadChunks.empty())
    {
        uint8_t* ptr = GetPayloadDataPtr(block, relative_offset, data_size, nullptr);
        if (ptr)
        {
            if (data_size > 0)
                ntd::memcpy(buf, ptr, data_size);
            res = true;
        }
    }
    else if (relative_offset >= block.virtualStartOffset)
    {
        relative_offset -= block.virtualStartOffset;
        
        IDataContext& data_context = GetDataContext();
        mpsize_t bytes_read = data_context.IsBlockFetchSupported()
            ? data_context.ReadBlockDataToBuffer(block.headerPtr->block_id, relative_offset, data_size, buf, buf_size)
            : data_context.ReadAbsDataToBuffer(block.payloadOffsetOrig + relative_offset, data_size, buf, buf_size);
        res = (bytes_read == data_size);
    }
    PopIgnoreLock();
    return res;
}

bool ContainerContext::AppendPayloadData(BlockContext& block, const uint8_t* data, mpsize_t size)
{
    if (!block.bNewWritableBlock || !block.headerPtr)
        return false; // Can only append to new blocks.
    if (size == 0)
        return true;
    if (block.bUsesExternalPayloadChunks)
        return false;
    if (size > max_mpsize_t - block.headerPtr->payload_uncompressed_size)
        return false;

    // Work out every allocation before modifying the existing payload.  This keeps
    // allocation failure from leaving a partially-appended block.
    mpsize_t first_copy_size = 0;
    if (!block.payloadChunks.empty())
    {
        auto& last_chunk = block.payloadChunks.back();
        if (!last_chunk.ptr || last_chunk.capacity() != PAYLOAD_ALLOC_CHUNK_SIZE ||
            last_chunk.size() > last_chunk.capacity())
            return false;
        first_copy_size = ntd::min(size, last_chunk.capacity() - last_chunk.size());
    }

    const mpsize_t bytes_to_copy = size - first_copy_size;
    MemChunk new_big_chunk;
    mpsize_t num_virtual_chunks = 0;
    if (bytes_to_copy > 0)
    {
        if (bytes_to_copy > max_mpsize_t - (PAYLOAD_ALLOC_CHUNK_SIZE - 1))
            return false;
        num_virtual_chunks = (bytes_to_copy + PAYLOAD_ALLOC_CHUNK_SIZE - 1) / PAYLOAD_ALLOC_CHUNK_SIZE;
        if (num_virtual_chunks > max_mpsize_t / PAYLOAD_ALLOC_CHUNK_SIZE)
            return false;
        const mpsize_t total_alloc_size = num_virtual_chunks * PAYLOAD_ALLOC_CHUNK_SIZE;
        if (!new_big_chunk.Allocate(total_alloc_size))
            return false;
        new_big_chunk.totalBytes = PAYLOAD_ALLOC_CHUNK_SIZE;
        new_big_chunk.usedBytes = ntd::min(bytes_to_copy, PAYLOAD_ALLOC_CHUNK_SIZE);

        const uint8_t* remaining_data = data ? data + first_copy_size : nullptr;
        if (remaining_data)
            ntd::memcpy(new_big_chunk.data(), remaining_data, bytes_to_copy);
        else
            ntd::memset(new_big_chunk.data(), 0, bytes_to_copy);
    }

    if (first_copy_size > 0)
    {
        auto& last_chunk = block.payloadChunks.back();
        const mpsize_t old_size = last_chunk.size();
        last_chunk.usedBytes += first_copy_size;
        if (data)
            ntd::memcpy(last_chunk.data() + old_size, data, first_copy_size);
        else
            ntd::memset(last_chunk.data() + old_size, 0, first_copy_size);
    }

    if (bytes_to_copy > 0)
    {
        uint8_t* const allocation = new_big_chunk.data();
        block.payloadChunks.emplace_back(ntd::move(new_big_chunk));
        mpsize_t bytes_covered_by_chunks = block.payloadChunks.back().usedBytes;
        for (mpsize_t i = 1; i < num_virtual_chunks; ++i)
        {
            mpsize_t chunk_used = ntd::min(bytes_to_copy - bytes_covered_by_chunks, PAYLOAD_ALLOC_CHUNK_SIZE);
            block.payloadChunks.emplace_back();
            auto& new_virtual_chunk = block.payloadChunks.back();
            new_virtual_chunk.AssignMemUnsafe(
                allocation + (i * PAYLOAD_ALLOC_CHUNK_SIZE), chunk_used, PAYLOAD_ALLOC_CHUNK_SIZE);

            bytes_covered_by_chunks += chunk_used;
        }
    }

    block.encodedPayloadChunks.clear();
    block.headerPtr->payload_uncompressed_size += size;
    block.headerPtr->payload_size = block.headerPtr->payload_uncompressed_size;

    return true;
}

bool ContainerContext::AppendAlignmentPadding(BlockContext& block, mpsize_t alignment)
{
    if (!block.headerPtr || !block.bNewWritableBlock || alignment == 0)
        return false;
    // Calculate current size and any necessary padding.
    mpsize_t current_payload_size = block.headerPtr->payload_uncompressed_size;
    mpsize_t remainder = current_payload_size % alignment;
    if (remainder != 0)
    {
        mpsize_t padding_size = alignment - remainder;
        return AppendPayloadData(block, nullptr, padding_size);
    }
    return true;
}

Locator ContainerContext::AppendPayloadDataAligned(BlockContext& block, const uint8_t* data, mpsize_t size, bool bWriteSize, bool bAddNullTermination)
{
    if (!block.bNewWritableBlock || !block.headerPtr)
        return {};
    if (size == 0)
        return {};

    if (!AppendAlignmentPadding(block))
        return {};

    // Append the data size
    if (bWriteSize)
    {
        uint64_t size_to_write = size + (bAddNullTermination ? 1 : 0);
        if (!AppendPayloadData(block, reinterpret_cast<uint8_t*>(&size_to_write), sizeof(size_to_write)))
            return {}; // Failure to append the data size
    }

    // The start offset for the new data is now the current end of the payload.
    // This offset is guaranteed to be aligned.
    mpsize_t data_start_offset = block.headerPtr->payload_uncompressed_size;
    
    // Append the actual data.
    if (!AppendPayloadData(block, data, size))
        return {};

    // Append the null terminator
    if (bAddNullTermination)
    {
        if (!AppendPayloadData(block, nullptr, 1))
            return {};
    }

    // Construct the 64-bit locator.
    uint32_t block_id = block.headerPtr->block_id;
    Locator locator = BuildDataLocator(block_id, data_start_offset);
    return locator;
}

bool ContainerContext::AppendPayloadDataContiguous(BlockContext& block, const uint8_t* data, mpsize_t size, uint32_t& out_offset, bool aligned)
{
    if (!block.headerPtr || !block.bNewWritableBlock)
        return false;
    if (aligned && !AppendAlignmentPadding(block))
        return false;

    mpsize_t current_payload_end = block.headerPtr->payload_uncompressed_size;
    mpsize_t offset_in_current_chunk = current_payload_end % PAYLOAD_CONTIGUOUS_CHUNK_SIZE;
    
    // If we are exactly at a chunk boundary (but not at the very beginning), there is no space left.
    if (offset_in_current_chunk == 0 && current_payload_end > 0)
        offset_in_current_chunk = PAYLOAD_CONTIGUOUS_CHUNK_SIZE;
    
    mpsize_t space_left_in_chunk = PAYLOAD_CONTIGUOUS_CHUNK_SIZE - offset_in_current_chunk;

    // If the data won't fit and there's a partial chunk to fill, add padding.
    if (size > space_left_in_chunk && space_left_in_chunk < PAYLOAD_CONTIGUOUS_CHUNK_SIZE)
    {
        if (!AppendPayloadData(block, nullptr, space_left_in_chunk))
            return false;
    }

    // The new offset is the current end of the payload (after any padding).
    out_offset = block.headerPtr->payload_uncompressed_size;
    return AppendPayloadData(block, data, size);
}

inline uint8_t* ContainerContext::GetDataByLocatorImpl(const Locator& locator, TypeOfData type_of_data, mpsize_t* in_out_data_size, mpsize_t* out_ready_to_access_len)
{
    if (locator.entry == 0)
        return nullptr;

    // Decode the locator into its components.
    auto [block_id, data_start_offset] = DecodeLocator(locator);

    // Find the block using the fast lookup.
    BlockContext* block = GetBlock(block_id);
    if (!block)
        return nullptr;

    mpsize_t final_data_size = in_out_data_size ? *in_out_data_size : 0;
    mpsize_t extra_offset = 0;

    // A zero size means that it must be read from the stored prefix.
    if (final_data_size == 0)
    {
        mpsize_t active_lock_level = GetActiveLockLevel();

        mpsize_t local_ready_to_access_len = 0;
        mpsize_t size_header_len = 0;
        uint8_t* size_header_ptr = nullptr;

        if (type_of_data == TypeOfData::SizePrefixedObject)
        {
            size_header_len = sizeof(uint32_t);
            size_header_ptr = GetPayloadDataPtr(*block, data_start_offset, size_header_len, &local_ready_to_access_len);
            if (!size_header_ptr)
                return nullptr;

            uint32_t read_size = *reinterpret_cast<uint32_t*>(size_header_ptr);
            final_data_size = static_cast<mpsize_t>(read_size);
            extra_offset = size_header_len;
        }
        else // TypeOfData::Default
        {
            // Safety check: ensure there's room to read an 8-byte header before the data.
            if (data_start_offset < block->virtualStartOffset + kDataAlignment)
                return nullptr;
            const mpsize_t size_header_offset = data_start_offset - kDataAlignment;

            size_header_len = sizeof(uint64_t);
            size_header_ptr = GetPayloadDataPtr(*block, size_header_offset, size_header_len, &local_ready_to_access_len);
            if (!size_header_ptr)
                return nullptr;

            // The pointer is 8-byte aligned, so it can be read as uint64 or uint32.
            uint64_t read_size = *reinterpret_cast<uint64_t*>(size_header_ptr);
            final_data_size = static_cast<mpsize_t>(read_size);
        }

        if (in_out_data_size)
            *in_out_data_size = final_data_size;

        // Return directly if the prefix and object are already contiguous.
        if (size_header_len <= local_ready_to_access_len &&
            final_data_size <= local_ready_to_access_len - size_header_len)
        {
            if (out_ready_to_access_len)
                *out_ready_to_access_len = local_ready_to_access_len - size_header_len;
            return size_header_ptr + size_header_len;
        }

        if (active_lock_level)
        {
            // Release the prefix-only scratch buffer before fetching the object.
            ResetCurrentLock();
        }
    }

    // Keep a size-prefixed object adjacent to its prefix when copying is required.
    if (type_of_data == TypeOfData::SizePrefixedObject)
    {
        if (final_data_size > max_mpsize_t - extra_offset)
            return nullptr;
        mpsize_t ready_with_prefix = 0;
        uint8_t* prefixed_object = GetPayloadDataPtr(
            *block, data_start_offset, final_data_size + extra_offset,
            out_ready_to_access_len ? &ready_with_prefix : nullptr);
        if (!prefixed_object)
            return nullptr;
        if (out_ready_to_access_len)
        {
            if (ready_with_prefix < extra_offset)
                return nullptr;
            *out_ready_to_access_len = ready_with_prefix - extra_offset;
        }
        return prefixed_object + extra_offset;
    }

    return GetPayloadDataPtr(*block, data_start_offset, final_data_size, out_ready_to_access_len);
}

uint8_t* ContainerContext::GetDataByLocator(const Locator& locator, TypeOfData type_of_data, mpsize_t* in_out_data_size, mpsize_t* out_ready_to_access_len)
{
    uint8_t* res = GetDataByLocatorImpl(locator, type_of_data, in_out_data_size, out_ready_to_access_len);
    DeactivateLock();
    return res;
}

void ContainerContext::PushDataLockImpl()
{
}

void ContainerContext::PopDataLockImpl()
{
    // Keep the size of m_cross_chunk_buffers[m_lock_level] intact so it can be reused
}

void ContainerContext::SetObjectCachingAllowed(bool allowed)
{
    bObjectCachingAllowed = allowed;
}

bool ContainerContext::IsObjectCachingAllowed()
{
    return bObjectCachingAllowed;
}

void ContainerContext::ForceBlockDataPersistence(bool enabled)
{
    bBlockPersistanceRequired = (enabled && !m_context->ArePointersStable());
}

void ContainerContext::UnloadForceLoadedBlockData()
{
    for (BlockContext& block : m_blocks)
    {
        if (block.bForcedPersistance && !block.payloadChunks.empty())
        {
            block.payloadChunks.clear();
        }
        block.bForcedPersistance = false;
    }
}

// The static member definition and initialization.
const ntd::array<uint8_t, ContainerContext::PADDING_BUFFER_SIZE> ContainerContext::s_zero_padding_buffer = 
    make_filled_array<ContainerContext::PADDING_BUFFER_SIZE, '\0'>();

uint8_t ContainerContext::s_dataformat_major_version = 0;
uint8_t ContainerContext::s_dataformat_minor_version = 0;
uint64_t ContainerContext::s_dataformat_signature = 0;

} // GPRX

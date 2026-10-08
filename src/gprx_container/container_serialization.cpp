// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "container_context.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/limits.h"

namespace GPRX
{

bool ContainerContext::PreparePayloadsForSerialization(
    ntd::vector<BlockContext*>& out_temporarily_cached_blocks)
{
    for (auto& block : m_blocks)
    {
        if (!block.headerPtr || block.bDontSave ||
            block.headerPtr->type_id == BlockType::GlobalIndex)
            continue;

        // Raw source bytes can be copied straight into SaveToMemory's final
        // allocation. Only encoding requires a decoded in-memory payload first.
        if (block.headerPtr->encoding_method == BlockPayloadEncodingMethod::Raw ||
            !block.encodedPayloadChunks.empty())
            continue;

        bool was_cached = false;
        if (!TryCacheReadOnlyBlockPayload(block, was_cached))
            return false;
        if (was_cached)
        {
            if (out_temporarily_cached_blocks.empty())
                out_temporarily_cached_blocks.reserve(m_blocks.size());
            out_temporarily_cached_blocks.push_back(&block);
        }
    }
    return EnsurePayloadsAreEncoded();
}

void ContainerContext::ClearTemporarySerializationData(
    const ntd::vector<BlockContext*>& temporarily_cached_blocks)
{
    ClearEncodedPayloads();
    for (BlockContext* cached : temporarily_cached_blocks)
        cached->payloadChunks.clear();
}

bool ContainerContext::BuildGlobalIndex(BlockContext& index_block)
{
    if (!index_block.bNewWritableBlock || !index_block.headerPtr)
        return false;

    bool index_block_found = false;
    for (const auto& block : m_blocks)
    {
        if (&block == &index_block)
            index_block_found = true;
        else if (block.headerPtr &&
            block.headerPtr->block_id == index_block.headerPtr->block_id)
            return false;
    }
    if (!index_block_found)
        return false;

    // This live index describes the container's current in-memory layout. It remains raw so
    // SaveToMemory can copy it and InitExternalDataContextWithDataChunks can expose it directly.
    // SaveToStream optionally builds a separate index for its output layout.
    index_block.headerPtr->type_id = BlockType::GlobalIndex;
    index_block.headerPtr->encoding_method = BlockPayloadEncodingMethod::Raw;
    index_block.ClearPayloadHard();
    index_block.encodedPayloadChunks.clear();

    const mpsize_t headerSize = GetBlockHeaderDiskSize();
    // 1. Write all block references (Header + Original Offset)
    mpdatasz_t offset = 0;
    for (const auto& block : m_blocks)
    {
        // Existing indexes and the target itself are not part of the new snapshot.
        if (!block.headerPtr || block.bDontSave || &block == &index_block ||
            block.headerPtr->type_id == BlockType::GlobalIndex)
            continue;

        // Append the BlockHeader struct
        Locator loc = AppendPayloadDataAligned(index_block,
            reinterpret_cast<const uint8_t*>(block.headerPtr), sizeof(BlockHeader));
        if (!loc.entry)
            return false;

        offset += headerSize;

        const mpdatasz_t payloadOffsetOrig =
            block.payloadOffsetOrig ? block.payloadOffsetOrig : offset;
        if (bLargeContainersDisabled && payloadOffsetOrig > max_mpsize_t)
            return false;
        if (!AppendPayloadData(index_block,
                reinterpret_cast<const uint8_t*>(&payloadOffsetOrig),
                sizeof(payloadOffsetOrig)))
            return false;

        offset += CalculatePayloadSaveSize(block);
        if (bLargeContainersDisabled && offset > max_mpsize_t)
            return false;
    }

    // 2. Remember the actual data size before we add padding and the trailer.
    const mpsize_t actual_data_size = index_block.headerPtr->payload_uncompressed_size;

    // 3. Calculate alignment for the trailer
    // The scanner searches in steps of PAYLOAD_TRAILER_SEARCH_STEP, so the region must align to it.
    mpsize_t total_region_size = 0;
    if (!TryGetTrailedPayloadRegionSize(
            actual_data_size, true, total_region_size))
        return false;
    const mpsize_t padding_size = total_region_size - actual_data_size -
        GetBlockTrailerDiskSize();

    // 4. Append zero-padding
    if (padding_size > 0)
    {
        if (!AppendPayloadData(index_block, nullptr, padding_size))
            return false;
    }

    // 5. Construct and append the Trailer struct
    PayloadTrailer trailer{};
    trailer.payload_actual_size = actual_data_size;
    trailer.distance_between_blocks = total_region_size;

    if (!AppendPayloadData(index_block,
            reinterpret_cast<const uint8_t*>(&trailer), sizeof(PayloadTrailer)))
        return false;

    // 6. Append the GUID
    if (!AppendPayloadData(index_block, PAYLOAD_TRAILER_GUID.data(),
            to_mpsize(PAYLOAD_TRAILER_GUID.size())))
        return false;

    index_block.bDontSave = false;

    // A successfully rebuilt target is now the only live index exported by snapshot paths.
    for (auto& block : m_blocks)
    {
        if (&block != &index_block && block.headerPtr &&
            block.headerPtr->type_id == BlockType::GlobalIndex)
            block.bDontSave = true;
    }

    m_global_index_block_id = index_block.headerPtr->block_id;
    return true;
}

bool ContainerContext::AppendGlobalIndexEntryForSerialization(
    BlockContext& index_block,
    const BlockHeader& block_header,
    mpdatasz_t payload_offset)
{
    if (!index_block.headerPtr ||
        block_header.type_id == BlockType::GlobalIndex ||
        (bLargeContainersDisabled && payload_offset > max_mpsize_t))
        return false;

    return AppendPayloadDataAligned(index_block,
               reinterpret_cast<const uint8_t*>(&block_header),
               sizeof(BlockHeader)).entry &&
        AppendPayloadData(index_block,
            reinterpret_cast<const uint8_t*>(&payload_offset),
            sizeof(payload_offset));
}

bool ContainerContext::StreamBlockWithTrailer(
    IStreamProcessor& writer,
    const BlockContext& block,
    bool raw_headers,
    mpsize_t* out_payload_actual_size)
{
    auto pipeline = CreateEncodingPipeline(
        block.headerPtr ? block.headerPtr->encoding_method : BlockPayloadEncodingMethod::Raw, &writer);
    return pipeline && StreamBlockWithTrailer(
        writer, *pipeline, block, raw_headers, out_payload_actual_size);
}

bool ContainerContext::StreamBlockWithTrailer(
    IStreamProcessor& writer,
    IStreamProcessor& encoding_pipeline,
    const BlockContext& block,
    bool raw_headers,
    mpsize_t* out_payload_actual_size)
{
    if (!block.headerPtr ||
        !IsKnownPayloadEncoding(block.headerPtr->encoding_method))
        return false;

    const mpsize_t uncompressed_size =
        block.headerPtr->payload_uncompressed_size;
    BlockHeader streamed_header = *block.headerPtr;
    streamed_header.payload_size = BlockPayloadSizeDeferredMarker;
    if (!StreamBlockHeader(writer, streamed_header, raw_headers))
        return false;

    if (!StreamPayloadChunks(encoding_pipeline, block.payloadChunks,
            uncompressed_size, true))
        return false;

    const mpdatasz_t payload_actual_size_wide = encoding_pipeline.GetBytesWritten();
    if (payload_actual_size_wide > max_mpsize_t)
        return false;
    const mpsize_t payload_actual_size = to_mpsize(payload_actual_size_wide);
    mpsize_t total_region_size = 0;
    if (!TryGetTrailedPayloadRegionSize(
            payload_actual_size, raw_headers, total_region_size))
        return false;
    const mpsize_t trailer_size = GetBlockTrailerDiskSize(raw_headers);
    const mpsize_t padding_size =
        total_region_size - payload_actual_size - trailer_size;

    PayloadTrailer trailer{};
    trailer.payload_actual_size = payload_actual_size;
    trailer.distance_between_blocks = total_region_size;

    if (!StreamPadding(writer, padding_size, raw_headers))
        return false;
    if (raw_headers)
    {
        if (!writer.ProcessChunk(
                reinterpret_cast<const uint8_t*>(&trailer), sizeof(trailer)) ||
            !writer.ProcessChunk(PAYLOAD_TRAILER_GUID.data(),
                to_mpsize(PAYLOAD_TRAILER_GUID.size())))
            return false;
    }
    else
    {
        uint8_t trailer_hex[sizeof(PayloadTrailer) * 2];
        SerializeToHex(&trailer, sizeof(trailer), trailer_hex);
        if (!writer.ProcessChunk(trailer_hex, sizeof(trailer_hex)) ||
            !writer.ProcessChunk(PAYLOAD_TRAILER_GUID_HEX.data(),
                to_mpsize(PAYLOAD_TRAILER_GUID_HEX.size())))
            return false;
    }
    if (out_payload_actual_size)
        *out_payload_actual_size = payload_actual_size;
    return true;
}

void ContainerContext::InitExternalDataContextWithDataChunks(DataContextMemChunks& external_context)
{
    external_context.ClearMemChunks();
    if (m_blocks.size() > max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "ContainerContext::InitExternalDataContextWithDataChunks - block count exceeds mpsize_t");
        return;
    }

    external_context.ReserveBlockInfo(to_mpsize(m_blocks.size()));
    ntd::vector<uint8_t*> block_chunk_ptrs;
    ntd::vector<mpsize_t> block_chunk_sizes;
    const bool success = ProcessBlocksForFlattening(block_chunk_ptrs, block_chunk_sizes,
        [&](uint32_t block_id)
        {
            const bool added = external_context.AddBlockMemChunks(
                block_id,
                block_chunk_ptrs.data(),
                block_chunk_sizes.data(),
                to_mpsize(block_chunk_ptrs.size()));
            block_chunk_ptrs.clear();
            block_chunk_sizes.clear();
            return added;
        }
    );
    if (!success)
    {
        external_context.ClearMemChunks();
        return;
    }
    external_context.ProcessBlockInfo();
}

bool ContainerContext::SaveToStream(
    IStreamProcessor& writer,
    bool use_trailer,
    bool raw_headers,
    bool emit_global_index,
    const StreamFinalizerOptions* finalizer_options)
{
    return SaveToStreamImpl(
        writer, use_trailer, raw_headers, emit_global_index,
        finalizer_options, nullptr);
}

bool ContainerContext::SaveToStreamWithEncoding(
    IStreamProcessor& writer,
    BlockPayloadEncodingMethod output_encoding,
    bool use_trailer,
    bool raw_headers,
    bool emit_global_index,
    const StreamFinalizerOptions* finalizer_options)
{
    if (!IsKnownPayloadEncoding(output_encoding))
        return false;
    return SaveToStreamImpl(
        writer, use_trailer, raw_headers, emit_global_index,
        finalizer_options, &output_encoding);
}

bool ContainerContext::SaveToStreamImpl(
    IStreamProcessor& writer,
    bool use_trailer,
    bool raw_headers,
    bool emit_global_index,
    const StreamFinalizerOptions* finalizer_options,
    const BlockPayloadEncodingMethod* output_encoding)
{
    if (finalizer_options &&
        ((!finalizer_options->finalizer_data && finalizer_options->finalizer_size > 0) ||
            finalizer_options->finalizer_size >
                max_decoded_payload_size - HtmlCommentClose.size()))
        return false;

    const BlockContext* live_index = nullptr;
    const BlockContext* live_terminator = nullptr;
    uint32_t max_block_id = 0;
    bool has_block = false;
    if (emit_global_index || finalizer_options)
    {
        for (const auto& block : m_blocks)
        {
            if (!block.headerPtr)
                continue;
            has_block = true;
            max_block_id = ntd::max(max_block_id, block.headerPtr->block_id);
            if (block.bDontSave)
                continue;

            if (emit_global_index &&
                block.headerPtr->type_id == BlockType::GlobalIndex)
            {
                if (live_index)
                    return false;
                live_index = &block;
            }
            if (finalizer_options &&
                block.headerPtr->type_id == BlockType::Terminator)
            {
                if (live_terminator)
                    return false;
                live_terminator = &block;
            }
        }
    }

    auto allocate_block_id = [&]() -> uint32_t
    {
        if (!has_block)
        {
            has_block = true;
            max_block_id = 0;
            return 0;
        }
        return ++max_block_id;
    };

    BlockContext serialized_index;
    if (emit_global_index)
    {
        if (!live_index && (!has_block || max_block_id == max_u32))
            return false;

        serialized_index.bNewWritableBlock = true;
        serialized_index.AllocateHeader();
        if (live_index)
        {
            ntd::memcpy(serialized_index.headerPtr,
                live_index->headerPtr, sizeof(BlockHeader));
        }
        else
        {
            serialized_index.headerPtr->block_id = allocate_block_id();
        }
        serialized_index.headerPtr->type_id = BlockType::GlobalIndex;
        if (output_encoding)
        {
            serialized_index.headerPtr->encoding_method = *output_encoding;
        }
        else if (bGlobalIndexSerializationEncodingSet)
        {
            serialized_index.headerPtr->encoding_method =
                m_global_index_serialization_encoding;
        }
        serialized_index.headerPtr->payload_size = 0;
        serialized_index.headerPtr->payload_uncompressed_size = 0;
    }

    if (bLargeContainersDisabled && !use_trailer && !emit_global_index &&
        !finalizer_options &&
        CalculateTotalSaveSize(raw_headers) > max_mpsize_t)
        return false;

    writer.SetIgnorePropagatedCalls(true);
    writer.BeginStream();

    auto write_block = [&](BlockContext& block, bool allow_streaming_trailer)
    {
        const mpsize_t uncompressed_size =
            block.headerPtr->payload_uncompressed_size;
        bool forceReadPayload = false;
        if (!TryCacheReadOnlyBlockPayload(block, forceReadPayload))
            return false;

        const bool is_encoded =
            block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw;
        const bool add_to_index =
            emit_global_index && block.headerPtr->type_id != BlockType::GlobalIndex;
        mpdatasz_t index_payload_offset = 0;
        if (add_to_index)
        {
            const mpdatasz_t header_offset = writer.GetBytesWritten();
            const mpsize_t header_size = GetBlockHeaderDiskSize(raw_headers);
            if (header_offset > max_mpdatasz_t - header_size)
                return false;
            index_payload_offset = header_offset + header_size;
        }

        // --- PATH 1: Stream the payload directly and record its final size in a trailer. ---
        if (allow_streaming_trailer &&
            (is_encoded || block.headerPtr->type_id == BlockType::GlobalIndex))
        {
            mpsize_t payload_actual_size = 0;
            if (!StreamBlockWithTrailer(
                    writer, block, raw_headers, &payload_actual_size))
                return false;

            if (add_to_index)
            {
                BlockHeader indexed_header = *block.headerPtr;
                indexed_header.payload_size = payload_actual_size;
                if (!AppendGlobalIndexEntryForSerialization(
                        serialized_index, indexed_header, index_payload_offset))
                    return false;
            }
        }
        // --- PATH 2: Write a known-size payload, pre-encoding it when necessary. ---
        else
        {
            const MemChunkList* chunks_to_write = &block.payloadChunks;
            if (!is_encoded)
                block.headerPtr->payload_size = uncompressed_size;
            if (is_encoded)
            {
                if (block.encodedPayloadChunks.empty())
                {
                    StreamProcessorChunkedMemory temp_sink;
                    auto pipeline = CreateEncodingPipeline(block.headerPtr->encoding_method, &temp_sink);
                    if (!pipeline)
                        return false;
                    if (!StreamPayloadChunks(*pipeline, block.payloadChunks, uncompressed_size, true))
                        return false;

                    const mpdatasz_t encoded_size = temp_sink.GetBytesWritten();
                    if (encoded_size > max_mpsize_t)
                        return false;
                    block.headerPtr->payload_size = to_mpsize(encoded_size);
                    block.encodedPayloadChunks = temp_sink.GetChunks();
                }
                chunks_to_write = &block.encodedPayloadChunks;
            }

            if (add_to_index)
            {
                if (!AppendGlobalIndexEntryForSerialization(
                        serialized_index, *block.headerPtr, index_payload_offset))
                    return false;
            }

            if (!StreamBlockHeader(writer, *block.headerPtr, raw_headers))
                return false;
            if (!StreamPayloadChunks(writer, *chunks_to_write, block.headerPtr->payload_size))
                return false;
            
            // This path uses the simple 8-byte inter-block alignment padding.
            mpsize_t remainder = block.headerPtr->payload_size % kDataAlignment;
            if (remainder != 0)
            {
                mpsize_t padding_size = kDataAlignment - remainder;
                 if (!StreamPadding(writer, padding_size, raw_headers))
                    return false;
            }
            
            if (is_encoded)
                block.encodedPayloadChunks.clear();
        }

        if (forceReadPayload)
            block.payloadChunks.clear();

        if (bLargeContainersDisabled && writer.GetBytesWritten() > max_mpsize_t)
            return false;

        return true;
    };

    for (auto& block : m_blocks)
    {
        if (!output_encoding && block.headerPtr && !block.bDontSave &&
            Block::IsMagic(block.headerPtr))
            raw_headers = !IsMagicAsciiEncoded(block.headerPtr);

        if (!block.headerPtr || block.bDontSave ||
            block.headerPtr->type_id == BlockType::GlobalIndex ||
            ((finalizer_options || output_encoding) &&
                block.headerPtr->type_id == BlockType::Terminator))
            continue;

        bool temporarily_cached = false;
        BlockHeader output_header{};
        BlockHeader* source_header = nullptr;
        MemChunkList original_encoded_payload;
        if (output_encoding)
        {
            // Decode under the source header before selecting the output encoding.
            if (!TryCacheReadOnlyBlockPayload(block, temporarily_cached))
                return false;
            source_header = block.headerPtr;
            output_header = *source_header;
            block.headerPtr = &output_header;
            // Encoded chunks match the source header's encoding. Keep them out of
            // the temporary output path and restore the cache when it completes.
            original_encoded_payload = ntd::move(block.encodedPayloadChunks);
            if (Block::IsMagic(block.headerPtr))
                InitMagicBlock(block, *output_encoding);
            if (!block.bFixedEncoding)
            {
                block.headerPtr->encoding_method =
                    block.headerPtr->payload_uncompressed_size > 0
                    ? *output_encoding
                    : BlockPayloadEncodingMethod::Raw;
            }
        }
        ScopeExit restore_block([&]()
        {
            if (output_encoding)
            {
                block.headerPtr = source_header;
                block.encodedPayloadChunks = ntd::move(original_encoded_payload);
            }
            if (temporarily_cached)
                block.payloadChunks.clear();
        });

        if (!write_block(block, use_trailer))
            return false;
    }

    if (emit_global_index)
    {
        if (!write_block(serialized_index, true))
            return false;
    }

    if (finalizer_options)
    {
        if (raw_headers)
        {
            BlockHeader header{};
            if (live_terminator)
                header = *live_terminator->headerPtr;
            else
            {
                if (has_block && max_block_id == max_u32)
                    return false;
                header.block_id = allocate_block_id();
            }
            header.type_id = BlockType::Terminator;
            header.encoding_method = BlockPayloadEncodingMethod::Raw;
            header.payload_uncompressed_size = finalizer_options->finalizer_size;
            header.payload_size = finalizer_options->finalizer_size;

            if (!StreamBlockHeader(writer, header, true))
                return false;
        }
        else if (!writer.ProcessChunk(
                HtmlCommentClose.data(), to_mpsize(HtmlCommentClose.size())))
            return false;

        if (finalizer_options->finalizer_size > 0 &&
            !writer.ProcessChunk(finalizer_options->finalizer_data,
                finalizer_options->finalizer_size))
            return false;

        // The finalizer is necessarily last. Padding here would become part of it.
        if (bLargeContainersDisabled && writer.GetBytesWritten() > max_mpsize_t)
            return false;
    }

    return writer.EndStream();
}

bool ContainerContext::StreamPayloadChunks(IStreamProcessor& pipeline, const MemChunkList& chunks, mpsize_t size, bool beginEndStreamWithPropagation)
{
    if (beginEndStreamWithPropagation)
        pipeline.BeginStream(true);

    if (!ProcessChunkSequence(chunks, size,
        [&](const uint8_t* chunk_data, mpsize_t size_from_chunk) -> bool
        {
            return pipeline.ProcessChunk(chunk_data, size_from_chunk);
        }
    ))
    {
        return false;
    }

    if (beginEndStreamWithPropagation)
        return pipeline.EndStream(true);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool IncrementalContainerWriter::Begin(
    IStreamProcessor& writer,
    BlockPayloadEncodingMethod encoding,
    bool raw_headers)
{
    if (m_open || !ContainerContext::IsKnownPayloadEncoding(encoding))
        return false;

    m_writer = &writer;
    m_encoding = encoding;
    m_raw_headers = raw_headers;
    m_encoding_pipeline = m_index_storage.CreateEncodingPipeline(encoding, m_writer);
    if (!m_encoding_pipeline)
    {
        m_writer = nullptr;
        return false;
    }
    m_writer->SetIgnorePropagatedCalls(true);
    m_writer->BeginStream();

    m_block_ids.clear();
    m_max_block_id = 0;
    m_has_block = false;
    m_index_block.FreeHeader();
    m_index_block.ResetSoft();
    m_index_block.bNewWritableBlock = true;
    m_index_block.AllocateHeader();
    if (!m_index_block.headerPtr)
        return false;
    // AppendPayloadDataAligned reports failure with the all-zero locator, so a
    // temporary nonzero ID is required while the index payload is assembled.
    // Finish replaces it with the ID following the last streamed data block.
    m_index_block.headerPtr->block_id = 1;
    m_index_block.headerPtr->type_id = BlockType::GlobalIndex;
    m_index_block.headerPtr->encoding_method = encoding;
    m_open = true;
    return true;
}

bool IncrementalContainerWriter::Append(ContainerContext& container)
{
    if (!m_open || !m_writer)
        return false;

    for (auto& block : container.m_blocks)
    {
        if (!block.headerPtr || block.bDontSave ||
            block.headerPtr->type_id == BlockType::GlobalIndex ||
            block.headerPtr->type_id == BlockType::Terminator)
            continue;

        const uint32_t block_id = block.headerPtr->block_id;
        for (uint32_t seen_id : m_block_ids)
        {
            if (seen_id == block_id)
                return false;
        }
        m_block_ids.push_back(block_id);
        m_has_block = true;
        m_max_block_id = ntd::max(m_max_block_id, block_id);

        bool temporarily_cached = false;
        if (!container.TryCacheReadOnlyBlockPayload(block, temporarily_cached))
            return false;

        BlockHeader output_header = *block.headerPtr;
        BlockHeader* source_header = block.headerPtr;
        MemChunkList original_encoded_payload = ntd::move(block.encodedPayloadChunks);
        block.headerPtr = &output_header;
        if (Block::IsMagic(block.headerPtr))
            container.InitMagicBlock(block, m_encoding);
        if (!block.bFixedEncoding)
        {
            block.headerPtr->encoding_method =
                block.headerPtr->payload_uncompressed_size > 0
                ? m_encoding
                : BlockPayloadEncodingMethod::Raw;
        }

        ScopeExit restore_block([&]()
        {
            block.headerPtr = source_header;
            block.encodedPayloadChunks = ntd::move(original_encoded_payload);
            if (temporarily_cached)
                block.payloadChunks.clear();
        });

        const mpdatasz_t header_offset = m_writer->GetBytesWritten();
        const mpsize_t header_size = GetBlockHeaderDiskSize(m_raw_headers);
        if (header_offset > max_mpdatasz_t - header_size)
            return false;
        const mpdatasz_t payload_offset = header_offset + header_size;

        mpsize_t payload_actual_size = 0;
        if (block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw)
        {
            const bool streamed = block.headerPtr->encoding_method == m_encoding
                ? container.StreamBlockWithTrailer(
                    *m_writer, *m_encoding_pipeline, block, m_raw_headers, &payload_actual_size)
                : container.StreamBlockWithTrailer(
                    *m_writer, block, m_raw_headers, &payload_actual_size);
            if (!streamed)
                return false;
        }
        else
        {
            block.headerPtr->payload_size =
                block.headerPtr->payload_uncompressed_size;
            payload_actual_size = block.headerPtr->payload_size;
            if (!container.StreamBlockHeader(
                    *m_writer, *block.headerPtr, m_raw_headers) ||
                !container.StreamPayloadChunks(
                    *m_writer, block.payloadChunks, payload_actual_size))
                return false;
            const mpsize_t remainder = payload_actual_size % kDataAlignment;
            if (remainder != 0 && !container.StreamPadding(
                    *m_writer, kDataAlignment - remainder, m_raw_headers))
                return false;
        }

        BlockHeader indexed_header = *block.headerPtr;
        indexed_header.payload_size = payload_actual_size;
        if (!m_index_storage.AppendGlobalIndexEntryForSerialization(
                m_index_block, indexed_header, payload_offset))
            return false;
    }
    return true;
}

bool IncrementalContainerWriter::Finish(
    const StreamFinalizerOptions* finalizer_options)
{
    if (!m_open || !m_writer ||
        (finalizer_options &&
            ((!finalizer_options->finalizer_data && finalizer_options->finalizer_size > 0) ||
                finalizer_options->finalizer_size >
                    max_decoded_payload_size - HtmlCommentClose.size())) ||
        (m_has_block && m_max_block_id == max_u32))
        return false;

    m_index_block.headerPtr->block_id = m_has_block ? m_max_block_id + 1 : 0;
    m_index_block.headerPtr->type_id = BlockType::GlobalIndex;
    m_index_block.headerPtr->encoding_method =
        m_index_block.headerPtr->payload_uncompressed_size > 0
        ? m_encoding
        : BlockPayloadEncodingMethod::Raw;
    const bool indexStreamed = m_index_block.headerPtr->encoding_method == m_encoding
        ? m_index_storage.StreamBlockWithTrailer(
            *m_writer, *m_encoding_pipeline, m_index_block, m_raw_headers)
        : m_index_storage.StreamBlockWithTrailer(
            *m_writer, m_index_block, m_raw_headers);
    if (!indexStreamed)
        return false;

    if (finalizer_options)
    {
        if (m_raw_headers)
        {
            if (m_index_block.headerPtr->block_id == max_u32)
                return false;
            BlockHeader terminator{};
            terminator.block_id = m_index_block.headerPtr->block_id + 1;
            terminator.type_id = BlockType::Terminator;
            terminator.payload_uncompressed_size = finalizer_options->finalizer_size;
            terminator.payload_size = finalizer_options->finalizer_size;
            if (!m_index_storage.StreamBlockHeader(
                    *m_writer, terminator, true))
                return false;
        }
        else if (!m_writer->ProcessChunk(
                HtmlCommentClose.data(), to_mpsize(HtmlCommentClose.size())))
            return false;

        if (finalizer_options->finalizer_size > 0 &&
            !m_writer->ProcessChunk(finalizer_options->finalizer_data,
                finalizer_options->finalizer_size))
            return false;
    }

    const bool success = m_writer->EndStream();
    m_open = false;
    m_writer = nullptr;
    m_encoding_pipeline.reset();
    return success;
}

mpdatasz_t IncrementalContainerWriter::GetBytesWritten() const
{
    return m_writer ? m_writer->GetBytesWritten() : 0;
}

bool ContainerContext::StreamBlockHeader(IStreamProcessor& writer, const BlockHeader& header, bool raw_headers)
{
    if (raw_headers)
        return writer.ProcessChunk(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    else
    {
        uint8_t header_hex[BlockHeaderHexSize];
        HeaderToHex(header, &header_hex[0]);
        return writer.ProcessChunk(&header_hex[0], sizeof(header_hex));
    }
}

bool ContainerContext::StreamPadding(IStreamProcessor& writer, mpsize_t num_bytes, bool raw_headers)
{
    if (num_bytes == 0)
        return true;
    
    // The no-entry WASM build does not run dynamic initializers for arbitrary
    // library globals. Fill text padding locally so it cannot remain zeroed.
    uint8_t text_padding_buffer[PADDING_BUFFER_SIZE];
    if (!raw_headers)
        ntd::memset(text_padding_buffer, BlockPaddingByteHex, PADDING_BUFFER_SIZE);
    const uint8_t* source_buffer = raw_headers
        ? s_zero_padding_buffer.data()
        : text_padding_buffer;
    
    mpsize_t bytes_remaining = num_bytes;
    while (bytes_remaining > 0)
    {
        mpsize_t bytes_to_write = ntd::min(bytes_remaining, PADDING_BUFFER_SIZE);
        if (!writer.ProcessChunk(source_buffer, bytes_to_write))
            return false;
        bytes_remaining -= bytes_to_write;
    }

    return true;
}

ntd::vector<uint8_t> ContainerContext::SaveToMemory(bool raw_headers)
{
    // The first block is authoritative for header serialization.  Silently
    // producing a byte stream that contradicts its magic makes the result
    // unreadable and differs from SaveToStream's behavior.
    for (const auto& block : m_blocks)
    {
        if (block.headerPtr && !block.bDontSave && Block::IsMagic(block.headerPtr))
        {
            raw_headers = !IsMagicAsciiEncoded(block.headerPtr);
            break;
        }
    }

    // A live index prepared by BuildGlobalIndex already contains its required raw trailer.
    // Re-encoding it or changing the header representation requires the output-specific
    // index built by SaveToStream(..., emit_global_index = true).
    const BlockContext* memory_index = nullptr;
    bool has_encoded_saved_block = false;
    for (const auto& block : m_blocks)
    {
        if (!block.headerPtr || block.bDontSave)
            continue;
        if (block.headerPtr->type_id == BlockType::GlobalIndex)
        {
            if (memory_index || !block.bNewWritableBlock || !raw_headers ||
                block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw)
                return {};
            memory_index = &block;
        }
        else if (block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw)
        {
            has_encoded_saved_block = true;
        }
    }
    if (memory_index && has_encoded_saved_block)
        return {};

    ntd::vector<BlockContext*> temporarily_cached_blocks;
    ScopeExit temporary_data_cleanup([&]()
    {
        ClearTemporarySerializationData(temporarily_cached_blocks);
    });
    if (!PreparePayloadsForSerialization(temporarily_cached_blocks))
        return {};

    // 1. Calculate the exact final size.
    const mpdatasz_t total_size = CalculateTotalSaveSize(raw_headers);
    if (total_size == 0 ||
        (bLargeContainersDisabled && total_size > max_mpsize_t) ||
        total_size > ntd::numeric_limits<size_t>::max())
        return {};

    // 2. Pre-allocate the final buffer.
    ntd::vector<uint8_t> buffer(static_cast<size_t>(total_size));
    uint8_t* write_ptr = buffer.data();

    const mpsize_t header_disk_size = GetBlockHeaderDiskSize(raw_headers);
    const char padding_char = GetPaddingChar(raw_headers);

    auto write_block = [&](BlockContext& block)
    {
        // Copy Header
        if (raw_headers)
            ntd::memcpy(write_ptr, block.headerPtr, header_disk_size);
        else
            HeaderToHex(*block.headerPtr, write_ptr);
        write_ptr += header_disk_size;

        // Copy Payload
        const auto& chunks_to_use = (block.headerPtr->encoding_method == BlockPayloadEncodingMethod::Raw)
            ? block.payloadChunks
            : block.encodedPayloadChunks;
        const mpsize_t size_to_use = (block.headerPtr->encoding_method == BlockPayloadEncodingMethod::Raw)
            ? block.headerPtr->payload_uncompressed_size
            : block.headerPtr->payload_size;

        if (block.headerPtr->encoding_method == BlockPayloadEncodingMethod::Raw &&
            !block.bNewWritableBlock && block.payloadChunks.empty())
        {
            if (!ReadPayloadDataToBuffer(block, block.virtualStartOffset,
                    size_to_use, write_ptr, size_to_use))
                return false;
            write_ptr += size_to_use;
        }
        else
        {
            if (!ProcessChunkSequence(chunks_to_use, size_to_use,
                [&](const uint8_t* chunk_data, mpsize_t size_from_chunk) -> bool
                {
                    ntd::memcpy(write_ptr, chunk_data, size_from_chunk);
                    write_ptr += size_from_chunk;
                    return true;
                }
            ))
                return false;
        }

        // Copy Padding
        mpsize_t remainder = size_to_use % kDataAlignment;
        if (remainder != 0)
        {
            mpsize_t padding_size = kDataAlignment - remainder;
            ntd::memset(write_ptr, padding_char, padding_size);
            write_ptr += padding_size;
        }
        return true;
    };

    // 3. Perform the "wet run" to copy all data into the buffer.
    for (auto& block : m_blocks)
    {
        if (!block.headerPtr || block.bDontSave)
            continue;
        if (!write_block(block))
            return {};
    }

    if (write_ptr != buffer.data() + buffer.size())
        return {};
    return buffer;
}

inline mpdatasz_t ContainerContext::CalculatePayloadSaveSize(const BlockContext& block) const
{
    const mpsize_t size_to_use = (block.headerPtr->encoding_method == BlockPayloadEncodingMethod::Raw)
        ? block.headerPtr->payload_uncompressed_size
        : block.headerPtr->payload_size;

    mpdatasz_t total_size = size_to_use;

    // Add padding
    mpsize_t remainder = size_to_use % kDataAlignment;
    if (remainder != 0)
        total_size += kDataAlignment - remainder;

    return total_size;
}

mpdatasz_t ContainerContext::CalculateTotalSaveSize(bool raw_headers) const
{
    mpdatasz_t total_size = 0;
    const mpsize_t header_disk_size = GetBlockHeaderDiskSize(raw_headers);

    for (const auto& block : m_blocks)
    {
        if (!block.headerPtr || block.bDontSave)
            continue;

        if (total_size > max_mpdatasz_t - header_disk_size)
            return 0;
        const mpdatasz_t payload_offset = total_size + header_disk_size;
        const mpdatasz_t payload_save_size = CalculatePayloadSaveSize(block);
        if (payload_offset > max_mpdatasz_t - payload_save_size)
            return 0;
        total_size += header_disk_size;
        total_size += payload_save_size;
    }

    return total_size;
}

bool ContainerContext::EnsurePayloadsAreEncoded()
{
    for (auto& block : m_blocks)
    {
        if (!block.headerPtr || block.bDontSave ||
            block.headerPtr->type_id == BlockType::GlobalIndex)
            continue;

        if (block.headerPtr->encoding_method != BlockPayloadEncodingMethod::Raw &&
            block.headerPtr->payload_uncompressed_size > 0 && block.encodedPayloadChunks.empty())
        {
            StreamProcessorChunkedMemory chunk_writer;
            auto pipeline = CreateEncodingPipeline(block.headerPtr->encoding_method, &chunk_writer);
            if (!pipeline)
                return false;

            pipeline->BeginStream(true);

            if (!ProcessChunkSequence(block.payloadChunks, block.headerPtr->payload_uncompressed_size,
                [&](const uint8_t* chunk_data, mpsize_t size_from_chunk) -> bool
                {
                    return pipeline->ProcessChunk(chunk_data, size_from_chunk);
                }
            ))
                return false;
            
            if (!pipeline->EndStream(true))
                return false;

            // Update the header with the new compressed/encoded size
            const mpdatasz_t encoded_size = chunk_writer.GetBytesWritten();
            if (encoded_size > max_mpsize_t)
                return false;
            block.encodedPayloadChunks = chunk_writer.GetChunks();
            block.headerPtr->payload_size = to_mpsize(encoded_size);
        }
    }

    return true;
}

void ContainerContext::ClearEncodedPayloads()
{
    for (auto& block : m_blocks)
        block.encodedPayloadChunks.clear();
}

ntd::unique_ptr<IStreamProcessor> ContainerContext::CreateEncodingPipeline(BlockPayloadEncodingMethod method, IStreamProcessor* sink)
{
    switch (method)
    {
        case BlockPayloadEncodingMethod::Raw:
            return ntd::make_unique<PassThroughProcessor>(sink);
        case BlockPayloadEncodingMethod::Base64:
            return ntd::make_unique<Base64Processor>(IStreamProcessor::Direction::Encode, sink);
        case BlockPayloadEncodingMethod::LZ4:
            return ntd::make_unique<LZ4Processor>(IStreamProcessor::Direction::Encode, sink);
        case BlockPayloadEncodingMethod::LZ4_Base64:
        {
            auto stage2 = ntd::make_unique<Base64Processor>(IStreamProcessor::Direction::Encode, sink);
            auto res = ntd::make_unique<LZ4Processor>(IStreamProcessor::Direction::Encode, stage2.release());
            res->SetOwnsNextStage(true);
            return res;
        }
    }

    return nullptr;
}

} // GPRX

// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "utils_ex.h"
#include "block.h"
#include "lockable.h"
#include "data_context.h"
#include "stream_processor.h"
#include "ntd_stdlib/array.h"
#include "ntd_stdlib/deque.h"
#include "ntd_stdlib/functional.h"
#include "ntd_stdlib/memory.h"
#include "ntd_stdlib/utility.h"
#include "ntd_stdlib/vector.h"

namespace GPRX
{

// Controls whether an input-provided global index may be used as authoritative
// metadata. Untrusted inputs are always discovered from their sequential headers.
enum class InputTrust : uint8_t
{
    Untrusted,
    Trusted,
};

// Passing this descriptor to SaveToStream appends bytes after the serialized container.
// The pointed-to bytes are consumed synchronously and are not retained by the container.
struct StreamFinalizerOptions
{
    const uint8_t* finalizer_data = nullptr;
    mpsize_t finalizer_size = 0;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
static constexpr uint16_t CONTAINER_VERSION = 1;
#ifndef ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY
    #define ENABLE_V1CONTAINER_GLOBALINDEX_U32OFFSET_COMPATIBILITY 1
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
/*
 * A representation of a block inside a ContainerContext.
 * Its header and payload can each be in either owning or non-owning mode.
 * If data decompression is necessary, it will store the result.
 */
struct BlockContext
{
    enum class PayloadCopyMode
    {
        ForNonRawData = 0,
        ForNonStablePointerDataContext,
        Always
    };

    struct Fingerprint
    {
        uint32_t block_id;
        uint32_t payload_size;
        uint32_t payload_uncompressed_size;
        mpsize_t virtualStartOffset;
    };

    bool bNewWritableBlock = false;
    bool bDontSave = false;
    bool bFixedEncoding = false;
    bool bForcedPersistance = false;
    // External chunks and appended owned storage are mutually exclusive.
    bool bUsesExternalPayloadChunks = false;
    PayloadCopyMode payloadCopyMode = PayloadCopyMode::ForNonRawData;
    BlockHeader* headerPtr = nullptr;
    bool bOwnsHeaderPtr = false;
    MemChunkList payloadChunks;
    MemChunkList encodedPayloadChunks;
    mpdatasz_t payloadOffsetOrig = 0;
    mpsize_t virtualStartOffset = 0;

    bool AddExternalPayloadChunk(uint8_t* chunk_ptr, mpsize_t chunk_size);
    void AssignExternalHeader(BlockHeader* header_ptr);
    void AllocateHeader();
    void FreeHeader();

    void ClearPayloadSoft(bool bAllowNonWritableBlocks = false);
    void ClearPayloadHard(bool bAllowNonWritableBlocks = false);
    void TrimPayloadFront(mpsize_t upToVirtualOffset);
    void ZeroPayloadRegion(mpsize_t start_offset, mpsize_t size);

    ~BlockContext();

    void ResetSoft()
    {
        bNewWritableBlock = false;
        bDontSave = false;
        bFixedEncoding = false;
        bForcedPersistance = false;
        bUsesExternalPayloadChunks = false;
        payloadCopyMode = PayloadCopyMode::ForNonRawData;
        
        // If we own the header, don't delete it so it can be reused
        if (!bOwnsHeaderPtr || !headerPtr)
        {
            headerPtr = nullptr;
            bOwnsHeaderPtr = false;
        }
        
        payloadChunks.clear();
        encodedPayloadChunks.clear();
        payloadOffsetOrig = 0;
        virtualStartOffset = 0;
    }

    BlockContext() = default;
    BlockContext(const BlockContext&) = delete;
    BlockContext& operator=(const BlockContext&) = delete;

    // Move Constructor
    BlockContext(BlockContext&& other) noexcept
        : bNewWritableBlock(other.bNewWritableBlock)
        , bDontSave(other.bDontSave)
        , bFixedEncoding(other.bFixedEncoding)
        , bForcedPersistance(other.bForcedPersistance)
        , bUsesExternalPayloadChunks(other.bUsesExternalPayloadChunks)
        , payloadCopyMode(other.payloadCopyMode)
        , headerPtr(other.headerPtr)
        , bOwnsHeaderPtr(other.bOwnsHeaderPtr)
        , payloadChunks(ntd::move(other.payloadChunks))
        , encodedPayloadChunks(ntd::move(other.encodedPayloadChunks))
        , payloadOffsetOrig(other.payloadOffsetOrig)
        , virtualStartOffset(other.virtualStartOffset)
    {
        other.headerPtr = nullptr;
        other.bOwnsHeaderPtr = false;
    }

    // Move Assignment Operator
    BlockContext& operator=(BlockContext&& other) noexcept
    {
        if (this != &other)
        {
            FreeHeader();

            bNewWritableBlock = other.bNewWritableBlock;
            bDontSave = other.bDontSave;
            bFixedEncoding = other.bFixedEncoding;
            bForcedPersistance = other.bForcedPersistance;
            bUsesExternalPayloadChunks = other.bUsesExternalPayloadChunks;
            payloadCopyMode = other.payloadCopyMode;
            headerPtr = other.headerPtr;
            bOwnsHeaderPtr = other.bOwnsHeaderPtr;
            payloadChunks = ntd::move(other.payloadChunks);
            encodedPayloadChunks = ntd::move(other.encodedPayloadChunks);
            payloadOffsetOrig = other.payloadOffsetOrig;
            virtualStartOffset = other.virtualStartOffset;

            other.headerPtr = nullptr;
            other.bOwnsHeaderPtr = false;
        }
        return *this;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/*
 * A highly flexible container that represents a sequence of blocks. Each block has a header and a payload.
 * Codename: GPRX (Game Performance Recording eXchange)
 * In case of extreme popularity, the first letters could stand for 'General-Purpose'.
 *
 * The format is uniform, and even the initial file magic value is part of the first block header.
 * Headers can be encoded either as raw binary or ASCII-hex-formatted text.
 * Payloads can contain raw data, or they can be individually encoded or compressed using LZ4, Base64, or both.
 * Thus, the full container data can be represented as embeddable ASCII text.
 *
 * In the case of compression and streaming, the final payload size may not be known at the time the block header is written.
 * In such cases, a trailer containing additional data is written after the payload.
 * Regardless of encoding or compression, you can navigate to any block by its ID without fully unpacking or decoding the container.
 *
 * Decompression happens in lazy mode, only for blocks that are accessed.
 * Blocks can be read from an input data source interface, added explicitly using external memory as the payload,
 * or created and filled by appending new data to them.
 *
 * The container's content can be exported to an output data stream, a contiguous piece of memory,
 * a sequence of memory chunks, or an external data source interface capable of receiving the data.
 */

class ContainerContext : public ILockable
{
    friend class IncrementalContainerWriter;
public:
    // The fixed size for new payload chunks allocated for runtime-created blocks.
    static constexpr mpsize_t PAYLOAD_ALLOC_CHUNK_SIZE = 32 * 1024;
    // The fixed size for sub-chunks that are expected to contain contiguous data.
    // This is helpful for efficient use in combination with paged data sources.
    static constexpr mpsize_t PAYLOAD_CONTIGUOUS_CHUNK_SIZE = 16 * 1024;
    static_assert(
        PAYLOAD_CONTIGUOUS_CHUNK_SIZE <= PAYLOAD_ALLOC_CHUNK_SIZE,
        "PAYLOAD_CONTIGUOUS_CHUNK_SIZE must not be greater than PAYLOAD_ALLOC_CHUNK_SIZE"
    );

    enum class TypeOfData
    {
        Default = 0,
        SizePrefixedObject,
    };

    struct ValidationStatus
    {
        bool is_valid = false;
        bool container_magic_match = false;
        bool container_version_match = false;
        bool dataformat_major_version_match = false;
        bool dataformat_signature_match = false;
        uint16_t container_version = 0;
        uint8_t dataformat_major_version = 0;
        uint8_t dataformat_minor_version = 0;
        uint64_t dataformat_signature = 0;
        
        void CheckMatches();
        bool IsMatched();
    };

private:
    // When payload decompression or copying is required, the data will be read in chunks of this size.
    static constexpr mpsize_t PAYLOAD_READ_CHUNK_SIZE = 32 * 1024;
    // Max size of continuous padding buffer that can be written in one piece.
    static constexpr mpsize_t PADDING_BUFFER_SIZE = 512;

    // --- Pre-allocated padding buffers ---
    static const ntd::array<uint8_t, PADDING_BUFFER_SIZE> s_zero_padding_buffer;

    static uint8_t s_dataformat_major_version;
    static uint8_t s_dataformat_minor_version;
    static uint64_t s_dataformat_signature;

    IDataContext* m_context = nullptr;
    MagicParsed m_magic;
    ValidationStatus m_validation;

    MemChunk m_global_index_chunk;
    ntd::deque<BlockContext> m_blocks;
    ntd::deque<BlockContext> m_scan_blocks;
    HybridLookupTable<BlockContext*> m_block_map;
    ntd::vector<BlockContext::Fingerprint> m_block_fingerprints;
    bool m_block_id_generation_changed = false;
    uint32_t m_magic_block_id = 0;
    uint32_t m_global_index_block_id = 0;
    uint32_t m_block_id_min = 0;
    uint32_t m_block_id_max = 0;

    bool bObjectCachingAllowed = false;
    bool bBlockPersistanceRequired = false;
    bool bLargeContainersDisabled = false;
    // Applied only to the transient index emitted by SaveToStream.
    BlockPayloadEncodingMethod m_global_index_serialization_encoding =
        BlockPayloadEncodingMethod::Raw;
    bool bGlobalIndexSerializationEncodingSet = false;
    InputTrust m_input_trust = InputTrust::Untrusted;

    // A buffer that contains IDs of blocks that were removed during the last SyncWithDataSource call
    ntd::vector<uint32_t> m_removed_block_ids;

    // A temporary buffer used when GetPayloadDataPtr needs to stitch together data from multiple owned chunks.
    ntd::vector<uint8_t> m_cross_chunk_buffers[IDataContext::LOCK_LEVEL_MAX + 1];

    // --- Internal storage for the flattened chunk arrays ---
    ntd::vector<uint8_t*> m_flat_chunk_ptrs;
    ntd::vector<mpsize_t> m_flat_chunk_sizes;

    /**
     * @brief Private helper to iterate through blocks and populate flattened chunk lists.
     * Calls a provided callback after processing each valid block.
     * @param out_ptrs Vector to store chunk pointers.
     * @param out_sizes Vector to store chunk sizes.
     * @param callback Function called after each block's chunks are added, receiving the block ID.
     */
    bool ProcessBlocksForFlattening(
        ntd::vector<uint8_t*>& out_ptrs,
        ntd::vector<mpsize_t>& out_sizes,
        const ntd::function<bool(uint32_t block_id)>& callback
    );

    inline mpdatasz_t ToFullHeaderLayoutOffset(mpdatasz_t physical_offset) const;
    inline mpdatasz_t ToPhysicalLayoutOffset(mpdatasz_t full_layout_offset) const;
    inline bool TryGetNextAlignedBlockOffset(mpdatasz_t payload_end_offset, mpdatasz_t& out_next_block_offset) const;
    inline BlockContext* AllocateScanBlock(mpsize_t& scan_blocks_count);
    inline bool RegisterScannedBlock(BlockContext& ctx, mpdatasz_t orig_offset, bool is_first);
    static bool IsKnownPayloadEncoding(BlockPayloadEncodingMethod method);
    static bool TryGetTrailedPayloadRegionSize(
        mpsize_t payload_actual_size,
        bool raw_headers,
        mpsize_t& out_region_size);
    bool ScanGlobalIndex(mpdatasz_t total_size, mpsize_t& scan_blocks_count);
    bool ScanSequential(mpdatasz_t total_size, bool isRawHeaders, mpsize_t& scan_blocks_count);

    void Init();
    void PerformBlockScan();

    uint8_t* StitchChunks(BlockContext& block, mpsize_t start_chunk_idx, mpsize_t offset_in_start_chunk, mpsize_t end_chunk_idx, mpsize_t data_size, mpsize_t* out_ready_to_access_len);
    ntd::unique_ptr<IStreamProcessor> CreateDecompressorForBlock(const BlockContext& block, uint8_t* destination_buffer, mpsize_t destination_capacity);
    /**
     * @brief Creates a processing pipeline for ENCODING data.
     * @param method The target encoding method (e.g., LZ4, Base64).
     * @param sink The final destination processor for the encoded data.
     * @return A unique_ptr to the head of the processing pipeline.
     */
    ntd::unique_ptr<IStreamProcessor> CreateEncodingPipeline(BlockPayloadEncodingMethod method, IStreamProcessor* sink);
    bool StreamPadding(IStreamProcessor& writer, mpsize_t num_bytes, bool raw_headers = true);
    bool StreamBlockHeader(IStreamProcessor& writer, const BlockHeader& header, bool raw_headers = true);
    bool StreamBlockWithTrailer(
        IStreamProcessor& writer,
        const BlockContext& block,
        bool raw_headers,
        mpsize_t* out_payload_actual_size = nullptr);
    bool StreamBlockWithTrailer(
        IStreamProcessor& writer,
        IStreamProcessor& encoding_pipeline,
        const BlockContext& block,
        bool raw_headers,
        mpsize_t* out_payload_actual_size = nullptr);
    /**
     * @brief Processes a block's payload chunks, respecting the total uncompressed size.
     */
    bool StreamPayloadChunks(IStreamProcessor& pipeline, const MemChunkList& chunks, mpsize_t size, bool beginEndStreamWithPropagation = false);

    /**
     * @brief Iterates all blocks and compresses/encodes any payloads that are marked
     * for encoding but do not yet have an encoded version.
     */    
    bool EnsurePayloadsAreEncoded();
    void ClearEncodedPayloads();
    bool AppendGlobalIndexEntryForSerialization(
        BlockContext& index_block,
        const BlockHeader& block_header,
        mpdatasz_t payload_offset);
    /**
     * @brief Performs a "dry run" to calculate the exact total size of the serialized data.
     */
    mpdatasz_t CalculateTotalSaveSize(bool raw_headers = true) const;
    inline mpdatasz_t CalculatePayloadSaveSize(const BlockContext& block) const;

    inline uint8_t* GetPayloadDataPtrImpl(BlockContext& block, mpsize_t relative_offset, mpsize_t data_size, mpsize_t* out_ready_to_access_len);
    inline uint8_t* GetDataByLocatorImpl(const Locator& locator, TypeOfData type_of_data, mpsize_t* in_out_data_size, mpsize_t* out_ready_to_access_len);
    bool TryCacheReadOnlyBlockPayload(BlockContext& block, bool& out_was_cached);
    bool PreparePayloadsForSerialization(ntd::vector<BlockContext*>& out_temporarily_cached_blocks);
    void ClearTemporarySerializationData(const ntd::vector<BlockContext*>& temporarily_cached_blocks);

protected:
    void PushDataLockImpl() override;
    void PopDataLockImpl() override;

public:
    ContainerContext();
    ContainerContext(IDataContext& context);
    ContainerContext(IDataContext& context, InputTrust input_trust);

    ~ContainerContext()
    {
    }

    static void SetDataFormatMeta(uint64_t signature56bit, uint8_t majorVersion8bit, uint8_t minorVersion8bit);

    IDataContext& GetDataContext() { return *m_context; }
    const MagicParsed& GetMagic() const { return m_magic; }
    const ValidationStatus& GetValidationStatus() { return m_validation; }
    ntd::deque<BlockContext>& GetBlocks() { return m_blocks; }
    inline bool IsInputTrusted() const { return m_input_trust == InputTrust::Trusted; }
    inline bool HasGlobalIndex() const { return m_global_index_block_id != 0; }
    inline uint32_t GetGlobalIndexBlockId() const { return m_global_index_block_id; }
    inline uint32_t GetBlockIdMin() const { return m_block_id_min; }
    inline uint32_t GetBlockIdMax() const { return m_block_id_max; }

    void AddBlock(uint32_t block_id, bool bNewWritableBlock = false);
    bool RemoveBlock(uint32_t block_id);
    BlockContext* GetBlock(uint32_t block_id);

    bool CacheReadOnlyBlockPayload(BlockContext& block);
    void CacheReadOnlyBlockPayloads();
    void ClearReadOnlyBlockPayloadCaches();
    void InitMagicBlock(BlockContext& block, BlockPayloadEncodingMethod containerEncoding = BlockPayloadEncodingMethod::Raw);
    // Also selects the encoding of a temporary global index emitted by SaveToStream.
    void SetContainerEncodingMethod(BlockPayloadEncodingMethod method);

    // Rebuilds the GetBlock lookup after blocks have been added or m_blocks was modified directly.
    void RebuildDirectLookupTable();

    /**
     * @brief Synchronizes the container with the underlying data context.
     * Should be called if the data source is modified.
     */
    bool SyncWithDataSource(uint32_t** out_removed_block_ids, mpsize_t* out_removed_block_count, bool* out_force_clear_caches);

    // Clears data that can be cached from block payloads
    void ClearCaches();

    // Whether users of this container are allowed to cache the returned data
    void SetObjectCachingAllowed(bool allowed);
    bool IsObjectCachingAllowed();

    /**
     * @brief Enables or disables writing containers whose absolute offsets exceed uint32_t.
     * Large containers are enabled by default.
     */
    void SetLargeContainersEnabled(bool enabled) { bLargeContainersDisabled = !enabled; }
    bool AreLargeContainersEnabled() const { return !bLargeContainersDisabled; }

    /**
     * @brief When the block is accessed, force-load the full block data into memory
     * and keep it loaded until explicitly released.
     */
    void ForceBlockDataPersistence(bool enabled);

    /**
     * @brief Unloads blocks that were previously force-loaded into memory.
     */
    void UnloadForceLoadedBlockData();

    /**
     * @brief The primary method for accessing a block's payload data.
     * Handles on-demand decompression based on header flags or container policy.
     */
    uint8_t* GetPayloadDataPtr(BlockContext& block, mpsize_t relative_offset, mpsize_t data_size, mpsize_t* out_ready_to_access_len);

    /**
     * @brief Reads the requested payload data range directly into the provided buffer.
     * For runtime-created blocks, the payload data is copied into the buffer.
     */
    bool ReadPayloadDataToBuffer(BlockContext& block, mpsize_t relative_offset, mpsize_t data_size, uint8_t* buf, mpsize_t buf_size);

    /**
     * @brief Appends data to the payload of a runtime-created block.
     * Automatically allocates new fixed-size chunks as needed.
     * @param block The block to modify (must have bNewWritableBlock = true).
     * @param data Pointer to the data to append.
     * @param size Size of the data to append.
     * @return true on success, false on failure.
     */
    bool AppendPayloadData(BlockContext& block, const uint8_t* data, mpsize_t size);

    /**
     * @brief Appends data to a block's payload, ensuring it starts on kDataAlignment (uint64_t) boundary.
     * If the current payload size is not a multiple of the alignment, padding is added first.
     * @param bWriteSize If true, write the data size before the actual data.
     * @return A 64-bit locator. High 32 bits = Block ID, Low 32 bits = uint64_t element
     * index of the start of the appended data within the payload. Returns 0 on failure.
     */
    Locator AppendPayloadDataAligned(BlockContext& block, const uint8_t* data, mpsize_t size, bool bWriteSize = false, bool bAddNullTermination = false);

    /**
     * @brief Ensures that data is written in one piece and not split between runtime-allocated memory chunks (pages).
     * If necessary, padding will be written to close the current page and start writing from a new one.
     * If the size of the data exceeds one page, a single "large" memory chunk will be allocated to fit all the data,
     * and that chunk will be divided into virtual pages.
     */
    bool AppendPayloadDataContiguous(BlockContext& block, const uint8_t* data, mpsize_t size, uint32_t& out_offset, bool aligned = false);

    /**
     * @brief Appends the necessary number of zeros so that the next write start offset is aligned to the specified byte alignment.
     */
    bool AppendAlignmentPadding(BlockContext& block, mpsize_t alignment = kDataAlignment);

    /**
     * @brief Retrieves a pointer to data within the container using a 64-bit locator.
     * @param in_out_data_size The number of bytes to retrieve. If this is nullptr or 0, the method will
     * attempt to read a uint64_t size from the 8 bytes immediately preceding the data.
     * @return A pointer to the start of the data, or nullptr if the locator is invalid,
     * the block is not found, or the data is out of bounds.
     */
    uint8_t* GetDataByLocator(const Locator& locator, TypeOfData type_of_data = TypeOfData::Default, mpsize_t* in_out_data_size = nullptr, mpsize_t* out_ready_to_access_len = nullptr);

    /**
     * @brief Gets pointers to all data chunks (headers and payloads).
     * The container owns the returned arrays; the caller MUST NOT delete them.
     * @note The returned pointers are only valid until the next call to this method
     * or until the ContainerContext is destroyed.
     */
    void GetAllDataChunks(
        uint8_t** &out_chunk_ptrs,
        mpsize_t* &out_chunk_sizes,
        mpsize_t &out_num_chunks
    );

    /**
     * @brief Populates the given block as the raw live index of the current in-memory layout.
     * The live index can be copied by SaveToMemory or exposed through
     * InitExternalDataContextWithDataChunks. SaveToStream does not serialize its contents;
     * when emit_global_index is true, it builds a separate index for the streamed layout.
     */
    bool BuildGlobalIndex(BlockContext& index_block);

    /**
     * @brief Initializes external DataContextMemChunks with the blocks and their payloads contained
     * in the current ContainerContext. The data context will then be ready for block-relative operations
     * without additional initialization.
     */
    void InitExternalDataContextWithDataChunks(DataContextMemChunks& external_context);

    /**
     * @brief Serializes the entire container's state into a single, contiguous memory buffer.
     * Raw payloads and a raw global index are copied directly into one exact-size output
     * allocation. Encoded output may require temporary storage to determine final sizes.
     * Global-index transformation is not supported; use SaveToStream for that case.
     * @param raw_headers If true, headers are written as raw binary. If false, they are hex-encoded.
     * @return A ntd::vector<uint8_t> containing the complete, serialized data.
     */
    ntd::vector<uint8_t> SaveToMemory(bool raw_headers = true);

    /**
     * @brief Serializes the entire container's state into a stream.
     * @param use_trailer If true, encoded payloads are streamed immediately, without prior
     * knowledge of their resulting size, and use trailers containing that data. Initial scanning
     * of such a block will take a bit longer. If false, each encoded payload is fully processed
     * before writing, resulting in higher peak memory use but more compact output. An emitted
     * global-index block always uses a trailer independently of this option.
     * @param emit_global_index If true, builds and emits a temporary global index. A visible live
     * index supplies its block ID and header metadata, but its contents are not serialized. If
     * false, no global index is emitted, even when a live one exists.
     * @param finalizer_options If non-null, appends the supplied finalizer. With hex headers, the
     * GPRX data is followed directly by an HTML comment closer and the finalizer. With raw headers,
     * the finalizer is the payload of a synthetic raw Terminator block so a sequential reader can
     * stop before any following data. No alignment padding follows the finalizer.
     */
    bool SaveToStream(
        IStreamProcessor& writer,
        bool use_trailer = false,
        bool raw_headers = true,
        bool emit_global_index = false,
        const StreamFinalizerOptions* finalizer_options = nullptr);

    /**
     * @brief Serializes using a temporary output encoding without changing the live container.
     * Read-only source payloads are decoded before temporary header copies are adjusted, and at
     * most one such decoded payload is retained by this operation at a time.
     */
    bool SaveToStreamWithEncoding(
        IStreamProcessor& writer,
        BlockPayloadEncodingMethod output_encoding,
        bool use_trailer,
        bool raw_headers,
        bool emit_global_index,
        const StreamFinalizerOptions* finalizer_options = nullptr);

private:
    bool SaveToStreamImpl(
        IStreamProcessor& writer,
        bool use_trailer,
        bool raw_headers,
        bool emit_global_index,
        const StreamFinalizerOptions* finalizer_options,
        const BlockPayloadEncodingMethod* output_encoding);
};

// Serializes a sequence of independently-owned containers into one output
// stream and emits one cumulative global index when Finish is called. Block
// IDs supplied by appended containers must be unique across the stream.
class IncrementalContainerWriter
{
    IStreamProcessor* m_writer = nullptr;
    BlockPayloadEncodingMethod m_encoding = BlockPayloadEncodingMethod::Raw;
    BlockContext m_index_block;
    ContainerContext m_index_storage;
    ntd::unique_ptr<IStreamProcessor> m_encoding_pipeline;
    ntd::vector<uint32_t> m_block_ids;
    uint32_t m_max_block_id = 0;
    bool m_has_block = false;
    bool m_raw_headers = true;
    bool m_open = false;

public:
    bool Begin(
        IStreamProcessor& writer,
        BlockPayloadEncodingMethod encoding,
        bool raw_headers);
    bool Append(ContainerContext& container);
    bool Finish(const StreamFinalizerOptions* finalizer_options = nullptr);
    mpdatasz_t GetBytesWritten() const;
};
    
} // GPRX

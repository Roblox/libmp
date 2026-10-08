// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "utils_ex.h"
#include "lockable.h"
#include "ntd_stdlib/list.h" // caching proxy, lru
#include "ntd_stdlib/string.h"
#include "ntd_stdlib/unordered_map.h"
#include "ntd_stdlib/vector.h"

#ifndef EXCLUDE_FILE_DATA_CONTEXT
    #include "ntd_stdlib/file_io.h" // ntd::fstream (OS-level or std:: alias per --ntd-fileio)
#endif

#ifndef ENABLE_EXTERNAL_DATA_CONTEXT
#ifdef LIBMP_MODULE_BUILD
    #define ENABLE_EXTERNAL_DATA_CONTEXT 1
#else
    #define ENABLE_EXTERNAL_DATA_CONTEXT 0
#endif
#endif

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Abstract interface for a data source context.
 *
 * Defines a contract for accessing raw data and block payload data,
 * allowing the parsing logic to be independent of the underlying storage mechanism.
 */
class IDataContext : public ILockable
{
public:
    virtual ~IDataContext() = default;

    /**
     * @brief Retrieves a pointer to a contiguous block of payload data for a specific block.
     * @param out_readyToAccessLen Returns the actual length of a contiguous block of payload data that can be accessed.
     * It can be equal to or greater than dataSize.
     */
    virtual uint8_t* GetBlockDataPtr(
        uint32_t /*block_id*/,
        mpsize_t /*payloadOffsetRelative*/,
        mpsize_t /*dataSize*/,
        mpsize_t* out_readyToAccessLen)
    {
        if (out_readyToAccessLen)
            *out_readyToAccessLen = 0;
        return nullptr;
    }

    virtual mpsize_t ReadBlockDataToBuffer(
        uint32_t /*block_id*/,
        mpsize_t /*payloadOffsetRelative*/,
        mpsize_t /*dataSize*/,
        uint8_t* /*destination_buffer*/,
        mpsize_t /*destination_buffer_size*/)
    {
        return 0;
    }

    /**
     * @brief Reads data from an absolute offset directly into a caller-provided buffer.
     * This is an optimization for "copying" contexts (where ArePointersStable is false)
     * to avoid an intermediate copy into the context's internal buffer.
     * @param destination_buffer The caller-owned buffer to write into.
     * @return The number of bytes successfully read.
     */
    virtual mpsize_t ReadAbsDataToBuffer(
        mpdatasz_t /*absolute_offset*/,
        mpsize_t /*dataSize*/,
        uint8_t* /*destination_buffer*/,
        mpsize_t /*destination_buffer_size*/)
    {
        return 0;
    }

    /**
     * @brief Retrieves a pointer to a contiguous block of raw data from an absolute offset.
     * This is the low-level access method used by the parser during the initial scan or for
     * obtaining raw compressed payload data bypassing read caches.
     * It is also used for data contexts that do not support block-level reads
     * (IsBlockFetchSupported() == false).
     */
    virtual uint8_t* GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) = 0;

    /**
     * @brief A stateful, optimized method for sequential forward scanning.
     * Uses a hint from the previous call to find the next chunk faster.
     */
    virtual uint8_t* GetNextAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
    {
        return GetAbsDataPtr(absolute_offset, dataSize, out_readyToAccessLen);
    }
  
    /**
     * @brief Resets the internal state/hint used by GetNextAbsDataPtr.
     * Call this before starting a new sequential scan.
     */
    virtual void ResetScanState() {}

    /**
     * @brief Reserves space for the estimated number of blocks.
     */
    virtual void ReserveBlockInfo(mpsize_t /*count*/) {}

    /**
     * @brief Informs the data context about a discovered block's layout.
     * The context can use this information to build optimization tables (e.g., block_id -> chunk range).
     */
    virtual void RememberBlockInfo(
        uint32_t /*block_id*/,
        mpdatasz_t /*payloadAbsOffset*/,
        mpsize_t /*payloadSize*/)
    {}

    /**
     * @brief Removes information about one or all blocks.
     */
    virtual void ForgetBlockInfo(uint32_t /*block_id*/) {}
    virtual void ForgetAllBlockInfo() {}

    /**
     * @brief Called by the parser after all blocks have been discovered via RememberBlockInfo.
     * This gives the context a chance to build better optimization structures.
     */
    virtual void ProcessBlockInfo() {}

    /**
     * @brief Called when the underlying data has changed and the context should be synced
     * (e.g., to get the new total size).
     */
    virtual void SyncWithDataSource() {}

    /**
     * @brief Forces the DataContext to clear all its caches, for example after
     * SyncWithDataSource is called and block IDs are reused for other blocks
     * with different payloads.
     */
    virtual void ClearCaches() {}

    /**
     * @brief Forces the DataContext to clear its caches related to a specific block,
     * for example after SyncWithDataSource is called and a block's payload is
     * appended, but the block still has the same ID.
     */
    virtual void ClearBlockCache(uint32_t /*block_id*/, mpsize_t /*payloadSize*/) {}

    /**
     * @brief Returns the total size in bytes of the assigned data source.
     */
    virtual mpdatasz_t GetTotalDataSize() const = 0;

    /**
     * @brief Informs the parser whether pointers returned by data getters are stable.
     * If false, the parser should not hold onto them. If true, it can avoid copying.
     */
    virtual bool ArePointersStable() const = 0;

    /**
     * @brief Informs whether pointers to small structs (such as Block headers) returned by data getters are stable.
     */
    virtual bool AreSmallStructPointersStable() const = 0;

    /**
     * @brief Reports whether the context supports block-level fetching.
     * If false, the parser should not call block-related methods.
     */
    virtual bool IsBlockFetchSupported() const = 0;

    /**
     * @brief Reports whether the context supports data prefetching.
     * If false, only the exact requested data size will be returned on read.
     */
    virtual bool IsPrefetchSupported() const = 0;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A data context for a single, contiguous memory buffer.
 * It provides direct, zero-copy access but does not support block-level access.
 */
class DataContextSingleBuffer : public IDataContext
{
private:
    const uint8_t* m_data = nullptr;
    mpdatasz_t m_size = 0;

public:
    DataContextSingleBuffer() = default;

    void AssignBuffer(const uint8_t* data, mpdatasz_t size);

    // --- Core IDataContext Interface Implementation ---

    bool ArePointersStable() const override { return true; }
    bool AreSmallStructPointersStable() const override { return true; }
    bool IsBlockFetchSupported() const override { return false; }
    bool IsPrefetchSupported() const override { return true; }
    mpdatasz_t GetTotalDataSize() const override { return m_size; }

    uint8_t* GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    mpsize_t ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size) override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Unified implementation of data-lock-aware reading using ReadAbsDataToBuffer.
 */
template <typename T>
inline static uint8_t* GetAbsDataPtrUnified(T& context, mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen);

#ifndef EXCLUDE_FILE_DATA_CONTEXT

/**
 * @brief A data context that reads from a local file.
 * It does not support block-level access.
 */
class DataContextFile : public IDataContext
{
    friend uint8_t* GetAbsDataPtrUnified<DataContextFile>(DataContextFile&, mpdatasz_t, mpsize_t, mpsize_t*);

private:
    ntd::fstream m_file;
    mpdatasz_t m_size = 0;

    ntd::vector<uint8_t> m_temp_buffers[ILockable::LOCK_LEVEL_MAX + 1];

protected:
    void PushDataLockImpl() override;
    void PopDataLockImpl() override;

public:
    DataContextFile() = default;
    ~DataContextFile() override;

    bool Open(const ntd::string& filepath);
    bool Adopt(ntd::fstream&& file, mpdatasz_t size);

    // --- Core IDataContext Interface Implementation ---

    bool ArePointersStable() const override { return false; }
    bool AreSmallStructPointersStable() const override { return false; }
    bool IsBlockFetchSupported() const override { return false; }
    bool IsPrefetchSupported() const override { return false; }
    mpdatasz_t GetTotalDataSize() const override { return m_size; }

    uint8_t* GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    mpsize_t ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size) override;
};

#endif //ifndef EXCLUDE_FILE_DATA_CONTEXT

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A smart data context that acts as a caching proxy for another IDataContext.
 * It implements its own block information cache and a page cache with an LRU policy
 * to speed up repeated access to block data, using either absolute or block-relative reads
 * from the wrapped source.
 */
class DataContextCachingProxy : public IDataContext
{
private:
    using page_id_type = uint64_t;
    using pin_mask_type = uint32_t;
    static constexpr pin_mask_type kPageProtectionBit = static_cast<pin_mask_type>(1) << (sizeof(pin_mask_type) * 8 - 1);

    IDataContext* m_wrapped_context;
    bool m_owns_wrapped_context = false;
    bool m_use_block_relative_reads = false;

    // --- Block Info Cache (Proxy's own knowledge) ---
    struct BlockLocationInfo
    {
        mpdatasz_t payloadAbsOffset;
        mpsize_t payloadSize;
    };
    HybridLookupTable<BlockLocationInfo> m_block_locations;

    // --- Page Cache Members ---
    struct PageInfo
    {
        page_id_type page_id;
        mpsize_t physical_page_index;
        ntd::list<page_id_type>::iterator lru_iterator;
        pin_mask_type pin_mask = 0; // Init by default

        inline bool IsPinned();
    };
    
    ntd::vector<uint8_t> m_cache_pool;
    ntd::unordered_map<page_id_type, PageInfo> m_page_map;
    ntd::list<page_id_type> m_lru_list;
    ntd::vector<uint8_t> m_cross_page_buffers[ILockable::LOCK_LEVEL_MAX + 1];
    ntd::vector<page_id_type> m_pinned_pages[ILockable::LOCK_LEVEL_MAX + 1];
    ntd::vector<PageInfo*> protected_pages;
    ntd::vector<mpsize_t> m_free_physical_pages;

    const mpsize_t m_page_size;
    const mpsize_t m_num_pages;

    inline static page_id_type create_page_id(uint32_t block_id, uint32_t page_num)
    {
        return (static_cast<page_id_type>(block_id) << (sizeof(page_id_type) * 8 / 2)) | page_num;
    }

    inline PageInfo* FetchPageIntoCache(uint32_t block_id, const BlockLocationInfo* block_info_ptr, page_id_type page_id, uint32_t page_num);

    inline void PinPage(PageInfo& info, mpsize_t pin_level);
    inline void UnpinPage(PageInfo& info, mpsize_t pin_level);

protected:
    void PushDataLockImpl() override;
    void PopDataLockImpl() override;

public:
    DataContextCachingProxy(IDataContext* wrapped_context, mpsize_t page_size_bytes, mpsize_t num_pages);
    ~DataContextCachingProxy() override;

    void SetOwnsWrappedContext(bool x) { m_owns_wrapped_context = x; }
    void SetUseBlockRelativeReads(bool x)
    {
        m_use_block_relative_reads = x && m_wrapped_context && m_wrapped_context->IsBlockFetchSupported();
    }

    // --- IDataContext Interface Implementation ---
    bool ArePointersStable() const override { return false; }
    bool AreSmallStructPointersStable() const override { return false; }
    bool IsBlockFetchSupported() const override { return true; }
    bool IsPrefetchSupported() const override { return true; }

    // Implemented by the Proxy
    void ReserveBlockInfo(mpsize_t count) override;
    void RememberBlockInfo(uint32_t block_id, mpdatasz_t payloadAbsOffset, mpsize_t payloadSize) override;
    void ProcessBlockInfo() override;
    void ForgetAllBlockInfo() override;
    void ClearCaches() override;
    void ClearBlockCache(uint32_t block_id, mpsize_t payloadSize) override;

    uint8_t* GetBlockDataPtr(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    mpsize_t ReadBlockDataToBuffer(uint32_t block_id, mpsize_t o, mpsize_t s, uint8_t* b, mpsize_t bs) override;

    // Pass-through methods to the wrapped context
    mpsize_t ReadAbsDataToBuffer(mpdatasz_t o, mpsize_t s, uint8_t* b, mpsize_t bs) override
    {
        return m_wrapped_context ? m_wrapped_context->ReadAbsDataToBuffer(o, s, b, bs) : 0;
    }
    uint8_t* GetAbsDataPtr(mpdatasz_t o, mpsize_t s, mpsize_t* l) override
    {
        return m_wrapped_context ? m_wrapped_context->GetAbsDataPtr(o, s, l) : nullptr;
    }
    mpdatasz_t GetTotalDataSize() const override
    {
        return m_wrapped_context ? m_wrapped_context->GetTotalDataSize() : 0;
    }
    void SyncWithDataSource() override
    {
        if (m_wrapped_context)
            m_wrapped_context->SyncWithDataSource();
    }
    void ResetScanState() override
    {
        if (m_wrapped_context)
            m_wrapped_context->ResetScanState();
    }
    uint8_t* GetNextAbsDataPtr(mpdatasz_t o, mpsize_t s, mpsize_t* l) override
    {
        return m_wrapped_context ? m_wrapped_context->GetNextAbsDataPtr(o, s, l) : nullptr;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief An implementation of IDataContext that manages data stored in multiple, non-contiguous memory chunks.
 */
class DataContextMemChunks : public IDataContext
{
private:
    static constexpr mpsize_t ABS_LOOKUP_TABLE_SIZE = 1 * 1024;

    struct BlockLocationInfo
    {
        mpdatasz_t payloadAbsOffset;
        mpsize_t payloadSize;
        mpsize_t startChunkIndex;
        mpsize_t endChunkIndex;
        bool hasManyChunks;
    };

    ntd::vector<const uint8_t*> m_chunks;
    ntd::vector<mpsize_t> m_sizes;
    const uint8_t* const* m_chunk_ptrs = nullptr;
    const mpsize_t* m_chunk_sizes = nullptr;
    bool m_uses_borrowed_chunk_arrays = false;
    mpsize_t m_num_chunks = 0;
    mpdatasz_t m_total_size = 0;
    ntd::vector<uint8_t> m_temp_buffers[ILockable::LOCK_LEVEL_MAX + 1];
    ntd::vector<mpdatasz_t> m_chunk_start_offsets;

    HybridLookupTable<BlockLocationInfo> m_block_locations;
    ntd::vector<mpsize_t> m_abs_lookup_table;
    mpdatasz_t m_abs_lookup_region_size = 0;

    // State for the sequential scan optimization
    mpsize_t m_scan_hint_chunk_index = 0;

    // Internal helper for getting data from a known chunk range.
    inline uint8_t* GetDataPtr_Ranged(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen, bool search_start_chunk, mpsize_t start_chunk, mpsize_t end_chunk, mpsize_t* out_end_chunk, uint8_t* dest_copy_buf = nullptr);

    inline mpsize_t GetAbsStartChunk(mpdatasz_t absolute_offset);
    void RememberBlockInfoImpl(uint32_t block_id, mpdatasz_t payloadAbsOffset, mpsize_t payloadSize, mpsize_t newChunksNum);

protected:
    void PushDataLockImpl() override;
    void PopDataLockImpl() override;

public:
    DataContextMemChunks();
    ~DataContextMemChunks() override = default;

    /** Replaces all chunks with borrowed pointer and size arrays. */
    bool AssignMemChunks(const uint8_t* const* chunk_ptrs, const mpsize_t* chunk_sizes, mpsize_t num_chunks);
    /** Appends chunks using internally owned pointer and size arrays; cannot follow a non-empty assignment. */
    bool AddMemChunks(const uint8_t* const* chunk_ptrs, const mpsize_t* chunk_sizes, mpsize_t num_chunks);

    /**
     * @brief Assigns memory chunks to this context and registers them as part of the specified block ID.
     * The first memory chunk is always the block header; the following chunks are the payload.
     * After adding all blocks you should call ProcessBlockInfo to build the internal lookup table.
     */
    bool AddBlockMemChunks(uint32_t block_id, const uint8_t* const* chunk_ptrs, const mpsize_t* chunk_sizes, mpsize_t num_chunks);
    void ClearMemChunks();

    // --- IDataContext Interface Implementation ---

    uint8_t* GetBlockDataPtr(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    uint8_t* GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    uint8_t* GetNextAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    mpsize_t ReadBlockDataToBuffer(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size) override;
    mpsize_t ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size) override;
    void ResetScanState() override;
    void ReserveBlockInfo(mpsize_t count) override;
    void ForgetAllBlockInfo() override;
    void RememberBlockInfo(uint32_t block_id, mpdatasz_t payloadAbsOffset, mpsize_t payloadSize) override;
    void ProcessBlockInfo() override;
    mpdatasz_t GetTotalDataSize() const override { return m_total_size; }
    bool ArePointersStable() const override { return false; }
    bool AreSmallStructPointersStable() const override { return true; }
    bool IsBlockFetchSupported() const override { return true; }
    bool IsPrefetchSupported() const override { return true; }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief External read functions provided at runtime
 */
struct ExternalIO
{
    uint32_t (*open)(uint8_t* handle) = nullptr;
    uint32_t (*close)(uint8_t* handle) = nullptr;
    mpdatasz_t (*getSize)(uint8_t* handle) = nullptr;
    mpsize_t (*read)(uint8_t* handle, uint8_t* dst, uint32_t block_id, mpdatasz_t offset, mpsize_t sz) = nullptr;
    bool isBlockFetchSupported = false;
};

/**
 * @brief Data context that reads using externally provided functions at runtime.
 */
class DataContextExternalIO : public IDataContext
{
    friend uint8_t* GetAbsDataPtrUnified<DataContextExternalIO>(DataContextExternalIO&, mpdatasz_t, mpsize_t, mpsize_t*);

private:
    bool is_open = false;
    ExternalIO m_io = {};
    uint8_t* m_handle = nullptr;
    mpdatasz_t m_size = 0;

    ntd::vector<uint8_t> m_temp_buffers[ILockable::LOCK_LEVEL_MAX + 1];

protected:
    void PushDataLockImpl() override;
    void PopDataLockImpl() override;

public:
    static ExternalIO GetDefaultIO();

    DataContextExternalIO();
    ~DataContextExternalIO() override;

    void AssignIO(const ExternalIO& io);
    bool Open(uint8_t* handle);

    // --- Core IDataContext Interface Implementation ---

    void SyncWithDataSource() override;
    bool ArePointersStable() const override { return false; }
    bool AreSmallStructPointersStable() const override { return false; }
    bool IsBlockFetchSupported() const override { return m_io.isBlockFetchSupported; }
    bool IsPrefetchSupported() const override { return false; }
    mpdatasz_t GetTotalDataSize() const override { return m_size; }

    uint8_t* GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    mpsize_t ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size) override;

    uint8_t* GetBlockDataPtr(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, mpsize_t* out_readyToAccessLen) override;
    mpsize_t ReadBlockDataToBuffer(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size) override;
};

} // GPRX

////////////////////////////////////////////////////////////////////////////////////////////////////
// Externally provided data-reading functions
extern "C"
{

uint32_t GprxExtDataOpen(uint8_t* handle);
uint32_t GprxExtDataClose(uint8_t* handle);
uint32_t GprxExtDataGetSize(uint8_t* handle, uint32_t* out_size_low, uint32_t* out_size_high);
mpsize_t GprxExtDataRead(
    uint8_t* handle,
    uint8_t* dst,
    uint32_t block_id,
    uint32_t offset_low,
    uint32_t offset_high,
    mpsize_t sz);

}

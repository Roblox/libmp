// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "data_context.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/iterator.h"
#include "ntd_stdlib/limits.h"
#include "ntd_stdlib/utility.h"

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
// Validate and resolve a block-relative range without overflow.
static inline bool ResolveBlockRelative(
    mpdatasz_t payloadAbsOffset, mpsize_t payloadSize,
    mpsize_t relOffset, mpsize_t size, mpdatasz_t& out_absolute_offset)
{
    if (!range_within(relOffset, size, payloadSize))
        return false;
    if (payloadAbsOffset > max_mpdatasz_t - relOffset)
        return false;
    out_absolute_offset = payloadAbsOffset + relOffset;
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void DataContextSingleBuffer::AssignBuffer(const uint8_t* data, mpdatasz_t size)
{
    m_data = data;
    m_size = size;
}

uint8_t* DataContextSingleBuffer::GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    if (!m_data || !range_within<mpdatasz_t>(absolute_offset, dataSize, m_size))
    {
        if (out_readyToAccessLen)
            *out_readyToAccessLen = 0;
        return nullptr;
    }

    if (out_readyToAccessLen)
        *out_readyToAccessLen = to_mpsize(
            ntd::min<mpdatasz_t>(m_size - absolute_offset, max_mpsize_t));
    return const_cast<uint8_t*>(m_data + absolute_offset);
}

mpsize_t DataContextSingleBuffer::ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size)
{
    if (!m_data || !destination_buffer || dataSize > destination_buffer_size ||
        !range_within<mpdatasz_t>(absolute_offset, dataSize, m_size))
        return 0;
    ntd::memcpy(destination_buffer, m_data + absolute_offset, dataSize);
    return dataSize;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
inline static uint8_t* GetAbsDataPtrUnified(T& context, mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    if (out_readyToAccessLen)
        *out_readyToAccessLen = 0;
    mpsize_t active_lock_level = context.GetActiveLockLevel();
    context.DeactivateLock();

    auto& active_temp_buffer = context.m_temp_buffers[active_lock_level];
    resize_if_needed(active_temp_buffer, dataSize);
    mpsize_t bytes_read = context.ReadAbsDataToBuffer(
        absolute_offset,
        dataSize,
        active_temp_buffer.data(),
        to_mpsize(active_temp_buffer.size()));

    if (bytes_read != dataSize)
        return nullptr;

    if (out_readyToAccessLen)
        *out_readyToAccessLen = dataSize;
    return active_temp_buffer.data();
}

#ifndef EXCLUDE_FILE_DATA_CONTEXT

DataContextFile::~DataContextFile()
{
    if (m_file.is_open())
        m_file.close();
}

bool DataContextFile::Open(const ntd::string& filepath)
{
    if (m_file.is_open())
        m_file.close();
    
    m_file.open(filepath.c_str(), ntd::ios::in | ntd::ios::binary | ntd::ios::ate);
    if (!m_file.is_open())
        return false;

    const ntd::streamoff file_size = m_file.tellg();
    if (file_size < 0)
    {
        m_file.close();
        return false;
    }
    m_size = static_cast<mpdatasz_t>(file_size);
    m_file.seekg(0, ntd::ios::beg);
    return true;
}

bool DataContextFile::Adopt(ntd::fstream&& file, mpdatasz_t size)
{
    if (!file.is_open())
        return false;

    if (m_file.is_open())
        m_file.close();
    m_file = ntd::move(file);
    m_size = size;
    m_file.clear();
    m_file.seekg(0, ntd::ios::beg);
    return !m_file.fail();
}

uint8_t* DataContextFile::GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    return GetAbsDataPtrUnified(*this, absolute_offset, dataSize, out_readyToAccessLen);
}

mpsize_t DataContextFile::ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size)
{
    if (!m_file.is_open() || !destination_buffer || dataSize > destination_buffer_size ||
        !range_within<mpdatasz_t>(absolute_offset, dataSize, m_size) ||
        absolute_offset > static_cast<mpdatasz_t>(ntd::numeric_limits<ntd::streamoff>::max()))
        return 0;
    m_file.clear(); // Clear eofbit/failbit
    m_file.seekg(static_cast<ntd::streamoff>(absolute_offset));

    mpsize_t total_read = 0;
    while (total_read < dataSize)
    {
        const ntd::streamsize request_size = static_cast<ntd::streamsize>(ntd::min<uint64_t>(
            dataSize - total_read,
            static_cast<uint64_t>(ntd::numeric_limits<ntd::streamsize>::max())));
        m_file.read(reinterpret_cast<char*>(destination_buffer + total_read), request_size);
        const ntd::streamsize bytes_read = m_file.gcount();
        if (bytes_read < 0 || bytes_read > request_size)
            return 0;

        total_read += static_cast<mpsize_t>(bytes_read);
        if (bytes_read != request_size)
            break;
    }
    return total_read;
}

void DataContextFile::PushDataLockImpl()
{
}

void DataContextFile::PopDataLockImpl()
{
    // Keep the size of m_temp_buffers[m_lock_level] intact so it can be reused
}

#endif //ifndef EXCLUDE_FILE_DATA_CONTEXT

////////////////////////////////////////////////////////////////////////////////////////////////////
DataContextCachingProxy::DataContextCachingProxy(IDataContext* wrapped_context, mpsize_t page_size_bytes, mpsize_t num_pages)
    : m_wrapped_context(wrapped_context)
    , m_page_size(page_size_bytes ? page_size_bytes : 1)
    , m_num_pages(num_pages ? num_pages : 1)
{
    m_block_locations.LimitDirectLookupSizeByMaxId(false);
    m_cache_pool.resize(ntd::detail::SizeMultiplyOrTrap(
        static_cast<size_t>(m_page_size), static_cast<size_t>(m_num_pages)));
    m_page_map.reserve(m_num_pages);
}

DataContextCachingProxy::~DataContextCachingProxy()
{
    if (m_owns_wrapped_context && m_wrapped_context)
        ntd::Delete(m_wrapped_context);
}

void DataContextCachingProxy::ReserveBlockInfo(mpsize_t count)
{
    m_block_locations.Reserve(count);
}

void DataContextCachingProxy::RememberBlockInfo(uint32_t block_id, mpdatasz_t payloadAbsOffset, mpsize_t payloadSize)
{
    if (!m_wrapped_context)
        return;
    const mpdatasz_t total_size = m_wrapped_context->GetTotalDataSize();
    if (payloadAbsOffset > total_size || payloadSize > total_size - payloadAbsOffset)
        return;
    m_block_locations.Insert(block_id, {payloadAbsOffset, payloadSize});
}

void DataContextCachingProxy::ProcessBlockInfo()
{
    m_block_locations.BuildLookupTable();
}

void DataContextCachingProxy::ForgetAllBlockInfo()
{
    m_block_locations.Clear();
}

void DataContextCachingProxy::ClearCaches()
{
    if (m_wrapped_context)
        m_wrapped_context->ClearCaches();

    // Invalidate all cached pages
    m_free_physical_pages.clear();
    m_page_map.clear();
    m_lru_list.clear();
}

void DataContextCachingProxy::ClearBlockCache(uint32_t block_id, mpsize_t payloadSize)
{
    if (m_wrapped_context)
        m_wrapped_context->ClearBlockCache(block_id, payloadSize);

    // If there is no payload, there are no pages to clear
    if (payloadSize == 0)
        return;

    // Determine the page number of the very last byte of the payload
    uint32_t last_page_num = static_cast<uint32_t>((payloadSize - 1) / m_page_size);

    // Loop through all potential pages for this block's payload
    for (uint32_t page_num = 0; page_num <= last_page_num; ++page_num)
    {
        page_id_type p_id = create_page_id(block_id, page_num);
        
        // Locate the page in the cache map
        auto it = m_page_map.find(p_id);
        if (it != m_page_map.end())
        {
            // Save the physical index for reuse
            m_free_physical_pages.push_back(it->second.physical_page_index);

            // Remove it from the LRU list using its stored iterator
            m_lru_list.erase(it->second.lru_iterator);
            
            // Remove it from the page map
            m_page_map.erase(it);
        }
    }
}

mpsize_t DataContextCachingProxy::ReadBlockDataToBuffer(uint32_t block_id, mpsize_t o, mpsize_t s, uint8_t* b, mpsize_t bs)
{
    if (!m_wrapped_context || !b || s > bs)
        return 0;

    const BlockLocationInfo* block_info_ptr = m_block_locations.Find(block_id);
    mpdatasz_t absolute_offset;
    if (!block_info_ptr ||
        !ResolveBlockRelative(block_info_ptr->payloadAbsOffset, block_info_ptr->payloadSize, o, s, absolute_offset))
        return 0;

    // Apply proxy bounds before using native block reads.
    if (m_use_block_relative_reads)
        return m_wrapped_context->ReadBlockDataToBuffer(block_id, o, s, b, bs);

    // Read using the validated absolute offset.
    return m_wrapped_context->ReadAbsDataToBuffer(absolute_offset, s, b, bs);
}

inline DataContextCachingProxy::PageInfo* DataContextCachingProxy::FetchPageIntoCache(uint32_t block_id, const BlockLocationInfo* block_info_ptr, page_id_type page_id, uint32_t page_num)
{
    // 1. Find the block's metadata
    if (!block_info_ptr)
        block_info_ptr = m_block_locations.Find(block_id);
    if (!block_info_ptr)
        return nullptr;
    const BlockLocationInfo& block_info = *block_info_ptr;

    // 2. Find a physical page slot to use (evicting if necessary).
    mpsize_t physical_page_index = 0;
    bool reclaim_slot_on_failure = false;
    if (!m_free_physical_pages.empty())
    {
        physical_page_index = m_free_physical_pages.back();
        m_free_physical_pages.pop_back();
        reclaim_slot_on_failure = true;
    }
    else if (m_page_map.size() >= m_num_pages)
    {
        // Cache is full. Find the LRU page that is not pinned.
        bool page_evicted = false;
        auto lru_it = m_lru_list.rbegin(); // Start from the back (LRU)
        while (lru_it != m_lru_list.rend())
        {
            page_id_type lru_page_id = *lru_it;
            PageInfo& candidate_info = m_page_map.at(lru_page_id);
            if (!candidate_info.IsPinned())
            {
                // Found one. Evict this page.
                physical_page_index = candidate_info.physical_page_index;
                m_page_map.erase(lru_page_id);
                m_lru_list.erase(ntd::next(lru_it).base());
                page_evicted = true;
                reclaim_slot_on_failure = true;
                break;
            }
            ++lru_it; // Check the next-most-LRU page
        }

        if (!page_evicted)
        {
            // Nested reads can pin every page.
            return nullptr;
        }
    }
    else
    {
        // Cache is not full, use the next available physical slot.
        physical_page_index = to_mpsize(m_page_map.size());
    }

    ScopeExit slot_rollback([&]()
    {
        if (reclaim_slot_on_failure)
            m_free_physical_pages.push_back(physical_page_index);
    });

    // 3. Calculate the absolute offset and size for the read.
    mpsize_t read_offset_rel = static_cast<mpsize_t>(page_num) * m_page_size;
    if (block_info.payloadAbsOffset > max_mpdatasz_t - read_offset_rel)
        return nullptr;
    mpdatasz_t read_offset_abs = block_info.payloadAbsOffset + read_offset_rel;
    mpsize_t remaining_payload = block_info.payloadSize - read_offset_rel;
    mpsize_t bytes_to_read = ntd::min(m_page_size, remaining_payload);
    if (bytes_to_read == 0)
        return nullptr;

    // 4. Get a pointer to the destination slot in our cache pool.
    uint8_t* destination_buffer = m_cache_pool.data() + (physical_page_index * m_page_size);

    // 5. Use specialized methods for an efficient single-copy read.
    mpsize_t bytes_read = m_use_block_relative_reads
        ? m_wrapped_context->ReadBlockDataToBuffer(block_id, read_offset_rel, bytes_to_read, destination_buffer, bytes_to_read)
        : m_wrapped_context->ReadAbsDataToBuffer(read_offset_abs, bytes_to_read, destination_buffer, bytes_to_read);
    if (bytes_read != bytes_to_read)
        return nullptr; // The underlying read failed.

    // 6. Update the LRU list and page map.
    m_lru_list.push_front(page_id);
    // pin_mask member is default-initialized to 0
    auto [map_it, success] = m_page_map.emplace(page_id, PageInfo{page_id, physical_page_index, m_lru_list.begin()});
    slot_rollback.Release();
    return &map_it->second;
}

uint8_t* DataContextCachingProxy::GetBlockDataPtr(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    mpsize_t active_lock_level = GetActiveLockLevel();
    DeactivateLock();

    // Validate before computing page ranges.
    const BlockLocationInfo* block_info_ptr = m_block_locations.Find(block_id);
    if (dataSize == 0 || !block_info_ptr ||
        !range_within(payloadOffsetRelative, dataSize, block_info_ptr->payloadSize))
    {
        if (out_readyToAccessLen)
            *out_readyToAccessLen = 0;
        return nullptr;
    }

    // 1. Determine which pages are needed for this request.
    const uint32_t start_page_num = static_cast<uint32_t>(payloadOffsetRelative / m_page_size);
    const mpsize_t end_offset_relative = payloadOffsetRelative + dataSize - 1;
    const uint32_t end_page_num = static_cast<uint32_t>(end_offset_relative / m_page_size);
    const bool multi_page = (end_page_num > start_page_num);

    const uint64_t required_pages =
        static_cast<uint64_t>(end_page_num) - start_page_num + 1;
    if (required_pages > m_num_pages)
    {
        // Read requests wider than the cache directly.
        auto& active_cross_page_buffer = m_cross_page_buffers[active_lock_level];
        resize_if_needed(active_cross_page_buffer, dataSize);
        const mpsize_t bytes_read = ReadBlockDataToBuffer(
            block_id, payloadOffsetRelative, dataSize,
            active_cross_page_buffer.data(), dataSize);
        if (bytes_read != dataSize)
            return nullptr;
        if (out_readyToAccessLen)
            *out_readyToAccessLen = dataSize;
        return active_cross_page_buffer.data();
    }
    
    // Use protected_pages to store pointers for the required pages.
    // This serves as a temporary "lookup cache".
    protected_pages.clear();
    protected_pages.reserve(end_page_num - start_page_num + 1);
    
    ScopeExit protected_pages_cleanup([&]()
    {
        if (!multi_page)
            return;
        // Remove temporary protection.
        for (PageInfo* page : protected_pages)
            page->pin_mask &= ~kPageProtectionBit;
    });

    // 2. Ensure all required pages are in the cache, fetching them if they are not.
    page_id_type p_id = create_page_id(block_id, start_page_num);

    for (uint32_t p_num = start_page_num; p_num <= end_page_num; ++p_num)
    {
        PageInfo* current_page_info = nullptr;

        auto it = m_page_map.find(p_id);
        if (it == m_page_map.end())
        {
            // CACHE MISS: Fetch the page from the real source.
            current_page_info = FetchPageIntoCache(block_id, block_info_ptr, p_id, p_num);
            if (current_page_info == nullptr)
                return nullptr; // Failed to fetch a required page.
        }
        else
        {
            // CACHE HIT: Update its position in the LRU list to mark it as recently used.
            m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second.lru_iterator);
            current_page_info = &it->second;
        }

        // Store the pointer so we don't have to find it again later.
        protected_pages.push_back(current_page_info);

        if (multi_page)
        {
            // Use a high bit to mark the page as temporarily pinned so subsequent 
            // fetches in this loop don't evict *this* page.
            current_page_info->pin_mask |= kPageProtectionBit;
        }

        p_id++;
    }

    // 3. At this point, all required pages are guaranteed to be in the cache.
    // We use the pointers stored in protected_pages to access data without map lookups.
    const mpsize_t offset_in_start_page = payloadOffsetRelative % m_page_size;
    uint8_t* result_ptr = nullptr;

    if (start_page_num == end_page_num)
    {
        // CASE A: The entire request fits within a single cache page (most common).
        // Retrieve the pointer directly from our vector.
        PageInfo* info = protected_pages[0];

        // If pinning mode is active, mark this page with the current valid generation.
        if (active_lock_level)
            PinPage(*info, active_lock_level);

        if (out_readyToAccessLen)
        {
            // Exclude the unused tail of the final page.
            const mpsize_t bytes_in_page = m_page_size - offset_in_start_page;
            const mpsize_t bytes_in_payload = block_info_ptr->payloadSize - payloadOffsetRelative;
            *out_readyToAccessLen = ntd::min(bytes_in_page, bytes_in_payload);
        }

        result_ptr = m_cache_pool.data() + (info->physical_page_index * m_page_size) + offset_in_start_page;
    }
    else
    {
        // CASE B: The request spans multiple pages and requires copying to a temporary buffer.
        auto& active_cross_page_buffer = m_cross_page_buffers[active_lock_level];
        resize_if_needed(active_cross_page_buffer, dataSize);
        mpsize_t bytes_copied = 0;

        // Unified loop that handles the first, middle, and last pages using the stored pointers.
        for (mpsize_t i = 0; i < protected_pages.size(); ++i)
        {
            // The source offset is non-zero only for the very first page in our range.
            const mpsize_t source_offset_in_page = (i == 0) ? offset_in_start_page : 0;

            // Get the metadata for the current page directly from the vector.
            const PageInfo* p_info = protected_pages[i];
            const uint8_t* source_page_ptr = m_cache_pool.data() + (p_info->physical_page_index * m_page_size);

            // Determine how many bytes to copy from this specific page.
            const mpsize_t bytes_available_in_page = m_page_size - source_offset_in_page;
            const mpsize_t bytes_remaining_for_request = dataSize - bytes_copied;
            const mpsize_t bytes_to_copy = ntd::min(bytes_available_in_page, bytes_remaining_for_request);

            // Perform the copy for this iteration.
            if (bytes_to_copy > 0)
            {
                ntd::memcpy(active_cross_page_buffer.data() + bytes_copied, source_page_ptr + source_offset_in_page, bytes_to_copy);
                bytes_copied += bytes_to_copy;
            }
        }

        if (out_readyToAccessLen)
            *out_readyToAccessLen = dataSize;
        result_ptr = active_cross_page_buffer.data();
    }

    return result_ptr;
}

void DataContextCachingProxy::PushDataLockImpl()
{
}

void DataContextCachingProxy::PopDataLockImpl()
{
    // Keep the size of m_cross_page_buffers[m_lock_level] intact so it can be reused
    
    auto& pages = m_pinned_pages[m_lock_level];
    for (auto page_id : pages)
    {
        auto it = m_page_map.find(page_id);
        if (it != m_page_map.end())
        {
            UnpinPage(it->second, m_lock_level);
        }
    }
    pages.clear();
}

inline void DataContextCachingProxy::PinPage(PageInfo& info, mpsize_t pin_level)
{
    m_pinned_pages[pin_level].push_back(info.page_id);
    info.pin_mask = set_bit(info.pin_mask, pin_level);
}

inline void DataContextCachingProxy::UnpinPage(PageInfo& info, mpsize_t pin_level)
{
    info.pin_mask = clear_bit(info.pin_mask, pin_level);
}

inline bool DataContextCachingProxy::PageInfo::IsPinned()
{
    return !!pin_mask;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
DataContextMemChunks::DataContextMemChunks()
{
    m_block_locations.LimitDirectLookupSizeByMaxId(false);
}

bool DataContextMemChunks::AssignMemChunks(const uint8_t* const* chunk_ptrs, const mpsize_t* chunk_sizes, mpsize_t num_chunks)
{
    if (num_chunks > 0 && (!chunk_ptrs || !chunk_sizes))
        return false;
    mpdatasz_t validated_total_size = 0;
    for (mpsize_t i = 0; i < num_chunks; ++i)
    {
        if ((!chunk_ptrs[i] && chunk_sizes[i] > 0) ||
            validated_total_size > max_mpdatasz_t - chunk_sizes[i])
            return false;
        validated_total_size += chunk_sizes[i];
    }
    m_chunk_ptrs = chunk_ptrs;
    m_chunk_sizes = chunk_sizes;
    m_uses_borrowed_chunk_arrays = num_chunks > 0;
    m_num_chunks = num_chunks;
    
    m_chunks.clear();
    m_sizes.clear();

    m_abs_lookup_table.clear();
    m_abs_lookup_region_size = 0;
    m_block_locations.Clear();

    m_chunk_start_offsets.resize(num_chunks);
    mpdatasz_t current_offset = 0;
    for (mpsize_t i = 0; i < num_chunks; ++i)
    {
        m_chunk_start_offsets[i] = current_offset;
        current_offset += chunk_sizes[i];
    }
    m_total_size = current_offset;
    return true;
}

bool DataContextMemChunks::AddMemChunks(const uint8_t* const* chunk_ptrs, const mpsize_t* chunk_sizes, mpsize_t num_chunks)
{
    if (num_chunks == 0)
        return true;
    if (m_uses_borrowed_chunk_arrays || !chunk_ptrs || !chunk_sizes)
        return false;

    mpdatasz_t added_size = 0;
    for (mpsize_t i = 0; i < num_chunks; ++i)
    {
        if ((!chunk_ptrs[i] && chunk_sizes[i] > 0) ||
            added_size > max_mpdatasz_t - chunk_sizes[i])
            return false;
        added_size += chunk_sizes[i];
    }
    if (m_total_size > max_mpdatasz_t - added_size || num_chunks > max_mpsize_t - m_num_chunks)
        return false;

    m_num_chunks += num_chunks;

    m_chunks.reserve(m_num_chunks);
    m_sizes.reserve(m_num_chunks);
    m_chunk_start_offsets.reserve(m_num_chunks);

    for (mpsize_t i = 0; i < num_chunks; ++i)
    {
        m_chunks.push_back(chunk_ptrs[i]);
        m_sizes.push_back(chunk_sizes[i]);
        m_chunk_start_offsets.push_back(m_total_size);
        m_total_size += chunk_sizes[i];
    }

    m_chunk_ptrs = m_chunks.data();
    m_chunk_sizes = m_sizes.data();
    return true;
}

bool DataContextMemChunks::AddBlockMemChunks(uint32_t block_id, const uint8_t* const* chunk_ptrs, const mpsize_t* chunk_sizes, mpsize_t num_chunks)
{
    if (num_chunks == 0 || !chunk_ptrs || !chunk_sizes)
        return false;

    mpdatasz_t totalSize = 0;
    mpsize_t payloadSize = 0;
    for (mpsize_t i = 0; i < num_chunks; ++i)
    {
        if ((!chunk_ptrs[i] && chunk_sizes[i] > 0) ||
            totalSize > max_mpdatasz_t - chunk_sizes[i])
            return false;
        totalSize += chunk_sizes[i];
        if (i > 0)
        {
            if (chunk_sizes[i] > max_mpsize_t - payloadSize)
                return false;
            payloadSize += chunk_sizes[i];
        }
    }
    if (m_total_size > max_mpdatasz_t - totalSize ||
        m_total_size > max_mpdatasz_t - chunk_sizes[0])
        return false;
    const mpdatasz_t payloadAbsOffset = m_total_size + chunk_sizes[0];

    if (!AddMemChunks(chunk_ptrs, chunk_sizes, num_chunks))
        return false;
    RememberBlockInfoImpl(block_id, payloadAbsOffset, payloadSize, num_chunks);
    return true;
}

void DataContextMemChunks::ClearMemChunks()
{
    (void)AssignMemChunks(nullptr, nullptr, 0);
}

void DataContextMemChunks::ResetScanState()
{
    m_scan_hint_chunk_index = 0;
}

void DataContextMemChunks::ReserveBlockInfo(mpsize_t count)
{
    m_block_locations.Reserve(count);
}

void DataContextMemChunks::ForgetAllBlockInfo()
{
    m_block_locations.Clear();
}

void DataContextMemChunks::RememberBlockInfoImpl(uint32_t block_id, mpdatasz_t payloadAbsOffset, mpsize_t payloadSize, mpsize_t newChunksNum)
{
    constexpr mpsize_t chunkNumberSearchThreshold = 8;

    if (m_chunk_start_offsets.empty())
        return;

    BlockLocationInfo info;
    info.payloadAbsOffset = payloadAbsOffset;
    info.payloadSize = payloadSize;

    if (newChunksNum == 0 && info.payloadSize > 0)
    {
        if (info.payloadAbsOffset >= m_total_size ||
            info.payloadSize > m_total_size - info.payloadAbsOffset)
            return;
        auto it_start = ntd::upper_bound(m_chunk_start_offsets.begin(), m_chunk_start_offsets.end(), info.payloadAbsOffset);
        info.startChunkIndex = to_mpsize(
            ntd::distance(m_chunk_start_offsets.begin(), it_start) - 1);

        auto it_end = ntd::upper_bound(m_chunk_start_offsets.begin(), m_chunk_start_offsets.end(), info.payloadAbsOffset + info.payloadSize - 1);
        info.endChunkIndex = to_mpsize(
            ntd::distance(m_chunk_start_offsets.begin(), it_end) - 1);
    }
    else if (newChunksNum > 1)
    {
        info.startChunkIndex = to_mpsize(
            m_chunk_start_offsets.size() - (newChunksNum - 1));
        info.endChunkIndex = to_mpsize(m_chunk_start_offsets.size() - 1);
    }
    else
    {
        info.startChunkIndex = 0;
        info.endChunkIndex = 0;
    }

    info.hasManyChunks = (info.endChunkIndex > info.startChunkIndex) &&
        (info.endChunkIndex - info.startChunkIndex > chunkNumberSearchThreshold);
    m_block_locations.Emplace(block_id, ntd::move(info));
}

void DataContextMemChunks::RememberBlockInfo(uint32_t block_id, mpdatasz_t payloadAbsOffset, mpsize_t payloadSize)
{
    RememberBlockInfoImpl(block_id, payloadAbsOffset, payloadSize, 0);
}

void DataContextMemChunks::ProcessBlockInfo()
{
    constexpr bool useAbsLookupTable = true;

    m_block_locations.BuildLookupTable();

    // Build the absolute offset lookup table
    if (useAbsLookupTable && m_total_size > 0 && m_num_chunks > 0)
    {
        // Allocate +1 extra entry to safely absorb the maximum possible region_index 
        // without bounds checking during lookup.
        resize_if_needed(m_abs_lookup_table, ABS_LOOKUP_TABLE_SIZE + 1);
        m_abs_lookup_region_size = m_total_size / ABS_LOOKUP_TABLE_SIZE +
            (m_total_size % ABS_LOOKUP_TABLE_SIZE != 0 ? 1 : 0);

        // Fill up to ABS_LOOKUP_TABLE_SIZE + 1
        mpdatasz_t region_start = 0;
        mpsize_t current_chunk = 0;
        for (mpsize_t i = 0; i <= ABS_LOOKUP_TABLE_SIZE; ++i)
        {
            while (current_chunk < m_num_chunks - 1)
            {
                // A chunk overlaps if it ends strictly after the region starts
                if (m_chunk_start_offsets[current_chunk] + m_chunk_sizes[current_chunk] > region_start)
                    break;
                // Don't skip a 0-sized chunk if it sits exactly on the region boundary
                if (m_chunk_start_offsets[current_chunk] == region_start)
                    break;
                current_chunk++;
            }
            
            m_abs_lookup_table[i] = current_chunk;
            region_start = region_start > max_mpdatasz_t - m_abs_lookup_region_size
                ? max_mpdatasz_t
                : region_start + m_abs_lookup_region_size;
        }
    }    
}

mpsize_t DataContextMemChunks::GetAbsStartChunk(mpdatasz_t absolute_offset)
{
    mpsize_t start_chunk = 0;
    if (m_abs_lookup_region_size > 0)
    {
        start_chunk = m_abs_lookup_table[to_mpsize(
            absolute_offset / m_abs_lookup_region_size)];
    }
    else
    {
        auto it = ntd::upper_bound(m_chunk_start_offsets.begin(), m_chunk_start_offsets.end(), absolute_offset);
        start_chunk = to_mpsize(
            ntd::distance(m_chunk_start_offsets.begin(), it) - 1);
    }
    return start_chunk;
}

uint8_t* DataContextMemChunks::GetBlockDataPtr(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    const BlockLocationInfo* info = m_block_locations.Find(block_id);
    mpdatasz_t absolute_offset;
    if (!info || !ResolveBlockRelative(info->payloadAbsOffset, info->payloadSize, payloadOffsetRelative, dataSize, absolute_offset))
    {
        if (out_readyToAccessLen)
            *out_readyToAccessLen = 0;
        DeactivateLock();
        return nullptr;
    }

    return GetDataPtr_Ranged(absolute_offset, dataSize, out_readyToAccessLen, info->hasManyChunks, info->startChunkIndex, info->endChunkIndex, nullptr);
}

mpsize_t DataContextMemChunks::ReadBlockDataToBuffer(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size)
{
    const BlockLocationInfo* info = m_block_locations.Find(block_id);
    mpdatasz_t absolute_offset;
    if (!info || !destination_buffer || dataSize > destination_buffer_size ||
        !ResolveBlockRelative(info->payloadAbsOffset, info->payloadSize, payloadOffsetRelative, dataSize, absolute_offset))
        return 0;

    mpsize_t copied_size = 0;
    if (!GetDataPtr_Ranged(absolute_offset, dataSize, &copied_size, info->hasManyChunks, info->startChunkIndex, info->endChunkIndex, nullptr, destination_buffer))
        return 0;

    return copied_size;
}

uint8_t* DataContextMemChunks::GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    if (m_num_chunks == 0 || !range_within<mpdatasz_t>(absolute_offset, dataSize, m_total_size))
    {
        if (out_readyToAccessLen)
            *out_readyToAccessLen = 0;
        DeactivateLock();
        return nullptr;
    }

    // For an absolute request, the end chunk is unknown, so iterate to the end.
    mpsize_t start_chunk = GetAbsStartChunk(absolute_offset);
    return GetDataPtr_Ranged(absolute_offset, dataSize, out_readyToAccessLen, false, start_chunk, m_num_chunks - 1, nullptr);
}

mpsize_t DataContextMemChunks::ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size)
{
    if (m_num_chunks == 0 || !destination_buffer || dataSize > destination_buffer_size ||
        !range_within<mpdatasz_t>(absolute_offset, dataSize, m_total_size))
        return 0;

    // For an absolute request, the end chunk is unknown, so iterate to the end.
    mpsize_t copied_size = 0;
    mpsize_t start_chunk = GetAbsStartChunk(absolute_offset);
    if (!GetDataPtr_Ranged(absolute_offset, dataSize, &copied_size, false, start_chunk, m_num_chunks - 1, nullptr, destination_buffer))
        return 0;

    return copied_size;
}

uint8_t* DataContextMemChunks::GetNextAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    if (m_num_chunks == 0 || !range_within<mpdatasz_t>(absolute_offset, dataSize, m_total_size))
    {
        if (out_readyToAccessLen)
            *out_readyToAccessLen = 0;
        DeactivateLock();
        return nullptr;
    }

    // Ignore stale or forward scan hints.
    mpsize_t start_chunk = m_scan_hint_chunk_index;
    if (start_chunk >= m_num_chunks || m_chunk_start_offsets[start_chunk] > absolute_offset)
        start_chunk = GetAbsStartChunk(absolute_offset);

    return GetDataPtr_Ranged(absolute_offset, dataSize, out_readyToAccessLen, false, start_chunk, m_num_chunks - 1, &m_scan_hint_chunk_index);
}

inline uint8_t* DataContextMemChunks::GetDataPtr_Ranged(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen, bool search_start_chunk, mpsize_t start_chunk, mpsize_t end_chunk, mpsize_t* out_end_chunk, uint8_t* dest_copy_buf)
{
    mpsize_t active_lock_level = GetActiveLockLevel();
    if (!dest_copy_buf)
        DeactivateLock();

    if (out_readyToAccessLen)
        *out_readyToAccessLen = 0;

    if (search_start_chunk)
    {
        using ChunkOffsetDifference = typename decltype(m_chunk_start_offsets)::difference_type;
        auto search_start = m_chunk_start_offsets.begin()
            + static_cast<ChunkOffsetDifference>(start_chunk);
        auto search_end = m_chunk_start_offsets.begin()
            + static_cast<ChunkOffsetDifference>(end_chunk) + 1; // end iterator is exclusive
        
        auto it = ntd::upper_bound(search_start, search_end, absolute_offset);
        start_chunk = to_mpsize(
            ntd::distance(m_chunk_start_offsets.begin(), it) - 1);
    }

    for (mpsize_t i = start_chunk; i <= end_chunk && i < m_num_chunks; ++i)
    {
        if (absolute_offset >= m_chunk_start_offsets[i] &&
            absolute_offset - m_chunk_start_offsets[i] < m_chunk_sizes[i])
        {
            const mpsize_t offset_in_chunk = to_mpsize(
                absolute_offset - m_chunk_start_offsets[i]);
            const mpsize_t remaining_in_chunk = m_chunk_sizes[i] - offset_in_chunk;
            if (remaining_in_chunk >= dataSize)
            {
                if (out_end_chunk)
                    *out_end_chunk = i;
                if (dest_copy_buf)
                {
                    if (out_readyToAccessLen)
                        *out_readyToAccessLen = dataSize;
                    ntd::memcpy(dest_copy_buf, m_chunk_ptrs[i] + offset_in_chunk, dataSize);
                    return dest_copy_buf;
                }
                else
                {
                    if (out_readyToAccessLen)
                        *out_readyToAccessLen = remaining_in_chunk;
                    return const_cast<uint8_t*>(m_chunk_ptrs[i] + offset_in_chunk);
                }
            }

            if (!dest_copy_buf)
            {
                auto& active_temp_buffer = m_temp_buffers[active_lock_level];
                resize_if_needed(active_temp_buffer, dataSize);
                dest_copy_buf = active_temp_buffer.data();
            }
            mpsize_t bytes_copied = 0;
            mpsize_t last_chunk = i;
            // Reject invalid chunk metadata.
            for (mpsize_t j = i; j <= end_chunk && j < m_num_chunks && bytes_copied < dataSize; ++j)
            {
                const mpsize_t source_offset = (j == i) ? offset_in_chunk : 0;
                if (source_offset > m_chunk_sizes[j])
                    break;
                const uint8_t* source_ptr = m_chunk_ptrs[j] + source_offset;
                const mpsize_t bytes_available_in_chunk = m_chunk_sizes[j] - source_offset;
                const mpsize_t bytes_to_copy = ntd::min(dataSize - bytes_copied, bytes_available_in_chunk);
                ntd::memcpy(dest_copy_buf + bytes_copied, source_ptr, bytes_to_copy);
                bytes_copied += bytes_to_copy;
                last_chunk = j;
            }
            if (bytes_copied != dataSize)
                return nullptr;
            if (out_readyToAccessLen)
                *out_readyToAccessLen = dataSize;
            if (out_end_chunk)
                *out_end_chunk = last_chunk;
            return dest_copy_buf;
        }
    }

    return nullptr;
}

void DataContextMemChunks::PushDataLockImpl()
{
}

void DataContextMemChunks::PopDataLockImpl()
{
    // Keep the size of m_temp_buffers[m_lock_level] intact so it can be reused
}

////////////////////////////////////////////////////////////////////////////////////////////////////
DataContextExternalIO::DataContextExternalIO()
{
    AssignIO(GetDefaultIO());
}

DataContextExternalIO::~DataContextExternalIO()
{
    if (is_open && m_io.close)
        m_io.close(m_handle);
}

static mpdatasz_t GprxExtDataGetSizeWrapper(uint8_t* handle)
{
    U64Split size{};
    if (!GprxExtDataGetSize(handle, &size.parts.low, &size.parts.high))
        return 0;
    return size.entry;
}

static mpsize_t GprxExtDataReadWrapper(
    uint8_t* handle,
    uint8_t* dst,
    uint32_t block_id,
    mpdatasz_t offset,
    mpsize_t size)
{
    U64Split offset_split;
    offset_split.entry = offset;
    return GprxExtDataRead(
        handle,
        dst,
        block_id,
        offset_split.parts.low,
        offset_split.parts.high,
        size);
}

ExternalIO DataContextExternalIO::GetDefaultIO()
{
    ExternalIO io;
    io.open = GprxExtDataOpen;
    io.close = GprxExtDataClose;
    io.getSize = GprxExtDataGetSizeWrapper;
    io.read = GprxExtDataReadWrapper;
    io.isBlockFetchSupported = false;
    return io;
}

void DataContextExternalIO::AssignIO(const ExternalIO& io)
{
    if (is_open && m_io.close)
        m_io.close(m_handle);
    is_open = false;
    m_handle = nullptr;
    m_size = 0;
    m_io = io;
}

bool DataContextExternalIO::Open(uint8_t* handle)
{
    if (is_open && m_io.close)
        m_io.close(m_handle);
    is_open = false;

    // Require every callback used after opening.
    if (!m_io.open || !m_io.close || !m_io.getSize || !m_io.read)
        return false;

    m_handle = handle;
    is_open = !!m_io.open(m_handle);
    if (!is_open)
        return false;

    m_size = m_io.getSize(m_handle);
    return true;
}

void DataContextExternalIO::SyncWithDataSource()
{
    if (is_open && m_io.getSize)
        m_size = m_io.getSize(m_handle);
}

uint8_t* DataContextExternalIO::GetAbsDataPtr(mpdatasz_t absolute_offset, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    return GetAbsDataPtrUnified(*this, absolute_offset, dataSize, out_readyToAccessLen);
}

mpsize_t DataContextExternalIO::ReadAbsDataToBuffer(mpdatasz_t absolute_offset, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size)
{
    if (!is_open || !m_io.read || !destination_buffer || dataSize > destination_buffer_size ||
        !range_within<mpdatasz_t>(absolute_offset, dataSize, m_size))
        return 0;
    const mpsize_t bytes_read = m_io.read(m_handle, destination_buffer, 0, absolute_offset, dataSize);
    // Reject invalid callback results.
    return bytes_read > dataSize ? 0 : bytes_read;
}

uint8_t* DataContextExternalIO::GetBlockDataPtr(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, mpsize_t* out_readyToAccessLen)
{
    if (out_readyToAccessLen)
        *out_readyToAccessLen = 0;
    mpsize_t active_lock_level = GetActiveLockLevel();
    DeactivateLock();

    if (!is_open || !m_io.isBlockFetchSupported)
        return nullptr;

    auto& active_temp_buffer = m_temp_buffers[active_lock_level];
    resize_if_needed(active_temp_buffer, dataSize);
    mpsize_t bytes_read = ReadBlockDataToBuffer(
        block_id,
        payloadOffsetRelative,
        dataSize,
        active_temp_buffer.data(),
        to_mpsize(active_temp_buffer.size()));

    if (bytes_read != dataSize)
        return nullptr;

    if (out_readyToAccessLen)
        *out_readyToAccessLen = dataSize;
    return active_temp_buffer.data();
}

mpsize_t DataContextExternalIO::ReadBlockDataToBuffer(uint32_t block_id, mpsize_t payloadOffsetRelative, mpsize_t dataSize, uint8_t* destination_buffer, mpsize_t destination_buffer_size)
{
    if (!is_open || !m_io.read || !destination_buffer || dataSize > destination_buffer_size || !m_io.isBlockFetchSupported)
        return 0;
    const mpsize_t bytes_read = m_io.read(m_handle, destination_buffer, block_id, payloadOffsetRelative, dataSize);
    // Reject invalid callback results.
    return bytes_read > dataSize ? 0 : bytes_read;
}

void DataContextExternalIO::PushDataLockImpl()
{
}

void DataContextExternalIO::PopDataLockImpl()
{
    // Keep the size of m_temp_buffers[m_lock_level] intact so it can be reused
}

} // GPRX

////////////////////////////////////////////////////////////////////////////////////////////////////
#if !ENABLE_EXTERNAL_DATA_CONTEXT

uint32_t GprxExtDataOpen(uint8_t* /*handle*/)
{
    return 0;
}

uint32_t GprxExtDataClose(uint8_t* /*handle*/)
{
    return 0;
}

uint32_t GprxExtDataGetSize(
    uint8_t* /*handle*/,
    uint32_t* /*out_size_low*/,
    uint32_t* /*out_size_high*/)
{
    return 0;
}

mpsize_t GprxExtDataRead(
    uint8_t* /*handle*/,
    uint8_t* /*dst*/,
    uint32_t /*block_id*/,
    uint32_t /*offset_low*/,
    uint32_t /*offset_high*/,
    mpsize_t /*sz*/)
{
    return 0;
}

#endif // !ENABLE_EXTERNAL_DATA_CONTEXT

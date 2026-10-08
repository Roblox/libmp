// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "utils.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/allocator.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/deque.h"
#include "ntd_stdlib/tuple.h"
#include "ntd_stdlib/type_traits.h"
#include "ntd_stdlib/unordered_map.h"
#include "ntd_stdlib/unordered_set.h"
#include "ntd_stdlib/utility.h"
#include "ntd_stdlib/vector.h"

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
static constexpr mpsize_t GLOBAL_ZERO_BUFFER_LEN = 64 * 1024;
extern const uint8_t kGlobalZeroBuffer[GLOBAL_ZERO_BUFFER_LEN];

////////////////////////////////////////////////////////////////////////////////////////////////////
// Runs a callback when the current scope exits unless ownership is moved or explicitly released.
template <typename Callback>
class ScopeExit
{
public:
    explicit ScopeExit(Callback callback)
        : m_callback(ntd::move(callback))
    {
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ScopeExit(ScopeExit&& other)
        : m_callback(ntd::move(other.m_callback))
        , m_active(other.m_active)
    {
        other.m_active = false;
    }

    ~ScopeExit()
    {
        if (m_active)
            m_callback();
    }

    void Release()
    {
        m_active = false;
    }

private:
    Callback m_callback;
    bool m_active = true;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// --- raw binary <--> hex ascii string ---
void SerializeToHex(const void* data, mpsize_t size, uint8_t* out_hex_buf);
// Returns false if hex_size is odd or any input character is not a hex digit.
bool DeserializeFromHex(const uint8_t* hex_buf, mpsize_t hex_size, void* out_data);

////////////////////////////////////////////////////////////////////////////////////////////////////
struct MemChunk
{
    uint8_t* ptr = nullptr;
    mpsize_t usedBytes = 0;
    mpsize_t totalBytes = 0;
    mpsize_t allocatedBytes = 0;
    bool bForceZero = false;
    bool bOwnsPtr = false;

    inline void AssignMemUnsafe(uint8_t* new_ptr, mpsize_t new_used, mpsize_t new_total = 0)
    {
        ptr = new_ptr;
        usedBytes = new_used;
        totalBytes = new_total ? new_total : new_used;
    }

    void AssignMem(uint8_t* new_ptr, mpsize_t new_used, mpsize_t new_total = 0)
    {
        Free();
        AssignMemUnsafe(new_ptr, new_used, new_total);
    }

    bool Allocate(mpsize_t size_to_allocate)
    {
        if (allocatedBytes >= size_to_allocate)
        {
            usedBytes = size_to_allocate;
            totalBytes = size_to_allocate;
            return true;
        }

        Free();
        if (size_to_allocate > 0)
        {
            ptr = reinterpret_cast<uint8_t*>(ntd::mem::Alloc(size_to_allocate));
            if (!ptr)
                return false;
            usedBytes = size_to_allocate;
            totalBytes = size_to_allocate;
            allocatedBytes = size_to_allocate;
            bOwnsPtr = true;
        }
        return true;
    }

    void Free()
    {
        if (bOwnsPtr)
            ntd::mem::Free(ptr);
        ptr = nullptr;
        usedBytes = 0;
        totalBytes = 0;
        allocatedBytes = 0;
        bForceZero = false;
        bOwnsPtr = false;
    }

    // --- ntd::vector-like API ---
    const uint8_t* data() const { return ptr; }
    uint8_t* data() { return ptr; }
    mpsize_t size() const { return usedBytes; }
    mpsize_t capacity() const { return totalBytes; }
    bool empty() const { return usedBytes == 0; }

    MemChunk() = default;

    // --- Rule of Five to handle ownership correctly ---
    // 1. Destructor
    ~MemChunk()
    {
        Free();
    }
    // 2. Copy constructor (deep copy)
    MemChunk(const MemChunk& other) = delete;
    // 3. Copy assignment (deep copy)
    MemChunk& operator=(const MemChunk& other) = delete;
    // 4. Move constructor
    MemChunk(MemChunk&& other) noexcept
    {
        Free();
        ptr = other.ptr;
        usedBytes = other.usedBytes;
        totalBytes = other.totalBytes;
        allocatedBytes = other.allocatedBytes;
        bForceZero = other.bForceZero;
        bOwnsPtr = other.bOwnsPtr;
        // The moved-from object no longer owns the pointer
        other.ptr = nullptr;
        other.usedBytes = 0;
        other.totalBytes = 0;
        other.allocatedBytes = 0;
        other.bForceZero = false;
        other.bOwnsPtr = false;
    }
    // 5. Move assignment
    MemChunk& operator=(MemChunk&& other) noexcept
    {
        if (this != &other)
        {
            Free();
            ptr = other.ptr;
            usedBytes = other.usedBytes;
            totalBytes = other.totalBytes;
            allocatedBytes = other.allocatedBytes;
            bForceZero = other.bForceZero;
            bOwnsPtr = other.bOwnsPtr;
            // The moved-from object no longer owns the pointer
            other.ptr = nullptr;
            other.usedBytes = 0;
            other.totalBytes = 0;
            other.allocatedBytes = 0;
            other.bForceZero = false;
            other.bOwnsPtr = false;
        }
        return *this;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
using MemChunkList = ntd::deque<MemChunk>;

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Processes a sequence of memory chunks up to a total specified size.
 * @param process_action A function/lambda called for each relevant chunk segment.
 * @return true if all necessary chunks were processed successfully.
 */
template <typename ProcessAction>
inline bool ProcessChunkSequence(
    const MemChunkList& chunks,
    mpsize_t total_size_to_process,
    ProcessAction&& process_action)
{
    mpsize_t bytes_left_to_process = total_size_to_process;
    for (const auto& chunk : chunks)
    {
        if (bytes_left_to_process == 0)
            break;
        mpsize_t size_from_this_chunk = ntd::min(bytes_left_to_process, chunk.size());
        if (chunk.bForceZero)
        {
            mpsize_t zeros_left = size_from_this_chunk;
            while (zeros_left > 0)
            {
                // Feed the zeroes in sizes up to our allocated vector capacity
                mpsize_t to_process = ntd::min(zeros_left, GLOBAL_ZERO_BUFFER_LEN);
                if (!process_action(kGlobalZeroBuffer, to_process))
                    return false; // Action failed, stop processing
                zeros_left -= to_process;
            }
        }
        else
        {
            if (size_from_this_chunk > 0 && !chunk.data())
                return false;
            if (!process_action(chunk.data(), size_from_this_chunk))
                return false; // Action failed, stop processing
        }
        bytes_left_to_process -= size_from_this_chunk;
    }
    return bytes_left_to_process == 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A template class that combines a fast, direct-access lookup table
 * (ntd::vector) with a standard ntd::unordered_map for sparse data.
 *
 * This class owns the data in an ntd::unordered_map.
 * When BuildLookupTable() is called, it creates a ntd::vector that holds
 * non-owning pointers to the values stored within the map.
 *
 * - Find() operations will first try the vector for O(1) access and fall
 * back to the map if the ID is outside the vector's range.
 * - Insert() operations will invalidate the fast lookup table (as the map
 * may rehash, or the ID range may change), requiring a call to
 * BuildLookupTable() to reactivate it.
 * - Erase() operations are safe and will null out the entry in the fast
 * lookup table without invalidating the whole table.
 *
 * @tparam TValue The type of the value to be stored. It can be a pointer type.
 */
template <typename TValue>
class HybridLookupTable
{
public:
    static constexpr mpsize_t DIRECT_LOOKUP_THRESHOLD = 65536;
    static constexpr mpsize_t FAST_CLEAR_FILL_FRACTION = 4;

    // --- Type definitions for correct pointer handling ---

    // The "real" value type, stripping any top-level pointer
    // e.g., TValue=MyObject -> MyObject
    // e.g., TValue=MyObject* -> MyObject
    using StoredValueType = ntd::remove_pointer_t<TValue>;

    // The type we'll return from non-const Find()
    // e.g., MyObject*
    using ReturnPointerType = StoredValueType *;

    // The type we'll return from const Find()
    // e.g., const MyObject*
    using ConstReturnPointerType = const StoredValueType *;

    // The type we'll store in the m_lookup_table vector
    // e.g., MyObject*
    using LookupPointerType = StoredValueType *;

    // Expose the map's iterator types
    using iterator = typename ntd::unordered_map<uint32_t, TValue>::iterator;
    using const_iterator = typename ntd::unordered_map<uint32_t, TValue>::const_iterator;

    /**
     * @brief Constructs a HybridLookupTable.
     *
     * @param threshold The maximum size (number of elements) to allocate for
     * the fast lookup table.
     * @param limitSizeByMaxId If true, limit direct lookup table size by
     * the max relative element ID.
     */
    HybridLookupTable(mpsize_t threshold = DIRECT_LOOKUP_THRESHOLD, bool limitSizeByMaxId = true)
        : m_start_id(0)
        , m_threshold(threshold)
        , m_limit_by_max_id(limitSizeByMaxId)
    {
    }

    void LimitDirectLookupSizeByMaxId(bool enabled)
    {
        m_limit_by_max_id = enabled;
    }

    void Reserve(mpsize_t count)
    {
        m_map.reserve(count);
    }

    ntd::pair<iterator, bool> Insert(uint32_t id, TValue value)
    {
        ClearLookupTable();
        return m_map.emplace(id, ntd::move(value));
    }

    template <typename... Args>
    ntd::pair<iterator, bool> Emplace(uint32_t id, Args &&...args)
    {
        ClearLookupTable();
        return m_map.emplace(ntd::piecewise_construct,
            ntd::forward_as_tuple(id),
            ntd::forward_as_tuple(ntd::forward<Args>(args)...));
    }

    /**
     * @brief Erases an element from the map by key.
     * @note This operation is "safe" and will not invalidate the entire
     * fast lookup table. It will null out the specific pointer in the
     * table if it exists.
     * @param id The key (ID) of the element to erase.
     * @return The number of elements erased (0 or 1).
     */
    mpsize_t Erase(uint32_t id)
    {
        if (!m_is_lookup_clear && id >= m_start_id)
        {
            uint32_t relative_id = id - m_start_id;
            if (relative_id < m_lookup_table.size())
                m_lookup_table[relative_id] = nullptr;
        }

        return static_cast<mpsize_t>(m_map.erase(id));
    }

    /**
     * @brief Clears all data from both the map and the lookup table.
     */
    void Clear()
    {
        ClearLookupTable();
        m_map.clear();
    }

    /**
     * @brief Builds (or rebuilds) the fast, direct-access lookup table.
     * This function iterates over the internal map to find the ID range
     * and populates the vector with pointers to the map's values.
     */
    void BuildLookupTable()
    {
        ClearLookupTable();

        if (m_map.empty())
            return;

        uint32_t min_id = max_u32;
        uint32_t max_id = 0;
        for (const auto &pair : m_map)
        {
            if (pair.first < min_id)
                min_id = pair.first;
            if (pair.first > max_id)
                max_id = pair.first;
        }

        m_start_id = min_id;

        // Calculate the safe range and the desired table size
        mpsize_t range = max_id - min_id + 1;
        mpsize_t table_size = m_limit_by_max_id ? ntd::min(range, m_threshold) : m_threshold;

        if (table_size == 0)
            return;

        m_is_lookup_clear = false;
        m_lookup_table.resize(table_size, nullptr);

        // Populate the lookup table with non-owning pointers to map values
        for (auto &pair : m_map)
        {
            uint32_t relative_id = pair.first - m_start_id;
            if (relative_id < table_size)
            {
                // Handle two cases at compile time:
                // 1. The map stores pointers (e.g., map<int, MyType*>)
                // 2. The map stores objects directly (e.g., map<int, MyType>)
                if constexpr (ntd::is_pointer_v<TValue>)
                    m_lookup_table[relative_id] = pair.second;
                else
                    m_lookup_table[relative_id] = &pair.second;
            }
        }
    }

    /**
     * @brief Finds an element by its ID using the hybrid lookup strategy.
     * Tries the O(1) fast lookup table first, then falls back to the map.
     *
     * @param id The key (ID) to find.
     * @return A pointer to the stored value, or nullptr if not found.
     */
    ReturnPointerType Find(uint32_t id)
    {
        return const_cast<ReturnPointerType>(
            static_cast<const HybridLookupTable *>(this)->Find(id));
    }

    ConstReturnPointerType Find(uint32_t id) const
    {
        // Try the fast lookup table first
        if  (!m_is_lookup_clear && id >= m_start_id)
        {
            uint32_t relative_id = id - m_start_id;
            if (relative_id < m_lookup_table.size())
                return m_lookup_table[relative_id];
        }

        // Fallback to the map
        auto it = m_map.find(id);
        if (it != m_map.end())
        {
            if constexpr (ntd::is_pointer_v<TValue>)
                return it->second;
            else
                return &it->second;
        }

        return nullptr;
    }

    // --- Passthrough map functions ---

    iterator begin() { return m_map.begin(); }
    const_iterator begin() const { return m_map.begin(); }
    iterator end() { return m_map.end(); }
    const_iterator end() const { return m_map.end(); }

    mpsize_t size() const { return static_cast<mpsize_t>(m_map.size()); }
    bool empty() const { return m_map.empty(); }

private:
    /**
     * @brief Clears the lookup table and resets its start ID.
     */
    void ClearLookupTable()
    {
        if (m_is_lookup_clear)
            return;

        mpsize_t table_size = static_cast<mpsize_t>(m_lookup_table.size());
        if (table_size > 0 && size() <= table_size / FAST_CLEAR_FILL_FRACTION)
        {
            for (auto &pair : m_map)
            {
                uint32_t relative_id = pair.first - m_start_id;
                if (relative_id < table_size)
                {
                    m_lookup_table[relative_id] = nullptr;
                }
            }
        }
        else
        {
            m_lookup_table.clear();
        }

        m_start_id = 0;
        m_is_lookup_clear = true;
    }

    // The source of truth for all data.
    // TValue can be MyObject or MyObject*
    ntd::unordered_map<uint32_t, TValue> m_map;

    // The fast lookup table.
    // This will be ntd::vector<MyObject*> in both cases.
    ntd::vector<LookupPointerType> m_lookup_table;
    bool m_is_lookup_clear = true;

    // The minimum ID used as the offset for the lookup table
    uint32_t m_start_id;

    // Configuration for building the lookup table
    mpsize_t m_threshold;
    bool m_limit_by_max_id;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A hybrid set implementation that combines a fast, direct-access
 * lookup vector with a standard unordered_set.
 */
class HybridLookupSet
{
public:
    static constexpr mpsize_t DIRECT_LOOKUP_THRESHOLD = 65536;
    static constexpr mpsize_t FAST_CLEAR_FILL_FRACTION = 4;

    using iterator = ntd::unordered_set<uint32_t>::iterator;
    using const_iterator = ntd::unordered_set<uint32_t>::const_iterator;

    HybridLookupSet(mpsize_t fastLookupSize = 0)
        : m_start_id(0)
    {
        if (fastLookupSize > 0)
        {
            SetFastLookupSize(fastLookupSize);
        }
    }

    void SetFastLookupSize(mpsize_t fastLookupSize)
    {
        XASSERT_DIAG(m_set.empty() || fastLookupSize <= m_fast_lookup.size(), "HybridLookupSet::SetFastLookupSize - the new size should be smaller, or m_set should be empty");
        m_fast_lookup.resize(fastLookupSize, 0);
    }

    bool Insert(uint32_t id)
    {
        if (id >= m_start_id)
        {
            uint32_t relative_id = id - m_start_id;

            if (relative_id < m_fast_lookup.size())
            {
                uint8_t& is_present = m_fast_lookup[relative_id];
                if (is_present)
                    return false;

                is_present = 1;
                m_set.insert(id);
                return true;
            }
        }

        auto [_, inserted] = m_set.insert(id);
        return inserted;
    }

    bool Contains(uint32_t id) const
    {
        if (id >= m_start_id)
        {
            uint32_t relative_id = id - m_start_id;

            if (relative_id < m_fast_lookup.size())
            {
                return !!m_fast_lookup[relative_id];
            }
        }

        return m_set.find(id) != m_set.end();
    }

    mpsize_t Erase(uint32_t id)
    {
        if (id >= m_start_id)
        {
            uint32_t relative_id = id - m_start_id;

            if (relative_id < m_fast_lookup.size())
            {
                m_fast_lookup[relative_id] = 0;
            }
        }

        return static_cast<mpsize_t>(m_set.erase(id));
    }

    void Clear()
    {
        FastClearVector(m_start_id);
        m_set.clear();
    }

    /**
     * @brief Rebuilds the fast lookup vector to exactly fit the current range of IDs.
     */
    void RebuildToFit()
    {
        if (m_set.empty())
        {
            m_fast_lookup.clear();
            m_start_id = 0;
            return;
        }

        uint32_t min_id = max_u32;
        uint32_t max_id = 0;

        for (uint32_t id : m_set)
        {
            if (id < min_id)
                min_id = id;
            if (id > max_id)
                max_id = id;
        }

        m_start_id = min_id;
        mpsize_t new_size = max_id - min_id + 1;

        // Assign new size and fill with 0.
        // This is more efficient than resize() + ntd::memset() as we are discarding old data.
        m_fast_lookup.assign(static_cast<mpsize_t>(new_size), 0);

        // Populate with current data
        for (uint32_t id : m_set)
        {
            m_fast_lookup[id - m_start_id] = 1;
        }
    }

    /**
     * @brief Shifts the fast lookup window to a new start ID.
     * Moves existing values into the new vector positions.
     * Uses Fast Clear logic to unmark old positions before marking new ones.
     */
    void SetStartId(uint32_t new_start_id)
    {
        if (m_start_id == new_start_id)
            return;

        if (m_fast_lookup.empty())
        {
            m_start_id = new_start_id;
            return;
        }

        FastClearVector(m_start_id);
        m_start_id = new_start_id;

        if (!m_set.empty())
        {
            mpsize_t vec_size = static_cast<mpsize_t>(m_fast_lookup.size());
            for (uint32_t id : m_set)
            {
                uint32_t new_relative_id = id - m_start_id;
                if (new_relative_id < vec_size)
                {
                    m_fast_lookup[new_relative_id] = 1;
                }
            }
        }
    }

    uint32_t GetStartId() const { return m_start_id; }

    // --- Passthrough set functions ---

    iterator begin() { return m_set.begin(); }
    const_iterator begin() const { return m_set.begin(); }
    iterator end() { return m_set.end(); }
    const_iterator end() const { return m_set.end(); }

    mpsize_t size() const { return static_cast<mpsize_t>(m_set.size()); }
    bool empty() const { return m_set.empty(); }

private:
    /**
     * @brief Clears the fast lookup vector based on the current set contents.
     * Uses optimization based on fill ratio to avoid expensive memset.
     */
    void FastClearVector(uint32_t current_start_id)
    {
        mpsize_t vec_size = static_cast<mpsize_t>(m_fast_lookup.size());
        if (vec_size == 0)
            return;

        if (m_set.size() <= vec_size / FAST_CLEAR_FILL_FRACTION)
        {
            for (uint32_t id : m_set)
            {
                uint32_t relative_id = id - current_start_id;
                if (relative_id < vec_size)
                {
                    m_fast_lookup[relative_id] = 0;
                }
            }
        }
        else
        {
            ntd::memset(m_fast_lookup.data(), 0, vec_size * sizeof(uint8_t));
        }
    }

    // The fast lookup table (1 byte per bool)
    ntd::vector<uint8_t> m_fast_lookup;

    // The source of truth for all data
    ntd::unordered_set<uint32_t> m_set;

    // The ID offset for the fast lookup table
    uint32_t m_start_id;
};

} // GPRX

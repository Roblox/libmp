// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "kv_store.h"
#include "utils.h"

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
inline void KVStore::FetchValue(uint32_t* dest_buf, const uint32_t* value_elements, mpsize_t num_elements)
{
    for (mpsize_t i = 0; i < num_elements; ++i)
    {
        dest_buf[i] = value_elements[i] & VALUE_DATA_MASK;
    }
}

inline void KVStore::PutValue(uint32_t* line_ptr, mpsize_t write_idx, const uint32_t* value, mpsize_t value_size_in_u32)
{
    for (mpsize_t i = 0; i < value_size_in_u32; ++i)
    {
        line_ptr[write_idx++] = (value[i] & VALUE_DATA_MASK) | VALUE_MARKER_BIT;
    }
}

inline void KVStore::MigrateValue(uint32_t* line_ptr, mpsize_t write_idx, const uint32_t* value, mpsize_t value_size_in_u32)
{
    for (mpsize_t i = 0; i < value_size_in_u32; ++i)
    {
        line_ptr[write_idx++] = value[i];
    }
}

inline uint8_t* KVStore::GetPayloadPtr(mpsize_t offset, mpsize_t size)
{
    m_container.PushIgnoreLock();
    uint8_t* ptr = m_container.GetPayloadDataPtr(*m_block, offset, size, nullptr);
    m_container.PopIgnoreLock();
    return ptr;
}

inline uint32_t* KVStore::GetPayloadU32Ptr(mpsize_t offset, mpsize_t size)
{
    const bool offset_misaligned = offset % alignof(uint32_t) != 0;
    uint8_t* ptr = offset_misaligned ? nullptr : GetPayloadPtr(offset, size);
    const bool pointer_misaligned = ptr && !is_aligned<alignof(uint32_t)>(ptr);
    if (offset_misaligned || pointer_misaligned)
    {
        XASSERT_DIAG(false,
            "KVStore::GetPayloadU32Ptr - uint32_t payload data must be aligned");
        return nullptr;
    }
    return reinterpret_cast<uint32_t*>(ptr);
}

inline bool KVStore::ReadPayloadToBuf(mpsize_t offset, mpsize_t size, uint8_t* buf, mpsize_t buf_size)
{
    return m_container.ReadPayloadDataToBuffer(*m_block, offset, size, buf, buf_size);
}

inline bool KVStore::AppendAlignment()
{
    return m_container.AppendAlignmentPadding(*m_block);
}

inline bool KVStore::AppendContiguousData(const uint8_t* data, mpsize_t size, uint32_t& out_offset)
{
    return m_container.AppendPayloadDataContiguous(*m_block, data, size, out_offset);
}

inline bool KVStore::AppendContiguousData(mpsize_t size, uint32_t& out_offset)
{
    return AppendContiguousData(nullptr, size, out_offset);
}

bool KVStore::ConfigureLayout(const Config& config)
{
    // Hash shifts must stay within uint32_t, and every line needs room for its
    // metadata plus at least one key/value element. These fields come from disk for a
    // read-only store, so validate them before deriving masks, indexes, or byte sizes.
    if (config.l1_bits >= ntd::numeric_limits<uint32_t>::digits ||
        config.l2_bits >= ntd::numeric_limits<uint32_t>::digits ||
        static_cast<uint32_t>(config.l1_bits) + config.l2_bits >
            ntd::numeric_limits<uint32_t>::digits ||
        config.kv_entries < 3 || config.v_entries < 4)
        return false;

    const uint64_t l1_entries = uint64_t{1} << config.l1_bits;
    const uint64_t l2_entries = uint64_t{1} << config.l2_bits;
    if (l1_entries > max_mpsize_t / sizeof(uint32_t) ||
        l2_entries > max_mpsize_t / sizeof(uint32_t))
        return false;

    is_layout_configured = true;

    HASH_LOW_MASK = create_mask_u32(config.l1_bits);
    HASH_HIGH_SHIFT = config.l1_bits;
    HASH_HIGH_MASK = create_mask_u32(config.l2_bits);

    L1_TABLE_ENTRIES = pow2(config.l1_bits);
    L2_TABLE_ENTRIES = pow2(config.l2_bits);
    L1_TABLE_SIZE_BYTES = L1_TABLE_ENTRIES * sizeof(uint32_t);
    L2_TABLE_SIZE_BYTES = L2_TABLE_ENTRIES * sizeof(uint32_t);

    KV_LINE_TOTAL_ENTRIES = config.kv_entries;
    KV_LINE_USABLE_ENTRIES = KV_LINE_TOTAL_ENTRIES - 1;
    KV_LINE_NEXT_PTR_IDX = KV_LINE_USABLE_ENTRIES;
    KV_LINE_SIZE_BYTES = KV_LINE_TOTAL_ENTRIES * sizeof(uint32_t);

    V_LINE_TOTAL_ENTRIES = config.v_entries;
    V_LINE_USABLE_ENTRIES = V_LINE_TOTAL_ENTRIES - 1;
    V_LINE_THRESHOLD_ENTRIES = V_LINE_TOTAL_ENTRIES - 3;
    V_LINE_NEXT_PTR_IDX = V_LINE_USABLE_ENTRIES;
    V_LINE_FIRST_PTR_IDX = V_LINE_TOTAL_ENTRIES - 2;
    V_LINE_VALUES_COUNT_IDX = V_LINE_THRESHOLD_ENTRIES;
    V_LINE_SIZE_BYTES = V_LINE_TOTAL_ENTRIES * sizeof(uint32_t);
    return true;
}

bool KVStore::Initialize(const Config& config)
{
    if (!m_block || !m_block->headerPtr)
        return false;
    if (m_block->bNewWritableBlock && m_block->payloadChunks.empty())
    {
        if (!ConfigureLayout(config))
            return false;
        uint32_t data_offset;
        // Allocate the L1 table and the config structure, ensuring they are contiguous and aligned.
        if (AppendContiguousData(L1_TABLE_SIZE_BYTES, data_offset) &&
            AppendAlignment() &&
            AppendContiguousData(reinterpret_cast<const uint8_t*>(&config), sizeof(Config), data_offset) &&
            AppendAlignment())
        {
            PutKvStoreConfigOffsetToMeta(m_block->headerPtr, data_offset);
            return true;
        }
        return false;
    }
    return true;
}

KVStore::KVStore(ContainerContext& container, BlockContext* block)
    : m_container(container)
{
    SetBlock(block);
}

KVStore::FindResult KVStore::FindKey(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash)
{
    FindResult result{};

    if (!m_block || !m_block->headerPtr || !key_data || key_size_in_u32 == 0 ||
        key_size_in_u32 > KEY_MAX_SIZE_IN_U32)
        return result;

    if (!is_layout_configured)
    {
        mpsize_t config_offset = GetKvStoreConfigOffsetFromMeta(m_block->headerPtr);
        Config config;
        if (!ReadPayloadToBuf(config_offset, sizeof(config),
                reinterpret_cast<uint8_t*>(&config), sizeof(config)))
        {
            result.status = FindStatus::ERROR_STATE;
            return result;
        }
        if (!ConfigureLayout(config) ||
            L1_TABLE_SIZE_BYTES > m_block->headerPtr->payload_uncompressed_size)
            return result;
        
        if (L1_TABLE_CACHING_ENABLED && !m_block->bNewWritableBlock && m_container.IsObjectCachingAllowed())
        {
            const mpsize_t sz = L1_TABLE_SIZE_BYTES;
            resize_if_needed(l1_table_cache, L1_TABLE_ENTRIES);
            if (!ReadPayloadToBuf(0, sz,
                    reinterpret_cast<uint8_t*>(l1_table_cache.data()), sz))
            {
                l1_table_cache.clear();
                return result;
            }
            is_l1_table_cache_valid = true;
        }
    }

    // If the hash_high bit count is 0, we have only 1 entry in the L2 table,
    // so we can skip it entirely and store the KV line offsets directly in L1.
    bool skip_l2 = (L2_TABLE_ENTRIES == 1);

    mphash_t hash = precomputed_hash ? *precomputed_hash : CalculateHash(key_data, key_size_in_u32);
    result.hash_low = hash & HASH_LOW_MASK;
    result.hash_high = (hash >> HASH_HIGH_SHIFT) & HASH_HIGH_MASK;

    result.l1_table_ptr = (!L1_TABLE_CACHING_ENABLED || !is_l1_table_cache_valid)
        ? GetPayloadU32Ptr(0, L1_TABLE_SIZE_BYTES)
        : l1_table_cache.data();
    if (!result.l1_table_ptr)
    {
        result.status = FindStatus::ERROR_STATE;
        return result;
    }

    if (skip_l2)
    {
        result.l2_table_ptr = &result.l1_table_ptr[result.hash_low];
    }
    else
    {
        result.l2_table_offset = result.l1_table_ptr[result.hash_low];
        if (result.l2_table_offset == 0)
        {
            result.status = FindStatus::L2_TABLE_MISSING;
            return result;
        }

        result.l2_table_ptr = GetPayloadU32Ptr(result.l2_table_offset, L2_TABLE_SIZE_BYTES);
        if (!result.l2_table_ptr)
        {
            result.status = FindStatus::ERROR_STATE;
            return result;
        }
    }

    uint32_t current_line_offset = result.l2_table_ptr[result.hash_high];
    if (current_line_offset == 0)
    {
        result.status = FindStatus::KV_LINE_MISSING;
        return result;
    }
    
    // If we get here, a chain exists. We scan it.
    const mpsize_t maximum_kv_lines =
        m_block->headerPtr->payload_uncompressed_size / KV_LINE_SIZE_BYTES;
    mpsize_t visited_kv_lines = 0;
    while (current_line_offset != 0 && visited_kv_lines++ < maximum_kv_lines)
    {
        result.last_kv_line_ptr = GetPayloadU32Ptr(current_line_offset, KV_LINE_SIZE_BYTES);
        if (!result.last_kv_line_ptr)
            return {}; // Error state

        auto is_valid_entry = [&](mpsize_t idx)
        {
            return idx < KV_LINE_USABLE_ENTRIES && result.last_kv_line_ptr[idx] != 0;
        };
        auto is_key_part = [&](mpsize_t idx)
        {
            return idx < KV_LINE_USABLE_ENTRIES && !(result.last_kv_line_ptr[idx] & VALUE_MARKER_BIT);
        };
        auto is_value_part = [&](mpsize_t idx)
        {
            return idx < KV_LINE_USABLE_ENTRIES && (result.last_kv_line_ptr[idx] & VALUE_MARKER_BIT);
        };

        // Scan the KV-Line buffer
        mpsize_t read_idx = 0;
        while (is_valid_entry(read_idx))
        {
            // 1. Scan key
            bool is_match = true;
            mpsize_t k_idx = 0;
            while (is_key_part(read_idx))
            {
                // Check match on the fly
                if (is_match && (k_idx >= key_size_in_u32 || result.last_kv_line_ptr[read_idx] != key_data[k_idx]))
                {
                    is_match = false;
                }
                read_idx++;
                k_idx++;
            }

            // Length mismatch check (stored key was shorter than input)
            if (k_idx != key_size_in_u32)
                is_match = false;

            // 2. Scan value
            mpsize_t value_start_idx = read_idx;
            mpsize_t current_val_len = 0;
            while (is_value_part(read_idx))
            {
                read_idx++;
                current_val_len++;
            }

            // 3. Handle result
            if (is_match)
            {
                if (current_val_len == 0 || current_val_len > VALUE_MAX_SIZE_IN_U32)
                    return {};
                result.status = FindStatus::KEY_FOUND;
                result.kv_line_ptr = result.last_kv_line_ptr;
                result.value_start_idx = value_start_idx;
                result.value_len_u32 = static_cast<uint32_t>(current_val_len);
                return result; 
            }
        }
        
        result.first_empty_idx = read_idx; // Store where we can write
        current_line_offset = result.last_kv_line_ptr[KV_LINE_NEXT_PTR_IDX];
    }

    if (current_line_offset != 0)
        return {}; // cyclic or impossibly long line chain
    
    result.status = FindStatus::CHAIN_SEARCHED_NOT_FOUND;
    return result;
}

bool KVStore::Put(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* value, mpsize_t value_size_in_u32, SetStrategy strategy, mpsize_t* out_values_count, PutOrReplaceCallback replaceCb)
{
    auto process_replacement_value = [&](const uint32_t* old_value_ptr = nullptr)
    {
        if (replaceCb)
        {
            replaceCb(value, old_value_ptr, value_size_in_u32);
        }
    };

    if (out_values_count)
        *out_values_count = 0;

    if (!key_data || !value || key_size_in_u32 == 0 ||
        key_size_in_u32 > KEY_MAX_SIZE_IN_U32 || value_size_in_u32 == 0 ||
        value_size_in_u32 > VALUE_MAX_SIZE_IN_U32)
        return false;

    // Find the key's location, or where to write it if not found.
    FindResult result = FindKey(key_data, key_size_in_u32, precomputed_hash);
    if (!is_layout_configured ||
        key_size_in_u32 > KV_LINE_USABLE_ENTRIES ||
        value_size_in_u32 > KV_LINE_USABLE_ENTRIES - key_size_in_u32 ||
        value_size_in_u32 > V_LINE_THRESHOLD_ENTRIES)
        return false;

    if (result.status == FindStatus::KEY_FOUND)
    {
        if (strategy == SetStrategy::PutNew)
            return false;

        if (value_size_in_u32 != result.value_len_u32)
            return false;

        // --- KEY EXISTS: MIGRATE TO V-LINE OR APPEND TO V-LINE ---
        uint32_t* value_ptr = result.kv_line_ptr + result.value_start_idx;

        if ((value_ptr[0] & VALUE_MARKER_MASK) == V_LINE_PTR_MARKER)
        {
            // The key already stores multiple values, so the value can't be replaced
            if (strategy == SetStrategy::PutOrReplaceSingle)
                return false;

            // --- APPEND TO EXISTING V-LINE CHAIN ---
            uint32_t current_v_line_offset = value_ptr[0] & POINTER_DATA_MASK;
            uint32_t* current_v_line_ptr = GetPayloadU32Ptr(current_v_line_offset, V_LINE_SIZE_BYTES);
            if (!current_v_line_ptr)
                return false;
            uint32_t* values_count_ptr = &current_v_line_ptr[V_LINE_VALUES_COUNT_IDX];

            // Determine the end indexes for the ordinary and the last filled v-lines
            auto [ordinary_line_end_idx, last_line_end_idx] = CalcVLineEndIndexes(current_v_line_ptr, value_size_in_u32, *values_count_ptr);
            mpsize_t write_idx = last_line_end_idx;
            bool is_last_allocated_line = true;

            // If the last v-line contains no values and points to a previous v-line,
            // there might be a few free slots in that previous v-line.
            bool is_empty_line = (current_v_line_ptr[0] == 0);
            if (is_empty_line && write_idx + value_size_in_u32 <= V_LINE_USABLE_ENTRIES)
            {
                uint32_t previous_v_line_offset = current_v_line_ptr[V_LINE_NEXT_PTR_IDX];
                if (previous_v_line_offset)
                {
                    uint32_t* previous_v_line_ptr = GetPayloadU32Ptr(previous_v_line_offset, V_LINE_SIZE_BYTES);
                    if (!previous_v_line_ptr)
                        return false;
                    current_v_line_ptr = previous_v_line_ptr;
                    is_last_allocated_line = false;
                }
            }
            
            if (is_last_allocated_line && is_empty_line)
                write_idx = 0;

            // Check for overflow and create a new V-line if needed
            if (is_last_allocated_line && !is_empty_line && write_idx + value_size_in_u32 > V_LINE_THRESHOLD_ENTRIES)
            {
                uint32_t new_v_line_offset = 0;
                if (!AppendContiguousData(V_LINE_SIZE_BYTES, new_v_line_offset))
                    return false;
                uint32_t* new_v_line_ptr = GetPayloadU32Ptr(new_v_line_offset, V_LINE_SIZE_BYTES);
                if (!new_v_line_ptr)
                    return false;

                // New V-line points to the previous one and inherits its values counter and the pointer to the first v-line
                new_v_line_ptr[V_LINE_NEXT_PTR_IDX] = current_v_line_offset;
                new_v_line_ptr[V_LINE_FIRST_PTR_IDX] = current_v_line_ptr[V_LINE_FIRST_PTR_IDX];
                new_v_line_ptr[V_LINE_VALUES_COUNT_IDX] = values_count_ptr[0];
                // KV-line entry's value now points to the new V-line
                value_ptr[0] = new_v_line_offset | V_LINE_PTR_MARKER;

                // Reset the old v-line's pointer to the first v-line
                current_v_line_ptr[V_LINE_FIRST_PTR_IDX] = 0;
                // Reset the values counter in the old v-line
                values_count_ptr[0] = 0;
                // Use the values counter from the new v-line
                values_count_ptr = &new_v_line_ptr[V_LINE_VALUES_COUNT_IDX];

                // If the new record does not fit the current v-line, it will be placed to the beginning of the new v-line
                if (write_idx + value_size_in_u32 > V_LINE_USABLE_ENTRIES)
                {
                    current_v_line_ptr = new_v_line_ptr;
                    write_idx = 0;
                }
            }

            // Write the new value to the V-line
            PutValue(current_v_line_ptr, write_idx, value, value_size_in_u32);
            values_count_ptr[0]++;
            if (out_values_count)
                *out_values_count = values_count_ptr[0];
        }
        else
        {
            // The key already stores 1 value, and it can be replaced if the new value has the same size
            if (strategy == SetStrategy::PutOrReplaceSingle)
            {
                process_replacement_value(result.kv_line_ptr + result.value_start_idx);
                PutValue(result.kv_line_ptr, result.value_start_idx, value, value_size_in_u32);
                if (out_values_count)
                    *out_values_count = 1;
                return true;
            }

            // --- MIGRATE SINGLE VALUE TO NEW V-LINE ---
            
            // Allocate a new V-Line
            uint32_t new_v_line_offset = 0;
            if (!AppendContiguousData(V_LINE_SIZE_BYTES, new_v_line_offset))
                return false;
            uint32_t* v_line_ptr = GetPayloadU32Ptr(new_v_line_offset, V_LINE_SIZE_BYTES);
            if (!v_line_ptr)
                return false;

            mpsize_t write_idx = 0;
            // Write original value (value_ptr from KV-line) to the new V-Line
            MigrateValue(v_line_ptr, write_idx, value_ptr, result.value_len_u32);
            write_idx += result.value_len_u32;
            // Write new value
            PutValue(v_line_ptr, write_idx, value, value_size_in_u32);
            v_line_ptr[V_LINE_FIRST_PTR_IDX] = new_v_line_offset;
            v_line_ptr[V_LINE_VALUES_COUNT_IDX] = 2;
            if (out_values_count)
                *out_values_count = 2;

            // Replace the original value (found in the KV-LINE) with the offset of the new V-LINE
            value_ptr[0] = new_v_line_offset | V_LINE_PTR_MARKER;
            // Fill rest with empty value markers so we can understand the size of the values stored in V-LINES
            for (mpsize_t i = 1; i < result.value_len_u32; ++i)
            {
                value_ptr[i] = VALUE_MARKER_BIT;
            }
        }

        // The append-to-array and single->array-migration paths above stored the value
        // successfully, so report success. (PutNew on an existing key, a value-size
        // mismatch, and PutOrReplaceSingle on an existing array already returned false;
        // PutOrReplaceSingle on a single value already returned true.)
        return true;
    }
    else if (result.status != FindStatus::ERROR_STATE)
    {
        // --- KEY NOT FOUND: APPEND NEW KV PAIR ---
        uint32_t* current_line_ptr = result.last_kv_line_ptr;
        mpsize_t write_idx = result.first_empty_idx;

        // The logic cascades: if a KV line is missing, the L2/L1 tables must exist.
        // If an L2 table is missing, the L1 table must exist.
        switch (result.status)
        {
            case FindStatus::L2_TABLE_MISSING:
            {
                // Create L2 table
                if (!AppendContiguousData(L2_TABLE_SIZE_BYTES, result.l2_table_offset))
                    return false;
                result.l1_table_ptr[result.hash_low] = result.l2_table_offset;
                result.l2_table_ptr = GetPayloadU32Ptr(result.l2_table_offset, L2_TABLE_SIZE_BYTES);
                if (!result.l2_table_ptr)
                    return false;
                [[fallthrough]]; // Fallthrough to create the KV line
            }
            case FindStatus::KV_LINE_MISSING:
            {
                // Create the first KV line for this hash
                uint32_t new_line_offset = 0;
                if (!AppendContiguousData(KV_LINE_SIZE_BYTES, new_line_offset))
                    return false;
                result.l2_table_ptr[result.hash_high] = new_line_offset;
                current_line_ptr = GetPayloadU32Ptr(new_line_offset, KV_LINE_SIZE_BYTES);
                if (!current_line_ptr)
                    return false;
                write_idx = 0;
                [[fallthrough]]; // Fallthrough to write the data
            }
            case FindStatus::CHAIN_SEARCHED_NOT_FOUND:
            {
                // Check if the current line has space. If not, create and link a new one.
                if (write_idx + key_size_in_u32 + value_size_in_u32 > KV_LINE_USABLE_ENTRIES)
                {
                    uint32_t new_line_offset = 0;
                    if (!AppendContiguousData(KV_LINE_SIZE_BYTES, new_line_offset))
                        return false;
                    current_line_ptr[KV_LINE_NEXT_PTR_IDX] = new_line_offset; // Link old line to new
                    current_line_ptr = GetPayloadU32Ptr(new_line_offset, KV_LINE_SIZE_BYTES);
                    if (!current_line_ptr)
                        return false;
                    write_idx = 0;
                }
                
                // Write the key and value directly into the chosen KV Line buffer.
                for (mpsize_t i = 0; i < key_size_in_u32; ++i)
                {
                    current_line_ptr[write_idx++] = key_data[i] & KEY_DATA_MASK;
                }
                process_replacement_value();
                PutValue(current_line_ptr, write_idx, value, value_size_in_u32);
                if (out_values_count)
                    *out_values_count = 1;
                break;
            }
            default:
                break;
        }
        return true;
    }
    
    return false; // Could not find or set key (error state)
}

bool KVStore::Get(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* out_value, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    if (!out_value)
        return false;
    const mpsize_t index = 0;
    return GetValueAtIndexImpl(key_data, key_size_in_u32, precomputed_hash, index, out_value, out_value_size_in_u32, out_values_count);
}

inline ntd::pair<mpsize_t, mpsize_t> KVStore::CalcVLinesInfo(const uint32_t* v_line_ptr, mpsize_t value_size_u32, mpsize_t values_count)
{
    bool lastLineIsEmpty = (v_line_ptr[0] == 0);
    mpsize_t values_per_line = V_LINE_USABLE_ENTRIES / value_size_u32;
    // Round up to make sure we have enough lines even if the division doesn't come out even.
    // The last line may be empty, so add extra 1.
    mpsize_t lines_count = (values_count + values_per_line - 1) / values_per_line + (lastLineIsEmpty ? 1 : 0);
    return { values_per_line, lines_count };
}

inline ntd::pair<mpsize_t, mpsize_t> KVStore::CalcValueLocation(mpsize_t value_index, mpsize_t values_per_line)
{
    mpsize_t target_line_idx = value_index / values_per_line;
    mpsize_t target_line_value_idx = value_index % values_per_line;
    return { target_line_idx, target_line_value_idx };
}

inline ntd::pair<mpsize_t, mpsize_t> KVStore::CalcVLineEndIndexes(const uint32_t* v_line_ptr, mpsize_t value_size_u32, mpsize_t values_count)
{
    mpsize_t last_value_index = values_count - 1; // We are in v-line, so values_count is > 1
    auto [values_per_line, _] = CalcVLinesInfo(v_line_ptr, value_size_u32, values_count);
    auto [__, last_line_value_idx] = CalcValueLocation(last_value_index, values_per_line);
    mpsize_t ordinary_line_end_idx = values_per_line * value_size_u32;
    mpsize_t last_line_end_idx = (last_line_value_idx + 1) * value_size_u32;
    return { ordinary_line_end_idx, last_line_end_idx };
}

const uint32_t* KVStore::GetAllValues(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    if (out_values_count)
        *out_values_count = 0;
    if (out_value_size_in_u32)
        *out_value_size_in_u32 = 0;
    
    FindResult result = FindKey(key_data, key_size_in_u32, precomputed_hash);
    if (result.status != FindStatus::KEY_FOUND)
    {
        all_values_buf.clear();
        return nullptr;
    }

    const uint32_t* value_ptr = result.kv_line_ptr + result.value_start_idx;
    const mpsize_t value_size_u32 = result.value_len_u32; // Implicit size for all values

    if ((value_ptr[0] & VALUE_MARKER_MASK) == V_LINE_PTR_MARKER)
    {
        if (value_size_u32 > V_LINE_THRESHOLD_ENTRIES)
            return nullptr;
        // It's a pointer to the last V-line in the chain. Traverse it.
        uint32_t current_v_line_offset = value_ptr[0] & POINTER_DATA_MASK;
        mpsize_t values_count = 0;
        mpsize_t ordinary_line_end_idx = 0;
        mpsize_t last_line_end_idx = 0;
        bool lastNonEmptyLineVisited = false;
        bool lastLine = true;
        const mpsize_t maximum_v_lines =
            m_block->headerPtr->payload_uncompressed_size / V_LINE_SIZE_BYTES;
        mpsize_t visited_v_lines = 0;
        while (current_v_line_offset != 0 && visited_v_lines++ < maximum_v_lines)
        {
            const uint32_t* v_line_ptr = GetPayloadU32Ptr(current_v_line_offset, V_LINE_SIZE_BYTES);
            if (!v_line_ptr)
                break; // Error reading V-Line

            bool skipLine = false;
            if (lastLine)
            {
                lastLine = false;
                values_count = v_line_ptr[V_LINE_VALUES_COUNT_IDX];
                const mpsize_t maximum_values =
                    m_block->headerPtr->payload_uncompressed_size /
                    (value_size_u32 * static_cast<mpsize_t>(sizeof(uint32_t)));
                if (values_count < 2 || values_count > maximum_values)
                {
                    all_values_buf.clear();
                    return nullptr;
                }
                all_values_buf.resize(values_count * value_size_u32);
                if (out_values_count)
                    *out_values_count = values_count;

                // If the last V-line contains no values, skip it without scanning
                skipLine = (v_line_ptr[0] == 0);

                // Determine the end indexes for the ordinary and the last filled v-lines
                ntd::tie(ordinary_line_end_idx, last_line_end_idx) = CalcVLineEndIndexes(v_line_ptr, value_size_u32, values_count);
            }

            // If the last line was empty, skip it.
            if (!skipLine)
            {
                mpsize_t end_idx = lastNonEmptyLineVisited ? ordinary_line_end_idx : last_line_end_idx;
                lastNonEmptyLineVisited = true;

                // Scan this line backwards from the determined end point.
                while (end_idx >= value_size_u32 && values_count >= 1)
                {
                    end_idx -= value_size_u32;
                    mpsize_t dest_buf_idx = (values_count - 1) * value_size_u32;
                    FetchValue(&all_values_buf[dest_buf_idx], &v_line_ptr[end_idx], value_size_u32);
                    values_count--;
                }
            }

            current_v_line_offset = v_line_ptr[V_LINE_NEXT_PTR_IDX];
        }
        if (current_v_line_offset != 0 || values_count != 0)
        {
            all_values_buf.clear();
            if (out_values_count)
                *out_values_count = 0;
            return nullptr;
        }
    }
    else
    {
        // It's a simple, single value.
        if (out_values_count)
            *out_values_count = 1;
        all_values_buf.resize(value_size_u32);
        FetchValue(&all_values_buf[0], value_ptr, value_size_u32);
    }

    if (out_value_size_in_u32)
        *out_value_size_in_u32 = value_size_u32;
    return all_values_buf.data();
}

inline bool KVStore::GetValueAtIndexImpl(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    if (out_values_count)
        *out_values_count = 0;
    if (!out_value)
        return false;

    FindResult result = FindKey(key_data, key_size_in_u32, precomputed_hash);
    if (result.status != FindStatus::KEY_FOUND)
        return false;

    const uint32_t* value_ptr = result.kv_line_ptr + result.value_start_idx;
    const mpsize_t value_size_u32 = result.value_len_u32; // Implicit size for all values
    
    // Check if the value is a pointer to a V-Line (multi-value case)
    if ((value_ptr[0] & VALUE_MARKER_MASK) == V_LINE_PTR_MARKER)
    {
        if (value_size_u32 > V_LINE_THRESHOLD_ENTRIES)
            return false;
        uint32_t v_line_offset = value_ptr[0] & POINTER_DATA_MASK;
        const uint32_t* v_line_ptr = nullptr;
        
        // Fast-forward to a specific V-line in the chain
        mpsize_t values_count = 0;
        mpsize_t target_line_value_idx = 0;
        mpsize_t need_to_skip_lines = 0;
        bool skipAllLines = true;
        bool lastLine = true;
        const mpsize_t maximum_v_lines =
            m_block->headerPtr->payload_uncompressed_size / V_LINE_SIZE_BYTES;
        mpsize_t visited_v_lines = 0;
        while (v_line_offset != 0 && visited_v_lines++ < maximum_v_lines)
        {
            v_line_ptr = GetPayloadU32Ptr(v_line_offset, V_LINE_SIZE_BYTES);
            if (!v_line_ptr)
                return false;
            uint32_t next_v_line_offset = v_line_ptr[V_LINE_NEXT_PTR_IDX];

            if (lastLine)
            {
                lastLine = false;
                values_count = v_line_ptr[V_LINE_VALUES_COUNT_IDX];
                const mpsize_t maximum_values =
                    m_block->headerPtr->payload_uncompressed_size /
                    (value_size_u32 * static_cast<mpsize_t>(sizeof(uint32_t)));
                if (values_count < 2 || values_count > maximum_values)
                    return false;
                if (out_values_count)
                    *out_values_count = values_count;

                if (index == max_mpsize_t)
                {
                    index = values_count - 1; // values_count is > 0
                }
                else if (index >= values_count)
                    return false;
                
                if (index == 0)
                {
                    next_v_line_offset = v_line_ptr[V_LINE_FIRST_PTR_IDX];
                }
                else // index > 0
                { 
                    mpsize_t target_line_idx = 0;
                    auto [values_per_line, lines_count] = CalcVLinesInfo(v_line_ptr, value_size_u32, values_count);
                    ntd::tie(target_line_idx, target_line_value_idx) = CalcValueLocation(index, values_per_line);
                    target_line_value_idx *= value_size_u32;

                    need_to_skip_lines = lines_count - target_line_idx - 1;
                    skipAllLines = false;
                }
            }

            if (next_v_line_offset == 0)
                break;

            if (!skipAllLines && need_to_skip_lines == 0)
                break;

            v_line_offset = next_v_line_offset;
            if (need_to_skip_lines > 0)
                need_to_skip_lines--;
        }

        if (!v_line_ptr || visited_v_lines > maximum_v_lines)
            return false;

        // Fetch the value from the target V-Line. The size of each value
        // is implicitly defined by the size of the pointer that replaced the original value.
        if (out_value_size_in_u32)
            *out_value_size_in_u32 = value_size_u32;
        FetchValue(out_value, &v_line_ptr[target_line_value_idx], value_size_u32);
    }
    else
    {
        // It's a simple, single value stored in-place in the KV-Line.
        if (out_values_count)
            *out_values_count = 1;
        if (index != 0 && index != max_mpsize_t)
            return false;
        if (out_value_size_in_u32)
            *out_value_size_in_u32 = value_size_u32;
        FetchValue(out_value, value_ptr, value_size_u32);
    }
    return true;
}

bool KVStore::GetValueAtIndex(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    return GetValueAtIndexImpl(key_data, key_size_in_u32, precomputed_hash, index, out_value, out_value_size_in_u32, out_values_count);
}

bool KVStore::Contains(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_values_count)
{
    if (out_values_count)
        *out_values_count = 0;

    FindResult result = FindKey(key_data, key_size_in_u32, precomputed_hash);
    if (result.status != FindStatus::KEY_FOUND)
        return false;

    if (out_values_count)
    {
        const uint32_t* value_ptr = result.kv_line_ptr + result.value_start_idx;
        if ((value_ptr[0] & VALUE_MARKER_MASK) == V_LINE_PTR_MARKER)
        {
            uint32_t v_line_offset = value_ptr[0] & POINTER_DATA_MASK;
            uint32_t* v_line_ptr = GetPayloadU32Ptr(v_line_offset, V_LINE_SIZE_BYTES);
            if (!v_line_ptr)
                return false;
            mpsize_t values_count = v_line_ptr[V_LINE_VALUES_COUNT_IDX];
            const mpsize_t maximum_values =
                m_block->headerPtr->payload_uncompressed_size /
                (result.value_len_u32 * static_cast<mpsize_t>(sizeof(uint32_t)));
            if (result.value_len_u32 > V_LINE_THRESHOLD_ENTRIES ||
                values_count < 2 || values_count > maximum_values)
                return false;
            if (out_values_count)
                *out_values_count = values_count;
        }
        else
        {
            if (out_values_count)
                *out_values_count = 1;
        }
    }
    return true;
}

bool KVStore::SetBlock(BlockContext* block, mpsize_t store_index)
{
    if (store_index != 0 || !block || !block->headerPtr)
        return false;
    uint32_t new_block_id = block->headerPtr->block_id;
    if (m_block_id != new_block_id)
    {
        m_block_id = new_block_id;
        ClearCaches();
    }
    m_block = block;
    return true;
}

void KVStore::ClearCaches()
{
    is_layout_configured = false;
    is_l1_table_cache_valid = false;
}

ntd::vector<uint32_t>& KVStore::GetAllValuesBufRef()
{
    return all_values_buf;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void ValueBuf::FromString(const char* source, mpsize_t source_len)
{
    DefaultModeResetBufState();
    if (!source)
        return;
    const bool null_terminated = source_len == 0;

    mpsize_t source_idx = 0;
    mpsize_t dest_idx = 0;
    mpsize_t dest_capacity_u32 = capacity_in_u32 - cursor_pos;
    uint32_t* destination = &elements_ptr[cursor_pos];

    // Continue as long as there are characters to read and space in the destination.
    while (dest_idx < dest_capacity_u32 &&
        (null_terminated ? source[source_idx] != '\0' : source_idx < source_len))
    {
        // Zero out the current destination element to ensure the 4th byte is clear.
        destination[dest_idx] = 0;

        // Pack up to 3 characters into the current uint32_t element.
        for (mpsize_t i = 0; i < element_avail_full_bytes; ++i)
        {
            if (null_terminated ? source[source_idx] != '\0' : source_idx < source_len)
            {
                // Shift the character by 0, 8, or 16 bits to place it in the correct byte.
                destination[dest_idx] |= (static_cast<uint32_t>(source[source_idx]) << (i * 8));
                source_idx++;
            }
            else
            {
                // No more source characters to pack.
                break;
            }
        }
        dest_idx++;
    }

    cursor_pos += dest_idx;
    size_in_u32 = cursor_pos;
    DefaultModeResetCursor();
}

mpsize_t ValueBuf::ToString(char* destination, mpsize_t dest_capacity_char, bool force_null_term, ntd::function<bool (mpsize_t)> reserveCb)
{
    if (!destination)
        return 0;
    const mpsize_t remainder_in_u32 = size_in_u32 - cursor_pos;
    if (!remainder_in_u32)
        return 0;
    if (!dest_capacity_char)
    {
        if (!reserveCb)
            return 0;
        dest_capacity_char = remainder_in_u32 * element_avail_full_bytes + (force_null_term ? 1 : 0);
        if (!reserveCb(dest_capacity_char))
            return 0;
    }

    if (force_null_term)
    {
        mpsize_t null_term_idx = ntd::min(remainder_in_u32 * element_avail_full_bytes, dest_capacity_char - 1);
        destination[null_term_idx] = '\0';
        dest_capacity_char--;
    }

    uint32_t* source = &elements_ptr[cursor_pos];
    mpsize_t dest_idx = 0;
    bool end_of_string = false;

    for (mpsize_t i = 0; i < remainder_in_u32 && !end_of_string; ++i)
    {
        // Unpack up to 3 characters from the current uint32_t element.
        for (mpsize_t j = 0; j < element_avail_full_bytes; ++j)
        {
            // Check if the destination buffer is full.
            if (dest_idx >= dest_capacity_char)
            {
                end_of_string = true;
                break;
            }

            // Extract character by shifting right by 0, 8, or 16 bits.
            char c = static_cast<char>((source[i] >> (j * 8)) & 0xFF);
            destination[dest_idx] = c;

            if (force_null_term && c == '\0')
            {
                end_of_string = true;
                break;
            }
            dest_idx++;
        }
    }

    mpsize_t res_len = dest_idx;
    MoveCursor(res_len);
    return res_len;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
KVStoreReaderProxy::~KVStoreReaderProxy()
{
    ClearSources();
}

void KVStoreReaderProxy::SetOwnsSources(bool x)
{
    m_owns_sources = x;
}

void KVStoreReaderProxy::SetActiveSourcesNum(mpsize_t num)
{
    m_active_sources = ntd::min(num, GetSourcesNum());
}

ntd::vector<KVStore*>& KVStoreReaderProxy::GetSources()
{
    return m_stores;
}

mpsize_t KVStoreReaderProxy::GetSourcesNum()
{
    if (m_stores.size() > max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "KVStoreReaderProxy::GetSourcesNum - source count exceeds mpsize_t");
        return max_mpsize_t;
    }
    return to_mpsize(m_stores.size());
}

bool KVStoreReaderProxy::AddSource(KVStore* store)
{
    if (!store)
    {
        XASSERT_DIAG(false,
            "KVStoreReaderProxy::AddSource - source must not be null");
        return false;
    }

    if (m_stores.size() >= max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "KVStoreReaderProxy::AddSource - source count exceeds mpsize_t");
        return false;
    }

    m_stores.push_back(store);
    m_active_sources = to_mpsize(m_stores.size());
    return true;
}

void KVStoreReaderProxy::ClearSources()
{
    if (m_owns_sources)
    {
        for (KVStore* store : m_stores)
        {
            ntd::Delete(store);
        }
    }
    m_stores.clear();
    m_active_sources = 0;
}

bool KVStoreReaderProxy::Get(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* out_value, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    const mpsize_t index = 0;
    return GetValueAtIndexImpl(key_data, key_size_in_u32, precomputed_hash, index, out_value, out_value_size_in_u32, out_values_count);
}

const uint32_t* KVStoreReaderProxy::GetAllValues(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    if (out_values_count)
        *out_values_count = 0;
    if (out_value_size_in_u32)
        *out_value_size_in_u32 = 0;

    all_values_buf.clear();

    // --- Pass 1: Collect all result vectors and calculate the total final size. ---
    ntd::vector<ntd::vector<uint32_t>> all_results_from_stores;
    mpsize_t value_size_in_u32 = 0;
    mpsize_t total_size = 0;

    for (mpsize_t i = 0; i < m_active_sources; ++i)
    {
        KVStore* store = m_stores[i];
        if (!store)
            continue;
        
        mpsize_t local_value_size_in_u32 = 0;
        mpsize_t local_values_count = 0;
        if (!store->GetAllValues(key_data, key_size_in_u32, precomputed_hash, &local_value_size_in_u32, &local_values_count))
            continue;
        if (value_size_in_u32 != 0 && value_size_in_u32 != local_value_size_in_u32)
            return nullptr;
        value_size_in_u32 = local_value_size_in_u32;
        ntd::vector<uint32_t>& current_results_ref = store->GetAllValuesBufRef();
        if (!current_results_ref.empty())
        {
            if (local_values_count > max_mpsize_t - total_size)
                return nullptr;
            total_size += local_values_count;
            // Move the vector to avoid a copy
            all_results_from_stores.push_back(ntd::move(current_results_ref));
            current_results_ref.clear();
        }
    }
    
    if (total_size == 0)
        return nullptr;

    // Pre-allocate the exact memory needed to avoid any reallocations during insertion.
    if (value_size_in_u32 == 0 || total_size > max_mpsize_t / value_size_in_u32)
        return nullptr;
    all_values_buf.reserve(total_size * value_size_in_u32);

    // --- Pass 2: Insert all collected data into the perfectly-sized final vector. ---
    for (const auto& result_vec : all_results_from_stores)
    {
        all_values_buf.insert(
            all_values_buf.end(),
            result_vec.begin(),
            result_vec.end()
        );
    }
    
    if (out_values_count)
        *out_values_count = total_size;
    if (out_value_size_in_u32)
        *out_value_size_in_u32 = value_size_in_u32;
    return all_values_buf.data();
}

inline bool KVStoreReaderProxy::GetValueAtIndexImpl(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    if (out_values_count)
        *out_values_count = 0;

    bool fetched = false;
    mpsize_t values_counter = 0;
    
    bool is_reverse = (index == max_mpsize_t);
    for (mpsize_t i = 0; i < m_active_sources; ++i)
    {
        // Traverse back-to-front if looking for the last index, otherwise front-to-back
        mpsize_t actual_idx = is_reverse ? (m_active_sources - 1 - i) : i;
        KVStore* store = m_stores[actual_idx];   
        if (!store)
            continue;

        mpsize_t local_count = 0;
        if (fetched)
        {
            if (!out_values_count)
                break; // Value already fetched - skip remaining stores if total count isn't needed
            store->Contains(key_data, key_size_in_u32, precomputed_hash, &local_count);
        }
        else
        {
            if (!is_reverse && index < values_counter)
                return false;
            mpsize_t local_index = is_reverse ? max_mpsize_t : (index - values_counter);
            if (store->GetValueAtIndex(key_data, key_size_in_u32, precomputed_hash, local_index, out_value, out_value_size_in_u32, &local_count))
                fetched = true;
        }

        if (local_count > max_mpsize_t - values_counter)
            return false;
        values_counter += local_count;
    }

    if (out_values_count)
        *out_values_count = values_counter;
    return fetched;
}

bool KVStoreReaderProxy::GetValueAtIndex(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32, mpsize_t* out_values_count)
{
    return GetValueAtIndexImpl(key_data, key_size_in_u32, precomputed_hash, index, out_value, out_value_size_in_u32, out_values_count);
}

bool KVStoreReaderProxy::Contains(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_values_count)
{
    if (out_values_count)
        *out_values_count = 0;

    bool contains = false;
    mpsize_t values_counter = 0;

    for (mpsize_t i = 0; i < m_active_sources; ++i)
    {
        KVStore* store = m_stores[i];
        if (!store)
            continue;
        
        mpsize_t local_count;
        if (store->Contains(key_data, key_size_in_u32, precomputed_hash, &local_count))
        {
            if (local_count > max_mpsize_t - values_counter)
            {
                if (out_values_count)
                    *out_values_count = 0;
                return false;
            }
            values_counter += local_count;
            contains = true;
        }
    }
    
    if (out_values_count)
        *out_values_count = values_counter;
    return contains;
}

bool KVStoreReaderProxy::SetBlock(BlockContext* block, mpsize_t store_index)
{
    if (store_index >= m_stores.size() || !block)
        return false;
    return m_stores[store_index] && m_stores[store_index]->SetBlock(block);
}

void KVStoreReaderProxy::ClearCaches()
{
    for (mpsize_t i = 0; i < m_active_sources; ++i)
    {
        KVStore* store = m_stores[i];
        if (store)
            store->ClearCaches();
    }
}

ntd::vector<uint32_t>& KVStoreReaderProxy::GetAllValuesBufRef()
{
    return all_values_buf;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
bool SimpleKVLog::PutKeyValue(const uint32_t* key_data, mpsize_t key_size_in_u32, const uint32_t* value, mpsize_t value_size_in_u32)
{
    if (!m_block || !m_block->headerPtr || !key_data || !value ||
        key_size_in_u32 == 0 || key_size_in_u32 > KVStore::KEY_MAX_SIZE_IN_U32 ||
        value_size_in_u32 == 0 || value_size_in_u32 > KVStore::VALUE_MAX_SIZE_IN_U32)
        return false;

    const mpsize_t total_elements = key_size_in_u32 + value_size_in_u32;

    // Allocate the buffer on the stack.
    uint32_t stack_buffer[STACK_BUFFER_ELEMENTS];

    // Serialize the key.
    mpsize_t current_idx = 0;
    for (mpsize_t i = 0; i < key_size_in_u32; ++i)
    {
        stack_buffer[current_idx++] = key_data[i] & KVStore::KEY_DATA_MASK;
    }

    // Serialize the value.
    for (mpsize_t i = 0; i < value_size_in_u32; ++i)
    {
        stack_buffer[current_idx++] = (value[i] & KVStore::VALUE_DATA_MASK) | KVStore::VALUE_MARKER_BIT;
    }

    // Append the serialized data to the block's payload.
    return m_container.AppendPayloadData(
        *m_block,
        reinterpret_cast<const uint8_t*>(stack_buffer),
        total_elements * sizeof(uint32_t)
    );
}

bool SimpleKVLog::ExportToKVStore(KVStore& dest_kv_store)
{
    if (!m_block || !m_block->headerPtr)
        return false;

    const mpsize_t total_payload_size = m_block->headerPtr->payload_uncompressed_size;
    if (total_payload_size == 0)
        return true;
    if (total_payload_size % sizeof(uint32_t) != 0)
        return false;

    mpsize_t main_read_offset = 0;

    // The state of our parser, which persists across loop iterations.
    enum class ParseState
    {
        ReadingKey,
        ReadingValue
    };
    ParseState state = ParseState::ReadingKey;
    ntd::vector<uint32_t> current_key;
    current_key.reserve(KVStore::KEY_MAX_SIZE_IN_U32);
    ValueBuf current_value;

    // Helper lambda to process a single uint32 element and update the state machine.
    auto process_element = [&](uint32_t element) -> bool
    {
        if (state == ParseState::ReadingKey)
        {
            if (element & KVStore::VALUE_MARKER_BIT)
            {
                if (current_key.empty())
                    return false;
                state = ParseState::ReadingValue;
                current_value.ResetBufState();
                current_value.PushElement(element & KVStore::VALUE_DATA_MASK);
            }
            else
            {
                if (current_key.size() >= KVStore::KEY_MAX_SIZE_IN_U32)
                    return false;
                current_key.push_back(element);
            }
        }
        else // ReadingValue
        {
            if (element & KVStore::VALUE_MARKER_BIT)
            {
                if (current_value.size_in_u32 >= KVStore::VALUE_MAX_SIZE_IN_U32)
                    return false;
                current_value.PushElement(element & KVStore::VALUE_DATA_MASK);
            }
            else
            {
                if (!dest_kv_store.Put(
                    current_key.data(),
                    to_mpsize(current_key.size()),
                    nullptr,
                    current_value.elements_ptr,
                    current_value.size_in_u32))
                    return false;
                current_key.clear();
                current_key.push_back(element);
                state = ParseState::ReadingKey;
            }
        }
        return true;
    };

    // Main loop to read the entire payload in efficient chunks.
    while (main_read_offset < total_payload_size)
    {
        mpsize_t ready_to_access_len = 0;
        const mpsize_t chunk_size = ntd::min(EXPORT_READ_CHUNK_SIZE, total_payload_size - main_read_offset);
        uint8_t* data_ptr = m_container.GetPayloadDataPtr(*m_block, main_read_offset, chunk_size, &ready_to_access_len);

        if (!data_ptr || ready_to_access_len == 0 ||
            ready_to_access_len > total_payload_size - main_read_offset ||
            ready_to_access_len % sizeof(uint32_t) != 0)
            return false;

        // Since ready_to_access_len is guaranteed to be a multiple of 4, we can process it directly.
        const uint32_t* p_current = reinterpret_cast<const uint32_t*>(data_ptr);
        const uint32_t* p_end = p_current + (ready_to_access_len / sizeof(uint32_t));

        while (p_current < p_end)
        {
            if (!process_element(*p_current))
                return false;
            p_current++;
        }

        main_read_offset += ready_to_access_len;
    }

    // After the loop, if the last record was being read, it needs to be processed.
    if (state == ParseState::ReadingValue && !current_key.empty())
    {
        return dest_kv_store.Put(
            current_key.data(),
            to_mpsize(current_key.size()),
            nullptr,
            current_value.elements_ptr,
            current_value.size_in_u32);
    }

    // A trailing key without a value is an incomplete record, not a successful export.
    return false;
}
    
} // GPRX

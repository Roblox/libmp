// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "block.h"
#include "container_context.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"
#include "ntd_stdlib/functional.h"
#include "ntd_stdlib/type_traits.h"
#include "ntd_stdlib/utility.h"
#include "ntd_stdlib/vector.h"

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Read-only interface for key-value stores
 */
class IKVStoreReader : public ntd::mem::RoutedAllocation
{
public:
    virtual ~IKVStoreReader() = default;
    virtual bool Get(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) = 0;
    virtual const uint32_t* GetAllValues(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) = 0;
    virtual bool GetValueAtIndex(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) = 0;
    virtual bool Contains(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_values_count = nullptr) = 0;
    virtual bool SetBlock(BlockContext* block, mpsize_t store_index = 0) = 0;
    virtual void ClearCaches() = 0;
    virtual ntd::vector<uint32_t>& GetAllValuesBufRef() = 0;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A persistent hash-based key-value storage system designed to live inside a single BlockContext.
 * It is memory-mappable and uses relative offsets for all internal pointers.
 * Codename: k1v1 (kEE-vee)
 * 
 * Keys and values are sequences of uint32 entries.
 * Each key entry must have its MSB set to 0 (31 bits usable). Each value entry must have its two MSBs set to 0 (30 bits usable).
 * Keys are hashed into N+M bit hashes. The lower N-bit part is used to index a preallocated 2^N-slot L1 table.
 * Each slot in that table points to its own dynamically allocated M^2-entry L2 table.
 * The higher M-bit part of the hash is used to look up entries in the L2 table.
 *
 * Each L2 entry points to a list of KV-lines (key-value lines), where keys with the same hash and their corresponding values
 * are stored sequentially. If a KV-line overflows, another line is allocated, and the old one points to the new one.
 *
 * If a second value is inserted for the same key and we choose not to replace the existing value, the key will then hold
 * an array of values. In that case, the original value in the KV-line is replaced with a pointer to a dynamically
 * allocated V-line (value line). V-lines store only the values for the corresponding key. The newest V-line points to
 * the previous one.
 */
class KVStore : public IKVStoreReader
{
public:
    static constexpr bool L1_TABLE_CACHING_ENABLED = true;
    static constexpr mpsize_t KEY_MAX_SIZE_IN_U32 = 8;
    static constexpr mpsize_t VALUE_MAX_SIZE_IN_U32 = 8;
    static constexpr uint32_t KEY_DATA_MASK = 0x7FFFFFFF;
    static constexpr uint32_t VALUE_MARKER_BIT = 0x80000000;
    static constexpr uint32_t VALUE_MARKER_MASK = 0xC0000000;
    static constexpr uint32_t VALUE_DATA_MASK = 0x3FFFFFFF;

    static constexpr mpsize_t L1_HASH_BITS_DEFAULT = 8;
    static constexpr mpsize_t L2_HASH_BITS_DEFAULT = 8;
    static constexpr mpsize_t KV_LINE_TOTAL_ENTRIES_DEFAULT = 32;
    static constexpr mpsize_t V_LINE_TOTAL_ENTRIES_DEFAULT = 16;

    #pragma pack(push, 1) // Disable padding
    struct Config
    {
        uint8_t l1_bits = static_cast<uint8_t>(L1_HASH_BITS_DEFAULT);
        uint8_t l2_bits = static_cast<uint8_t>(L2_HASH_BITS_DEFAULT);
        uint16_t kv_entries = static_cast<uint16_t>(KV_LINE_TOTAL_ENTRIES_DEFAULT);
        uint16_t v_entries = static_cast<uint16_t>(V_LINE_TOTAL_ENTRIES_DEFAULT);
        uint16_t unused0 = 0;
    };
    #pragma pack(pop)

private:
    ContainerContext& m_container;
    BlockContext* m_block = nullptr;
    uint32_t m_block_id = 0;
    ntd::vector<uint32_t> all_values_buf;
    ntd::vector<uint32_t> l1_table_cache;
    bool is_l1_table_cache_valid = false;
    bool is_layout_configured = false;

    // --- Constants for the data structure layout ---
    static constexpr uint32_t V_LINE_PTR_MARKER = 0xC0000000;
    static constexpr uint32_t POINTER_DATA_MASK = 0x3FFFFFFF;

    static constexpr uint64_t METADATA1_CONFIG_OFFSET_MASK = 0xFFFFFFFFull;

    // --- Configurable variables for the data structure layout ---
    uint32_t HASH_LOW_MASK;
    uint32_t HASH_HIGH_SHIFT;
    uint32_t HASH_HIGH_MASK;

    mpsize_t L1_TABLE_ENTRIES;
    mpsize_t L2_TABLE_ENTRIES;
    mpsize_t L1_TABLE_SIZE_BYTES;
    mpsize_t L2_TABLE_SIZE_BYTES;

    mpsize_t KV_LINE_TOTAL_ENTRIES;
    mpsize_t KV_LINE_USABLE_ENTRIES;
    mpsize_t KV_LINE_NEXT_PTR_IDX;
    mpsize_t KV_LINE_SIZE_BYTES;

    mpsize_t V_LINE_TOTAL_ENTRIES;
    mpsize_t V_LINE_USABLE_ENTRIES;
    mpsize_t V_LINE_THRESHOLD_ENTRIES;
    mpsize_t V_LINE_NEXT_PTR_IDX;
    mpsize_t V_LINE_FIRST_PTR_IDX;
    mpsize_t V_LINE_VALUES_COUNT_IDX;
    mpsize_t V_LINE_SIZE_BYTES;
   
    // --- Private helper structs and methods ---

    /**
     * @brief An enum indicating the result of a key search.
     */
    enum class FindStatus
    {
        ERROR_STATE = 0,          // A pointer could not be retrieved.
        KEY_FOUND,
        CHAIN_SEARCHED_NOT_FOUND, // Found a chain, but the key wasn't in it.
        KV_LINE_MISSING,          // The L2 table entry for this hash was empty.
        L2_TABLE_MISSING,         // The L1 table entry for this hash was empty.
    };

    /**
     * @brief A rich result structure returned by FindKey. It provides all necessary
     * information for both read (Get) and write (Put) operations.
     */
    struct FindResult
    {
        FindStatus status;
        
        uint32_t hash_low;
        uint32_t hash_high;

        // --- If KEY_FOUND ---
        uint32_t* kv_line_ptr = nullptr;
        mpsize_t value_start_idx = 0;
        mpsize_t value_len_u32 = 0;

        // --- If CHAIN_SEARCHED_NOT_FOUND ---
        uint32_t* last_kv_line_ptr = nullptr;
        mpsize_t first_empty_idx = 0;

        // --- If KV_LINE_MISSING ---
        uint32_t* l2_table_ptr = nullptr;

        // --- If L2_TABLE_MISSING ---
        uint32_t* l1_table_ptr = nullptr;
        
        uint32_t l2_table_offset = 0; // The offset of the L2 table, if it exists
    };

    bool ConfigureLayout(const Config& config);

    inline static mpsize_t GetKvStoreConfigOffsetFromMeta(const BlockHeader* headerPtr)
    {
        return Block::GetMeta1View(headerPtr)->parts.low;
    }

    inline static void PutKvStoreConfigOffsetToMeta(BlockHeader* headerPtr, mpsize_t data_offset)
    {
        Block::GetMeta1View(headerPtr)->parts.low = data_offset;
    }
    
    inline bool AppendContiguousData(const uint8_t* data, mpsize_t size, uint32_t& out_offset);
    inline bool AppendContiguousData(mpsize_t size, uint32_t& out_offset);
    inline bool AppendAlignment();
    inline uint8_t* GetPayloadPtr(mpsize_t offset, mpsize_t size);
    inline uint32_t* GetPayloadU32Ptr(mpsize_t offset, mpsize_t size);
    inline bool ReadPayloadToBuf(mpsize_t offset, mpsize_t size, uint8_t* buf, mpsize_t buf_size);
    FindResult FindKey(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash = nullptr);
    inline ntd::pair<mpsize_t, mpsize_t> CalcVLinesInfo(const uint32_t* v_line_ptr, mpsize_t value_size_u32, mpsize_t values_count);
    inline ntd::pair<mpsize_t, mpsize_t> CalcValueLocation(mpsize_t value_index, mpsize_t values_per_line);
    inline ntd::pair<mpsize_t, mpsize_t> CalcVLineEndIndexes(const uint32_t* v_line_ptr, mpsize_t value_size_u32, mpsize_t values_count);

    /**
     * @brief Helpers to read or write values from a kv-line or v-line
     */
    inline static void FetchValue(uint32_t* dest_buf, const uint32_t* value_elements, mpsize_t num_elements);
    inline static void PutValue(uint32_t* line_ptr, mpsize_t write_idx, const uint32_t* value, mpsize_t value_size_in_u32);
    inline static void MigrateValue(uint32_t* line_ptr, mpsize_t write_idx, const uint32_t* value, mpsize_t value_size_in_u32);

    inline bool GetValueAtIndexImpl(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr);

public:
    enum class SetStrategy
    {
        PutToArray = 0,
        PutOrReplaceSingle,
        PutNew,
    };

    // Args: new_value_ptr, old_value_ptr, value_size_in_uint32
    // If old_value_ptr = nullptr then there is no old value.
    // new_value_ptr buffer can be modified by the function and the result will be written to the KV store
    using PutOrReplaceCallback = ntd::function<void (uint32_t*, const uint32_t*, mpsize_t)>;

    inline static mphash_t CalculateHash(const uint32_t* key_data, mpsize_t key_size_in_u32);

    bool Initialize(const Config& config);

    // The overload that handles the default case
    inline bool Initialize()
    {
        return Initialize(Config{}); 
    }

    KVStore(ContainerContext& container, BlockContext* block);

    /**
     * @brief Puts the value into the specified key slot.
     * @param strategy Specifies whether the value overwrites the previous value, discards the operation
     * if a previous value already exists, or adds it alongside the previous value, forming an array of values.
     * @param replaceCb An external function that receives both the old and new values before writing anything.
     * It can bypass the new value or modify it before overwriting the old one.
     * @return true if the key-value pair was recorded.
     */    
    bool Put(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* value, mpsize_t value_size_in_u32, SetStrategy strategy = SetStrategy::PutToArray, mpsize_t* out_values_count = nullptr, PutOrReplaceCallback replaceCb = nullptr);
    
    /**
     * @brief Gets the FIRST value associated with a given key.
     * @param out_value (output) The retrieved value. The provided buffer should be able to hold at least VALUE_MAX_SIZE_IN_U32 elements.
     * @param out_value_size_in_u32 (output) The number of uint32_t elements in the value.
     * @param out_values_count (output) The total number of values stored under the provided key.
     * @return true if the key was found, false otherwise.
     */
    bool Get(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) override;

    /**
     * @brief Gets ALL values associated with a given key.
     * @return A ptr to internal buffer with all found values. Returns nullptr if the key is not found.
     * The returned pointer is only valid until the next call to this method or until the object is destroyed.
     */
    const uint32_t* GetAllValues(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) override;

    /**
     * @brief Gets the value at the specified index for a given key, which may store one or multiple values.
     */
    bool GetValueAtIndex(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) override;

    /**
     * @brief Checks whether the key exists (and how many values it contains).
     */
    bool Contains(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_values_count = nullptr) override;

    /**
     * @brief Set BlockContext (m_block) where the KV map is stored.
     */
    bool SetBlock(BlockContext* block, mpsize_t store_index = 0) override;

    /**
     * @brief Clears any cached data and configs.
     */
    void ClearCaches() override;

    /**
     * @brief Getter for the results of the last GetAllValues call.
     */
    ntd::vector<uint32_t>& GetAllValuesBufRef() override;
};

inline mphash_t KVStore::CalculateHash(const uint32_t* key_data, mpsize_t key_size_in_u32)
{
    if (!key_data && key_size_in_u32 > 0)
        return 0;
    uint32_t hash = 2166136261u; // FNV-1a 32-bit offset basis
    for (mpsize_t i = 0; i < key_size_in_u32; ++i)
    {
        hash ^= key_data[i];
        hash *= 16777619u; // FNV-1a 32-bit prime
    }
    return static_cast<mphash_t>(hash);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
struct ValueBuf
{
    static constexpr mpsize_t element_avail_full_bytes = sizeof(uint32_t) - 1;
    static constexpr mpsize_t elements_count = KVStore::VALUE_MAX_SIZE_IN_U32;
    uint32_t elements[elements_count];
    mpsize_t capacity_in_u32 = elements_count;
    uint32_t* elements_ptr = &elements[0];
    mpsize_t size_in_u32 = 0;
    mpsize_t cursor_pos = 0;
    bool sequential_mode = false;

    void SetSequentialMode(bool x)
    {
        sequential_mode = x;
    }

    void ResetCursor()
    {
        cursor_pos = 0;
    }

    void ResetBufState(bool reset_external_ptr = false)
    {
        if (reset_external_ptr)
        {
            elements_ptr = &elements[0];
            capacity_in_u32 = elements_count;
        }
        size_in_u32 = 0;
        cursor_pos = 0;
    }

    void AssignExternalBuf(uint32_t* entries, mpsize_t count)
    {
        elements_ptr = entries;
        capacity_in_u32 = count;
        size_in_u32 = count;
        cursor_pos = 0;
    }

    void PushElement(uint32_t element)
    {
        if (cursor_pos < capacity_in_u32)
        {
            elements_ptr[cursor_pos] = element;
            cursor_pos++;
            size_in_u32 = cursor_pos;
        }
    }

    bool HasElements(mpsize_t expected_num)
    {
        if (sequential_mode)
        {
            const mpsize_t remainder_in_u32 = size_in_u32 - cursor_pos;
            return remainder_in_u32 >= expected_num;
        }
        return size_in_u32 == expected_num;
    }

    void MoveCursor(mpsize_t num)
    {
        if (sequential_mode)
            cursor_pos = ntd::min(cursor_pos + num, size_in_u32);
    }

    void FromU32(uint32_t value_30bit)
    {
        WriteWrapper([&]()
        {
            PushElement(value_30bit);
        });
    }

    uint32_t ToU32()
    {
        return ReadWrapper(1, [&]()
        {
            return elements_ptr[cursor_pos];
        });
    }

    void FromI32(int32_t val)
    {
        // Negative numbers are preserved correctly as long as they fit within the 30-bit range
        uint32_t raw = static_cast<uint32_t>(val) & MASK_30_BIT;
        WriteWrapper([&]()
        {
            PushElement(raw);
        });
    }

    int32_t ToI32()
    {
        return ReadWrapper(1, [&]() 
        {
            return GetI32();
        });
    }

    void FromU64(uint64_t value_60bit)
    {
        // Split the 60-bit value into two 30-bit parts.
        uint32_t entry0 = static_cast<uint32_t>(value_60bit & MASK_30_BIT);
        uint32_t entry1 = static_cast<uint32_t>((value_60bit >> 30) & MASK_30_BIT);
        WriteWrapper([&]()
        {
            PushElement(entry0);
            PushElement(entry1);
        });
    }

    uint64_t ToU64()
    {
        return ReadWrapper(2, [&]()
        {
            return GetU64();
        });
    }
    
    void FromI64(int64_t value_60bit_sign)
    {
        // Cast to uint64_t to get raw bits
        uint64_t raw_value = static_cast<uint64_t>(value_60bit_sign);
        FromU64(raw_value);
    }

    int64_t ToI64()
    {
        return ReadWrapper(2, [&]()
        {
            return GetI64();
        });
    }

    void FromDouble(double val)
    {
        // Pack 64-bit double into 60 bits (Drop the 4 LSBs of precision)
        uint64_t raw_dbl;
        ntd::memcpy(&raw_dbl, &val, sizeof(double));
        uint64_t d_packed = raw_dbl >> 4;
        FromU64(d_packed);
    }

    double ToDouble()
    {
        return ReadWrapper(2, [&]()
        {
            return GetDouble();
        });
    }
    
    void FromLocator(const Locator& locator)
    {
        WriteWrapper([&]()
        {
            PushElement(locator.parts.low);
            PushElement(locator.parts.high);
        });
    }

    Locator ToLocator()
    {
        return ReadWrapper(2, [&]()
        {
            Locator locator;
            locator.parts.low = elements_ptr[cursor_pos + 0];
            locator.parts.high = elements_ptr[cursor_pos + 1];
            return locator;
        });
    }

    void FromString(const char* source, mpsize_t source_len = 0);
    mpsize_t ToString(char* destination, mpsize_t dest_capacity_char, bool force_null_term = false, ntd::function<bool (mpsize_t)> reserveCb = nullptr);

protected:
    // A mask to extract 30 bits of data. 0x3FFFFFFF = 0b00111111...
    const uint32_t MASK_30_BIT = KVStore::VALUE_DATA_MASK;

    uint32_t GetU32(mpsize_t offset = 0)
    {
        return elements_ptr[cursor_pos + offset];
    }

    int32_t GetI32(mpsize_t offset = 0)
    {
        uint32_t raw = GetU32(offset);
        // Sign extension logic.
        // The final shift fills the upper bits with the sign bit.
        return (static_cast<int32_t>(raw << 2) >> 2);
    }

    uint64_t GetU64(mpsize_t offset = 0)
    {
        uint64_t lower_part = elements_ptr[cursor_pos + offset + 0];
        uint64_t upper_part = elements_ptr[cursor_pos + offset + 1];
        uint64_t value = (upper_part << 30) | lower_part;
        return value;
    }

    int64_t GetI64(mpsize_t offset = 0)
    {
        uint64_t raw_value = GetU64(offset);
        // Perform sign-extension to convert from 60-bit signed to 64-bit signed
        // 1. Shift left by 4 to move the 60-bit sign bit to the 64-bit sign bit position
        // 2. Cast to int64_t to make the compiler treat it as signed
        // 3. Arithmetic right shift by 4 to copy the sign bit into the top 4 bits
        int64_t value = static_cast<int64_t>(raw_value << 4) >> 4;
        return value;
    }

    double GetDouble(mpsize_t offset = 0)
    {
        double result;

        // Read 60-bit payload
        uint64_t d_packed = GetU64(offset);

        // Restore to 64-bit alignment and save to double
        uint64_t raw_dbl = d_packed << 4;
        ntd::memcpy(&result, &raw_dbl, sizeof(double));

        return result;
    }

    inline void DefaultModeResetBufState()
    {
        if (!sequential_mode)
            ResetBufState();
    }

    inline void DefaultModeResetCursor()
    {
        if (!sequential_mode)
            cursor_pos = 0;
    }

    template <typename Func>
    void WriteWrapper(Func func)
    {
        DefaultModeResetBufState();
        func();
        DefaultModeResetCursor();
    }

    template <typename Func>
    auto ReadWrapper(mpsize_t required_num, Func func) -> ntd::invoke_result_t<Func>
    {
        using ReturnType = ntd::invoke_result_t<Func>;
        if (!HasElements(required_num))
            return ReturnType{};
        auto value = func();
        MoveCursor(required_num);
        return value;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Serves as a read-only proxy for one or more KVStore instances.
 * It combines search results from all underlying stores.
 */
class KVStoreReaderProxy : public IKVStoreReader
{
private:
    // A list of KVStore instances to query.
    ntd::vector<KVStore*> m_stores;
    bool m_owns_sources = false;
    mpsize_t m_active_sources = 0;

    ntd::vector<uint32_t> all_values_buf;

    inline bool GetValueAtIndexImpl(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr);

public:
    /**
     * @brief Constructs the proxy.
     * @param stores A vector of pointers to the KVStore instances to be queried.
     * Until SetOwnsSources is set to True, the caller is responsible for ensuring that
     * the KVStore objects outlive this proxy.
     */
    explicit KVStoreReaderProxy(const ntd::vector<KVStore*>& stores)
    {
        if (stores.size() > max_mpsize_t)
        {
            XASSERT_DIAG(false,
                "KVStoreReaderProxy - source count exceeds mpsize_t");
            return;
        }
        m_stores = stores;
    }
    explicit KVStoreReaderProxy() {}
    ~KVStoreReaderProxy() override;

    void SetOwnsSources(bool x);
    void SetActiveSourcesNum(mpsize_t num);
    ntd::vector<KVStore*>& GetSources();
    mpsize_t GetSourcesNum();
    /** Adds a source, returning false if the source is null or the count is not representable. */
    bool AddSource(KVStore* store);
    void ClearSources();

    bool Get(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) override;
    const uint32_t* GetAllValues(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) override;
    bool GetValueAtIndex(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t index, uint32_t* out_value, mpsize_t* out_value_size_in_u32 = nullptr, mpsize_t* out_values_count = nullptr) override;
    bool Contains(const uint32_t* key_data, mpsize_t key_size_in_u32, const mphash_t* precomputed_hash, mpsize_t* out_values_count = nullptr) override;
    bool SetBlock(BlockContext* block, mpsize_t store_index = 0) override;
    void ClearCaches() override;
    ntd::vector<uint32_t>& GetAllValuesBufRef() override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief A simple, append-only key-value storage system that logs entries sequentially
 * into a single BlockContext. It does not provide any lookup capabilities, but can be
 * exported to a KVStore.
 */
class SimpleKVLog
{
private:
    ContainerContext& m_container;
    BlockContext* m_block;

    // Define a reasonable size for a stack-based buffer used for PutKeyValue.
    static constexpr mpsize_t STACK_BUFFER_ELEMENTS = KVStore::KEY_MAX_SIZE_IN_U32 + KVStore::VALUE_MAX_SIZE_IN_U32;
    // Define the chunk size for reading all key-value pairs for ExportToKVStore.
    static constexpr mpsize_t EXPORT_READ_CHUNK_SIZE = 8 * 1024;

public:
    SimpleKVLog(ContainerContext& container, BlockContext* block)
      : m_container(container), m_block(block) {}

    bool PutKeyValue(const uint32_t* key_data, mpsize_t key_size_in_u32, const uint32_t* value, mpsize_t value_size_in_u32);
    bool ExportToKVStore(KVStore& dest_kv_store);
};
    
} // GPRX

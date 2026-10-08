// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "kv_store.h"
#include "ntd_stdlib/iterator.h"

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
* @brief A list of data retrieval queries, where the result is specified first,
* followed by the input arguments. 
* For example, 'CounterValue_u24CounterId_u32FrameId' means that we are
* querying the value for a specific counter in a specific game frame, where:
*   1) The counter ID is passed as the first argument.
*   2) The frame number is passed as the second argument.
*   3) The operation code (Query) may be encoded in the most significant byte of the
*      first argument for a more compact representation.
* It is expected that this enum will be extended as QueryEx to include
* application-specific queries.
*/
#define GPRX_QUERIES_BASIC \
    Invalid = 0,

enum class Query : uint32_t
{
    GPRX_QUERIES_BASIC
};

template <typename T>
inline constexpr Query ToQuery(T value) { return static_cast<Query>(value); }

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Each data retrieval query has a unique UID (a key), where the operation code and arguments
 * are tightly packed whenever possible. For this key, a hash is precomputed, which speeds up further
 * hash table lookups.
 */
struct Uid
{
    static constexpr uint32_t BigOperationIdMarker = 127;
    static constexpr mpsize_t UID_MAX_ENTRIES = KVStore::KEY_MAX_SIZE_IN_U32;
    uint32_t entries[UID_MAX_ENTRIES];
    uint32_t* entries_ptr = &entries[0];
    mpsize_t size = 0;

    mphash_t precomputed_hash = 0;
    Query operation = Query::Invalid;

    template <typename... Args>
    void Set(Query operation_id, [[maybe_unused]] bool isCompactFormatAllowed = false, Args... args)
    {
        size = 0;
        operation = operation_id;

        uint32_t operation_id_u32 = static_cast<uint32_t>(operation_id);
        bool use_compact_format = false;

        // This block is only compiled if arguments exist.
        // This protects pack_compact from being instantiated with zero arguments.
        if constexpr (sizeof...(args) > 0)
        {
            use_compact_format = isCompactFormatAllowed && operation_id_u32 < BigOperationIdMarker;
            if (use_compact_format)
            {
                // Compact format: op_id is merged with the first argument.
                static_assert(sizeof...(args) <= UID_MAX_ENTRIES, "Too many arguments for Uid capacity in compact mode.");
                pack_compact(operation_id_u32, args...);
            }
        }

        if (!use_compact_format)
        {
            // Standard format: op_id gets its own entry.
            static_assert(1 + sizeof...(args) <= UID_MAX_ENTRIES, "Too many arguments for Uid capacity in standard mode.");
            pack_full(operation_id_u32, args...);
        }

        CalcHash();
    }

    template <typename... Args>
    Uid(Args... args) { Set(args...); }

    Uid() {}

private:
    inline void CalcHash()
    {
        precomputed_hash = KVStore::CalculateHash(&entries[0], size);
    }

    /**
     * @brief Unpacks arguments into the entries array using a C++17 fold expression.
     */
    template <typename... Args>
    void pack_args(Args... args)
    {
        ([&]{
            if (size < ntd::size(entries))
            {
                entries[size++] = args;
            }
        }(), ...);
    }

    /**
     * @brief Helper for compact mode. Merges op_id with the first argument ('head')
     * and then packs the remaining arguments ('tail').
     */
    template <typename... Tail>
    void pack_compact(uint32_t operation_id, uint32_t head, Tail... tail)
    {
        if (size < ntd::size(entries))
        {
            // Most significant byte is the operation_id, lower 24 bits are the argument.
            entries[size++] = (operation_id << 24) | (head & 0x00FFFFFF);
        }
        // Pack the rest of the arguments normally.
        pack_args(tail...);
    }

    template <typename... Args>
    void pack_full(uint32_t operation_id, Args... args)
    {
        if (size < ntd::size(entries))
        {
            // The first entry holds the operation ID. The most significant byte is
            // set to 127 (0x7F) to signify the standard format.
            // The lower 24 bits store the actual operation_id.
            entries[size++] = (BigOperationIdMarker << 24) | (operation_id & 0x00FFFFFF);
        }
        pack_args(args...);
    }
};

} // GPRX

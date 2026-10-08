// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#include "types.h"
#include "utils.h"
#include "ntd_stdlib/array.h"
#include "ntd_stdlib/utility.h"

namespace GPRX
{

#define GPRX_BLOCK_TYPES_BASIC \
    Unknown = 0, \
    Magic = 1, \
    MagicVerMax = 1024, \
    MagicTmp = 1025, \
    Terminator = 1031, \
    GlobalIndex = 1033, \
    Data = 1050, \
    KVLog = 1060, \
    KVStore = 1070, \
    ReservedMax = 2047,

enum class BlockType : uint16_t
{
    GPRX_BLOCK_TYPES_BASIC
};

enum class BlockPayloadEncodingMethod : uint8_t
{
    Raw = 0,
    Base64 = 1,
    LZ4 = 2,
    LZ4_Base64 = 3,
    First = Raw,
    Last = LZ4_Base64,
};

enum class HeaderSerializationMethod : uint8_t
{
    Raw = 0,
    Hex = 1,
};

struct MagicParsed
{
    bool isOk = false;
    bool isFirstHeaderTruncated = false;
    HeaderSerializationMethod headerSerializationMethod = HeaderSerializationMethod::Raw;
};

#pragma pack(push, 1) // Disable padding

struct BlockHeader
{
    uint64_t metadata0;
    uint64_t metadata1;
    BlockType type_id; // uint16_t
    uint8_t type_meta;
    BlockPayloadEncodingMethod encoding_method; // uint8_t
    uint32_t block_id;
    uint32_t payload_uncompressed_size;
    uint32_t payload_size;
    // Total: 32 bytes
};

struct PayloadTrailer
{
    uint32_t payload_actual_size;
    uint32_t distance_between_blocks;
    // Total: 8 bytes
};

#pragma pack(pop)

class Block
{
public:
    constexpr static uint32_t BLOCK_ID_DIFF_OPTIMIZED_MAX = (64 * 1024) / 2;

    inline static U64Split* GetMeta0View(const BlockHeader* headerPtr)
    {
        return reinterpret_cast<U64Split*>(&const_cast<BlockHeader*>(headerPtr)->metadata0);
    }

    inline static U64Split* GetMeta1View(const BlockHeader* headerPtr)
    {
        return reinterpret_cast<U64Split*>(&const_cast<BlockHeader*>(headerPtr)->metadata1);
    }

    inline static mpsize_t GetDataBlockVirtualStartOffsetFromMeta(const BlockHeader* headerPtr)
    {
        return GetMeta1View(headerPtr)->parts.low;
    }

    inline static void PutDataBlockVirtualStartOffsetToMeta(const BlockHeader* headerPtr, mpsize_t offset)
    {
        GetMeta1View(headerPtr)->parts.low = offset;
    }

    inline static bool IsMagic(const BlockHeader* headerPtr)
    {
        return (headerPtr->type_id >= BlockType::Magic) && (headerPtr->type_id <= BlockType::MagicTmp);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
constexpr uint32_t MagicFormat = 'A' | ('0' << 8) | ('B' << 16) | ('F' << 24);
constexpr uint32_t MagicHexEncoding = '<' | ('!' << 8) | ('-' << 16) | ('-' << 24);
constexpr uint32_t MagicRawEncoding = 0;
constexpr mpsize_t MagicPartSize = sizeof(MagicFormat);
constexpr uint64_t MagicFormatRawFull = static_cast<uint64_t>(MagicRawEncoding) | (static_cast<uint64_t>(MagicFormat) << 32);
constexpr uint64_t MagicFormatHexFull = static_cast<uint64_t>(MagicHexEncoding) | (static_cast<uint64_t>(MagicFormat) << 32);

constexpr char BlockPaddingByteHex = '.';
constexpr uint32_t BlockPayloadSizeDeferredMarker = max_u32;
// The alignment size for payloads whose size is determined by a trailer.
constexpr mpsize_t PAYLOAD_TRAILER_SEARCH_STEP = 512;
// The unique 16-byte GUID that marks the end of the trailer.
constexpr ntd::array<uint8_t, 16> PAYLOAD_TRAILER_GUID = {
    0x71, 0x8C, 0x50, 0x7D, 0x3A, 0xBD, 0x43, 0x6A,
    0xA3, 0x1C, 0xB7, 0xA5, 0x2B, 0xB5, 0xE4, 0x24
};
constexpr ntd::array<uint8_t, 32> PAYLOAD_TRAILER_GUID_HEX = bin_to_hex<16>(PAYLOAD_TRAILER_GUID);

constexpr mpsize_t BlockHeaderMetadata0Size = sizeof(decltype(BlockHeader::metadata0));
constexpr mpsize_t BlockHeaderRawSize = sizeof(BlockHeader);
constexpr mpsize_t BlockHeaderHexSize = BlockHeaderRawSize * 2;

constexpr ntd::array<uint8_t, 3> HtmlCommentClose = { '-', '-', '>' };

constexpr mpsize_t BlockTrailerFullSize = sizeof(PayloadTrailer) + sizeof(PAYLOAD_TRAILER_GUID);
constexpr mpsize_t BlockTrailerFullHexSize = BlockTrailerFullSize * 2;

////////////////////////////////////////////////////////////////////////////////////////////////////
MagicParsed MagicParseData(const uint8_t* ptr);
// Hex magic headers store metadata0 as eight literal bytes so the container can begin with
// a text-format marker such as an HTML comment opener.
void HeaderToHex(const BlockHeader& header, uint8_t* out_hex_buf);
// Returns false if a hex-encoded portion of the header contains an invalid character.
bool HeaderFromHex(BlockHeader& header, const uint8_t* hex_buf, bool isMagic = false, bool isTruncated = false);

inline mpsize_t GetBlockHeaderDiskSize(bool isRaw = true) { return isRaw ? BlockHeaderRawSize : BlockHeaderHexSize; }
inline mpsize_t GetBlockTrailerDiskSize(bool isRaw = true) { return isRaw ? BlockTrailerFullSize : BlockTrailerFullHexSize; }
inline char GetPaddingChar(bool isRaw = true) { return isRaw ? '\0' : BlockPaddingByteHex; }

inline bool IsMagicAsciiEncoded(const BlockHeader* headerPtr) { return headerPtr->metadata0 == MagicFormatHexFull; }
inline bool IsPayloadAsciiEncoded(BlockPayloadEncodingMethod encoding_method)
{
    return (
        encoding_method == BlockPayloadEncodingMethod::Base64 ||
        encoding_method == BlockPayloadEncodingMethod::LZ4_Base64
    );
}

// A block is materialized into one contiguous allocation when first decoded.
// Keep corrupt metadata from requesting an effectively unbounded allocation.
constexpr mpsize_t max_decoded_payload_size = 2u * 1024u * 1024u * 1024u;

template <typename T>
inline constexpr BlockType ToBlockType(T value) { return static_cast<BlockType>(value); }

////////////////////////////////////////////////////////////////////////////////////////////////////
inline constexpr mpsize_t LocatorSizeInU32 = 2;
using Locator = U64Split;

inline Locator BuildDataLocator(uint32_t blockId, uint32_t offset)
{
    // The offset is a multiple of alignment.
    uint32_t element_index = static_cast<uint32_t>(offset / kDataAlignment);
    Locator locator;
    locator.parts.low = element_index;
    locator.parts.high = blockId;
    return locator;
}

inline ntd::pair<uint32_t, uint32_t> DecodeLocator(const Locator& locator)
{
    // Decode the locator into its components.
    const uint32_t block_id = locator.parts.high;
    const uint32_t element_index = locator.parts.low;
    const uint32_t data_start_offset = static_cast<uint32_t>(element_index * kDataAlignment);
    return { block_id, data_start_offset };
}

} // GPRX

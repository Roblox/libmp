// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "block.h"
#include "utils_ex.h"
#include "ntd_stdlib/cstring.h"

namespace GPRX
{

void HeaderToHex(const BlockHeader& header, uint8_t* out_hex_buf)
{
    SerializeToHex(&header, BlockHeaderRawSize, out_hex_buf);
    if (Block::IsMagic(&header))
    {
        const decltype(BlockHeader::metadata0) zeroMetadata0 = 0;
        SerializeToHex(&zeroMetadata0, BlockHeaderMetadata0Size, out_hex_buf);
        ntd::memcpy(out_hex_buf, &header.metadata0, BlockHeaderMetadata0Size);
    }
}

bool HeaderFromHex(BlockHeader& header, const uint8_t* hex_buf, bool isMagic, bool isTruncated)
{
    const mpsize_t blockHeaderSizeWithoutMetadata0 = BlockHeaderRawSize - BlockHeaderMetadata0Size;
    const mpsize_t offset = BlockHeaderMetadata0Size * 2 - (isTruncated ? MagicPartSize : 0);
    if (!DeserializeFromHex(hex_buf + offset, blockHeaderSizeWithoutMetadata0 * 2, &header.metadata1))
        return false;
    if (!isMagic && !isTruncated)
    {
        if (!DeserializeFromHex(hex_buf, BlockHeaderMetadata0Size * 2, &header.metadata0))
            return false;
    }
    else if (isMagic && !isTruncated)
    {
        ntd::memcpy(&header.metadata0, hex_buf, BlockHeaderMetadata0Size);
    }
    else if (isMagic && isTruncated)
    {
        header.metadata0 = MagicFormatHexFull;
    }
    else
    {
        header.metadata0 = 0;
    }
    return true;
}

MagicParsed MagicParseData(const uint8_t* ptr)
{
    MagicParsed res{};
    auto magicPtr = reinterpret_cast<const decltype(MagicFormat)*>(ptr);
    auto metadata0 = magicPtr[0];
    if (metadata0 == MagicFormat)
    {
        res.isOk = true;
        res.isFirstHeaderTruncated = true;
        res.headerSerializationMethod = HeaderSerializationMethod::Hex;
    }
    else
    {
        auto metadata1 = magicPtr[1];
        res.isOk = (metadata1 == MagicFormat);
        res.headerSerializationMethod = (metadata0 == MagicHexEncoding) ? HeaderSerializationMethod::Hex : HeaderSerializationMethod::Raw;
    }
    return res;
}

} // GPRX

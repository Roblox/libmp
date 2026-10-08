// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "utils_ex.h"
#include "ntd_stdlib/limits.h"

namespace GPRX
{

////////////////////////////////////////////////////////////////////////////////////////////////////
// Shared read-only zeros, as a BSS array so there is no allocation (in particular
// none before main()). 16-byte aligned to match the allocator's block alignment.
alignas(16) const uint8_t kGlobalZeroBuffer[GLOBAL_ZERO_BUFFER_LEN] = {};

////////////////////////////////////////////////////////////////////////////////////////////////////
void SerializeToHex(const void* data, mpsize_t size, uint8_t* out_hex_buf)
{
    const auto* in_ptr = reinterpret_cast<const uint8_t*>(data);
    uint8_t* out_ptr = out_hex_buf;
    for (mpsize_t i = 0; i < size; i++)
    {
        const uint8_t byte = *in_ptr;
        out_ptr[0] = to_hex_char(static_cast<uint8_t>((byte >> 4) & 0x0F));
        out_ptr[1] = to_hex_char(static_cast<uint8_t>(byte & 0x0F));
        out_ptr += 2;
        in_ptr += 1;
    }
}

inline uint8_t HexCharToInt(uint8_t c)
{
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
    return ntd::numeric_limits<uint8_t>::max(); // Return an invalid value marker
}

bool DeserializeFromHex(const uint8_t* hex_buf, mpsize_t hex_size, void* out_data)
{
    if (hex_size % 2)
        return false;
    auto* out_ptr = reinterpret_cast<uint8_t*>(out_data);
    const auto* in_ptr = hex_buf;
    for (mpsize_t i = 0; i < hex_size / 2; i++)
    {
        const uint8_t high_nibble = HexCharToInt(in_ptr[0]);
        const uint8_t low_nibble = HexCharToInt(in_ptr[1]);
        // HexCharToInt returns 0xFF for a non-hex character. Reject rather than folding
        // the sentinel into the decoded byte.
        if (high_nibble > 0x0F || low_nibble > 0x0F)
            return false;
        *out_ptr = static_cast<uint8_t>((high_nibble << 4) | low_nibble);
        out_ptr += 1;
        in_ptr += 2;
    }
    return true;
}

} // GPRX

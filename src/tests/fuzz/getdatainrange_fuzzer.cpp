// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Fuzzes the full Luau-number validation and range-read boundary used by
// MicroProfilerService::GetDataInRange.  Raw double/int64 bit patterns reach NaN,
// infinities, fractions, negatives, narrowing edges, packed offsets, and short buffers.

#include "gprx_container/data_context.h"
#include "mp_api/integration/data_range.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
GPRX::DataContextMemChunks* g_ctx = nullptr;
uint8_t g_header[32];
uint8_t g_payload[257];
const uint8_t* g_ptrs[2];
mpsize_t g_sizes[2];

constexpr uint32_t kGuard = 32;
constexpr uint32_t kCap = 512;
} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    for (size_t i = 0; i < sizeof(g_payload); ++i)
        g_payload[i] = static_cast<uint8_t>(i * 7 + 3);
    g_ptrs[0] = g_header;  g_sizes[0] = sizeof(g_header);
    g_ptrs[1] = g_payload; g_sizes[1] = sizeof(g_payload);
    g_ctx = new GPRX::DataContextMemChunks();
    g_ctx->AddBlockMemChunks(5, g_ptrs, g_sizes, 2);
    g_ctx->AddBlockMemChunks(6, g_ptrs, g_sizes, 2);
    g_ctx->ProcessBlockInfo();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < sizeof(double) + 3 * sizeof(int64_t))
        return 0;

    double packed_offset = 0;
    int64_t slot_id = 0;
    int64_t read_size = 0;
    int64_t destination_offset = 0;
    std::memcpy(&packed_offset, data, sizeof(packed_offset));
    std::memcpy(&slot_id, data + 8, sizeof(slot_id));
    std::memcpy(&read_size, data + 16, sizeof(read_size));
    std::memcpy(&destination_offset, data + 24, sizeof(destination_offset));

    alignas(8) static uint8_t dst[kGuard + kCap + kGuard];
    std::memset(dst, 0x5A, sizeof(dst));
    uint8_t* out = dst + kGuard;

    LibMP::ValidatedDataRangeArguments args;
    const bool valid = LibMP::ValidateDataRangeArguments(
        slot_id, packed_offset, read_size, kCap, destination_offset, args);
    uint32_t written = 0;
    if (valid)
        written = LibMP::ReadDataInRange(
            *g_ctx, args.packed_offset, args.size, out, kCap, args.destination_offset);

    if (written > (valid ? args.size : 0) || written > kCap)
        __builtin_trap();
    for (uint32_t i = 0; i < kCap; ++i)
    {
        const bool was_written = written > 0 && i >= args.destination_offset &&
            static_cast<uint64_t>(i) <
                static_cast<uint64_t>(args.destination_offset) + written;
        if (!was_written && out[i] != 0x5A)
            __builtin_trap();
    }
    for (uint32_t i = 0; i < kGuard; ++i)
    {
        if (dst[i] != 0x5A || dst[kGuard + kCap + i] != 0x5A)
            __builtin_trap();
    }
    return 0;
}

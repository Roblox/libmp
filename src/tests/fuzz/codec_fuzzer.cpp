// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Fuzzes the LZ4 and Base64 stream DECODERS on arbitrary bytes. These sit on the
// container's decompression path for attacker-influenced payloads, so decoding must
// always terminate (no decompression-bomb hang), never over-write the destination, and
// never over-read the input. ASan/UBSan plus the guard bytes below enforce that.

#include "gprx_container/stream_processor.h"
#include "mp_api/c_api.h" // MpInit() installs the embedded LZ4 backend

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
constexpr uint32_t kGuard = 64;
constexpr uint32_t kCap = 64 * 1024;
uint8_t g_dst[kGuard + kCap + kGuard];
} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    MpInit();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1)
        return 0;
    if (size - 1 > GPRX::max_mpsize_t)
        return 0;

    const bool use_lz4 = (data[0] & 1) != 0;
    const uint8_t* body = data + 1;
    mpsize_t body_size = static_cast<mpsize_t>(size - 1);

    std::memset(g_dst, 0xC3, kGuard);
    std::memset(g_dst + kGuard + kCap, 0xC3, kGuard);
    uint8_t* out = g_dst + kGuard;

    using Dir = GPRX::IStreamProcessor::Direction;
    if (use_lz4)
    {
        GPRX::LZ4Processor dec(Dir::Decode);
        dec.SetMemoryOutput(out, kCap);
        dec.BeginStream();
        dec.ProcessChunk(body, body_size);
        dec.EndStream();
    }
    else
    {
        GPRX::Base64Processor dec(Dir::Decode);
        dec.SetMemoryOutput(out, kCap);
        dec.BeginStream();
        dec.ProcessChunk(body, body_size);
        dec.EndStream();
    }

    for (uint32_t i = 0; i < kGuard; ++i)
    {
        if (g_dst[i] != 0xC3 || g_dst[kGuard + kCap + i] != 0xC3)
            __builtin_trap();
    }
    return 0;
}

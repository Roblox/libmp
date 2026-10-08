// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// codec_tests.cpp -- round-trip and adversarial tests for the LZ4/Base64 stream codecs.
//
// The container decompresses attacker-influenced block payloads through these codecs, so
// beyond correctness they must reject corrupt, truncated, and decompression-bomb input
// cleanly (return false) rather than emitting garbage or looping forever.

#include "test_harness.h"

#include "gprx_container/stream_processor.h"
#include "mp_api/c_api.h" // MpInit() installs the embedded LZ4 backend

#include <cstring>

using GPRX::Base64Processor;
using GPRX::LZ4Processor;
using IStreamProc = GPRX::IStreamProcessor;

namespace
{
void fill_pattern(uint8_t* buf, mpsize_t n, uint32_t seed)
{
    for (mpsize_t i = 0; i < n; ++i)
        buf[i] = static_cast<uint8_t>((i * 131 + seed * 7 + (i >> 3)) & 0xFF);
}

// Single-stage base64 encode of [src, src+n) into out (capacity out_cap). Returns bytes.
mpsize_t base64_encode(const uint8_t* src, mpsize_t n, uint8_t* out, mpsize_t out_cap)
{
    Base64Processor enc(IStreamProc::Direction::Encode);
    enc.SetMemoryOutput(out, out_cap);
    enc.BeginStream();
    if ((n > 0 && !enc.ProcessChunk(src, n)) || !enc.EndStream())
        return 0;
    return GPRX::to_mpsize(enc.GetOwnBytesWritten());
}

bool base64_decode(const uint8_t* src, mpsize_t n, uint8_t* out, mpsize_t out_cap, mpsize_t& out_size)
{
    Base64Processor dec(IStreamProc::Direction::Decode);
    dec.SetMemoryOutput(out, out_cap);
    dec.BeginStream();
    bool ok = dec.ProcessChunk(src, n);
    ok = dec.EndStream() && ok;
    out_size = GPRX::to_mpsize(dec.GetOwnBytesWritten());
    return ok;
}

mpsize_t lz4_compress(const uint8_t* src, mpsize_t n, uint8_t* out, mpsize_t out_cap)
{
    LZ4Processor enc(IStreamProc::Direction::Encode);
    enc.SetMemoryOutput(out, out_cap);
    enc.BeginStream();
    if ((n > 0 && !enc.ProcessChunk(src, n)) || !enc.EndStream())
        return 0;
    return GPRX::to_mpsize(enc.GetOwnBytesWritten());
}

bool lz4_decompress(const uint8_t* src, mpsize_t n, uint8_t* out, mpsize_t out_cap, mpsize_t& out_size)
{
    LZ4Processor dec(IStreamProc::Direction::Decode);
    dec.SetMemoryOutput(out, out_cap);
    dec.BeginStream();
    bool ok = dec.ProcessChunk(src, n);
    ok = dec.EndStream() && ok;
    out_size = GPRX::to_mpsize(dec.GetOwnBytesWritten());
    return ok;
}
} // namespace

// ------------------------------------------------------------------------------------
TEST_CASE(base64_roundtrip_all_lengths)
{
    // Cover every input-length residue mod 3 (the three padding cases) plus 0.
    for (mpsize_t n = 0; n <= 200; ++n)
    {
        uint8_t src[200];
        fill_pattern(src, n, n);
        uint8_t enc[400];
        uint8_t dec[200];

        mpsize_t enc_size = base64_encode(src, n, enc, sizeof(enc));
        CHECK((enc_size % 4) == 0); // canonical base64 is 4-char aligned

        mpsize_t dec_size = 0;
        CHECK(base64_decode(enc, enc_size, dec, sizeof(dec), dec_size));
        CHECK_EQ(dec_size, n);
        CHECK(std::memcmp(src, dec, n) == 0);
    }
}

TEST_CASE(base64_rejects_invalid_char)
{
    // A well-formed group with one non-alphabet, non-padding character must be rejected.
    const uint8_t bad[] = { 'A', 'B', '!', 'D' };
    uint8_t out[8];
    mpsize_t out_size = 0;
    CHECK(!base64_decode(bad, sizeof(bad), out, sizeof(out), out_size));
}

TEST_CASE(base64_rejects_bad_padding)
{
    // "XX=X": padding in the middle of a group is illegal.
    const uint8_t bad[] = { 'A', 'B', '=', 'D' };
    uint8_t out[8];
    mpsize_t out_size = 0;
    CHECK(!base64_decode(bad, sizeof(bad), out, sizeof(out), out_size));
}

TEST_CASE(base64_rejects_noncanonical_padding_bits)
{
    uint8_t out[8];
    mpsize_t out_size = 0;

    // Both spellings would decode to the same bytes as canonical "TQ==" / "TWE="
    // if the otherwise-unused low bits were silently discarded.
    const uint8_t two_padding[] = { 'T', 'R', '=', '=' };
    const uint8_t one_padding[] = { 'T', 'W', 'F', '=' };
    CHECK(!base64_decode(two_padding, sizeof(two_padding), out, sizeof(out), out_size));
    CHECK(!base64_decode(one_padding, sizeof(one_padding), out, sizeof(out), out_size));

    // The same unused-bit rule applies to accepted, unpadded terminal groups.
    const uint8_t unpadded_two[] = { 'T', 'R' };
    const uint8_t unpadded_three[] = { 'T', 'W', 'F' };
    CHECK(!base64_decode(unpadded_two, sizeof(unpadded_two), out, sizeof(out), out_size));
    CHECK(!base64_decode(unpadded_three, sizeof(unpadded_three), out, sizeof(out), out_size));
}

TEST_CASE(base64_rejects_data_after_padding)
{
    uint8_t out[8];
    mpsize_t out_size = 0;
    const uint8_t same_chunk[] = { 'T', 'Q', '=', '=', 'A', 'A', 'A', 'A' };
    CHECK(!base64_decode(same_chunk, sizeof(same_chunk), out, sizeof(out), out_size));

    Base64Processor split(IStreamProc::Direction::Decode);
    split.SetMemoryOutput(out, sizeof(out));
    split.BeginStream();
    const uint8_t padded[] = { 'T', 'Q', '=', '=' };
    const uint8_t following[] = { 'A', 'A', 'A', 'A' };
    CHECK(split.ProcessChunk(padded, sizeof(padded)));
    CHECK(!split.ProcessChunk(following, sizeof(following)));
}

TEST_CASE(stream_processors_reject_null_nonempty_input)
{
    uint8_t out[32] = {};

    GPRX::PassThroughProcessor pass;
    pass.SetMemoryOutput(out, sizeof(out));
    pass.BeginStream();
    CHECK(!pass.ProcessChunk(nullptr, 1));

    GPRX::StreamProcessorChunkedMemory chunks(8);
    chunks.BeginStream();
    CHECK(!chunks.ProcessChunk(nullptr, 1));

    Base64Processor base64(IStreamProc::Direction::Decode);
    base64.SetMemoryOutput(out, sizeof(out));
    base64.BeginStream();
    CHECK(!base64.ProcessChunk(nullptr, 1));

    MpInit();
    LZ4Processor lz4(IStreamProc::Direction::Decode);
    lz4.SetMemoryOutput(out, sizeof(out));
    lz4.BeginStream();
    CHECK(!lz4.ProcessChunk(nullptr, 1));
}

TEST_CASE(base64_rejects_orphan_char)
{
    // A single trailing base64 character cannot form a byte.
    const uint8_t bad[] = { 'A' };
    uint8_t out[8];
    mpsize_t out_size = 0;
    CHECK(!base64_decode(bad, sizeof(bad), out, sizeof(out), out_size));
}

TEST_CASE(base64_rejects_combined_length_overflow)
{
    const uint8_t byte = 'A';

    Base64Processor decoder(IStreamProc::Direction::Decode);
    decoder.BeginStream();
    CHECK(decoder.ProcessChunk(&byte, 1));
    CHECK(!decoder.ProcessChunk(&byte, GPRX::max_mpsize_t));

    Base64Processor encoder(IStreamProc::Direction::Encode);
    encoder.BeginStream();
    CHECK(encoder.ProcessChunk(&byte, 1));
    CHECK(!encoder.ProcessChunk(&byte, GPRX::max_mpsize_t));
}

TEST_CASE(lz4_roundtrip)
{
    MpInit();
    const mpsize_t sizes[] = { 0, 1, 7, 64, 1000, 64 * 1024, 200 * 1024 };
    for (mpsize_t n : sizes)
    {
        ntd::vector<uint8_t> src(n);
        fill_pattern(src.data(), n, 1234);
        ntd::vector<uint8_t> comp(n + n / 2 + 4096);
        ntd::vector<uint8_t> plain(n + 16);

        mpsize_t comp_size = lz4_compress(src.data(), n, comp.data(), GPRX::to_mpsize(comp.size()));
        CHECK(comp_size > 0);

        mpsize_t plain_size = 0;
        CHECK(lz4_decompress(comp.data(), comp_size, plain.data(), GPRX::to_mpsize(plain.size()), plain_size));
        CHECK_EQ(plain_size, n);
        if (n > 0)
            CHECK(std::memcmp(src.data(), plain.data(), n) == 0);
    }
}

TEST_CASE(lz4_rejects_small_encoding_destination)
{
    MpInit();
    uint8_t source[4096];
    fill_pattern(source, sizeof(source), 17);
    uint8_t destination[16];

    LZ4Processor encoder(IStreamProc::Direction::Encode);
    encoder.SetMemoryOutput(destination, sizeof(destination));
    encoder.BeginStream();
    bool ok = encoder.ProcessChunk(source, sizeof(source));
    ok = encoder.EndStream() && ok;

    CHECK(!ok);
    CHECK(encoder.GetOwnBytesWritten() <= sizeof(destination));
}

TEST_CASE(lz4_rejects_truncated_frame)
{
    MpInit();
    uint8_t plain[8192];
    mpsize_t plain_size = 0;
    CHECK(!lz4_decompress(nullptr, 0, plain, sizeof(plain), plain_size));

    const mpsize_t n = 4096;
    uint8_t src[4096];
    fill_pattern(src, n, 99);
    uint8_t comp[8192];
    mpsize_t comp_size = lz4_compress(src, n, comp, sizeof(comp));
    CHECK(comp_size > 8);

    // Feed all but the last few bytes: the frame never completes -> EndStream fails.
    CHECK(!lz4_decompress(comp, comp_size - 4, plain, sizeof(plain), plain_size));
}

TEST_CASE(lz4_rejects_corrupt_frame)
{
    MpInit();
    const mpsize_t n = 4096;
    uint8_t src[4096];
    fill_pattern(src, n, 7);
    uint8_t comp[8192];
    mpsize_t comp_size = lz4_compress(src, n, comp, sizeof(comp));
    CHECK(comp_size > 16);

    // Corrupt the compressed block body (past the frame header).
    for (mpsize_t i = 8; i < comp_size; ++i)
        comp[i] ^= 0x5A;

    uint8_t plain[8192];
    mpsize_t plain_size = 0;
    // Must fail (isError) or produce a bounded, non-matching result -- never hang.
    bool ok = lz4_decompress(comp, comp_size, plain, sizeof(plain), plain_size);
    if (ok)
        CHECK(!(plain_size == n && std::memcmp(src, plain, n) == 0)); // corruption cannot round-trip
}

// The infinite-loop guard: a payload whose decompressed size exceeds the destination
// must fail cleanly instead of spinning forever.
TEST_CASE(lz4_bomb_bounded_destination)
{
    MpInit();
    const mpsize_t n = 256 * 1024; // compresses tiny (all-zero), expands past a small dest
    ntd::vector<uint8_t> src(n, 0);
    ntd::vector<uint8_t> comp(n + n / 2 + 4096);
    mpsize_t comp_size = lz4_compress(src.data(), n, comp.data(), GPRX::to_mpsize(comp.size()));
    CHECK(comp_size > 0);

    uint8_t small_dest[1024];
    mpsize_t plain_size = 0;
    CHECK(!lz4_decompress(comp.data(), comp_size, small_dest, sizeof(small_dest), plain_size));
    CHECK(plain_size <= sizeof(small_dest)); // never wrote past the destination
}

// Feeding data one small chunk at a time must round-trip identically to a single call.
// This exercises the carry-over/leftover path across every group boundary.
TEST_CASE(base64_chunked_feeding)
{
    const mpsize_t chunk_sizes[] = { 1, 2, 3, 5, 7 };
    for (mpsize_t cs : chunk_sizes)
    {
        const mpsize_t n = 137;
        uint8_t src[137];
        fill_pattern(src, n, cs);

        uint8_t enc[400];
        Base64Processor e(IStreamProc::Direction::Encode);
        e.SetMemoryOutput(enc, sizeof(enc));
        e.BeginStream();
        for (mpsize_t i = 0; i < n; i += cs)
            CHECK(e.ProcessChunk(src + i, ntd::min(cs, n - i)));
        CHECK(e.EndStream());
        mpsize_t enc_size = static_cast<mpsize_t>(e.GetOwnBytesWritten());

        uint8_t dec[137];
        Base64Processor d(IStreamProc::Direction::Decode);
        d.SetMemoryOutput(dec, sizeof(dec));
        d.BeginStream();
        bool ok = true;
        for (mpsize_t i = 0; i < enc_size; i += cs)
            ok = d.ProcessChunk(enc + i, ntd::min(cs, enc_size - i)) && ok;
        ok = d.EndStream() && ok;
        CHECK(ok);
        CHECK_EQ(d.GetOwnBytesWritten(), n);
        CHECK(std::memcmp(src, dec, n) == 0);
    }
}

TEST_CASE(lz4_chunked_feeding)
{
    MpInit();
    const mpsize_t n = 5000;
    ntd::vector<uint8_t> src(n);
    fill_pattern(src.data(), n, 42);
    ntd::vector<uint8_t> comp(n + n / 2 + 4096);
    mpsize_t comp_size = lz4_compress(src.data(), n, comp.data(), GPRX::to_mpsize(comp.size()));
    CHECK(comp_size > 0);

    for (mpsize_t cs : { (mpsize_t)1, (mpsize_t)13, (mpsize_t)512 })
    {
        ntd::vector<uint8_t> plain(n + 16);
        LZ4Processor d(IStreamProc::Direction::Decode);
        d.SetMemoryOutput(plain.data(), GPRX::to_mpsize(plain.size()));
        d.BeginStream();
        bool ok = true;
        for (mpsize_t i = 0; i < comp_size; i += cs)
            ok = d.ProcessChunk(comp.data() + i, ntd::min(cs, comp_size - i)) && ok;
        ok = d.EndStream() && ok;
        CHECK(ok);
        CHECK_EQ(d.GetOwnBytesWritten(), n);
        CHECK(std::memcmp(src.data(), plain.data(), n) == 0);
    }
}

LIBMP_TEST_MAIN("codec")

// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// getdatainrange_tests.cpp -- tests for the shared GetDataInRange helper (data_range.h)
// against a DataContextMemChunks, the same context the engine's live capture uses.
//
// Covers offset decoding (block-relative vs absolute), destination-window validation,
// and rejection of out-of-range or wrapping block offsets.

#include "test_harness.h"

#include "gprx_container/data_context.h"
#include "mp_api/integration/data_range.h"

#include <cstring>
#include <limits>

namespace
{
constexpr uint32_t kBlockId = 5;
constexpr mpsize_t kHeaderSize = 32;
constexpr mpsize_t kPayloadSize = 100;

// Packs a block id + block-relative offset the way MicroProfilerService encodes them.
uint64_t pack_block(uint32_t block_id, uint32_t offset)
{
    return (uint64_t(1) << 52) | (uint64_t(block_id) << 32) | offset;
}

struct Fixture
{
    alignas(8) uint8_t header[kHeaderSize] = {};
    uint8_t payload[kPayloadSize];
    const uint8_t* ptrs[2];
    mpsize_t sizes[2];
    GPRX::DataContextMemChunks ctx;

    Fixture()
    {
        for (mpsize_t i = 0; i < kPayloadSize; ++i)
            payload[i] = static_cast<uint8_t>(i * 3 + 1);
        ptrs[0] = header;  sizes[0] = kHeaderSize;
        ptrs[1] = payload; sizes[1] = kPayloadSize;
        CHECK(ctx.AddBlockMemChunks(kBlockId, ptrs, sizes, 2));
        ctx.ProcessBlockInfo();
        // payload sits at absolute offset kHeaderSize; total size is header + payload.
    }
};
} // namespace

TEST_CASE(getdatainrange_decode_offset)
{
    LibMP::DecodedDataOffset b = LibMP::DecodeDataOffset(pack_block(kBlockId, 40));
    CHECK(b.is_block_relative);
    CHECK_EQ(b.block_id, kBlockId);
    CHECK_EQ(b.offset, 40);

    LibMP::DecodedDataOffset a = LibMP::DecodeDataOffset(1234);
    CHECK(!a.is_block_relative);
    CHECK_EQ(a.offset, 1234);

    // Block id zero is the API's absolute-read sentinel. Preserve the engine's
    // existing decode semantics even when callers redundantly set the marker bit.
    LibMP::DecodedDataOffset zero = LibMP::DecodeDataOffset(pack_block(0, 17));
    CHECK(!zero.is_block_relative);
    CHECK_EQ(zero.offset, 17u);
}

TEST_CASE(getdatainrange_validates_luau_numbers_before_cast)
{
    LibMP::ValidatedDataRangeArguments out;
    CHECK(LibMP::ValidateDataRangeArguments(0, 42.0, 8, 32, 4, out));
    CHECK_EQ(out.slot_id, 0u);
    CHECK_EQ(out.packed_offset, 42u);
    CHECK_EQ(out.size, 8u);
    CHECK_EQ(out.destination_offset, 4u);

    CHECK(!LibMP::ValidateDataRangeArguments(-1, 0.0, 1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(0, -1.0, 1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(0, 1.5, 1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(
        0, std::numeric_limits<double>::quiet_NaN(), 1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(
        0, std::numeric_limits<double>::infinity(), 1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(
        0, static_cast<double>(LibMP::MAX_EXACT_LUAU_INTEGER) + 1.0, 1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(0, 0.0, -1, 16, 0, out));
    CHECK(!LibMP::ValidateDataRangeArguments(0, 0.0, 1, 16, -1, out));
    CHECK(!LibMP::ValidateDataRangeArguments(0, 0.0, 8, 16, 9, out));
    CHECK(LibMP::ValidateDataRangeArguments(
        0, static_cast<double>(LibMP::MAX_EXACT_LUAU_INTEGER), 0, 0, 0, out));
}

TEST_CASE(getdatainrange_block_read)
{
    Fixture f;
    uint8_t dst[64] = {};
    uint32_t n = LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, 10), 20, dst, sizeof(dst), 0);
    CHECK_EQ(n, 20);
    CHECK(std::memcmp(dst, f.payload + 10, 20) == 0);
}

TEST_CASE(getdatainrange_abs_read)
{
    Fixture f;
    uint8_t dst[64] = {};
    // Absolute offset kHeaderSize is the start of the payload.
    uint32_t n = LibMP::ReadDataInRange(f.ctx, kHeaderSize, 20, dst, sizeof(dst), 0);
    CHECK_EQ(n, 20);
    CHECK(std::memcmp(dst, f.payload, 20) == 0);
}

TEST_CASE(getdatainrange_dest_offset_honored)
{
    Fixture f;
    uint8_t dst[64];
    std::memset(dst, 0xCC, sizeof(dst));
    uint32_t n = LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, 0), 16, dst, sizeof(dst), 8);
    CHECK_EQ(n, 16);
    CHECK(std::memcmp(dst + 8, f.payload, 16) == 0);
    // Guard bytes before the destination window stay untouched.
    for (int i = 0; i < 8; ++i)
        CHECK_EQ(dst[i], 0xCC);
}

TEST_CASE(getdatainrange_rejects_dest_overflow)
{
    Fixture f;
    uint8_t dst[16] = {};
    // dest_offset + size exceeds capacity -> reject, no write.
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, 0), 20, dst, sizeof(dst), 0), 0u);
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, 0), 10, dst, sizeof(dst), 10), 0u);
    // null destination.
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, 0), 4, nullptr, 16, 0), 0u);
}

TEST_CASE(getdatainrange_rejects_oob_block_offset)
{
    Fixture f;
    uint8_t dst[256] = {};
    // Offset at/after the payload end.
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, kPayloadSize), 1, dst, sizeof(dst), 0), 0u);
    // Range straddling the end.
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, kPayloadSize - 4), 8, dst, sizeof(dst), 0), 0u);
    // Reject a range whose offset plus size would wrap a 32-bit addition.
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(kBlockId, 0xFFFFFF00u), 0x200, dst, sizeof(dst), 0), 0u);
}

TEST_CASE(getdatainrange_rejects_unknown_block)
{
    Fixture f;
    uint8_t dst[64] = {};
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, pack_block(999, 0), 4, dst, sizeof(dst), 0), 0u);
}

TEST_CASE(getdatainrange_rejects_oob_absolute)
{
    Fixture f;
    uint8_t dst[64] = {};
    const uint64_t total = kHeaderSize + kPayloadSize;
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, total, 1, dst, sizeof(dst), 0), 0u);
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, total - 4, 8, dst, sizeof(dst), 0), 0u);
    // A very large (bit-52-range) absolute offset must not read anything.
    CHECK_EQ(LibMP::ReadDataInRange(f.ctx, (uint64_t(1) << 51), 4, dst, sizeof(dst), 0), 0u);
    // Values that cannot be represented exactly by a Luau number are outside this API.
    CHECK_EQ(LibMP::ReadDataInRange(
        f.ctx, LibMP::MAX_EXACT_LUAU_INTEGER + 1, 4, dst, sizeof(dst), 0), 0u);
}

LIBMP_TEST_MAIN("getdatainrange")

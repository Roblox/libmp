// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Focused coverage for every GPRX data-source implementation and the cache layer.

#include "test_harness.h"

#include "gprx_container/data_context.h"

#include <cstring>

namespace
{
struct ExternalFixture
{
    const uint8_t* bytes = nullptr;
    mpsize_t size = 0;
    const uint8_t* block = nullptr;
    mpsize_t block_size = 0;
    uint32_t block_id = 0;
    int opens = 0;
    int closes = 0;
    bool over_report = false;
    bool fail_reads = false;
};

uint32_t external_open(uint8_t* handle)
{
    reinterpret_cast<ExternalFixture*>(handle)->opens++;
    return 1;
}

uint32_t external_close(uint8_t* handle)
{
    reinterpret_cast<ExternalFixture*>(handle)->closes++;
    return 1;
}

mpdatasz_t external_size(uint8_t* handle)
{
    return reinterpret_cast<ExternalFixture*>(handle)->size;
}

mpsize_t external_read(
    uint8_t* handle, uint8_t* destination, uint32_t block_id,
    mpdatasz_t offset, mpsize_t size)
{
    ExternalFixture& fixture = *reinterpret_cast<ExternalFixture*>(handle);
    if (fixture.fail_reads)
        return 0;
    const uint8_t* source = block_id == 0 ? fixture.bytes : fixture.block;
    const mpdatasz_t source_size = block_id == 0 ? fixture.size : fixture.block_size;
    if ((block_id != 0 && block_id != fixture.block_id) || !source || !destination ||
        !GPRX::range_within<mpdatasz_t>(offset, size, source_size))
        return 0;
    std::memcpy(destination, source + offset, size);
    return fixture.over_report ? size + 1 : size;
}

GPRX::ExternalIO make_external_io(bool block_reads)
{
    GPRX::ExternalIO io;
    io.open = external_open;
    io.close = external_close;
    io.getSize = external_size;
    io.read = external_read;
    io.isBlockFetchSupported = block_reads;
    return io;
}
} // namespace

TEST_CASE(single_buffer_bounds_and_zero_length)
{
    const uint8_t source[] = { 1, 2, 3, 4, 5 };
    GPRX::DataContextSingleBuffer context;
    context.AssignBuffer(source, sizeof(source));

    uint8_t output[8] = {};
    CHECK_EQ(context.ReadAbsDataToBuffer(1, 3, output, sizeof(output)), 3u);
    CHECK(std::memcmp(output, source + 1, 3) == 0);
    CHECK_EQ(context.ReadAbsDataToBuffer(4, 2, output, sizeof(output)), 0u);
    CHECK_EQ(context.ReadAbsDataToBuffer(0, 1, nullptr, 1), 0u);

    mpsize_t ready = 99;
    CHECK(context.GetAbsDataPtr(sizeof(source), 0, &ready) == source + sizeof(source));
    CHECK_EQ(ready, 0u);
}

TEST_CASE(mem_chunks_cross_chunk_and_backward_scan)
{
    const uint8_t a[] = { 0, 1, 2 };
    const uint8_t b[] = { 3, 4, 5, 6, 7 };
    const uint8_t c[] = { 8, 9, 10, 11 };
    const uint8_t* chunks[] = { a, b, c };
    mpsize_t sizes[] = { sizeof(a), sizeof(b), sizeof(c) };
    GPRX::DataContextMemChunks context;
    CHECK(context.AssignMemChunks(chunks, sizes, 3));
    context.ProcessBlockInfo();

    uint8_t output[12] = {};
    CHECK_EQ(context.ReadAbsDataToBuffer(2, 8, output, sizeof(output)), 8u);
    for (uint8_t i = 0; i < 8; ++i)
        CHECK_EQ(output[i], static_cast<uint8_t>(i + 2));

    mpsize_t ready = 0;
    CHECK(context.GetNextAbsDataPtr(8, 2, &ready) != nullptr);
    CHECK(context.GetNextAbsDataPtr(1, 2, &ready) != nullptr); // stale hint must recover
    CHECK_EQ(ready, 2u);
}

TEST_CASE(mem_chunks_rejects_append_after_assign)
{
    const uint8_t a[] = { 1, 2 };
    const uint8_t b[] = { 3, 4 };
    const uint8_t c[] = { 5, 6 };
    const uint8_t* assigned[] = { a, b };
    mpsize_t assigned_sizes[] = { sizeof(a), sizeof(b) };
    const uint8_t* appended[] = { c };
    mpsize_t appended_sizes[] = { sizeof(c) };

    GPRX::DataContextMemChunks context;
    CHECK(context.AssignMemChunks(assigned, assigned_sizes, 2));
    CHECK(!context.AddMemChunks(appended, appended_sizes, 1));
    context.ProcessBlockInfo();

    uint8_t output[4] = {};
    CHECK_EQ(context.ReadAbsDataToBuffer(0, sizeof(output), output, sizeof(output)), sizeof(output));
    const uint8_t expected[] = { 1, 2, 3, 4 };
    CHECK(std::memcmp(output, expected, sizeof(expected)) == 0);

    // Invalid append inputs leave the existing context untouched.
    CHECK(!context.AddMemChunks(nullptr, nullptr, 1));
    CHECK_EQ(context.GetTotalDataSize(), sizeof(expected));

    context.ClearMemChunks();
    CHECK(context.AddMemChunks(appended, appended_sizes, 1));
    CHECK_EQ(context.GetTotalDataSize(), sizeof(c));
}

TEST_CASE(mem_chunks_inconsistent_metadata_reports_short_copy)
{
    const uint8_t a[] = { 1, 2, 3, 4 };
    const uint8_t b[] = { 5, 6, 7, 8 };
    const uint8_t* chunks[] = { a, b };
    mpsize_t sizes[] = { sizeof(a), sizeof(b) };
    GPRX::DataContextMemChunks context;
    CHECK(context.AssignMemChunks(chunks, sizes, 2));
    context.RememberBlockInfo(9, 0, 8);
    context.ProcessBlockInfo();

    CHECK_EQ(context.ReadBlockDataToBuffer(9, 0, 1, nullptr, 1), 0u);

    // Simulate backing storage becoming shorter after metadata was recorded.
    sizes[1] = 1;
    uint8_t output[8];
    std::memset(output, 0xCC, sizeof(output));
    CHECK_EQ(context.ReadBlockDataToBuffer(9, 0, 8, output, sizeof(output)), 0u);
}

TEST_CASE(caching_proxy_clamps_final_page_and_spans_pages)
{
    uint8_t source[22];
    for (uint8_t i = 0; i < sizeof(source); ++i)
        source[i] = i;

    GPRX::DataContextSingleBuffer underlying;
    underlying.AssignBuffer(source, sizeof(source));
    GPRX::DataContextCachingProxy cache(&underlying, 8, 3);
    cache.RememberBlockInfo(7, 4, 18);
    cache.RememberBlockInfo(8, 21, 2); // exceeds the wrapped source and is ignored
    cache.ProcessBlockInfo();

    mpsize_t ready = 0;
    uint8_t* final = cache.GetBlockDataPtr(7, 16, 1, &ready);
    CHECK(final != nullptr);
    CHECK_EQ(ready, 2u);
    if (final)
        CHECK_EQ(*final, 20u);

    uint8_t* spanning = cache.GetBlockDataPtr(7, 3, 12, &ready);
    CHECK(spanning != nullptr);
    CHECK_EQ(ready, 12u);
    if (spanning)
        CHECK(std::memcmp(spanning, source + 7, 12) == 0);

    CHECK(cache.GetBlockDataPtr(7, 17, 2, &ready) == nullptr);
    CHECK_EQ(ready, 0u);
    CHECK(cache.GetBlockDataPtr(8, 0, 1, &ready) == nullptr);
}

TEST_CASE(caching_proxy_reclaims_a_slot_after_failed_fill)
{
    uint8_t source[24];
    for (uint8_t i = 0; i < sizeof(source); ++i)
        source[i] = static_cast<uint8_t>(i + 1);

    ExternalFixture fixture{source, sizeof(source), nullptr, 0, 0};
    GPRX::DataContextExternalIO underlying;
    underlying.AssignIO(make_external_io(false));
    CHECK(underlying.Open(reinterpret_cast<uint8_t*>(&fixture)));

    GPRX::DataContextCachingProxy cache(&underlying, 8, 2);
    cache.RememberBlockInfo(1, 0, sizeof(source));
    cache.ProcessBlockInfo();

    mpsize_t ready = 0;
    uint8_t* page0 = cache.GetBlockDataPtr(1, 0, 1, &ready);
    uint8_t* page1 = cache.GetBlockDataPtr(1, 8, 1, &ready);
    CHECK(page0 != nullptr);
    CHECK(page1 != nullptr);
    if (page0)
        CHECK_EQ(*page0, 1u);
    if (page1)
        CHECK_EQ(*page1, 9u);

    fixture.fail_reads = true;
    CHECK(cache.GetBlockDataPtr(1, 16, 1, &ready) == nullptr);
    fixture.fail_reads = false;
    uint8_t* page2 = cache.GetBlockDataPtr(1, 16, 1, &ready);
    CHECK(page2 != nullptr);
    if (page2)
        CHECK_EQ(*page2, 17u);
    // Page 1 retained its own physical slot when the failed replacement page was
    // reclaimed; it must not alias the later page-2 fill.
    page1 = cache.GetBlockDataPtr(1, 8, 1, &ready);
    CHECK(page1 != nullptr);
    if (page1)
        CHECK_EQ(*page1, 9u);
}

TEST_CASE(caching_proxy_serves_requests_wider_than_its_page_budget)
{
    uint8_t source[24];
    for (uint8_t i = 0; i < sizeof(source); ++i)
        source[i] = static_cast<uint8_t>(i + 11);

    GPRX::DataContextSingleBuffer underlying;
    underlying.AssignBuffer(source, sizeof(source));
    GPRX::DataContextCachingProxy cache(&underlying, 8, 1);
    cache.RememberBlockInfo(3, 0, sizeof(source));
    cache.ProcessBlockInfo();

    mpsize_t ready = 0;
    uint8_t* result = cache.GetBlockDataPtr(3, 5, 15, &ready);
    CHECK(result != nullptr);
    CHECK_EQ(ready, 15u);
    if (result)
        CHECK(std::memcmp(result, source + 5, 15) == 0);
}

TEST_CASE(external_io_validates_callbacks_ranges_and_counts)
{
    const uint8_t source[] = { 10, 11, 12, 13, 14, 15 };
    ExternalFixture fixture{source, sizeof(source), source + 2, 4, 44};

    {
        GPRX::DataContextExternalIO context;
        GPRX::ExternalIO incomplete;
        incomplete.open = external_open;
        context.AssignIO(incomplete);
        CHECK(!context.Open(reinterpret_cast<uint8_t*>(&fixture)));
    }
    CHECK_EQ(fixture.closes, 0);

    {
        GPRX::DataContextExternalIO context;
        context.AssignIO(make_external_io(false));
        CHECK(context.Open(reinterpret_cast<uint8_t*>(&fixture)));
        uint8_t output[4] = {};
        CHECK_EQ(context.ReadAbsDataToBuffer(1, 4, output, sizeof(output)), 4u);
        CHECK(std::memcmp(output, source + 1, 4) == 0);
        CHECK_EQ(context.ReadAbsDataToBuffer(5, 2, output, sizeof(output)), 0u);
        fixture.over_report = true;
        CHECK_EQ(context.ReadAbsDataToBuffer(0, 2, output, sizeof(output)), 0u);
        fixture.over_report = false;
    }
    CHECK_EQ(fixture.opens, 1);
    CHECK_EQ(fixture.closes, 1);

    {
        GPRX::DataContextExternalIO context;
        context.AssignIO(make_external_io(true));
        CHECK(context.Open(reinterpret_cast<uint8_t*>(&fixture)));
        uint8_t output[4] = {};
        CHECK_EQ(context.ReadBlockDataToBuffer(44, 1, 3, output, sizeof(output)), 3u);
        CHECK(std::memcmp(output, source + 3, 3) == 0);
        CHECK_EQ(context.ReadBlockDataToBuffer(45, 0, 1, output, sizeof(output)), 0u);

        GPRX::DataContextCachingProxy cache(&context, 2, 2);
        cache.SetUseBlockRelativeReads(true);
        cache.RememberBlockInfo(44, 0, fixture.block_size);
        cache.ProcessBlockInfo();
        CHECK_EQ(cache.ReadBlockDataToBuffer(44, 1, 3, output, sizeof(output)), 3u);
        CHECK_EQ(cache.ReadBlockDataToBuffer(44, 3, 2, output, sizeof(output)), 0u);
    }
}

TEST_CASE(external_io_stacked_data_locks_preserve_claimed_buffers)
{
    const uint8_t source[] = {
        10, 11, 12, 13, 20, 21, 22, 23,
        30, 31, 32, 33, 40, 41, 42, 43,
    };
    ExternalFixture fixture{source, sizeof(source), nullptr, 0, 0};
    GPRX::DataContextExternalIO context;
    context.AssignIO(make_external_io(false));
    CHECK(context.Open(reinterpret_cast<uint8_t*>(&fixture)));

    mpsize_t ready = 0;
    CHECK(context.PushDataLock());
    uint8_t* outer = context.GetAbsDataPtr(0, 4, &ready);
    CHECK(outer != nullptr);
    CHECK_EQ(ready, 4u);

    CHECK(context.PushDataLock());
    uint8_t* inner = context.GetAbsDataPtr(4, 4, &ready);
    CHECK(inner != nullptr);
    CHECK_EQ(ready, 4u);

    // Unlocked reads reuse level zero and must not overwrite either claimed buffer.
    CHECK(context.GetAbsDataPtr(8, 4, &ready) != nullptr);
    CHECK(context.GetAbsDataPtr(12, 4, &ready) != nullptr);
    if (outer)
        CHECK(std::memcmp(outer, source, 4) == 0);
    if (inner)
        CHECK(std::memcmp(inner, source + 4, 4) == 0);

    CHECK(context.PopDataLock());
    CHECK(context.GetAbsDataPtr(8, 4, &ready) != nullptr);
    if (outer)
        CHECK(std::memcmp(outer, source, 4) == 0);
    CHECK(context.PopDataLock());
}

TEST_CASE(caching_proxy_stacked_data_locks_pin_claimed_pages)
{
    const uint8_t source[] = {
        10, 11, 12, 13, 20, 21, 22, 23, 30, 31,
        32, 33, 40, 41, 42, 43, 50, 51, 52, 53,
    };
    ExternalFixture fixture{source, sizeof(source), source, sizeof(source), 44};
    GPRX::DataContextExternalIO underlying;
    underlying.AssignIO(make_external_io(true));
    CHECK(underlying.Open(reinterpret_cast<uint8_t*>(&fixture)));

    // Two pages remain pinned while the third slot is repeatedly evicted.
    GPRX::DataContextCachingProxy cache(&underlying, 4, 3);
    cache.SetUseBlockRelativeReads(true);
    cache.RememberBlockInfo(44, 0, sizeof(source));
    cache.ProcessBlockInfo();

    mpsize_t ready = 0;
    CHECK(cache.PushDataLock());
    uint8_t* outer = cache.GetBlockDataPtr(44, 0, 1, &ready);
    CHECK(outer != nullptr);
    CHECK_EQ(ready, 4u);

    CHECK(cache.PushDataLock());
    uint8_t* inner = cache.GetBlockDataPtr(44, 4, 1, &ready);
    CHECK(inner != nullptr);
    CHECK_EQ(ready, 4u);

    CHECK(cache.GetBlockDataPtr(44, 8, 1, &ready) != nullptr);
    CHECK(cache.GetBlockDataPtr(44, 12, 1, &ready) != nullptr);
    if (outer)
        CHECK_EQ(*outer, 10u);
    if (inner)
        CHECK_EQ(*inner, 20u);

    CHECK(cache.PopDataLock());
    CHECK(cache.GetBlockDataPtr(44, 16, 1, &ready) != nullptr);
    if (outer)
        CHECK_EQ(*outer, 10u);
    CHECK(cache.PopDataLock());
}

LIBMP_TEST_MAIN("data_context")

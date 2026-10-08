// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// kv_store_tests.cpp -- focused tests for the write-only KV store: single vs array values,
// value-line growth and link consistency, uniform-value-size enforcement, the
// SimpleKVLog -> KVStore conversion, and the multi-store reader proxy.

#include "test_harness.h"
#include "diagnostic_assert_test.h"

#include "gprx_container/block.h"
#include "gprx_container/container_context.h"
#include "gprx_container/kv_store.h"

using GPRX::ContainerContext;
using GPRX::KVStore;
using Strategy = GPRX::KVStore::SetStrategy;

namespace
{
// Adds a fresh writable KV-store block to `c` and returns it. KVStore holds a container
// reference plus internal caches and is used via unique_ptr in production, so construct it
// in place (never copy/return it by value).
GPRX::BlockContext& add_kv_block(ContainerContext& c, uint32_t block_id)
{
    c.AddBlock(block_id, true);
    GPRX::BlockContext& b = c.GetBlocks().back();
    b.headerPtr->type_id = GPRX::BlockType::KVStore;
    c.RebuildDirectLookupTable();
    return b;
}
} // namespace

TEST_CASE(kv_single_value)
{
    ContainerContext c;
    KVStore kv(c, &add_kv_block(c, 1));
    kv.Initialize({ 8, 0, 32, 16 });

    uint32_t key[] = { 10, 20 };
    uint32_t val = 12345;
    CHECK(kv.Put(key, 2, nullptr, &val, 1, Strategy::PutOrReplaceSingle));

    uint32_t out[8] = {};
    mpsize_t vsize = 0, vcount = 0;
    CHECK(kv.Get(key, 2, nullptr, out, &vsize, &vcount));
    CHECK_EQ(out[0], 12345u);
    CHECK_EQ(vsize, 1u);
    CHECK_EQ(vcount, 1u);
}

TEST_CASE(kv_replace_single)
{
    ContainerContext c;
    KVStore kv(c, &add_kv_block(c, 1));
    kv.Initialize({ 8, 0, 32, 16 });

    uint32_t key[] = { 7 };
    uint32_t a = 111, b = 222;
    CHECK(kv.Put(key, 1, nullptr, &a, 1, Strategy::PutOrReplaceSingle));
    CHECK(kv.Put(key, 1, nullptr, &b, 1, Strategy::PutOrReplaceSingle));

    uint32_t out[8] = {};
    mpsize_t vcount = 0;
    CHECK(kv.Get(key, 1, nullptr, out, nullptr, &vcount));
    CHECK_EQ(out[0], 222u);
    CHECK_EQ(vcount, 1u);
}

TEST_CASE(kv_missing_key)
{
    ContainerContext c;
    KVStore kv(c, &add_kv_block(c, 1));
    kv.Initialize({ 8, 0, 32, 16 });
    uint32_t key[] = { 99 };
    uint32_t out[8] = {};
    CHECK(!kv.Get(key, 1, nullptr, out, nullptr, nullptr));
    CHECK(!kv.Contains(key, 1, nullptr, nullptr));
}

TEST_CASE(value_buf_bounded_null_terminated_string)
{
    char source[128];
    for (size_t i = 0; i + 1 < sizeof(source); ++i)
        source[i] = static_cast<char>('a' + i % 26);
    source[sizeof(source) - 1] = '\0';

    GPRX::ValueBuf implicit_length;
    GPRX::ValueBuf explicit_length;
    implicit_length.FromString(source);
    explicit_length.FromString(source, sizeof(source) - 1);

    CHECK_EQ(implicit_length.size_in_u32, implicit_length.elements_count);
    CHECK_EQ(implicit_length.size_in_u32, explicit_length.size_in_u32);
    for (mpsize_t i = 0; i < implicit_length.size_in_u32; ++i)
        CHECK_EQ(implicit_length.elements[i], explicit_length.elements[i]);
}

TEST_CASE(kv_put_new_rejects_existing)
{
    ContainerContext c;
    KVStore kv(c, &add_kv_block(c, 1));
    kv.Initialize({ 8, 0, 32, 16 });
    uint32_t key[] = { 5 };
    uint32_t v = 1;
    CHECK(kv.Put(key, 1, nullptr, &v, 1, Strategy::PutNew));
    CHECK(!kv.Put(key, 1, nullptr, &v, 1, Strategy::PutNew)); // already present
}

// Push enough array values to force a second value-line allocation, then verify order
// and count are preserved across the line link.
TEST_CASE(kv_array_growth_and_order)
{
    ContainerContext c;
    KVStore kv(c, &add_kv_block(c, 1));
    kv.Initialize({ 8, 0, 32, 16 }); // small v_entries -> forces a new v-line

    uint32_t key[] = { 42 };
    const uint32_t N = 25;
    for (uint32_t i = 0; i < N; ++i)
    {
        uint32_t v = 1000 + i;
        CHECK(kv.Put(key, 1, nullptr, &v, 1, Strategy::PutToArray));
    }

    mpsize_t vsize = 0, vcount = 0;
    const uint32_t* all = kv.GetAllValues(key, 1, nullptr, &vsize, &vcount);
    CHECK(all != nullptr);
    CHECK_EQ(vcount, N);
    CHECK_EQ(vsize, 1u);
    if (all && vcount == N)
    {
        for (uint32_t i = 0; i < N; ++i)
            CHECK_EQ(all[i], 1000u + i);
    }

    // Index access, including the "last" sentinel.
    uint32_t out[8] = {};
    CHECK(kv.GetValueAtIndex(key, 1, nullptr, 0, out, nullptr, nullptr));
    CHECK_EQ(out[0], 1000u);
    CHECK(kv.GetValueAtIndex(key, 1, nullptr, N - 1, out, nullptr, nullptr));
    CHECK_EQ(out[0], 1000u + (N - 1));
    CHECK(kv.GetValueAtIndex(key, 1, nullptr, GPRX::max_mpsize_t, out, nullptr, nullptr)); // last
    CHECK_EQ(out[0], 1000u + (N - 1));
}

TEST_CASE(kv_uniform_value_size_enforced)
{
    ContainerContext c;
    KVStore kv(c, &add_kv_block(c, 1));
    kv.Initialize({ 8, 0, 32, 16 });
    uint32_t key[] = { 3 };
    uint32_t one = 1;
    uint32_t two[2] = { 1, 2 };
    CHECK(kv.Put(key, 1, nullptr, &one, 1, Strategy::PutToArray));
    // A different value width under the same key must be rejected.
    CHECK(!kv.Put(key, 1, nullptr, two, 2, Strategy::PutToArray));
}

// SimpleKVLog is append/write-only; converting it to a KVStore must recover the pairs
// (repeated keys become arrays under the default PutToArray strategy).
TEST_CASE(kv_simplelog_export)
{
    ContainerContext c;

    c.AddBlock(1, true);
    GPRX::BlockContext& logBlock = c.GetBlocks().back();
    logBlock.headerPtr->type_id = GPRX::BlockType::KVLog;
    c.RebuildDirectLookupTable();
    GPRX::SimpleKVLog log(c, &c.GetBlocks().back());

    uint32_t keyA[] = { 1 };
    uint32_t keyB[] = { 2 };
    uint32_t v;
    v = 10; CHECK(log.PutKeyValue(keyA, 1, &v, 1));
    v = 11; CHECK(log.PutKeyValue(keyA, 1, &v, 1)); // repeat key -> array
    v = 20; CHECK(log.PutKeyValue(keyB, 1, &v, 1));

    KVStore dest(c, &add_kv_block(c, 2));
    dest.Initialize({ 8, 0, 32, 16 });
    CHECK(log.ExportToKVStore(dest));

    mpsize_t vcount = 0;
    const uint32_t* a = dest.GetAllValues(keyA, 1, nullptr, nullptr, &vcount);
    CHECK(a != nullptr);
    CHECK_EQ(vcount, 2u);
    if (a && vcount == 2)
    {
        CHECK_EQ(a[0], 10u);
        CHECK_EQ(a[1], 11u);
    }
    uint32_t out[8] = {};
    CHECK(dest.Get(keyB, 1, nullptr, out, nullptr, &vcount));
    CHECK_EQ(out[0], 20u);
}

TEST_CASE(kv_simplelog_rejects_invalid_and_incomplete_records)
{
    ContainerContext c;
    c.AddBlock(1, true);
    GPRX::BlockContext& log_block = c.GetBlocks().back();
    log_block.headerPtr->type_id = GPRX::BlockType::KVLog;
    GPRX::SimpleKVLog log(c, &log_block);

    uint32_t key = 1;
    uint32_t value = 2;
    CHECK(!log.PutKeyValue(nullptr, 1, &value, 1));
    CHECK(!log.PutKeyValue(&key, 1, nullptr, 1));
    CHECK(!log.PutKeyValue(&key, GPRX::KVStore::KEY_MAX_SIZE_IN_U32 + 1, &value, 1));
    CHECK(!log.PutKeyValue(&key, 1, &value, GPRX::KVStore::VALUE_MAX_SIZE_IN_U32 + 1));

    // A key without a following value is a truncated log record.
    CHECK(c.AppendPayloadData(log_block, reinterpret_cast<const uint8_t*>(&key), sizeof(key)));
    KVStore destination(c, &add_kv_block(c, 2));
    CHECK(destination.Initialize({8, 0, 32, 16}));
    CHECK(!log.ExportToKVStore(destination));
}

// A single Get across a group of stores via the reader proxy.
TEST_CASE(kv_multistore_proxy)
{
    ContainerContext c;
    KVStore kv0(c, &add_kv_block(c, 1));
    kv0.Initialize({ 8, 0, 32, 16 });
    KVStore kv1(c, &add_kv_block(c, 2));
    kv1.Initialize({ 8, 0, 32, 16 });

    uint32_t keyA[] = { 100 };
    uint32_t keyB[] = { 200 };
    uint32_t va = 7, vb = 8;
    CHECK(kv0.Put(keyA, 1, nullptr, &va, 1, Strategy::PutOrReplaceSingle));
    CHECK(kv1.Put(keyB, 1, nullptr, &vb, 1, Strategy::PutOrReplaceSingle));

    GPRX::KVStoreReaderProxy proxy;
    CHECK(proxy.AddSource(&kv0));
    CHECK(proxy.AddSource(&kv1));
    {
        libmp_test::ScopedExpectedDiagnostic expected(
            "KVStoreReaderProxy::AddSource - source must not be null");
        CHECK(!proxy.AddSource(nullptr));
        CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(1));
    }

    uint32_t out[8] = {};
    CHECK(proxy.Get(keyA, 1, nullptr, out, nullptr, nullptr));
    CHECK_EQ(out[0], 7u);
    CHECK(proxy.Get(keyB, 1, nullptr, out, nullptr, nullptr));
    CHECK_EQ(out[0], 8u);
}

TEST_CASE(kv_rejects_invalid_persistent_layout)
{
    ContainerContext c;
    GPRX::BlockContext& block = add_kv_block(c, 1);
    KVStore kv(c, &block);
    CHECK(kv.Initialize({ 8, 0, 32, 16 }));

    const mpsize_t config_offset = GPRX::Block::GetMeta1View(block.headerPtr)->parts.low;
    auto* config = reinterpret_cast<KVStore::Config*>(
        c.GetPayloadDataPtr(block, config_offset, sizeof(KVStore::Config), nullptr));
    CHECK(config != nullptr);
    if (!config)
        return;

    config->l1_bits = 32; // would make mask/shift arithmetic undefined without validation
    kv.ClearCaches();
    uint32_t key[] = { 1 };
    uint32_t out[KVStore::VALUE_MAX_SIZE_IN_U32] = {};
    CHECK(!kv.Get(key, 1, nullptr, out));
}

TEST_CASE(kv_put_reloads_persistent_layout_after_cache_clear)
{
    ContainerContext c;
    GPRX::BlockContext& block = add_kv_block(c, 1);
    KVStore kv(c, &block);
    CHECK(kv.Initialize({ 8, 0, 32, 16 }));

    uint32_t firstKey[] = { 1 };
    uint32_t firstValue = 11;
    CHECK(kv.Put(firstKey, 1, nullptr, &firstValue, 1, Strategy::PutOrReplaceSingle));

    kv.ClearCaches();

    uint32_t secondKey[] = { 2 };
    uint32_t secondValue = 22;
    CHECK(kv.Put(secondKey, 1, nullptr, &secondValue, 1, Strategy::PutOrReplaceSingle));

    uint32_t out[KVStore::VALUE_MAX_SIZE_IN_U32] = {};
    CHECK(kv.Get(secondKey, 1, nullptr, out));
    CHECK_EQ(out[0], secondValue);
}

TEST_CASE(kv_rejects_cyclic_kv_line_chain)
{
    ContainerContext c;
    GPRX::BlockContext& block = add_kv_block(c, 1);
    KVStore kv(c, &block);
    constexpr mpsize_t l1Bits = 8;
    constexpr mpsize_t kvEntries = 32;
    CHECK(kv.Initialize({ l1Bits, 0, kvEntries, 16 }));

    uint32_t key[] = { 17 };
    uint32_t value = 99;
    CHECK(kv.Put(key, 1, nullptr, &value, 1, Strategy::PutOrReplaceSingle));

    const uint32_t bucket = KVStore::CalculateHash(key, 1) & ((1u << l1Bits) - 1);
    auto* l1 = reinterpret_cast<uint32_t*>(
        c.GetPayloadDataPtr(block, 0, (1u << l1Bits) * sizeof(uint32_t), nullptr));
    CHECK(l1 != nullptr);
    if (!l1)
        return;
    const uint32_t line_offset = l1[bucket];
    auto* line = reinterpret_cast<uint32_t*>(
        c.GetPayloadDataPtr(block, line_offset, kvEntries * sizeof(uint32_t), nullptr));
    CHECK(line != nullptr);
    if (!line)
        return;
    line[kvEntries - 1] = line_offset; // malicious self-cycle

    // Pick another key in the same bucket so lookup walks the cyclic chain.
    uint32_t missing = key[0] + 1;
    while ((KVStore::CalculateHash(&missing, 1) & ((1u << l1Bits) - 1)) != bucket)
        ++missing;
    CHECK(!kv.Contains(&missing, 1, nullptr));
}

TEST_CASE(kv_rejects_misaligned_line_offset)
{
    ContainerContext c;
    GPRX::BlockContext& block = add_kv_block(c, 1);
    KVStore kv(c, &block);
    constexpr mpsize_t l1Bits = 8;
    CHECK(kv.Initialize({ l1Bits, 0, 32, 16 }));

    uint32_t key = 17;
    uint32_t value = 99;
    CHECK(kv.Put(&key, 1, nullptr, &value, 1, Strategy::PutOrReplaceSingle));

    const uint32_t bucket = KVStore::CalculateHash(&key, 1) & ((1u << l1Bits) - 1);
    auto* l1 = reinterpret_cast<uint32_t*>(
        c.GetPayloadDataPtr(block, 0, (1u << l1Bits) * sizeof(uint32_t), nullptr));
    CHECK(l1 != nullptr);
    if (!l1)
        return;
    l1[bucket] -= 2; // corrupt the persisted uint32_t-aligned KV-line offset

    libmp_test::ScopedExpectedDiagnostic expected(
        "KVStore::GetPayloadU32Ptr - uint32_t payload data must be aligned");
    CHECK(!kv.Contains(&key, 1, nullptr));
    CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(1));
}

TEST_CASE(kv_rejects_corrupt_value_chain_metadata)
{
    ContainerContext c;
    GPRX::BlockContext& block = add_kv_block(c, 1);
    KVStore kv(c, &block);
    constexpr mpsize_t l1Bits = 8;
    constexpr mpsize_t kvEntries = 32;
    constexpr mpsize_t vEntries = 16;
    CHECK(kv.Initialize({ l1Bits, 0, kvEntries, vEntries }));

    uint32_t key[] = { 23 };
    uint32_t value = 1;
    CHECK(kv.Put(key, 1, nullptr, &value, 1, Strategy::PutToArray));
    value = 2;
    CHECK(kv.Put(key, 1, nullptr, &value, 1, Strategy::PutToArray));

    const uint32_t bucket = KVStore::CalculateHash(key, 1) & ((1u << l1Bits) - 1);
    auto* l1 = reinterpret_cast<uint32_t*>(
        c.GetPayloadDataPtr(block, 0, (1u << l1Bits) * sizeof(uint32_t), nullptr));
    CHECK(l1 != nullptr);
    if (!l1)
        return;
    auto* line = reinterpret_cast<uint32_t*>(
        c.GetPayloadDataPtr(block, l1[bucket], kvEntries * sizeof(uint32_t), nullptr));
    CHECK(line != nullptr);
    if (!line)
        return;

    // One-word key means the following entry is the encoded V-line pointer.
    constexpr uint32_t pointerMask = 0x3FFFFFFFu;
    const uint32_t vline_offset = line[1] & pointerMask;
    auto* vline = reinterpret_cast<uint32_t*>(
        c.GetPayloadDataPtr(block, vline_offset, vEntries * sizeof(uint32_t), nullptr));
    CHECK(vline != nullptr);
    if (!vline)
        return;

    vline[vEntries - 3] = GPRX::max_mpsize_t; // impossible values count
    mpsize_t count = 123;
    CHECK(kv.GetAllValues(key, 1, nullptr, nullptr, &count) == nullptr);
    CHECK_EQ(count, 0u);

    vline[vEntries - 3] = 2;
    vline[vEntries - 1] = vline_offset; // malicious self-cycle
    CHECK(kv.GetAllValues(key, 1, nullptr, nullptr, &count) == nullptr);
    CHECK_EQ(count, 0u);
}

LIBMP_TEST_MAIN("kv_store")

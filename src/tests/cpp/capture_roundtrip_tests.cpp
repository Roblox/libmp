// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// capture_roundtrip_tests.cpp -- end-to-end CaptureBuilder round-trip against REAL data.
//
// Proves the whole create->consume pipeline without committing a binary dump:
//   1. roundtrip_capture_data.inc holds real profiler data extracted from a .gprx dump
//      (by src/tests/tools/extract_capture.cpp).
//   2. BuildCaptureBuffer() rebuilds an equivalent in-memory GPRX capture with
//      LibMP::LiveContext, following the live-capture update sequence.
//   3. Each TEST_CASE serializes that capture to memory, reopens it with the public
//      reader (LibMP::Session), and asserts the consumed data matches the source.
//
// Frame timestamps and thread-log words are copied from the source dump exactly. Raw
// records are then checked bit-for-bit together with their iterator-visible metadata.

#include "test_harness.h"
#include "diagnostic_assert_test.h"
#include "mp_api/cpp_api.h"
#include "roundtrip_capture_builder.h"
#include "mp_api/integration/data_range.h"
#include "mp_api/log_event.h"
#include "mp_api/tools.h"
#include "mp_api/version.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
constexpr const char* kKvSubtypeDiagnostic =
    "MpFindKVStore - KVStore block subtype must be within the "
    "BlockTypeMeta::Kv_SubTypes_Count range";

LibMP::DefaultFlavor::optional<LibMP::DefaultFlavor::string>
fetch_label_from_offset(MpSession* session, uint64_t offset)
{
    uint32_t byteLength = 0;
    const uint8_t* value = MpGetLabelFromOffset(session, &offset, &byteLength);
    if (!value)
        return {};
    return LibMP::DefaultFlavor::string(
        reinterpret_cast<const char*>(value), byteLength);
}

struct ExternalCapture
{
    const ntd::vector<uint8_t>* bytes = nullptr;
    uint32_t opens = 0;
    uint32_t closes = 0;
    uint32_t reads = 0;
};

struct LiveExternalCapture
{
    GPRX::DataContextMemChunks* data = nullptr;
};

class RejectingToolOutput final : public GPRX::IStreamProcessor
{
public:
    bool ProcessChunk(const uint8_t* /*data*/, mpsize_t size) override
    {
        if (++calls == 2)
            return false;
        m_bytes_written += size;
        return true;
    }

    uint32_t calls = 0;
};

uint32_t external_open(uint8_t* handle)
{
    ++reinterpret_cast<ExternalCapture*>(handle)->opens;
    return 1;
}

uint32_t external_close(uint8_t* handle)
{
    ++reinterpret_cast<ExternalCapture*>(handle)->closes;
    return 1;
}

mpdatasz_t external_size(uint8_t* handle)
{
    return reinterpret_cast<ExternalCapture*>(handle)->bytes->size();
}

mpsize_t external_read(
    uint8_t* handle, uint8_t* dst, uint32_t block_id, mpdatasz_t offset, mpsize_t size)
{
    ExternalCapture& capture = *reinterpret_cast<ExternalCapture*>(handle);
    ++capture.reads;
    if (block_id != 0 || !dst ||
        !GPRX::range_within<mpdatasz_t>(offset, size, capture.bytes->size()))
        return 0;
    std::memcpy(dst, capture.bytes->data() + offset, size);
    return size;
}

GPRX::ExternalIO make_external_io()
{
    GPRX::ExternalIO io;
    io.open = external_open;
    io.close = external_close;
    io.getSize = external_size;
    io.read = external_read;
    io.isBlockFetchSupported = false;
    return io;
}

mpdatasz_t live_external_size(uint8_t* handle)
{
    return reinterpret_cast<LiveExternalCapture*>(handle)->data->GetTotalDataSize();
}

mpsize_t live_external_read(
    uint8_t* handle, uint8_t* dst, uint32_t block_id, mpdatasz_t offset, mpsize_t size)
{
    GPRX::DataContextMemChunks& data =
        *reinterpret_cast<LiveExternalCapture*>(handle)->data;
    return block_id
        ? data.ReadBlockDataToBuffer(block_id, static_cast<mpsize_t>(offset), size, dst, size)
        : data.ReadAbsDataToBuffer(offset, size, dst, size);
}

GPRX::ExternalIO make_live_external_io()
{
    GPRX::ExternalIO io;
    io.open = [](uint8_t*) { return uint32_t{1}; };
    io.close = [](uint8_t*) { return uint32_t{1}; };
    io.getSize = live_external_size;
    io.read = live_external_read;
    io.isBlockFetchSupported = true;
    return io;
}

struct CacheBucketRecorder
{
    std::vector<uint32_t> buckets;
    uint32_t calls = 0;
};

void record_cleared_cache_buckets(
    uint8_t* handle, const uint32_t* bucket_ids, mpsize_t bucket_count)
{
    CacheBucketRecorder& recorder = *reinterpret_cast<CacheBucketRecorder*>(handle);
    ++recorder.calls;
    recorder.buckets.clear();
    if (bucket_count > 0)
        recorder.buckets.assign(bucket_ids, bucket_ids + bucket_count);
}

struct ExpectedLogRecord
{
    uint32_t frame_index;
    const rd::ThreadLogRow* thread_log;
    uint32_t word_offset;
    uint32_t word_count;
};

// Partition the flat fixture words into the logical records returned by one iterator step.
std::vector<ExpectedLogRecord> index_expected_log_records()
{
    std::vector<ExpectedLogRecord> result;
    size_t nextThreadLogOffset = 0;
    for (uint32_t frameIndex = 0; frameIndex < countof(rd::kFrames); ++frameIndex)
    {
        const auto& frame = rd::kFrames[frameIndex];
        CHECK_EQ(frame.frame_index, frameIndex);
        CHECK_EQ(frame.thread_log_offset, nextThreadLogOffset);
        const bool rangeValid = frame.thread_log_offset <= countof(rd::kThreadLogs)
            && frame.thread_log_count <= countof(rd::kThreadLogs) - frame.thread_log_offset;
        CHECK(rangeValid);
        if (!rangeValid)
            return result;
        nextThreadLogOffset = frame.thread_log_offset + frame.thread_log_count;
        for (uint32_t logIndex = 0; logIndex < frame.thread_log_count; ++logIndex)
        {
            const auto& threadLog = rd::kThreadLogs[frame.thread_log_offset + logIndex];
            if (threadLog.is_removed)
                continue;
            uint32_t offset = 0;
            while (offset < threadLog.entry_count)
            {
                const uint64_t entry = rd::kThreadLogEntries[threadLog.entry_offset + offset];
                uint32_t count = 1;
                if (LibMP::MicroProfilerLogType(entry) == MP_LOG_EVENT)
                {
                    bool hasComment = false;
                    bool isExtraEntry = false;
                    uint32_t eventSize = 0;
                    ntd::tie(hasComment, eventSize, isExtraEntry) = LibMP::MicroProfilerEventEntryInfo(entry);
                    const bool validMainEntry = !isExtraEntry && eventSize <= MP_EVENT_SIZE_L;
                    CHECK(validMainEntry);
                    if (validMainEntry)
                        count = LibMP::MicroProfilerEventEntryLastOffset(eventSize, hasComment) + 1;
                }
                CHECK(count <= threadLog.entry_count - offset);
                if (count > threadLog.entry_count - offset)
                    count = threadLog.entry_count - offset;
                result.push_back({frameIndex, &threadLog, threadLog.entry_offset + offset, count});
                offset += count;
            }
        }
    }
    CHECK_EQ(nextThreadLogOffset, countof(rd::kThreadLogs));
    return result;
}

void check_counter_matches(
    LibMP::Session& session,
    const char* mask,
    bool caseSensitive,
    std::initializer_list<uint32_t> expected)
{
    const auto matches = session.FindCounterIds(mask, caseSensitive);
    CHECK_EQ(matches.size(), expected.size());

    size_t index = 0;
    for (uint32_t id : expected)
    {
        if (index < matches.size())
            CHECK_EQ(matches[index], id);
        ++index;
    }

    CHECK_EQ(session.FindCounterId(mask, caseSensitive),
        expected.size() > 0 ? *expected.begin() : 0u);
}

void check_counter_iterator_rejects_root(
    LibMP::Session& session,
    uint32_t root_id,
    const char* expected_diagnostic,
    mpsize_t expected_successful_steps)
{
    LibMP::TypesNtd::CounterIteratorConfigT config;
    config.root_counter_ids.push_back(root_id);
    LibMP::FbBuilder builder;
    builder.Finish(LibMP::TypesNtd::CounterIteratorConfig::Pack(builder, &config));

    MpCounterIterator* iterator = MpCreateCounterIterator(session.GetHandle());
    CHECK(iterator != nullptr);
    if (!iterator)
        return;

    uint8_t* config_buffer = MpGetCounterIteratorConfigBuffer(
        iterator, GPRX::to_mpsize(builder.GetSize()));
    CHECK(config_buffer != nullptr);
    if (!config_buffer)
    {
        MpDeleteCounterIterator(iterator);
        return;
    }
    std::memcpy(config_buffer, builder.GetBufferPointer(), builder.GetSize());
    CHECK(MpConfigureCounterIterator(iterator, false));

    mpsize_t successful_steps = 0;
    libmp_test::ScopedExpectedDiagnostic expected(expected_diagnostic);
    while (MpStepCounterIterator(iterator))
    {
        ++successful_steps;
        if (successful_steps > expected_successful_steps)
            break;
    }
    CHECK_EQ(successful_steps, expected_successful_steps);
    CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(1));
    MpDeleteCounterIterator(iterator);
}
} // namespace


TEST_CASE(capture_roundtrip_descriptors)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    auto rs = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(rs);
    if (!rs)
        return;
    CHECK(rs->IsValid());

    // Threads: ids, names, gpu flag, and the dedicated gpu-thread id.
    CHECK_EQ(rs->GetThreadIds().Size(), (long long)countof(rd::kThreads));
    for (size_t i = 0; i < countof(rd::kThreads); ++i)
    {
        const auto& row = rd::kThreads[i];
        const auto* d = rs->GetThreadDesc(row.id);
        CHECK(d != nullptr);
        if (!d)
            continue;
        CHECK(d->thread_name() && std::strcmp(d->thread_name()->c_str(), row.name) == 0);
        CHECK_EQ(d->buffer_size(), row.buffer_size);
        CHECK_EQ(d->is_gpu(), row.is_gpu);
    }
    CHECK_EQ(rs->GetThreadIdGpu(), rd::kThreadIdGpu);

    const auto* generalInfo = rs->GetGeneralInfo();
    CHECK_EQ(generalInfo != nullptr, rd::kHasGeneralInfo);
    if (generalInfo)
    {
        CHECK_EQ(generalInfo->platform_info_json() != nullptr, rd::kHasPlatformInfoJson);
        if (generalInfo->platform_info_json())
            CHECK(std::strcmp(generalInfo->platform_info_json()->c_str(), rd::kPlatformInfoJson) == 0);
    }

    // Groups.
    CHECK_EQ(rs->GetGroupIdMax(), rd::kGroupIdMax);
    for (size_t i = 0; i < countof(rd::kGroups); ++i)
    {
        const auto& row = rd::kGroups[i];
        const auto* d = rs->GetGroupDesc(row.id);
        CHECK(d != nullptr);
        if (!d)
            continue;
        CHECK(d->group_name() && std::strcmp(d->group_name()->c_str(), row.name) == 0);
        CHECK_EQ(d->color(), row.color);
        CHECK_EQ(d->is_gpu(), row.is_gpu);
    }

    // Timers.
    CHECK_EQ(rs->GetTimerIds().Size(), (long long)countof(rd::kTimers));
    for (size_t i = 0; i < countof(rd::kTimers); ++i)
    {
        const auto& row = rd::kTimers[i];
        const auto* d = rs->GetTimerDesc(row.id);
        CHECK(d != nullptr);
        if (!d)
            continue;
        CHECK(d->timer_name() && std::strcmp(d->timer_name()->c_str(), row.name) == 0);
        CHECK_EQ(d->group_id(), row.group_id);
        CHECK_EQ(d->color(), row.color);
        CHECK_EQ(d->is_user_timer(), row.is_user_timer);
    }

    // Counters.
    CHECK_EQ(rs->GetCounterIds().Size(), (long long)countof(rd::kCounters));
    for (size_t i = 0; i < countof(rd::kCounters); ++i)
    {
        const auto& row = rd::kCounters[i];
        const auto* d = rs->GetCounterDesc(row.id);
        CHECK(d != nullptr);
        if (!d)
            continue;
        CHECK(d->counter_name() && std::strcmp(d->counter_name()->c_str(), row.name) == 0);
        CHECK_EQ(d->parent(), row.parent);
        CHECK_EQ(d->sibling(), row.sibling);
        CHECK_EQ(d->first_child(), row.first_child);
        CHECK_EQ(d->level(), row.level);
    }
}

TEST_CASE(capture_builder_normalizes_frame_window)
{
    LibMP::LiveContext minimum;
    minimum.config.max_frames_per_kv = 0;
    minimum.Init();
    CHECK_EQ(minimum.config.max_frames_per_kv, 1u);

    LibMP::LiveContext maximum;
    maximum.config.max_frames_per_kv = LibMP::LiveContext::MAX_FRAMES_PER_KV_LIMIT + 1;
    maximum.Init();
    CHECK_EQ(maximum.config.max_frames_per_kv, LibMP::LiveContext::MAX_FRAMES_PER_KV_LIMIT);
}

TEST_CASE(capture_builder_accepts_first_block_id)
{
    LibMP::LiveContext explicitFirstBlock;
    explicitFirstBlock.Init(17);

    CHECK_EQ(explicitFirstBlock.magic_block.GetBlockId(), 17u);
    CHECK_EQ(explicitFirstBlock.max_block_id, 32u);

    LibMP::LiveContext seededMaximum;
    seededMaximum.max_block_id = 16;
    seededMaximum.Init();

    CHECK_EQ(seededMaximum.magic_block.GetBlockId(), 17u);
    CHECK_EQ(seededMaximum.max_block_id, 32u);
}

TEST_CASE(capture_builder_replaces_provisional_counter_snapshot)
{
    LibMP::LiveContext live;
    live.Init();

    const auto putCounterSample = [&](double value, bool replace)
    {
        const LibMP::TypesNtd::CounterSample sample(live.frames_counter, value);
        live.value_buf.FromCounterSample(sample);
        live.SelectKV(live.kvcounters1_block, live.kvcounters1_block);
        live.SetUid(LibMP::QueryEx::CounterSamples_u24CounterId, true, 1);
        if (replace)
            live.PutSingleValueBuf();
        else
            live.PutArrayValueBuf();
    };

    putCounterSample(10, true);
    live.bCounterSamplesProvisional = true;
    live.bInitialFillingCompleted = true;
    live.SetDataChanged();
    live.CheckExport();

    LiveExternalCapture source{live.live_datacontext.get()};
    GPRX::ExternalIO io = make_live_external_io();
    auto session = LibMP::Session::OpenFromExternalProviderTrusted(
        &io, reinterpret_cast<uint8_t*>(&source));
    CHECK(session && session->IsValid());
    if (!session || !session->IsValid())
        return;

    {
        LibMP::CounterSampleArray samples = session->GetCounterSamples(1);
        CHECK_EQ(samples.Size(), 1u);
        const LibMP::TypesNtd::CounterSample* sample = samples.GetSampleByFrameId(0);
        CHECK(sample);
        if (sample)
            CHECK_EQ(sample->value(), 10);
    }

    const uint32_t initialCounterBlockId = live.kvcounters1_block.GetBlockId();
    CHECK(!live.StartReplacingCounterSnapshot(false));
    CHECK(live.kvcounters1_block.GetBlockId() != initialCounterBlockId);
    putCounterSample(20, true);
    live.SetDataChanged();
    live.CheckExport();
    session->SetSyncRequired();
    const bool replacementSynced = session->SyncIfRequired();
    CHECK(replacementSynced);
    if (!replacementSynced)
    {
        return;
    }

    {
        LibMP::CounterSampleArray samples = session->GetCounterSamples(1);
        CHECK_EQ(samples.Size(), 1u);
        const LibMP::TypesNtd::CounterSample* sample = samples.GetSampleByFrameId(0);
        CHECK(sample);
        if (sample)
            CHECK_EQ(sample->value(), 20);
    }

    CHECK(!live.StartAddingCounterSample(false));
    putCounterSample(30, true);
    live.FinishAddingCounterSample();
    live.bCounterSamplesProvisional = false;
    live.SetDataChanged();
    live.CheckExport();
    session->SetSyncRequired();
    const bool firstHistoricalSampleSynced = session->SyncIfRequired();
    CHECK(firstHistoricalSampleSynced);
    if (!firstHistoricalSampleSynced)
    {
        return;
    }

    {
        LibMP::CounterSampleArray samples = session->GetCounterSamples(1);
        CHECK_EQ(samples.Size(), 1u);
        CHECK(samples.GetSampleByFrameId(0) == nullptr);
        const LibMP::TypesNtd::CounterSample* sample = samples.GetSampleByFrameId(1);
        CHECK(sample);
        if (sample)
            CHECK_EQ(sample->value(), 30);
    }

    CHECK(!live.StartAddingCounterSample(false));
    putCounterSample(40, false);
    live.FinishAddingCounterSample();
    live.SetDataChanged();
    live.CheckExport();
    session->SetSyncRequired();
    const bool secondHistoricalSampleSynced = session->SyncIfRequired();
    CHECK(secondHistoricalSampleSynced);
    if (!secondHistoricalSampleSynced)
    {
        return;
    }

    LibMP::CounterSampleArray samples = session->GetCounterSamples(1);
    CHECK_EQ(samples.Size(), 2u);
    const LibMP::TypesNtd::CounterSample* first = samples.GetSampleByFrameId(1);
    CHECK(first);
    if (first)
        CHECK_EQ(first->value(), 30);
    const LibMP::TypesNtd::CounterSample* second = samples.GetSampleByFrameId(2);
    CHECK(second);
    if (second)
        CHECK_EQ(second->value(), 40);
}

TEST_CASE(capture_builder_exposes_guarded_live_label_ring)
{
    static constexpr mpsize_t ringSize = 128;
    uint8_t labels[ringSize] = {};

    // The logical interval [41, 161) wraps at 128. A label that would cross the
    // physical boundary consumes the remaining tail as padding and starts at 128.
    std::memcpy(labels + 40, "x", 2);
    std::memcpy(labels + 42, "old", 4);
    std::memcpy(labels + 120, "tail", 5);
    std::memcpy(labels, "wrapped", 8);

    LibMP::LiveContext live;
    live.Init();
    const uint32_t initialBlockId = live.labelbuffer_block.GetBlockId();
    CHECK(live.UpdateLabelBuffer(labels, ringSize, 161));
    CHECK(live.labelbuffer_block.GetBlockId() != initialBlockId);
    CHECK_EQ(live.labelbuffer_block.ptr->payloadChunks.size(), 3u);
    CHECK_EQ(live.labelbuffer_block.ptr->headerPtr->payload_uncompressed_size, 121u);

    // Repeating the same update is harmless and does not rotate the block ID or
    // append duplicate KV entries.
    const uint32_t firstSnapshotBlockId = live.labelbuffer_block.GetBlockId();
    CHECK(live.UpdateLabelBuffer(labels, ringSize, 161));
    CHECK_EQ(live.labelbuffer_block.GetBlockId(), firstSnapshotBlockId);

    live.SetDataChanged();
    live.CheckExport();
    LiveExternalCapture source{live.live_datacontext.get()};
    GPRX::ExternalIO io = make_live_external_io();
    auto session = LibMP::Session::OpenFromExternalProviderTrusted(
        &io, reinterpret_cast<uint8_t*>(&source));
    CHECK(session && session->IsValid());
    if (!session)
        return;

    // The oldest write-guard bytes are omitted. Both ordinary sides of the
    // physical wrap remain readable without copying.
    CHECK(!fetch_label_from_offset(session->GetHandle(), 40).has_value());
    auto oldLabel = fetch_label_from_offset(session->GetHandle(), 42);
    auto tailLabel = fetch_label_from_offset(session->GetHandle(), 120);
    auto wrappedLabel = fetch_label_from_offset(session->GetHandle(), 128);
    CHECK(oldLabel && *oldLabel == "old");
    CHECK(tailLabel && *tailLabel == "tail");
    CHECK(wrappedLabel && *wrappedLabel == "wrapped");

    // Simulate the next live refresh. New writes consume the guarded physical
    // bytes while the newly exposed range drops their old virtual offsets.
    live.ClearAndIncBlockId(live.shortterm_block);
    live.IncBlockId(live.kvshortterm_block);
    live.ResetKvStore(live.kvshortterm_block);
    std::memcpy(labels + 33, "latest", 7);
    auto oldLabelBeforeRefresh = fetch_label_from_offset(session->GetHandle(), 42);
    CHECK(oldLabelBeforeRefresh && *oldLabelBeforeRefresh == "old");

    const uint32_t previousBlockId = live.labelbuffer_block.GetBlockId();
    CHECK(live.UpdateLabelBuffer(labels, ringSize, 168));
    CHECK(live.labelbuffer_block.GetBlockId() != previousBlockId);
    live.SetDataChanged();
    live.CheckExport();

    session->SetSyncRequired();
    CHECK(session->SyncIfRequired());
    CHECK(!fetch_label_from_offset(session->GetHandle(), 42).has_value());
    auto latestLabel = fetch_label_from_offset(session->GetHandle(), 161);
    CHECK(latestLabel && *latestLabel == "latest");
}

TEST_CASE(capture_context_exposes_the_complete_label_ring)
{
    static constexpr mpsize_t ringSize = 128;
    uint8_t labels[ringSize] = {};
    std::memcpy(labels + 42, "old", 4);
    std::memcpy(labels + 120, "tail", 5);
    std::memcpy(labels, "wrapped", 8);

    LibMP::CaptureContext capture;
    capture.Init();
    CHECK(capture.UpdateLabelBuffer(labels, ringSize, 161));
    capture.SetDataChanged();
    capture.CheckExport();

    LiveExternalCapture source{capture.live_datacontext.get()};
    GPRX::ExternalIO io = make_live_external_io();
    auto session = LibMP::Session::OpenFromExternalProviderTrusted(
        &io, reinterpret_cast<uint8_t*>(&source));
    CHECK(session && session->IsValid());
    if (!session)
        return;

    auto oldLabel = fetch_label_from_offset(session->GetHandle(), 42);
    auto tailLabel = fetch_label_from_offset(session->GetHandle(), 120);
    auto wrappedLabel = fetch_label_from_offset(session->GetHandle(), 128);
    CHECK(oldLabel && *oldLabel == "old");
    CHECK(tailLabel && *tailLabel == "tail");
    CHECK(wrappedLabel && *wrappedLabel == "wrapped");
}

TEST_CASE(capture_counter_path_matching)
{
    static const rd::CounterRow counters[] = {
        {1u, "RootA", 0u, 0u, 2u, 0u},
        {2u, "Branch", 1u, 7u, 3u, 1u},
        {3u, "Leaf", 2u, 0u, 0u, 2u},
        {4u, "RootB", 0u, 0u, 5u, 0u},
        {5u, "Branch", 4u, 0u, 6u, 1u},
        {6u, "Leaf", 5u, 0u, 0u, 2u},
        {7u, "Direct", 1u, 0u, 0u, 1u},
    };

    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, false, counters, countof(counters));
    auto session = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(session && session->IsValid());
    if (!session)
        return;

    check_counter_matches(*session, "Branch", true, {2u, 5u});
    check_counter_matches(*session, "/Branch", true, {});
    check_counter_matches(*session, "/RootA/Branch/Leaf", true, {3u});
    check_counter_matches(*session, "RootA/Leaf", true, {});
    check_counter_matches(*session, "RootA/*/Leaf", true, {3u});
    check_counter_matches(*session, "**/Branch/Leaf", true, {3u, 6u});
    check_counter_matches(*session, "RootA/**/Leaf", true, {3u});
    check_counter_matches(*session, "**/**/Leaf", true, {3u, 6u});
    check_counter_matches(*session, "**/Leaf$", true, {3u, 6u});
    check_counter_matches(*session, "**/Branch$", true, {});
    check_counter_matches(*session, "/roota/branch/leaf", false, {3u});
    check_counter_matches(*session, "/roota/branch/leaf", true, {});
    check_counter_matches(*session, "**", true, {1u, 2u, 3u, 4u, 5u, 6u, 7u});
    check_counter_matches(*session, "*$", true, {3u, 6u, 7u});
    check_counter_matches(*session, "RootA/Branch/Leaf|RootB/Branch/Leaf", true, {3u, 6u});
}

TEST_CASE(capture_counter_path_matching_handles_invalid_hierarchies)
{
    static const rd::CounterRow counters[] = {
        {1u, "root", 0u, 0u, 2u, 0u},
        {2u, "valid", 1u, 3u, 0u, 1u},
        {3u, "orphan", 99u, 4u, 0u, 1u},
        {4u, "cycle", 4u, 5u, 0u, 1u},
        {5u, nullptr, 1u, 0u, 6u, 1u},
        {6u, "child", 5u, 0u, 0u, 2u},
        {7u, "child-cycle", 0u, 0u, 7u, 0u},
    };

    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, false, counters, countof(counters));
    auto session = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(session && session->IsValid());
    if (!session)
        return;

    check_counter_matches(*session, "root/valid", false, {2u});
    {
        libmp_test::ScopedExpectedDiagnostic expected(
            "MpFindCountersImpl - counter parent ID has no descriptor");
        check_counter_matches(*session, "root/orphan", false, {});
        check_counter_matches(*session, "**/root/orphan", false, {});
        CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(4));
    }
    check_counter_matches(*session, "root/*/child", false, {});
    check_counter_matches(*session, "**/root/cycle", false, {});
    {
        libmp_test::ScopedExpectedDiagnostic expected(
            "MpFindCountersImpl - counter hierarchy exceeds counter count");
        check_counter_matches(*session, "root/**/cycle", false, {});
        CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(2));
    }

    check_counter_iterator_rejects_root(
        *session, 3,
        "MpCounterIterator::RebuildStackToParent - counter parent ID has no descriptor", 0);
    check_counter_iterator_rejects_root(
        *session, 4,
        "MpCounterIterator::RebuildStackToParent - counter hierarchy exceeds safe depth", 0);
    check_counter_iterator_rejects_root(
        *session, 7,
        "MpCounterIterator::PushToStack - counter hierarchy exceeds safe depth",
        GPRX::to_mpsize(countof(counters)));
}

TEST_CASE(capture_roundtrip_frames)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    auto rs = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(rs && rs->IsValid());
    if (!rs)
        return;

    CHECK_EQ(rs->GetFrameIdMin(), 1u);
    CHECK_EQ(rs->GetFrameIdMax(), rd::kNumFramesExtracted);

    for (uint32_t i = 0; i < rd::kNumFramesExtracted; ++i)
    {
        const auto& fr = rd::kFrames[i];
        const auto* d = rs->GetFrameDesc(i + 1); // rebuilt frame_id == frame_index + 1
        CHECK(d != nullptr);
        if (!d)
            continue;
        CHECK_EQ(d->frame_absolute_id(), fr.absolute_id);
        CHECK_EQ(d->tick_start_cpu(), fr.tsc_start);
        CHECK_EQ(d->tick_end_cpu(), fr.tsc_end);
        CHECK_EQ(d->tick_start_gpu(), fr.tsg_start);
        CHECK_EQ(d->tick_end_gpu(), fr.tsg_end);
        CHECK_EQ(d->label_offset_min(), fr.label_offset_min);
        CHECK_EQ(d->label_offset_max(), fr.label_offset_max);
        CHECK_EQ(d->is_paused(), fr.paused);
        CHECK_EQ(d->is_incomplete(), fr.incomplete);

        const bool rangeValid = fr.thread_log_offset <= countof(rd::kThreadLogs)
            && fr.thread_log_count <= countof(rd::kThreadLogs) - fr.thread_log_offset;
        CHECK(rangeValid);
        if (!rangeValid)
            continue;
        CHECK(d->thread_logs() != nullptr);
        if (!d->thread_logs())
            continue;
        CHECK_EQ(d->thread_logs()->size(), fr.thread_log_count);
        const uint32_t comparableCount = ntd::min<uint32_t>(d->thread_logs()->size(), fr.thread_log_count);
        for (uint32_t logIndex = 0; logIndex < comparableCount; ++logIndex)
        {
            const auto* actual = d->thread_logs()->Get(logIndex);
            const auto& expected = rd::kThreadLogs[fr.thread_log_offset + logIndex];
            CHECK_EQ(actual->thread_id(), expected.thread_id);
            CHECK_EQ(actual->log_entries_num(), expected.entry_count);
            CHECK_EQ(actual->is_removed(), expected.is_removed);
        }
    }

    const auto* gd = rs->GetGlobalDesc();
    CHECK(gd != nullptr);
    if (gd)
    {
        CHECK(gd->tick_to_ms_cpu() == rd::kGlobalTickToMsCpu);
        CHECK(gd->tick_to_ms_gpu() == rd::kGlobalTickToMsGpu);
    }
}

TEST_CASE(capture_roundtrip_counter_samples)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    auto rs = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(rs && rs->IsValid());
    if (!rs)
        return;

    for (size_t k = 0; k < countof(rd::kCounterSamples); ++k)
    {
        const auto& row = rd::kCounterSamples[k];
        auto samples = rs->GetCounterSamples(row.counter_id);
        const auto* smp = samples.GetSampleByFrameId(row.frame_index + 1);
        CHECK(smp != nullptr);
        if (!smp)
            continue;
        CHECK_EQ(smp->frame_id(), row.frame_index + 1);
        CHECK(smp->value() == row.value); // exact: value stored as a hex-float literal
    }
}

TEST_CASE(capture_roundtrip_u64_samples)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    auto rs = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(rs && rs->IsValid());
    if (!rs)
        return;

    auto placeIds = rs->GetPlaceIdSamples();
    CHECK_EQ(placeIds.Size(), countof(rd::kPlaceIdSamples));
    for (mpsize_t i = 0; i < placeIds.Size(); ++i)
    {
        const auto* sample = placeIds.Get(i);
        CHECK(sample != nullptr);
        if (sample)
        {
            CHECK_EQ(sample->frame_id(), rd::kPlaceIdSamples[i].frame_index + 1);
            CHECK_EQ(sample->value(), rd::kPlaceIdSamples[i].value);
        }
    }

    auto timestamps = rs->GetUtcTimestampSamples();
    CHECK_EQ(timestamps.Size(), countof(rd::kUtcTimestampSamples));
    for (mpsize_t i = 0; i < timestamps.Size(); ++i)
    {
        const auto* sample = timestamps.Get(i);
        CHECK(sample != nullptr);
        if (sample)
        {
            CHECK_EQ(sample->frame_id(), rd::kUtcTimestampSamples[i].frame_index + 1);
            CHECK_EQ(sample->value(), rd::kUtcTimestampSamples[i].value);
        }
    }
}

TEST_CASE(capture_roundtrip_thread_logs)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    auto rs = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(rs && rs->IsValid());
    if (!rs)
        return;

    const uint32_t fmin = rs->GetFrameIdMin();
    const uint32_t fmax = rs->GetFrameIdMax();

    auto it = rs->CreateLogIterator();
    it.RewindTo(fmin, fmax);
    const auto* st = it.GetState();

    const std::vector<ExpectedLogRecord> expectedRecords = index_expected_log_records();
    size_t recordIndex = 0;
    size_t mismatches = 0;
    size_t eventsBySize[MP_EVENT_SIZE_L + 1] = {};
    while (it.Step())
    {
        const uint64_t* actualEntries = it.GetCurrentEntry();
        if (!actualEntries)
        {
            CHECK_EQ(it.GetCurrentEntryCount(), 0u);
            continue; // synthetic frame boundaries and thread-frame-end reports
        }

        const bool inRange = recordIndex < expectedRecords.size();
        bool match = inRange;
        if (inRange)
        {
            const auto& record = expectedRecords[recordIndex];
            const auto& log = *record.thread_log;
            const uint64_t* expectedEntries = rd::kThreadLogEntries + record.word_offset;
            CHECK_EQ(it.GetCurrentEntryCount(), record.word_count);
            CHECK(it.GetCurrentEntryAt(record.word_count) == nullptr);
            CHECK_EQ(it.GetCurrentEntryPart(0, 2), 0u);
            for (uint32_t i = 0; i < record.word_count; ++i)
            {
                CHECK(it.GetCurrentEntryAt(i) == actualEntries + i);
                CHECK_EQ(it.GetCurrentEntryPart(i, 0),
                    static_cast<uint32_t>(expectedEntries[i]));
                CHECK_EQ(it.GetCurrentEntryPart(i, 1),
                    static_cast<uint32_t>(expectedEntries[i] >> 32));
            }
            const uint32_t frameIndex = st->frame_id() - fmin;
            match = frameIndex == record.frame_index && st->thread_id() == log.thread_id;
            for (uint32_t i = 0; match && i < record.word_count; ++i)
                match = actualEntries[i] == expectedEntries[i];

            const uint32_t entryType = LibMP::MicroProfilerLogType(expectedEntries[0]);
            if (entryType == MP_LOG_ENTER || entryType == MP_LOG_LEAVE)
            {
                match = match
                    && st->is_enter() == (entryType == MP_LOG_ENTER)
                    && st->is_exit() == (entryType == MP_LOG_LEAVE)
                    && st->timer_id() == LibMP::MicroProfilerLogTimerId(expectedEntries[0])
                    && (st->timestamp() & MP_LOG_TICK_MASK) == LibMP::MicroProfilerLogTick(expectedEntries[0]);
            }
            else if (entryType == MP_LOG_EVENT)
            {
                bool expectedHasComment = false;
                bool isExtraEntry = false;
                uint32_t expectedSize = 0;
                ntd::tie(expectedHasComment, expectedSize, isExtraEntry) =
                    LibMP::MicroProfilerEventEntryInfo(expectedEntries[0]);
                const bool validMainEntry = !isExtraEntry && expectedSize <= MP_EVENT_SIZE_L;
                match = match && validMainEntry;
                if (validMainEntry)
                {
                    ++eventsBySize[expectedSize];
                    const LibMP::EventInfo event = it.GetCurrentEventInfo();
                    match = match && st->is_event()
                        && event.firstEntry == actualEntries
                        && event.eventType == static_cast<uint32_t>(LibMP::MicroProfilerEventEntryType(expectedEntries[0]))
                        && event.eventSize == expectedSize
                        && event.hasComment == expectedHasComment;

                }
            }
            else
                match = match && !st->is_event();
            if (!match && mismatches < 8)
            {
                std::printf("  log mismatch @%zu: got f=%u t=%u first=0x%llx | "
                            "exp f=%u t=%u first=0x%llx words=%u\n",
                    recordIndex, frameIndex, st->thread_id(), (unsigned long long)actualEntries[0],
                    record.frame_index, log.thread_id, (unsigned long long)expectedEntries[0], record.word_count);
            }
        }
        if (!match)
            ++mismatches;
        CHECK(match);
        ++recordIndex;
    }

    // Every raw logical record was reproduced, in order, with nothing extra. Multi-entry
    // Events compare all dependent words and their iterator-visible boundaries and metadata.
    CHECK_EQ(recordIndex, (long long)expectedRecords.size());
    CHECK_EQ(mismatches, 0);
    CHECK(eventsBySize[MP_EVENT_SIZE_S] > 0);
    CHECK(eventsBySize[MP_EVENT_SIZE_M] > 0);
}

// Variation: a small KV window (8) over the same 16 frames forces block rotation, evicting
// the oldest frames. Descriptors and the (narrowed) frame range must still read back.
TEST_CASE(capture_roundtrip_block_rotation)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(8, false);
    auto rs = LibMP::Session::OpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(rs && rs->IsValid());
    if (!rs)
        return;

    const uint32_t fmin = rs->GetFrameIdMin();
    const uint32_t fmax = rs->GetFrameIdMax();
    CHECK_EQ(fmax, rd::kNumFramesExtracted); // newest frame retained
    CHECK(fmin > 1u);                        // oldest frames rotated out
    CHECK_EQ(fmax - fmin + 1u, 8u);          // exactly one KV window remains

    // Descriptors are long-term and survive rotation intact.
    CHECK_EQ(rs->GetThreadIds().Size(), (long long)countof(rd::kThreads));
    CHECK_EQ(rs->GetTimerIds().Size(), (long long)countof(rd::kTimers));
    CHECK_EQ(rs->GetCounterIds().Size(), (long long)countof(rd::kCounters));
    CHECK_EQ(rs->GetGroupIdMax(), rd::kGroupIdMax);
    CHECK_EQ(rs->GetThreadIdGpu(), rd::kThreadIdGpu);

    // A retained frame's descriptor is intact and matches the source frame.
    const auto* d = rs->GetFrameDesc(fmax);
    CHECK(d != nullptr);
    if (d)
    {
        const auto& fr = rd::kFrames[rd::kNumFramesExtracted - 1];
        CHECK_EQ(d->frame_absolute_id(), fr.absolute_id);
        CHECK_EQ(d->tick_start_cpu(), fr.tsc_start);
    }

    // The retained window still yields scope events (log data survived rotation/trim).
    auto it = rs->CreateLogIterator();
    it.RewindTo(fmin, fmax);
    const auto* st = it.GetState();
    size_t scopes = 0;
    while (it.Step())
    {
        if (st->is_enter() || st->is_exit())
            ++scopes;
    }
    CHECK(scopes > 0);
}

TEST_CASE(capture_native_object_cache_preserves_pointer_and_size)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    MpSession* session = MpOpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    MpEnableObjectCache(session, 1, 1, 1, 1);
    const uint8_t* first = MpGetTimerDesc(session, rd::kTimers[0].id);
    const mpsize_t first_size = MpGetObjSize(session);
    CHECK(first != nullptr);
    CHECK(first_size > 0);

    // Interleave another object query, then hit the first object's cache entry.
    CHECK(MpGetThreadDesc(session, rd::kThreads[0].id) != nullptr);
    const uint8_t* again = MpGetTimerDesc(session, rd::kTimers[0].id);
    CHECK(again == first);
    CHECK_EQ(MpGetObjSize(session), first_size);
    CHECK(MpCloseSession(session));
}

TEST_CASE(capture_native_object_cache_hits_consume_stacked_data_locks)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    ExternalCapture source{&buf};
    GPRX::ExternalIO io = make_external_io();

    auto context = ntd::make_unique<GPRX::DataContextExternalIO>();
    GPRX::DataContextExternalIO* lock_state = context.get();
    context->AssignIO(io);
    CHECK(context->Open(reinterpret_cast<uint8_t*>(&source)));
    MpSession* session = MpOpenFromDataContextTrusted(ntd::move(context), 0);
    CHECK(session != nullptr);
    if (!session)
        return;

    // Warm two descriptor entries in the C-level object cache.
    MpEnableObjectCache(session, 0, 1, 0, 0);
    const uint8_t* timer = MpGetTimerDesc(session, rd::kTimers[0].id);
    const uint8_t* thread = MpGetThreadDesc(session, rd::kThreads[0].id);
    CHECK(timer != nullptr);
    CHECK(thread != nullptr);
    CHECK_EQ(lock_state->GetActiveLockLevel(), 0u);
    const uint32_t reads_after_warmup = source.reads;

    CHECK(MpPushDataLock(session));
    CHECK_EQ(lock_state->GetActiveLockLevel(), 1u);
    CHECK(MpGetTimerDesc(session, rd::kTimers[0].id) == timer);
    CHECK_EQ(source.reads, reads_after_warmup);
    CHECK_EQ(lock_state->GetActiveLockLevel(), 0u);

    CHECK(MpPushDataLock(session));
    CHECK_EQ(lock_state->GetActiveLockLevel(), 2u);
    CHECK(MpGetThreadDesc(session, rd::kThreads[0].id) == thread);
    CHECK_EQ(source.reads, reads_after_warmup);
    CHECK_EQ(lock_state->GetActiveLockLevel(), 0u);

    // An ordinary uncached query cannot claim either already-consumed lock level.
    CHECK(MpGetFrameDesc(session, 1) != nullptr);
    CHECK_EQ(lock_state->GetActiveLockLevel(), 0u);
    CHECK(MpPopDataLock(session));
    CHECK(MpPopDataLock(session));

    CHECK(MpPushDataLock(session));
    CHECK_EQ(lock_state->GetActiveLockLevel(), 1u);
    CHECK(MpConsumeDataLock(session));
    CHECK_EQ(lock_state->GetActiveLockLevel(), 0u);
    CHECK(!MpConsumeDataLock(session));
    CHECK(MpPopDataLock(session));

    CHECK(MpCloseSession(session));
    CHECK_EQ(source.opens, 1u);
    CHECK_EQ(source.closes, 1u);
}

TEST_CASE(capture_unknown_kv_subtype_uses_generic_fallback)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);

    mpsize_t changedLongTermStores = 0;
    {
        GPRX::DataContextSingleBuffer source;
        source.AssignBuffer(buf.data(), GPRX::to_mpsize(buf.size()));
        GPRX::ContainerContext container(source);
        CHECK(container.GetValidationStatus().is_valid);

        for (auto& block : container.GetBlocks())
        {
            if (block.headerPtr->type_id == GPRX::BlockType::KVStore &&
                block.headerPtr->type_meta == static_cast<uint8_t>(LibMP::BlockTypeMeta::Kv_LongTerm))
            {
                CHECK(block.payloadOffsetOrig >= sizeof(GPRX::BlockHeader));
                if (block.payloadOffsetOrig < sizeof(GPRX::BlockHeader))
                    continue;

                const size_t disk_header_offset = static_cast<size_t>(
                    block.payloadOffsetOrig - sizeof(GPRX::BlockHeader));
                CHECK(disk_header_offset <= buf.size() - sizeof(GPRX::BlockHeader));
                if (disk_header_offset > buf.size() - sizeof(GPRX::BlockHeader))
                    continue;

                GPRX::BlockHeader disk_header;
                std::memcpy(&disk_header, buf.data() + disk_header_offset, sizeof(disk_header));
                disk_header.type_meta = 255;
                std::memcpy(buf.data() + disk_header_offset, &disk_header, sizeof(disk_header));
                block.headerPtr->type_meta = 255;
                changedLongTermStores++;
            }
        }
    }
    CHECK(changedLongTermStores > 0);
    if (changedLongTermStores == 0)
        return;

    MpSession* session = nullptr;
    {
        libmp_test::ScopedExpectedDiagnostic expected(kKvSubtypeDiagnostic);
        session = MpOpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
        CHECK_EQ(expected.Hits(),
            libmp_test::ExpectedDiagnosticHits(changedLongTermStores));
    }
    CHECK(session != nullptr);
    if (!session)
        return;

    CHECK(MpIsSessionValid(session));
    CHECK(!MpGetContainerFromSession(session)->HasGlobalIndex());
    CHECK(MpGetTimerDesc(session, rd::kTimers[0].id) != nullptr);
    CHECK(MpCloseSession(session));

    // Trusted loading uses the compact index headers directly, so it does not
    // revisit the deliberately modified scattered header.
    MpSession* trusted = nullptr;
    {
        libmp_test::ScopedExpectedDiagnostic expected(kKvSubtypeDiagnostic);
        trusted = MpOpenFromMemTrusted(buf.data(), GPRX::to_mpsize(buf.size()));
        CHECK_EQ(expected.Hits(), 0);
    }
    CHECK(trusted != nullptr);
    if (trusted)
    {
        CHECK(MpIsSessionValid(trusted));
        CHECK(MpGetContainerFromSession(trusted)->HasGlobalIndex());
        CHECK(MpGetTimerDesc(trusted, rd::kTimers[0].id) != nullptr);
        CHECK(MpCloseSession(trusted));
    }
}

TEST_CASE(capture_table_verification_policy)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);

    MpSession* first = MpOpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(first != nullptr);
    if (!first)
        return;
    const uint8_t* object = MpGetTimerDesc(first, rd::kTimers[0].id);
    const mpsize_t object_size = MpGetObjSize(first);
    CHECK(object != nullptr);
    CHECK(object_size >= sizeof(uint32_t));
    const size_t object_offset = static_cast<size_t>(object - buf.data());
    CHECK(MpCloseSession(first));
    if (!object || object_size < sizeof(uint32_t))
        return;

    // Corrupt only the root offset of a size-bounded object. Container/KV structure
    // remains valid, so rejection proves the typed object itself is verified.
    std::memset(buf.data() + object_offset, 0xFF, sizeof(uint32_t));
    MpSession* corrupt = MpOpenFromMem(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(corrupt != nullptr);
    if (!corrupt)
        return;
    CHECK(MpGetTimerDesc(corrupt, rd::kTimers[0].id) == nullptr);
    CHECK(MpCloseSession(corrupt));

    // Trusted sessions retain all container/range checks but deliberately skip the
    // FlatBuffers graph walk normally performed before typed objects are returned.
    MpSession* trusted = MpOpenFromMemTrusted(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(trusted != nullptr);
    if (trusted)
    {
        CHECK(MpGetTimerDesc(trusted, rd::kTimers[0].id) != nullptr);
        CHECK_EQ(MpGetObjSize(trusted), object_size);
        CHECK(MpCloseSession(trusted));
    }
}

TEST_CASE(capture_rejects_malformed_iterator_configs)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    // Iterator configurations are API inputs, not part of the trusted GPRX producer
    // contract, and remain verified even for a trusted-data session.
    MpSession* session = MpOpenFromMemTrusted(buf.data(), GPRX::to_mpsize(buf.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    MpLogIterator* log = MpCreateLogIterator(session);
    CHECK(log != nullptr);
    if (log)
    {
        uint8_t* config = MpGetLogIteratorConfigBuffer(log, 64);
        CHECK(config != nullptr);
        if (config)
            std::memset(config, 0xFF, 64);
        CHECK(MpConfigureLogIterator(log, false));
        CHECK(!MpStepLogIterator(log));
        MpDeleteLogIterator(log);
    }

    MpCounterIterator* counter = MpCreateCounterIterator(session);
    CHECK(counter != nullptr);
    if (counter)
    {
        uint8_t* config = MpGetCounterIteratorConfigBuffer(counter, 64);
        CHECK(config != nullptr);
        if (config)
            std::memset(config, 0xFF, 64);
        CHECK(MpConfigureCounterIterator(counter, false));
        CHECK(!MpStepCounterIterator(counter));
        MpDeleteCounterIterator(counter);
    }
    CHECK(MpCloseSession(session));
}

TEST_CASE(capture_iterators_outlive_their_session)
{
    LibMP::Init();
    ntd::vector<uint8_t> buf = BuildCaptureBuffer(256, true);
    GPRX::ExternalIO io = make_external_io();

    // With a caching proxy, the log iterator's data locks also pin cache pages.
    for (mpsize_t cache_size : {mpsize_t{0}, mpsize_t{32 * 1024}})
    {
        ExternalCapture source{&buf};
        MpSession* session = MpOpenFromExternalProvider(
            &io, reinterpret_cast<uint8_t*>(&source), cache_size);
        CHECK(session != nullptr);
        if (!session)
            continue;

        MpLogIterator* log = MpCreateLogIterator(session);
        MpCounterIterator* counter = MpCreateCounterIterator(session);
        CHECK(log != nullptr);
        CHECK(counter != nullptr);
        // Deleted while the session is open, so the session must stop tracking it.
        MpDeleteLogIterator(MpCreateLogIterator(session));
        if (!log || !counter)
        {
            MpDeleteLogIterator(log);
            MpDeleteCounterIterator(counter);
            CHECK(MpCloseSession(session));
            continue;
        }

        // Stop the log iterator on a raw entry, which it reads under a data lock.
        while (MpGetLogIteratorCurrentEntryCount(log) == 0 && MpStepLogIterator(log)) {}
        CHECK(MpGetLogIteratorCurrentEntry(log) != nullptr);
        CHECK(MpStepCounterIterator(counter));
        CHECK(MpCloseSession(session));
        CHECK_EQ(source.closes, 1u);

        // Detached iterators stop stepping, keep their own memory readable, and are
        // deleted without touching the closed session.
        CHECK(!MpStepLogIterator(log));
        CHECK(MpGetLogIteratorCurrentEntry(log) == nullptr);
        uint32_t label_length = 1;
        CHECK(MpGetLogIteratorCurrentLabel(log, &label_length) == nullptr);
        CHECK_EQ(label_length, 0u);
        const auto* log_state = reinterpret_cast<const LibMP::TypesNtd::LogIteratorState*>(
            MpGetLogIteratorState(log));
        CHECK(log_state != nullptr);
        if (log_state)
            CHECK(!log_state->started());
        CHECK(MpGetLogIteratorConfigBuffer(log, 64) != nullptr);
        CHECK(!MpConfigureLogIterator(log, true));
        MpRewindLogIteratorTo(log, 1, 2);
        MpRewindLogIterator(log);
        CHECK(!MpStepLogIterator(log));
        MpDeleteLogIterator(log);

        const auto* counter_state = reinterpret_cast<
            const LibMP::TypesNtd::CounterIteratorState*>(MpGetCounterIteratorState(counter));
        CHECK(counter_state != nullptr);
        if (counter_state)
            CHECK(!counter_state->started());
        CHECK(MpGetCounterIteratorConfigBuffer(counter, 64) != nullptr);
        CHECK(!MpConfigureCounterIterator(counter, true));
        MpRewindCounterIterator(counter);
        CHECK(!MpStepCounterIterator(counter));
        MpDeleteCounterIterator(counter);
    }
}

TEST_CASE(capture_trusted_cpp_open_variants)
{
    LibMP::Init();
    ntd::vector<uint8_t> capture = BuildCaptureBuffer(256, true);

    auto memory_session = LibMP::Session::OpenFromMemTrusted(
        capture.data(), GPRX::to_mpsize(capture.size()));
    CHECK(memory_session && memory_session->IsValid());
    if (memory_session)
        CHECK(memory_session->GetTimerDesc(rd::kTimers[0].id) != nullptr);

    auto context = ntd::make_unique<GPRX::DataContextSingleBuffer>();
    context->AssignBuffer(capture.data(), capture.size());
    auto context_session = LibMP::Session::OpenFromDataContextTrusted(ntd::move(context));
    CHECK(context_session && context_session->IsValid());

    ExternalCapture source{&capture};
    GPRX::ExternalIO io = make_external_io();
    auto provider_session = LibMP::Session::OpenFromExternalProviderTrusted(
        &io, reinterpret_cast<uint8_t*>(&source));
    CHECK(provider_session && provider_session->IsValid());
    provider_session.reset();
    CHECK_EQ(source.opens, 1u);
    CHECK_EQ(source.closes, 1u);

    const char* path = "libmp_trusted_open_test.gprx";
    std::ofstream file(path, std::ios::binary);
    CHECK(!!file);
    if (file)
    {
        file.write(reinterpret_cast<const char*>(capture.data()),
                   static_cast<std::streamsize>(capture.size()));
        file.close();
        CHECK(!file.fail());
        auto file_session = LibMP::Session::OpenFromFileTrusted(
            LibMP::DefaultFlavor::string(path));
        CHECK(file_session && file_session->IsValid());
        file_session.reset();
        CHECK_EQ(std::remove(path), 0);
    }
}

TEST_CASE(capture_conversion_presets_round_trip_without_mutating_session)
{
    LibMP::Init();
    ntd::vector<uint8_t> capture = BuildCaptureBuffer(256, true);
    auto source = LibMP::Session::OpenFromMemTrusted(
        capture.data(), GPRX::to_mpsize(capture.size()));
    CHECK(source && source->IsValid());
    if (!source)
        return;

    GPRX::ContainerContext* source_container =
        MpGetContainerFromSession(source->GetHandle());
    for (GPRX::BlockContext& block : source_container->GetBlocks())
    {
        if (block.headerPtr && GPRX::Block::IsMagic(block.headerPtr))
            block.bFixedEncoding = true;
    }

    const LibMP::Types::ContainerFormat formats[] = {
        LibMP::Enums::ContainerFormat::Raw,
        LibMP::Enums::ContainerFormat::Compressed,
        LibMP::Enums::ContainerFormat::CompressedAscii,
        LibMP::Enums::ContainerFormat::CompressedHtml,
    };
    LibMP::DefaultFlavor::vector<uint8_t> html;
    for (LibMP::Types::ContainerFormat format : formats)
    {
        auto converted =
            LibMP::Tools::ConvertToBuffer(*source, format);
        CHECK(!converted.empty());
        if (converted.empty())
            continue;
        auto reopened = LibMP::Session::OpenFromMemTrusted(
            converted.data(), GPRX::to_mpsize(converted.size()));
        CHECK(reopened && reopened->IsValid());
        if (reopened)
        {
            CHECK_EQ(reopened->GetFrameIdMin(), 1u);
            CHECK_EQ(reopened->GetFrameIdMax(), rd::kNumFramesExtracted);
            CHECK(reopened->GetTimerDesc(rd::kTimers[0].id) != nullptr);

            GPRX::ContainerContext* converted_container =
                MpGetContainerFromSession(reopened->GetHandle());
            const auto expected_encoding =
                format == LibMP::Enums::ContainerFormat::Raw
                ? GPRX::BlockPayloadEncodingMethod::Raw
                : format == LibMP::Enums::ContainerFormat::Compressed
                ? GPRX::BlockPayloadEncodingMethod::LZ4
                : GPRX::BlockPayloadEncodingMethod::LZ4_Base64;
            if (format != LibMP::Enums::ContainerFormat::CompressedHtml)
            {
                CHECK(converted_container->HasGlobalIndex());
                GPRX::BlockContext* converted_index = converted_container->GetBlock(
                    converted_container->GetGlobalIndexBlockId());
                CHECK(converted_index != nullptr);
                if (converted_index)
                    CHECK_EQ(converted_index->headerPtr->encoding_method, expected_encoding);
            }

            bool saw_magic = false;
            bool saw_payload = false;
            for (const GPRX::BlockContext& block : converted_container->GetBlocks())
            {
                if (!block.headerPtr)
                    continue;
                if (GPRX::Block::IsMagic(block.headerPtr))
                {
                    saw_magic = true;
                    CHECK_EQ(GPRX::IsMagicAsciiEncoded(block.headerPtr),
                        format == LibMP::Enums::ContainerFormat::CompressedAscii ||
                            format == LibMP::Enums::ContainerFormat::CompressedHtml);
                    if (block.headerPtr->payload_uncompressed_size > 0)
                        CHECK_EQ(block.headerPtr->encoding_method,
                            GPRX::BlockPayloadEncodingMethod::Raw);
                }
                else if (block.headerPtr->type_id != GPRX::BlockType::GlobalIndex &&
                    block.headerPtr->payload_uncompressed_size > 0)
                {
                    saw_payload = true;
                    CHECK_EQ(block.headerPtr->encoding_method, expected_encoding);
                }
            }
            CHECK(saw_magic);
            CHECK(saw_payload);
        }
        if (format == LibMP::Enums::ContainerFormat::CompressedHtml)
            html = ntd::move(converted);
    }

    static constexpr char html_suffix[] = "</html>\n";
    static constexpr char banner[] =
        "[[ Open with LibMP :: https://github.com/Roblox/libmp ]]";
    CHECK(std::search(html.begin(), html.end(), banner, banner + sizeof(banner) - 1) !=
        html.end());
    CHECK(html.size() >= sizeof(html_suffix) - 1);
    if (html.size() >= sizeof(html_suffix) - 1)
    {
        CHECK(std::memcmp(
            html.data() + html.size() - (sizeof(html_suffix) - 1),
            html_suffix, sizeof(html_suffix) - 1) == 0);
    }

    size_t comment_close_offset = html.size();
    for (size_t i = GPRX::MagicPartSize;
        i + GPRX::HtmlCommentClose.size() <= html.size(); ++i)
    {
        if (std::memcmp(html.data() + i,
                GPRX::HtmlCommentClose.data(),
                GPRX::HtmlCommentClose.size()) == 0)
        {
            comment_close_offset = i;
            break;
        }
    }
    CHECK(comment_close_offset > GPRX::MagicPartSize);
    if (comment_close_offset > GPRX::MagicPartSize)
    {
        bool ascii_only = true;
        for (size_t i = 0; i < comment_close_offset; ++i)
        {
            if (html[i] < 0x20 || html[i] > 0x7e)
            {
                ascii_only = false;
                break;
            }
        }
        CHECK(ascii_only);

        GPRX::DataContextSingleBuffer html_comment_context;
        html_comment_context.AssignBuffer(
            html.data() + GPRX::MagicPartSize,
            comment_close_offset - GPRX::MagicPartSize);
        GPRX::ContainerContext html_comment(
            html_comment_context, GPRX::InputTrust::Trusted);
        CHECK(html_comment.GetValidationStatus().is_valid);
        CHECK(html_comment.HasGlobalIndex());
        GPRX::BlockContext* html_index =
            html_comment.GetBlock(html_comment.GetGlobalIndexBlockId());
        CHECK(html_index != nullptr);
        if (html_index)
        {
            CHECK_EQ(html_index->headerPtr->encoding_method,
                GPRX::BlockPayloadEncodingMethod::LZ4_Base64);
        }
    }

    const std::string html_text(
        reinterpret_cast<const char*>(html.data()), html.size());
    const uint32_t source_data_format = source->GetDataFormatVersion();
    const std::string data_format_directory =
        std::to_string(LibMP::Version::GetMajor(source_data_format)) + "." +
        std::to_string(LibMP::Version::GetMinor(source_data_format)) + "/";
    const std::string script_start = "<script src=\"";
    std::string html_asset_prefix;
    bool found_asset_prefix = false;
    size_t script_offset = comment_close_offset;
    for (const char* asset : {"libmp.js", "tools.js", "viewer.js"})
    {
        const size_t start = html_text.find(script_start, script_offset);
        CHECK(start != std::string::npos);
        if (start == std::string::npos)
            break;
        const size_t source_start = start + script_start.size();
        const size_t source_end = html_text.find('"', source_start);
        CHECK(source_end != std::string::npos);
        if (source_end == std::string::npos)
            break;
        const std::string script_source =
            html_text.substr(source_start, source_end - source_start);
        CHECK(script_source.size() >= std::strlen(asset));
        if (script_source.size() < std::strlen(asset))
            break;
        const std::string prefix =
            script_source.substr(0, script_source.size() - std::strlen(asset));
        CHECK(script_source.substr(prefix.size()) == asset);
        if (found_asset_prefix)
            CHECK(prefix == html_asset_prefix);
        else
        {
            html_asset_prefix = prefix;
            found_asset_prefix = true;
        }
        script_offset = source_end + 1;
    }
    if (found_asset_prefix && !html_asset_prefix.empty())
    {
        CHECK(html_asset_prefix.size() >= data_format_directory.size());
        if (html_asset_prefix.size() >= data_format_directory.size())
        {
            CHECK(html_asset_prefix.compare(
                html_asset_prefix.size() - data_format_directory.size(),
                data_format_directory.size(), data_format_directory) == 0);
        }
    }

    // Exercise encoded-input decoding as well as raw-input encoding.
    auto html_source = LibMP::Session::OpenFromMemTrusted(
        html.data(), GPRX::to_mpsize(html.size()));
    CHECK(html_source && html_source->IsValid());
    if (html_source)
    {
        auto raw = LibMP::Tools::ConvertToBuffer(
            *html_source, LibMP::Enums::ContainerFormat::Raw);
        CHECK(!raw.empty());
        auto reopened = LibMP::Session::OpenFromMemTrusted(
            raw.data(), GPRX::to_mpsize(raw.size()));
        CHECK(reopened && reopened->IsValid());
    }

    CHECK(source->IsValid());
    CHECK(source->GetTimerDesc(rd::kTimers[0].id) != nullptr);

    RejectingToolOutput rejected_output;
    CHECK(!LibMP::ConvertSessionToStream(
        source->GetHandle(), LibMP::TypesNtd::ContainerFormat_CompressedHtml,
        rejected_output));
    CHECK(rejected_output.calls == 2);
    CHECK(LibMP::Tools::ConvertToBuffer(*source,
        static_cast<LibMP::Types::ContainerFormat>(4)).empty());
    CHECK(!LibMP::Tools::ConvertToBuffer(
        *source, LibMP::Enums::ContainerFormat::Raw).empty());

    static constexpr char custom_asset_base[] =
        "https://assets.example.test/libmp/";
    CHECK(LibMP::Tools::SetHtmlAssetsBaseUrl(
        LibMP::DefaultFlavor::string(custom_asset_base)));
    auto custom_html = LibMP::Tools::ConvertToBuffer(
        *source, LibMP::Enums::ContainerFormat::CompressedHtml);
    const std::string custom_html_text(
        reinterpret_cast<const char*>(custom_html.data()), custom_html.size());
    const std::string custom_asset_prefix =
        std::string(custom_asset_base) + data_format_directory;
    for (const char* asset : {"libmp.js", "tools.js", "viewer.js"})
    {
        CHECK(custom_html_text.find(
            "<script src=\"" + custom_asset_prefix + asset + "\"></script>") !=
            std::string::npos);
    }
    CHECK(LibMP::Tools::SetHtmlAssetsBaseUrl(""));

    const char* output_path = "libmp_conversion_test.gprx";
    CHECK(LibMP::Tools::ConvertToFile(
        *source, LibMP::Enums::ContainerFormat::Raw,
        LibMP::DefaultFlavor::string(output_path)));
    auto file_session = LibMP::Session::OpenFromFileTrusted(
        LibMP::DefaultFlavor::string(output_path));
    CHECK(file_session && file_session->IsValid());
    file_session.reset();
    CHECK_EQ(std::remove(output_path), 0);
}

TEST_CASE(incremental_html_writer_builds_one_index_for_appended_containers)
{
    LibMP::Init();
    GPRX::ContainerContext first;
    first.AddBlock(1, true);
    first.InitMagicBlock(first.GetBlocks().back());
    static const uint8_t magic_payload[] =
        "[[ Open with LibMP :: https://github.com/Roblox/libmp ]]";
    CHECK(first.AppendPayloadData(
        first.GetBlocks().back(), magic_payload, sizeof(magic_payload) - 1));
    first.GetBlocks().back().bFixedEncoding = true;
    first.AddBlock(2, true);
    GPRX::BlockContext& first_data = first.GetBlocks().back();
    first_data.headerPtr->type_id = GPRX::BlockType::Data;
    static const uint8_t first_payload[] = {1, 2, 3, 4};
    CHECK(first.AppendPayloadData(first_data, first_payload, sizeof(first_payload)));

    GPRX::ContainerContext second;
    second.AddBlock(3, true);
    GPRX::BlockContext& second_data = second.GetBlocks().back();
    second_data.headerPtr->type_id = GPRX::BlockType::Data;
    static const uint8_t second_payload[] = {5, 6, 7, 8};
    CHECK(second.AppendPayloadData(second_data, second_payload, sizeof(second_payload)));

    GPRX::StreamProcessorChunkedMemory output;
    GPRX::IncrementalContainerWriter writer;
    CHECK(LibMP::BeginIncrementalHtmlStream(writer, output));
    CHECK(writer.Append(first));
    CHECK(writer.Append(second));
    CHECK(LibMP::FinishIncrementalHtmlStream(writer));
    ntd::vector<uint8_t> bytes = output.GetComposedChunks();
    CHECK(!bytes.empty());
    CHECK(std::search(bytes.begin(), bytes.end(),
        magic_payload, magic_payload + sizeof(magic_payload) - 1) != bytes.end());

    auto session = LibMP::Session::OpenFromMemTrusted(
        bytes.data(), GPRX::to_mpsize(bytes.size()));
    CHECK(session && session->IsValid());
    if (!session)
        return;
    GPRX::ContainerContext* container =
        MpGetContainerFromSession(session->GetHandle());
    GPRX::BlockContext* magic = container->GetBlock(1);
    CHECK(magic != nullptr);
    if (magic)
        CHECK_EQ(magic->headerPtr->encoding_method,
            GPRX::BlockPayloadEncodingMethod::Raw);
    CHECK(container->GetBlock(2) != nullptr);
    CHECK(container->GetBlock(3) != nullptr);

    size_t comment_close_offset = bytes.size();
    for (size_t index = GPRX::MagicPartSize;
        index + GPRX::HtmlCommentClose.size() <= bytes.size(); ++index)
    {
        if (std::memcmp(
                bytes.data() + index,
                GPRX::HtmlCommentClose.data(),
                GPRX::HtmlCommentClose.size()) == 0)
        {
            comment_close_offset = index;
            break;
        }
    }
    GPRX::DataContextSingleBuffer comment_data;
    comment_data.AssignBuffer(
        bytes.data() + GPRX::MagicPartSize,
        comment_close_offset - GPRX::MagicPartSize);
    GPRX::ContainerContext comment_container(
        comment_data, GPRX::InputTrust::Trusted);
    CHECK(comment_container.GetValidationStatus().is_valid);
    CHECK(comment_container.HasGlobalIndex());
}

TEST_CASE(capture_context_keeps_all_512_frames_in_one_generation)
{
    LibMP::Init();
    LibMP::CaptureContext capture(512);
    capture.Init();
    for (uint32_t frame = 0; frame < 512; ++frame)
    {
        capture.StartAddingFrame(frame == 0, true);
        capture.FinishAddingFrame();
    }
    CHECK_EQ(capture.frames_in_kv0, 0u);
    CHECK_EQ(capture.frames_in_kv1, 512u);
    CHECK_EQ(capture.GetFramesInKv(), 512u);
}

TEST_CASE(segment_capture_context_resets_for_reuse)
{
    LibMP::Init();
    LibMP::SegmentCaptureContext segment;
    segment.Reset(17, 41);

    CHECK_EQ(segment.live_container->GetBlocks().size(), 10u);
    CHECK_EQ(segment.basicmeta_block.GetBlockId(), 17u);
    CHECK_EQ(segment.trackfinish_block.GetBlockId(), 26u);
    CHECK(segment.trackfinish_block.ptr->bDontSave);

    segment.StartAddingFrame();
    segment.SelectKV(segment.kvshortterm_block, segment.shortterm_block);
    segment.SetUid(LibMP::QueryEx::FrameIdMin);
    segment.PutU32(41);
    CHECK_EQ(segment.frames_counter, 41u);

    segment.Reset(101, 73);
    CHECK_EQ(segment.live_container->GetBlocks().size(), 10u);
    CHECK_EQ(segment.basicmeta_block.GetBlockId(), 101u);
    CHECK_EQ(segment.trackfinish_block.GetBlockId(), 110u);
    CHECK_EQ(segment.frames_counter, 72u);
    CHECK_EQ(segment.storedFramesCount, 0u);
    CHECK(segment.trackfinish_block.ptr->bDontSave);
}

TEST_CASE(capture_external_provider_cache_and_sync_pipeline)
{
    LibMP::Init();
    ntd::vector<uint8_t> full = BuildCaptureBuffer(256, true);
    ntd::vector<uint8_t> rotated = BuildCaptureBuffer(8, false);
    GPRX::ExternalIO io = make_external_io();

    for (mpsize_t cache_size : {mpsize_t{0}, mpsize_t{32 * 1024}})
    {
        ExternalCapture source{&full};
        MpSession* session = cache_size == 0
            ? MpOpenFromExternalProvider(&io, reinterpret_cast<uint8_t*>(&source), cache_size)
            : MpOpenFromExternalProviderTrusted(
                &io, reinterpret_cast<uint8_t*>(&source), cache_size);
        CHECK(session != nullptr);
        if (!session)
            continue;
        CHECK(MpIsSessionValid(session));
        CHECK_EQ(MpGetFrameIdMin(session), 1u);
        CHECK_EQ(MpGetFrameIdMax(session), rd::kNumFramesExtracted);
        CHECK(source.reads > 0);
        const uint8_t* timer_object = MpGetTimerDesc(session, rd::kTimers[0].id);
        CHECK(timer_object != nullptr);
        CHECK(MpGetObjSize(session) > 0);

        // Atomically replace the external provider's capture, then exercise the
        // full-sync/cache-invalidation path used after data replacement.
        source.bytes = &rotated;
        MpSetClearCacheOnSyncRequired(session);
        MpSetSyncRequired(session);
        CHECK(MpSyncIfRequired(session));
        CHECK(MpIsSessionValid(session));
        CHECK_EQ(MpGetFrameIdMax(session), rd::kNumFramesExtracted);
        CHECK_EQ(MpGetFrameIdMax(session) - MpGetFrameIdMin(session) + 1u, 8u);
        CHECK(MpCloseSession(session));
        CHECK_EQ(source.opens, 1u);
        CHECK_EQ(source.closes, 1u);
    }
}

TEST_CASE(capture_related_snapshots_invalidate_changed_cache_bucket_only)
{
    LibMP::Init();
    ntd::vector<uint8_t> baseline =
        BuildRelatedCaptureBuffer(RelatedCaptureStage::Baseline);
    ntd::vector<uint8_t> short_term_update =
        BuildRelatedCaptureBuffer(RelatedCaptureStage::ShortTermChanged);
    ntd::vector<uint8_t> long_term_update =
        BuildRelatedCaptureBuffer(RelatedCaptureStage::LongTermChanged);

    ExternalCapture source{&baseline};
    GPRX::ExternalIO io = make_external_io();
    MpSession* session = MpOpenFromExternalProvider(
        &io, reinterpret_cast<uint8_t*>(&source), 0);
    CHECK(session != nullptr);
    if (!session)
        return;

    MpEnableObjectCache(session, 1, 1, 1, 1);
    CHECK(MpGetGlobalDesc(session) != nullptr);
    CHECK(MpGetGeneralInfo(session) != nullptr);
    CHECK(MpGetTimerDesc(session, rd::kTimers[0].id) != nullptr);

    CacheBucketRecorder recorder;
    MpSetClearObjCacheCallback(
        session, record_cleared_cache_buckets,
        reinterpret_cast<uint8_t*>(&recorder));

    source.bytes = &short_term_update;
    MpSetSyncRequired(session);
    CHECK(MpSyncIfRequired(session));
    CHECK_EQ(recorder.calls, 1u);
    CHECK_EQ(recorder.buckets.size(), 1u);
    if (recorder.buckets.size() == 1)
        CHECK_EQ(recorder.buckets[0], 0u); // ShortTerm

    const uint8_t* general_object = MpGetGeneralInfo(session);
    CHECK(general_object != nullptr);
    if (general_object)
    {
        const auto* general = LibMP::flatbuffers::GetRoot<
            LibMP::TypesNtd::GeneralInfo>(general_object);
        CHECK(general->platform_info_json() != nullptr);
        if (general->platform_info_json())
            CHECK(std::strcmp(
                general->platform_info_json()->c_str(),
                "{\"libmp_fixture\":\"short-term-updated\"}") == 0);
    }
    CHECK(MpGetGlobalDesc(session) != nullptr);
    CHECK(MpGetTimerDesc(session, rd::kTimers[0].id) != nullptr);

    source.bytes = &long_term_update;
    MpSetSyncRequired(session);
    CHECK(MpSyncIfRequired(session));
    CHECK_EQ(recorder.calls, 2u);
    CHECK_EQ(recorder.buckets.size(), 1u);
    if (recorder.buckets.size() == 1)
        CHECK_EQ(recorder.buckets[0], 1u); // LongTerm

    const uint8_t* timer_object = MpGetTimerDesc(session, rd::kTimers[0].id);
    CHECK(timer_object != nullptr);
    if (timer_object)
    {
        const auto* timer = LibMP::flatbuffers::GetRoot<
            LibMP::TypesNtd::TimerDesc>(timer_object);
        CHECK(timer->timer_name() != nullptr);
        if (timer->timer_name())
            CHECK(std::strcmp(
                timer->timer_name()->c_str(),
                "libmp-fixture-long-term-updated-timer") == 0);
    }
    CHECK(MpCloseSession(session));
}

TEST_CASE(capture_range_serving_to_consumer_pipeline)
{
    LibMP::Init();
    ntd::vector<uint8_t> source = BuildCaptureBuffer(256, true);
    GPRX::DataContextSingleBuffer live_data;
    live_data.AssignBuffer(source.data(), source.size());
    ntd::vector<uint8_t> snapshot(source.size());

    // Recreate a snapshot through repeated service-sized range calls, including a
    // short final read. This covers CaptureBuilder -> GetDataSize/GetDataInRange-like
    // serving -> public reader consumption without relying on a binary fixture.
    uint64_t offset = 0;
    while (offset < source.size())
    {
        const uint32_t request = static_cast<uint32_t>(
            ntd::min<uint64_t>(137, source.size() - offset));
        CHECK_EQ(LibMP::ReadDataInRange(
            live_data, offset, request, snapshot.data(),
            GPRX::to_mpsize(snapshot.size()), static_cast<uint32_t>(offset)), request);
        offset += request;
    }

    CHECK(std::memcmp(snapshot.data(), source.data(), source.size()) == 0);
    auto session = LibMP::Session::OpenFromMem(
        snapshot.data(), GPRX::to_mpsize(snapshot.size()));
    CHECK(session && session->IsValid());
    if (session)
    {
        CHECK_EQ(session->GetFrameIdMin(), 1u);
        CHECK_EQ(session->GetFrameIdMax(), rd::kNumFramesExtracted);
        CHECK_EQ(session->GetThreadIds().Size(), countof(rd::kThreads));
    }
}

LIBMP_TEST_MAIN("capture_roundtrip")

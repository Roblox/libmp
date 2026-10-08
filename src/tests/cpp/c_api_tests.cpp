// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// c_api_tests.cpp -- robustness of the public C ABI against null handles, invalid
// indices, and invalid/empty containers. These are the boundary contracts a
// source-available consumer (and the fuzzers) can hit; they must fail safe, not crash.
//
// Valid-session behavior (cache hits, pointer lifetimes, real data) is covered by the
// capture round-trip and cross-language tests, which produce a valid session.

#include "test_harness.h"
#include "diagnostic_assert_test.h"

#include "gprx_container/block.h"
#include "gprx_container/container_context.h"
#include "gprx_container/data_context.h"
#include "gprx_container/kv_store.h"
#include "mp_api/block_ex.h"
#include "mp_api/c_api.h"
#include "mp_api/capture_builder.h"
#include "mp_api/cpp_api.h"
#include "mp_api/log_entry.h"
#include "mp_api/uid_ex.h"

#include <cstdint>
#include <cstring>

namespace
{
constexpr uint64_t HIGH_LABEL_OFFSET = (uint64_t{1} << 40) + 17;

const char* label_text(const uint8_t* label)
{
    return reinterpret_cast<const char*>(label);
}

struct CacheClearNotifications
{
    uint32_t calls = 0;
    uint32_t bucket_mask = 0;
};

struct SessionNotifications
{
    uint32_t calls = 0;
    uint32_t flags = 0;
};

struct ExternalCapture
{
    const ntd::vector<uint8_t>* bytes = nullptr;
};

uint32_t external_open(uint8_t*) { return 1; }
uint32_t external_close(uint8_t*) { return 1; }

mpdatasz_t external_size(uint8_t* handle)
{
    return reinterpret_cast<ExternalCapture*>(handle)->bytes->size();
}

mpsize_t external_read(
    uint8_t* handle, uint8_t* dst, uint32_t blockId, mpdatasz_t offset, mpsize_t size)
{
    const ExternalCapture& capture = *reinterpret_cast<ExternalCapture*>(handle);
    if (blockId != 0 || !dst ||
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

void record_cache_clear(
    uint8_t* handle, const uint32_t* bucket_ids, mpsize_t bucket_count)
{
    auto* notifications = reinterpret_cast<CacheClearNotifications*>(handle);
    notifications->calls++;
    for (mpsize_t i = 0; i < bucket_count; ++i)
        notifications->bucket_mask |= 1u << bucket_ids[i];
}

void record_session_notifications(uint8_t* handle, uint32_t notificationFlags)
{
    auto* notifications = reinterpret_cast<SessionNotifications*>(handle);
    notifications->calls++;
    notifications->flags |= notificationFlags;
}

void add_boundary_block(
    GPRX::ContainerContext& container,
    uint32_t block_id,
    LibMP::BlockTypeEx type,
    uint64_t metadata0,
    uint64_t metadata1)
{
    container.AddBlock(block_id, true);
    GPRX::BlockHeader* header = container.GetBlocks().back().headerPtr;
    header->type_id = GPRX::ToBlockType(type);
    header->metadata0 = metadata0;
    header->metadata1 = metadata1;
}

void add_frame_bounds_block(
    GPRX::ContainerContext& container,
    uint32_t block_id,
    uint32_t frame_id_min,
    uint32_t frame_id_max)
{
    container.AddBlock(block_id, true);
    GPRX::BlockContext& block = container.GetBlocks().back();
    block.headerPtr->type_id = GPRX::BlockType::KVStore;
    block.headerPtr->type_meta =
        static_cast<uint8_t>(LibMP::BlockTypeMeta::Kv_ShortTerm);
    container.RebuildDirectLookupTable();

    GPRX::KVStore store(container, &block);
    CHECK(store.Initialize({8, 0, 32, 16}));

    GPRX::Uid uid;
    uid.Set(GPRX::ToQuery(LibMP::QueryEx::FrameIdMin));
    CHECK(store.Put(uid.entries, uid.size, &uid.precomputed_hash,
        &frame_id_min, 1, GPRX::KVStore::SetStrategy::PutOrReplaceSingle));
    uid.Set(GPRX::ToQuery(LibMP::QueryEx::FrameIdMax));
    CHECK(store.Put(uid.entries, uid.size, &uid.precomputed_hash,
        &frame_id_max, 1, GPRX::KVStore::SetStrategy::PutOrReplaceSingle));
}

ntd::vector<uint8_t> build_track_segment_container()
{
    GPRX::ContainerContext container;
    container.AddBlock(1, true);
    container.InitMagicBlock(container.GetBlocks().back());

    add_boundary_block(container, 2, LibMP::BlockTypeEx::TrackStart,
        LibMP::Enums::AppType::Client, 0);
    add_boundary_block(container, 3, LibMP::BlockTypeEx::SegmentStart,
        (uint64_t{100} << 32) | 10, 1000);
    add_frame_bounds_block(container, 4, 10, 19);
    add_boundary_block(container, 5, LibMP::BlockTypeEx::SegmentFinish,
        (uint64_t{0x8009} << 32) | 19, 2000);
    add_boundary_block(container, 6, LibMP::BlockTypeEx::SegmentStart,
        (uint64_t{120} << 32) | 20, 2100);
    add_frame_bounds_block(container, 7, 20, 29);
    add_boundary_block(container, 8, LibMP::BlockTypeEx::SegmentFinish,
        (uint64_t{0x0004} << 32) | 29, 3000);
    add_boundary_block(container, 9, LibMP::BlockTypeEx::TrackFinish, 0, 0);

    add_boundary_block(container, 10, LibMP::BlockTypeEx::TrackStart,
        LibMP::Enums::AppType::Studio, 0);
    add_boundary_block(container, 11, LibMP::BlockTypeEx::SegmentStart,
        (uint64_t{500} << 32) | 100, 5000);
    add_frame_bounds_block(container, 12, 100, 109);
    add_boundary_block(container, 13, LibMP::BlockTypeEx::SegmentFinish,
        (uint64_t{0x0020} << 32) | 109, 6000);
    add_boundary_block(container, 14, LibMP::BlockTypeEx::TrackFinish, 0, 0);

    return container.SaveToMemory(true);
}

void add_segment_frame(
    GPRX::ContainerContext& container,
    uint32_t& nextBlockId,
    uint32_t frameId,
    uint32_t frameAbsoluteId,
    uint64_t tickStart,
    uint64_t tickEnd,
    uint64_t logEntry)
{
    container.AddBlock(nextBlockId++, true);
    GPRX::BlockContext& logBlock = container.GetBlocks().back();
    logBlock.headerPtr->type_id = GPRX::BlockType::Data;
    const GPRX::Locator logLocator = container.AppendPayloadDataAligned(
        logBlock, reinterpret_cast<const uint8_t*>(&logEntry), sizeof(logEntry));

    container.AddBlock(nextBlockId++, true);
    const mpsize_t frameDataBlockNumber =
        GPRX::to_mpsize(container.GetBlocks().size() - 1);
    container.GetBlocks()[frameDataBlockNumber].headerPtr->type_id =
        GPRX::BlockType::Data;

    container.AddBlock(nextBlockId++, true);
    GPRX::BlockContext& frameKvBlock = container.GetBlocks().back();
    frameKvBlock.headerPtr->type_id = GPRX::BlockType::KVStore;
    frameKvBlock.headerPtr->type_meta =
        static_cast<uint8_t>(LibMP::BlockTypeMeta::Kv_Frames);
    LibMP::KVStoreEx::PutKvStoreFrameIdToMeta(frameKvBlock.headerPtr, frameId);
    container.RebuildDirectLookupTable();

    GPRX::KVStore frameStore(container, &frameKvBlock);
    CHECK(frameStore.Initialize({8, 0, 32, 16}));

    ntd::vector<LibMP::TypesNtd::ThreadLogDescr> threadLogs;
    threadLogs.emplace_back(7, 1, logLocator.entry, false);
    LibMP::FbBuilder builder;
    auto frame = LibMP::TypesNtd::CreateFrameDescDirect(
        builder, frameId, frameAbsoluteId,
        tickStart, tickEnd, 0, 0, 0, 0, false, false, &threadLogs);
    builder.FinishSizePrefixed(frame);

    GPRX::Uid uid;
    uid.Set(GPRX::ToQuery(LibMP::QueryEx::FrameDesc_u24FrameId), true, frameId);
    CHECK(GPRX::KVHelper::PutObj(
        uid, builder.GetBufferPointer(), builder.GetSize(), container,
        container.GetBlocks()[frameDataBlockNumber], &frameStore));

    add_frame_bounds_block(container, nextBlockId++, frameId, frameId);
}

ntd::vector<uint8_t> build_contiguous_segment_log_container()
{
    GPRX::ContainerContext container;
    uint32_t nextBlockId = 1;
    container.AddBlock(nextBlockId++, true);
    container.InitMagicBlock(container.GetBlocks().back());

    add_boundary_block(container, nextBlockId++, LibMP::BlockTypeEx::TrackStart,
        LibMP::Enums::AppType::Client, 0);

    add_boundary_block(container, nextBlockId++, LibMP::BlockTypeEx::SegmentStart,
        (uint64_t{100} << 32) | 10, 100);
    add_segment_frame(container, nextBlockId, 10, 100, 100, 200,
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_ENTER, 1, 150));
    add_boundary_block(container, nextBlockId++, LibMP::BlockTypeEx::SegmentFinish,
        10, 200);

    add_boundary_block(container, nextBlockId++, LibMP::BlockTypeEx::SegmentStart,
        (uint64_t{101} << 32) | 11, 200);
    add_segment_frame(container, nextBlockId, 11, 101, 200, 300,
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LEAVE, 1, 250));
    add_boundary_block(container, nextBlockId++, LibMP::BlockTypeEx::SegmentFinish,
        11, 300);

    add_boundary_block(container, nextBlockId++, LibMP::BlockTypeEx::TrackFinish, 0, 0);
    return container.SaveToMemory(true);
}

ntd::vector<uint8_t> build_label_capture(
    bool includeAlpha = true,
    bool rotateShortTermBlocks = false,
    bool addLookupRegions = false,
    bool addSizeLimitRegions = false,
    uint32_t scopeDepth = 1)
{
    ntd::vector<uint64_t> log_entries;
    log_entries.reserve(scopeDepth * 2 + 2);
    for (uint32_t i = 0; i < scopeDepth; ++i)
        log_entries.push_back(LibMP::MicroProfilerLogMakeEntry(MP_LOG_ENTER, 1, 10));
    log_entries.push_back(LibMP::MicroProfilerLogMakeEntry(MP_LOG_LABEL, 1, 100));
    log_entries.push_back(LibMP::MicroProfilerLogMakeEntry(MP_LOG_LABEL_LITERAL, 1, 0x1234));
    for (uint32_t i = 0; i < scopeDepth; ++i)
        log_entries.push_back(LibMP::MicroProfilerLogMakeEntry(MP_LOG_LEAVE, 1, 20));
    static const uint8_t label_data[] = {
        'a', 'l', 'p', 'h', 'a', 0,
        'b', 'e', 't', 'a', 0,
    };
    static const uint8_t extra_label_data[] = {'x', 0};
    static const uint8_t crossing_label_data[] = {'y', 0, 'z', 0};

    LibMP::LiveContext live;
    live.config.max_frames_per_kv = 8;
    live.Init();
    live.StartAddingFrame(true, true);

    GPRX::Locator log_locator = GPRX::BuildDataLocator(
        live.logs1_block.GetBlockId(), 0);
    CHECK(live.logs1_block.ptr->AddExternalPayloadChunk(
        reinterpret_cast<uint8_t*>(log_entries.data()),
        GPRX::to_mpsize(log_entries.size() * sizeof(uint64_t))));

    ntd::vector<LibMP::TypesNtd::ThreadLogDescr> thread_logs;
    thread_logs.emplace_back(
        7, GPRX::to_uint32(log_entries.size()), log_locator.entry, false);
    live.SelectKV(live.kvframes1_block, live.frames1_block);
    live.BuildTable([&](auto& builder) {
        return LibMP::TypesNtd::CreateFrameDescDirect(
            builder, live.frames_counter, 1, 1, 100, 0, 0,
            100, 111, false, false, &thread_logs);
    });
    live.SetUid(LibMP::QueryEx::FrameDesc_u24FrameId, true, live.frames_counter);
    live.PutTable();
    live.FinishAddingFrame();
    live.bInitialFillingCompleted = true;
    live.TrimLogs(1, 0);

    // Related snapshots need fresh block IDs so an external-provider sync can
    // distinguish replacement short-term data from the previous snapshot.
    if (rotateShortTermBlocks)
    {
        live.ClearAndIncBlockId(live.shortterm_block);
        live.IncBlockId(live.kvshortterm_block);
        live.ResetKvStore(live.kvshortterm_block);
        live.live_container->RebuildDirectLookupTable();
    }

    live.SelectKV(live.kvshortterm_block, live.shortterm_block);
    if (includeAlpha)
    {
        // One logical region may be supplied as two physical ring spans. Split
        // inside alpha to exercise lookup across the composed payload boundary.
        CHECK(live.PutLabelBufferRegion(
            100, label_data, 3, label_data + 3,
            static_cast<mpsize_t>(sizeof(label_data) - 3)));
    }
    else
        CHECK(live.PutLabelBufferRegion(106, label_data + 6, 5));
    if (addLookupRegions)
    {
        CHECK(live.PutLabelBufferRegion(1000, extra_label_data, 2));
        CHECK(live.PutLabelBufferRegion(2000, extra_label_data, 2));
        CHECK(live.PutLabelBufferRegion(3000, extra_label_data, 2));
        CHECK(live.PutLabelBufferRegion(
            64 * 1024 - 2, crossing_label_data, 4));
        CHECK(live.PutLabelBufferRegion(HIGH_LABEL_OFFSET, extra_label_data, 2));
    }
    if (addSizeLimitRegions)
    {
        uint8_t largest_label[1024];
        std::memset(largest_label, 'a', sizeof(largest_label));
        largest_label[sizeof(largest_label) - 1] = 0;
        CHECK(live.PutLabelBufferRegion(
            100000, largest_label, static_cast<mpsize_t>(sizeof(largest_label))));

        uint8_t oversized_label[1025];
        std::memset(oversized_label, 'b', sizeof(oversized_label));
        oversized_label[sizeof(oversized_label) - 1] = 0;
        CHECK(live.PutLabelBufferRegion(
            200000, oversized_label, static_cast<mpsize_t>(sizeof(oversized_label))));
    }
    live.SetUid(LibMP::QueryEx::FrameIdMin);
    live.PutU32(1);
    live.SetUid(LibMP::QueryEx::FrameIdMax);
    live.PutU32(1);

    live.SetDataChanged();
    live.CheckExport();
    GPRX::StreamProcessorChunkedMemory output;
    CHECK(live.live_container->SaveToStream(output, true, true, true));
    return output.GetComposedChunks();
}
} // namespace

// ---- Null-handle array accessors ---------------------------------------------------
TEST_CASE(c_api_null_array_accessors)
{
    CHECK_EQ(MpGetArraySize(nullptr), 0u);
    MpReleaseArray(nullptr); // must be a safe no-op
    CHECK_EQ(MpGetIdFromArray(nullptr, nullptr, 0), 0u);
    CHECK_EQ(MpGetObjSize(nullptr), 0u);
    CHECK(MpGetU64SampleFromArray(nullptr, nullptr, 0) == nullptr);
    CHECK(MpGetCounterSampleFromArray(nullptr, nullptr, 0) == nullptr);
    CHECK(MpGetLastCounterSampleFromArray(nullptr, nullptr) == nullptr);
    CHECK(MpGetCounterSampleFromArrayByFrameId(nullptr, nullptr, 0) == nullptr);
    CHECK_EQ(MpGetTrackCount(nullptr), 0u);
    CHECK(MpGetTracks(nullptr) == nullptr);
    CHECK(MpGetTrackDescFromArray(nullptr, nullptr, 0) == nullptr);
    CHECK_EQ(MpGetSegmentCount(nullptr, 0), 0u);
    CHECK(MpGetSegments(nullptr, 0) == nullptr);
    CHECK(MpGetSegmentDescFromArray(nullptr, nullptr, 0) == nullptr);
    CHECK(!MpSelectTrack(nullptr, 0));
    CHECK(!MpSelectSegment(nullptr, 0));
}

TEST_CASE(c_api_sample_accessors_require_session)
{
    // An array handle is normally created by a session, but the exported accessors must
    // still reject a missing session before touching its scratch objects.
    CHECK(MpGetU64SampleFromArray(nullptr, reinterpret_cast<MpArray*>(1), 0) == nullptr);
    CHECK(MpGetCounterSampleFromArray(nullptr, reinterpret_cast<MpArray*>(1), 0) == nullptr);
    CHECK(MpGetLastCounterSampleFromArray(nullptr, reinterpret_cast<MpArray*>(1)) == nullptr);
    CHECK(MpGetCounterSampleFromArrayByFrameId(
        nullptr, reinterpret_cast<MpArray*>(1), 0) == nullptr);
}

TEST_CASE(c_api_close_null_session)
{
    CHECK(MpOpenFromMem(nullptr, 1) == nullptr);
    CHECK(MpOpenFromMemTrusted(nullptr, 1) == nullptr);
    CHECK(MpOpenFromFileTrusted(nullptr) == nullptr);
    CHECK(MpOpenFromDataContextTrusted({}, 0) == nullptr);
    CHECK(!MpCloseSession(nullptr));
}

TEST_CASE(c_api_null_session_contract)
{
    CHECK(!MpSyncWithDataSource(nullptr));
    CHECK(!MpSyncIfRequired(nullptr));
    MpSetSyncRequired(nullptr);
    MpSetClearCacheOnSyncRequired(nullptr);
    MpSetSessionNotificationCallback(nullptr, nullptr, nullptr);
    MpSetClearObjCacheCallback(nullptr, nullptr);
    MpEnableObjectCache(nullptr, 1, 1, 1, 1);
    MpClearObjectCache(nullptr);
    MpEnableLabelCache(nullptr, 1);
    MpClearLabelCache(nullptr);
    uint32_t label_length = 123;
    CHECK(MpGetLabelFromOffset(nullptr, nullptr, &label_length) == nullptr);
    CHECK(MpGetLabelFromOffsetParts(nullptr, 0, 0, &label_length) == nullptr);
    CHECK_EQ(label_length, 0u);
    label_length = 123;
    CHECK(MpGetLabelFromEntryParts(
        nullptr, 0, static_cast<uint32_t>(MP_LOG_LABEL << 29),
        &label_length) == nullptr);
    CHECK_EQ(label_length, 0u);
    CHECK(!MpIsSessionValid(nullptr));
    CHECK_EQ(MpGetSessionDataFormatVersion(nullptr), 0u);
    CHECK(MpGetContainerFromSession(nullptr) == nullptr);
    CHECK(!MpPushDataLock(nullptr));
    CHECK(!MpConsumeDataLock(nullptr));
    CHECK(!MpPopDataLock(nullptr));

    CHECK_EQ(MpFindGroupId(nullptr, "*"), 0u);
    CHECK(MpFindGroupIds(nullptr, "*") == nullptr);
    CHECK_EQ(MpFindTimerId(nullptr, "*"), 0u);
    CHECK(MpFindTimerIds(nullptr, "*") == nullptr);
    CHECK_EQ(MpFindCounterId(nullptr, "*"), 0u);
    CHECK(MpFindCounterIds(nullptr, "*") == nullptr);
    CHECK_EQ(MpFindThreadId(nullptr, "*"), 0u);
    CHECK(MpFindThreadIds(nullptr, "*") == nullptr);
    CHECK(MpGetTimerIdsByGroupId(nullptr, 1) == nullptr);
    CHECK(MpGetTimerIdsByGroupNames(nullptr, "*") == nullptr);

    CHECK(MpGetGlobalDesc(nullptr) == nullptr);
    CHECK(MpGetGeneralInfo(nullptr) == nullptr);
    CHECK(MpGetGroupDesc(nullptr, 1) == nullptr);
    CHECK(MpGetTimerDesc(nullptr, 1) == nullptr);
    CHECK(MpGetCounterDesc(nullptr, 1) == nullptr);
    CHECK(MpGetThreadDesc(nullptr, 1) == nullptr);
    CHECK(MpGetFrameDesc(nullptr, 1) == nullptr);
    CHECK(MpGetLastCounterSample(nullptr, 1) == nullptr);
    CHECK_EQ(MpGetGroupIdMax(nullptr), 0u);
    CHECK_EQ(MpGetThreadIdGpu(nullptr), 0u);
    CHECK_EQ(MpGetFrameIdMin(nullptr), 0u);
    CHECK_EQ(MpGetFrameIdMax(nullptr), 0u);
    CHECK(MpGetTimerIds(nullptr) == nullptr);
    CHECK(MpGetCounterIds(nullptr) == nullptr);
    CHECK(MpGetThreadIds(nullptr) == nullptr);
    CHECK(MpGetCounterSamples(nullptr, 1) == nullptr);
    CHECK(MpGetPlaceIdSamples(nullptr) == nullptr);
    CHECK(MpGetUtcTimestampSamples(nullptr) == nullptr);
}

TEST_CASE(c_api_null_iterator_contract)
{
    CHECK(MpCreateLogIterator(nullptr) == nullptr);
    MpDeleteLogIterator(nullptr);
    CHECK(!MpStepLogIterator(nullptr));
    MpRewindLogIterator(nullptr);
    MpRewindLogIteratorTo(nullptr, 1, 2);
    CHECK(!MpConfigureLogIterator(nullptr, true));
    CHECK(MpGetLogIteratorConfigBuffer(nullptr, 1) == nullptr);
    CHECK(MpGetLogIteratorState(nullptr) == nullptr);
    CHECK(MpGetLogIteratorThreadStackState(nullptr, 1) == nullptr);
    CHECK(MpGetLogIteratorThreadStackElement(nullptr, 1, 0) == nullptr);
    CHECK(MpGetLogIteratorCurrentThreadStackElement(nullptr, 0) == nullptr);
    CHECK_EQ(MpGetLogIteratorThreadStackElementStride(nullptr), 0u);
    CHECK(MpGetLogIteratorCurrentEntry(nullptr) == nullptr);
    CHECK_EQ(MpGetLogIteratorCurrentEntryCount(nullptr), 0u);
    CHECK(MpGetLogIteratorCurrentEntryAt(nullptr, 0) == nullptr);
    CHECK_EQ(MpGetLogIteratorCurrentEntryPart(nullptr, 0, 0), 0u);
    CHECK_EQ(MpGetLogIteratorCurrentEntryPart(nullptr, 0, 1), 0u);
    CHECK_EQ(MpGetLogIteratorCurrentEntryPart(nullptr, 0, 2), 0u);
    uint32_t event_type = 1;
    uint32_t event_size = 1;
    uint32_t has_comment = 1;
    CHECK(MpGetLogIteratorCurrentEventInfo(
        nullptr, &event_type, &event_size, &has_comment) == nullptr);
    CHECK_EQ(event_type, 0u);
    CHECK_EQ(event_size, 0u);
    CHECK_EQ(has_comment, 0u);
    uint32_t label_length = 123;
    CHECK(MpGetLogIteratorCurrentLabel(nullptr, &label_length) == nullptr);
    CHECK_EQ(label_length, 0u);
    CHECK(MpGetLogIteratorCurrentLabelOffset(nullptr) == nullptr);
    CHECK_EQ(MpGetLogIteratorScopeLabelCount(nullptr, 1, 0), 0u);
    CHECK_EQ(MpGetLogIteratorCurrentScopeLabelCount(nullptr), 0u);
    CHECK(MpGetLogIteratorScopeLabelOffset(nullptr, 1, 0, 0) == nullptr);
    CHECK(MpGetLogIteratorCurrentScopeLabelOffset(nullptr, 0) == nullptr);

    CHECK(MpCreateCounterIterator(nullptr) == nullptr);
    MpDeleteCounterIterator(nullptr);
    CHECK(!MpStepCounterIterator(nullptr));
    MpRewindCounterIterator(nullptr);
    CHECK(!MpConfigureCounterIterator(nullptr, true));
    CHECK(MpGetCounterIteratorConfigBuffer(nullptr, 1) == nullptr);
    CHECK(MpGetCounterIteratorState(nullptr) == nullptr);
    mpsize_t stack_size = 123;
    CHECK(MpGetCounterIteratorStack(nullptr, &stack_size) == nullptr);
    CHECK_EQ(stack_size, 0u);
    CHECK_EQ(MpGetCounterIteratorStackElement(nullptr, 0), 0u);
}

TEST_CASE(c_api_label_lookup_and_scope_collection)
{
    MpInit();
    ntd::vector<uint8_t> data = build_label_capture();
    CHECK(!data.empty());
    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    const uint64_t invalidOffset = 0;
    CHECK(MpGetLabelFromOffset(session, &invalidOffset, nullptr) == nullptr);
    CHECK(MpGetLabelFromOffsetParts(session, 0, 0, nullptr) == nullptr);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(
        session, 100, 0, nullptr)), "alpha"), 0);
    const uint64_t alphaOffset = 100;
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffset(
        session, &alphaOffset, nullptr)), "alpha"), 0);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(
        session, 106, 0, nullptr)), "beta"), 0);

    MpEnableLabelCache(session, 1);
    uint32_t alpha_length = 0;
    const uint8_t* alpha = MpGetLabelFromOffsetParts(session, 100, 0, &alpha_length);
    CHECK(alpha != nullptr);
    CHECK_EQ(alpha_length, 5u);
    alpha_length = 0;
    CHECK(alpha == MpGetLabelFromOffsetParts(session, 100, 0, &alpha_length));
    CHECK_EQ(alpha_length, 5u);
    uint32_t beta_length = 0;
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(
        session, 106, 0, &beta_length)), "beta"), 0);
    CHECK_EQ(beta_length, 4u);
    CHECK_EQ(std::strcmp(label_text(alpha), "alpha"), 0);
    MpClearLabelCache(session);

    MpLogIterator* iterator = MpCreateLogIterator(session);
    CHECK(iterator != nullptr);
    LibMP::FbBuilder builder;
    LibMP::TypesNtd::LogIteratorConfigT config;
    config.start_frame_id = 1;
    config.end_frame_id = 1;
    config.skip_frame_boundaries = true;
    config.track_scope_labels = true;
    auto root = LibMP::TypesNtd::LogIteratorConfig::Pack(builder, &config);
    builder.Finish(root);
    CHECK(MpGetLogIteratorConfigBuffer(iterator, builder.GetSize()) != nullptr);
    std::memcpy(
        MpGetLogIteratorConfigBuffer(iterator, builder.GetSize()),
        builder.GetBufferPointer(), builder.GetSize());
    CHECK(MpConfigureLogIterator(iterator, false));

    uint32_t label_steps = 0;
    uint32_t exit_steps = 0;
    uint32_t stored_label_entry_low = 0;
    uint32_t stored_label_entry_high = 0;
    while (MpStepLogIterator(iterator))
    {
        const auto* state = reinterpret_cast<const LibMP::TypesNtd::LogIteratorState*>(
            MpGetLogIteratorState(iterator));
        CHECK(state != nullptr);
        if (state && state->is_exit())
        {
            ++exit_steps;
            CHECK_EQ(MpGetLogIteratorCurrentScopeLabelCount(iterator), 2u);
            const uint64_t* labelOffset =
                MpGetLogIteratorCurrentScopeLabelOffset(iterator, 0);
            CHECK(labelOffset != nullptr);
            CHECK_EQ(labelOffset ? *labelOffset : 0, uint64_t{100});
        }
        if (!state || !state->is_label())
            continue;

        CHECK_EQ(state->timer_id(), 0u);
        label_steps++;
        CHECK_EQ(MpGetLogIteratorCurrentScopeLabelCount(iterator), label_steps);
        CHECK_EQ(MpGetLogIteratorScopeLabelCount(iterator, 7, 0), label_steps);
        if (label_steps == 1)
        {
            const uint8_t* element0 = MpGetLogIteratorThreadStackElement(iterator, 7, 0);
            const uint8_t* element1 = MpGetLogIteratorThreadStackElement(iterator, 7, 1);
            const mpsize_t stride = MpGetLogIteratorThreadStackElementStride(iterator);
            CHECK(element0 != nullptr);
            CHECK(element1 != nullptr);
            CHECK(stride >= sizeof(LibMP::TypesNtd::LogIteratorThreadStackElement));
            if (element0 && element1)
                CHECK_EQ(reinterpret_cast<uintptr_t>(element1) -
                    reinterpret_cast<uintptr_t>(element0), stride);
        }
        const uint32_t entry_low = MpGetLogIteratorCurrentEntryPart(iterator, 0, 0);
        const uint32_t entry_high = MpGetLogIteratorCurrentEntryPart(iterator, 0, 1);
        if (label_steps == 1)
        {
            stored_label_entry_low = entry_low;
            stored_label_entry_high = entry_high;
            uint32_t entry_label_length = 0;
            CHECK_EQ(std::strcmp(label_text(MpGetLabelFromEntryParts(
                session, entry_low, entry_high, &entry_label_length)), "alpha"), 0);
            CHECK_EQ(entry_label_length, 5u);
            const uint64_t* entry = MpGetLogIteratorCurrentEntry(iterator);
            CHECK_EQ(std::strcmp(label_text(MpGetLabelFromEntry(
                session, entry, nullptr)), "alpha"), 0);
            uint32_t iterator_label_length = 0;
            CHECK_EQ(std::strcmp(label_text(MpGetLogIteratorCurrentLabel(
                iterator, &iterator_label_length)), "alpha"), 0);
            CHECK_EQ(iterator_label_length, 5u);
            const uint64_t* currentOffset = MpGetLogIteratorCurrentLabelOffset(iterator);
            CHECK(currentOffset != nullptr);
            CHECK_EQ(currentOffset ? *currentOffset : 0, uint64_t{100});
            const uint64_t* currentScopeOffset =
                MpGetLogIteratorCurrentScopeLabelOffset(iterator, 0);
            CHECK(currentScopeOffset != nullptr);
            CHECK_EQ(currentScopeOffset ? *currentScopeOffset : 0, uint64_t{100});
            const uint64_t* scopeOffset =
                MpGetLogIteratorScopeLabelOffset(iterator, 7, 0, 0);
            CHECK(scopeOffset != nullptr);
            CHECK_EQ(scopeOffset ? *scopeOffset : 0, uint64_t{100});
        }
        else
        {
            uint32_t entry_label_length = 123;
            CHECK(MpGetLabelFromEntryParts(
                session, entry_low, entry_high, &entry_label_length) == nullptr);
            CHECK_EQ(entry_label_length, 0u);
            uint32_t iterator_label_length = 123;
            CHECK(MpGetLogIteratorCurrentLabel(
                iterator, &iterator_label_length) == nullptr);
            CHECK_EQ(iterator_label_length, 0u);
            CHECK(MpGetLogIteratorCurrentLabelOffset(iterator) == nullptr);
            CHECK(MpGetLogIteratorCurrentScopeLabelOffset(iterator, 1) == nullptr);
        }
    }
    CHECK_EQ(label_steps, 2u);
    CHECK_EQ(exit_steps, 1u);
    MpDeleteLogIterator(iterator);
    uint32_t stored_label_length = 0;
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromEntryParts(
        session, stored_label_entry_low, stored_label_entry_high,
        &stored_label_length)), "alpha"), 0);
    CHECK_EQ(stored_label_length, 5u);
    CHECK(MpCloseSession(session));
}

TEST_CASE(c_api_scope_labels_skip_overflowed_stack)
{
    MpInit();
    ntd::vector<uint8_t> data = build_label_capture(
        true, false, false, false, 50);
    CHECK(!data.empty());
    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    MpLogIterator* iterator = MpCreateLogIterator(session);
    CHECK(iterator != nullptr);
    LibMP::FbBuilder builder;
    LibMP::TypesNtd::LogIteratorConfigT config;
    config.start_frame_id = 1;
    config.end_frame_id = 1;
    config.skip_frame_boundaries = true;
    config.track_scope_labels = true;
    auto root = LibMP::TypesNtd::LogIteratorConfig::Pack(builder, &config);
    builder.Finish(root);
    CHECK(MpGetLogIteratorConfigBuffer(iterator, builder.GetSize()) != nullptr);
    std::memcpy(
        MpGetLogIteratorConfigBuffer(iterator, builder.GetSize()),
        builder.GetBufferPointer(), builder.GetSize());
    CHECK(MpConfigureLogIterator(iterator, false));

    uint32_t label_steps = 0;
    while (MpStepLogIterator(iterator))
    {
        const auto* state = reinterpret_cast<const LibMP::TypesNtd::LogIteratorState*>(
            MpGetLogIteratorState(iterator));
        if (!state || !state->is_label())
            continue;

        ++label_steps;
        CHECK_EQ(MpGetLogIteratorCurrentScopeLabelCount(iterator), 0u);
        CHECK_EQ(MpGetLogIteratorScopeLabelCount(iterator, 7, 49), 0u);
    }
    CHECK_EQ(label_steps, 2u);

    MpDeleteLogIterator(iterator);
    CHECK(MpCloseSession(session));
}

TEST_CASE(c_api_label_region_lookup_chunks)
{
    MpInit();
    ntd::vector<uint8_t> data = build_label_capture(true, false, true);
    CHECK(!data.empty());
    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(session, 100, 0, nullptr)), "alpha"), 0);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(session, 1000, 0, nullptr)), "x"), 0);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(session, 2000, 0, nullptr)), "x"), 0);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(session, 3000, 0, nullptr)), "x"), 0);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(
        session, 64 * 1024 - 2, 0, nullptr)), "y"), 0);
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(
        session, 64 * 1024, 0, nullptr)), "z"), 0);
    GPRX::U64Split high_offset;
    high_offset.entry = HIGH_LABEL_OFFSET;
    CHECK_EQ(std::strcmp(label_text(MpGetLabelFromOffsetParts(
        session, high_offset.parts.low, high_offset.parts.high, nullptr)), "x"), 0);
    CHECK(MpCloseSession(session));
}

TEST_CASE(c_api_label_length_and_size_limit)
{
    MpInit();
    ntd::vector<uint8_t> data = build_label_capture(true, false, false, true);
    CHECK(!data.empty());
    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    uint32_t byte_length = 0;
    const uint8_t* largest = MpGetLabelFromOffsetParts(session, 100000, 0, &byte_length);
    CHECK(largest != nullptr);
    CHECK_EQ(byte_length, 1023u);
    if (largest)
    {
        CHECK_EQ(largest[0], 'a');
        CHECK_EQ(largest[1022], 'a');
        CHECK_EQ(largest[1023], '\0');
    }
    CHECK(MpGetLabelFromOffsetParts(session, 100000, 0, nullptr) != nullptr);

    byte_length = 123;
    CHECK(MpGetLabelFromOffsetParts(session, 200000, 0, &byte_length) == nullptr);
    CHECK_EQ(byte_length, 0u);

    CHECK(MpCloseSession(session));
}

TEST_CASE(c_api_label_cache_refreshes_after_sync)
{
    MpInit();
    ntd::vector<uint8_t> baseline = build_label_capture();
    ntd::vector<uint8_t> alphaEvicted = build_label_capture(false, true);
    CHECK(!baseline.empty());
    CHECK(!alphaEvicted.empty());

    ExternalCapture source{&baseline};
    GPRX::ExternalIO io = make_external_io();
    MpSession* session = MpOpenFromExternalProviderTrusted(
        &io, reinterpret_cast<uint8_t*>(&source), 0);
    CHECK(session != nullptr);
    if (!session)
        return;

    MpEnableLabelCache(session, 1);
    SessionNotifications notifications;
    MpSetSessionNotificationCallback(
        session, record_session_notifications,
        reinterpret_cast<uint8_t*>(&notifications));
    const uint8_t* alpha = MpGetLabelFromOffsetParts(session, 100, 0, nullptr);
    const uint8_t* beta = MpGetLabelFromOffsetParts(session, 106, 0, nullptr);
    CHECK(alpha != nullptr);
    CHECK(beta != nullptr);
    CHECK_EQ(std::strcmp(label_text(alpha), "alpha"), 0);
    CHECK_EQ(std::strcmp(label_text(beta), "beta"), 0);

    source.bytes = &alphaEvicted;
    MpSetSyncRequired(session);
    CHECK_EQ(notifications.calls, 1u);
    CHECK_EQ(notifications.flags,
        uint32_t{MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED});

    // Repeated invalidation requests and the subsequent sync must not produce
    // duplicate notifications for the same pending change.
    MpSetSyncRequired(session);
    CHECK_EQ(notifications.calls, 1u);
    CHECK(MpSyncIfRequired(session));
    CHECK_EQ(notifications.calls, 1u);

    CHECK(MpGetLabelFromOffsetParts(session, 100, 0, nullptr) == nullptr);

    // The changed range only partially covers this chunk, so the chunk cache is
    // discarded wholesale. Beta remains available and is re-read on demand.
    const uint8_t* betaAfterSync = MpGetLabelFromOffsetParts(session, 106, 0, nullptr);
    CHECK(betaAfterSync != nullptr);
    if (betaAfterSync)
    {
        CHECK_EQ(std::strcmp(label_text(betaAfterSync), "beta"), 0);
    }

    notifications = {};
    MpSetClearCacheOnSyncRequired(session);
    MpSetSyncRequired(session);
    CHECK(MpSyncIfRequired(session));
    CHECK_EQ(notifications.calls, 2u);
    CHECK_EQ(notifications.flags,
        uint32_t{MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED |
            MP_SESSION_NOTIFICATION_LABEL_CACHE_RESET});

    CHECK(MpCloseSession(session));
}

TEST_CASE(cpp_api_exposes_high_level_labels)
{
    LibMP::Init();
    ntd::vector<uint8_t> data = build_label_capture();
    auto session = LibMP::Session::OpenFromMem(
        data.data(), GPRX::to_mpsize(data.size()));
    CHECK(!!session);
    if (!session)
        return;

    session->EnableLabelCache(true);

    auto iterator = session->CreateLogIterator();
    LibMP::DefaultFlavor::LogIteratorConfigT config;
    config.start_frame_id = 1;
    config.end_frame_id = 1;
    config.skip_frame_boundaries = true;
    config.track_scope_labels = true;
    iterator.Configure(config);

    uint32_t labelSteps = 0;
    uint32_t exitSteps = 0;
    while (iterator.Step())
    {
        const auto* state = iterator.GetState();
        if (state && state->is_exit())
        {
            ++exitSteps;
            CHECK_EQ(iterator.GetCurrentScopeLabelCount(), 2u);
            auto label = iterator.FetchCurrentScopeLabel(0);
            CHECK(label.has_value());
            if (label)
                CHECK_EQ(std::strcmp(label->c_str(), "alpha"), 0);
        }
        if (!state || !state->is_label())
            continue;
        ++labelSteps;
        CHECK_EQ(iterator.GetCurrentScopeLabelCount(), labelSteps);
        if (labelSteps == 1)
        {
            uint32_t labelLength = 0;
            CHECK_EQ(std::strcmp(
                iterator.GetCurrentLabel(&labelLength), "alpha"), 0);
            CHECK_EQ(labelLength, 5u);
            labelLength = 0;
            CHECK_EQ(std::strcmp(
                iterator.GetCurrentScopeLabel(0, &labelLength), "alpha"), 0);
            CHECK_EQ(labelLength, 5u);
            labelLength = 0;
            CHECK_EQ(std::strcmp(
                iterator.GetScopeLabel(7, 0, 0, &labelLength), "alpha"), 0);
            CHECK_EQ(labelLength, 5u);
            auto currentLabel = iterator.FetchCurrentLabel();
            CHECK(currentLabel.has_value());
            if (currentLabel)
                CHECK_EQ(std::strcmp(currentLabel->c_str(), "alpha"), 0);
            auto label = iterator.FetchCurrentScopeLabel(0);
            CHECK(label.has_value());
            if (label)
                CHECK_EQ(std::strcmp(label->c_str(), "alpha"), 0);
            auto explicitLabel = iterator.FetchScopeLabel(7, 0, 0);
            CHECK(explicitLabel.has_value());
            if (explicitLabel)
                CHECK_EQ(std::strcmp(explicitLabel->c_str(), "alpha"), 0);
        }
        else
        {
            uint32_t labelLength = 123;
            CHECK(iterator.GetCurrentLabel(&labelLength) == nullptr);
            CHECK_EQ(labelLength, 0u);
            CHECK(!iterator.FetchCurrentLabel().has_value());
        }
    }
    CHECK_EQ(labelSteps, 2u);
    CHECK_EQ(exitSteps, 1u);
    session->ClearLabelCache();
}

TEST_CASE(c_api_open_null_and_empty)
{
    MpInit();
    // Neither a null buffer nor an empty buffer may crash; any session returned closes.
    if (MpSession* s = MpOpenFromMem(nullptr, 0))
        CHECK(MpCloseSession(s));
    if (MpSession* s = MpOpenFromMem(reinterpret_cast<const uint8_t*>(""), 0))
        CHECK(MpCloseSession(s));
}

TEST_CASE(c_api_tracks_segments_and_selection)
{
    MpInit();
    ntd::vector<uint8_t> data = build_track_segment_container();
    CHECK(!data.empty());
    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    CHECK(MpIsSessionValid(session));
    CHECK_EQ(MpGetTrackCount(session), 2u);
    CHECK_EQ(MpGetSegmentCount(session, 0), 2u);
    CHECK_EQ(MpGetSegmentCount(session, 1), 1u);
    CHECK_EQ(MpGetSegmentCount(session, 2), 0u);

    MpArray* tracks = MpGetTracks(session);
    CHECK_EQ(MpGetArraySize(tracks), 2u);
    const auto* track0Ptr = reinterpret_cast<const LibMP::Types::TrackDesc*>(
        MpGetTrackDescFromArray(session, tracks, 0));
    CHECK(track0Ptr != nullptr);
    CHECK_EQ(MpGetObjSize(session), sizeof(LibMP::Types::TrackDesc));
    if (track0Ptr)
    {
        const LibMP::Types::TrackDesc track0 = *track0Ptr;
        CHECK_EQ(track0.timestamp_start(), 1000u);
        CHECK_EQ(track0.timestamp_stop(), 3000u);
        CHECK_EQ(track0.app_type(), LibMP::Enums::AppType::Client);
    }
    const auto* track1 = reinterpret_cast<const LibMP::Types::TrackDesc*>(
        MpGetTrackDescFromArray(session, tracks, 1));
    CHECK(track1 != nullptr);
    if (track1)
    {
        CHECK_EQ(track1->timestamp_start(), 5000u);
        CHECK_EQ(track1->timestamp_stop(), 6000u);
        CHECK_EQ(track1->app_type(), LibMP::Enums::AppType::Studio);
    }
    CHECK(MpGetTrackDescFromArray(session, tracks, 2) == nullptr);
    CHECK_EQ(MpGetObjSize(session), 0u);

    MpArray* segments0 = MpGetSegments(session, 0);
    CHECK_EQ(MpGetArraySize(segments0), 2u);
    const auto* segment0Ptr = reinterpret_cast<const LibMP::Types::SegmentDesc*>(
        MpGetSegmentDescFromArray(session, segments0, 0));
    CHECK(segment0Ptr != nullptr);
    CHECK_EQ(MpGetObjSize(session), sizeof(LibMP::Types::SegmentDesc));
    if (segment0Ptr)
    {
        const LibMP::Types::SegmentDesc segment0 = *segment0Ptr;
        CHECK_EQ(segment0.timestamp_start(), 1000u);
        CHECK_EQ(segment0.timestamp_stop(), 2000u);
        CHECK_EQ(segment0.frame_id_begin(), 10u);
        CHECK_EQ(segment0.frame_id_end(), 19u);
        CHECK_EQ(segment0.frame_absolute_id_begin(), 100u);
        CHECK_EQ(segment0.markers(), 0x8009u);
    }
    const auto* segment1 = reinterpret_cast<const LibMP::Types::SegmentDesc*>(
        MpGetSegmentDescFromArray(session, segments0, 1));
    CHECK(segment1 != nullptr);
    if (segment1)
    {
        CHECK_EQ(segment1->timestamp_start(), 2100u);
        CHECK_EQ(segment1->timestamp_stop(), 3000u);
        CHECK_EQ(segment1->frame_id_begin(), 20u);
        CHECK_EQ(segment1->frame_id_end(), 29u);
        CHECK_EQ(segment1->frame_absolute_id_begin(), 120u);
        CHECK_EQ(segment1->markers(), 0x0004u);
    }
    CHECK(MpGetSegmentDescFromArray(session, segments0, 2) == nullptr);
    CHECK_EQ(MpGetObjSize(session), 0u);
    CHECK(MpGetTrackDescFromArray(session, segments0, 0) == nullptr);
    CHECK(MpGetSegmentDescFromArray(session, tracks, 0) == nullptr);

    MpArray* segments1 = MpGetSegments(session, 1);
    CHECK_EQ(MpGetArraySize(segments1), 1u);
    CHECK(MpGetSegments(session, 2) == nullptr);

    // Selection must not reuse cached values from the previously selected segment.
    CacheClearNotifications notifications;
    SessionNotifications sessionNotifications;
    MpSetClearObjCacheCallback(session, record_cache_clear,
        reinterpret_cast<uint8_t*>(&notifications));
    MpSetSessionNotificationCallback(session, record_session_notifications,
        reinterpret_cast<uint8_t*>(&sessionNotifications));
    MpEnableObjectCache(session, 1, 1, 1, 1);
    CHECK_EQ(MpGetFrameIdMin(session), 10u);
    CHECK_EQ(MpGetFrameIdMax(session), 19u);
    CHECK(MpSelectTrack(session, 0));
    CHECK_EQ(notifications.calls, 0u);
    CHECK_EQ(sessionNotifications.calls, 0u);
    CHECK(MpSelectSegment(session, 1));
    CHECK_EQ(notifications.calls, 1u);
    CHECK_EQ(notifications.bucket_mask, 3u);
    CHECK_EQ(sessionNotifications.calls, 1u);
    CHECK_EQ(sessionNotifications.flags,
        uint32_t{MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED});
    CHECK_EQ(MpGetFrameIdMin(session), 20u);
    CHECK_EQ(MpGetFrameIdMax(session), 29u);
    CHECK(MpSelectSegment(session, 1));
    CHECK_EQ(notifications.calls, 1u);
    CHECK_EQ(sessionNotifications.calls, 1u);
    CHECK(MpSelectTrack(session, 1));
    CHECK_EQ(notifications.calls, 2u);
    CHECK_EQ(sessionNotifications.calls, 2u);
    CHECK_EQ(MpGetFrameIdMin(session), 100u);
    CHECK_EQ(MpGetFrameIdMax(session), 109u);

    CHECK(!MpSelectTrack(session, 2));
    CHECK(!MpSelectSegment(session, 1));
    CHECK_EQ(notifications.calls, 2u);
    CHECK_EQ(sessionNotifications.calls, 2u);
    CHECK_EQ(MpGetFrameIdMin(session), 100u);
    CHECK(MpSelectTrack(session, 0));
    CHECK_EQ(sessionNotifications.calls, 3u);
    CHECK_EQ(MpGetFrameIdMin(session), 10u);

    MpReleaseArray(segments1);
    MpReleaseArray(segments0);
    MpReleaseArray(tracks);
    CHECK(MpCloseSession(session));
}

TEST_CASE(c_api_log_iterator_resumes_scope_stack_across_contiguous_segments)
{
    MpInit();
    ntd::vector<uint8_t> data = build_contiguous_segment_log_container();
    CHECK(!data.empty());
    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    CHECK_EQ(MpGetSegmentCount(session, 0), 2u);
    MpLogIterator* iterator = MpCreateLogIterator(session);
    CHECK(iterator != nullptr);
    if (!iterator)
    {
        CHECK(MpCloseSession(session));
        return;
    }

    LibMP::FbBuilder builder;
    LibMP::TypesNtd::LogIteratorConfigT config;
    config.start_frame_id = 10;
    config.end_frame_id = 10;
    config.skip_frame_boundaries = true;
    auto root = LibMP::TypesNtd::LogIteratorConfig::Pack(builder, &config);
    builder.Finish(root);
    uint8_t* configBuffer = MpGetLogIteratorConfigBuffer(iterator, builder.GetSize());
    CHECK(configBuffer != nullptr);
    if (configBuffer)
        std::memcpy(configBuffer, builder.GetBufferPointer(), builder.GetSize());
    CHECK(MpConfigureLogIterator(iterator, false));

    uint32_t enterCount = 0;
    while (MpStepLogIterator(iterator))
    {
        const auto* state = reinterpret_cast<const LibMP::TypesNtd::LogIteratorState*>(
            MpGetLogIteratorState(iterator));
        if (state && state->is_enter())
        {
            ++enterCount;
            CHECK_EQ(state->frame_id(), 10u);
            CHECK_EQ(state->thread_stack_depth(), 1u);
        }
    }
    CHECK_EQ(enterCount, 1u);

    CHECK(MpSelectSegment(session, 1));
    MpRewindLogIteratorTo(iterator, 11, 11);

    uint32_t exitCount = 0;
    while (MpStepLogIterator(iterator))
    {
        const auto* state = reinterpret_cast<const LibMP::TypesNtd::LogIteratorState*>(
            MpGetLogIteratorState(iterator));
        if (state && state->is_exit())
        {
            ++exitCount;
            CHECK_EQ(state->frame_id(), 11u);
            CHECK_EQ(state->frame_absolute_id(), 101u);
            CHECK_EQ(state->thread_stack_depth(), 0u);
            CHECK(!state->thread_stack_is_underflowed());
            CHECK_EQ(state->thread_prev_timestamp(), 150u);

            const auto* scope = reinterpret_cast<
                const LibMP::TypesNtd::LogIteratorThreadStackElement*>(
                    MpGetLogIteratorCurrentThreadStackElement(iterator, 0));
            CHECK(scope != nullptr);
            if (scope)
            {
                CHECK_EQ(scope->enter_frame_id(), 10u);
                CHECK_EQ(scope->enter_timer_id(), 1u);
            }
        }
    }
    CHECK_EQ(exitCount, 1u);

    MpDeleteLogIterator(iterator);
    CHECK(MpCloseSession(session));
}

TEST_CASE(c_api_segment_without_track_uses_implicit_unknown_track)
{
    MpInit();
    GPRX::ContainerContext container;
    container.AddBlock(1, true);
    container.InitMagicBlock(container.GetBlocks().back());
    add_boundary_block(container, 2, LibMP::BlockTypeEx::SegmentStart,
        (uint64_t{700} << 32) | 70, 7000);
    add_boundary_block(container, 3, LibMP::BlockTypeEx::SegmentFinish,
        (uint64_t{1} << 32) | 79, 8000);
    ntd::vector<uint8_t> data = container.SaveToMemory(true);

    MpSession* session = MpOpenFromMem(data.data(), GPRX::to_mpsize(data.size()));
    CHECK(session != nullptr);
    if (!session)
        return;

    CHECK_EQ(MpGetTrackCount(session), 1u);
    CHECK_EQ(MpGetSegmentCount(session, 0), 1u);
    MpArray* tracks = MpGetTracks(session);
    const auto* track = reinterpret_cast<const LibMP::Types::TrackDesc*>(
        MpGetTrackDescFromArray(session, tracks, 0));
    CHECK(track != nullptr);
    if (track)
    {
        CHECK_EQ(track->timestamp_start(), 7000u);
        CHECK_EQ(track->timestamp_stop(), 8000u);
        CHECK_EQ(track->app_type(), LibMP::Enums::AppType::Unknown);
    }
    MpReleaseArray(tracks);
    CHECK(MpCloseSession(session));
}

TEST_CASE(cpp_api_tracks_segments_and_selection)
{
    LibMP::Init();
    ntd::vector<uint8_t> data = build_track_segment_container();
    auto session = LibMP::Session::OpenFromMem(
        data.data(), GPRX::to_mpsize(data.size()));
    CHECK(!!session);
    if (!session)
        return;

    CHECK_EQ(session->GetTrackCount(), 2u);
    CHECK_EQ(session->GetSegmentCount(0), 2u);
    CHECK_EQ(session->GetSegmentCount(1), 1u);
    CHECK_EQ(session->GetSegmentCount(2), 0u);

    auto tracks = session->GetTracks();
    CHECK_EQ(tracks.Size(), 2u);
    const auto* track0 = tracks.Get(0);
    CHECK(track0 != nullptr);
    if (track0)
    {
        CHECK_EQ(track0->timestamp_start(), 1000u);
        CHECK_EQ(track0->timestamp_stop(), 3000u);
        CHECK_EQ(track0->app_type(), LibMP::Enums::AppType::Client);
    }
    auto fetchedTracks = session->FetchTracks();
    CHECK_EQ(fetchedTracks.size(), 2u);
    if (fetchedTracks.size() == 2)
        CHECK_EQ(fetchedTracks[1].app_type(), LibMP::Enums::AppType::Studio);

    auto segments = session->GetSegments(0);
    CHECK_EQ(segments.Size(), 2u);
    const auto* segment0 = segments.Get(0);
    CHECK(segment0 != nullptr);
    if (segment0)
    {
        CHECK_EQ(segment0->frame_id_begin(), 10u);
        CHECK_EQ(segment0->frame_id_end(), 19u);
        CHECK_EQ(segment0->markers(), 0x8009u);
    }
    auto fetchedSegments = session->FetchSegments(0);
    CHECK_EQ(fetchedSegments.size(), 2u);
    if (fetchedSegments.size() == 2)
        CHECK_EQ(fetchedSegments[1].frame_absolute_id_begin(), 120u);

    CHECK(session->SelectSegment(1));
    CHECK_EQ(session->GetFrameIdMin(), 20u);
    CHECK(session->SelectTrack(1));
    CHECK_EQ(session->GetFrameIdMin(), 100u);
    CHECK(!session->SelectTrack(2));
    CHECK(!session->SelectSegment(1));
}

// An invalid container must not yield a session that crashes when read or iterated.
TEST_CASE(c_api_invalid_container_is_safe)
{
    MpInit();
    static const uint8_t junk[64] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    };
    MpSession* s = MpOpenFromMem(junk, sizeof(junk));
    if (!s)
        return; // rejecting the container outright is also acceptable

    CHECK(!MpIsSessionValid(s));
    CHECK(MpGetGlobalDesc(s) == nullptr);
    CHECK(MpGetGeneralInfo(s) == nullptr);
    CHECK(MpGetFrameDesc(s, 1) == nullptr);
    CHECK(MpGetThreadDesc(s, 1) == nullptr);
    CHECK(MpGetCounterDesc(s, 1) == nullptr);

    MpArray* counters = MpGetCounterIds(s);
    CHECK_EQ(MpGetArraySize(counters), 0u);          // null-or-empty either way
    CHECK_EQ(MpGetIdFromArray(s, counters, 999), 0u); // out-of-range index is safe
    MpReleaseArray(counters);

    // Configuring and stepping iterators on a counter-less session must be safe.
    if (MpCounterIterator* cit = MpCreateCounterIterator(s))
    {
        MpConfigureCounterIterator(cit, true);
        MpRewindCounterIterator(cit);
        int steps = 0;
        while (MpStepCounterIterator(cit) && steps++ < 1000) {}
        MpDeleteCounterIterator(cit);
    }
    if (MpLogIterator* it = MpCreateLogIterator(s))
    {
        MpConfigureLogIterator(it, true);
        MpRewindLogIterator(it);
        int steps = 0;
        while (MpStepLogIterator(it) && steps++ < 1000) {}
        MpDeleteLogIterator(it);
    }

    CHECK(MpCloseSession(s));
}

TEST_CASE(c_api_rejects_oversized_decoded_payload)
{
    MpInit();
    // Minimized fuzzer input: an empty Base64 payload claims a 2.18 GB
    // uncompressed size and must be rejected before allocating a decoded buffer.
    static const uint8_t input[GPRX::BlockHeaderRawSize] = {
        0xff, 0xff, 0xff, 0xff, 0x41, 0x30, 0x42, 0x46,
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2e, 0x04, 0x00, 0x01, 0xff, 0x00, 0x53, 0x58,
        0x00, 0x1a, 0xdb, 0x81, 0x00, 0x00, 0x00, 0x00,
    };

    libmp_test::ScopedExpectedDiagnostic expected(
        "ContainerContext::ScanSequential - decoded payload exceeds the per-block limit");
    MpSession* session = MpOpenFromMem(input, sizeof(input));
    CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(1));
    if (!session)
        return;
    CHECK(!MpIsSessionValid(session));
    CHECK(MpGetGlobalDesc(session) == nullptr);
    CHECK(MpCloseSession(session));
}

// Find* with an empty/oversized mask returns the invalid-id sentinel / a safe empty array.
TEST_CASE(c_api_find_edge_masks)
{
    MpInit();
    static const uint8_t junk[32] = { 0 };
    MpSession* s = MpOpenFromMem(junk, sizeof(junk));
    if (!s)
        return;
    CHECK_EQ(MpFindCounterId(s, "", 0), 0u);
    CHECK_EQ(MpFindTimerId(s, "**/anything", 0), 0u);
    MpArray* ids = MpFindCounterIds(s, "", 0);
    CHECK_EQ(MpGetArraySize(ids), 0u); // non-crashing; empty on no match
    MpReleaseArray(ids);

    char oversized_mask[4097];
    std::memset(oversized_mask, 'a', sizeof(oversized_mask));
    oversized_mask[sizeof(oversized_mask) - 1] = '\0';
    {
        libmp_test::ScopedExpectedDiagnostic expected(
            "PatternMatcher - mask must be null-terminated within MAX_MASK_LEN_ON_STACK");
        CHECK_EQ(MpFindCounterId(s, oversized_mask, 0), 0u);
        CHECK_EQ(MpFindCounterId(s, oversized_mask, 1), 0u);
        CHECK_EQ(MpFindTimerId(s, oversized_mask, 0), 0u);
        CHECK_EQ(MpFindTimerId(s, oversized_mask, 1), 0u);
        CHECK_EQ(expected.Hits(), libmp_test::ExpectedDiagnosticHits(4));
    }

    MpCloseSession(s);
}

LIBMP_TEST_MAIN("c_api")

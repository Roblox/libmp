// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

// roundtrip_capture_builder.h -- shared: rebuilds a real capture from
// roundtrip_capture_data.inc via LibMP::LiveContext. Used by
// capture_roundtrip_tests.cpp and src/tests/tools/parity_tool.cpp.

#include "mp_api/capture_builder.h" // LibMP::LiveContext, LibMP::TypesNtd, LibMP::QueryEx
#include "mp_api/cpp_api.h"         // LibMP::Session (public reader)
#include "mp_api/log_entry.h"

#include <cstring>
#include <vector>

#include "roundtrip_capture_data.inc"

namespace
{
namespace rd = roundtrip_data;

template <class T, size_t N>
constexpr size_t countof(const T (&)[N])
{
    return N;
}

// Rebuilds the capture from the extracted data and serializes it to a memory buffer,
// following the initial-fill sequence used for a live capture.
//   maxFramesPerKv     -- LiveContext KV window; a value < frame count forces block rotation.
//   writeCounterSamples-- write per-frame counter samples (disabled under rotation, where the
//                         counter KV holds fewer samples per line and samples are not asserted).
ntd::vector<uint8_t> BuildCaptureBuffer(
    uint32_t maxFramesPerKv,
    bool writeCounterSamples,
    const rd::CounterRow* counterRows = rd::kCounters,
    size_t counterCount = countof(rd::kCounters),
    bool rotateShortTermBlocks = false,
    bool rotateLongTermBlocks = false,
    const char* platformInfoJsonOverride = nullptr,
    const char* firstTimerNameOverride = nullptr)
{
    LibMP::LiveContext live;
    live.config.max_frames_per_kv = maxFramesPerKv;
    live.Init();

    const uint32_t numFrames = rd::kNumFramesExtracted;
    bool longTermChanged = true;

    for (uint32_t i = 0; i < numFrames; ++i)
    {
        longTermChanged = live.StartAddingFrame(longTermChanged, true);
        live.SelectKV(live.kvframes1_block, live.frames1_block);

        ntd::vector<LibMP::TypesNtd::ThreadLogDescr> frameThreadLogs;
        const auto& fr = rd::kFrames[i];

        // The generated fixture preserves each source ThreadLogDescr and its exact raw entry
        // span. Static fixture storage outlives the container's external payload references.
        if (fr.thread_log_offset > countof(rd::kThreadLogs)
            || fr.thread_log_count > countof(rd::kThreadLogs) - fr.thread_log_offset)
            return {};
        for (uint32_t logIndex = 0; logIndex < fr.thread_log_count; ++logIndex)
        {
            const auto& row = rd::kThreadLogs[fr.thread_log_offset + logIndex];
            if (row.entry_offset > countof(rd::kThreadLogEntries)
                || row.entry_count > countof(rd::kThreadLogEntries) - row.entry_offset
                || row.entry_count > GPRX::max_mpsize_t / sizeof(uint64_t))
                return {};
            GPRX::Locator locator = {};
            if (row.entry_count)
            {
                locator = GPRX::BuildDataLocator(
                    live.logs1_block.GetBlockId(),
                    static_cast<uint32_t>(live.logs1_block.ptr->virtualStartOffset
                        + live.logs1_block.ptr->headerPtr->payload_uncompressed_size));
                const uint64_t* entries = rd::kThreadLogEntries + row.entry_offset;
                if (!live.logs1_block.ptr->AddExternalPayloadChunk(
                        const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(entries)),
                        static_cast<mpsize_t>(row.entry_count * sizeof(uint64_t))))
                    return {};
            }
            frameThreadLogs.emplace_back(
                row.thread_id, row.entry_count, locator.entry, row.is_removed);
        }

        const uint32_t frameId = live.frames_counter;
        live.BuildTable([&](auto& b) {
            return LibMP::TypesNtd::CreateFrameDescDirect(
                b, frameId, fr.absolute_id,
                fr.tsc_start, fr.tsc_end, fr.tsg_start, fr.tsg_end,
                fr.label_offset_min, fr.label_offset_max, fr.paused, fr.incomplete, &frameThreadLogs);
        });
        live.SetUid(LibMP::QueryEx::FrameDesc_u24FrameId, true, frameId);
        live.PutTable();

        live.FinishAddingFrame();
    }

    live.bInitialFillingCompleted = true;
    // Computes live.frameIdMin / live.frameIdMax (and trims evicted log data). Passing a small
    // firstFrameAbsId keeps capacity-based eviction as the only trimming criterion.
    live.TrimLogs(live.frames_counter, 0);

    // Related snapshots are rebuilt independently, so explicitly reproduce the
    // block-ID changes that a live metadata refresh would make. Rotate short term
    // first in every later stage so its IDs remain stable when long term changes.
    if (rotateShortTermBlocks)
    {
        live.ClearAndIncBlockId(live.shortterm_block);
        live.IncBlockId(live.kvshortterm_block);
        live.ResetKvStore(live.kvshortterm_block);
    }
    if (rotateLongTermBlocks)
    {
        live.ClearAndIncBlockId(live.longterm_block);
        live.IncBlockId(live.kvlongterm_block);
        live.ResetKvStore(live.kvlongterm_block);
    }
    if (rotateShortTermBlocks || rotateLongTermBlocks)
        live.live_container->RebuildDirectLookupTable();

    // ---- Long-term descriptors ----
    live.SelectKV(live.kvlongterm_block, live.longterm_block);

    for (size_t g = 0; g < countof(rd::kGroups); ++g)
    {
        const auto& row = rd::kGroups[g];
        live.BuildTable([&](auto& b) {
            return LibMP::TypesNtd::CreateGroupDescDirect(b, row.id, row.name, row.color, row.is_gpu);
        });
        live.SetUid(LibMP::QueryEx::GroupDesc_u24GroupId, true, row.id);
        live.PutTable();
    }
    live.SetUid(LibMP::QueryEx::GroupIdMax);
    live.PutU32(rd::kGroupIdMax);

    for (size_t t = 0; t < countof(rd::kTimers); ++t)
    {
        const auto& row = rd::kTimers[t];
        const char* timerName = t == 0 && firstTimerNameOverride
            ? firstTimerNameOverride : row.name;
        live.BuildTable([&](auto& b) {
            return LibMP::TypesNtd::CreateTimerDescDirect(
                b, row.id, timerName, row.group_id, row.color, row.is_user_timer);
        });
        live.SetUid(LibMP::QueryEx::TimerDesc_u24TimerId, true, row.id);
        live.PutTable();
        live.SetUid(LibMP::QueryEx::TimerIds);
        live.PutArrayU32(row.id);
    }

    for (size_t c = 0; c < counterCount; ++c)
    {
        const auto& row = counterRows[c];
        live.BuildTable([&](auto& b) {
            return LibMP::TypesNtd::CreateCounterDescDirect(
                b, row.id, row.name, row.parent, row.sibling, row.first_child, static_cast<uint8_t>(row.level));
        });
        live.SetUid(LibMP::QueryEx::CounterDesc_u24CounterId, true, row.id);
        live.PutTable();
        live.SetUid(LibMP::QueryEx::CounterIds);
        live.PutArrayU32(row.id);
    }

    for (size_t th = 0; th < countof(rd::kThreads); ++th)
    {
        const auto& row = rd::kThreads[th];
        live.BuildTable([&](auto& b) {
            return LibMP::TypesNtd::CreateThreadDescDirect(b, row.id, row.name, row.buffer_size, row.is_gpu);
        });
        live.SetUid(LibMP::QueryEx::ThreadDesc_u24ThreadId, true, row.id);
        live.PutTable();
        live.SetUid(LibMP::QueryEx::ThreadIds);
        live.PutArrayU32(row.id);
        if (row.is_gpu)
        {
            live.SetUid(LibMP::QueryEx::ThreadIdGpu);
            live.PutU32(row.id);
        }
    }

    // ---- Short-term: frame range + global descriptor ----
    live.SelectKV(live.kvshortterm_block, live.shortterm_block);
    live.SetUid(LibMP::QueryEx::FrameIdMin);
    live.PutU32(live.frameIdMin);
    live.SetUid(LibMP::QueryEx::FrameIdMax);
    live.PutU32(live.frameIdMax);

    LibMP::TypesNtd::GlobalDesc gdesc(rd::kGlobalTickToMsCpu, rd::kGlobalTickToMsGpu);
    live.SetUid(LibMP::QueryEx::GlobalDesc);
    live.PutStruct(gdesc);

    if (rd::kHasGeneralInfo)
    {
        const char* platformInfoJson = platformInfoJsonOverride
            ? platformInfoJsonOverride
            : (rd::kHasPlatformInfoJson ? rd::kPlatformInfoJson : nullptr);
        live.BuildTable([&](auto& b) {
            return LibMP::TypesNtd::CreateGeneralInfoDirect(
                b, platformInfoJson);
        });
        live.SetUid(LibMP::QueryEx::GeneralInfo);
        live.PutTable();
    }

    for (const auto& row : rd::kPlaceIdSamples)
    {
        LibMP::TypesNtd::U64Sample sample(row.frame_index + 1, row.value);
        live.value_buf.FromU64Sample(sample);
        live.SetUid(LibMP::QueryEx::PlaceIdSamples);
        live.PutArrayValueBuf();
    }
    for (const auto& row : rd::kUtcTimestampSamples)
    {
        LibMP::TypesNtd::U64Sample sample(row.frame_index + 1, row.value);
        live.value_buf.FromU64Sample(sample);
        live.SetUid(LibMP::QueryEx::UtcTimestampSamples);
        live.PutArrayValueBuf();
    }

    // ---- Counter samples (per counter per frame) ----
    if (writeCounterSamples)
    {
        live.SelectKV(live.kvcounters1_block, live.kvcounters1_block);
        for (size_t k = 0; k < countof(rd::kCounterSamples); ++k)
        {
            const auto& row = rd::kCounterSamples[k];
            LibMP::TypesNtd::CounterSample sample(row.frame_index + 1, row.value);
            live.value_buf.FromCounterSample(sample);
            live.SetUid(LibMP::QueryEx::CounterSamples_u24CounterId, true, row.counter_id);
            live.PutArrayValueBuf();
        }
    }

    live.SetDataChanged();
    live.CheckExport();

    GPRX::StreamProcessorChunkedMemory mem;
    live.live_container->SaveToStream(mem, true, true, true);
    return mem.GetComposedChunks();
}

[[maybe_unused]] ntd::vector<uint8_t> BuildLabeledCaptureBuffer()
{
    static constexpr uint32_t kThreadId = 7;
    static constexpr uint32_t kTimerId = 1;
    static constexpr uint32_t kOverflowTimerId = 2;
    static constexpr uint64_t kLabelOffset = 100;
    static constexpr uint8_t kLabels[] = {
        'a', 'l', 'p', 'h', 'a', 0,
        'b', 'e', 't', 'a', 0,
        'g', 'a', 'm', 'm', 'a', 0,
    };
    static const uint64_t kLogEntries[] = {
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_ENTER, kTimerId, 10),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LABEL, kTimerId, kLabelOffset),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LEAVE, kTimerId, 20),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_ENTER, kTimerId, 30),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LABEL, kTimerId, kLabelOffset + 6),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LEAVE, kTimerId, 40),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_ENTER, kOverflowTimerId, 50),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LABEL, kOverflowTimerId, kLabelOffset + 11),
        LibMP::MicroProfilerLogMakeEntry(MP_LOG_LEAVE, kOverflowTimerId, 60),
    };

    LibMP::LiveContext live;
    live.config.max_frames_per_kv = 8;
    live.Init();
    live.StartAddingFrame(true, true);

    const GPRX::Locator logLocator = GPRX::BuildDataLocator(live.logs1_block.GetBlockId(), 0);
    if (!live.logs1_block.ptr->AddExternalPayloadChunk(
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(kLogEntries)),
            static_cast<mpsize_t>(sizeof(kLogEntries))))
        return {};

    ntd::vector<LibMP::TypesNtd::ThreadLogDescr> threadLogs;
    threadLogs.emplace_back(
        kThreadId, static_cast<uint32_t>(countof(kLogEntries)), logLocator.entry, false);
    live.SelectKV(live.kvframes1_block, live.frames1_block);
    live.BuildTable([&](auto& builder) {
        return LibMP::TypesNtd::CreateFrameDescDirect(
            builder, live.frames_counter, 1, 1, 100, 0, 0,
            kLabelOffset, kLabelOffset + sizeof(kLabels), false, false, &threadLogs);
    });
    live.SetUid(LibMP::QueryEx::FrameDesc_u24FrameId, true, live.frames_counter);
    live.PutTable();
    live.FinishAddingFrame();
    live.bInitialFillingCompleted = true;
    live.TrimLogs(live.frames_counter, 0);

    live.SelectKV(live.kvlongterm_block, live.longterm_block);
    live.BuildTable([&](auto& builder) {
        return LibMP::TypesNtd::CreateGroupDescDirect(
            builder, 1, "Labels", 0x123456, false);
    });
    live.SetUid(LibMP::QueryEx::GroupDesc_u24GroupId, true, 1);
    live.PutTable();
    live.SetUid(LibMP::QueryEx::GroupIdMax);
    live.PutU32(1);

    live.BuildTable([&](auto& builder) {
        return LibMP::TypesNtd::CreateTimerDescDirect(
            builder, kTimerId, "$Scope", 1, 0x654321, false);
    });
    live.SetUid(LibMP::QueryEx::TimerDesc_u24TimerId, true, kTimerId);
    live.PutTable();
    live.SetUid(LibMP::QueryEx::TimerIds);
    live.PutArrayU32(kTimerId);

    live.BuildTable([&](auto& builder) {
        return LibMP::TypesNtd::CreateTimerDescDirect(
            builder, kOverflowTimerId, "$ScriptOverflow", 1, 0x456321, false);
    });
    live.SetUid(LibMP::QueryEx::TimerDesc_u24TimerId, true, kOverflowTimerId);
    live.PutTable();
    live.SetUid(LibMP::QueryEx::TimerIds);
    live.PutArrayU32(kOverflowTimerId);

    live.BuildTable([&](auto& builder) {
        return LibMP::TypesNtd::CreateThreadDescDirect(
            builder, kThreadId, "Label thread",
            static_cast<uint32_t>(countof(kLogEntries)), false);
    });
    live.SetUid(LibMP::QueryEx::ThreadDesc_u24ThreadId, true, kThreadId);
    live.PutTable();
    live.SetUid(LibMP::QueryEx::ThreadIds);
    live.PutArrayU32(kThreadId);

    struct CounterFixture
    {
        uint32_t id;
        const char* name;
        uint32_t parent;
        uint32_t sibling;
        uint32_t firstChild;
        uint32_t level;
    };
    static constexpr CounterFixture kCounterDescriptors[] = {
        {1, "frame_stats", 0, 0, 2, 0},
        {2, "basic", 1, 0, 3, 1},
        {3, "cpu_frequency_mhz", 2, 4, 0, 2},
        {4, "memory_used_bytes", 2, 5, 0, 2},
        {5, "memory_free_bytes", 2, 6, 0, 2},
        {6, "alloc_time_microsec", 2, 7, 0, 2},
        {7, "alloc_count", 2, 8, 0, 2},
        {8, "free_time_microsec", 2, 9, 0, 2},
        {9, "free_count", 2, 10, 0, 2},
        {10, "gpu_device_time_microsec", 2, 0, 0, 2},
    };
    struct CounterSampleFixture
    {
        uint32_t id;
        double value;
    };
    static constexpr CounterSampleFixture kCounterSamples[] = {
        {3, 2500.0},
        {4, 64.0 * 1024.0 * 1024.0},
        {5, 128.0 * 1024.0 * 1024.0},
        {6, 1250.0},
        {7, 7.0},
        {8, 2500.0},
        {9, 5.0},
        {10, 3750.0},
    };
    for (const CounterFixture& counter : kCounterDescriptors)
    {
        live.BuildTable([&](auto& builder) {
            return LibMP::TypesNtd::CreateCounterDescDirect(
                builder, counter.id, counter.name, counter.parent,
                counter.sibling, counter.firstChild, static_cast<uint8_t>(counter.level));
        });
        live.SetUid(LibMP::QueryEx::CounterDesc_u24CounterId, true, counter.id);
        live.PutTable();
        live.SetUid(LibMP::QueryEx::CounterIds);
        live.PutArrayU32(counter.id);
    }

    live.SelectKV(live.kvshortterm_block, live.shortterm_block);
    if (!live.PutLabelBufferRegion(kLabelOffset, kLabels, sizeof(kLabels)))
        return {};
    live.SetUid(LibMP::QueryEx::FrameIdMin);
    live.PutU32(live.frameIdMin);
    live.SetUid(LibMP::QueryEx::FrameIdMax);
    live.PutU32(live.frameIdMax);
    const LibMP::TypesNtd::GlobalDesc global(1.0, 1.0);
    live.SetUid(LibMP::QueryEx::GlobalDesc);
    live.PutStruct(global);

    live.SelectKV(live.kvcounters1_block, live.kvcounters1_block);
    for (const CounterSampleFixture& counter : kCounterSamples)
    {
        const LibMP::TypesNtd::CounterSample sample(live.frames_counter, counter.value);
        live.value_buf.FromCounterSample(sample);
        live.SetUid(LibMP::QueryEx::CounterSamples_u24CounterId, true, counter.id);
        live.PutArrayValueBuf();
    }

    live.SetDataChanged();
    live.CheckExport();
    GPRX::StreamProcessorChunkedMemory output;
    if (!live.live_container->SaveToStream(output, true, true, true))
        return {};
    return output.GetComposedChunks();
}

enum class RelatedCaptureStage
{
    Baseline,
    ShortTermChanged,
    LongTermChanged,
};

// Rebuilds three snapshots with stable block IDs. Each stage retains the prior
// stage's changes so consecutive reloads alter exactly one metadata KV block.
ntd::vector<uint8_t> BuildRelatedCaptureBuffer(RelatedCaptureStage stage)
{
    const bool shortTermChanged = stage != RelatedCaptureStage::Baseline;
    const bool longTermChanged = stage == RelatedCaptureStage::LongTermChanged;
    return BuildCaptureBuffer(
        256,
        true,
        rd::kCounters,
        countof(rd::kCounters),
        shortTermChanged,
        longTermChanged,
        shortTermChanged ? "{\"libmp_fixture\":\"short-term-updated\"}" : nullptr,
        longTermChanged ? "libmp-fixture-long-term-updated-timer" : nullptr);
}
} // namespace

// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// parity_tool.cpp -- C++ reference for cross-language dump parity.
//
//   parity_tool build <out.gprx>
//       Rebuild the known-good capture from the committed real-data fixture.
//
//   parity_tool build-related <base.gprx> <short-update.gprx> <long-update.gprx>
//       Rebuild related snapshots that change one metadata cache bucket at a time.
//
//   parity_tool summary <capture.gprx>
//       Emit a canonical, line-oriented representation of every public dump-data family:
//       descriptors, samples, frames/thread-log descriptors, counter traversal, iterator
//       state for every public iterator report.
//
// The summary deliberately uses only simple loops and public LibMP calls. Integer fields
// are decimal, strings are length-prefixed hexadecimal bytes, floats are their IEEE bits,
// and uint64_t values are fixed-width hexadecimal. A new language wrapper should be able
// to translate each Write* function directly without reproducing C++ implementation details.

#include "../cpp/roundtrip_capture_builder.h"

#include "mp_api/c_api.h"
#include "mp_api/cpp_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
int Bool(bool value)
{
    return value ? 1 : 0;
}

std::string HexU64(uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%08x%08x",
        static_cast<uint32_t>(value >> 32), static_cast<uint32_t>(value));
    return text;
}

std::string FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", bits);
    return text;
}

std::string DoubleBits(double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return HexU64(bits);
}

std::string HexBytes(const char* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(24 + size * 2);
    result += std::to_string(size);
    result += ':';
    for (size_t i = 0; i < size; ++i)
    {
        const uint8_t byte = static_cast<uint8_t>(data[i]);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xf]);
    }
    return result;
}

template<typename String>
std::string NativeString(const String& value)
{
    return HexBytes(value.data(), value.size());
}

void WriteSessionInfo(LibMP::Session& session)
{
    std::printf("LIBMP_PARITY 3\n");
    std::printf("SESSION %u %u %u %u\n",
        session.GetDataFormatVersion(), LibMP::Versions::library,
        session.GetFrameIdMin(), session.GetFrameIdMax());

    const auto global = session.FetchGlobalDesc();
    if (global)
        std::printf("GLOBAL 1 %s %s\n",
            FloatBits(global->tick_to_ms_cpu()).c_str(),
            FloatBits(global->tick_to_ms_gpu()).c_str());
    else
        std::printf("GLOBAL 0\n");

    const auto general = session.FetchGeneralInfo();
    if (general)
        std::printf("GENERAL 1 %s\n", NativeString(general->platform_info_json).c_str());
    else
        std::printf("GENERAL 0\n");
}

void WriteGroups(LibMP::Session& session)
{
    const uint32_t groupIdMax = session.GetGroupIdMax();
    std::printf("GROUPS %u\n", groupIdMax);
    for (uint32_t queryId = 1; queryId <= groupIdMax; ++queryId)
    {
        const auto group = session.FetchGroupDesc(queryId);
        if (!group)
        {
            std::printf("GROUP %u 0\n", queryId);
            continue;
        }
        std::printf("GROUP %u 1 %u %s %08x %d\n",
            queryId, group->group_id, NativeString(group->group_name).c_str(),
            group->color, Bool(group->is_gpu));
    }
}

void WriteTimers(LibMP::Session& session)
{
    const auto timerIds = session.FetchTimerIds();
    std::printf("TIMER_IDS %zu\n", timerIds.size());
    for (size_t index = 0; index < timerIds.size(); ++index)
    {
        const uint32_t id = timerIds[index];
        std::printf("TIMER_ID %zu %u\n", index, id);
        const auto timer = session.FetchTimerDesc(id);
        if (!timer)
        {
            std::printf("TIMER %u 0\n", id);
            continue;
        }
        std::printf("TIMER %u 1 %u %s %u %08x %d\n",
            id, timer->timer_id, NativeString(timer->timer_name).c_str(),
            timer->group_id, timer->color, Bool(timer->is_user_timer));
    }

    const uint32_t groupIdMax = session.GetGroupIdMax();
    for (uint32_t groupId = 1; groupId <= groupIdMax; ++groupId)
    {
        const auto ids = session.FetchTimerIdsByGroupId(groupId);
        std::printf("GROUP_TIMER_IDS %u %zu", groupId, ids.size());
        for (size_t index = 0; index < ids.size(); ++index)
            std::printf(" %u", ids[index]);
        std::printf("\n");
    }
}

void WriteCounters(LibMP::Session& session)
{
    const auto counterIds = session.FetchCounterIds();
    std::printf("COUNTER_IDS %zu\n", counterIds.size());
    for (size_t index = 0; index < counterIds.size(); ++index)
    {
        const uint32_t id = counterIds[index];
        std::printf("COUNTER_ID %zu %u\n", index, id);
        const auto counter = session.FetchCounterDesc(id);
        if (!counter)
        {
            std::printf("COUNTER %u 0\n", id);
            continue;
        }
        std::printf("COUNTER %u 1 %u %s %u %u %u %u\n",
            id, counter->counter_id, NativeString(counter->counter_name).c_str(),
            counter->parent, counter->sibling, counter->first_child, counter->level);

        const auto samples = session.FetchCounterSamples(id);
        std::printf("COUNTER_SAMPLES %u %zu\n", id, samples.size());
        for (size_t sampleIndex = 0; sampleIndex < samples.size(); ++sampleIndex)
        {
            const auto& sample = samples[sampleIndex];
            std::printf("COUNTER_SAMPLE %u %zu 1 %u %s\n", id, sampleIndex,
                sample.frame_id(), DoubleBits(sample.value()).c_str());
        }

        const auto last = session.FetchLastCounterSample(id);
        if (last)
            std::printf("COUNTER_LAST %u 1 %u %s\n",
                id, last->frame_id(), DoubleBits(last->value()).c_str());
        else
            std::printf("COUNTER_LAST %u 0\n", id);
    }
}

void WriteThreads(LibMP::Session& session)
{
    std::printf("GPU_THREAD %u\n", session.GetThreadIdGpu());
    const auto threadIds = session.FetchThreadIds();
    std::printf("THREAD_IDS %zu\n", threadIds.size());
    for (size_t index = 0; index < threadIds.size(); ++index)
    {
        const uint32_t id = threadIds[index];
        std::printf("THREAD_ID %zu %u\n", index, id);
        const auto thread = session.FetchThreadDesc(id);
        if (!thread)
        {
            std::printf("THREAD %u 0\n", id);
            continue;
        }
        std::printf("THREAD %u 1 %u %s %u %d\n",
            id, thread->thread_id, NativeString(thread->thread_name).c_str(),
            thread->buffer_size, Bool(thread->is_gpu));
    }
}

void WriteFrames(LibMP::Session& session)
{
    const uint32_t frameIdMin = session.GetFrameIdMin();
    const uint32_t frameIdMax = session.GetFrameIdMax();
    const uint64_t frameCount = frameIdMax >= frameIdMin
        ? static_cast<uint64_t>(frameIdMax) - frameIdMin + 1
        : 0;
    std::printf("FRAMES %llu\n", static_cast<unsigned long long>(frameCount));
    for (uint64_t offset = 0; offset < frameCount; ++offset)
    {
        const uint32_t queryId = frameIdMin + static_cast<uint32_t>(offset);
        const auto frame = session.FetchFrameDesc(queryId);
        if (!frame)
        {
            std::printf("FRAME %u 0\n", queryId);
            continue;
        }
        const size_t threadLogCount = frame->thread_logs.size();
        std::printf("FRAME %u 1 %u %u %s %s %s %s %s %s %d %d %u\n",
            queryId, frame->frame_id, frame->frame_absolute_id,
            HexU64(frame->tick_start_cpu).c_str(), HexU64(frame->tick_end_cpu).c_str(),
            HexU64(frame->tick_start_gpu).c_str(), HexU64(frame->tick_end_gpu).c_str(),
            HexU64(frame->label_offset_min).c_str(), HexU64(frame->label_offset_max).c_str(),
            Bool(frame->is_paused), Bool(frame->is_incomplete),
            static_cast<unsigned int>(threadLogCount));
        for (size_t logIndex = 0; logIndex < threadLogCount; ++logIndex)
        {
            const auto& log = frame->thread_logs[logIndex];
            std::printf("FRAME_THREAD_LOG %u %zu %u %u %s %d\n",
                queryId, logIndex, log.thread_id(), log.log_entries_num(),
                HexU64(log.log_start_locator()).c_str(), Bool(log.is_removed()));
        }
    }
}

void WriteAuxiliarySamples(LibMP::Session& session)
{
    auto writeU64Samples = [](const char* name, const auto& samples) {
        std::printf("%s_SAMPLES %zu\n", name, samples.size());
        for (size_t index = 0; index < samples.size(); ++index)
        {
            const auto& sample = samples[index];
            std::printf("%s_SAMPLE %zu 1 %u %s\n", name, index,
                sample.frame_id(), HexU64(sample.value()).c_str());
        }
    };
    writeU64Samples("PLACE_ID", session.FetchPlaceIdSamples());
    writeU64Samples("UTC", session.FetchUtcTimestampSamples());
}

void WriteFindQueries(LibMP::Session& session)
{
    struct Probe
    {
        const char* mask;
        bool caseSensitive;
    };
    static const Probe probes[] = {
        { "*", false },
        { "*", true },
        { "*A*", false },
        { "*A*", true },
        { "__libmp_parity_missing__", false },
    };

    std::printf("FIND_QUERIES %zu\n", sizeof(probes) / sizeof(probes[0]));
    auto writeFindFamily = [&](const char* family, auto findFirst, auto findAll) {
        for (size_t probeIndex = 0; probeIndex < sizeof(probes) / sizeof(probes[0]); ++probeIndex)
        {
            const Probe& probe = probes[probeIndex];
            const uint32_t first = findFirst(probe.mask, probe.caseSensitive);
            auto ids = findAll(probe.mask, probe.caseSensitive);
            std::printf("FIND %s %zu %d %s %u %zu", family, probeIndex,
                Bool(probe.caseSensitive), HexBytes(probe.mask, std::strlen(probe.mask)).c_str(),
                first, ids.size());
            for (uint32_t id : ids)
                std::printf(" %u", id);
            std::printf("\n");
        }
    };

    writeFindFamily("GROUP",
        [&](const char* mask, bool sensitive) { return session.FindGroupId(mask, sensitive); },
        [&](const char* mask, bool sensitive) { return session.FindGroupIds(mask, sensitive); });
    writeFindFamily("TIMER",
        [&](const char* mask, bool sensitive) { return session.FindTimerId(mask, sensitive); },
        [&](const char* mask, bool sensitive) { return session.FindTimerIds(mask, sensitive); });
    writeFindFamily("COUNTER",
        [&](const char* mask, bool sensitive) { return session.FindCounterId(mask, sensitive); },
        [&](const char* mask, bool sensitive) { return session.FindCounterIds(mask, sensitive); });
    writeFindFamily("THREAD",
        [&](const char* mask, bool sensitive) { return session.FindThreadId(mask, sensitive); },
        [&](const char* mask, bool sensitive) { return session.FindThreadIds(mask, sensitive); });

    std::printf("GROUP_NAME_TIMER_QUERIES %zu\n", sizeof(probes) / sizeof(probes[0]));
    for (size_t probeIndex = 0; probeIndex < sizeof(probes) / sizeof(probes[0]); ++probeIndex)
    {
        const Probe& probe = probes[probeIndex];
        auto ids = session.FetchTimerIdsByGroupNames(probe.mask, probe.caseSensitive);
        std::printf("GROUP_NAME_TIMERS %zu %d %s %zu", probeIndex,
            Bool(probe.caseSensitive), HexBytes(probe.mask, std::strlen(probe.mask)).c_str(),
            ids.size());
        for (uint32_t id : ids)
            std::printf(" %u", id);
        std::printf("\n");
    }
}

void WriteCounterIterator(LibMP::Session& session)
{
    std::printf("COUNTER_ITERATOR_BEGIN\n");
    auto iterator = session.CreateCounterIterator();
    LibMP::TypesNtd::CounterIteratorConfigT config;
    iterator.Configure(config);
    const auto* state = iterator.GetState();
    size_t reportIndex = 0;
    while (iterator.Step())
    {
        std::printf("CI %zu %d %d %u %u %u %u %u %u %d %d %d\n",
            reportIndex++, Bool(state->started()), Bool(state->finished()),
            state->counter_id(), state->parent_id(), state->level(), state->relative_level(),
            state->root_level(), state->index_in_siblings(), Bool(state->is_leaf()),
            Bool(state->hierarchy_changed()), Bool(state->counter_stack_is_overflowed()));
    }
    std::printf("COUNTER_ITERATOR_END %zu %d\n", reportIndex, Bool(state->finished()));
}

void WriteLogIterator(LibMP::Session& session)
{
    std::printf("LOG_ITERATOR_BEGIN\n");
    auto iterator = session.CreateLogIterator();
    LibMP::TypesNtd::LogIteratorConfigT config;
    config.emit_thread_frame_end = true;
    iterator.Configure(config);
    const auto* state = iterator.GetState();
    size_t reportIndex = 0;
    while (iterator.Step())
    {
        std::printf("LI %zu %d %d %u %d %d %d %s %u %u %s %u %d %d %d %d "
                    "%s %u %s %s %u %u %d %d %d %d %d %d %d %u\n",
            reportIndex++, Bool(state->started()), Bool(state->finished()),
            state->thread_stack_depth(), Bool(state->thread_stack_is_underflowed()),
            Bool(state->thread_stack_is_overflowed()), Bool(state->thread_stack_was_overflowed()),
            HexU64(state->thread_prev_timestamp()).c_str(), state->frame_id(),
            state->frame_absolute_id(), HexU64(state->frame_desc_locator()).c_str(),
            state->thread_id(), Bool(state->is_gpu_thread()), Bool(state->is_frame_stitching()),
            Bool(state->frame_changed()), Bool(state->thread_changed()),
            HexU64(state->entry_locator()).c_str(), state->timer_id(),
            HexU64(state->timestamp()).c_str(), HexU64(state->timestamp_gpu()).c_str(),
            state->entry_type_raw(), state->event_type(), Bool(state->is_unknown()),
            Bool(state->is_enter()), Bool(state->is_exit()), Bool(state->is_label()),
            Bool(state->is_event()), Bool(state->is_thread_frame_end()),
            Bool(state->is_frame_boundary()), iterator.GetCurrentEntryCount());
    }
    std::printf("LOG_ITERATOR_END %zu %d\n", reportIndex, Bool(state->finished()));
}

int WriteCapture(const char* outputPath, const ntd::vector<uint8_t>& data)
{
    if (data.empty())
        return 1;
    std::ofstream file(outputPath, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "parity_tool: cannot write %s\n", outputPath);
        return 1;
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.close();
    if (file.fail())
        return 1;
    std::fprintf(stderr, "parity_tool: wrote %zu-byte capture -> %s\n", data.size(), outputPath);
    return 0;
}

int BuildCapture(const char* outputPath)
{
    return WriteCapture(outputPath, BuildRelatedCaptureBuffer(RelatedCaptureStage::Baseline));
}

int BuildLabeledCapture(const char* outputPath)
{
    return WriteCapture(outputPath, BuildLabeledCaptureBuffer());
}

int BuildRelatedCaptures(
    const char* baselinePath,
    const char* shortTermUpdatePath,
    const char* longTermUpdatePath)
{
    if (WriteCapture(
            baselinePath,
            BuildRelatedCaptureBuffer(RelatedCaptureStage::Baseline)) != 0)
        return 1;
    if (WriteCapture(
            shortTermUpdatePath,
            BuildRelatedCaptureBuffer(RelatedCaptureStage::ShortTermChanged)) != 0)
        return 1;
    return WriteCapture(
        longTermUpdatePath,
        BuildRelatedCaptureBuffer(RelatedCaptureStage::LongTermChanged));
}

int WriteSummary(const char* path)
{
    LibMP::Init();
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        std::fprintf(stderr, "parity_tool: failed to read %s\n", path);
        return 1;
    }
    const std::streamoff end = stream.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > std::numeric_limits<mpsize_t>::max())
    {
        std::fprintf(stderr, "parity_tool: invalid capture size for %s\n", path);
        return 1;
    }
    std::vector<uint8_t> capture(static_cast<size_t>(end));
    stream.seekg(0);
    if (!capture.empty() &&
        !stream.read(reinterpret_cast<char*>(capture.data()), end))
    {
        std::fprintf(stderr, "parity_tool: failed to read %s\n", path);
        return 1;
    }
    auto session = LibMP::Session::OpenFromMemTrusted(
        capture.data(), static_cast<mpsize_t>(capture.size()));
    if (!session || !session->IsValid())
    {
        std::fprintf(stderr, "parity_tool: failed to open %s\n", path);
        return 1;
    }
    session->EnableObjectCache(true, true, true, true);

    WriteSessionInfo(*session);
    WriteGroups(*session);
    WriteTimers(*session);
    WriteCounters(*session);
    WriteThreads(*session);
    WriteFrames(*session);
    WriteAuxiliarySamples(*session);
    WriteFindQueries(*session);
    WriteCounterIterator(*session);
    WriteLogIterator(*session);
    std::printf("END\n");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "build") == 0)
        return BuildCapture(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "build-labeled") == 0)
        return BuildLabeledCapture(argv[2]);
    if (argc == 5 && std::strcmp(argv[1], "build-related") == 0)
        return BuildRelatedCaptures(argv[2], argv[3], argv[4]);
    if (argc >= 3 && std::strcmp(argv[1], "summary") == 0)
        return WriteSummary(argv[2]);
    std::fprintf(stderr,
        "usage: parity_tool build <out.gprx> | build-labeled <out.gprx> | "
        "build-related <base.gprx> <short-update.gprx> <long-update.gprx> | "
        "summary <capture.gprx>\n");
    return 2;
}

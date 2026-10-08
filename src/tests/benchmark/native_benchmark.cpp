// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// Native runner for scripts/benchmark.py. Keep Workload aligned with the language runners.

#include "mp_api/cpp_api.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
volatile uint32_t g_sink = 0;

struct WorkloadResult
{
    uint32_t checksum;
    uint64_t item_count;
};

uint32_t Mix(uint32_t hash, uint32_t value)
{
    hash = (hash << 5) | (hash >> 27);
    return (hash ^ value) + UINT32_C(0x9e3779b9);
}

template<typename String>
uint32_t StringSize(const String& value)
{
    if (value.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("descriptor string is too large");
    return static_cast<uint32_t>(value.size());
}

WorkloadResult Workload(LibMP::Session& session, uint32_t passes)
{
    uint32_t hash = UINT32_C(0x811c9dc5);
    uint64_t itemCount = 0;
    for (uint32_t pass = 0; pass < passes; ++pass)
    {
        hash = Mix(hash, session.GetFrameIdMin());
        hash = Mix(hash, session.GetFrameIdMax());

        const auto global = session.FetchGlobalDesc();
        hash = Mix(hash, global ? 1u : 0u);
        const auto general = session.FetchGeneralInfo();
        hash = Mix(hash, general ? StringSize(general->platform_info_json) : 0u);
        itemCount += 2;

        const uint32_t groupIdMax = session.GetGroupIdMax();
        hash = Mix(hash, groupIdMax);
        itemCount += groupIdMax;
        for (uint32_t id = 1; id <= groupIdMax; ++id)
        {
            const auto value = session.FetchGroupDesc(id);
            if (!value)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value->group_id);
            hash = Mix(hash, StringSize(value->group_name));
            hash = Mix(hash, value->color);
            hash = Mix(hash, value->is_gpu ? 1u : 0u);
        }

        const auto timerIds = session.FetchTimerIds();
        hash = Mix(hash, static_cast<uint32_t>(timerIds.size()));
        itemCount += timerIds.size();
        for (uint32_t id : timerIds)
        {
            hash = Mix(hash, id);
            const auto value = session.FetchTimerDesc(id);
            if (!value)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value->timer_id);
            hash = Mix(hash, StringSize(value->timer_name));
            hash = Mix(hash, value->group_id);
            hash = Mix(hash, value->color);
            hash = Mix(hash, value->is_user_timer ? 1u : 0u);
        }

        const auto counterIds = session.FetchCounterIds();
        hash = Mix(hash, static_cast<uint32_t>(counterIds.size()));
        itemCount += counterIds.size();
        for (uint32_t id : counterIds)
        {
            hash = Mix(hash, id);
            const auto value = session.FetchCounterDesc(id);
            if (!value)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value->counter_id);
            hash = Mix(hash, StringSize(value->counter_name));
            hash = Mix(hash, value->parent);
            hash = Mix(hash, value->sibling);
            hash = Mix(hash, value->first_child);
            hash = Mix(hash, value->level);
        }

        const auto threadIds = session.FetchThreadIds();
        hash = Mix(hash, static_cast<uint32_t>(threadIds.size()));
        itemCount += threadIds.size();
        for (uint32_t id : threadIds)
        {
            hash = Mix(hash, id);
            const auto value = session.FetchThreadDesc(id);
            if (!value)
            {
                hash = Mix(hash, 0);
                continue;
            }
            hash = Mix(hash, value->thread_id);
            hash = Mix(hash, StringSize(value->thread_name));
            hash = Mix(hash, value->buffer_size);
            hash = Mix(hash, value->is_gpu ? 1u : 0u);
        }
    }
    return {hash, itemCount};
}

WorkloadResult LogWorkload(LibMP::Session& session, uint32_t passes)
{
    uint32_t hash = UINT32_C(0x811c9dc5);
    uint64_t itemCount = 0;
    for (uint32_t pass = 0; pass < passes; ++pass)
    {
        auto iterator = session.CreateLogIterator();
        LibMP::DefaultFlavor::LogIteratorConfigT config;
        config.emit_thread_frame_end = true;
        iterator.Configure(config);
        const auto* state = iterator.GetState();
        if (!state)
            throw std::runtime_error("log iterator has no state");

        uint32_t entryCount = 0;
        while (iterator.Step())
        {
            hash = Mix(hash, state->frame_id());
            hash = Mix(hash, state->thread_id());
            hash = Mix(hash, state->timer_id());
            hash = Mix(hash, state->entry_type_raw());
            hash = Mix(hash, state->event_type());
            hash = Mix(hash, state->is_event() ? 1u : 0u);

            const auto timer = session.FetchTimerDesc(state->timer_id());
            if (timer)
            {
                hash = Mix(hash, timer->timer_id);
                hash = Mix(hash, StringSize(timer->timer_name));
                hash = Mix(hash, timer->color);
            }
            else
                hash = Mix(hash, 0);
            ++entryCount;
        }
        hash = Mix(hash, entryCount);
        itemCount += entryCount;
    }
    return {hash, itemCount};
}

std::vector<uint8_t> ReadFile(const char* path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error(std::string("cannot open capture: ") + path);
    const std::streamoff end = stream.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > std::numeric_limits<size_t>::max())
        throw std::runtime_error("capture size is invalid");
    std::vector<uint8_t> result(static_cast<size_t>(end));
    stream.seekg(0);
    if (!result.empty() && !stream.read(reinterpret_cast<char*>(result.data()), end))
        throw std::runtime_error("cannot read capture");
    return result;
}

LibMP::DefaultFlavor::unique_ptr<LibMP::Session> OpenSession(
    const std::vector<uint8_t>& capture,
    bool cache)
{
    if (capture.size() > std::numeric_limits<mpsize_t>::max())
        throw std::runtime_error("capture exceeds mpsize_t");
    auto session = LibMP::Session::OpenFromMemTrusted(
        capture.data(), static_cast<mpsize_t>(capture.size()));
    if (!session || !session->IsValid())
        throw std::runtime_error("failed to open capture");
    session->EnableObjectCache(cache, cache, cache, cache);
    return session;
}

uint32_t ParsePositive(const char* text, const char* name, bool allowZero)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (!text[0] || !end || *end || value > std::numeric_limits<uint32_t>::max()
        || (!allowZero && value == 0))
        throw std::runtime_error(std::string("invalid ") + name);
    return static_cast<uint32_t>(value);
}

using Clock = std::chrono::steady_clock;

void PrintResult(const char* scenario, bool cache, const WorkloadResult& result,
    const std::vector<uint64_t>& samples)
{
    std::printf("BENCH\t%s\t%s\t%u\t%llu\t", scenario, cache ? "on" : "off",
        result.checksum, static_cast<unsigned long long>(result.item_count));
    for (size_t i = 0; i < samples.size(); ++i)
        std::printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(samples[i]));
    std::printf("\n");
}

void RunSharedSession(const std::vector<uint8_t>& capture, bool cache,
    uint32_t warmups, uint32_t iterations, uint32_t passes)
{
    auto session = OpenSession(capture, cache);
    for (uint32_t i = 0; i < warmups; ++i)
        g_sink = Workload(*session, passes).checksum;

    std::vector<uint64_t> samples;
    samples.reserve(iterations);
    WorkloadResult result{};
    for (uint32_t i = 0; i < iterations; ++i)
    {
        const auto start = Clock::now();
        const WorkloadResult current = Workload(*session, passes);
        const auto end = Clock::now();
        if (i && (current.checksum != result.checksum
                || current.item_count != result.item_count))
            throw std::runtime_error("workload result changed");
        result = current;
        g_sink = current.checksum;
        samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
    PrintResult("shared", cache, result, samples);
}

void RunFreshSessions(const std::vector<uint8_t>& capture, bool cache,
    uint32_t warmups, uint32_t iterations, uint32_t passes)
{
    auto run = [&]() {
        auto session = OpenSession(capture, cache);
        const WorkloadResult result = Workload(*session, passes);
        session.reset();
        return result;
    };
    for (uint32_t i = 0; i < warmups; ++i)
        g_sink = run().checksum;

    std::vector<uint64_t> samples;
    samples.reserve(iterations);
    WorkloadResult result{};
    for (uint32_t i = 0; i < iterations; ++i)
    {
        const auto start = Clock::now();
        const WorkloadResult current = run();
        const auto end = Clock::now();
        if (i && (current.checksum != result.checksum
                || current.item_count != result.item_count))
            throw std::runtime_error("workload result changed");
        result = current;
        g_sink = current.checksum;
        samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
    PrintResult("fresh", cache, result, samples);
}

void RunLogTraversal(const std::vector<uint8_t>& capture, bool cache,
    uint32_t warmups, uint32_t iterations, uint32_t passes)
{
    auto session = OpenSession(capture, cache);
    for (uint32_t i = 0; i < warmups; ++i)
        g_sink = LogWorkload(*session, passes).checksum;

    std::vector<uint64_t> samples;
    samples.reserve(iterations);
    WorkloadResult result{};
    for (uint32_t i = 0; i < iterations; ++i)
    {
        const auto start = Clock::now();
        const WorkloadResult current = LogWorkload(*session, passes);
        const auto end = Clock::now();
        if (i && (current.checksum != result.checksum
                || current.item_count != result.item_count))
            throw std::runtime_error("log workload result changed");
        result = current;
        g_sink = current.checksum;
        samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
    PrintResult("logs", cache, result, samples);
}
}

int main(int argc, char** argv)
{
#ifndef NDEBUG
    // The rest of main is in the #else branch, which keeps it from being unreachable code
    (void)argc;
    (void)argv;
    std::fprintf(stderr, "native benchmark must be compiled in Release mode\n");
    return 2;
#else
    if (argc != 8)
    {
        std::fprintf(stderr,
            "usage: libmp_benchmark <capture.gprx> <warmups> <iterations> "
            "<metadata-passes> <log-warmups> <log-iterations> <log-passes>\n");
        return 2;
    }
    try
    {
        LibMP::Init();
        const uint32_t warmups = ParsePositive(argv[2], "warmup count", true);
        const uint32_t iterations = ParsePositive(argv[3], "iteration count", false);
        const uint32_t metadataPasses = ParsePositive(argv[4], "metadata pass count", false);
        const uint32_t logWarmups = ParsePositive(argv[5], "log warmup count", true);
        const uint32_t logIterations = ParsePositive(argv[6], "log iteration count", false);
        const uint32_t logPasses = ParsePositive(argv[7], "log pass count", false);
        const auto capture = ReadFile(argv[1]);
        for (bool cache : {false, true})
        {
            std::fprintf(stderr, "  cache=%s: metadata / reused session\n",
                cache ? "on" : "off");
            RunSharedSession(capture, cache, warmups, iterations, metadataPasses);
            std::fprintf(stderr, "  cache=%s: metadata / new session\n",
                cache ? "on" : "off");
            RunFreshSessions(capture, cache, warmups, iterations, metadataPasses);
            std::fprintf(stderr, "  cache=%s: full log traversal / reused session\n",
                cache ? "on" : "off");
            RunLogTraversal(capture, cache, logWarmups, logIterations, logPasses);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "native benchmark: %s\n", error.what());
        return 1;
    }
#endif
}

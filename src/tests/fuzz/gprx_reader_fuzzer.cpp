// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

// End-to-end fuzzer for the GPRX reader: opens arbitrary bytes as an in-memory container
// and traverses the public C API (descriptors, arrays, samples, and both iterators).
// ASan/UBSan cover the parser, data contexts, KV store, iterators, and caches reached
// from here. A generated valid-capture seed exercises the deeper parsing and traversal paths.

#include "mp_api/c_api.h"
#include "gprx_container/xassert.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
GPRX::detail::DiagnosticAssertionAction continue_expected_input_diagnostic(
    const GPRX::detail::DiagnosticAssertionInfo& info) noexcept
{
    static const char* const expected_messages[] = {
        "ContainerContext::AllocateScanBlock - block count exceeds mpsize_t",
        "ContainerContext::ScanSequential - decoded payload exceeds the per-block limit",
        "ContainerContext::GetPayloadDataPtrImpl - decoded payload exceeds the per-block limit",
        "ContainerContext::GetPayloadDataPtrImpl - decoded payload size does not match header",
        "KVStore::GetPayloadU32Ptr - uint32_t payload data must be aligned",
        "MpFindKVStore - KVStore block subtype must be within the "
            "BlockTypeMeta::Kv_SubTypes_Count range",
        "PatternMatcher::FindMatches - result count exceeds mpsize_t",
        "MpFindCountersImpl - counter parent ID has no descriptor",
        "MpFindCountersImpl - counter hierarchy exceeds counter count",
        "MpFindCountersImpl - result count exceeds mpsize_t",
        "MpLogIterator::CheckState - stack depth exceeds mpssize_t",
        "MpCounterIterator::PushToStack - counter hierarchy exceeds safe depth",
        "MpCounterIterator::RebuildStackToParent - counter hierarchy exceeds safe depth",
        "MpCounterIterator::RebuildStackToParent - counter parent ID has no descriptor",
    };
    for (const char* expected : expected_messages)
    {
        if (info.message && std::strcmp(info.message, expected) == 0)
            return GPRX::detail::DiagnosticAssertionAction::Continue;
    }
    return GPRX::detail::DiagnosticAssertionAction::UseDefault;
}
} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    GPRX::detail::SetDiagnosticAssertionHandler(continue_expected_input_diagnostic);
    MpInit();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > GPRX::max_mpsize_t)
        return 0;
    // DataContextSingleBuffer references `data` directly; it stays valid for this call.
    MpSession* s = MpOpenFromMem(data, static_cast<mpsize_t>(size));
    if (!s)
        return 0;

    MpIsSessionValid(s);
    MpEnableObjectCache(s, 1, 1, 1, 1);
    MpGetGlobalDesc(s);
    MpGetGeneralInfo(s);
    MpFindGroupId(s, "*", false);
    MpFindTimerId(s, "*", false);
    MpFindCounterId(s, "**/*", false);
    MpFindThreadId(s, "*", false);

    const uint32_t frame_min = MpGetFrameIdMin(s);
    const uint32_t frame_max = MpGetFrameIdMax(s);
    uint32_t f = frame_min;
    for (uint32_t count = 0; count < 64 && f <= frame_max; ++count, ++f)
    {
        MpGetFrameDesc(s, f);
        if (f == GPRX::max_u32)
            break;
    }

    if (MpArray* threads = MpGetThreadIds(s))
    {
        mpsize_t n = MpGetArraySize(threads);
        for (mpsize_t i = 0; i < n && i < 256; ++i)
            MpGetThreadDesc(s, MpGetIdFromArray(s, threads, i));
        MpReleaseArray(threads);
    }
    if (MpArray* timers = MpGetTimerIds(s))
    {
        mpsize_t n = MpGetArraySize(timers);
        for (mpsize_t i = 0; i < n && i < 256; ++i)
            MpGetTimerDesc(s, MpGetIdFromArray(s, timers, i));
        MpReleaseArray(timers);
    }
    if (MpArray* counters = MpGetCounterIds(s))
    {
        mpsize_t n = MpGetArraySize(counters);
        for (mpsize_t i = 0; i < n && i < 256; ++i)
        {
            uint32_t id = MpGetIdFromArray(s, counters, i);
            MpGetCounterDesc(s, id);
            if (MpArray* samples = MpGetCounterSamples(s, id))
                MpReleaseArray(samples);
        }
        MpReleaseArray(counters);
    }

    if (MpLogIterator* it = MpCreateLogIterator(s))
    {
        MpConfigureLogIterator(it, true);
        MpRewindLogIterator(it);
        int steps = 0;
        while (MpStepLogIterator(it) && steps++ < 4096)
            MpGetLogIteratorState(it);
        MpDeleteLogIterator(it);
    }
    if (MpCounterIterator* cit = MpCreateCounterIterator(s))
    {
        MpConfigureCounterIterator(cit, true);
        MpRewindCounterIterator(cit);
        int steps = 0;
        while (MpStepCounterIterator(cit) && steps++ < 4096)
            MpGetCounterIteratorState(cit);
        MpDeleteCounterIterator(cit);
    }

    MpCloseSession(s);
    return 0;
}

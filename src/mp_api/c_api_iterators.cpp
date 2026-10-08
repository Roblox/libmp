// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#include "c_api_private.h"
#include "log_entry.h"
#include "log_event.h"
#include "gprx_container/utils_ex.h"
#include "ntd_stdlib/algorithm.h"
#include "ntd_stdlib/cstring.h"

#include <cstddef>

using namespace GPRX;
using namespace LibMP;

////////////////////////////////////////////////////////////////////////////////////////////////////
struct MpLogIterator
{
    static constexpr mpsize_t THREAD_STACK_DEPTH_MAX = 50;
    static constexpr mpsize_t CONFIG_BUFFER_SIZE_DEFAULT = 1024;
    static constexpr mpsize_t CHUNK_READ_ENTRY_COUNT = 64;
    static constexpr mpsize_t SCOPE_LABEL_COUNT_MAX = 16;

    struct ThreadStackElementSupplement
    {
        uint64_t scope_labels[SCOPE_LABEL_COUNT_MAX] = {};
        uint8_t scope_label_count = 0;
    };

    struct ThreadStackElementWrapper
    {
        LibMP::TypesNtd::LogIteratorThreadStackElement element = {};
        ThreadStackElementSupplement supplement = {};
    };

    static_assert(sizeof(LibMP::TypesNtd::LogIteratorThreadStackElement) == 48,
        "The public stack element layout must remain wrapper-compatible");
    static_assert(offsetof(ThreadStackElementWrapper, element) == 0,
        "The public stack element must begin each internal stack record");

    struct ThreadStack
    {
        LibMP::TypesNtd::LogIteratorThreadStackState state = {};
        uint64_t prev_timestamp = 0;
        ntd::vector<ThreadStackElementWrapper> elements;

        inline bool IsEmpty()
        {
            return state.depth() == 0;
        }
        
        inline mpsize_t GetCappedDepth()
        {
            return ntd::min<mpsize_t>(state.depth(), THREAD_STACK_DEPTH_MAX);
        }

        inline bool IsOverflowed()
        {
            return (state.depth() >= elements.size());
        }

        inline void AllocateElements()
        {
            elements.resize(THREAD_STACK_DEPTH_MAX);
        }

        inline void Reset()
        {
            state = {};
            prev_timestamp = 0;
        }
    };

    struct PreprocessedConfig
    {
        uint32_t start_frame_id = 0;
        uint32_t end_frame_id = 0;
        uint32_t gpu_thread_id = 0;
        HybridLookupSet thread_ids;
        HybridLookupSet timer_ids;
        HybridLookupSet skip_timer_ids;
        bool has_gpu_thread = false;
        bool has_cpu_thread = false;
        uint32_t override_start_frame_id = 0;
        uint32_t override_end_frame_id = 0;
        bool override_frame_range = false;
        uint32_t processed_region_final_frame_id = 0;
        uint32_t processed_region_last_frame_abs_id = 0;
    };

    struct EpochInfo
    {
        uint64_t tick_start = 0;
        uint64_t tick_end = 0;
        uint32_t high16 = 0;
        bool is_safe = false;
        bool force_skip_normalization = false;

        inline void Init(uint64_t start, uint64_t end)
        {
            tick_start = start;
            tick_end = end;

            const uint32_t start_top16 = static_cast<uint32_t>(start >> 48);
            const uint32_t end_top16 = static_cast<uint32_t>(end >> 48);

            // Position the top 16 bits into the upper half of a 32-bit integer
            high16 = start_top16 << 16;

            is_safe = (start_top16 == end_top16);
            force_skip_normalization = (start_top16 == 0) && (end_top16 == 0);
        }
    };

    struct StatePrivate
    {
        uint32_t loaded_frame_id = 0;
        uint32_t frame_id = 0;
        uint32_t last_frame_abs_id = 0;
        bool loaded_frame_postprocessed = false;
        bool frame_stitch = false;
        const LibMP::TypesNtd::ThreadLogDescr *current_thread_log = nullptr;
        mpsize_t loaded_thread_num = GPRX::max_mpsize_t;
        mpsize_t thread_num = 0;
        mpsize_t entry_num = 0;
        mpsize_t reports_counter = 0;
        LibMP::TypesNtd::FrameDescT frame_desc = {};
        EpochInfo epoch_gpu = {};
        EpochInfo epoch_cpu = {};
        EpochInfo* current_thread_epoch = nullptr;
        bool current_thread_skip_ts_normalization = false;
        uint32_t thread_id = 0;
        mpsize_t log_entries_num = 0;
        uint64_t log_start_locator = 0;
        uint64_t* log_portion_ptr = nullptr;
        uint32_t log_portion_offset = 0;
        uint32_t log_portion_size = 0;
        ThreadStack* current_thread_stack = nullptr;
        mpsize_t step_entries_count = 0;
        mpsize_t event_size = 0;
        bool event_has_comment = false;
        uint32_t event_type = 0;
        bool was_event_entry = false;
        bool thread_changed = false;
        bool frame_changed = false;
        bool is_fast_path = false;
        bool pre_finish = false;
        bool thread_frame_end_fired = false;
        bool skip_step = false;
    };

    MpSession* m_session = nullptr;
    bool bUseChunkReads = false;

    bool custom_config = false;
    bool config_applied = false;
    ntd::vector<uint8_t> config_buffer;
    const LibMP::TypesNtd::LogIteratorConfig* config_reader = nullptr;
    PreprocessedConfig config_preprocessed = {};
    mpsize_t data_lock_depth = 0;
    uint64_t label_offset_result = 0;
    StatePrivate state_private = {};
    LibMP::TypesNtd::LogIteratorState state = {};
    ntd::unordered_map<uint32_t, ThreadStack> thread_stacks;

    MpLogIterator(MpSession* session)
        : m_session(session)
    {
        XASSERT_TRAP(GPRX::is_little_endian()); // MicroProfilerLogTypeU32 and MicroProfilerLogTimerIdU32 are little-endian specific
        GetConfigBuffer();
        bUseChunkReads = !session->dumpContainer->GetDataContext().IsPrefetchSupported();
    }

    static MpLogIterator* Create(MpSession* session)
    {
        return ntd::New<MpLogIterator>(session);
    }

    bool Step();
    void Rewind(bool keepThreadStacks = false);
    void Detach();
    uint8_t* GetConfigBuffer(mpsize_t minSize = CONFIG_BUFFER_SIZE_DEFAULT);
    void OverrideFrameRange(bool use_override, uint32_t start_frame_id = 0, uint32_t end_frame_id = 0);

private:
    inline bool CheckState();
    inline bool ShouldSkipThread();
    inline bool IsGpuThread(uint32_t thread_id);
    void MarkCrossFrameThreadStacks();
    void MarkFrameBoundary(bool isFrameStitch);
    void MarkThreadFrameEnd();
    void UpdateFrameAndThreadChanged();
    inline void UpdateThreadStackState(const LibMP::TypesNtd::LogIteratorThreadStackState& stack_state);
    void UnlockLogDataPtr(bool keep_offset = false);
    bool AdvanceLogDataPtr(mpsize_t entries_count = 1, mpsize_t start_entry_num = GPRX::max_mpsize_t);
    void Finish(bool bError = false);
    void PreprocessFrameRange();
    void PreprocessConfig();
    bool ConfigureAndStart();
    void ResetThreadStacks();
};

void MpLogIterator::ResetThreadStacks()
{
    config_preprocessed.processed_region_final_frame_id = 0;
    config_preprocessed.processed_region_last_frame_abs_id = 0;
    for (auto& [thread_id, stack] : thread_stacks)
    {
        stack.Reset();
    }
}

void MpLogIterator::MarkCrossFrameThreadStacks()
{
    for (auto& [t_id, stack] : thread_stacks)
    {
        if (stack.state.depth() == 0)
            continue;
        uint64_t boundary_ts = IsGpuThread(t_id)
            ? state_private.frame_desc.tick_start_gpu
            : state_private.frame_desc.tick_start_cpu;
        mpsize_t capped_depth = stack.GetCappedDepth();
        for (mpsize_t i = 0; i < capped_depth; ++i)
        {
            stack.elements[i].element.mutate_enter_timestamp_frame_local(boundary_ts);
        }
    }
}

void MpLogIterator::MarkFrameBoundary(bool isFrameStitch)
{
    state_private.step_entries_count = 0;
    state.mutate_is_frame_boundary(true);
    state.mutate_is_frame_stitching(isFrameStitch);
    state.mutate_is_thread_frame_end(false);
    state.mutate_is_enter(false);
    state.mutate_is_exit(false);
    state.mutate_is_label(false);
    state.mutate_is_event(false);
    state.mutate_is_unknown(false);
    state_private.skip_step = true;
}

void MpLogIterator::MarkThreadFrameEnd()
{
    state_private.step_entries_count = 0;
    state.mutate_is_frame_boundary(false);
    state.mutate_is_frame_stitching(false);
    state.mutate_is_thread_frame_end(true);
    state.mutate_is_enter(false);
    state.mutate_is_exit(false);
    state.mutate_is_label(false);
    state.mutate_is_event(false);
    state.mutate_is_unknown(false);
    uint64_t end_ts = state.is_gpu_thread()
        ? state_private.frame_desc.tick_end_gpu
        : state_private.frame_desc.tick_end_cpu;
    state.mutate_timestamp(end_ts);
    UpdateFrameAndThreadChanged();
    state_private.skip_step = true;
}

void MpLogIterator::UpdateFrameAndThreadChanged()
{
    state.mutate_frame_changed(state_private.frame_changed);
    state.mutate_thread_changed(state_private.thread_changed);
    state_private.frame_changed = false;
    state_private.thread_changed = false;
}

inline void MpLogIterator::UpdateThreadStackState(const LibMP::TypesNtd::LogIteratorThreadStackState& stack_state)
{
    state.mutate_thread_stack_depth(stack_state.depth());
    state.mutate_thread_stack_is_underflowed(stack_state.is_underflowed());
    state.mutate_thread_stack_is_overflowed(stack_state.is_overflowed());
    state.mutate_thread_stack_was_overflowed(stack_state.was_overflowed());
}

bool MpLogIterator::CheckState()
{
    while (true)
    {
        if (!state_private.is_fast_path)
        {
            if (state.finished())
                return false;

            if (!state.started())
                if (!ConfigureAndStart())
                {
                    Finish(true);
                    return false;
                }

            if (state_private.frame_id > config_preprocessed.end_frame_id)
            {
                if (!config_reader->skip_frame_boundaries() && state_private.loaded_frame_id > 0 && !state_private.pre_finish)
                {
                    state_private.pre_finish = true;
                    state.mutate_frame_id(state_private.frame_id); // Keep frame_absolute_id intact
                    state.mutate_timestamp(state_private.frame_desc.tick_end_cpu);
                    state.mutate_timestamp_gpu(state_private.frame_desc.tick_end_gpu);
                    MarkFrameBoundary(false);
                    return true;
                }

                Finish();
                return false;
            }

            if (state_private.frame_id != state_private.loaded_frame_id)
            {
                state_private.loaded_frame_id = state_private.frame_id;
                state_private.loaded_frame_postprocessed = false;
                const uint8_t* obj = MpGetFrameDesc(m_session, state_private.frame_id);
                if (!obj)
                {
                    Finish(true);
                    return false;
                }
                auto frameDesc = flatbuffers::GetRoot<LibMP::TypesNtd::FrameDesc>(obj);
                frameDesc->UnPackTo(&state_private.frame_desc);

                state_private.frame_changed = true;
                if (!config_reader->skip_timestamp_normalization())
                {
                    state_private.epoch_gpu.Init(state_private.frame_desc.tick_start_gpu, state_private.frame_desc.tick_end_gpu);
                    state_private.epoch_cpu.Init(state_private.frame_desc.tick_start_cpu, state_private.frame_desc.tick_end_cpu);
                }

                const bool isFrameDropped = state_private.last_frame_abs_id > 0 && (state_private.frame_desc.frame_absolute_id != state_private.last_frame_abs_id + 1);
                const bool isFramePaused = state_private.frame_desc.is_paused && config_reader->skip_paused_frames();
                state_private.frame_stitch = (isFrameDropped || isFramePaused);
                state_private.last_frame_abs_id = state_private.frame_desc.frame_absolute_id;

                state.mutate_frame_id(state_private.frame_id);
                state.mutate_frame_absolute_id(state_private.last_frame_abs_id);
                state.mutate_frame_desc_locator(m_session->global_obj_locator.entry);

                if (!config_reader->skip_frame_boundaries())
                {
                    state.mutate_timestamp(state_private.frame_desc.tick_start_cpu);
                    state.mutate_timestamp_gpu(state_private.frame_desc.tick_start_gpu);
                    MarkFrameBoundary(state_private.frame_stitch);
                    return true;
                }
            }

            if (!state_private.loaded_frame_postprocessed)
            {
                state_private.loaded_frame_postprocessed = true;
                if (state_private.frame_stitch)
                {
                    ResetThreadStacks();
                }
                if (!config_reader->skip_thread_stack_depth_sync())
                {
                    for (auto& [t_id, stack] : thread_stacks)
                    {
                        if (stack.state.depth() >
                            static_cast<uint32_t>(GPRX::max_mpssize_t))
                        {
                            XASSERT_DIAG(false,
                                "MpLogIterator::CheckState - stack depth exceeds mpssize_t");
                            Finish(true);
                            return false;
                        }
                        stack.state.mutate_depth_absolute(
                            static_cast<int32_t>(stack.state.depth()));
                        stack.state.mutate_depth_absolute_min(0);
                    }
                }
                if (state_private.frame_desc.is_paused && config_reader->skip_paused_frames())
                {
                    state_private.frame_id++;
                    continue;
                }
                MarkCrossFrameThreadStacks();
            }

            if (state_private.thread_num >= state_private.frame_desc.thread_logs.size())
            {
                state_private.frame_id++;
                state_private.thread_num = 0;
                state_private.loaded_thread_num = GPRX::max_mpsize_t;
                UnlockLogDataPtr();
                continue;
            }

            if (state_private.thread_num != state_private.loaded_thread_num)
            {
                const auto& thread_log = state_private.frame_desc.thread_logs[state_private.thread_num];
                const bool isSkipEmptyThreadLog = (thread_log.log_entries_num() == 0 && !config_reader->emit_thread_frame_end());
                state_private.thread_id = thread_log.thread_id();
                if (isSkipEmptyThreadLog || thread_log.is_removed() || ShouldSkipThread())
                {
                    state_private.thread_num++;
                    // UnlockLogDataPtr was already called in the previous step
                    continue;
                }

                state_private.current_thread_log = &thread_log;
                state_private.loaded_thread_num = state_private.thread_num;
                state_private.log_start_locator = thread_log.log_start_locator();
                state_private.log_entries_num = thread_log.log_entries_num();

                auto [it, inserted] = thread_stacks.try_emplace(state_private.thread_id);
                state_private.current_thread_stack = &it->second;
                if (inserted)
                {
                    state_private.current_thread_stack->AllocateElements();
                }

                bool is_gpu_thread = IsGpuThread(state_private.thread_id);
                state_private.thread_frame_end_fired = false;
                state_private.thread_changed = true;
                
                state_private.current_thread_epoch = is_gpu_thread ? &state_private.epoch_gpu : &state_private.epoch_cpu;
                state_private.current_thread_skip_ts_normalization =
                    config_reader->skip_timestamp_normalization() ||
                    state_private.current_thread_epoch->force_skip_normalization;

                UpdateThreadStackState(state_private.current_thread_stack->state);
                state.mutate_thread_prev_timestamp(state_private.current_thread_stack->prev_timestamp);
                state.mutate_thread_id(state_private.thread_id);
                state.mutate_is_gpu_thread(is_gpu_thread);
                state.mutate_is_frame_boundary(false);
                state.mutate_is_frame_stitching(false);
                state.mutate_is_thread_frame_end(false);
                state.mutate_timestamp_gpu(0);
            }

            state_private.is_fast_path = true;
        }

        // Fast path
        if (state_private.entry_num >= state_private.log_entries_num)
        {
            if (config_reader->emit_thread_frame_end() && !state_private.thread_frame_end_fired)
            {
                state_private.thread_frame_end_fired = true;
                MarkThreadFrameEnd();
                return true;
            }
            
            state_private.is_fast_path = false;
            state_private.thread_num++;
            UnlockLogDataPtr();
            continue;
        }

        break;
    }

    return true;
}

bool MpLogIterator::Step()
{
    while (true)
    {
        if (!CheckState())
            return false;

        if (state_private.skip_step)
        {
            state_private.skip_step = false;
            return true;
        }

        // Fetch the next data portion if necessary
        if (state_private.entry_num >= state_private.log_portion_offset + state_private.log_portion_size)
        {
            if (!AdvanceLogDataPtr())
            {
                Finish(true);
                return false;
            }
        }

        uint64_t* pEntry = &state_private.log_portion_ptr[state_private.entry_num - state_private.log_portion_offset];
        mpsize_t entries_count = 1;

        auto advanceEntryNum = [&]()
        {
            state_private.step_entries_count = entries_count;
            state_private.entry_num += entries_count;
        };

        uint32_t entryType = MicroProfilerLogTypeU32(pEntry);
        bool isEnter = (entryType == MP_LOG_ENTER);
        bool isExit = (entryType == MP_LOG_LEAVE);
        bool isLabel = (entryType == MP_LOG_LABEL || entryType == MP_LOG_LABEL_LITERAL);
        bool isScope = (isEnter || isExit);

        uint32_t timerId = 0;
        if (isScope)
        {
            timerId = MicroProfilerLogTimerIdU32(pEntry);
        }
        if (isScope)
        {
            if ((config_preprocessed.timer_ids.size() && !config_preprocessed.timer_ids.Contains(timerId)) ||
                (config_preprocessed.skip_timer_ids.size() && config_preprocessed.skip_timer_ids.Contains(timerId)))
            {
                advanceEntryNum();
                continue;
            }
        }

        uint64_t entry = *pEntry;

        bool isEvent = (entryType == MP_LOG_EVENT);
        if (isEvent && config_reader->skip_events())
        {
            // If single-entry Events prevail, it's cheaper to skip additional checks
#if 0
            auto [hasComment, eventSize, isExtraEntry] = MicroProfilerEventEntryInfo(entry);
            if (!isExtraEntry)
            {
                entries_count = MicroProfilerEventEntryLastOffset(eventSize, hasComment) + 1;
            }
#endif
            advanceEntryNum();
            continue;
        }

        uint64_t entry_locator = state_private.log_start_locator + state_private.entry_num;
        U64Split timestamp48;
        timestamp48.entry = isScope ? MicroProfilerLogTick(entry) : uint64_t{0};

        U64Split timestamp64;
        if (state_private.current_thread_skip_ts_normalization)
        {
            timestamp64 = timestamp48;
        }
        else if (state_private.current_thread_epoch->is_safe)
        {
            // Fast path
            timestamp64.parts.low = timestamp48.parts.low;
            timestamp64.parts.high = timestamp48.parts.high | state_private.current_thread_epoch->high16;
        }
        else
        {
            // Slow path: Fallback for the rare frame that spans a rollover boundary
            U64Split diff;
            diff.entry = timestamp48.entry - state_private.current_thread_epoch->tick_start;
            // The trick: Sign-extend the 48-bit delta.
            // By casting the high part down to int16_t, the CPU treats the 16th bit 
            // (which is the 48th bit of the full timestamp) as the sign bit.
            // Casting it back to int32_t automatically sign-extends it across the upper 16 bits.
            diff.parts.high = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int16_t>(diff.parts.high)));
            timestamp64.entry = state_private.current_thread_epoch->tick_start + diff.entry;
        }

        // Capture the previous timestamp and overflow state from the stack state before updating
        uint64_t prev_timestamp = 0;
        auto& stack_state = state_private.current_thread_stack->state;
        if (isScope)
        {
	        prev_timestamp = state_private.current_thread_stack->prev_timestamp;
            state_private.current_thread_stack->prev_timestamp = timestamp64.entry;
            stack_state.mutate_was_overflowed(stack_state.is_overflowed());
        }

        if (isEnter)
        {
            stack_state.mutate_depth(stack_state.depth() + 1);
            stack_state.mutate_depth_absolute(stack_state.depth_absolute() + 1);

            stack_state.mutate_is_underflowed(false);
            
            bool is_overflowed = state_private.current_thread_stack->IsOverflowed();
            stack_state.mutate_is_overflowed(is_overflowed);

            if (!is_overflowed)
            {
                auto& wrapper = state_private.current_thread_stack->elements[stack_state.depth() - 1];
                wrapper = ThreadStackElementWrapper{};
                auto& element = wrapper.element;
                element.mutate_enter_entry_locator(entry_locator);
                element.mutate_enter_frame_id(state_private.frame_id);
                element.mutate_enter_timer_id(timerId);
                element.mutate_enter_timestamp(timestamp64.entry);
                element.mutate_enter_timestamp_frame_local(timestamp64.entry);
            }
        }
        else if (isExit)
        {
            if (stack_state.depth() > 0)
            {
                stack_state.mutate_depth(stack_state.depth() - 1);
            }
            else
            {
                stack_state.mutate_is_underflowed(true);
            }
            
            int32_t depth_abs = stack_state.depth_absolute() - 1;
            stack_state.mutate_depth_absolute(depth_abs);
            if (depth_abs < stack_state.depth_absolute_min())
            {
                stack_state.mutate_depth_absolute_min(depth_abs);
            }

            bool is_overflowed = state_private.current_thread_stack->IsOverflowed();
            stack_state.mutate_is_overflowed(is_overflowed);
        }

        if (isLabel && config_reader->track_scope_labels() && stack_state.depth() > 0 &&
            stack_state.depth() < state_private.current_thread_stack->elements.size())
        {
            auto& supplement = state_private.current_thread_stack->elements[
                stack_state.depth() - 1].supplement;
            const mpsize_t labelCount = supplement.scope_label_count;
            if (labelCount < SCOPE_LABEL_COUNT_MAX)
            {
                supplement.scope_labels[labelCount] = entry;
                supplement.scope_label_count = static_cast<uint8_t>(labelCount + 1);
            }
        }
        
        if (isEvent)
        {
            auto [hasComment, eventSize, isExtraEntry] = MicroProfilerEventEntryInfo(entry);
            state_private.event_size = eventSize;
            state_private.event_has_comment = hasComment;
            state_private.event_type = static_cast<uint32_t>(MicroProfilerEventEntryType(entry));

            bool isUnknown = false;
            uint32_t eventType = 0;
            
            if (isExtraEntry)
            {
                isUnknown = true;
            }
            else
            {
                eventType = MicroProfilerEventEntryType(entry);
                mpsize_t extraEntriesCount = MicroProfilerEventEntryLastOffset(eventSize, hasComment);
                entries_count = extraEntriesCount + 1;
                if ((extraEntriesCount > 0) &&
                    (state_private.entry_num + extraEntriesCount >= state_private.log_portion_offset + state_private.log_portion_size))
                {
                    if (!AdvanceLogDataPtr(entries_count, state_private.entry_num))
                    {
                        Finish(true);
                        return false;
                    }
                }
            }

            if (!state_private.was_event_entry)
            {
                state.mutate_is_enter(false);
                state.mutate_is_exit(false);
                state.mutate_is_label(false);
                state.mutate_timer_id(0);
                state.mutate_timestamp(0);
            }            
            state.mutate_is_event(true);
            state.mutate_is_unknown(isUnknown);
            state.mutate_event_type(eventType);
        }
        else
        {
            if (state_private.was_event_entry)
            {
                state.mutate_is_unknown(false);
                state.mutate_is_event(false);
                state.mutate_event_type(0);
            }
            
            UpdateThreadStackState(stack_state);
            state.mutate_thread_prev_timestamp(prev_timestamp);
            state.mutate_is_enter(isEnter);
            state.mutate_is_exit(isExit);
            state.mutate_is_label(isLabel);
            state.mutate_timer_id(timerId);
            state.mutate_timestamp(isScope ? timestamp64.entry : 0);
        }

        if (state_private.reports_counter <= 1)
        {
            UpdateFrameAndThreadChanged();
        }

        state.mutate_entry_locator(entry_locator);
        state.mutate_entry_type_raw(entryType);
        state_private.was_event_entry = isEvent;
        state_private.reports_counter++;

        advanceEntryNum();
        return true;
    }

    return false;
}

bool MpLogIterator::AdvanceLogDataPtr(mpsize_t entries_count, mpsize_t start_entry_num)
{
    UnlockLogDataPtr(true);

    if (start_entry_num != GPRX::max_mpsize_t)
    {
        state_private.log_portion_offset = start_entry_num;
    }
    else
    {
        state_private.log_portion_offset += state_private.log_portion_size;
    }

    if (bUseChunkReads)
    {
        entries_count = ntd::min(
            ntd::max(entries_count, CHUNK_READ_ENTRY_COUNT),
            state_private.log_entries_num - state_private.log_portion_offset
        );
    }
    const mpsize_t element_size = sizeof(uint64_t);
    mpsize_t data_size = element_size * entries_count;
    mpsize_t out_ready_to_access_len = 0;

    Locator next_portion_locator;
    next_portion_locator.entry = state_private.log_start_locator + state_private.log_portion_offset;

    MpPushDataLock(m_session);
    data_lock_depth++;
    state_private.log_portion_ptr = reinterpret_cast<uint64_t*>(
        m_session->dumpContainer->GetDataByLocator(
            next_portion_locator, ContainerContext::TypeOfData::Default, &data_size,
            &out_ready_to_access_len
        )
    );
    state_private.log_portion_size = out_ready_to_access_len / element_size;
    return !!state_private.log_portion_ptr;
}

void MpLogIterator::Rewind(bool keepThreadStacks)
{
    state.mutate_started(false);
    state.mutate_finished(false);
    state_private.is_fast_path = false;
    // 'state_private' and 'state' will be reset the next time the iterator is used
    UnlockLogDataPtr();
    if (!keepThreadStacks)
        ResetThreadStacks();
}

void MpLogIterator::Detach()
{
    // The session is being destroyed with all its lock levels; forget ours rather than pop
    // them, so Rewind neither pops nor syncs the closing session through MpPopDataLock.
    data_lock_depth = 0;
    Rewind();
    m_session = nullptr;
}

uint8_t* MpLogIterator::GetConfigBuffer(mpsize_t minSize)
{
    if (config_buffer.size() < minSize)
        config_buffer.resize(minSize);
    return config_buffer.data();
}

inline bool MpLogIterator::IsGpuThread(uint32_t thread_id)
{
    return (thread_id == config_preprocessed.gpu_thread_id);
}

inline bool MpLogIterator::ShouldSkipThread()
{
    return config_preprocessed.thread_ids.size() && !config_preprocessed.thread_ids.Contains(state_private.thread_id);
}

void MpLogIterator::UnlockLogDataPtr(bool keep_offset)
{
    if (state_private.log_portion_ptr)
    {
        state_private.log_portion_ptr = nullptr;
        if (!keep_offset)
        {
            state_private.log_portion_offset = 0;
            state_private.log_portion_size = 0;
            state_private.current_thread_stack = nullptr;
            state_private.entry_num = 0;
            state_private.reports_counter = 0;
        }
    }
    while (data_lock_depth > 0)
    {
        MpPopDataLock(m_session);
        data_lock_depth--;
    }    
}

void MpLogIterator::Finish(bool bError)
{
    if (bError)
    {
        Rewind();
    }
    else
    {
        Rewind(true);
        config_preprocessed.processed_region_final_frame_id = state_private.frame_id;
        config_preprocessed.processed_region_last_frame_abs_id = state_private.last_frame_abs_id;
    }
    state.mutate_finished(true);
}

void MpLogIterator::OverrideFrameRange(bool use_override, uint32_t start_frame_id, uint32_t end_frame_id)
{
    config_preprocessed.override_frame_range = use_override;
    config_preprocessed.override_start_frame_id = start_frame_id;
    config_preprocessed.override_end_frame_id = end_frame_id;
    if (config_applied)
        PreprocessFrameRange();
}

void MpLogIterator::PreprocessFrameRange()
{
    uint32_t start_frame_id = config_preprocessed.override_frame_range
        ? config_preprocessed.override_start_frame_id
        : config_reader->start_frame_id();
    uint32_t end_frame_id = config_preprocessed.override_frame_range
        ? config_preprocessed.override_end_frame_id
        : config_reader->end_frame_id();

    uint32_t frameIdMin = MpGetFrameIdMin(m_session);
    uint32_t frameIdMax = MpGetFrameIdMax(m_session);

    config_preprocessed.start_frame_id = (start_frame_id > 0 && start_frame_id >= frameIdMin)
        ? start_frame_id : frameIdMin;
    config_preprocessed.end_frame_id = (end_frame_id > 0 && end_frame_id <= frameIdMax)
        ? end_frame_id : frameIdMax;
}

void MpLogIterator::PreprocessConfig()
{
    PreprocessFrameRange();

    auto populateIds = [](auto& target_container, const auto* source_ids, auto extra_logic)
    {
        target_container.SetFastLookupSize(0);
        target_container.Clear();
        if (source_ids)
        {
            for (uint32_t id : *source_ids)
            {
                target_container.Insert(id);
            }
        }
        extra_logic();
        target_container.RebuildToFit();
    };

    config_preprocessed.gpu_thread_id = MpGetThreadIdGpu(m_session);

    // Thread ids
    populateIds(config_preprocessed.thread_ids, config_reader->thread_ids(),
        [&]()
        {
            if (config_reader->skip_gpu_threads() && config_preprocessed.thread_ids.empty())
            {
                MpIterateAndReleaseArray(
                    MpGetThreadIds(m_session),
                    [&](MpArray* arr, mpsize_t i)
                    {
                        uint32_t id = MpGetIdFromArray(m_session, arr, i);
                        if (id != config_preprocessed.gpu_thread_id)
                        {
                            config_preprocessed.thread_ids.Insert(id);
                        }
                    }
                );
            }
        }
    );

    config_preprocessed.has_gpu_thread = config_preprocessed.thread_ids.Contains(config_preprocessed.gpu_thread_id);
    config_preprocessed.has_cpu_thread =
        (config_preprocessed.has_gpu_thread && config_preprocessed.thread_ids.size() > 1) ||
        (!config_preprocessed.has_gpu_thread && config_preprocessed.thread_ids.size() > 0);

    // Timer ids whitelist and blacklist
    populateIds(config_preprocessed.timer_ids, config_reader->timer_ids(),
        [&]()
        {
            const auto* group_ids = config_reader->group_ids();
            if (group_ids && !group_ids->empty())
            {
                for (uint32_t group_id : *group_ids)
                {
                    MpIterateAndReleaseArray(
                        MpGetTimerIdsByGroupId(m_session, group_id),
                        [&](MpArray* arr, mpsize_t i)
                        {
                            uint32_t id = MpGetIdFromArray(m_session, arr, i);
                            config_preprocessed.timer_ids.Insert(id);
                        }
                    );
                }
            }
        }
    );

    populateIds(config_preprocessed.skip_timer_ids, config_reader->skip_timer_ids(), []{});
}

bool MpLogIterator::ConfigureAndStart()
{
    UnlockLogDataPtr();
    state_private = StatePrivate{};
    state = LibMP::TypesNtd::LogIteratorState{};
    state.mutate_started(true);

    // Applying the config is deferred until the iterator is first used. This allows us to:
    // 1) make setting the configuration optional - the default config will be applied automatically
    // 2) avoid applying the default config and then immediately overwriting it with a user-defined one
    if (!config_applied)
    {
        if (!custom_config)
        {
            // Create a builder. It allocates its own memory.
            LibMP::FbBuilder builder;

            // Create your table with default values
            LibMP::TypesNtd::LogIteratorConfigBuilder config_builder(builder);
            auto config_offset = config_builder.Finish();

            // Finish the buffer
            builder.Finish(config_offset);

            // Get the pointer and size of the finished data
            uint8_t* buffer_data = builder.GetBufferPointer();
            mpsize_t buffer_size = builder.GetSize();

            // Copy the data to the final config buffer
            ntd::memcpy(GetConfigBuffer(buffer_size), buffer_data, buffer_size);
        }

#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-input-verification !LIBMP_TRUSTED_INPUTS_ONLY
        flatbuffers::Verifier verifier(config_buffer.data(), config_buffer.size());
        if (!verifier.VerifyBuffer<LibMP::TypesNtd::LogIteratorConfig>(nullptr))
            return false;
// @amalgamate: config-end untrusted-input-verification
#endif
        config_reader = flatbuffers::GetRoot<LibMP::TypesNtd::LogIteratorConfig>(config_buffer.data());
        PreprocessConfig();
        config_applied = true;
    }

    if (config_preprocessed.processed_region_final_frame_id > 0)
    {
        if ((config_preprocessed.processed_region_final_frame_id != config_preprocessed.start_frame_id)
            || config_reader->skip_thread_stack_resume())
        {
            ResetThreadStacks();
        }
    }
    
    state_private.last_frame_abs_id = config_preprocessed.processed_region_last_frame_abs_id;
    config_preprocessed.processed_region_final_frame_id = 0;
    config_preprocessed.processed_region_last_frame_abs_id = 0;

    state_private.frame_id = config_preprocessed.start_frame_id;

    if (config_preprocessed.start_frame_id == 0 || config_preprocessed.end_frame_id == 0 ||
        config_preprocessed.start_frame_id > config_preprocessed.end_frame_id)
    {
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename Iterator>
static void UnregisterIterator(ntd::vector<Iterator*>& iterators, Iterator* iter)
{
    for (Iterator*& entry : iterators)
    {
        if (entry == iter)
        {
            entry = iterators.back();
            iterators.pop_back();
            return;
        }
    }
}

// A detached iterator (m_session == nullptr) keeps answering queries about its own state,
// but fails every operation that needs the session.
MpLogIterator* MpCreateLogIterator(MpSession* session)
{
    CheckSync(session);
    if (!session)
        return nullptr;
    MpLogIterator* iter = MpLogIterator::Create(session);
    session->logIterators.push_back(iter);
    return iter;
}

void MpDeleteLogIterator(MpLogIterator* iter)
{
    if (!iter)
        return;
    if (iter->m_session)
    {
        CheckSync(iter->m_session);
        iter->Rewind();
        UnregisterIterator(iter->m_session->logIterators, iter);
    }
    ntd::Delete(iter);
}

bool MpStepLogIterator(MpLogIterator* iter)
{
    if (!iter || !iter->m_session)
        return false;
    CheckSync(iter->m_session);
    return iter->Step();
}

void MpRewindLogIterator(MpLogIterator* iter)
{
    if (!iter)
        return;
    CheckSync(iter->m_session);
    iter->Rewind();
}

void MpRewindLogIteratorTo(MpLogIterator* iter, uint32_t frameIdMin, uint32_t frameIdMax)
{
    if (!iter || !iter->m_session)
        return;
    CheckSync(iter->m_session);
    iter->OverrideFrameRange(true, frameIdMin, frameIdMax);
    iter->Rewind(true);
}

bool MpConfigureLogIterator(MpLogIterator* iter, bool useDefault)
{
    if (!iter || !iter->m_session)
        return false;
    CheckSync(iter->m_session);
    iter->custom_config = !useDefault;
    iter->config_applied = false;
    iter->OverrideFrameRange(false);
    iter->Rewind();
    return true;
}

uint8_t* MpGetLogIteratorConfigBuffer(MpLogIterator* iter, mpsize_t minSize)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    return iter->GetConfigBuffer(minSize);
}

const uint8_t* MpGetLogIteratorState(MpLogIterator* iter)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    return reinterpret_cast<const uint8_t*>(&iter->state);
}

const uint8_t* MpGetLogIteratorThreadStackState(MpLogIterator* iter, uint32_t threadId)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);

    auto& m = iter->thread_stacks;
    auto it = m.find(threadId);
    if (it == m.end())
        return nullptr;

    return reinterpret_cast<const uint8_t*>(&it->second.state);
}

const uint8_t* MpGetLogIteratorThreadStackElement(MpLogIterator* iter, uint32_t threadId, mpsize_t stackLevel)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);

    auto& m = iter->thread_stacks;
    auto it = m.find(threadId);
    if (it == m.end())
        return nullptr;

    auto& stack = it->second;
    if (stackLevel >= stack.elements.size())
        return nullptr;

    return reinterpret_cast<const uint8_t*>(&stack.elements[stackLevel].element);
}

const uint8_t* MpGetLogIteratorCurrentThreadStackElement(MpLogIterator* iter, mpsize_t stackLevel)
{
    if (!iter)
        return nullptr;
    return MpGetLogIteratorThreadStackElement(iter, iter->state_private.thread_id, stackLevel); // CheckSync is called internally
}

mpsize_t MpGetLogIteratorThreadStackElementStride(MpLogIterator* iter)
{
    if (!iter)
        return 0;
    CheckSync(iter->m_session);
    return sizeof(MpLogIterator::ThreadStackElementWrapper);
}

static const uint64_t* GetLogIteratorCurrentEntryStart(MpLogIterator* iter)
{
    auto& s = iter->state_private;
    const uint64_t consumed =
        static_cast<uint64_t>(s.step_entries_count) + s.log_portion_offset;
    if (!s.log_portion_ptr || s.entry_num < consumed)
        return nullptr;
    const uint64_t index = static_cast<uint64_t>(s.entry_num) - consumed;
    if (index >= s.log_portion_size ||
        s.step_entries_count > s.log_portion_size - index)
        return nullptr;
    return &s.log_portion_ptr[index];
}

const uint64_t* MpGetLogIteratorCurrentEntry(MpLogIterator* iter)
{
    return MpGetLogIteratorCurrentEntryAt(iter, 0);
}

mpsize_t MpGetLogIteratorCurrentEntryCount(MpLogIterator* iter)
{
    if (!iter)
        return 0;
    CheckSync(iter->m_session);
    return GetLogIteratorCurrentEntryStart(iter)
        ? iter->state_private.step_entries_count
        : 0;
}

const uint64_t* MpGetLogIteratorCurrentEntryAt(
    MpLogIterator* iter, mpsize_t entryIndex)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    auto& s = iter->state_private;
    if (entryIndex >= s.step_entries_count)
        return nullptr;
    const uint64_t* first = GetLogIteratorCurrentEntryStart(iter);
    return first ? first + entryIndex : nullptr;
}

uint32_t MpGetLogIteratorCurrentEntryPart(
    MpLogIterator* iter, mpsize_t entryIndex, uint32_t partIndex)
{
    if (partIndex > 1)
        return 0;
    const uint64_t* entry = MpGetLogIteratorCurrentEntryAt(iter, entryIndex);
    if (!entry)
        return 0;
    return partIndex == 0
        ? static_cast<uint32_t>(*entry)
        : static_cast<uint32_t>(*entry >> 32);
}

const uint64_t* MpGetLogIteratorCurrentEventInfo(MpLogIterator* iter, uint32_t* outEventType, uint32_t* outEventSize, uint32_t* outHasComment)
{
    if (outEventType)
        *outEventType = 0;
    if (outEventSize)
        *outEventSize = 0;
    if (outHasComment)
        *outHasComment = 0;
    if (!iter || !outEventType || !outEventSize || !outHasComment)
        return nullptr;
    CheckSync(iter->m_session);
    const auto& s = iter->state_private;
    *outEventType = s.event_type;
    *outEventSize = s.event_size;
    *outHasComment = bool_to_int(s.event_has_comment);
    return iter->state.is_event() ? MpGetLogIteratorCurrentEntry(iter) : nullptr;
}

static const MpLogIterator::ThreadStackElementSupplement*
GetLogIteratorScopeLabelSupplement(
    MpLogIterator* iter, uint32_t threadId, mpsize_t stackLevel)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    auto found = iter->thread_stacks.find(threadId);
    if (found == iter->thread_stacks.end())
        return nullptr;
    auto& stack = found->second;
    if (stackLevel >= stack.GetCappedDepth())
        return nullptr;
    return &stack.elements[stackLevel].supplement;
}

static uint64_t MpGetLabelOffsetFromEntry(uint64_t entry)
{
    return MicroProfilerLogType(entry) == MP_LOG_LABEL
        ? MicroProfilerLogTick(entry)
        : 0;
}

static uint64_t MpGetLogIteratorScopeLabelOffsetImpl(
    MpLogIterator* iter,
    uint32_t threadId,
    mpsize_t stackLevel,
    mpsize_t labelIndex)
{
    const auto* supplement = GetLogIteratorScopeLabelSupplement(iter, threadId, stackLevel);
    if (!supplement || labelIndex >= supplement->scope_label_count)
        return 0;
    return MpGetLabelOffsetFromEntry(supplement->scope_labels[labelIndex]);
}

static const MpLogIterator::ThreadStackElementSupplement*
GetLogIteratorCurrentScopeLabelSupplement(MpLogIterator* iter)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    auto* stack = iter->state_private.current_thread_stack;
    if (!stack)
        return nullptr;

    const mpsize_t depth = stack->GetCappedDepth();
    if (iter->state.is_exit())
    {
        if (iter->state.thread_stack_is_underflowed() ||
            iter->state.thread_stack_was_overflowed() ||
            depth >= stack->elements.size())
            return nullptr;
        return &stack->elements[depth].supplement;
    }

    if (depth == 0 || iter->state.thread_stack_is_overflowed())
        return nullptr;
    return &stack->elements[depth - 1].supplement;
}

static uint64_t MpGetLogIteratorCurrentScopeLabelOffsetImpl(
    MpLogIterator* iter, mpsize_t labelIndex)
{
    const auto* supplement = GetLogIteratorCurrentScopeLabelSupplement(iter);
    if (!supplement || labelIndex >= supplement->scope_label_count)
        return 0;
    return MpGetLabelOffsetFromEntry(supplement->scope_labels[labelIndex]);
}

const uint8_t* MpGetLogIteratorCurrentLabel(
    MpLogIterator* iter, uint32_t* outByteLength)
{
    if (outByteLength)
        *outByteLength = 0;
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    if (!iter->state.is_label())
        return nullptr;
    const uint64_t* entry = MpGetLogIteratorCurrentEntry(iter);
    if (!entry)
        return nullptr;
    return MpGetLabelFromEntry(iter->m_session, entry, outByteLength);
}

const uint64_t* MpGetLogIteratorCurrentLabelOffset(MpLogIterator* iter)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    if (!iter->state.is_label())
        return nullptr;
    const uint64_t* entry = MpGetLogIteratorCurrentEntry(iter);
    iter->label_offset_result = entry ? MpGetLabelOffsetFromEntry(*entry) : 0;
    return iter->label_offset_result ? &iter->label_offset_result : nullptr;
}

mpsize_t MpGetLogIteratorScopeLabelCount(
    MpLogIterator* iter, uint32_t threadId, mpsize_t stackLevel)
{
    const auto* supplement = GetLogIteratorScopeLabelSupplement(iter, threadId, stackLevel);
    return supplement ? GPRX::to_mpsize(supplement->scope_label_count) : mpsize_t{0};
}

mpsize_t MpGetLogIteratorCurrentScopeLabelCount(MpLogIterator* iter)
{
    const auto* supplement = GetLogIteratorCurrentScopeLabelSupplement(iter);
    return supplement ? GPRX::to_mpsize(supplement->scope_label_count) : mpsize_t{0};
}

const uint64_t* MpGetLogIteratorScopeLabelOffset(
    MpLogIterator* iter,
    uint32_t threadId,
    mpsize_t stackLevel,
    mpsize_t labelIndex)
{
    if (!iter)
        return nullptr;
    iter->label_offset_result = MpGetLogIteratorScopeLabelOffsetImpl(
        iter, threadId, stackLevel, labelIndex);
    return iter->label_offset_result ? &iter->label_offset_result : nullptr;
}

const uint64_t* MpGetLogIteratorCurrentScopeLabelOffset(
    MpLogIterator* iter, mpsize_t labelIndex)
{
    if (!iter)
        return nullptr;
    iter->label_offset_result = MpGetLogIteratorCurrentScopeLabelOffsetImpl(
        iter, labelIndex);
    return iter->label_offset_result ? &iter->label_offset_result : nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
struct MpCounterIterator
{
    static constexpr mpsize_t COUNTER_STACK_DEPTH_MAX = 20;
    static constexpr mpsize_t CONFIG_BUFFER_SIZE_DEFAULT = 1024;

    struct PreprocessedConfig
    {
        ntd::vector<uint32_t> root_counter_ids;
        mpsize_t counter_count = 0;
    };

    struct TraversalStackEntry
    {
        uint32_t counter_id;
        uint32_t first_child_id;
        uint32_t sibling_id;
        mpsize_t index_in_siblings;
    };

    MpSession* m_session = nullptr;

    ntd::vector<uint8_t> config_buffer;
    bool custom_config = false;
    bool config_applied = false;
    const LibMP::TypesNtd::CounterIteratorConfig* config_reader = nullptr;
    PreprocessedConfig config_preprocessed;

    // Private state
    ntd::vector<TraversalStackEntry> stack;
    mpsize_t current_root_index = 0;
    mpsize_t base_stack_depth = 0;
    mpssize_t prev_level = -1;

    // Public state
    LibMP::TypesNtd::CounterIteratorState state;
    ntd::vector<uint32_t> stack_ids;

    MpCounterIterator(MpSession* session)
        : m_session(session)
    {
        GetConfigBuffer();
        stack.reserve(COUNTER_STACK_DEPTH_MAX);
        stack_ids.resize(COUNTER_STACK_DEPTH_MAX);
    }

    static MpCounterIterator* Create(MpSession* session)
    {
        return ntd::New<MpCounterIterator>(session);
    }

    uint8_t* GetConfigBuffer(mpsize_t minSize = CONFIG_BUFFER_SIZE_DEFAULT);
    bool Step();
    void Rewind();
    void Detach();

private:
    void ResetState();
    void PreprocessConfig();
    bool ConfigureAndStart();
    bool NextRoot();
    bool NextNode(uint32_t nodeId, mpsize_t index_in_siblings);
    inline const LibMP::TypesNtd::CounterDesc* GetDesc(uint32_t counterId);
    inline bool RebuildStackToParent(const LibMP::TypesNtd::CounterDesc* startNodeDesc);
    void UpdatePublicState(uint32_t nodeId, const LibMP::TypesNtd::CounterDesc* desc, bool fullStackUpdate);
    bool PushToStack(uint32_t node_id, const LibMP::TypesNtd::CounterDesc* nodeDesc, mpsize_t index_in_siblings);
    void Finish();
};

uint8_t* MpCounterIterator::GetConfigBuffer(mpsize_t minSize)
{
    if (config_buffer.size() < minSize)
        config_buffer.resize(minSize);
    return config_buffer.data();
}

void MpCounterIterator::Rewind()
{
    state.mutate_started(false);
    state.mutate_finished(false);
}

void MpCounterIterator::Detach()
{
    Rewind();
    m_session = nullptr;
}

void MpCounterIterator::Finish()
{
    Rewind();
    state.mutate_finished(true);
}

void MpCounterIterator::ResetState()
{
    // Private
    stack.clear();
    current_root_index = 0;
    base_stack_depth = 0;
    prev_level = -1;
    
    // Public
    state = {};
}

void MpCounterIterator::PreprocessConfig()
{
    config_preprocessed.root_counter_ids.clear();

    if (custom_config && config_reader)
    {
        auto roots = config_reader->root_counter_ids();
        if (roots && roots->size() > 0)
        {
            // CounterIds defines the traversal universe, but custom roots only need
            // its count as a cycle-safe stack bound; avoid materializing the array.
            config_preprocessed.counter_count = MpGetCounterIdsCountInternal(m_session);
            for (auto id : *roots)
            {
                config_preprocessed.root_counter_ids.push_back(id);
            }
            return;
        }
    }

    // Default: materialize CounterIds because every descriptor must be checked for roots.
    MpArray* counterIdsArr = MpGetCounterIds(m_session);
    ScopeExit counter_ids_cleanup([&]() { MpReleaseArray(counterIdsArr); });
    const mpsize_t counterIdsArrSize = MpGetArraySize(counterIdsArr);
    config_preprocessed.counter_count = counterIdsArrSize;

    for (mpsize_t i = 0; i < counterIdsArrSize; i++)
    {
        uint32_t id = MpGetIdFromArray(m_session, counterIdsArr, i);
        const auto* desc = GetDesc(id);
        if (desc && desc->parent() == 0)
        {
            config_preprocessed.root_counter_ids.push_back(id);
        }
    }
}

bool MpCounterIterator::ConfigureAndStart()
{
    ResetState();
    if (!config_applied)
    {
        if (custom_config)
        {
#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-input-verification !LIBMP_TRUSTED_INPUTS_ONLY
            flatbuffers::Verifier verifier(config_buffer.data(), config_buffer.size());
            if (!verifier.VerifyBuffer<LibMP::TypesNtd::CounterIteratorConfig>(nullptr))
                return false;
// @amalgamate: config-end untrusted-input-verification
#endif
            config_reader = flatbuffers::GetRoot<LibMP::TypesNtd::CounterIteratorConfig>(config_buffer.data());
        }
        else
        {
            config_reader = nullptr;
        }
        PreprocessConfig();
        config_applied = true;
    }
    return true;
}

const LibMP::TypesNtd::CounterDesc* MpCounterIterator::GetDesc(uint32_t counterId)
{
    const uint8_t* ptr = MpGetCounterDesc(m_session, counterId);
    if (!ptr) return
        nullptr;
    return flatbuffers::GetRoot<LibMP::TypesNtd::CounterDesc>(ptr);
}

bool MpCounterIterator::PushToStack(uint32_t node_id, const LibMP::TypesNtd::CounterDesc* nodeDesc, mpsize_t index_in_siblings)
{
    if (stack.size() >= config_preprocessed.counter_count ||
        stack.size() >= static_cast<size_t>(GPRX::max_mpssize_t))
    {
        XASSERT_DIAG(false,
            "MpCounterIterator::PushToStack - counter hierarchy exceeds safe depth");
        return false;
    }

    stack.push_back({node_id, nodeDesc->first_child(), nodeDesc->sibling(), index_in_siblings});
    return true;
}

bool MpCounterIterator::RebuildStackToParent(const LibMP::TypesNtd::CounterDesc* startNodeDesc)
{
    const auto* pDesc = startNodeDesc;
    uint32_t parentId = pDesc->parent();
    if (parentId == 0)
        return true;

    if (stack.size() > GPRX::max_mpsize_t)
    {
        XASSERT_DIAG(false,
            "MpCounterIterator::RebuildStackToParent - stack count exceeds mpsize_t");
        return false;
    }

    mpsize_t startOffset = GPRX::to_mpsize(stack.size());
    mpsize_t ancestors_visited = 0;

    do
    {
        // A valid parent chain cannot contain more nodes than the capture's counter-ID set.
        // This also terminates cycles in malformed descriptor hierarchies.
        if (ancestors_visited >= config_preprocessed.counter_count ||
            stack.size() >= static_cast<size_t>(GPRX::max_mpssize_t))
        {
            XASSERT_DIAG(false,
                "MpCounterIterator::RebuildStackToParent - counter hierarchy exceeds safe depth");
            stack.resize(startOffset);
            return false;
        }

        uint32_t id = parentId;
        pDesc = GetDesc(id);
        if (!pDesc)
        {
            XASSERT_DIAG(false,
                "MpCounterIterator::RebuildStackToParent - counter parent ID has no descriptor");
            stack.resize(startOffset);
            return false;
        }
        stack.push_back({id, 0, 0, 0});
        ancestors_visited++;
        parentId = pDesc->parent();
    }
    while (parentId > 0);

    // Reverse the newly added portion in-place
    if (stack.size() > startOffset)
    {
        using StackDifference = decltype(stack.end() - stack.begin());
        ntd::reverse(
            stack.begin() + static_cast<StackDifference>(startOffset), stack.end());
    }

    return true;
}

void MpCounterIterator::UpdatePublicState(uint32_t nodeId, const LibMP::TypesNtd::CounterDesc* desc, bool fullStackUpdate)
{
    // Stack is not empty
    mpsize_t depth = GPRX::to_mpsize(stack.size());
    mpssize_t level = static_cast<mpssize_t>(depth) - 1;
    mpsize_t rel_level = (depth > base_stack_depth) ? depth - base_stack_depth - 1 : 0;
    mpsize_t index_in_siblings = stack.back().index_in_siblings;
    bool h_changed = (level != prev_level);
    prev_level = level;

    state.mutate_counter_id(nodeId);
    state.mutate_parent_id(desc->parent());
    state.mutate_level(static_cast<uint32_t>(level >= 0 ? level : 0));
    state.mutate_relative_level(rel_level);
    state.mutate_index_in_siblings(index_in_siblings);
    state.mutate_is_leaf(desc->first_child() == 0);
    state.mutate_hierarchy_changed(h_changed);
    state.mutate_counter_stack_is_overflowed(depth > stack_ids.size());

    mpsize_t validStackDepth = fullStackUpdate ? 0 : depth - 1;
    mpsize_t maxStackDepth = ntd::min(depth, GPRX::to_mpsize(stack_ids.size()));
    for (mpsize_t i = validStackDepth; i < maxStackDepth; i++)
    {
        stack_ids[i] = stack[i].counter_id;
    }
}

bool MpCounterIterator::NextRoot()
{
    while (current_root_index < config_preprocessed.root_counter_ids.size())
    {
        uint32_t rootId = config_preprocessed.root_counter_ids[current_root_index];

        DataLockHelper lock(m_session);
        const auto* desc = GetDesc(rootId);
        if (!desc)
        {
            current_root_index++;
            continue;
        }

        stack.clear();
        if (!RebuildStackToParent(desc))
        {
            current_root_index++;
            continue;
        }
        base_stack_depth = GPRX::to_mpsize(stack.size());

        state.mutate_root_level(base_stack_depth);

        if (!PushToStack(rootId, desc, 0))
        {
            current_root_index++;
            continue;
        }
        UpdatePublicState(rootId, desc, true);
        return true;
    }

    return false;
}

bool MpCounterIterator::NextNode(uint32_t nodeId, mpsize_t index_in_siblings)
{
    if (nodeId == 0)
        return false;

    uint32_t id = nodeId;
    const auto* desc = GetDesc(id);
    if (!desc)
        return false;

    if (!PushToStack(id, desc, index_in_siblings))
        return false;
    UpdatePublicState(id, desc, false);
    return true;
}

bool MpCounterIterator::Step()
{
    if (!state.started())
    {
        ConfigureAndStart();
        if (NextRoot())
        {
            state.mutate_started(true);
            return true;
        }
        else
        {
            Finish();
            return false;
        }
    }

    // 1. Child
    const auto& currentStackNode = stack.back();
    if (NextNode(currentStackNode.first_child_id, 0))
        return true;

    // 2. Backtrack / Sibling
    while (stack.size() > base_stack_depth)
    {
        TraversalStackEntry popped = stack.back();
        stack.pop_back();
        if (stack.size() == base_stack_depth)
            break;
        if (NextNode(popped.sibling_id, popped.index_in_siblings + 1))
            return true;
    }

    // 3. Next Root
    current_root_index++;
    bool res = NextRoot();
    if (!res)
        Finish();
    return res;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
MpCounterIterator* MpCreateCounterIterator(MpSession* session)
{
    CheckSync(session);
    if (!session)
        return nullptr;
    MpCounterIterator* iter = MpCounterIterator::Create(session);
    session->counterIterators.push_back(iter);
    return iter;
}

void MpDeleteCounterIterator(MpCounterIterator* iter)
{
    if (!iter)
        return;
    if (iter->m_session)
    {
        CheckSync(iter->m_session);
        UnregisterIterator(iter->m_session->counterIterators, iter);
    }
    ntd::Delete(iter);
}

bool MpConfigureCounterIterator(MpCounterIterator* iter, bool useDefault)
{
    if (!iter || !iter->m_session)
        return false;
    CheckSync(iter->m_session);
    iter->custom_config = !useDefault;
    iter->config_applied = false;
    iter->Rewind();
    return true;
}

uint8_t* MpGetCounterIteratorConfigBuffer(MpCounterIterator* iter, mpsize_t minSize)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    return iter->GetConfigBuffer(minSize);
}

bool MpStepCounterIterator(MpCounterIterator* iter)
{
    if (!iter || !iter->m_session)
        return false;
    CheckSync(iter->m_session);
    return iter->Step();
}

void MpRewindCounterIterator(MpCounterIterator* iter)
{
    if (!iter)
        return;
    CheckSync(iter->m_session);
    iter->Rewind();
}

const uint8_t* MpGetCounterIteratorState(MpCounterIterator* iter)
{
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    return reinterpret_cast<const uint8_t*>(&iter->state);
}

const uint32_t* MpGetCounterIteratorStack(MpCounterIterator* iter, mpsize_t* outSize)
{
    if (outSize)
        *outSize = 0;
    if (!iter)
        return nullptr;
    CheckSync(iter->m_session);
    if (outSize)
        *outSize = static_cast<mpsize_t>(iter->stack_ids.size());
    return iter->stack_ids.data();
}

uint32_t MpGetCounterIteratorStackElement(MpCounterIterator* iter, mpsize_t stackLevel)
{
    if (!iter)
        return 0;
    CheckSync(iter->m_session);
    if (stackLevel >= iter->stack_ids.size())
        return 0;
    return iter->stack_ids[stackLevel];
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void MpDetachIterators(MpSession* session)
{
    for (MpLogIterator* iter : session->logIterators)
        iter->Detach();
    for (MpCounterIterator* iter : session->counterIterators)
        iter->Detach();
    session->logIterators.clear();
    session->counterIterators.clear();
}

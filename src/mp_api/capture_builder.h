// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#ifndef LIBMP_EXCLUDE_CAPTURE_BUILDER
    #define LIBMP_EXCLUDE_CAPTURE_BUILDER 0
#endif

#if !LIBMP_EXCLUDE_CAPTURE_BUILDER
// @amalgamate: config-begin capture-builder !LIBMP_EXCLUDE_CAPTURE_BUILDER

#include "gprx_container/gprx.h"
#include "uid_ex.h"
#include "block_ex.h"
#include "kv_store_ex.h"
#include "c_api_internal.h"
#include "ntd_stdlib/memory.h"
#include "ntd_stdlib/tuple.h"
#include "ntd_stdlib/unordered_set.h"
#include "ntd_stdlib/utility.h"

namespace LibMP
{

////////////////////////////////////////////////////////////////////////////////////////////////////
struct BlockDesc
{
    uint32_t num = 0;
    GPRX::BlockContext* ptr = nullptr;
    ntd::unique_ptr<GPRX::KVStore> kv;
    GPRX::KVStore::Config* kvConfig = nullptr;

    BlockDesc(GPRX::KVStore::Config* c = nullptr)
        : kvConfig(c)
    {}

    uint32_t GetBlockId()
    {
        return ptr->headerPtr->block_id;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
struct CaptureBlockWriter
{
    struct BlockDescWrapper
    {
        BlockDesc* blockDesc;
        uint16_t type_id;
        uint8_t type_meta;

        BlockDescWrapper(BlockDesc* b, GPRX::BlockType t, LibMP::BlockTypeMeta m)
            : blockDesc(b)
            , type_id(static_cast<uint16_t>(t))
            , type_meta(static_cast<uint8_t>(m))
        {}

        BlockDescWrapper(BlockDesc* b, LibMP::BlockTypeEx t, LibMP::BlockTypeMeta m)
            : blockDesc(b)
            , type_id(static_cast<uint16_t>(t))
            , type_meta(static_cast<uint8_t>(m))
        {}
    };

    ntd::unique_ptr<GPRX::ContainerContext> live_container;
    bool is_init = false;
    uint32_t max_block_id = 0;
    GPRX::Uid uid;
    LibMP::ValueBufEx value_buf;

    LibMP::FbBuilder builder;
    uint8_t* obj_buf = nullptr;
    mpsize_t obj_size = 0;
    
    GPRX::KVStore* sel_kv = nullptr;
    GPRX::BlockContext* sel_block = nullptr;

public:
    // Block manipulation
    void InitializeBlocks(const BlockDescWrapper* blockDescriptors, mpsize_t blockCount, uint32_t firstBlockId = 1);
    void ResetBlocks(const BlockDescWrapper* blockDescriptors, mpsize_t blockCount, uint32_t firstBlockId = 1);
    void AddBlock(BlockDesc& b);
    void MarkBlockAsModified(BlockDesc& b);

    inline uint32_t GenBlockId()
    {
        max_block_id++;
        return max_block_id;
    }

    inline void IncBlockId(BlockDesc& b)
    {
        b.ptr->headerPtr->block_id = GenBlockId();
    }

    inline void ClearAndIncBlockId(BlockDesc& b)
    {
        b.ptr->ClearPayloadHard(true);
        IncBlockId(b);
    }

    inline void SwapBlocks(BlockDesc& b0, BlockDesc& b1)
    {
        auto& live_blocks = live_container->GetBlocks();
        ntd::swap(live_blocks[b0.num], live_blocks[b1.num]);
    }

    inline void UpdateKvStore(BlockDesc& b)
    {
        b.kv->ClearCaches();
        b.kv->SetBlock(b.ptr);
    }

    inline void ResetKvStore(BlockDesc& b)
    {
        b.ptr->ClearPayloadHard();
        UpdateKvStore(b);
        if (b.kvConfig)
        {
            b.kv->Initialize(*b.kvConfig);
        }
        else
        {
            b.kv->Initialize();
        }
    }

    // Data manipulation
    inline void SelectKV(BlockDesc& kv_block, BlockDesc& data_block)
    {
        sel_kv = kv_block.kv.get();
        SelectDataBlock(data_block);
    }

    inline void SelectDataBlock(BlockDesc& b)
    {
        sel_block = b.ptr;
    }

    inline void PutU32_Impl(uint32_t s, GPRX::KVStore::SetStrategy strategy)
    {
        uint32_t val = s;
        sel_kv->Put(&uid.entries[0], uid.size, &uid.precomputed_hash,
            &val, 1, strategy);
    }

    inline void PutU32(uint32_t s)
    {
        PutU32_Impl(s, GPRX::KVStore::SetStrategy::PutOrReplaceSingle);
    }

    inline void PutU64_Impl(uint64_t s, GPRX::KVStore::SetStrategy strategy)
    {
        value_buf.FromU64(s);
        sel_kv->Put(&uid.entries[0], uid.size, &uid.precomputed_hash,
            value_buf.elements_ptr, value_buf.size_in_u32, strategy);
    }

    inline void PutU64(uint64_t s)
    {
        PutU64_Impl(s, GPRX::KVStore::SetStrategy::PutOrReplaceSingle);
    }

    inline void PutArrayU32(uint32_t s)
    {
        PutU32_Impl(s, GPRX::KVStore::SetStrategy::PutToArray);
    }

    inline void PutArrayU64(uint64_t s)
    {
        PutU64_Impl(s, GPRX::KVStore::SetStrategy::PutToArray);
    }

    inline void PutArrayValueBuf()
    {
        sel_kv->Put(&uid.entries[0], uid.size, &uid.precomputed_hash,
            value_buf.elements_ptr, value_buf.size_in_u32,
            GPRX::KVStore::SetStrategy::PutToArray);
    }

    inline void PutSingleValueBuf()
    {
        sel_kv->Put(&uid.entries[0], uid.size, &uid.precomputed_hash,
            value_buf.elements_ptr, value_buf.size_in_u32,
            GPRX::KVStore::SetStrategy::PutOrReplaceSingle);
    }

    inline void PutTable()
    {
        GPRX::KVHelper::PutObj(uid, obj_buf, obj_size, *live_container, *sel_block, sel_kv);
    }

    template <typename T>
    inline void PutStruct(const T& s)
    {
        GPRX::KVHelper::PutObj(uid, reinterpret_cast<const uint8_t*>(&s), sizeof(s), *live_container, *sel_block, sel_kv);
    }    

    template <typename T>
    inline bool PutArrayStruct(const T& s)
    {
        if (!sel_kv || !sel_block)
            return false;
        GPRX::Locator locator = live_container->AppendPayloadDataAligned(*sel_block, reinterpret_cast<const uint8_t*>(&s), sizeof(s));
        if (!locator.entry)
            return false;
        value_buf.FromLocator(locator);
        return sel_kv->Put(&uid.entries[0], uid.size, &uid.precomputed_hash,
            value_buf.elements_ptr, value_buf.size_in_u32,
            GPRX::KVStore::SetStrategy::PutToArray);
    }

    template <typename T>
    inline const T* GetTable()
    {
        obj_size = 0;
        obj_buf = GPRX::KVHelper::GetObj(uid, &obj_size, *live_container, sel_kv);
        return obj_buf ? flatbuffers::GetRoot<T>(obj_buf) : nullptr;
    }

    template <typename Callable>
    inline void BuildTable(Callable createFn)
    {
        builder.Clear();
        auto root = createFn(builder);
        builder.FinishSizePrefixed(root);
        obj_buf = builder.GetBufferPointer();
        obj_size = builder.GetSize();
    }

    template <typename... Args>
    inline void SetUid(LibMP::QueryEx operation_id, bool isCompactFormatAllowed = false, Args... args)
    {
        uid.Set(GPRX::ToQuery(operation_id), isCompactFormatAllowed, args...);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
struct LiveContext : CaptureBlockWriter
{
    using BlockDescWrapper = CaptureBlockWriter::BlockDescWrapper;

    static constexpr uint32_t MAX_FRAMES_PER_KV_DEFAULT = 256;
    static constexpr uint32_t MAX_FRAMES_PER_KV_LIMIT = 512;
    static constexpr uint32_t MAX_BLOCK_ID_DIFF = GPRX::Block::BLOCK_ID_DIFF_OPTIMIZED_MAX;
    static constexpr mpsize_t LABEL_BUFFER_WRITE_GUARD_DIVISOR = 16;

    struct Config
    {
        uint32_t max_frames_per_kv = MAX_FRAMES_PER_KV_DEFAULT;
        bool bSkipCounters = false;
        bool bSkipCounterHistory = false;
        bool bExportOnChange = false;
        bool bRotateFrameBlocks = true;
        bool bProtectLiveLabelWrites = true;
    };

    struct State
    {
        bool bExportRequired = false;
        bool bChangeNotificationRequired = false;
    };

    ntd::unique_ptr<GPRX::DataContextMemChunks> live_datacontext;
    ntd::unordered_set<uint32_t> live_threads;

    Config config;

    int32_t lastFlipId = -1;
    uint64_t lastPlaceId = 0;
    State state;

    /*
     * Internal state
     */
    bool bInitialFillingCompleted = false;
    bool bCounterSamplesProvisional = false;
    uint32_t frames_counter = 0;
    uint32_t frames_in_kv0 = 0;
    uint32_t frames_in_kv1 = 0;

    uint32_t storedFramesCount = 0;
    uint32_t frameIdMin = 0;
    uint32_t frameIdMax = 0;

    /*
     * Fingerprints of the lists of long term descriptors
     */
    uint32_t timerUidGlobal = 0;
    uint32_t timerIdMax = 0;
    mpsize_t timerCount = 0;

    uint32_t counterIdMax = 0;
    mpsize_t counterCount = 0;

    uint32_t threadUidGlobal = 0;
    mpsize_t threadCount = 0;

    /*
     * Container blocks
     */
    GPRX::KVStore::Config config_frames    = { 11, 0, 10,  16 }; //   2k hashes
    GPRX::KVStore::Config config_counters  = { 10, 3, 12, 388 }; //   8k hashes + arrays of samples
    GPRX::KVStore::Config config_shortterm = {  9, 0, 12,  16 }; // 0.5k hashes
    GPRX::KVStore::Config config_longterm  = { 10, 4, 10, 132 }; //  16k hashes + arrays of ids

    BlockDesc magic_block;
    BlockDesc basicmeta_block;
    BlockDesc shortterm_block;
    BlockDesc longterm_block;
    BlockDesc logs0_block;
    BlockDesc logs1_block;
    BlockDesc frames0_block;
    BlockDesc frames1_block;
    BlockDesc kvframes0_block   = { &config_frames };
    BlockDesc kvframes1_block   = { &config_frames };
    BlockDesc kvcounters0_block = { &config_counters };
    BlockDesc kvcounters1_block = { &config_counters };
    BlockDesc kvshortterm_block = { &config_shortterm };
    BlockDesc kvlongterm_block  = { &config_longterm };
    BlockDesc labelbuffer_block;
    BlockDesc globalindex_block;

    const BlockDescWrapper blocks[16] =
    {
        { &magic_block,        GPRX::BlockType::Magic,       LibMP::BlockTypeMeta::Generic },
        { &basicmeta_block, LibMP::BlockTypeEx::BasicMeta,   LibMP::BlockTypeMeta::Generic },
        { &shortterm_block,    GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &longterm_block,     GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &logs0_block,        GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &logs1_block,        GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &frames0_block,      GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &frames1_block,      GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &kvframes0_block,    GPRX::BlockType::KVStore,     LibMP::BlockTypeMeta::Kv_Frames },
        { &kvframes1_block,    GPRX::BlockType::KVStore,     LibMP::BlockTypeMeta::Kv_Frames },
        { &kvcounters0_block,  GPRX::BlockType::KVStore,     LibMP::BlockTypeMeta::Kv_Counters },
        { &kvcounters1_block,  GPRX::BlockType::KVStore,     LibMP::BlockTypeMeta::Kv_Counters },
        { &kvshortterm_block,  GPRX::BlockType::KVStore,     LibMP::BlockTypeMeta::Kv_ShortTerm },
        { &kvlongterm_block,   GPRX::BlockType::KVStore,     LibMP::BlockTypeMeta::Kv_LongTerm },
        { &labelbuffer_block,  GPRX::BlockType::Data,        LibMP::BlockTypeMeta::Generic },
        { &globalindex_block,  GPRX::BlockType::GlobalIndex, LibMP::BlockTypeMeta::Generic },
    };

    struct LabelBufferRegionState
    {
        uint64_t start_offset = 0;
        uint64_t end_offset = 0;
        mpsize_t payload_offset = 0;
    };

    const uint8_t* label_buffer_data = nullptr;
    mpsize_t label_buffer_size = 0;
    uint64_t label_buffer_end_offset = 0;
    LabelBufferRegionState label_buffer_regions[2];
    mpsize_t label_buffer_region_count = 0;
    uint32_t label_regions_kv_block_id = 0;
    bool label_buffer_initialized = false;
    bool label_block_id_rotated = false;

public:
    inline void ResetKvStore(BlockDesc& b)
    {
        CaptureBlockWriter::ResetKvStore(b);
        if (&b == &kvshortterm_block)
            label_regions_kv_block_id = 0;
    }

    void Init();
    void Init(uint32_t firstBlockId);
    void AdjustKvConfigs();
    void CheckExport();
    void SetDataChanged();
    bool IsNotificationRequired();
    void ResetNotificationFlag();
    void SanitizeThreads(bool sanitizeOnly);
    void TrimLogs(uint32_t availFrameNum, uint32_t firstFrameAbsId);

    // Updates the zero-copy view of the producer's label ring and registers its
    // readable virtual ranges in the short-term KV store. Call this once per
    // refresh, after StartAddingFrame has prepared the short-term blocks.
    // Labels must be physically contiguous: when one would cross the ring end,
    // the producer consumes the remaining tail as padding and starts the label
    // at the ring beginning. An oldest portion of a full ring is reserved as a
    // write guard so producer writes made before the next refresh cannot
    // overwrite the exposed range under the expected per-refresh write bound.
    bool UpdateLabelBuffer(
        const uint8_t* data,
        mpsize_t ringSize,
        uint64_t endOffset);

    // Stores copied label ranges in the currently selected KV/data blocks. These
    // helpers are useful for importers and tests with non-ring label storage.
    bool PutLabelBufferRegion(uint64_t startOffset, const uint8_t* data, mpsize_t size);
    bool PutLabelBufferRegion(
        uint64_t startOffset,
        const uint8_t* firstData,
        mpsize_t firstSize,
        const uint8_t* secondData,
        mpsize_t secondSize);

    // Frame manipulation
    bool StartAddingFrame(bool longTermChanged, bool hasFrames);
    void FinishAddingFrame();
    // Counter-only samples advance the shared sample sequence without adding frame descriptors.
    bool StartAddingCounterSample(bool longTermChanged);
    void FinishAddingCounterSample();
    // Refreshes mutable blocks without advancing the sample sequence.
    bool StartReplacingCounterSnapshot(bool longTermChanged);
    uint32_t GetFramesInKv();
    uint32_t GetFrameIdMinBlock1();
    uint32_t GetFrameIdMinAbs();
    ntd::tuple<BlockDesc&, BlockDesc&, bool> GetKvAndLogBlockRefs(uint32_t frameId, uint32_t frameIdMinBlock1);
};

// Fixed segment storage shares only block-writing mechanics with LiveContext. It has no live
// export, rotating generations, label-ring views, or notification state.
struct SegmentCaptureContext : CaptureBlockWriter
{
    static constexpr uint32_t MAX_FRAMES = 256;

    GPRX::KVStore::Config config_frames    = { 11, 0, 10,  16 };
    GPRX::KVStore::Config config_counters  = { 10, 3, 12, 388 };
    GPRX::KVStore::Config config_shortterm = {  9, 0, 12,  16 };
    GPRX::KVStore::Config config_longterm  = { 10, 4, 10, 132 };

    BlockDesc basicmeta_block;
    BlockDesc shortterm_block;
    BlockDesc longterm_block;
    BlockDesc frames_block;
    BlockDesc kvframes_block   = { &config_frames };
    BlockDesc kvcounters_block = { &config_counters };
    BlockDesc kvshortterm_block = { &config_shortterm };
    BlockDesc kvlongterm_block  = { &config_longterm };
    BlockDesc segmentfinish_block;
    BlockDesc trackfinish_block;

    const BlockDescWrapper blocks[10] =
    {
        { &basicmeta_block,     LibMP::BlockTypeEx::BasicMeta,     LibMP::BlockTypeMeta::Generic },
        { &shortterm_block,     GPRX::BlockType::Data,             LibMP::BlockTypeMeta::Generic },
        { &longterm_block,      GPRX::BlockType::Data,             LibMP::BlockTypeMeta::Generic },
        { &frames_block,        GPRX::BlockType::Data,             LibMP::BlockTypeMeta::Generic },
        { &kvframes_block,      GPRX::BlockType::KVStore,          LibMP::BlockTypeMeta::Kv_Frames },
        { &kvcounters_block,    GPRX::BlockType::KVStore,          LibMP::BlockTypeMeta::Kv_Counters },
        { &kvshortterm_block,   GPRX::BlockType::KVStore,          LibMP::BlockTypeMeta::Kv_ShortTerm },
        { &kvlongterm_block,    GPRX::BlockType::KVStore,          LibMP::BlockTypeMeta::Kv_LongTerm },
        { &segmentfinish_block, LibMP::BlockTypeEx::SegmentFinish, LibMP::BlockTypeMeta::Generic },
        { &trackfinish_block,   LibMP::BlockTypeEx::TrackFinish,   LibMP::BlockTypeMeta::Generic },
    };

    uint32_t frames_counter = 0;
    uint32_t storedFramesCount = 0;
    uint32_t frameIdMin = 0;
    uint32_t frameIdMax = 0;

    explicit SegmentCaptureContext(uint32_t maxFrames = MAX_FRAMES);

    void Reset(uint32_t firstBlockId, uint32_t firstFrameId);
    void StartAddingFrame();
};

// A fixed, one-shot capture uses the same schema as LiveContext without the
// two-generation block rotation required by a live ring.  This lets callers
// capture the full 512-frame format limit without mutating an active live
// capture or moving the final frame generation into the stale block set.
struct CaptureContext : LiveContext
{
    explicit CaptureContext(uint32_t maxFrames = MAX_FRAMES_PER_KV_DEFAULT)
    {
        config.max_frames_per_kv = maxFrames;
        config.bRotateFrameBlocks = false;
        config.bProtectLiveLabelWrites = false;
    }
};

} // LibMP

// @amalgamate: config-end capture-builder
#endif // LIBMP_EXCLUDE_CAPTURE_BUILDER

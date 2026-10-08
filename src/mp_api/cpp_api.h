// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#pragma once

#ifndef EXCLUDE_CPP_API

#include "api_config.h"
#include "c_api.h" // Accepts LIBMP_TYPES_INTERNAL
#include "tools.h"
#if !LIBMP_PUBLIC_NTD_API || !NTD_TYPES_DISTINCT_FROM_STD
    #include "flatbuffer_types/mp_types_std.h" // ::MpTypesStd (std-flavor schema)
    #include <memory>
    #include <optional>
    #include <string>
    #include <vector>
#endif
#if (LIBMP_PUBLIC_NTD_API || defined(LIBMP_TYPES_INTERNAL)) && NTD_TYPES_DISTINCT_FROM_STD
    #include "flatbuffer_types/mp_types_ntd.h" // ::MpTypesNtd (distinct ntd-flavor schema)
    #include "ntd_stdlib/memory.h"
    #include "ntd_stdlib/optional.h"
    #include "ntd_stdlib/string.h"
    #include "ntd_stdlib/vector.h"
#endif
#include "gprx_container/xassert.h"
#include "ntd_stdlib/utility.h"
#include "ntd_stdlib/type_traits.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// C++ API
////////////////////////////////////////////////////////////////////////////////////////////////////

namespace LibMP
{

////////////////////////////////////////////////////////////////////////////////////////////////////
// Flavor traits
// The two coexisting flavors of the C++ API. A "flavor" bundles the boundary container namespace
// (std or ntd) with the matching FlatBuffers schema namespace (::MpTypesStd or ::MpTypesNtd). The API
// classes are templated on a flavor F so SessionT<StdFlavor> and SessionT<NtdFlavor> are distinct
// types with distinct symbols -- both can live in one binary (external std + internal ntd).
#define LIBMP_FB_TYPES(NS)                                                              \
    using GlobalDesc = NS::GlobalDesc;                                                  \
    using GeneralInfo = NS::GeneralInfo; using GeneralInfoT = NS::GeneralInfoT;         \
    using GroupDesc = NS::GroupDesc; using GroupDescT = NS::GroupDescT;                 \
    using TimerDesc = NS::TimerDesc; using TimerDescT = NS::TimerDescT;                 \
    using CounterDesc = NS::CounterDesc; using CounterDescT = NS::CounterDescT;         \
    using CounterSample = NS::CounterSample;                                            \
    using ThreadDesc = NS::ThreadDesc; using ThreadDescT = NS::ThreadDescT;             \
    using FrameDesc = NS::FrameDesc; using FrameDescT = NS::FrameDescT;                 \
    using AppType = NS::AppType;                                                        \
    using ContainerFormat = NS::ContainerFormat;                                       \
    using ContentMode = NS::ContentMode;                                               \
    using TrackDesc = NS::TrackDesc;                                                    \
    using SegmentDesc = NS::SegmentDesc;                                                \
    using U64Sample = NS::U64Sample;                                                    \
    using LogIteratorState = NS::LogIteratorState;                                      \
    using LogIteratorThreadStackElement = NS::LogIteratorThreadStackElement;            \
    using LogIteratorConfig = NS::LogIteratorConfig; using LogIteratorConfigT = NS::LogIteratorConfigT; \
    using CounterIteratorState = NS::CounterIteratorState;                              \
    using CounterIteratorConfig = NS::CounterIteratorConfig; using CounterIteratorConfigT = NS::CounterIteratorConfigT; \
    using ExperimentalStreamStatus = NS::ExperimentalStreamStatus;

#if !LIBMP_PUBLIC_NTD_API || !NTD_TYPES_DISTINCT_FROM_STD
    struct StdFlavor
    {
        template <class T> using vector = ::std::vector<T>;
        template <class T> using optional = ::std::optional<T>;
        template <class T> using unique_ptr = ::std::unique_ptr<T>;
        using string = ::std::string;
        template <class T, class... A> static ::std::unique_ptr<T> make_unique(A&&... a)
        { return ::std::make_unique<T>(::std::forward<A>(a)...); }
        LIBMP_FB_TYPES(::MpTypesStd)
    };
#endif

#if (LIBMP_PUBLIC_NTD_API || defined(LIBMP_TYPES_INTERNAL))
    #if NTD_TYPES_DISTINCT_FROM_STD
        struct NtdFlavor
        {
            template <class T> using vector = ::ntd::vector<T>;
            template <class T> using optional = ::ntd::optional<T>;
            template <class T> using unique_ptr = ::ntd::unique_ptr<T>;
            using string = ::ntd::string;
            template <class T, class... A> static ::ntd::unique_ptr<T> make_unique(A&&... a)
            { return ::ntd::make_unique<T>(::ntd::forward<A>(a)...); }
            LIBMP_FB_TYPES(::MpTypesNtd)
        };
    #else
        using NtdFlavor = StdFlavor; // ntd types equal std: collapse to one flavor.
    #endif

    // Which flavor a consumer sees as LibMP::Session etc.
    using DefaultFlavor = NtdFlavor;
#else
    using DefaultFlavor = StdFlavor;
#endif

#undef LIBMP_FB_TYPES

////////////////////////////////////////////////////////////////////////////////////////////////////
// Flavor-neutral helpers (one definition, no flavor)

// Internal handle: manages the C session lifetime
struct SessionHandle
{
    MpSession* session = nullptr;

    SessionHandle(MpSession* s)
        : session(s)
    {}

    ~SessionHandle()
    {
        if (session)
            MpCloseSession(session);
    }
};

template <class F, typename T, typename Generator>
typename F::template vector<T> FetchToVector(mpsize_t count, Generator&& gen)
{
    typename F::template vector<T> result;
    if (count > 0)
    {
        result.reserve(count);
        for (mpsize_t i = 0; i < count; i++)
        {
            result.push_back(gen(i));
        }
    }
    return result;
}

// Base array class (flavor-neutral: owns the C handle/array only)
class ArrayBase
{
protected:
    ntd::shared_ptr<SessionHandle> m_handle;
    MpArray* m_array;

public:
    ArrayBase(ntd::shared_ptr<SessionHandle> handle, MpArray* array)
        : m_handle(ntd::move(handle)), m_array(array)
    {}

    ~ArrayBase()
    {
        if (m_array)
            MpReleaseArray(m_array);
    }

    // Disable copy
    ArrayBase(const ArrayBase&) = delete;
    ArrayBase& operator=(const ArrayBase&) = delete;

    // Allow move
    ArrayBase(ArrayBase&& other) noexcept
        : m_handle(ntd::move(other.m_handle)), m_array(other.m_array)
    {
        other.m_array = nullptr;
    }

    mpsize_t Size() const
    {
        return m_array ? MpGetArraySize(m_array) : 0;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Flavored array wrappers (templated on flavor F)

// Generic array for scalar types. Automatically detects the return type of the GetterFunc.
template <class F, typename GetterFunc>
class ScalarArrayT : public ArrayBase
{
    GetterFunc m_getter;

public:
    ScalarArrayT(ntd::shared_ptr<SessionHandle> h, MpArray* a, GetterFunc g)
        : ArrayBase(ntd::move(h), a), m_getter(g)
    {}

    auto Get(mpsize_t index) const { return m_getter(m_handle->session, m_array, index); }
    auto Fetch(mpsize_t index) const { return Get(index); }
    auto operator[](mpsize_t index) const { return Get(index); }

    auto FetchAll() const
    {
        using ValueType = ntd::decay_t<decltype((*this)[0])>;
        return FetchToVector<F, ValueType>(Size(), [this](mpsize_t i) { return (*this)[i]; });
    }
};

// Array of FlatBuffer tables
template <class F, typename T, typename GetterFunc>
class TableArrayT : public ArrayBase
{
    GetterFunc m_getter;

public:
    TableArrayT(ntd::shared_ptr<SessionHandle> h, MpArray* a, GetterFunc g)
        : ArrayBase(ntd::move(h), a), m_getter(g) {}

    const T* Get(mpsize_t index) const
    {
        const uint8_t* ptr = m_getter(m_handle->session, m_array, index);
        return ptr ? flatbuffers::GetRoot<T>(ptr) : nullptr;
    }

    typename F::template optional<typename T::NativeTableType> Fetch(mpsize_t index) const
    {
        const T* ptr = Get(index);
        if (!ptr)
            return {};
        typename T::NativeTableType obj;
        ptr->UnPackTo(&obj);
        return obj;
    }

    const T* operator[](mpsize_t index) const { return Get(index); }

    typename F::template vector<typename T::NativeTableType> FetchAll() const
    {
        return FetchToVector<F, typename T::NativeTableType>(
            Size(),
            [this](mpsize_t i)
            {
                auto val = Fetch(i);
                return val ? ntd::move(*val) : typename T::NativeTableType();
            }
        );
    }
};

// Array of FlatBuffer structs
template <class F, typename T, typename GetterFunc>
class StructArrayT : public ArrayBase
{
    GetterFunc m_getter;

public:
    StructArrayT(ntd::shared_ptr<SessionHandle> h, MpArray* a, GetterFunc g)
        : ArrayBase(ntd::move(h), a), m_getter(g)
    {}

    const T* Get(mpsize_t index) const
    {
        const uint8_t* ptr = m_getter(m_handle->session, m_array, index);
        return reinterpret_cast<const T*>(ptr);
    }

    typename F::template optional<T> Fetch(mpsize_t index) const
    {
        const T* ptr = Get(index);
        if (!ptr)
            return {};
        return *ptr;
    }

    const T* operator[](mpsize_t index) const { return Get(index); }

    typename F::template vector<T> FetchAll() const
    {
        return FetchToVector<F, T>(
            Size(),
            [this](mpsize_t i)
            {
                auto val = Fetch(i);
                return val ? *val : T();
            }
        );
    }
};

// Dedicated class for counter samples to encapsulate array-specific search/retrieval
template <class F>
class CounterSampleArrayT : public StructArrayT<F, typename F::CounterSample, decltype(&MpGetCounterSampleFromArray)>
{
    using Base = StructArrayT<F, typename F::CounterSample, decltype(&MpGetCounterSampleFromArray)>;
    using CounterSample = typename F::CounterSample;

public:
    CounterSampleArrayT(ntd::shared_ptr<SessionHandle> h, MpArray* a)
        : Base(ntd::move(h), a, MpGetCounterSampleFromArray)
    {}

    const CounterSample* GetSampleByFrameId(uint32_t frameId) const
    {
        const uint8_t* ptr = MpGetCounterSampleFromArrayByFrameId(this->m_handle->session, this->m_array, frameId);
        return reinterpret_cast<const CounterSample*>(ptr);
    }

    typename F::template optional<CounterSample> FetchSampleByFrameId(uint32_t frameId) const
    {
        const auto* ptr = GetSampleByFrameId(frameId);
        if (!ptr)
            return {};
        return *ptr;
    }

    const CounterSample* GetLastSample() const
    {
        const uint8_t* ptr = MpGetLastCounterSampleFromArray(this->m_handle->session, this->m_array);
        return reinterpret_cast<const CounterSample*>(ptr);
    }

    typename F::template optional<CounterSample> FetchLastSample() const
    {
        const auto* ptr = GetLastSample();
        if (!ptr)
            return {};
        return *ptr;
    }
};

struct EventInfo
{
    const uint64_t* firstEntry = nullptr;
    uint32_t eventType = 0;
    uint32_t eventSize = 0;
    bool hasComment = false;
};

template <class F>
class LogIteratorT
{
    ntd::shared_ptr<SessionHandle> m_handle;
    MpLogIterator* m_iter = nullptr;

public:
    LogIteratorT(ntd::shared_ptr<SessionHandle> h);
    ~LogIteratorT();

    // Disable copy, allow move
    LogIteratorT(const LogIteratorT&) = delete;
    LogIteratorT& operator=(const LogIteratorT&) = delete;
    LogIteratorT(LogIteratorT&& other) noexcept;

    void Configure(const typename F::LogIteratorConfigT& config);
    bool Step();
    void Rewind();
    void RewindTo(uint32_t frameIdMin, uint32_t frameIdMax);
    const typename F::LogIteratorState* GetState() const;
    const typename F::LogIteratorThreadStackElement* GetThreadStackElement(uint32_t threadId, mpsize_t stackLevel) const;
    const typename F::LogIteratorThreadStackElement* GetCurrentThreadStackElement(mpsize_t stackLevel) const;
    const uint64_t* GetCurrentEntry() const;
    mpsize_t GetCurrentEntryCount() const;
    const uint64_t* GetCurrentEntryAt(mpsize_t entryIndex) const;
    uint32_t GetCurrentEntryPart(mpsize_t entryIndex, uint32_t partIndex) const;
    EventInfo GetCurrentEventInfo() const;
    // Zero-copy label pointers are session-owned. Treat them as valid only until
    // the next label lookup, cache invalidation, synchronization, or session close.
    const char* GetCurrentLabel(uint32_t* outByteLength = nullptr) const;
    typename F::template optional<typename F::string> FetchCurrentLabel() const;
    mpsize_t GetScopeLabelCount(uint32_t threadId, mpsize_t stackLevel) const;
    mpsize_t GetCurrentScopeLabelCount() const;
    const char* GetScopeLabel(
        uint32_t threadId, mpsize_t stackLevel, mpsize_t labelIndex,
        uint32_t* outByteLength = nullptr) const;
    typename F::template optional<typename F::string> FetchScopeLabel(
        uint32_t threadId, mpsize_t stackLevel, mpsize_t labelIndex) const;
    const char* GetCurrentScopeLabel(
        mpsize_t labelIndex, uint32_t* outByteLength = nullptr) const;
    typename F::template optional<typename F::string> FetchCurrentScopeLabel(mpsize_t labelIndex) const;
};

template <class F>
class CounterIteratorT
{
    ntd::shared_ptr<SessionHandle> m_handle;
    MpCounterIterator* m_iter = nullptr;

public:
    CounterIteratorT(ntd::shared_ptr<SessionHandle> h);
    ~CounterIteratorT();

    // Disable copy, allow move
    CounterIteratorT(const CounterIteratorT&) = delete;
    CounterIteratorT& operator=(const CounterIteratorT&) = delete;
    CounterIteratorT(CounterIteratorT&& other) noexcept;

    void Configure(const typename F::CounterIteratorConfigT& config);
    bool Step();
    void Rewind();
    const typename F::CounterIteratorState* GetState() const;
    const uint32_t* GetStack(mpsize_t* outSize) const;
    uint32_t GetStackElement(mpsize_t stackLevel) const;
};

// Flavor-neutral control surface
class Control
{
private:
    MpSession* session;

public:
    constexpr Control(MpSession* s = nullptr)
        : session(s)
    {}

    static Control& Global();

    static bool UseControlChannel(bool use);
    static bool IsBackendVersionCompatible();
    static bool IsBackendAccessible();
    static bool IsBackendReady();

    bool EnableProfiler(bool enable);
    bool EnableCapture(bool enable);
    bool ShowUI(bool show);
    bool SetFrameLimit(int32_t num);
    bool SetMemTargetMB(int32_t targetMB);
    bool CaptureToSlot(uint32_t targetSlotId, uint32_t maxFrameCount = 0);
    bool CaptureToSlot(uint32_t targetSlotId, uint32_t maxFrameCount,
        TypesNtd::ContainerFormat containerFormat);
#if !LIBMP_PUBLIC_NTD_API && NTD_TYPES_DISTINCT_FROM_STD
    bool CaptureToSlot(uint32_t targetSlotId, uint32_t maxFrameCount,
        MpTypesStd::ContainerFormat containerFormat);
#endif
    bool ResetSlot(uint32_t slotId);
};

namespace Experimental
{
namespace V1
{
bool StartFileStream(const DefaultFlavor::string& filenamePostfix = {}, uint32_t targetSlotId = 0);
bool StartMemoryStream(uint32_t targetSlotId);
bool StopStream();
bool SetStreamMarker(uint32_t marker, bool enabled);
DefaultFlavor::ExperimentalStreamStatus GetStreamStatus();
bool SetContentMode(TypesNtd::ContentMode contentMode);
#if !LIBMP_PUBLIC_NTD_API && NTD_TYPES_DISTINCT_FROM_STD
bool SetContentMode(MpTypesStd::ContentMode contentMode);
#endif
} // namespace V1
} // namespace Experimental

template <class F> class ScopedDataLockT;

template <class F>
class SessionT
{
    friend class ScopedDataLockT<F>;

    ntd::shared_ptr<SessionHandle> m_handle;

    template <class T> using opt = typename F::template optional<T>;
    template <class T> using vec = typename F::template vector<T>;
    template <class T> using uptr = typename F::template unique_ptr<T>;

    // Helper for FlatBuffer structs. Zero copy.
    template <typename T, typename Func, typename... Args>
    const T* GetStruct(Func c_api_func, Args... args) const
    {
        const uint8_t* ptr = c_api_func(m_handle->session, args...);
        return reinterpret_cast<const T*>(ptr);
    }

    // Helper for FlatBuffer structs. Full copy.
    template <typename T, typename Func, typename... Args>
    opt<T> FetchStruct(Func c_api_func, Args... args) const
    {
        const T* ptr = GetStruct<T>(c_api_func, args...);
        if (!ptr)
            return {};
        return *ptr;
    }

    // Helper for FlatBuffer tables. Zero copy.
    template <typename T, typename Func, typename... Args>
    const T* GetTable(Func c_api_func, Args... args) const
    {
        const uint8_t* ptr = c_api_func(m_handle->session, args...);
        return ptr ? flatbuffers::GetRoot<T>(ptr) : nullptr;
    }

    // Helper for FlatBuffer tables. Full copy.
    template <typename T, typename Func, typename... Args>
    opt<typename T::NativeTableType> FetchTable(Func c_api_func, Args... args) const
    {
        const T* ptr = GetTable<T>(c_api_func, args...);
        if (!ptr)
            return {};
        typename T::NativeTableType obj;
        ptr->UnPackTo(&obj);
        return obj;
    }

    // Helper for simple scalars. Automatically deduces return type from the C function signature.
    template <typename Func, typename... Args>
    auto GetScalar(Func c_api_func, Args... args) const
    {
        return c_api_func(m_handle->session, args...);
    }

public:
    // Readable, flavor-specific return types for the Get* array methods below.
    using IdsArray           = ScalarArrayT<F, decltype(&MpGetIdFromArray)>;
    using CounterSampleArray = CounterSampleArrayT<F>;
    using U64SamplesArray    = StructArrayT<F, typename F::U64Sample, decltype(&MpGetU64SampleFromArray)>;
    using TracksArray        = StructArrayT<F, typename F::TrackDesc, decltype(&MpGetTrackDescFromArray)>;
    using SegmentsArray      = StructArrayT<F, typename F::SegmentDesc, decltype(&MpGetSegmentDescFromArray)>;

    Control control;

    explicit SessionT(MpSession* session);

    static uptr<SessionT> Create(MpSession* session);
#if !LIBMP_TRUSTED_INPUTS_ONLY
    static uptr<SessionT> OpenFromDataContext(ntd::unique_ptr<GPRX::IDataContext> dataSource, mpsize_t cachingProxySize = 0);
    static uptr<SessionT> OpenFromMem(const uint8_t* mem, mpsize_t size);
    static uptr<SessionT> OpenFromFile(const typename F::string& fileName, mpsize_t cachingProxySize = 0);
    static uptr<SessionT> OpenFromExternalData(uint8_t* handle, bool useBlockFetch, mpsize_t cachingProxySize = 0);
    static uptr<SessionT> OpenFromExternalProvider(const GPRX::ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize = 0);
#endif
    static uptr<SessionT> OpenFromDataContextTrusted(ntd::unique_ptr<GPRX::IDataContext> dataSource, mpsize_t cachingProxySize = 0);
    static uptr<SessionT> OpenFromMemTrusted(const uint8_t* mem, mpsize_t size);
    static uptr<SessionT> OpenFromFileTrusted(const typename F::string& fileName, mpsize_t cachingProxySize = 0);
    static uptr<SessionT> OpenFromExternalDataTrusted(uint8_t* handle, bool useBlockFetch, mpsize_t cachingProxySize = 0);
    static uptr<SessionT> OpenFromExternalProviderTrusted(const GPRX::ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize = 0);
    bool SyncWithDataSource();
    bool SyncIfRequired();
    void SetSyncRequired();
    void SetClearCacheOnSyncRequired();
    bool IsValid();
    uint32_t GetDataFormatVersion();

    mpsize_t GetTrackCount() const;
    TracksArray GetTracks();
    vec<typename F::TrackDesc> FetchTracks();
    mpsize_t GetSegmentCount(mpsize_t trackIndex) const;
    SegmentsArray GetSegments(mpsize_t trackIndex);
    vec<typename F::SegmentDesc> FetchSegments(mpsize_t trackIndex);
    bool SelectTrack(mpsize_t trackIndex);
    bool SelectSegment(mpsize_t segmentIndex);

    MpSession* GetHandle();

    template <typename Func>
    bool WithDataLock(Func&& func) const;

    mpsize_t GetObjSize() const;
    void EnableObjectCache(bool kvL1Cache, bool descrCache, bool arrayCache, bool scalarCache);
    void ClearObjectCache();
    void EnableLabelCache(bool enabled);
    void ClearLabelCache();

    uint32_t FindGroupId(const char* nameMask, bool caseSensitive = false) const;
    vec<uint32_t> FindGroupIds(const char* nameMask, bool caseSensitive = false);
    uint32_t FindTimerId(const char* nameMask, bool caseSensitive = false) const;
    vec<uint32_t> FindTimerIds(const char* nameMask, bool caseSensitive = false);
    IdsArray GetTimerIdsByGroupId(uint32_t groupId);
    vec<uint32_t> FetchTimerIdsByGroupId(uint32_t groupId);
    IdsArray GetTimerIdsByGroupNames(const char* nameMask, bool caseSensitive = false);
    vec<uint32_t> FetchTimerIdsByGroupNames(const char* nameMask, bool caseSensitive = false);
    uint32_t FindCounterId(const char* nameMask, bool caseSensitive = false) const;
    vec<uint32_t> FindCounterIds(const char* nameMask, bool caseSensitive = false);
    uint32_t FindThreadId(const char* nameMask, bool caseSensitive = false) const;
    vec<uint32_t> FindThreadIds(const char* nameMask, bool caseSensitive = false);

    const typename F::GlobalDesc* GetGlobalDesc() const;
    opt<typename F::GlobalDesc> FetchGlobalDesc() const;
    const typename F::GeneralInfo* GetGeneralInfo() const;
    opt<typename F::GeneralInfoT> FetchGeneralInfo() const;

    uint32_t GetGroupIdMax() const;
    const typename F::GroupDesc* GetGroupDesc(uint32_t groupId) const;
    opt<typename F::GroupDescT> FetchGroupDesc(uint32_t groupId) const;

    IdsArray GetTimerIds();
    vec<uint32_t> FetchTimerIds();
    const typename F::TimerDesc* GetTimerDesc(uint32_t timerId) const;
    opt<typename F::TimerDescT> FetchTimerDesc(uint32_t timerId) const;

    IdsArray GetCounterIds();
    vec<uint32_t> FetchCounterIds();
    const typename F::CounterDesc* GetCounterDesc(uint32_t counterId) const;
    opt<typename F::CounterDescT> FetchCounterDesc(uint32_t counterId) const;
    CounterSampleArray GetCounterSamples(uint32_t counterId);
    vec<typename F::CounterSample> FetchCounterSamples(uint32_t counterId);
    const typename F::CounterSample* GetLastCounterSample(uint32_t counterId) const;
    opt<typename F::CounterSample> FetchLastCounterSample(uint32_t counterId) const;

    IdsArray GetThreadIds();
    vec<uint32_t> FetchThreadIds();
    const typename F::ThreadDesc* GetThreadDesc(uint32_t threadId) const;
    opt<typename F::ThreadDescT> FetchThreadDesc(uint32_t threadId) const;
    uint32_t GetThreadIdGpu() const;

    uint32_t GetFrameIdMin() const;
    uint32_t GetFrameIdMax() const;
    const typename F::FrameDesc* GetFrameDesc(uint32_t frameId) const;
    opt<typename F::FrameDescT> FetchFrameDesc(uint32_t frameId) const;


    U64SamplesArray GetPlaceIdSamples();
    vec<typename F::U64Sample> FetchPlaceIdSamples();
    U64SamplesArray GetUtcTimestampSamples();
    vec<typename F::U64Sample> FetchUtcTimestampSamples();

    LogIteratorT<F> CreateLogIterator() const;
    CounterIteratorT<F> CreateCounterIterator() const;
};

// RAII wrapper for data locks
template <class F>
class ScopedDataLockT
{
    ntd::shared_ptr<SessionHandle> m_handle;
    bool is_valid = false;

    void Init(ntd::shared_ptr<SessionHandle> h)
    {
        m_handle = ntd::move(h);
        if (m_handle && m_handle->session)
            is_valid = MpPushDataLock(m_handle->session);
        XASSERT_DIAG(is_valid, "ScopedDataLock: Failed to push data lock");
    }

public:
    explicit ScopedDataLockT(const typename F::template unique_ptr<SessionT<F>>& sessionPtr)
    {
        if (sessionPtr)
            Init(sessionPtr->m_handle);
    }

    explicit ScopedDataLockT(const SessionT<F>* sessionPtr)
    {
        if (sessionPtr)
            Init(sessionPtr->m_handle);
    }

    ~ScopedDataLockT()
    {
        if (is_valid && m_handle && m_handle->session)
            MpPopDataLock(m_handle->session);
    }

    bool IsValid() const { return is_valid; }

    // Disable copy
    ScopedDataLockT(const ScopedDataLockT&) = delete;
    ScopedDataLockT& operator=(const ScopedDataLockT&) = delete;

    // Allow move
    ScopedDataLockT(ScopedDataLockT&& other) noexcept = default;
};

template <class F>
template <typename Func>
inline bool SessionT<F>::WithDataLock(Func&& func) const
{
    ScopedDataLockT<F> lock(this);
    if (lock.IsValid())
        func();
    return lock.IsValid();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Public aliases for the selected API flavor
using Session = SessionT<DefaultFlavor>;
using LogIterator = LogIteratorT<DefaultFlavor>;
using CounterIterator = CounterIteratorT<DefaultFlavor>;
using ScopedDataLock = ScopedDataLockT<DefaultFlavor>;

template <typename G> using ScalarArray = ScalarArrayT<DefaultFlavor, G>;
template <typename T, typename G> using TableArray = TableArrayT<DefaultFlavor, T, G>;
template <typename T, typename G> using StructArray = StructArrayT<DefaultFlavor, T, G>;

using IdsArray = ScalarArray<decltype(&MpGetIdFromArray)>;
using U64SamplesArray = StructArray<typename DefaultFlavor::U64Sample, decltype(&MpGetU64SampleFromArray)>;
using TracksArray = StructArray<typename DefaultFlavor::TrackDesc, decltype(&MpGetTrackDescFromArray)>;
using SegmentsArray = StructArray<typename DefaultFlavor::SegmentDesc, decltype(&MpGetSegmentDescFromArray)>;
using CounterSampleArray = CounterSampleArrayT<DefaultFlavor>;

namespace Tools
{

// Copies the base URL used by CompressedHtml conversions. An empty string restores the
// build-provided default. This must not race a conversion.
bool SetHtmlAssetsBaseUrl(const DefaultFlavor::string& baseUrl);

template <class F>
bool ConvertToFile(
    SessionT<F>& session,
    typename F::ContainerFormat format,
    const typename F::string& outputName);
template <class F>
typename F::template vector<uint8_t> ConvertToBuffer(
    SessionT<F>& session,
    typename F::ContainerFormat format);

} // namespace Tools

struct Versions
{
    static uint32_t dataFormat;
    static uint32_t library;

    Versions() = default;
};

void Init();

} // LibMP

#endif //ifndef EXCLUDE_CPP_API

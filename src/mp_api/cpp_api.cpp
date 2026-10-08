// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

#ifndef EXCLUDE_CPP_API

#include "cpp_api_internal.h"
#include "gprx_container/data_context.h"
#include "gprx_container/stream_processor.h"
#include "gprx_container/utils.h"

namespace LibMP
{

namespace Tools
{

bool SetHtmlAssetsBaseUrl(const DefaultFlavor::string& baseUrl)
{
    return MpToolSetHtmlAssetsBaseUrl(
        baseUrl.data(), GPRX::to_mpsize(baseUrl.size()));
}

// Keep native Tools on stream processors instead of routing them through the C/WASM
// output callback; MpToolConvertSession adapts the same helper for language wrappers.
template <class F>
bool ConvertToFile(
    SessionT<F>& session,
    typename F::ContainerFormat format,
    const typename F::string& outputName)
{
#ifndef EXCLUDE_FILE_STREAM_PROCESSOR
    GPRX::StreamProcessorFile output;
    const ntd::string output_path(outputName.c_str(), outputName.size());
    return output.Open(output_path) &&
        ConvertSessionToStream(
            session.GetHandle(), static_cast<TypesNtd::ContainerFormat>(format), output);
#else
    (void)session;
    (void)format;
    (void)outputName;
    return false;
#endif
}

template <class F>
typename F::template vector<uint8_t> ConvertToBuffer(
    SessionT<F>& session,
    typename F::ContainerFormat format)
{
    GPRX::StreamProcessorChunkedMemory output;
    if (!ConvertSessionToStream(
            session.GetHandle(), static_cast<TypesNtd::ContainerFormat>(format), output))
        return {};
    ntd::vector<uint8_t> composed = output.GetComposedChunks();
    using Result = typename F::template vector<uint8_t>;
    if constexpr (ntd::is_same_v<Result, ntd::vector<uint8_t>>)
    {
        return composed;
    }
    else
    {
        Result result;
        result.resize(composed.size());
        if (!composed.empty())
            ntd::memcpy(result.data(), composed.data(), composed.size());
        return result;
    }
}

} // namespace Tools

////////////////////////////////////////////////////////////////////////////////////////////////////
// SessionT<F>
////////////////////////////////////////////////////////////////////////////////////////////////////
template <class F>
SessionT<F>::SessionT(MpSession* session)
    : control(session)
{
    m_handle = ntd::make_shared<SessionHandle>(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::Create(MpSession* session)
{
    if (!session)
        return nullptr;
    return F::template make_unique<SessionT<F>>(session);
}

#if !LIBMP_TRUSTED_INPUTS_ONLY
template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromDataContext(ntd::unique_ptr<GPRX::IDataContext> dataSource, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromDataContext(ntd::move(dataSource), cachingProxySize);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromMem(const uint8_t* mem, mpsize_t size)
{
    MpSession* session = MpOpenFromMem(mem, size);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromFile(const typename F::string& fileName, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromFile(fileName.c_str(), cachingProxySize);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromExternalData(uint8_t* handle, bool useBlockFetch, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromExternalData(handle, GPRX::bool_to_int(useBlockFetch), cachingProxySize);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromExternalProvider(const GPRX::ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromExternalProvider(ioInterface, handle, cachingProxySize);
    return Create(session);
}
#endif

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromDataContextTrusted(ntd::unique_ptr<GPRX::IDataContext> dataSource, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromDataContextTrusted(ntd::move(dataSource), cachingProxySize);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromMemTrusted(const uint8_t* mem, mpsize_t size)
{
    MpSession* session = MpOpenFromMemTrusted(mem, size);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromFileTrusted(const typename F::string& fileName, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromFileTrusted(fileName.c_str(), cachingProxySize);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromExternalDataTrusted(uint8_t* handle, bool useBlockFetch, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromExternalDataTrusted(handle, GPRX::bool_to_int(useBlockFetch), cachingProxySize);
    return Create(session);
}

template <class F>
typename F::template unique_ptr<SessionT<F>> SessionT<F>::OpenFromExternalProviderTrusted(const GPRX::ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize)
{
    MpSession* session = MpOpenFromExternalProviderTrusted(ioInterface, handle, cachingProxySize);
    return Create(session);
}

template <class F> bool SessionT<F>::SyncWithDataSource() { return MpSyncWithDataSource(m_handle->session); }
template <class F> bool SessionT<F>::SyncIfRequired() { return MpSyncIfRequired(m_handle->session); }
template <class F> void SessionT<F>::SetSyncRequired() { MpSetSyncRequired(m_handle->session); }
template <class F> void SessionT<F>::SetClearCacheOnSyncRequired() { MpSetClearCacheOnSyncRequired(m_handle->session); }
template <class F> bool SessionT<F>::IsValid() { return !!MpIsSessionValid(m_handle->session); }
template <class F> uint32_t SessionT<F>::GetDataFormatVersion() { return MpGetSessionDataFormatVersion(m_handle->session); }
template <class F> MpSession* SessionT<F>::GetHandle() { return m_handle->session; }

template <class F> mpsize_t SessionT<F>::GetTrackCount() const { return MpGetTrackCount(m_handle->session); }

template <class F>
StructArrayT<F, typename F::TrackDesc, decltype(&MpGetTrackDescFromArray)> SessionT<F>::GetTracks()
{
    return { m_handle, MpGetTracks(m_handle->session), MpGetTrackDescFromArray };
}

template <class F>
typename F::template vector<typename F::TrackDesc> SessionT<F>::FetchTracks()
{
    return GetTracks().FetchAll();
}

template <class F>
mpsize_t SessionT<F>::GetSegmentCount(mpsize_t trackIndex) const
{
    return MpGetSegmentCount(m_handle->session, trackIndex);
}

template <class F>
StructArrayT<F, typename F::SegmentDesc, decltype(&MpGetSegmentDescFromArray)>
SessionT<F>::GetSegments(mpsize_t trackIndex)
{
    return { m_handle, MpGetSegments(m_handle->session, trackIndex), MpGetSegmentDescFromArray };
}

template <class F>
typename F::template vector<typename F::SegmentDesc>
SessionT<F>::FetchSegments(mpsize_t trackIndex)
{
    return GetSegments(trackIndex).FetchAll();
}

template <class F> bool SessionT<F>::SelectTrack(mpsize_t trackIndex) { return MpSelectTrack(m_handle->session, trackIndex); }
template <class F> bool SessionT<F>::SelectSegment(mpsize_t segmentIndex) { return MpSelectSegment(m_handle->session, segmentIndex); }

////////////////////////////////////////////////////////////////////////////////////////////////////
template <class F>
void SessionT<F>::EnableObjectCache(bool kvL1Cache, bool descrCache, bool arrayCache, bool scalarCache)
{
    MpEnableObjectCache(
        m_handle->session,
        GPRX::bool_to_int(kvL1Cache),
        GPRX::bool_to_int(descrCache),
        GPRX::bool_to_int(arrayCache),
        GPRX::bool_to_int(scalarCache)
    );
}

template <class F> void SessionT<F>::ClearObjectCache() { MpClearObjectCache(m_handle->session); }
template <class F> void SessionT<F>::EnableLabelCache(bool enabled) { MpEnableLabelCache(m_handle->session, GPRX::bool_to_int(enabled)); }
template <class F> void SessionT<F>::ClearLabelCache() { MpClearLabelCache(m_handle->session); }

template <class F> mpsize_t SessionT<F>::GetObjSize() const { return MpGetObjSize(m_handle->session); }

// Search: Groups
template <class F>
uint32_t SessionT<F>::FindGroupId(const char* nameMask, bool caseSensitive) const
{
    return MpFindGroupId(m_handle->session, nameMask, caseSensitive);
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FindGroupIds(const char* nameMask, bool caseSensitive)
{
    return ScalarArrayT<F, decltype(&MpGetIdFromArray)>{ m_handle, MpFindGroupIds(m_handle->session, nameMask, caseSensitive), MpGetIdFromArray }.FetchAll();
}

// Search/Filter: Timers
template <class F>
uint32_t SessionT<F>::FindTimerId(const char* nameMask, bool caseSensitive) const
{
    return MpFindTimerId(m_handle->session, nameMask, caseSensitive);
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FindTimerIds(const char* nameMask, bool caseSensitive)
{
    return ScalarArrayT<F, decltype(&MpGetIdFromArray)>{ m_handle, MpFindTimerIds(m_handle->session, nameMask, caseSensitive), MpGetIdFromArray }.FetchAll();
}

template <class F>
ScalarArrayT<F, decltype(&MpGetIdFromArray)> SessionT<F>::GetTimerIdsByGroupId(uint32_t groupId)
{
    return { m_handle, MpGetTimerIdsByGroupId(m_handle->session, groupId), MpGetIdFromArray };
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FetchTimerIdsByGroupId(uint32_t groupId)
{
    return GetTimerIdsByGroupId(groupId).FetchAll();
}

template <class F>
ScalarArrayT<F, decltype(&MpGetIdFromArray)> SessionT<F>::GetTimerIdsByGroupNames(const char* nameMask, bool caseSensitive)
{
    return { m_handle, MpGetTimerIdsByGroupNames(m_handle->session, nameMask, caseSensitive), MpGetIdFromArray };
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FetchTimerIdsByGroupNames(const char* nameMask, bool caseSensitive)
{
    return GetTimerIdsByGroupNames(nameMask, caseSensitive).FetchAll();
}

// Search: Counters
template <class F>
uint32_t SessionT<F>::FindCounterId(const char* nameMask, bool caseSensitive) const
{
    return MpFindCounterId(m_handle->session, nameMask, caseSensitive);
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FindCounterIds(const char* nameMask, bool caseSensitive)
{
    return ScalarArrayT<F, decltype(&MpGetIdFromArray)>{ m_handle, MpFindCounterIds(m_handle->session, nameMask, caseSensitive), MpGetIdFromArray }.FetchAll();
}

// Search: Threads
template <class F>
uint32_t SessionT<F>::FindThreadId(const char* nameMask, bool caseSensitive) const
{
    return MpFindThreadId(m_handle->session, nameMask, caseSensitive);
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FindThreadIds(const char* nameMask, bool caseSensitive)
{
    return ScalarArrayT<F, decltype(&MpGetIdFromArray)>{ m_handle, MpFindThreadIds(m_handle->session, nameMask, caseSensitive), MpGetIdFromArray }.FetchAll();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
template <class F>
const typename F::GlobalDesc* SessionT<F>::GetGlobalDesc() const
{
    return this->template GetStruct<typename F::GlobalDesc>(MpGetGlobalDesc);
}

template <class F>
typename F::template optional<typename F::GlobalDesc> SessionT<F>::FetchGlobalDesc() const
{
    return this->template FetchStruct<typename F::GlobalDesc>(MpGetGlobalDesc);
}

template <class F>
const typename F::GeneralInfo* SessionT<F>::GetGeneralInfo() const
{
    return this->template GetTable<typename F::GeneralInfo>(MpGetGeneralInfo);
}

template <class F>
typename F::template optional<typename F::GeneralInfoT> SessionT<F>::FetchGeneralInfo() const
{
    return this->template FetchTable<typename F::GeneralInfo>(MpGetGeneralInfo);
}

template <class F> uint32_t SessionT<F>::GetGroupIdMax() const { return this->GetScalar(MpGetGroupIdMax); }

template <class F>
const typename F::GroupDesc* SessionT<F>::GetGroupDesc(uint32_t groupId) const
{
    return this->template GetTable<typename F::GroupDesc>(MpGetGroupDesc, groupId);
}

template <class F>
typename F::template optional<typename F::GroupDescT> SessionT<F>::FetchGroupDesc(uint32_t groupId) const
{
    return this->template FetchTable<typename F::GroupDesc>(MpGetGroupDesc, groupId);
}

template <class F>
ScalarArrayT<F, decltype(&MpGetIdFromArray)> SessionT<F>::GetTimerIds()
{
    return { m_handle, MpGetTimerIds(m_handle->session), MpGetIdFromArray };
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FetchTimerIds()
{
    return GetTimerIds().FetchAll();
}

template <class F>
const typename F::TimerDesc* SessionT<F>::GetTimerDesc(uint32_t timerId) const
{
    return this->template GetTable<typename F::TimerDesc>(MpGetTimerDesc, timerId);
}

template <class F>
typename F::template optional<typename F::TimerDescT> SessionT<F>::FetchTimerDesc(uint32_t timerId) const
{
    return this->template FetchTable<typename F::TimerDesc>(MpGetTimerDesc, timerId);
}

template <class F>
ScalarArrayT<F, decltype(&MpGetIdFromArray)> SessionT<F>::GetCounterIds()
{
    return { m_handle, MpGetCounterIds(m_handle->session), MpGetIdFromArray };
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FetchCounterIds()
{
    return GetCounterIds().FetchAll();
}

template <class F>
const typename F::CounterDesc* SessionT<F>::GetCounterDesc(uint32_t counterId) const
{
    return this->template GetTable<typename F::CounterDesc>(MpGetCounterDesc, counterId);
}

template <class F>
typename F::template optional<typename F::CounterDescT> SessionT<F>::FetchCounterDesc(uint32_t counterId) const
{
    return this->template FetchTable<typename F::CounterDesc>(MpGetCounterDesc, counterId);
}

template <class F>
CounterSampleArrayT<F> SessionT<F>::GetCounterSamples(uint32_t counterId)
{
    return { m_handle, MpGetCounterSamples(m_handle->session, counterId) };
}

template <class F>
typename F::template vector<typename F::CounterSample> SessionT<F>::FetchCounterSamples(uint32_t counterId)
{
    return GetCounterSamples(counterId).FetchAll();
}

template <class F>
const typename F::CounterSample* SessionT<F>::GetLastCounterSample(uint32_t counterId) const
{
    const uint8_t* ptr = MpGetLastCounterSample(m_handle->session, counterId);
    return reinterpret_cast<const typename F::CounterSample*>(ptr);
}

template <class F>
typename F::template optional<typename F::CounterSample> SessionT<F>::FetchLastCounterSample(uint32_t counterId) const
{
    const auto* ptr = GetLastCounterSample(counterId);
    if (!ptr)
        return {};
    return *ptr;
}

template <class F>
ScalarArrayT<F, decltype(&MpGetIdFromArray)> SessionT<F>::GetThreadIds()
{
    return { m_handle, MpGetThreadIds(m_handle->session), MpGetIdFromArray };
}

template <class F>
typename F::template vector<uint32_t> SessionT<F>::FetchThreadIds()
{
    return GetThreadIds().FetchAll();
}

template <class F>
const typename F::ThreadDesc* SessionT<F>::GetThreadDesc(uint32_t threadId) const
{
    return this->template GetTable<typename F::ThreadDesc>(MpGetThreadDesc, threadId);
}

template <class F>
typename F::template optional<typename F::ThreadDescT> SessionT<F>::FetchThreadDesc(uint32_t threadId) const
{
    return this->template FetchTable<typename F::ThreadDesc>(MpGetThreadDesc, threadId);
}

template <class F> uint32_t SessionT<F>::GetThreadIdGpu() const { return this->GetScalar(MpGetThreadIdGpu); }
template <class F> uint32_t SessionT<F>::GetFrameIdMin() const { return this->GetScalar(MpGetFrameIdMin); }
template <class F> uint32_t SessionT<F>::GetFrameIdMax() const { return this->GetScalar(MpGetFrameIdMax); }

template <class F>
const typename F::FrameDesc* SessionT<F>::GetFrameDesc(uint32_t frameId) const
{
    return this->template GetTable<typename F::FrameDesc>(MpGetFrameDesc, frameId);
}

template <class F>
typename F::template optional<typename F::FrameDescT> SessionT<F>::FetchFrameDesc(uint32_t frameId) const
{
    return this->template FetchTable<typename F::FrameDesc>(MpGetFrameDesc, frameId);
}

template <class F>
StructArrayT<F, typename F::U64Sample, decltype(&MpGetU64SampleFromArray)> SessionT<F>::GetPlaceIdSamples()
{
    return { m_handle, MpGetPlaceIdSamples(m_handle->session), MpGetU64SampleFromArray };
}

template <class F>
typename F::template vector<typename F::U64Sample> SessionT<F>::FetchPlaceIdSamples()
{
    return GetPlaceIdSamples().FetchAll();
}

template <class F>
StructArrayT<F, typename F::U64Sample, decltype(&MpGetU64SampleFromArray)> SessionT<F>::GetUtcTimestampSamples()
{
    return { m_handle, MpGetUtcTimestampSamples(m_handle->session), MpGetU64SampleFromArray };
}

template <class F>
typename F::template vector<typename F::U64Sample> SessionT<F>::FetchUtcTimestampSamples()
{
    return GetUtcTimestampSamples().FetchAll();
}

template <class F> LogIteratorT<F> SessionT<F>::CreateLogIterator() const { return LogIteratorT<F>(m_handle); }
template <class F> CounterIteratorT<F> SessionT<F>::CreateCounterIterator() const { return CounterIteratorT<F>(m_handle); }

////////////////////////////////////////////////////////////////////////////////////////////////////
// LogIteratorT<F>
////////////////////////////////////////////////////////////////////////////////////////////////////
template <class F>
LogIteratorT<F>::LogIteratorT(ntd::shared_ptr<SessionHandle> h)
    : m_handle(ntd::move(h))
{
    m_iter = MpCreateLogIterator(m_handle->session);
}

template <class F>
LogIteratorT<F>::~LogIteratorT()
{
    if (m_iter)
        MpDeleteLogIterator(m_iter);
}

template <class F>
LogIteratorT<F>::LogIteratorT(LogIteratorT&& other) noexcept
    : m_handle(ntd::move(other.m_handle)),
      m_iter(other.m_iter)
{
    other.m_iter = nullptr;
}

template <class F>
void LogIteratorT<F>::Configure(const typename F::LogIteratorConfigT& config)
{
    LibMP::FbBuilder builder;
    auto config_offset = F::LogIteratorConfig::Pack(builder, &config);
    builder.Finish(config_offset);

    uint8_t* buf = builder.GetBufferPointer();
    mpsize_t size = builder.GetSize();
    memcpy(MpGetLogIteratorConfigBuffer(m_iter, size), buf, size);
    MpConfigureLogIterator(m_iter, false);
}

template <class F> bool LogIteratorT<F>::Step() { return MpStepLogIterator(m_iter); }
template <class F> void LogIteratorT<F>::Rewind() { MpRewindLogIterator(m_iter); }
template <class F> void LogIteratorT<F>::RewindTo(uint32_t frameIdMin, uint32_t frameIdMax) { MpRewindLogIteratorTo(m_iter, frameIdMin, frameIdMax); }

template <class F>
const typename F::LogIteratorState* LogIteratorT<F>::GetState() const
{
    return reinterpret_cast<const typename F::LogIteratorState*>(MpGetLogIteratorState(m_iter));
}

template <class F>
const typename F::LogIteratorThreadStackElement* LogIteratorT<F>::GetThreadStackElement(uint32_t threadId, mpsize_t stackLevel) const
{
    return reinterpret_cast<const typename F::LogIteratorThreadStackElement*>(
        MpGetLogIteratorThreadStackElement(m_iter, threadId, stackLevel)
    );
}

template <class F>
const typename F::LogIteratorThreadStackElement* LogIteratorT<F>::GetCurrentThreadStackElement(mpsize_t stackLevel) const
{
    return reinterpret_cast<const typename F::LogIteratorThreadStackElement*>(
        MpGetLogIteratorCurrentThreadStackElement(m_iter, stackLevel)
    );
}

template <class F> const uint64_t* LogIteratorT<F>::GetCurrentEntry() const { return MpGetLogIteratorCurrentEntry(m_iter); }
template <class F> mpsize_t LogIteratorT<F>::GetCurrentEntryCount() const { return MpGetLogIteratorCurrentEntryCount(m_iter); }
template <class F> const uint64_t* LogIteratorT<F>::GetCurrentEntryAt(mpsize_t entryIndex) const { return MpGetLogIteratorCurrentEntryAt(m_iter, entryIndex); }
template <class F> uint32_t LogIteratorT<F>::GetCurrentEntryPart(mpsize_t entryIndex, uint32_t partIndex) const { return MpGetLogIteratorCurrentEntryPart(m_iter, entryIndex, partIndex); }

template <class F>
EventInfo LogIteratorT<F>::GetCurrentEventInfo() const
{
    EventInfo info;
    uint32_t hasComment = 0;
    info.firstEntry = MpGetLogIteratorCurrentEventInfo(m_iter, &info.eventType, &info.eventSize, &hasComment);
    info.hasComment = !!hasComment;
    return info;
}

template <class F>
const char* LogIteratorT<F>::GetCurrentLabel(uint32_t* outByteLength) const
{
    return reinterpret_cast<const char*>(
        MpGetLogIteratorCurrentLabel(m_iter, outByteLength));
}

template <class F>
typename F::template optional<typename F::string> LogIteratorT<F>::FetchCurrentLabel() const
{
    uint32_t byteLength = 0;
    const char* value = GetCurrentLabel(&byteLength);
    if (!value)
        return {};
    return typename F::string(value, byteLength);
}

template <class F>
mpsize_t LogIteratorT<F>::GetScopeLabelCount(uint32_t threadId, mpsize_t stackLevel) const
{
    return MpGetLogIteratorScopeLabelCount(m_iter, threadId, stackLevel);
}

template <class F>
mpsize_t LogIteratorT<F>::GetCurrentScopeLabelCount() const
{
    return MpGetLogIteratorCurrentScopeLabelCount(m_iter);
}

template <class F>
const char* LogIteratorT<F>::GetScopeLabel(
    uint32_t threadId, mpsize_t stackLevel, mpsize_t labelIndex,
    uint32_t* outByteLength) const
{
    const uint64_t* offset = MpGetLogIteratorScopeLabelOffset(
        m_iter, threadId, stackLevel, labelIndex);
    return reinterpret_cast<const char*>(MpGetLabelFromOffset(
        m_handle->session, offset, outByteLength));
}

template <class F>
typename F::template optional<typename F::string> LogIteratorT<F>::FetchScopeLabel(
    uint32_t threadId, mpsize_t stackLevel, mpsize_t labelIndex) const
{
    uint32_t byteLength = 0;
    const char* value = GetScopeLabel(
        threadId, stackLevel, labelIndex, &byteLength);
    if (!value)
        return {};
    return typename F::string(value, byteLength);
}

template <class F>
const char* LogIteratorT<F>::GetCurrentScopeLabel(
    mpsize_t labelIndex, uint32_t* outByteLength) const
{
    const uint64_t* offset = MpGetLogIteratorCurrentScopeLabelOffset(
        m_iter, labelIndex);
    return reinterpret_cast<const char*>(MpGetLabelFromOffset(
        m_handle->session, offset, outByteLength));
}

template <class F>
typename F::template optional<typename F::string>
LogIteratorT<F>::FetchCurrentScopeLabel(mpsize_t labelIndex) const
{
    uint32_t byteLength = 0;
    const char* value = GetCurrentScopeLabel(labelIndex, &byteLength);
    if (!value)
        return {};
    return typename F::string(value, byteLength);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// CounterIteratorT<F>
////////////////////////////////////////////////////////////////////////////////////////////////////
template <class F>
CounterIteratorT<F>::CounterIteratorT(ntd::shared_ptr<SessionHandle> h)
    : m_handle(ntd::move(h))
{
    m_iter = MpCreateCounterIterator(m_handle->session);
}

template <class F>
CounterIteratorT<F>::~CounterIteratorT()
{
    if (m_iter)
        MpDeleteCounterIterator(m_iter);
}

template <class F>
CounterIteratorT<F>::CounterIteratorT(CounterIteratorT&& other) noexcept
    : m_handle(ntd::move(other.m_handle)), m_iter(other.m_iter)
{
    other.m_iter = nullptr;
}

template <class F>
void CounterIteratorT<F>::Configure(const typename F::CounterIteratorConfigT& config)
{
    LibMP::FbBuilder builder;
    auto config_offset = F::CounterIteratorConfig::Pack(builder, &config);
    builder.Finish(config_offset);

    uint8_t* buf = builder.GetBufferPointer();
    mpsize_t size = builder.GetSize();

    memcpy(MpGetCounterIteratorConfigBuffer(m_iter, size), buf, size);
    MpConfigureCounterIterator(m_iter, false);
}

template <class F> bool CounterIteratorT<F>::Step() { return MpStepCounterIterator(m_iter); }
template <class F> void CounterIteratorT<F>::Rewind() { MpRewindCounterIterator(m_iter); }

template <class F>
const typename F::CounterIteratorState* CounterIteratorT<F>::GetState() const
{
    return reinterpret_cast<const typename F::CounterIteratorState*>(MpGetCounterIteratorState(m_iter));
}

template <class F>
const uint32_t* CounterIteratorT<F>::GetStack(mpsize_t* outSize) const
{
    return MpGetCounterIteratorStack(m_iter, outSize);
}

template <class F>
uint32_t CounterIteratorT<F>::GetStackElement(mpsize_t stackLevel) const
{
    return MpGetCounterIteratorStackElement(m_iter, stackLevel);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Explicit instantiation: emit only the flavors this build actually uses.
//   * StdFlavor when the public boundary is std (!LIBMP_PUBLIC_NTD_API), or when ntd types equal
//     default-allocator std types (!NTD_TYPES_DISTINCT_FROM_STD) so it is the single collapsed
//     flavor. Not emitted in a fully-ntd build -- nothing there uses std boundary containers.
//   * NtdFlavor whenever it is a distinct family (std implementations using
//     ntd::allocator, or the reserved embedded mode) -- used internally and,
//     under LIBMP_PUBLIC_NTD_API, publicly.
////////////////////////////////////////////////////////////////////////////////////////////////////
#if !LIBMP_PUBLIC_NTD_API || !NTD_TYPES_DISTINCT_FROM_STD
    template class SessionT<StdFlavor>;
    template class LogIteratorT<StdFlavor>;
    template class CounterIteratorT<StdFlavor>;
    template bool Tools::ConvertToFile<StdFlavor>(
        SessionT<StdFlavor>&, StdFlavor::ContainerFormat, const StdFlavor::string&);
    template StdFlavor::vector<uint8_t> Tools::ConvertToBuffer<StdFlavor>(
        SessionT<StdFlavor>&, StdFlavor::ContainerFormat);
#endif
#if NTD_TYPES_DISTINCT_FROM_STD
    template class SessionT<NtdFlavor>;
    template class LogIteratorT<NtdFlavor>;
    template class CounterIteratorT<NtdFlavor>;
    template bool Tools::ConvertToFile<NtdFlavor>(
        SessionT<NtdFlavor>&, NtdFlavor::ContainerFormat, const NtdFlavor::string&);
    template NtdFlavor::vector<uint8_t> Tools::ConvertToBuffer<NtdFlavor>(
        SessionT<NtdFlavor>&, NtdFlavor::ContainerFormat);
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// Flavor-neutral (one definition)
////////////////////////////////////////////////////////////////////////////////////////////////////
Control& Control::Global()
{
    static Control global;
    return global;
}

bool Control::UseControlChannel(bool use) { return !!MpUseControlChannel(GPRX::bool_to_int(use)); }
bool Control::IsBackendVersionCompatible() { return !!MpIsBackendVersionCompatible(); }
bool Control::IsBackendAccessible() { return !!MpIsBackendAccessible(); }
bool Control::IsBackendReady() { return !!MpIsBackendReady(); }
bool Control::EnableProfiler(bool enable) { return !!MpControlEnableProfiler(session, GPRX::bool_to_int(enable)); }
bool Control::EnableCapture(bool enable) { return !!MpControlEnableCapture(session, GPRX::bool_to_int(enable)); }
bool Control::ShowUI(bool show) { return !!MpControlShowUI(session, GPRX::bool_to_int(show)); }
bool Control::SetFrameLimit(int32_t num) { return !!MpControlSetFrameLimit(session, num); }
bool Control::SetMemTargetMB(int32_t targetMB) { return !!MpControlSetMemTargetMB(session, targetMB); }
bool Control::CaptureToSlot(uint32_t targetSlotId, uint32_t maxFrameCount)
{
    return CaptureToSlot(targetSlotId, maxFrameCount, TypesNtd::ContainerFormat_Raw);
}
bool Control::CaptureToSlot(uint32_t targetSlotId, uint32_t maxFrameCount, TypesNtd::ContainerFormat containerFormat)
{
    return !!MpControlCaptureToSlot(session, targetSlotId, maxFrameCount, static_cast<uint32_t>(containerFormat));
}
#if !LIBMP_PUBLIC_NTD_API && NTD_TYPES_DISTINCT_FROM_STD
bool Control::CaptureToSlot(uint32_t targetSlotId, uint32_t maxFrameCount, MpTypesStd::ContainerFormat containerFormat)
{
    return !!MpControlCaptureToSlot(session, targetSlotId, maxFrameCount, static_cast<uint32_t>(containerFormat));
}
#endif
bool Control::ResetSlot(uint32_t slotId) { return !!MpControlResetSlot(session, slotId); }

namespace Experimental
{
namespace V1
{
bool StartFileStream(const DefaultFlavor::string& filenamePostfix, uint32_t targetSlotId)
{
    return !!MpControlExperimentalV1StartFileStream(
        filenamePostfix.data(), GPRX::to_mpsize(filenamePostfix.size()), targetSlotId);
}

bool StartMemoryStream(uint32_t targetSlotId)
{
    return !!MpControlExperimentalV1StartMemoryStream(targetSlotId);
}

bool StopStream()
{
    return !!MpControlExperimentalV1StopStream();
}

bool SetStreamMarker(uint32_t marker, bool enabled)
{
    return !!MpControlExperimentalV1SetStreamMarker(marker, GPRX::bool_to_int(enabled));
}

DefaultFlavor::ExperimentalStreamStatus GetStreamStatus()
{
    const auto* status = reinterpret_cast<const DefaultFlavor::ExperimentalStreamStatus*>(
        MpControlExperimentalV1GetStreamStatus());
    return status ? *status : DefaultFlavor::ExperimentalStreamStatus();
}

bool SetContentMode(TypesNtd::ContentMode contentMode)
{
    return !!MpControlExperimentalV1SetContentMode(static_cast<uint32_t>(contentMode));
}
#if !LIBMP_PUBLIC_NTD_API && NTD_TYPES_DISTINCT_FROM_STD
bool SetContentMode(MpTypesStd::ContentMode contentMode)
{
    return !!MpControlExperimentalV1SetContentMode(static_cast<uint32_t>(contentMode));
}
#endif
} // namespace V1
} // namespace Experimental

////////////////////////////////////////////////////////////////////////////////////////////////////
void Init()
{
    MpInit();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t Versions::dataFormat = MpGetDataFormatVersion();
uint32_t Versions::library = MpGetLibraryVersion();

} // LibMP

#endif //ifndef EXCLUDE_CPP_API

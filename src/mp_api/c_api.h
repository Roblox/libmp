// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

/*
 * Changelog
 *
 * # 09.2026
 * - Added track and segment selection for long captures.
 * - Added label retrieval and caching.
 * - Expanded language wrapper support.
 *
 * # 08.2026
 * - Added support for replacement memory allocators.
 * - Added support for source amalgamation.
 *
 * # 05.2026
 * - Caching layer and control channel introduced.
 * - Counters, thread log iterator, and scope stacks reworked.
 *
 * # 01.2026
 *   Initial implementation.
 */

#pragma once

#include "gprx_container/types.h"
#include "ntd_stdlib/memory.h"
#include "flatbuffer_types/mp_types.h" // Accepts LIBMP_TYPES_INTERNAL

////////////////////////////////////////////////////////////////////////////////////////////////////
// C-style API
////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(LIBMP_WASM_BUILD)
    #include <emscripten/emscripten.h>
    #define MP_API_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(LIBMP_MODULE_BUILD)
    #define MP_API_EXPORT __attribute__((used, visibility("default")))
#else
    #define MP_API_EXPORT
#endif

#ifndef ENABLE_EXTERNAL_CLEAR_OBJCACHE_CALLBACK
#ifdef LIBMP_MODULE_BUILD
    #define ENABLE_EXTERNAL_CLEAR_OBJCACHE_CALLBACK 1
#else
    #define ENABLE_EXTERNAL_CLEAR_OBJCACHE_CALLBACK 0
#endif
#endif

#ifndef ENABLE_EXTERNAL_SESSION_NOTIFICATION_CALLBACK
#ifdef LIBMP_MODULE_BUILD
    #define ENABLE_EXTERNAL_SESSION_NOTIFICATION_CALLBACK 1
#else
    #define ENABLE_EXTERNAL_SESSION_NOTIFICATION_CALLBACK 0
#endif
#endif

struct MpSession;
struct MpArray;
struct MpLogIterator;
struct MpCounterIterator;

namespace GPRX
{
    class IDataContext;
    class ContainerContext;
    struct ExternalIO;
}

namespace LibMP
{
    class ControlChannel;
}

typedef void (*ClearObjCacheCallback)(
    uint8_t* handle, const uint32_t* bucketIds, mpsize_t bucketCount);
typedef void (*SessionNotificationCallback)(uint8_t* handle, uint32_t notificationFlags);

enum MpSessionNotificationFlags : uint32_t
{
    MP_SESSION_NOTIFICATION_LABEL_REGIONS_CHANGED = 1u << 0,
    MP_SESSION_NOTIFICATION_LABEL_CACHE_RESET = 1u << 1,
};

#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-session-api !LIBMP_TRUSTED_INPUTS_ONLY
MpSession* MpOpenFromDataContext(ntd::unique_ptr<GPRX::IDataContext> dataSource, mpsize_t cachingProxySize = 0);
MpSession* MpOpenFromExternalProvider(const GPRX::ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize = 0);
// @amalgamate: config-end untrusted-session-api
#endif
MpSession* MpOpenFromDataContextTrusted(ntd::unique_ptr<GPRX::IDataContext> dataSource, mpsize_t cachingProxySize = 0);
MpSession* MpOpenFromExternalProviderTrusted(const GPRX::ExternalIO* ioInterface, uint8_t* handle, mpsize_t cachingProxySize = 0);
GPRX::ContainerContext* MpGetContainerFromSession(MpSession* session);
void MpSetClearObjCacheCallback(
    MpSession* session, ClearObjCacheCallback fn, uint8_t* handle = nullptr);
// Notifications are synchronous. Callbacks should record their flags and defer
// queries against the session until after the notifying API call returns.
void MpSetSessionNotificationCallback(
    MpSession* session, SessionNotificationCallback fn, uint8_t* handle = nullptr);
LibMP::ControlChannel* MpGetControlChannel();

extern "C"
{

void LibMpExtClearObjCacheCallback(uint8_t* handle, const uint32_t* bucketIds, mpsize_t bucketCount);
void LibMpExtSessionNotificationCallback(uint8_t* handle, uint32_t notificationFlags);

MP_API_EXPORT void MpInit();
MP_API_EXPORT uint32_t MpGetMemUsed();
MP_API_EXPORT uint32_t MpGetMemReserved();
#if !LIBMP_TRUSTED_INPUTS_ONLY
// @amalgamate: config-begin untrusted-session-api !LIBMP_TRUSTED_INPUTS_ONLY
MP_API_EXPORT MpSession* MpOpenFromMem(const uint8_t* mem, mpsize_t size);
MP_API_EXPORT MpSession* MpOpenFromFile(const char* file_name, mpsize_t cachingProxySize = 0);
MP_API_EXPORT MpSession* MpOpenFromExternalData(uint8_t* handle, uint32_t use_block_fetch = 0, mpsize_t cachingProxySize = 0);
// @amalgamate: config-end untrusted-session-api
#endif
MP_API_EXPORT MpSession* MpOpenFromMemTrusted(const uint8_t* mem, mpsize_t size);
MP_API_EXPORT MpSession* MpOpenFromFileTrusted(const char* file_name, mpsize_t cachingProxySize = 0);
MP_API_EXPORT MpSession* MpOpenFromExternalDataTrusted(uint8_t* handle, uint32_t use_block_fetch = 0, mpsize_t cachingProxySize = 0);
MP_API_EXPORT bool MpCloseSession(MpSession* session);
MP_API_EXPORT bool MpSyncWithDataSource(MpSession* session);
MP_API_EXPORT bool MpSyncIfRequired(MpSession* session);
MP_API_EXPORT void MpSetSyncRequired(MpSession* session);
MP_API_EXPORT void MpSetClearCacheOnSyncRequired(MpSession* session);
MP_API_EXPORT uint32_t MpGetDataFormatVersion();
MP_API_EXPORT uint32_t MpGetLibraryVersion();

// Streams a converted container through LibMpExtToolOutput. The callback may be
// invoked multiple times. The format value matches a LibMP::Enums::ContainerFormat constant.
MP_API_EXPORT uint32_t MpToolConvertSession(MpSession* session, uint32_t format);
// Copies the base URL used by CompressedHtml conversions. An empty value restores the
// build-provided default. This must not race a conversion.
MP_API_EXPORT bool MpToolSetHtmlAssetsBaseUrl(const char* base_url, mpsize_t size);

MP_API_EXPORT mpsize_t MpGetObjSize(MpSession* session);
MP_API_EXPORT mpsize_t MpGetArraySize(MpArray* array);
MP_API_EXPORT void MpReleaseArray(MpArray* array);
MP_API_EXPORT uint32_t MpGetIdFromArray(MpSession* /* session */, MpArray* array, mpsize_t index);
MP_API_EXPORT const uint8_t* MpGetU64SampleFromArray(MpSession* session, MpArray* array, mpsize_t index);
MP_API_EXPORT const uint8_t* MpGetCounterSampleFromArray(MpSession* session, MpArray* array, mpsize_t index);
MP_API_EXPORT const uint8_t* MpGetLastCounterSampleFromArray(MpSession* session, MpArray* array);
MP_API_EXPORT const uint8_t* MpGetCounterSampleFromArrayByFrameId(MpSession* session, MpArray* array, mpsize_t frameId);

MP_API_EXPORT void MpEnableObjectCache(MpSession* session, uint32_t kvL1Cache, uint32_t descrCache, uint32_t arrayCache, uint32_t scalarCache);
MP_API_EXPORT void MpClearObjectCache(MpSession* session);

/*
 * The methods below call CheckSync to ensure data is synchronized before execution
 */
MP_API_EXPORT uint32_t MpIsSessionValid(MpSession* session);
MP_API_EXPORT uint32_t MpGetSessionDataFormatVersion(MpSession* session);

MP_API_EXPORT mpsize_t MpGetTrackCount(MpSession* session);
MP_API_EXPORT MpArray* MpGetTracks(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetTrackDescFromArray(
    MpSession* session, MpArray* array, mpsize_t index);
MP_API_EXPORT mpsize_t MpGetSegmentCount(MpSession* session, mpsize_t trackIndex);
MP_API_EXPORT MpArray* MpGetSegments(MpSession* session, mpsize_t trackIndex);
MP_API_EXPORT const uint8_t* MpGetSegmentDescFromArray(
    MpSession* session, MpArray* array, mpsize_t index);
MP_API_EXPORT bool MpSelectTrack(MpSession* session, mpsize_t trackIndex);
MP_API_EXPORT bool MpSelectSegment(MpSession* session, mpsize_t segmentIndex);

// Arms the next data-returning request so its view remains valid until the matching pop.
// To stack locks, claim each level before pushing the next one:
// Push -> Get A -> Push -> Get B -> Pop -> Pop.
MP_API_EXPORT bool MpPushDataLock(MpSession* session);
// Consumes the currently armed level without releasing it. Cached language wrappers use
// this when a Get/Fetch is served locally and therefore does not enter another C API getter.
MP_API_EXPORT bool MpConsumeDataLock(MpSession* session);
MP_API_EXPORT bool MpPopDataLock(MpSession* session);

MP_API_EXPORT uint32_t MpFindGroupId(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT MpArray* MpFindGroupIds(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);

MP_API_EXPORT uint32_t MpFindTimerId(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT MpArray* MpFindTimerIds(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT MpArray* MpGetTimerIdsByGroupId(MpSession* session, uint32_t groupId);
MP_API_EXPORT MpArray* MpGetTimerIdsByGroupNames(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT uint32_t MpFindCounterId(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT MpArray* MpFindCounterIds(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT uint32_t MpFindThreadId(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);
MP_API_EXPORT MpArray* MpFindThreadIds(MpSession* session, const char* nameMask, uint32_t caseSensitive = 0);

MP_API_EXPORT const uint8_t* MpGetGlobalDesc(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetGeneralInfo(MpSession* session);
MP_API_EXPORT uint32_t MpGetGroupIdMax(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetGroupDesc(MpSession* session, uint32_t groupId);
MP_API_EXPORT MpArray* MpGetTimerIds(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetTimerDesc(MpSession* session, uint32_t timerId);
MP_API_EXPORT MpArray* MpGetCounterIds(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetCounterDesc(MpSession* session, uint32_t counterId);
MP_API_EXPORT MpArray* MpGetCounterSamples(MpSession* session, uint32_t counterId);
MP_API_EXPORT const uint8_t* MpGetLastCounterSample(MpSession* session, uint32_t counterId);
MP_API_EXPORT MpArray* MpGetThreadIds(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetThreadDesc(MpSession* session, uint32_t threadId);
MP_API_EXPORT uint32_t MpGetThreadIdGpu(MpSession* session);
MP_API_EXPORT const uint8_t* MpGetFrameDesc(MpSession* session, uint32_t frameId);
MP_API_EXPORT uint32_t MpGetFrameIdMin(MpSession* session);
MP_API_EXPORT uint32_t MpGetFrameIdMax(MpSession* session);
// Native callers may pass an aligned uint64_t pointer. The Parts variant lets
// WASM wrappers preserve the full offset without relying on 64-bit numbers.
// outByteLength may be null; otherwise it receives the byte count excluding the
// NUL terminator, or zero on failure. Stored strings are limited to 1024 bytes
// including the terminator.
MP_API_EXPORT const uint8_t* MpGetLabelFromOffset(
    MpSession* session, const uint64_t* offset, uint32_t* outByteLength);
MP_API_EXPORT const uint8_t* MpGetLabelFromOffsetParts(
    MpSession* session, uint32_t offsetLow, uint32_t offsetHigh, uint32_t* outByteLength);
// Resolves a stored raw thread-log entry. Only regular (non-literal) label
// entries have a global label-buffer offset and can return a string. The parts
// may be saved from MpGetLogIteratorCurrentEntryPart(..., 0/1).
MP_API_EXPORT const uint8_t* MpGetLabelFromEntry(
    MpSession* session, const uint64_t* entry, uint32_t* outByteLength);
MP_API_EXPORT const uint8_t* MpGetLabelFromEntryParts(
    MpSession* session, uint32_t entryLow, uint32_t entryHigh, uint32_t* outByteLength);
MP_API_EXPORT void MpEnableLabelCache(MpSession* session, uint32_t enabled);
MP_API_EXPORT void MpClearLabelCache(MpSession* session);
MP_API_EXPORT MpArray* MpGetPlaceIdSamples(MpSession* session);
MP_API_EXPORT MpArray* MpGetUtcTimestampSamples(MpSession* session);

MP_API_EXPORT MpLogIterator* MpCreateLogIterator(MpSession* session);
MP_API_EXPORT void MpDeleteLogIterator(MpLogIterator* iter);
MP_API_EXPORT bool MpStepLogIterator(MpLogIterator* iter);
MP_API_EXPORT void MpRewindLogIterator(MpLogIterator* iter);
MP_API_EXPORT void MpRewindLogIteratorTo(MpLogIterator* iter, uint32_t frameIdMin, uint32_t frameIdMax);
MP_API_EXPORT bool MpConfigureLogIterator(MpLogIterator* iter, bool useDefault);
MP_API_EXPORT uint8_t* MpGetLogIteratorConfigBuffer(MpLogIterator* iter, mpsize_t minSize);
MP_API_EXPORT const uint8_t* MpGetLogIteratorState(MpLogIterator* iter);
MP_API_EXPORT const uint8_t* MpGetLogIteratorThreadStackState(MpLogIterator* iter, uint32_t threadId);
MP_API_EXPORT const uint8_t* MpGetLogIteratorThreadStackElement(MpLogIterator* iter, uint32_t threadId, mpsize_t stackLevel);
MP_API_EXPORT const uint8_t* MpGetLogIteratorCurrentThreadStackElement(MpLogIterator* iter, mpsize_t stackLevel);
// Byte distance between the public elements of adjacent stack levels. Internal
// per-scope data may make this larger than LogIteratorThreadStackElement.
MP_API_EXPORT mpsize_t MpGetLogIteratorThreadStackElementStride(MpLogIterator* iter);
MP_API_EXPORT const uint64_t* MpGetLogIteratorCurrentEntry(MpLogIterator* iter);
MP_API_EXPORT mpsize_t MpGetLogIteratorCurrentEntryCount(MpLogIterator* iter);
MP_API_EXPORT const uint64_t* MpGetLogIteratorCurrentEntryAt(MpLogIterator* iter, mpsize_t entryIndex);
// partIndex 0 returns the low 32 bits; partIndex 1 returns the high 32 bits.
// Invalid indexes return zero; query the entry count before reading parts.
MP_API_EXPORT uint32_t MpGetLogIteratorCurrentEntryPart(
    MpLogIterator* iter, mpsize_t entryIndex, uint32_t partIndex);
MP_API_EXPORT const uint64_t* MpGetLogIteratorCurrentEventInfo(MpLogIterator* iter, uint32_t* outEventType, uint32_t* outEventSize, uint32_t* outHasComment);
// outByteLength is optional and follows MpGetLabelFromOffset's length/failure contract.
MP_API_EXPORT const uint8_t* MpGetLogIteratorCurrentLabel(
    MpLogIterator* iter, uint32_t* outByteLength);
// A null pointer or an offset value of zero means no regular label offset is available.
// Returned pointers remain valid until the next label-offset query on this iterator.
MP_API_EXPORT const uint64_t* MpGetLogIteratorCurrentLabelOffset(MpLogIterator* iter);
MP_API_EXPORT mpsize_t MpGetLogIteratorScopeLabelCount(MpLogIterator* iter, uint32_t threadId, mpsize_t stackLevel);
MP_API_EXPORT mpsize_t MpGetLogIteratorCurrentScopeLabelCount(MpLogIterator* iter);
MP_API_EXPORT const uint64_t* MpGetLogIteratorScopeLabelOffset(
    MpLogIterator* iter, uint32_t threadId, mpsize_t stackLevel, mpsize_t labelIndex);
MP_API_EXPORT const uint64_t* MpGetLogIteratorCurrentScopeLabelOffset(
    MpLogIterator* iter, mpsize_t labelIndex);

MP_API_EXPORT MpCounterIterator* MpCreateCounterIterator(MpSession* session);
MP_API_EXPORT void MpDeleteCounterIterator(MpCounterIterator* iter);
MP_API_EXPORT bool MpStepCounterIterator(MpCounterIterator* iter);
MP_API_EXPORT void MpRewindCounterIterator(MpCounterIterator* iter);
MP_API_EXPORT bool MpConfigureCounterIterator(MpCounterIterator* iter, bool useDefault);
MP_API_EXPORT uint8_t* MpGetCounterIteratorConfigBuffer(MpCounterIterator* iter, mpsize_t minSize);
MP_API_EXPORT const uint8_t* MpGetCounterIteratorState(MpCounterIterator* iter);
MP_API_EXPORT const uint32_t* MpGetCounterIteratorStack(MpCounterIterator* iter, mpsize_t* outSize);
MP_API_EXPORT uint32_t MpGetCounterIteratorStackElement(MpCounterIterator* iter, mpsize_t stackLevel);

/*
 * Control channel methods
 */
MP_API_EXPORT uint32_t MpUseControlChannel(uint32_t use);
MP_API_EXPORT uint32_t MpIsBackendVersionCompatible();
MP_API_EXPORT uint32_t MpIsBackendAccessible();
MP_API_EXPORT uint32_t MpIsBackendReady();
MP_API_EXPORT uint32_t MpControlEnableProfiler(MpSession* session, uint32_t enable);
MP_API_EXPORT uint32_t MpControlEnableCapture(MpSession* session, uint32_t enable);
MP_API_EXPORT uint32_t MpControlShowUI(MpSession* session, uint32_t show);
MP_API_EXPORT uint32_t MpControlSetFrameLimit(MpSession* session, int32_t num);
MP_API_EXPORT uint32_t MpControlSetMemTargetMB(MpSession* session, int32_t targetMB);
MP_API_EXPORT uint32_t MpControlCaptureToSlot(
    MpSession* session, uint32_t targetSlotId, uint32_t maxFrameCount, uint32_t containerFormat);
MP_API_EXPORT uint32_t MpControlResetSlot(MpSession* session, uint32_t slotId);
MP_API_EXPORT uint32_t MpControlExperimentalV1StartFileStream(
    const char* filenamePostfix, mpsize_t filenamePostfixSize, uint32_t targetSlotId);
MP_API_EXPORT uint32_t MpControlExperimentalV1StartMemoryStream(uint32_t targetSlotId);
MP_API_EXPORT uint32_t MpControlExperimentalV1StopStream();
MP_API_EXPORT uint32_t MpControlExperimentalV1SetStreamMarker(uint32_t marker, uint32_t enabled);
// Returns a view valid until the next control-channel command, or null when unavailable.
MP_API_EXPORT const uint8_t* MpControlExperimentalV1GetStreamStatus();
// contentMode matches a LibMP::Enums::ContentMode constant.
MP_API_EXPORT uint32_t MpControlExperimentalV1SetContentMode(uint32_t contentMode);

} // extern "C"

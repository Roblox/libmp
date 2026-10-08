// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

package main

import (
	"bytes"
	"fmt"
	"os"
	"runtime"
	"time"

	"libmp-smoke/libmp"
)

func check(condition bool, message string) {
	if !condition {
		panic(message)
	}
}

type mutableProvider struct {
	data       []byte
	closed     bool
	beforeRead func()
}

func (provider *mutableProvider) GetSize() uint64 { return uint64(len(provider.data)) }
func (provider *mutableProvider) Close() bool     { provider.closed = true; return true }
func (provider *mutableProvider) Read(
	destination []byte, blockID uint32, offset uint64, size uint32,
) int {
	if provider.beforeRead != nil {
		provider.beforeRead()
	}
	if blockID != 0 || offset > uint64(len(provider.data)) ||
		uint64(size) > uint64(len(provider.data))-offset {
		return 0
	}
	return copy(destination, provider.data[offset:offset+uint64(size)])
}

func main() {
	if len(os.Args) != 2 && len(os.Args) != 4 {
		panic("usage: smoke <capture.gprx> [<short-update.gprx> <long-update.gprx>]")
	}
	capture, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	library, err := libmp.New()
	if err != nil {
		panic(err)
	}
	defer library.Dispose()
	check(library.Versions.Library != 0, "library version is missing")
	_ = library.GetMemReserved()
	check(!library.Control.EnableProfiler(false), "default control channel must be a no-op")
	check(!library.Control.SetMemTargetMB(64), "default memory-target control must be a no-op")
	check(libmp.Enums.DataSlot.Live == 0 && libmp.Enums.DataSlot.Temporary == 5,
		"data-slot enum is unavailable")
	check(libmp.Enums.ContentMode.Full == libmp.ContentModeFull &&
		libmp.Enums.ContentMode.CountersOnly == libmp.ContentModeCountersOnly,
		"content-mode enum is unavailable")
	check(library.Control.CaptureToBufferSync() == nil &&
		!library.Control.CaptureToSlotSync(libmp.Enums.DataSlot.FirstUserManaged) &&
		library.Control.SaveSlotToBuffer(libmp.Enums.DataSlot.FirstUserManaged) == nil &&
		!library.Control.ResetSlot(libmp.Enums.DataSlot.FirstUserManaged),
		"standalone slot controls must be no-op stubs")
	check(library.OpenFromSlot(libmp.Enums.DataSlot.Live, 4096) == nil,
		"standalone slot session must be a no-op stub")
	check(!library.Experimental.V1.StartFileStream("") &&
		!library.Experimental.V1.StartMemoryStream(1) &&
		!library.Experimental.V1.StopStream() &&
		!library.Experimental.V1.SetStreamMarker(1, true) &&
		!library.Experimental.V1.SetContentMode(libmp.Enums.ContentMode.CountersOnly),
		"standalone experimental controls must be no-op stubs")
	streamStatus := library.Experimental.V1.GetStreamStatus()
	check(!streamStatus.Active && streamStatus.RecordedSizeKb == 0 &&
		streamStatus.MaxSizeKb == 0 && streamStatus.ElapsedMs == 0,
		"standalone stream status must be empty")
	check(library.Tools.SetHtmlAssetsBaseUrl(""), "HTML asset-base setter failed")

	session := library.OpenFromMem(capture)
	check(session != nil && session.IsValid(), "OpenFromMem failed")
	defer session.Dispose()
	check(session.GetFrameIdMax() >= session.GetFrameIdMin(), "invalid frame bounds")
	check(len(session.FetchTimerIds()) != 0, "timer array is empty")
	check(len(session.FetchCounterIds()) != 0, "counter array is empty")
	check(len(session.FetchThreadIds()) != 0, "thread array is empty")

	globalView := session.GetGlobalDesc()
	check(globalView != nil, "global descriptor is missing")
	expectedTick := globalView.TickToMsCpu()
	frameView := session.GetFrameDesc(session.GetFrameIdMin())
	check(frameView != nil, "frame descriptor is missing")
	var threadLog libmp.ThreadLogDescr
	hasThreadLog := frameView.ThreadLogsLength() != 0 && frameView.ThreadLogs(&threadLog, 0)
	expectedThreadID := threadLog.ThreadId()
	growthSession := library.OpenFromMem(capture)
	check(growthSession != nil && growthSession.IsValid(), "second session failed")
	check(globalView.TickToMsCpu() == expectedTick,
		"FlatBuffers view did not follow grown WASM memory")
	if hasThreadLog {
		check(threadLog.ThreadId() == expectedThreadID,
			"nested FlatBuffers view did not follow grown WASM memory")
	}
	growthSession.Dispose()
	check(session.GetGlobalDesc() == globalView, "object cache did not reuse a view")
	session.ClearObjectCache()
	check(session.GetGlobalDesc() != globalView, "object cache did not clear a view")

	timerID := session.FetchTimerIds()[0]
	shortTermCached := session.FetchGlobalDesc()
	longTermCached := session.FetchTimerDesc(timerID)
	check(session.FetchGlobalDesc() == shortTermCached,
		"short-term object cache did not reuse a fetched object")
	check(session.FetchTimerDesc(timerID) == longTermCached,
		"long-term object cache did not reuse a fetched object")
	session.SetClearCacheOnSyncRequired()
	session.SetSyncRequired()
	check(session.FetchGlobalDesc() != shortTermCached,
		"sync callback did not clear the short-term wrapper cache bucket")
	check(session.FetchTimerDesc(timerID) != longTermCached,
		"sync callback did not clear the long-term wrapper cache bucket")

	lockCachedView := session.GetGlobalDesc()
	lockCachedObject := session.FetchTimerDesc(timerID)
	check(session.WithDataLock(func() {
		check(session.GetGlobalDesc() == lockCachedView,
			"locked cache hit did not preserve view identity")
		check(session.WithDataLock(func() {
			check(session.FetchTimerDesc(timerID) == lockCachedObject,
				"nested locked cache hit did not preserve fetched-object identity")
		}), "nested data lock failed")
	}), "outer data lock failed")

	counters := session.CreateCounterIterator()
	check(counters.Configure(&libmp.CounterIteratorConfigT{}), "counter configuration failed")
	check(counters.Step(), "counter iterator is empty")
	check(counters.GetState() != nil, "counter iterator state is missing")
	counters.Dispose()

	logs := session.CreateLogIterator()
	check(logs.Configure(&libmp.LogIteratorConfigT{StartFrameId: session.GetFrameIdMin()}),
		"log configuration failed")
	var entryCount uint32
	for logs.Step() && entryCount == 0 {
		check(logs.GetState() != nil, "log iterator state is missing")
		entryCount = logs.GetCurrentEntryCount()
	}
	check(entryCount != 0, "log iterator has no physical entry")
	for index := uint32(0); index < entryCount; index++ {
		_ = logs.GetCurrentEntryPart(index, 0)
		_ = logs.GetCurrentEntryPart(index, 1)
	}
	logs.Dispose()

	var rawConversion []byte
	formats := []libmp.ContainerFormat{
		libmp.Enums.ContainerFormat.Raw,
		libmp.Enums.ContainerFormat.Compressed,
		libmp.Enums.ContainerFormat.CompressedAscii,
		libmp.Enums.ContainerFormat.CompressedHtml,
	}
	for _, format := range formats {
		converted, convertErr := library.Tools.ConvertToBuffer(session, format)
		check(convertErr == nil && len(converted) != 0, "buffer conversion failed")
		convertedSession := library.OpenFromMem(converted)
		check(convertedSession != nil && convertedSession.IsValid(),
			"converted buffer is invalid")
		check(convertedSession.GetFrameIdMax() == session.GetFrameIdMax(),
			"converted buffer changed frame bounds")
		convertedSession.Dispose()
		if format == libmp.Enums.ContainerFormat.Raw {
			rawConversion = converted
		}
	}
	convertedPath := os.Args[1] + ".go-converted.gprx"
	defer os.Remove(convertedPath)
	check(library.Tools.ConvertToFile(
		session, libmp.Enums.ContainerFormat.Raw, convertedPath) == nil,
		"file conversion failed")
	convertedFile, err := os.ReadFile(convertedPath)
	check(err == nil && bytes.Equal(convertedFile, rawConversion),
		"file and buffer conversion differ")
	check(session.IsValid(), "conversion invalidated its source session")

	externalSession := library.OpenFromBuffer(capture, 64*1024)
	check(externalSession != nil && externalSession.IsValid(), "OpenFromBuffer failed")
	externalSession.Dispose()
	fileSession, err := library.OpenFromFile(os.Args[1])
	check(err == nil && fileSession != nil && fileSession.IsValid(), "OpenFromFile failed")
	fileSession.Dispose()

	// Closing a session detaches its iterators: they stop stepping but stay disposable
	closedSession := library.OpenFromMem(capture)
	detachedCounters := closedSession.CreateCounterIterator()
	detachedLog := closedSession.CreateLogIterator()
	check(detachedCounters.Configure(&libmp.CounterIteratorConfigT{}) && detachedCounters.Step(),
		"counter iterator did not step")
	check(detachedLog.Configure(&libmp.LogIteratorConfigT{StartFrameId: closedSession.GetFrameIdMin()}) &&
		detachedLog.Step(), "log iterator did not step")
	closedSession.Dispose()
	check(!detachedCounters.Step() && !detachedLog.Step(), "detached iterator kept stepping")
	check(detachedCounters.GetState() != nil && detachedLog.GetState() != nil,
		"detached iterator lost its state")
	check(!detachedCounters.Configure(&libmp.CounterIteratorConfigT{}) &&
		!detachedLog.Configure(&libmp.LogIteratorConfigT{}),
		"detached iterator accepted a configuration")
	detachedCounters.Rewind()
	detachedLog.Rewind()
	detachedCounters.Dispose()
	detachedLog.Dispose()

	// Arrays read nothing once they or their session are disposed
	arraySession := library.OpenFromMem(capture)
	counterID := arraySession.FetchCounterIds()[0]
	samples := arraySession.GetCounterSamples(counterID)
	released := arraySession.GetCounterSamples(counterID)
	check(samples.Fetch(0) != nil && released.Fetch(0) != nil, "counter samples are empty")
	released.Dispose()
	check(released.Fetch(0) == nil, "a disposed array read its elements")
	arraySession.Dispose()
	check(samples.Fetch(0) == nil && samples.FetchLastSample() == nil,
		"an array read its closed session")
	samples.Dispose()

	// Finalizers only queue releases, which the next LibMP call frees on this goroutine
	waitForRelease := func(memUsed uint32, message string) {
		released := false
		for attempt := 0; attempt < 100 && !released; attempt++ {
			runtime.GC()
			time.Sleep(10 * time.Millisecond)
			released = library.GetMemUsed() == memUsed
		}
		check(released, message)
	}
	memUsed := library.GetMemUsed()
	finalizedSession := library.OpenFromMem(capture)
	createGarbage := func(session *libmp.Session) []func() {
		var disposers []func()
		for index := 0; index < 64; index++ {
			disposers = append(disposers, session.CreateLogIterator().Dispose,
				session.CreateCounterIterator().Dispose, session.GetTimerIds().Dispose)
		}
		return disposers
	}
	for _, dispose := range createGarbage(finalizedSession) { // grows the session's iterator lists first
		dispose()
	}
	sessionMemUsed := library.GetMemUsed()
	createGarbage(finalizedSession)
	waitForRelease(sessionMemUsed, "finalizers did not release unreachable objects")
	createGarbage(finalizedSession)
	finalizedSession.Dispose()
	waitForRelease(memUsed, "finalizers did not release objects that outlived their session")
	finalizedProvider := &mutableProvider{data: capture}
	createGarbage(library.OpenFromMem(capture))
	createGarbage(library.OpenFromExternalProvider(finalizedProvider, 0, false))
	waitForRelease(memUsed, "finalizers did not release unreachable sessions")
	check(finalizedProvider.closed, "finalizers did not close an unreachable session's provider")

	if len(os.Args) == 4 {
		provider := &mutableProvider{data: capture}
		relatedSession := library.OpenFromExternalProvider(provider, 0, false)
		check(relatedSession != nil && relatedSession.IsValid(),
			"related-snapshot session failed")
		relatedTimerID := relatedSession.FetchTimerIds()[0]
		baselineGlobal := relatedSession.FetchGlobalDesc()
		baselineGeneral := relatedSession.FetchGeneralInfo()
		baselineTimer := relatedSession.FetchTimerDesc(relatedTimerID)

		provider.data, err = os.ReadFile(os.Args[2])
		if err != nil {
			panic(err)
		}
		relatedSession.SetSyncRequired()
		shortGeneral := relatedSession.FetchGeneralInfo()
		shortGlobal := relatedSession.FetchGlobalDesc()
		timerAfterShortUpdate := relatedSession.FetchTimerDesc(relatedTimerID)
		check(shortGeneral.PlatformInfoJson != baselineGeneral.PlatformInfoJson,
			"short-term fixture did not change its metadata")
		check(shortGlobal != baselineGlobal,
			"short-term update did not invalidate the short-term cache bucket")
		check(timerAfterShortUpdate == baselineTimer,
			"short-term update invalidated the long-term cache bucket")

		provider.data, err = os.ReadFile(os.Args[3])
		if err != nil {
			panic(err)
		}
		relatedSession.SetSyncRequired()
		generalAfterLongUpdate := relatedSession.FetchGeneralInfo()
		globalAfterLongUpdate := relatedSession.FetchGlobalDesc()
		longTimer := relatedSession.FetchTimerDesc(relatedTimerID)
		check(generalAfterLongUpdate == shortGeneral && globalAfterLongUpdate == shortGlobal,
			"long-term update invalidated the short-term cache bucket")
		check(longTimer != timerAfterShortUpdate,
			"long-term update did not invalidate the long-term cache bucket")
		check(longTimer.TimerName != timerAfterShortUpdate.TimerName,
			"long-term fixture did not change its metadata")
		relatedSession.Dispose()
	}

	// A callback runs inside a Wasmtime call, so it must not close that call's store.
	callbackLibrary, err := libmp.New()
	check(err == nil, "callback-disposal LibMP instance failed")
	callbackProvider := &mutableProvider{data: capture}
	disposeRejected := false
	callbackProvider.beforeRead = func() {
		callbackProvider.beforeRead = nil
		func() {
			defer func() {
				if recover() != nil {
					disposeRejected = true
				}
			}()
			callbackLibrary.Dispose()
		}()
	}
	callbackSession := callbackLibrary.OpenFromExternalProvider(callbackProvider, 0, false)
	check(disposeRejected, "disposing a LibMP instance from its callback was allowed")
	check(callbackSession != nil && callbackSession.IsValid(),
		"callback disposal attempt damaged the LibMP instance")
	callbackSession.Dispose()
	callbackLibrary.Dispose()

	otherA, err := libmp.New()
	check(err == nil, "first secondary LibMP instance failed")
	defer otherA.Dispose()
	otherB, err := libmp.New()
	check(err == nil, "second secondary LibMP instance failed")
	defer otherB.Dispose()
	otherSessionA := otherA.OpenFromBuffer(capture)
	otherSessionB := otherB.OpenFromBuffer(capture)
	check(otherSessionA != nil && otherSessionA.IsValid() &&
		otherSessionB != nil && otherSessionB.IsValid(),
		"multiple LibMP instances interfered with external IO routing")
	otherSessionA.Dispose()
	otherSessionB.Dispose()

	fmt.Println("LibMP Go smoke test passed")
}

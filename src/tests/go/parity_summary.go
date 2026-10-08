// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"math"
	"os"

	"libmp-parity/libmp"
)

type summaryWriter struct {
	out     *bufio.Writer
	library *libmp.LibMPInstance
	session *libmp.Session
}

func boolean(value bool) int {
	if value {
		return 1
	}
	return 0
}

func hexU32(value uint32) string      { return fmt.Sprintf("%08x", value) }
func hexU64(value uint64) string      { return fmt.Sprintf("%016x", value) }
func floatBits(value float32) string  { return hexU32(math.Float32bits(value)) }
func doubleBits(value float64) string { return hexU64(math.Float64bits(value)) }
func hexBytes(value string) string {
	return fmt.Sprintf("%d:%s", len(value), hex.EncodeToString([]byte(value)))
}
func (w *summaryWriter) emit(fields ...any) {
	for index, field := range fields {
		if index != 0 {
			fmt.Fprint(w.out, " ")
		}
		fmt.Fprint(w.out, field)
	}
	fmt.Fprintln(w.out)
}

func (w *summaryWriter) write() {
	session := w.session
	frameIDMin := session.GetFrameIdMin()
	frameIDMax := session.GetFrameIdMax()
	w.emit("LIBMP_PARITY", 3)
	w.emit("SESSION", session.GetDataFormatVersion(), w.library.Versions.Library,
		frameIDMin, frameIDMax)

	global := session.FetchGlobalDesc()
	if global != nil {
		w.emit("GLOBAL", 1, floatBits(global.TickToMsCpu), floatBits(global.TickToMsGpu))
	} else {
		w.emit("GLOBAL", 0)
	}
	general := session.FetchGeneralInfo()
	if general != nil {
		w.emit("GENERAL", 1, hexBytes(general.PlatformInfoJson))
	} else {
		w.emit("GENERAL", 0)
	}

	groupIDMax := session.GetGroupIdMax()
	w.emit("GROUPS", groupIDMax)
	for queryID := uint32(1); queryID <= groupIDMax; queryID++ {
		group := session.FetchGroupDesc(queryID)
		if group != nil {
			w.emit("GROUP", queryID, 1, group.GroupId, hexBytes(group.GroupName),
				hexU32(group.Color), boolean(group.IsGpu))
		} else {
			w.emit("GROUP", queryID, 0)
		}
	}

	timerIDs := session.FetchTimerIds()
	w.emit("TIMER_IDS", len(timerIDs))
	for index, id := range timerIDs {
		w.emit("TIMER_ID", index, id)
		timer := session.FetchTimerDesc(id)
		if timer != nil {
			w.emit("TIMER", id, 1, timer.TimerId, hexBytes(timer.TimerName),
				timer.GroupId, hexU32(timer.Color), boolean(timer.IsUserTimer))
		} else {
			w.emit("TIMER", id, 0)
		}
	}
	for groupID := uint32(1); groupID <= groupIDMax; groupID++ {
		ids := session.FetchTimerIdsByGroupId(groupID)
		fields := []any{"GROUP_TIMER_IDS", groupID, len(ids)}
		for _, id := range ids {
			fields = append(fields, id)
		}
		w.emit(fields...)
	}

	counterIDs := session.FetchCounterIds()
	w.emit("COUNTER_IDS", len(counterIDs))
	for index, id := range counterIDs {
		w.emit("COUNTER_ID", index, id)
		counter := session.FetchCounterDesc(id)
		if counter == nil {
			w.emit("COUNTER", id, 0)
			continue
		}
		w.emit("COUNTER", id, 1, counter.CounterId, hexBytes(counter.CounterName),
			counter.Parent, counter.Sibling, counter.FirstChild, counter.Level)
		samples := session.FetchCounterSamples(id)
		w.emit("COUNTER_SAMPLES", id, len(samples))
		for sampleIndex, sample := range samples {
			w.emit("COUNTER_SAMPLE", id, sampleIndex, 1,
				sample.FrameId, doubleBits(sample.Value))
		}
		last := session.FetchLastCounterSample(id)
		if last != nil {
			w.emit("COUNTER_LAST", id, 1, last.FrameId, doubleBits(last.Value))
		} else {
			w.emit("COUNTER_LAST", id, 0)
		}
	}

	w.emit("GPU_THREAD", session.GetThreadIdGpu())
	threadIDs := session.FetchThreadIds()
	w.emit("THREAD_IDS", len(threadIDs))
	for index, id := range threadIDs {
		w.emit("THREAD_ID", index, id)
		thread := session.FetchThreadDesc(id)
		if thread != nil {
			w.emit("THREAD", id, 1, thread.ThreadId, hexBytes(thread.ThreadName),
				thread.BufferSize, boolean(thread.IsGpu))
		} else {
			w.emit("THREAD", id, 0)
		}
	}

	frameCount := uint64(0)
	if frameIDMax >= frameIDMin {
		frameCount = uint64(frameIDMax) - uint64(frameIDMin) + 1
	}
	w.emit("FRAMES", frameCount)
	for offset := uint64(0); offset < frameCount; offset++ {
		queryID := frameIDMin + uint32(offset)
		frame := session.FetchFrameDesc(queryID)
		if frame == nil {
			w.emit("FRAME", queryID, 0)
			continue
		}
		w.emit("FRAME", queryID, 1, frame.FrameId, frame.FrameAbsoluteId,
			hexU64(frame.TickStartCpu), hexU64(frame.TickEndCpu),
			hexU64(frame.TickStartGpu), hexU64(frame.TickEndGpu),
			hexU64(frame.LabelOffsetMin), hexU64(frame.LabelOffsetMax),
			boolean(frame.IsPaused), boolean(frame.IsIncomplete), len(frame.ThreadLogs))
		for logIndex, log := range frame.ThreadLogs {
			w.emit("FRAME_THREAD_LOG", queryID, logIndex, log.ThreadId, log.LogEntriesNum,
				hexU64(log.LogStartLocator), boolean(log.IsRemoved))
		}
	}

	w.writeU64Samples("PLACE_ID", session.FetchPlaceIdSamples())
	w.writeU64Samples("UTC", session.FetchUtcTimestampSamples())
	w.writeFindQueries()
	w.writeCounterIterator()
	w.writeLogIterator()
	w.emit("END")
}

func (w *summaryWriter) writeU64Samples(name string, samples []*libmp.U64SampleT) {
	w.emit(name+"_SAMPLES", len(samples))
	for index, sample := range samples {
		w.emit(name+"_SAMPLE", index, 1, sample.FrameId, hexU64(sample.Value))
	}
}

func check(condition bool, message string) {
	if !condition {
		panic(message)
	}
}

func checkTrackAndLabelAPIs(session *libmp.Session) {
	check(libmp.Enums.AppType.Unknown == libmp.AppTypeUnknown &&
		libmp.Enums.AppType.Server == libmp.AppTypeServer,
		"application-type enum is unavailable")
	tracks := session.GetTracks()
	check(tracks.Size() == int(session.GetTrackCount()), "track count mismatch")
	tracks.Dispose()
	fetchedTracks := session.FetchTracks()
	check(len(fetchedTracks) == int(session.GetTrackCount()), "fetched track count mismatch")
	if len(fetchedTracks) != 0 {
		segments := session.GetSegments(0)
		check(segments.Size() == int(session.GetSegmentCount(0)), "segment count mismatch")
		segments.Dispose()
		check(len(session.FetchSegments(0)) == int(session.GetSegmentCount(0)),
			"fetched segment count mismatch")
		check(session.SelectTrack(0), "failed to select first track")
		if session.GetSegmentCount(0) != 0 {
			check(session.SelectSegment(0), "failed to select first segment")
		}
	}

	session.EnableLabelCache(true)
	session.ClearLabelCache()
}

func checkCachedDescriptor[View any, Copy any](
	session *libmp.Session, label string, firstID uint32, secondID *uint32,
	get func(uint32) *View, fetch func(uint32) *Copy,
	viewValue func(*View) string, fetchedValue func(*Copy) string,
) {
	firstView := get(firstID)
	repeatedView := get(firstID)
	check(firstView != nil && firstView == repeatedView, label+": Get identity miss")
	expected := viewValue(firstView)
	if secondID != nil {
		get(*secondID)
		check(get(firstID) == firstView, label+": interleaved Get identity miss")
	}

	firstCopy := fetch(firstID)
	repeatedCopy := fetch(firstID)
	check(firstCopy != nil && firstCopy == repeatedCopy, label+": Fetch identity miss")
	check(fetchedValue(firstCopy) == expected, label+": Get/Fetch value mismatch")
	if secondID != nil {
		fetch(*secondID)
		check(fetch(firstID) == firstCopy, label+": interleaved Fetch identity miss")
	}

	session.ClearObjectCache()
	clearedView := get(firstID)
	clearedCopy := fetch(firstID)
	check(clearedView != firstView, label+": ClearObjectCache kept Get wrapper")
	check(clearedCopy != firstCopy, label+": ClearObjectCache kept Fetch object")
	check(viewValue(clearedView) == expected && fetchedValue(clearedCopy) == expected,
		label+": ClearObjectCache changed value")
}

func second(values []uint32) *uint32 {
	if len(values) < 2 {
		return nil
	}
	return &values[1]
}

type mutableProvider struct{ data []byte }

func (provider *mutableProvider) GetSize() uint64 { return uint64(len(provider.data)) }
func (provider *mutableProvider) Read(
	destination []byte, blockID uint32, offset uint64, size uint32,
) int {
	if blockID != 0 || offset > uint64(len(provider.data)) ||
		uint64(size) > uint64(len(provider.data))-offset {
		return 0
	}
	return copy(destination, provider.data[offset:offset+uint64(size)])
}

func checkCacheContract(library *libmp.LibMPInstance, session *libmp.Session,
	capture []byte, shortUpdatePath, longUpdatePath string,
) {
	session.EnableObjectCache(true)
	timerIDs := session.FetchTimerIds()
	counterIDs := session.FetchCounterIds()
	threadIDs := session.FetchThreadIds()
	groupIDs := []uint32{}
	for id := uint32(1); id <= session.GetGroupIdMax(); id++ {
		if session.FetchGroupDesc(id) != nil {
			groupIDs = append(groupIDs, id)
		}
	}

	if session.FetchGlobalDesc() != nil {
		checkCachedDescriptor(session, "GlobalDesc", 0, nil,
			func(uint32) *libmp.GlobalDesc { return session.GetGlobalDesc() },
			func(uint32) *libmp.GlobalDescT { return session.FetchGlobalDesc() },
			func(value *libmp.GlobalDesc) string { return floatBits(value.TickToMsCpu()) },
			func(value *libmp.GlobalDescT) string { return floatBits(value.TickToMsCpu) })
	}
	if session.FetchGeneralInfo() != nil {
		checkCachedDescriptor(session, "GeneralInfo", 0, nil,
			func(uint32) *libmp.GeneralInfo { return session.GetGeneralInfo() },
			func(uint32) *libmp.GeneralInfoT { return session.FetchGeneralInfo() },
			func(value *libmp.GeneralInfo) string { return string(value.PlatformInfoJson()) },
			func(value *libmp.GeneralInfoT) string { return value.PlatformInfoJson })
	}
	if len(groupIDs) != 0 {
		checkCachedDescriptor(session, "GroupDesc", groupIDs[0], second(groupIDs),
			session.GetGroupDesc, session.FetchGroupDesc,
			func(value *libmp.GroupDesc) string {
				return fmt.Sprintf("%d:%s", value.GroupId(), value.GroupName())
			},
			func(value *libmp.GroupDescT) string {
				return fmt.Sprintf("%d:%s", value.GroupId, value.GroupName)
			})
	}
	if len(timerIDs) != 0 {
		checkCachedDescriptor(session, "TimerDesc", timerIDs[0], second(timerIDs),
			session.GetTimerDesc, session.FetchTimerDesc,
			func(value *libmp.TimerDesc) string {
				return fmt.Sprintf("%d:%s", value.TimerId(), value.TimerName())
			},
			func(value *libmp.TimerDescT) string {
				return fmt.Sprintf("%d:%s", value.TimerId, value.TimerName)
			})
	}
	if len(counterIDs) != 0 {
		checkCachedDescriptor(session, "CounterDesc", counterIDs[0], second(counterIDs),
			session.GetCounterDesc, session.FetchCounterDesc,
			func(value *libmp.CounterDesc) string {
				return fmt.Sprintf("%d:%s", value.CounterId(), value.CounterName())
			},
			func(value *libmp.CounterDescT) string {
				return fmt.Sprintf("%d:%s", value.CounterId, value.CounterName)
			})
	}
	if len(threadIDs) != 0 {
		checkCachedDescriptor(session, "ThreadDesc", threadIDs[0], second(threadIDs),
			session.GetThreadDesc, session.FetchThreadDesc,
			func(value *libmp.ThreadDesc) string {
				return fmt.Sprintf("%d:%s", value.ThreadId(), value.ThreadName())
			},
			func(value *libmp.ThreadDescT) string {
				return fmt.Sprintf("%d:%s", value.ThreadId, value.ThreadName)
			})

		session.EnableObjectCache(false)
		view1 := session.GetThreadDesc(threadIDs[0])
		view2 := session.GetThreadDesc(threadIDs[0])
		copy1 := session.FetchThreadDesc(threadIDs[0])
		copy2 := session.FetchThreadDesc(threadIDs[0])
		check(view1 != nil && view2 != nil && view1 != view2,
			"disabled cache reused Get wrapper")
		check(copy1 != nil && copy2 != nil && copy1 != copy2,
			"disabled cache reused Fetch object")
		check(view1.ThreadId() == view2.ThreadId() && copy1.ThreadId == copy2.ThreadId,
			"disabled cache changed descriptor value")
		session.EnableObjectCache(true)
	}

	timerArray1 := session.GetTimerIds()
	timerArray2 := session.GetTimerIds()
	check(timerArray1 != timerArray2, "array wrappers unexpectedly share identity")
	check(timerArray1.Size() == timerArray2.Size(), "array wrapper sizes differ")
	if timerArray1.Size() != 0 {
		check(timerArray1.Get(0) == timerArray2.Get(0), "array wrapper values differ")
	}
	timerArray1.Dispose()
	timerArray2.Dispose()

	if shortUpdatePath != "" && longUpdatePath != "" {
		provider := &mutableProvider{data: capture}
		related := library.OpenFromExternalProvider(provider, 0, false)
		check(related != nil && related.IsValid(), "related-snapshot session failed")
		defer related.Dispose()
		id := related.FetchTimerIds()[0]
		baselineGlobal := related.FetchGlobalDesc()
		baselineGeneral := related.FetchGeneralInfo()
		baselineTimer := related.FetchTimerDesc(id)

		var err error
		provider.data, err = os.ReadFile(shortUpdatePath)
		if err != nil {
			panic(err)
		}
		related.SetSyncRequired()
		shortGeneral := related.FetchGeneralInfo()
		shortGlobal := related.FetchGlobalDesc()
		timerAfterShort := related.FetchTimerDesc(id)
		check(shortGeneral.PlatformInfoJson != baselineGeneral.PlatformInfoJson,
			"short-term fixture did not change its metadata")
		check(shortGlobal != baselineGlobal,
			"short-term update did not invalidate the short-term cache bucket")
		check(timerAfterShort == baselineTimer,
			"short-term update invalidated the long-term cache bucket")

		provider.data, err = os.ReadFile(longUpdatePath)
		if err != nil {
			panic(err)
		}
		related.SetSyncRequired()
		generalAfterLong := related.FetchGeneralInfo()
		globalAfterLong := related.FetchGlobalDesc()
		longTimer := related.FetchTimerDesc(id)
		check(generalAfterLong == shortGeneral && globalAfterLong == shortGlobal,
			"long-term update invalidated the short-term cache bucket")
		check(longTimer != timerAfterShort,
			"long-term update did not invalidate the long-term cache bucket")
		check(longTimer.TimerName != timerAfterShort.TimerName,
			"long-term fixture did not change its metadata")
	}
}

type findProbe struct {
	mask      string
	sensitive bool
}

func (w *summaryWriter) writeFindFamily(family string, probes []findProbe,
	findFirst func(string, ...bool) uint32, findAll func(string, ...bool) []uint32,
) {
	for probeIndex, probe := range probes {
		ids := findAll(probe.mask, probe.sensitive)
		fields := []any{"FIND", family, probeIndex, boolean(probe.sensitive),
			hexBytes(probe.mask), findFirst(probe.mask, probe.sensitive), len(ids)}
		for _, id := range ids {
			fields = append(fields, id)
		}
		w.emit(fields...)
	}
}

func (w *summaryWriter) writeFindQueries() {
	probes := []findProbe{
		{"*", false}, {"*", true}, {"*A*", false}, {"*A*", true},
		{"__libmp_parity_missing__", false},
	}
	w.emit("FIND_QUERIES", len(probes))
	w.writeFindFamily("GROUP", probes, w.session.FindGroupId, w.session.FindGroupIds)
	w.writeFindFamily("TIMER", probes, w.session.FindTimerId, w.session.FindTimerIds)
	w.writeFindFamily("COUNTER", probes, w.session.FindCounterId, w.session.FindCounterIds)
	w.writeFindFamily("THREAD", probes, w.session.FindThreadId, w.session.FindThreadIds)
	w.emit("GROUP_NAME_TIMER_QUERIES", len(probes))
	for probeIndex, probe := range probes {
		ids := w.session.FetchTimerIdsByGroupNames(probe.mask, probe.sensitive)
		fields := []any{"GROUP_NAME_TIMERS", probeIndex, boolean(probe.sensitive),
			hexBytes(probe.mask), len(ids)}
		for _, id := range ids {
			fields = append(fields, id)
		}
		w.emit(fields...)
	}
}

func (w *summaryWriter) writeCounterIterator() {
	w.emit("COUNTER_ITERATOR_BEGIN")
	iterator := w.session.CreateCounterIterator()
	defer iterator.Dispose()
	if !iterator.Configure(&libmp.CounterIteratorConfigT{RootCounterIds: []uint32{}}) {
		panic("failed to configure counter iterator")
	}
	state := iterator.GetState()
	reportIndex := 0
	for iterator.Step() {
		w.emit("CI", reportIndex, boolean(state.Started()), boolean(state.Finished()),
			state.CounterId(), state.ParentId(), state.Level(), state.RelativeLevel(),
			state.RootLevel(), state.IndexInSiblings(), boolean(state.IsLeaf()),
			boolean(state.HierarchyChanged()), boolean(state.CounterStackIsOverflowed()))
		reportIndex++
	}
	w.emit("COUNTER_ITERATOR_END", reportIndex, boolean(state.Finished()))
}

func (w *summaryWriter) writeLogIterator() {
	w.emit("LOG_ITERATOR_BEGIN")
	iterator := w.session.CreateLogIterator()
	defer iterator.Dispose()
	if !iterator.Configure(&libmp.LogIteratorConfigT{
		EmitThreadFrameEnd: true, TrackScopeLabels: true}) {
		panic("failed to configure log iterator")
	}
	state := iterator.GetState()
	reportIndex := 0
	checkedLabelAPIs := false
	for iterator.Step() {
		if !checkedLabelAPIs && !state.IsLabel() {
			_, currentLabel := iterator.FetchCurrentLabel()
			_, currentScopeLabel := iterator.FetchCurrentScopeLabel(0)
			_, explicitScopeLabel := iterator.FetchScopeLabel(state.ThreadId(), 0, 0)
			check(!currentLabel &&
				iterator.GetCurrentScopeLabelCount() == 0 &&
				iterator.GetScopeLabelCount(state.ThreadId(), 0) == 0 &&
				!currentScopeLabel && !explicitScopeLabel,
				"non-label entry exposed label data")
			checkedLabelAPIs = true
		}
		w.emit("LI", reportIndex, boolean(state.Started()), boolean(state.Finished()),
			state.ThreadStackDepth(), boolean(state.ThreadStackIsUnderflowed()),
			boolean(state.ThreadStackIsOverflowed()), boolean(state.ThreadStackWasOverflowed()),
			hexU64(state.ThreadPrevTimestamp()), state.FrameId(), state.FrameAbsoluteId(),
			hexU64(state.FrameDescLocator()), state.ThreadId(), boolean(state.IsGpuThread()),
			boolean(state.IsFrameStitching()), boolean(state.FrameChanged()),
			boolean(state.ThreadChanged()), hexU64(state.EntryLocator()), state.TimerId(),
			hexU64(state.Timestamp()), hexU64(state.TimestampGpu()), state.EntryTypeRaw(),
			state.EventType(), boolean(state.IsUnknown()), boolean(state.IsEnter()),
			boolean(state.IsExit()), boolean(state.IsLabel()), boolean(state.IsEvent()),
			boolean(state.IsThreadFrameEnd()), boolean(state.IsFrameBoundary()),
			iterator.GetCurrentEntryCount())
		reportIndex++
	}
	check(checkedLabelAPIs, "log iterator has no non-label entry")
	w.emit("LOG_ITERATOR_END", reportIndex, boolean(state.Finished()))
}

func main() {
	if len(os.Args) != 2 && len(os.Args) != 4 {
		panic("usage: parity-summary <capture.gprx> [<short-update.gprx> <long-update.gprx>]")
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
	session := library.OpenFromMem(capture)
	if session == nil || !session.IsValid() {
		panic("failed to open capture")
	}
	defer session.Dispose()
	checkTrackAndLabelAPIs(session)
	writer := bufio.NewWriterSize(os.Stdout, 1024*1024)
	defer writer.Flush()
	(&summaryWriter{out: writer, library: library, session: session}).write()
	shortUpdatePath, longUpdatePath := "", ""
	if len(os.Args) == 4 {
		shortUpdatePath, longUpdatePath = os.Args[2], os.Args[3]
	}
	checkCacheContract(library, session, capture, shortUpdatePath, longUpdatePath)
}

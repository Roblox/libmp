// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

package main

import (
	"fmt"
	"math/bits"
	"os"
	"strconv"
	"time"

	"libmp-benchmark/libmp"
)

func mix(hash uint32, value uint32) uint32 {
	return (bits.RotateLeft32(hash, 5) ^ value) + 0x9e3779b9
}

type workloadResult struct {
	checksum  uint32
	itemCount uint64
}

func workload(session *libmp.Session, passes int) workloadResult {
	hash := uint32(0x811c9dc5)
	itemCount := uint64(0)
	for pass := 0; pass < passes; pass++ {
		hash = mix(hash, session.GetFrameIdMin())
		hash = mix(hash, session.GetFrameIdMax())
		if session.FetchGlobalDesc() != nil {
			hash = mix(hash, 1)
		} else {
			hash = mix(hash, 0)
		}
		general := session.FetchGeneralInfo()
		if general != nil {
			hash = mix(hash, uint32(len(general.PlatformInfoJson)))
		} else {
			hash = mix(hash, 0)
		}
		itemCount += 2

		groupIDMax := session.GetGroupIdMax()
		hash = mix(hash, groupIDMax)
		itemCount += uint64(groupIDMax)
		for id := uint32(1); id <= groupIDMax; id++ {
			value := session.FetchGroupDesc(id)
			if value == nil {
				hash = mix(hash, 0)
				continue
			}
			hash = mix(hash, value.GroupId)
			hash = mix(hash, uint32(len(value.GroupName)))
			hash = mix(hash, value.Color)
			if value.IsGpu {
				hash = mix(hash, 1)
			} else {
				hash = mix(hash, 0)
			}
		}

		timerIDs := session.FetchTimerIds()
		hash = mix(hash, uint32(len(timerIDs)))
		itemCount += uint64(len(timerIDs))
		for _, id := range timerIDs {
			hash = mix(hash, id)
			value := session.FetchTimerDesc(id)
			if value == nil {
				hash = mix(hash, 0)
				continue
			}
			hash = mix(hash, value.TimerId)
			hash = mix(hash, uint32(len(value.TimerName)))
			hash = mix(hash, value.GroupId)
			hash = mix(hash, value.Color)
			if value.IsUserTimer {
				hash = mix(hash, 1)
			} else {
				hash = mix(hash, 0)
			}
		}

		counterIDs := session.FetchCounterIds()
		hash = mix(hash, uint32(len(counterIDs)))
		itemCount += uint64(len(counterIDs))
		for _, id := range counterIDs {
			hash = mix(hash, id)
			value := session.FetchCounterDesc(id)
			if value == nil {
				hash = mix(hash, 0)
				continue
			}
			hash = mix(hash, value.CounterId)
			hash = mix(hash, uint32(len(value.CounterName)))
			hash = mix(hash, value.Parent)
			hash = mix(hash, value.Sibling)
			hash = mix(hash, value.FirstChild)
			hash = mix(hash, uint32(value.Level))
		}

		threadIDs := session.FetchThreadIds()
		hash = mix(hash, uint32(len(threadIDs)))
		itemCount += uint64(len(threadIDs))
		for _, id := range threadIDs {
			hash = mix(hash, id)
			value := session.FetchThreadDesc(id)
			if value == nil {
				hash = mix(hash, 0)
				continue
			}
			hash = mix(hash, value.ThreadId)
			hash = mix(hash, uint32(len(value.ThreadName)))
			hash = mix(hash, value.BufferSize)
			if value.IsGpu {
				hash = mix(hash, 1)
			} else {
				hash = mix(hash, 0)
			}
		}
	}
	return workloadResult{hash, itemCount}
}

func logWorkload(session *libmp.Session, passes int) workloadResult {
	hash := uint32(0x811c9dc5)
	itemCount := uint64(0)
	for pass := 0; pass < passes; pass++ {
		iterator := session.CreateLogIterator()
		if !iterator.Configure(&libmp.LogIteratorConfigT{EmitThreadFrameEnd: true}) {
			iterator.Dispose()
			panic("failed to configure log iterator")
		}
		state := iterator.GetState()
		if state == nil {
			iterator.Dispose()
			panic("log iterator has no state")
		}
		entryCount := uint32(0)
		for iterator.Step() {
			hash = mix(hash, state.FrameId())
			hash = mix(hash, state.ThreadId())
			hash = mix(hash, state.TimerId())
			hash = mix(hash, state.EntryTypeRaw())
			hash = mix(hash, state.EventType())
			if state.IsEvent() {
				hash = mix(hash, 1)
			} else {
				hash = mix(hash, 0)
			}
			timer := session.FetchTimerDesc(state.TimerId())
			if timer != nil {
				hash = mix(hash, timer.TimerId)
				hash = mix(hash, uint32(len(timer.TimerName)))
				hash = mix(hash, timer.Color)
			} else {
				hash = mix(hash, 0)
			}
			entryCount++
		}
		iterator.Dispose()
		hash = mix(hash, entryCount)
		itemCount += uint64(entryCount)
	}
	return workloadResult{hash, itemCount}
}

func openSession(library *libmp.LibMPInstance, capture []byte, cache bool) *libmp.Session {
	session := library.OpenFromMem(capture)
	if session == nil || !session.IsValid() {
		panic("failed to open capture")
	}
	session.EnableObjectCache(cache)
	return session
}

func emit(scenario string, cache bool, result workloadResult, samples []int64) {
	fmt.Printf("BENCH\t%s\t%s\t%d\t%d\t", scenario, cacheName(cache),
		result.checksum, result.itemCount)
	for index, sample := range samples {
		if index != 0 {
			fmt.Print(",")
		}
		fmt.Print(sample)
	}
	fmt.Println()
}

func cacheName(cache bool) string {
	if cache {
		return "on"
	}
	return "off"
}

func runShared(library *libmp.LibMPInstance, capture []byte, cache bool, warmups, iterations, passes int) {
	session := openSession(library, capture, cache)
	defer session.Dispose()
	for index := 0; index < warmups; index++ {
		workload(session, passes)
	}
	samples := make([]int64, 0, iterations)
	var result workloadResult
	for index := 0; index < iterations; index++ {
		start := time.Now()
		current := workload(session, passes)
		samples = append(samples, time.Since(start).Nanoseconds())
		if index != 0 && current != result {
			panic("workload checksum changed")
		}
		result = current
	}
	emit("shared", cache, result, samples)
}

func runFresh(library *libmp.LibMPInstance, capture []byte, cache bool, warmups, iterations, passes int) {
	run := func() workloadResult {
		session := openSession(library, capture, cache)
		result := workload(session, passes)
		session.Dispose()
		return result
	}
	for index := 0; index < warmups; index++ {
		run()
	}
	samples := make([]int64, 0, iterations)
	var result workloadResult
	for index := 0; index < iterations; index++ {
		start := time.Now()
		current := run()
		samples = append(samples, time.Since(start).Nanoseconds())
		if index != 0 && current != result {
			panic("workload checksum changed")
		}
		result = current
	}
	emit("fresh", cache, result, samples)
}

func runLogs(library *libmp.LibMPInstance, capture []byte, cache bool, warmups, iterations, passes int) {
	session := openSession(library, capture, cache)
	defer session.Dispose()
	for index := 0; index < warmups; index++ {
		logWorkload(session, passes)
	}
	samples := make([]int64, 0, iterations)
	var result workloadResult
	for index := 0; index < iterations; index++ {
		start := time.Now()
		current := logWorkload(session, passes)
		samples = append(samples, time.Since(start).Nanoseconds())
		if index != 0 && current != result {
			panic("log workload checksum changed")
		}
		result = current
	}
	emit("logs", cache, result, samples)
}

func positive(text, name string, allowZero bool) int {
	value, err := strconv.Atoi(text)
	if err != nil || value < 0 || (!allowZero && value == 0) {
		panic("invalid " + name)
	}
	return value
}

func main() {
	if len(os.Args) != 8 {
		panic("usage: benchmark <capture> <warmups> <iterations> <metadata-passes> " +
			"<log-warmups> <log-iterations> <log-passes>")
	}
	capture, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	warmups := positive(os.Args[2], "warmup count", true)
	iterations := positive(os.Args[3], "iteration count", false)
	metadataPasses := positive(os.Args[4], "metadata pass count", false)
	logWarmups := positive(os.Args[5], "log warmup count", true)
	logIterations := positive(os.Args[6], "log iteration count", false)
	logPasses := positive(os.Args[7], "log pass count", false)
	library, err := libmp.New()
	if err != nil {
		panic(err)
	}
	defer library.Dispose()
	for _, cache := range []bool{false, true} {
		fmt.Fprintf(os.Stderr, "  cache=%s: metadata / reused session\n", cacheName(cache))
		runShared(library, capture, cache, warmups, iterations, metadataPasses)
		fmt.Fprintf(os.Stderr, "  cache=%s: metadata / new session\n", cacheName(cache))
		runFresh(library, capture, cache, warmups, iterations, metadataPasses)
		fmt.Fprintf(os.Stderr, "  cache=%s: full log traversal / reused session\n", cacheName(cache))
		runLogs(library, capture, cache, logWarmups, logIterations, logPasses)
	}
}

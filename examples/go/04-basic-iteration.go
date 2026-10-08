// 04-basic-iteration.go -- Iterating Scopes (Single Frame, Single Thread)
//
// Setup:
//   - Install Go 1.18+ and a C compiler on PATH. LibMP.go depends on
//     wasmtime-go, which uses cgo; on Windows, use a 64-bit MinGW-w64 GCC.
//   - Download LibMP.go from https://github.com/Roblox/libmp/releases, or build
//     it from the repository root with `python3 src/scripts/build.py --go`
//     (output: src/build/go/LibMP.go).
//   - Put this file in an empty directory and LibMP.go in its libmp/
//     subdirectory.
//
// Then run from that directory (go mod tidy downloads wasmtime-go):
//
//	go mod init libmp-example
//	go mod tidy
//	go run . /path/to/capture.x.html # or capture.gprx
//
// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

package main

import (
	"fmt"
	"os"
	"strings"

	libmp "libmp-example/libmp"
)

func run() error {
	if len(os.Args) != 2 {
		return fmt.Errorf("usage: go run . <capture.x.html|capture.gprx>")
	}

	library, err := libmp.New()
	if err != nil {
		return err
	}
	defer library.Dispose()

	session, err := library.OpenFromFile(os.Args[1])
	if err != nil {
		return err
	}
	if session == nil || !session.IsValid() {
		return fmt.Errorf("failed to open session")
	}
	defer session.Dispose()

	frameID := session.GetFrameIdMax()
	if frameID == 0 {
		fmt.Println("No frames available")
		return nil
	}

	mainThreadID := session.FindThreadId("*rbx main*")
	if mainThreadID == 0 {
		fmt.Println("No Main thread found")
		return nil
	}

	globalDesc := session.FetchGlobalDesc()
	threadDesc := session.FetchThreadDesc(mainThreadID)
	if globalDesc == nil || threadDesc == nil {
		return fmt.Errorf("required capture metadata is missing")
	}
	fmt.Printf("Frame %d, Thread: '%s'\n", frameID, threadDesc.ThreadName)
	fmt.Println(strings.Repeat("-", 60))

	iterator := session.CreateLogIterator()
	defer iterator.Dispose()
	if !iterator.Configure(&libmp.LogIteratorConfigT{
		ThreadIds: []uint32{mainThreadID},
	}) {
		return fmt.Errorf("failed to configure log iterator")
	}

	state := iterator.GetState()
	if state == nil {
		return fmt.Errorf("log iterator state is missing")
	}
	iterator.RewindTo(frameID, frameID)

	const scopeLimit = 50
	scopeCounter := 0
	for iterator.Step() {
		timer := session.FetchTimerDesc(state.TimerId())
		timerName := fmt.Sprintf("<timer %d>", state.TimerId())
		if timer != nil {
			timerName = timer.TimerName
		}

		if state.IsEnter() {
			fmt.Printf("%s> %s\n", strings.Repeat("  ", int(state.ThreadStackDepth())), timerName)
		} else if state.IsExit() {
			scopeCounter++
			if scopeCounter > scopeLimit {
				break
			}

			depth := state.ThreadStackDepth()
			duration := ""
			if !state.ThreadStackIsUnderflowed() {
				element := iterator.GetCurrentThreadStackElement(int32(depth))
				if element != nil {
					durationMS := float64(state.Timestamp()-element.EnterTimestamp()) *
						float64(globalDesc.TickToMsCpu)
					duration = fmt.Sprintf(" (%.3f ms)", durationMS)
				}
			}
			fmt.Printf("%s< %s%s\n", strings.Repeat("  ", int(depth)+1), timerName, duration)
		}
	}

	fmt.Println("\nDone.")
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

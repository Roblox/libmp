#!/usr/bin/env python3
"""Iterate scopes in one frame and one thread.

Setup (Python 3.10+):
- Download LibMP.py from https://github.com/Roblox/libmp/releases, or build it
  from the repository root with `python3 src/scripts/build.py --python`
  (output: src/build/python/LibMP.py).
- Place LibMP.py beside this file and install its only dependency:
  python -m pip install "wasmtime>=20.0.0,<50.0.0"
- Run: python 04-basic-iteration.py /path/to/capture.x.html  # or capture.gprx

This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.
"""

import sys

import LibMP


def display_text(value):
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python 04-basic-iteration.py <capture.x.html|capture.gprx>")

    with LibMP.create() as libmp:
        session = libmp.Session.OpenFromFile(sys.argv[1])
        if session is None:
            raise RuntimeError("Failed to open session")
        with session:
            if not session.IsValid():
                raise RuntimeError("Failed to open session")

            frame_id = session.GetFrameIdMax()
            if frame_id == 0:
                print("No frames available")
                return

            main_thread_id = session.FindThreadId("*rbx main*")
            if main_thread_id == 0:
                print("No Main thread found")
                return

            global_desc = session.FetchGlobalDesc()
            thread_desc = session.FetchThreadDesc(main_thread_id)
            if global_desc is None or thread_desc is None:
                raise RuntimeError("Required capture metadata is missing")

            print(f"Frame {frame_id}, Thread: '{display_text(thread_desc.threadName)}'")
            print("-" * 60)

            with session.CreateLogIterator() as iterator:
                if not iterator.Configure({"ThreadIds": [main_thread_id]}):
                    raise RuntimeError("Failed to configure log iterator")

                state = iterator.GetState()
                iterator.RewindTo(frame_id, frame_id)

                scope_limit = 50
                scope_counter = 0
                while iterator.Step():
                    timer = session.FetchTimerDesc(state.TimerId())
                    timer_name = (display_text(timer.timerName) if timer else
                                  f"<timer {state.TimerId()}>")
                    if state.IsEnter():
                        print(f"{'  ' * state.ThreadStackDepth()}> {timer_name}")
                    elif state.IsExit():
                        scope_counter += 1
                        if scope_counter > scope_limit:
                            break

                        depth = state.ThreadStackDepth()
                        duration = ""
                        if not state.ThreadStackIsUnderflowed():
                            element = iterator.GetCurrentThreadStackElement(depth)
                            if element:
                                duration_ms = ((state.Timestamp() - element.EnterTimestamp()) *
                                               global_desc.tickToMsCpu)
                                duration = f" ({duration_ms:.3f} ms)"
                        print(f"{'  ' * (depth + 1)}< {timer_name}{duration}")

    print("\nDone.")


if __name__ == "__main__":
    main()

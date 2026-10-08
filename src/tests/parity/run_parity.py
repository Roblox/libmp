#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Cross-language parity: every reader must produce the same canonical dump model.

By default, the runner builds a known-good current-format capture from the committed real
data in src/tests/cpp/roundtrip_capture_data.inc. Pass one or more --input paths or patterns
to test other GPRX files instead. For each input, every reader consumes the same capture.

Steps:
  1. Build the default capture, or select the requested input capture.
  2. Traverse all common public data families and every iterator report in every available
     language wrapper.
  3. Validate that each summary is structurally complete, then diff them byte-for-byte.
  4. Every wrapper runs the same cache-correctness contract and exits non-zero on failure.

The C++ and Luau readers are the required baseline. Other built language artifacts join the
same comparison when their runtimes are available; --require-all makes all readers mandatory.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

SRC = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SRC / "scripts"))

from _language_runner_utils import (  # noqa: E402
    build_csharp_runner,
    build_go_runner,
    python_wasmtime_environment,
)
from _utils import (  # noqa: E402
    NATIVE_BUILD_DIR,
    expand_file_patterns,
    find_built_executable,
    resolve_lute,
    stage_luau_script,
)

DEFAULT_PARITY_TOOL = (
    find_built_executable(NATIVE_BUILD_DIR, "Release", "parity_tool")
    or NATIVE_BUILD_DIR / "parity_tool"
)
DEFAULT_LIBMP_LUAU = SRC / "build" / "luau" / "LibMP.luau"
DEFAULT_LIBMP_JS = SRC / "build" / "js" / "LibMP.js"
DEFAULT_LIBMP_CSHARP = SRC / "build" / "csharp" / "LibMP.cs"
DEFAULT_LIBMP_PYTHON = SRC / "build" / "python" / "LibMP.py"
DEFAULT_LIBMP_GO = SRC / "build" / "go" / "LibMP.go"
LUAU_SCRIPT = SRC / "tests" / "luau" / "parity_summary.luau"
JS_SCRIPT = SRC / "tests" / "js" / "parity_summary.js"
PYTHON_SCRIPT = SRC / "tests" / "python" / "parity_summary.py"
CSHARP_PROJECT = SRC / "tests" / "csharp" / "LibMPParity.csproj"
GO_SCRIPT = SRC / "tests" / "go" / "parity_summary.go"
LUTE = resolve_lute()
INPUT_ENV = "LIBMP_PARITY_INPUTS"
REQUIRE_ALL_ENV = "LIBMP_PARITY_REQUIRE_ALL"
LUAU_ARTIFACT_ENV = "LIBMP_PARITY_LUAU_ARTIFACT"


class ReaderRunner:
    """One language implementation that emits the canonical summary."""

    def __init__(self, name, summarize, prerequisite_error, prepare=None, required=False):
        self.name = name
        self.summarize = summarize
        self.prerequisite_error = prerequisite_error
        self.prepare = prepare or (lambda: None)
        self.required = required


def skip(reason):
    print(f"SKIP cross-language parity: {reason}")
    sys.exit(77)


def run_luau(lute, module, capture, related_updates=None):
    short_update, long_update = related_updates or (None, None)
    replacements = {
        '"@PARITY_CAPTURE@"': json.dumps(capture.as_posix()),
        '"@PARITY_SHORT_UPDATE@"': json.dumps(
            short_update.as_posix() if short_update else ""),
        '"@PARITY_LONG_UPDATE@"': json.dumps(
            long_update.as_posix() if long_update else ""),
    }
    staged = stage_luau_script(
        module, LUAU_SCRIPT, replacements, prefix="_parity_summary_")
    try:
        return subprocess.run([str(lute), "run", str(staged)], capture_output=True, text=True)
    finally:
        staged.unlink(missing_ok=True)


def capture_arguments(capture, related_updates):
    arguments = [str(capture)]
    if related_updates:
        arguments.extend(str(path) for path in related_updates)
    return arguments


def create_runners(args):
    """The only registry entry point needed when another language wrapper is added."""
    python_state = {}
    csharp_state = {}
    go_state = {}

    def prepare_python():
        environment, error = python_wasmtime_environment(SRC)
        python_state["environment"] = environment
        return error

    def prepare_csharp():
        command, environment, error = build_csharp_runner(
            CSHARP_PROJECT, "LibMPParity", args.libmp_csharp)
        csharp_state["command"] = command
        csharp_state["environment"] = environment
        return error

    def prepare_go():
        command, error = build_go_runner(
            SRC, args.libmp_go, GO_SCRIPT, "parity", "libmp-parity", "libmp-parity")
        go_state["command"] = command
        return error

    return [
        ReaderRunner(
            "C++",
            lambda capture, related_updates=None: subprocess.run(
                [str(args.parity_tool), "summary", str(capture)],
                capture_output=True,
                text=True,
            ),
            lambda: None if args.parity_tool.is_file() else f"{args.parity_tool} not built",
            required=True,
        ),
        ReaderRunner(
            "Luau",
            lambda capture, related_updates=None: run_luau(
                LUTE, args.libmp_luau, capture, related_updates),
            lambda: (
                f"{args.libmp_luau} not built (run build.py --luau)"
                if not args.libmp_luau.is_file()
                else ("lute not found (set LIBMP_LUTE or add it to PATH)" if LUTE is None else None)
            ),
            required=True,
        ),
        ReaderRunner(
            "JavaScript",
            lambda capture, related_updates=None: subprocess.run(
                [shutil.which("node"), str(JS_SCRIPT), str(args.libmp_js),
                 *capture_arguments(capture, related_updates)],
                capture_output=True, text=True,
            ),
            lambda: (
                f"{args.libmp_js} not built (run build.py --js)"
                if not args.libmp_js.is_file()
                else ("node not found" if not shutil.which("node") else None)
            ),
        ),
        ReaderRunner(
            "C#",
            lambda capture, related_updates=None: subprocess.run(
                [*csharp_state["command"],
                 *capture_arguments(capture, related_updates)],
                env=csharp_state["environment"], capture_output=True, text=True,
            ),
            lambda: (
                f"{args.libmp_csharp} not built (run build.py --csharp)"
                if not args.libmp_csharp.is_file()
                else ("dotnet not found" if not shutil.which("dotnet") else None)
            ),
            prepare=prepare_csharp,
        ),
        ReaderRunner(
            "Python",
            lambda capture, related_updates=None: subprocess.run(
                [sys.executable, "-O", str(PYTHON_SCRIPT),
                 str(args.libmp_python), *capture_arguments(capture, related_updates)],
                env=python_state["environment"], capture_output=True, text=True,
            ),
            lambda: (
                f"{args.libmp_python} not built (run build.py --python)"
                if not args.libmp_python.is_file() else None
            ),
            prepare=prepare_python,
        ),
        ReaderRunner(
            "Go",
            lambda capture, related_updates=None: subprocess.run(
                [str(go_state["command"]),
                 *capture_arguments(capture, related_updates)],
                capture_output=True, text=True,
            ),
            lambda: (
                f"{args.libmp_go} not built (run build.py --go)"
                if not args.libmp_go.is_file()
                else ("go not found" if not shutil.which("go") else None)
            ),
            prepare=prepare_go,
        ),
    ]


def validate_complete_summary(implementation, summary, require_fixture_coverage=True):
    lines = summary.strip().splitlines()
    if not lines or lines[0] != "LIBMP_PARITY 3" or lines[-1] != "END":
        raise ValueError(f"{implementation}: missing parity format header or END marker")

    records = {}
    for line in lines:
        tag = line.split(" ", 1)[0]
        records.setdefault(tag, []).append(line.split())

    required_singletons = [
        "SESSION", "GLOBAL", "GENERAL", "GROUPS", "TIMER_IDS", "COUNTER_IDS",
        "GPU_THREAD", "THREAD_IDS", "FRAMES", "PLACE_ID_SAMPLES", "UTC_SAMPLES",
        "FIND_QUERIES", "GROUP_NAME_TIMER_QUERIES",
        "COUNTER_ITERATOR_BEGIN", "COUNTER_ITERATOR_END", "LOG_ITERATOR_BEGIN",
        "LOG_ITERATOR_END",
    ]
    missing = [tag for tag in required_singletons if len(records.get(tag, [])) != 1]
    if missing:
        raise ValueError(f"{implementation}: missing or repeated sections: {', '.join(missing)}")

    def integer(row, index, description):
        if len(row) <= index:
            raise ValueError(f"{implementation}: truncated {description} record")
        try:
            return int(row[index])
        except ValueError as error:
            raise ValueError(f"{implementation}: invalid integer in {description} record") from error

    count_pairs = [
        ("GROUPS", "GROUP"),
        ("TIMER_IDS", "TIMER_ID"),
        ("COUNTER_IDS", "COUNTER_ID"),
        ("THREAD_IDS", "THREAD_ID"),
        ("FRAMES", "FRAME"),
        ("PLACE_ID_SAMPLES", "PLACE_ID_SAMPLE"),
        ("UTC_SAMPLES", "UTC_SAMPLE"),
    ]
    for header, item in count_pairs:
        expected = integer(records[header][0], 1, header)
        actual = len(records.get(item, []))
        if actual != expected:
            raise ValueError(f"{implementation}: {header} declares {expected}, found {actual} {item} records")

    descriptor_pairs = [
        ("TIMER_IDS", "TIMER"),
        ("COUNTER_IDS", "COUNTER"),
        ("THREAD_IDS", "THREAD"),
    ]
    for header, descriptor in descriptor_pairs:
        expected = integer(records[header][0], 1, header)
        actual = len(records.get(descriptor, []))
        if actual != expected:
            raise ValueError(
                f"{implementation}: {header} declares {expected}, found {actual} {descriptor} records"
            )

    find_query_count = integer(records["FIND_QUERIES"][0], 1, "FIND_QUERIES")
    if find_query_count <= 0:
        raise ValueError(f"{implementation}: FIND_QUERIES must not be empty")
    expected_find_records = find_query_count * 4
    find_records = records.get("FIND", [])
    if len(find_records) != expected_find_records:
        raise ValueError(
            f"{implementation}: expected {expected_find_records} FIND records, "
            f"found {len(find_records)}"
        )
    for row in find_records:
        result_count = integer(row, 6, "FIND")
        if len(row) != 7 + result_count:
            raise ValueError(f"{implementation}: malformed FIND result count")
        first = integer(row, 5, "FIND")
        expected_first = integer(row, 7, "FIND") if result_count else 0
        if first != expected_first:
            raise ValueError(f"{implementation}: FIND first result disagrees with result array")

    expected_families = {"GROUP", "TIMER", "COUNTER", "THREAD"}
    for probe_index in range(find_query_count):
        families = {
            row[1] for row in find_records
            if integer(row, 2, "FIND") == probe_index
        }
        if families != expected_families:
            raise ValueError(
                f"{implementation}: FIND probe {probe_index} has invalid families"
            )

    group_query_count = integer(
        records["GROUP_NAME_TIMER_QUERIES"][0], 1, "GROUP_NAME_TIMER_QUERIES")
    if group_query_count != find_query_count:
        raise ValueError(
            f"{implementation}: group-name and find query counts differ"
        )
    group_query_records = records.get("GROUP_NAME_TIMERS", [])
    if len(group_query_records) != group_query_count:
        raise ValueError(
            f"{implementation}: expected {group_query_count} GROUP_NAME_TIMERS records, "
            f"found {len(group_query_records)}"
        )
    for row in group_query_records:
        result_count = integer(row, 4, "GROUP_NAME_TIMERS")
        if len(row) != 5 + result_count:
            raise ValueError(f"{implementation}: malformed GROUP_NAME_TIMERS result count")

    present_counters = sum(1 for row in records.get("COUNTER", [])
                           if len(row) >= 3 and row[2] == "1")
    for item in ("COUNTER_SAMPLES", "COUNTER_LAST"):
        actual = len(records.get(item, []))
        if actual != present_counters:
            raise ValueError(
                f"{implementation}: found {present_counters} counters but {actual} {item} records"
            )

    expected_thread_logs = 0
    for frame in records.get("FRAME", []):
        if len(frame) < 3:
            raise ValueError(f"{implementation}: truncated FRAME record")
        if frame[2] == "1":
            expected_thread_logs += integer(frame, 13, "FRAME")
    actual_thread_logs = len(records.get("FRAME_THREAD_LOG", []))
    if actual_thread_logs != expected_thread_logs:
        raise ValueError(
            f"{implementation}: frames declare {expected_thread_logs} thread logs, "
            f"found {actual_thread_logs}"
        )

    expected_samples = sum(integer(row, 2, "COUNTER_SAMPLES")
                           for row in records.get("COUNTER_SAMPLES", []))
    actual_samples = len(records.get("COUNTER_SAMPLE", []))
    if actual_samples != expected_samples:
        raise ValueError(
            f"{implementation}: counters declare {expected_samples} samples, found {actual_samples}"
        )

    iterator_pairs = [
        ("COUNTER_ITERATOR_END", "CI"),
        ("LOG_ITERATOR_END", "LI"),
    ]
    for footer, item in iterator_pairs:
        expected = integer(records[footer][0], 1, footer)
        actual = len(records.get(item, []))
        if actual != expected:
            raise ValueError(f"{implementation}: {footer} declares {expected}, found {actual} {item} records")

    log_records = records.get("LI", [])
    if any(len(row) != 31 for row in log_records):
        raise ValueError(f"{implementation}: malformed LI record")
    for row in log_records:
        entry_count = integer(row, 30, "LI")
        if entry_count < 0 or entry_count > 6:
            raise ValueError(f"{implementation}: invalid current-entry count")
        if (row[28] == "1" or row[29] == "1") and entry_count != 0:
            raise ValueError(
                f"{implementation}: synthetic LI record has raw entries")
        if (row[24] == "1" or row[25] == "1") and entry_count != 1:
            raise ValueError(
                f"{implementation}: scope LI record does not contain one raw entry")
        if row[27] == "1" and entry_count == 0:
            raise ValueError(
                f"{implementation}: event LI record has no raw entries")
    required_log_kinds = {
        "scope-enter": lambda row: row[24] == "1",
        "scope-exit": lambda row: row[25] == "1",
        "event": lambda row: row[27] == "1",
        "thread-end": lambda row: row[28] == "1",
        "frame-boundary": lambda row: row[29] == "1",
    }
    if require_fixture_coverage:
        absent = [name for name, predicate in required_log_kinds.items()
                  if not any(predicate(row) for row in log_records)]
        if absent:
            raise ValueError(f"{implementation}: log traversal did not cover {', '.join(absent)}")

    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--parity-tool", type=Path, default=DEFAULT_PARITY_TOOL)
    parser.add_argument(
        "--libmp-luau",
        type=Path,
        default=Path(os.environ.get(LUAU_ARTIFACT_ENV, DEFAULT_LIBMP_LUAU)),
    )
    parser.add_argument("--libmp-js", type=Path, default=DEFAULT_LIBMP_JS)
    parser.add_argument("--libmp-csharp", type=Path, default=DEFAULT_LIBMP_CSHARP)
    parser.add_argument("--libmp-python", type=Path, default=DEFAULT_LIBMP_PYTHON)
    parser.add_argument("--libmp-go", type=Path, default=DEFAULT_LIBMP_GO)
    parser.add_argument(
        "--require-all",
        action="store_true",
        default=os.environ.get(REQUIRE_ALL_ENV) == "1",
        help="fail unless C++, Luau, JavaScript, C#, Python, and Go can all run",
    )
    parser.add_argument(
        "--input",
        action="append",
        nargs="+",
        metavar="PATH_OR_GLOB",
        help=(
            "existing GPRX files, directories, or glob patterns to test; repeat as needed; "
            "by default a capture is reconstructed from the committed source-data fixture"
        ),
    )
    args = parser.parse_args()
    for name in (
        "parity_tool", "libmp_luau", "libmp_js", "libmp_csharp",
        "libmp_python", "libmp_go",
    ):
        setattr(args, name, getattr(args, name).resolve())

    runners = create_runners(args)
    active_runners = []
    for runner in runners:
        error = runner.prerequisite_error()
        if error:
            if runner.required:
                skip(f"{runner.name}: {error}")
            if args.require_all:
                print(f"required parity reader unavailable: {runner.name}: {error}", file=sys.stderr)
                return 2
            print(f"Skipping optional parity reader {runner.name}: {error}")
            continue
        try:
            error = runner.prepare()
        except (OSError, RuntimeError) as prepare_error:
            print(f"failed to prepare {runner.name} parity reader: {prepare_error}", file=sys.stderr)
            return 1
        if error:
            if args.require_all:
                print(f"required parity reader unavailable: {runner.name}: {error}", file=sys.stderr)
                return 2
            print(f"Skipping optional parity reader {runner.name}: {error}")
            continue
        active_runners.append(runner)

    requested_inputs = [value for group in (args.input or []) for value in group]
    if not requested_inputs and os.environ.get(INPUT_ENV):
        try:
            requested_inputs = json.loads(os.environ[INPUT_ENV])
        except json.JSONDecodeError as error:
            print(f"invalid {INPUT_ENV}: {error}", file=sys.stderr)
            return 2
        if not isinstance(requested_inputs, list) or not all(
                isinstance(value, str) for value in requested_inputs):
            print(f"invalid {INPUT_ENV}: expected a JSON string array", file=sys.stderr)
            return 2

    try:
        captures = expand_file_patterns(requested_inputs, "*.gprx") if requested_inputs else []
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    with TemporaryDirectory(prefix="libmp-parity-") as temporary_directory_text:
        temporary_directory = Path(temporary_directory_text)
        using_default_fixture = not captures
        related_updates = None
        if using_default_fixture:
            capture = temporary_directory / "fixture.gprx"
            short_update = temporary_directory / "fixture-short-update.gprx"
            long_update = temporary_directory / "fixture-long-update.gprx"
            build = subprocess.run(
                [str(args.parity_tool), "build-related", str(capture),
                 str(short_update), str(long_update)],
                capture_output=True,
                text=True,
            )
            if build.returncode != 0:
                print("capture build failed:", build.stderr.strip())
                return 1
            print("Parity input: reconstructed committed fixture", flush=True)
            captures = [capture]
            related_updates = (short_update, long_update)

        for capture in captures:
            if not using_default_fixture:
                print(f"Parity input: {capture}", flush=True)
            if run_parity(
                    active_runners, capture, using_default_fixture, related_updates) != 0:
                return 1
        if len(captures) > 1:
            print(f"ALL PARITY INPUTS PASSED ({len(captures)} captures)")
        return 0


def run_parity(
        runners, capture, require_fixture_coverage, related_updates=None):
    summaries = {}
    for runner in runners:
        result = runner.summarize(capture, related_updates)
        if result.returncode != 0:
            print(f"{runner.name} reader FAILED:")
            print(result.stderr)
            return 1
        summaries[runner.name] = result.stdout
    validated = {}
    try:
        for implementation, summary in summaries.items():
            validated[implementation] = validate_complete_summary(
                implementation, summary, require_fixture_coverage)
    except ValueError as error:
        print(f"INCOMPLETE PARITY SUMMARY: {error}")
        return 1

    reference_name, reference = next(iter(summaries.items()))
    reference_lines = validated[reference_name]
    for implementation, summary in list(summaries.items())[1:]:
        if reference.strip() == summary.strip():
            continue
        print(f"PARITY MISMATCH: {implementation} differs from {reference_name}.")
        implementation_lines = validated[implementation]
        for i in range(max(len(reference_lines), len(implementation_lines))):
            expected = reference_lines[i] if i < len(reference_lines) else "<none>"
            actual = implementation_lines[i] if i < len(implementation_lines) else "<none>"
            if expected != actual:
                print(
                    f"  line {i}: {reference_name}[{expected}] != "
                    f"{implementation}[{actual}]"
                )
                if i > 3:
                    break
        return 1

    print("PARITY OK")
    print(
        f"  {len(reference_lines)} summary lines matched across "
        f"{len(summaries)} implementations"
    )
    # A zero exit from each wrapper means its additional cache contract passed.
    print("  Wrapper cache-correctness checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

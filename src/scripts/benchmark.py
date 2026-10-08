#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Benchmark built LibMP language artifacts against one common capture and workload."""

import argparse
import shutil
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Dict, List, Optional

from _language_runner_utils import (
    build_csharp_runner,
    build_go_runner,
    python_wasmtime_environment,
    run_build,
)
from _utils import resolve_lute, stage_luau_script


SRC = Path(__file__).resolve().parent.parent
BUILD = SRC / "build"
BENCHMARKS = SRC / "tests" / "benchmark"
LANGUAGES = ("native", "luau", "js", "csharp", "python", "go")


@dataclass
class Runner:
    key: str
    name: str
    mode: str
    command: List[str]
    environment: Optional[Dict[str, str]] = None
    cleanup_path: Optional[Path] = None


@dataclass
class Result:
    language: str
    scenario: str
    cache: str
    checksum: int
    item_count: int
    samples: List[int]


def executable(directory: Path, name: str) -> Optional[Path]:
    candidates = (
        directory / name,
        directory / f"{name}.exe",
        directory / "Release" / name,
        directory / "Release" / f"{name}.exe",
    )
    return next((path for path in candidates if path.is_file()), None)


def prepare_luau(capture: Path, counts: List[str]):
    lute = resolve_lute()
    if lute is None:
        return None, "lute is absent (install it or set LIBMP_LUTE)"

    artifacts = []
    standard = BUILD / "luau" / "LibMP.luau"
    if standard.is_file():
        artifacts.append((standard, "Luau", "Lute VM + release WASM"))
    if BUILD.is_dir():
        for directory in sorted(BUILD.iterdir(), key=lambda path: path.name.casefold()):
            artifact = directory / "LibMP.luau"
            if (directory.is_dir() and directory.name.casefold().startswith("luau-")
                    and artifact.is_file()):
                artifacts.append((
                    artifact, directory.name,
                    f"Lute VM + {directory.name} library",
                ))
    if not artifacts:
        return None, (
            "no build/luau/LibMP.luau or build/Luau-*/LibMP.luau artifact "
            "(run build.py --luau)"
        )

    runners = []
    for artifact, name, mode in artifacts:
        staged = stage_luau_script(
            artifact, BENCHMARKS / "benchmark.luau", prefix="_benchmark_")
        runners.append(Runner(
            "luau", name, mode,
            [str(lute), "run", str(staged), str(capture), *counts],
            cleanup_path=staged,
        ))
    return runners, None


def prepare_js(capture: Path, counts: List[str]):
    artifact = BUILD / "js" / "LibMP.js"
    node = shutil.which("node")
    if not artifact.is_file():
        return None, "build/js/LibMP.js is absent (run build.py --js)"
    if not node:
        return None, "node is absent"
    return [Runner(
        "js", "JavaScript", "Node/V8 + release WASM",
        [node, str(BENCHMARKS / "benchmark.js"), str(artifact), str(capture), *counts],
    )], None


def prepare_python(capture: Path, counts: List[str]):
    artifact = BUILD / "python" / "LibMP.py"
    if not artifact.is_file():
        return None, "build/python/LibMP.py is absent (run build.py --python)"
    environment, error = python_wasmtime_environment(SRC)
    if error:
        return None, error
    return [Runner(
        "python", "Python", "CPython -O + release WASM",
        [sys.executable, "-O", str(BENCHMARKS / "benchmark.py"),
         str(artifact), str(capture), *counts],
        environment,
    )], None


def prepare_csharp(capture: Path, counts: List[str]):
    artifact = BUILD / "csharp" / "LibMP.cs"
    if not artifact.is_file():
        return None, "build/csharp/LibMP.cs is absent (run build.py --csharp)"
    project = BENCHMARKS / "LibMPBenchmark.csproj"
    command, environment, error = build_csharp_runner(
        project, "LibMPBenchmark", artifact)
    if error:
        return None, error
    return [Runner(
        "csharp", "C#", ".NET Release/full JIT + release WASM",
        [*command, str(capture), *counts], environment,
    )], None


def prepare_go(capture: Path, counts: List[str]):
    artifact = BUILD / "go" / "LibMP.go"
    if not artifact.is_file():
        return None, "build/go/LibMP.go is absent (run build.py --go)"
    output, error = build_go_runner(
        SRC, artifact, BENCHMARKS / "benchmark.go", "benchmark",
        "libmp-benchmark", "libmp-benchmark",
    )
    if error:
        return None, error
    return [Runner(
        "go", "Go", "optimized Go binary + release WASM",
        [str(output), str(capture), *counts],
    )], None


def prepare_native(capture: Path, counts: List[str]):
    artifact = executable(BUILD / "native", "libmp_benchmark")
    if artifact is None:
        return None, (
            "native benchmark is absent "
            "(run build_native.py --config Release --tests, then build target libmp_benchmark)"
        )
    return [Runner(
        "native", "Native C++", "Release native binary",
        [str(artifact), str(capture), *counts],
    )], None


PREPARERS = {
    "native": prepare_native,
    "luau": prepare_luau,
    "js": prepare_js,
    "csharp": prepare_csharp,
    "python": prepare_python,
    "go": prepare_go,
}


def parse_output(
        runner: Runner, output: str,
        metadata_iterations: int, log_iterations: int) -> List[Result]:
    results = []
    for line in output.splitlines():
        if not line.startswith("BENCH\t"):
            continue
        fields = line.split("\t")
        if len(fields) != 6:
            raise RuntimeError(f"{runner.name}: malformed benchmark record: {line}")
        _, scenario, cache, checksum_text, item_count_text, samples_text = fields
        if scenario not in ("shared", "fresh", "logs") or cache not in ("off", "on"):
            raise RuntimeError(f"{runner.name}: invalid benchmark case: {line}")
        try:
            checksum = int(checksum_text)
            item_count = int(item_count_text)
            samples = [int(value) for value in samples_text.split(",")]
        except ValueError as error:
            raise RuntimeError(f"{runner.name}: non-integer benchmark result: {line}") from error
        expected_iterations = (
            log_iterations if scenario == "logs" else metadata_iterations)
        if (item_count < 0 or len(samples) != expected_iterations
                or any(value < 0 for value in samples)):
            raise RuntimeError(f"{runner.name}: invalid sample count or duration")
        results.append(Result(
            runner.name, scenario, cache, checksum, item_count, samples))
    expected = {
        (scenario, cache)
        for scenario in ("shared", "fresh", "logs")
        for cache in ("off", "on")
    }
    actual = {(result.scenario, result.cache) for result in results}
    if actual != expected or len(results) != len(expected):
        raise RuntimeError(f"{runner.name}: runner did not emit all six benchmark cases")
    return results


def duration(nanoseconds):
    return f"{nanoseconds / 1_000_000:.3f}"


def throughput(item_count, nanoseconds):
    if not nanoseconds:
        return "inf" if item_count else "0.0000"
    items_per_second = item_count * 1_000_000_000 / nanoseconds
    return f"{items_per_second / 1_000_000:.4f}"


def print_table(results: List[Result]):
    medians = {
        (result.language, result.scenario, result.cache): statistics.median(result.samples)
        for result in results
    }
    native_medians = {
        (result.scenario, result.cache): medians[
            (result.language, result.scenario, result.cache)]
        for result in results if result.language == "Native C++"
    }
    headers = (
        "Language", "Workload", "Session", "Cache",
        "Median ms", "M items/s", "vs C++", "Cache benefit",
    )
    language_order = {
        language: index for index, language in enumerate(dict.fromkeys(
            result.language for result in results))
    }
    ordered_results = sorted(results, key=lambda result: (
        language_order[result.language],
        {"shared": 0, "fresh": 1, "logs": 2}[result.scenario],
        0 if result.cache == "off" else 1,
    ))
    rows = []
    for result in ordered_results:
        median = medians[(result.language, result.scenario, result.cache)]
        benefit = "-"
        if result.cache == "on":
            uncached = medians[(result.language, result.scenario, "off")]
            benefit = f"{uncached / median:.3f}x" if median else "inf"
        native = native_medians.get((result.scenario, result.cache))
        versus_native = f"{median / native:.3f}x" if native else "-"
        rows.append((
            result.language,
            "metadata lookups" if result.scenario != "logs" else "log entries",
            "new each run" if result.scenario == "fresh" else "reused",
            result.cache,
            duration(median),
            throughput(result.item_count, median),
            versus_native,
            benefit,
        ))
    widths = [max(len(headers[index]), *(len(row[index]) for row in rows))
              for index in range(len(headers))]
    print("  ".join(value.ljust(widths[index]) for index, value in enumerate(headers)))
    print("  ".join("-" * width for width in widths))
    for row in rows:
        print("  ".join(
            value.rjust(widths[index]) if index >= 4 else value.ljust(widths[index])
            for index, value in enumerate(row)
        ))


def create_fixture(parity_tool: Path, directory: Path) -> Path:
    if not parity_tool.is_file():
        raise RuntimeError(
            "no --input was given and parity_tool is absent; build native tests in Release "
            "or pass an existing GPRX capture with --input"
        )
    capture = directory / "benchmark.gprx"
    run_build([str(parity_tool), "build", str(capture)])
    return capture


def run_runner(runner: Runner):
    process = subprocess.Popen(
        runner.command,
        env=runner.environment,
        stdout=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    output = []
    assert process.stdout is not None
    for line in process.stdout:
        if line.startswith("PROGRESS\t"):
            print(f"  {line.removeprefix('PROGRESS').strip()}", flush=True)
        else:
            output.append(line)
    return process.wait(), "".join(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--input", type=Path,
                        help="GPRX capture (default: parity fixture generated from source data)")
    parser.add_argument("--warmups", type=int, default=2,
                        help="untimed metadata iterations per case (default: 2)")
    parser.add_argument("--iterations", type=int, default=7,
                        help="timed metadata iterations per case (default: 7)")
    parser.add_argument("--passes", type=int, default=3,
                        help="repeated metadata traversals per iteration (default: 3)")
    parser.add_argument("--log-warmups", type=int, default=1,
                        help="untimed full-log iterations per cache mode (default: 1)")
    parser.add_argument("--log-iterations", type=int, default=2,
                        help="timed full-log iterations per cache mode (default: 2)")
    parser.add_argument("--log-passes", type=int, default=1,
                        help="complete log traversals per log iteration (default: 1)")
    parser.add_argument("--language", action="append", choices=LANGUAGES,
                        help="language to run; repeat to select several (default: all available)")
    parser.add_argument("--parity-tool", type=Path,
                        default=None,
                        help="fixture generator used when --input is omitted")
    args = parser.parse_args(argv)
    if (args.warmups < 0 or args.iterations <= 0 or args.passes <= 0 or
            args.log_warmups < 0 or args.log_iterations <= 0 or args.log_passes <= 0):
        parser.error(
            "warmup counts must be non-negative; iteration and pass counts must be positive")
    selected = tuple(dict.fromkeys(args.language or LANGUAGES))

    runners = []
    try:
        with TemporaryDirectory(prefix="libmp-benchmark-") as temp:
            parity_tool = args.parity_tool.resolve() if args.parity_tool else (
                executable(BUILD / "native", "parity_tool")
                or BUILD / "native" / "parity_tool"
            )
            capture = args.input.resolve() if args.input else create_fixture(
                parity_tool, Path(temp))
            if not capture.is_file():
                parser.error(f"capture does not exist: {capture}")
            counts = [
                str(args.warmups), str(args.iterations),
                str(args.passes), str(args.log_warmups),
                str(args.log_iterations), str(args.log_passes),
            ]
            skipped = []
            for language in selected:
                prepared, reason = PREPARERS[language](capture, counts)
                if prepared:
                    runners.extend(prepared)
                else:
                    skipped.append((language, reason))
            if not runners:
                raise RuntimeError("no requested benchmark runner is available")

            print(f"Capture: {capture}")
            print(f"Metadata: {args.warmups} warmups, {args.iterations} measured, "
                  f"{args.passes} passes per iteration")
            print(f"Logs: {args.log_warmups} warmups, {args.log_iterations} measured, "
                  f"{args.log_passes} full traversals per iteration")
            results = []
            for runner in runners:
                print(f"Running {runner.name} ({runner.mode})...", flush=True)
                returncode, output = run_runner(runner)
                if returncode:
                    details = output.strip()
                    if not details:
                        details = "see the runner diagnostics above"
                    raise RuntimeError(f"{runner.name} benchmark failed:\n{details}")
                results.extend(parse_output(
                    runner, output, args.iterations, args.log_iterations))

            checksum_groups = {
                "metadata": {
                    result.checksum for result in results if result.scenario != "logs"
                },
                "log iteration": {
                    result.checksum for result in results if result.scenario == "logs"
                },
            }
            invalid_checksums = {
                name: checksums for name, checksums in checksum_groups.items()
                if len(checksums) != 1
            }
            if invalid_checksums:
                detail = ", ".join(
                    f"{result.language}/{result.scenario}/{result.cache}={result.checksum}"
                    for result in results
                )
                raise RuntimeError(f"benchmark workloads produced different results: {detail}")

            item_count_groups = {
                "metadata object lookups": {
                    result.item_count for result in results if result.scenario != "logs"
                },
                "log entries": {
                    result.item_count for result in results if result.scenario == "logs"
                },
            }
            invalid_item_counts = {
                name: counts for name, counts in item_count_groups.items()
                if len(counts) != 1
            }
            if invalid_item_counts:
                detail = ", ".join(
                    f"{result.language}/{result.scenario}/{result.cache}={result.item_count}"
                    for result in results
                )
                raise RuntimeError(f"benchmark workloads processed different item counts: {detail}")

            print()
            print_table(results)
            print("\nChecksums: " + ", ".join(
                f"{name}={next(iter(checksums))}"
                for name, checksums in checksum_groups.items()
            ) + " (identical across languages and cache modes)")
            print("Work per timed iteration: " + ", ".join(
                f"{name}={next(iter(counts))}"
                for name, counts in item_count_groups.items()
            ))
            if skipped:
                print("\nSkipped unavailable runners:")
                for language, reason in skipped:
                    print(f"  {language}: {reason}")
            return 0
    except (OSError, RuntimeError) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        return 1
    finally:
        for runner in runners:
            if runner.cleanup_path:
                runner.cleanup_path.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())

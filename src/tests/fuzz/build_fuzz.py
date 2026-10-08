#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build, replay, and run LibMP's libFuzzer targets under ASan/UBSan."""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


FUZZ_DIR = Path(__file__).resolve().parent
SRC = FUZZ_DIR.parent.parent
BUILD_ROOT = SRC / "build" / "fuzz"
SEED_ROOT = FUZZ_DIR / "corpus"
DEFAULT_STATE_ROOT = SRC / "build" / "fuzz-state"

HARNESSES = (
    "control_channel",
    "getdatainrange",
    "gprx_reader",
    "codec",
)

DURATION_PATTERN = re.compile(r"([1-9][0-9]*)([smh]?)")
FINAL_STAT_PATTERN = re.compile(r"stat::([a-z_]+):\s*([0-9]+(?:\.[0-9]+)?)")
COVERAGE_PATTERN = re.compile(r"cov:\s*([0-9]+).*?ft:\s*([0-9]+)")
MANAGED_FUZZER_ARGUMENTS = (
    "-max_total_time=",
    "-timeout=",
    "-rss_limit_mb=",
    "-seed=",
    "-artifact_prefix=",
    "-reload=",
    "-print_final_stats=",
)


def parse_duration(value):
    match = DURATION_PATTERN.fullmatch(value)
    if not match:
        raise argparse.ArgumentTypeError("duration must be a positive integer followed by s, m, or h")
    amount = int(match.group(1))
    multiplier = {"": 1, "s": 1, "m": 60, "h": 60 * 60}[match.group(2)]
    return amount * multiplier


def validate_fuzzer_arguments(arguments):
    for argument in arguments:
        if not argument.startswith("-"):
            raise ValueError(f"libFuzzer argument must start with '-': {argument}")
        if argument.startswith(MANAGED_FUZZER_ARGUMENTS):
            raise ValueError(f"use the runner option instead of overriding {argument.split('=', 1)[0]}")


def find_tool(env_name, candidates):
    configured = os.environ.get(env_name)
    if configured:
        path = shutil.which(configured)
        if path:
            return path
        raise RuntimeError(f"{env_name} names an unavailable executable: {configured}")
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError(f"none of these executables is available: {', '.join(candidates)}")


def find_gcc_install_dir():
    configured = os.environ.get("LIBMP_FUZZ_GCC_INSTALL_DIR")
    if configured:
        return configured
    roots = sorted(
        {
            path.parent
            for pattern in ("*/*/libstdc++.so", "*/*/libstdc++.a")
            for path in Path("/usr/lib/gcc").glob(pattern)
        },
        key=lambda path: tuple(
            int(part) if part.isdigit() else part for part in path.parts
        ),
        reverse=True,
    )
    return str(roots[0]) if roots else ""


def sanitizer_flags(with_asserts, gcc_install_dir):
    flags = [
        "-g",
        "-O1",
        "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
        "-fno-sanitize=vptr,function",
        "-fno-sanitize-recover=all",
    ]
    if gcc_install_dir:
        flags.insert(0, f"--gcc-install-dir={gcc_install_dir}")
    if not with_asserts:
        flags.append("-DNDEBUG")
    return flags


def configure_and_build_lib(compiler, gcc_install_dir, build_dir, with_asserts, build_jobs):
    instrument = sanitizer_flags(with_asserts, gcc_install_dir) + ["-fsanitize=fuzzer-no-link"]
    configure = [
        "cmake", "-S", str(SRC), "-B", str(build_dir),
        "-G", "Ninja",
        f"-DCMAKE_CXX_COMPILER={compiler}",
        f"-DCMAKE_CXX_FLAGS={' '.join(instrument)}",
        "-DLIBMP_BUILD_TESTS=OFF",
    ]
    subprocess.run(configure, check=True)
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "libmp", "--parallel", str(build_jobs)],
        check=True,
    )
    library = next(build_dir.rglob("libmp.a"), None)
    if library is None:
        raise RuntimeError("libmp.a not found after build")
    return library


def build_harness(compiler, gcc_install_dir, build_dir, name, library, with_asserts):
    source = FUZZ_DIR / f"{name}_fuzzer.cpp"
    if not source.exists():
        raise RuntimeError(f"fuzzer source not found: {source}")
    output = build_dir / f"fuzz_{name}"
    command = [
        compiler, "-std=c++17", *sanitizer_flags(with_asserts, gcc_install_dir),
        "-fsanitize=fuzzer", f"-I{SRC}", f"-I{SRC / 'dependencies'}",
        str(source), str(library), "-o", str(output),
    ]
    subprocess.run(command, check=True)
    return output


def build_seed_generator(compiler, gcc_install_dir, build_dir, library, with_asserts):
    output = build_dir / "make_seed_corpus"
    command = [
        compiler, "-std=c++17", *sanitizer_flags(with_asserts, gcc_install_dir),
        f"-I{SRC}", f"-I{SRC / 'dependencies'}", f"-I{SRC / 'tests' / 'cpp'}",
        str(FUZZ_DIR / "make_seed_corpus.cpp"), str(library), "-o", str(output),
    ]
    subprocess.run(command, check=True)
    return output


def generate_seed_corpus(executable, output_root):
    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)
    subprocess.run([str(executable), str(output_root)], check=True)


def corpus_directories(name, generated_seed_root, state_root, include_work=True):
    directories = []
    if include_work:
        work = state_root / "corpus" / name
        work.mkdir(parents=True, exist_ok=True)
        directories.append(work)
    directories.extend((generated_seed_root / name, SEED_ROOT / name))
    return [directory for directory in directories if directory.is_dir()]


def corpus_files(directories):
    return sorted(
        path
        for directory in directories
        for path in directory.iterdir()
        if path.is_file()
    )


def smoke_harness(executable, name, generated_seed_root, state_root, include_work):
    directories = corpus_directories(name, generated_seed_root, state_root, include_work)
    seeds = corpus_files(directories)
    if not seeds:
        raise RuntimeError(f"no seeds available for {name}")
    for seed in seeds:
        result = subprocess.run(
            [str(executable), str(seed)],
            cwd=str(executable.parent),
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise RuntimeError(f"{name} failed while replaying {seed}")


def campaign_command(
    executable,
    corpus_dirs,
    artifact_dir,
    duration_seconds,
    timeout_seconds,
    rss_limit_mb,
    random_seed,
    max_length,
    extra_arguments,
):
    command = [
        str(executable),
        *(str(path) for path in corpus_dirs),
        f"-max_total_time={duration_seconds}",
        f"-timeout={timeout_seconds}",
        f"-rss_limit_mb={rss_limit_mb}",
        f"-seed={random_seed}",
        f"-artifact_prefix={artifact_dir}{os.sep}",
        "-reload=1",
        "-print_final_stats=1",
    ]
    if max_length:
        command.append(f"-max_len={max_length}")
    command.extend(extra_arguments)
    return command


def snapshot_files(directory):
    if not directory.exists():
        return {}
    return {
        path.relative_to(directory).as_posix(): path.stat().st_size
        for path in directory.rglob("*")
        if path.is_file()
    }


def snapshot_summary(snapshot):
    return {"files": len(snapshot), "bytes": sum(snapshot.values())}


def parse_fuzzer_logs(paths):
    totals = {}
    coverage = 0
    features = 0
    for path in paths:
        contents = path.read_text(encoding="utf-8", errors="replace")
        for name, value in FINAL_STAT_PATTERN.findall(contents):
            parsed = float(value) if "." in value else int(value)
            if name in (
                "number_of_executed_units",
                "new_units_added",
                "average_exec_per_sec",
            ):
                totals[name] = totals.get(name, 0) + parsed
            else:
                totals[name] = max(totals.get(name, 0), parsed)
        for cov, ft in COVERAGE_PATTERN.findall(contents):
            coverage = max(coverage, int(cov))
            features = max(features, int(ft))
    totals["coverage_edges"] = coverage
    totals["coverage_features"] = features
    return totals


def terminate_processes(processes):
    for item in processes:
        process = item["process"]
        if process.poll() is not None:
            continue
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGTERM)
            else:
                process.terminate()
        except ProcessLookupError:
            pass
    for item in processes:
        process = item["process"]
        if process.poll() is None:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if os.name == "posix":
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
                process.wait()


def run_campaigns(
    executables,
    generated_seed_root,
    state_root,
    duration_seconds,
    workers,
    timeout_seconds,
    rss_limit_mb,
    base_seed,
    max_length,
    extra_arguments,
    with_asserts,
):
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    campaign_name = f"{timestamp}-{'assert' if with_asserts else 'no-assert'}"
    logs_root = state_root / "logs"
    logs_root.mkdir(parents=True, exist_ok=True)
    processes = []
    before = {}
    artifact_dirs = {}
    interrupted = False
    failed_worker = None

    try:
        for target_index, (name, executable) in enumerate(executables):
            work_corpus = state_root / "corpus" / name
            artifact_dir = state_root / "artifacts" / name / campaign_name
            work_corpus.mkdir(parents=True, exist_ok=True)
            artifact_dir.mkdir(parents=True, exist_ok=True)
            before[name] = snapshot_files(work_corpus)
            artifact_dirs[name] = artifact_dir
            inputs = corpus_directories(name, generated_seed_root, state_root)

            for worker in range(workers):
                worker_seed = (base_seed + target_index * workers + worker) & 0xFFFFFFFF
                if worker_seed == 0:
                    worker_seed = 1
                log_path = logs_root / (
                    f"{campaign_name}-{name}-w{worker}.log"
                )
                command = campaign_command(
                    executable,
                    inputs,
                    artifact_dir,
                    duration_seconds,
                    timeout_seconds,
                    rss_limit_mb,
                    worker_seed,
                    max_length,
                    extra_arguments,
                )
                print(f"Starting {name} worker {worker + 1}/{workers}; log: {log_path}", flush=True)
                log = open(log_path, "w", encoding="utf-8")
                try:
                    process = subprocess.Popen(
                        command,
                        cwd=str(executable.parent),
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        start_new_session=os.name == "posix",
                    )
                except BaseException:
                    log.close()
                    raise
                processes.append({
                    "target": name,
                    "worker": worker,
                    "seed": worker_seed,
                    "log_path": log_path,
                    "log": log,
                    "process": process,
                    "started": time.monotonic(),
                    "elapsed_seconds": None,
                    "returncode": None,
                    "cancelled": False,
                })

        remaining = len(processes)
        next_status = time.monotonic() + 60
        while remaining:
            for item in processes:
                if item["returncode"] is not None:
                    continue
                returncode = item["process"].poll()
                if returncode is not None:
                    item["returncode"] = returncode
                    item["elapsed_seconds"] = round(time.monotonic() - item["started"], 3)
                    remaining -= 1
                    if returncode != 0:
                        failed_worker = item
                        break
            if failed_worker:
                for item in processes:
                    if item["returncode"] is None:
                        item["cancelled"] = True
                terminate_processes(processes)
                break
            if remaining and time.monotonic() >= next_status:
                elapsed = round(max(time.monotonic() - item["started"] for item in processes))
                print(f"Campaign running: {elapsed}s elapsed, {remaining} worker(s) active", flush=True)
                next_status = time.monotonic() + 60
            if remaining:
                time.sleep(0.25)
    except KeyboardInterrupt:
        interrupted = True
        for item in processes:
            if item["returncode"] is None:
                item["cancelled"] = True
        print("Campaign interrupted; stopping workers", flush=True)
        terminate_processes(processes)
    except BaseException:
        terminate_processes(processes)
        raise
    finally:
        for item in processes:
            item["log"].close()

    for item in processes:
        if item["returncode"] is None:
            item["returncode"] = item["process"].poll()
            item["elapsed_seconds"] = round(time.monotonic() - item["started"], 3)

    results = []
    overall_result = 130 if interrupted else 0
    for name, _ in executables:
        target_workers = [item for item in processes if item["target"] == name]
        work_corpus = state_root / "corpus" / name
        corpus_after = snapshot_files(work_corpus)
        artifacts_after = snapshot_files(artifact_dirs[name])
        new_corpus = set(corpus_after) - set(before[name])
        new_artifacts = [
            f"{campaign_name}/{path}" for path in sorted(artifacts_after)
        ]
        returncodes = [item["returncode"] for item in target_workers]
        failed = any(
            item["returncode"] != 0 and not item["cancelled"]
            for item in target_workers
        )
        cancelled = not failed and any(item["cancelled"] for item in target_workers)
        overall_result |= int(failed)
        logs = [item["log_path"] for item in target_workers]
        result = {
            "target": name,
            "status": "failed" if failed else "cancelled" if cancelled else "passed",
            "returncodes": returncodes,
            "seeds": [item["seed"] for item in target_workers],
            "elapsed_seconds": max(item["elapsed_seconds"] for item in target_workers),
            "corpus_before": snapshot_summary(before[name]),
            "corpus_after": snapshot_summary(corpus_after),
            "new_corpus_files": len(new_corpus),
            "new_corpus_bytes": sum(corpus_after[path] for path in new_corpus),
            "new_artifacts": new_artifacts,
            "logs": [str(path) for path in logs],
            "stats": parse_fuzzer_logs(logs),
        }
        results.append(result)
        print(
            f"{name}: {result['status']}; "
            f"{result['stats'].get('number_of_executed_units', 0)} executions; "
            f"{len(new_corpus)} new corpus file(s); {len(new_artifacts)} artifact(s)",
            flush=True,
        )

    summary = {
        "started_utc": timestamp,
        "duration_seconds_per_worker": duration_seconds,
        "assertions": with_asserts,
        "workers_per_target": workers,
        "base_seed": base_seed,
        "timeout_seconds": timeout_seconds,
        "rss_limit_mb": rss_limit_mb,
        "max_input_bytes": max_length or None,
        "fuzzer_arguments": extra_arguments,
        "interrupted": interrupted,
        "targets": results,
    }
    state_root.mkdir(parents=True, exist_ok=True)
    summary_path = state_root / f"summary-{timestamp}-{'assert' if with_asserts else 'no-assert'}.json"
    summary_text = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    summary_path.write_text(summary_text, encoding="utf-8")
    (state_root / "latest-summary.json").write_text(summary_text, encoding="utf-8")
    print(f"Summary: {summary_path}", flush=True)

    for result in results:
        for artifact in result["new_artifacts"]:
            path = state_root / "artifacts" / result["target"] / artifact
            print(f"Reproduce: {BUILD_ROOT / ('assert' if with_asserts else 'no-assert') / ('fuzz_' + result['target'])} {path}")
    return overall_result


def merge_corpus(executable, name, generated_seed_root, state_root):
    work_corpus = state_root / "corpus" / name
    work_corpus.mkdir(parents=True, exist_ok=True)
    merge_dir = state_root / "merge" / name
    if merge_dir.exists():
        shutil.rmtree(merge_dir)
    merge_dir.mkdir(parents=True)
    inputs = corpus_directories(name, generated_seed_root, state_root)
    result = subprocess.run(
        [str(executable), "-merge=1", str(merge_dir), *(str(path) for path in inputs)],
        cwd=str(executable.parent),
    )
    if result.returncode != 0:
        return result.returncode
    shutil.rmtree(work_corpus)
    merge_dir.rename(work_corpus)
    return 0


def minimize_crash(executable, crash, state_root, name, timeout_seconds, rss_limit_mb):
    artifact_dir = state_root / "artifacts" / name
    artifact_dir.mkdir(parents=True, exist_ok=True)
    output = artifact_dir / f"minimized-{crash.name}"
    command = [
        str(executable),
        "-minimize_crash=1",
        f"-exact_artifact_path={output}",
        f"-timeout={timeout_seconds}",
        f"-rss_limit_mb={rss_limit_mb}",
        str(crash),
    ]
    result = subprocess.run(command, cwd=str(executable.parent))
    if result.returncode == 0:
        print(f"Minimized reproducer: {output}")
    return result.returncode


def main(argv=None):
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("harnesses", nargs="*", default=[], help="targets to build (default: all)")
    parser.add_argument("--run", type=parse_duration, metavar="DURATION",
                        help="fuzz all selected targets concurrently (for example 30m or 2h)")
    parser.add_argument("--workers", type=int, default=1, metavar="N",
                        help="independent workers per target (default: 1)")
    parser.add_argument("--smoke", action="store_true",
                        help="replay generated and optional local seeds")
    parser.add_argument("--replay", action="store_true",
                        help="replay generated and optional local seeds plus the persistent corpus")
    parser.add_argument("--merge", action="store_true",
                        help="minimize each selected target's persistent corpus")
    parser.add_argument("--minimize-crash", type=Path, metavar="FILE",
                        help="minimize one reproducer; requires exactly one target")
    parser.add_argument("--state-dir", type=Path, default=DEFAULT_STATE_ROOT,
                        help=f"persistent corpus/artifact/log directory (default: {DEFAULT_STATE_ROOT})")
    parser.add_argument("--reset-corpus", action="store_true",
                        help="delete the persistent corpus before running")
    parser.add_argument("--timeout", type=int, default=25, metavar="SECONDS",
                        help="per-input timeout (default: 25)")
    parser.add_argument("--rss-limit-mb", type=int, default=2048, metavar="MB",
                        help="memory limit per worker (default: 2048)")
    parser.add_argument("--max-len", type=int, default=0, metavar="BYTES",
                        help="maximum generated input length (default: inferred by libFuzzer)")
    parser.add_argument("--seed", type=int, metavar="N",
                        help="base random seed (default: generated and recorded)")
    parser.add_argument("--fuzzer-arg", action="append", default=[], metavar="ARG",
                        help="additional libFuzzer argument; may be repeated")
    parser.add_argument("--build-jobs", type=int, default=os.cpu_count() or 1, metavar="N")
    parser.add_argument("--no-assert", action="store_true", help="build with -DNDEBUG")
    parser.add_argument("--clean", action="store_true",
                        help="wipe build outputs but preserve campaign state")
    args = parser.parse_args(argv)

    selected = args.harnesses or list(HARNESSES)
    unknown = [name for name in selected if name not in HARNESSES]
    if unknown:
        parser.error(f"unknown harness(es): {', '.join(unknown)}; known: {', '.join(HARNESSES)}")
    if args.workers < 1 or args.build_jobs < 1:
        parser.error("worker counts must be positive")
    if args.timeout < 1 or args.rss_limit_mb < 1 or args.max_len < 0:
        parser.error("timeout and memory limits must be positive; max length cannot be negative")
    if args.minimize_crash and len(selected) != 1:
        parser.error("--minimize-crash requires exactly one harness")
    if args.minimize_crash and not args.minimize_crash.is_file():
        parser.error(f"reproducer does not exist: {args.minimize_crash}")
    try:
        validate_fuzzer_arguments(args.fuzzer_arg)
    except ValueError as error:
        parser.error(str(error))

    state_root = args.state_dir.expanduser().resolve()
    build_dir = BUILD_ROOT / ("no-assert" if args.no_assert else "assert")
    if args.clean and BUILD_ROOT.exists():
        shutil.rmtree(BUILD_ROOT)
    if args.reset_corpus and (state_root / "corpus").exists():
        shutil.rmtree(state_root / "corpus")

    compiler = find_tool("LIBMP_FUZZ_CLANGXX", ["clang++-18", "clang++"])
    gcc_install_dir = find_gcc_install_dir()
    with_asserts = not args.no_assert
    library = configure_and_build_lib(
        compiler, gcc_install_dir, build_dir, with_asserts, args.build_jobs)
    executables = [
        (name, build_harness(
            compiler, gcc_install_dir, build_dir, name, library, with_asserts))
        for name in selected
    ]
    seed_generator = build_seed_generator(
        compiler, gcc_install_dir, build_dir, library, with_asserts)
    generated_seed_root = build_dir / "seeds"
    generate_seed_corpus(seed_generator, generated_seed_root)
    print(
        f"Built {len(executables)} target(s) in {build_dir} "
        f"(assertions {'ON' if with_asserts else 'OFF'})",
        flush=True,
    )

    if args.smoke or args.replay:
        for name, executable in executables:
            smoke_harness(
                executable, name, generated_seed_root, state_root, args.replay)
        print("Corpus replay passed", flush=True)

    result = 0
    if args.merge:
        for name, executable in executables:
            result |= merge_corpus(executable, name, generated_seed_root, state_root)
    if args.minimize_crash:
        name, executable = executables[0]
        result |= minimize_crash(
            executable,
            args.minimize_crash.resolve(),
            state_root,
            name,
            args.timeout,
            args.rss_limit_mb,
        )
    if args.run:
        base_seed = args.seed if args.seed is not None else int(time.time_ns() & 0xFFFFFFFF)
        if base_seed == 0:
            base_seed = 1
        print(
            f"Running {len(executables)} target(s) concurrently for "
            f"{args.run}s with {args.workers} worker(s) per target; seed={base_seed}",
            flush=True,
        )
        result |= run_campaigns(
            executables,
            generated_seed_root,
            state_root,
            args.run,
            args.workers,
            args.timeout,
            args.rss_limit_mb,
            base_seed,
            args.max_len,
            args.fuzzer_arg,
            with_asserts,
        )
    return result


if __name__ == "__main__":
    sys.exit(main())

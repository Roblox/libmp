#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build every language wrapper, then require all readers in cross_language_parity."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from _utils import expand_file_patterns, resolve_lute


SCRIPT_DIR = Path(__file__).resolve().parent
INPUT_ENV = "LIBMP_PARITY_INPUTS"
REQUIRE_ALL_ENV = "LIBMP_PARITY_REQUIRE_ALL"
LUAU_ARTIFACT_ENV = "LIBMP_PARITY_LUAU_ARTIFACT"


def run_step(label, command, environment):
    print(f"\n--- {label} ---", flush=True)
    return subprocess.run(command, env=environment).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Build and run the cross-language parity test",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--config",
        default="Release",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
        help="native build configuration (default: Release)",
    )
    parser.add_argument(
        "--input",
        action="append",
        nargs="+",
        metavar="PATH_OR_GLOB",
        help=(
            "existing GPRX files, directories, or glob patterns to test; repeat as needed; "
            "by default the test reconstructs the committed source-data fixture"
        ),
    )
    parser.add_argument(
        "--luau-artifact",
        type=Path,
        metavar="PATH",
        help=(
            "replacement LibMP.luau file or containing directory; "
            "use it instead of building build/luau/LibMP.luau"
        ),
    )
    args = parser.parse_args(argv)

    requested_inputs = [value for group in (args.input or []) for value in group]
    try:
        captures = expand_file_patterns(requested_inputs, "*.gprx") if requested_inputs else []
    except ValueError as error:
        parser.error(str(error))

    luau_artifact = None
    if args.luau_artifact:
        luau_artifact = args.luau_artifact.expanduser()
        if luau_artifact.is_dir():
            luau_artifact /= "LibMP.luau"
        luau_artifact = luau_artifact.resolve()
        if not luau_artifact.is_file():
            parser.error(f"Luau artifact does not exist: {luau_artifact}")

    lute = resolve_lute()
    if lute is None:
        print(
            "lute was not found. Install it in PATH or set LIBMP_LUTE to its path.\n"
            "Download: https://github.com/luau-lang/lute/releases",
            file=sys.stderr,
        )
        return 2

    environment = os.environ.copy()
    environment["LIBMP_LUTE"] = str(lute.resolve())
    environment[REQUIRE_ALL_ENV] = "1"
    if captures:
        environment[INPUT_ENV] = json.dumps([str(path) for path in captures])
    else:
        environment.pop(INPUT_ENV, None)
    if luau_artifact:
        environment[LUAU_ARTIFACT_ENV] = str(luau_artifact)
    else:
        environment.pop(LUAU_ARTIFACT_ENV, None)

    build_arguments = [
        sys.executable, str(SCRIPT_DIR / "build.py"),
    ]
    if not luau_artifact:
        build_arguments.append("--luau")
    build_arguments.extend(["--js", "--csharp", "--python", "--go"])

    result = run_step(
        "Build language parity artifacts",
        build_arguments,
        environment,
    )
    if result != 0:
        return result

    return run_step(
        "Build and run cross-language parity",
        [
            sys.executable,
            str(SCRIPT_DIR / "test_native.py"),
            "cross_language_parity",
            "--config",
            args.config,
        ],
        environment,
    )


if __name__ == "__main__":
    sys.exit(main())

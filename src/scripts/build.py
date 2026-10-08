#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Main build script. Runs the full pipeline for selected language targets.

Usage:
  python build.py --wasm
  python build.py --luau
  python build.py --js
  python build.py --csharp
  python build.py --python
  python build.py --go
  python build.py --native
  python build.py --luau --native
"""
import argparse
import subprocess
import sys
from pathlib import Path

from _autonomy import add_autonomy_arguments, enabled_autonomy_arguments
from _utils import (
    UserConfigError,
    add_conditional_define_arguments,
    enabled_conditional_define_options,
    enabled_flag_arguments,
    load_user_config,
)

SCRIPT_DIR = Path(__file__).resolve().parent

PIPELINES = {
    "wasm": [
        ([sys.executable, str(SCRIPT_DIR / "build_wasm.py")], "Build WASM", True, True),
    ],
    "luau": [
        ([sys.executable, str(SCRIPT_DIR / "build_wasm.py")], "Build WASM", True, True),
        ([sys.executable, str(SCRIPT_DIR / "build_luau.py")], "Build Luau from WASM", False, True),
        ([sys.executable, str(SCRIPT_DIR / "gen_flatbuffers.py"), "--luau"], "Generate FlatBuffers (Luau)", False, False),
        ([sys.executable, str(SCRIPT_DIR / "bundle_luau.py")], "Bundle Luau modules", False, False),
    ],
    "js": [
        ([sys.executable, str(SCRIPT_DIR / "build_wasm.py")], "Build WASM", True, True),
        ([sys.executable, str(SCRIPT_DIR / "gen_flatbuffers.py"), "--js"], "Generate FlatBuffers (TypeScript)", False, False),
        ([sys.executable, str(SCRIPT_DIR / "build_js.py")], "Bundle self-contained JavaScript", False, True),
    ],
    "csharp": [
        ([sys.executable, str(SCRIPT_DIR / "build_wasm.py")], "Build WASM", True, True),
        ([sys.executable, str(SCRIPT_DIR / "gen_flatbuffers.py"), "--csharp"], "Generate FlatBuffers (C#)", False, False),
        ([sys.executable, str(SCRIPT_DIR / "build_csharp.py")], "Bundle self-contained C#", False, True),
    ],
    "python": [
        ([sys.executable, str(SCRIPT_DIR / "build_wasm.py")], "Build WASM", True, True),
        ([sys.executable, str(SCRIPT_DIR / "gen_flatbuffers.py"), "--python"], "Generate FlatBuffers (Python)", False, False),
        ([sys.executable, str(SCRIPT_DIR / "build_python.py")], "Bundle self-contained Python", False, True),
    ],
    "go": [
        ([sys.executable, str(SCRIPT_DIR / "build_wasm.py")], "Build WASM", True, True),
        ([sys.executable, str(SCRIPT_DIR / "gen_flatbuffers.py"), "--go"], "Generate FlatBuffers (Go)", False, False),
        ([sys.executable, str(SCRIPT_DIR / "build_go.py")], "Bundle self-contained Go", False, True),
    ],
    "native": [
        ([sys.executable, str(SCRIPT_DIR / "gen_flatbuffers.py"), "--cpp"], "Generate FlatBuffers (C++)", False, False),
        ([sys.executable, str(SCRIPT_DIR / "build_native.py")], "Build native C++ (CMake)", True, True),
    ],
}


def main():
    parser = argparse.ArgumentParser(
        description="Run build pipeline for selected language targets",
        allow_abbrev=False,
    )
    for target in PIPELINES:
        parser.add_argument(f"--{target}", action="store_true", help=f"Build {target} target")
    parser.add_argument("--tests", action="store_true", help="Build and run native tests (requires --native)")
    parser.add_argument(
        "--amalgamation-matrix-tests",
        action="store_true",
        help="Add the isolated amalgamation compiler matrix (requires --native; implies --tests)",
    )
    parser.add_argument("--amalgamated", action="store_true",
                        help="Compile LibMP from generated libmp.cpp/libmp.h files")
    parser.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                        help="Native build configuration (default: Release; requires --native)")
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Remove the selected targets' build output first; cached tools are preserved",
    )
    add_autonomy_arguments(parser)
    try:
        user_config = load_user_config()
    except UserConfigError as error:
        parser.error(str(error))
    add_conditional_define_arguments(parser, user_config)
    args = parser.parse_args()
    enabled_options = enabled_conditional_define_options(args, user_config)
    forwarded_build_arguments = [f"--{option}" for option in enabled_options]
    forwarded_build_arguments.extend(
        enabled_flag_arguments(args, "--amalgamated")
    )

    targets = [t for t in PIPELINES if getattr(args, t)]

    native_only_requested = (
        args.tests
        or args.amalgamation_matrix_tests
        or args.config is not None
        or bool(enabled_autonomy_arguments(args))
    )
    if native_only_requested and "native" not in targets:
        parser.error(
            "--tests, --amalgamation-matrix-tests, --config, "
            "and strict-check options require --native"
        )
    if not targets:
        parser.print_help()
        sys.exit(0)

    native_arguments = enabled_flag_arguments(
        args,
        "--tests",
        "--amalgamation-matrix-tests",
    )
    if args.config is not None:
        native_arguments.extend(("--config", args.config))
    native_arguments.extend(enabled_autonomy_arguments(args))

    completed_commands = set()
    for target in targets:
        print(f"{'='*60}", flush=True)
        print(f" Building: {target}", flush=True)
        print(f"{'='*60}", flush=True)

        for cmd, description, accepts_build_arguments, accepts_clean in PIPELINES[target]:
            print(f"\n--- {description} ---", flush=True)
            forwarded_arguments = forwarded_build_arguments if accepts_build_arguments else []
            if target == "native" and accepts_build_arguments:
                forwarded_arguments = [*forwarded_arguments, *native_arguments]
            if args.clean and accepts_clean:
                forwarded_arguments = [*forwarded_arguments, "--clean"]
            command = [*cmd, *forwarded_arguments]
            command_key = tuple(str(item) for item in command)
            if command_key in completed_commands:
                print(f"\n--- Reusing completed step: {description} ---", flush=True)
                continue
            result = subprocess.run(command)
            if result.returncode != 0:
                print(f"\nFailed: {description}", file=sys.stderr)
                sys.exit(result.returncode)
            completed_commands.add(command_key)

    print(f"\n{'='*60}", flush=True)
    print(f" All targets built successfully: {', '.join(targets)}", flush=True)
    print(f"{'='*60}\n", flush=True)


if __name__ == "__main__":
    main()

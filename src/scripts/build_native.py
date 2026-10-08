#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Builds the native C++ library and examples using CMake.
"""
import argparse
import json
import platform
import re
import subprocess
import sys
from pathlib import Path

from _autonomy import (
    AUTONOMY_GATES,
    NO_CXX_STDLIB_DEST,
    NO_MALLOC_GATE,
    add_autonomy_arguments,
)
from _utils import (
    UserConfigError,
    add_conditional_define_arguments,
    compiler_define_enabled,
    enabled_conditional_define_options,
    find_built_executable,
    find_built_file,
    load_user_config,
    resolve_compiler_defines,
)


def resolve_autonomy_gates(args, compiler_defines):
    """Selects requested gates and validates that the build can support them."""
    if args.strict or args.allocator_autonomous_strict:
        requested = list(AUTONOMY_GATES)
    else:
        requested = [gate for gate in AUTONOMY_GATES if getattr(args, gate.argparse_dest)]

    owns_alloc = (
        compiler_define_enabled(compiler_defines, "NTD_EMBEDDED_ALLOC")
        or compiler_define_enabled(compiler_defines, "NTD_EXTERNAL_MALLOC")
    )
    routes_dependency_allocations = compiler_define_enabled(
        compiler_defines, "CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD", default=True
    )
    system = platform.system()
    unsupported = [gate.option for gate in requested if not gate.supports(system)]
    if unsupported:
        raise UserConfigError(
            f"{', '.join(unsupported)} is not supported on {system}; "
            "the symbol-analysis gates require a Linux, Apple, or Windows toolchain"
        )

    # Fail before the slow build if a requested gate cannot be meaningful.
    if NO_MALLOC_GATE in requested and not owns_alloc:
        raise UserConfigError("--strict-no-malloc / --strict needs an allocator backend "
                              "(--embedded-alloc or --external-malloc)")
    if NO_MALLOC_GATE in requested and not routes_dependency_allocations:
        raise UserConfigError("the no-malloc autonomy gate requires dependency allocation routing "
                              "(remove --no-dependency-alloc-routing)")

    return requested


def find_built_library(build_dir, config):
    for filename in ("libmp.a", "mp.lib"):
        library = find_built_file(build_dir, config, filename)
        if library is not None:
            return library
    return None


def run_autonomy_gates(script_path, build_dir, config, gates):
    if not gates:
        return 0
    library = find_built_library(build_dir, config)
    if library is None:
        print("LibMP static library was not produced.", file=sys.stderr)
        return 1
    labels = "; ".join(gate.label for gate in gates)
    print(f"Checking allocator autonomy ({labels})...", flush=True)
    return subprocess.run([
        sys.executable,
        str(script_path / "check_symbols.py"),
        *(gate.option for gate in gates),
        str(library),
    ]).returncode


def run_no_cxx_stdlib_smoke(build_dir, config):
    executable = find_built_executable(build_dir, config, "libmp_no_cxx_stdlib_smoke")
    if executable is None:
        print("No-C++-stdlib smoke executable was not produced.", file=sys.stderr)
        return 1
    print("Checking C++ standard-library independence (complete archive)...", flush=True)
    return subprocess.run([str(executable)]).returncode


def available_test_names(build_dir, config):
    # json-v1 is CTest's versioned machine-readable test-list format. Using it
    # avoids parsing localized, human-oriented `ctest --show-only` output.
    result = subprocess.run([
        "ctest",
        "--test-dir", str(build_dir),
        "--build-config", config,
        "--show-only=json-v1",
    ], capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise UserConfigError(f"could not list configured tests: {detail}")
    try:
        test_data = json.loads(result.stdout)
        return {test["name"] for test in test_data["tests"]}
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise UserConfigError("ctest returned an invalid JSON test listing") from error


def main(argv=None, selected_tests=()):
    parser = argparse.ArgumentParser(
        description="Build native C++ via CMake",
        allow_abbrev=False,
    )
    parser.add_argument("--config", default="Release", choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                        help="Build configuration (default: Release)")
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Remove the native CMake build directory before configuring",
    )
    parser.add_argument("--tests", action="store_true", help="Build and run the native test suite")
    parser.add_argument(
        "--amalgamation-matrix-tests",
        action="store_true",
        help="Add the isolated std/dual/ntd amalgamation compiler matrix; implies --tests",
    )
    parser.add_argument("--amalgamated", action="store_true",
                        help="Compile LibMP from generated libmp.cpp/libmp.h files")
    add_autonomy_arguments(parser)
    try:
        user_config = load_user_config()
    except UserConfigError as error:
        parser.error(str(error))
    add_conditional_define_arguments(parser, user_config)
    args = parser.parse_args(argv)
    run_amalgamation_matrix_tests = (
        args.amalgamation_matrix_tests
        or "amalgamation_matrix_tests" in selected_tests
    )
    run_tests = args.tests or bool(selected_tests) or run_amalgamation_matrix_tests
    enabled_options = enabled_conditional_define_options(args, user_config)
    if args.allocator_autonomous_strict:
        if "allocator-autonomous" not in enabled_options:
            enabled_options = (*enabled_options, "allocator-autonomous")
    try:
        compiler_defines = resolve_compiler_defines(user_config, enabled_options)

        external_flatbuffers = compiler_define_enabled(
            compiler_defines, "CFG_USE_EXTERNAL_FLATBUFFERS"
        )
        external_lz4 = compiler_define_enabled(compiler_defines, "CFG_USE_EXTERNAL_LZ4")
        no_cxx_stdlib_requested = getattr(args, NO_CXX_STDLIB_DEST)
        gates = resolve_autonomy_gates(args, compiler_defines)
        no_libc_alloc_link_requested = (
            platform.system() == "Linux"
            and NO_MALLOC_GATE in gates
            and compiler_define_enabled(compiler_defines, "NTD_EMBEDDED_ALLOC")
            and not compiler_define_enabled(compiler_defines, "NTD_EXTERNAL_PAGES")
            and not compiler_define_enabled(compiler_defines, "NTD_EXTERNAL_MALLOC")
        )
    except UserConfigError as error:
        parser.error(str(error))

    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent
    build_dir = project_root / "build" / "native"

    if args.clean and build_dir.exists():
        import shutil
        print("Cleaning build directory...", flush=True)
        shutil.rmtree(build_dir)

    build_dir.mkdir(parents=True, exist_ok=True)

    print(f"Configuring ({args.config})...", flush=True)
    result = subprocess.run([
        "cmake",
        "-S", str(project_root),
        "-B", str(build_dir),
        f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DLIBMP_BUILD_COMPILE_DEFINITIONS={';'.join(compiler_defines)}",
        f"-DLIBMP_BUILD_USE_EXTERNAL_FLATBUFFERS={'ON' if external_flatbuffers else 'OFF'}",
        f"-DLIBMP_BUILD_USE_EXTERNAL_LZ4={'ON' if external_lz4 else 'OFF'}",
        f"-DLIBMP_BUILD_AMALGAMATED={'ON' if args.amalgamated else 'OFF'}",
        f"-DLIBMP_BUILD_TESTS={'ON' if run_tests else 'OFF'}",
        "-DLIBMP_BUILD_AMALGAMATION_MATRIX_TESTS="
        f"{'ON' if run_amalgamation_matrix_tests else 'OFF'}",
        "-DLIBMP_BUILD_NO_CXX_STDLIB_TEST="
        f"{'ON' if no_cxx_stdlib_requested else 'OFF'}",
        "-DLIBMP_BUILD_NO_LIBC_ALLOC_LINK_TEST="
        f"{'ON' if no_libc_alloc_link_requested else 'OFF'}",
        "-DLIBMP_BUILD_ENABLE_RTTI=OFF",
    ])
    if result.returncode != 0:
        print("CMake configure failed.", file=sys.stderr)
        sys.exit(result.returncode)

    print("Building...", flush=True)
    result = subprocess.run([
        "cmake",
        "--build", str(build_dir),
        "--config", args.config,
    ])
    if result.returncode != 0:
        print("Build failed.", file=sys.stderr)
        sys.exit(result.returncode)

    if run_tests:
        print("Running tests...", flush=True)
        test_command = [
            "ctest",
            "--test-dir", str(build_dir),
            "--build-config", args.config,
            "--output-on-failure",
        ]
        if selected_tests:
            try:
                available_tests = available_test_names(build_dir, args.config)
            except UserConfigError as error:
                print(f"Test discovery failed: {error}", file=sys.stderr)
                sys.exit(2)
            missing_tests = sorted(set(selected_tests) - available_tests)
            if missing_tests:
                print(f"Unknown native test(s): {', '.join(missing_tests)}", file=sys.stderr)
                print(f"Available tests: {', '.join(sorted(available_tests))}", file=sys.stderr)
                sys.exit(2)
            exact_names = "|".join(re.escape(name) for name in selected_tests)
            test_command.extend(["-R", f"^({exact_names})$"])
        result = subprocess.run(test_command)
        if result.returncode != 0:
            print("Tests failed.", file=sys.stderr)
            sys.exit(result.returncode)

    if no_cxx_stdlib_requested:
        rc = run_no_cxx_stdlib_smoke(build_dir, args.config)
        if rc != 0:
            print("No-C++-stdlib link/run check failed.", file=sys.stderr)
            sys.exit(rc)

    rc = run_autonomy_gates(script_path, build_dir, args.config, gates)
    if rc != 0:
        print("Allocator-autonomy check failed.", file=sys.stderr)
        sys.exit(rc)

    print("Success!")


if __name__ == "__main__":
    main()

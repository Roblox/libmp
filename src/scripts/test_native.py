#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build and run all native tests, or selected tests by exact CTest name."""

import argparse
import sys

import build_native
from _autonomy import add_autonomy_arguments, enabled_autonomy_arguments

from _utils import (
    UserConfigError,
    add_conditional_define_arguments,
    enabled_conditional_define_options,
    enabled_flag_arguments,
    load_user_config,
)


def main():
    parser = argparse.ArgumentParser(
        description="Build and run LibMP native tests",
        epilog="Examples: test_native.py | test_native.py ntd_unit_tests",
        allow_abbrev=False,
    )
    parser.add_argument("tests", nargs="*", metavar="NAME",
                        help="exact CTest names to run (default: all)")
    parser.add_argument("--config", default="Release",
                        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                        help="build configuration (default: Release)")
    parser.add_argument(
        "--clean",
        action="store_true",
        help="remove the native CMake build directory before configuring",
    )
    parser.add_argument("--amalgamated", action="store_true",
                        help="compile LibMP from generated libmp.cpp/libmp.h files")
    parser.add_argument(
        "--amalgamation-matrix-tests",
        action="store_true",
        help="add the isolated std/dual/ntd amalgamation compiler matrix",
    )
    add_autonomy_arguments(parser)
    try:
        user_config = load_user_config()
    except UserConfigError as error:
        parser.error(str(error))
    add_conditional_define_arguments(parser, user_config)
    args = parser.parse_args()

    build_arguments = ["--config", args.config]
    build_arguments.extend(enabled_flag_arguments(
        args,
        "--clean",
        "--amalgamated",
        "--amalgamation-matrix-tests",
    ))
    build_arguments.extend(enabled_autonomy_arguments(args))
    for option in enabled_conditional_define_options(args, user_config):
        build_arguments.append(f"--{option}")
    if not args.tests:
        build_arguments.append("--tests")

    return build_native.main(build_arguments, selected_tests=args.tests) or 0


if __name__ == "__main__":
    sys.exit(main())

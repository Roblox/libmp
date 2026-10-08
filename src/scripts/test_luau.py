#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build and smoke-test the single-file Luau wrapper."""

import argparse
import json
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from _utils import ensure_parity_tool, resolve_lute, stage_luau_script


def run(command):
    print("  >", " ".join(str(item) for item in command), flush=True)
    result = subprocess.run([str(item) for item in command])
    if result.returncode:
        sys.exit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="Use an existing GPRX capture")
    parser.add_argument("--skip-build", action="store_true",
                        help="Test the existing Luau artifact")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    lute = resolve_lute()
    if lute is None:
        parser.error("lute was not found; install it or set LIBMP_LUTE")

    if not args.skip_build:
        run([sys.executable, script_dir / "build.py", "--luau"])

    artifact = project_root / "build" / "luau" / "LibMP.luau"
    if not artifact.is_file():
        parser.error(f"Luau artifact does not exist: {artifact}")

    with TemporaryDirectory(prefix="libmp-luau-") as directory:
        temporary = Path(directory)
        capture = args.input.resolve() if args.input else temporary / "fixture.gprx"
        if not args.input:
            run([ensure_parity_tool(), "build", capture])

        output = temporary / "converted.gprx"
        staged = stage_luau_script(
            artifact,
            project_root / "tests" / "luau" / "smoke.luau",
            {
                '"@SMOKE_CAPTURE@"': json.dumps(capture.as_posix()),
                '"@SMOKE_OUTPUT@"': json.dumps(output.as_posix()),
            },
            prefix="_smoke_",
        )
        try:
            run([lute, "run", staged])
        finally:
            staged.unlink(missing_ok=True)


if __name__ == "__main__":
    main()

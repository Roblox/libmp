#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build and smoke-test the self-contained JavaScript wrapper."""

import argparse
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from _utils import ensure_parity_tool


def run(command):
    print("  >", " ".join(str(item) for item in command), flush=True)
    result = subprocess.run([str(item) for item in command])
    if result.returncode != 0:
        sys.exit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="Use an existing GPRX capture")
    parser.add_argument("--skip-build", action="store_true", help="Test the existing JS artifact")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    if not args.skip_build:
        run([sys.executable, script_dir / "build.py", "--js"])

    artifact = project_root / "build" / "js" / "LibMP.js"
    test = project_root / "tests" / "js" / "smoke.js"
    if args.input:
        run(["node", test, artifact.resolve(), args.input.resolve()])
        return

    parity_tool = ensure_parity_tool()
    with TemporaryDirectory(prefix="libmp-js-") as directory:
        capture = Path(directory) / "fixture.gprx"
        short_update = Path(directory) / "fixture-short-update.gprx"
        long_update = Path(directory) / "fixture-long-update.gprx"
        run([parity_tool, "build-related", capture, short_update, long_update])
        run(["node", test, artifact.resolve(), capture, short_update, long_update])


if __name__ == "__main__":
    main()

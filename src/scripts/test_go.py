#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build and smoke-test the single-file Go wrapper."""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from _utils import ensure_parity_tool, load_build_config


def run(command, cwd=None):
    print("  >", " ".join(str(item) for item in command), flush=True)
    result = subprocess.run([str(item) for item in command], cwd=cwd)
    if result.returncode:
        sys.exit(result.returncode)


def main():
    parser = argparse.ArgumentParser(description="Smoke-test the Go wrapper")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--input", type=Path, help="Use an existing GPRX capture")
    args = parser.parse_args()
    project_root = Path(__file__).resolve().parent.parent
    if not args.skip_build:
        run([sys.executable, project_root / "scripts" / "build.py", "--go"])

    config = load_build_config()
    work = project_root / "build" / "go_smoke"
    if work.exists():
        shutil.rmtree(work)
    (work / "libmp").mkdir(parents=True)
    shutil.copy2(project_root / "build" / "go" / "LibMP.go", work / "libmp" / "LibMP.go")
    shutil.copy2(project_root / "tests" / "go" / "smoke.go", work / "smoke.go")
    (work / "go.mod").write_text(
        "module libmp-smoke\n\ngo 1.18\n\nrequire "
        "github.com/bytecodealliance/wasmtime-go/v39 "
        + config["wasmtime"]["go_version"] + "\n",
        encoding="utf-8",
    )
    run(["go", "mod", "tidy"], cwd=work)
    if args.input:
        run(["go", "run", ".", str(args.input.resolve())], cwd=work)
        return

    parity_tool = ensure_parity_tool()
    with TemporaryDirectory(prefix="libmp-go-") as directory:
        capture = Path(directory) / "fixture.gprx"
        short_update = Path(directory) / "fixture-short-update.gprx"
        long_update = Path(directory) / "fixture-long-update.gprx"
        run([parity_tool, "build-related", capture, short_update, long_update])
        run(["go", "run", ".", str(capture), str(short_update), str(long_update)], cwd=work)


if __name__ == "__main__":
    main()

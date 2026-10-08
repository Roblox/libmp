#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build and smoke-test the single-file Python wrapper."""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from _utils import ensure_parity_tool, load_build_config


def run(command, env=None):
    print("  >", " ".join(str(item) for item in command), flush=True)
    result = subprocess.run([str(item) for item in command], env=env)
    if result.returncode:
        sys.exit(result.returncode)


def test_missing_runtime_message(artifact: Path, expected_requirement: str):
    result = subprocess.run(
        [sys.executable, "-S", str(artifact)],
        capture_output=True,
        text=True,
    )
    expected = f"python -m pip install 'wasmtime{expected_requirement}'"
    if result.returncode == 0 or expected not in result.stderr:
        print("Python wrapper did not report the expected missing-runtime command.",
              file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        sys.exit(1)


def ensure_wasmtime(project_root: Path, version: str) -> Path:
    target = project_root / "build" / "utils" / "python_test_deps"
    marker = target / "wasmtime-version.txt"
    if not marker.exists() or marker.read_text().strip() != version:
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        run([
            sys.executable, "-m", "pip", "install", "--disable-pip-version-check",
            "--no-deps", "--target", target, f"wasmtime=={version}",
        ])
        marker.write_text(version + "\n", encoding="utf-8")
    return target


def main():
    parser = argparse.ArgumentParser(description="Smoke-test the Python wrapper")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--input", type=Path, help="Use an existing GPRX capture")
    args = parser.parse_args()
    project_root = Path(__file__).resolve().parent.parent
    if not args.skip_build:
        run([sys.executable, project_root / "scripts" / "build.py", "--python"])

    config = load_build_config()
    dependencies = ensure_wasmtime(project_root, config["wasmtime"]["python_version"])
    env = dict(__import__("os").environ)
    env["PYTHONPATH"] = str(dependencies)
    artifact = project_root / "build" / "python" / "LibMP.py"
    test = project_root / "tests" / "python" / "smoke.py"
    test_missing_runtime_message(artifact, config["wasmtime"]["python_requirement"])
    if args.input:
        run([sys.executable, test, artifact, args.input.resolve()], env=env)
        return

    parity_tool = ensure_parity_tool()
    with TemporaryDirectory(prefix="libmp-python-") as directory:
        capture = Path(directory) / "fixture.gprx"
        short_update = Path(directory) / "fixture-short-update.gprx"
        long_update = Path(directory) / "fixture-long-update.gprx"
        run([parity_tool, "build-related", capture, short_update, long_update])
        run([sys.executable, test, artifact, capture, short_update, long_update], env=env)


if __name__ == "__main__":
    main()

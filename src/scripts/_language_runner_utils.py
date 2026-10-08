#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Shared preparation helpers for cross-language test and benchmark runners."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

from _utils import load_build_config


def run_build(command, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        details = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed: {' '.join(map(str, command))}\n{details}")


def python_wasmtime_environment(source_root):
    environment = os.environ.copy()
    dependency_dir = source_root / "build" / "utils" / "python_test_deps"
    if dependency_dir.is_dir():
        old_path = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = (
            str(dependency_dir) + (os.pathsep + old_path if old_path else "")
        )
    probe = subprocess.run(
        [sys.executable, "-O", "-c", "import wasmtime"],
        env=environment, capture_output=True, text=True,
    )
    if probe.returncode:
        config = load_build_config()
        version = config["wasmtime"]["python_version"]
        return None, f"Python wasmtime {version} is absent (run test_python.py once)"
    return environment, None


def build_csharp_runner(project, assembly_name, wrapper_artifact=None):
    dotnet = shutil.which("dotnet")
    if not dotnet:
        return None, None, "dotnet is absent"
    command = [dotnet, "build", str(project), "-c", "Release", "--nologo", "-v:q"]
    if wrapper_artifact is not None:
        command.append(f"-p:LibMPArtifact={Path(wrapper_artifact).resolve()}")
    run_build(command)
    assembly = project.parent / "bin" / "Release" / "net8.0" / f"{assembly_name}.dll"
    if not assembly.is_file():
        raise RuntimeError(f"C# runner build did not produce {assembly}")
    environment = os.environ.copy()
    environment["DOTNET_TieredCompilation"] = "0"
    environment["DOTNET_ReadyToRun"] = "0"
    return [dotnet, str(assembly)], environment, None


def build_go_runner(
        source_root, wrapper_artifact, runner_source, work_name, module_name, binary_name):
    go = shutil.which("go")
    if not go:
        return None, "go is absent"
    config = load_build_config()
    work = source_root / "build" / work_name / "go"
    if work.exists():
        shutil.rmtree(work)
    (work / "libmp").mkdir(parents=True)
    shutil.copy2(wrapper_artifact, work / "libmp" / "LibMP.go")
    shutil.copy2(runner_source, work / "main.go")
    (work / "go.mod").write_text(
        f"module {module_name}\n\ngo 1.18\n\nrequire "
        "github.com/bytecodealliance/wasmtime-go/v39 "
        f"{config['wasmtime']['go_version']}\n",
        encoding="utf-8",
    )
    output = work / (f"{binary_name}.exe" if os.name == "nt" else binary_name)
    run_build([go, "mod", "tidy"], cwd=work)
    run_build([go, "build", "-trimpath", "-ldflags=-s -w", "-o", output, "."], cwd=work)
    return output, None

#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Provision the MSVC toolchain environment for direct compiler invocations."""

import os
import platform
import subprocess
from pathlib import Path


def _normalize_architecture(architecture):
    # CMake generator-platform strings may append fields such as
    # "x64,version=10.0"; vcvarsall only needs the platform name.
    architecture = architecture.split(",", 1)[0].strip().lower()
    aliases = {
        "amd64": "x64",
        "x86_64": "x64",
        "x64": "x64",
        "win32": "x86",
        "x86": "x86",
        "i386": "x86",
        "i686": "x86",
        "arm": "arm",
        "arm64": "arm64",
        "aarch64": "arm64",
    }
    normalized = aliases.get(architecture)
    if normalized is None:
        raise ValueError(f"unsupported MSVC target architecture: {architecture}")
    return normalized


def _compiler_architectures(compiler):
    if not compiler:
        return None, None
    compiler_path = Path(compiler)
    target = compiler_path.parent.name.lower()
    host_directory = compiler_path.parent.parent.name.lower()
    if target not in ("x64", "x86", "arm", "arm64"):
        target = None
    host = host_directory[4:] if host_directory.startswith("host") else None
    if host not in ("x64", "x86", "arm", "arm64"):
        host = None
    return host, target


def target_architecture(compiler=None, requested=None):
    """Return the selected compiler target, independent of the host CPU."""
    if requested:
        return _normalize_architecture(requested)

    # A cl.exe selected from .../bin/Host<host>/<target>/cl.exe is stronger
    # evidence than the host CPU when CMake is cross-compiling.
    _, compiler_target = _compiler_architectures(compiler)
    if compiler_target:
        return compiler_target

    return _normalize_architecture(platform.machine())


def vcvars_architecture(compiler=None, requested=None):
    """Return vcvarsall's native or host-to-target architecture argument."""
    compiler_host, _ = _compiler_architectures(compiler)
    host = compiler_host or _normalize_architecture(platform.machine())
    target = target_architecture(compiler, requested)
    return target if host == target else f"{host}_{target}"


def find_vcvarsall(compiler, architecture=None):
    """Locate the vcvarsall.bat that owns the selected MSVC toolchain.

    cl.exe lives at <vs_root>\\VC\\Tools\\MSVC\\<ver>\\bin\\Host<arch>\\<arch>\\cl.exe,
    so walking up finds the matching batch file and keeps the captured headers and
    libraries consistent with the exact compiler. Compilers outside a VS tree (for
    example clang-cl) fall back to vswhere's newest C++ installation.
    """
    for parent in Path(compiler).resolve().parents:
        candidate = parent / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
        if candidate.is_file():
            return candidate
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = (
        Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    )
    if vswhere.is_file():
        component = {
            "arm": "Microsoft.VisualStudio.Component.VC.Tools.ARM",
            "arm64": "Microsoft.VisualStudio.Component.VC.Tools.ARM64",
        }.get(architecture, "Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
        fallback_component = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        for required_component in (component, fallback_component):
            located = subprocess.run(
                [
                    str(vswhere), "-latest", "-products", "*",
                    "-requires", required_component,
                    "-property", "installationPath",
                ],
                capture_output=True, text=True,
            ).stdout.strip()
            if located:
                candidate = Path(located) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
                if candidate.is_file():
                    return candidate
            if required_component == fallback_component:
                break
    return None


def msvc_toolchain_environment(compiler, requested_architecture=None):
    """Return an environment with the MSVC toolchain provisioned, or None.

    Direct cl.exe invocations need INCLUDE/LIB/PATH from vcvarsall; MSBuild sets
    these up internally but a bare shell does not. A Developer Command Prompt or
    another shell that already ran vcvars leaves them in place, so respect that
    and return None to reuse the ambient environment. Otherwise capture what
    vcvarsall establishes so the caller compiles from any shell.
    """
    target = target_architecture(compiler, requested_architecture)
    architecture = vcvars_architecture(compiler, requested_architecture)
    ambient_architecture = os.environ.get("VSCMD_ARG_TGT_ARCH")
    host = _normalize_architecture(platform.machine())
    needs_specific_target = bool(requested_architecture) or target != host
    ambient_matches = False
    if ambient_architecture:
        try:
            ambient_matches = _normalize_architecture(ambient_architecture) == target
        except ValueError:
            pass
    if os.environ.get("INCLUDE") and (
        (not ambient_architecture and not needs_specific_target)
        or ambient_matches
    ):
        return None
    vcvarsall = find_vcvarsall(compiler, target)
    if vcvarsall is None:
        print(
            "warning: could not locate vcvarsall.bat; compiling with the ambient "
            "environment (run from a Developer Command Prompt if headers are not "
            "found)",
            flush=True,
        )
        return None
    dump = subprocess.run(
        f'"{vcvarsall}" {architecture} >nul 2>&1 && set',
        shell=True, capture_output=True, text=True,
    ).stdout
    environment = os.environ.copy()
    for line in dump.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            environment[key] = value
    if "INCLUDE" not in environment:
        print(
            f"warning: {vcvarsall.name} {architecture} did not yield a usable "
            "environment; compiling with the ambient environment",
            flush=True,
        )
        return None
    return environment

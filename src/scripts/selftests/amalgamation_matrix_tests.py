#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Compile the amalgamated type/allocator flavor matrix without source-tree access."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from amalgamate import generate  # noqa: E402
from _compiler_flags import library_warning_flags  # noqa: E402
from msvc_env import msvc_toolchain_environment  # noqa: E402


# Names describe the internal ntd implementation followed by the public API type
# family. "routed-ntd" uses standard containers instantiated with ntd::allocator.
FLAVOR_CONFIGURATIONS = {
    "default-std": (),
    "module-default-std": (
        "LIBMP_EXCLUDE_CAPTURE_BUILDER=1",
        "LIBMP_TRUSTED_INPUTS_ONLY=1",
    ),
    "routed-ntd-public-std": ("NTD_EMBEDDED_ALLOC=1",),
    "routed-ntd-public-ntd": ("NTD_EMBEDDED_ALLOC=1", "LIBMP_PUBLIC_NTD_API=1"),
    "allocator-autonomous": (
        "NTD_EMBEDDED_ALLOC=1",
        "LIBMP_PUBLIC_NTD_API=1",
        "NTD_OWN_FILEIO=1",
    ),
}


def compile_translation_unit(
    compiler,
    compiler_args,
    source,
    output,
    include_dir,
    msvc_style,
    compiler_id,
    disable_cpp_runtime,
    environment=None,
):
    if msvc_style:
        runtime_args = (
            ["/EHs-c-", "/D_HAS_EXCEPTIONS=0", "/GR-"]
            if disable_cpp_runtime
            else ["/EHsc", "/GR"]
        )
        command = [
            compiler,
            *compiler_args,
            "/nologo",
            "/std:c++17",
            *runtime_args,
            *library_warning_flags("MSVC"),
            "/c",
            str(source),
            f"/I{include_dir}",
            f"/Fo{output}",
        ]
    else:
        runtime_args = (
            ["-fno-exceptions", "-fno-rtti"] if disable_cpp_runtime else []
        )
        command = [
            compiler,
            *compiler_args,
            "-std=c++17",
            *runtime_args,
            *library_warning_flags(compiler_id),
            f"-I{include_dir}",
            "-c",
            str(source),
            "-o",
            str(output),
        ]
    result = subprocess.run(command, env=environment)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def main():
    parser = argparse.ArgumentParser(
        description="Compile isolated LibMP stdlib/allocator flavor combinations"
    )
    parser.add_argument("--cxx", required=True, help="C++ compiler executable")
    parser.add_argument(
        "--cxx-flag",
        action="append",
        default=[],
        help="additional compiler argument inherited from the parent build",
    )
    parser.add_argument(
        "--compiler-id",
        default="",
        help="compiler family reported by CMake (GNU, Clang, AppleClang, or MSVC)",
    )
    parser.add_argument(
        "--msvc-style",
        action="store_true",
        help="use cl/clang-cl command-line syntax",
    )
    parser.add_argument(
        "--msvc-architecture",
        help="vcvarsall target architecture (x86, x64, arm, or arm64)",
    )
    args = parser.parse_args()
    compiler_id = args.compiler_id or ("MSVC" if args.msvc_style else "unknown")

    # A direct cl.exe call needs the toolchain environment MSBuild would otherwise
    # provide; provision it once so the matrix compiles from any shell, not only a
    # Developer Command Prompt.
    environment = (
        msvc_toolchain_environment(args.cxx, args.msvc_architecture)
        if args.msvc_style
        else None
    )

    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary_root = Path(temporary_directory)
        for flavor, defines in FLAVOR_CONFIGURATIONS.items():
            print(f"Compiling isolated {flavor} amalgamation...", flush=True)
            output_dir = temporary_root / flavor
            header, implementation = generate(output_dir, defines=defines)
            consumer = output_dir / "consumer.cpp"
            consumer.write_text(
                '#include "libmp.h"\n'
                "using ApiTypes = LibMP::DefaultFlavor;\n"
                'static_assert(sizeof(ApiTypes::string) > 0, "public flavor unavailable");\n'
                "#if defined(_MSC_VER)\n"
                'static_assert(_HAS_EXCEPTIONS == 1, "consumer exception mode leaked");\n'
                "#endif\n"
                "#if NTD_TYPES_DISTINCT_FROM_STD && LIBMP_PUBLIC_NTD_API && "
                "CFG_ROUTE_DEPENDENCY_ALLOCATIONS_TO_NTD\n"
                "static_assert(sizeof(LibMPFlatbuffersStd::set<int>) > 0, "
                '"FlatBuffers set compatibility requires the default comparator");\n'
                "#endif\n"
                "int main() { return 0; }\n",
                encoding="utf-8",
            )
            object_suffix = ".obj" if args.msvc_style else ".o"
            compile_translation_unit(
                args.cxx,
                args.cxx_flag,
                implementation,
                temporary_root / f"{flavor}-libmp{object_suffix}",
                header.parent,
                args.msvc_style,
                compiler_id,
                True,
                environment,
            )
            compile_translation_unit(
                args.cxx,
                args.cxx_flag,
                consumer,
                temporary_root / f"{flavor}-consumer{object_suffix}",
                header.parent,
                args.msvc_style,
                compiler_id,
                False,
                environment,
            )


if __name__ == "__main__":
    main()

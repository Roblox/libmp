#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from amalgamate import generate as generate_amalgamation, implementation_sources
from _compiler_flags import library_warning_flags
from _utils import (
    UserConfigError,
    add_conditional_define_arguments,
    compiler_define_enabled,
    configured_emsdk_path,
    enabled_conditional_define_options,
    find_emsdk_tool,
    load_user_config,
    override_compiler_define,
    resolve_compiler_defines,
    robust_rmtree,
)


def main():
    parser = argparse.ArgumentParser(description="Compile LibMpCore to WASM or WASM with a JS wrapper via Emscripten")
    parser.add_argument("--js", action="store_true", help="Include a JS wrapper for the WASM module")
    parser.add_argument("--clean", action="store_true", help="Remove this target's output directory first")
    parser.add_argument("--amalgamated", action="store_true",
                        help="Compile LibMP from generated libmp.cpp/libmp.h files")
    try:
        config = load_user_config()
    except UserConfigError as error:
        parser.error(str(error))
    add_conditional_define_arguments(parser, config)
    args = parser.parse_args()

    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent
    env = os.environ.copy()
    enabled_options = enabled_conditional_define_options(args, config)
    compiler_defines = resolve_compiler_defines(config, enabled_options)

    # Apply the module's reduced feature surface before direct or amalgamated compilation.
    compiler_defines = override_compiler_define(
        compiler_defines, "LIBMP_EXCLUDE_CAPTURE_BUILDER=1"
    )
    compiler_defines = override_compiler_define(
        compiler_defines, "LIBMP_TRUSTED_INPUTS_ONLY=1"
    )
    use_external_flatbuffers = compiler_define_enabled(
        compiler_defines, "CFG_USE_EXTERNAL_FLATBUFFERS"
    )
    use_external_lz4 = compiler_define_enabled(
        compiler_defines, "CFG_USE_EXTERNAL_LZ4"
    )
    emsdk_path = configured_emsdk_path(config)
    emcc_bin = find_emsdk_tool(config, "upstream/emscripten", "em++")
    if emsdk_path:
        if not emcc_bin:
            print(f"Error: configured Emscripten SDK has no em++ under {emsdk_path}", file=sys.stderr)
            sys.exit(1)
        emscripten_path = emcc_bin.parent
        emscripten_str = str(emscripten_path)
        if emscripten_str not in env.get("PATH", ""):
            env["PATH"] = emscripten_str + os.pathsep + env.get("PATH", "")
    else:
        emcc_bin = shutil.which("em++")
        if not emcc_bin:
            print("Error: 'em++' was not found in PATH and no Emscripten SDK was configured.", file=sys.stderr)
            sys.exit(1)

    if args.js:
        sub_dir = "js"
        ext = ".js"
    else:
        sub_dir = "wasm"
        ext = ".wasm"

    out_dir = project_root / "build" / sub_dir
    out_file = out_dir / f"LibMpCore{ext}"

    if args.clean and out_dir.exists():
        print(f"Cleaning build directory: {out_dir}")
        robust_rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Compiling to {sub_dir}...")

    if args.amalgamated:
        external_dependencies = {
            name
            for name, enabled in (
                ("flatbuffers", use_external_flatbuffers),
                ("lz4", use_external_lz4),
            )
            if enabled
        }
        _, amalgamated_source = generate_amalgamation(
            out_dir / "amalgamated",
            external_dependencies=external_dependencies,
            defines=compiler_defines,
        )
        sources = [str(amalgamated_source)]
        # Do not expose the canonical tree: this makes a missed inlining step fail
        # instead of silently resolving the original header.
        include_dirs = [amalgamated_source.parent]
    else:
        sources = [str(project_root / source) for source in implementation_sources()]
        include_dirs = [project_root]
        if not use_external_flatbuffers:
            include_dirs.append(project_root / "dependencies")
    user_defines = [f"-D{define}" for define in compiler_defines]
    include_args = [f"-I{path}" for path in include_dirs]
    js_wrapper_settings = []
    if args.js:
        js_wrapper_settings.extend([
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=LibMpCore",
        ])

    cmd = [
        str(emcc_bin),
        *include_args,
        "-DLIBMP_WASM_BUILD",
        "-DLIBMP_MODULE_BUILD",
        "-DGPRX_NO_ASSERT",
        *user_defines,
        "-DEXCLUDE_FILE_DATA_CONTEXT",
        "-DEXCLUDE_FILE_STREAM_PROCESSOR",
        "-DEXCLUDE_CPP_API",
        "-std=c++17",
        "-O3",
        "-fno-exceptions",
        "-fno-rtti",
        *library_warning_flags("Emscripten"),
        *sources,
        "-o", str(out_file),
        "-sWASM=1",
        "-sEXPORTED_FUNCTIONS=['_malloc', '_free']",
        "-sEXPORTED_RUNTIME_METHODS=['cwrap', 'ccall', 'HEAPU8']",
        "-sASSERTIONS=0",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sTOTAL_MEMORY=4MB",
        "-sTOTAL_STACK=256KB",
        "-sSINGLE_FILE=1",
        "-sWARN_ON_UNDEFINED_SYMBOLS=0",
        *js_wrapper_settings,
        "-sERROR_ON_UNDEFINED_SYMBOLS=0",
        "--no-entry",
    ]

    result = subprocess.run(cmd, env=env)
    if result.returncode == 0:
        print(f"Success: {out_file}")
    else:
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()

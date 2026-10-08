#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Bundles LibMP Luau modules into a single file.
"""
import argparse
import os
import re
import sys
from pathlib import Path

from _build_metadata import (
    extract_line_comment_header,
    prepare_source_for_bundle_header,
    render_line_comment_header,
)
from _utils import load_build_config, resolve_repository_ref, utc_timestamp


SPIDER_RUNTIME_SOURCE_PATH = "Targets/Luau/Printer/runtime"


def spider_runtime_source_url(spider_config):
    repository = spider_config["repo"].rstrip("/")
    if repository.endswith(".git"):
        repository = repository[:-4]
    spider_ref = resolve_repository_ref(spider_config)
    return f"{repository}/tree/{spider_ref}/{SPIDER_RUNTIME_SOURCE_PATH}"


def render_bundle_header(local_header_lines, build_timestamp, spider_config):
    return render_line_comment_header(
        "--",
        build_timestamp=build_timestamp,
        bundled_dependencies=("flatbuffers", "lz4", "spider_runtime"),
        dependency_source_urls={
            "spider_runtime": spider_runtime_source_url(spider_config),
        },
        local_header_lines=local_header_lines,
        generated_by="scripts/bundle_luau.py",
    )


def main():
    parser = argparse.ArgumentParser(description="Bundle Luau modules into a single LibMP.luau file")
    parser.add_argument("target_dir", nargs="?", default="build/luau",
                        help="Target directory relative to project root (default: build/luau)")
    args = parser.parse_args()

    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent
    base_dir = project_root / args.target_dir

    libmp_file = base_dir / ".." / ".." / "mp_api" / "langs" / "luau" / "libmp_api.luau"
    libmptypes_file = base_dir / "LibMpTypes.luau"
    libmpcore_file = base_dir / "LibMpCore.luau"
    output_file = base_dir / "LibMP.luau"

    # Resolve to absolute for cleaner error messages
    libmp_file = libmp_file.resolve()
    libmptypes_file = libmptypes_file.resolve()
    libmpcore_file = libmpcore_file.resolve()

    print("    [Bundler] Starting full Luau module bundling...")

    for f in [libmp_file, libmptypes_file, libmpcore_file]:
        if not f.exists():
            print(f"    [Bundler] ERROR: Missing source file: {f}", file=sys.stderr)
            sys.exit(1)

    build_timestamp = utc_timestamp()
    spider_config = load_build_config()["spider"]

    # latin-1 maps bytes 1:1 and reproduces them exactly on write
    libmp_code = libmp_file.read_text(encoding="latin-1")
    libmptypes_code = libmptypes_file.read_text(encoding="latin-1")
    libmpcore_code = libmpcore_file.read_text(encoding="latin-1")

    libmp_code = prepare_source_for_bundle_header(libmp_code)
    local_header_lines, libmp_code = extract_line_comment_header(libmp_code, "--")
    libmp_code = (
        render_bundle_header(local_header_lines, build_timestamp, spider_config)
        + "\n"
        + libmp_code
    )

    re_libmptypes = re.compile(r"local\s+MpTypes\s*=\s*require\(['\"]\.\/LibMpTypes['\"]\)")
    re_libmpcore = re.compile(r"local\s+LibMpCoreFactory\s*=\s*require\(['\"]\.\/LibMpCore['\"]\)")

    libmptypes_header = """
-- >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
-- >>>>>>>>>>>>>>>>>>>>>>>>>> START OF: LibMpTypes <<<<<<<<<<<<<<<<<<<<<<<<<<<<<
-- >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"""

    libmpcore_header = """
-- >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
-- >>>>>>>>>>>>>>>>>>>>>>>>>>> START OF: LibMpCore <<<<<<<<<<<<<<<<<<<<<<<<<<<<<
-- >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"""

    module_footer = """
-- <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
-- <<<<<<<<<<<<<<<<<<<<<<<<<<<<<< END OF MODULE >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
-- <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"""

    libmptypes_replacement = f"{libmptypes_header}\nlocal MpTypes = (function()\n{libmptypes_code}\nend)()\n{module_footer}"
    libmpcore_replacement = f"{libmpcore_header}\nlocal LibMpCoreFactory = (function()\n{libmpcore_code}\nend)()\n{module_footer}"

    if re_libmptypes.search(libmp_code):
        libmp_code = re_libmptypes.sub(lambda m: libmptypes_replacement, libmp_code)
    else:
        print("    [Bundler] WARNING: Could not find LibMpTypes require statement.")

    if re_libmpcore.search(libmp_code):
        libmp_code = re_libmpcore.sub(lambda m: libmpcore_replacement, libmp_code)
    else:
        print("    [Bundler] WARNING: Could not find LibMpCore require statement.")

    output_file.write_text(libmp_code, encoding="latin-1")
    print(f"    [Bundler] Timestamp: {build_timestamp}")
    print(f"    [Bundler] Success! Bundled module saved to: {output_file}")


if __name__ == "__main__":
    main()

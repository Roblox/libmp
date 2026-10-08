#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Builds the Luau output from WASM:
1. Clones/builds Spider (wasm-to-luau converter)
2. Converts .wasm to .luau
3. Minifies local variable names
4. Appends module return
"""
import argparse
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

from _utils import load_build_config, resolve_repository_ref, robust_rmtree

BASE62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
STRIP_GENERATED_CORE_LEADING_TAB = True


def encode_base62(n):
    if n == 0:
        return BASE62[0]
    result = ""
    while n > 0:
        result = BASE62[n % 62] + result
        n //= 62
    return result


def minify_locals(file_path):
    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    count = 0

    def replacer(match):
        nonlocal count
        count += 1
        return "L" + encode_base62(int(match.group(1)))

    content = re.sub(r"\bloc_(\d+)_\b", replacer, content)

    stripped_tabs = 0
    if STRIP_GENERATED_CORE_LEADING_TAB:
        content, stripped_tabs = re.subn(r"(?m)^\t", "", content)

    with open(file_path, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"Minified {count} local variable references in {file_path}")
    if STRIP_GENERATED_CORE_LEADING_TAB:
        print(f"Removed {stripped_tabs} leading tabs from {file_path}")


def main():
    parser = argparse.ArgumentParser(description="Translate the LibMP WASM module to Luau")
    parser.add_argument("--clean", action="store_true", help="Remove the Luau output directory first")
    args = parser.parse_args()

    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent

    build_dir = project_root / "build"
    utils_dir = build_dir / "utils"
    spider_dir = utils_dir / "Spider"
    spider_manifest = spider_dir / "Cargo.toml"
    is_windows = platform.system() == "Windows"
    spider_exe = spider_dir / "target" / "release" / ("spider-cli.exe" if is_windows else "spider-cli")
    input_wasm = build_dir / "wasm" / "LibMpCore.wasm"
    output_luau_dir = build_dir / "luau"
    output_luau = output_luau_dir / "LibMpCore.luau"

    if args.clean and output_luau_dir.exists():
        print(f"Cleaning build directory: {output_luau_dir}")
        robust_rmtree(output_luau_dir)

    # 1) Read build config (comment-tolerant)
    build_config = load_build_config()
    spider_config = build_config["spider"]
    spider_repo = spider_config["repo"]
    spider_ref = resolve_repository_ref(spider_config)

    # 2) Create build/utils folder if missing
    utils_dir.mkdir(parents=True, exist_ok=True)

    # 3) Smart Git Checkout (Offline-first, only fetches if ref is missing locally)
    if not spider_dir.exists():
        print("Cloning Spider repository...")
        result = subprocess.run(["git", "clone", spider_repo, str(spider_dir)])
        if result.returncode != 0:
            print("Failed to clone Spider repo.", file=sys.stderr)
            sys.exit(1)

    print(f"Checking out Spider repository ref: {spider_ref}...")
    # Attempt local checkout first to avoid unnecessary network calls
    result = subprocess.run(["git", "checkout", spider_ref], cwd=str(spider_dir), capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Ref '{spider_ref}' not found locally. Fetching from remote...")
        fetch_res = subprocess.run(["git", "fetch", "--all"], cwd=str(spider_dir))
        if fetch_res.returncode != 0:
            print("Failed to fetch from remote repository.", file=sys.stderr)
            sys.exit(1)
        # Retry checkout after fetching new objects
        result = subprocess.run(["git", "checkout", spider_ref], cwd=str(spider_dir))
        if result.returncode != 0:
            print(f"Failed to checkout ref: {spider_ref}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"Successfully checked out '{spider_ref}' (local).")

    # 4) Build Spider (Cargo automatically skips compilation if files are unchanged)
    print("Verifying Spider build (incremental)...")
    result = subprocess.run(["cargo", "build", "--release", "--manifest-path", str(spider_manifest)])
    if result.returncode != 0:
        print("Cargo build failed. Ensure Rust is installed.", file=sys.stderr)
        sys.exit(1)

    # 5) Validate input
    if not input_wasm.exists():
        print(f"Input file not found: {input_wasm}", file=sys.stderr)
        sys.exit(1)

    # 6) Ensure output dir
    output_luau_dir.mkdir(parents=True, exist_ok=True)

    # 7) Execute wasm-to-luau
    print("Converting WASM to Luau (Spider optimizations disabled)...")
    print(f"Input:  {input_wasm}")
    print(f"Output: {output_luau}")

    result = subprocess.run([str(spider_exe), str(input_wasm)], capture_output=True, text=True)
    if result.returncode != 0:
        print("Conversion failed.", file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        sys.exit(1)

    with open(output_luau, "w", encoding="utf-8") as f:
        f.write(result.stdout)

    # 8) Minify variable names
    print("Minifying local variable names...")
    minify_locals(str(output_luau))

    # 9) Append module return
    print("Appending return statement...")
    with open(output_luau, "r", encoding="utf-8") as f:
        content = f.read()

    content = content + "\nreturn module\n"

    with open(output_luau, "w", encoding="utf-8") as f:
        f.write(content)

    print("Success!")


if __name__ == "__main__":
    main()

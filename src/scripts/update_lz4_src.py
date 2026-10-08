#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Fetches and updates LZ4 source files from Git and applies the extern "C" patch.
"""
import argparse
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

from _utils import robust_rmtree


def patch_sources_in_repo(lib_dir):
    """
    Replaces target C++ extern guards across source files inside the git repo.
    Returns the number of modified files.
    """
    print("Performing string replacements in git cache repository...")
    modified_count = 0

    # Group 1 captures '#if defined(__cplusplus)' with no other characters before the line break.
    # Group 2 captures the newline and the exact next line if it is 'extern "C" {' or '}'.
    target_pattern = re.compile(
        r'(#[ \t]*if[ \t]+)defined\s*\(\s*__cplusplus\s*\)[ \t]*(\r?\n\s*(?:extern\s+"C"\s*\{|\}))',
        re.MULTILINE
    )

    # \1 outputs their exact '#' and 'if' spacing; \2 preserves the next line untouched
    replacement_str = r'\1defined (__cplusplus) && defined (LZ4PATCH_USE_EXTERN_C)\2'

    for file_path in sorted(lib_dir.iterdir()):
        if not file_path.is_file() or file_path.suffix not in [".h", ".c"]:
            continue

        original_text = file_path.read_text(encoding="utf-8", errors="ignore")
        modified_text = target_pattern.sub(replacement_str, original_text)

        if original_text != modified_text:
            file_path.write_text(modified_text, encoding="utf-8")
            modified_count += 1

    return modified_count


def generate_git_patch(cache_dir, patch_file):
    """
    Uses native git diff to generate a patch file.
    Uses --relative=lib so patch paths match the trimmed target directory.
    """
    print("Generating patch using git diff...")
    res = subprocess.run(
        ["git", "diff", "-U0", "--relative=lib"],
        cwd=str(cache_dir),
        capture_output=True,
        text=True
    )
    if res.returncode != 0:
        print(f"Error executing git diff:\n{res.stderr}", file=sys.stderr)
        sys.exit(1)

    if res.stdout.strip():
        patch_file.parent.mkdir(parents=True, exist_ok=True)
        patch_file.write_text(res.stdout, encoding="utf-8")
        print(f"Successfully generated patch:\n  -> {patch_file}")
    else:
        print("No changes detected by Git. No patch generated.")


def main():
    parser = argparse.ArgumentParser(description="Fetch LZ4 sources and regenerate patch file using Git")
    parser.add_argument("--clean", action="store_true", help="Remove cached repository and target directory to start from scratch")
    args = parser.parse_args()

    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent
    dep_dir = project_root / "dependencies"
    target_dir = dep_dir / "lz4"
    version_file = dep_dir / "lz4_version.txt"
    patch_file = dep_dir / "patches" / "lz4_use_extern_c.patch"
    cache_dir = project_root / "dependencies" / "lz4_temp_repo"

    # 1. Read configuration
    if not version_file.exists():
        print(f"Version file missing: {version_file}", file=sys.stderr)
        sys.exit(1)

    with open(version_file, "r", encoding="utf-8") as f:
        config = json.load(f)

    repo_url = config.get("repo")
    commit_ref = config.get("commit") or config.get("ref") or config.get("tag") or config.get("branch")
    if not repo_url or not commit_ref:
        print("Missing 'repo' or 'commit'/'branch' in lz4_version.txt", file=sys.stderr)
        sys.exit(1)

    print(f"Target LZ4 Ref: {commit_ref}")

    # 2. Pre-process cleanup: Wipe leftover git cache or target dir if requested
    if cache_dir.exists():
        print("Removing leftover temporary git repository...")
        robust_rmtree(cache_dir)
    if args.clean and target_dir.exists():
        print("Removing target lz4 directory...")
        robust_rmtree(target_dir)

    try:
        # 3. Fresh Git Clone & Checkout
        print(f"Cloning {repo_url}...")
        cache_dir.parent.mkdir(parents=True, exist_ok=True)
        res = subprocess.run(["git", "clone", repo_url, str(cache_dir)])
        if res.returncode != 0:
            print("Failed to clone LZ4 repository.", file=sys.stderr)
            sys.exit(1)

        print(f"Checking out target ref: {commit_ref}...")
        res = subprocess.run(["git", "checkout", commit_ref], cwd=str(cache_dir), capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Failed to checkout ref: {commit_ref}", file=sys.stderr)
            sys.exit(1)

        lib_dir = cache_dir / "lib"
        if not lib_dir.exists():
            print(f"Error: 'lib' directory not found in repository at {lib_dir}", file=sys.stderr)
            sys.exit(1)

        # 4. Perform replacements inside the temporary Git repo & generate patch
        mod_count = patch_sources_in_repo(lib_dir)
        print(f"Modified {mod_count} files in temporary clone.")
        generate_git_patch(cache_dir, patch_file)

        # 5. Copy only direct files from lib/ to dependencies/lz4
        print("Copying direct library sources to dependencies/lz4...")
        if target_dir.exists():
            robust_rmtree(target_dir)
        target_dir.mkdir(parents=True, exist_ok=True)

        for item in lib_dir.iterdir():
            if item.is_file():
                shutil.copy2(item, target_dir / item.name)

        file_count = len(list(target_dir.iterdir()))
        print(f"Installed {file_count} files to {target_dir}")
        print(
            "Review the system-header hoist list in gprx_container/lz4_wrapper.cpp; "
            "the pinned commit may change without changing LZ4_VERSION_NUMBER."
        )
        print("\nLZ4 source update and native Git patch generation complete.")

    finally:
        # 6. Post-process cleanup: Delete the cached git repo when finished
        if cache_dir.exists():
            print("Cleaning up temporary git repository...")
            robust_rmtree(cache_dir)


if __name__ == "__main__":
    main()

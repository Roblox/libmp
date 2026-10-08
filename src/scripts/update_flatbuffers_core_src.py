#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Downloads and installs the FlatBuffers C++ headers and C# runtime sources from
the tagged release, along with LICENSE, README, and SECURITY files.
"""
import json
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

from _utils import robust_rmtree


def copy_csharp_source(source: Path, destination: Path):
    text = source.read_text(encoding="utf-8")
    lines = []
    for line in text.splitlines():
        line = line.rstrip()
        indentation = line[:len(line) - len(line.lstrip())]
        if " " in indentation and "\t" in indentation:
            line = indentation.expandtabs(4) + line[len(indentation):]
        lines.append(line)
    normalized = "\n".join(lines)
    if text.endswith(("\n", "\r")):
        normalized += "\n"
    destination.write_text(normalized, encoding="utf-8")


def main():
    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent
    dep_dir = project_root / "dependencies"
    cpp_target_dir = dep_dir / "flatbuffers"
    csharp_target_dir = dep_dir / "flatbuffers_csharp"
    version_file = dep_dir / "flatbuffers_version.txt"

    # Read core version
    if not version_file.exists():
        print(f"Version file missing: {version_file}", file=sys.stderr)
        sys.exit(1)

    with open(version_file, "r") as f:
        json_config = json.load(f)

    core_version = json_config.get("core_version")
    if not core_version:
        print("Missing 'core_version' field in version file.", file=sys.stderr)
        sys.exit(1)

    print(f"Target FlatBuffers Version: {core_version}")

    # Fetch URL template from config
    url_template = json_config.get("flatbuffers_url", "")
    if not url_template:
        print(f"Missing 'flatbuffers_url' field in version file: {version_file}", file=sys.stderr)
        sys.exit(1)
    url = url_template.format(core_version=core_version)

    # Download source archive
    zip_path = dep_dir / "flatbuffers_src.zip"
    temp_dir = dep_dir / "fb_temp_extract"

    print(f"Downloading {url}...")
    try:
        urllib.request.urlretrieve(url, str(zip_path))
    except Exception as e:
        print(f"Download failed: {e}", file=sys.stderr)
        sys.exit(1)

    # Extract & install
    print("Extracting FlatBuffers sources...")
    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(temp_dir)

    extracted_root = temp_dir / f"flatbuffers-{core_version}"
    cpp_source_dir = extracted_root / "include" / "flatbuffers"
    csharp_source_dir = extracted_root / "net" / "FlatBuffers"
    csharp_sources = sorted(csharp_source_dir.glob("*.cs"))
    if not cpp_source_dir.is_dir():
        print("Could not find 'include/flatbuffers' inside the archive.", file=sys.stderr)
        sys.exit(1)
    if not csharp_sources:
        print("Could not find C# runtime sources inside the archive.", file=sys.stderr)
        sys.exit(1)

    if cpp_target_dir.exists():
        print("Removing existing FlatBuffers C++ headers...")
        robust_rmtree(cpp_target_dir)
    shutil.move(str(cpp_source_dir), str(cpp_target_dir))
    print(f"C++ headers installed to: {cpp_target_dir}")

    print("Copying license, readme, and security files...")
    for pattern in ("LICENSE*", "README*", "SECURITY*"):
        for doc_file in extracted_root.glob(pattern):
            if doc_file.is_file():
                shutil.copy2(str(doc_file), str(cpp_target_dir))
                print(f"  Copied {doc_file.name}")

    csharp_target_dir.mkdir(parents=True, exist_ok=True)
    for old_source in csharp_target_dir.glob("*.cs"):
        old_source.unlink()
    for source in csharp_sources:
        copy_csharp_source(source, csharp_target_dir / source.name)
    print(f"C# runtime sources installed to: {csharp_target_dir}")

    # Cleanup
    print("Cleaning up...")
    if zip_path.exists():
        try:
            zip_path.unlink()
        except OSError as e:
            print(f"Warning: Could not delete zip file: {e}", file=sys.stderr)

    if temp_dir.exists():
        robust_rmtree(temp_dir)

    print("Done.")


if __name__ == "__main__":
    main()

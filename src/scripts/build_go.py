#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build the single-file Go wrapper with embedded WASM and FlatBuffers."""

import argparse
import base64
import json
import re
import subprocess
import sys
from pathlib import Path

from _build_metadata import (
    extract_line_comment_header,
    prepare_source_for_bundle_header,
    render_line_comment_header,
)


FLATBUFFER_GO_FILES = (
    "builder.go", "encode.go", "lib.go", "sizes.go", "struct.go", "table.go",
)


def run(command, **kwargs):
    print("  >", " ".join(str(item) for item in command), flush=True)
    result = subprocess.run([str(item) for item in command], **kwargs)
    if result.returncode:
        sys.exit(result.returncode)
    return result


def module_directory(module: str, version: str) -> Path:
    result = run(
        ["go", "mod", "download", "-json", f"{module}@{version}"],
        capture_output=True, text=True,
    )
    data = json.loads(result.stdout)
    if "Dir" not in data:
        raise RuntimeError(f"Go module download did not return a source directory: {data}")
    return Path(data["Dir"])


def strip_go_header(source: str) -> str:
    source = re.sub(r"^// Code generated.*?\n\n", "", source, count=1)
    source = re.sub(r"^package\s+\w+\s*\n", "", source, count=1, flags=re.MULTILINE)
    source = re.sub(r"^import\s*\(.*?^\)\s*\n", "", source, flags=re.MULTILINE | re.DOTALL)
    source = re.sub(r'^import\s+(?:\w+\s+)?"[^"]+"\s*\n', "", source,
                    flags=re.MULTILINE)
    return source.lstrip()


def make_views_growth_safe(source: str) -> str:
    source = source.replace("flatbuffers.", "")
    view_pattern = re.compile(
        r"type (?P<name>[A-Za-z_][A-Za-z0-9_]*) struct \{\n"
        r"\t_tab (?P<table>Table|Struct)\n\}"
    )
    views = [match.group("name") for match in view_pattern.finditer(source)]
    source = view_pattern.sub(
        lambda match: (
            f"type {match.group('name')} struct {{\n"
            f"\t_tab {match.group('table')}\n"
            "\t_memory *memoryRef\n"
            "\t_memoryGeneration uint64\n}"
        ),
        source,
    )
    for name in views:
        init_pattern = re.compile(
            rf"(func \(rcv \*{name}\) Init\(buf \[\]byte, i UOffsetT\) \{{\n"
            r"\trcv\._tab\.Bytes = buf\n\trcv\._tab\.Pos = i\n\})"
        )
        replacement = (
            r"\1\n\n"
            f"func (rcv *{name}) initShared(memory *memoryRef, i UOffsetT) {{\n"
            "\trcv.Init(memory.Bytes(), i)\n"
            "\trcv._memory = memory\n"
            "\trcv._memoryGeneration = memory.generation\n}\n\n"
            f"func (rcv *{name}) refreshMemory() {{\n"
            "\tif rcv._memory != nil && "
            "rcv._memoryGeneration != rcv._memory.generation {\n"
            "\t\trcv._tab.Bytes = rcv._memory.Bytes()\n"
            "\t\trcv._memoryGeneration = rcv._memory.generation\n\t}\n}"
        )
        source, count = init_pattern.subn(replacement, source, count=1)
        if count != 1:
            raise RuntimeError(f"could not instrument generated Go view {name}")

        method_pattern = re.compile(
            rf"(func \(rcv \*{name}\) "
            r"(?!(?:Init|initShared|refreshMemory)\b)"
            r"[A-Za-z_][A-Za-z0-9_]*\([^\{]*?\)(?: [^\{\n]+)? \{\n)"
        )
        source = method_pattern.sub(r"\1\trcv.refreshMemory()\n", source)

        source = re.sub(
            rf"(func \(rcv \*{name}\) Init\(buf \[\]byte, i UOffsetT\) \{{\n"
            r"\trcv\._tab\.Bytes = buf\n\trcv\._tab\.Pos = i\n)",
            r"\1\trcv._memory = nil\n",
            source,
            count=1,
        )

    source = re.sub(
        r"(?P<indent>\t+)obj\.Init\(rcv\._tab\.Bytes, (?P<position>[^\n]+)\)",
        lambda match: (
            f"{match.group('indent')}obj.Init(rcv._tab.Bytes, {match.group('position')})\n"
            f"{match.group('indent')}obj._memory = rcv._memory\n"
            f"{match.group('indent')}obj._memoryGeneration = rcv._memoryGeneration"
        ),
        source,
    )
    return source


def main():
    parser = argparse.ArgumentParser(
        description="Bundle the Go API, FlatBuffers types/runtime, and WASM"
    )
    parser.add_argument("--clean", action="store_true",
                        help="Remove the Go output first; module downloads are preserved")
    args = parser.parse_args()
    project_root = Path(__file__).resolve().parent.parent
    build_dir = project_root / "build" / "go"
    output = build_dir / "LibMP.go"
    wasm = project_root / "build" / "wasm" / "LibMpCore.wasm"
    generated = build_dir / "mp_types_generated.go"
    api = project_root / "mp_api" / "langs" / "go" / "libmp_api.go"
    version_config = json.loads(
        (project_root / "dependencies" / "flatbuffers_version.txt").read_text(encoding="utf-8"))
    if args.clean:
        output.unlink(missing_ok=True)
    missing = [path for path in (wasm, generated, api) if not path.exists()]
    if missing:
        print("Missing generated input(s): " + ", ".join(map(str, missing)), file=sys.stderr)
        print("Run scripts/build.py --go.", file=sys.stderr)
        return 1

    flatbuffers_dir = module_directory(
        "github.com/google/flatbuffers", "v" + version_config["core_version"] + "+incompatible"
    ) / "go"
    runtime = []
    for filename in FLATBUFFER_GO_FILES:
        path = flatbuffers_dir / filename
        if not path.exists():
            raise RuntimeError(f"missing FlatBuffers Go runtime source: {path}")
        runtime.append(f"// --- FlatBuffers {filename} ---\n" + strip_go_header(
            path.read_text(encoding="utf-8")))

    generated_source = make_views_growth_safe(strip_go_header(
        generated.read_text(encoding="utf-8")))
    api_source = prepare_source_for_bundle_header(api.read_text(encoding="utf-8"))
    local_header_lines, api_source = extract_line_comment_header(api_source, "//")
    api_source = strip_go_header(api_source)
    wasm_source = (
        "const libMPWasmBase64 = `" +
        base64.b64encode(wasm.read_bytes()).decode("ascii") + "`\n"
    )
    imports = """import (
    "bytes"
    "encoding/base64"
    "encoding/binary"
    "errors"
    "fmt"
    "io"
    "math"
    "os"
    "runtime"
    "sort"
    "strconv"
    "sync"
    "unsafe"

    wasmtime "github.com/bytecodealliance/wasmtime-go/v39"
)
"""
    contents = (
        render_line_comment_header(
            "//",
            local_header_lines=local_header_lines,
            generated_by="scripts/build_go.py",
        ) + "\n" +
        "package libmp\n\n" + imports + "\n" +
        "\n\n".join(runtime) +
        "\n\n// === Generated FlatBuffers types ===\n\n" + generated_source +
        "\n\n// === LibMP API ===\n\n" + api_source +
        "\n\n// === Embedded WASM ===\n\n" + wasm_source
    )
    output.write_text(contents, encoding="utf-8")
    run(["gofmt", "-w", output])
    print(f"Success: {output} ({output.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

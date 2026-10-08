#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Build the single-file Python wrapper with embedded WASM and FlatBuffers."""

import argparse
import base64
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Tuple

from _build_metadata import (
    extract_line_comment_header,
    prepare_source_for_bundle_header,
    render_line_comment_header,
)
from _utils import load_build_config


FLATBUFFER_MODULES = (
    "compat", "packer", "number_types", "encode", "table", "builder",
    "util", "_version", "__init__",
)


def run(command):
    print("  >", " ".join(str(item) for item in command), flush=True)
    result = subprocess.run([str(item) for item in command])
    if result.returncode:
        sys.exit(result.returncode)


def ensure_flatbuffers(project_root: Path, version: str) -> Path:
    tools_dir = project_root / "build" / "utils" / "python_tools"
    package_dir = tools_dir / "flatbuffers"
    marker = tools_dir / "flatbuffers-version.txt"
    if package_dir.exists() and marker.exists() and marker.read_text().strip() == version:
        return package_dir
    if tools_dir.exists():
        shutil.rmtree(tools_dir)
    tools_dir.mkdir(parents=True)
    run([
        sys.executable, "-m", "pip", "install", "--disable-pip-version-check",
        "--no-deps", "--target", tools_dir, f"flatbuffers=={version}",
    ])
    marker.write_text(version + "\n", encoding="utf-8")
    return package_dir


def runtime_sources(package_dir: Path) -> Dict[str, str]:
    result = {}
    for module in FLATBUFFER_MODULES:
        path = package_dir / f"{module}.py"
        if not path.exists():
            raise RuntimeError(f"missing FlatBuffers runtime source: {path}")
        result[module] = path.read_text(encoding="utf-8")
    return result


def generated_sources(types_dir: Path) -> Dict[str, str]:
    result = {}
    for path in sorted(types_dir.glob("*.py")):
        if path.name == "__init__.py":
            continue
        source = path.read_text(encoding="utf-8")
        source = source.replace("import flatbuffers", "import _libmp_flatbuffers as flatbuffers")
        source = source.replace("from flatbuffers.", "from _libmp_flatbuffers.")
        source = source.replace("from MpTypes.", "from _libmp_types.")
        source = source.replace("import MpTypes.", "import _libmp_types.")
        source = source.replace("MpTypes.", "_libmp_types.")
        result[path.stem] = source
    if not result:
        raise RuntimeError(f"no generated Python types found in {types_dir}")
    return result


def ordered_type_modules(sources: Dict[str, str]) -> Tuple[str, ...]:
    dependencies = {
        name: set(re.findall(r"_libmp_types\.([A-Za-z_][A-Za-z0-9_]*)", source))
        for name, source in sources.items()
    }
    result = []
    active = set()
    complete = set()

    def visit(name):
        if name in complete:
            return
        if name in active:
            raise RuntimeError(f"cyclic generated Python type imports at {name}")
        active.add(name)
        for dependency in sorted(dependencies[name]):
            if dependency in sources:
                visit(dependency)
        active.remove(name)
        complete.add(name)
        result.append(name)

    for name in sources:
        visit(name)
    return tuple(result)


def main():
    parser = argparse.ArgumentParser(
        description="Bundle the Python API, FlatBuffers types/runtime, and WASM"
    )
    parser.add_argument("--clean", action="store_true",
                        help="Remove the Python output first; cached build tools are preserved")
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parent.parent
    output = project_root / "build" / "python" / "LibMP.py"
    wasm = project_root / "build" / "wasm" / "LibMpCore.wasm"
    types_dir = project_root / "build" / "python" / "MpTypes"
    api = project_root / "mp_api" / "langs" / "python" / "libmp_api.py"
    version_config = json.loads(
        (project_root / "dependencies" / "flatbuffers_version.txt").read_text(encoding="utf-8"))
    build_config = load_build_config()

    if args.clean:
        output.unlink(missing_ok=True)
    missing = [path for path in (wasm, types_dir, api) if not path.exists()]
    if missing:
        print("Missing generated input(s): " + ", ".join(map(str, missing)), file=sys.stderr)
        print("Run scripts/build.py --python.", file=sys.stderr)
        return 1

    package_dir = ensure_flatbuffers(project_root, version_config["python_version"])
    flatbuffer_sources = runtime_sources(package_dir)
    type_sources = generated_sources(types_dir)
    type_order = ordered_type_modules(type_sources)
    api_source = api.read_text(encoding="utf-8")
    api_source = prepare_source_for_bundle_header(api_source)
    local_header_lines, api_source = extract_line_comment_header(api_source, "#")
    api_source = re.sub(r"^from __future__ import annotations\s*\n", "", api_source,
                        count=1, flags=re.MULTILINE)
    bundle_header = render_line_comment_header(
        "#",
        local_header_lines=local_header_lines,
        generated_by="scripts/build_python.py",
    )

    bootstrap = f'''{bundle_header}
from __future__ import annotations

import sys as _sys
import types as _module_types

_FLATBUFFER_SOURCES = {flatbuffer_sources!r}
_TYPE_SOURCES = {type_sources!r}


def _install_embedded_modules():
    package = _module_types.ModuleType("_libmp_flatbuffers")
    package.__path__ = []
    package.__package__ = "_libmp_flatbuffers"
    _sys.modules[package.__name__] = package
    for name in {FLATBUFFER_MODULES[:-1]!r}:
        module_name = package.__name__ + "." + name
        module = _module_types.ModuleType(module_name)
        module.__package__ = package.__name__
        _sys.modules[module_name] = module
        setattr(package, name, module)
        exec(compile(_FLATBUFFER_SOURCES[name], module_name, "exec"), module.__dict__)
    exec(compile(_FLATBUFFER_SOURCES["__init__"], package.__name__, "exec"), package.__dict__)

    types_package = _module_types.ModuleType("_libmp_types")
    types_package.__path__ = []
    types_package.__package__ = "_libmp_types"
    _sys.modules[types_package.__name__] = types_package
    modules = {{}}
    for name in _TYPE_SOURCES:
        module_name = types_package.__name__ + "." + name
        module = _module_types.ModuleType(module_name)
        module.__package__ = types_package.__name__
        _sys.modules[module_name] = module
        setattr(types_package, name, module)
        modules[name] = module
    for name in {type_order!r}:
        module = modules[name]
        module_name = module.__name__
        source = _TYPE_SOURCES[name]
        exec(compile(source, module_name, "exec"), module.__dict__)

    exported = {{}}
    for module in modules.values():
        for name, value in vars(module).items():
            if isinstance(value, type) and value.__module__ == module.__name__:
                exported[name] = value
    return package, _module_types.SimpleNamespace(**exported)


flatbuffers, Types = _install_embedded_modules()
FlatBuffers = flatbuffers
LIBMP_WASM_BASE64 = {base64.b64encode(wasm.read_bytes()).decode("ascii")!r}
WASMTIME_PYTHON_REQUIREMENT = {build_config["wasmtime"]["python_requirement"]!r}
WASMTIME_FAST_MIN_MAJOR = {build_config["wasmtime"]["python_fast_min_major"]!r}
WASMTIME_FAST_MAX_MAJOR_EXCLUSIVE = {build_config["wasmtime"]["python_fast_max_major_exclusive"]!r}

'''
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(bootstrap + api_source, encoding="utf-8")
    print(f"Success: {output} ({output.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

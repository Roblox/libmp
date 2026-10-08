#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Generates FlatBuffers code for one or more target languages.
"""
import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from _build_metadata import prepare_source_for_bundle_header


CONTROL_COMMAND_RE = re.compile(
    r"^\s*/\*\s*@control_command\s+"
    r"id=(?P<id>\d+)\s+"
    r"name=(?P<name>[A-Z][A-Za-z0-9]*)\s+"
    r"request=(?P<request>[A-Za-z_][A-Za-z0-9_]*)\s+"
    r"response=(?P<response>[A-Za-z_][A-Za-z0-9_]*)\s*\*/\s*$"
)

FLATBUFFER_TYPE_RE = re.compile(
    r"^\s*(?P<kind>struct|table)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b[^\{]*\{",
    re.MULTILINE,
)


@dataclass(frozen=True)
class ControlCommand:
    command_id: int
    name: str
    request: str
    response: str
    request_kind: str
    response_kind: str


def parse_control_commands(schema_text):
    declared_types = {}
    for match in FLATBUFFER_TYPE_RE.finditer(schema_text):
        declared_types[match.group("name")] = match.group("kind").title()

    commands = []
    names = set()
    ids = set()
    for line_number, line in enumerate(schema_text.splitlines(), 1):
        if "@control_command" not in line:
            continue
        match = CONTROL_COMMAND_RE.match(line)
        if not match:
            raise RuntimeError(
                f"Malformed @control_command annotation on line {line_number}; expected "
                "'/* @control_command id=N name=Name request=Type response=Type */'"
            )

        command_id = int(match.group("id"))
        name = match.group("name")
        request = match.group("request")
        response = match.group("response")
        if command_id <= 0 or command_id > 0xFFFF:
            raise RuntimeError(
                f"@control_command id on line {line_number} must be in 1..65535"
            )
        if command_id in ids:
            raise RuntimeError(f"Duplicate @control_command id={command_id}")
        if name in names:
            raise RuntimeError(f"Duplicate @control_command name={name}")
        for field, type_name in (("request", request), ("response", response)):
            if type_name not in declared_types:
                raise RuntimeError(
                    f"Unknown {field} type '{type_name}' for @control_command {name}"
                )

        command = ControlCommand(
            command_id=command_id,
            name=name,
            request=request,
            response=response,
            request_kind=declared_types[request],
            response_kind=declared_types[response],
        )
        ids.add(command_id)
        names.add(name)
        commands.append(command)

    if not commands:
        raise RuntimeError("No @control_command annotations found")
    return commands


def render_control_command_registry(commands):
    has_table_payloads = any(
        command.request_kind == "Table" or command.response_kind == "Table"
        for command in commands
    )
    lines = [
        "// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.",
        "",
        "// Generated from @control_command annotations in mp_types.fbs.",
        "// Do not edit directly; run scripts/gen_flatbuffers.py --cpp.",
        "// Contains schema facts; expansion logic lives in control_channel.h/.cpp.",
        "// Command APIs use the complete generated struct or table types.",
        "",
        f"#define LIBMP_CONTROL_HAS_TABLE_PAYLOADS {int(has_table_payloads)}",
        "",
    ]
    macro_lines = ["#define LIBMP_CONTROL_COMMANDS(X)"]
    for command in commands:
        macro_lines.append(
            f"    X({command.command_id}, {command.name}, "
            f"{command.request}, {command.request_kind}, "
            f"{command.response}, {command.response_kind})"
        )
    continuation_column = max(len(line) for line in macro_lines) + 1
    lines.extend(
        line.ljust(continuation_column) + "\\" if index + 1 < len(macro_lines) else line
        for index, line in enumerate(macro_lines)
    )
    lines.append("")
    return "\n".join(lines)


def generate_control_commands(schema_path, target_dir):
    commands = parse_control_commands(schema_path.read_text(encoding="utf-8"))
    registry_path = target_dir / "control_commands_generated.inc"
    registry_path.write_text(render_control_command_registry(commands), encoding="utf-8")
    print(f"    [Control] Wrote {registry_path.name}")


def download_file(url, dest):
    print(f"    Downloading {url}...")
    urllib.request.urlretrieve(url, dest)


def get_flatc_version(exe_path):
    """Executes flatc and returns the version string (e.g., '25.12.19'), or None if it fails."""
    if not exe_path.exists():
        return None
    try:
        res = subprocess.run([str(exe_path), "--version"], capture_output=True, text=True)
        if res.returncode == 0:
            # Expected output format: "flatc version 25.12.19"
            parts = res.stdout.strip().split()
            return parts[-1] if parts else None
    except Exception:
        pass
    return None


def versions_match(actual_ver, config_ver):
    """
    Checks if the reported binary version matches the config version.
    Handles extended tag strings like '25.12.19-2026-02-06-03fffb2' reporting as '25.12.19'.
    """
    if not actual_ver or not config_ver:
        return False
    if actual_ver == config_ver:
        return True
    # Extract base semantic versions (e.g., "25.12.19" from "25.12.19-2026-02-06-03fffb2")
    config_base = re.split(r'[-+]', config_ver)[0]
    actual_base = re.split(r'[-+]', actual_ver)[0]
    return actual_base == config_base


def ensure_flatc(flatc_dir, core_version, version_config, clean=False):
    """Returns a verified flatc executable, downloading the configured version if needed."""
    system_name = platform.system()
    is_windows = system_name == "Windows"
    flatc_exe = flatc_dir / ("flatc.exe" if is_windows else "flatc")

    if clean and flatc_dir.exists():
        print("Removing existing flatc directory to start from scratch...")
        shutil.rmtree(flatc_dir)

    if flatc_exe.exists():
        actual_ver = get_flatc_version(flatc_exe)
        if versions_match(actual_ver, core_version):
            print(f"flatc version {actual_ver}")
        else:
            print(f"Version mismatch (found '{actual_ver}', expected '{core_version}'). Re-downloading...")
            shutil.rmtree(flatc_dir)

    if flatc_exe.exists():
        return flatc_exe

    print("flatc not found or version mismatched. Downloading...")
    os_name = {"Windows": "Windows", "Darwin": "Mac", "Linux": "Linux"}.get(system_name)
    if not os_name:
        print(f"Unsupported platform: {system_name}", file=sys.stderr)
        sys.exit(1)

    url_template = version_config.get("flatc_url", "")
    if not url_template:
        print("Missing 'flatc_url' in FlatBuffers version configuration.", file=sys.stderr)
        sys.exit(1)

    # Some platforms publish the asset with a compiler-specific suffix
    # (e.g. Linux.flatc.binary.clang++-18.zip); Mac/Windows have none.
    asset_suffix = version_config.get("flatc_asset_suffix", {}).get(os_name, "")
    url = url_template.format(core_version=core_version, os_name=os_name, suffix=asset_suffix)

    flatc_dir.mkdir(parents=True, exist_ok=True)
    zip_path = flatc_dir / "flatc.zip"
    try:
        download_file(url, str(zip_path))
    except Exception as error:
        print(f"Download failed: {error}", file=sys.stderr)
        sys.exit(1)

    with zipfile.ZipFile(zip_path, "r") as archive:
        archive.extractall(flatc_dir)
    zip_path.unlink()

    if not is_windows:
        flatc_exe.chmod(0o755)

    actual_ver = get_flatc_version(flatc_exe)
    if not versions_match(actual_ver, core_version):
        print(
            f"Error: Downloaded flatc version ('{actual_ver}') does not match expected '{core_version}'.",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"Successfully downloaded and verified: flatc version {actual_ver}")
    return flatc_exe


# =============================================================================
# Luau Post-Process
# =============================================================================

def luau_postprocess(target_dir):
    print("    [Luau-Post] Starting unification of the Luau FlatBuffers library and structs...")

    compat_file = target_dir / ".." / ".." / "mp_api" / "langs" / "luau" / "flatbuffers_compat.luau"
    compat_file = compat_file.resolve()
    types_dir = target_dir / "MpTypes"
    output_file = target_dir / "LibMpTypes.luau"
    namespace_name = "MpTypes"

    # Section 1: Library
    if not compat_file.exists():
        print(f"ERROR: Compatibility library missing: {compat_file}", file=sys.stderr)
        sys.exit(1)

    lib_code = compat_file.read_text(encoding="utf-8")
    lib_code = prepare_source_for_bundle_header(lib_code)
    lib_code = re.sub(r"return\s+FlatBuffers\s*$", "", lib_code, flags=re.MULTILINE)

    # Section 2: Types
    types_content = ""
    if types_dir.exists():
        lua_files = sorted(
            f for f in types_dir.iterdir()
            if f.suffix == ".lua"
            and f.name != output_file.name
        )

        content_parts = []
        re_flatbuffers = re.compile(r"local\s+flatbuffers\s*=\s*require\(['\"]flatbuffers['\"]\)")
        re_return = re.compile(r"^return\s+(\w+)\s*$", re.MULTILINE)
        re_require = re.compile(r"local\s+(\w+)\s*=\s*require\(['\"].*\.([^.]+?)['\"]\)")

        for lua_file in lua_files:
            code = lua_file.read_text(encoding="utf-8")

            # Remove FlatBuffers require
            code = re_flatbuffers.sub("", code)

            # Resolve cross-type dependencies
            requires = list(re_require.finditer(code))
            for req in requires:
                code = code.replace(req.group(0), "")
                alias_regex = re.compile(r"\b" + re.escape(req.group(1)) + r"\b")
                code = alias_regex.sub(f"{namespace_name}.{req.group(2)}", code)

            # Mount types to namespace
            return_match = re_return.search(code)
            if return_match:
                class_name = return_match.group(1)
                code = re_return.sub(f"{namespace_name}.{class_name} = {class_name}", code)

            content_parts.append(f"-- Type: {lua_file.name}")
            content_parts.append("do")
            content_parts.append(code)
            content_parts.append("end\n")

        types_content = "\n".join(content_parts)

    # Section 3: Assembly
    final_output = f"""
--[[
   UNIFIED FLATBUFFERS FILE
   Contains:
   1. FlatBuffers Luau Buffer Implementation
   2. Generated Types
--]]

local UnifiedResult = (function()

-- 1. FLATBUFFERS LIBRARY CORE =================================================
{lib_code}

-- 2. GENERATED TYPES ==========================================================
local {namespace_name} = {{}}
local flatbuffers = FlatBuffers

{types_content}

-- 3. EXPORT ===================================================================
{namespace_name}.FlatBuffers = FlatBuffers

return {namespace_name}

end)()

return UnifiedResult
"""

    output_file.write_text(final_output, encoding="utf-8")
    print(f"    [Luau-Post] Success! Saved to: {output_file}")


# =============================================================================
# C++ Post-Process: raw flatc header -> parameterized, macro-driven fragment
# =============================================================================

# The object-API native members flatc emits for our schema. The C++ fragment is
# parameterized by a blanket std::->FBSTD:: rename, which is only valid while the
# generated std footprint stays within the container types ntd:: mirrors.
CPP_REWRITABLE_STD_TYPES = {"string", "vector", "unique_ptr"}


def required_substitution(text, pattern, replacement, expectation):
    rewritten, count = re.subn(pattern, replacement, text)
    if count == 0:
        print(f"ERROR: [Cpp-Post] {expectation}", file=sys.stderr)
        sys.exit(1)
    return rewritten


def cpp_postprocess(target_dir):
    """Rewrite flatc's mp_types_generated.h into mp_types_generated.inc: a
    guard-less, macro-driven fragment included once per flavor by mp_types_std.h
    and mp_types_ntd.h (FBSTD = std|ntd container namespace; FBTYPES =
    MpTypesStd|MpTypesNtd schema namespace). This lets both object-API type
    families coexist from one source of truth."""
    print("  > [Cpp-Post] Parameterizing generated header into .inc fragment...")
    header_path = target_dir / "mp_types_generated.h"
    text = header_path.read_text(encoding="utf-8")

    # The std/ntd wrappers select the bundled or host FlatBuffers core before
    # instantiating this fragment. Keeping the include here would make that choice
    # too late and could reuse an incompatible namespace through the include guard.
    text = required_substitution(
        text,
        r'#include "flatbuffers/flatbuffers\.h"',
        "",
        "expected the generated FlatBuffers core include; flatc output changed -- "
        "update the wrapper-owned include rewrite in cpp_postprocess.",
    )

    # Validate the generated standard-library surface. A blanket rename below
    # would turn any unsupported std type into a bogus FBSTD type.
    found = set(re.findall(r"std::([A-Za-z_][A-Za-z0-9_]*)", text))
    unexpected = sorted(found - CPP_REWRITABLE_STD_TYPES)
    if unexpected:
        print(
            f"ERROR: [Cpp-Post] Unexpected std:: types in generated code: {unexpected}.\n"
            f"       The ntd FlatBuffers flavor mirrors only {sorted(CPP_REWRITABLE_STD_TYPES)};\n"
            f"       extend ntd_stdlib + CPP_REWRITABLE_STD_TYPES before regenerating.",
            file=sys.stderr,
        )
        sys.exit(1)

    # Rewrite vector construction to generic pointer forms. CreateVectorOfStructs
    # has only std::vector and raw-pointer overloads, while object-API Pack calls to
    # CreateVector cannot deduce a non-std vector. Pointer forms work for both flavors.
    # Pack: _fbb.CreateVectorOfStructs(_o->field)
    text = required_substitution(
        text,
        r"CreateVectorOfStructs\((_o->\w+)\)",
        r"CreateVectorOfStructs(\1.data(), \1.size())",
        "expected object-API CreateVectorOfStructs calls; flatc output changed -- "
        "update the vector rewrite in cpp_postprocess.",
    )
    # Direct: _fbb.CreateVectorOfStructs<T>(*field) (field is a pointer parameter)
    text = required_substitution(
        text,
        r"CreateVectorOfStructs<[^>]*>\(\*(\w+)\)",
        r"CreateVectorOfStructs(\1->data(), \1->size())",
        "expected direct CreateVectorOfStructs calls; flatc output changed -- "
        "update the vector rewrite in cpp_postprocess.",
    )
    # Pack: _fbb.CreateVector(_o->field). Calls with an explicit <T> already bind.
    text = required_substitution(
        text,
        r"CreateVector\((_o->\w+)\)",
        r"CreateVector(\1.data(), \1.size())",
        "expected object-API CreateVector calls; flatc output changed -- "
        "update the vector rewrite in cpp_postprocess.",
    )

    # Avoid std::string temporaries in String UnPackTo. assign(c_str(), size()) is
    # supported by both flavors and copies directly from the FlatBuffers String.
    text = required_substitution(
        text,
        r"(_o->\w+) = (\w+)->str\(\)",
        r"\1.assign(\2->c_str(), \2->size())",
        "expected object-API string assignments; flatc output changed -- "
        "update the string rewrite in cpp_postprocess.",
    )

    # flatc constructs each UnPack result as unique_ptr<T>(new T()). In the ntd
    # flavor, allocation and deletion must use the same ntd::mem backend, so use
    # make_unique to create the object and its owner through the matching API.
    text = required_substitution(
        text,
        r"std::unique_ptr<(\w+)>\(new \1\(\)\)",
        r"std::make_unique<\1>()",
        "expected FlatBuffers UnPack unique_ptr(new T) expressions; flatc output changed -- "
        "update the make_unique rewrite in cpp_postprocess.",
    )

    # Replace hidden std::function aliases in resolver/rehasher parameters. These
    # callbacks are always null for this schema, but their types appear in generated
    # signatures and must use ntd::function in the autonomous flavor.
    text = required_substitution(
        text,
        r"::flatbuffers::resolver_function_t",
        r"FBSTD::function<void (void **, ::flatbuffers::hash_value_t)>",
        "expected flatc resolver_function_t references; flatc output changed -- update "
        "the resolver rewrite in cpp_postprocess.",
    )
    text = required_substitution(
        text,
        r"::flatbuffers::rehasher_function_t",
        r"FBSTD::function<::flatbuffers::hash_value_t (void *)>",
        "expected flatc rehasher_function_t references; flatc output changed -- update "
        "the resolver rewrite in cpp_postprocess.",
    )

    # Strip the generated include guard because this fragment is included once per
    # flavor. The wrapper includes flatbuffers.h before instantiating the fragment.
    text = required_substitution(
        text,
        r"#ifndef FLATBUFFERS_GENERATED_\w+\s*\n#define FLATBUFFERS_GENERATED_\w+\s*\n",
        "",
        "expected the generated include-guard opening; flatc output changed -- "
        "update the guard rewrite in cpp_postprocess.",
    )
    text = required_substitution(
        text,
        r"\n#endif\s*//\s*FLATBUFFERS_GENERATED_\w+\s*\n?",
        "\n",
        "expected the generated include-guard closing; flatc output changed -- "
        "update the guard rewrite in cpp_postprocess.",
    )

    # Parameterize the standard-library and schema namespaces. flatbuffers:: stays
    # unchanged here; bundled wrappers rename it token-precisely, while external
    # wrappers intentionally bind it to the host's ::flatbuffers namespace.
    text = required_substitution(
        text,
        r"\bstd::",
        "FBSTD::",
        "expected standard-library namespace references; flatc output changed -- "
        "update namespace parameterization in cpp_postprocess.",
    )
    text = required_substitution(
        text,
        r"\bMpTypes\b",
        "FBTYPES",
        "expected the schema namespace; flatc output changed -- update namespace "
        "parameterization in cpp_postprocess.",
    )

    banner = (
        "// Generated by flatc, then parameterized by scripts/gen_flatbuffers.py.\n"
        "// DO NOT include directly or edit: include mp_types_std.h (std) or\n"
        "// mp_types_ntd.h (ntd), which select the core and set FBSTD / FBTYPES.\n"
        "#if !defined(FBSTD) || !defined(FBTYPES)\n"
        '#error "mp_types_generated.inc requires FBSTD/FBTYPES; include a mp_types wrapper"\n'
        "#endif\n\n"
    )
    out_path = header_path.with_suffix(".inc")
    out_path.write_text(banner + text, encoding="utf-8")
    header_path.unlink()  # only the parameterized .inc fragment is used / checked in
    print(f"    [Cpp-Post] Wrote {out_path.name}")


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description="Generate FlatBuffers code for target languages")
    parser.add_argument("--clean", action="store_true", help="Remove existing flatc executable and download from scratch")
    parser.add_argument("--cpp", action="store_true")
    parser.add_argument("--js", action="store_true")
    parser.add_argument("--luau", action="store_true")
    parser.add_argument("--python", action="store_true")
    parser.add_argument("--csharp", action="store_true")
    parser.add_argument("--rust", action="store_true")
    parser.add_argument("--go", action="store_true")
    args = parser.parse_args()

    script_path = Path(__file__).resolve().parent
    project_root = script_path.parent
    flatc_dir = project_root / "build" / "utils" / "flatc"
    schema_path = project_root / "mp_api" / "flatbuffer_types" / "mp_types.fbs"
    version_file = project_root / "dependencies" / "flatbuffers_version.txt"

    # Read versions
    if not version_file.exists():
        print(f"Version file missing: {version_file}", file=sys.stderr)
        sys.exit(1)

    with open(version_file, "r") as f:
        json_config = json.load(f)

    core_version = json_config.get("core_version")
    npm_version = json_config.get("npm_version")
    if not core_version or not npm_version:
        print("Missing core_version or npm_version fields in version file.", file=sys.stderr)
        sys.exit(1)

    print(f"FlatBuffers Core Ver: {core_version} | NPM Ver: {npm_version}")
    flatc_exe = ensure_flatc(flatc_dir, core_version, json_config, clean=args.clean)

    # Define language targets
    targets = {}
    if args.cpp:
        targets["Cpp"] = {
            "flag": "--cpp",
            "out_dir": "mp_api/flatbuffer_types",
            "extra_flags": ["--gen-mutable"],
            "postprocess": cpp_postprocess,
        }
    if args.js:
        targets["Js"] = {"flag": "--ts", "out_dir": "build/js"}
    if args.luau:
        targets["Luau"] = {
            "flag": "--lua",
            "out_dir": "build/luau",
            "postprocess": luau_postprocess,
        }
    if args.python:
        targets["Python"] = {"flag": "--python", "out_dir": "build/python"}
    if args.csharp:
        targets["CSharp"] = {"flag": "--csharp", "out_dir": "build/csharp"}
    if args.rust:
        targets["Rust"] = {"flag": "--rust", "out_dir": "build/rust"}
    if args.go:
        targets["Go"] = {
            "flag": "--go",
            "out_dir": "build/go",
            "extra_flags": ["--gen-onefile", "--go-namespace", "libmp"],
        }

    if not targets:
        if args.clean:
            print("\nClean and re-download complete. No language targets specified for generation.")
            sys.exit(0)
        print("No language switch provided.")
        sys.exit(0)

    # Generation loop
    for key, conf in targets.items():
        full_out_dir = project_root / conf["out_dir"]
        full_out_dir.mkdir(parents=True, exist_ok=True)

        print(f"\n[{key}] Generating Code...")

        flatc_args = [str(flatc_exe), conf["flag"], "--gen-object-api"]
        flatc_args.extend(conf.get("extra_flags", []))
        flatc_args.append(str(schema_path))

        result = subprocess.run(flatc_args, cwd=str(full_out_dir))
        if result.returncode != 0:
            print(f"Flatc failed for {key}", file=sys.stderr)
            sys.exit(1)

        # Post-process
        postprocess = conf.get("postprocess")
        if postprocess:
            print("  > Running Post-Process...")
            postprocess(full_out_dir)
        if key == "Cpp":
            generate_control_commands(schema_path, full_out_dir)

    print("\nAll Done.")


if __name__ == "__main__":
    main()

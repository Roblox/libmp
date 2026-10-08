#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""
Shared utilities for project scripts.
"""
import argparse
import copy
import glob
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from tempfile import NamedTemporaryFile


SCRIPT_DIR = Path(__file__).resolve().parent
USER_CONFIG_PATH = SCRIPT_DIR / "user-config.json"
LOCAL_USER_CONFIG_PATH = SCRIPT_DIR / ".user-config.json"
BUILD_CONFIG_PATH = SCRIPT_DIR / "build-config.json"
NATIVE_BUILD_DIR = SCRIPT_DIR.parent / "build" / "native"
DEFINE_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(=.*)?")
OPTION_PATTERN = re.compile(r"[a-z0-9][a-z0-9-]*")
LUTE_EXECUTABLE = "lute.exe" if os.name == "nt" else "lute"
DEFAULT_LUTE_PATHS = (
    Path.home() / ".lute" / "bin" / LUTE_EXECUTABLE,  # `lute self install`
    Path.home() / ".local" / "bin" / LUTE_EXECUTABLE,
)


class UserConfigError(ValueError):
    pass


def resolve_lute():
    """Find the explicitly configured, PATH-installed, or conventional Lute binary."""
    configured = os.environ.get("LIBMP_LUTE")
    if configured:
        candidates = [Path(configured).expanduser()]
    else:
        discovered = shutil.which("lute")
        candidates = [Path(discovered)] if discovered else DEFAULT_LUTE_PATHS
    return next((path for path in candidates if path.is_file()), None)


def stage_luau_script(module, source, replacements=None, prefix="_libmp_"):
    """Stage a runner beside LibMP.luau so its relative require resolves consistently."""
    script = Path(source).read_text(encoding="utf-8")
    for old, new in (replacements or {}).items():
        script = script.replace(old, new)
    with NamedTemporaryFile(
        mode="w", encoding="utf-8", prefix=prefix, suffix=".luau",
        dir=Path(module).parent, delete=False,
    ) as staged_file:
        staged_file.write(script)
        return Path(staged_file.name)


def expand_file_patterns(patterns, directory_pattern="*"):
    """Resolve files, directories, and shell-style patterns in deterministic order."""
    files = []
    seen = set()
    for pattern in patterns:
        expanded = str(Path(pattern).expanduser())
        if glob.has_magic(expanded):
            candidates = [Path(path) for path in sorted(glob.glob(expanded, recursive=True))]
        else:
            path = Path(expanded)
            candidates = sorted(path.glob(directory_pattern)) if path.is_dir() else [path]
        matches = [path.resolve() for path in candidates if path.is_file()]
        if not matches:
            raise ValueError(f"input did not match a file: {pattern}")
        for path in matches:
            if path not in seen:
                seen.add(path)
                files.append(path)
    return files


def utc_timestamp():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def enabled_flag_arguments(args, *options):
    """Return the listed boolean CLI flags that are enabled in args."""
    return [
        option
        for option in options
        if getattr(args, option[2:].replace("-", "_"))
    ]


def _strip_json_comments(text: str) -> str:
    result = []
    index = 0
    in_string = False
    escaped = False

    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""

        if in_string:
            result.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
            continue

        if char == '"':
            in_string = True
            result.append(char)
            index += 1
        elif char == "/" and next_char == "/":
            index += 2
            while index < len(text) and text[index] not in "\r\n":
                index += 1
        elif char == "/" and next_char == "*":
            index += 2
            while index + 1 < len(text) and text[index:index + 2] != "*/":
                if text[index] in "\r\n":
                    result.append(text[index])
                index += 1
            if index + 1 >= len(text):
                raise UserConfigError("unterminated block comment")
            index += 2
        else:
            result.append(char)
            index += 1

    return "".join(result)


def _read_json_with_comments(path: Path) -> dict:
    try:
        contents = _strip_json_comments(path.read_text(encoding="utf-8"))
        result = json.loads(contents)
    except (OSError, json.JSONDecodeError, UserConfigError) as error:
        raise UserConfigError(f"failed to read {path}: {error}") from error
    if not isinstance(result, dict):
        raise UserConfigError(f"{path} must contain a JSON object")
    return result


def _merge_config(base: dict, overrides: dict) -> dict:
    result = copy.deepcopy(base)
    for key, value in overrides.items():
        if isinstance(result.get(key), dict) and isinstance(value, dict):
            result[key] = _merge_config(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def _validate_defines(defines, location: str):
    if not isinstance(defines, list):
        raise UserConfigError(f"'{location}' must be an array")
    for define in defines:
        if (
            not isinstance(define, str)
            or not DEFINE_PATTERN.fullmatch(define)
            or ";" in define
        ):
            raise UserConfigError(f"invalid compiler definition in '{location}': {define!r}")


def _validate_define_group(group, location: str, *, allow_description=False):
    if not isinstance(group, dict):
        raise UserConfigError(f"'{location}' must be an object")
    allowed_keys = {"set", "append"}
    if allow_description:
        allowed_keys.add("description")
    unknown_keys = set(group) - allowed_keys
    if unknown_keys:
        raise UserConfigError(f"unknown keys in '{location}': {', '.join(sorted(unknown_keys))}")
    if "description" in group and not isinstance(group["description"], str):
        raise UserConfigError(f"'{location}.description' must be a string")
    operations = [operation for operation in ("set", "append") if operation in group]
    if len(operations) > 1:
        raise UserConfigError(f"'{location}' must contain at most one of 'set' or 'append'")
    if operations:
        _validate_defines(group[operations[0]], f"{location}.{operations[0]}")


def _validate_user_config_layer(config: dict):
    if "emsdk_path" in config and not isinstance(config["emsdk_path"], str):
        raise UserConfigError("'emsdk_path' must be a string")

    defines = config.get("defines", {})
    if not isinstance(defines, dict):
        raise UserConfigError("'defines' must be an object")
    unknown_keys = set(defines) - {"always", "conditional"}
    if unknown_keys:
        raise UserConfigError(f"unknown keys in 'defines': {', '.join(sorted(unknown_keys))}")
    if "always" in defines:
        _validate_define_group(defines["always"], "defines.always")

    conditional = defines.get("conditional", {})
    if not isinstance(conditional, dict):
        raise UserConfigError("'defines.conditional' must be an object")
    for option, group in conditional.items():
        if not isinstance(option, str) or not OPTION_PATTERN.fullmatch(option):
            raise UserConfigError(f"invalid conditional command-line option: {option!r}")
        _validate_define_group(group, f"defines.conditional.{option}", allow_description=True)


def _apply_define_group(inherited, group):
    if "set" in group:
        return copy.deepcopy(group["set"])
    if "append" in group:
        return [*inherited, *copy.deepcopy(group["append"])]
    return list(inherited)


def _resolve_define_layers(config_layers):
    always = []
    conditional = {}
    conditional_descriptions = {}
    for config in config_layers:
        defines = config.get("defines", {})
        if "always" in defines:
            always = _apply_define_group(always, defines["always"])
        for option, group in defines.get("conditional", {}).items():
            conditional[option] = _apply_define_group(conditional.get(option, []), group)
            if "description" in group:
                conditional_descriptions[option] = group["description"]
    return {
        "always": always,
        "conditional": conditional,
        "conditional-descriptions": conditional_descriptions,
    }


def load_user_config(
    config_path: Path = USER_CONFIG_PATH,
    local_config_path: Path = LOCAL_USER_CONFIG_PATH,
) -> dict:
    if not config_path.is_file():
        raise UserConfigError(f"repository user config does not exist: {config_path}")

    repository_config = _read_json_with_comments(config_path)
    _validate_user_config_layer(repository_config)
    config_layers = [repository_config]

    if local_config_path.is_file():
        local_config = _read_json_with_comments(local_config_path)
        _validate_user_config_layer(local_config)
        config_layers.append(local_config)
    else:
        local_config = {}

    config = _merge_config(
        {key: value for key, value in repository_config.items() if key != "defines"},
        {key: value for key, value in local_config.items() if key != "defines"},
    )
    config["defines"] = _resolve_define_layers(config_layers)
    return config


def configured_emsdk_path(config: dict):
    """Returns the configured Emscripten SDK root, if one was provided."""
    value = os.environ.get("EMSDK_PATH", "") or os.environ.get("EMSDK", "")
    if not value:
        value = config.get("emsdk_path", "")
    return Path(value).expanduser() if value else None


def find_emsdk_tool(config: dict, relative_directory: str, name: str):
    """Finds a tool inside the configured Emscripten SDK, including Windows suffixes."""
    emsdk_path = configured_emsdk_path(config)
    if not emsdk_path:
        return None
    directory = emsdk_path / relative_directory
    for executable in (name, f"{name}.exe", f"{name}.bat"):
        candidate = directory / executable
        if candidate.is_file():
            return candidate
    return None


def load_build_config(*extra_paths: Path) -> dict:
    """Returns build-config.json deep-merged with each extra config file, in order."""
    config = _read_json_with_comments(BUILD_CONFIG_PATH)
    for path in extra_paths:
        config = _merge_config(config, _read_json_with_comments(path))
    return config


def resolve_repository_ref(config: dict, default="stable"):
    """Resolve a configured repository ref using the build scripts' precedence."""
    return (
        config.get("commit")
        or config.get("ref")
        or config.get("tag")
        or config.get("branch")
        or default
    )


def conditional_define_options(config: dict):
    return tuple(config.get("defines", {}).get("conditional", {}).keys())


def add_conditional_define_arguments(parser: argparse.ArgumentParser, config: dict):
    descriptions = config.get("defines", {}).get("conditional-descriptions", {})
    for option in conditional_define_options(config):
        parser.add_argument(
            f"--{option}",
            action="store_true",
            help=descriptions.get(option, f"Enable the '{option}' conditional compiler definitions"),
        )


def enabled_conditional_define_options(args, config: dict):
    return tuple(
        option
        for option in conditional_define_options(config)
        if getattr(args, option.replace("-", "_"), False)
    )


def resolve_compiler_defines(config: dict, enabled_options=()):
    defines_config = config.get("defines", {})
    defines = list(defines_config.get("always", []))
    conditional = defines_config.get("conditional", {})
    for option in enabled_options:
        if option not in conditional:
            raise UserConfigError(f"unknown conditional compiler-definition option: {option}")
        defines.extend(conditional[option])

    resolved = {}
    for define in defines:
        name = define.partition("=")[0]
        resolved[name] = define
    return list(resolved.values())


def override_compiler_define(defines, definition: str):
    """Returns definitions with any previous value of this definition replaced."""
    if not DEFINE_PATTERN.fullmatch(definition) or ";" in definition:
        raise UserConfigError(f"invalid compiler definition: {definition!r}")
    name = definition.partition("=")[0]
    result = [item for item in defines if item.partition("=")[0] != name]
    result.append(definition)
    return result


def compiler_define_value(defines, name: str, default=None):
    """Returns a definition's value, treating a bare -DNAME as -DNAME=1."""
    prefix = name + "="
    for define in reversed(defines):
        if define == name:
            return "1"
        if define.startswith(prefix):
            return define[len(prefix):]
    return default


def compiler_define_enabled(defines, name: str, default=False):
    """Returns the state of a boolean (bare, =0, or =1) compiler definition."""
    value = compiler_define_value(defines, name)
    if value is None:
        return default
    if value not in ("0", "1"):
        raise UserConfigError(f"compiler definition {name} must be bare, 0, or 1 (got {value!r})")
    return value == "1"


def find_built_file(build_dir, config, filename):
    """Finds a CMake output in a single- or multi-config build directory."""
    candidates = (build_dir / filename, build_dir / config / filename)
    return next((path for path in candidates if path.is_file()), None)


def find_built_executable(build_dir, config, filename):
    return (
        find_built_file(build_dir, config, filename)
        or find_built_file(build_dir, config, f"{filename}.exe")
    )


def ensure_parity_tool(config="Release"):
    """Returns the native parity_tool, building the native tests if it is absent."""
    parity_tool = find_built_executable(NATIVE_BUILD_DIR, config, "parity_tool")
    if parity_tool is None:
        command = [sys.executable, str(SCRIPT_DIR / "test_native.py"),
                   "capture_roundtrip_tests", "--config", config]
        print("  >", " ".join(command), flush=True)
        result = subprocess.run(command)
        if result.returncode:
            sys.exit(result.returncode)
        parity_tool = find_built_executable(NATIVE_BUILD_DIR, config, "parity_tool")
    if parity_tool is None:
        sys.exit(f"parity_tool was not built in {NATIVE_BUILD_DIR}")
    return parity_tool


def main():
    parser = argparse.ArgumentParser(description="Read the merged LibMP user configuration")
    parser.add_argument("--print-defines-cmake", action="store_true")
    parser.add_argument("--conditional", action="append", default=[])
    args = parser.parse_args()

    try:
        config = load_user_config()
        defines = resolve_compiler_defines(config, args.conditional)
    except UserConfigError as error:
        parser.error(str(error))

    if args.print_defines_cmake:
        print(";".join(defines))


def robust_rmtree(path: Path):
    """
    Removes a directory tree robustly on Windows, handling read-only files
    and temporary file locks from Antivirus/indexing services.
    """
    max_attempts = 3
    sleep_seconds = 1

    def remove_readonly(func, file_path, exc_info):
        # Clear the read-only bit and retry the deletion
        try:
            os.chmod(file_path, stat.S_IWRITE)
            func(file_path)
        except Exception:
            pass

    for attempt in range(max_attempts):
        try:
            if path.exists():
                shutil.rmtree(path, onerror=remove_readonly)
            return
        except PermissionError:
            if attempt < max_attempts - 1:
                time.sleep(sleep_seconds)
            else:
                print(
                    f"Warning: Could not remove {path} due to an active file lock. "
                    "You can delete it manually.",
                    file=sys.stderr,
                )
        except Exception as e:
            print(f"Warning: Failed to clean up {path}: {e}", file=sys.stderr)
            return


if __name__ == "__main__":
    main()

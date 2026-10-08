#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Parse the shared allocator-autonomy symbol policy."""

import argparse
import platform
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from _utils import SCRIPT_DIR, UserConfigError


AUTONOMY_SYMBOLS_PATH = SCRIPT_DIR / "autonomy-symbols.txt"


@dataclass(frozen=True)
class AutonomyRules:
    libc_memory_symbols: frozenset
    premain_allocator_tokens: tuple


def load_rules(system=None, path=None):
    path = AUTONOMY_SYMBOLS_PATH if path is None else Path(path)
    system = (system or platform.system()).lower()
    active_scopes = {"all", system}
    if system in ("darwin", "linux"):
        active_scopes.add("unix")
    valid_scopes = {"all", "unix", "linux", "darwin", "windows"}
    valid_kinds = {"direct", "premain"}
    direct_symbols = set()
    premain_tokens = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise UserConfigError(f"failed to read {path}: {error}") from error
    for line_number, line in enumerate(lines, 1):
        content = line.partition("#")[0].strip()
        if not content:
            continue
        fields = content.split(maxsplit=2)
        if (
            len(fields) != 3
            or fields[0] not in valid_kinds
            or fields[1] not in valid_scopes
        ):
            raise UserConfigError(f"invalid autonomy symbol at {path}:{line_number}")
        kind, scope, symbol = fields
        if kind == "direct" and not re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*", symbol
        ):
            raise UserConfigError(f"invalid direct symbol at {path}:{line_number}")
        if scope not in active_scopes:
            continue
        if kind == "direct":
            direct_symbols.add(symbol)
        elif symbol not in premain_tokens:
            premain_tokens.append(symbol)
    if not direct_symbols:
        raise UserConfigError(f"no direct allocator symbols selected from {path}")
    if not premain_tokens:
        raise UserConfigError(f"no pre-main allocator tokens selected from {path}")
    return AutonomyRules(
        libc_memory_symbols=frozenset(direct_symbols),
        premain_allocator_tokens=tuple(premain_tokens),
    )


def main():
    parser = argparse.ArgumentParser(
        description="Print direct allocation symbols selected for a target system"
    )
    parser.add_argument("--system", default=platform.system())
    args = parser.parse_args()
    try:
        rules = load_rules(args.system)
    except UserConfigError as error:
        print(error, file=sys.stderr)
        return 2
    print("\n".join(sorted(rules.libc_memory_symbols)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

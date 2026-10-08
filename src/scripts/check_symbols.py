#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Post-build allocator-autonomy symbol checks. Two independent gates, each an opt-in
flag; --strict enables both. Linux uses GNU binutils; Apple and Windows use the equivalent
LLVM tools for Mach-O/COFF. A missing tool is an error. Tunable symbol classifications live
in autonomy-symbols.txt.

  --strict-no-malloc  The library must not directly reference a libc allocation/deallocation
      API listed as `direct` in autonomy-symbols.txt. Such calls (e.g. from LZ4/xxhash) bypass
      ntd::mem, so a reference is an unambiguous leak. These C symbols are inlining-proof,
      so this gate is reliable at any optimization level. Tool: nm.

  --strict-no-premain-alloc  No allocator activity before main(): no function reachable from a
      static initializer references a libc memory-management function, global new/delete, or an
      ntd::mem allocation function. Call-graph reachability makes this reliable at any
      optimization level; its one blind spot is calls through function pointers / vtables,
      which static initializers rarely use. Registered initializers are read from
      ELF .preinit_array/.init_array/.ctors, Mach-O __mod_init_func, and COFF .CRT$X*.
      Tools: objdump + c++filt on Linux; llvm-objdump + llvm-nm on Apple/Windows.

The separate no-C++-stdlib test force-loads every archive member, omits the platform C++
standard library, and relies on no global operator override. Successful linking therefore
proves that the tested archive requires neither global new/delete nor a C++ standard-library
runtime.
For an embedded allocator with native pages on Linux, the no-malloc gate also builds a
force-linked executable with undefined --wrap targets for the same APIs. Calls hidden inside
shared libraries or resolved indirectly at runtime are outside these static guarantees.
"""
import argparse
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _autonomy import (  # noqa: E402
    AUTONOMY_GATES,
    NO_MALLOC_GATE,
    NO_PREMAIN_ALLOC_GATE,
)
from _autonomy_symbols import (  # noqa: E402
    AUTONOMY_SYMBOLS_PATH,
    load_rules as _load_rules,
)
from _utils import (  # noqa: E402
    UserConfigError,
    configured_emsdk_path,
    find_emsdk_tool,
    load_user_config,
)

# Allocating operator new/new[] (+ aligned/nothrow variants), but NOT the non-allocating
# placement form operator new(size_t, void*) used by ntd::New. Itanium mangles size_t as
# its underlying unsigned type: j (int), m (long), or y (long long), depending on the ABI.
_ITANIUM_SIZE_T = r"[jmy]"
_ITANIUM_ALLOCATING_NEW = re.compile(rf"^_Zn[wa]{_ITANIUM_SIZE_T}")
_ITANIUM_PLACEMENT_NEW = re.compile(rf"^_Zn[wa]{_ITANIUM_SIZE_T}Pv")
_ITANIUM_DEALLOCATING_DELETE = re.compile(r"^_Zd[la]")
_MSVC_ALLOCATING_NEW = ("??2@", "??_U@")
_MSVC_DEALLOCATING_DELETE = ("??3@", "??_V@")
_FUNC_HDR = re.compile(r"^[0-9a-fA-F]+ <(.+)>:$")
_INSTRUCTION = re.compile(r"^\s*[0-9a-fA-F]+:\s+([A-Za-z][A-Za-z0-9_.]*)\b")
_RELOC = re.compile(r"\s(?:R_\S+|IMAGE_REL_\S+|[A-Z0-9_]+_RELOC_\S+)\s+(\S+)")
_RELOCATION_ADDEND = re.compile(r"[-+]0x[0-9a-fA-F]+$")
_OBJ_HDR = re.compile(r"^(.+\.(?:o|obj)):$", re.IGNORECASE)
_NM_UNDEFINED_SYMBOL = re.compile(r"^\s+U (\S+)$")
# Static-initializer roots (run before main): GCC/Clang names plus MSVC's
# `dynamic initializer for ...` decorated prefix. Destruction roots are excluded.
_PREMAIN_ROOT = re.compile(
    r"_GLOBAL__sub_I_|static_initialization_and_destruction|__cxx_global_var_init|^\?\?__E"
)
_RELOCATION_SECTION = re.compile(r"^RELOCATION RECORDS FOR \[(.+)\]:$")
_ELF_INIT_SECTION = re.compile(r"^\.(?:preinit_array|init_array|ctors)(?:\.|$)")
_COFF_INIT_SECTION = re.compile(r"^\.CRT\$X[CI][A-Z]$")
_MACHO_INIT_SECTION = "__mod_init_func"
_MACHO_TEMPORARY_LABEL = re.compile(r"^ltmp[0-9]+$")
_ARM_BRANCH_CONDITIONS = {
    "al", "cc", "cs", "eq", "ge", "gt", "hi", "hs", "le", "lo", "ls",
    "lt", "mi", "ne", "pl", "vc", "vs",
}


@dataclass(frozen=True)
class BinaryTools:
    nm: str
    objdump: str
    demangle_with_nm: bool = False
    object_format: str = "elf"


class ToolError(RuntimeError):
    pass


def _llvm_tools_in_directory(directory):
    directory = Path(directory)
    tools = []
    for name in ("llvm-nm", "llvm-objdump"):
        tool = next(
            (directory / executable for executable in (name, f"{name}.exe")
             if (directory / executable).is_file()),
            None,
        )
        if not tool:
            return None
        tools.append(str(tool))
    return tuple(tools)


def _system_llvm_tools(use_xcrun=False):
    nm = shutil.which("llvm-nm") or shutil.which("llvm-nm.exe")
    objdump = shutil.which("llvm-objdump") or shutil.which("llvm-objdump.exe")
    if nm and objdump:
        return nm, objdump

    if use_xcrun and shutil.which("xcrun"):
        try:
            return (
                _run_tool(["xcrun", "--find", "llvm-nm"]).strip(),
                _run_tool(["xcrun", "--find", "llvm-objdump"]).strip(),
            )
        except ToolError:
            pass

    compiler_names = ["clang-cl", "clang++", "clang"]
    compiler_names.extend(f"clang-{version}" for version in range(30, 9, -1))
    for compiler in compiler_names:
        compiler_path = shutil.which(compiler)
        if not compiler_path:
            continue
        tools = _llvm_tools_in_directory(Path(compiler_path).resolve().parent)
        if tools:
            return tools

    config_names = ["llvm-config"]
    config_names.extend(f"llvm-config-{version}" for version in range(30, 9, -1))
    for config_name in config_names:
        llvm_config = shutil.which(config_name)
        if not llvm_config:
            continue
        try:
            tools = _llvm_tools_in_directory(_run_tool([llvm_config, "--bindir"]).strip())
        except ToolError:
            continue
        if tools:
            return tools
    return None


def _binary_tool_candidates(system=None):
    system = system or platform.system()
    candidates = []
    try:
        config = load_user_config()
    except UserConfigError as error:
        raise ToolError(f"could not resolve the configured Emscripten SDK: {error}") from error
    emsdk_path = configured_emsdk_path(config)
    if emsdk_path:
        nm = find_emsdk_tool(config, "upstream/bin", "llvm-nm")
        objdump = find_emsdk_tool(config, "upstream/bin", "llvm-objdump")
        if nm and objdump:
            candidates.append(BinaryTools(
                nm=str(nm),
                objdump=str(objdump),
                demangle_with_nm=True,
                object_format={"Darwin": "macho", "Windows": "coff"}.get(system, "elf"),
            ))
    system_llvm = _system_llvm_tools(use_xcrun=system == "Darwin")
    if system_llvm:
        object_format = {"Darwin": "macho", "Windows": "coff"}.get(system, "elf")
        candidates.append(BinaryTools(
            nm=system_llvm[0],
            objdump=system_llvm[1],
            demangle_with_nm=True,
            object_format=object_format,
        ))
    if system == "Linux":
        candidates.append(BinaryTools(nm="nm", objdump="objdump"))
    elif not candidates:
        candidates.append(BinaryTools(
            nm="llvm-nm",
            objdump="llvm-objdump",
            demangle_with_nm=True,
            object_format="macho" if system == "Darwin" else "coff",
        ))

    unique = []
    for candidate in candidates:
        if not any(
            existing.nm == candidate.nm and existing.objdump == candidate.objdump
            for existing in unique
        ):
            unique.append(candidate)
    return tuple(unique)


def _binary_tools(system=None):
    return _binary_tool_candidates(system)[0]


def _run_tool(cmd, stdin=None):
    """Runs a binary-analysis tool and returns its stdout."""
    try:
        result = subprocess.run(cmd, input=stdin, capture_output=True, text=True)
    except FileNotFoundError as error:
        raise ToolError(f"required tool '{cmd[0]}' was not found") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit code {result.returncode}"
        raise ToolError(f"tool '{cmd[0]}' failed: {detail}")
    return result.stdout


def _strip_relocation_addend(symbol):
    return _RELOCATION_ADDEND.sub("", symbol)


def _strip_coff_import_prefix(symbol):
    return symbol[6:] if symbol.startswith("__imp_") else symbol


def _canonical_cpp_symbol(symbol):
    symbol = _strip_coff_import_prefix(symbol)
    # Mach-O adds one ABI-level underscore to external C++ symbols.
    return symbol[1:] if symbol.startswith("__Z") else symbol


def _canonical_c_symbol(symbol):
    symbol = _strip_coff_import_prefix(_strip_relocation_addend(symbol))
    if symbol.startswith("_") and not symbol.startswith("_Z"):
        symbol = symbol[1:]
    return symbol


def _is_direct_control_transfer(mnemonic):
    """Whether an instruction relocation denotes a reachable call or tail branch."""
    mnemonic = mnemonic.lower()
    if mnemonic.startswith("call") or mnemonic.startswith("j"):
        return True
    if mnemonic in {
        "b", "ba", "bl", "bla", "blx", "bx", "cbnz", "cbz", "jal", "jalr",
        "tail", "tbnz", "tbz", "bras", "brasl",
    }:
        return True
    if mnemonic.startswith("b."):
        return True
    arm_mnemonic = mnemonic.removesuffix(".w")
    if arm_mnemonic in {"b", "bl", "blx", "bx"}:
        return True
    return arm_mnemonic.startswith("b") and arm_mnemonic[1:] in _ARM_BRANCH_CONDITIONS


def _is_allocating_operator_new(symbol, demangled=None):
    symbol = _canonical_cpp_symbol(symbol)
    if bool(_ITANIUM_ALLOCATING_NEW.match(symbol)):
        return not _ITANIUM_PLACEMENT_NEW.match(symbol)
    if symbol.startswith(_MSVC_ALLOCATING_NEW):
        # Standard placement new is normally inlined. Keep the distinction for
        # unusual objects that retain an out-of-line reference.
        if demangled and re.search(r"operator new(?:\[\])?\([^,]+,\s*void \*\)", demangled):
            return False
        return True
    return False


def _is_deallocating_operator_delete(symbol):
    symbol = _canonical_cpp_symbol(symbol)
    return bool(_ITANIUM_DEALLOCATING_DELETE.match(symbol)) or symbol.startswith(
        _MSVC_DEALLOCATING_DELETE
    )


def _nm_symbol_names(output):
    """Returns symbols in nm output order, preserving demangled names with spaces."""
    names = []
    for line in output.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) >= 2 and len(fields[0]) == 1:
            names.append(line.strip().split(None, 1)[1])
        elif len(fields) == 3 and len(fields[1]) == 1:
            names.append(fields[2])
    return names


def _llvm_nm_demangle(lib, nm):
    raw_names = _nm_symbol_names(_run_tool([nm, str(lib)]))
    demangled_names = _nm_symbol_names(_run_tool([nm, "--demangle", str(lib)]))
    if len(raw_names) != len(demangled_names):
        raise ToolError("llvm-nm produced inconsistent raw and demangled symbol listings")
    return dict(zip(raw_names, demangled_names))


def _registered_premain_roots(lib, tools):
    """Reads function pointers registered in platform initializer sections."""
    output = _run_tool([tools.objdump, "-r", str(lib)])
    roots = set()
    in_initializer_section = False
    for line in output.splitlines():
        section = _RELOCATION_SECTION.match(line)
        if section:
            section_name = section.group(1)
            in_initializer_section = (
                tools.object_format == "elf" and bool(_ELF_INIT_SECTION.match(section_name))
            ) or (
                tools.object_format == "coff" and bool(_COFF_INIT_SECTION.match(section_name))
            ) or (
                tools.object_format == "macho" and section_name == _MACHO_INIT_SECTION
            )
            continue
        if in_initializer_section:
            relocation = _RELOC.search(line)
            if relocation:
                roots.add(_strip_relocation_addend(relocation.group(1)))
    return roots


def _demangle(symbols, lib=None, tools=None):
    if not symbols:
        return {}
    tools = tools or _binary_tools()
    if tools.demangle_with_nm:
        if lib is None:
            raise ToolError("COFF demangling requires the input library")
        names = _llvm_nm_demangle(lib, tools.nm)
        return {symbol: names.get(symbol, symbol) for symbol in symbols}
    ordered = sorted(symbols)
    output = _run_tool(["c++filt"], stdin="\n".join(ordered))
    return dict(zip(ordered, output.splitlines()))


def _libc_memory_refs(lib, forbidden_symbols, tools=None):
    """Returns (object, symbol) pairs for undefined libc-memory references."""
    tools = tools or _binary_tools()
    out = _run_tool([tools.nm, str(lib)])
    cur, refs = None, []
    for line in out.splitlines():
        m = _OBJ_HDR.match(line)
        if m:
            cur = m.group(1)
            continue
        m = _NM_UNDEFINED_SYMBOL.match(line)
        if m and cur and _canonical_c_symbol(m.group(1)) in forbidden_symbols:
            refs.append((cur, m.group(1)))
    return refs


def _premain_offenders(lib, libc_memory_symbols, allocator_tokens, tools=None):
    """Return allocator references reachable from registered static initializers."""
    tools = tools or _binary_tools()
    objdump_command = [tools.objdump, "-dr", "--no-show-raw-insn"]
    if tools.object_format == "macho":
        objdump_command.append("--show-all-symbols")
    objdump_command.append(str(lib))
    out = _run_tool(objdump_command)
    funcs = {}  # mangled fn -> set(mangled referenced symbols)
    cur = None
    instruction = None
    for line in out.splitlines():
        m = _FUNC_HDR.match(line)
        if m:
            function = m.group(1)
            if tools.object_format == "macho" and _MACHO_TEMPORARY_LABEL.match(function):
                continue
            cur = function
            funcs.setdefault(cur, set())
            instruction = None
            continue
        m = _RELOC.search(line)
        if m and cur is not None:
            # Data-address relocations are not call-graph edges: static
            # initializers commonly take destructor addresses for __cxa_atexit.
            if instruction and _is_direct_control_transfer(instruction):
                funcs[cur].add(_strip_relocation_addend(m.group(1)))
            instruction = None
            continue
        m = _INSTRUCTION.match(line)
        if m:
            instruction = m.group(1)

    names = set(funcs) | {s for refs in funcs.values() for s in refs}
    dem = _demangle(names, lib, tools)

    def is_allocator_activity(sym):
        d = dem.get(sym, sym)
        return (
            _canonical_c_symbol(sym) in libc_memory_symbols
            or _is_allocating_operator_new(sym, d)
            or _is_deallocating_operator_delete(sym)
            or any(token in d for token in allocator_tokens)
        )

    registered_roots = _registered_premain_roots(lib, tools)
    roots = [
        function
        for function in funcs
        if function in registered_roots
        or _PREMAIN_ROOT.search(function)
        or _PREMAIN_ROOT.search(dem.get(function, ""))
    ]
    seen, stack, offenders = set(), list(roots), set()
    while stack:
        f = stack.pop()
        if f in seen:
            continue
        seen.add(f)
        for s in funcs.get(f, ()):
            if is_allocator_activity(s):
                offenders.add((dem.get(f, f), dem.get(s, s)))
            elif s in funcs and s not in seen:
                stack.append(s)
    return sorted(offenders)


def _check_no_malloc(lib, rules, tools=None):
    refs = _libc_memory_refs(lib, rules.libc_memory_symbols, tools)
    if refs:
        sys.stderr.write("check_symbols: FAIL (no-malloc) -- references a direct libc allocation API\n"
                         "  All library and dependency allocation must route through ntd::mem:\n")
        for object_name, symbol in sorted(refs):
            sys.stderr.write(f"    {object_name}: {symbol}\n")
        return False
    print("check_symbols: OK (no-malloc) -- no direct libc allocation APIs referenced")
    return True


def _check_no_premain_alloc(lib, rules, tools=None):
    offenders = _premain_offenders(
        lib,
        rules.libc_memory_symbols,
        rules.premain_allocator_tokens,
        tools,
    )
    if offenders:
        sys.stderr.write("check_symbols: FAIL (no-premain-alloc) -- static initializers use an allocator before main()\n"
                         "  (keep namespace-scope init BSS-only). Reachable allocator operation:\n")
        for function, symbol in offenders:
            sys.stderr.write(f"    {function}  ->  {symbol}\n")
        return False
    print("check_symbols: OK (no-premain-alloc) -- no allocator activity reachable from static initializers")
    return True


GATE_CHECKS = {
    NO_MALLOC_GATE: _check_no_malloc,
    NO_PREMAIN_ALLOC_GATE: _check_no_premain_alloc,
}


def main():
    parser = argparse.ArgumentParser(
        description="Allocator-autonomy checks on a built static library",
        allow_abbrev=False,
    )
    parser.add_argument("lib", help="path to libmp.a")
    for gate in AUTONOMY_GATES:
        parser.add_argument(gate.option, action="store_true", help=gate.help)
    parser.add_argument("--strict", action="store_true", help="enable both symbol gates")
    args = parser.parse_args()

    lib = Path(args.lib)
    if not lib.exists():
        print(f"check_symbols: library not found: {lib}", file=sys.stderr)
        return 2

    if args.strict:
        selected_gates = AUTONOMY_GATES
    else:
        selected_gates = tuple(gate for gate in AUTONOMY_GATES if getattr(args, gate.argparse_dest))
    if not selected_gates:
        selected_gates = (NO_MALLOC_GATE,)  # standalone default

    try:
        rules = _load_rules()
    except UserConfigError as error:
        print(f"check_symbols: ERROR -- invalid autonomy-check configuration: {error}", file=sys.stderr)
        return 2

    try:
        tool_candidates = _binary_tool_candidates()
    except ToolError as error:
        print(f"check_symbols: ERROR -- {error}", file=sys.stderr)
        return 2

    results = []
    for gate in selected_gates:
        tool_errors = []
        for tools in tool_candidates:
            try:
                results.append(GATE_CHECKS[gate](lib, rules, tools))
                break
            except ToolError as error:
                tool_errors.append(str(error))
        else:
            print(
                f"check_symbols: ERROR ({gate.name}) -- " + " | ".join(tool_errors),
                file=sys.stderr,
            )
            return 2

    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

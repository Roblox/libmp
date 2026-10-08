#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Focused parser tests for check_symbols.py's ELF and COFF readers."""

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_symbols as checks  # noqa: E402
from _utils import UserConfigError  # noqa: E402


class CheckSymbolsParserTests(unittest.TestCase):
    def test_all_platforms_prefer_llvm_tools_from_configured_emsdk(self):
        with TemporaryDirectory() as directory:
            fixture_emsdk_root = Path(directory)
            # Mirror Emscripten's real tool location with empty fixture files;
            # this test exercises path selection and never executes LLVM.
            fixture_llvm_bin = fixture_emsdk_root / "upstream" / "bin"
            fixture_llvm_bin.mkdir(parents=True)
            (fixture_llvm_bin / "llvm-nm.exe").touch()
            (fixture_llvm_bin / "llvm-objdump.exe").touch()
            with patch.object(checks.shutil, "which", return_value=None):
                with patch.object(
                    checks,
                    "load_user_config",
                    return_value={"emsdk_path": str(fixture_emsdk_root)},
                ):
                    candidates = checks._binary_tool_candidates("Linux")
                    tools = candidates[0]
            self.assertEqual(tools.nm, str(fixture_llvm_bin / "llvm-nm.exe"))
            self.assertEqual(
                tools.objdump, str(fixture_llvm_bin / "llvm-objdump.exe")
            )
            self.assertTrue(tools.demangle_with_nm)
            self.assertEqual(tools.object_format, "elf")
            self.assertEqual(candidates[1], checks.BinaryTools("nm", "objdump"))

    def test_linux_rules_include_common_unix_and_premain_entries(self):
        # Select Linux explicitly so this test covers both common and Unix rules
        # on every host. Windows selection is covered independently below.
        rules = checks._load_rules("Linux")
        self.assertIn("free", rules.libc_memory_symbols)
        self.assertIn("strdup", rules.libc_memory_symbols)
        self.assertIn("posix_memalign", rules.libc_memory_symbols)
        self.assertIn("ntd::mem::Free", rules.premain_allocator_tokens)

    def test_autonomy_symbol_manifest_is_platform_aware(self):
        linux = checks._load_rules("Linux").libc_memory_symbols
        darwin = checks._load_rules("Darwin").libc_memory_symbols
        windows = checks._load_rules("Windows").libc_memory_symbols
        self.assertIn("memalign", linux)
        self.assertNotIn("malloc_zone_malloc", linux)
        self.assertIn("malloc_zone_malloc", darwin)
        self.assertNotIn("aligned_malloc", darwin)
        self.assertIn("malloc", windows)
        self.assertIn("free", windows)
        self.assertIn("aligned_malloc", windows)
        self.assertNotIn("posix_memalign", windows)

    def test_missing_rules_fail_closed(self):
        with TemporaryDirectory() as directory:
            with self.assertRaises(UserConfigError):
                checks._load_rules(
                    path=Path(directory) / "fixture_missing_symbols.txt"
                )

    def test_invalid_rules_fail_closed(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "fixture_invalid_symbols.txt"
            path.write_text(
                "direct unsupported malloc\npremain all ntd::mem::Alloc\n",
                encoding="utf-8",
            )
            with self.assertRaises(UserConfigError):
                checks._load_rules("Linux", path)

    def test_libc_memory_references_keep_archive_member_names(self):
        nm_output = """
allocator.cpp.o:
                 U malloc
                 U free
                 U mmap

other.cpp.o:
                 U calloc
"""
        tools = checks.BinaryTools("nm", "objdump")
        with patch.object(checks, "_run_tool", return_value=nm_output):
            refs = checks._libc_memory_refs(
                "libmp.a", {"malloc", "calloc", "realloc", "free"}, tools
            )
        self.assertEqual(
            refs,
            [
                ("allocator.cpp.o", "malloc"),
                ("allocator.cpp.o", "free"),
                ("other.cpp.o", "calloc"),
            ],
        )

    def test_premain_operator_new_recognizes_common_size_t_types(self):
        for size_type in "jmy":
            with self.subTest(size_type=size_type):
                self.assertTrue(checks._is_allocating_operator_new(f"_Znw{size_type}"))
                self.assertTrue(checks._is_allocating_operator_new(f"_Zna{size_type}"))
                self.assertFalse(checks._is_allocating_operator_new(f"_Znw{size_type}Pv"))

        self.assertTrue(
            checks._is_allocating_operator_new(
                "??2@YAPEAX_K@Z",
                "void * __cdecl operator new(unsigned __int64)",
            )
        )
        self.assertFalse(
            checks._is_allocating_operator_new(
                "??2@YAPEAX_KPEAX@Z",
                "void * __cdecl operator new(unsigned __int64, void *)",
            )
        )

    def test_premain_operator_delete_recognizes_itanium_and_msvc(self):
        self.assertTrue(checks._is_deallocating_operator_delete("_ZdlPv"))
        self.assertTrue(checks._is_deallocating_operator_delete("_ZdaPvm"))
        self.assertTrue(checks._is_deallocating_operator_delete("??3@YAXPEAX_K@Z"))
        self.assertTrue(checks._is_deallocating_operator_delete("__imp_??_V@YAXPEAX@Z"))

    def test_coff_libc_names_are_normalized(self):
        nm_output = """
allocator.obj:
                 U __imp_malloc
                 U __imp__calloc
                 U _realloc
                 U free
"""
        tools = checks.BinaryTools(
            "llvm-nm", "llvm-objdump", demangle_with_nm=True, object_format="coff"
        )
        with patch.object(checks, "_run_tool", return_value=nm_output):
            refs = checks._libc_memory_refs(
                "mp.lib", {"malloc", "calloc", "realloc", "free"}, tools
            )
        self.assertEqual(
            refs,
            [
                ("allocator.obj", "__imp_malloc"),
                ("allocator.obj", "__imp__calloc"),
                ("allocator.obj", "_realloc"),
                ("allocator.obj", "free"),
            ],
        )

    def test_llvm_nm_demangles_coff_symbols(self):
        raw = """
file.obj:
00000000 T ?Init@@YAXXZ
         U ?Alloc@mem@ntd@@YAPEAX_K@Z
"""
        demangled = """
file.obj:
00000000 T void __cdecl Init(void)
         U void * __cdecl ntd::mem::Alloc(unsigned __int64)
"""
        with patch.object(checks, "_run_tool", side_effect=[raw, demangled]):
            names = checks._llvm_nm_demangle("mp.lib", "llvm-nm")
        self.assertEqual(names["?Init@@YAXXZ"], "void __cdecl Init(void)")
        self.assertIn("ntd::mem::Alloc", names["?Alloc@mem@ntd@@YAPEAX_K@Z"])

    def test_demangling_tool_failure_is_not_hidden(self):
        with patch.object(checks, "_run_tool", side_effect=checks.ToolError("c++filt failed")):
            with self.assertRaises(checks.ToolError):
                checks._demangle({"_Z8allocatev"}, tools=checks.BinaryTools("nm", "objdump"))

    def test_control_transfer_mnemonics_cover_x86_and_arm(self):
        for mnemonic in ("call", "callq", "jmp", "jne", "bl", "bl.w", "b.eq", "bne"):
            with self.subTest(mnemonic=mnemonic):
                self.assertTrue(checks._is_direct_control_transfer(mnemonic))
        for mnemonic in ("lea", "leaq", "adrp", "mov"):
            with self.subTest(mnemonic=mnemonic):
                self.assertFalse(checks._is_direct_control_transfer(mnemonic))

    def test_premain_scan_follows_calls_to_allocator(self):
        objdump_output = """
0000000000000000 <_GLOBAL__sub_I_state>:
   0: callq 5 <_GLOBAL__sub_I_state+0x5>
   1: R_X86_64_PLT32 _Z4initv-0x4
0000000000000010 <_Z4initv>:
  10: callq 15 <_Z4initv+0x5>
  11: R_X86_64_PLT32 _ZN3ntd3mem5AllocEm-0x4
"""

        def tool_output(command, stdin=None):
            if command[0] == "objdump":
                return objdump_output
            if command[0] == "c++filt":
                names = stdin.splitlines()
                demangled = {
                    "_GLOBAL__sub_I_state": "_GLOBAL__sub_I_state",
                    "_Z4initv": "init()",
                    "_ZN3ntd3mem5AllocEm": "ntd::mem::Alloc(unsigned long)",
                }
                return "\n".join(demangled[name] for name in names)
            self.fail(f"unexpected tool: {command}")

        with patch.object(checks, "_run_tool", side_effect=tool_output):
            offenders = checks._premain_offenders(
                "libmp.a",
                {"malloc", "calloc", "realloc", "free"},
                ("ntd::mem::Alloc", "ntd::mem::Realloc", "ntd::mem::Free"),
                checks.BinaryTools("nm", "objdump"),
            )
        self.assertEqual(offenders, [("init()", "ntd::mem::Alloc(unsigned long)")])

    def test_premain_scan_ignores_address_taken_for_exit_destructor(self):
        objdump_output = """
0000000000000000 <_GLOBAL__sub_I_state>:
   0: leaq 0x0(%rip),%rdi
   3: R_X86_64_PC32 _Z7destroyv-0x4
   7: callq c <_GLOBAL__sub_I_state+0xc>
   8: R_X86_64_PLT32 __cxa_atexit-0x4
0000000000000010 <_Z7destroyv>:
  10: callq 15 <_Z7destroyv+0x5>
  11: R_X86_64_PLT32 _ZN3ntd3mem4FreeEPv-0x4
"""

        def tool_output(command, stdin=None):
            if command[0] == "objdump":
                return objdump_output
            if command[0] == "c++filt":
                names = stdin.splitlines()
                demangled = {
                    "_GLOBAL__sub_I_state": "_GLOBAL__sub_I_state",
                    "_Z7destroyv": "destroy()",
                    "_ZN3ntd3mem4FreeEPv": "ntd::mem::Free(void*)",
                    "__cxa_atexit": "__cxa_atexit",
                }
                return "\n".join(demangled[name] for name in names)
            self.fail(f"unexpected tool: {command}")

        with patch.object(checks, "_run_tool", side_effect=tool_output):
            offenders = checks._premain_offenders(
                "libmp.a",
                {"malloc", "calloc", "realloc", "free"},
                ("ntd::mem::Alloc", "ntd::mem::Realloc", "ntd::mem::Free"),
                checks.BinaryTools("nm", "objdump"),
            )
        self.assertEqual(offenders, [])

    def test_elf_premain_scan_uses_registered_initializer(self):
        disassembly = """
0000000000000000 <_Z7startupv>:
   0: callq 5 <_Z7startupv+0x5>
   1: R_X86_64_PLT32 _Z4initv-0x4
0000000000000010 <_Z4initv>:
  10: callq 15 <_Z4initv+0x5>
  11: R_X86_64_PLT32 malloc-0x4
"""
        relocations = """
RELOCATION RECORDS FOR [.init_array]:
0000000000000000 R_X86_64_64 _Z7startupv
"""

        def tool_output(command, stdin=None):
            if command[:2] == ["objdump", "-r"]:
                return relocations
            if command[0] == "objdump":
                return disassembly
            if command[0] == "c++filt":
                names = stdin.splitlines()
                demangled = {
                    "_Z7startupv": "startup()",
                    "_Z4initv": "init()",
                    "malloc": "malloc",
                }
                return "\n".join(demangled[name] for name in names)
            self.fail(f"unexpected tool: {command}")

        with patch.object(checks, "_run_tool", side_effect=tool_output):
            offenders = checks._premain_offenders(
                "libmp.a",
                {"malloc", "calloc", "realloc", "free"},
                ("ntd::mem::Alloc", "ntd::mem::Realloc", "ntd::mem::Free"),
                checks.BinaryTools("nm", "objdump"),
            )
        self.assertEqual(offenders, [("init()", "malloc")])

    def test_coff_premain_scan_follows_dynamic_initializer(self):
        objdump_output = """
0000000000000000 <?Startup@@YAXXZ>:
   0: callq 5 <?Startup@@YAXXZ+0x5>
   1: IMAGE_REL_AMD64_REL32 ?Init@@YAXXZ
0000000000000010 <?Init@@YAXXZ>:
  10: callq 15 <?Init@@YAXXZ+0x5>
  11: IMAGE_REL_AMD64_REL32 __imp_free
"""
        nm_raw = """
state.obj:
00000000 t ?Startup@@YAXXZ
00000010 T ?Init@@YAXXZ
         U __imp_free
"""
        nm_demangled = """
state.obj:
00000000 t void __cdecl Startup(void)
00000010 T void __cdecl Init(void)
         U __imp_free
"""
        relocation_output = """
RELOCATION RECORDS FOR [.CRT$XCU]:
0000000000000000 IMAGE_REL_AMD64_ADDR64 ?Startup@@YAXXZ
"""

        def tool_output(command, stdin=None):
            if command[:2] == ["llvm-objdump", "-r"]:
                return relocation_output
            if command[0] == "llvm-objdump":
                return objdump_output
            if command[:2] == ["llvm-nm", "--demangle"]:
                return nm_demangled
            if command[0] == "llvm-nm":
                return nm_raw
            self.fail(f"unexpected tool: {command}")

        tools = checks.BinaryTools(
            "llvm-nm", "llvm-objdump", demangle_with_nm=True, object_format="coff"
        )
        with patch.object(checks, "_run_tool", side_effect=tool_output):
            offenders = checks._premain_offenders(
                "mp.lib",
                {"malloc", "calloc", "realloc", "free"},
                ("ntd::mem::Alloc", "ntd::mem::Realloc", "ntd::mem::Free"),
                tools,
            )
        self.assertEqual(offenders, [("void __cdecl Init(void)", "__imp_free")])

    def test_macho_premain_scan_uses_registered_initializer_and_symbol_alias(self):
        objdump_output = """
0000000000000000 <__Z7Startupv>:
0000000000000000 <ltmp0>:
   0: bl 0x10 <ltmp0+0x10>
   0: ARM64_RELOC_BRANCH26 __Z4Initv
0000000000000010 <__Z4Initv>:
  10: bl 0x20 <__Z4Initv+0x10>
  10: ARM64_RELOC_BRANCH26 _free
"""
        nm_raw = """
state.o:
00000000 T __Z7Startupv
00000010 T __Z4Initv
         U _free
"""
        nm_demangled = """
state.o:
00000000 T Startup()
00000010 T Init()
         U _free
"""
        relocation_output = """
RELOCATION RECORDS FOR [__mod_init_func]:
0000000000000000 ARM64_RELOC_UNSIGNED __Z7Startupv
"""

        def tool_output(command, stdin=None):
            if command[:2] == ["llvm-objdump", "-r"]:
                return relocation_output
            if command[0] == "llvm-objdump":
                return objdump_output
            if command[:2] == ["llvm-nm", "--demangle"]:
                return nm_demangled
            if command[0] == "llvm-nm":
                return nm_raw
            self.fail(f"unexpected tool: {command}")

        tools = checks.BinaryTools(
            "llvm-nm", "llvm-objdump", demangle_with_nm=True, object_format="macho"
        )
        with patch.object(checks, "_run_tool", side_effect=tool_output):
            offenders = checks._premain_offenders(
                "libmp.a",
                {"malloc", "calloc", "realloc", "free"},
                ("ntd::mem::Alloc", "ntd::mem::Realloc", "ntd::mem::Free"),
                tools,
            )
        self.assertEqual(offenders, [("Init()", "_free")])


if __name__ == "__main__":
    unittest.main()

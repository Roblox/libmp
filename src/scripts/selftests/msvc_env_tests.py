#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Focused tests for MSVC target and vcvarsall architecture selection."""

import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import msvc_env  # noqa: E402


class MsvcEnvironmentTests(unittest.TestCase):
    def test_native_host_architecture_is_normalized(self):
        with patch.object(msvc_env.platform, "machine", return_value="AMD64"):
            self.assertEqual(msvc_env.target_architecture(), "x64")
            self.assertEqual(msvc_env.vcvars_architecture(), "x64")

    def test_compiler_path_identifies_cross_target(self):
        compiler = "/vs/VC/Tools/MSVC/1/bin/Hostx64/arm64/cl.exe"
        self.assertEqual(msvc_env.target_architecture(compiler), "arm64")
        self.assertEqual(msvc_env.vcvars_architecture(compiler), "x64_arm64")

    def test_explicit_cmake_platform_overrides_compiler_target(self):
        compiler = "/vs/VC/Tools/MSVC/1/bin/Hostx64/x64/cl.exe"
        self.assertEqual(msvc_env.target_architecture(compiler, "Win32"), "x86")
        self.assertEqual(msvc_env.vcvars_architecture(compiler, "Win32"), "x64_x86")
        self.assertEqual(
            msvc_env.target_architecture(compiler, "ARM64,version=10.0"),
            "arm64",
        )

    def test_unknown_architecture_is_not_silently_treated_as_x64(self):
        with patch.object(msvc_env.platform, "machine", return_value="mips"):
            with self.assertRaises(ValueError):
                msvc_env.target_architecture()


if __name__ == "__main__":
    unittest.main()

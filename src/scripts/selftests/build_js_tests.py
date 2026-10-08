#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Regression tests for JavaScript bundle metadata."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import build_js  # noqa: E402
from _build_metadata import read_libmp_versions  # noqa: E402


class BuildJsTests(unittest.TestCase):
    def test_renders_source_header_for_bundle(self):
        root = Path(__file__).resolve().parents[2]
        version_header = root / "mp_api" / "control_channel.h"
        versions = read_libmp_versions(version_header)
        header = build_js.render_bundle_header(
            root / "mp_api" / "langs" / "js" / "libmp_api.js",
            "2000-01-02 03:04:05 UTC",
        )

        self.assertTrue(header.startswith("/*!\n"))
        self.assertIn("Build timestamp: 2000-01-02 03:04:05 UTC", header)
        self.assertIn(f"LibMP library version: {versions['LIBRARY']}", header)
        self.assertIn(
            f"LibMP data-format version: {versions['DATA_FORMAT']}", header
        )
        self.assertIn("LibMP is distributed under the MIT License", header)
        self.assertIn("FlatBuffers (Apache License 2.0)", header)
        self.assertIn("LZ4 (BSD 2-Clause License)", header)
        self.assertNotIn("%LIBMP_SHARED_METADATA%", header)


if __name__ == "__main__":
    unittest.main()

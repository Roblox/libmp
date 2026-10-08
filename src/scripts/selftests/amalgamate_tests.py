#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

"""Regression tests for the LibMP amalgamation generator."""

import re
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import amalgamate  # noqa: E402


FLATBUFFER_SCHEMA_MARKER = (
    "// BEGIN mp_api/flatbuffer_types/mp_types_generated.inc"
)
SOURCE_ROOT = amalgamate.PROJECT_ROOT
C_API_HEADER = SOURCE_ROOT / "mp_api" / "c_api.h"
FIXTURE_GENERATION_TIMESTAMP = "2000-01-02 03:04:05 UTC"


class AmalgamationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.default_output_directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.default_output_directory.cleanup)
        cls.catalog = amalgamate.SourceCatalog()
        cls.default_header, cls.default_source = amalgamate.generate(
            Path(cls.default_output_directory.name),
            catalog=cls.catalog,
            generation_timestamp=FIXTURE_GENERATION_TIMESTAMP,
        )
        cls.default_header_text = cls.default_header.read_text(encoding="utf-8")
        cls.default_source_text = cls.default_source.read_text(encoding="utf-8")

    def assert_no_resolvable_project_includes(self, *outputs):
        """Verify that only the intentional libmp.cpp -> libmp.h include remains."""
        leftovers = []
        for output in outputs:
            for line_number, line in enumerate(
                output.read_text(encoding="utf-8").splitlines(), 1
            ):
                include = amalgamate.parse_include(line, output, line_number)
                if include is None or include[0] == "libmp.h":
                    continue
                resolved = amalgamate.resolve_local_include(output, *include)
                if resolved is not None:
                    leftovers.append((output.name, line_number, include[0]))
        self.assertEqual(leftovers, [])

    def assert_configuration_state(self, output, label, retained):
        expected_state = "retained" if retained else "omitted"
        unexpected_state = "omitted" if retained else "retained"
        self.assertIn(
            f"// Configuration body {expected_state}: {label}={int(retained)}",
            output,
        )
        self.assertNotIn(
            f"// Configuration body {unexpected_state}: {label}={int(not retained)}",
            output,
        )

    @staticmethod
    def expanded_paths(output):
        return set(re.findall(r"^// BEGIN (.+)$", output, re.MULTILINE))

    def dependency_prefix(self, name):
        root = self.catalog.dependencies[name].root
        return f"{amalgamate.relative_path(root).as_posix()}/"

    def assert_dependency_body_state(self, output, name, retained):
        prefix = self.dependency_prefix(name)
        self.assertEqual(
            any(path.startswith(prefix) for path in self.expanded_paths(output)),
            retained,
        )

    def test_include_parser_accepts_both_literal_forms(self):
        self.assertEqual(
            amalgamate.parse_include(
                '#include "mp_api/cpp_api.h"\n', C_API_HEADER, 1
            ),
            ("mp_api/cpp_api.h", True),
        )
        self.assertEqual(
            amalgamate.parse_include(
                "  # include <mp_api/cpp_api.h> // public\n", C_API_HEADER, 2
            ),
            ("mp_api/cpp_api.h", False),
        )
        self.assertIsNone(
            amalgamate.parse_include("int value;\n", C_API_HEADER, 3)
        )

    def test_macro_include_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, r"c_api\.h:7.*PROJECT_HEADER"):
            amalgamate.parse_include("#include PROJECT_HEADER\n", C_API_HEADER, 7)

    def test_dependency_metadata_is_discovered_from_source_annotations(self):
        specs = self.catalog.dependencies
        self.assertEqual(set(specs), {"flatbuffers", "lz4"})
        self.assertEqual(specs["flatbuffers"].define, "CFG_USE_EXTERNAL_FLATBUFFERS")
        self.assertEqual(specs["lz4"].define, "CFG_USE_EXTERNAL_LZ4")
        source_entries = self.catalog.dependency_source_entries["lz4"]
        self.assertGreater(len(source_entries), 1)
        self.assertTrue(all(path.suffix == ".c" for path in source_entries))
        self.assertTrue(all(
            amalgamate.is_under(path, specs["lz4"].root)
            for path in source_entries
        ))
        self.assertNotIn("flatbuffers", self.catalog.dependency_source_entries)

    def test_configuration_metadata_is_discovered_from_source_annotations(self):
        self.assertEqual(
            amalgamate.configuration_defines(self.catalog.configurations),
            {
                "LIBMP_EXCLUDE_CAPTURE_BUILDER",
                "CFG_EXCLUDE_LZ4_ENCODER",
                "LIBMP_PUBLIC_NTD_API",
                "LIBMP_TRUSTED_INPUTS_ONLY",
                "NTD_EMBEDDED_ALLOC",
                "NTD_EXTERNAL_MALLOC",
                "NTD_OWN_FILEIO",
                "NTD_OWN_STDLIB",
            },
        )

    def test_configuration_expression_precedence(self):
        values = {"A": True, "B": False, "C": False}
        self.assertTrue(amalgamate.evaluate_configuration("A || B && C", values))
        self.assertFalse(amalgamate.evaluate_configuration("(A || B) && C", values))
        self.assertFalse(amalgamate.evaluate_configuration("!A || B", values))
        with self.assertRaisesRegex(RuntimeError, "incomplete configuration expression"):
            amalgamate.evaluate_configuration("A &&", values)

    def test_nested_configuration_annotation_is_rejected(self):
        synthetic = SOURCE_ROOT / "fixture_synthetic.h"
        with self.assertRaisesRegex(
            RuntimeError, "directly inside one conditional branch"
        ):
            amalgamate.parse_source(
                synthetic,
                [
                    "#if OUTER\n",
                    "#if NTD_OWN_STDLIB\n",
                    "// @amalgamate: config-begin NTD_OWN_STDLIB\n",
                    "// @amalgamate: config-end NTD_OWN_STDLIB\n",
                    "#endif\n",
                    "#endif\n",
                ],
            )

    def test_conventional_include_guards_are_classified(self):
        ordinary_guard = [
            "#ifndef HEADER_H\n",
            "#define HEADER_H\n",
            "int x;\n",
            "#endif\n",
        ]
        config_default = [
            "#ifndef FEATURE\n",
            "#define FEATURE 0\n",
            "#endif\n",
            "int x;\n",
        ]
        self.assertEqual(
            amalgamate.conventional_guard_lines(ordinary_guard), (1, 2, 4)
        )
        self.assertIsNone(amalgamate.conventional_guard_lines(config_default))

        extern_c_guard = [
            "#if defined(__cplusplus)\n",
            'extern "C" {\n',
            "#endif\n",
            "#ifndef HEADER_H\n",
            "#define HEADER_H\n",
            "int x;\n",
            "#endif\n",
            "#if defined(__cplusplus)\n",
            "}\n",
            "#endif\n",
        ]
        content_after_guard = ordinary_guard + ["int y;\n"]
        self.assertEqual(
            amalgamate.conventional_guard_lines(extern_c_guard), (4, 5, 7)
        )
        self.assertIsNone(amalgamate.conventional_guard_lines(content_after_guard))

    def test_default_output_is_relocatable_and_deterministic(self):
        amalgamate.generate(
            Path(self.default_output_directory.name),
            catalog=self.catalog,
            generation_timestamp=FIXTURE_GENERATION_TIMESTAMP,
        )
        self.assertEqual(
            self.default_header_text,
            self.default_header.read_text(encoding="utf-8"),
        )
        self.assertEqual(
            self.default_source_text,
            self.default_source.read_text(encoding="utf-8"),
        )
        timestamp_line = f"// Build timestamp: {FIXTURE_GENERATION_TIMESTAMP}"
        self.assertEqual(self.default_header_text.count(timestamp_line), 1)
        self.assertEqual(self.default_source_text.count(timestamp_line), 1)

        self.assertNotIn(str(SOURCE_ROOT), self.default_header_text)
        self.assertNotIn(str(SOURCE_ROOT), self.default_source_text)
        self.assert_no_resolvable_project_includes(
            self.default_header, self.default_source
        )

    def test_include_resolution_follows_compiler_search_semantics(self):
        self.assertEqual(
            amalgamate.resolve_local_include(
                C_API_HEADER,
                "mp_api/cpp_api.h",
                False,
            ),
            (SOURCE_ROOT / "mp_api" / "cpp_api.h").resolve(),
        )
        self.assertIsNone(amalgamate.resolve_local_include(
            SOURCE_ROOT / "ntd_stdlib" / "file_io.h",
            "limits.h",
            False,
        ))

    def test_default_output_contains_each_first_party_source_once(self):
        for public_header in amalgamate.PUBLIC_HEADERS:
            self.assertIn(
                f"// BEGIN {public_header.as_posix()}", self.default_header_text
            )
        self.assertIn('#include "libmp.h"', self.default_source_text)
        for implementation in amalgamate.implementation_sources():
            marker = f"// BEGIN {implementation.as_posix()}"
            self.assertEqual(self.default_source_text.count(marker), 1, marker)
        self.assertIn(
            '#line 1 "ntd_stdlib/limits.h"',
            self.default_header_text,
        )

    def test_bundled_dependencies_are_expanded_once(self):
        header_text = self.default_header_text
        source_text = self.default_source_text
        combined = header_text + source_text

        source_paths = self.expanded_paths(source_text)
        # Bundled mode folds both dependency implementations into the pair.
        for name in self.catalog.dependencies:
            self.assert_dependency_body_state(combined, name, True)
        flatbuffers_prefix = self.dependency_prefix("flatbuffers")
        self.assertFalse(
            any(path.startswith(flatbuffers_prefix) for path in source_paths)
        )
        source_entries = {
            amalgamate.relative_path(path).as_posix()
            for entries in self.catalog.dependency_source_entries.values()
            for path in entries
        }
        for dependency_source in source_entries:
            marker = f"// BEGIN {dependency_source}"
            self.assertEqual(source_text.count(marker), 1, marker)
        omitted_nested_sources = set(re.findall(
            r"^// Dependency source already supplied by its wrapper: (.+)$",
            source_text,
            re.MULTILINE,
        ))
        self.assertTrue(omitted_nested_sources)
        self.assertLessEqual(omitted_nested_sources, source_entries)
        expanded_paths = Counter(re.findall(
            r"^// BEGIN (.+)$", combined, re.MULTILINE
        ))

        def has_whole_file_guard(relative_path):
            lines = (SOURCE_ROOT / relative_path).read_text(
                encoding="utf-8"
            ).splitlines(keepends=True)
            return (
                any(amalgamate.PRAGMA_ONCE_RE.match(line) for line in lines)
                or amalgamate.conventional_guard_lines(lines) is not None
            )

        duplicated_whole_guard_headers = {
            path: count
            for path, count in expanded_paths.items()
            if count > 1
            and path.startswith("dependencies/")
            and path.endswith(".h")
            and has_whole_file_guard(path)
        }
        self.assertEqual(duplicated_whole_guard_headers, {})

    def test_default_configuration_prunes_disabled_implementations(self):
        combined = self.default_header_text + self.default_source_text

        self.assert_configuration_state(combined, "NTD_OWN_STDLIB", False)
        self.assert_configuration_state(combined, "NTD_OWN_FILEIO", False)
        self.assert_configuration_state(combined, "NTD_EMBEDDED_ALLOC", False)
        self.assert_configuration_state(combined, "capture-builder", True)
        self.assert_configuration_state(combined, "std-api-flavor", True)
        self.assert_configuration_state(combined, "ntd-api-flavor", False)
        self.assertEqual(
            combined.count(FLATBUFFER_SCHEMA_MARKER),
            1,
        )

    def test_reserved_ntd_configuration_is_retained(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=(
                    "NTD_OWN_STDLIB=1",
                    "NTD_OWN_FILEIO=1",
                    "NTD_EMBEDDED_ALLOC=1",
                ),
            )
            header_text = header.read_text(encoding="utf-8")
            source_text = source.read_text(encoding="utf-8")
            combined = header_text + source_text
            self.assert_no_resolvable_project_includes(header, source)

            self.assert_configuration_state(combined, "NTD_OWN_STDLIB", True)
            self.assert_configuration_state(combined, "NTD_OWN_FILEIO", True)
            self.assert_configuration_state(combined, "NTD_EMBEDDED_ALLOC", True)
            self.assertEqual(
                combined.count(FLATBUFFER_SCHEMA_MARKER),
                2,
            )

    def test_ntd_only_api_emits_one_schema_flavor(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("NTD_OWN_STDLIB=1", "LIBMP_PUBLIC_NTD_API=1"),
            )
            combined = header.read_text(encoding="utf-8") + source.read_text(
                encoding="utf-8"
            )
            self.assert_no_resolvable_project_includes(header, source)

            self.assertEqual(
                combined.count(FLATBUFFER_SCHEMA_MARKER),
                1,
            )
            self.assert_configuration_state(combined, "std-api-flavor", False)
            self.assert_configuration_state(combined, "ntd-api-flavor", True)

    def test_std_implementation_with_ntd_allocator_has_distinct_schema_flavors(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("NTD_EMBEDDED_ALLOC=1",),
            )
            combined = header.read_text(encoding="utf-8") + source.read_text(
                encoding="utf-8"
            )
            self.assertEqual(
                combined.count(FLATBUFFER_SCHEMA_MARKER),
                2,
            )
            self.assert_configuration_state(combined, "std-api-flavor", True)
            self.assert_configuration_state(combined, "ntd-api-flavor", True)

    def test_std_implementation_with_ntd_allocator_and_ntd_api_emits_one_flavor(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("NTD_EMBEDDED_ALLOC=1", "LIBMP_PUBLIC_NTD_API=1"),
            )
            combined = header.read_text(encoding="utf-8") + source.read_text(
                encoding="utf-8"
            )
            self.assertEqual(
                combined.count(FLATBUFFER_SCHEMA_MARKER),
                1,
            )
            self.assert_configuration_state(combined, "std-api-flavor", False)
            self.assert_configuration_state(combined, "ntd-api-flavor", True)

    def test_external_allocator_does_not_retain_embedded_arena(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("NTD_EXTERNAL_MALLOC=1",),
            )
            combined = header.read_text(encoding="utf-8") + source.read_text(
                encoding="utf-8"
            )

            self.assert_configuration_state(combined, "NTD_EXTERNAL_MALLOC", True)
            self.assert_configuration_state(combined, "NTD_EMBEDDED_ALLOC", False)

    def test_excluded_capture_builder_is_omitted(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("LIBMP_EXCLUDE_CAPTURE_BUILDER=1",),
            )
            combined = header.read_text(encoding="utf-8") + source.read_text(
                encoding="utf-8"
            )

            self.assert_configuration_state(combined, "capture-builder", False)

    def test_excluded_lz4_encoder_is_omitted(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("CFG_EXCLUDE_LZ4_ENCODER=1",),
            )
            combined = header.read_text(encoding="utf-8") + source.read_text(
                encoding="utf-8"
            )

            self.assert_configuration_state(combined, "lz4-encoder", False)

    def test_trusted_only_build_omits_untrusted_api_and_verification(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            header, source = amalgamate.generate(
                Path(temporary_directory),
                defines=("LIBMP_TRUSTED_INPUTS_ONLY=1",),
            )
            header_text = header.read_text(encoding="utf-8")
            combined = header_text + source.read_text(encoding="utf-8")

            self.assert_configuration_state(combined, "untrusted-session-api", False)
            self.assert_configuration_state(
                combined, "untrusted-input-verification", False
            )
            self.assertNotIn("MpOpenFromMem(const uint8_t*", combined)
            self.assertIn("MpOpenFromMemTrusted(const uint8_t*", header_text)

    def test_external_dependency_modes_are_independent(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            for external_flatbuffers in (False, True):
                for external_lz4 in (False, True):
                    with self.subTest(
                        external_flatbuffers=external_flatbuffers,
                        external_lz4=external_lz4,
                    ):
                        external_dependencies = {
                            name
                            for name, external in (
                                ("flatbuffers", external_flatbuffers),
                                ("lz4", external_lz4),
                            )
                            if external
                        }
                        header, source = amalgamate.generate(
                            Path(temporary_directory),
                            external_dependencies=external_dependencies,
                            catalog=self.catalog,
                        )
                        header_text = header.read_text(encoding="utf-8")
                        source_text = source.read_text(encoding="utf-8")
                        combined = header_text + source_text
                        for name, external in (
                            ("flatbuffers", external_flatbuffers),
                            ("lz4", external_lz4),
                        ):
                            spec = self.catalog.dependencies[name]
                            define = spec.define
                            mode = int(external)
                            self.assert_dependency_body_state(
                                combined, name, not external
                            )
                            self.assertIn(f"#define {define} {mode}", header_text)
                            self.assertIn(f"#elif {define} != {mode}", header_text)
                        for source_entry in self.catalog.dependency_source_entries["lz4"]:
                            marker = (
                                f"// BEGIN {amalgamate.relative_path(source_entry).as_posix()}"
                            )
                            self.assertEqual(marker in source_text, not external_lz4)
                        if external_flatbuffers:
                            self.assertIn(
                                "#include <flatbuffers/flatbuffers.h>", header_text
                            )
                        if external_lz4:
                            lz4_prefix = self.dependency_prefix("lz4")
                            include_prefix = f'#include "{lz4_prefix}'
                            self.assertNotIn(
                                include_prefix,
                                source_text,
                            )

    def test_unknown_external_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            with self.assertRaisesRegex(
                RuntimeError, "unknown external dependencies: unknown"
            ):
                amalgamate.generate(
                    Path(temporary_directory),
                    external_dependencies={"unknown"},
                    catalog=self.catalog,
                )


if __name__ == "__main__":
    unittest.main()

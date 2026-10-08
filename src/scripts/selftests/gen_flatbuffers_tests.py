#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.


import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from gen_flatbuffers import (  # noqa: E402
    parse_control_commands,
    render_control_command_registry,
)


class ControlCommandGenerationTests(unittest.TestCase):
    def test_parses_struct_and_table_payloads(self):
        commands = parse_control_commands(
            """
            struct FixedRequest { value: int; }
            struct FixedResponse { value: bool; }
            table VariableRequest { enabled: bool; level: int; }
            table VariableResponse { value: int; }
            table ComplexResponse { accepted: bool; reason: string; }
            /* @control_command id=7 name=Example request=FixedRequest response=VariableResponse */
            /* @control_command id=8 name=SetUIState request=VariableRequest response=FixedResponse */
            /* @control_command id=9 name=Query request=FixedRequest response=ComplexResponse */
            """
        )

        self.assertEqual(len(commands), 3)
        command = commands[0]
        self.assertEqual(command.command_id, 7)
        self.assertEqual(command.name, "Example")
        self.assertEqual(command.request_kind, "Struct")
        self.assertEqual(command.response_kind, "Table")
        self.assertEqual(commands[1].request_kind, "Table")
        self.assertEqual(commands[1].response_kind, "Struct")

        registry = render_control_command_registry(commands)
        self.assertIn("#define LIBMP_CONTROL_HAS_TABLE_PAYLOADS 1", registry)
        continuation_columns = {
            line.index("\\") for line in registry.splitlines() if line.endswith("\\")
        }
        self.assertEqual(len(continuation_columns), 1)
        self.assertIn(
            "X(7, Example, FixedRequest, Struct, VariableResponse, Table)",
            registry,
        )
        self.assertIn(
            "X(8, SetUIState, VariableRequest, Table, FixedResponse, Struct)",
            registry,
        )
        self.assertIn(
            "X(9, Query, FixedRequest, Struct, ComplexResponse, Table)",
            registry,
        )

    def test_rejects_malformed_annotations(self):
        schema = """
            struct Request { value: int; }
            /* @control_command name=WrongOrder id=1 request=Request response=Request */
        """
        with self.assertRaisesRegex(RuntimeError, "Malformed @control_command"):
            parse_control_commands(schema)

        line_comment = """
            struct Request { value: int; }
            // @control_command id=1 name=OldStyle request=Request response=Request
        """
        with self.assertRaisesRegex(RuntimeError, "Malformed @control_command"):
            parse_control_commands(line_comment)

    def test_rejects_duplicate_ids_and_unknown_types(self):
        duplicate = """
            struct Message { value: int; }
            /* @control_command id=1 name=First request=Message response=Message */
            /* @control_command id=1 name=Second request=Message response=Message */
        """
        with self.assertRaisesRegex(RuntimeError, "Duplicate @control_command id=1"):
            parse_control_commands(duplicate)

        unknown = """
            struct Message { value: int; }
            /* @control_command id=1 name=Missing request=Unknown response=Message */
        """
        with self.assertRaisesRegex(RuntimeError, "Unknown request type 'Unknown'"):
            parse_control_commands(unknown)

    def test_rejects_duplicate_names_and_out_of_range_ids(self):
        duplicate = """
            struct Message { value: int; }
            /* @control_command id=1 name=Same request=Message response=Message */
            /* @control_command id=2 name=Same request=Message response=Message */
        """
        with self.assertRaisesRegex(RuntimeError, "Duplicate @control_command name=Same"):
            parse_control_commands(duplicate)

        for command_id in (0, 65536):
            schema = f"""
                struct Message {{ value: int; }}
                /* @control_command id={command_id} name=Bad request=Message response=Message */
            """
            with self.assertRaisesRegex(RuntimeError, "must be in 1..65535"):
                parse_control_commands(schema)

    def test_payload_fields_do_not_change_registry_shape(self):
        commands = parse_control_commands(
            """
            enum Mode : byte { Off, On }
            struct ScalarRequest { value: Mode; }
            struct MultiFieldRequest { value: int; enabled: bool; }
            table Response { value: bool; }
            /* @control_command id=1 name=Scalar request=ScalarRequest response=Response */
            /* @control_command id=2 name=Complex request=MultiFieldRequest response=Response */
            """
        )

        registry = render_control_command_registry(commands)
        self.assertIn(
            "X(1, Scalar, ScalarRequest, Struct, Response, Table)", registry
        )
        self.assertIn(
            "X(2, Complex, MultiFieldRequest, Struct, Response, Table)", registry
        )

    def test_accepts_complex_responses(self):
        commands = parse_control_commands(
            """
            table Request { values: [int]; }
            struct StructResponse { accepted: bool; code: int; }
            table TableResponse { values: [int]; }
            /* @control_command id=1 name=QueryStruct request=Request response=StructResponse */
            /* @control_command id=2 name=QueryTable request=Request response=TableResponse */
            """
        )

        self.assertEqual(commands[0].request_kind, "Table")
        self.assertEqual(commands[0].response_kind, "Struct")
        self.assertEqual(commands[1].response_kind, "Table")

    def test_checked_in_control_files_are_current(self):
        source_root = Path(__file__).resolve().parents[2]
        types_dir = source_root / "mp_api" / "flatbuffer_types"
        commands = parse_control_commands(
            (types_dir / "mp_types.fbs").read_text(encoding="utf-8")
        )

        self.assertEqual(
            (types_dir / "control_commands_generated.inc").read_text(encoding="utf-8"),
            render_control_command_registry(commands),
        )


if __name__ == "__main__":
    unittest.main()

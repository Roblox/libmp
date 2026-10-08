#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.


import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import test_parity  # noqa: E402

PARITY_IMPL_PATH = Path(__file__).resolve().parents[2] / "tests" / "parity" / "run_parity.py"
PARITY_IMPL_SPEC = importlib.util.spec_from_file_location("libmp_parity_impl", PARITY_IMPL_PATH)
parity_impl = importlib.util.module_from_spec(PARITY_IMPL_SPEC)
PARITY_IMPL_SPEC.loader.exec_module(parity_impl)


def complete_summary():
    lines = [
        "LIBMP_PARITY 3",
        "SESSION 1 1 1 1",
        "GLOBAL 0",
        "GENERAL 0",
        "GROUPS 0",
        "TIMER_IDS 0",
        "COUNTER_IDS 0",
        "GPU_THREAD 0",
        "THREAD_IDS 0",
        "FRAMES 0",
        "PLACE_ID_SAMPLES 0",
        "UTC_SAMPLES 0",
        "FIND_QUERIES 1",
        "FIND GROUP 0 0 1:2a 0 0",
        "FIND TIMER 0 0 1:2a 0 0",
        "FIND COUNTER 0 0 1:2a 0 0",
        "FIND THREAD 0 0 1:2a 0 0",
        "GROUP_NAME_TIMER_QUERIES 1",
        "GROUP_NAME_TIMERS 0 0 1:2a 0",
        "COUNTER_ITERATOR_BEGIN",
        "COUNTER_ITERATOR_END 0 1",
        "LOG_ITERATOR_BEGIN",
    ]
    flag_indexes = [24, 25, 27, 28, 29]
    for report_index, flag_index in enumerate(flag_indexes):
        fields = ["LI", str(report_index)] + ["0"] * 29
        fields[flag_index] = "1"
        fields[30] = "0" if flag_index >= 28 else "1"
        lines.append(" ".join(fields))
    lines.extend(["LOG_ITERATOR_END 5 1", "END"])
    return "\n".join(lines) + "\n"


class ParityRunnerTests(unittest.TestCase):
    def test_registry_covers_every_language_wrapper(self):
        path = Path("/not-needed-for-registry-test")
        runners = parity_impl.create_runners(SimpleNamespace(
            parity_tool=path,
            libmp_luau=path,
            libmp_js=path,
            libmp_csharp=path,
            libmp_python=path,
            libmp_go=path,
        ))
        self.assertEqual(
            [runner.name for runner in runners],
            ["C++", "Luau", "JavaScript", "C#", "Python", "Go"],
        )
        self.assertEqual(
            [runner.name for runner in runners if runner.required],
            ["C++", "Luau"],
        )

    def test_complete_summary_validator_accepts_all_required_sections(self):
        lines = parity_impl.validate_complete_summary("test", complete_summary())
        self.assertEqual(lines[0], "LIBMP_PARITY 3")
        self.assertEqual(lines[-1], "END")

    def test_complete_summary_validator_rejects_the_old_small_subset(self):
        with self.assertRaisesRegex(ValueError, "missing parity format header"):
            parity_impl.validate_complete_summary("test", "FRAMES 1 16\nTHREADS 1\n")

    def test_complete_summary_validator_rejects_missing_log_kind(self):
        lines = [line for line in complete_summary().splitlines() if not line.startswith("LI 2 ")]
        summary = "\n".join(lines).replace("LOG_ITERATOR_END 5 1", "LOG_ITERATOR_END 4 1")
        with self.assertRaisesRegex(ValueError, "log traversal did not cover event"):
            parity_impl.validate_complete_summary("test", summary)

    def test_external_capture_does_not_require_fixture_specific_log_kinds(self):
        lines = [line for line in complete_summary().splitlines() if not line.startswith("LI ")]
        summary = "\n".join(lines).replace("LOG_ITERATOR_END 5 1", "LOG_ITERATOR_END 0 1")
        validated = parity_impl.validate_complete_summary(
            "test", summary, require_fixture_coverage=False)
        self.assertEqual(validated[-1], "END")

    def test_complete_summary_validator_rejects_count_mismatch(self):
        summary = complete_summary().replace("TIMER_IDS 0", "TIMER_IDS 1")
        with self.assertRaisesRegex(ValueError, "TIMER_IDS declares 1"):
            parity_impl.validate_complete_summary("test", summary)

    def test_complete_summary_validator_rejects_incomplete_find_matrix(self):
        summary = complete_summary().replace("FIND THREAD 0 0 1:2a 0 0\n", "")
        with self.assertRaisesRegex(ValueError, "expected 4 FIND records"):
            parity_impl.validate_complete_summary("test", summary)

    def test_resolve_lute_prefers_explicit_path(self):
        with TemporaryDirectory() as directory:
            lute = Path(directory) / "lute"
            lute.touch()
            with patch.dict(test_parity.os.environ, {"LIBMP_LUTE": str(lute)}):
                self.assertEqual(test_parity.resolve_lute(), lute)

    def test_main_builds_luau_then_runs_only_parity(self):
        lute = Path("/tools/lute")
        completed = subprocess.CompletedProcess([], 0)
        with (
            patch.object(test_parity, "resolve_lute", return_value=lute),
            patch.object(test_parity.subprocess, "run", return_value=completed) as run,
        ):
            self.assertEqual(test_parity.main([]), 0)

        self.assertEqual(run.call_count, 2)
        self.assertEqual(
            run.call_args_list[0].args[0],
            [
                sys.executable,
                str(test_parity.SCRIPT_DIR / "build.py"),
                "--luau",
                "--js",
                "--csharp",
                "--python",
                "--go",
            ],
        )
        self.assertEqual(
            run.call_args_list[1].args[0],
            [
                sys.executable,
                str(test_parity.SCRIPT_DIR / "test_native.py"),
                "cross_language_parity",
                "--config",
                "Release",
            ],
        )
        self.assertEqual(
            run.call_args_list[1].kwargs["env"]["LIBMP_LUTE"],
            str(lute.resolve()),
        )
        self.assertEqual(
            run.call_args_list[1].kwargs["env"][test_parity.REQUIRE_ALL_ENV],
            "1",
        )

    def test_main_stops_when_luau_build_fails(self):
        failed = subprocess.CompletedProcess([], 7)
        with (
            patch.object(test_parity, "resolve_lute", return_value=Path("/tools/lute")),
            patch.object(test_parity.subprocess, "run", return_value=failed) as run,
        ):
            self.assertEqual(test_parity.main([]), 7)
        run.assert_called_once()

    def test_main_replaces_default_luau_artifact(self):
        completed = subprocess.CompletedProcess([], 0)
        with TemporaryDirectory() as directory:
            variant_directory = Path(directory) / "Luau-AiGen"
            variant_directory.mkdir()
            artifact = variant_directory / "LibMP.luau"
            artifact.touch()
            with (
                patch.object(test_parity, "resolve_lute", return_value=Path("/tools/lute")),
                patch.object(test_parity.subprocess, "run", return_value=completed) as run,
            ):
                self.assertEqual(test_parity.main([
                    "--luau-artifact", str(variant_directory),
                ]), 0)

        self.assertNotIn("--luau", run.call_args_list[0].args[0])
        for call in run.call_args_list:
            self.assertEqual(
                call.kwargs["env"][test_parity.LUAU_ARTIFACT_ENV],
                str(artifact.resolve()),
            )

    def test_main_forwards_input_captures_to_registered_test(self):
        completed = subprocess.CompletedProcess([], 0)
        with TemporaryDirectory() as directory:
            capture1 = Path(directory) / "a.gprx"
            capture2 = Path(directory) / "b.gprx"
            capture1.touch()
            capture2.touch()
            with (
                patch.object(test_parity, "resolve_lute", return_value=Path("/tools/lute")),
                patch.object(test_parity.subprocess, "run", return_value=completed) as run,
            ):
                self.assertEqual(test_parity.main(["--input", str(Path(directory) / "*.gprx")]), 0)

        expected = json.dumps([str(capture1.resolve()), str(capture2.resolve())])
        self.assertEqual(run.call_args_list[0].kwargs["env"][test_parity.INPUT_ENV], expected)
        self.assertEqual(run.call_args_list[1].kwargs["env"][test_parity.INPUT_ENV], expected)

    def test_main_fails_before_build_when_lute_is_missing(self):
        with (
            patch.object(test_parity, "resolve_lute", return_value=None),
            patch.object(test_parity.subprocess, "run") as run,
        ):
            self.assertEqual(test_parity.main([]), 2)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()

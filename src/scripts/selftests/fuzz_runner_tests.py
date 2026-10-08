#!/usr/bin/env python3
# This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.


import argparse
import importlib.util
import os
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory


FUZZ_RUNNER_PATH = (
    Path(__file__).resolve().parents[2] / "tests" / "fuzz" / "build_fuzz.py"
)
FUZZ_RUNNER_SPEC = importlib.util.spec_from_file_location(
    "libmp_fuzz_runner", FUZZ_RUNNER_PATH
)
fuzz_runner = importlib.util.module_from_spec(FUZZ_RUNNER_SPEC)
FUZZ_RUNNER_SPEC.loader.exec_module(fuzz_runner)


class FuzzRunnerTests(unittest.TestCase):
    def test_duration_units(self):
        self.assertEqual(fuzz_runner.parse_duration("17"), 17)
        self.assertEqual(fuzz_runner.parse_duration("30s"), 30)
        self.assertEqual(fuzz_runner.parse_duration("5m"), 300)
        self.assertEqual(fuzz_runner.parse_duration("2h"), 7200)
        for invalid in ("0", "-1", "1.5h", "hour"):
            with self.assertRaises(argparse.ArgumentTypeError):
                fuzz_runner.parse_duration(invalid)

    def test_campaign_command_preserves_corpus_order_and_limits(self):
        executable = Path("/build/fuzzer")
        corpus_dirs = [Path("/state/corpus"), Path("/generated"), Path("/seeds")]
        artifact_dir = Path("/state/artifacts")
        command = fuzz_runner.campaign_command(
            executable,
            corpus_dirs,
            artifact_dir,
            3600,
            15,
            1024,
            123,
            4096,
            ["-use_value_profile=1"],
        )
        # Paths use the host's native separators, e.g. backslashes on Windows.
        self.assertEqual(
            command[:4],
            [str(path) for path in (executable, *corpus_dirs)],
        )
        self.assertIn("-max_total_time=3600", command)
        self.assertIn("-timeout=15", command)
        self.assertIn("-rss_limit_mb=1024", command)
        self.assertIn("-seed=123", command)
        self.assertIn(f"-artifact_prefix={artifact_dir}{os.sep}", command)
        self.assertIn("-max_len=4096", command)
        self.assertEqual(command[-1], "-use_value_profile=1")

    def test_managed_fuzzer_arguments_cannot_be_overridden(self):
        fuzz_runner.validate_fuzzer_arguments(["-use_value_profile=1", "-dict=words.txt"])
        for invalid in ("input", "-timeout=1", "-artifact_prefix=/tmp/"):
            with self.assertRaises(ValueError):
                fuzz_runner.validate_fuzzer_arguments([invalid])

    def test_snapshot_summary(self):
        self.assertEqual(
            fuzz_runner.snapshot_summary({"first": 10, "second": 25}),
            {"files": 2, "bytes": 35},
        )

    def test_fuzzer_log_summary_combines_workers(self):
        with TemporaryDirectory() as directory:
            first = Path(directory) / "first.log"
            second = Path(directory) / "second.log"
            first.write_text(
                "#10 NEW cov: 20 ft: 30 corp: 4/100b\n"
                "stat::number_of_executed_units: 100\n"
                "stat::average_exec_per_sec: 50\n"
                "stat::peak_rss_mb: 40\n",
                encoding="utf-8",
            )
            second.write_text(
                "#20 DONE cov: 25 ft: 35 corp: 5/120b\n"
                "stat::number_of_executed_units: 200\n"
                "stat::average_exec_per_sec: 75\n"
                "stat::peak_rss_mb: 60\n",
                encoding="utf-8",
            )

            summary = fuzz_runner.parse_fuzzer_logs([first, second])
            self.assertEqual(summary["number_of_executed_units"], 300)
            self.assertEqual(summary["average_exec_per_sec"], 125)
            self.assertEqual(summary["peak_rss_mb"], 60)
            self.assertEqual(summary["coverage_edges"], 25)
            self.assertEqual(summary["coverage_features"], 35)


if __name__ == "__main__":
    unittest.main()

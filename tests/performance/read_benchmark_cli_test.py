#!/usr/bin/env python3

import argparse
import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import read_performance

_COMMAND_LINE_ARGUMENTS: argparse.Namespace | None = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-binary", type=pathlib.Path, required=True)
    parser.add_argument("--diagnostic-binary", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--workloads", type=pathlib.Path, required=True)
    return parser.parse_args()


class ReadBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("benchmark binaries were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.workload_path = cls.arguments.workloads.resolve()
        cls.workloads = read_performance.validate_workload_manifest(
            read_performance.load_json_strict(cls.workload_path),
            repository_root=cls.root,
            manifest_path=cls.workload_path,
        )
        cls.fixtures = {
            fixture["id"]: cls.root / fixture["path"]
            for fixture in cls.workloads["fixtures"]
        }

    def run_binary(
        self,
        binary: pathlib.Path,
        *arguments: str,
        timeout: float = 120.0,
    ) -> read_performance.ChildResult:
        return read_performance.run_bounded(
            [str(binary), *arguments],
            cwd=self.root,
            timeout_seconds=timeout,
            stdout_limit=4 * 1024 * 1024,
            stderr_limit=1024 * 1024,
        )

    def test_usage_and_unknown_values_fail_as_harness_errors(self) -> None:
        no_arguments = self.run_binary(self.arguments.timing_binary)
        self.assertEqual(1, no_arguments.returncode)
        self.assertEqual(b"", no_arguments.stdout)
        self.assertIn(b"usage:", no_arguments.stderr)

        unknown = self.run_binary(
            self.arguments.timing_binary,
            "run",
            "unknown",
            "point-present-ipk-fit",
            str(self.fixtures["fit"]),
            "smoke",
        )
        self.assertEqual(1, unknown.returncode)
        self.assertEqual(b"", unknown.stdout)
        self.assertIn(b"engine", unknown.stderr)

    def test_binary_identity_reports_compiler_standard_library_and_sqlite(
        self,
    ) -> None:
        for binary, instrumented in (
            (self.arguments.timing_binary, False),
            (self.arguments.diagnostic_binary, True),
        ):
            with self.subTest(binary=binary.name):
                completed = self.run_binary(binary, "identity")
                self.assertEqual(0, completed.returncode, completed.stderr)
                self.assertEqual(b"", completed.stderr)
                identity = read_performance.load_json_bytes_strict(
                    completed.stdout,
                    "benchmark identity",
                )
                self.assertEqual(
                    {"build", "mode", "schema_version", "source", "sqlite"},
                    set(identity),
                )
                self.assertEqual("identity", identity["mode"])
                self.assertEqual(1, identity["schema_version"])
                self.assertEqual(instrumented, identity["build"]["instrumentation"])
                self.assertIn(
                    identity["build"]["compiler"]["id"],
                    {"clang", "gcc", "msvc"},
                )
                self.assertIn(
                    identity["build"]["standard_library"]["id"],
                    {"libc++", "libstdc++", "msvc-stl"},
                )
                self.assertEqual("3.54.0", identity["sqlite"]["version"])
                self.assertEqual(
                    self.workloads["sqlite_semantic_compile_options"],
                    identity["sqlite"]["compile_options"],
                )
                self.assertRegex(identity["source"]["revision"], r"^[0-9a-f]{40,64}$")
                self.assertRegex(identity["source"]["tree"], r"^[0-9a-f]{40,64}$")

    def test_timing_smoke_matrix_uses_strict_public_results(self) -> None:
        for case in self.workloads["cases"]:
            database = self.fixtures[case["fixture"]]
            for engine in ("modern", "sqlite"):
                with self.subTest(case=case["id"], engine=engine):
                    completed = self.run_binary(
                        self.arguments.timing_binary,
                        "run",
                        engine,
                        case["id"],
                        str(database),
                        "smoke",
                    )
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    self.assertEqual(b"", completed.stderr)
                    report = read_performance.load_json_bytes_strict(
                        completed.stdout,
                        "timing smoke",
                    )
                    read_performance.validate_raw_timing_report(
                        report,
                        workload_manifest=self.workloads,
                        expected_engine=engine,
                        expected_case=case["id"],
                        expected_run_kind="smoke",
                    )

    def test_diagnostic_matrix_has_complete_engine_specific_counters(self) -> None:
        for case in self.workloads["cases"]:
            database = self.fixtures[case["fixture"]]
            for engine in ("modern", "sqlite"):
                with self.subTest(case=case["id"], engine=engine):
                    completed = self.run_binary(
                        self.arguments.diagnostic_binary,
                        "run",
                        engine,
                        case["id"],
                        str(database),
                        "diagnostic",
                    )
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    self.assertEqual(b"", completed.stderr)
                    report = read_performance.load_json_bytes_strict(
                        completed.stdout,
                        "diagnostic smoke",
                    )
                    read_performance.validate_raw_diagnostic_report(
                        report,
                        workload_manifest=self.workloads,
                        expected_engine=engine,
                        expected_case=case["id"],
                    )

    def test_profile_replay_is_selected_and_verified(self) -> None:
        completed = self.run_binary(
            self.arguments.timing_binary,
            "profile",
            "modern",
            "point-present-ipk-fit",
            str(self.fixtures["fit"]),
            "8",
        )
        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual(b"", completed.stderr)
        report = read_performance.load_json_bytes_strict(
            completed.stdout,
            "profile replay",
        )
        self.assertEqual(
            {
                "case",
                "completion",
                "completion_schema_version",
                "engine",
                "iterations",
                "mode",
                "schema_version",
                "work",
            },
            set(report),
        )
        self.assertEqual("profile", report["mode"])
        self.assertEqual("modern", report["engine"])
        self.assertEqual("point-present-ipk-fit", report["case"])
        self.assertEqual(8, report["iterations"])
        self.assertEqual(
            {
                "operations": 8,
                "items": 8,
                "rows": 8,
                "bytes": 8 * 256,
                "result_hits": 8,
                "result_misses": 0,
                "digest": read_performance.workload_digest(
                    kind="point_present",
                    row_count=4096,
                    iterations=8,
                    seed=0x9E3779B97F4A7C15,
                ),
            },
            report["work"],
        )
        self.assertEqual(
            {
                "status": "complete",
                "session_opens": 1,
                "statement_prepares": 3,
                "statement_finalizes": 3,
                "statement_resets": 4096 + 8 + 2,
                "pre_verifications": 1,
                "post_verifications": 1,
            },
            report["completion"],
        )


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[sys.argv[0]])

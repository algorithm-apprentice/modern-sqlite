#!/usr/bin/env python3

import argparse
import json
import pathlib
import subprocess
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import distinct_compound_performance
from tools import read_performance


_COMMAND_LINE_ARGUMENTS: argparse.Namespace | None = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-binary", type=pathlib.Path, required=True)
    parser.add_argument("--diagnostic-binary", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--workloads", type=pathlib.Path, required=True)
    return parser.parse_args()


class DistinctCompoundBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("benchmark binaries were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.workload_path = cls.arguments.workloads.resolve()
        distinct_compound_performance._configure_common()
        cls.workloads = distinct_compound_performance.validate_workload_manifest(
            read_performance.load_json_strict(cls.workload_path),
            repository_root=cls.root,
            manifest_path=cls.workload_path,
        )
        cls.fixture = (
            cls.root
            / pathlib.PurePosixPath(cls.workloads["fixtures"][0]["path"])
        )

    def run_binary(
        self,
        binary: pathlib.Path,
        *arguments: str,
    ) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [str(binary), *arguments],
            cwd=self.root,
            check=False,
            capture_output=True,
            timeout=180,
        )

    def test_smoke_matrix_matches_between_engines(self) -> None:
        for case_id in distinct_compound_performance.CASE_IDS:
            reports: dict[str, dict[str, object]] = {}
            for engine in ("modern", "sqlite"):
                with self.subTest(case=case_id, engine=engine):
                    completed = self.run_binary(
                        self.arguments.timing_binary,
                        "run",
                        engine,
                        case_id,
                        str(self.fixture),
                        "smoke",
                    )
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    self.assertEqual(b"", completed.stderr)
                    report = json.loads(completed.stdout)
                    read_performance.validate_raw_timing_report(
                        report,
                        workload_manifest=self.workloads,
                        expected_engine=engine,
                        expected_case=case_id,
                        expected_run_kind="smoke",
                    )
                    if (
                        engine == "modern"
                        and case_id == distinct_compound_performance.CASE_IDS[0]
                    ):
                        for field, replacement in (
                            ("schema_version", 1.0),
                            ("page_size", 4096.0),
                        ):
                            malformed = json.loads(json.dumps(report))
                            if field == "page_size":
                                malformed["effective_configuration"][field] = replacement
                            else:
                                malformed[field] = replacement
                            with self.assertRaises(read_performance.HarnessError):
                                read_performance.validate_raw_timing_report(
                                    malformed,
                                    workload_manifest=self.workloads,
                                    expected_engine=engine,
                                    expected_case=case_id,
                                    expected_run_kind="smoke",
                                )
                    reports[engine] = report
            self.assertEqual(
                reports["modern"]["warmup"],
                reports["sqlite"]["warmup"],
            )
            self.assertEqual(
                reports["modern"]["repetitions"][0]["digest"],
                reports["sqlite"]["repetitions"][0]["digest"],
            )

    def test_diagnostics_match_logical_work(self) -> None:
        sqlite_spill_bytes: dict[str, int] = {}
        modern_pages_written: dict[str, int] = {}
        for case_id in distinct_compound_performance.CASE_IDS:
            work: dict[str, object] | None = None
            for engine in ("modern", "sqlite"):
                with self.subTest(case=case_id, engine=engine):
                    completed = self.run_binary(
                        self.arguments.diagnostic_binary,
                        "run",
                        engine,
                        case_id,
                        str(self.fixture),
                        "diagnostic",
                    )
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    self.assertEqual(b"", completed.stderr)
                    report = json.loads(completed.stdout)
                    read_performance.validate_raw_diagnostic_report(
                        report,
                        workload_manifest=self.workloads,
                        expected_engine=engine,
                        expected_case=case_id,
                    )
                    if engine == "sqlite":
                        sqlite_spill_bytes[case_id] = report["counters"]["sqlite"][
                            "temp_bytes_spilled"
                        ]
                    else:
                        modern_pages_written[case_id] = report["counters"]["modern"][
                            "pages_written"
                        ]
                    expected_probe_counts = (
                        distinct_compound_performance.expected_probe_counts(case_id)
                    )
                    self.assertEqual(
                        expected_probe_counts,
                        report["probe_counts"],
                    )
                    self.assertEqual(
                        sum(expected_probe_counts),
                        report["source_rows"],
                    )
                    if engine == "modern" and case_id == distinct_compound_performance.CASE_IDS[0]:
                        malformed = json.loads(json.dumps(report))
                        del malformed["source_rows"]
                        with self.assertRaises(read_performance.HarnessError):
                            read_performance.validate_raw_diagnostic_report(
                                malformed,
                                workload_manifest=self.workloads,
                                expected_engine=engine,
                                expected_case=case_id,
                            )
                        malformed = json.loads(json.dumps(report))
                        malformed["counters"] = []
                        with self.assertRaises(read_performance.HarnessError):
                            read_performance.validate_raw_diagnostic_report(
                                malformed,
                                workload_manifest=self.workloads,
                                expected_engine=engine,
                                expected_case=case_id,
                            )
                        for replacement in (True, 1.0):
                            malformed = json.loads(json.dumps(report))
                            malformed["probe_counts"][0] = replacement
                            with self.assertRaises(read_performance.HarnessError):
                                read_performance.validate_raw_diagnostic_report(
                                    malformed,
                                    workload_manifest=self.workloads,
                                    expected_engine=engine,
                                    expected_case=case_id,
                                )
                        for field, replacement in (
                            ("schema_version", 1.0),
                            ("page_size", 4096.0),
                        ):
                            malformed = json.loads(json.dumps(report))
                            if field == "page_size":
                                malformed["effective_configuration"][field] = replacement
                            else:
                                malformed[field] = replacement
                            with self.assertRaises(read_performance.HarnessError):
                                read_performance.validate_raw_diagnostic_report(
                                    malformed,
                                    workload_manifest=self.workloads,
                                    expected_engine=engine,
                                    expected_case=case_id,
                                )
                        malformed = json.loads(json.dumps(report))
                        malformed["probe_counts"][0] -= 1
                        with self.assertRaises(read_performance.HarnessError):
                            read_performance.validate_raw_diagnostic_report(
                                malformed,
                                workload_manifest=self.workloads,
                                expected_engine=engine,
                                expected_case=case_id,
                            )
                    if work is None:
                        work = report["work"]
                    else:
                        self.assertEqual(work, report["work"])
        self.assertGreater(
            sqlite_spill_bytes["union-keyed-relation-before-file"],
            0,
        )
        self.assertEqual(0, sqlite_spill_bytes["union-replace-file"])
        self.assertGreater(
            modern_pages_written["union-keyed-relation-before-file"],
            0,
        )
        self.assertEqual(0, modern_pages_written["union-replace-file"])

        for case_id, replacement in (
            ("union-keyed-relation-before-file", 0),
            ("union-replace-file", 1),
        ):
            completed = self.run_binary(
                self.arguments.diagnostic_binary,
                "run",
                "modern",
                case_id,
                str(self.fixture),
                "diagnostic",
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            malformed = json.loads(completed.stdout)
            malformed["counters"]["modern"]["pages_written"] = replacement
            with self.assertRaises(read_performance.HarnessError):
                read_performance.validate_raw_diagnostic_report(
                    malformed,
                    workload_manifest=self.workloads,
                    expected_engine="modern",
                    expected_case=case_id,
                )

    def test_malformed_reports_are_harness_errors(self) -> None:
        with self.assertRaises(read_performance.HarnessError):
            read_performance.validate_raw_timing_report(
                {},
                workload_manifest=self.workloads,
                expected_engine="modern",
                expected_case=distinct_compound_performance.CASE_IDS[0],
                expected_run_kind="smoke",
            )
        with self.assertRaises(read_performance.HarnessError):
            read_performance.validate_raw_diagnostic_report(
                {},
                workload_manifest=self.workloads,
                expected_engine="modern",
                expected_case=distinct_compound_performance.CASE_IDS[0],
            )


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[sys.argv[0]])

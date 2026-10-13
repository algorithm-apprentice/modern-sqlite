import argparse
import json
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools import aggregate_performance
from tools import read_performance


ARGUMENTS = None


class AggregateBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if ARGUMENTS is None:
            raise unittest.SkipTest("aggregate benchmark binaries were not supplied")
        cls.arguments = ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.path = cls.arguments.workloads.resolve()
        cls.workloads = aggregate_performance.validate_workload_manifest(
            read_performance.load_json_strict(cls.path),
            repository_root=cls.root, manifest_path=cls.path,
        )
        cls.fixture = cls.root / cls.workloads["fixtures"][0]["path"]
        aggregate_performance._configure_common()

    @classmethod
    def tearDownClass(cls) -> None:
        aggregate_performance._restore_common()

    def run_report(self, binary, engine, case_id, mode):
        result = subprocess.run(
            [str(binary), "run", engine, case_id, str(self.fixture), mode],
            cwd=self.root, capture_output=True, timeout=180, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr.decode(errors="replace"))
        self.assertEqual(b"", result.stderr)
        return json.loads(result.stdout)

    def test_timing_smoke_matrix(self) -> None:
        for case in aggregate_performance.CASE_IDS:
            reports = {}
            for engine in ("modern", "sqlite"):
                with self.subTest(case=case, engine=engine):
                    report = self.run_report(self.arguments.timing_binary, engine, case, "smoke")
                    aggregate_performance.validate_raw_timing_report(
                        report, workload_manifest=self.workloads,
                        expected_engine=engine, expected_case=case, expected_run_kind="smoke",
                    )
                    reports[engine] = report
            self.assertEqual(reports["modern"]["warmup"], reports["sqlite"]["warmup"])
            self.assertEqual(
                reports["modern"]["repetitions"][0]["digest"],
                reports["sqlite"]["repetitions"][0]["digest"],
            )

    def test_diagnostic_matrix(self) -> None:
        for case in aggregate_performance.CASE_IDS:
            reports = {}
            for engine in ("modern", "sqlite"):
                with self.subTest(case=case, engine=engine):
                    report = self.run_report(
                        self.arguments.diagnostic_binary, engine, case, "diagnostic"
                    )
                    aggregate_performance.validate_raw_diagnostic_report(
                        report, workload_manifest=self.workloads,
                        expected_engine=engine, expected_case=case,
                    )
                    self.assertEqual(
                        aggregate_performance.expected_probe_counts(case), report["probe_counts"]
                    )
                    reports[engine] = report
            self.assertEqual(reports["modern"]["work"], reports["sqlite"]["work"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-binary", type=pathlib.Path, required=True)
    parser.add_argument("--diagnostic-binary", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--workloads", type=pathlib.Path, required=True)
    ARGUMENTS = parser.parse_args()
    unittest.main(argv=[sys.argv[0]])

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

from tools import index_performance
from tools import read_performance


_COMMAND_LINE_ARGUMENTS: argparse.Namespace | None = None

CASE_IDS = (
    "index-equality-covering-hit",
    "index-equality-covering-miss",
    "index-equality-noncovering-hit",
    "index-multi-equality-covering",
    "index-range-covering",
    "index-range-noncovering",
    "index-unselective-noncovering",
    "index-unselective-covering",
)

EXPECTED_SMOKE = {
    "index-equality-covering-hit": {
        "bytes": 16,
        "digest": "9ffef6e3243d783e",
        "items": 1,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 1,
    },
    "index-equality-covering-miss": {
        "bytes": 0,
        "digest": "4a8be64d5309cff3",
        "items": 1,
        "operations": 1,
        "result_hits": 0,
        "result_misses": 1,
        "rows": 0,
    },
    "index-equality-noncovering-hit": {
        "bytes": 128,
        "digest": "50df843383db7694",
        "items": 1,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 1,
    },
    "index-multi-equality-covering": {
        "bytes": 8,
        "digest": "85e2a2550e2e0b0d",
        "items": 1,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 1,
    },
    "index-range-covering": {
        "bytes": 1024,
        "digest": "93049f748b0d35a5",
        "items": 64,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 64,
    },
    "index-range-noncovering": {
        "bytes": 8192,
        "digest": "e955cfbed36e5f4e",
        "items": 64,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 64,
    },
    "index-unselective-noncovering": {
        "bytes": 262144,
        "digest": "9156e0a13a7f27da",
        "items": 2048,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 2048,
    },
    "index-unselective-covering": {
        "bytes": 16384,
        "digest": "a8ccc1a94d5c8252",
        "items": 2048,
        "operations": 1,
        "result_hits": 1,
        "result_misses": 0,
        "rows": 2048,
    },
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-binary", type=pathlib.Path, required=True)
    parser.add_argument("--diagnostic-binary", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--workloads", type=pathlib.Path, required=True)
    return parser.parse_args()


class IndexBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("benchmark binaries were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.workload_path = cls.arguments.workloads.resolve()
        index_performance._configure_common()
        cls.workloads = index_performance.validate_workload_manifest(
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
            timeout=120,
        )

    def test_index_smoke_matrix_matches_between_engines(self) -> None:
        for case_id in CASE_IDS:
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
                    self.assertEqual(case_id, report["case"])
                    self.assertEqual(engine, report["engine"])
                    self.assertEqual("smoke", report["run_kind"])
                    self.assertEqual(1, len(report["repetitions"]))
                    reports[engine] = report
            self.assertEqual({"modern", "sqlite"}, set(reports))
            self.assertEqual(
                reports["modern"]["warmup"],
                reports["sqlite"]["warmup"],
            )
            for engine in ("modern", "sqlite"):
                repetition = reports[engine]["repetitions"][0]
                self.assertEqual(0, repetition["index"])
                self.assertEqual(
                    EXPECTED_SMOKE[case_id],
                    {
                        key: value
                        for key, value in repetition.items()
                        if key not in {"index", "wall_ns", "cpu_ns"}
                    },
                )

    def test_index_diagnostics_match_logical_work(self) -> None:
        for case_id in CASE_IDS:
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
                    self.assertEqual(case_id, report["case"])
                    self.assertEqual(engine, report["engine"])
                    if engine == "sqlite":
                        fullscan_steps = report["counters"]["sqlite"][
                            "fullscan_steps"
                        ]
                        if case_id == "index-unselective-noncovering":
                            self.assertGreater(fullscan_steps, 0)
                        else:
                            self.assertEqual(0, fullscan_steps)
                    if work is None:
                        work = report["work"]
                    else:
                        self.assertEqual(work, report["work"])


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[sys.argv[0]])

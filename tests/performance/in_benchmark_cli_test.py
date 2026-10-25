import argparse
import json
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import in_performance
from tools import read_performance


ARGUMENTS: argparse.Namespace | None = None


class InBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if ARGUMENTS is None:
            raise unittest.SkipTest("benchmark binaries were not supplied")
        cls.arguments = ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.workloads = in_performance.validate_workload_manifest(
            read_performance.load_json_strict(cls.arguments.workloads),
            repository_root=cls.root,
            manifest_path=cls.arguments.workloads.resolve(),
        )
        cls.fixture = cls.root / cls.workloads["fixtures"][0]["path"]

    def run_case(self, binary: pathlib.Path, engine: str, case: str, mode: str) -> dict:
        completed = subprocess.run(
            [str(binary), "run", engine, case, str(self.fixture), mode],
            cwd=self.root,
            capture_output=True,
            check=False,
            timeout=120,
        )
        self.assertEqual(0, completed.returncode, completed.stderr.decode(errors="replace"))
        self.assertEqual(b"", completed.stderr)
        return json.loads(completed.stdout)

    def test_both_engines_match_pinned_typed_work_in_smoke_and_diagnostics(self) -> None:
        with in_performance.configured_contract():
            for case in self.workloads["cases"]:
                for engine in ("modern", "sqlite"):
                    with self.subTest(case=case["id"], engine=engine):
                        for run_kind in ("smoke", "baseline"):
                            timing = self.run_case(
                                self.arguments.timing_binary, engine, case["id"], run_kind
                            )
                            in_performance.validate_raw_timing_report(
                                timing,
                                workload_manifest=self.workloads,
                                expected_engine=engine,
                                expected_case=case["id"],
                                expected_run_kind=run_kind,
                            )
                        diagnostic = self.run_case(
                            self.arguments.diagnostic_binary, engine, case["id"], "diagnostic"
                        )
                        in_performance.validate_raw_diagnostic_report(
                            diagnostic,
                            workload_manifest=self.workloads,
                            expected_engine=engine,
                            expected_case=case["id"],
                        )
                        self.assertEqual(
                            case["expected"]["diagnostic"], diagnostic["work"]
                        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-binary", type=pathlib.Path, required=True)
    parser.add_argument("--diagnostic-binary", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--workloads", type=pathlib.Path, required=True)
    ARGUMENTS = parser.parse_args()
    unittest.main(argv=[sys.argv[0]])

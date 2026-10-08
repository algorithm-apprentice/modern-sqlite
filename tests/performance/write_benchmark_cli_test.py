#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile
import unittest


_COMMAND_LINE_ARGUMENTS: argparse.Namespace | None = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--schema-fixture", type=pathlib.Path, required=True)
    parser.add_argument("--populated-fixture", type=pathlib.Path, required=True)
    return parser.parse_args()


class WriteBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("write benchmark inputs were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS

    def fixture_for(self, case: str, zero_fixture: pathlib.Path) -> pathlib.Path:
        if case == "create-table-implicit":
            return zero_fixture
        if case.startswith("insert-"):
            return self.arguments.schema_fixture
        return self.arguments.populated_fixture

    def run_case(
        self,
        engine: str,
        profile: str,
        case: str,
        fixture: pathlib.Path,
        scratch: pathlib.Path,
        run_kind: str = "smoke",
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                self.arguments.binary,
                "run",
                engine,
                profile,
                case,
                fixture,
                scratch,
                run_kind,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

    def assert_work(
        self,
        work: dict[str, object],
        expected: tuple[int, int, int, int, int, int, int],
    ) -> None:
        (
            transactions,
            dml_operations,
            row_mutations,
            changed_rows,
            final_rows,
            last_insert_rowid,
            schema_objects,
        ) = expected
        self.assertEqual(transactions, work["transactions"])
        self.assertEqual(dml_operations, work["dml_operations"])
        self.assertEqual(row_mutations, work["row_mutations"])
        self.assertEqual(changed_rows, work["changed_rows"])
        self.assertEqual(final_rows, work["final_rows"])
        self.assertEqual(last_insert_rowid, work["last_insert_rowid"])
        self.assertEqual(schema_objects, work["schema_objects"])
        self.assertRegex(work["digest"], r"^[0-9a-f]{16}$")

    def test_smoke_matrix_uses_fresh_images_and_strict_results(self) -> None:
        expected = {
            "create-table-implicit": (1, 1, 0, 0, 0, 0, 1),
            "insert-point-implicit": (1, 1, 1, 1, 1, 1, 1),
            "insert-batch-explicit": (1, 8, 8, 8, 8, 8, 1),
            "update-point-implicit": (1, 1, 1, 1, 65_536, 0, 1),
            "update-scan-implicit": (1, 1, 8, 8, 65_536, 0, 1),
            "delete-point-implicit": (1, 1, 1, 1, 65_535, 0, 1),
            "delete-scan-implicit": (1, 1, 8, 8, 65_528, 0, 1),
            "mixed-batch-commit": (1, 8, 8, 8, 65_538, 65_540, 1),
            "mixed-batch-rollback": (1, 8, 8, 8, 65_536, 65_540, 1),
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            zero_fixture = root / "zero.db"
            zero_fixture.write_bytes(b"")
            cases = tuple(expected)
            fixture_bytes = {
                path: path.read_bytes()
                for path in (
                    zero_fixture,
                    self.arguments.schema_fixture,
                    self.arguments.populated_fixture,
                )
            }
            for profile in ("engine-default", "matched-durable"):
                for case in cases:
                    reports: dict[str, dict[str, object]] = {}
                    for engine in ("modern", "sqlite"):
                        scratch = root / f"{profile}-{case}-{engine}"
                        scratch.mkdir()
                        with self.subTest(
                            profile=profile,
                            case=case,
                            engine=engine,
                        ):
                            completed = self.run_case(
                                engine,
                                profile,
                                case,
                                self.fixture_for(case, zero_fixture),
                                scratch,
                            )
                            self.assertEqual(
                                0, completed.returncode, completed.stderr
                            )
                            self.assertEqual("", completed.stderr)
                            report = json.loads(completed.stdout)
                            self.assertEqual(
                                {
                                    "case",
                                    "completion",
                                    "effective_configuration",
                                    "engine",
                                    "mode",
                                    "profile",
                                    "repetitions",
                                    "run_kind",
                                    "schema_version",
                                    "timer",
                                    "warmup",
                                    "workload_semantics_version",
                                },
                                set(report),
                            )
                            self.assertEqual(case, report["case"])
                            self.assertEqual(engine, report["engine"])
                            self.assertEqual(profile, report["profile"])
                            self.assertEqual("timing", report["mode"])
                            self.assertEqual("smoke", report["run_kind"])
                            self.assertEqual(1, report["schema_version"])
                            self.assertEqual(
                                1, report["workload_semantics_version"]
                            )
                            self.assertEqual(
                                {
                                    "wall": "steady_clock",
                                    "cpu": "CLOCK_PROCESS_CPUTIME_ID",
                                },
                                report["timer"],
                            )
                            self.assert_work(report["warmup"], expected[case])
                            self.assertEqual(1, len(report["repetitions"]))
                            repetition = report["repetitions"][0]
                            self.assertEqual(0, repetition["index"])
                            self.assertGreater(repetition["wall_ns"], 0)
                            self.assertGreaterEqual(repetition["cpu_ns"], 0)
                            self.assert_work(repetition, expected[case])
                            self.assertEqual(
                                report["warmup"]["digest"],
                                repetition["digest"],
                            )
                            self.assertEqual(
                                {
                                    "fresh_databases": 2,
                                    "measured_repetitions": 1,
                                    "post_verifications": 2,
                                    "pre_verifications": 1,
                                    "status": "complete",
                                    "warmups": 1,
                                },
                                report["completion"],
                            )
                            configuration = report["effective_configuration"]
                            self.assertEqual(4096, configuration["page_size"])
                            self.assertEqual(
                                "single", configuration["thread_mode"]
                            )
                            self.assertEqual([], list(scratch.iterdir()))
                            reports[engine] = report
                    if set(reports) == {"modern", "sqlite"}:
                        self.assertEqual(
                            reports["modern"]["warmup"]["digest"],
                            reports["sqlite"]["warmup"]["digest"],
                        )
                        self.assertEqual(
                            reports["modern"]["repetitions"][0]["digest"],
                            reports["sqlite"]["repetitions"][0]["digest"],
                        )
            for path, before in fixture_bytes.items():
                self.assertEqual(before, path.read_bytes())

    def test_usage_and_invalid_scratch_are_harness_errors(self) -> None:
        no_arguments = subprocess.run(
            [self.arguments.binary],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(1, no_arguments.returncode)
        self.assertEqual("", no_arguments.stdout)
        self.assertIn("usage:", no_arguments.stderr)

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            scratch = root / "scratch"
            scratch.mkdir()
            (scratch / "warmup.db").write_bytes(b"occupied")
            completed = self.run_case(
                "modern",
                "matched-durable",
                "insert-point-implicit",
                self.arguments.schema_fixture,
                scratch,
            )
            self.assertEqual(1, completed.returncode)
            self.assertEqual("", completed.stdout)
            self.assertIn("scratch database already exists", completed.stderr)

    def test_unknown_case_and_wrong_fixture_use_distinct_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            unknown_scratch = root / "unknown"
            unknown_scratch.mkdir()
            unknown = self.run_case(
                "modern",
                "matched-durable",
                "missing-case",
                self.arguments.schema_fixture,
                unknown_scratch,
            )
            self.assertEqual(1, unknown.returncode)
            self.assertEqual("", unknown.stdout)
            self.assertIn("unsupported write benchmark case", unknown.stderr)

            mismatch_scratch = root / "mismatch"
            mismatch_scratch.mkdir()
            mismatch = self.run_case(
                "modern",
                "matched-durable",
                "update-point-implicit",
                self.arguments.schema_fixture,
                mismatch_scratch,
            )
            self.assertEqual(2, mismatch.returncode)
            self.assertEqual("", mismatch.stdout)
            self.assertIn("row count differs", mismatch.stderr)


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[__file__])

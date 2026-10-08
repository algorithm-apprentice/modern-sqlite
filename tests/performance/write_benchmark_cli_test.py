#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
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

    def run_case(
        self,
        engine: str,
        profile: str,
        case: str,
        database: pathlib.Path,
        operations: int = 8,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                self.arguments.binary,
                "smoke",
                engine,
                profile,
                case,
                database,
                str(operations),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

    def initial_database(
        self, root: pathlib.Path, engine: str, case: str
    ) -> pathlib.Path:
        database = root / f"{engine}-{case}.db"
        if case == "create-table-implicit":
            database.write_bytes(b"")
        elif case.startswith("insert-"):
            shutil.copyfile(self.arguments.schema_fixture, database)
        else:
            shutil.copyfile(self.arguments.populated_fixture, database)
        return database

    def test_matched_smoke_completes_for_every_case_and_engine(self) -> None:
        cases = (
            "create-table-implicit",
            "insert-point-implicit",
            "insert-batch-explicit",
            "update-point-implicit",
            "update-scan-implicit",
            "delete-point-implicit",
            "delete-scan-implicit",
            "mixed-batch-commit",
            "mixed-batch-rollback",
        )
        expected = {
            "create-table-implicit": (0, 0, 0, 8),
            "insert-point-implicit": (8, 8, 8, 1),
            "insert-batch-explicit": (8, 8, 8, 1),
            "update-point-implicit": (8, 65_536, 0, 1),
            "update-scan-implicit": (8, 65_536, 0, 1),
            "delete-point-implicit": (8, 65_528, 0, 1),
            "delete-scan-implicit": (8, 65_528, 0, 1),
            "mixed-batch-commit": (8, 65_538, 65_540, 1),
            "mixed-batch-rollback": (8, 65_536, 65_540, 1),
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for case in cases:
                reports: dict[str, dict[str, object]] = {}
                for engine in ("modern", "sqlite"):
                    database = self.initial_database(root, engine, case)
                    before = database.read_bytes()
                    with self.subTest(case=case, engine=engine):
                        completed = self.run_case(
                            engine,
                            "matched-durable",
                            case,
                            database,
                        )
                        self.assertEqual(
                            0, completed.returncode, completed.stderr
                        )
                        self.assertEqual("", completed.stderr)
                        report = json.loads(completed.stdout)
                        self.assertEqual(case, report["case"])
                        self.assertEqual(engine, report["engine"])
                        self.assertEqual(
                            "matched-durable", report["profile"]
                        )
                        self.assertEqual("smoke", report["mode"])
                        self.assertEqual("complete", report["status"])
                        self.assertEqual(1, report["schema_version"])
                        self.assertEqual(8, report["operations"])
                        (
                            changed_rows,
                            final_rows,
                            last_insert_rowid,
                            schema_objects,
                        ) = expected[case]
                        self.assertEqual(changed_rows, report["changed_rows"])
                        self.assertEqual(final_rows, report["final_rows"])
                        self.assertEqual(
                            last_insert_rowid,
                            report["last_insert_rowid"],
                        )
                        self.assertEqual(
                            schema_objects, report["schema_objects"]
                        )
                        self.assertRegex(report["digest"], r"^[0-9a-f]{16}$")
                        if case == "mixed-batch-rollback":
                            self.assertEqual(before, database.read_bytes())
                        else:
                            self.assertNotEqual(before, database.read_bytes())
                        for suffix in ("-journal", "-wal", "-shm"):
                            self.assertFalse(
                                pathlib.Path(f"{database}{suffix}").exists()
                            )
                        reports[engine] = report
                self.assertEqual(
                    reports["modern"]["digest"],
                    reports["sqlite"]["digest"],
                )
                self.assertEqual(
                    reports["modern"]["final_rows"],
                    reports["sqlite"]["final_rows"],
                )
                self.assertEqual(
                    reports["modern"]["changed_rows"],
                    reports["sqlite"]["changed_rows"],
                )

    def test_mixed_workload_splits_updates_deletes_and_inserts_evenly(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            reports: dict[str, dict[str, object]] = {}
            for engine in ("modern", "sqlite"):
                with self.subTest(engine=engine):
                    database = self.initial_database(
                        root, engine, "mixed-batch-commit"
                    )
                    completed = self.run_case(
                        engine,
                        "matched-durable",
                        "mixed-batch-commit",
                        database,
                        operations=12,
                    )
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    report = json.loads(completed.stdout)
                    self.assertEqual(12, report["changed_rows"])
                    self.assertEqual(65_536, report["final_rows"])
                    self.assertEqual(65_540, report["last_insert_rowid"])
                    reports[engine] = report
            self.assertEqual(
                reports["modern"]["digest"],
                reports["sqlite"]["digest"],
            )

    def test_engine_default_profile_is_reported_separately(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for engine in ("modern", "sqlite"):
                with self.subTest(engine=engine):
                    database = self.initial_database(
                        root, engine, "insert-point-implicit"
                    )
                    completed = self.run_case(
                        engine,
                        "engine-default",
                        "insert-point-implicit",
                        database,
                    )
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    report = json.loads(completed.stdout)
                    self.assertEqual("engine-default", report["profile"])

    def test_invalid_case_is_a_usage_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = pathlib.Path(temporary) / "input.db"
            shutil.copyfile(self.arguments.schema_fixture, database)
            completed = subprocess.run(
                [
                    self.arguments.binary,
                    "smoke",
                    "modern",
                    "matched-durable",
                    "missing-case",
                    database,
                    "8",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            self.assertEqual(1, completed.returncode)
            self.assertEqual("", completed.stdout)
            self.assertIn("unsupported write benchmark case", completed.stderr)


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[__file__])

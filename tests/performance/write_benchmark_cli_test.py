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
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    return parser.parse_args()


class WriteBenchmarkCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("write benchmark inputs were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS

    def run_case(
        self, engine: str, database: pathlib.Path
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                self.arguments.binary,
                "smoke",
                engine,
                "matched-durable",
                "insert-point-implicit",
                database,
                "8",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

    def test_matched_insert_smoke_completes_for_both_engines(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            reports: dict[str, dict[str, object]] = {}
            for engine in ("modern", "sqlite"):
                with self.subTest(engine=engine):
                    database = root / f"{engine}.db"
                    shutil.copyfile(self.arguments.fixture, database)
                    before = database.read_bytes()
                    completed = self.run_case(engine, database)
                    self.assertEqual(0, completed.returncode, completed.stderr)
                    self.assertEqual("", completed.stderr)
                    report = json.loads(completed.stdout)
                    self.assertEqual(
                        {
                            "case": "insert-point-implicit",
                            "changed_rows": 8,
                            "engine": engine,
                            "final_rows": 8,
                            "last_insert_rowid": 8,
                            "mode": "smoke",
                            "operations": 8,
                            "profile": "matched-durable",
                            "schema_version": 1,
                            "status": "complete",
                        },
                        {
                            key: report[key]
                            for key in (
                                "case",
                                "changed_rows",
                                "engine",
                                "final_rows",
                                "last_insert_rowid",
                                "mode",
                                "operations",
                                "profile",
                                "schema_version",
                                "status",
                            )
                        },
                    )
                    self.assertRegex(report["digest"], r"^[0-9a-f]{16}$")
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

    def test_invalid_case_is_a_usage_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = pathlib.Path(temporary) / "input.db"
            shutil.copyfile(self.arguments.fixture, database)
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

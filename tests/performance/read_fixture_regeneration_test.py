#!/usr/bin/env python3

import argparse
import os
import pathlib
import sys
import tempfile
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import read_performance

_COMMAND_LINE_ARGUMENTS: argparse.Namespace | None = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--workloads", type=pathlib.Path, required=True)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    return parser.parse_args()


class ReadFixtureRegenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("pinned SQLite inputs were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.workload_path = cls.arguments.workloads.resolve()
        cls.workloads = read_performance.validate_workload_manifest(
            read_performance.load_json_strict(cls.workload_path),
            repository_root=cls.root,
            manifest_path=cls.workload_path,
        )

    def test_fixtures_regenerate_byte_identically(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output_directory = pathlib.Path(temporary)
            for fixture in self.workloads["fixtures"]:
                with self.subTest(fixture=fixture["id"]):
                    output = output_directory / f"{fixture['id']}.db"
                    metadata = read_performance.create_fixture(
                        profile_path=self.arguments.profile.resolve(),
                        sqlite_library_path=(
                            self.arguments.sqlite_library.resolve()
                        ),
                        sqlite_c_path=self.arguments.sqlite_c.resolve(),
                        sqlite_h_path=self.arguments.sqlite_h.resolve(),
                        sql_path=(
                            self.root
                            / pathlib.PurePosixPath(fixture["sql_path"])
                        ),
                        fixture_id=fixture["id"],
                        output_path=output,
                    )
                    self.assertEqual(
                        fixture,
                        {
                            **metadata,
                            "path": fixture["path"],
                            "sql_path": fixture["sql_path"],
                        },
                    )
                    self.assertEqual(
                        (
                            self.root
                            / pathlib.PurePosixPath(fixture["path"])
                        ).read_bytes(),
                        output.read_bytes(),
                    )

            fit = self.workloads["fixtures"][0]
            fit_sql = self.root / pathlib.PurePosixPath(fit["sql_path"])
            blocked_output = output_directory / "blocked.db"
            blocked_sidecar = pathlib.Path(f"{blocked_output}-journal")
            os.symlink(output_directory / "missing-target", blocked_sidecar)
            with self.assertRaisesRegex(
                read_performance.HarnessError,
                "sidecar paths already exist",
            ):
                read_performance.create_fixture(
                    profile_path=self.arguments.profile.resolve(),
                    sqlite_library_path=(
                        self.arguments.sqlite_library.resolve()
                    ),
                    sqlite_c_path=self.arguments.sqlite_c.resolve(),
                    sqlite_h_path=self.arguments.sqlite_h.resolve(),
                    sql_path=fit_sql,
                    fixture_id="fit",
                    output_path=blocked_output,
                )
            self.assertTrue(os.path.lexists(blocked_sidecar))
            self.assertFalse(blocked_output.exists())

            noncanonical_sql = output_directory / "fit-noncanonical.sql"
            noncanonical_sql.write_text(
                fit_sql.read_text(encoding="utf-8").replace(
                    "v BLOB NOT NULL",
                    "v BLOB",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                read_performance.HarnessError,
                "schema contract",
            ):
                read_performance.create_fixture(
                    profile_path=self.arguments.profile.resolve(),
                    sqlite_library_path=(
                        self.arguments.sqlite_library.resolve()
                    ),
                    sqlite_c_path=self.arguments.sqlite_c.resolve(),
                    sqlite_h_path=self.arguments.sqlite_h.resolve(),
                    sql_path=noncanonical_sql,
                    fixture_id="fit",
                    output_path=output_directory / "noncanonical.db",
                )


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[sys.argv[0]])

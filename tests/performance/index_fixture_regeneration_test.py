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

from tools import index_performance


_COMMAND_LINE_ARGUMENTS: argparse.Namespace | None = None


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--profile", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    return parser.parse_args()


class IndexFixtureRegenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if _COMMAND_LINE_ARGUMENTS is None:
            raise unittest.SkipTest("pinned SQLite inputs were not supplied")
        cls.arguments = _COMMAND_LINE_ARGUMENTS
        cls.root = cls.arguments.repository_root.resolve()
        cls.sql = cls.root / "tests/fixtures/index_performance/indexed.sql"
        cls.fixture = cls.root / "tests/fixtures/index_performance/indexed.db"

    def test_fixture_regenerates_byte_identically(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "indexed.db"
            metadata = index_performance.create_fixture(
                profile_path=self.arguments.profile.resolve(),
                sqlite_library_path=self.arguments.sqlite_library.resolve(),
                sqlite_c_path=self.arguments.sqlite_c.resolve(),
                sqlite_h_path=self.arguments.sqlite_h.resolve(),
                sql_path=self.sql,
                output_path=output,
            )
            self.assertEqual("indexed", metadata["id"])
            self.assertEqual(206, metadata["page_count"])
            self.assertEqual(4096, metadata["row_count"])
            self.assertEqual(128, metadata["payload_size"])
            self.assertEqual(self.fixture.read_bytes(), output.read_bytes())

            blocked = pathlib.Path(temporary) / "blocked.db"
            blocked_sidecar = pathlib.Path(f"{blocked}-journal")
            os.symlink(pathlib.Path(temporary) / "missing", blocked_sidecar)
            with self.assertRaisesRegex(
                index_performance.HarnessError,
                "sidecar path",
            ):
                index_performance.create_fixture(
                    profile_path=self.arguments.profile.resolve(),
                    sqlite_library_path=self.arguments.sqlite_library.resolve(),
                    sqlite_c_path=self.arguments.sqlite_c.resolve(),
                    sqlite_h_path=self.arguments.sqlite_h.resolve(),
                    sql_path=self.sql,
                    output_path=blocked,
                )
            self.assertTrue(os.path.lexists(blocked_sidecar))
            self.assertFalse(blocked.exists())


if __name__ == "__main__":
    _COMMAND_LINE_ARGUMENTS = parse_arguments()
    unittest.main(argv=[sys.argv[0]])

#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--golden", type=pathlib.Path, required=True)
    return parser.parse_args()


class StorageInspectorCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.arguments = parse_arguments()

    def run_command(self, *arguments: pathlib.Path) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [self.arguments.binary, *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_clean_database_matches_exact_golden_output(self) -> None:
        completed = self.run_command(self.arguments.fixture)

        self.assertEqual(0, completed.returncode)
        self.assertEqual(b"", completed.stderr)
        self.assertEqual(self.arguments.golden.read_bytes(), completed.stdout)

    def test_structural_corruption_returns_exit_code_two(self) -> None:
        database = bytearray(self.arguments.fixture.read_bytes())
        struct.pack_into(">I", database, 36, 5)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "corrupt.db"
            path.write_bytes(database)

            completed = self.run_command(path)

        self.assertEqual(2, completed.returncode)
        self.assertEqual(b"", completed.stderr)
        report = json.loads(completed.stdout)
        self.assertFalse(report["ok"])
        self.assertTrue(any(issue["code"] == "freelist" for issue in report["issues"]))

    def test_usage_error_is_stderr_only(self) -> None:
        completed = self.run_command()

        self.assertEqual(1, completed.returncode)
        self.assertEqual(b"", completed.stdout)
        self.assertEqual(b"usage: modern_sqlite_inspect DATABASE\n", completed.stderr)

    def test_fatal_error_is_stable_json_on_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = pathlib.Path(directory) / "missing.db"
            completed = self.run_command(missing)

        self.assertEqual(1, completed.returncode)
        self.assertEqual(b"", completed.stderr)
        self.assertEqual(
            b'{"format_version":1,"ok":false,"fatal_error":{"code":"cannot_open"}}\n',
            completed.stdout,
        )

    def test_closed_stdout_returns_exit_code_one(self) -> None:
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            completed = subprocess.run(
                [self.arguments.binary, self.arguments.fixture],
                stdout=write_fd,
                stderr=subprocess.PIPE,
                check=False,
            )
        finally:
            os.close(write_fd)

        self.assertEqual(1, completed.returncode)
        self.assertEqual(b"", completed.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__])

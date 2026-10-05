#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import sys
import tempfile
import unittest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--oracle", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    return parser.parse_args()


class SqliteOracleRegenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.arguments = parse_arguments()

    def test_regeneration_matches_the_committed_oracle(self) -> None:
        with tempfile.TemporaryDirectory(
            dir=self.arguments.repository_root
        ) as temporary:
            generated = pathlib.Path(temporary) / "read-oracle-v1.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    self.arguments.tool,
                    "regenerate",
                    "--repository-root",
                    self.arguments.repository_root,
                    "--corpus",
                    self.arguments.corpus,
                    "--output",
                    generated,
                    "--sqlite-library",
                    self.arguments.sqlite_library,
                    "--sqlite-c",
                    self.arguments.sqlite_c,
                    "--sqlite-h",
                    self.arguments.sqlite_h,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )

            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertEqual(
                self.arguments.oracle.read_bytes(),
                generated.read_bytes(),
            )
            self.assertEqual("", completed.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__])

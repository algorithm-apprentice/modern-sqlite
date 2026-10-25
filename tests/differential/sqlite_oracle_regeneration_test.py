#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from tools import read_compatibility


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--oracle", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    parser.add_argument(
        "--sqlite-compiler-profile",
        choices=read_compatibility.ORACLE_COMPILER_PROFILES,
        default="gcc-clang",
    )
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
                    "--sqlite-compiler-profile",
                    self.arguments.sqlite_compiler_profile,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )

            self.assertEqual(0, completed.returncode, completed.stderr)
            expected_bytes = self.arguments.oracle.read_bytes()
            if self.arguments.sqlite_compiler_profile == "msvc":
                corpus = read_compatibility.validate_corpus(
                    self.arguments.repository_root, self.arguments.corpus
                )
                read_compatibility.validate_oracle(
                    self.arguments.repository_root,
                    self.arguments.oracle,
                    self.arguments.corpus,
                    corpus,
                )
                expected = read_compatibility.load_json_strict(self.arguments.oracle)
                profile = read_compatibility.validate_profile(
                    self.arguments.repository_root, corpus["oracle_profile_path"]
                )
                expected["sqlite"]["compile_options"] = (
                    read_compatibility.oracle_compile_options(profile, "msvc")
                )
                expected_path = pathlib.Path(temporary) / "expected-native-oracle.json"
                read_compatibility._write_json_atomic(expected_path, expected)
                expected_bytes = expected_path.read_bytes()
            self.assertEqual(
                expected_bytes,
                generated.read_bytes(),
            )
            self.assertEqual("", completed.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__])

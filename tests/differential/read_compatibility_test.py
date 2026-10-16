#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--runner", type=pathlib.Path, required=True)
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--corpus", type=pathlib.Path, required=True)
    parser.add_argument("--oracle", type=pathlib.Path, required=True)
    return parser.parse_args()


class ReadCompatibilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.arguments = parse_arguments()

    def test_committed_read_corpus_matches_pinned_sqlite(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                self.arguments.tool,
                "verify",
                "--repository-root",
                self.arguments.repository_root,
                "--corpus",
                self.arguments.corpus,
                "--oracle",
                self.arguments.oracle,
                "--runner",
                self.arguments.runner,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertRegex(
            completed.stdout,
            r"^verified [1-9][0-9]* compatibility cases? and "
            r"[1-9][0-9]* unsupported boundar(?:y|ies)\n$",
        )
        self.assertEqual("", completed.stderr)

    def test_newly_accepted_rejections_are_mismatches_not_harness_failures(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            corpus = json.loads(self.arguments.corpus.read_text())
            relative_paths = [
                pathlib.Path("tools/read_compatibility.py"),
                pathlib.Path(
                    "tests/compatibility/sqlite-oracle-profile-v1.json"
                ),
                pathlib.Path(
                    "tests/fixtures/read_compatibility/read-compatibility.sql"
                ),
            ]
            relative_paths.extend(
                pathlib.Path(database["path"])
                for database in corpus["databases"]
            )
            for relative in relative_paths:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(
                    self.arguments.repository_root / relative, destination
                )

            oracle = json.loads(self.arguments.oracle.read_text())
            oracle_cases = {case["id"]: case for case in oracle["cases"]}
            unsupported_controls = [
                case["id"]
                for case in corpus["cases"]
                if case["expectation"] == "unsupported"
                and case["modern_rejection"]["phase"] == "prepare"
                and oracle_cases[case["id"]]["boundary"]["prepare"]
                == "statement"
            ]
            prepare_error_controls = [
                case["id"]
                for case in corpus["cases"]
                if case["expectation"] == "compare"
                and oracle_cases[case["id"]]["outcome"]["kind"]
                == "prepare_error"
            ]
            self.assertTrue(
                unsupported_controls,
                "each corpus requires a native-valid unsupported prepare control",
            )
            self.assertTrue(
                prepare_error_controls,
                "each corpus requires an ordinary prepare-error control",
            )
            scenarios = [
                (unsupported_controls[0], "unsupported boundary regression"),
                (prepare_error_controls[0], "compatibility mismatch"),
            ]
            for case_id, classification in scenarios:
                with self.subTest(case_id=case_id):
                    modified_corpus = json.loads(json.dumps(corpus))
                    modified_cases = {
                        case["id"]: case for case in modified_corpus["cases"]
                    }
                    modified_cases[case_id]["sql_hex"] = b"SELECT 1".hex()
                    modified_cases[case_id]["operations"] = [
                        {"op": "finalize"}
                    ]

                    corpus_path = root / (
                        self.arguments.corpus.resolve().relative_to(
                            self.arguments.repository_root.resolve()
                        )
                    )
                    corpus_path.parent.mkdir(parents=True, exist_ok=True)
                    corpus_path.write_text(
                        json.dumps(
                            modified_corpus,
                            indent=2,
                            sort_keys=True,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    modified_oracle = json.loads(json.dumps(oracle))
                    modified_oracle["corpus_sha256"] = hashlib.sha256(
                        corpus_path.read_bytes()
                    ).hexdigest()
                    oracle_path = root / (
                        self.arguments.oracle.resolve().relative_to(
                            self.arguments.repository_root.resolve()
                        )
                    )
                    oracle_path.parent.mkdir(parents=True, exist_ok=True)
                    oracle_path.write_text(
                        json.dumps(
                            modified_oracle,
                            indent=2,
                            sort_keys=True,
                        )
                        + "\n",
                        encoding="utf-8",
                    )

                    completed = subprocess.run(
                        [
                            sys.executable,
                            self.arguments.tool,
                            "verify",
                            "--repository-root",
                            root,
                            "--corpus",
                            corpus_path,
                            "--oracle",
                            oracle_path,
                            "--runner",
                            self.arguments.runner,
                            "--case",
                            case_id,
                        ],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        check=False,
                    )

                    self.assertEqual(2, completed.returncode, completed.stderr)
                    self.assertIn(
                        f"{classification}: {case_id}",
                        completed.stderr,
                    )
                    self.assertNotIn("runner exited with code", completed.stderr)


if __name__ == "__main__":
    unittest.main(argv=[__file__])

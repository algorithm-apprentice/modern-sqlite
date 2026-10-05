#!/usr/bin/env python3

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tools import read_compatibility


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


class ReadCompatibilityValidationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

        generator = self.root / "tools/read_compatibility.py"
        generator.parent.mkdir(parents=True)
        generator.write_text("# synthetic generator\n", encoding="utf-8")

        fixture_sql = self.root / "tests/fixtures/read_compatibility/schema.sql"
        fixture_sql.parent.mkdir(parents=True)
        fixture_sql.write_text("CREATE TABLE items(id INTEGER PRIMARY KEY);\n")

        database = self.root / "tests/fixtures/read_compatibility/core.db"
        database.write_bytes(b"SQLite synthetic fixture")

        self.profile_path = (
            self.root / "tests/compatibility/sqlite-oracle-profile-v1.json"
        )
        write_json(
            self.profile_path,
            {
                "schema_version": 1,
                "sqlite": {
                    "version": "3.54.0",
                    "source_id": (
                        "2026-10-02 20:18:07 "
                        "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                    ),
                    "fossil_check_in": (
                        "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                    ),
                    "git_mirror_commit": "cb547ab3e931c7766e24834af5ef6c4578863e3d",
                    "sqlite3_c_sha256": "1" * 64,
                    "sqlite3_h_sha256": "2" * 64,
                },
                "build": {
                    "defines": [
                        "NDEBUG",
                        "SQLITE_DQS=3",
                        "SQLITE_THREADSAFE=0",
                    ],
                    "required_compile_options": ["DQS=3", "THREADSAFE=0"],
                    "forbidden_compile_options": [
                        "OMIT_DECLTYPE",
                        "OMIT_FLOATING_POINT",
                    ],
                    "semantic_compile_options": [
                        "DQS=3",
                        "THREADSAFE=0",
                    ],
                    "commands": {
                        "linux": "cc -shared sqlite3.c -o libsqlite3.so",
                        "macos": "cc -dynamiclib sqlite3.c -o libsqlite3.dylib",
                    },
                },
                "connection": {
                    "db_config": {
                        "DQS_DML": 1,
                        "DQS_DDL": 1,
                        "REVERSE_SCANORDER": 0,
                        "ENABLE_COMMENTS": 1,
                        "FP_DIGITS": 17,
                    },
                    "extended_result_codes": False,
                    "limits": {
                        "LENGTH": 1048576,
                        "SQL_LENGTH": 65536,
                        "COLUMN": 256,
                        "EXPR_DEPTH": 1000,
                        "VDBE_OP": 25000,
                        "FUNCTION_ARG": 1000,
                        "VARIABLE_NUMBER": 256,
                    },
                },
                "generator": {
                    "path": "tools/read_compatibility.py",
                    "sha256": sha256(generator),
                },
                "fixture_sources": [
                    {
                        "path": "tests/fixtures/read_compatibility/schema.sql",
                        "sha256": sha256(fixture_sql),
                    }
                ],
            },
        )

        self.corpus_path = self.root / "tests/compatibility/read-corpus-v1.json"
        write_json(
            self.corpus_path,
            {
                "schema_version": 1,
                "oracle_profile": (
                    "tests/compatibility/sqlite-oracle-profile-v1.json"
                ),
                "limits": {
                    "corpus_bytes": 4194304,
                    "oracle_bytes": 67108864,
                    "cases": 512,
                    "sql_bytes": 65536,
                    "operations": 1024,
                    "binding_operations": 256,
                    "result_columns": 256,
                    "result_rows": 4096,
                    "value_bytes": 1048576,
                    "output_bytes": 16777216,
                    "stderr_bytes": 1048576,
                    "child_seconds": 10,
                },
                "databases": [
                    {
                        "id": "core",
                        "path": "tests/fixtures/read_compatibility/core.db",
                        "sha256": sha256(database),
                    }
                ],
                "supported_matrix": ["constant-select", "typed-bindings"],
                "cases": [
                    {
                        "id": "constant-one",
                        "database": "core",
                        "feature": "constant-select",
                        "expectation": "compare",
                        "sql_hex": b"SELECT 1".hex(),
                        "operations": [
                            {"op": "step"},
                            {"op": "step"},
                            {"op": "finalize"},
                        ],
                    },
                    {
                        "id": "order-by-boundary",
                        "database": "core",
                        "feature": "order-by",
                        "expectation": "unsupported",
                        "sql_hex": b"SELECT id FROM items ORDER BY id".hex(),
                        "operations": [{"op": "finalize"}],
                        "modern_rejection": {
                            "phase": "prepare",
                            "primary_code": "generic",
                        },
                    },
                ],
            },
        )

        self.constant_outcome = {
            "format_version": 1,
            "kind": "statement",
            "preparation": {
                "next_offset": 8,
                "parameter_count": 0,
                "parameter_names": [],
                "columns": [{"name_hex": "31", "declared_type_hex": None}],
            },
            "observations": [
                {
                    "operation_index": 0,
                    "op": "step",
                    "result": "row",
                    "return_code": 100,
                    "columns": [{"name_hex": "31", "declared_type_hex": None}],
                    "row": [{"type": "integer", "value": "1"}],
                },
                {
                    "operation_index": 1,
                    "op": "step",
                    "result": "done",
                    "return_code": 101,
                },
                {
                    "operation_index": 2,
                    "op": "finalize",
                    "status": {
                        "primary_code": "ok",
                        "return_code": 0,
                        "extended_code": 0,
                    },
                },
            ],
        }
        self.unsupported_outcome = {
            "format_version": 1,
            "kind": "prepare_error",
            "status": {
                "primary_code": "generic",
                "return_code": 1,
                "extended_code": 1,
                "message_hex": b"unsupported syntax".hex(),
            },
        }
        self.runner = self.root / "fake_runner.py"
        self._write_runner(
            self.runner,
            {
                b"SELECT 1".hex(): self.constant_outcome,
                b"SELECT id FROM items ORDER BY id".hex(): self.unsupported_outcome,
            },
        )
        self.oracle_path = self.root / "tests/compatibility/read-oracle-v1.json"
        self._write_oracle(self.constant_outcome)

    def _write_runner(
        self, path: pathlib.Path, outcomes: dict[str, dict[str, object]], exit_code: int = 0
    ) -> None:
        path.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import sys\n"
            f"outcomes = {outcomes!r}\n"
            "protocol = sys.stdin.read()\n"
            "sql_line = next(line for line in protocol.splitlines() if line.startswith('SQL '))\n"
            "print(json.dumps(outcomes[sql_line[4:]], separators=(',', ':')))\n"
            f"raise SystemExit({exit_code})\n",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def _write_oracle(self, constant_outcome: dict[str, object]) -> None:
        profile = json.loads(self.profile_path.read_text())
        corpus = json.loads(self.corpus_path.read_text())
        write_json(
            self.oracle_path,
            {
                "schema_version": 1,
                "oracle_profile_path": (
                    "tests/compatibility/sqlite-oracle-profile-v1.json"
                ),
                "oracle_profile_schema_version": 1,
                "oracle_profile_sha256": sha256(self.profile_path),
                "corpus_path": "tests/compatibility/read-corpus-v1.json",
                "corpus_sha256": sha256(self.corpus_path),
                "generator": profile["generator"],
                "generator_format_version": 1,
                "sqlite": {
                    "version": profile["sqlite"]["version"],
                    "source_id": profile["sqlite"]["source_id"],
                    "sqlite3_c_sha256": profile["sqlite"]["sqlite3_c_sha256"],
                    "sqlite3_h_sha256": profile["sqlite"]["sqlite3_h_sha256"],
                    "compile_options": profile["build"][
                        "semantic_compile_options"
                    ],
                },
                "databases": corpus["databases"],
                "fixture_sources": profile["fixture_sources"],
                "cases": [
                    {
                        "id": "constant-one",
                        "expectation": "compare",
                        "outcome": constant_outcome,
                    },
                    {
                        "id": "order-by-boundary",
                        "expectation": "unsupported",
                        "boundary": {
                            "prepare": "statement",
                            "first_step": "row",
                        },
                    },
                ],
            },
        )

    def _verify(
        self,
        *,
        runner: pathlib.Path | None = None,
        case_id: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            "tools/read_compatibility.py",
            "verify",
            "--repository-root",
            str(self.root),
            "--corpus",
            str(self.corpus_path),
            "--oracle",
            str(self.oracle_path),
            "--runner",
            str(runner if runner is not None else self.runner),
        ]
        if case_id is not None:
            command.extend(["--case", case_id])
        return subprocess.run(
            command,
            cwd=pathlib.Path(__file__).resolve().parents[2],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_strict_json_rejects_duplicate_object_keys(self) -> None:
        duplicate = self.root / "duplicate.json"
        duplicate.write_text('{"schema_version":1,"schema_version":1}\n')

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "duplicate JSON key: schema_version"
        ):
            read_compatibility.load_json_strict(duplicate)

    def test_valid_profile_and_corpus_publish_normalized_paths(self) -> None:
        profile = read_compatibility.validate_profile(
            self.root, self.profile_path
        )
        corpus = read_compatibility.validate_corpus(self.root, self.corpus_path)

        self.assertEqual("3.54.0", profile["sqlite"]["version"])
        self.assertEqual(self.profile_path.resolve(), corpus["oracle_profile_path"])
        self.assertEqual(
            (self.root / "tests/fixtures/read_compatibility/core.db").resolve(),
            corpus["database_paths"]["core"],
        )
        self.assertEqual(["constant-one", "order-by-boundary"], corpus["case_ids"])

    def test_stale_database_hash_is_rejected_before_execution(self) -> None:
        database = self.root / "tests/fixtures/read_compatibility/core.db"
        database.write_bytes(b"changed")

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "database hash mismatch: core"
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_transcript_requires_final_finalize_and_valid_typed_values(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][0]["operations"] = [
            {
                "op": "bind",
                "index": 1,
                "value": {"type": "real", "bits": "not-real-bits"},
            },
            {"op": "step"},
        ]
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case constant-one: REAL bits must contain 16 hexadecimal digits",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_bind_index_must_fit_the_sqlite_c_int_contract(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][0]["operations"] = [
            {
                "op": "bind",
                "index": 2_147_483_648,
                "value": {"type": "integer", "value": "1"},
            },
            {"op": "finalize"},
        ]
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case constant-one: operation 0 index exceeds SQLite C int range",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_rejected_statement_requires_a_runnable_fallback_transcript(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][1]["operations"] = []
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case order-by-boundary: rejected statement requires a FINALIZE fallback",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_prepare_error_requires_a_runnable_fallback_transcript(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][0]["feature"] = "prepare-errors"
        corpus["cases"][0]["sql_hex"] = b"SELECT )".hex()
        corpus["cases"][0]["operations"] = []
        corpus["supported_matrix"].append("prepare-errors")
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case constant-one: rejected statement requires a FINALIZE fallback",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_unsupported_case_requires_a_coarse_modern_rejection(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        del corpus["cases"][1]["modern_rejection"]
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case order-by-boundary: unsupported case requires modern_rejection",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_profile_rejects_stale_generator_hash(self) -> None:
        generator = self.root / "tools/read_compatibility.py"
        generator.write_text("# changed generator\n", encoding="utf-8")

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "generator hash mismatch"
        ):
            read_compatibility.validate_profile(self.root, self.profile_path)

    def test_oracle_requires_the_exact_semantic_compile_options(self) -> None:
        oracle = json.loads(self.oracle_path.read_text())
        oracle["sqlite"]["compile_options"].append("OMIT_AUTORESET")
        write_json(self.oracle_path, oracle)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "oracle compile options do not match the pinned semantic profile",
        ):
            read_compatibility.validate_oracle(
                self.root,
                self.oracle_path,
                self.corpus_path,
                read_compatibility.validate_corpus(
                    self.root, self.corpus_path
                ),
            )

    def test_verify_accepts_matching_outcomes_and_ignores_diagnostic_codes(self) -> None:
        oracle_outcome = json.loads(json.dumps(self.constant_outcome))
        oracle_outcome["observations"][2]["status"].update(
            {
                "return_code": 999,
                "extended_code": 999,
                "message_hex": b"diagnostic only".hex(),
            }
        )
        self._write_oracle(oracle_outcome)

        completed = self._verify()

        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual(
            "verified 1 compatibility case and 1 unsupported boundary\n",
            completed.stdout,
        )
        self.assertEqual("", completed.stderr)

    def test_verify_reports_first_difference_and_reproduction_protocol(self) -> None:
        wrong = json.loads(json.dumps(self.constant_outcome))
        wrong["observations"][0]["row"][0]["value"] = "2"
        self._write_oracle(wrong)

        completed = self._verify(case_id="constant-one")

        self.assertEqual(2, completed.returncode)
        self.assertEqual("", completed.stdout)
        self.assertIn("compatibility mismatch: constant-one", completed.stderr)
        self.assertIn(
            "first difference: $.observations[0].row[0].value", completed.stderr
        )
        self.assertIn("reproduce:", completed.stderr)
        self.assertIn("MSRT1", completed.stderr)

    def test_verify_rejects_nonzero_child_even_with_valid_json(self) -> None:
        failing_runner = self.root / "failing_runner.py"
        self._write_runner(
            failing_runner,
            {
                b"SELECT 1".hex(): self.constant_outcome,
                b"SELECT id FROM items ORDER BY id".hex(): self.unsupported_outcome,
            },
            exit_code=3,
        )

        completed = self._verify(runner=failing_runner, case_id="constant-one")

        self.assertEqual(1, completed.returncode)
        self.assertIn("runner exited with code 3", completed.stderr)

    def test_verify_reports_unsupported_acceptance_separately(self) -> None:
        accepting_runner = self.root / "accepting_runner.py"
        self._write_runner(
            accepting_runner,
            {
                b"SELECT 1".hex(): self.constant_outcome,
                b"SELECT id FROM items ORDER BY id".hex(): self.constant_outcome,
            },
        )

        completed = self._verify(
            runner=accepting_runner, case_id="order-by-boundary"
        )

        self.assertEqual(2, completed.returncode)
        self.assertIn(
            "unsupported boundary regression: order-by-boundary", completed.stderr
        )
        self.assertNotIn("compatibility mismatch", completed.stderr)

    def test_verify_rejects_unknown_case_before_launching_runner(self) -> None:
        completed = self._verify(case_id="missing")

        self.assertEqual(1, completed.returncode)
        self.assertIn("unknown compatibility case: missing", completed.stderr)

    def test_command_line_usage_errors_return_exit_code_one(self) -> None:
        completed = subprocess.run(
            [sys.executable, read_compatibility.__file__, "verify"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        self.assertEqual(1, completed.returncode)
        self.assertEqual("", completed.stdout)
        self.assertIn("read compatibility error:", completed.stderr)
        self.assertIn("the following arguments are required:", completed.stderr)

    def test_regenerate_writes_a_deterministic_valid_oracle(self) -> None:
        class FakePinnedSqliteOracle:
            compile_options = ("DQS=3", "THREADSAFE=0")

            def run_case(
                self,
                path: pathlib.Path,
                sql: bytes,
                operations: list[dict[str, object]],
            ) -> dict[str, object]:
                self_path = (
                    self_outer.root
                    / "tests/fixtures/read_compatibility/core.db"
                ).resolve()
                self_outer.assertEqual(self_path, path)
                self_outer.assertEqual(b"SELECT 1", sql)
                self_outer.assertEqual(
                    [{"op": "step"}, {"op": "step"}, {"op": "finalize"}],
                    operations,
                )
                return self_outer.constant_outcome

            def run_boundary(
                self, path: pathlib.Path, sql: bytes
            ) -> dict[str, object]:
                self_outer.assertEqual(
                    (
                        self_outer.root
                        / "tests/fixtures/read_compatibility/core.db"
                    ).resolve(),
                    path,
                )
                self_outer.assertEqual(
                    b"SELECT id FROM items ORDER BY id", sql
                )
                return {"prepare": "statement", "first_step": "row"}

        self_outer = self
        output = self.root / "tests/compatibility/generated-oracle.json"
        sqlite_library = self.root / "libsqlite3.so"
        sqlite_c = self.root / "sqlite3.c"
        sqlite_h = self.root / "sqlite3.h"
        for path in (sqlite_library, sqlite_c, sqlite_h):
            path.write_bytes(b"synthetic")

        with mock.patch.object(
            read_compatibility,
            "PinnedSqliteOracle",
            return_value=FakePinnedSqliteOracle(),
        ):
            counts = read_compatibility.regenerate_oracle(
                self.root,
                self.corpus_path,
                output,
                sqlite_library,
                sqlite_c,
                sqlite_h,
            )
            first = output.read_bytes()
            repeated_counts = read_compatibility.regenerate_oracle(
                self.root,
                self.corpus_path,
                output,
                sqlite_library,
                sqlite_c,
                sqlite_h,
            )

        self.assertEqual((1, 1), counts)
        self.assertEqual(counts, repeated_counts)
        self.assertEqual(first, output.read_bytes())
        oracle = read_compatibility.validate_oracle(
            self.root,
            output,
            self.corpus_path,
            read_compatibility.validate_corpus(self.root, self.corpus_path),
        )
        self.assertEqual(
            ["constant-one", "order-by-boundary"],
            [case["id"] for case in oracle["cases"]],
        )

    def test_regenerate_refuses_to_overwrite_a_corpus_provenance_input(self) -> None:
        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "oracle output must not overwrite a provenance input",
        ):
            read_compatibility.regenerate_oracle(
                self.root,
                self.corpus_path,
                self.corpus_path,
                self.root / "libsqlite3.so",
                self.root / "sqlite3.c",
                self.root / "sqlite3.h",
            )

    def test_regenerate_refuses_to_overwrite_sqlite_provenance_inputs(self) -> None:
        sqlite_library = self.root / "libsqlite3.so"
        sqlite_c = self.root / "sqlite3.c"
        sqlite_h = self.root / "sqlite3.h"
        for path in (sqlite_library, sqlite_c, sqlite_h):
            path.write_bytes(b"synthetic")

        for output in (sqlite_library, sqlite_c, sqlite_h):
            with self.subTest(output=output):
                with mock.patch.object(
                    read_compatibility,
                    "PinnedSqliteOracle",
                    side_effect=AssertionError(
                        "oracle must not load an overwritten provenance input"
                    ),
                ):
                    with self.assertRaisesRegex(
                        read_compatibility.HarnessError,
                        "oracle output must not overwrite a provenance input",
                    ):
                        read_compatibility.regenerate_oracle(
                            self.root,
                            self.corpus_path,
                            output,
                            sqlite_library,
                            sqlite_c,
                            sqlite_h,
                        )

    def test_regenerate_refuses_sqlite_provenance_path_aliases(self) -> None:
        sqlite_library = self.root / "libsqlite3.so"
        sqlite_c = self.root / "sqlite3.c"
        sqlite_h = self.root / "sqlite3.h"
        for path in (sqlite_library, sqlite_c, sqlite_h):
            path.write_bytes(b"synthetic")

        aliases: list[pathlib.Path] = []
        for path in (sqlite_library, sqlite_c, sqlite_h):
            alias = path.with_name(path.name.upper())
            if not alias.exists():
                alias.hardlink_to(path)
            aliases.append(alias)

        for output in aliases:
            with self.subTest(output=output):
                with mock.patch.object(
                    read_compatibility,
                    "PinnedSqliteOracle",
                    side_effect=AssertionError(
                        "oracle must not load an aliased provenance input"
                    ),
                ):
                    with self.assertRaisesRegex(
                        read_compatibility.HarnessError,
                        "oracle output must not overwrite a provenance input",
                    ):
                        read_compatibility.regenerate_oracle(
                            self.root,
                            self.corpus_path,
                            output,
                            sqlite_library,
                            sqlite_c,
                            sqlite_h,
                        )


if __name__ == "__main__":
    unittest.main()

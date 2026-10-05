#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from tools import read_compatibility


def parse_arguments() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    parser.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    arguments, remaining = parser.parse_known_args()
    return arguments, [sys.argv[0], *remaining]


arguments, unittest_argv = parse_arguments()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def oracle_profile(arguments: argparse.Namespace) -> dict[str, object]:
    profile_path = (
        pathlib.Path(__file__).resolve().parents[1]
        / "compatibility/sqlite-oracle-profile-v1.json"
    )
    profile = json.loads(profile_path.read_text())
    profile["sqlite"]["sqlite3_c_sha256"] = sha256(arguments.sqlite_c)
    profile["sqlite"]["sqlite3_h_sha256"] = sha256(arguments.sqlite_h)
    return profile


class SqliteOracleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.arguments = arguments
        cls.oracle = read_compatibility.PinnedSqliteOracle(
            cls.arguments.sqlite_library,
            oracle_profile(cls.arguments),
            cls.arguments.sqlite_c,
            cls.arguments.sqlite_h,
        )

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.database = pathlib.Path(self.temporary.name) / "oracle.db"
        self.oracle.create_database(
            self.database,
            """
            PRAGMA page_size=512;
            CREATE TABLE values_table(
              id INTEGER PRIMARY KEY,
              text_value TEXT,
              blob_value BLOB,
              real_value REAL
            );
            INSERT INTO values_table
            VALUES(1, CAST(X'610062' AS TEXT), X'', 1.5);
            """,
        )

    def test_rejects_wrong_source_identity(self) -> None:
        profile = oracle_profile(self.arguments)
        profile["sqlite"]["source_id"] = "wrong"

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "SQLite source ID mismatch"
        ):
            read_compatibility.PinnedSqliteOracle(
                self.arguments.sqlite_library,
                profile,
                self.arguments.sqlite_c,
                self.arguments.sqlite_h,
            )

    def test_rejects_semantic_compile_option_drift_and_omits_compiler_identity(
        self,
    ) -> None:
        self.assertFalse(
            any(
                option.startswith("COMPILER=")
                for option in self.oracle.compile_options
            )
        )
        profile = oracle_profile(self.arguments)
        profile["build"]["semantic_compile_options"] = [
            *self.oracle.compile_options,
            "OMIT_AUTORESET",
        ]

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "SQLite semantic compile options do not match the pinned profile",
        ):
            read_compatibility.PinnedSqliteOracle(
                self.arguments.sqlite_library,
                profile,
                self.arguments.sqlite_c,
                self.arguments.sqlite_h,
            )

    def test_extracts_tail_metadata_nul_text_empty_blob_and_real_bits(self) -> None:
        sql = (
            b"SELECT text_value, blob_value, real_value FROM values_table;"
            b" SELECT 2"
        )
        outcome = self.oracle.run_case(
            self.database,
            sql,
            [{"op": "step"}, {"op": "step"}, {"op": "finalize"}],
        )

        self.assertEqual("statement", outcome["kind"])
        self.assertEqual(sql.index(b";") + 1, outcome["preparation"]["next_offset"])
        self.assertEqual(
            [
                {"name_hex": b"text_value".hex(), "declared_type_hex": b"TEXT".hex()},
                {"name_hex": b"blob_value".hex(), "declared_type_hex": b"BLOB".hex()},
                {"name_hex": b"real_value".hex(), "declared_type_hex": b"REAL".hex()},
            ],
            outcome["preparation"]["columns"],
        )
        self.assertEqual(
            [
                {"type": "text", "hex": "610062"},
                {"type": "blob", "hex": ""},
                {"type": "real", "bits": "3ff8000000000000"},
            ],
            outcome["observations"][0]["row"],
        )

    def test_records_rebinding_range_and_typed_parameter_values(self) -> None:
        outcome = self.oracle.run_case(
            self.database,
            b"SELECT ?1, typeof(?1)",
            [
                {
                    "op": "bind",
                    "index": 0,
                    "value": {"type": "integer", "value": "1"},
                },
                {
                    "op": "bind",
                    "index": 1,
                    "value": {"type": "text", "hex": "610062"},
                },
                {"op": "step"},
                {"op": "step"},
                {"op": "finalize"},
            ],
        )

        self.assertEqual(
            "out_of_range",
            outcome["observations"][0]["status"]["primary_code"],
        )
        self.assertEqual(
            [
                {"type": "text", "hex": "610062"},
                {"type": "text", "hex": b"text".hex()},
            ],
            outcome["observations"][2]["row"],
        )

    def test_rejects_bind_indices_that_do_not_fit_sqlite_c_int(self) -> None:
        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "bind index exceeds SQLite C int range",
        ):
            self.oracle.run_case(
                self.database,
                b"SELECT ?1",
                [
                    {
                        "op": "bind",
                        "index": 4_294_967_297,
                        "value": {"type": "integer", "value": "7"},
                    },
                    {"op": "finalize"},
                ],
            )

    def test_preserves_step_reset_and_finalize_error_precedence(self) -> None:
        reset = self.oracle.run_case(
            self.database,
            b"SELECT abs(-9223372036854775808)",
            [{"op": "step"}, {"op": "reset"}, {"op": "finalize"}],
        )
        self.assertEqual(
            ["generic", "generic", "ok"],
            [
                observation["status"]["primary_code"]
                for observation in reset["observations"]
            ],
        )

        direct = self.oracle.run_case(
            self.database,
            b"SELECT abs(-9223372036854775808)",
            [{"op": "step"}, {"op": "finalize"}],
        )
        self.assertEqual(
            ["generic", "generic"],
            [
                observation["status"]["primary_code"]
                for observation in direct["observations"]
            ],
        )

    def test_prepare_error_empty_statement_and_boundary_record_are_stable(self) -> None:
        failed = self.oracle.run_case(self.database, b"SELECT )", [])
        self.assertEqual("prepare_error", failed["kind"])
        self.assertEqual("generic", failed["status"]["primary_code"])

        empty = self.oracle.run_case(self.database, b" ; -- empty\n ;", [])
        self.assertEqual(
            {"format_version": 1, "kind": "empty", "next_offset": 14}, empty
        )

        boundary = self.oracle.run_boundary(self.database, b"SELECT CURRENT_DATE")
        self.assertEqual("statement", boundary["prepare"])
        self.assertEqual("row", boundary["first_step"])
        self.assertNotIn("row", boundary)


if __name__ == "__main__":
    unittest.main(argv=unittest_argv)

#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import subprocess
import unittest


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    return parser.parse_args()


def sql_hex(sql: bytes) -> str:
    return sql.hex()


def transcript(sql: bytes, *operations: str) -> bytes:
    lines = ["MSRT1", f"SQL {sql_hex(sql)}", *operations]
    return ("\n".join(lines) + "\n").encode("ascii")


class ReadTraceCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.arguments = parse_arguments()

    def run_command(
        self, input_bytes: bytes, database: pathlib.Path | None = None
    ) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [
                self.arguments.binary,
                database if database is not None else self.arguments.fixture,
            ],
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_constant_statement_records_each_lifecycle_operation(self) -> None:
        completed = self.run_command(
            transcript(b"SELECT 1", "STEP", "STEP", "FINALIZE")
        )

        self.assertEqual(0, completed.returncode)
        self.assertEqual(b"", completed.stderr)
        self.assertEqual(
            {
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
                        "columns": [
                            {"name_hex": "31", "declared_type_hex": None}
                        ],
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
            },
            json.loads(completed.stdout),
        )

    def test_rebinding_and_out_of_range_bind_are_engine_observations(self) -> None:
        completed = self.run_command(
            transcript(
                b"SELECT ?1",
                "BIND 0 integer 1",
                "BIND 1 integer 7",
                "BIND 1 integer 9",
                "STEP",
                "STEP",
                "FINALIZE",
            )
        )

        self.assertEqual(0, completed.returncode)
        self.assertEqual(b"", completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual("statement", report["kind"])
        self.assertEqual(["3f31"], report["preparation"]["parameter_names"])
        status = report["observations"][0]["status"]
        self.assertEqual("out_of_range", status["primary_code"])
        self.assertEqual(25, status["return_code"])
        self.assertEqual(25, status["extended_code"])
        self.assertEqual(
            "parameter index is out of range",
            bytes.fromhex(status["message_hex"]).decode("utf-8"),
        )
        self.assertEqual(
            [{"type": "integer", "value": "9"}],
            report["observations"][3]["row"],
        )

    def test_step_reset_and_finalize_preserve_distinct_error_results(self) -> None:
        completed = self.run_command(
            transcript(
                b"SELECT abs(-9223372036854775808)",
                "STEP",
                "RESET",
                "FINALIZE",
            )
        )

        self.assertEqual(0, completed.returncode)
        self.assertEqual(b"", completed.stderr)
        observations = json.loads(completed.stdout)["observations"]
        self.assertEqual("error", observations[0]["result"])
        self.assertEqual("generic", observations[0]["status"]["primary_code"])
        self.assertEqual("generic", observations[1]["status"]["primary_code"])
        self.assertEqual("ok", observations[2]["status"]["primary_code"])

    def test_empty_input_and_open_failure_are_trace_outcomes(self) -> None:
        empty = self.run_command(transcript(b" ; -- empty\n ;"))
        self.assertEqual(0, empty.returncode)
        self.assertEqual(b"", empty.stderr)
        self.assertEqual(
            {"format_version": 1, "kind": "empty", "next_offset": 14},
            json.loads(empty.stdout),
        )

        missing = self.arguments.fixture.parent / "missing-read-trace.db"
        failed = self.run_command(
            transcript(b"SELECT 1", "STEP", "STEP", "FINALIZE"), missing
        )
        self.assertEqual(0, failed.returncode)
        self.assertEqual(b"", failed.stderr)
        report = json.loads(failed.stdout)
        self.assertEqual("open_error", report["kind"])
        self.assertEqual("cannot_open", report["status"]["primary_code"])

    def test_malformed_protocol_is_stderr_only(self) -> None:
        completed = self.run_command(b"MSRT1\nSQL 00xz\n")

        self.assertEqual(1, completed.returncode)
        self.assertEqual(b"", completed.stdout)
        self.assertEqual(
            b"invalid read trace protocol: SQL payload is not hexadecimal\n",
            completed.stderr,
        )

    def test_bind_index_above_sqlite_c_int_range_is_a_protocol_error(self) -> None:
        completed = self.run_command(
            transcript(
                b"SELECT ?1",
                "BIND 2147483648 integer 7",
                "FINALIZE",
            )
        )

        self.assertEqual(1, completed.returncode)
        self.assertEqual(b"", completed.stdout)
        self.assertEqual(
            b"invalid read trace protocol: "
            b"BIND index exceeds SQLite C int range\n",
            completed.stderr,
        )

    def test_usage_error_is_stderr_only(self) -> None:
        completed = subprocess.run(
            [self.arguments.binary],
            input=b"",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        self.assertEqual(1, completed.returncode)
        self.assertEqual(b"", completed.stdout)
        self.assertEqual(
            b"usage: modern_sqlite_read_trace DATABASE\n", completed.stderr
        )

    def test_closed_stdout_returns_exit_code_one(self) -> None:
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            completed = subprocess.run(
                [self.arguments.binary, self.arguments.fixture],
                input=transcript(b"SELECT 1", "STEP", "STEP", "FINALIZE"),
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

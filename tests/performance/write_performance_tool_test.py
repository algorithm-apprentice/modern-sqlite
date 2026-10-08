#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/write_performance.py"


CASES = (
    (
        "create-table-implicit",
        "create",
        "zero",
        "statement",
        ["CREATE TABLE tNNN(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')"],
        256,
        256,
        0,
    ),
    (
        "insert-point-implicit",
        "insert_point",
        "schema",
        "row",
        ["INSERT INTO kv(k,v,version) VALUES(?1,?2,0)"],
        512,
        512,
        512,
    ),
    (
        "insert-batch-explicit",
        "insert_batch",
        "schema",
        "row",
        ["BEGIN", "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)", "COMMIT"],
        1,
        65_536,
        65_536,
    ),
    (
        "update-point-implicit",
        "update_point",
        "populated",
        "row",
        ["UPDATE kv SET v=?1,version=version+1 WHERE k=?2"],
        512,
        512,
        512,
    ),
    (
        "update-scan-implicit",
        "update_scan",
        "populated",
        "row",
        ["UPDATE kv SET v=?1,version=version+1 WHERE k>=1"],
        1,
        1,
        65_536,
    ),
    (
        "delete-point-implicit",
        "delete_point",
        "populated",
        "row",
        ["DELETE FROM kv WHERE k=?1"],
        512,
        512,
        512,
    ),
    (
        "delete-scan-implicit",
        "delete_scan",
        "populated",
        "row",
        ["DELETE FROM kv WHERE k>=1"],
        1,
        1,
        65_536,
    ),
    (
        "mixed-batch-commit",
        "mixed_commit",
        "populated",
        "row_mutation",
        [
            "BEGIN",
            "UPDATE kv SET v=?1,version=version+1 WHERE k=?2",
            "DELETE FROM kv WHERE k=?1",
            "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)",
            "COMMIT",
        ],
        1,
        12_288,
        12_288,
    ),
    (
        "mixed-batch-rollback",
        "mixed_rollback",
        "populated",
        "row_mutation",
        [
            "BEGIN",
            "UPDATE kv SET v=?1,version=version+1 WHERE k=?2",
            "DELETE FROM kv WHERE k=?1",
            "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)",
            "ROLLBACK",
        ],
        1,
        12_288,
        12_288,
    ),
)


def valid_manifest() -> dict[str, object]:
    digest = "1" * 16
    sha256 = "a" * 64
    return {
        "schema_version": 1,
        "workload_semantics_version": 1,
        "sqlite_profile": "sqlite-oracle-profile-v1",
        "configuration": {
            "page_size": 4096,
            "cache_pages": 512,
            "mmap_bytes": 0,
            "temp_store": "memory",
            "journal_mode": "delete",
            "matched_synchronous": "full",
            "locking_mode": "normal",
            "thread_mode": "single",
        },
        "profiles": [
            {
                "id": "engine-default",
                "guard": False,
                "sqlite_configuration": "default",
            },
            {
                "id": "matched-durable",
                "guard": True,
                "sqlite_configuration": "explicit",
            },
        ],
        "rounds": [
            {
                "index": 0,
                "case_order": "canonical",
                "engine_order": ["modern", "sqlite"],
            },
            {
                "index": 1,
                "case_order": "reverse",
                "engine_order": ["sqlite", "modern"],
            },
            {
                "index": 2,
                "case_order": "canonical",
                "engine_order": ["modern", "sqlite"],
            },
        ],
        "timing_repetitions": 3,
        "minimum_wall_ns": 20_000_000,
        "fixtures": [
            {
                "id": "schema",
                "path": "tests/fixtures/write_performance/schema.db",
                "sql_path": "tests/fixtures/write_performance/schema.sql",
                "sha256": sha256,
                "sql_sha256": sha256,
                "size_bytes": 8192,
                "page_size": 4096,
                "page_count": 2,
                "row_count": 0,
                "value_size": 256,
                "content_digest": digest,
            },
            {
                "id": "populated",
                "path": "tests/fixtures/write_performance/populated.db",
                "sql_path": "tests/fixtures/write_performance/populated.sql",
                "sha256": sha256,
                "sql_sha256": sha256,
                "size_bytes": 17_952_768,
                "page_size": 4096,
                "page_count": 4383,
                "row_count": 65_536,
                "value_size": 256,
                "content_digest": digest,
            },
        ],
        "cases": [
            {
                "id": case_id,
                "kind": kind,
                "fixture": fixture,
                "primary_unit": primary_unit,
                "sql": list(sql),
                "transactions": transactions,
                "dml_operations": dml_operations,
                "row_mutations": row_mutations,
            }
            for (
                case_id,
                kind,
                fixture,
                primary_unit,
                sql,
                transactions,
                dml_operations,
                row_mutations,
            ) in CASES
        ],
        "guard": {
            "profile": "matched-durable",
            "maximum_wall_ratio": {"numerator": 10, "denominator": 1},
            "maximum_cpu_ratio": {"numerator": 10, "denominator": 1},
        },
        "key_order": {
            "algorithm": "splitmix64-rejection-fisher-yates-v1",
            "seed": "d1b54a32d192ed03",
        },
    }


class WritePerformanceToolTest(unittest.TestCase):
    def run_tool(
        self, manifest_text: str | None, *extra: str
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            manifest = pathlib.Path(temporary) / "workloads.json"
            if manifest_text is not None:
                manifest.write_text(manifest_text, encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    TOOL,
                    "validate-workloads",
                    "--workloads",
                    manifest,
                    *extra,
                ],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )

    def test_accepts_the_canonical_manifest_shape(self) -> None:
        completed = self.run_tool(
            json.dumps(valid_manifest(), sort_keys=True)
        )
        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual(
            "validated 9 write performance cases and 2 profiles\n",
            completed.stdout,
        )
        self.assertEqual("", completed.stderr)

    def test_rejects_duplicate_json_keys(self) -> None:
        manifest = json.dumps(valid_manifest(), sort_keys=True)
        manifest = manifest.replace(
            '"schema_version": 1',
            '"schema_version": 1, "schema_version": 1',
            1,
        )
        completed = self.run_tool(manifest)
        self.assertEqual(1, completed.returncode)
        self.assertIn("duplicate JSON key: schema_version", completed.stderr)

    def test_rejects_unknown_fields_and_boolean_counts(self) -> None:
        for mutate, expected in (
            (
                lambda value: value.__setitem__("unknown", 1),
                "workload manifest has invalid keys",
            ),
            (
                lambda value: value["cases"][0].__setitem__(
                    "transactions", True
                ),
                "case create-table-implicit.transactions must be an integer",
            ),
            (
                lambda value: value.__setitem__("schema_version", True),
                "schema_version must be an integer",
            ),
            (
                lambda value: value["profiles"][0].__setitem__("guard", 0),
                "profiles[0].guard must be bool",
            ),
        ):
            with self.subTest(expected=expected):
                manifest = valid_manifest()
                mutate(manifest)
                completed = self.run_tool(json.dumps(manifest))
                self.assertEqual(1, completed.returncode)
                self.assertIn(expected, completed.stderr)

    def test_requires_exact_profiles_rounds_and_guard(self) -> None:
        mutations = (
            (
                lambda value: value["profiles"].reverse(),
                "profiles must be engine-default then matched-durable",
            ),
            (
                lambda value: value["rounds"][1].__setitem__(
                    "engine_order", ["modern", "sqlite"]
                ),
                "round 1 has the wrong engine order",
            ),
            (
                lambda value: value["guard"].__setitem__(
                    "profile", "engine-default"
                ),
                "guard.profile must be matched-durable",
            ),
        )
        for mutate, expected in mutations:
            with self.subTest(expected=expected):
                manifest = valid_manifest()
                mutate(manifest)
                completed = self.run_tool(json.dumps(manifest))
                self.assertEqual(1, completed.returncode)
                self.assertIn(expected, completed.stderr)

    def test_requires_every_immutable_case_exactly_once(self) -> None:
        manifest = valid_manifest()
        manifest["cases"].pop()
        completed = self.run_tool(json.dumps(manifest))
        self.assertEqual(1, completed.returncode)
        self.assertIn("case IDs do not match workload semantics version 1", completed.stderr)

        manifest = valid_manifest()
        manifest["cases"][0]["sql"][0] += " "
        completed = self.run_tool(json.dumps(manifest))
        self.assertEqual(1, completed.returncode)
        self.assertIn("case create-table-implicit.sql is not canonical", completed.stderr)

    def test_requires_the_pinned_key_order(self) -> None:
        manifest = valid_manifest()
        manifest["key_order"]["seed"] = "0000000000000000"
        completed = self.run_tool(json.dumps(manifest))
        self.assertEqual(1, completed.returncode)
        self.assertIn("key_order is not canonical", completed.stderr)

    def test_usage_and_missing_file_are_harness_failures(self) -> None:
        completed = subprocess.run(
            [sys.executable, TOOL],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        self.assertEqual(1, completed.returncode)

        completed = self.run_tool(None)
        self.assertEqual(1, completed.returncode)
        self.assertIn("workload manifest does not exist", completed.stderr)


if __name__ == "__main__":
    unittest.main()

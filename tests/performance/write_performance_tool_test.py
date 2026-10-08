#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from copy import deepcopy


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/write_performance.py"
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import write_performance


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

EXPECTED_WORK = {
    "create-table-implicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 0,
            "changed_rows": 0,
            "final_rows": 0,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "424687ee21154511",
        },
        "baseline": {
            "transactions": 256,
            "dml_operations": 256,
            "row_mutations": 0,
            "changed_rows": 0,
            "final_rows": 0,
            "last_insert_rowid": 0,
            "schema_objects": 256,
            "digest": "67829c991b8509a9",
        },
    },
    "insert-point-implicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 1,
            "changed_rows": 1,
            "final_rows": 1,
            "last_insert_rowid": 1,
            "schema_objects": 1,
            "digest": "e0552c11f499c717",
        },
        "baseline": {
            "transactions": 512,
            "dml_operations": 512,
            "row_mutations": 512,
            "changed_rows": 512,
            "final_rows": 512,
            "last_insert_rowid": 512,
            "schema_objects": 1,
            "digest": "2138add81b82284d",
        },
    },
    "insert-batch-explicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 8,
            "row_mutations": 8,
            "changed_rows": 8,
            "final_rows": 8,
            "last_insert_rowid": 8,
            "schema_objects": 1,
            "digest": "d0bea8480021ad75",
        },
        "baseline": {
            "transactions": 1,
            "dml_operations": 65_536,
            "row_mutations": 65_536,
            "changed_rows": 65_536,
            "final_rows": 65_536,
            "last_insert_rowid": 65_536,
            "schema_objects": 1,
            "digest": "32312236c967f3ff",
        },
    },
    "update-point-implicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 1,
            "changed_rows": 1,
            "final_rows": 65_536,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "0a45750b4534b536",
        },
        "baseline": {
            "transactions": 512,
            "dml_operations": 512,
            "row_mutations": 512,
            "changed_rows": 512,
            "final_rows": 65_536,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "518e297c1461308c",
        },
    },
    "update-scan-implicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 8,
            "changed_rows": 8,
            "final_rows": 65_536,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "dc2ea3dddf5856ff",
        },
        "baseline": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 65_536,
            "changed_rows": 65_536,
            "final_rows": 65_536,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "2821e47c2a7bca80",
        },
    },
    "delete-point-implicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 1,
            "changed_rows": 1,
            "final_rows": 65_535,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "7839b461492409a8",
        },
        "baseline": {
            "transactions": 512,
            "dml_operations": 512,
            "row_mutations": 512,
            "changed_rows": 512,
            "final_rows": 65_024,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "6de42b7c34551b32",
        },
    },
    "delete-scan-implicit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 8,
            "changed_rows": 8,
            "final_rows": 65_528,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "ff71062fc55ae92f",
        },
        "baseline": {
            "transactions": 1,
            "dml_operations": 1,
            "row_mutations": 65_536,
            "changed_rows": 65_536,
            "final_rows": 0,
            "last_insert_rowid": 0,
            "schema_objects": 1,
            "digest": "cbf29ce484222325",
        },
    },
    "mixed-batch-commit": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 8,
            "row_mutations": 8,
            "changed_rows": 8,
            "final_rows": 65_538,
            "last_insert_rowid": 65_540,
            "schema_objects": 1,
            "digest": "132249e7b75e8215",
        },
        "baseline": {
            "transactions": 1,
            "dml_operations": 12_288,
            "row_mutations": 12_288,
            "changed_rows": 12_288,
            "final_rows": 65_536,
            "last_insert_rowid": 69_632,
            "schema_objects": 1,
            "digest": "43efe28707af1ec6",
        },
    },
    "mixed-batch-rollback": {
        "smoke": {
            "transactions": 1,
            "dml_operations": 8,
            "row_mutations": 8,
            "changed_rows": 8,
            "final_rows": 65_536,
            "last_insert_rowid": 65_540,
            "schema_objects": 1,
            "digest": "32312236c967f3ff",
        },
        "baseline": {
            "transactions": 1,
            "dml_operations": 12_288,
            "row_mutations": 12_288,
            "changed_rows": 12_288,
            "final_rows": 65_536,
            "last_insert_rowid": 69_632,
            "schema_objects": 1,
            "digest": "32312236c967f3ff",
        },
    },
}


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
                "expected": deepcopy(EXPECTED_WORK[case_id]),
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


def valid_timing_report(
    *,
    engine: str = "modern",
    profile: str = "matched-durable",
    case_id: str = "insert-point-implicit",
    run_kind: str = "smoke",
) -> dict[str, object]:
    work = deepcopy(EXPECTED_WORK[case_id][run_kind])
    repetition_count = 1 if run_kind == "smoke" else 3
    matched_configuration = {
        "page_size": 4096,
        "cache_size": 512,
        "mmap_bytes": 0,
        "temp_store": "memory",
        "journal_mode": "delete",
        "synchronous": "full",
        "locking_mode": "normal",
        "thread_mode": "single",
    }
    configuration = deepcopy(matched_configuration)
    if engine == "sqlite" and profile == "engine-default":
        configuration["cache_size"] = -2000
        configuration["temp_store"] = "default"
    repetitions = []
    for index in range(repetition_count):
        repetition = deepcopy(work)
        repetition.update(
            {
                "index": index,
                "wall_ns": 1_000_000
                if run_kind == "smoke"
                else 20_000_000 + index,
                "cpu_ns": 0,
            }
        )
        repetitions.append(repetition)
    return {
        "case": case_id,
        "completion": {
            "fresh_databases": repetition_count + 1,
            "measured_repetitions": repetition_count,
            "post_verifications": repetition_count + 1,
            "pre_verifications": 1,
            "status": "complete",
            "warmups": 1,
        },
        "effective_configuration": configuration,
        "engine": engine,
        "mode": "timing",
        "profile": profile,
        "repetitions": repetitions,
        "run_kind": run_kind,
        "schema_version": 1,
        "timer": {
            "cpu": "CLOCK_PROCESS_CPUTIME_ID",
            "wall": "steady_clock",
        },
        "warmup": work,
        "workload_semantics_version": 1,
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

        manifest = valid_manifest()
        manifest["cases"][0]["expected"]["baseline"]["digest"] = "0" * 16
        completed = self.run_tool(json.dumps(manifest))
        self.assertEqual(1, completed.returncode)
        self.assertIn(
            "case create-table-implicit.expected.baseline is not canonical",
            completed.stderr,
        )

    def test_requires_the_pinned_key_order(self) -> None:
        manifest = valid_manifest()
        manifest["key_order"]["seed"] = "0000000000000000"
        completed = self.run_tool(json.dumps(manifest))
        self.assertEqual(1, completed.returncode)
        self.assertIn("key_order is not canonical", completed.stderr)

    def test_accepts_strict_smoke_and_baseline_timing_reports(self) -> None:
        manifest = valid_manifest()
        for engine, profile, run_kind in (
            ("modern", "engine-default", "smoke"),
            ("sqlite", "engine-default", "smoke"),
            ("modern", "matched-durable", "baseline"),
            ("sqlite", "matched-durable", "baseline"),
        ):
            with self.subTest(
                engine=engine,
                profile=profile,
                run_kind=run_kind,
            ):
                report = valid_timing_report(
                    engine=engine,
                    profile=profile,
                    run_kind=run_kind,
                )
                self.assertIs(
                    report,
                    write_performance.validate_raw_timing_report(
                        report,
                        workload_manifest=manifest,
                        expected_engine=engine,
                        expected_profile=profile,
                        expected_case="insert-point-implicit",
                        expected_run_kind=run_kind,
                    ),
                )

    def test_rejects_malformed_timing_report_structure_and_types(self) -> None:
        manifest = valid_manifest()
        mutations = (
            (
                lambda report: report.__setitem__("unknown", 1),
                "timing report has invalid keys",
            ),
            (
                lambda report: report["repetitions"][0].__setitem__(
                    "wall_ns", True
                ),
                "wall_ns must be an integer",
            ),
            (
                lambda report: report.__setitem__("schema_version", True),
                "schema_version must be an integer",
            ),
            (
                lambda report: report["completion"].__setitem__(
                    "pre_verifications", True
                ),
                "pre_verifications must be an integer",
            ),
            (
                lambda report: report["completion"].__setitem__(
                    "fresh_databases", 3
                ),
                "completion does not match the run kind",
            ),
            (
                lambda report: report["effective_configuration"].__setitem__(
                    "cache_size", -2000
                ),
                "effective configuration does not match",
            ),
        )
        for mutate, expected in mutations:
            with self.subTest(expected=expected):
                report = valid_timing_report()
                mutate(report)
                with self.assertRaisesRegex(
                    write_performance.HarnessError,
                    expected,
                ):
                    write_performance.validate_raw_timing_report(
                        report,
                        workload_manifest=manifest,
                        expected_engine="modern",
                        expected_profile="matched-durable",
                        expected_case="insert-point-implicit",
                        expected_run_kind="smoke",
                    )

    def test_rejects_wrong_work_repetitions_and_short_baseline(self) -> None:
        manifest = valid_manifest()
        mutations = (
            (
                lambda report: report["warmup"].__setitem__(
                    "changed_rows", 2
                ),
                "warmup does not match the workload manifest",
            ),
            (
                lambda report: report["repetitions"][1].__setitem__("index", 0),
                "repetition indexes are incomplete or duplicated",
            ),
            (
                lambda report: report["repetitions"][0].__setitem__(
                    "wall_ns", 19_999_999
                ),
                "below the 20000000 minimum wall time",
            ),
            (
                lambda report: report["repetitions"][0].__setitem__(
                    "digest", "0" * 16
                ),
                "does not match the workload manifest",
            ),
        )
        for mutate, expected in mutations:
            with self.subTest(expected=expected):
                report = valid_timing_report(run_kind="baseline")
                mutate(report)
                with self.assertRaisesRegex(
                    write_performance.HarnessError,
                    expected,
                ):
                    write_performance.validate_raw_timing_report(
                        report,
                        workload_manifest=manifest,
                        expected_engine="modern",
                        expected_profile="matched-durable",
                        expected_case="insert-point-implicit",
                        expected_run_kind="baseline",
                    )

    def test_strict_json_bytes_reject_duplicate_and_nonfinite_values(self) -> None:
        for data, expected in (
            (b'{"schema_version":1,"schema_version":1}', "duplicate JSON key"),
            (b'{"value":NaN}', "non-finite JSON number"),
            (b"\xff", "not UTF-8"),
            (b" " * (4 * 1024 * 1024 + 1), "exceeds the byte limit"),
        ):
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(
                    write_performance.HarnessError,
                    expected,
                ):
                    write_performance.load_json_bytes_strict(
                        data,
                        "timing report",
                    )

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

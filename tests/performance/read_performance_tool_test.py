#!/usr/bin/env python3

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from tools import read_performance


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


class ReadPerformanceValidationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

        self.sqlite_c_path = self.root / "vendor/sqlite3.c"
        self.sqlite_h_path = self.root / "vendor/sqlite3.h"
        self.sqlite_c_path.parent.mkdir(parents=True)
        self.sqlite_c_path.write_bytes(b"pinned sqlite amalgamation\n")
        self.sqlite_h_path.write_bytes(b"pinned sqlite header\n")
        profile = self.root / "tests/compatibility/sqlite-oracle-profile-v1.json"
        write_json(
            profile,
            {
                "schema_version": 1,
                "sqlite": {
                    "version": "3.54.0",
                    "source_id": (
                        "2026-10-02 20:18:07 "
                        "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                    ),
                    "sqlite3_c_sha256": sha256(self.sqlite_c_path),
                    "sqlite3_h_sha256": sha256(self.sqlite_h_path),
                },
                "build": {
                    "semantic_compile_options": [
                        "DQS=3",
                        "THREADSAFE=0",
                    ]
                },
            },
        )
        self.profile_path = profile

        fixtures = []
        for fixture_id, row_count, page_count in (
            ("fit", 4096, 300),
            ("pressure", 65536, 4500),
        ):
            database = (
                self.root
                / f"tests/fixtures/read_performance/{fixture_id}.db"
            )
            database.parent.mkdir(parents=True, exist_ok=True)
            database.write_bytes(f"{fixture_id}-database".encode("ascii"))
            sql = database.with_suffix(".sql")
            sql.write_text(
                "CREATE TABLE kv(k INTEGER PRIMARY KEY, v BLOB NOT NULL);\n",
                encoding="utf-8",
            )
            fixtures.append(
                {
                    "id": fixture_id,
                    "path": str(database.relative_to(self.root)),
                    "sql_path": str(sql.relative_to(self.root)),
                    "sha256": sha256(database),
                    "sql_sha256": sha256(sql),
                    "size_bytes": database.stat().st_size,
                    "page_size": 4096,
                    "page_count": page_count,
                    "row_count": row_count,
                    "value_size": 256,
                    "content_fingerprint": "1" * 16,
                    "present_order_fingerprint": "2" * 16,
                    "missing_order_fingerprint": "3" * 16,
                }
            )

        self.manifest = {
            "schema_version": 1,
            "workload_semantics_version": 1,
            "sqlite_profile": str(profile.relative_to(self.root)),
            "sqlite_semantic_compile_options": [
                "DQS=3",
                "THREADSAFE=0",
            ],
            "configuration": {
                "page_size": 4096,
                "cache_pages": 512,
                "mmap_bytes": 0,
                "temp_store": "memory",
                "synchronous": "full",
                "journal_mode": "delete",
                "query_only": True,
                "thread_mode": "single",
            },
            "permutation": {
                "algorithm": "splitmix64-rejection-fisher-yates-v1",
                "fingerprint": "fnv1a64-v1",
                "fit_seed": "9e3779b97f4a7c15",
                "pressure_seed": "d1b54a32d192ed03",
            },
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
            "fixtures": fixtures,
            "cases": self._cases(),
            "guard": {
                "maximum_wall_ratio": {
                    "numerator": 10,
                    "denominator": 1,
                },
                "maximum_cpu_ratio": {
                    "numerator": 10,
                    "denominator": 1,
                },
            },
        }
        self.manifest_path = (
            self.root / "tests/performance/read-workloads-v1.json"
        )
        write_json(self.manifest_path, self.manifest)

    @staticmethod
    def _expected(
        *,
        operations: int,
        items: int,
        rows: int,
        result_bytes: int,
        hits: int,
        misses: int,
        digest: str,
    ) -> dict[str, object]:
        return {
            "operations": operations,
            "items": items,
            "rows": rows,
            "bytes": result_bytes,
            "result_hits": hits,
            "result_misses": misses,
            "digest": digest,
        }

    def _cases(self) -> list[dict[str, object]]:
        cases = []
        for fixture_id, row_count in (("fit", 4096), ("pressure", 65536)):
            measured_scans = 1024 if fixture_id == "fit" else 64
            for kind in ("point-present", "point-missing", "scan"):
                case_id = (
                    f"{kind}-ipk-{fixture_id}"
                    if kind != "scan"
                    else f"scan-ipk-{fixture_id}"
                )
                if kind == "point-present":
                    sql = "SELECT v FROM kv WHERE k=?1"
                    warmup = self._expected(
                        operations=row_count,
                        items=row_count,
                        rows=row_count,
                        result_bytes=row_count * 256,
                        hits=row_count,
                        misses=0,
                        digest="4" * 16,
                    )
                    measured = self._expected(
                        operations=1_048_576,
                        items=1_048_576,
                        rows=1_048_576,
                        result_bytes=1_048_576 * 256,
                        hits=1_048_576,
                        misses=0,
                        digest="5" * 16,
                    )
                    diagnostic = warmup
                    primary_unit = "lookup"
                    measured_iterations = 1_048_576
                    diagnostic_iterations = row_count
                elif kind == "point-missing":
                    sql = "SELECT v FROM kv WHERE k=?1"
                    warmup = self._expected(
                        operations=row_count,
                        items=row_count,
                        rows=0,
                        result_bytes=0,
                        hits=0,
                        misses=row_count,
                        digest="6" * 16,
                    )
                    measured = self._expected(
                        operations=1_048_576,
                        items=1_048_576,
                        rows=0,
                        result_bytes=0,
                        hits=0,
                        misses=1_048_576,
                        digest="7" * 16,
                    )
                    diagnostic = warmup
                    primary_unit = "lookup"
                    measured_iterations = 1_048_576
                    diagnostic_iterations = row_count
                else:
                    sql = "SELECT k,v FROM kv"
                    warmup = self._expected(
                        operations=1,
                        items=row_count,
                        rows=row_count,
                        result_bytes=row_count * 264,
                        hits=1,
                        misses=0,
                        digest="8" * 16,
                    )
                    measured = self._expected(
                        operations=measured_scans,
                        items=4_194_304,
                        rows=4_194_304,
                        result_bytes=4_194_304 * 264,
                        hits=measured_scans,
                        misses=0,
                        digest="9" * 16,
                    )
                    diagnostic = warmup
                    primary_unit = "traversal"
                    measured_iterations = measured_scans
                    diagnostic_iterations = 1

                cases.append(
                    {
                        "id": case_id,
                        "fixture": fixture_id,
                        "kind": kind.replace("-", "_"),
                        "sql": sql,
                        "primary_unit": primary_unit,
                        "warmup_iterations": (
                            row_count if kind.startswith("point") else 1
                        ),
                        "measured_iterations": measured_iterations,
                        "diagnostic_iterations": diagnostic_iterations,
                        "items_per_iteration": (
                            1 if kind.startswith("point") else row_count
                        ),
                        "expected": {
                            "warmup": warmup,
                            "measured": measured,
                            "diagnostic": diagnostic,
                            "verification": self._expected(
                                operations=1,
                                items=row_count,
                                rows=row_count,
                                result_bytes=row_count * 264,
                                hits=1,
                                misses=0,
                                digest="a" * 16,
                            ),
                        },
                    }
                )
        return cases

    def _timing_report(self) -> dict[str, object]:
        case = self.manifest["cases"][0]
        assert isinstance(case, dict)
        expected = case["expected"]
        assert isinstance(expected, dict)
        measured = expected["measured"]
        assert isinstance(measured, dict)
        warmup = expected["warmup"]
        assert isinstance(warmup, dict)
        repetitions = []
        for index, duration in enumerate((210_000_000, 220_000_000, 230_000_000)):
            repetition = dict(measured)
            repetition.update(
                {
                    "index": index,
                    "wall_ns": duration,
                    "cpu_ns": duration - 10_000_000,
                }
            )
            repetitions.append(repetition)
        return {
            "schema_version": 1,
            "completion_schema_version": 1,
            "workload_semantics_version": 1,
            "mode": "timing",
            "run_kind": "baseline",
            "engine": "modern",
            "case": case["id"],
            "build": {
                "build_type": "Release",
                "instrumentation": False,
                "sanitizers": False,
                "coverage": False,
            },
            "sqlite": {
                "version": "3.54.0",
                "source_id": (
                    "2026-10-02 20:18:07 "
                    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                ),
                "compile_options": ["DQS=3", "THREADSAFE=0"],
            },
            "effective_configuration": self.manifest["configuration"],
            "timer": {
                "wall": "steady_clock",
                "cpu": "CLOCK_PROCESS_CPUTIME_ID",
            },
            "warmup": warmup,
            "repetitions": repetitions,
            "completion": {
                "status": "complete",
                "session_opens": 1,
                "statement_prepares": 3,
                "statement_finalizes": 3,
                "statement_resets": (
                    case["warmup_iterations"]
                    + (case["measured_iterations"] * 3)
                    + 2
                ),
                "pre_verifications": 1,
                "post_verifications": 1,
            },
        }

    def _timing_matrix(
        self,
        *,
        modern_multiplier: int = 2,
    ) -> dict[tuple[int, str, str], dict[str, object]]:
        reports = {}
        for round_definition in self.manifest["rounds"]:
            round_index = round_definition["index"]
            for case in self.manifest["cases"]:
                expected = case["expected"]
                for engine in ("modern", "sqlite"):
                    report = self._timing_report()
                    report["engine"] = engine
                    report["case"] = case["id"]
                    report["warmup"] = expected["warmup"]
                    report["completion"]["statement_resets"] = (
                        case["warmup_iterations"]
                        + (case["measured_iterations"] * 3)
                        + 2
                    )
                    multiplier = modern_multiplier if engine == "modern" else 1
                    report["repetitions"] = [
                        {
                            **expected["measured"],
                            "index": index,
                            "wall_ns": (
                                (210_000_000 + round_index * 3_000_000 + index * 10_000_000)
                                * multiplier
                            ),
                            "cpu_ns": (
                                (200_000_000 + round_index * 3_000_000 + index * 10_000_000)
                                * multiplier
                            ),
                        }
                        for index in range(3)
                    ]
                    reports[(round_index, case["id"], engine)] = report
        return reports

    def _diagnostic_report(self) -> dict[str, object]:
        case = self.manifest["cases"][0]
        assert isinstance(case, dict)
        expected = case["expected"]
        assert isinstance(expected, dict)
        diagnostic = expected["diagnostic"]
        assert isinstance(diagnostic, dict)
        return {
            "schema_version": 1,
            "completion_schema_version": 1,
            "diagnostic_schema_version": 1,
            "workload_semantics_version": 1,
            "mode": "diagnostic",
            "engine": "modern",
            "case": case["id"],
            "build": {
                "build_type": "Release",
                "instrumentation": True,
                "sanitizers": False,
                "coverage": False,
            },
            "sqlite": {
                "version": "3.54.0",
                "source_id": (
                    "2026-10-02 20:18:07 "
                    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                ),
                "compile_options": ["DQS=3", "THREADSAFE=0"],
            },
            "effective_configuration": self.manifest["configuration"],
            "work": diagnostic,
            "counters": {
                "modern": {
                    "allocations": 10,
                    "bytes_copied": 20,
                    "vfs_calls": 30,
                    "pages_read": 0,
                    "pages_written": 0,
                    "cache_hits": 40,
                    "cache_misses": 0,
                    "btree_comparisons": 50,
                    "vm_instructions": 60,
                    "planner_work": 2,
                },
                "sqlite": {},
            },
            "completion": {
                "status": "complete",
                "session_opens": 1,
                "statement_prepares": 4,
                "statement_finalizes": 4,
                "statement_resets": (
                    case["warmup_iterations"]
                    + case["diagnostic_iterations"]
                    + 2
                ),
                "pre_verifications": 1,
                "post_verifications": 1,
            },
        }

    def _diagnostic_report_for(
        self,
        *,
        case_id: str,
        engine: str,
    ) -> dict[str, object]:
        case = next(
            candidate
            for candidate in self.manifest["cases"]
            if candidate["id"] == case_id
        )
        report = self._diagnostic_report()
        report["engine"] = engine
        report["case"] = case_id
        report["work"] = case["expected"]["diagnostic"]
        report["completion"]["statement_resets"] = (
            case["warmup_iterations"] + case["diagnostic_iterations"] + 2
        )
        misses = 0 if case["fixture"] == "fit" else 1
        if engine == "modern":
            report["counters"]["modern"]["cache_misses"] = misses
            report["counters"]["modern"]["pages_read"] = misses
        else:
            report["counters"] = {
                "modern": {},
                "sqlite": {
                    "cache_hits": 1,
                    "cache_misses": misses,
                    "cache_writes": 0,
                    "cache_bytes_current": 1,
                    "vm_steps": 1,
                    "fullscan_steps": (
                        1 if case["kind"] == "scan" else 0
                    ),
                    "statement_runs": 1,
                    "reprepares": 0,
                    "malloc_count_current": 1,
                    "malloc_count_highwater": 1,
                    "malloc_size_highwater": 1,
                },
            }
        return report

    def test_load_json_strict_rejects_duplicate_keys_and_nonfinite_numbers(
        self,
    ) -> None:
        duplicate = self.root / "duplicate.json"
        duplicate.write_text('{"value":1,"value":2}\n', encoding="utf-8")
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "duplicate JSON key",
        ):
            read_performance.load_json_strict(duplicate)

        nonfinite = self.root / "nonfinite.json"
        nonfinite.write_text('{"value":NaN}\n', encoding="utf-8")
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "non-finite JSON number",
        ):
            read_performance.load_json_strict(nonfinite)

    def test_benchmark_flags_require_optimization_and_reject_instrumentation(
        self,
    ) -> None:
        read_performance._validate_benchmark_flags(
            "-O3 -DNDEBUG",
            require_optimization=True,
            label="test flags",
        )
        for flags in (
            "-O0 -DNDEBUG",
            "-Og -DNDEBUG",
            "-O3 -fsanitize=address",
            "-O3 --coverage",
            "-O3 -fprofile-generate",
            "-O3 -fcoverage-mapping",
            "-O3 -finstrument-functions",
            "-O3 -fxray-instrument",
            "-O3 -fsanitize-coverage=trace-pc",
            "-O3 -flto=thin",
            "-O3 -p",
            "-O3 -pg",
            "-O3 -Wl,-pg",
        ):
            with self.subTest(flags=flags), self.assertRaises(
                read_performance.HarnessError
            ):
                read_performance._validate_benchmark_flags(
                    flags,
                    require_optimization=True,
                    label="test flags",
                )

    def test_identity_report_is_bound_to_exact_source_and_sqlite_profile(
        self,
    ) -> None:
        expected_source = {"revision": "a" * 40, "tree": "b" * 40}
        identity = {
            "schema_version": 1,
            "mode": "identity",
            "build": {
                "architecture": "x86_64",
                "build_type": "Release",
                "compiler": {"id": "clang", "version": "test"},
                "cplusplus": 202302,
                "coverage": False,
                "instrumentation": False,
                "sanitizers": False,
                "standard_library": {"id": "libc++", "version": "test"},
            },
            "source": dict(expected_source),
            "sqlite": self._timing_report()["sqlite"],
        }
        read_performance._validate_identity_report(
            identity,
            expected_source=expected_source,
            expected_semantic_options=self.manifest[
                "sqlite_semantic_compile_options"
            ],
            instrumentation=False,
            label="test identity",
        )

        wrong_source = json.loads(json.dumps(identity))
        wrong_source["source"]["tree"] = "c" * 40
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "current Git revision and tree",
        ):
            read_performance._validate_identity_report(
                wrong_source,
                expected_source=expected_source,
                expected_semantic_options=self.manifest[
                    "sqlite_semantic_compile_options"
                ],
                instrumentation=False,
                label="test identity",
            )

    def test_workload_manifest_accepts_only_the_exact_pinned_matrix(self) -> None:
        validated = read_performance.validate_workload_manifest(
            self.manifest,
            repository_root=self.root,
            manifest_path=self.manifest_path,
        )
        self.assertEqual(6, len(validated["cases"]))

        malformed = dict(self.manifest)
        malformed["unexpected"] = True
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "workload manifest keys",
        ):
            read_performance.validate_workload_manifest(
                malformed,
                repository_root=self.root,
                manifest_path=self.manifest_path,
            )

        incomplete = dict(self.manifest)
        incomplete["cases"] = self.manifest["cases"][:-1]
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "case IDs",
        ):
            read_performance.validate_workload_manifest(
                incomplete,
                repository_root=self.root,
                manifest_path=self.manifest_path,
            )

    def test_workload_manifest_checks_fixture_hashes_and_cache_classes(
        self,
    ) -> None:
        malformed = json.loads(json.dumps(self.manifest))
        malformed["fixtures"][0]["sha256"] = "f" * 64
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "fixture SHA-256",
        ):
            read_performance.validate_workload_manifest(
                malformed,
                repository_root=self.root,
                manifest_path=self.manifest_path,
            )

        malformed = json.loads(json.dumps(self.manifest))
        malformed["fixtures"][0]["page_count"] = 512
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "fit fixture",
        ):
            read_performance.validate_workload_manifest(
                malformed,
                repository_root=self.root,
                manifest_path=self.manifest_path,
            )

    def test_timing_report_rejects_empty_duplicate_short_and_instrumented_runs(
        self,
    ) -> None:
        report = self._timing_report()
        read_performance.validate_raw_timing_report(
            report,
            workload_manifest=self.manifest,
            expected_engine="modern",
            expected_case="point-present-ipk-fit",
        )

        empty = json.loads(json.dumps(report))
        empty["repetitions"] = []
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "repetitions",
        ):
            read_performance.validate_raw_timing_report(
                empty,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        duplicate = json.loads(json.dumps(report))
        duplicate["repetitions"][1]["index"] = 0
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "repetition indexes",
        ):
            read_performance.validate_raw_timing_report(
                duplicate,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        short = json.loads(json.dumps(report))
        short["repetitions"][0]["wall_ns"] = 199_999_999
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "minimum wall time",
        ):
            read_performance.validate_raw_timing_report(
                short,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        instrumented = json.loads(json.dumps(report))
        instrumented["build"]["instrumentation"] = True
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "uninstrumented",
        ):
            read_performance.validate_raw_timing_report(
                instrumented,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        wrong_sqlite = json.loads(json.dumps(report))
        wrong_sqlite["sqlite"]["compile_options"] = [
            "DQS=0",
            "THREADSAFE=1",
        ]
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "semantic compile options",
        ):
            read_performance.validate_raw_timing_report(
                wrong_sqlite,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

    def test_timing_report_rejects_wrong_work_and_incomplete_completion(
        self,
    ) -> None:
        report = self._timing_report()
        wrong_work = json.loads(json.dumps(report))
        wrong_work["repetitions"][0]["rows"] += 1
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "measured work",
        ):
            read_performance.validate_raw_timing_report(
                wrong_work,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        incomplete = json.loads(json.dumps(report))
        incomplete["completion"]["status"] = "skipped"
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "completion status",
        ):
            read_performance.validate_raw_timing_report(
                incomplete,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        wrong_resets = json.loads(json.dumps(report))
        wrong_resets["completion"]["statement_resets"] += 1
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "completion counters",
        ):
            read_performance.validate_raw_timing_report(
                wrong_resets,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

    def test_diagnostic_report_requires_exact_engine_specific_counters(
        self,
    ) -> None:
        report = self._diagnostic_report()
        read_performance.validate_raw_diagnostic_report(
            report,
            workload_manifest=self.manifest,
            expected_engine="modern",
            expected_case="point-present-ipk-fit",
        )

        missing = json.loads(json.dumps(report))
        del missing["counters"]["modern"]["cache_hits"]
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "Modern counter names",
        ):
            read_performance.validate_raw_diagnostic_report(
                missing,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        written = json.loads(json.dumps(report))
        written["counters"]["modern"]["pages_written"] = 1
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "pages_written",
        ):
            read_performance.validate_raw_diagnostic_report(
                written,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        empty_work = json.loads(json.dumps(report))
        empty_work["counters"]["modern"]["vm_instructions"] = 0
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "fixed-work counters",
        ):
            read_performance.validate_raw_diagnostic_report(
                empty_work,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-fit",
            )

        pressure = json.loads(json.dumps(report))
        pressure["case"] = "point-present-ipk-pressure"
        pressure["counters"]["modern"]["cache_misses"] = 0
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "pressure diagnostic",
        ):
            read_performance.validate_raw_diagnostic_report(
                pressure,
                workload_manifest=self.manifest,
                expected_engine="modern",
                expected_case="point-present-ipk-pressure",
            )

    def test_same_file_detects_hard_link_aliases(self) -> None:
        source = self.root / "source"
        alias = self.root / "alias"
        source.write_bytes(b"content")
        try:
            alias.hardlink_to(source)
        except OSError as error:
            self.skipTest(f"hard links unavailable: {error}")
        self.assertTrue(read_performance.paths_refer_to_same_file(source, alias))

    def test_permutation_and_fingerprint_have_portable_golden_vectors(
        self,
    ) -> None:
        permutation = read_performance.generate_permutation(
            8,
            0x9E3779B97F4A7C15,
        )
        self.assertEqual([2, 1, 4, 6, 7, 8, 3, 5], permutation)
        self.assertEqual(
            "b3835e1931f6bded",
            read_performance.fingerprint_integers(permutation),
        )
        self.assertEqual(
            b"00000001" + (b"0" * 248),
            read_performance.expected_value(1),
        )

    def test_exact_ratio_uses_reduced_integers_and_checked_decimal(self) -> None:
        self.assertEqual(
            {
                "numerator": 3,
                "denominator": 2,
                "decimal": "1.500000",
            },
            read_performance.exact_ratio(9, 6),
        )
        self.assertEqual(5, read_performance.median_integer([9, 1, 5]))
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "denominator",
        ):
            read_performance.exact_ratio(1, 0)

    def test_aggregate_report_recomputes_all_samples_medians_and_ratios(
        self,
    ) -> None:
        timing_reports = self._timing_matrix()
        aggregate = read_performance.build_aggregate_report(
            workload_manifest=self.manifest,
            timing_reports=timing_reports,
            run_manifest_path="run-manifest.json",
            run_manifest_sha256="b" * 64,
        )
        validated = read_performance.validate_aggregate_report(
            aggregate,
            workload_manifest=self.manifest,
            timing_reports=timing_reports,
            expected_run_manifest_path="run-manifest.json",
            expected_run_manifest_sha256="b" * 64,
        )
        self.assertTrue(validated["guard_passed"])
        self.assertEqual(6, len(validated["cases"]))
        first = validated["cases"][0]
        self.assertEqual(9, len(first["engines"]["modern"]["repetitions"]))
        self.assertEqual(9, len(first["engines"]["sqlite"]["repetitions"]))
        self.assertEqual(
            self.manifest["cases"][0]["expected"]["measured"],
            {
                key: first["engines"]["modern"]["repetitions"][0][key]
                for key in (
                    "operations",
                    "items",
                    "rows",
                    "bytes",
                    "result_hits",
                    "result_misses",
                    "digest",
                )
            },
        )
        self.assertEqual(
            {
                "numerator": 2,
                "denominator": 1,
                "decimal": "2.000000",
            },
            first["ratios"]["aggregate"]["wall"],
        )

        malformed = json.loads(json.dumps(aggregate))
        malformed["cases"][0]["ratios"]["aggregate"]["wall"]["decimal"] = (
            "1.000000"
        )
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "aggregate report derived values",
        ):
            read_performance.validate_aggregate_report(
                malformed,
                workload_manifest=self.manifest,
                timing_reports=timing_reports,
                expected_run_manifest_path="run-manifest.json",
                expected_run_manifest_sha256="b" * 64,
            )

    def test_performance_guard_failure_is_preserved_and_raises_exit_two(
        self,
    ) -> None:
        aggregate = read_performance.build_aggregate_report(
            workload_manifest=self.manifest,
            timing_reports=self._timing_matrix(modern_multiplier=11),
            run_manifest_path="run-manifest.json",
            run_manifest_sha256="c" * 64,
        )
        self.assertFalse(aggregate["guard_passed"])
        with self.assertRaisesRegex(
            read_performance.PerformanceMismatch,
            "severe-regression guard",
        ):
            read_performance.enforce_performance_guard(aggregate)

    def test_generate_and_validate_baseline_preserves_raw_artifact_hashes(
        self,
    ) -> None:
        benchmark_source = self.root / "benchmarks/read_performance.cpp"
        runner_source = self.root / "tools/read_performance.py"
        benchmark_source.parent.mkdir(parents=True)
        runner_source.parent.mkdir(parents=True)
        benchmark_source.write_text("int main() { return 0; }\n", encoding="utf-8")
        runner_source.write_text("# test runner\n", encoding="utf-8")
        unrelated_source = self.root / "docs/unrelated.txt"
        unrelated_source.parent.mkdir(parents=True)
        unrelated_source.write_text("original\n", encoding="utf-8")

        timing_binary = self.root / "build/modern_sqlite_read_benchmark"
        diagnostic_binary = self.root / "build/modern_sqlite_read_diagnostics"
        timing_binary.parent.mkdir(parents=True)
        timing_binary.write_bytes(b"timing-binary")
        diagnostic_binary.write_bytes(b"diagnostic-binary")
        c_compiler = shutil.which("cc")
        cxx_compiler = shutil.which("c++")
        cmake = shutil.which("cmake")
        if c_compiler is None or cxx_compiler is None or cmake is None:
            self.skipTest("test requires C, C++, and CMake executables")
        fake_ninja = timing_binary.parent / "fake-ninja"
        fake_ninja.write_text(
            "#!/bin/sh\n"
            f"echo ': && {cxx_compiler} -O3 -DNDEBUG "
            "-Wl,-dead_strip -o modern_sqlite_read_benchmark "
            "libmodern_sqlite.a libmodern_sqlite_benchmark_sqlite.a && :'\n",
            encoding="utf-8",
        )
        fake_ninja.chmod(0o755)
        write_json(
            timing_binary.parent / "compile_commands.json",
            [
                {
                    "directory": str(timing_binary.parent),
                    "command": (
                        f"{c_compiler} -DNDEBUG -DSQLITE_DQS=3 "
                        "-DSQLITE_THREADSAFE=0 -O3 "
                        "-o CMakeFiles/modern_sqlite_benchmark_sqlite.dir/"
                        "sqlite3.c.o -c "
                        f"{self.sqlite_c_path}"
                    ),
                    "file": str(self.sqlite_c_path),
                },
                {
                    "directory": str(timing_binary.parent),
                    "command": (
                        f"{cxx_compiler} -O3 -DNDEBUG "
                        "-o CMakeFiles/modern_sqlite.dir/read_session.cpp.o "
                        "-c src/session/read_session.cpp"
                    ),
                    "file": "src/session/read_session.cpp",
                },
                {
                    "directory": str(timing_binary.parent),
                    "command": (
                        f"{cxx_compiler} -O3 -DNDEBUG "
                        "-o CMakeFiles/modern_sqlite_read_benchmark.dir/"
                        "read_performance.cpp.o "
                        f"-c {benchmark_source}"
                    ),
                    "file": str(benchmark_source),
                },
            ],
        )
        (timing_binary.parent / "CMakeCache.txt").write_text(
            "\n".join(
                (
                    "CMAKE_BUILD_TYPE:STRING=Release",
                    f"CMAKE_C_COMPILER:FILEPATH={c_compiler}",
                    f"CMAKE_CXX_COMPILER:FILEPATH={cxx_compiler}",
                    "CMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG",
                    "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG",
                    "CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=",
                    "CMAKE_GENERATOR:INTERNAL=Ninja",
                    f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}",
                    f"CMAKE_MAKE_PROGRAM:FILEPATH={fake_ninja}",
                    "",
                )
            ),
            encoding="utf-8",
        )

        subprocess.run(
            ["git", "init", "--quiet"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Read Performance Test"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.email", "read-performance@example.invalid"],
            cwd=self.root,
            check=True,
        )
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "-m", "Create test inputs"],
            cwd=self.root,
            check=True,
        )
        source_revision = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=self.root,
            text=True,
        ).strip()
        source_tree = subprocess.check_output(
            ["git", "rev-parse", "HEAD^{tree}"],
            cwd=self.root,
            text=True,
        ).strip()

        timing_reports = self._timing_matrix()

        def fake_child(
            command: list[str],
            *,
            cwd: pathlib.Path,
            timeout_seconds: float,
            stdout_limit: int,
            stderr_limit: int,
        ) -> read_performance.ChildResult:
            self.assertEqual(self.root.resolve(), cwd.resolve())
            self.assertGreater(timeout_seconds, 0)
            self.assertGreater(stdout_limit, 0)
            self.assertGreater(stderr_limit, 0)
            if command[1] == "identity":
                instrumented = (
                    pathlib.Path(command[0]).resolve()
                    == diagnostic_binary.resolve()
                )
                value = {
                    "schema_version": 1,
                    "mode": "identity",
                    "build": {
                        "architecture": "x86_64",
                        "build_type": "Release",
                        "compiler": {"id": "clang", "version": "test"},
                        "cplusplus": 202302,
                        "coverage": False,
                        "instrumentation": instrumented,
                        "sanitizers": False,
                        "standard_library": {
                            "id": "libc++",
                            "version": "test",
                        },
                    },
                    "source": {
                        "revision": source_revision,
                        "tree": source_tree,
                    },
                    "sqlite": self._timing_report()["sqlite"],
                }
            elif command[5] == "baseline":
                value = timing_reports[(0, command[3], command[2])]
            else:
                value = self._diagnostic_report_for(
                    case_id=command[3],
                    engine=command[2],
                )
            return read_performance.ChildResult(
                returncode=0,
                stdout=(
                    json.dumps(value, sort_keys=True, separators=(",", ":"))
                    + "\n"
                ).encode("utf-8"),
                stderr=b"",
                elapsed_ns=10_000_000_000,
            )

        output = self.root / "benchmarks/read-baseline-v1"
        aggregate = read_performance.generate_baseline(
            repository_root=self.root,
            workload_path=self.manifest_path,
            timing_binary_path=timing_binary,
            diagnostic_binary_path=diagnostic_binary,
            sqlite_c_path=self.sqlite_c_path,
            sqlite_h_path=self.sqlite_h_path,
            output_path=output,
            child_runner=fake_child,
        )
        self.assertTrue(aggregate["guard_passed"])
        read_performance.validate_baseline_directory(
            baseline_path=output,
            repository_root=self.root,
            workload_path=self.manifest_path,
        )
        unrelated_source.write_text("future change\n", encoding="utf-8")
        read_performance.validate_baseline_directory(
            baseline_path=output,
            repository_root=self.root,
            workload_path=self.manifest_path,
        )
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "current source worktree",
        ):
            read_performance.validate_baseline_directory(
                baseline_path=output,
                repository_root=self.root,
                workload_path=self.manifest_path,
                verify_current_source=True,
            )
        unrelated_source.write_text("original\n", encoding="utf-8")

        run_manifest = read_performance.load_json_strict(
            output / "run-manifest.json"
        )
        for key in (
            "actual_sqlite_c_compile_flags",
            "actual_modern_cxx_compile_flags",
            "actual_harness_cxx_compile_flags",
            "actual_timing_link_flags",
        ):
            self.assertIn("-O3", run_manifest["build"][key])
            self.assertNotIn(str(self.root), run_manifest["build"][key])
        raw_path = output / run_manifest["timing_runs"][0]["stdout"]["path"]
        raw_path.write_bytes(raw_path.read_bytes() + b" ")
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "artifact (size|SHA-256)",
        ):
            read_performance.validate_baseline_directory(
                baseline_path=output,
                repository_root=self.root,
                workload_path=self.manifest_path,
            )

        shutil.rmtree(output)

        def failing_child(
            command: list[str],
            *,
            cwd: pathlib.Path,
            timeout_seconds: float,
            stdout_limit: int,
            stderr_limit: int,
        ) -> read_performance.ChildResult:
            if command[1] == "identity":
                return fake_child(
                    command,
                    cwd=cwd,
                    timeout_seconds=timeout_seconds,
                    stdout_limit=stdout_limit,
                    stderr_limit=stderr_limit,
                )
            return read_performance.ChildResult(
                returncode=1,
                stdout=b"",
                stderr=b"child failure\n",
                elapsed_ns=1,
            )

        with self.assertRaises(read_performance.BaselineChildFailure):
            read_performance.generate_baseline(
                repository_root=self.root,
                workload_path=self.manifest_path,
                timing_binary_path=timing_binary,
                diagnostic_binary_path=diagnostic_binary,
                sqlite_c_path=self.sqlite_c_path,
                sqlite_h_path=self.sqlite_h_path,
                output_path=output,
                child_runner=failing_child,
            )
        failure = read_performance.load_json_strict(output / "failure.json")
        self.assertEqual(1, failure["child"]["process"]["returncode"])
        self.assertFalse(failure["child"]["process"]["timed_out"])
        self.assertEqual("preserved", failure["child"]["database"]["cleanup"])
        self.assertTrue(
            (output / failure["child"]["database"]["copy_path"]).is_file()
        )

        cache_path = timing_binary.parent / "CMakeCache.txt"
        valid_cache = cache_path.read_text(encoding="utf-8")
        cache_path.write_text(
            valid_cache.replace(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}",
                "CMAKE_HOME_DIRECTORY:INTERNAL=/wrong/source",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "source directory does not match",
        ):
            read_performance._capture_build_provenance(
                repository_root=self.root,
                timing_binary_path=timing_binary,
                diagnostic_binary_path=diagnostic_binary,
                sqlite_source_directory=self.sqlite_c_path.parent,
            )
        cache_path.write_text(valid_cache, encoding="utf-8")

    def test_bounded_child_rejects_timeout_and_oversized_output(self) -> None:
        with self.assertRaisesRegex(
            read_performance.ChildExecutionError,
            "stdout limit",
        ) as oversized:
            read_performance.run_bounded(
                [
                    sys.executable,
                    "-c",
                    "import sys; sys.stdout.write('x' * 1024)",
                ],
                cwd=self.root,
                timeout_seconds=2.0,
                stdout_limit=32,
                stderr_limit=32,
            )
        self.assertFalse(oversized.exception.timed_out)
        self.assertEqual(32, len(oversized.exception.stdout))

        with self.assertRaisesRegex(
            read_performance.ChildExecutionError,
            "timed out",
        ) as timed_out:
            read_performance.run_bounded(
                [sys.executable, "-c", "import time; time.sleep(5)"],
                cwd=self.root,
                timeout_seconds=0.1,
                stdout_limit=32,
                stderr_limit=32,
            )
        self.assertTrue(timed_out.exception.timed_out)
        self.assertNotEqual(0, timed_out.exception.returncode)

        descendant_pid_path = self.root / "descendant.pid"
        child_code = (
            "import os,pathlib,time;"
            f"pathlib.Path({str(descendant_pid_path)!r}).write_text("
            "str(os.getpid()),encoding='ascii');"
            "time.sleep(5)"
        )
        parent_code = (
            "import subprocess,sys;"
            f"subprocess.Popen([sys.executable,'-c',{child_code!r}])"
        )
        with self.assertRaisesRegex(
            read_performance.ChildExecutionError,
            "timed out",
        ):
            read_performance.run_bounded(
                [sys.executable, "-c", parent_code],
                cwd=self.root,
                timeout_seconds=0.2,
                stdout_limit=32,
                stderr_limit=32,
            )
        descendant_pid = int(
            descendant_pid_path.read_text(encoding="ascii")
        )
        for _ in range(100):
            try:
                os.kill(descendant_pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.01)
        else:
            self.fail("timed-out child process group was not terminated")

        result = read_performance.run_bounded(
            [
                sys.executable,
                "-c",
                "import sys; print('ok'); print('note', file=sys.stderr)",
            ],
            cwd=self.root,
            timeout_seconds=2.0,
            stdout_limit=32,
            stderr_limit=32,
        )
        self.assertEqual(0, result.returncode)
        self.assertEqual(b"ok\n", result.stdout)
        self.assertEqual(b"note\n", result.stderr)
        self.assertGreater(result.elapsed_ns, 0)

    def test_new_output_path_rejects_existing_and_protected_aliases(self) -> None:
        existing = self.root / "existing"
        existing.write_bytes(b"content")
        with self.assertRaisesRegex(
            read_performance.HarnessError,
            "already exists",
        ):
            read_performance.validate_new_output_path(
                existing,
                protected_paths=[],
            )

        output = self.root / "new-output"
        read_performance.validate_new_output_path(
            output,
            protected_paths=[existing],
        )
        output.write_bytes(b"new")
        self.addCleanup(lambda: output.unlink(missing_ok=True))
        if os.path.normcase(str(output)) != os.path.normcase(str(existing)):
            self.assertFalse(
                read_performance.paths_refer_to_same_file(output, existing)
            )

    def test_cli_usage_errors_return_one(self) -> None:
        completed = subprocess.run(
            [sys.executable, "tools/read_performance.py", "unknown-command"],
            cwd=pathlib.Path(__file__).resolve().parents[2],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(1, completed.returncode, completed.stderr)
        self.assertIn("usage:", completed.stderr)


if __name__ == "__main__":
    unittest.main()

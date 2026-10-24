from __future__ import annotations

import copy
import hashlib
import pathlib
import tempfile
import unittest
from unittest import mock

from tools import preparation_performance as preparation


class PreparationPerformanceToolTest(unittest.TestCase):
    def raw(self, report):
        stdout = preparation.common._canonical_json_bytes(report).decode()
        return {
            "stdout": stdout, "stderr": "", "returncode": 0, "elapsed_ns": 100_000_000,
            "stdout_sha256": hashlib.sha256(stdout.encode()).hexdigest(),
        }

    def report(self, phase="parse", case_id="short", diagnostic=False):
        case = preparation.case_by_id(case_id)
        iterations = 8 if diagnostic else case["measured_iterations"]
        attempts = 10 + iterations * (1 if diagnostic else 3)
        report = {
            "schema_version": 1,
            "mode": "diagnostic" if diagnostic else "timing",
            "run_kind": "diagnostic" if diagnostic else "baseline",
            "engine": "modern",
            "phase": phase,
            "case": case_id,
            "reference_role": preparation.reference_role(phase),
            "build": {
                "build_type": "Release", "coverage": False,
                "instrumentation": diagnostic, "sanitizers": False,
            },
            "effective_configuration": preparation.configuration(phase),
            "completion": {
                "status": "complete", "parse_calls": attempts if phase == "parse" else 0,
                "prepare_calls": attempts if phase == "prepare" else 0,
                "statement_finalizes": attempts if phase == "prepare" and case["accepted"] else 0,
                "statement_steps": 0, "statement_resets": 0,
            },
            "warmup": preparation.expected_work(case_id, phase, 8),
            "repetitions": [] if diagnostic else [
                {"index": index, "wall_ns": 10_000_000, "cpu_ns": 9_000_000,
                 "work": preparation.expected_work(case_id, phase, iterations)}
                for index in range(3)
            ],
        }
        if diagnostic:
            report["work"] = preparation.expected_work(case_id, phase, 8)
            report["counters"] = {
                "modern": dict.fromkeys(preparation.common.MODERN_COUNTER_NAMES, 0),
                "sqlite": {},
            }
            report["counters"]["modern"]["allocations"] = 1
        return report

    def validate(self, report):
        return preparation.validate_report(
            report, engine="modern", phase=report["phase"], case_id=report["case"],
            diagnostic=report["mode"] == "diagnostic",
        )

    def test_independent_work_digest_and_phase_boundaries(self):
        self.assertEqual(
            preparation.expected_work("wide-512", "parse", 256)["digest"],
            "ea7785432fa92525",
        )
        self.assertNotEqual(
            preparation.expected_work("malformed-short", "parse", 1),
            preparation.expected_work("malformed-short", "prepare", 1),
        )

    def test_accepts_exact_parse_prepare_and_diagnostic_work(self):
        for phase in ("parse", "prepare"):
            for case in preparation.expected_cases()["cases"]:
                for diagnostic in (False, True):
                    self.validate(self.report(phase, case["id"], diagnostic))

    def test_rejects_weakened_timing_work_and_completion(self):
        original = self.report()
        mutations = (
            lambda value: value["repetitions"].clear(),
            lambda value: value["repetitions"][0].update(wall_ns=1),
            lambda value: value["repetitions"][0].update(cpu_ns=0),
            lambda value: value["repetitions"][0]["work"].update(operations=1),
            lambda value: value["repetitions"][0]["work"].update(digest="0" * 16),
            lambda value: value["completion"].update(parse_calls=3),
            lambda value: value["completion"].update(statement_steps=1),
            lambda value: value.update(reference_role="unmodified-public-prepare"),
            lambda value: value["build"].update(instrumentation=True),
            lambda value: value["effective_configuration"].update(catalog=True),
            lambda value: value.update(schema_version=True),
            lambda value: value.update(unrecognized_field=1),
        )
        for mutation in mutations:
            changed = copy.deepcopy(original)
            mutation(changed)
            with self.subTest(mutation=mutation):
                with self.assertRaises(preparation.HarnessError):
                    self.validate(changed)

    def test_rejects_prepare_error_relabelled_as_parse_error(self):
        report = self.report("prepare", "malformed-short")
        report["repetitions"][0]["work"] = preparation.expected_work(
            "malformed-short", "parse", report["repetitions"][0]["work"]["operations"]
        )
        with self.assertRaises(preparation.HarnessError):
            self.validate(report)

    def test_schedule_keeps_three_alternating_rounds(self):
        schedule = preparation.timing_schedule()
        self.assertEqual(len(schedule), 96)
        self.assertEqual(len(set(schedule)), 96)
        self.assertEqual(schedule[:2], [(0, "parse:short", "modern"), (0, "parse:short", "sqlite")])
        self.assertEqual(schedule[32][2], "sqlite")
        self.assertEqual(schedule[64][2], "modern")

    def test_dirty_source_is_rejected_before_creating_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "baseline.json"
            with mock.patch.object(
                preparation.common, "_capture_source_state", return_value={"clean": False}
            ):
                with self.assertRaisesRegex(preparation.HarnessError, "clean"):
                    preparation.generate(
                        preparation.REPOSITORY_ROOT,
                        pathlib.Path("missing-timing"),
                        pathlib.Path("missing-diagnostic"),
                        output,
                    )
            self.assertFalse(output.exists())

    def archive(self):
        common = preparation.common
        source = {
            "revision": "a" * 40, "tree": "b" * 40, "clean": True,
            "status_sha256": hashlib.sha256(b"").hexdigest(),
            "worktree_content_sha256": "c" * 64,
        }
        build = dict.fromkeys(
            common._BUILD_PROVENANCE_KEYS
            - {"actual_harness_cxx_compile_flags", "actual_timing_link_flags"}, ""
        )
        for name in ("c_effective_flags", "cxx_effective_flags", "actual_sqlite_c_compile_flags",
                     "actual_modern_cxx_compile_flags"):
            build[name] = "-O3 -DNDEBUG"
        build["generator"], build["build_type"] = "Ninja", "Release"
        for name in ("cmake_version", "c_compiler", "c_compiler_version", "cxx_compiler", "cxx_compiler_version"):
            build[name] = "test"
        for target in preparation.PREPARATION_TARGETS:
            build[f"{target}_compile_flags"] = (
                "-O3 -DNDEBUG -DSQLITE_DQS=3 -DSQLITE_THREADSAFE=0 "
                f"-DMODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS={int(target.endswith('diagnostics'))}"
            )
        build["modern_sqlite_diagnostic_engine_compile_flags"] = (
            "-O3 -DNDEBUG -DMODERN_SQLITE_ENABLE_INSTRUMENTATION=1"
        )
        for suffix in ("benchmark", "diagnostics"):
            build[f"modern_sqlite_preparation_{suffix}_link_command"] = (
                f"-O3 -DNDEBUG libmodern_sqlite_native_parser_{suffix}.a "
                + ("libmodern_sqlite_diagnostic_engine.a" if suffix == "diagnostics" else "libmodern_sqlite.a")
            )
        archive = {
            "schema_version": 1, "status": "complete", "purpose": "baseline-not-a-disposition",
            "source": source, "build": build,
            "host": {"os": "test", "kernel": "test", "architecture": "arm64", "cpu": "test",
                     "logical_cpu_count": 1, "wall_timer": "steady_clock",
                     "cpu_timer": "CLOCK_PROCESS_CPUTIME_ID"},
            "inputs": preparation._inputs(preparation.REPOSITORY_ROOT),
            "workloads": preparation.expected_cases(), "binaries": {}, "controls": {},
            "timing_runs": [], "diagnostic_runs": [],
        }
        shared = common.load_json_strict(preparation.REPOSITORY_ROOT / preparation.SHARED_WORKLOADS)
        for role in ("timing", "diagnostics"):
            diagnostic = role == "diagnostics"
            identity = {
                "measurement": {
                    "schema_version": 1, "mode": "identity",
                    "source": {"revision": source["revision"], "tree": source["tree"]},
                    "build": {"architecture": "arm64", "build_type": "Release",
                              "compiler": {"id": "clang", "version": "test"},
                              "cplusplus": 202302, "coverage": False,
                              "instrumentation": diagnostic, "sanitizers": False,
                              "standard_library": {"id": "libc++", "version": "test"}},
                    "sqlite": {"version": common.SQLITE_VERSION, "source_id": common.SQLITE_SOURCE_ID,
                               "compile_options": shared["sqlite_semantic_compile_options"]},
                },
                "native_syntax_adapter": {
                    "role": "derived-select-syntax", "semantic_callback_guards": 2,
                    "symbol_linkage": "translation-unit-local",
                    "source_sha256": preparation.adapter.SQLITE_SOURCE_SHA256,
                    "generated_sha256": preparation.adapter.SYNTAX_SOURCE_SHA256,
                    "source_id": common.SQLITE_SOURCE_ID,
                    "diagnostic_allocator_wrappers": diagnostic,
                },
            }
            archive["binaries"][role] = {
                "logical_name": "modern_sqlite_preparation_" + ("diagnostics" if diagnostic else "benchmark"),
                "size_bytes": 1, "sha256": "d" * 64, "identity": identity,
            }
            archive["controls"][role] = self.raw({
                "status": "complete", "no_vdbe_or_schema_mutation": True, "same_context_retry": True,
                "ordinary_prepare_still_rejects_unknown_table": True,
                "unsupported_statement_controls": 5, "balanced_cleanup_iterations": 1000,
                "oom_failures": 1 if diagnostic else 0,
            })
        for index, key, engine in preparation.timing_schedule():
            phase, case_id = key.split(":")
            report = self.report(phase, case_id)
            report["engine"] = engine
            archive["timing_runs"].append({
                "round": index, "key": key, "engine": engine, "raw": self.raw(report),
            })
        for phase in preparation.PHASES:
            for case in preparation.expected_cases()["cases"]:
                for engine in preparation.ENGINES:
                    report = self.report(phase, case["id"], True)
                    report["engine"] = engine
                    if engine == "sqlite":
                        report["counters"] = {
                            "modern": {}, "sqlite": {
                                "malloc_count_current": 1, "malloc_count_highwater": 2,
                                "memory_bytes_current": 1, "memory_bytes_highwater": 2,
                                "lookaside_used": 0, "lookaside_highwater": 1, "lookaside_hits": 1,
                            },
                        }
                    archive["diagnostic_runs"].append({
                        "key": f"{phase}:{case['id']}", "engine": engine, "raw": self.raw(report),
                    })
        archive["summary"] = preparation._summary(archive["timing_runs"])
        return archive

    def test_archive_requires_complete_schedule_and_recorded_input_identity(self):
        original = self.archive()
        preparation.validate_archive(original, preparation.REPOSITORY_ROOT, verify_current_source=False)
        mutations = (
            lambda value: value["timing_runs"].pop(),
            lambda value: value["timing_runs"].reverse(),
            lambda value: value["diagnostic_runs"].pop(),
            lambda value: value["inputs"].pop(),
            lambda value: value["inputs"][0].update(path="../outside"),
            lambda value: value["source"].update(clean=False),
            lambda value: value["source"].update(worktree_content_sha256="not-a-hash"),
            lambda value: value["binaries"]["timing"]["identity"]["native_syntax_adapter"].update(
                generated_sha256="e" * 64
            ),
            lambda value: value["summary"][0]["ratios"].update(wall_ns={"numerator": 1}),
            lambda value: value["timing_runs"][0]["raw"].update(elapsed_ns=1),
            lambda value: value["timing_runs"][0]["raw"].update(stdout_sha256="e" * 64),
        )
        for mutation in mutations:
            changed = copy.deepcopy(original)
            mutation(changed)
            with self.subTest(mutation=mutation):
                with self.assertRaises(preparation.HarnessError):
                    preparation.validate_archive(
                        changed, preparation.REPOSITORY_ROOT, verify_current_source=False
                    )


if __name__ == "__main__":
    unittest.main()

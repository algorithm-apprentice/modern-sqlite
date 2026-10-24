from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT))
from tools import read_performance as common


class PreparationBenchmarkTest(unittest.TestCase):
    timing: pathlib.Path
    diagnostics: pathlib.Path
    database: pathlib.Path

    def invoke(self, binary: pathlib.Path, *arguments: str, success: bool = True):
        result = common.run_bounded(
            [str(binary), *arguments],
            cwd=REPOSITORY_ROOT,
            timeout_seconds=10,
            stdout_limit=128 * 1024,
            stderr_limit=32 * 1024,
        )
        if not success:
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(result.stderr)
            return None
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertFalse(result.stderr)
        return common.load_json_bytes_strict(result.stdout, "preparation benchmark")

    def test_native_isolation_cleanup_and_failure_controls(self) -> None:
        for binary in (self.timing, self.diagnostics):
            control = self.invoke(binary, "self-test")
            self.assertEqual(control["status"], "complete")
            self.assertTrue(control["no_vdbe_or_schema_mutation"])
            self.assertTrue(control["same_context_retry"])
            self.assertTrue(control["ordinary_prepare_still_rejects_unknown_table"])
            self.assertGreater(control["unsupported_statement_controls"], 0)
            self.assertGreater(control["balanced_cleanup_iterations"], 0)
            if binary == self.diagnostics:
                self.assertGreater(control["oom_failures"], 0)
            else:
                self.assertEqual(control["oom_failures"], 0)

    def test_every_phase_has_equal_work_and_real_lifecycles(self) -> None:
        original = hashlib.sha256(self.database.read_bytes()).hexdigest()
        manifest = self.invoke(self.timing, "cases")
        cases = manifest["cases"]
        iterations = manifest["smoke_iterations"]
        self.assertEqual(iterations, 64)
        self.assertEqual(len(cases), 8)
        self.assertEqual(len({case["id"] for case in cases}), 8)
        for case in cases:
            for phase in ("parse", "prepare"):
                reports = []
                for engine in ("modern", "sqlite"):
                    with self.subTest(case=case["id"], phase=phase, engine=engine):
                        report = self.invoke(
                            self.timing, "run", engine, phase, case["id"],
                            str(self.database), "smoke",
                        )
                        self.assertEqual(report["completion"]["status"], "complete")
                        self.assertEqual(report["phase"], phase)
                        work = report["repetitions"][0]["work"]
                        self.assertEqual(work["operations"], iterations)
                        self.assertEqual(work["rows"], 0)
                        self.assertEqual(work["result_hits"], iterations if case["accepted"] else 0)
                        lifecycle = report["completion"]
                        attempts = 10 + iterations
                        self.assertEqual(lifecycle["parse_calls"], attempts if phase == "parse" else 0)
                        self.assertEqual(lifecycle["prepare_calls"], attempts if phase == "prepare" else 0)
                        self.assertEqual(
                            lifecycle["statement_finalizes"],
                            attempts if phase == "prepare" and case["accepted"] else 0,
                        )
                        self.assertEqual(lifecycle["statement_steps"], 0)
                        self.assertEqual(lifecycle["statement_resets"], 0)
                        reports.append(work)
                self.assertEqual(len(reports), 2)
                self.assertEqual(reports[0], reports[1])
        self.assertEqual(hashlib.sha256(self.database.read_bytes()).hexdigest(), original)

    def test_diagnostics_are_separate_from_timing(self) -> None:
        for engine in ("modern", "sqlite"):
            for phase in ("parse", "prepare"):
                report = self.invoke(
                    self.diagnostics, "run", engine, phase, "wide-512",
                    str(self.database), "diagnostic",
                )
                self.assertEqual(report["mode"], "diagnostic")
                self.assertEqual(report["work"]["operations"], 8)
                if engine == "modern":
                    self.assertGreater(report["counters"]["modern"]["allocations"], 0)
                else:
                    self.assertEqual(report["counters"]["sqlite"]["lookaside_used"], 0)
                    self.assertGreater(report["counters"]["sqlite"]["malloc_count_highwater"], 0)
        self.invoke(
            self.timing, "run", "modern", "parse", "short", str(self.database),
            "diagnostic", success=False,
        )
        self.invoke(
            self.diagnostics, "run", "modern", "parse", "short", str(self.database),
            "baseline", success=False,
        )

    def test_identity_names_the_derived_native_adapter(self) -> None:
        for binary in (self.timing, self.diagnostics):
            identity = self.invoke(binary, "identity")
            self.assertEqual(identity["measurement"]["mode"], "identity")
            adapter = identity["native_syntax_adapter"]
            self.assertEqual(adapter["role"], "derived-select-syntax")
            self.assertEqual(adapter["semantic_callback_guards"], 2)
            self.assertEqual(adapter["symbol_linkage"], "translation-unit-local")
            self.assertEqual(len(adapter["source_sha256"]), 64)
            self.assertEqual(len(adapter["generated_sha256"]), 64)
            self.assertNotEqual(adapter["source_sha256"], adapter["generated_sha256"])

    def test_prepare_reports_the_actual_memory_configuration(self) -> None:
        for binary in (self.timing, self.diagnostics):
            mode = "diagnostic" if binary == self.diagnostics else "smoke"
            configurations = [
                self.invoke(binary, "run", engine, "prepare", "short", str(self.database), mode)[
                    "effective_configuration"
                ]
                for engine in ("modern", "sqlite")
            ]
            self.assertEqual(configurations[0]["temp_store"], "memory")
            self.assertEqual(configurations[0], configurations[1])

    def test_native_prepare_error_preserves_only_its_warmed_connection_owner(self) -> None:
        report = self.invoke(
            self.diagnostics, "run", "sqlite", "prepare", "malformed-short",
            str(self.database), "diagnostic",
        )
        counters = report["counters"]["sqlite"]
        self.assertIn("lookaside_used_before", counters)
        self.assertGreater(counters["lookaside_used_before"], 0)
        self.assertEqual(counters["lookaside_used"], counters["lookaside_used_before"])
        self.assertEqual(counters["malloc_count_current"], counters["malloc_count_before"])
        self.assertEqual(counters["memory_bytes_current"], counters["memory_bytes_before"])

    def test_invalid_engine_phase_case_or_mode_fails(self) -> None:
        for arguments in (
            ("wrong-engine", "parse", "short", "smoke"),
            ("modern", "execute", "short", "smoke"),
            ("modern", "parse", "unknown", "smoke"),
            ("modern", "parse", "short", "wrong-mode"),
        ):
            self.invoke(
                self.timing, "run", *arguments[:3], str(self.database),
                arguments[3], success=False,
            )

    def test_private_native_apis_cannot_interpose_public_prepare(self) -> None:
        options = ["-gU"] if sys.platform == "darwin" else ["-g", "--defined-only"]
        for role in ("benchmark", "diagnostics"):
            library = self.timing.parent / f"libmodern_sqlite_native_parser_{role}.a"
            result = common.run_bounded(
                ["nm", *options, str(library)], cwd=REPOSITORY_ROOT,
                timeout_seconds=10, stdout_limit=128 * 1024, stderr_limit=32 * 1024,
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            symbols = result.stdout.decode()
            self.assertIn("ModernSqliteNativeParserCreate", symbols)
            self.assertIsNone(re.search(r"\b_?sqlite3\w*\b", symbols))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing", required=True, type=pathlib.Path)
    parser.add_argument("--diagnostics", required=True, type=pathlib.Path)
    parser.add_argument("--database", required=True, type=pathlib.Path)
    options, remaining = parser.parse_known_args()
    PreparationBenchmarkTest.timing = options.timing.resolve()
    PreparationBenchmarkTest.diagnostics = options.diagnostics.resolve()
    PreparationBenchmarkTest.database = options.database.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])

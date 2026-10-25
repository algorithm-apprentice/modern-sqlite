#!/usr/bin/env python3

import json
import pathlib
import re
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
STANDARD_CI_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/ci.yml"
FULL_QUALITY_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/full-quality.yml"


def event_block(workflow: str, event: str) -> str:
    lines = workflow.splitlines()
    marker = f"  {event}:"
    start = lines.index(marker)
    block = []
    for line in lines[start + 1 :]:
        indentation = len(line) - len(line.lstrip())
        if line.strip() and indentation <= 2:
            break
        block.append(line)
    return "\n".join(block)


def list_values(block: str, key: str) -> list[str]:
    lines = block.splitlines()
    marker = f"    {key}:"
    start = lines.index(marker)
    values = []
    for line in lines[start + 1 :]:
        indentation = len(line) - len(line.lstrip())
        if line.strip() and indentation <= 4:
            break
        if line.startswith("      - "):
            values.append(line.removeprefix("      - ").strip("\"'"))
    return values


class CiWorkflowPolicyTest(unittest.TestCase):
    def test_native_windows_executes_the_declared_correctness_selection(self) -> None:
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")
        windows = event_block(workflow, "windows-native")
        for required in (
            "runs-on: windows-2025",
            "msys2/setup-msys2@ec48f7c5447b3140e2b088413ae3a55687bccb6e",
            "msystem: UCRT64",
            "mingw-w64-ucrt-x86_64-gcc",
            "mingw-w64-ucrt-x86_64-python",
            "cb547ab3e931c7766e24834af5ef6c4578863e3d",
            "4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6",
            "cmake --preset windows-debug",
            "cmake --build --preset windows-debug",
            "ctest --preset windows-debug",
            "mingw-w64-ucrt-x86_64-clang-tools-extra",
            "src/platform/windows_vfs.cpp.obj",
        ):
            with self.subTest(required=required):
                self.assertIn(required, windows)
        presets = json.loads((REPOSITORY_ROOT / "CMakePresets.json").read_text())
        configure = next(
            preset for preset in presets["configurePresets"]
            if preset["name"] == "windows-debug"
        )
        self.assertEqual("gcc", configure["cacheVariables"]["CMAKE_C_COMPILER"])
        self.assertEqual("g++", configure["cacheVariables"]["CMAKE_CXX_COMPILER"])
        native_tests = next(
            preset for preset in presets["testPresets"]
            if preset["name"] == "windows-debug"
        )
        selection = native_tests["filter"]["include"]["label"]
        for label in (
            "unit", "platform", "model", "compatibility", "differential",
            "cli", "project", "fuzz",
        ):
            with self.subTest(label=label):
                self.assertRegex(label, selection)

    def test_platform_regression_is_an_explicit_dispatch_without_changed_limits(self) -> None:
        workflow = FULL_QUALITY_WORKFLOW.read_text(encoding="utf-8")
        dispatch = event_block(workflow, "workflow_dispatch")
        self.assertIn("platform_regression:", dispatch)
        self.assertIn("default: false", dispatch)
        for job in ("platform-regression", "platform-fuzz"):
            block = event_block(workflow, job)
            self.assertIn("github.event_name == 'workflow_dispatch'", block)
            self.assertIn("inputs.platform_regression", block)
        regression = event_block(workflow, "platform-regression")
        self.assertIn("cmake --preset dev-debug", regression)
        self.assertIn("cmake --preset instrumentation", regression)
        self.assertIn("tools/verify.py run-shared", regression)
        fuzz = event_block(workflow, "platform-fuzz")
        for required in ("-runs=20000", "-seed=102", "-timeout=10", "-max_len=65536",
                         "modern_sqlite_read_sql_fuzz", "modern_sqlite_read_database_fuzz",
                         "modern_sqlite_write_sql_fuzz", "modern_sqlite_write_database_fuzz"):
            self.assertIn(required, fuzz)

    def test_standard_ci_is_not_triggered_by_labels(self) -> None:
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")

        self.assertNotRegex(workflow, re.compile(r"^      - labeled$", re.MULTILINE))
        self.assertNotRegex(
            workflow, re.compile(r"^  full-quality:$", re.MULTILINE)
        )

    def test_full_quality_has_isolated_triggers_and_job(self) -> None:
        workflow = FULL_QUALITY_WORKFLOW.read_text(encoding="utf-8")

        for trigger in (
            "  workflow_dispatch:",
            "  pull_request:",
            "  push:",
            "      - labeled",
            "      - synchronize",
            "      - reopened",
            "      - main",
        ):
            with self.subTest(trigger=trigger):
                self.assertIn(trigger, workflow)

        self.assertRegex(workflow, re.compile(r"^  full-quality:$", re.MULTILINE))
        self.assertIn("github.event.label.name == 'full-quality'", workflow)
        self.assertIn(
            "contains(github.event.pull_request.labels.*.name, 'full-quality')",
            workflow,
        )
        self.assertIn("group: full-quality-", workflow)

    def test_automatic_workflows_ignore_markdown_only_changes(self) -> None:
        for workflow_path in (STANDARD_CI_WORKFLOW, FULL_QUALITY_WORKFLOW):
            workflow = workflow_path.read_text(encoding="utf-8")
            for event in ("pull_request", "push"):
                with self.subTest(workflow=workflow_path.name, event=event):
                    block = event_block(workflow, event)
                    self.assertEqual(["**/*.md"], list_values(block, "paths-ignore"))

    def test_write_diagnostics_have_independent_registration_and_ci_coverage(self) -> None:
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        for required in (
            "performance.write_benchmark_diagnostic_${write_engine}_${write_profile_id}_${write_case_id}",
            "--suite diagnostic",
            "TIMEOUT 630",
            "foreach(write_engine IN ITEMS modern sqlite)",
            "foreach(write_profile IN ITEMS engine-default matched-durable)",
        ):
            with self.subTest(required=required):
                self.assertTrue(required in cmake, f"CTest must include {required}")
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")
        patterns = re.findall(r"-R '([^']+)'", workflow)
        self.assertTrue(any(re.fullmatch(
            pattern,
            "performance.write_benchmark_diagnostic_modern_matched_durable_update_point_implicit",
        ) for pattern in patterns))

    def test_ci_covers_aggregate_performance_contracts(self) -> None:
        workflow = event_block(
            STANDARD_CI_WORKFLOW.read_text(encoding="utf-8"),
            "read-performance-contract",
        )
        for required in (
            "tools/aggregate_performance.py validate-workloads",
            "tools/aggregate_performance.py validate-historical-baseline",
            "benchmarks/aggregate-baseline-v1",
        ):
            with self.subTest(required=required):
                self.assertTrue(required in workflow, f"CI must include {required}")
        targets = json.loads(
            (REPOSITORY_ROOT / "project/verification-policy.json").read_text()
        )["native_targets"]
        self.assertIn("tools/verify.py build-native", workflow)
        self.assertIn("modern_sqlite_aggregate_benchmark", targets)
        self.assertIn("modern_sqlite_aggregate_diagnostics", targets)

        smoke_patterns = re.findall(r"-R '([^']+)'", workflow)
        self.assertTrue(
            any(
                re.fullmatch(pattern, "performance.aggregate_benchmark_cli")
                for pattern in smoke_patterns
            ),
            "CI must execute the aggregate timing and diagnostic CLI matrix",
        )

    def test_native_target_set_preserves_the_previous_ci_contract(self) -> None:
        expected = {
            "modern_sqlite_read_benchmark", "modern_sqlite_read_diagnostics",
            "modern_sqlite_index_benchmark", "modern_sqlite_index_diagnostics",
            "modern_sqlite_order_by_benchmark", "modern_sqlite_order_by_diagnostics",
            "modern_sqlite_distinct_compound_benchmark",
            "modern_sqlite_distinct_compound_diagnostics",
            "modern_sqlite_aggregate_benchmark", "modern_sqlite_aggregate_diagnostics",
            "modern_sqlite_write_benchmark", "modern_sqlite_write_diagnostics",
            "modern_sqlite_benchmark_sqlite_shared",
            "modern_sqlite_btree_writer_sqlite_compatibility",
            "modern_sqlite_transaction_sqlite_compatibility",
            "modern_sqlite_write_session_sqlite_compatibility",
            "modern_sqlite_write_pager_sqlite_compatibility",
            "modern_sqlite_rollback_journal_fixture",
        }
        expected |= {"modern_sqlite_preparation_benchmark", "modern_sqlite_preparation_diagnostics"}
        actual = json.loads(
            (REPOSITORY_ROOT / "project/verification-policy.json").read_text()
        )["native_targets"]
        self.assertEqual(expected, set(actual))
        self.assertEqual(len(expected), len(actual))

    def test_ci_executes_the_preparation_phase_and_isolation_contract(self) -> None:
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")
        patterns = re.findall(r"-R '([^']+)'", workflow)
        self.assertTrue(any(
            re.fullmatch(pattern, "performance.preparation_benchmark_contract")
            for pattern in patterns
        ))


if __name__ == "__main__":
    unittest.main()

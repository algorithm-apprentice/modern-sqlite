#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import json
import pathlib
import re
import tempfile
import unittest
from unittest import mock

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
STANDARD_CI_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/ci.yml"
FULL_QUALITY_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/full-quality.yml"


def storage_probe():
    spec = importlib.util.spec_from_file_location(
        "native_temporary_storage_probe",
        REPOSITORY_ROOT / "tools/native_temporary_storage_probe.py",
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("native storage control module is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
    def test_native_storage_probe_preserves_complete_paired_work_and_owned_cleanup(self) -> None:
        probe = storage_probe()
        with tempfile.TemporaryDirectory() as temporary:
            roots = {role: pathlib.Path(temporary) / role for role in ("system", "runner")}
            for root in roots.values():
                root.mkdir()
                (root / "existing").write_bytes(b"untouched")
            output = io.StringIO()
            with mock.patch.object(probe.os, "fsync") as sync:
                probe.run_probe(roots["system"], roots["runner"], output)
            self.assertEqual(300, sync.call_count)
            prefix = "native-storage-probe "
            records = [json.loads(line.removeprefix(prefix)) for line in output.getvalue().splitlines()]
            operations = [record for record in records if record["phase"] == "sync"]
            expected = []
            for index in range(50):
                roles = ("system", "runner") if index % 2 == 0 else ("runner", "system")
                for role in roles:
                    for operation, size in (("journal-header", 512), ("journal-payload", 4096),
                                            ("database-page", 4096)):
                        expected.append((role, index, operation, size))
            self.assertEqual(expected, [
                (record["role"], record["round"], record["operation"], record["bytes"])
                for record in operations
            ])
            for record in operations:
                for key in ("write_ns", "sync_ns", "cpu_ns"):
                    self.assertGreaterEqual(record[key], 0)
            completed = [record for record in records if record["phase"] == "root-complete"]
            self.assertEqual(["system", "runner"], [record["role"] for record in completed])
            for record in completed:
                self.assertEqual((150, 150, 435200),
                                 (record["writes"], record["syncs"], record["verified_bytes"]))
            self.assertEqual({"phase": "complete", "roots": 2, "writes": 300, "syncs": 300}, records[-1])
            for record in records:
                if record["phase"] == "root":
                    child = pathlib.Path(record["owned_child"])
                    self.assertEqual(roots[record["role"]].resolve(), child.parent)
                    self.assertFalse(child.exists())
            for root in roots.values():
                self.assertEqual(["existing"], sorted(path.name for path in root.iterdir()))
                self.assertEqual(b"untouched", (root / "existing").read_bytes())

    def test_native_storage_probe_rejects_both_invalid_roots_before_creating_children(self) -> None:
        probe = storage_probe()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            existing = root / "existing"
            existing.write_bytes(b"untouched")
            for system, runner in ((root / "missing", root), (root, root / "missing"),
                                   (existing, root), (root, existing)):
                with self.subTest(system=system, runner=runner):
                    output = io.StringIO()
                    with self.assertRaises((OSError, ValueError)):
                        probe.run_probe(system, runner, output)
                    self.assertEqual("", output.getvalue())
                    self.assertEqual(["existing"], [path.name for path in root.iterdir()])

    def test_native_storage_probe_rejects_short_writes_before_sync_and_wrong_bytes(self) -> None:
        probe = storage_probe()
        file = mock.Mock()
        file.write.return_value = 2
        with mock.patch.object(probe.os, "fsync") as sync:
            with self.assertRaisesRegex(OSError, "short write"):
                probe.write_and_sync(file, b"full", io.StringIO(), "system", 0, "database-page")
            sync.assert_not_called()
        with self.assertRaisesRegex(OSError, "byte verification"):
            probe.verify_bytes(io.BytesIO(b"wrong"), b"right")

    def test_native_storage_probe_errors_preserve_existing_files_and_remove_owned_children(self) -> None:
        probe = storage_probe()
        for failure in ("sync", "clock", "bytes", "output", "short-output", "backwards-clock"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                (root / "existing").write_bytes(b"untouched")
                output = io.StringIO()
                with contextlib.ExitStack() as patches:
                    patches.enter_context(mock.patch.object(probe.os, "fsync"))
                    if failure == "sync":
                        patches.enter_context(mock.patch.object(
                            probe.os, "fsync", side_effect=OSError("injected sync failure")))
                    elif failure == "clock":
                        patches.enter_context(mock.patch.object(
                            probe.time, "process_time_ns", side_effect=OSError("injected clock failure")))
                    elif failure == "bytes":
                        patches.enter_context(mock.patch.object(
                            probe, "verify_bytes", side_effect=OSError("injected byte failure")))
                    elif failure == "output":
                        patches.enter_context(mock.patch.object(
                            output, "write", side_effect=OSError("injected output failure")))
                    elif failure == "short-output":
                        patches.enter_context(mock.patch.object(output, "write", return_value=0))
                    else:
                        patches.enter_context(mock.patch.object(
                            probe.time, "perf_counter_ns", side_effect=(10, 9, 20)))
                    with self.assertRaises(OSError):
                        probe.run_probe(root, root, output)
                self.assertNotIn('"phase": "complete"', output.getvalue())
                self.assertEqual(["existing"], [path.name for path in root.iterdir()])
                self.assertEqual(b"untouched", (root / "existing").read_bytes())

    def test_native_storage_probe_cli_reports_errors_instead_of_returning_success(self) -> None:
        probe = storage_probe()
        with tempfile.TemporaryDirectory() as temporary:
            arguments = ["probe", "--system-root", temporary, "--runner-root", temporary]
            errors = io.StringIO()
            with mock.patch.object(probe.sys, "argv", arguments), \
                    mock.patch.object(probe.os, "fsync", side_effect=OSError("sync failed")), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(errors):
                self.assertEqual(1, probe.main())
            self.assertIn("native storage control failed: sync failed", errors.getvalue())
            self.assertEqual([], list(pathlib.Path(temporary).iterdir()))

    def test_native_storage_controls_precede_the_unchanged_complete_selection(self) -> None:
        windows = event_block(STANDARD_CI_WORKFLOW.read_text(encoding="utf-8"), "windows-native")
        name = "- name: Measure native temporary storage controls"
        self.assertIn(name, windows)
        control = windows.split(name, 1)[1].split("- name:", 1)[0]
        for required in ("native_temporary_storage_probe.py", '--system-root "$env:TEMP"',
                         '--runner-root "$env:RUNNER_TEMP"'):
            with self.subTest(required=required):
                self.assertIn(required, control)
        self.assertLess(windows.index(name), windows.index(
            "- name: Run the complete declared native correctness tier"
        ))
        self.assertNotRegex(windows, r"\$env:(?:TEMP|TMP)\s*=")
        for excluded in ("continue-on-error", "--repeat", "--retry"):
            self.assertNotIn(excluded, windows)

    def test_native_model_progress_preserves_the_original_complete_owner(self) -> None:
        windows = event_block(STANDARD_CI_WORKFLOW.read_text(encoding="utf-8"), "windows-native")
        name = "- name: Publish native write-owner progress"
        self.assertIn(name, windows)
        publication = windows.split(name, 1)[1].split("- name:", 1)[0]
        for required in ("if: always()", "LastTest.log", "Select-String", "^write-(model|session)-",
                         "write-model-", "write-session-", "TEMP=", "TMP=", "RUNNER_TEMP="):
            with self.subTest(required=required):
                self.assertIn(required, publication)
        self.assertEqual(
            ["ctest --preset windows-debug"],
            re.findall(r"^\s+run:\s+(ctest --preset windows-debug[^\n]*)$", windows, re.MULTILINE),
        )
        self.assertLess(
            windows.index("- name: Run the complete declared native correctness tier"),
            windows.index(name),
        )
        for excluded in ("continue-on-error", "--repeat", "--retry", "run_fast_tests.py"):
            self.assertNotIn(excluded, windows)
        self.assertNotRegex(windows, r"\$env:(?:TEMP|TMP)\s*=")
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        model = re.search(
            r"gtest_discover_tests\(\s+modern_sqlite_write_session_model_test\b(.*?)\n\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(model)
        for required in ("DISCOVERY_MODE PRE_TEST", "LABELS model", "TIMEOUT 20"):
            self.assertIn(required, model.group(1))

    def test_native_windows_executes_the_declared_correctness_selection(self) -> None:
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")
        windows = event_block(workflow, "windows-native")
        for required in (
            "runs-on: windows-2025-vs2026",
            "Native Windows (MSVC)",
            "vswhere.exe",
            "vcvars64.bat",
            "nmake /nologo /f",
            "JIM_TCLSH=",
            "host-tools",
            "file normalize",
            "cb547ab3e931c7766e24834af5ef6c4578863e3d",
            "4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6",
            "cmake --preset windows-debug",
            "cmake --build --preset windows-debug",
            "ctest --preset windows-debug",
            "Build native owning contracts",
            "Run native owning contracts",
            "modern_sqlite_windows_vfs_tests",
            "modern_sqlite_windows_vfs_oom_test",
            "clang-tidy.exe",
            "src/platform/windows_vfs.cpp.obj",
        ):
            with self.subTest(required=required):
                self.assertIn(required, windows)
        self.assertNotIn("msys2/", windows)
        self.assertNotIn("mingw", windows.lower())
        steps = (
            "- name: Build native owning contracts",
            "- name: Run native owning contracts",
            "- name: Build native Windows",
            "- name: Run the complete declared native correctness tier",
        )
        for before, after in zip(steps, steps[1:]):
            self.assertLess(windows.index(before), windows.index(after))
        presets = json.loads((REPOSITORY_ROOT / "CMakePresets.json").read_text())
        configure = next(
            preset for preset in presets["configurePresets"]
            if preset["name"] == "windows-debug"
        )
        self.assertEqual("cl", configure["cacheVariables"]["CMAKE_C_COMPILER"])
        self.assertEqual("cl", configure["cacheVariables"]["CMAKE_CXX_COMPILER"])
        self.assertEqual("${sourceDir}/build/windows-msvc-debug", configure["binaryDir"])
        self.assertEqual({"value": "x64", "strategy": "external"}, configure["architecture"])
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
        for required in ("runs-on: ubuntu-26.04", "CC: clang-22", "CXX: clang++-22",
                         "#include <expected>", "-std=c++23 -pedantic-errors"):
            self.assertIn(required, fuzz)
        for required in ("family: [read, write]", "fail-fast: false", "timeout-minutes: 25",
                         "-runs=20000", "-seed=102", "-timeout=10", "-max_len=65536",
                         "-max_len=4096", "modern_sqlite_${{ matrix.family }}_sql_fuzz",
                         "modern_sqlite_${{ matrix.family }}_database_fuzz"):
            self.assertIn(required, fuzz)

    def test_standard_ci_is_not_triggered_by_labels(self) -> None:
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")

        self.assertNotRegex(workflow, re.compile(r"^      - labeled$", re.MULTILINE))
        self.assertNotRegex(
            workflow, re.compile(r"^  full-quality:$", re.MULTILINE)
        )

    def test_sanitizer_preserves_limits_and_isolation_with_bounded_jobs(self) -> None:
        workflow = event_block(
            STANDARD_CI_WORKFLOW.read_text(encoding="utf-8"), "sanitizer"
        )
        self.assertIn("timeout-minutes: 15", workflow)
        self.assertIn(
            "cmake --preset asan-ubsan -DCMAKE_CXX_COMPILER_LAUNCHER=ccache", workflow
        )
        self.assertIn("cmake --build --preset asan-ubsan --parallel 3", workflow)
        self.assertEqual(
            ["ctest --preset asan-ubsan --parallel 2"],
            re.findall(r"^\s+run:\s+(ctest[^\n]+)$", workflow, re.MULTILINE),
        )
        for excluded in ("run_fast_tests.py", "ASAN_OPTIONS", "UBSAN_OPTIONS"):
            self.assertNotIn(excluded, workflow)

    def test_lowering_oom_retains_one_complete_two_cpu_watchdog_owner(self) -> None:
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        registration = re.search(
            r"add_test\(\s+NAME lowering\.plan_lowering_oom\s+"
            r"COMMAND\s+modern_sqlite_plan_lowering_oom_test\s*\)",
            cmake,
        )
        self.assertIsNotNone(registration)
        properties = re.search(
            r"set_tests_properties\(\s+lowering\.plan_lowering_oom\s+"
            r"lowering\.plan_lowering_oom_thread_domains\s+PROPERTIES(.*?)\n\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(properties)
        for required in ("LABELS unit", "TIMEOUT 10", "PROCESSORS 2"):
            self.assertIn(required, properties.group(1))
        self.assertRegex(
            cmake,
            r"NAME lowering\.plan_lowering_oom_thread_domains\s+"
            r"COMMAND modern_sqlite_plan_lowering_oom_test --verify-thread-domains",
        )
        source = (
            REPOSITORY_ROOT / "tests/unit/lowering/plan_lowering_oom_test.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("std::array<std::jthread, 2> workers", source)
        self.assertIn("static_assert(case_count == 162U)", source)
        self.assertIn("failure < allocation_count", source)
        self.assertIn("item.prefixes != item.allocations", source)
        self.assertIn("item.recoveries != 1U", source)
        self.assertNotIn("--allocation-partition", source)
        for relative in (
            "tests/unit/lowering/plan_lowering_oom_test.cpp",
            "tests/unit/optimizer/physical_plan_oom_test.cpp",
        ):
            self.assertIn(
                '#include "tests/support/compiler_allocation_fault.hpp"',
                (REPOSITORY_ROOT / relative).read_text(encoding="utf-8"),
            )

    def test_sanitizer_discovery_partitions_preserve_lowering_and_parallel_units(self) -> None:
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        family = "_modern_sqlite_sanitizer_serial_unit_filter"
        complement = "_modern_sqlite_unit_discovery_filter"
        serial_properties = "_modern_sqlite_sanitizer_serial_properties"
        self.assertRegex(
            cmake,
            rf'set\({family}\s+"ReadLowering\*\.\*"\)',
        )
        self.assertRegex(
            cmake,
            rf'set\({complement}\s+TEST_FILTER\s+"-\$\{{{family}\}}"\)',
        )
        discoveries = re.findall(
            r"gtest_discover_tests\(\s+modern_sqlite_unit_tests\b(.*?)\n\s*\)",
            cmake, re.DOTALL,
        )
        self.assertEqual(2, len(discoveries))
        protected = [body for body in discoveries if f'TEST_FILTER "${{{family}}}"' in body]
        parallel = [body for body in discoveries if f"${{{complement}}}" in body]
        self.assertEqual(1, len(protected))
        self.assertEqual(1, len(parallel))
        self.assertIn(f"${{{serial_properties}}}", protected[0])
        self.assertNotIn(serial_properties, parallel[0])
        for body in discoveries:
            self.assertIn("DISCOVERY_MODE PRE_TEST", body)
            self.assertIn("LABELS unit", body)
            self.assertIn("TIMEOUT 10", body)
            self.assertNotIn("TEST_PREFIX", body)
            self.assertNotIn("TEST_SUFFIX", body)

    def test_sanitizer_lowering_and_oom_retain_uncontended_watchdog_domains(self) -> None:
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        properties = "_modern_sqlite_sanitizer_serial_properties"
        self.assertIsNotNone(
            re.search(
                rf"if\(MODERN_SQLITE_ENABLE_ASAN\)\s+"
                rf"set\({properties}\s+RUN_SERIAL\s+TRUE\)", cmake,
            ),
            "ASan must retain the shared uncontended resource-property list",
        )
        unit_discovery = re.search(
            r"gtest_discover_tests\(\s+modern_sqlite_unit_tests\b(.*?)\n\s*\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(unit_discovery)
        self.assertIn(
            'TEST_FILTER "${_modern_sqlite_sanitizer_serial_unit_filter}"',
            unit_discovery.group(1),
        )
        self.assertIn(f"${{{properties}}}", unit_discovery.group(1))
        oom_cases = re.search(
            r"foreach\(partition RANGE 0 3\)(.*?)\nendforeach\(\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(oom_cases)
        self.assertIn("TIMEOUT 10 RESOURCE_LOCK session_read_oom_databases", oom_cases.group(1))
        self.assertIn(f"${{{properties}}}", oom_cases.group(1))

    def test_sanitizer_native_differentials_retain_uncontended_deadlines(self) -> None:
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        registrations = re.findall(
            r"add_test\(\s+NAME (differential\.\w+_compatibility)\s+"
            r"COMMAND(.*?)\n\)", cmake, re.DOTALL,
        )
        self.assertEqual(7, len(registrations))
        for name, command in registrations:
            with self.subTest(name=name):
                self.assertIn("tests/differential/read_compatibility_test.py", command)
                properties = re.search(
                    rf"set_tests_properties\(\s+{re.escape(name)}\s+"
                    r"PROPERTIES(.*?)\n\)", cmake, re.DOTALL,
                )
                self.assertIsNotNone(properties)
                self.assertIn("TIMEOUT 60", properties.group(1))
                self.assertIn(
                    "${_modern_sqlite_sanitizer_serial_properties}", properties.group(1)
                )
        tool_control = re.search(
            r"set_tests_properties\(\s+differential\.read_compatibility_tool\s+"
            r"PROPERTIES(.*?)\n\)", cmake, re.DOTALL,
        )
        self.assertIsNotNone(tool_control)
        self.assertNotIn("_modern_sqlite_sanitizer_serial_properties", tool_control.group(1))

    def test_read_session_preparation_sweeps_retain_every_partition_and_deadline(self) -> None:
        source = (
            REPOSITORY_ROOT / "tests/unit/session/read_session_oom_test.cpp"
        ).read_text(encoding="utf-8")
        cases = re.search(r"kPrepareConsumers\{(.*?)\n\};", source, re.DOTALL)
        self.assertIsNotNone(cases)
        names = re.findall(r'\.name\s*=\s*"([^"]+)"', cases.group(1))
        self.assertEqual(10, len(names))
        self.assertEqual(len(names), len(set(names)))
        cmake = (REPOSITORY_ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        registrations = re.search(
            r"foreach\(prepare_case IN ITEMS(.*?)\)(.*?)\nendforeach\(\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(registrations)
        self.assertEqual(sorted(names), sorted(registrations.group(1).split()))
        body = registrations.group(2)
        for required in (
            "foreach(partition RANGE 0 3)",
            "session.read_session_oom.prepare_${prepare_case}.${partition}",
            '--prepare-consumer "${prepare_case}"',
            '--allocation-partition "${partition}"',
            "LABELS unit TIMEOUT 10 RESOURCE_LOCK session_read_oom_databases",
            "${_modern_sqlite_sanitizer_serial_properties}",
        ):
            with self.subTest(required=required):
                self.assertIn(required, body)
        self.assertIn("NAME session.read_session_oom.invalid_prepare_selector", cmake)

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

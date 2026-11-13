#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/verify.py"


def initialize_repository(root: Path, *, commit: bool = True) -> None:
    subprocess.run(["git", "init", "--quiet", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    if commit:
        subprocess.run(
            ["git", "-C", str(root), "-c", "user.name=Test",
             "-c", "user.email=test@example.invalid", "commit", "--quiet", "-m", "base"],
            check=True,
        )


class VerificationPlanTest(unittest.TestCase):
    def run_plan(self, *paths: str) -> dict:
        command = [sys.executable, str(TOOL), "plan", "--format", "json"]
        for path in paths:
            command.extend(["--changed-file", path])
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(0, result.returncode, result.stderr)
        return json.loads(result.stdout)

    def test_documentation_change_does_not_schedule_engine_validation(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "plan",
                "--changed-file",
                "docs/example.md",
                "--format",
                "json",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        plan = json.loads(result.stdout)
        self.assertEqual("plan", plan["status"])
        self.assertEqual(["documentation"], plan["domains"])
        self.assertEqual("documentation", plan["profile"])
        commands = [step["command"] for step in plan["steps"]]
        self.assertFalse(any(command[0] in {"ctest", "cmake"} for command in commands))
        self.assertIn("documentation-review", plan["outstanding_requirements"])

    def test_unknown_change_escalates_to_full(self) -> None:
        plan = self.run_plan("unmapped/component.cpp")
        self.assertEqual("full", plan["profile"])
        self.assertIn("unknown", plan["domains"])
        self.assertTrue(any(step["id"] == "isolated-asan-ubsan" for step in plan["steps"]))

    def test_unknown_tool_cannot_claim_generic_tool_tests_as_its_owner(self) -> None:
        plan = self.run_plan("tools/unregistered_checker.py")
        self.assertEqual("full", plan["profile"])
        self.assertIn("owned-tests-reviewed-and-executed", plan["outstanding_requirements"])

    def test_test_registration_only_change_uses_discovery_integration(self) -> None:
        plan = self.run_plan("tests/CMakeLists.txt")
        self.assertEqual("tooling", plan["profile"])
        self.assertIn("configured-ctest-discovery-reconciliation", plan["outstanding_requirements"])
        self.assertTrue(any(step["id"] == "owned-cmake" for step in plan["steps"]))

    def test_shared_public_header_escalates_to_full(self) -> None:
        plan = self.run_plan("include/modern_sqlite/base/result.hpp")
        self.assertEqual("full", plan["profile"])
        self.assertIn("foundation", plan["domains"])

    def test_vm_change_uses_the_known_sql_domain(self) -> None:
        plan = self.run_plan("src/vm/vm.cpp")
        self.assertEqual("targeted", plan["profile"])
        self.assertIn("sql", plan["domains"])
        self.assertNotIn("unknown", plan["domains"])

    def test_durable_and_shared_cache_changes_escalate_to_full(self) -> None:
        for path in (
            "src/storage/btree/writer_internal.cpp",
            "src/storage/btree/writer_internal.hpp",
            "src/pager/write_pager.cpp",
            "src/transaction/transaction_coordinator.cpp",
            "src/platform/posix_vfs.cpp",
            "src/storage/cache/page_cache.cpp",
        ):
            with self.subTest(path=path):
                self.assertEqual("full", self.run_plan(path)["profile"])

    def test_full_durability_plan_selects_write_campaigns(self) -> None:
        plan = self.run_plan("src/pager/write_pager.cpp")
        commands = [" ".join(step["command"]) for step in plan["steps"]]
        self.assertTrue(any("modern_sqlite_write_sql_fuzz" in command for command in commands))
        self.assertTrue(any("modern_sqlite_write_database_fuzz" in command for command in commands))

    def test_tool_and_test_changes_run_their_actual_owners(self) -> None:
        for path, expected in (
            ("tests/project/test_project_graph.py", "-s tests/project"),
            ("tools/read_compatibility.py", "-L differential"),
            ("tests/differential/read_compatibility_test.py", "differential"),
            ("tests/cli/read_trace_cli_test.py", "-L cli"),
            ("tests/cmake/sanitizer_scope/CMakeLists.txt", "-L cmake"),
        ):
            with self.subTest(path=path):
                plan = self.run_plan(path)
                self.assertTrue(
                    any(expected in " ".join(step["command"]) for step in plan["steps"])
                    or plan["profile"] == "full"
                )

    def test_manifest_automation_is_tooling_not_document_only(self) -> None:
        plan = self.run_plan("project/manifest.json")
        self.assertNotEqual("documentation", plan["profile"])
        self.assertIn("manifest-automation-review", plan["outstanding_requirements"])

    def test_production_plan_requires_focused_behavior_evidence(self) -> None:
        plan = self.run_plan("src/storage/btree/cursor.cpp")
        self.assertIn(
            "focused-red-green-or-green-refactor-baseline",
            plan["outstanding_requirements"],
        )

    def test_mixed_changes_take_domain_union(self) -> None:
        plan = self.run_plan("docs/example.md", "src/storage/btree/cursor.cpp")
        self.assertEqual(["documentation", "storage"], plan["domains"])
        self.assertEqual("targeted", plan["profile"])
        self.assertTrue(any("run_fast_tests.py" in " ".join(s["command"]) for s in plan["steps"]))
        self.assertTrue(any(s["id"] == "affected-asan" for s in plan["steps"]))
        self.assertTrue(any(s["id"] == "native-contracts" for s in plan["steps"]))

    def test_changed_analysis_combines_all_explicit_sources_in_one_reported_step(self) -> None:
        paths = ("src/binder/bound_select.cpp", "src/optimizer/physical_plan.cpp")
        plan = self.run_plan(*paths)
        steps = [step for step in plan["steps"] if step["id"].startswith("tidy-")]
        self.assertEqual(1, len(steps))
        command = steps[0]["command"]
        self.assertEqual(2, command.count("--smoke-source"))
        self.assertTrue(all(path in command for path in paths))
        self.assertIn("--report", command)
        self.assertEqual(
            command[command.index("--report") + 1], command[command.index("--reuse-report") + 1],
        )
        self.assertEqual(
            {"shared-checks", "fast-local", "tidy-changed"},
            {step["id"] for step in plan["steps"] if step.get("writes_report")},
        )

    def test_registered_python_performance_tests_select_exact_owners_without_engine_work(self) -> None:
        plan = self.run_plan(
            "tools/verify.py",
            "tests/performance/read_performance_tool_test.py",
            "tests/performance/write_performance_tool_test.py",
        )
        self.assertEqual("tooling", plan["profile"])
        commands = {step["id"]: step["command"] for step in plan["steps"]}
        self.assertIn("configure-dev-debug", commands)
        self.assertIn("tool-tests", commands)
        owners = [command for command in commands.values() if command[0] == "ctest"]
        self.assertEqual(2, len(owners))
        self.assertEqual(
            {r"^performance\.read_performance_tool$", r"^performance\.write_performance_tool$"},
            {command[command.index("-R") + 1] for command in owners},
        )
        self.assertTrue(all("--no-tests=error" in command for command in owners))
        self.assertNotIn("native-build", commands)
        self.assertNotIn("build-dev-debug", commands)

    def test_performance_owner_recognition_does_not_reduce_unknown_or_runtime_coverage(self) -> None:
        for paths in (
            ("tests/performance/unregistered_tool_test.py",),
            ("tests/performance/nested/read_performance_tool_test.py",),
            ("tools/read_performance.py",),
            ("benchmarks/read_performance.cpp",),
            ("tests/performance/read_performance_tool_test.py", "src/binder/bound_select.cpp"),
        ):
            with self.subTest(paths=paths):
                plan = self.run_plan(*paths)
                self.assertIn(plan["profile"], {"targeted", "full"})
                self.assertTrue(any(step["id"] == "native-build" for step in plan["steps"]))

    def test_shared_checks_appear_once_and_are_excluded_later(self) -> None:
        plan = self.run_plan("src/storage/btree/page.cpp")
        shared = [s for s in plan["steps"] if s["id"] == "shared-checks"]
        self.assertEqual(1, len(shared))
        for step in plan["steps"]:
            if step["command"][0] == "ctest" and step["id"] != "shared-checks":
                with self.subTest(step=step["id"]):
                    self.assertIn("-LE", step["command"])
                    exclusion = step["command"][step["command"].index("-LE") + 1]
                    self.assertIn("configuration-independent", exclusion)

    def test_ci_and_original_model_are_outstanding_not_inferred_passes(self) -> None:
        plan = self.run_plan("src/runtime/sql_value.cpp")
        self.assertIn("required-exact-head-hosted-checks", plan["outstanding_requirements"])
        self.assertIn("isolated-sanitizer-original-model", plan["outstanding_requirements"])
        self.assertEqual("plan", plan["status"])

    def test_new_cli_does_not_offer_lower_risk_override(self) -> None:
        result = subprocess.run(
            [sys.executable, str(TOOL), "plan", "--changed-file", "unknown.cpp",
             "--profile", "documentation"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(0, result.returncode)

    def test_rejects_paths_outside_repository(self) -> None:
        for path in ("../outside.cpp", "/outside.cpp"):
            result = subprocess.run(
                [sys.executable, str(TOOL), "plan", "--changed-file", path],
                cwd=ROOT, capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("path", result.stderr.lower())

    def test_git_changes_include_deletion_rename_and_untracked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "project").mkdir()
            (root / "project/verification-policy.json").write_bytes(
                (ROOT / "project/verification-policy.json").read_bytes()
            )
            for name in ("old.cpp", "remove.cpp"):
                (root / name).write_text("initial\n")
            initialize_repository(root)
            (root / "old.cpp").rename(root / "new.cpp")
            (root / "remove.cpp").unlink()
            (root / "untracked.cpp").write_text("new\n")
            subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
            (root / "unstaged.cpp").write_text("untracked after staging\n")
            result = subprocess.run(
                [sys.executable, str(TOOL), "plan", "--repository-root", str(root),
                 "--base-ref", "HEAD", "--format", "json"],
                cwd=ROOT, capture_output=True, text=True, check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            plan = json.loads(result.stdout)
            self.assertEqual(
                {"old.cpp", "new.cpp", "remove.cpp", "untracked.cpp", "unstaged.cpp"},
                set(plan["changes"]),
            )
            self.assertEqual("full", plan["profile"])

    def test_native_target_set_is_canonical_and_targeted(self) -> None:
        result = subprocess.run(
            [sys.executable, str(TOOL), "native-targets", "--format", "json"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        targets = json.loads(result.stdout)
        self.assertEqual(20, len(targets))
        self.assertEqual(20, len(set(targets)))
        self.assertIn("modern_sqlite_aggregate_benchmark", targets)
        self.assertIn("modern_sqlite_aggregate_diagnostics", targets)
        self.assertIn("modern_sqlite_preparation_benchmark", targets)
        self.assertIn("modern_sqlite_preparation_diagnostics", targets)
        self.assertNotIn("modern_sqlite_unit_tests", targets)

    def test_build_native_uses_explicit_target_set(self) -> None:
        from tools.verify import build_native, load_policy

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "MODERN_SQLITE_BUILD_BENCHMARKS:BOOL=ON\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n"
            )
            with patch("tools.verify.subprocess.run") as run:
                run.return_value = subprocess.CompletedProcess([], 0)
                self.assertEqual(0, build_native(build, 2, load_policy(ROOT), ROOT))
                run.assert_called_once()
                command = run.call_args.args[0]
                self.assertEqual(
                    ["cmake", "--build", str(build), "--parallel", "2"], command[:5]
                )
                self.assertEqual({"cwd": ROOT, "check": False}, run.call_args.kwargs)
            arguments = command[1:]
            self.assertIn("--target", arguments)
            targets = arguments[arguments.index("--target") + 1:]
            self.assertEqual(
                json.loads((ROOT / "project/verification-policy.json").read_text())["native_targets"],
                targets,
            )

    def test_build_native_rejects_instrumented_timing_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "MODERN_SQLITE_BUILD_BENCHMARKS:BOOL=ON\n"
                "MODERN_SQLITE_ENABLE_INSTRUMENTATION:BOOL=ON\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n"
            )
            result = subprocess.run(
                [sys.executable, str(TOOL), "build-native", "--build-directory", str(build)],
                cwd=ROOT, capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("instrument", result.stderr.lower())


class VerificationCmakeTest(unittest.TestCase):
    def test_shared_label_is_additive_without_changing_command_or_deadline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "project").mkdir()
            (root / "project/verification-policy.json").write_bytes(
                (ROOT / "project/verification-policy.json").read_bytes()
            )
            helper = (ROOT / "cmake/Verification.cmake").as_posix()
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(verification_fixture NONE)\n"
                "enable_testing()\n"
                "add_test(NAME project.graph COMMAND \"${CMAKE_COMMAND}\" -E echo checked)\n"
                "set_tests_properties(project.graph PROPERTIES LABELS project TIMEOUT 10)\n"
                f'include("{helper}")\n'
                "modern_sqlite_label_shared_verification_tests()\n"
            )
            build = root / "build"
            configured = subprocess.run(
                ["cmake", "-G", "Ninja", "-S", str(root), "-B", str(build)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(0, configured.returncode, configured.stderr)
            discovered = subprocess.run(
                ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(0, discovered.returncode, discovered.stderr)
            tests = json.loads(discovered.stdout)["tests"]
            self.assertEqual(1, len(tests))
            properties = {p["name"]: p["value"] for p in tests[0]["properties"]}
            self.assertEqual({"project", "configuration-independent"}, set(properties["LABELS"]))
            self.assertEqual(10, properties["TIMEOUT"])
            self.assertEqual(["-E", "echo", "checked"], tests[0]["command"][1:])


class VerificationExecutionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repository"
        self.root.mkdir()
        (self.root / "project").mkdir()
        (self.root / "tools").mkdir()
        (self.root / "project/verification-policy.json").write_bytes(
            (ROOT / "project/verification-policy.json").read_bytes()
        )
        (self.root / "tools/project_graph.py").write_text("print('graph checked')\n")
        (self.root / ".gitignore").write_text("/build/\n")
        initialize_repository(self.root, commit=False)
        self.report = Path(self.temporary.name) / "result.json"

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_run_executes_the_documentation_plan_and_preserves_outstanding_gates(self) -> None:
        result = subprocess.run(
            [sys.executable, str(TOOL), "run", "--repository-root", str(self.root),
             "--changed-file", "docs/example.md", "--report", str(self.report)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual("passed", report["status"])
        self.assertFalse(report["qualification_complete"])
        self.assertEqual(["documentation-review"], report["outstanding_requirements"])
        self.assertEqual(["graph"], [step["id"] for step in report["steps"]])
        self.assertEqual("passed", report["steps"][0]["status"])
        self.assertIn("graph checked", Path(report["steps"][0]["log"]).read_text())

    def plan(self, *programs: str) -> dict:
        return {
            "schema_version": 1, "status": "plan", "profile": "tooling",
            "outstanding_requirements": ["required-exact-head-hosted-checks"],
            "steps": [
                {"id": f"step-{index}", "command": [sys.executable, "-c", program],
                 "purpose": "Owned execution fixture."}
                for index, program in enumerate(programs)
            ],
        }

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_failed_step_keeps_logs_and_prevents_later_execution(self) -> None:
        from tools.verify import execute_plan

        marker = self.root / "build" / "must-not-run"
        report = execute_plan(
            self.root, self.plan("print('first')", "print('failure'); raise SystemExit(7)",
                                 f"from pathlib import Path; Path({str(marker)!r}).touch()"),
            self.report,
        )
        self.assertEqual("failed", report["status"])
        self.assertEqual(["passed", "failed", "not-run"],
                         [step["status"] for step in report["steps"]])
        self.assertEqual(7, report["steps"][1]["return_code"])
        self.assertIn("failure", Path(report["steps"][1]["log"]).read_text())
        self.assertFalse(marker.exists())
        self.assertEqual(report, json.loads(self.report.read_text()))

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_declared_report_creation_and_same_path_reuse_are_not_input_drift(self) -> None:
        import hashlib
        from tools.verify import execute_plan

        child_report = self.root / "build" / "child.json"
        child_report.parent.mkdir()
        script = self.root / "tools" / "report_writer.py"
        script.write_text(
            "import sys\nfrom pathlib import Path\n"
            "target=Path(sys.argv[sys.argv.index('--report')+1])\n"
            "previous=Path(sys.argv[sys.argv.index('--reuse-report')+1])\n"
            "print(previous.read_text() if previous.is_file() else 'new report')\n"
            "target.write_text('updated report')\n"
        )
        for previous in (None, "prior report"):
            with self.subTest(previous=previous):
                child_report.unlink(missing_ok=True)
                if previous is not None:
                    child_report.write_text(previous)
                plan = self.plan("unused")
                plan["steps"][0].update(
                    command=[sys.executable, str(script), "--report", str(child_report),
                             "--reuse-report", str(child_report)],
                    writes_report=True,
                )
                result = execute_plan(self.root, plan, self.report)
                self.assertEqual("passed", result["status"], result)
                step = result["steps"][0]
                self.assertEqual(str(child_report.resolve()), step["report_output"])
                self.assertNotIn(str(child_report.resolve()), step["command_files_sha256"])
                self.assertEqual(
                    None if previous is None else hashlib.sha256(previous.encode()).hexdigest(),
                    step["report_before_sha256"],
                )
                self.assertEqual(
                    hashlib.sha256(child_report.read_bytes()).hexdigest(), step["report_after_sha256"],
                )
                self.assertIn(previous or "new report", Path(step["log"]).read_text())

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_report_declaration_does_not_exempt_a_distinct_consumed_file(self) -> None:
        from tools.verify import execute_plan

        directory = self.root / "build"
        directory.mkdir()
        previous = directory / "previous.json"
        previous.write_text("unchanged")
        target = directory / "next.json"
        script = self.root / "tools" / "writer.py"
        script.write_text(
            "import sys\nfrom pathlib import Path\n"
            "Path(sys.argv[sys.argv.index('--reuse-report')+1]).write_text('drift')\n"
            "Path(sys.argv[sys.argv.index('--report')+1]).write_text('output')\n"
        )
        plan = self.plan("unused")
        plan["steps"][0].update(
            command=[sys.executable, str(script), "--report", str(target),
                     "--reuse-report", str(previous)],
            writes_report=True,
        )
        failed = execute_plan(self.root, plan, self.report)
        self.assertEqual("failed", failed["status"])
        self.assertIn(str(previous.resolve()), failed["steps"][0]["command_files_sha256"])
        self.assertIn("input identity", failed["steps"][0]["error"])
        target.write_text("before unmarked execution")
        plan["steps"][0]["writes_report"] = False
        plan["steps"][0]["command"][-1] = str(target)
        unmarked = execute_plan(self.root, plan, self.report)
        self.assertEqual("failed", unmarked["status"])
        self.assertIn(str(target.resolve()), unmarked["steps"][0]["command_files_sha256"])

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_report_output_cannot_alias_an_ignored_entry_script(self) -> None:
        from tools.verify import execute_plan

        script = self.root / "build" / "entry.py"
        script.parent.mkdir()
        source = (
            "import sys\nfrom pathlib import Path\n"
            "Path(sys.argv[sys.argv.index('--report')+1]).write_text('overwritten')\n"
        )
        script.write_text(source)
        symbolic = script.with_name("symbolic.py")
        symbolic.symlink_to(script)
        hard = script.with_name("hard.py")
        hard.hardlink_to(script)
        for target in (script, symbolic, hard):
            with self.subTest(target=target.name):
                script.write_text(source)
                plan = self.plan("unused")
                plan["steps"][0].update(
                    command=[sys.executable, str(script), "--report", str(target)],
                    writes_report=True,
                )
                failed = execute_plan(self.root, plan, self.report)
                self.assertEqual("failed", failed["status"])
                self.assertEqual(source, script.read_text())
                self.assertIn("entry", failed["error"])
                self.assertNotIn("return_code", failed["steps"][0])

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_only_the_explicit_reuse_argument_can_overlap_a_report_output(self) -> None:
        from tools.verify import execute_plan

        target = self.root / "build" / "input.json"
        target.parent.mkdir()
        target.write_text("original input")
        script = self.root / "tools" / "writer.py"
        script.write_text(
            "import sys\nfrom pathlib import Path\n"
            "Path(sys.argv[-1]).write_text('overwritten')\n"
        )
        plan = self.plan("unused")
        plan["steps"][0].update(
            command=[sys.executable, str(script), "--report", str(target), "--input", str(target)],
            writes_report=True,
        )
        failed = execute_plan(self.root, plan, self.report)
        self.assertEqual("failed", failed["status"])
        self.assertEqual("original input", target.read_text())
        self.assertIn("immutable input", failed["error"])
        self.assertNotIn("return_code", failed["steps"][0])

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_source_drift_and_launch_errors_cannot_produce_a_pass(self) -> None:
        from tools.verify import execute_plan

        plan = self.plan(
            "from pathlib import Path; Path('tools/project_graph.py').write_text('changed')",
            "print('not reached')",
        )
        report = execute_plan(self.root, plan, self.report)
        self.assertEqual("failed", report["status"])
        self.assertIn("input", report["error"].lower())
        self.assertFalse(report["inputs_unchanged"])
        plan = self.plan("print('unused')")
        plan["steps"][0]["command"] = [str(self.root / "missing-executable")]
        failed = execute_plan(self.root, plan, self.report)
        self.assertEqual("failed", failed["status"])
        self.assertIn("error", failed["steps"][0])

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_report_cannot_overwrite_source_or_enter_unignored_inputs(self) -> None:
        from tools.verify import VerificationError, execute_plan

        for path in (self.root / "tools/project_graph.py", self.root / "unignored.json"):
            with self.subTest(path=path), self.assertRaises(VerificationError):
                execute_plan(self.root, self.plan("print('must not run')"), path)
        self.assertEqual("print('graph checked')\n", (self.root / "tools/project_graph.py").read_text())
        self.assertFalse((self.root / "unignored.json").exists())
        ignored = self.root / "build" / "verification" / "result.json"
        self.assertEqual("passed", execute_plan(self.root, self.plan("print('checked')"), ignored)["status"])

    def test_directory_mapping_preserves_selectors_and_quality_tool_options(self) -> None:
        directory = Path(self.temporary.name) / "development space"
        quality = Path(self.temporary.name) / "quality"
        command = [
            sys.executable, str(TOOL), "plan", "--changed-file", "src/binder/bound_select.cpp",
            "--configuration", f"dev-debug={directory}",
            "--configuration", f"clang-tidy={quality}",
            "--clang-format-executable", sys.executable, "--format", "json",
        ]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(0, result.returncode, result.stderr)
        plan = json.loads(result.stdout)
        steps = {step["id"]: step["command"] for step in plan["steps"]}
        self.assertEqual(str(directory.resolve()), steps["configure-dev-debug"][-1])
        self.assertEqual(["cmake", "--build", str(directory.resolve()), "--parallel", "2"],
                         steps["build-dev-debug"])
        self.assertIn(str(directory.resolve()), steps["fast-local"])
        self.assertIn(str(directory.resolve()), steps["development-remainder"])
        self.assertIn("unit|configuration-independent", steps["development-remainder"])
        self.assertIn(f"-DMODERN_SQLITE_CLANG_FORMAT_EXECUTABLE={Path(sys.executable).resolve()}",
                      steps["configure-quality"])
        self.assertIn(str(quality.resolve()), steps["tidy-changed"])
        analysis = str(quality.resolve() / "verification" / "changed-analysis.json")
        self.assertEqual(2, steps["tidy-changed"].count(analysis))
        outputs = {
            step["id"]: step["command"][step["command"].index("--report") + 1]
            for step in plan["steps"] if step.get("writes_report")
        }
        self.assertEqual(
            {
                "shared-checks": str(directory.resolve() / "verification" / "shared.json"),
                "fast-local": str(directory.resolve() / "verification" / "fast-local.json"),
                "tidy-changed": analysis,
            },
            outputs,
        )
        duplicate = [*command[:-2], "--configuration", f"asan-ubsan={directory}", "--format", "json"]
        rejected = subprocess.run(duplicate, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertNotEqual(0, rejected.returncode)
        self.assertIn("configuration", rejected.stderr.lower())

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_interruption_is_recorded_without_running_the_remaining_plan(self) -> None:
        from tools.verify import execute_plan

        with patch("tools.verify.run_owned_command", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            execute_plan(self.root, self.plan("print('interrupted')", "print('not reached')"), self.report)
        report = json.loads(self.report.read_text())
        self.assertEqual("interrupted", report["status"])
        self.assertEqual(["interrupted", "not-run"], [step["status"] for step in report["steps"]])

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_interruption_retires_a_real_descendant_before_report_finalization(self) -> None:
        from tools.verify import execute_plan

        marker = self.root / "build" / "writes"
        marker.parent.mkdir()
        child = (
            "import time;from pathlib import Path;"
            f"p=Path({str(marker)!r});"
            "f=p.open('ab',buffering=0)\n"
            "while True:f.write(b'x');time.sleep(0.01)\n"
        )
        parent = (
            "import os,signal,subprocess,sys,time\n"
            f"child=subprocess.Popen([sys.executable,'-c',{child!r}],start_new_session=True)\n"
            "try:time.sleep(60)\n"
            "finally:\n"
            " os.killpg(child.pid,signal.SIGKILL)\n"
            " child.wait()\n"
        )

        def interrupt_after_write(process):
            deadline = time.monotonic() + 5
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists())
            raise KeyboardInterrupt

        with patch("tools.verify.wait_owned_process", side_effect=interrupt_after_write), self.assertRaises(KeyboardInterrupt):
            execute_plan(self.root, self.plan(parent), self.report)
        size = marker.stat().st_size
        time.sleep(0.1)
        self.assertEqual(size, marker.stat().st_size)
        self.assertEqual("interrupted", json.loads(self.report.read_text())["status"])

    def test_configuration_aliases_and_incompatible_caches_fail_before_execution(self) -> None:
        import argparse
        from tools.verify import VerificationError, configure_plan, load_policy, make_plan

        directory = self.root / "build" / "development"
        directory.mkdir(parents=True)
        alias = self.root / "build" / "alias"
        alias.symlink_to(directory, target_is_directory=True)
        args = argparse.Namespace(
            operation="run", base_ref=None, profile="auto",
            gnu_debug_directory=None, gnu_release_directory=None,
            configuration=[f"dev-debug={directory}", f"asan-ubsan={alias}"],
            clang_format_executable=None, clang_tidy_executable=None,
        )

        def verify():
            return configure_plan(self.root, make_plan(
                self.root, ["src/binder/bound_select.cpp"], load_policy(self.root), args,
            ), args)

        with self.assertRaisesRegex(VerificationError, "share"):
            verify()
        args.configuration = [f"dev-debug={directory}"]
        cache = directory / "CMakeCache.txt"
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nCMAKE_BUILD_TYPE:STRING=Release\n")
        with self.assertRaisesRegex(VerificationError, "build type"):
            verify()
        cache.write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nCMAKE_BUILD_TYPE:STRING=Debug\n"
            "MODERN_SQLITE_ENABLE_ASAN:BOOL=ON\n"
        )
        with self.assertRaisesRegex(VerificationError, "ASAN"):
            verify()
        cache.write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root.parent}\nCMAKE_BUILD_TYPE:STRING=Debug\n"
        )
        with self.assertRaisesRegex(VerificationError, "checkout"):
            verify()

    def test_configuration_source_root_alias_resolves_to_its_existing_owner(self) -> None:
        import argparse
        from tools.verify import build_native, configure_plan, load_policy, make_plan, validate_configuration

        alias = self.root.parent / "source-alias"
        alias.symlink_to(self.root, target_is_directory=True)
        directory = self.root / "build" / "dev-debug"
        directory.mkdir(parents=True)
        (directory / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root.resolve()}\nCMAKE_BUILD_TYPE:STRING=Debug\n"
        )
        args = argparse.Namespace(
            operation="run", base_ref=None, profile="auto",
            gnu_debug_directory=None, gnu_release_directory=None, configuration=None,
            clang_format_executable=None, clang_tidy_executable=None,
        )
        plan = make_plan(
            alias, ["tests/performance/read_performance_tool_test.py"], load_policy(alias), args,
        )
        result = configure_plan(alias, plan, args)
        self.assertTrue(result["physical_configurations"][str(directory.resolve())]["cache_preflight"])
        self.assertEqual("Debug", validate_configuration(alias, directory, "dev-debug")["CMAKE_BUILD_TYPE"])
        (directory / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root.resolve()}\n"
            "CMAKE_BUILD_TYPE:STRING=Release\nMODERN_SQLITE_BUILD_BENCHMARKS:BOOL=ON\n"
        )
        with patch("tools.verify.subprocess.run") as run:
            run.return_value.returncode = 0
            self.assertEqual(0, build_native(directory, 2, load_policy(ROOT), alias))
            run.assert_called_once()

    def test_missing_or_empty_cache_owner_is_not_inferred_from_the_working_directory(self) -> None:
        from tools.verify import VerificationError, build_native, load_policy, validate_configuration

        directory = self.root / "build"
        directory.mkdir()
        for owner in ("", "CMAKE_HOME_DIRECTORY:INTERNAL=\n"):
            with self.subTest(owner=owner):
                (directory / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\n" + owner)
                with self.assertRaisesRegex(VerificationError, "checkout"):
                    validate_configuration(Path.cwd().resolve(), directory, "dev-debug")
                (directory / "CMakeCache.txt").write_text(
                    "CMAKE_BUILD_TYPE:STRING=Release\nMODERN_SQLITE_BUILD_BENCHMARKS:BOOL=ON\n" + owner
                )
                with patch("tools.verify.subprocess.run") as run:
                    run.return_value.returncode = 0
                    with self.assertRaisesRegex(VerificationError, "checkout"):
                        build_native(directory, 2, load_policy(ROOT), Path.cwd().resolve())
                    run.assert_not_called()

    def test_unsupported_process_ownership_fails_before_artifact_creation(self) -> None:
        from tools.verify import VerificationError, execute_plan

        with patch("tools.verify.os.killpg", None, create=True), self.assertRaisesRegex(
            VerificationError, "POSIX owned process groups",
        ):
            execute_plan(self.root, self.plan("print('must not run')"), self.report)
        self.assertFalse(self.report.exists())
        self.assertFalse(self.report.with_name(self.report.name + ".lock").exists())

    def test_cli_rejects_unsupported_host_before_git_discovery_or_tool_preflight(self) -> None:
        script = (
            "import sys;from tools import verify;"
            "verify.os.killpg=None;"
            f"sys.argv=['verify','run','--repository-root',{str(self.root)!r},"
            f"'--base-ref','nonexistent-base','--report',{str(self.report)!r}];"
            "sys.exit(verify.main())"
        )
        result = subprocess.run(
            [sys.executable, "-c", script], cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("POSIX owned process groups", result.stderr)
        self.assertNotIn("nonexistent-base...", result.stderr)
        self.assertFalse(self.report.exists())

    def test_directory_mapping_does_not_redirect_explicit_gnu_coverage(self) -> None:
        directory = Path(self.temporary.name) / "development"
        result = subprocess.run(
            [sys.executable, str(TOOL), "plan", "--changed-file", "src/binder/bound_select.cpp",
             "--configuration", f"dev-debug={directory}", "--gnu-debug-directory", "build/dev-debug",
             "--format", "json"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        steps = {step["id"]: step["command"] for step in json.loads(result.stdout)["steps"]}
        original = str((ROOT / "build" / "dev-debug").resolve())
        self.assertEqual(original, steps["build-gnu-debug"][2])
        self.assertEqual(original, steps["affected-gnu"][2])
        shared = steps["shared-checks"]
        self.assertIn(str(directory.resolve()), shared)
        self.assertIn(original, shared)

    def test_file_only_ignore_rule_does_not_admit_unignored_sibling_artifacts(self) -> None:
        from tools.verify import VerificationError, validate_report_path

        with (self.root / ".gitignore").open("a") as output:
            output.write("/result.json\n")
        with self.assertRaisesRegex(VerificationError, "artifact"):
            validate_report_path(self.root, self.root / "result.json")
        self.assertFalse((self.root / "result.json.lock").exists())

    def test_case_alias_content_remains_part_of_the_source_identity(self) -> None:
        from tools.verify import repository_identity

        source = self.root / "tools" / "project_graph.py"
        alias = self.root / "tools" / "PROJECT_GRAPH.PY"
        source.rename(alias)
        if not source.exists():
            self.skipTest("requires a case-insensitive filesystem")
        before = repository_identity(self.root)
        alias.write_text("changed case-alias content\n")
        self.assertNotEqual(before, repository_identity(self.root))

    @unittest.skipIf(os.name == "nt", "plan execution uses POSIX process ownership")
    def test_real_sigterm_retires_descendants_and_finalizes_interrupted_report(self) -> None:
        marker, group = self.root / "build" / "writes", self.root / "build" / "group"
        marker.parent.mkdir()
        child = (
            "import time;from pathlib import Path;"
            f"f=Path({str(marker)!r}).open('ab',buffering=0)\n"
            "while True:f.write(b'x');time.sleep(0.01)\n"
        )
        parent = (
            "import os,subprocess,sys,time;from pathlib import Path;"
            f"Path({str(group)!r}).write_text(str(os.getpid()));"
            f"subprocess.Popen([sys.executable,'-c',{child!r}]);time.sleep(60)"
        )
        script = (
            "from pathlib import Path;from tools.verify import execute_plan;"
            f"execute_plan(Path({str(self.root)!r}),{self.plan(parent)!r},Path({str(self.report)!r}))"
        )
        process = subprocess.Popen(
            [sys.executable, "-c", script], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 5
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists())
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=10)
            self.assertEqual("interrupted", json.loads(self.report.read_text())["status"])
            size = marker.stat().st_size
            time.sleep(0.1)
            self.assertEqual(size, marker.stat().st_size)
        finally:
            if group.exists():
                try:
                    os.killpg(int(group.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if process.poll() is None:
                process.kill()
            process.communicate()


class EvidenceLockTest(unittest.TestCase):
    def test_symbolic_lock_path_is_rejected_without_modifying_its_target(self) -> None:
        from tools.verify import VerificationError, evidence_lock

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target"
            target.write_text("unchanged")
            report = root / "report.json"
            report.with_name("report.json.lock").symlink_to(target)
            with self.assertRaises((OSError, VerificationError)):
                with evidence_lock(report):
                    self.fail("a symbolic evidence lock must not be followed")
            self.assertEqual("unchanged", target.read_text())
            self.assertFalse(report.exists())

    def test_lock_excludes_other_processes_and_releases_after_exit(self) -> None:
        from tools.verify import evidence_lock

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            script = (
                "from pathlib import Path\n"
                "from tools.verify import evidence_lock\n"
                f"with evidence_lock(Path({str(report)!r})):\n"
                "    print('acquired')\n"
            )
            with evidence_lock(report):
                blocked = subprocess.run(
                    [sys.executable, "-c", script], cwd=ROOT,
                    capture_output=True, text=True, timeout=10, check=False,
                )
                self.assertNotEqual(0, blocked.returncode)
                self.assertIn("in use", blocked.stderr)
            acquired = subprocess.run(
                [sys.executable, "-c", script], cwd=ROOT,
                capture_output=True, text=True, timeout=10, check=False,
            )
            self.assertEqual(0, acquired.returncode, acquired.stderr)
            self.assertEqual("acquired", acquired.stdout.strip())

    def test_exception_releases_the_owned_lock(self) -> None:
        from tools.verify import evidence_lock

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            with self.assertRaisesRegex(ValueError, "injected"):
                with evidence_lock(report):
                    raise ValueError("injected")
            with evidence_lock(report):
                pass

    def test_lock_is_not_inherited_by_an_owned_child(self) -> None:
        from tools.verify import evidence_lock

        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            with evidence_lock(report):
                child = subprocess.Popen(
                    [sys.executable, "-c", "import sys; sys.stdin.buffer.read()"],
                    stdin=subprocess.PIPE, close_fds=False,
                )
            try:
                self.assertIsNone(child.poll())
                with evidence_lock(report):
                    pass
            finally:
                try:
                    child.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate()
                    raise


class SharedVerificationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.marker = self.root / "executions"
        self.script = self.root / "check.py"
        self.script.write_text(
            "from pathlib import Path\n"
            f"path=Path({str(self.marker)!r})\n"
            "path.write_text(str(int(path.read_text())+1 if path.exists() else 1))\n"
        )
        (self.root / "project").mkdir()
        (self.root / "project/verification-policy.json").write_bytes(
            (ROOT / "project/verification-policy.json").read_bytes()
        )
        (self.root / ".gitignore").write_text(
            "executions\nshared.json\nshared.json.lock\nshared-*/\nowner/\nother/\n"
        )
        initialize_repository(self.root)

    def fixture(self, name: str, extra: bool = False, timeout: int = 10) -> Path:
        directory = self.root / name
        directory.mkdir()
        interpreter = Path(sys.executable).as_posix()
        script = self.script.as_posix()
        working_directory = self.root.as_posix()
        text = (
            f'add_test(project.graph "{interpreter}" "{script}")\n'
            f'set_tests_properties(project.graph PROPERTIES LABELS configuration-independent '
            f'TIMEOUT {timeout} WORKING_DIRECTORY "{working_directory}")\n'
        )
        if extra:
            text += (
                f'add_test(differential.sqlite_oracle "{interpreter}" "{script}")\n'
                'set_tests_properties(differential.sqlite_oracle PROPERTIES '
                f'LABELS configuration-independent TIMEOUT 10 WORKING_DIRECTORY "{working_directory}")\n'
            )
        (directory / "CTestTestfile.cmake").write_text(text)
        return directory

    def run_shared(self, owner: Path, other: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), "run-shared", "--repository-root", str(self.root),
             "--owner-build-directory", str(owner),
             "--build-directory", str(other), "--report", str(self.root / "shared.json")],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )

    def test_matching_shared_instances_execute_once(self) -> None:
        result = self.run_shared(self.fixture("owner"), self.fixture("other"))
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("1", self.marker.read_text())
        report = json.loads((self.root / "shared.json").read_text())
        self.assertEqual("passed", report["status"])
        self.assertEqual(2, report["covered_instances"])
        self.assertEqual(1, report["executed_tests"])

    def test_optional_test_missing_from_owner_fails_before_execution(self) -> None:
        result = self.run_shared(self.fixture("owner"), self.fixture("other", extra=True))
        self.assertNotEqual(0, result.returncode)
        self.assertIn("owner", result.stderr.lower())
        self.assertFalse(self.marker.exists())

    def test_different_timeout_identity_cannot_be_reused(self) -> None:
        result = self.run_shared(self.fixture("owner"), self.fixture("other", timeout=20))
        self.assertNotEqual(0, result.returncode)
        self.assertIn("identity", result.stderr.lower())
        self.assertFalse(self.marker.exists())

    def test_different_interpreter_identity_cannot_be_reused(self) -> None:
        owner = self.fixture("owner")
        other = self.fixture("other")
        path = other / "CTestTestfile.cmake"
        alternative = shutil.which("cmake")
        self.assertIsNotNone(alternative)
        path.write_text(
            path.read_text().replace(
                Path(sys.executable).as_posix(), Path(alternative).as_posix()
            )
        )
        result = self.run_shared(owner, other)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("identity", result.stderr.lower())
        self.assertFalse(self.marker.exists())

    def test_shared_input_drift_cannot_produce_passed_evidence(self) -> None:
        with self.script.open("a") as stream:
            stream.write("with Path(__file__).open('a') as target: target.write('\\n# drift\\n')\n")
        result = self.run_shared(self.fixture("owner"), self.fixture("other"))
        self.assertNotEqual(0, result.returncode)
        self.assertIn("identity", result.stderr.lower())
        self.assertEqual("failed", json.loads((self.root / "shared.json").read_text())["status"])


if __name__ == "__main__":
    unittest.main()

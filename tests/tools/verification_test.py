#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/verify.py"


def initialize_repository(root: Path) -> None:
    subprocess.run(["git", "init", "--quiet", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
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
        self.assertIn("nine-exact-head-hosted-checks", plan["outstanding_requirements"])
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

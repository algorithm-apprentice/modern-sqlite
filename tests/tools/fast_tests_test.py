#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/run_fast_tests.py"
FAKE_GTEST = """
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

root = Path(__file__).parent
mode = json.loads((root / "mode.json").read_text())["mode"]
if mode in ("timeout", "descendant"):
    (root / "owned-group").write_text(str(os.getpgrp()))
launches = root / "launches"
launches.write_text(str(int(launches.read_text()) + 1 if launches.exists() else 1))
filters = next(a.split("=", 1)[1] for a in sys.argv if a.startswith("--gtest_filter="))
names = filters.split(":")
output = next((a.split("xml:", 1)[1] for a in sys.argv if a.startswith("--gtest_output=")),
              str(root / "isolated-gtest.xml"))
suite = ET.Element("testsuites", tests=str(len(names)), failures="0", errors="0",
                   skipped="0", disabled="0")
tests = ET.SubElement(suite, "testsuite", name="Fake")
for name in names:
    if mode == "descendant":
        subprocess.Popen(
            [sys.executable, "-c",
             "import time; from pathlib import Path; time.sleep(1); "
             + "Path(" + repr(str(root / "descendant-escaped")) + ").write_text('alive')"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    if mode != "missing-event":
        print("[ RUN      ] " + name, flush=True)
    if mode == "duplicate-event":
        print("[ RUN      ] " + name, flush=True)
    if mode == "timeout":
        time.sleep(1)
        (root / "timeout-escaped").write_text("not killed")
    if mode == "capture-timeout":
        time.sleep(0.25)
        (root / "timeout-escaped").write_text("not killed")
    if mode == "hold" and name == names[0]:
        time.sleep(0.3)
    if mode == "unknown-event":
        print("[ RUN      ] Unknown.Case", flush=True)
    if mode == "output-limit":
        print("x" * (17 * 1024 * 1024), flush=True)
    group, case = name.rsplit(".", 1)
    item = ET.SubElement(tests, "testcase", name=case, classname=group, status="run")
    if mode == "binary-drift" and name == names[0]:
        with Path(__file__).open("a") as stream:
            stream.write("\\n# changed during execution\\n")
    if mode == "partial-failure" and name == names[-1]:
        print("[  FAILED  ] " + name + " (0 ms)", flush=True)
        break
    if mode == "skip":
        ET.SubElement(item, "skipped")
        suite.set("skipped", "1")
        print("[  SKIPPED ] " + name + " (0 ms)", flush=True)
    elif mode != "missing-end":
        elapsed = 200 if mode == "reported-timeout" else 0
        print("[       OK ] " + name + " (" + str(elapsed) + " ms)", flush=True)
if mode == "extra-xml":
    ET.SubElement(tests, "testcase", name="Extra", classname="Unknown", status="run")
    suite.set("tests", str(len(names) + 1))
if mode == "missing-xml":
    tests.remove(tests[0])
    suite.set("tests", str(len(names) - 1))
if mode == "missing-attribute":
    tests[0].attrib.pop("name")
Path(output).parent.mkdir(parents=True, exist_ok=True)
ET.ElementTree(suite).write(output)
sys.exit(7 if mode == "bad-exit" else 0)
"""


def cmake_quote(value: str) -> str:
    return "[=[" + value + "]=]"


class FastLocalTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.binary = self.root / "modern_sqlite_unit_tests"
        self.binary.write_text(
            "#!" + sys.executable + "\n" + textwrap.dedent(FAKE_GTEST),
            encoding="utf-8",
        )
        self.binary.chmod(0o755)
        self.set_mode("pass")
        self.report = self.root / "report.json"
        self.configure_cases()

    def set_mode(self, mode: str) -> None:
        (self.root / "mode.json").write_text(json.dumps({"mode": mode}))

    def configure_cases(self, timeout: float = 10, standalone: bool = True,
                        extra_properties: str = "") -> None:
        lines = []
        for name in ("Fake.One", "Fake.Two"):
            lines.extend(
                [
                    "add_test(" + cmake_quote(name) + " "
                    + cmake_quote(str(self.binary)) + " "
                    + cmake_quote("--gtest_filter=" + name) + ")",
                    "set_tests_properties(" + cmake_quote(name)
                    + " PROPERTIES LABELS unit TIMEOUT " + str(timeout)
                    + " WORKING_DIRECTORY " + cmake_quote(str(self.root))
                    + " " + extra_properties + ")",
                ]
            )
        if standalone:
            other = self.root / "isolated.py"
            other.write_text(
                "from pathlib import Path\nimport sys\n"
                "Path(__file__).with_name('isolated-ran').write_text('yes')\n"
                "sys.exit(3)\n"
            )
            lines.extend(
                [
                    "add_test(isolated.oom " + cmake_quote(sys.executable) + " "
                    + cmake_quote(str(other)) + ")",
                    "set_tests_properties(isolated.oom PROPERTIES LABELS unit "
                    "TIMEOUT 10 WILL_FAIL TRUE)",
                ]
            )
        (self.root / "CTestTestfile.cmake").write_text("\n".join(lines) + "\n")

    def run_tool(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--build-directory",
                str(self.root),
                "--report",
                str(self.report),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )

    def assert_owned_group_has_no_live_members(self) -> None:
        from tools import run_fast_tests

        group = int((self.root / "owned-group").read_text())
        live = {
            pid: state
            for pid, state in run_fast_tests.owned_group_states(group).items()
            if not state.startswith("Z")
        }
        self.assertEqual({}, live)

    def test_accounts_complete_unit_union_and_preserves_standalone_exit_policy(self) -> None:
        result = self.run_tool()
        self.assertEqual(0, result.returncode, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual("passed", report["status"])
        self.assertEqual("fast-local", report["process_mode"])
        self.assertFalse(report["isolated_qualification"])
        self.assertEqual(["Fake.One", "Fake.Two", "isolated.oom"], report["test_names"])
        self.assertEqual(2, report["batched_tests"])
        self.assertEqual(1, report["isolated_tests"])
        self.assertTrue((self.root / "isolated-ran").exists())
        self.assertIn(str(Path(sys.executable).resolve()), report["binary_sha256"])
        self.assertIn("normalized_discovery", report)
        self.assertIn("environment_sha256", report)

    def test_enforces_discovered_case_timeout(self) -> None:
        self.set_mode("timeout")
        self.configure_cases(timeout=0.1, standalone=False)
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("timed out", result.stderr.lower())
        self.assert_owned_group_has_no_live_members()
        self.assertFalse((self.root / "timeout-escaped").exists())

    def test_rejects_duplicate_start(self) -> None:
        self.set_mode("duplicate-event")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("event", result.stderr.lower())

    def test_rejects_unknown_start(self) -> None:
        self.set_mode("unknown-event")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("event", result.stderr.lower())

    def test_rejects_xml_without_start_protocol(self) -> None:
        self.set_mode("missing-event")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("event", result.stderr.lower())

    def test_rejects_extra_or_missing_xml_cases(self) -> None:
        for mode in ("extra-xml", "missing-xml"):
            with self.subTest(mode=mode):
                self.set_mode(mode)
                result = self.run_tool()
                self.assertNotEqual(0, result.returncode)
                self.assertIn("xml", result.stderr.lower())

    def test_rejects_skipped_cases(self) -> None:
        self.set_mode("skip")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("skip", result.stderr.lower())

    def test_rejects_nonzero_process_exit_even_with_success_xml(self) -> None:
        self.set_mode("bad-exit")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("exit", result.stderr.lower())
        self.assertEqual("1", (self.root / "launches").read_text())
        self.assertFalse((self.root / "isolated-ran").exists())

    def test_rejects_empty_selection(self) -> None:
        (self.root / "CTestTestfile.cmake").write_text("")
        self.report.write_text(json.dumps({"status": "passed"}))
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("empty", result.stderr.lower())
        self.assertEqual("failed", json.loads(self.report.read_text())["status"])

    def test_rejects_missing_end_event(self) -> None:
        self.set_mode("missing-end")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("event", result.stderr.lower())

    def test_checks_reported_body_deadline_even_if_output_is_buffered(self) -> None:
        self.set_mode("reported-timeout")
        self.configure_cases(timeout=0.1, standalone=False)
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("timed out", result.stderr.lower())

    def test_bounds_output_capture(self) -> None:
        self.set_mode("output-limit")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("capture limit", result.stderr.lower())

    def test_cleans_descendants_after_successful_leader_exit(self) -> None:
        self.set_mode("descendant")
        self.configure_cases(standalone=False)
        result = self.run_tool()
        self.assertEqual(0, result.returncode, result.stderr)
        self.assert_owned_group_has_no_live_members()
        self.assertFalse((self.root / "descendant-escaped").exists())

    def test_preserves_unrecognized_case_properties_through_ctest(self) -> None:
        self.configure_cases(standalone=False, extra_properties="RESOURCE_LOCK owned-fixture")
        result = self.run_tool()
        self.assertEqual(0, result.returncode, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(0, report["batched_tests"])
        self.assertEqual(2, report["isolated_tests"])

    def test_cleans_owned_child_when_capture_setup_fails(self) -> None:
        from tools import run_fast_tests

        self.set_mode("capture-timeout")
        case = {
            "name": "Fake.One", "filter": "Fake.One", "binary": str(self.binary),
            "common_arguments": [], "directory": str(self.root), "timeout": 10,
        }
        policy = run_fast_tests.load_policy()["fast_local"]
        with mock.patch.object(
            run_fast_tests.selectors, "DefaultSelector", side_effect=OSError("capture setup")
        ):
            with self.assertRaises(OSError):
                run_fast_tests.run_batch([case], self.root, 0, policy)
        time.sleep(0.3)
        self.assertFalse((self.root / "timeout-escaped").exists())

    def test_partitions_large_selections_without_dropping_cases(self) -> None:
        from tools import run_fast_tests

        tests = [
            {
                "name": f"Fake.Case{index}",
                "command": [str(self.binary), f"--gtest_filter=Fake.Case{index}"],
                "property_map": {"TIMEOUT": 10, "WORKING_DIRECTORY": str(self.root)},
            }
            for index in range(257)
        ]
        batches, isolated = run_fast_tests.partition(
            tests, run_fast_tests.load_policy()["fast_local"]
        )
        self.assertEqual([], isolated)
        self.assertEqual([128, 128, 1], [len(batch) for batch in batches])
        self.assertEqual(
            {test["name"] for test in tests},
            {case["name"] for batch in batches for case in batch},
        )

    def test_permission_error_does_not_hide_a_live_owned_group(self) -> None:
        from tools import run_fast_tests

        process = mock.Mock(pid=12345)
        with mock.patch.object(run_fast_tests.os, "killpg", side_effect=PermissionError):
            with mock.patch.object(
                run_fast_tests, "owned_group_states", return_value={12345: "S"}
            ):
                with self.assertRaises(run_fast_tests.VerificationError):
                    run_fast_tests.kill_owned_group(process)
        process.wait.assert_not_called()

    def test_exited_zombie_group_is_reaped_only_after_inspection(self) -> None:
        from tools import run_fast_tests

        process = mock.Mock(pid=12345)
        with mock.patch.object(run_fast_tests.os, "killpg", side_effect=PermissionError):
            with mock.patch.object(
                run_fast_tests, "owned_group_states", return_value={12345: "Z"}
            ) as inspect:
                run_fast_tests.kill_owned_group(process)
        inspect.assert_called_once_with(12345)
        process.wait.assert_called_once_with()

    def test_binary_drift_invalidates_an_otherwise_successful_run(self) -> None:
        self.set_mode("binary-drift")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("identity", result.stderr.lower())
        self.assertEqual("failed", json.loads(self.report.read_text())["status"])

    def test_malformed_xml_finalizes_failed_report(self) -> None:
        self.set_mode("missing-attribute")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("xml", result.stderr.lower())
        self.assertEqual("failed", json.loads(self.report.read_text())["status"])

    def test_partial_batch_completions_are_not_lost(self) -> None:
        self.set_mode("partial-failure")
        result = self.run_tool()
        self.assertNotEqual(0, result.returncode)
        report = json.loads(self.report.read_text())
        self.assertEqual(["Fake.One"], report["completed_test_names"])
        self.assertEqual("failed", report["case_outcomes"]["Fake.Two"])

    def test_concurrent_report_writer_is_rejected_without_overwriting_owner(self) -> None:
        self.set_mode("hold")
        command = [
            sys.executable, str(TOOL), "--build-directory", str(self.root),
            "--report", str(self.report),
        ]
        owner = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / "launches").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            result = self.run_tool()
            self.assertNotEqual(0, result.returncode)
            self.assertIn("in use", result.stderr.lower())
            stdout, stderr = owner.communicate(timeout=15)
            self.assertEqual(0, owner.returncode, stderr.decode())
            report = json.loads(self.report.read_text())
            self.assertEqual("passed", report["status"])
            self.assertTrue(report["run_id"])
            self.assertTrue(stdout)
        finally:
            if owner.poll() is None:
                owner.terminate()
                owner.wait()

    def test_invalid_discovery_shape_finalizes_failed_report(self) -> None:
        ctest = self.root / "ctest"
        ctest.write_text(
            "#!" + sys.executable + "\n"
            "print('{\"tests\": [null]}')\n"
        )
        ctest.chmod(0o755)
        environment = dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ.get("PATH", ""))
        result = subprocess.run(
            [sys.executable, str(TOOL), "--build-directory", str(self.root),
             "--report", str(self.report)],
            cwd=ROOT, env=environment, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertEqual("failed", json.loads(self.report.read_text())["status"])


if __name__ == "__main__":
    unittest.main()

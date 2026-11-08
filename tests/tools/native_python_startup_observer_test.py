#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

from tests.tools.ci_workflow_policy_test import (
    REPOSITORY_ROOT,
    STANDARD_CI_WORKFLOW,
    event_block,
)


@unittest.skipUnless(sys.platform == "win32", "Native Windows process metadata requires Windows")
class NativePythonStartupObserverTest(unittest.TestCase):
    def run_fixture(
        self, source: str, *, sampler_source: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        windows = event_block(STANDARD_CI_WORKFLOW.read_text(encoding="utf-8"), "windows-native")
        step = windows.split(
            "- name: Run the complete declared native correctness tier", 1
        )[1].split("- name:", 1)[0]
        body = step.split("run:", 1)[1].strip()
        if body.startswith("|"):
            body = textwrap.dedent(step.split("run: |", 1)[1])
        with tempfile.TemporaryDirectory(prefix="modern-sqlite-native-startup-") as temporary:
            root = pathlib.Path(temporary)
            fixture = root / "startup.py"
            fixture.write_text(
                "import _thread, json, os, sys, time\n"
                "def report(phase, pid=None):\n"
                "    print('read-compatibility-progress ' + json.dumps({\n"
                "        'test': None, 'phase': phase, 'pid': os.getpid() if pid is None else pid,\n"
                "        'unix_ns': time.time_ns(),\n"
                "        'native_thread_id': _thread.get_native_id(),\n"
                "    }, separators=(',', ':')), flush=True)\n"
                + source,
                encoding="utf-8",
            )
            (root / "CTestTestfile.cmake").write_text(
                f"add_test(native.startup_observer [=[{sys.executable}]=] "
                f"-u [=[{fixture}]=])\n"
                "set_tests_properties(native.startup_observer PROPERTIES TIMEOUT 10)\n",
                encoding="utf-8",
            )
            (root / "CMakeCache.txt").write_text(
                f"Python3_EXECUTABLE:FILEPATH={sys.executable}\n", encoding="utf-8",
            )
            root_argument = str(root).replace("'", "''")
            command = f"ctest --test-dir '{root_argument}'"
            body = body.replace(
                "'build\\windows-msvc-debug\\CMakeCache.txt'",
                f"'{root_argument}\\CMakeCache.txt'",
            )
            if sampler_source is not None:
                sampler = root / "sampler.ps1"
                sampler.write_text(
                    "param($TargetProcessId, $SupervisorProcessId, $ConfiguredInterpreter)\n" + sampler_source,
                    encoding="utf-8",
                )
                sampler_argument = str(sampler).replace("'", "''")
                constructor = next(
                    line for line in body.splitlines()
                    if line.strip().startswith("$observation = New-NativeStartupObservation")
                )
                body = body.replace(constructor, constructor + f"\n$observation.Script = '{sampler_argument}'")
            script = root / "observe.ps1"
            script.write_text(
                "$ErrorActionPreference = 'Stop'\n"
                "$env:NATIVE_STARTUP_FIXTURE_SUPERVISOR = [string]$PID\n"
                + body.replace("ctest --preset windows-debug", command),
                encoding="utf-8",
            )
            return subprocess.run(
                ["pwsh", "-NoLogo", "-NoProfile", "-NonInteractive", "-File", str(script)],
                cwd=REPOSITORY_ROOT, text=True, capture_output=True, check=False,
                timeout=20,
            )

    def observations(self, result: subprocess.CompletedProcess[str]) -> list[dict]:
        prefix = "native-python-startup-observation "
        return [
            json.loads(line.removeprefix(prefix))
            for line in result.stdout.splitlines() if line.startswith(prefix)
        ]

    def test_samples(self) -> None:
        result = self.run_fixture(
            "report('bootstrap-ready')\n"
            "time.sleep(2.5)\n"
            "until = time.perf_counter() + 0.5\n"
            "while time.perf_counter() < until:\n"
            "    pass\n"
            "report('module-ready')\n"
            "report('module-ready')\n"
        )
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        records = self.observations(result)
        self.assertTrue(records, "The real CTest owner produced no startup observations")
        samples = [record for record in records if record["phase"] == "sample"]
        self.assertTrue(samples, result.stdout)
        identities = [record for record in records if record["phase"] == "target"]
        self.assertEqual(1, len(identities))
        identity = identities[0]
        progress = [
            json.loads(line.split("read-compatibility-progress ", 1)[1])
            for line in result.stdout.splitlines() if ": read-compatibility-progress " in line
        ]
        bootstrap, ready = progress[:2]
        self.assertEqual(bootstrap["pid"], identity["owner_pid"])
        self.assertTrue(any(
            bootstrap["native_thread_id"] in {thread["id"] for thread in sample["threads"]}
            and bootstrap["unix_ns"] <= sample["unix_ms"] * 1_000_000 < ready["unix_ns"]
            for sample in samples
        ))
        self.assertEqual(pathlib.Path(sys.executable), pathlib.Path(identity["configured_interpreter"]))
        self.assertEqual(identity["redirected"], identity["owner_pid"] != identity["configured_pid"])
        self.assertLessEqual(identity["configured_created_utc_ticks"], identity["owner_created_utc_ticks"])
        self.assertNotEqual(identity["owner_pid"], identity["ctest_pid"])
        self.assertNotEqual(identity["ctest_pid"], identity["supervisor_pid"])
        self.assertGreater(identity["owner_created_utc_ticks"], 0)
        for sample in samples:
            self.assertEqual(identity["owner_pid"], sample["owner_pid"])
            self.assertGreaterEqual(sample["elapsed_ticks"], 0)
            self.assertGreaterEqual(sample["observation_ticks"], 0)
            self.assertGreaterEqual(sample["process_cpu_ticks"], 0)
            self.assertTrue(sample["threads"])
            for thread in sample["threads"]:
                self.assertGreater(thread["id"], 0)
                if thread["cpu_ticks"] is None:
                    self.assertEqual("thread-exited-during-sample", thread["error"])
                else:
                    self.assertGreaterEqual(thread["cpu_ticks"], 0)
                if thread["state"] != "Wait":
                    self.assertIsNone(thread["wait_reason"])
        self.assertEqual("observation-complete", records[-1]["phase"])
        self.assertTrue(records[-1]["readiness_observed"])
        self.assertTrue(records[-1]["sampler_exited"])
        self.assertFalse(records[-1]["forced_termination"])

    def test_protocol(self) -> None:
        records = [
            "not-json",
            "null",
            '{"phase":"bootstrap-ready","pid":7}',
            '{"test":null,"pid":7}',
            '{"test":null,"phase":"bootstrap-ready","pid":0}',
            '{"test":null,"phase":"module-ready","pid":7}',
        ]
        script = (
            "$ErrorActionPreference = 'Stop'\n"
            ". .\\tools\\native_python_startup_observer.ps1\n"
            "$inputs = @'\n" + json.dumps(records) + "\n'@ | ConvertFrom-Json\n"
            "$results = foreach ($record in $inputs) {\n"
            "  $observation = New-NativeStartupObservation -InterpreterPath "
            + "'" + sys.executable.replace("'", "''") + "'\n"
            "  Receive-NativeStartupProgress $observation ('17: read-compatibility-progress ' + $record)\n"
            "  @{ error = $observation.Error; started = $null -ne $observation.Sampler }\n"
            "}\n"
            "$path = Join-Path ([IO.Path]::GetTempPath()) ('modern-sqlite-startup-stream-' + [guid]::NewGuid())\n"
            "$source = $null; $reader = $null; $capture = $null\n"
            "try {\n"
            "  [IO.File]::WriteAllBytes($path, [byte[]]::new(3145728))\n"
            "  $source = [IO.File]::OpenRead($path)\n"
            "  $reader = [IO.StreamReader]::new($source, [Text.UTF8Encoding]::new($false, $true), $true, 1024, $true)\n"
            "  $capture = Start-NativeStartupCapture $reader\n"
            "  try { [void]$capture.Task.Wait(3000) } catch [AggregateException] { }\n"
            "  $overflow = @{ faulted = $capture.Task.IsFaulted; position = $source.Position; length = $source.Length }\n"
            "} finally {\n"
            "  if ($null -ne $capture) { $capture.Storage.Dispose() }\n"
            "  if ($null -ne $reader) { $reader.Dispose() }\n"
            "  if ($null -ne $source) { $source.Dispose() }\n"
            "  [IO.File]::Delete($path)\n"
            "}\n"
            "@{ progress = $results; oversized = $overflow } | ConvertTo-Json -Depth 4 -Compress\n"
        )
        result = subprocess.run(
            ["pwsh", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script],
            cwd=REPOSITORY_ROOT, text=True, capture_output=True, check=False, timeout=10,
        )
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        result_data = json.loads(result.stdout)
        observations = result_data["progress"]
        self.assertEqual(len(records), len(observations))
        for record, observation in zip(records, observations):
            with self.subTest(record=record):
                self.assertTrue(observation["error"])
                self.assertFalse(observation["started"])
        overflow = result_data["oversized"]
        self.assertTrue(overflow["faulted"], "Oversized output did not fail while being collected")
        self.assertLess(overflow["position"], overflow["length"], "The collector consumed all oversized output")

    def assert_failed_fixture(
        self, name: str, source: str, sampler_source: str | None, expected_exit: int,
    ) -> None:
        result = self.run_fixture(source, sampler_source=sampler_source)
        self.assertEqual(expected_exit, result.returncode, result.stdout + result.stderr)
        records = self.observations(result)
        complete = records[-1]
        self.assertEqual("observation-complete", complete["phase"])
        self.assertTrue(complete["sampler_exited"], result.stdout + result.stderr)
        if name != "overflowing-sampler":
            self.assertEqual(name == "unresponsive-sampler", complete["forced_termination"])
        self.assertFalse(any(record["phase"] == "sample" for record in records))
        if name != "failed-owner":
            self.assertTrue(complete["error"])
        if name == "overflowing-sampler":
            self.assertIn("output bound", complete["error"])

    def test_exit(self) -> None:
        for name, sampler_source in (
            ("failed-owner", None),
            ("failed-owner-and-sampler", "exit 19\n"),
            ("overflowing-sampler", "[Console]::Out.Write('x' * 3145728)\n"),
        ):
            with self.subTest(case=name):
                self.assert_failed_fixture(
                    name, "report('bootstrap-ready')\nreport('module-ready')\nsys.exit(7)\n",
                    sampler_source, 8,
                )

    def test_ownership(self) -> None:
        for name, source, sampler_source in (
            (
                "foreign-process",
                "report('bootstrap-ready', int(os.environ['NATIVE_STARTUP_FIXTURE_SUPERVISOR']))\n"
                "time.sleep(0.5)\n",
                None,
            ),
            (
                "unresponsive-sampler",
                "report('bootstrap-ready')\nreport('module-ready')\n",
                "Start-Sleep -Seconds 30\n",
            ),
            (
                "missing-sampler-evidence",
                "report('bootstrap-ready')\nreport('module-ready')\n",
                "exit 0\n",
            ),
        ):
            with self.subTest(case=name):
                self.assert_failed_fixture(name, source, sampler_source, 1)


if __name__ == "__main__":
    unittest.main()

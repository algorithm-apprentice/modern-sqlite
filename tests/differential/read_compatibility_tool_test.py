#!/usr/bin/env python3

import time

_MODULE_ENTRY_UNIX_NS = time.time_ns()
_MODULE_IMPORT_STARTED = (time.perf_counter_ns(), time.process_time_ns())

import json
import os
import sys


def _write_owner_progress(
    test: str | None,
    phase: str,
    started: tuple[int, int],
    *,
    return_code: int | None = None,
    extension: str | None = None,
    extension_origin: str | None = None,
) -> None:
    record = {
        "test": test,
        "phase": phase,
        "pid": os.getpid(),
        "unix_ns": time.time_ns(),
        "wall_ns": time.perf_counter_ns() - started[0],
        "owner_cpu_ns": time.process_time_ns() - started[1],
    }
    if return_code is not None:
        record["return_code"] = return_code
    if extension is not None:
        record["extension"] = extension
        record["extension_origin"] = extension_origin
    if test is None:
        record["module_entry_unix_ns"] = _MODULE_ENTRY_UNIX_NS
    print(
        "read-compatibility-progress " + json.dumps(record, separators=(",", ":")),
        file=sys.stderr,
        flush=True,
    )


class _ExtensionImportProgress:
    def __init__(self) -> None:
        self._installed = False

    def __enter__(self) -> "_ExtensionImportProgress":
        if os.name == "nt":
            if sys.getprofile() is None:
                sys.setprofile(self._observe)
                self._installed = True
            else:
                _write_owner_progress(
                    None, "extension-profile-busy", _MODULE_IMPORT_STARTED
                )
        return self

    def __exit__(self, _type, _error, _traceback) -> None:
        if self._installed:
            sys.setprofile(None)
            self._installed = False

    def _observe(self, frame, event: str, function) -> None:
        if event not in {"c_call", "c_return", "c_exception"}:
            return
        if getattr(function, "__module__", None) != "_imp":
            return
        operation = getattr(function, "__name__", None)
        if operation not in {"create_dynamic", "exec_dynamic"}:
            return
        context = frame.f_locals
        if frame.f_code.co_name == "_call_with_frames_removed":
            forwarded = context.get("args", ())
            if not forwarded:
                return
            target = forwarded[0]
        else:
            target = context.get(
                "spec" if operation == "create_dynamic" else "module"
            )
        if operation == "create_dynamic":
            name = getattr(target, "name", None)
            origin = getattr(target, "origin", None)
        else:
            name = getattr(target, "__name__", None)
            origin = getattr(target, "__file__", None)
        if name not in {"_hashlib", "_ssl"}:
            return
        phase = "extension-create" if operation == "create_dynamic" else "extension-exec"
        suffix = "begin" if event == "c_call" else event.removeprefix("c_")
        _write_owner_progress(
            None, phase + "-" + suffix, _MODULE_IMPORT_STARTED,
            extension=name, extension_origin=origin,
        )


_write_owner_progress(None, "bootstrap-ready", _MODULE_IMPORT_STARTED)

with _ExtensionImportProgress():
    import faulthandler
    _write_owner_progress(None, "stdlib-faulthandler-ready", _MODULE_IMPORT_STARTED)
    import hashlib
    _write_owner_progress(None, "stdlib-hashlib-ready", _MODULE_IMPORT_STARTED)
    import io
    _write_owner_progress(None, "stdlib-io-ready", _MODULE_IMPORT_STARTED)
    import pathlib
    _write_owner_progress(None, "stdlib-pathlib-ready", _MODULE_IMPORT_STARTED)
    import subprocess
    _write_owner_progress(None, "stdlib-subprocess-ready", _MODULE_IMPORT_STARTED)
    import tempfile
    _write_owner_progress(None, "stdlib-tempfile-ready", _MODULE_IMPORT_STARTED)
    import unittest
    _write_owner_progress(None, "stdlib-unittest-ready", _MODULE_IMPORT_STARTED)
    from collections.abc import Callable
    from unittest import mock

_write_owner_progress(None, "stdlib-ready", _MODULE_IMPORT_STARTED)

from tools import read_compatibility

_write_owner_progress(None, "harness-ready", _MODULE_IMPORT_STARTED)


def setUpModule() -> None:
    _write_owner_progress(
        None, "module-ready", (time.perf_counter_ns(), time.process_time_ns())
    )
    faulthandler.dump_traceback_later(8, repeat=False)


def tearDownModule() -> None:
    faulthandler.cancel_dump_traceback_later()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


class ReadCompatibilityValidationTest(unittest.TestCase):
    def run(self, result: unittest.TestResult | None = None) -> unittest.TestResult:
        self._progress_started = (time.perf_counter_ns(), time.process_time_ns())
        self._report_progress("test-enter")
        try:
            return super().run(result)
        finally:
            self._report_progress("test-finished")

    def _report_progress(self, phase: str, *, return_code: int | None = None) -> None:
        _write_owner_progress(
            self.id(), phase, self._progress_started, return_code=return_code
        )

    def setUp(self) -> None:
        self._report_progress("fixture-start")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

        generator = self.root / "tools/read_compatibility.py"
        generator.parent.mkdir(parents=True)
        generator.write_text("# synthetic generator\n", encoding="utf-8")

        fixture_sql = self.root / "tests/fixtures/read_compatibility/schema.sql"
        fixture_sql.parent.mkdir(parents=True)
        fixture_sql.write_text("CREATE TABLE items(id INTEGER PRIMARY KEY);\n")

        database = self.root / "tests/fixtures/read_compatibility/core.db"
        database.write_bytes(b"SQLite synthetic fixture")

        self.profile_path = (
            self.root / "tests/compatibility/sqlite-oracle-profile-v1.json"
        )
        write_json(
            self.profile_path,
            {
                "schema_version": 1,
                "sqlite": {
                    "version": "3.54.0",
                    "source_id": (
                        "2026-10-02 20:18:07 "
                        "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                    ),
                    "fossil_check_in": (
                        "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
                    ),
                    "git_mirror_commit": "cb547ab3e931c7766e24834af5ef6c4578863e3d",
                    "sqlite3_c_sha256": "1" * 64,
                    "sqlite3_h_sha256": "2" * 64,
                },
                "build": {
                    "defines": [
                        "NDEBUG",
                        "SQLITE_DQS=3",
                        "SQLITE_THREADSAFE=0",
                    ],
                    "required_compile_options": ["DQS=3", "THREADSAFE=0"],
                    "forbidden_compile_options": [
                        "OMIT_DECLTYPE",
                        "OMIT_FLOATING_POINT",
                    ],
                    "semantic_compile_options": [
                        "DQS=3",
                        "THREADSAFE=0",
                    ],
                    "commands": {
                        "linux": "cc -shared sqlite3.c -o libsqlite3.so",
                        "macos": "cc -dynamiclib sqlite3.c -o libsqlite3.dylib",
                    },
                },
                "connection": {
                    "db_config": {
                        "DQS_DML": 1,
                        "DQS_DDL": 1,
                        "REVERSE_SCANORDER": 0,
                        "ENABLE_COMMENTS": 1,
                        "FP_DIGITS": 17,
                    },
                    "extended_result_codes": False,
                    "limits": {
                        "LENGTH": 1048576,
                        "SQL_LENGTH": 65536,
                        "COLUMN": 256,
                        "EXPR_DEPTH": 1000,
                        "VDBE_OP": 25000,
                        "FUNCTION_ARG": 1000,
                        "VARIABLE_NUMBER": 256,
                    },
                },
                "generator": {
                    "path": "tools/read_compatibility.py",
                    "sha256": sha256(generator),
                },
                "fixture_sources": [
                    {
                        "path": "tests/fixtures/read_compatibility/schema.sql",
                        "sha256": sha256(fixture_sql),
                    }
                ],
            },
        )

        self.corpus_path = self.root / "tests/compatibility/read-corpus-v1.json"
        write_json(
            self.corpus_path,
            {
                "schema_version": 1,
                "oracle_profile": (
                    "tests/compatibility/sqlite-oracle-profile-v1.json"
                ),
                "limits": {
                    "corpus_bytes": 4194304,
                    "oracle_bytes": 67108864,
                    "cases": 512,
                    "sql_bytes": 65536,
                    "operations": 1024,
                    "binding_operations": 256,
                    "result_columns": 256,
                    "result_rows": 4096,
                    "value_bytes": 1048576,
                    "output_bytes": 16777216,
                    "stderr_bytes": 1048576,
                    "child_seconds": 10,
                },
                "databases": [
                    {
                        "id": "core",
                        "path": "tests/fixtures/read_compatibility/core.db",
                        "sha256": sha256(database),
                    }
                ],
                "supported_matrix": ["constant-select", "typed-bindings"],
                "cases": [
                    {
                        "id": "constant-one",
                        "database": "core",
                        "feature": "constant-select",
                        "expectation": "compare",
                        "sql_hex": b"SELECT 1".hex(),
                        "operations": [
                            {"op": "step"},
                            {"op": "step"},
                            {"op": "finalize"},
                        ],
                    },
                    {
                        "id": "order-by-boundary",
                        "database": "core",
                        "feature": "order-by",
                        "expectation": "unsupported",
                        "sql_hex": b"SELECT id FROM items ORDER BY id".hex(),
                        "operations": [{"op": "finalize"}],
                        "modern_rejection": {
                            "phase": "prepare",
                            "primary_code": "generic",
                        },
                    },
                ],
            },
        )

        self.constant_outcome = {
            "format_version": 1,
            "kind": "statement",
            "preparation": {
                "next_offset": 8,
                "parameter_count": 0,
                "parameter_names": [],
                "columns": [{"name_hex": "31", "declared_type_hex": None}],
            },
            "observations": [
                {
                    "operation_index": 0,
                    "op": "step",
                    "result": "row",
                    "return_code": 100,
                    "columns": [{"name_hex": "31", "declared_type_hex": None}],
                    "row": [{"type": "integer", "value": "1"}],
                },
                {
                    "operation_index": 1,
                    "op": "step",
                    "result": "done",
                    "return_code": 101,
                },
                {
                    "operation_index": 2,
                    "op": "finalize",
                    "status": {
                        "primary_code": "ok",
                        "return_code": 0,
                        "extended_code": 0,
                    },
                },
            ],
        }
        self.unsupported_outcome = {
            "format_version": 1,
            "kind": "prepare_error",
            "status": {
                "primary_code": "generic",
                "return_code": 1,
                "extended_code": 1,
                "message_hex": b"unsupported syntax".hex(),
            },
        }
        self.runner = self.root / "fake_runner.py"
        self._write_runner(
            self.runner,
            {
                b"SELECT 1".hex(): self.constant_outcome,
                b"SELECT id FROM items ORDER BY id".hex(): self.unsupported_outcome,
            },
        )
        self.oracle_path = self.root / "tests/compatibility/read-oracle-v1.json"
        self._write_oracle(self.constant_outcome)
        self._report_progress("fixture-ready")

    def tearDown(self) -> None:
        self._report_progress("body-finished")
        super().tearDown()

    def doCleanups(self) -> bool:
        self._report_progress("cleanup-start")
        try:
            return super().doCleanups()
        finally:
            self._report_progress("cleanup-finished")

    def _write_runner(
        self, path: pathlib.Path, outcomes: dict[str, dict[str, object]], exit_code: int = 0
    ) -> None:
        path.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import sys\n"
            f"outcomes = {outcomes!r}\n"
            "protocol = sys.stdin.read()\n"
            "sql_line = next(line for line in protocol.splitlines() if line.startswith('SQL '))\n"
            "print(json.dumps(outcomes[sql_line[4:]], separators=(',', ':')))\n"
            f"raise SystemExit({exit_code})\n",
            encoding="utf-8",
        )
        path.chmod(0o755)
        if os.name == "nt":
            stderr = path.with_suffix(".stderr")
            stderr.write_text("", encoding="utf-8")
            dispatch: list[str] = []
            responses: list[str] = []
            for index, (sql_hex, outcome) in enumerate(outcomes.items()):
                payload = path.with_suffix(f".outcome-{index}.json")
                payload.write_text(
                    json.dumps(outcome, separators=(",", ":")) + "\n",
                    encoding="utf-8",
                )
                dispatch.append(
                    f'if "%fixture_sql%"=="SQL {sql_hex}" goto response_{index}'
                )
                responses.extend(
                    [
                        f":response_{index}",
                        f'type "%~dp0{payload.name}"',
                        "if errorlevel 1 exit /b 1",
                        f'type "%~dp0{stderr.name}" 1>&2',
                        "if errorlevel 1 exit /b 1",
                        f"exit /b {exit_code}",
                    ]
                )
            path.with_suffix(".cmd").write_text(
                "\n".join(
                    [
                        "@echo off",
                        "setlocal EnableDelayedExpansion",
                        'set "fixture_sql="',
                        'set /p "fixture_header="',
                        'set /p "fixture_sql="',
                        'if not defined fixture_sql for /f "tokens=1,*" %%a in ("!fixture_header!") '
                        'do if "%%a"=="SQL" if not defined fixture_sql set "fixture_sql=SQL %%b"',
                        "setlocal DisableDelayedExpansion",
                        *dispatch,
                        "echo unknown synthetic fixture SQL 1>&2",
                        "exit /b 1",
                        *responses,
                    ]
                ) + "\n",
                encoding="utf-8",
            )

    def _write_oracle(self, constant_outcome: dict[str, object]) -> None:
        profile = json.loads(self.profile_path.read_text())
        corpus = json.loads(self.corpus_path.read_text())
        write_json(
            self.oracle_path,
            {
                "schema_version": 1,
                "oracle_profile_path": (
                    "tests/compatibility/sqlite-oracle-profile-v1.json"
                ),
                "oracle_profile_schema_version": 1,
                "oracle_profile_sha256": sha256(self.profile_path),
                "corpus_path": "tests/compatibility/read-corpus-v1.json",
                "corpus_sha256": sha256(self.corpus_path),
                "generator": profile["generator"],
                "generator_format_version": 1,
                "sqlite": {
                    "version": profile["sqlite"]["version"],
                    "source_id": profile["sqlite"]["source_id"],
                    "sqlite3_c_sha256": profile["sqlite"]["sqlite3_c_sha256"],
                    "sqlite3_h_sha256": profile["sqlite"]["sqlite3_h_sha256"],
                    "compile_options": profile["build"][
                        "semantic_compile_options"
                    ],
                },
                "databases": corpus["databases"],
                "fixture_sources": profile["fixture_sources"],
                "cases": [
                    {
                        "id": "constant-one",
                        "expectation": "compare",
                        "outcome": constant_outcome,
                    },
                    {
                        "id": "order-by-boundary",
                        "expectation": "unsupported",
                        "boundary": {
                            "prepare": "statement",
                            "first_step": "row",
                        },
                    },
                ],
            },
        )

    def _verify(
        self,
        *,
        runner: pathlib.Path | None = None,
        case_id: str | None = None,
        jobs: int | None = None,
        in_process: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        selected_runner = self._executable_runner(runner if runner is not None else self.runner)
        command = [
            sys.executable,
            "tools/read_compatibility.py",
            "verify",
            "--repository-root",
            str(self.root),
            "--corpus",
            str(self.corpus_path),
            "--oracle",
            str(self.oracle_path),
            "--runner",
            str(selected_runner),
        ]
        if case_id is not None:
            command.extend(["--case", case_id])
        if jobs is not None:
            command.extend(["--jobs", str(jobs)])
        if in_process:
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                mock.patch.object(sys, "stdout", stdout),
                mock.patch.object(sys, "stderr", stderr),
            ):
                return_code = read_compatibility.main(command[2:])
            return subprocess.CompletedProcess(
                command, return_code, stdout.getvalue(), stderr.getvalue()
            )
        return self._run_verifier(
            command, cwd=pathlib.Path(__file__).resolve().parents[2]
        )

    def _run_verifier(
        self, command: list[str], *, cwd: pathlib.Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        self._report_progress("cli-start")
        completed = subprocess.run(
            command, cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self._report_progress("cli-finished", return_code=completed.returncode)
        return completed

    @staticmethod
    def _executable_runner(path: pathlib.Path) -> pathlib.Path:
        return path.with_suffix(".cmd") if os.name == "nt" else path

    def _capture_runner(
        self,
        runner: pathlib.Path,
        *,
        cancelled: Callable[[], bool] | None = None,
        **limits: int,
    ) -> dict:
        options = {} if cancelled is None else {"cancelled": cancelled}
        return read_compatibility._run_bounded(
            self._executable_runner(runner),
            self.root / "tests/fixtures/read_compatibility/core.db",
            b"MSRT1\nSQL " + b"SELECT 1".hex().encode("ascii") + b"\nSTEP\nFINALIZE\n",
            {"child_seconds": 10, "output_bytes": 65536, "stderr_bytes": 1024, **limits},
            **options,
        )

    def test_native_capture_enforces_the_stdout_ceiling(self) -> None:
        for cancelled in (None, lambda: False):
            with self.subTest(cancellable=cancelled is not None):
                with self.assertRaisesRegex(read_compatibility.HarnessError, "stdout exceeded"):
                    self._capture_runner(self.runner, output_bytes=1, cancelled=cancelled)

    def test_native_capture_rejects_stderr(self) -> None:
        if os.name == "nt":
            self.runner.with_suffix(".stderr").write_text(
                "unexpected diagnostic\n", encoding="utf-8"
            )
        else:
            source = self.runner.read_text(encoding="utf-8")
            self.runner.write_text(
                source.replace(
                    "raise SystemExit", "print('unexpected diagnostic', file=sys.stderr)\nraise SystemExit"
                ),
                encoding="utf-8",
            )
        for cancelled in (None, lambda: False):
            with self.subTest(cancellable=cancelled is not None):
                with self.assertRaisesRegex(read_compatibility.HarnessError, "wrote to stderr"):
                    self._capture_runner(self.runner, cancelled=cancelled)

    def test_native_capture_keeps_the_absolute_child_deadline(self) -> None:
        runner = self.root / "timeout_runner.py"
        if os.name == "nt":
            runner.with_suffix(".cmd").write_text(
                "@for /L %%i in (1,1,2147483647) do @rem waiting\n", encoding="utf-8"
            )
        else:
            runner.write_text(
                "#!/usr/bin/env python3\nimport time\ntime.sleep(5)\n", encoding="utf-8"
            )
            runner.chmod(0o755)
        for cancelled in (None, lambda: False):
            with self.subTest(cancellable=cancelled is not None):
                with self.assertRaisesRegex(read_compatibility.HarnessError, "timed out"):
                    self._capture_runner(runner, child_seconds=1, cancelled=cancelled)

    def test_pre_cancelled_capture_acquires_no_process_resources(self) -> None:
        with (
            mock.patch.object(read_compatibility.subprocess, "Popen") as launch,
            mock.patch.object(read_compatibility.tempfile, "TemporaryFile") as temporary,
            self.assertRaisesRegex(read_compatibility.HarnessError, "cancelled"),
        ):
            self._capture_runner(self.runner, cancelled=lambda: True)
        launch.assert_not_called()
        temporary.assert_not_called()

    def _assert_capture_cancellation(self, *, after_eof: bool) -> None:
        from threading import Event

        script = self.root / "cancellation_runner.py"
        child_pid = self.root / "cancellation_runner.pid"
        script.write_text(
            "import os\nimport time\n"
            + f"with open({str(child_pid)!r}, 'w') as output:\n"
            + "    output.write(str(os.getpid()))\n"
            + ("os.close(1)\nos.close(2)\n" if after_eof else "")
            + "time.sleep(5)\n",
            encoding="utf-8",
        )
        cancelled = Event()
        processes = []
        inputs = []
        original_launch = subprocess.Popen
        original_readable = read_compatibility._readable_runner_pipes
        interpreter = pathlib.Path(sys.executable)
        if os.name == "nt":
            # A Windows venv redirector retains the real child's pipe handles.
            interpreter = pathlib.Path(sys.base_prefix) / interpreter.name

        def launch(*args, **kwargs):
            process = original_launch(*args, **kwargs)
            processes.append(process)
            inputs.append(kwargs["stdin"])
            if after_eof:
                original_wait = process.wait

                def wait(timeout=None):
                    if timeout is not None:
                        cancelled.set()
                    return original_wait(timeout=timeout)

                process.wait = wait
                self.addCleanup(setattr, process, "wait", original_wait)
            return process

        def readable(selector, deadline):
            if not after_eof:
                cancelled.set()
            return original_readable(selector, deadline)

        with (
            mock.patch.object(read_compatibility.subprocess, "Popen", side_effect=launch),
            mock.patch.object(read_compatibility, "_readable_runner_pipes", side_effect=readable),
            self.assertRaisesRegex(read_compatibility.HarnessError, "cancelled"),
        ):
            read_compatibility._run_bounded(
                interpreter, script, b"",
                {"child_seconds": 1, "output_bytes": 65536, "stderr_bytes": 1024},
                cancelled=cancelled.is_set,
            )
        self.assertTrue(cancelled.is_set())
        self.assertEqual(1, len(processes))
        self.assertIsNotNone(processes[0].poll())
        self.assertTrue(processes[0].stdout.closed)
        self.assertTrue(processes[0].stderr.closed)
        self.assertTrue(inputs[0].closed)
        if after_eof:
            self.assertEqual(processes[0].pid, int(child_pid.read_text()))

    def test_capture_cancellation_reaps_the_blocked_process(self) -> None:
        self._assert_capture_cancellation(after_eof=False)

    def test_capture_cancellation_reaps_a_process_that_closed_its_pipes(self) -> None:
        self._assert_capture_cancellation(after_eof=True)

    def test_timeout_diagnostics_keep_the_outer_ctest_deadline(self) -> None:
        with (
            mock.patch.object(faulthandler, "dump_traceback_later") as arm,
            mock.patch.object(faulthandler, "cancel_dump_traceback_later") as cancel,
        ):
            setUpModule()
            tearDownModule()

        arm.assert_called_once_with(8, repeat=False)
        cancel.assert_called_once_with()

    def test_import_progress_locates_module_preparation_without_running_tests(self) -> None:
        import importlib.util
        from types import ModuleType, SimpleNamespace

        spec = importlib.util.spec_from_file_location(
            "read_compatibility_import_probe", __file__
        )
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        def import_probe() -> ModuleType:
            module = importlib.util.module_from_spec(spec)
            stream = io.StringIO()
            original_profile = sys.getprofile()
            with (
                mock.patch.object(sys, "stderr", stream),
                mock.patch.object(unittest, "main") as main,
                mock.patch.object(faulthandler, "dump_traceback_later") as arm,
            ):
                spec.loader.exec_module(module)
            main.assert_not_called()
            arm.assert_not_called()
            self.assertIs(original_profile, sys.getprofile())
            records = [
                json.loads(line.removeprefix("read-compatibility-progress "))
                for line in stream.getvalue().splitlines()
                if line.startswith("read-compatibility-progress ")
            ]
            expected_phases = [
                "bootstrap-ready",
                "stdlib-faulthandler-ready",
                "stdlib-hashlib-ready",
                "stdlib-io-ready",
                "stdlib-pathlib-ready",
                "stdlib-subprocess-ready",
                "stdlib-tempfile-ready",
                "stdlib-unittest-ready",
                "stdlib-ready",
                "harness-ready",
            ]
            if os.name == "nt" and original_profile is not None:
                expected_phases.insert(1, "extension-profile-busy")
            self.assertEqual(expected_phases, [record["phase"] for record in records])
            entries = {record["module_entry_unix_ns"] for record in records}
            self.assertEqual(1, len(entries))
            self.assertGreater(next(iter(entries)), 0)
            self.assertEqual(
                sorted(record["wall_ns"] for record in records),
                [record["wall_ns"] for record in records],
            )
            for record in records:
                self.assertIsNone(record["test"])
                self.assertEqual(os.getpid(), record["pid"])
                self.assertGreaterEqual(record["wall_ns"], 0)
                self.assertGreaterEqual(record["owner_cpu_ns"], 0)
            self.assertEqual(
                unittest.defaultTestLoader.loadTestsFromTestCase(
                    ReadCompatibilityValidationTest
                ).countTestCases(),
                unittest.defaultTestLoader.loadTestsFromTestCase(
                    module.ReadCompatibilityValidationTest
                ).countTestCases(),
            )
            return module

        module = import_probe()
        with (
            mock.patch.object(sys, "getprofile", return_value=object()),
            mock.patch.object(sys, "setprofile") as configure,
        ):
            import_probe()
            configure.assert_not_called()

        observer = module._ExtensionImportProgress()
        events = io.StringIO()
        expected = []
        with mock.patch.object(sys, "stderr", events):
            for name in ("_hashlib", "_ssl"):
                origin = rf"C:\runtime\{name}.pyd"
                for operation, phase, argument_name in (
                    ("create_dynamic", "extension-create", "spec"),
                    ("exec_dynamic", "extension-exec", "module"),
                ):
                    argument = (
                        SimpleNamespace(name=name, origin=origin)
                        if argument_name == "spec"
                        else SimpleNamespace(__name__=name, __file__=origin)
                    )
                    function = getattr(sys.modules["_imp"], operation)
                    for forwarded in (False, True):
                        frame = SimpleNamespace(
                            f_code=SimpleNamespace(
                                co_name=(
                                    "_call_with_frames_removed"
                                    if forwarded else operation
                                )
                            ),
                            f_locals=(
                                {"args": (argument,)}
                                if forwarded else {argument_name: argument}
                            ),
                        )
                        for event, suffix in (
                            ("c_call", "begin"),
                            ("c_return", "return"),
                            ("c_exception", "exception"),
                        ):
                            observer._observe(frame, event, function)
                            expected.append((name, phase + "-" + suffix, origin))
                        observer._observe(frame, "call", function)
                        observer._observe(frame, "c_call", len)
                        observer._observe(
                            frame, "c_call", sys.modules["_imp"].is_builtin
                        )
            unrelated = SimpleNamespace(
                f_code=SimpleNamespace(co_name="create_module"),
                f_locals={"spec": SimpleNamespace(name="unrelated", origin="other")},
            )
            observer._observe(
                unrelated, "c_call", sys.modules["_imp"].create_dynamic
            )
        observed = [
            json.loads(line.removeprefix("read-compatibility-progress "))
            for line in events.getvalue().splitlines()
        ]
        self.assertEqual(
            expected,
            [
                (record["extension"], record["phase"], record["extension_origin"])
                for record in observed
            ],
        )
        for record in observed:
            self.assertIsNone(record["test"])
            self.assertEqual(module._MODULE_ENTRY_UNIX_NS, record["module_entry_unix_ns"])
            self.assertEqual(os.getpid(), record["pid"])

        for fail in (False, True):
            with (
                mock.patch.object(module.os, "name", "nt"),
                mock.patch.object(sys, "getprofile", return_value=None),
                mock.patch.object(sys, "setprofile") as configure,
            ):
                owner = module._ExtensionImportProgress()
                if fail:
                    with self.assertRaisesRegex(RuntimeError, "import failure control"):
                        with owner:
                            raise RuntimeError("import failure control")
                else:
                    with owner as active:
                        self.assertIs(owner, active)
                self.assertEqual(2, configure.call_count)
                self.assertEqual(owner._observe, configure.call_args_list[0].args[0])
                self.assertEqual(mock.call(None), configure.call_args_list[-1])

        busy = io.StringIO()
        with (
            mock.patch.object(module.os, "name", "nt"),
            mock.patch.object(sys, "getprofile", return_value=object()),
            mock.patch.object(sys, "setprofile") as configure,
            mock.patch.object(sys, "stderr", busy),
        ):
            with self.assertRaisesRegex(RuntimeError, "busy failure control"):
                with module._ExtensionImportProgress():
                    raise RuntimeError("busy failure control")
            configure.assert_not_called()
        busy_records = [
            json.loads(line.removeprefix("read-compatibility-progress "))
            for line in busy.getvalue().splitlines()
        ]
        self.assertEqual(["extension-profile-busy"], [row["phase"] for row in busy_records])

        with (
            mock.patch.object(module.os, "name", "posix"),
            mock.patch.object(sys, "getprofile") as current,
            mock.patch.object(sys, "setprofile") as configure,
        ):
            with module._ExtensionImportProgress():
                pass
            current.assert_not_called()
            configure.assert_not_called()

    def test_progress_preserves_a_failed_test_outcome(self) -> None:
        class FailedCase(ReadCompatibilityValidationTest):
            def setUp(self) -> None:
                pass

            def fail_progress_probe(self) -> None:
                raise RuntimeError("progress failure control")

        failed = FailedCase("fail_progress_probe")
        outcome = unittest.TestResult()
        stream = io.StringIO()

        with mock.patch.object(sys, "stderr", stream):
            failed.run(outcome)

        self.assertFalse(outcome.wasSuccessful())
        self.assertEqual(1, len(outcome.errors))
        self.assertIn("progress failure control", outcome.errors[0][1])
        records = [
            json.loads(line.removeprefix("read-compatibility-progress "))
            for line in stream.getvalue().splitlines()
            if line.startswith("read-compatibility-progress ")
        ]
        self.assertGreater(len(records), 0)
        self.assertEqual("test-enter", records[0]["phase"])
        self.assertEqual("test-finished", records[-1]["phase"])
        for record in records:
            self.assertEqual(failed.id(), record["test"])
            self.assertEqual(os.getpid(), record["pid"])
            self.assertGreaterEqual(record["wall_ns"], 0)
            self.assertGreaterEqual(record["owner_cpu_ns"], 0)

    def test_native_capture_requests_noninteractive_creation_without_a_shell(self) -> None:
        with mock.patch.object(
            read_compatibility.subprocess, "Popen", side_effect=OSError("launch control")
        ) as launch:
            with self.assertRaisesRegex(
                read_compatibility.HarnessError, "cannot launch compatibility runner"
            ):
                self._capture_runner(self.runner)

        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.assertEqual(flags, launch.call_args.kwargs.get("creationflags", 0))
        self.assertNotIn("shell", launch.call_args.kwargs)
        self.assertEqual(subprocess.PIPE, launch.call_args.kwargs["stdout"])
        self.assertEqual(subprocess.PIPE, launch.call_args.kwargs["stderr"])
        self.assertTrue(launch.call_args.kwargs["stdin"].closed)

    def test_strict_json_rejects_duplicate_object_keys(self) -> None:
        duplicate = self.root / "duplicate.json"
        duplicate.write_text('{"schema_version":1,"schema_version":1}\n')

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "duplicate JSON key: schema_version"
        ):
            read_compatibility.load_json_strict(duplicate)

    def test_valid_profile_and_corpus_publish_normalized_paths(self) -> None:
        profile = read_compatibility.validate_profile(
            self.root, self.profile_path
        )
        corpus = read_compatibility.validate_corpus(self.root, self.corpus_path)

        self.assertEqual("3.54.0", profile["sqlite"]["version"])
        self.assertEqual(self.profile_path.resolve(), corpus["oracle_profile_path"])
        self.assertEqual(
            (self.root / "tests/fixtures/read_compatibility/core.db").resolve(),
            corpus["database_paths"]["core"],
        )
        self.assertEqual(["constant-one", "order-by-boundary"], corpus["case_ids"])

    def test_stale_database_hash_is_rejected_before_execution(self) -> None:
        database = self.root / "tests/fixtures/read_compatibility/core.db"
        database.write_bytes(b"changed")

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "database hash mismatch: core"
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_transcript_requires_final_finalize_and_valid_typed_values(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][0]["operations"] = [
            {
                "op": "bind",
                "index": 1,
                "value": {"type": "real", "bits": "not-real-bits"},
            },
            {"op": "step"},
        ]
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case constant-one: REAL bits must contain 16 hexadecimal digits",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_bind_index_must_fit_the_sqlite_c_int_contract(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][0]["operations"] = [
            {
                "op": "bind",
                "index": 2_147_483_648,
                "value": {"type": "integer", "value": "1"},
            },
            {"op": "finalize"},
        ]
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case constant-one: operation 0 index exceeds SQLite C int range",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_rejected_statement_requires_a_runnable_fallback_transcript(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][1]["operations"] = []
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case order-by-boundary: rejected statement requires a FINALIZE fallback",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_prepare_error_requires_a_runnable_fallback_transcript(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"][0]["feature"] = "prepare-errors"
        corpus["cases"][0]["sql_hex"] = b"SELECT )".hex()
        corpus["cases"][0]["operations"] = []
        corpus["supported_matrix"].append("prepare-errors")
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case constant-one: rejected statement requires a FINALIZE fallback",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_unsupported_case_requires_a_coarse_modern_rejection(self) -> None:
        corpus = json.loads(self.corpus_path.read_text())
        del corpus["cases"][1]["modern_rejection"]
        write_json(self.corpus_path, corpus)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "case order-by-boundary: unsupported case requires modern_rejection",
        ):
            read_compatibility.validate_corpus(self.root, self.corpus_path)

    def test_profile_rejects_stale_generator_hash(self) -> None:
        generator = self.root / "tools/read_compatibility.py"
        generator.write_text("# changed generator\n", encoding="utf-8")

        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "generator hash mismatch"
        ):
            read_compatibility.validate_profile(self.root, self.profile_path)

    def test_oracle_requires_the_exact_semantic_compile_options(self) -> None:
        oracle = json.loads(self.oracle_path.read_text())
        oracle["sqlite"]["compile_options"].append("OMIT_AUTORESET")
        write_json(self.oracle_path, oracle)

        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "oracle compile options do not match the pinned semantic profile",
        ):
            read_compatibility.validate_oracle(
                self.root,
                self.oracle_path,
                self.corpus_path,
                read_compatibility.validate_corpus(
                    self.root, self.corpus_path
                ),
            )

    def test_native_oracle_metadata_requires_the_explicit_compiler_profile(self) -> None:
        profile = json.loads(self.profile_path.read_text())
        profile["build"]["semantic_compile_options"].insert(0, "ATOMIC_INTRINSICS=1")
        write_json(self.profile_path, profile)
        oracle = json.loads(self.oracle_path.read_text())
        oracle["oracle_profile_sha256"] = sha256(self.profile_path)
        oracle["sqlite"]["compile_options"].insert(0, "ATOMIC_INTRINSICS=0")
        write_json(self.oracle_path, oracle)
        corpus = read_compatibility.validate_corpus(self.root, self.corpus_path)
        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "pinned semantic profile"
        ):
            read_compatibility.validate_oracle(
                self.root, self.oracle_path, self.corpus_path, corpus
            )
        native = read_compatibility.validate_oracle(
            self.root, self.oracle_path, self.corpus_path, corpus,
            compiler_profile="msvc",
        )
        self.assertEqual("ATOMIC_INTRINSICS=0", native["sqlite"]["compile_options"][0])
        oracle["sqlite"]["compile_options"].append("OMIT_AUTORESET")
        write_json(self.oracle_path, oracle)
        with self.assertRaisesRegex(
            read_compatibility.HarnessError, "pinned semantic profile"
        ):
            read_compatibility.validate_oracle(
                self.root, self.oracle_path, self.corpus_path, corpus,
                compiler_profile="msvc",
            )

    def test_verify_accepts_matching_outcomes_and_ignores_diagnostic_codes(self) -> None:
        oracle_outcome = json.loads(json.dumps(self.constant_outcome))
        oracle_outcome["observations"][2]["status"].update(
            {
                "return_code": 999,
                "extended_code": 999,
                "message_hex": b"diagnostic only".hex(),
            }
        )
        self._write_oracle(oracle_outcome)

        completed = self._verify()

        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual(
            "verified 1 compatibility case and 1 unsupported boundary\n",
            completed.stdout,
        )
        self.assertEqual("", completed.stderr)

    def test_verify_accepts_two_isolated_workers(self) -> None:
        completed = self._verify(jobs=2)

        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual(
            "verified 1 compatibility case and 1 unsupported boundary\n",
            completed.stdout,
        )
        self.assertEqual("", completed.stderr)

    def test_verify_rejects_invalid_worker_counts_before_launch(self) -> None:
        for jobs in (0, 3):
            with self.subTest(entrypoint_jobs=jobs):
                completed = self._verify(jobs=jobs, in_process=True)
                self.assertEqual(1, completed.returncode)
                self.assertIn("invalid choice", completed.stderr)
                self.assertEqual("", completed.stdout)
        with mock.patch.object(read_compatibility, "_run_bounded") as run:
            for jobs in (False, 0, 3, "2"):
                with self.subTest(api_jobs=jobs):
                    with self.assertRaisesRegex(read_compatibility.HarnessError, "jobs"):
                        read_compatibility.verify_compatibility(
                            self.root, self.corpus_path, self.oracle_path,
                            self._executable_runner(self.runner), None, jobs=jobs,
                        )
            run.assert_not_called()

    def test_parallel_verification_validates_all_provenance_before_launch(self) -> None:
        oracle = json.loads(self.oracle_path.read_text())
        oracle["corpus_sha256"] = "0" * 64
        write_json(self.oracle_path, oracle)

        with (
            mock.patch.object(read_compatibility, "_run_bounded") as run,
            self.assertRaisesRegex(read_compatibility.HarnessError, "corpus hash mismatch"),
        ):
            read_compatibility.verify_compatibility(
                self.root, self.corpus_path, self.oracle_path,
                self._executable_runner(self.runner), None, jobs=2,
            )
        run.assert_not_called()

    def _append_compare_case(self, case_id: str, sql: bytes, outcome: dict) -> dict:
        corpus = json.loads(self.corpus_path.read_text())
        corpus["cases"].append({
            **corpus["cases"][0], "id": case_id, "sql_hex": sql.hex(),
        })
        write_json(self.corpus_path, corpus)
        oracle = json.loads(self.oracle_path.read_text())
        oracle["corpus_sha256"] = sha256(self.corpus_path)
        oracle["cases"].append({
            "id": case_id, "expectation": "compare", "outcome": outcome,
        })
        write_json(self.oracle_path, oracle)
        return corpus

    def test_parallel_verification_refills_the_window_without_missing_or_repeating_cases(self) -> None:
        second = json.loads(json.dumps(self.constant_outcome))
        second["preparation"]["columns"][0]["name_hex"] = "32"
        second["observations"][0]["columns"][0]["name_hex"] = "32"
        second["observations"][0]["row"][0]["value"] = "2"
        corpus = self._append_compare_case("constant-two", b"SELECT 2", second)
        self._write_runner(self.runner, {
            b"SELECT 1".hex(): self.constant_outcome,
            b"SELECT id FROM items ORDER BY id".hex(): self.unsupported_outcome,
            b"SELECT 2".hex(): second,
        })

        with mock.patch.object(
            read_compatibility, "_run_bounded", wraps=read_compatibility._run_bounded
        ) as run:
            completed = self._verify(jobs=2, in_process=True)
        self.assertEqual(0, completed.returncode, completed.stderr)
        self.assertEqual(
            "verified 2 compatibility cases and 1 unsupported boundary\n", completed.stdout
        )
        self.assertEqual("", completed.stderr)
        self.assertCountEqual(
            [read_compatibility._protocol_for_case(case) for case in corpus["cases"]],
            [call.args[2] for call in run.call_args_list],
        )

    def _assert_ordered_parallel_failure(self, *, later_failure: bool) -> None:
        from concurrent.futures import ThreadPoolExecutor
        from threading import Event

        corpus = self._append_compare_case("not-admitted", b"SELECT 1", self.constant_outcome)
        wrong = json.loads(json.dumps(self.constant_outcome))
        wrong["observations"][0]["row"][0]["value"] = "2"
        second_started = Event()
        sibling_cancelled = Event()
        submitted = []
        original_submit = ThreadPoolExecutor.submit

        def submit(executor, *args, **kwargs):
            submitted.append(args)
            return original_submit(executor, *args, **kwargs)

        def execute(_runner, _database, protocol, _limits, *, cancelled=None):
            self.assertIsNotNone(cancelled)
            if protocol == read_compatibility._protocol_for_case(corpus["cases"][0]):
                self.assertTrue(second_started.wait(1), "second isolated runner did not start")
                return wrong
            second_started.set()
            if later_failure:
                raise read_compatibility.HarnessError("later worker failure")
            deadline = time.monotonic() + 1
            while not cancelled():
                if time.monotonic() >= deadline:
                    self.fail("speculative runner was not cancelled")
                time.sleep(0.001)
            sibling_cancelled.set()
            raise read_compatibility.HarnessError("cancelled speculative runner")

        with (
            mock.patch.object(ThreadPoolExecutor, "submit", new=submit),
            mock.patch.object(read_compatibility, "_run_bounded", side_effect=execute),
            self.assertRaises(read_compatibility.VerificationMismatch) as caught,
        ):
            read_compatibility.verify_compatibility(
                self.root, self.corpus_path, self.oracle_path,
                self._executable_runner(self.runner), None, jobs=2,
            )
        self.assertEqual("constant-one", caught.exception.case["id"])
        self.assertEqual("compatibility mismatch", caught.exception.classification)
        self.assertEqual(2, len(submitted))
        if not later_failure:
            self.assertTrue(sibling_cancelled.is_set(), "worker was not joined before failure")

    def test_parallel_verification_keeps_first_corpus_failure_and_bounded_admission(self) -> None:
        self._assert_ordered_parallel_failure(later_failure=True)

    def test_parallel_verification_cancels_and_joins_speculative_work(self) -> None:
        self._assert_ordered_parallel_failure(later_failure=False)

    if os.name == "nt":
        def test_windows_fixture_replays_without_a_python_producer(self) -> None:
            self.runner.unlink()

            completed = self._verify()

            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertEqual(
                "verified 1 compatibility case and 1 unsupported boundary\n",
                completed.stdout,
            )
            self.assertEqual("", completed.stderr)

            literal_directory = self.root / "literal !%&() \u00e9"
            literal_directory.mkdir()
            runner = literal_directory / "literal_runner.py"
            self._write_runner(runner, {b"SELECT 1".hex(): self.constant_outcome})
            runner.unlink()
            limits = {"child_seconds": 10, "output_bytes": 65536, "stderr_bytes": 1024}
            database = self.root / "tests/fixtures/read_compatibility/core.db"
            for newline in (b"\n", b"\r\n"):
                with self.subTest(newline=newline):
                    protocol = newline.join(
                        (
                            b"MSRT1",
                            b"SQL " + b"SELECT 1".hex().encode("ascii"),
                            b"STEP",
                            b"FINALIZE",
                            b"",
                        )
                    )
                    outcome = read_compatibility._run_bounded(
                        runner.with_suffix(".cmd"), database, protocol, limits
                    )
                    self.assertEqual(self.constant_outcome, outcome)

            with self.assertRaisesRegex(
                read_compatibility.HarnessError, "unknown synthetic fixture SQL"
            ):
                read_compatibility._run_bounded(
                    runner.with_suffix(".cmd"), database,
                    b"MSRT1\nSQL " + b"SELECT 9".hex().encode("ascii") + b"\nFINALIZE\n",
                    limits,
                )
            runner.with_suffix(".outcome-0.json").unlink()
            with self.assertRaisesRegex(
                read_compatibility.HarnessError, "runner exited with code 1"
            ):
                self._capture_runner(runner)

    def test_verify_reports_first_difference_and_reproduction_protocol(self) -> None:
        wrong = json.loads(json.dumps(self.constant_outcome))
        wrong["observations"][0]["row"][0]["value"] = "2"
        self._write_oracle(wrong)

        for jobs in (None, 2):
            with self.subTest(jobs=jobs):
                completed = self._verify(
                    case_id="constant-one", jobs=jobs, in_process=jobs == 2
                )
                self.assertEqual(2, completed.returncode)
                self.assertEqual("", completed.stdout)
                self.assertIn("compatibility mismatch: constant-one", completed.stderr)
                self.assertIn(
                    "first difference: $.observations[0].row[0].value", completed.stderr
                )
                self.assertIn("reproduce:", completed.stderr)
                self.assertIn("MSRT1", completed.stderr)

    def test_verify_rejects_nonzero_child_even_with_valid_json(self) -> None:
        failing_runner = self.root / "failing_runner.py"
        self._write_runner(
            failing_runner,
            {
                b"SELECT 1".hex(): self.constant_outcome,
                b"SELECT id FROM items ORDER BY id".hex(): self.unsupported_outcome,
            },
            exit_code=3,
        )

        for jobs in (None, 2):
            with self.subTest(jobs=jobs):
                completed = self._verify(
                    runner=failing_runner, case_id="constant-one", jobs=jobs,
                    in_process=jobs == 2,
                )
                self.assertEqual(1, completed.returncode)
                self.assertIn("runner exited with code 3", completed.stderr)

    def test_verify_reports_unsupported_acceptance_separately(self) -> None:
        accepting_runner = self.root / "accepting_runner.py"
        self._write_runner(
            accepting_runner,
            {
                b"SELECT 1".hex(): self.constant_outcome,
                b"SELECT id FROM items ORDER BY id".hex(): self.constant_outcome,
            },
        )

        for jobs in (None, 2):
            with self.subTest(jobs=jobs):
                completed = self._verify(
                    runner=accepting_runner, case_id="order-by-boundary", jobs=jobs,
                    in_process=jobs == 2,
                )
                self.assertEqual(2, completed.returncode)
                self.assertIn(
                    "unsupported boundary regression: order-by-boundary", completed.stderr
                )
                self.assertNotIn("compatibility mismatch", completed.stderr)

    def test_verify_rejects_unknown_case_before_launching_runner(self) -> None:
        stream = io.StringIO()
        with mock.patch.object(sys, "stderr", stream):
            completed = self._verify(case_id="missing")

        self.assertEqual(1, completed.returncode)
        self.assertIn("unknown compatibility case: missing", completed.stderr)
        records = [
            json.loads(line.removeprefix("read-compatibility-progress "))
            for line in stream.getvalue().splitlines()
            if line.startswith("read-compatibility-progress ")
        ]
        self.assertEqual(
            ["cli-start", "cli-finished"], [record["phase"] for record in records]
        )
        self.assertEqual(completed.returncode, records[-1]["return_code"])
        self.assertNotIn("read-compatibility-progress", completed.stderr)

    def test_command_line_usage_errors_return_exit_code_one(self) -> None:
        completed = self._run_verifier(
            [sys.executable, read_compatibility.__file__, "verify"]
        )

        self.assertEqual(1, completed.returncode)
        self.assertEqual("", completed.stdout)
        self.assertIn("read compatibility error:", completed.stderr)
        self.assertIn("the following arguments are required:", completed.stderr)

    def test_regenerate_writes_a_deterministic_valid_oracle(self) -> None:
        class FakePinnedSqliteOracle:
            compile_options = ("DQS=3", "THREADSAFE=0")

            def run_case(
                self,
                path: pathlib.Path,
                sql: bytes,
                operations: list[dict[str, object]],
            ) -> dict[str, object]:
                self_path = (
                    self_outer.root
                    / "tests/fixtures/read_compatibility/core.db"
                ).resolve()
                self_outer.assertEqual(self_path, path)
                self_outer.assertEqual(b"SELECT 1", sql)
                self_outer.assertEqual(
                    [{"op": "step"}, {"op": "step"}, {"op": "finalize"}],
                    operations,
                )
                return self_outer.constant_outcome

            def run_boundary(
                self, path: pathlib.Path, sql: bytes
            ) -> dict[str, object]:
                self_outer.assertEqual(
                    (
                        self_outer.root
                        / "tests/fixtures/read_compatibility/core.db"
                    ).resolve(),
                    path,
                )
                self_outer.assertEqual(
                    b"SELECT id FROM items ORDER BY id", sql
                )
                return {"prepare": "statement", "first_step": "row"}

        self_outer = self
        output = self.root / "tests/compatibility/generated-oracle.json"
        sqlite_library = self.root / "libsqlite3.so"
        sqlite_c = self.root / "sqlite3.c"
        sqlite_h = self.root / "sqlite3.h"
        for path in (sqlite_library, sqlite_c, sqlite_h):
            path.write_bytes(b"synthetic")

        with mock.patch.object(
            read_compatibility,
            "PinnedSqliteOracle",
            return_value=FakePinnedSqliteOracle(),
        ):
            counts = read_compatibility.regenerate_oracle(
                self.root,
                self.corpus_path,
                output,
                sqlite_library,
                sqlite_c,
                sqlite_h,
            )
            first = output.read_bytes()
            repeated_counts = read_compatibility.regenerate_oracle(
                self.root,
                self.corpus_path,
                output,
                sqlite_library,
                sqlite_c,
                sqlite_h,
            )

        self.assertEqual((1, 1), counts)
        self.assertEqual(counts, repeated_counts)
        self.assertEqual(first, output.read_bytes())
        oracle = read_compatibility.validate_oracle(
            self.root,
            output,
            self.corpus_path,
            read_compatibility.validate_corpus(self.root, self.corpus_path),
        )
        self.assertEqual(
            ["constant-one", "order-by-boundary"],
            [case["id"] for case in oracle["cases"]],
        )

    def test_regenerate_refuses_to_overwrite_a_corpus_provenance_input(self) -> None:
        with self.assertRaisesRegex(
            read_compatibility.HarnessError,
            "oracle output must not overwrite a provenance input",
        ):
            read_compatibility.regenerate_oracle(
                self.root,
                self.corpus_path,
                self.corpus_path,
                self.root / "libsqlite3.so",
                self.root / "sqlite3.c",
                self.root / "sqlite3.h",
            )

    def test_regenerate_refuses_to_overwrite_sqlite_provenance_inputs(self) -> None:
        sqlite_library = self.root / "libsqlite3.so"
        sqlite_c = self.root / "sqlite3.c"
        sqlite_h = self.root / "sqlite3.h"
        for path in (sqlite_library, sqlite_c, sqlite_h):
            path.write_bytes(b"synthetic")

        for output in (sqlite_library, sqlite_c, sqlite_h):
            with self.subTest(output=output):
                with mock.patch.object(
                    read_compatibility,
                    "PinnedSqliteOracle",
                    side_effect=AssertionError(
                        "oracle must not load an overwritten provenance input"
                    ),
                ):
                    with self.assertRaisesRegex(
                        read_compatibility.HarnessError,
                        "oracle output must not overwrite a provenance input",
                    ):
                        read_compatibility.regenerate_oracle(
                            self.root,
                            self.corpus_path,
                            output,
                            sqlite_library,
                            sqlite_c,
                            sqlite_h,
                        )

    def test_regenerate_refuses_sqlite_provenance_path_aliases(self) -> None:
        sqlite_library = self.root / "libsqlite3.so"
        sqlite_c = self.root / "sqlite3.c"
        sqlite_h = self.root / "sqlite3.h"
        for path in (sqlite_library, sqlite_c, sqlite_h):
            path.write_bytes(b"synthetic")

        aliases: list[pathlib.Path] = []
        for path in (sqlite_library, sqlite_c, sqlite_h):
            alias = path.with_name(path.name.upper())
            if not alias.exists():
                alias.hardlink_to(path)
            aliases.append(alias)

        for output in aliases:
            with self.subTest(output=output):
                with mock.patch.object(
                    read_compatibility,
                    "PinnedSqliteOracle",
                    side_effect=AssertionError(
                        "oracle must not load an aliased provenance input"
                    ),
                ):
                    with self.assertRaisesRegex(
                        read_compatibility.HarnessError,
                        "oracle output must not overwrite a provenance input",
                    ):
                        read_compatibility.regenerate_oracle(
                            self.root,
                            self.corpus_path,
                            output,
                            sqlite_library,
                            sqlite_c,
                            sqlite_h,
                        )


if __name__ == "__main__":
    unittest.main()

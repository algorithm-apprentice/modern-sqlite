#!/usr/bin/env python3

import argparse
import contextlib
import io
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import run_changed_clang_tidy
from tests.tools.verification_test import initialize_repository


class ProductionSourceScopeTest(unittest.TestCase):
    def test_compilation_paths_use_the_entry_working_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            build = root / "build"
            build.mkdir()
            entries = [
                {
                    "directory": str(build),
                    "file": "../src/relative.cpp",
                    "output": "CMakeFiles/engine.dir/src/relative.cpp.o",
                },
                {
                    "directory": str(build),
                    "file": str(root / "src" / "absolute.cpp"),
                    "output": str(build / "CMakeFiles" / "engine.dir" / "src" / "absolute.cpp.o"),
                },
            ]
            (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
            with mock.patch.object(run_changed_clang_tidy, "ROOT", root):
                commands = run_changed_clang_tidy.compile_commands(pathlib.Path("build"))
            self.assertEqual(
                {
                    root / "src" / "relative.cpp": str(
                        pathlib.Path("CMakeFiles") / "engine.dir" / "src" / "relative.cpp.o"
                    ),
                    root / "src" / "absolute.cpp": str(
                        pathlib.Path("CMakeFiles") / "engine.dir" / "src" / "absolute.cpp.o"
                    ),
                },
                commands,
            )

    def test_partitions_whole_sources_once_with_stable_balanced_weights(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            sources = []
            for index, size in enumerate((20, 10, 9, 6, 4, 3, 2, 0)):
                path = root / f"unit{index}.cpp"
                path.write_bytes(b"x" * size)
                sources.append(path)
            partitions = run_changed_clang_tidy.partition_sources(set(sources))
            self.assertEqual(3, len(partitions))
            self.assertEqual(
                [[sources[0]], [sources[1], sources[4], sources[5], sources[7]],
                 [sources[2], sources[3], sources[6]]],
                partitions,
            )
            assigned = [path for partition in partitions for path in partition]
            self.assertEqual(sorted(sources), sorted(assigned))
            self.assertEqual(len(sources), len(set(assigned)))
            self.assertEqual(
                partitions, run_changed_clang_tidy.partition_sources(set(reversed(sources)))
            )

    def test_empty_and_zero_length_sources_keep_bounded_shards(self) -> None:
        self.assertEqual([[], [], []], run_changed_clang_tidy.partition_sources(set()))
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary).resolve() / "empty.cpp"
            path.touch()
            self.assertEqual([[path], [], []],
                             run_changed_clang_tidy.partition_sources({path}))

    def test_sharded_commands_cover_exactly_the_original_target_set(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            (root / "src").mkdir()
            (root / "tests").mkdir()
            sources = []
            for name, size in (("a", 9), ("b", 6), ("c", 2)):
                path = root / "src" / f"{name}.cpp"
                path.write_bytes(b"x" * size)
                sources.append(path)
            nonproduction = root / "tests" / "fixture.cpp"
            nonproduction.write_bytes(b"x")
            database = {
                path: f"CMakeFiles/engine.dir/src/{path.stem}.cpp.o" for path in sources
            }
            database[nonproduction] = "CMakeFiles/test.dir/fixture.cpp.o"

            def invoke(index, changed, return_code=0):
                args = argparse.Namespace(
                    base_ref="base", build_directory=pathlib.Path("build"),
                    jobs=3 if index is None else 1, smoke_source=[pathlib.Path("src/a.cpp")],
                    shard_index=index, report=None, reuse_report=None,
                )
                output = io.StringIO()
                with (
                    mock.patch.object(run_changed_clang_tidy, "ROOT", root),
                    mock.patch.object(run_changed_clang_tidy, "PRODUCTION_SOURCE_ROOT",
                                      root / "src"),
                    mock.patch.object(run_changed_clang_tidy, "parse_arguments",
                                      return_value=args),
                    mock.patch.object(run_changed_clang_tidy, "changed_files",
                                      return_value=changed),
                    mock.patch.object(run_changed_clang_tidy, "compile_commands",
                                      return_value=database),
                    mock.patch.object(run_changed_clang_tidy.subprocess, "run",
                                      return_value=subprocess.CompletedProcess([], return_code)) as build,
                    contextlib.redirect_stdout(output),
                ):
                    actual = run_changed_clang_tidy.main()
                marker = "changed-clang-tidy-selection "
                records = [
                    json.loads(line.removeprefix(marker)) for line in output.getvalue().splitlines()
                    if line.startswith(marker)
                ]
                self.assertEqual(1, len(records), output.getvalue())
                return actual, records[0], build

            changed = [path.relative_to(root) for path in [*sources, nonproduction]]
            result, original, build = invoke(None, changed)
            self.assertEqual(0, result)
            self.assertEqual(["src/a.cpp", "src/b.cpp", "src/c.cpp"],
                             original["selected_sources"])
            self.assertEqual(original["selected_sources"], original["assigned_sources"])
            expected_targets = sorted(database[path] for path in sources)
            self.assertEqual(
                ["cmake", "--build", str(root / "build"), "--parallel", "3",
                 "--target", *expected_targets],
                build.call_args.args[0],
            )
            assigned = []
            for index in range(3):
                with self.subTest(shard=index):
                    code = 7 if index == 1 else 0
                    result, record, build = invoke(index, changed, code)
                    self.assertEqual(code, result)
                    self.assertEqual(original["selected_sources"], record["selected_sources"])
                    self.assertEqual(3, record["shard_count"])
                    self.assertEqual(index, record["shard_index"])
                    self.assertEqual(1, record["parallel"])
                    self.assertEqual(
                        ["cmake", "--build", str(root / "build"), "--parallel", "1",
                         "--target", *record["targets"]],
                        build.call_args.args[0],
                    )
                    self.assertFalse(build.call_args.kwargs["check"])
                    assigned.extend(record["targets"])
            self.assertEqual(expected_targets, sorted(assigned))
            result, record, build = invoke(2, [pathlib.Path("src/a.cpp")])
            self.assertEqual(0, result)
            self.assertEqual(["src/a.cpp"], record["selected_sources"])
            self.assertEqual([], record["assigned_sources"])
            build.assert_not_called()

    def test_shard_arguments_keep_legacy_jobs_and_reject_conflicts(self) -> None:
        required = ["quality", "--base-ref", "base", "--build-directory", "build"]
        with mock.patch.object(sys, "argv", [*required, "--jobs", "3"]):
            args = run_changed_clang_tidy.parse_arguments()
        self.assertIsNone(args.shard_index)
        self.assertEqual(3, args.jobs)
        with mock.patch.object(sys, "argv", [*required, "--shard-index", "1"]):
            args = run_changed_clang_tidy.parse_arguments()
        self.assertEqual(1, args.jobs)
        with mock.patch.object(
            sys, "argv", [*required, "--smoke-source", "src/a.cpp", "--smoke-source", "src/b.cpp"],
        ):
            args = run_changed_clang_tidy.parse_arguments()
        self.assertEqual([pathlib.Path("src/a.cpp"), pathlib.Path("src/b.cpp")], args.smoke_source)
        for options in (
            ["--shard-index", "-1"], ["--shard-index", "3"],
            ["--shard-index", "0", "--jobs", "2"],
            ["--shard-index", "0", "--jobs", "0"],
        ):
            with (
                self.subTest(options=options),
                mock.patch.object(sys, "argv", [*required, *options]),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as raised,
            ):
                run_changed_clang_tidy.parse_arguments()
            self.assertEqual(2, raised.exception.code)

    def test_msvc_analysis_uses_complete_compilation_database(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "src").mkdir()
            (root / "src/fixture.cpp").write_text("void fixture();\n")
            helper = (REPOSITORY_ROOT / "cmake/ClangTidy.cmake").as_posix()
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(analysis_fixture NONE)\n"
                "set(MODERN_SQLITE_ENABLE_CLANG_TIDY ON)\n"
                'set(MODERN_SQLITE_CLANG_TIDY_EXECUTABLE "${CMAKE_COMMAND}")\n'
                f'include("{helper}")\n'
                "set(MSVC FALSE)\n"
                "add_library(other INTERFACE src/fixture.cpp)\n"
                "modern_sqlite_enable_clang_tidy(other)\n"
                "get_target_property(other_command other CXX_CLANG_TIDY)\n"
                "set(MSVC TRUE)\n"
                "add_library(msvc INTERFACE src/fixture.cpp)\n"
                "modern_sqlite_enable_clang_tidy(msvc)\n"
                "get_target_property(msvc_command msvc CXX_CLANG_TIDY)\n"
                "get_target_property(msvc_export msvc EXPORT_COMPILE_COMMANDS)\n"
                'file(WRITE "${PROJECT_BINARY_DIR}/commands.txt"\n'
                '     "${other_command}\\n${msvc_command}\\n${msvc_export}\\n")\n'
            )
            build = root / "build"
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(build)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            other, msvc, exported = (build / "commands.txt").read_text().splitlines()
            self.assertNotIn("-p", other.split(";"))
            msvc_arguments = msvc.split(";")
            self.assertIn("-p", msvc_arguments)
            self.assertEqual(
                build.as_posix(), msvc_arguments[msvc_arguments.index("-p") + 1]
            )
            self.assertEqual("ON", exported)
            for command in (other, msvc):
                self.assertIn("--warnings-as-errors=*", command.split(";"))

    def test_accepts_src_translation_units(self) -> None:
        self.assertTrue(
            run_changed_clang_tidy.is_production_source(
                REPOSITORY_ROOT / "src/base/bytes.cpp"
            )
        )

    def test_rejects_non_src_translation_units(self) -> None:
        for path in (
            "tests/unit/base/bytes_test.cpp",
            "benchmarks/read_performance.cpp",
            "tools/modern_sqlite_inspect.cpp",
            "src/../tests/unit/base/bytes_test.cpp",
        ):
            with self.subTest(path=path):
                self.assertFalse(
                    run_changed_clang_tidy.is_production_source(
                        REPOSITORY_ROOT / path
                    )
                )


@unittest.skipIf(os.name == "nt", "analysis report execution uses POSIX process ownership")
class ReportedAnalysisTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        directory = pathlib.Path(self.temporary.name).resolve()
        self.root = directory / "checkout with spaces"
        self.build = self.root / "build"
        self.build.mkdir(parents=True)
        (self.root / "src").mkdir()
        self.source = self.root / "src" / "unit.cpp"
        self.source.write_text("int value;\n")
        self.header = directory / "outside header.hpp"
        self.header.write_text("first header\n")
        (self.root / ".gitignore").write_text("/build/\n")
        (self.root / ".clang-tidy").write_text("Checks: '*'\n")
        self.counter = self.build / "analysis-count"
        checker = self.root / "checker.py"
        checker.write_text(
            "from pathlib import Path\n"
            f"p=Path({str(self.counter)!r})\n"
            "p.write_text(str(int(p.read_text())+1 if p.exists() else 1))\n"
        )
        compiler = self.root / "compiler.py"
        compiler.write_text(
            "import sys\nfrom pathlib import Path\n"
            "output=Path(sys.argv[sys.argv.index('-o')+1])\n"
            "source=Path(sys.argv[sys.argv.index('-c')+1])\n"
            f"header=Path({str(self.header)!r})\n"
            "output.parent.mkdir(parents=True,exist_ok=True)\n"
            "output.write_bytes(source.read_bytes()+header.read_bytes())\n"
            "deps=' '.join(str(p).replace(' ','\\\\ ') for p in (source,header))\n"
            "Path(str(output)+'.d').write_text(str(output)+': '+deps+'\\n')\n"
        )
        self.target = "CMakeFiles/engine.dir/src/unit.cpp.o"
        command = [
            shutil.which("cmake"), "-E", "__run_co_compile",
            f"--tidy={sys.executable};{checker}", f"--source={self.source}", "--",
            sys.executable, str(compiler), "-MD", "-MF", self.target + ".d",
            "-o", self.target, "-c", str(self.source),
        ]
        (self.build / "build.ninja").write_text(
            "rule analyze\n  command = " + shlex.join(command).replace("$", "$$")
            + "\n  depfile = $out.d\n  deps = gcc\n"
            f"build {self.target}: analyze\n"
        )
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\n"
            "CMAKE_PROJECT_NAME:STATIC=analysis_fixture\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            "MODERN_SQLITE_ENABLE_CLANG_TIDY:BOOL=ON\n"
            f"CMAKE_MAKE_PROGRAM:FILEPATH={shutil.which('ninja')}\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={sys.executable}\n"
            f"MODERN_SQLITE_CLANG_TIDY_EXECUTABLE:FILEPATH={sys.executable}\n"
        )
        (self.build / "compile_commands.json").write_text(json.dumps([{
            "directory": str(self.build), "file": str(self.source),
            "output": str(self.build / self.target), "command": shlex.join(command),
        }]))
        metadata = self.build / "CMakeFiles" / "fixture"
        metadata.mkdir(parents=True)
        (metadata / "CMakeCXXCompiler.cmake").write_text('set(CMAKE_CXX_COMPILER_ID "GNU")\n')
        initialize_repository(self.root, commit=False)
        self.report = self.build / "analysis.json"
        self.patch_root = mock.patch.object(run_changed_clang_tidy, "ROOT", self.root)
        self.patch_root.start()
        self.addCleanup(self.patch_root.stop)

    def execute(self, *, reuse=None, assigned=None, database=None):
        return run_changed_clang_tidy.run_reported_analysis(
            self.build, [self.source] if assigned is None else assigned,
            {self.source: self.target} if database is None else database,
            self.report, reuse, 1,
        )

    def prime_object(self) -> None:
        subprocess.run(["ninja", "-C", str(self.build), self.target],
                       check=True, stdout=subprocess.DEVNULL)

    def test_up_to_date_object_without_evidence_requires_real_analysis(self) -> None:
        self.prime_object()
        self.assertEqual("1", self.counter.read_text())
        result = self.execute()
        self.assertEqual("passed", result["status"])
        self.assertEqual("executed", result["analysis"])
        self.assertEqual([self.target], result["executed_targets"])
        self.assertEqual("2", self.counter.read_text())
        self.assertTrue(pathlib.Path(result["execution_record"]).is_file())

    def test_cold_dependency_closure_requires_observation_before_and_after_execution(self) -> None:
        cold = self.execute()
        self.assertEqual("executed", cold["analysis"])
        self.assertFalse(cold["reusable"])
        fresh = self.execute(reuse=self.report)
        self.assertEqual("executed", fresh["analysis"])
        self.assertTrue(fresh["reusable"])
        reused = self.execute(reuse=self.report)
        self.assertEqual("reused", reused["analysis"])
        self.assertEqual(fresh["execution_id"], reused["execution_id"])
        self.assertEqual("2", self.counter.read_text())

    def test_exact_reuse_preserves_execution_identity_and_checks_external_header_bytes(self) -> None:
        self.prime_object()
        first = self.execute()
        reused = self.execute(reuse=self.report)
        self.assertEqual("reused", reused["analysis"])
        self.assertEqual(first["execution_id"], reused["execution_id"])
        self.assertEqual([], reused["executed_targets"])
        self.assertEqual("2", self.counter.read_text())
        stat = self.header.stat()
        self.header.write_text("other header\n")
        os.utime(self.header, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        fresh = self.execute(reuse=self.report)
        self.assertEqual("executed", fresh["analysis"])
        self.assertNotEqual(first["execution_id"], fresh["execution_id"])
        self.assertEqual("3", self.counter.read_text())

    def test_tool_change_invalidates_reuse_but_markdown_only_change_does_not(self) -> None:
        self.prime_object()
        first = self.execute()
        (self.root / "notes.md").write_text("Documentation-only change.\n")
        self.assertEqual("reused", self.execute(reuse=self.report)["analysis"])
        with (self.root / "checker.py").open("a") as output:
            output.write("\n# changed diagnostic implementation\n")
        fresh = self.execute(reuse=self.report)
        self.assertEqual("executed", fresh["analysis"])
        self.assertNotEqual(first["execution_id"], fresh["execution_id"])

    def test_incomplete_dependency_mode_never_claims_reuse(self) -> None:
        manifest = self.build / "build.ninja"
        original = manifest.read_text()
        for flags in ("-MMD", "-MD -MM", "-MD --write-user-dependencies", "-MD -Wp,-MMD,extra.d"):
            with self.subTest(flags=flags):
                manifest.write_text(original.replace(" -MD ", f" {flags} "))
                count = int(self.counter.read_text()) if self.counter.exists() else 0
                self.execute()
                result = self.execute(reuse=self.report)
                self.assertEqual("executed", result["analysis"])
                self.assertFalse(result["reusable"])
                self.assertEqual(str(count + 2), self.counter.read_text())

    def test_emitted_launcher_without_a_cache_entry_is_not_reusable(self) -> None:
        launcher = self.root.parent / "launcher.py"
        launcher.write_text("import os,sys\nos.execv(sys.argv[1], sys.argv[1:])\n")
        option = shlex.quote(f"--launcher={sys.executable};{launcher}")
        manifest = self.build / "build.ninja"
        manifest.write_text(manifest.read_text().replace(
            " __run_co_compile ", f" __run_co_compile {option} ",
        ))
        self.execute()
        result = self.execute(reuse=self.report)
        self.assertEqual("executed", result["analysis"])
        self.assertFalse(result["reusable"])
        self.assertEqual("2", self.counter.read_text())

    def test_empty_assignment_and_escaped_output_never_create_execution_credit(self) -> None:
        from tools.verify import VerificationError

        result = self.execute(assigned=[])
        self.assertEqual("not-required", result["analysis"])
        self.assertFalse(self.counter.exists())
        outside = self.root / "outside.o"
        outside.write_text("preserve")
        with self.assertRaises(VerificationError):
            self.execute(database={self.source: "../outside.o"})
        self.assertEqual("preserve", outside.read_text())
        self.assertFalse(self.counter.exists())

    def test_analysis_failure_is_preserved_and_cannot_authorize_reuse(self) -> None:
        from tools.verify import VerificationError

        (self.root / "checker.py").write_text("raise SystemExit(4)\n")
        with self.assertRaises(VerificationError):
            self.execute()
        failed = json.loads(self.report.read_text())
        self.assertEqual("failed", failed["status"])
        self.assertFalse(failed["reusable"])
        self.assertEqual(failed, json.loads((pathlib.Path(failed["artifacts"]) / "result.json").read_text()))

    def test_successful_command_without_its_object_is_not_analysis_credit(self) -> None:
        from tools.verify import VerificationError

        (self.root / "compiler.py").write_text("print('no output object')\n")
        with self.assertRaisesRegex(VerificationError, "object"):
            self.execute()
        self.assertEqual("failed", json.loads(self.report.read_text())["status"])
        self.assertEqual("1", self.counter.read_text())

    def test_missing_or_empty_cache_owner_cannot_inherit_the_working_directory(self) -> None:
        from tools.verify import VerificationError

        cache = self.build / "CMakeCache.txt"
        original = "".join(
            line for line in cache.read_text().splitlines(keepends=True)
            if not line.startswith("CMAKE_HOME_DIRECTORY:")
        )
        previous = pathlib.Path.cwd()
        try:
            os.chdir(self.root)
            for owner in ("", "CMAKE_HOME_DIRECTORY:INTERNAL=\n"):
                with self.subTest(owner=owner):
                    cache.write_text(original + owner)
                    with self.assertRaisesRegex(VerificationError, "configured"):
                        self.execute()
                    self.assertFalse(self.counter.exists())
        finally:
            os.chdir(previous)

    def test_malformed_reuse_record_fails_without_removing_a_valid_object(self) -> None:
        from tools.verify import VerificationError

        self.prime_object()
        output = self.build / self.target
        before = output.read_bytes()
        invalid = self.build / "invalid.json"
        invalid.write_text("[]")
        with self.assertRaisesRegex(VerificationError, "malformed"):
            self.execute(reuse=invalid)
        self.assertEqual(before, output.read_bytes())
        self.assertEqual("1", self.counter.read_text())


if __name__ == "__main__":
    unittest.main()

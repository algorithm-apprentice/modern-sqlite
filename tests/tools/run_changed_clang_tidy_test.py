#!/usr/bin/env python3

import argparse
import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import run_changed_clang_tidy


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
                    jobs=3 if index is None else 1, smoke_source=pathlib.Path("src/a.cpp"),
                    shard_index=index,
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


if __name__ == "__main__":
    unittest.main()

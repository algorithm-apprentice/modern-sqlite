#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import run_changed_clang_tidy


class ProductionSourceScopeTest(unittest.TestCase):
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

import ast
import pathlib
import re
import sys
import unittest


class VerificationLayeringTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = pathlib.Path(__file__).resolve().parents[2]

    @staticmethod
    def quoted_includes(path: pathlib.Path) -> set[str]:
        return set(
            re.findall(
                r'^#include "([^"]+)"',
                path.read_text(encoding="utf-8"),
                flags=re.MULTILINE,
            )
        )

    def test_trace_runner_uses_only_public_application_contracts(self) -> None:
        includes = self.quoted_includes(
            self.root / "tools/modern_sqlite_read_trace.cpp"
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/bytes.hpp",
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/runtime/sql_value.hpp",
                "modern_sqlite/session/read_session.hpp",
                "modern_sqlite/text/text.hpp",
            },
            includes,
        )
        for include in includes:
            self.assertFalse(include.startswith("src/"), include)

    def test_production_sources_do_not_depend_on_verification_tools(self) -> None:
        forbidden = (
            "sqlite3",
            "read_compatibility",
            "libFuzzer",
            "LLVMFuzzer",
            "tests/fuzz",
        )
        for source_root in (
            self.root / "include/modern_sqlite",
            self.root / "src",
        ):
            for path in source_root.rglob("*"):
                if path.suffix not in {".hpp", ".cpp"}:
                    continue
                text = path.read_text(encoding="utf-8")
                for token in forbidden:
                    self.assertNotIn(token, text, f"{path}: {token}")

    def test_oracle_tool_uses_only_the_python_standard_library(self) -> None:
        tool = self.root / "tools/read_compatibility.py"
        tree = ast.parse(tool.read_text(encoding="utf-8"), filename=str(tool))
        imported: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".", 1)[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.level == 0:
                if node.module is not None:
                    imported.add(node.module.split(".", 1)[0])

        self.assertNotIn("sqlite3", imported)
        self.assertTrue(imported)
        for module in imported:
            self.assertIn(module, sys.stdlib_module_names, module)

    def test_fuzz_entry_sources_use_only_the_public_session_boundary(self) -> None:
        allowed = {
            "read_fuzz.hpp",
            "modern_sqlite/session/read_session.hpp",
            "modern_sqlite/text/text.hpp",
        }
        for relative in (
            "tests/fuzz/read_sql_fuzz.cpp",
            "tests/fuzz/database_image_fuzz.cpp",
        ):
            includes = self.quoted_includes(self.root / relative)
            self.assertTrue(includes.issubset(allowed), (relative, includes))
            for include in includes:
                self.assertFalse(include.startswith("src/"), (relative, include))

    def test_ordinary_engine_target_has_no_fuzzer_runtime_flags(self) -> None:
        cmake = (self.root / "CMakeLists.txt").read_text(encoding="utf-8")
        ordinary_begin = cmake.index("add_library(\n  modern_sqlite\n  STATIC")
        fuzz_begin = cmake.index("if(MODERN_SQLITE_BUILD_FUZZERS)")
        ordinary = cmake[ordinary_begin:fuzz_begin]

        self.assertIn("${MODERN_SQLITE_ENGINE_SOURCES}", ordinary)
        self.assertNotIn("-fsanitize=fuzzer", ordinary)
        self.assertNotIn("sqlite3", ordinary)
        self.assertIn(
            "target_link_libraries(\n  modern_sqlite_read_trace\n  PRIVATE\n"
            "    modern_sqlite::modern_sqlite\n)",
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
